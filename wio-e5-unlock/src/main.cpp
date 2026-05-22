// WIO-E5 unlock helper — Pico SDK build for FreeWili Display RP2350B
//
// Hardware UART1: TX=GPIO 40 (F2, standard), RX=GPIO 23 (F11, UART_AUX).
// Routed to WIO-E5 PB6/PB7 via IC113 demux, gated by LoRA_1101_SEL from
// the PCAL6524 IO expander at I2C1 0x23 (port 1 = 0xF4 → SEL=1).
//
// Three modes — set exactly one of the *_ONLY defines to 1. With both 0,
// the full WIO-E5 RDP unlock sequence runs.
//
//   SCOPE_TEST_ONLY: tight loop sending "AT\r\n" forever — for verifying
//     TX on a scope before committing to the unlock sequence.
//
//   BRIDGE_VERIFY_ONLY: switch UART to 115200 8-N-1 and loop sending
//     CMD_RADIO_GET_STATUS frames. Captures the bridge's ACK response into
//     SRAM globals for SWD inspection. Use after flashing wio-e5-bridge.
//
//   (default, both 0): full unlock —
//      a. Probe AT firmware ("AT\r\n" → "OK")
//      b. AT+DFU=ON → AT firmware soft-resets into STM32 system bootloader
//      c. Switch UART to 115200 8-E-1 (bootloader framing, AN3155/AN5482)
//      d. Send 0x7F sync → expect 0x79 ACK
//      e. Send 0x91 0x6E (Readout Unprotect + ~cmd checksum, AN3155)
//         → first 0x79 ACK, mass erase (~25s), second 0x79
//         → RDP→Level 0, flash erased, chip resets to bootloader.
//
// Status globals are inspectable over SWD with `mdw`.

#include <cstdint>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"

#include "rmpLib/rpSerialComm.h"
#include "uart_protocol.h"   // from ../wio-e5-bridge/src — frame builder + IDs

#define SCOPE_TEST_ONLY    0
#define BRIDGE_VERIFY_ONLY 1
static_assert(!(SCOPE_TEST_ONLY && BRIDGE_VERIFY_ONLY),
              "Pick at most one mode");

#define WIO_TX_PIN 40
#define WIO_RX_PIN 23
#define I2C_SDA_PIN 26
#define I2C_SCL_PIN 27
#define IO_EXPANDER_ADDR 0x23
#define AT_BAUD 9600

#define STATUS_PENDING        0u
#define STATUS_OK_RECEIVED    1u
#define STATUS_NO_RESPONSE    2u
#define STATUS_BYTES_NO_OK    3u
#define STATUS_BOOTLOADER_ACK 4u
#define STATUS_BOOTLOADER_NACK 5u
#define STATUS_DFU_RESPONSE   6u

#define UNLOCK_PENDING        0u
#define UNLOCK_SYNC_NACK      1u   // 0x7F got back NACK (0x1F)
#define UNLOCK_SYNC_TIMEOUT   2u   // 0x7F got back nothing
#define UNLOCK_SYNC_ACK       3u   // 0x7F → 0x79 received
#define UNLOCK_CMD_NACK       4u   // 0x91 0x6E got back NACK
#define UNLOCK_CMD_TIMEOUT    5u   // 0x91 0x6E got back nothing
#define UNLOCK_CMD_ACK        6u   // first 0x79 received (cmd accepted)
#define UNLOCK_ERASE_TIMEOUT  7u   // second 0x79 (post mass-erase) never came
#define UNLOCK_SUCCESS        8u   // both ACKs received — RDP should be Level 0

typedef struct {
    uint32_t magic;
    uint32_t status;
    uint32_t resp_len;
    char     response[512];
} at_result_t;

