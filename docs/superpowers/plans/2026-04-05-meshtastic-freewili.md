# Meshtastic FreeWili Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port Meshtastic firmware to FreeWili 2 hardware, producing a UF2 that functions like a LilyGo T-Deck mesh node.

**Architecture:** Two-firmware system — a Meshtastic fork on the Display RP2350B communicates with a small radio bridge firmware on the WIO-E5 (STM32WLE5JC) over UART. The bridge proxies radio commands to the internal SX1262 via RadioLib's STM32WL support.

**Tech Stack:** PlatformIO + Arduino-Pico (earlephilhower) for RP2350, STM32CubeIDE + RadioLib for WIO-E5, LovyanGFX for display, Meshtastic InputBroker for input.

**Spec:** `docs/superpowers/specs/2026-04-05-meshtastic-freewili-design.md`

---

## File Structure

### Meshtastic Fork (RP2350 side)

| File | Responsibility |
|------|---------------|
| `variants/rp2350/freewili/variant.h` | Pin mappings, feature flags, display/radio/input config |
| `variants/rp2350/freewili/platformio.ini` | Build environment extending rp2350_base |
| `src/mesh/UARTRadioInterface.h` | UART radio interface class declaration |
| `src/mesh/UARTRadioInterface.cpp` | UART radio interface implementation — send/receive/configure via serial protocol |
| `src/mesh/UARTRadioProtocol.h` | Shared protocol constants (sync bytes, command IDs, frame structure) — used by both RP2350 and WIO-E5 |
| `src/input/PICButtonInput.h` | PIC16 UART button input source declaration |
| `src/input/PICButtonInput.cpp` | PIC16 button parsing, debounce, InputBroker event emission |

### WIO-E5 Bridge Firmware

| File | Responsibility |
|------|---------------|
| `wio-e5-bridge/src/main.c` | Entry point, UART + radio init, main loop |
| `wio-e5-bridge/src/uart_protocol.h` | Frame parsing/building (mirrors UARTRadioProtocol.h constants) |
| `wio-e5-bridge/src/uart_protocol.c` | UART RX state machine, TX frame builder, CRC8 |
| `wio-e5-bridge/src/radio_bridge.h` | RadioLib SX1262 wrapper — configure, TX, RX, sleep |
| `wio-e5-bridge/src/radio_bridge.c` | RadioLib calls via SUBGHZSPI, interrupt handling |
| `wio-e5-bridge/CMakeLists.txt` | STM32CubeIDE build config |
| `wio-e5-bridge/platformio.ini` | Alternative PlatformIO build (STM32WL + Arduino) |

---

## Phase 0: Project Setup

### Task 1: Fork Meshtastic and Create FreeWili Variant Skeleton

**Files:**
- Create: `meshtastic-firmware/variants/rp2350/freewili/variant.h`
- Create: `meshtastic-firmware/variants/rp2350/freewili/platformio.ini`

- [ ] **Step 1: Clone Meshtastic firmware as a submodule**

```bash
cd wiligo
git clone https://github.com/meshtastic/firmware.git meshtastic-firmware
cd meshtastic-firmware
git submodule update --init --recursive
```

Note: This is a large repo. The submodule init pulls protobufs and other dependencies.

- [ ] **Step 2: Create variant directory**

```bash
mkdir -p variants/rp2350/freewili
```

- [ ] **Step 3: Create variant.h**

Create `variants/rp2350/freewili/variant.h`:

```cpp
#pragma once

#define PRIVATE_HW  // Community/DIY hardware designation

// --- Display (ST7789 TFT 480x320 via SPI0) ---
#define ST7789_CS 9
#define ST7789_RS 8          // DC pin
#define ST7789_SDA 11        // MOSI
#define ST7789_SCK 10
#define ST7789_MISO -1       // Not connected
#define ST7789_RESET -1      // Controlled via IO expander (SCREEN_nRST)
#define ST7789_BL 25         // Backlight enable (PWM)
#define TFT_BACKLIGHT_ON HIGH
#define TFT_WIDTH 480
#define TFT_HEIGHT 320
#define SPI_FREQUENCY 40000000
#define SCREEN_WIDTH TFT_WIDTH
#define SCREEN_HEIGHT TFT_HEIGHT
#define BRIGHTNESS_DEFAULT 200

// --- Touch (FT6336U via I2C) ---
#define TOUCH_SCREEN
#define HAS_TOUCHSCREEN 1
#define TOUCH_I2C_PORT 0
#define TOUCH_ADDRESS 0x38   // FT6336U / FT5x06 family

// --- I2C Bus ---
#define I2C_SDA 26
#define I2C_SCL 27

// --- Radio (UART to WIO-E5, NOT SPI) ---
#define USE_UART_RADIO 1
#define UART_RADIO_TX_PIN 32   // RP2350 TX -> WIO-E5 PB7 (UART1_RX)
#define UART_RADIO_RX_PIN 23   // RP2350 RX <- WIO-E5 PB6 (UART1_TX)
#define UART_RADIO_BAUD 115200

// Disable standard SPI radio defines
#undef USE_SX1262
#undef USE_SX1268
#undef USE_SX1280
#undef RF95_IRQ

// --- Buttons (PIC16 UART) ---
#define HAS_PIC_BUTTON_INPUT 1
#define PIC_UART_RX_PIN 38
#define PIC_UART_TX_PIN 39
#define PIC_UART_BAUD 9600   // PIC16 default baud

// No direct GPIO button
#define BUTTON_PIN -1

// --- Audio (NAU88C10 codec, optional) ---
#define I2S_DOUT 4
#define I2S_BCLK 7
#define I2S_LRCK 6
#define I2S_MCLK 22          // SPK_MCLK

// --- Power ---
// Battery monitored via I2C fuel gauge, not ADC
#define BATTERY_PIN -1

// --- LED ---
#define LED_PIN 21            // LED_SERIAL on display CPU
```

- [ ] **Step 4: Create platformio.ini**

Create `variants/rp2350/freewili/platformio.ini`:

```ini
[env:freewili]
extends = rp2350_base
board = rpipico2
board_level = extra
upload_protocol = picotool

build_flags =
  ${rp2350_base.build_flags}
  -D FREEWILI
  -D PRIVATE_HW
  -I variants/rp2350/freewili
  -D DEBUG_RP2040_PORT=Serial

debug_tool = cmsis-dap
```

- [ ] **Step 5: Verify the build compiles (expect radio init failure)**

```bash
cd meshtastic-firmware
pio run -e freewili
```

This will likely fail because `USE_UART_RADIO` is not yet recognized by the radio factory. That's expected — Task 5 will fix it. If it fails on other issues (missing includes, LovyanGFX), fix those first.

- [ ] **Step 6: Commit**

```bash
git add variants/rp2350/freewili/
git commit -m "feat: add FreeWili 2 variant skeleton for RP2350"
```

---

### Task 2: Set Up WIO-E5 Bridge Project

**Files:**
- Create: `wio-e5-bridge/platformio.ini`
- Create: `wio-e5-bridge/src/main.c`

We use PlatformIO with the STM32 Arduino core for the WIO-E5 bridge, since RadioLib supports this combination and it's simpler than raw STM32CubeIDE.

- [ ] **Step 1: Create project structure**

```bash
cd wiligo
mkdir -p wio-e5-bridge/src
```

- [ ] **Step 2: Create platformio.ini for WIO-E5**

Create `wio-e5-bridge/platformio.ini`:

