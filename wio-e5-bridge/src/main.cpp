#include <Arduino.h>
#include "uart_protocol.h"
#include "radio_bridge.h"

extern "C" {
    int uart_proto_parse_byte(uint8_t byte, uint8_t *out_cmd, uint8_t *out_payload, uint16_t *out_len);
    void uart_proto_reset(void);
}

HardwareSerial BridgeSerial(PB7, PB6);

volatile uint32_t g_radio_init_result __attribute__((used)) = 0xDEADBEEF;
volatile uint32_t g_cmd_count         __attribute__((used)) = 0;
volatile uint32_t g_loop_iters        __attribute__((used)) = 0;
volatile uint8_t  g_last_cmd          __attribute__((used)) = 0;
volatile uint8_t  g_last_status       __attribute__((used)) = 0;

static uint8_t tx_buf[UART_RADIO_HEADER_SIZE + UART_RADIO_MAX_PAYLOAD + UART_RADIO_CRC_SIZE];

static void send_response(uint8_t cmd, const uint8_t *payload, uint16_t len) {
    uint16_t frame_len = uart_proto_build_frame(tx_buf, cmd, payload, len);
    BridgeSerial.write(tx_buf, frame_len);
}

static void send_ack(uint8_t original_cmd, uint8_t status) {
    uint8_t payload[2] = { original_cmd, status };
    send_response(RSP_ACK, payload, 2);
}

static void handle_command(uint8_t cmd, const uint8_t *payload, uint16_t len) {
    int result;
    g_cmd_count++;
    g_last_cmd = cmd;
    switch (cmd) {
    case CMD_RADIO_SET_DIO:
        result = radio_bridge_set_dio(payload, len);
        g_last_status = (uint8_t)(result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_CONFIGURE:
        result = radio_bridge_configure(payload, len);
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_TX:
        result = radio_bridge_transmit(payload, len);
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_RX_START:
        result = radio_bridge_start_receive();
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_SLEEP:
        result = radio_bridge_sleep();
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_STANDBY:
        result = radio_bridge_standby();
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;
    case CMD_RADIO_GET_STATUS:
        send_ack(cmd, STATUS_OK);
        break;
    default:
        send_response(RSP_ERROR, (const uint8_t[]){STATUS_ERR_UNKNOWN}, 1);
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
    uart_proto_reset();
    force_usart1_pinmux();

    // First radio.begin() is expected to fail — TCXO/DIO2 config arrives
    // later via CMD_RADIO_SET_DIO. Capture the result, never block.
    g_radio_init_result = (uint32_t)radio_bridge_init();

    force_usart1_pinmux();
}

void loop() {
    g_loop_iters++;

    static uint32_t last_diag_ms = 0;
    uint32_t now = millis();
    if (now - last_diag_ms >= 50) {
        last_diag_ms = now;
        radio_bridge_diag_poll();
    }

    while (BridgeSerial.available()) {
        uint8_t byte = BridgeSerial.read();
        uint8_t cmd;
        uint8_t payload[UART_RADIO_MAX_PAYLOAD];
        uint16_t payload_len;
        if (uart_proto_parse_byte(byte, &cmd, payload, &payload_len)) {
            handle_command(cmd, payload, payload_len);
        }
    }

    if (radio_bridge_check_tx_done()) {
        send_response(RSP_TX_DONE, NULL, 0);
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
        send_response(RSP_RX_PACKET, rsp_payload, 3 + rx_len);
    }
}