volatile at_result_t g_at_result __attribute__((used)) = {0, STATUS_PENDING, 0, {0}};
volatile at_result_t g_dfu_result __attribute__((used)) = {0, STATUS_PENDING, 0, {0}};
volatile uint32_t g_boot_mark __attribute__((used)) = 0;
volatile uint32_t g_unlock_status __attribute__((used)) = UNLOCK_PENDING;
volatile uint8_t  g_unlock_sync_resp __attribute__((used)) = 0;
volatile uint8_t  g_unlock_cmd_resp __attribute__((used)) = 0;
volatile uint8_t  g_unlock_erase_resp __attribute__((used)) = 0;
volatile uint32_t g_scope_iter_count __attribute__((used)) = 0;
volatile uint32_t g_scope_last_rx_len __attribute__((used)) = 0;
volatile char     g_scope_last_rx[64] __attribute__((used)) = {0};
volatile uint32_t g_scope_total_rx_bytes __attribute__((used)) = 0;
volatile uint32_t g_scope_iters_with_rx __attribute__((used)) = 0;

// Bridge-verify mode globals.
volatile uint32_t g_bridge_iter_count __attribute__((used)) = 0;
volatile uint32_t g_bridge_acks       __attribute__((used)) = 0;   // valid ACK frames seen
volatile uint32_t g_bridge_total_rx   __attribute__((used)) = 0;   // running sum of RX bytes
volatile uint32_t g_bridge_resp_len   __attribute__((used)) = 0;   // length of last response
volatile uint8_t  g_bridge_resp[32]   __attribute__((used)) = {0}; // bytes of last response

volatile uint32_t g_ioexp_diag[9] __attribute__((used)) = {0};

#define GPIO23_SAMPLE_COUNT 8000
volatile uint8_t  g_gpio23_samples[GPIO23_SAMPLE_COUNT] __attribute__((used)) = {0};
volatile uint32_t g_gpio23_sample_done __attribute__((used)) = 0;

// Accumulating RX log across ALL AT iterations (the per-iter buffer in
// g_at_result gets overwritten). Reveals total byte volume + which iter
// each byte landed in.
#define RX_LOG_MAX 256
volatile uint8_t  g_rx_log[RX_LOG_MAX] __attribute__((used)) = {0};
volatile uint16_t g_rx_log_iter[RX_LOG_MAX] __attribute__((used)) = {0};  // iteration number per byte
volatile uint32_t g_rx_log_count __attribute__((used)) = 0;
volatile uint32_t g_rx_iters_with_bytes __attribute__((used)) = 0;

static rpSerialComm obLoRAComm;