```ini
[env:wio-e5-bridge]
platform = ststm32
board = lora_e5_mini
framework = arduino
monitor_speed = 115200

lib_deps =
  jgromes/RadioLib@^7.1.0

build_flags =
  -D RADIOLIB_EXCLUDE_SX127X=1
  -D RADIOLIB_EXCLUDE_SX128X=1
  -D RADIOLIB_EXCLUDE_LR11X0=1
  -D RADIOLIB_EXCLUDE_CC1101=1
  -D RADIOLIB_EXCLUDE_NRF24=1
  -D RADIOLIB_EXCLUDE_RF69=1
  -D RADIOLIB_EXCLUDE_SI443X=1
  -D RADIOLIB_EXCLUDE_AFSK=1
  -D RADIOLIB_EXCLUDE_AX25=1
  -D RADIOLIB_EXCLUDE_HELLSCHREIBER=1
  -D RADIOLIB_EXCLUDE_MORSE=1
  -D RADIOLIB_EXCLUDE_RTTY=1
  -D RADIOLIB_EXCLUDE_SSTV=1
  -D RADIOLIB_EXCLUDE_APRS=1
  -D RADIOLIB_EXCLUDE_PAGER=1
  -D RADIOLIB_EXCLUDE_LORAWAN=1
```

Note: The `lora_e5_mini` board definition may need adjustment for the WIO-E5 module. If PlatformIO doesn't have it, we may need a custom board JSON or use `genericSTM32WLE5JC`. Verify available boards with `pio boards | grep -i wle5`.

- [ ] **Step 3: Create minimal main.c to verify build**

Create `wio-e5-bridge/src/main.c`:

```cpp
#include <Arduino.h>

// WIO-E5 UART1 pins (to RP2350)
#define BRIDGE_UART_TX PB6
#define BRIDGE_UART_RX PB7

HardwareSerial BridgeSerial(BRIDGE_UART_RX, BRIDGE_UART_TX);

void setup() {
    BridgeSerial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);
}

void loop() {
    // Blink LED to confirm firmware is running
    digitalWrite(LED_BUILTIN, HIGH);
    delay(500);
    digitalWrite(LED_BUILTIN, LOW);
    delay(500);
}
```

- [ ] **Step 4: Verify build compiles**

```bash
cd wio-e5-bridge
pio run -e wio-e5-bridge
```

If the board definition doesn't exist, try `board = nucleo_wl55jc1` as a fallback and adjust pin definitions.

- [ ] **Step 5: Commit**

```bash
cd ..
git add wio-e5-bridge/
git commit -m "feat: add WIO-E5 bridge project skeleton"
```

---

## Phase 1: Radio Bridge

### Task 3: Shared UART Protocol Definition

**Files:**
- Create: `meshtastic-firmware/src/mesh/UARTRadioProtocol.h`
- Create: `wio-e5-bridge/src/uart_protocol.h`

Both sides need identical protocol constants. We define them in two separate files (different build systems) but keep them in sync.

- [ ] **Step 1: Create the protocol header for the Meshtastic side**

Create `meshtastic-firmware/src/mesh/UARTRadioProtocol.h`:

```cpp
#pragma once

#include <stdint.h>

// Frame sync bytes
#define UART_RADIO_SYNC1 0xAA
#define UART_RADIO_SYNC2 0x55

// Commands (Host -> Bridge)
#define CMD_RADIO_CONFIGURE   0x01
#define CMD_RADIO_TX          0x02
#define CMD_RADIO_RX_START    0x03
#define CMD_RADIO_SLEEP       0x04
#define CMD_RADIO_STANDBY     0x05
#define CMD_RADIO_GET_STATUS  0x06
#define CMD_RADIO_SET_DIO     0x07

// Responses (Bridge -> Host)
#define RSP_ACK               0x81
#define RSP_RX_PACKET         0x82
#define RSP_TX_DONE           0x83
#define RSP_STATUS            0x84
#define RSP_ERROR             0x8F

// Status codes
#define STATUS_OK             0x00
#define STATUS_ERR_UNKNOWN    0x01
#define STATUS_ERR_TIMEOUT    0x02
#define STATUS_ERR_CRC        0x03
#define STATUS_ERR_BUSY       0x04

// Radio states (in STATUS response)
#define RADIO_STATE_IDLE      0x00
#define RADIO_STATE_RX        0x01
#define RADIO_STATE_TX        0x02
#define RADIO_STATE_SLEEP     0x03

// Frame limits
#define UART_RADIO_MAX_PAYLOAD 256
#define UART_RADIO_HEADER_SIZE 5   // SYNC1 + SYNC2 + CMD + LEN_HI + LEN_LO
#define UART_RADIO_CRC_SIZE    1

// CONFIGURE payload offsets (14 bytes total)
#define CFG_FREQ_OFFSET    0   // uint32_t, Hz
#define CFG_BW_OFFSET      4   // uint8_t, encoded (0=125k, 1=250k, 2=500k)
#define CFG_SF_OFFSET      5   // uint8_t, 6-12
#define CFG_CR_OFFSET      6   // uint8_t, 5-8
#define CFG_POWER_OFFSET   7   // int8_t, dBm
#define CFG_PREAMBLE_OFFSET 8  // uint16_t
#define CFG_SYNCWORD_OFFSET 10 // uint16_t
#define CFG_PAYLOAD_SIZE   12

// SET_DIO payload (2 bytes)
#define DIO_RF_SWITCH_OFFSET  0  // uint8_t, 1=DIO2 as RF switch
#define DIO_TCXO_OFFSET       1  // uint8_t, TCXO voltage * 10 (e.g., 18 = 1.8V)
#define DIO_PAYLOAD_SIZE      2

// CRC8 (CCITT polynomial 0x07)
static inline uint8_t crc8(const uint8_t *data, uint16_t len) {
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ 0x07;
            else
                crc <<= 1;
        }
    }
    return crc;
}

// Build a protocol frame into buf. Returns total frame length.
static inline uint16_t uart_proto_build_frame(uint8_t *buf, uint8_t cmd,
                                               const uint8_t *payload, uint16_t payload_len) {
    buf[0] = UART_RADIO_SYNC1;
    buf[1] = UART_RADIO_SYNC2;
    buf[2] = cmd;
    buf[3] = (payload_len >> 8) & 0xFF;
    buf[4] = payload_len & 0xFF;
    if (payload_len > 0 && payload != NULL)
        memcpy(&buf[5], payload, payload_len);

    uint8_t crc_data[3 + UART_RADIO_MAX_PAYLOAD];
    crc_data[0] = cmd;
    crc_data[1] = buf[3];
    crc_data[2] = buf[4];
    if (payload_len > 0 && payload != NULL)
        memcpy(&crc_data[3], payload, payload_len);

    buf[5 + payload_len] = crc8(crc_data, 3 + payload_len);
    return UART_RADIO_HEADER_SIZE + payload_len + UART_RADIO_CRC_SIZE;
}
```

- [ ] **Step 2: Create matching header for WIO-E5 side**

Create `wio-e5-bridge/src/uart_protocol.h`:

```cpp
// Copy the exact same content as UARTRadioProtocol.h above.
// In a production setup, this would be a shared file or git submodule.
// For now, keep them manually in sync.
```

Copy the entire content of `UARTRadioProtocol.h` into this file verbatim.

- [ ] **Step 3: Commit**

```bash
git add meshtastic-firmware/src/mesh/UARTRadioProtocol.h wio-e5-bridge/src/uart_protocol.h
git commit -m "feat: define shared UART radio bridge protocol"
```

---

### Task 4: WIO-E5 Bridge — UART Frame Parser

**Files:**
- Create: `wio-e5-bridge/src/uart_protocol.c`

- [ ] **Step 1: Implement UART RX state machine and TX frame builder**

Create `wio-e5-bridge/src/uart_protocol.c`:

