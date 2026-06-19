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
#define BRIDGE_VERIFY_ONLY 0
// RECOVERY mode: when set to 1, the helper only inits the I/O expander to
// the helper's normal UART-routing defaults, asks the PIC to pulse WIO_RST
// (so the WIO gets a clean reset from any wedged state), then parks. No BOOT
// pin manipulation, no UART probes, no bootloader sync. Use when the WIO has
// fallen off the SWD bus and you want to recover before doing a clean unlock.
#define RECOVERY_ONLY      1
static_assert(!(SCOPE_TEST_ONLY && BRIDGE_VERIFY_ONLY),
              "Pick at most one mode");

#define WIO_TX_PIN 40
#define WIO_RX_PIN 23
#define I2C_SDA_PIN 26
#define I2C_SCL_PIN 27
#define IO_EXPANDER_ADDR 0x23
// WIO_BOOT lives on the same PCAL6524 the helper already uses (0x23, the
// Display IO expander on I2C1 sensor bus) at port 2 bit 1. The firmware
// enum names that bit HOTPLUG_DET in fw2IOExpanderDisplay.h; on this board
// rev it's actually wired to the WIO module's BOOT0 pin.
#define BOOT_EXPANDER_ADDR IO_EXPANDER_ADDR
#define BOOT_EXPANDER_P2_REG_OUT 0x06
#define BOOT_EXPANDER_P2_REG_CFG 0x0E
#define BOOT_EXPANDER_WIO_BOOT_MASK 0x02   // P2_1

// Baseline port 2 values the existing init_io_expander() writes — we OR/AND
// the WIO_BOOT bit into these so other bits (LED3, VREF selects, etc.) keep
// the value the helper expects for normal operation.
#define BOOT_EXPANDER_P2_OUT_BASE 0x85
#define BOOT_EXPANDER_P2_CFG_BASE 0x02
#define AT_BAUD 9600

// Bit-bang UART to PIC16: TX = GPIO 38 (PIC RX), RX = GPIO 39 (PIC TX), 62500 baud.
// Display CPU's hardware UARTs are uart0 (free here) / uart1 (taken by WIO above);
// GPIO 38/39 aren't on a uart-function pin pair on RP2350B, so bit-bang is simplest.
#define PIC_TX_PIN 38
#define PIC_RX_PIN 39
#define PIC_BIT_US 16    // 1/62500 = 16 µs/bit

#define PIC_CMD_HDR1 0xD0
#define PIC_CMD_HDR2 0xD5
#define PIC_CMD_WIO_PULSE 0x01
#define PIC_ACK_HDR1 0xD0
#define PIC_ACK_HDR2 0xD5
#define PIC_ACK_DONE 0x81

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

// PIC pulse path diagnostics — readable over SWD to debug the new turnkey flow.
#define PIC_PULSE_PENDING       0u
#define PIC_PULSE_BOOT_I2C_FAIL 1u   // setting WIO_BOOT high on expander 0x21 failed
#define PIC_PULSE_NO_ACK        2u   // PIC didn't ACK within timeout
#define PIC_PULSE_OK            3u
volatile uint32_t g_pic_pulse_status __attribute__((used)) = PIC_PULSE_PENDING;
volatile int32_t  g_pic_boot_rc1     __attribute__((used)) = 0;  // i2c_write rc for OUT reg
volatile int32_t  g_pic_boot_rc2     __attribute__((used)) = 0;  // i2c_write rc for CFG reg
volatile uint32_t g_pic_ack_byte_count __attribute__((used)) = 0;
volatile uint8_t  g_pic_ack_bytes[4]   __attribute__((used)) = {0};

// I2C bus scan results — 128 bits, one per 7-bit address. Bit set = ACK.
volatile uint32_t g_i2c_scan[4] __attribute__((used)) = {0, 0, 0, 0};

// Post-PIC-pulse diagnostic probes — exercise BOTH polarities to narrow down
// whether 0x23 P2_1 is WIO_BOOT and which direction enters the bootloader.
volatile uint32_t g_probe_at_lo_rxlen  __attribute__((used)) = 0;  // AT @ 9600  after BOOT=lo
volatile uint8_t  g_probe_at_lo_buf[16] __attribute__((used)) = {0};
volatile uint32_t g_probe_at_hi_rxlen  __attribute__((used)) = 0;  // AT @ 9600  after BOOT=hi
volatile uint8_t  g_probe_at_hi_buf[16] __attribute__((used)) = {0};
volatile uint32_t g_probe_bl_lo_rxlen  __attribute__((used)) = 0;  // 0x7F @ 115200 8-E-1 BOOT=lo
volatile uint8_t  g_probe_bl_lo_buf[16] __attribute__((used)) = {0};
volatile uint32_t g_probe_bl_hi_rxlen  __attribute__((used)) = 0;  // 0x7F @ 115200 8-E-1 BOOT=hi
volatile uint8_t  g_probe_bl_hi_buf[16] __attribute__((used)) = {0};

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

// ---- PIC link: bit-bang 8-N-1 @ 62500 baud on GPIO 38 (TX) / 39 (RX) ----