static void init_io_expander(void) {
    i2c_init(i2c1, 400000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    // Port 1 = 0xF4: V1_1 (bit 3) and V2_1 (bit 1) BOTH 0.
    // LoRA_1101_SEL = NOR(V1_1, V2_1) (IC82 SN74LVC1G02), so both 0 → SEL=1
    // → IC113 demux routes GPIO40_DISP to LoRA_PB7 (WIO USART1_RX).
    uint8_t out_cmd[] = {0x04, 0xDB, 0xF4, 0x85};
    int rc1 = i2c_write_blocking(i2c1, IO_EXPANDER_ADDR, out_cmd, sizeof(out_cmd), false);
    g_ioexp_diag[0] = (uint32_t)rc1;

    uint8_t cfg_cmd[] = {0x0C, 0x00, 0x00, 0x02};
    int rc2 = i2c_write_blocking(i2c1, IO_EXPANDER_ADDR, cfg_cmd, sizeof(cfg_cmd), false);
    g_ioexp_diag[1] = (uint32_t)rc2;

    sleep_ms(50);

    uint8_t reg = 0x04;
    uint8_t rb_out[3] = {0xAA, 0xAA, 0xAA};
    i2c_write_blocking(i2c1, IO_EXPANDER_ADDR, &reg, 1, true);
    int rc3 = i2c_read_blocking(i2c1, IO_EXPANDER_ADDR, rb_out, 3, false);
    g_ioexp_diag[2] = (uint32_t)rc3;
    g_ioexp_diag[3] = rb_out[0];
    g_ioexp_diag[4] = rb_out[1];
    g_ioexp_diag[5] = rb_out[2];

    reg = 0x0C;
    uint8_t rb_cfg[3] = {0xAA, 0xAA, 0xAA};
    i2c_write_blocking(i2c1, IO_EXPANDER_ADDR, &reg, 1, true);
    i2c_read_blocking(i2c1, IO_EXPANDER_ADDR, rb_cfg, 3, false);
    g_ioexp_diag[6] = rb_cfg[0];
    g_ioexp_diag[7] = rb_cfg[1];
    g_ioexp_diag[8] = rb_cfg[2];
}

static void write_at_result(uint32_t status, const char *resp, size_t len) {
    g_at_result.status = status;
    if (len > sizeof(g_at_result.response) - 1) {
        len = sizeof(g_at_result.response) - 1;
    }
    g_at_result.resp_len = len;
    for (size_t i = 0; i < len; i++) {
        g_at_result.response[i] = resp[i];
    }
    g_at_result.response[len] = 0;
    g_at_result.magic = 0xDEADBEEF;
}

static void wio_write(const char *s) {
    obLoRAComm.txData((const unsigned char *)s, (unsigned int)strlen(s));
}

static size_t wio_read_until(char *buf, size_t buf_max, uint32_t timeout_ms) {
    size_t len = 0;
    absolute_time_t end = make_timeout_time_ms(timeout_ms);
    while (!time_reached(end) && len < buf_max - 1) {
        int avail = obLoRAComm.rxDataCount();
        if (avail > 0) {
            int want = (int)(buf_max - 1 - len);
            if (want > avail) want = avail;
            int got = obLoRAComm.rxReadData((unsigned char *)(buf + len), want);
            if (got > 0) len += (size_t)got;
        } else {
            tight_loop_contents();
        }
    }
    buf[len] = 0;
    return len;
}

static void wio_drain(void) {
    unsigned char scratch[64];
    int avail;
    while ((avail = obLoRAComm.rxDataCount()) > 0) {
        int want = avail > (int)sizeof(scratch) ? (int)sizeof(scratch) : avail;
        (void)obLoRAComm.rxReadData(scratch, want);
    }
}

int main(void) {
    g_boot_mark = 0xC0FFEE01;
    init_io_expander();
    g_boot_mark = 0xC0FFEE02;

    // Hardware UART matching the original LoRa test (commit 3f678095):
    //   gpio_set_function(GPIO40_DSP, GPIO_FUNC_UART);      // uart1_tx
    //   gpio_set_function(LORA_SPI_CS, GPIO_FUNC_UART_AUX); // uart1_rx
    //   uart_init(uart1, 9600);
    // GPIO 23 isn't UART1_RX under F2 — only under F11 (UART AUX). The PIO
    // version (commit 6a1745c) also worked but pulls in more state; HW UART
    // is the minimum we can run and matches the first verified config.
    rpSerialCommConfig cfg = {};
    cfg.bHardwareUART = true;
    cfg.iModuleIndex  = 1;          // uart1
    cfg.iTxPin        = WIO_TX_PIN; // GPIO 40
    cfg.iRxPin        = WIO_RX_PIN; // GPIO 23
    cfg.bRxAuxPin     = true;       // RX uses GPIO_FUNC_UART_AUX (F11)
    cfg.iBaudRate     = AT_BAUD;
    obLoRAComm.init(cfg);
    g_boot_mark = 0xC0FFEE03;

    sleep_ms(200);
    wio_drain();

#if SCOPE_TEST_ONLY
    // Scope verification mode: spam AT continuously and capture the AT
    // firmware's reply into globals for SWD inspection / breakpointing.
    // Per cycle: drain → TX "AT\r\n" → wait up to 200 ms for reply →
    // record into g_scope_last_rx → 100 ms idle. Total ~300 ms per cycle.
    while (1) {
        wio_drain();
        wio_write("AT\r\n");
        g_scope_iter_count++;

        char rxbuf[64];
        size_t rxlen = wio_read_until(rxbuf, sizeof(rxbuf), 200);

        // Latch result into globals. Set len=0 first, then fill the buffer
        // (one volatile write per byte — new content or zero), then set len
        // last so a debugger sampling mid-update sees len==0 instead of a
        // mixed buffer. Two consecutive loops let the optimizer drop the
        // zero-fill because it sees the overwrite; a single conditional
        // loop forces one observable store per index.
        g_scope_last_rx_len = 0;
        const size_t kBufMax = sizeof(g_scope_last_rx) - 1;  // reserve NUL
        for (size_t i = 0; i < sizeof(g_scope_last_rx); i++) {
            g_scope_last_rx[i] = (i < rxlen && i < kBufMax) ? rxbuf[i] : '\0';
        }
        g_scope_last_rx_len = (uint32_t)rxlen;
        g_scope_total_rx_bytes += (uint32_t)rxlen;
        if (rxlen > 0) g_scope_iters_with_rx++;

        sleep_ms(100);
    }
#endif

#if BRIDGE_VERIFY_ONLY
    // Bridge talks at 115200 8-N-1 (HardwareSerial default in wio-e5-bridge
    // main.cpp: BridgeSerial.begin(115200)). UART was inited at AT_BAUD 9600
    // 8-N-1 above, so just bump the baud.
    uart_set_baudrate(uart1, 115200);
    sleep_ms(50);
    wio_drain();

    // Pre-build the CMD_RADIO_GET_STATUS frame: AA 55 06 00 00 [CRC].
    // Expected ACK from bridge: AA 55 81 00 02 06 00 [CRC] (8 bytes).
    uint8_t frame[UART_RADIO_HEADER_SIZE + UART_RADIO_CRC_SIZE];
    uint16_t frame_len = uart_proto_build_frame(frame, CMD_RADIO_GET_STATUS,
                                                 nullptr, 0);

    while (1) {
        wio_drain();
        obLoRAComm.txData(frame, frame_len);
        g_bridge_iter_count++;

        // Bridge replies within a few ms; allow 200 ms for safety.
        char rxbuf[32];
        size_t rxlen = wio_read_until(rxbuf, sizeof(rxbuf), 200);

        // Latch response into globals (single-loop, defeats optimizer's dead-
        // store elimination of a separate zero-fill).
        g_bridge_resp_len = 0;
        for (size_t i = 0; i < sizeof(g_bridge_resp); i++) {
            g_bridge_resp[i] = (i < rxlen) ? (uint8_t)rxbuf[i] : 0;
        }
        g_bridge_resp_len = (uint32_t)rxlen;
        g_bridge_total_rx += (uint32_t)rxlen;

        // Quick parse: valid ACK to GET_STATUS = 8 bytes,
        // [AA 55 81 00 02 06 00 CRC]. We skip CRC verification here — if the
        // first 7 bytes match, that's enough signal the bridge is alive.
        if (rxlen >= 8 &&
            (uint8_t)rxbuf[0] == UART_RADIO_SYNC1 &&
            (uint8_t)rxbuf[1] == UART_RADIO_SYNC2 &&
            (uint8_t)rxbuf[2] == RSP_ACK &&
            (uint8_t)rxbuf[3] == 0x00 &&
            (uint8_t)rxbuf[4] == 0x02 &&
            (uint8_t)rxbuf[5] == CMD_RADIO_GET_STATUS &&
            (uint8_t)rxbuf[6] == STATUS_OK) {
            g_bridge_acks++;
        }

        sleep_ms(500);
    }
#endif

    char resp[512];
    size_t resp_len;

    // Loop AT every 2 seconds so a HOME+CANCEL WIO reset during this window
    // catches a fresh, awake WIO before it returns to STANDBY.
    for (int iter = 0; iter < 30; iter++) {
        g_boot_mark = 0xC0FFEE10 + iter;
        sleep_ms(50);
        wio_drain();
        wio_write("AT\r\n");
        resp_len = wio_read_until(resp, sizeof(resp), 1500);

        // Accumulate across all iterations
        if (resp_len > 0) {
            g_rx_iters_with_bytes++;
            for (size_t i = 0; i < resp_len && g_rx_log_count < RX_LOG_MAX; i++) {
                g_rx_log[g_rx_log_count] = (uint8_t)resp[i];
                g_rx_log_iter[g_rx_log_count] = (uint16_t)iter;
                g_rx_log_count++;
            }
        }

        if (resp_len > 0 && strstr(resp, "OK") != NULL) {
            write_at_result(STATUS_OK_RECEIVED, resp, resp_len);
            break;
        }
        if (resp_len > 0) {
            write_at_result(STATUS_BYTES_NO_OK, resp, resp_len);
        }
        sleep_ms(500);
    }

    // If AT firmware acknowledged, request DFU per Seeed spec §4.22.
    // "For UART bootloader, AT+DFU=ON will make device enter bootloader
    //  mode automatically." → no repower needed on this hardware.
    if (g_at_result.status == STATUS_OK_RECEIVED) {
        g_boot_mark = 0xC0FFEE30;
        sleep_ms(50);
        wio_drain();
        wio_write("AT+DFU=ON\r\n");
        resp_len = wio_read_until(resp, sizeof(resp), 2000);

        // Capture into the dedicated DFU buffer (don't clobber g_at_result)
        g_dfu_result.resp_len = resp_len;
        size_t cap = resp_len < sizeof(g_dfu_result.response) - 1
                   ? resp_len : sizeof(g_dfu_result.response) - 1;
        for (size_t i = 0; i < cap; i++) {
            g_dfu_result.response[i] = resp[i];
        }
        g_dfu_result.response[cap] = 0;
        g_dfu_result.status = STATUS_DFU_RESPONSE;
        g_dfu_result.magic = 0xDEADBEEF;

        sleep_ms(200);
    }

    // After AT+DFU=ON, the AT firmware resets and the chip enters DFU mode.
    // Scope observation: the bootloader transmits at 115200 baud (not 9600
    // like the AT firmware) with 8-E-1 framing per AN5482. Switch BOTH baud
    // and parity before sending the sync byte.
    sleep_ms(200);          // let the chip finish resetting into bootloader
    wio_drain();
    uart_set_baudrate(uart1, 115200);
    uart_set_format(uart1, 8, 1, UART_PARITY_EVEN);

    // Bootloader sync: send 0x7F, expect 0x79 ACK.
    g_boot_mark = 0xC0FFEE40;
    unsigned char sync = 0x7F;
    obLoRAComm.txData(&sync, 1);
    resp_len = wio_read_until(resp, sizeof(resp), 1000);

    if (resp_len > 0) {
        g_unlock_sync_resp = (uint8_t)resp[0];
        write_at_result((uint8_t)resp[0] == 0x79 ? STATUS_BOOTLOADER_ACK : STATUS_BOOTLOADER_NACK, resp, resp_len);
    } else if (g_at_result.status != STATUS_BYTES_NO_OK) {
        write_at_result(STATUS_NO_RESPONSE, resp, resp_len);
    }

    if (resp_len == 0) {
        g_unlock_status = UNLOCK_SYNC_TIMEOUT;
        goto park;
    }
    if ((uint8_t)resp[0] != 0x79) {
        g_unlock_status = UNLOCK_SYNC_NACK;
        goto park;
    }
    g_unlock_status = UNLOCK_SYNC_ACK;

    // Readout Unprotect (AN3155): cmd 0x91, checksum = ~0x91 = 0x6E.
    // Bootloader responds with: ACK (cmd accepted) → mass erase (~25s max)
    // → ACK (erase complete) → reset. Mass erase puts RDP back to 0xAA.
    {
        g_boot_mark = 0xC0FFEE50;
        unsigned char unprot[] = {0x91, 0x6E};
        obLoRAComm.txData(unprot, sizeof(unprot));

        // First ACK: command accepted.
        resp_len = wio_read_until(resp, sizeof(resp), 1000);
        if (resp_len == 0) {
            g_unlock_status = UNLOCK_CMD_TIMEOUT;
            goto park;
        }
        g_unlock_cmd_resp = (uint8_t)resp[0];
        if ((uint8_t)resp[0] != 0x79) {
            g_unlock_status = UNLOCK_CMD_NACK;
            goto park;
        }
        g_unlock_status = UNLOCK_CMD_ACK;

        // Second ACK: mass erase complete. Datasheet says ≤25.4s for 256KB;
        // give it a generous 35s window. The bootloader resets after this.
        g_boot_mark = 0xC0FFEE51;
        resp_len = wio_read_until(resp, sizeof(resp), 35000);
        if (resp_len == 0) {
            g_unlock_status = UNLOCK_ERASE_TIMEOUT;
            goto park;
        }
        g_unlock_erase_resp = (uint8_t)resp[0];
        if ((uint8_t)resp[0] == 0x79) {
            g_unlock_status = UNLOCK_SUCCESS;
        } else {
            g_unlock_status = UNLOCK_ERASE_TIMEOUT;
        }
    }

park:
    g_boot_mark = 0xC0FFEE60;
    while (1) {
        sleep_ms(1000);
    }
}