```cpp
#include "uart_protocol.h"
#include <string.h>

// RX state machine
typedef enum {
    RX_WAIT_SYNC1,
    RX_WAIT_SYNC2,
    RX_WAIT_CMD,
    RX_WAIT_LEN_HI,
    RX_WAIT_LEN_LO,
    RX_WAIT_PAYLOAD,
    RX_WAIT_CRC
} rx_state_t;

typedef struct {
    rx_state_t state;
    uint8_t cmd;
    uint16_t payload_len;
    uint16_t payload_idx;
    uint8_t payload[UART_RADIO_MAX_PAYLOAD];
} rx_context_t;

static rx_context_t rx_ctx;

// Returns 1 when a complete frame is parsed, 0 otherwise.
// On success, cmd/payload/payload_len are filled in the output params.
int uart_proto_parse_byte(uint8_t byte, uint8_t *out_cmd, uint8_t *out_payload, uint16_t *out_len) {
    switch (rx_ctx.state) {
    case RX_WAIT_SYNC1:
        if (byte == UART_RADIO_SYNC1)
            rx_ctx.state = RX_WAIT_SYNC2;
        break;

    case RX_WAIT_SYNC2:
        if (byte == UART_RADIO_SYNC2)
            rx_ctx.state = RX_WAIT_CMD;
        else
            rx_ctx.state = RX_WAIT_SYNC1;
        break;

    case RX_WAIT_CMD:
        rx_ctx.cmd = byte;
        rx_ctx.state = RX_WAIT_LEN_HI;
        break;

    case RX_WAIT_LEN_HI:
        rx_ctx.payload_len = (uint16_t)byte << 8;
        rx_ctx.state = RX_WAIT_LEN_LO;
        break;

    case RX_WAIT_LEN_LO:
        rx_ctx.payload_len |= byte;
        rx_ctx.payload_idx = 0;
        if (rx_ctx.payload_len > UART_RADIO_MAX_PAYLOAD) {
            rx_ctx.state = RX_WAIT_SYNC1; // frame too large, discard
        } else if (rx_ctx.payload_len == 0) {
            rx_ctx.state = RX_WAIT_CRC;
        } else {
            rx_ctx.state = RX_WAIT_PAYLOAD;
        }
        break;

    case RX_WAIT_PAYLOAD:
        rx_ctx.payload[rx_ctx.payload_idx++] = byte;
        if (rx_ctx.payload_idx >= rx_ctx.payload_len)
            rx_ctx.state = RX_WAIT_CRC;
        break;

    case RX_WAIT_CRC: {
        // Compute CRC over CMD + LEN_HI + LEN_LO + PAYLOAD
        uint8_t crc_buf[3 + UART_RADIO_MAX_PAYLOAD];
        crc_buf[0] = rx_ctx.cmd;
        crc_buf[1] = (rx_ctx.payload_len >> 8) & 0xFF;
        crc_buf[2] = rx_ctx.payload_len & 0xFF;
        if (rx_ctx.payload_len > 0)
            memcpy(&crc_buf[3], rx_ctx.payload, rx_ctx.payload_len);

        uint8_t expected_crc = crc8(crc_buf, 3 + rx_ctx.payload_len);
        rx_ctx.state = RX_WAIT_SYNC1;

        if (byte == expected_crc) {
            *out_cmd = rx_ctx.cmd;
            *out_len = rx_ctx.payload_len;
            if (rx_ctx.payload_len > 0)
                memcpy(out_payload, rx_ctx.payload, rx_ctx.payload_len);
            return 1; // frame complete
        }
        // CRC mismatch, discard
        break;
    }
    }
    return 0;
}

// Build a response frame into buf. Returns total frame length.
uint16_t uart_proto_build_frame(uint8_t *buf, uint8_t cmd, const uint8_t *payload, uint16_t payload_len) {
    buf[0] = UART_RADIO_SYNC1;
    buf[1] = UART_RADIO_SYNC2;
    buf[2] = cmd;
    buf[3] = (payload_len >> 8) & 0xFF;
    buf[4] = payload_len & 0xFF;
    if (payload_len > 0)
        memcpy(&buf[5], payload, payload_len);

    // CRC over CMD + LEN + PAYLOAD
    uint8_t crc_data[3 + UART_RADIO_MAX_PAYLOAD];
    crc_data[0] = cmd;
    crc_data[1] = buf[3];
    crc_data[2] = buf[4];
    if (payload_len > 0)
        memcpy(&crc_data[3], payload, payload_len);

    buf[5 + payload_len] = crc8(crc_data, 3 + payload_len);
    return UART_RADIO_HEADER_SIZE + payload_len + UART_RADIO_CRC_SIZE;
}

void uart_proto_reset(void) {
    rx_ctx.state = RX_WAIT_SYNC1;
}
```

- [ ] **Step 2: Build to verify compilation**

```bash
cd wio-e5-bridge
pio run -e wio-e5-bridge
```

- [ ] **Step 3: Commit**

```bash
git add wio-e5-bridge/src/uart_protocol.c
git commit -m "feat: implement UART protocol parser for WIO-E5 bridge"
```

---

### Task 5: WIO-E5 Bridge — RadioLib Integration and Main Loop

**Files:**
- Create: `wio-e5-bridge/src/radio_bridge.h`
- Create: `wio-e5-bridge/src/radio_bridge.cpp`
- Modify: `wio-e5-bridge/src/main.c` → rename to `main.cpp`

- [ ] **Step 1: Create radio bridge header**

Create `wio-e5-bridge/src/radio_bridge.h`:

```cpp
#pragma once

#include <stdint.h>

// Initialize RadioLib STM32WL radio
// Returns 0 on success, RadioLib error code on failure
int radio_bridge_init(void);

// Configure radio parameters from CONFIGURE command payload
int radio_bridge_configure(const uint8_t *payload, uint16_t len);

// Set DIO2 as RF switch, DIO3 TCXO voltage
int radio_bridge_set_dio(const uint8_t *payload, uint16_t len);

// Transmit a packet. Blocks until TX_DONE interrupt.
int radio_bridge_transmit(const uint8_t *data, uint16_t len);

// Start continuous receive mode
int radio_bridge_start_receive(void);

// Put radio to sleep
int radio_bridge_sleep(void);

// Put radio to standby
int radio_bridge_standby(void);

// Check if a packet was received (non-blocking)
// Returns packet length if available, 0 if not, negative on error
// Fills rssi and snr output params
int radio_bridge_check_rx(uint8_t *buf, uint16_t buf_size, float *rssi, float *snr);

// Check if TX is complete (non-blocking)
// Returns 1 if TX done, 0 if still transmitting
int radio_bridge_check_tx_done(void);
```

- [ ] **Step 2: Create radio bridge implementation**

Create `wio-e5-bridge/src/radio_bridge.cpp`:

