#include <Arduino.h>
#include "uart_protocol.h"
#include "radio_bridge.h"

// Arduino-STM32 HardwareSerial(PC0, PC1) is supposed to bring up LPUART1, but
// on this fork it leaves GPIOC unclocked and LPUART1->CR1 = 0 — SWD probes
// after radio_bridge_init() show RCC->AHB2ENR bit 2 = 0 and APB1ENR2 bit 0 = 0.
// We bypass HardwareSerial for the ESP side and drive LPUART1 directly via LL.
#include <stm32wlxx_ll_bus.h>
#include <stm32wlxx_ll_gpio.h>
#include <stm32wlxx_ll_lpuart.h>
#include <stm32wlxx_ll_rcc.h>
#include <stm32wlxx_hal_rcc.h>

extern "C" {
    int  uart_proto_parse_byte_ctx(uart_proto_ctx_t *ctx, uint8_t byte,
                                   uint8_t *out_cmd, uint8_t *out_payload, uint16_t *out_len);
    void uart_proto_reset_ctx(uart_proto_ctx_t *ctx);
}

// Origin tag so send_response()/send_ack()/handle_command() can route a reply
// back to the correct UART without holding a Stream*. The ESP side has no
// Stream wrapper anymore (LPUART1 is driven raw via LL), so we dispatch on
// this enum instead of a Stream& parameter.
typedef enum {
    ORIGIN_BRIDGE = 0,  // USART1 (PB6/PB7) — Display CPU master
    ORIGIN_ESP    = 1,  // LPUART1 (PC0/PC1) — ESP32-C5 master
} response_origin_t;

// Display CPU master on PB6/PB7 (USART1).
HardwareSerial BridgeSerial(PB7, PB6);
// ESP32-C5 master on PC0/PC1 (LPUART1) — driven directly via LL (see above).

static uart_proto_ctx_t bridge_ctx;
static uart_proto_ctx_t esp_ctx;

volatile uint32_t g_radio_init_result __attribute__((used)) = 0xDEADBEEF;
volatile uint32_t g_cmd_count         __attribute__((used)) = 0;
volatile uint32_t g_loop_iters        __attribute__((used)) = 0;
volatile uint8_t  g_last_cmd          __attribute__((used)) = 0;
volatile uint8_t  g_last_status       __attribute__((used)) = 0;

// ESP-path counters — readable via SWD/OpenOCD mdw to confirm wiring.
volatile uint32_t g_esp_byte_count     __attribute__((used)) = 0;
volatile uint32_t g_esp_cmd_count      __attribute__((used)) = 0;
volatile uint32_t g_esp_response_count __attribute__((used)) = 0;
volatile uint32_t g_bridge_byte_count  __attribute__((used)) = 0;

// Computed LPUART1->BRR — exposed so we can verify the value via SWD.
volatile uint32_t g_lpuart_brr         __attribute__((used)) = 0;

static uint8_t tx_buf[UART_RADIO_HEADER_SIZE + UART_RADIO_MAX_PAYLOAD + UART_RADIO_CRC_SIZE];