static void pic_uart_init(void) {
    gpio_init(PIC_TX_PIN);
    gpio_set_dir(PIC_TX_PIN, GPIO_OUT);
    gpio_put(PIC_TX_PIN, 1);             // idle high
    gpio_init(PIC_RX_PIN);
    gpio_set_dir(PIC_RX_PIN, GPIO_IN);
    gpio_pull_up(PIC_RX_PIN);
}

static void pic_tx_byte(uint8_t b) {
    gpio_put(PIC_TX_PIN, 0);             // start bit
    busy_wait_us(PIC_BIT_US);
    for (int i = 0; i < 8; i++) {
        gpio_put(PIC_TX_PIN, (b >> i) & 1);
        busy_wait_us(PIC_BIT_US);
    }
    gpio_put(PIC_TX_PIN, 1);             // stop bit
    busy_wait_us(PIC_BIT_US);
}

static int pic_rx_byte(uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    while (!time_reached(deadline)) {
        if (!gpio_get(PIC_RX_PIN)) {
            // Align to middle of bit 0: half bit time past the start edge,
            // then full bit times between samples.
            busy_wait_us(PIC_BIT_US + PIC_BIT_US / 2);
            uint8_t v = 0;
            for (int i = 0; i < 8; i++) {
                if (gpio_get(PIC_RX_PIN)) v |= (1u << i);
                busy_wait_us(PIC_BIT_US);
            }
            return v;
        }
    }
    return -1;
}

// Drive WIO_BOOT (0x23 P2_1) high/low. Read-modify-write semantics on top of
// the baseline port-2 values init_io_expander() leaves, so siblings keep their
// values. rc's surfaced via globals so SWD can find an i2c NACK.
static bool set_wio_boot(bool high) {
    uint8_t out_val = high
        ? (uint8_t)(BOOT_EXPANDER_P2_OUT_BASE |  BOOT_EXPANDER_WIO_BOOT_MASK)
        : (uint8_t)(BOOT_EXPANDER_P2_OUT_BASE & ~BOOT_EXPANDER_WIO_BOOT_MASK);
    uint8_t out_cmd[] = {BOOT_EXPANDER_P2_REG_OUT, out_val};
    int rc1 = i2c_write_blocking(i2c1, BOOT_EXPANDER_ADDR, out_cmd, sizeof(out_cmd), false);
    g_pic_boot_rc1 = rc1;
    // Clear P2_1 in the config register so the bit is driven as an output
    // (PCAL6524 config: 0 = output, 1 = input).
    uint8_t cfg_val = (uint8_t)(BOOT_EXPANDER_P2_CFG_BASE & ~BOOT_EXPANDER_WIO_BOOT_MASK);
    uint8_t cfg_cmd[] = {BOOT_EXPANDER_P2_REG_CFG, cfg_val};
    int rc2 = i2c_write_blocking(i2c1, BOOT_EXPANDER_ADDR, cfg_cmd, sizeof(cfg_cmd), false);
    g_pic_boot_rc2 = rc2;
    return rc1 == 2 && rc2 == 2;
}

// Ask the PIC to pulse WIO_RST. Caller is expected to have already set BOOT.
// Returns true iff the PIC ACK'd within 1 s (50 ms hold + 200 ms post-reset +
// budget for bit-bang round-trip).
static bool pic_pulse_wio(void) {
    pic_tx_byte(PIC_CMD_HDR1);
    busy_wait_us(5000);   // 5 ms inter-byte — fits inside the PIC's 1 ms RX poll
    pic_tx_byte(PIC_CMD_HDR2);
    busy_wait_us(5000);
    pic_tx_byte(PIC_CMD_WIO_PULSE);

    // Wait for the 3-byte ACK. PIC's pollBtns also TX's button reports
    // (header 0xC0 0xC5) every ~1.6 s, so filter on the 0xD0 0xD5 prefix.
    uint8_t state = 0;
    absolute_time_t deadline = make_timeout_time_ms(1000);
    g_pic_ack_byte_count = 0;
    while (!time_reached(deadline)) {
        int b = pic_rx_byte(50 * 1000);
        if (b < 0) continue;
        if (g_pic_ack_byte_count < sizeof(g_pic_ack_bytes)) {
            g_pic_ack_bytes[g_pic_ack_byte_count] = (uint8_t)b;
        }
        g_pic_ack_byte_count++;
        switch (state) {
            case 0: state = (b == PIC_ACK_HDR1) ? 1 : 0; break;
            case 1: state = (b == PIC_ACK_HDR2) ? 2 : 0; break;
            case 2:
                return (b == PIC_ACK_DONE);
            default: state = 0; break;
        }
    }
    return false;
}

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

#if RECOVERY_ONLY
    g_boot_mark = 0xC0FFEE70;
    pic_uart_init();
    sleep_ms(50);
    pic_pulse_wio();   // best-effort, status not checked — recovery is fire-and-forget
    g_boot_mark = 0xC0FFEE71;
    while (1) sleep_ms(1000);