```cpp
#include "radio_bridge.h"
#include "uart_protocol.h"
#include <RadioLib.h>

// STM32WL internal radio — RadioLib handles SUBGHZSPI internally
STM32WLx radio = new STM32WLx_Module();

// Radio state
static volatile bool tx_done_flag = false;
static volatile bool rx_done_flag = false;

// ISR callbacks
static void tx_done_isr(void) {
    tx_done_flag = true;
}

static void rx_done_isr(void) {
    rx_done_flag = true;
}

int radio_bridge_init(void) {
    // Initialize with default parameters (will be reconfigured later)
    // frequency=915.0 MHz, bandwidth=125 kHz, SF=9, CR=4/7, syncword=0x12, power=10 dBm
    int state = radio.begin(915.0, 125.0, 9, 7, 0x12, 10);
    if (state != RADIOLIB_ERR_NONE)
        return state;

    // Set DIO interrupt callbacks
    radio.setDio1Action(rx_done_isr);

    return RADIOLIB_ERR_NONE;
}

int radio_bridge_set_dio(const uint8_t *payload, uint16_t len) {
    if (len < DIO_PAYLOAD_SIZE)
        return -1;

    uint8_t rf_switch = payload[DIO_RF_SWITCH_OFFSET];
    uint8_t tcxo_raw = payload[DIO_TCXO_OFFSET];
    float tcxo_voltage = (float)tcxo_raw / 10.0f;

    int state = RADIOLIB_ERR_NONE;

    if (tcxo_voltage > 0) {
        state = radio.setTCXO(tcxo_voltage);
        if (state != RADIOLIB_ERR_NONE)
            return state;
    }

    if (rf_switch) {
        radio.setDio2AsRfSwitch(true);
    }

    return state;
}

int radio_bridge_configure(const uint8_t *payload, uint16_t len) {
    if (len < CFG_PAYLOAD_SIZE)
        return -1;

    uint32_t freq_hz;
    memcpy(&freq_hz, &payload[CFG_FREQ_OFFSET], 4);
    float freq_mhz = (float)freq_hz / 1000000.0f;

    uint8_t bw_code = payload[CFG_BW_OFFSET];
    float bw;
    switch (bw_code) {
        case 0: bw = 125.0; break;
        case 1: bw = 250.0; break;
        case 2: bw = 500.0; break;
        default: bw = 125.0; break;
    }

    uint8_t sf = payload[CFG_SF_OFFSET];
    uint8_t cr = payload[CFG_CR_OFFSET];
    int8_t power = (int8_t)payload[CFG_POWER_OFFSET];

    uint16_t preamble;
    memcpy(&preamble, &payload[CFG_PREAMBLE_OFFSET], 2);

    uint16_t syncword;
    memcpy(&syncword, &payload[CFG_SYNCWORD_OFFSET], 2);

    int state;

    state = radio.setFrequency(freq_mhz);
    if (state != RADIOLIB_ERR_NONE) return state;

    state = radio.setBandwidth(bw);
    if (state != RADIOLIB_ERR_NONE) return state;

    state = radio.setSpreadingFactor(sf);
    if (state != RADIOLIB_ERR_NONE) return state;

    state = radio.setCodingRate(cr);
    if (state != RADIOLIB_ERR_NONE) return state;

    state = radio.setOutputPower(power);
    if (state != RADIOLIB_ERR_NONE) return state;

    state = radio.setPreambleLength(preamble);
    if (state != RADIOLIB_ERR_NONE) return state;

    state = radio.setSyncWord(syncword & 0xFF, (syncword >> 8) & 0xFF);
    if (state != RADIOLIB_ERR_NONE) return state;

    return RADIOLIB_ERR_NONE;
}

int radio_bridge_transmit(const uint8_t *data, uint16_t len) {
    tx_done_flag = false;
    int state = radio.startTransmit((uint8_t *)data, len);
    return state;
}

int radio_bridge_check_tx_done(void) {
    if (tx_done_flag) {
        tx_done_flag = false;
        radio.finishTransmit();
        return 1;
    }
    return 0;
}

int radio_bridge_start_receive(void) {
    rx_done_flag = false;
    int state = radio.startReceive();
    return state;
}

int radio_bridge_check_rx(uint8_t *buf, uint16_t buf_size, float *rssi, float *snr) {
    if (!rx_done_flag)
        return 0;

    rx_done_flag = false;

    int len = radio.getPacketLength();
    if (len <= 0 || (uint16_t)len > buf_size)
        return -1;

    int state = radio.readData(buf, len);
    if (state != RADIOLIB_ERR_NONE)
        return -1;

    *rssi = radio.getRSSI();
    *snr = radio.getSNR();

    // Re-enter receive mode
    radio.startReceive();

    return len;
}

int radio_bridge_sleep(void) {
    return radio.sleep();
}

int radio_bridge_standby(void) {
    return radio.standby();
}
```

- [ ] **Step 3: Rewrite main.cpp with full bridge loop**

Rename `wio-e5-bridge/src/main.c` to `wio-e5-bridge/src/main.cpp` and replace its content:

```cpp
#include <Arduino.h>
#include "uart_protocol.h"
#include "radio_bridge.h"

// UART1 to RP2350 Display CPU
HardwareSerial BridgeSerial(PB7, PB6); // RX=PB7, TX=PB6

// Transmit buffer
static uint8_t tx_buf[UART_RADIO_HEADER_SIZE + UART_RADIO_MAX_PAYLOAD + UART_RADIO_CRC_SIZE];

// Send a response frame
static void send_response(uint8_t cmd, const uint8_t *payload, uint16_t len) {
    uint16_t frame_len = uart_proto_build_frame(tx_buf, cmd, payload, len);
    BridgeSerial.write(tx_buf, frame_len);
}

// Send ACK for a command
static void send_ack(uint8_t original_cmd, uint8_t status) {
    uint8_t payload[2] = { original_cmd, status };
    send_response(RSP_ACK, payload, 2);
}

// Send error
static void send_error(uint8_t error_code) {
    send_response(RSP_ERROR, &error_code, 1);
}

// Handle a parsed command frame
static void handle_command(uint8_t cmd, const uint8_t *payload, uint16_t len) {
    int result;

    switch (cmd) {
    case CMD_RADIO_SET_DIO:
        result = radio_bridge_set_dio(payload, len);
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;

    case CMD_RADIO_CONFIGURE:
        result = radio_bridge_configure(payload, len);
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        break;

    case CMD_RADIO_TX:
        result = radio_bridge_transmit(payload, len);
        send_ack(cmd, result == 0 ? STATUS_OK : STATUS_ERR_UNKNOWN);
        // TX_DONE will be sent asynchronously when interrupt fires
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
        // TODO: return actual state — for now, ACK
        send_ack(cmd, STATUS_OK);
        break;

    default:
        send_error(STATUS_ERR_UNKNOWN);
        break;
    }
}

void setup() {
    BridgeSerial.begin(UART_RADIO_BAUD);
    uart_proto_reset();

    int result = radio_bridge_init();
    if (result != 0) {
        // Radio init failed — blink LED rapidly to signal error
        pinMode(LED_BUILTIN, OUTPUT);
        while (1) {
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
            delay(100);
        }
    }
}

void loop() {
    // Process incoming UART bytes
    while (BridgeSerial.available()) {
        uint8_t byte = BridgeSerial.read();
        uint8_t cmd;
        uint8_t payload[UART_RADIO_MAX_PAYLOAD];
        uint16_t payload_len;

        if (uart_proto_parse_byte(byte, &cmd, payload, &payload_len)) {
            handle_command(cmd, payload, payload_len);
        }
    }

    // Check for async radio events
    if (radio_bridge_check_tx_done()) {
        send_response(RSP_TX_DONE, NULL, 0);
    }

    // Check for received packets
    uint8_t rx_buf[UART_RADIO_MAX_PAYLOAD];
    float rssi, snr;
    int rx_len = radio_bridge_check_rx(rx_buf, sizeof(rx_buf), &rssi, &snr);

    if (rx_len > 0) {
        // Build RX_PACKET response: rssi(2) + snr(1) + data
        uint8_t rsp_payload[3 + UART_RADIO_MAX_PAYLOAD];
        int16_t rssi_int = (int16_t)(rssi * 10); // RSSI * 10 for 0.1 dB resolution
        int8_t snr_int = (int8_t)(snr * 4);       // SNR * 4 for 0.25 dB resolution
        memcpy(&rsp_payload[0], &rssi_int, 2);
        rsp_payload[2] = (uint8_t)snr_int;
        memcpy(&rsp_payload[3], rx_buf, rx_len);
        send_response(RSP_RX_PACKET, rsp_payload, 3 + rx_len);
    }
}
```