// ---------------------------------------------------------------------------
// LPUART1 direct-LL driver for the ESP side.
// ---------------------------------------------------------------------------
//
// Replaces the previous `HardwareSerial EspSerial(PC0, PC1)` because that
// constructor never actually programs LPUART1 on this Arduino-STM32 fork —
// PeripheralPins.c has the AF map, but HardwareSerial::begin() walks a
// USART-only enable list and silently skips LPUART1.
static void esp_uart_init(void) {
    // 1. Belt-and-braces GPIOC clock enable. The LL helper does a RMW on
    //    AHB2ENR, but we follow up with a raw OR so a later framework path
    //    that clears the register (LL_RCC_RF_*) still leaves bit 2 set if
    //    this function runs after it.
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOC);
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOCEN;
    (void)RCC->AHB2ENR;

    // 2. LPUART1 bus clock + clock-source selection. PCLK1 is the
    //    reset-default source; we set it explicitly so a stray
    //    LL_RCC_SetLPUARTClockSource() upstream can't strand us on LSE.
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_LPUART1);
    (void)RCC->APB1ENR2;
    LL_RCC_SetLPUARTClockSource(LL_RCC_LPUART1_CLKSOURCE_PCLK1);

    // 3. GPIO PC0/PC1 → AF8 (LPUART1_RX/TX). HardwareSerial occasionally
    //    leaves these as analog after radio.begin(); force the mux.
    LL_GPIO_SetPinMode(GPIOC,    LL_GPIO_PIN_0, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetPinMode(GPIOC,    LL_GPIO_PIN_1, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_0_7(GPIOC,  LL_GPIO_PIN_0, LL_GPIO_AF_8);
    LL_GPIO_SetAFPin_0_7(GPIOC,  LL_GPIO_PIN_1, LL_GPIO_AF_8);
    LL_GPIO_SetPinSpeed(GPIOC,   LL_GPIO_PIN_0, LL_GPIO_SPEED_FREQ_HIGH);
    LL_GPIO_SetPinSpeed(GPIOC,   LL_GPIO_PIN_1, LL_GPIO_SPEED_FREQ_HIGH);
    LL_GPIO_SetPinPull(GPIOC,    LL_GPIO_PIN_0, LL_GPIO_PULL_UP);
    LL_GPIO_SetPinPull(GPIOC,    LL_GPIO_PIN_1, LL_GPIO_PULL_UP);

    // 4. BRR. LPUART formula: BRR = (256 * f_LPUARTCLK) / baud.
    //    Use HAL_RCC_GetPCLK1Freq() so we don't bake in an assumption about
    //    the framework's sysclk. At 48 MHz / 115200 → 0x1A0AB.
    uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
    uint32_t brr = (uint32_t)(((uint64_t)pclk1 * 256ULL) / 115200ULL);
    // LPUART BRR must be in [0x300, 0xFFFFF] per RM0461.
    if (brr < 0x300)    brr = 0x300;
    if (brr > 0xFFFFF)  brr = 0xFFFFF;
    g_lpuart_brr = brr;

    // 5. Program LPUART1. Disable first so BRR/CR1 changes stick.
    LPUART1->CR1 = 0;
    LPUART1->BRR = brr;
    LPUART1->CR2 = 0;                                          // 1 stop, no LIN
    LPUART1->CR3 = 0;                                          // no flow control
    LPUART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE; // 8N1, TX+RX on
}

static inline bool esp_uart_available(void) {
    return (LPUART1->ISR & USART_ISR_RXNE_RXFNE) != 0;
}

static inline uint8_t esp_uart_read(void) {
    return (uint8_t)LPUART1->RDR;
}

static inline void esp_uart_write_bytes(const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        while ((LPUART1->ISR & USART_ISR_TXE_TXFNF) == 0) { /* spin */ }
        LPUART1->TDR = buf[i];
    }
}

// ---------------------------------------------------------------------------
// Response routing.
// ---------------------------------------------------------------------------
//
// Previously these took `Stream &out` and compared `&out == &EspSerial` to
// bump the per-origin counters. With LPUART1 no longer wrapped in a Stream,
// we pass an enum tag instead. Routing is explicit and the per-origin
// counters stay diagnostic-grade.
static void send_response(response_origin_t origin, uint8_t cmd,
                          const uint8_t *payload, uint16_t len) {
    uint16_t frame_len = uart_proto_build_frame(tx_buf, cmd, payload, len);
    if (origin == ORIGIN_BRIDGE) {
        BridgeSerial.write(tx_buf, frame_len);
    } else {
        esp_uart_write_bytes(tx_buf, frame_len);
        g_esp_response_count++;
    }
}

static void send_ack(response_origin_t origin, uint8_t original_cmd, uint8_t status) {
    uint8_t payload[2] = { original_cmd, status };
    send_response(origin, RSP_ACK, payload, 2);
}