#endif

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

    // Turnkey unlock path: drive WIO_BOOT high via expander 0x21, ask PIC to
    // pulse WIO_RST, and the WIO comes out of reset directly into the AN5482
    // UART system bootloader at 115200 8-E-1 — no AT firmware involvement.
    g_boot_mark = 0xC0FFEE20;
    pic_uart_init();

    // Scan all 7-bit addresses on i2c1 so SWD can see which expanders ACK.
    // 0-byte write is a no-op in the SDK — use a 1-byte read instead: the
    // address phase happens for real, and a NACK shows up as a negative rc.
    for (uint8_t a = 0; a < 128; a++) {
        uint8_t dummy = 0;
        int rc = i2c_read_blocking(i2c1, a, &dummy, 1, false);
        if (rc == 1) g_i2c_scan[a >> 5] |= (1u << (a & 31));
    }

    if (!set_wio_boot(true)) {
        g_pic_pulse_status = PIC_PULSE_BOOT_I2C_FAIL;
        goto park;
    }
    sleep_ms(10);

    if (!pic_pulse_wio()) {
        g_pic_pulse_status = PIC_PULSE_NO_ACK;
    } else {
        g_pic_pulse_status = PIC_PULSE_OK;
    }

    // Diagnostic 4-way probe — pulse WIO with BOOT=hi and BOOT=lo, and at each
    // pulse try AT @ 9600 and bootloader sync @ 115200 8-E-1. Whichever probe
    // gets bytes back tells us the correct BOOT polarity for this board rev.
    {
        auto probe_after_pulse = [&](bool boot_high,
                                      volatile uint32_t *at_len, volatile uint8_t *at_buf,
                                      volatile uint32_t *bl_len, volatile uint8_t *bl_buf) {
            char rxbuf[16];
            size_t rxlen;
            set_wio_boot(boot_high);
            sleep_ms(10);
            pic_pulse_wio();   // best-effort, status not re-stored
            sleep_ms(50);

            // AT @ 9600 8-N-1
            uart_set_baudrate(uart1, 9600);
            uart_set_format(uart1, 8, 1, UART_PARITY_NONE);
            wio_drain();
            wio_write("AT\r\n");
            rxlen = wio_read_until(rxbuf, sizeof(rxbuf), 500);
            *at_len = (uint32_t)rxlen;
            for (size_t i = 0; i < sizeof(rxbuf); i++) at_buf[i] = (i < rxlen) ? (uint8_t)rxbuf[i] : 0;

            // 0x7F @ 115200 8-E-1 — send REPEATEDLY (per AN5482, "send until ACK")
            // and capture up to 16 bytes across a 1 s window.
            sleep_ms(50);
            uart_set_baudrate(uart1, 115200);
            uart_set_format(uart1, 8, 1, UART_PARITY_EVEN);
            wio_drain();
            size_t collected = 0;
            absolute_time_t deadline = make_timeout_time_ms(1000);
            absolute_time_t next_tx = get_absolute_time();
            while (!time_reached(deadline) && collected < 16) {
                if (time_reached(next_tx)) {
                    unsigned char sync = 0x7F;
                    obLoRAComm.txData(&sync, 1);
                    next_tx = make_timeout_time_ms(50);   // ~20 syncs/sec
                }
                int avail = obLoRAComm.rxDataCount();
                if (avail > 0) {
                    int want = (int)(16 - collected);
                    if (want > avail) want = avail;
                    int got = obLoRAComm.rxReadData((unsigned char *)(rxbuf + collected), want);
                    if (got > 0) collected += (size_t)got;
                }
            }
            *bl_len = (uint32_t)collected;
            for (size_t i = 0; i < 16; i++) bl_buf[i] = (i < collected) ? (uint8_t)rxbuf[i] : 0;
        };

        probe_after_pulse(false, &g_probe_at_lo_rxlen, g_probe_at_lo_buf,
                                 &g_probe_bl_lo_rxlen, g_probe_bl_lo_buf);
        probe_after_pulse(true,  &g_probe_at_hi_rxlen, g_probe_at_hi_buf,
                                 &g_probe_bl_hi_rxlen, g_probe_bl_hi_buf);
    }

    g_boot_mark = 0xC0FFEE21;
    sleep_ms(50);
    wio_drain();
    uart_set_baudrate(uart1, 115200);
    uart_set_format(uart1, 8, 1, UART_PARITY_EVEN);

    // Bootloader sync: send 0x7F, expect 0x79 ACK. Wrapped in a block scope
    // so the earlier goto-park paths don't jump over `sync` / `unprot` inits.
    {
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
    }   // close the bootloader-sync scope

park:
    // Release WIO_BOOT so the next reset boots into the (now-flashable) user
    // flash, not back into the bootloader. Best-effort — if i2c fails here the
    // bridge SWD flash that follows will still write to address 0x08000000,
    // and the bridge code will run as long as BOOT is low at that next reset.
    (void)set_wio_boot(false);
    g_boot_mark = 0xC0FFEE60;
    while (1) {
        sleep_ms(1000);
    }
}