- [ ] **Step 4: Build**

```bash
cd wio-e5-bridge
pio run -e wio-e5-bridge
```

Fix any compilation errors. Common issues:
- `STM32WLx_Module` constructor may differ across RadioLib versions — check RadioLib STM32WL examples
- `HardwareSerial` pin assignments may need `UART1` instance instead

- [ ] **Step 5: Commit**

```bash
git add wio-e5-bridge/src/
git commit -m "feat: implement WIO-E5 radio bridge with RadioLib STM32WL"
```

---

### Task 6: UARTRadioInterface — Meshtastic Side

**Files:**
- Create: `meshtastic-firmware/src/mesh/UARTRadioInterface.h`
- Create: `meshtastic-firmware/src/mesh/UARTRadioInterface.cpp`
- Modify: `meshtastic-firmware/src/mesh/RadioInterface.cpp` (add `USE_UART_RADIO` to factory)

- [ ] **Step 1: Create UARTRadioInterface header**

Create `meshtastic-firmware/src/mesh/UARTRadioInterface.h`:

```cpp
#pragma once

#include "RadioInterface.h"
#include "UARTRadioProtocol.h"
#include <HardwareSerial.h>

class UARTRadioInterface : public RadioInterface
{
public:
    UARTRadioInterface();
    virtual ~UARTRadioInterface();

    // RadioInterface pure virtuals
    virtual bool init() override;
    virtual ErrorCode send(meshtastic_MeshPacket *p) override;
    virtual bool reconfigure() override;
    virtual uint32_t getPacketTime(uint32_t totalPacketLen, bool received = false) override;

    // RadioInterface virtual overrides
    virtual void startReceive() override;
    virtual bool canSleep() override;
    virtual bool isActivelyReceiving() override;
    virtual bool isSending() override;

    // Call from main loop to process incoming UART data
    void processUART();

private:
    HardwareSerial *radioSerial;

    // UART RX parsing state (mirrors wio-e5-bridge parser)
    enum RxState { SYNC1, SYNC2, CMD, LEN_HI, LEN_LO, PAYLOAD, CRC_BYTE };
    RxState rxState = SYNC1;
    uint8_t rxCmd;
    uint16_t rxPayloadLen;
    uint16_t rxPayloadIdx;
    uint8_t rxPayload[UART_RADIO_MAX_PAYLOAD];

    // Radio state tracking
    bool txPending = false;
    bool receiving = false;
    uint8_t radioState = RADIO_STATE_IDLE;

    // Send a command frame to the bridge
    void sendCommand(uint8_t cmd, const uint8_t *payload = nullptr, uint16_t len = 0);

    // Send current radio config to bridge
    void sendRadioConfig();

    // Handle a response frame from the bridge
    void handleResponse(uint8_t cmd, const uint8_t *payload, uint16_t len);

    // Parse one byte from UART
    void parseByte(uint8_t byte);
};
```

- [ ] **Step 2: Create UARTRadioInterface implementation**

Create `meshtastic-firmware/src/mesh/UARTRadioInterface.cpp`:

```cpp
#include "UARTRadioInterface.h"
#include "MeshPacketPool.h"
#include "NodeDB.h"
#include "configuration.h"
#include <string.h>

UARTRadioInterface::UARTRadioInterface() : RadioInterface()
{
    radioSerial = nullptr;
}

UARTRadioInterface::~UARTRadioInterface()
{
}

bool UARTRadioInterface::init()
{
    LOG_INFO("UARTRadioInterface::init()");

#if defined(UART_RADIO_TX_PIN) && defined(UART_RADIO_RX_PIN)
    // Use Serial2 on RP2350 (UART1)
    Serial2.setTX(UART_RADIO_TX_PIN);
    Serial2.setRX(UART_RADIO_RX_PIN);
    Serial2.begin(UART_RADIO_BAUD);
    radioSerial = &Serial2;
#else
    LOG_ERROR("UART_RADIO pins not defined");
    return false;
#endif

    // Wait for bridge to boot
    delay(500);

    // Configure DIO2 as RF switch, DIO3 TCXO at 1.8V
    uint8_t dio_payload[DIO_PAYLOAD_SIZE];
    dio_payload[DIO_RF_SWITCH_OFFSET] = 1;
    dio_payload[DIO_TCXO_OFFSET] = 18; // 1.8V * 10
    sendCommand(CMD_RADIO_SET_DIO, dio_payload, DIO_PAYLOAD_SIZE);
    delay(100);

    // Send initial radio config
    sendRadioConfig();
    delay(100);

    // Start receiving
    startReceive();

    return true;
}

void UARTRadioInterface::sendCommand(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    if (!radioSerial) return;

    uint8_t frame[UART_RADIO_HEADER_SIZE + UART_RADIO_MAX_PAYLOAD + UART_RADIO_CRC_SIZE];
    uint16_t frame_len = uart_proto_build_frame(frame, cmd, payload, len);
    radioSerial->write(frame, frame_len);
}

void UARTRadioInterface::sendRadioConfig()
{
    uint8_t payload[CFG_PAYLOAD_SIZE];
    memset(payload, 0, sizeof(payload));

    // Get frequency from Meshtastic config
    uint32_t freq_hz = (uint32_t)(RadioInterface::getFreq() * 1000000.0f);
    memcpy(&payload[CFG_FREQ_OFFSET], &freq_hz, 4);

    // Bandwidth encoding
    uint8_t bw_code = 0; // 125kHz default
    if (bw == 250000) bw_code = 1;
    else if (bw == 500000) bw_code = 2;
    payload[CFG_BW_OFFSET] = bw_code;

    payload[CFG_SF_OFFSET] = sf;
    payload[CFG_CR_OFFSET] = cr;
    payload[CFG_POWER_OFFSET] = (uint8_t)power;

    uint16_t preamble = preambleLength;
    memcpy(&payload[CFG_PREAMBLE_OFFSET], &preamble, 2);

    uint16_t sw = 0x2B; // Meshtastic default syncword
    memcpy(&payload[CFG_SYNCWORD_OFFSET], &sw, 2);

    sendCommand(CMD_RADIO_CONFIGURE, payload, CFG_PAYLOAD_SIZE);
}

bool UARTRadioInterface::reconfigure()
{
    sendRadioConfig();
    delay(50);
    startReceive();
    return true;
}

ErrorCode UARTRadioInterface::send(meshtastic_MeshPacket *p)
{
    if (disabled || !config.lora.tx_enabled) {
        packetPool.release(p);
        return ERRNO_DISABLED;
    }

    // Serialize the MeshPacket to raw bytes for radio transmission
    size_t numbytes = pb_encode_to_bytes(radiobuf.bytes, sizeof(radiobuf.bytes),
                                         &meshtastic_MeshPacket_msg, p);

    sendingPacket = p;
    txPending = true;

    sendCommand(CMD_RADIO_TX, radiobuf.bytes, numbytes);

    return ERRNO_OK;
}

void UARTRadioInterface::startReceive()
{
    receiving = true;
    sendCommand(CMD_RADIO_RX_START);
}

bool UARTRadioInterface::canSleep()
{
    return !txPending && !sending;
}

bool UARTRadioInterface::isActivelyReceiving()
{
    return false; // We can't know this without polling the bridge
}

bool UARTRadioInterface::isSending()
{
    return txPending;
}

uint32_t UARTRadioInterface::getPacketTime(uint32_t totalPacketLen, bool received)
{
    // Calculate LoRa airtime using standard formula
    // Same math as SX126xInterface::getPacketTime
    float symbolTime = (float)(1 << sf) / (float)bw * 1000.0f; // ms
    float preambleTime = (preambleLength + 4.25f) * symbolTime;

    int payloadSymbols;
    int de = (sf >= 11) ? 1 : 0; // Low data rate optimization
    int ih = 0; // Implicit header off
    float num = 8.0f * totalPacketLen - 4.0f * sf + 28.0f + 16.0f - 20.0f * ih;
    float den = 4.0f * (sf - 2.0f * de);
    payloadSymbols = 8 + (int)ceil(num / den) * cr;
    if (payloadSymbols < 8) payloadSymbols = 8;

    float payloadTime = payloadSymbols * symbolTime;
    return (uint32_t)(preambleTime + payloadTime);
}

void UARTRadioInterface::processUART()
{
    if (!radioSerial) return;

    while (radioSerial->available()) {
        parseByte(radioSerial->read());
    }
}

void UARTRadioInterface::parseByte(uint8_t byte)
{
    switch (rxState) {
    case SYNC1:
        if (byte == UART_RADIO_SYNC1) rxState = SYNC2;
        break;
    case SYNC2:
        rxState = (byte == UART_RADIO_SYNC2) ? CMD : SYNC1;
        break;
    case CMD:
        rxCmd = byte;
        rxState = LEN_HI;
        break;
    case LEN_HI:
        rxPayloadLen = (uint16_t)byte << 8;
        rxState = LEN_LO;
        break;
    case LEN_LO:
        rxPayloadLen |= byte;
        rxPayloadIdx = 0;
        if (rxPayloadLen > UART_RADIO_MAX_PAYLOAD) {
            rxState = SYNC1;
        } else if (rxPayloadLen == 0) {
            rxState = CRC_BYTE;
        } else {
            rxState = PAYLOAD;
        }
        break;
    case PAYLOAD:
        rxPayload[rxPayloadIdx++] = byte;
        if (rxPayloadIdx >= rxPayloadLen) rxState = CRC_BYTE;
        break;
    case CRC_BYTE: {
        // Verify CRC
        uint8_t crc_data[3 + UART_RADIO_MAX_PAYLOAD];
        crc_data[0] = rxCmd;
        crc_data[1] = (rxPayloadLen >> 8) & 0xFF;
        crc_data[2] = rxPayloadLen & 0xFF;
        if (rxPayloadLen > 0)
            memcpy(&crc_data[3], rxPayload, rxPayloadLen);

        rxState = SYNC1;
        if (byte == crc8(crc_data, 3 + rxPayloadLen)) {
            handleResponse(rxCmd, rxPayload, rxPayloadLen);
        }
        break;
    }
    }
}

void UARTRadioInterface::handleResponse(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    switch (cmd) {
    case RSP_TX_DONE:
        txPending = false;
        if (sendingPacket) {
            // Notify mesh stack that TX is complete
            completeSending();
        }
        // Return to RX mode
        startReceive();
        break;

    case RSP_RX_PACKET: {
        if (len < 4) break; // minimum: rssi(2) + snr(1) + 1 byte data

        int16_t rssi_raw;
        memcpy(&rssi_raw, &payload[0], 2);
        int8_t snr_raw = (int8_t)payload[2];

        const uint8_t *pktData = &payload[3];
        uint16_t pktLen = len - 3;

        // Allocate a MeshPacket and decode the radio payload
        meshtastic_MeshPacket *mp = packetPool.allocZeroed();
        if (!mp) break;

        // Decode protobuf from radio bytes
        if (!pb_decode_from_bytes(pktData, pktLen, &meshtastic_MeshPacket_msg, mp)) {
            packetPool.release(mp);
            break;
        }

        mp->rx_rssi = (float)rssi_raw / 10.0f;
        mp->rx_snr = (float)snr_raw / 4.0f;

        // Deliver to mesh routing stack
        deliverToReceiver(mp);
        break;
    }

    case RSP_ACK:
        // Command acknowledged — could log but generally ignore
        break;

    case RSP_ERROR:
        LOG_ERROR("Radio bridge error: 0x%02x", len > 0 ? payload[0] : 0xFF);
        break;
    }
}
```