static void handle_command(response_origin_t origin, uint8_t cmd,
                           const uint8_t *payload, uint16_t len) {
    int result;
    g_cmd_count++;
    g_last_cmd = cmd;
    if (origin == ORIGIN_ESP) g_esp_cmd_count++;
    switch (cmd) {
    case CMD_RADIO_SET_DIO:
        result = radio_bridge_set_dio(payload, len);
        g_last_status = (uint8_t)(result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        send_ack(origin, cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_CONFIGURE:
        result = radio_bridge_configure(payload, len);
        send_ack(origin, cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_TX:
        result = radio_bridge_transmit(payload, len);
        send_ack(origin, cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_RX_START:
        result = radio_bridge_start_receive();
        send_ack(origin, cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_SLEEP:
        result = radio_bridge_sleep();
        send_ack(origin, cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_STANDBY:
        result = radio_bridge_standby();
        send_ack(origin, cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_GET_STATUS:
        send_ack(origin, cmd, STATUS_OK);
        break;
    default:
        send_response(origin, RSP_ERROR, (const uint8_t[]){STATUS_ERR_UNKNOWN}, 1);
        break;
    }
}

// HardwareSerial::begin() and radio.begin() can leave PB6/PB7 in analog
// mode; force PB6/PB7 → AF7 (USART1) explicitly.
static void force_usart1_pinmux(void) {
    uint32_t moder = GPIOB->MODER;
    moder &= ~((0x3u << (6 * 2)) | (0x3u << (7 * 2)));
    moder |=  ((0x2u << (6 * 2)) | (0x2u << (7 * 2)));
    GPIOB->MODER = moder;

    uint32_t afrl = GPIOB->AFR[0];
    afrl &= ~((0xFu << (6 * 4)) | (0xFu << (7 * 4)));
    afrl |=  ((0x7u << (6 * 4)) | (0x7u << (7 * 4)));
    GPIOB->AFR[0] = afrl;
}

void setup() {
    BridgeSerial.begin(115200);
    uart_proto_reset_ctx(&bridge_ctx);
    uart_proto_reset_ctx(&esp_ctx);
    force_usart1_pinmux();
    esp_uart_init();

    // First radio.begin() is expected to fail — TCXO/DIO2 config arrives
    // later via CMD_RADIO_SET_DIO. Capture the result, never block.
    g_radio_init_result = (uint32_t)radio_bridge_init();

    // radio_bridge_init() pokes RCC for the SUBGHZ peripheral and can clobber
    // unrelated GPIO clock bits on this fork; re-assert both UARTs.
    force_usart1_pinmux();
    esp_uart_init();
}

// Drain bytes from BridgeSerial (HardwareSerial) and feed the bridge parser.
static inline void pump_bridge_serial(void) {
    while (BridgeSerial.available()) {
        uint8_t byte = BridgeSerial.read();
        g_bridge_byte_count++;
        uint8_t cmd;
        uint8_t payload[UART_RADIO_MAX_PAYLOAD];
        uint16_t payload_len;
        if (uart_proto_parse_byte_ctx(&bridge_ctx, byte, &cmd, payload, &payload_len)) {
            handle_command(ORIGIN_BRIDGE, cmd, payload, payload_len);
        }
    }
}

// Drain bytes from LPUART1 (ESP) and feed the esp parser.
static inline void pump_esp_serial(void) {
    while (esp_uart_available()) {
        uint8_t byte = esp_uart_read();
        g_esp_byte_count++;
        uint8_t cmd;
        uint8_t payload[UART_RADIO_MAX_PAYLOAD];
        uint16_t payload_len;
        if (uart_proto_parse_byte_ctx(&esp_ctx, byte, &cmd, payload, &payload_len)) {
            handle_command(ORIGIN_ESP, cmd, payload, payload_len);
        }
    }
}

void loop() {
    g_loop_iters++;

    static uint32_t last_diag_ms = 0;
    uint32_t now = millis();
    if (now - last_diag_ms >= 50) {
        last_diag_ms = now;
        radio_bridge_diag_poll();
    }

    pump_bridge_serial();
    pump_esp_serial();

    // Async TX-done / RX events have no originating serial. Broadcast to
    // both masters — each will see its own radio activity in context.
    if (radio_bridge_check_tx_done()) {
        send_response(ORIGIN_BRIDGE, RSP_TX_DONE, NULL, 0);
        send_response(ORIGIN_ESP,    RSP_TX_DONE, NULL, 0);
    }

    uint8_t rx_buf[UART_RADIO_MAX_PAYLOAD];
    float rssi, snr;
    int rx_len = radio_bridge_check_rx(rx_buf, sizeof(rx_buf), &rssi, &snr);
    if (rx_len > 0) {
        uint8_t rsp_payload[3 + UART_RADIO_MAX_PAYLOAD];
        int16_t rssi_int = (int16_t)(rssi * 10);
        int8_t snr_int = (int8_t)(snr * 4);
        memcpy(&rsp_payload[0], &rssi_int, 2);
        rsp_payload[2] = (uint8_t)snr_int;
        memcpy(&rsp_payload[3], rx_buf, rx_len);
        send_response(ORIGIN_BRIDGE, RSP_RX_PACKET, rsp_payload, 3 + rx_len);
        send_response(ORIGIN_ESP,    RSP_RX_PACKET, rsp_payload, 3 + rx_len);
    }
}