- [ ] **Step 3: Add USE_UART_RADIO to the radio factory**

Modify `meshtastic-firmware/src/mesh/RadioInterface.cpp`. Add this block at the **top** of the `initLoRa()` function, before any other `#ifdef USE_*` checks:

```cpp
#if defined(USE_UART_RADIO)
    #include "UARTRadioInterface.h"
#endif
```

And inside `initLoRa()`, add as the first radio check:

```cpp
#if defined(USE_UART_RADIO)
    rIf = std::unique_ptr<RadioInterface>(new UARTRadioInterface());
    return rIf;
#endif
```

This ensures `USE_UART_RADIO` takes priority and bypasses all SPI radio setup.

- [ ] **Step 4: Add processUART() call to the main loop**

Find the RP2040/RP2350 main loop in the Meshtastic firmware. The radio interface's UART processing needs to be called periodically. The cleanest approach is to make `UARTRadioInterface` extend `concurrency::OSThread` and override `runOnce()`, OR call `processUART()` from the existing loop.

For simplicity, add to `src/mesh/RadioInterface.h` a virtual method:

```cpp
virtual void serviceLoop() {}
```

And call it from the main loop in `src/main.cpp` (find the `loop()` function):

```cpp
// Add near other service calls in loop():
if (rIf)
    rIf->serviceLoop();
```

Then in `UARTRadioInterface.h`, override it:

```cpp
virtual void serviceLoop() override { processUART(); }
```

Note: The exact integration point depends on how the Meshtastic main loop is structured. Read `src/main.cpp`'s `loop()` function to find the right spot. The radio interface pointer is accessible via `router->getRadioInterface()` or a global.

- [ ] **Step 5: Build**

```bash
cd meshtastic-firmware
pio run -e freewili
```

Fix compilation errors. Expected issues:
- `pb_encode_to_bytes` / `pb_decode_from_bytes` — verify these are the correct protobuf helpers in Meshtastic (may be `MeshPacket_encode` / `MeshPacket_decode` or similar)
- `completeSending()` — this is a protected method on RadioInterface; verify access
- `radiobuf` — this is a global or member; verify its location
- `power` member variable — verify it exists on RadioInterface base class

- [ ] **Step 6: Commit**

```bash
git add src/mesh/UARTRadioInterface.h src/mesh/UARTRadioInterface.cpp src/mesh/RadioInterface.cpp
git commit -m "feat: add UARTRadioInterface for WIO-E5 bridge communication"
```

---

### Task 7: Integration Test — Headless Radio TX/RX

**Files:** No new files — this is a hardware test.

- [ ] **Step 1: Flash WIO-E5 bridge firmware via SWD**

Connect ST-Link or J-Link to WIO_SWDIO (PA13) and WIO_SWCLK (PA14) on the FreeWili board.

```bash
cd wio-e5-bridge
pio run -e wio-e5-bridge --target upload
```

If using ST-Link via OpenOCD, the upload protocol in `platformio.ini` may need:
```ini
upload_protocol = stlink
debug_tool = stlink
```

- [ ] **Step 2: Flash Meshtastic FreeWili firmware to RP2350B**

Put the Display RP2350B into bootloader mode (hold BOOTSEL, plug USB).

```bash
cd meshtastic-firmware
pio run -e freewili --target upload
```

Or manually copy the UF2 file from `.pio/build/freewili/firmware.uf2` to the USB mass storage drive.

- [ ] **Step 3: Set up a second Meshtastic node for testing**

Use any existing Meshtastic device (phone app + T-Beam, another Pico with SX1262, etc.) on the same region/channel. Open the Meshtastic serial console on the FreeWili:

```bash
pio device monitor -e freewili -b 115200
```

- [ ] **Step 4: Verify radio initialization**

In the serial output, look for:
```
UARTRadioInterface::init()
```
Without any `Radio bridge error` messages. If you see errors, check:
- UART TX/RX pin connections (GPIO 23/32 to WIO-E5 PB6/PB7)
- WIO-E5 bridge LED blinking (rapid = init failure)
- TCXO voltage setting (1.8V for LoRa-E5)

- [ ] **Step 5: Verify mesh packet exchange**

From the second node, send a message. Watch the FreeWili serial console for received packet logs. Send a message from FreeWili and verify the second node receives it.

- [ ] **Step 6: Commit any fixes**

```bash
git add -u
git commit -m "fix: radio bridge integration fixes from hardware testing"
```

---

## Phase 2: Display & UI

### Task 8: LovyanGFX ST7789 Display Configuration

**Files:**
- Modify: `meshtastic-firmware/src/graphics/TFTDisplay.cpp` (add `#ifdef FREEWILI` block)

- [ ] **Step 1: Add FreeWili LGFX configuration**

In `meshtastic-firmware/src/graphics/TFTDisplay.cpp`, find the section where `class LGFX` is defined (look for `#if defined(ST7789_CS)`). Add a new `#elif defined(FREEWILI)` block:

```cpp
#elif defined(FREEWILI)

class LGFX : public lgfx::LGFX_Device
{
    lgfx::Bus_SPI _bus_instance;
    lgfx::Panel_ST7789 _panel_instance;
    lgfx::Light_PWM _light_instance;
    lgfx::Touch_FT5x06 _touch_instance;

public:
    LGFX(void)
    {
        // SPI Bus
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host = 0;           // SPI0 on RP2350
            cfg.spi_mode = 0;
            cfg.freq_write = SPI_FREQUENCY;
            cfg.freq_read = 16000000;
            cfg.pin_sclk = ST7789_SCK;  // GPIO 10
            cfg.pin_mosi = ST7789_SDA;  // GPIO 11
            cfg.pin_miso = -1;
            cfg.pin_dc = ST7789_RS;     // GPIO 8
            cfg.pin_cs = ST7789_CS;     // GPIO 9
            cfg.dma_channel = -1;       // Auto-select DMA channel
            _bus_instance.config(cfg);
        }
        _panel_instance.setBus(&_bus_instance);

        // Panel
        {
            auto cfg = _panel_instance.config();
            cfg.pin_rst = -1;           // Reset via IO expander
            cfg.pin_busy = -1;
            cfg.panel_width = TFT_WIDTH;   // 480
            cfg.panel_height = TFT_HEIGHT; // 320
            cfg.offset_rotation = 0;
            cfg.readable = false;
            cfg.invert = true;          // ST7789 typically needs inversion
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel_instance.config(cfg);
        }

        // Backlight
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl = ST7789_BL;     // GPIO 25
            cfg.invert = false;
            cfg.freq = 44100;
            cfg.pwm_channel = 0;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        // Touch (FT6336U / FT5x06 family)
        {
            auto cfg = _touch_instance.config();
            cfg.pin_int = -1;           // No interrupt pin routed
            cfg.pin_rst = -1;
            cfg.i2c_port = TOUCH_I2C_PORT;
            cfg.i2c_addr = TOUCH_ADDRESS;  // 0x38
            cfg.freq = 400000;
            cfg.x_min = 0;
            cfg.x_max = TFT_WIDTH - 1;
            cfg.y_min = 0;
            cfg.y_max = TFT_HEIGHT - 1;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }

        setPanel(&_panel_instance);
    }
};
```

- [ ] **Step 2: Verify the display configuration is selected during build**

Ensure the `#elif defined(FREEWILI)` block is placed before any generic `#elif defined(ST7789_CS)` fallback, so it takes priority.

- [ ] **Step 3: Build and test**

```bash
cd meshtastic-firmware
pio run -e freewili
```

Flash to RP2350B and verify:
- Backlight turns on
- Meshtastic boot screen appears
- Touch responds (if touch connector is attached)

If the display is inverted, blank, or wrong colors, adjust:
- `cfg.invert` (try `false`)
- `cfg.rgb_order` (try `true`)
- `cfg.offset_rotation` (try 1, 2, or 3)

- [ ] **Step 4: Commit**

```bash
git add src/graphics/TFTDisplay.cpp
git commit -m "feat: add FreeWili LovyanGFX ST7789 480x320 display config"
```

---

### Task 9: Antenna Switching

**Files:**
- Modify: `meshtastic-firmware/variants/rp2350/freewili/variant.h` (add IO expander init note)
- May need: new init code in variant.cpp or platform init

- [ ] **Step 1: Determine antenna switch control**

From the schematic (page 15), `LoRA_1101_SEL` switches between CC1101 and LoRa antennas. Find which IO expander pin or GPIO controls this by:
1. Tracing `LoRA_1101_SEL` in the schematic
2. Checking `fw2IOExpanderMain.h` / `fwIOExpand.h` in the FreeWili firmware for the pin mapping

The signal likely goes through IO Expander Main (PCAL6524 at I2C 0x22). During Meshtastic init, we need to set this pin to select the LoRa antenna.

- [ ] **Step 2: Add IO expander init to variant**

Create `meshtastic-firmware/variants/rp2350/freewili/variant.cpp`:

```cpp
#include "variant.h"
#include <Wire.h>

// PCAL6524 IO Expander addresses
#define IO_EXPANDER_MAIN_ADDR 0x22

// Set LoRA_1101_SEL to select LoRa antenna path
// The exact register/bit depends on schematic tracing — adjust as needed
void initVariantHardware()
{
    Wire.begin();

    // Configure IO expander to select LoRa antenna
    // PCAL6524 output register for the antenna switch pin
    // TODO: Replace with exact register/bit once schematic page 15 is traced
    // Example: set Port 1, Bit 6 high for LoRa
    Wire.beginTransmission(IO_EXPANDER_MAIN_ADDR);
    Wire.write(0x04); // Output port 0 register (PCAL6524)
    Wire.write(0x00); // Value — adjust bit for antenna select
    Wire.endTransmission();
}
```

Note: The exact register and bit position must be determined from the FreeWili schematic page 15 and the PCAL6524 datasheet. This is a placeholder that needs hardware verification.

- [ ] **Step 3: Hook into Meshtastic init**

Ensure `initVariantHardware()` is called during startup. Meshtastic calls `initVariant()` from `src/platform/rp2xx0/main-rp2xx0.cpp` early in boot. Add a call to `initVariantHardware()` there, guarded by `#ifdef FREEWILI`.

- [ ] **Step 4: Build, flash, and test with an antenna analyzer or by verifying radio range**

```bash
pio run -e freewili --target upload
```

- [ ] **Step 5: Commit**

```bash
git add variants/rp2350/freewili/variant.cpp
git commit -m "feat: add antenna switch init for LoRa path selection"
```

---

## Phase 3: Input & Polish

### Task 10: PIC16 Button Input Source

**Files:**
- Create: `meshtastic-firmware/src/input/PICButtonInput.h`
- Create: `meshtastic-firmware/src/input/PICButtonInput.cpp`
- Modify: `meshtastic-firmware/src/input/InputBroker.cpp` (register source)

- [ ] **Step 1: Create PICButtonInput header**

Create `meshtastic-firmware/src/input/PICButtonInput.h`:

```cpp
#pragma once

#include "InputBroker.h"
#include "concurrency/OSThread.h"
#include <HardwareSerial.h>

class PICButtonInput : public Observable<const InputEvent *>, public concurrency::OSThread
{
public:
    PICButtonInput();
    void init();

protected:
    int32_t runOnce() override;

private:
    HardwareSerial *picSerial;

    // PIC UART protocol state machine
    enum PicRxState { WAIT_HEADER1, WAIT_HEADER2, WAIT_LSB, WAIT_MSB };
    PicRxState picState = WAIT_HEADER1;

    // Previous button state for edge detection
    uint16_t prevButtons = 0;

    // PIC protocol constants
    static const uint8_t PIC_SYNC1 = 0xC0;
    static const uint8_t PIC_SYNC2 = 0xC5;

    // Button bit positions
    static const uint16_t BTN_GREY    = (1 << 0);
    static const uint16_t BTN_YELLOW  = (1 << 1);
    static const uint16_t BTN_GREEN   = (1 << 2);
    static const uint16_t BTN_BLUE    = (1 << 3);
    static const uint16_t BTN_RED     = (1 << 4);
    static const uint16_t BTN_CENTER  = (1 << 5);
    static const uint16_t BTN_DOWN    = (1 << 6);
    static const uint16_t BTN_RIGHT   = (1 << 7);
    static const uint16_t BTN_UP      = (1 << 8);
    static const uint16_t BTN_LEFT    = (1 << 9);
    static const uint16_t BTN_HOME    = (1 << 10);
    static const uint16_t BTN_OK      = (1 << 11);
    static const uint16_t BTN_CANCEL  = (1 << 12);
    static const uint16_t BTN_AI      = (1 << 13);

    uint8_t lsb = 0;

    void emitEvent(input_broker_event event);
    void processButtonChange(uint16_t buttons);
};
```

- [ ] **Step 2: Create PICButtonInput implementation**

Create `meshtastic-firmware/src/input/PICButtonInput.cpp`:

```cpp
#include "PICButtonInput.h"
#include "configuration.h"

PICButtonInput::PICButtonInput() : concurrency::OSThread("PICButton")
{
    picSerial = nullptr;
}

void PICButtonInput::init()
{
#if defined(PIC_UART_RX_PIN) && defined(PIC_UART_TX_PIN)
    // Use Serial1 (UART0) for PIC communication
    Serial1.setRX(PIC_UART_RX_PIN);
    Serial1.setTX(PIC_UART_TX_PIN);
    Serial1.begin(PIC_UART_BAUD);
    picSerial = &Serial1;
    LOG_INFO("PICButtonInput initialized on RX=%d TX=%d", PIC_UART_RX_PIN, PIC_UART_TX_PIN);
#endif
}

int32_t PICButtonInput::runOnce()
{
    if (!picSerial) return 50;

    while (picSerial->available()) {
        uint8_t byte = picSerial->read();

        switch (picState) {
        case WAIT_HEADER1:
            if (byte == PIC_SYNC1) picState = WAIT_HEADER2;
            break;
        case WAIT_HEADER2:
            picState = (byte == PIC_SYNC2) ? WAIT_LSB : WAIT_HEADER1;
            break;
        case WAIT_LSB:
            lsb = byte;
            picState = WAIT_MSB;
            break;
        case WAIT_MSB: {
            uint16_t buttons = lsb | ((uint16_t)byte << 8);
            processButtonChange(buttons);
            prevButtons = buttons;
            picState = WAIT_HEADER1;
            break;
        }
        }
    }

    return 20; // Poll every 20ms
}

void PICButtonInput::emitEvent(input_broker_event event)
{
    InputEvent e;
    e.source = "picbutton";
    e.inputEvent = event;
    e.kbchar = 0;
    e.touchX = 0;
    e.touchY = 0;
    notifyObservers(&e);
}

void PICButtonInput::processButtonChange(uint16_t buttons)
{
    // Detect newly pressed buttons (transition from 0 to 1)
    uint16_t pressed = buttons & ~prevButtons;

    if (pressed & BTN_UP)      emitEvent(INPUT_BROKER_UP);
    if (pressed & BTN_DOWN)    emitEvent(INPUT_BROKER_DOWN);
    if (pressed & BTN_LEFT)    emitEvent(INPUT_BROKER_LEFT);
    if (pressed & BTN_RIGHT)   emitEvent(INPUT_BROKER_RIGHT);
    if (pressed & BTN_CENTER)  emitEvent(INPUT_BROKER_SELECT);
    if (pressed & BTN_OK)      emitEvent(INPUT_BROKER_SELECT);
    if (pressed & BTN_CANCEL)  emitEvent(INPUT_BROKER_CANCEL);
    if (pressed & BTN_HOME)    emitEvent(INPUT_BROKER_BACK);
    if (pressed & BTN_RED)     emitEvent(INPUT_BROKER_FN_F1);
    if (pressed & BTN_BLUE)    emitEvent(INPUT_BROKER_FN_F2);
    if (pressed & BTN_GREEN)   emitEvent(INPUT_BROKER_FN_F3);
    if (pressed & BTN_YELLOW)  emitEvent(INPUT_BROKER_FN_F4);
    if (pressed & BTN_GREY)    emitEvent(INPUT_BROKER_FN_F5);
    if (pressed & BTN_AI)      emitEvent(INPUT_BROKER_FN_F5);
}
```

- [ ] **Step 3: Register PICButtonInput in InputBroker**

Modify `meshtastic-firmware/src/input/InputBroker.cpp`, in the `Init()` method. Add at the end of the function:

```cpp
#if defined(HAS_PIC_BUTTON_INPUT)
    static PICButtonInput *picButtons = new PICButtonInput();
    picButtons->init();
    registerSource(picButtons);
#endif
```

Add the include at the top of the file:

```cpp
#if defined(HAS_PIC_BUTTON_INPUT)
#include "PICButtonInput.h"
#endif
```

- [ ] **Step 4: Build and test**

```bash
cd meshtastic-firmware
pio run -e freewili --target upload
```

Press DPAD buttons and verify:
- Navigation through Meshtastic menu
- Center/OK selects items
- Cancel/Home goes back
- Color buttons trigger F1-F5 events

- [ ] **Step 5: Commit**

```bash
git add src/input/PICButtonInput.h src/input/PICButtonInput.cpp src/input/InputBroker.cpp
git commit -m "feat: add PIC16 UART button input for FreeWili navigation"
```

---

### Task 11: Audio Notifications (Optional)

**Files:**
- Modify: `meshtastic-firmware/variants/rp2350/freewili/variant.h` (add buzzer/audio defines)

- [ ] **Step 1: Enable Meshtastic buzzer support**

Meshtastic has a buzzer/notification module. In `variant.h`, add:

```cpp
// Audio notification via I2S speaker
// Meshtastic's ExternalNotificationModule can drive a GPIO for alerts
#define EXT_NOTIFY_OUT 20  // Or use a spare GPIO connected to an amplifier enable
```

The full I2S audio integration with the NAU88C10 codec is a larger effort that can be deferred. For basic alerts, a simple GPIO toggle driving a buzzer or the amplifier enable is sufficient.

- [ ] **Step 2: Build and test**

```bash
pio run -e freewili --target upload
```

Send a message to the FreeWili node and verify the external notification pin toggles.

- [ ] **Step 3: Commit**

```bash
git add variants/rp2350/freewili/variant.h
git commit -m "feat: enable audio notification output for FreeWili"
```

---

## Verification Checklist

After all tasks are complete, verify end-to-end:

- [ ] FreeWili boots and shows Meshtastic UI on the 480x320 display
- [ ] Touch input navigates menus
- [ ] DPAD and color buttons navigate menus
- [ ] LoRa radio sends and receives mesh packets
- [ ] FreeWili appears as a node in the Meshtastic phone app (via USB serial or BLE if ESP32-C5 is configured)
- [ ] Device can be flashed via UF2 drag-and-drop
- [ ] WIO-E5 bridge firmware survives power cycles (flashed to internal flash)
