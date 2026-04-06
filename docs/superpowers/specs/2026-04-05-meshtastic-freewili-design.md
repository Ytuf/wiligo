# Meshtastic on FreeWili 2 — Design Specification

**Date:** 2026-04-05
**Status:** Approved
**Hardware:** FreeWili 2 Rev 20 (SCH 3/11/2026)

## Goal

Port Meshtastic firmware to the FreeWili 2 hardware, producing a UF2 image that makes the device function like a LilyGo T-Deck Meshtastic node — with LoRa mesh networking, color display UI, touch input, button navigation, and audio alerts.

## System Architecture

A two-firmware solution running on two processors:

```
┌─────────────────────────────────────┐     ┌──────────────────────┐
│     Display RP2350B (UF2 #1)        │     │  WIO-E5 / STM32WL   │
│     "Meshtastic FreeWili"           │     │  (Binary #2)         │
│                                     │     │                      │
│  ┌─────────────┐  ┌──────────────┐  │UART │  ┌────────────────┐  │
│  │ Meshtastic  │  │ UART Radio   │──┼─────┼──│ Radio Bridge   │  │
│  │ Firmware    │  │ Interface    │  │     │  │ Firmware       │  │
│  │ (fork)      │  └──────────────┘  │     │  │                │  │
│  │             │                    │     │  │ UART <-> SUBGHZ│  │
│  │ Screen/UI   │  ┌──────────────┐  │     │  │ SPI proxy      │  │
│  │ LovyanGFX   │──│ ST7789       │  │     │  └────────────────┘  │
│  │ 480x320     │  │ SPI 50MHz    │  │     │         │            │
│  │             │  └──────────────┘  │     │    ┌────┴─────┐      │
│  │ InputBroker │  ┌──────────────┐  │     │    │ SX1262   │      │
│  │             │──│ PIC16 UART   │  │     │    │(internal)│      │
│  │             │  │ Button Input │  │     │    └──────────┘      │
│  │             │  └──────────────┘  │     │         │            │
│  │             │  ┌──────────────┐  │     │    ┌────┴─────┐      │
│  │             │──│ FT6336 Touch │  │     │    │ Antenna  │      │
│  │             │  │ I2C          │  │     │    └──────────┘      │
│  └─────────────┘  └──────────────┘  │     └──────────────────────┘
└─────────────────────────────────────┘
```

**Firmware #1 (Display RP2350B):** Fork of Meshtastic firmware, built with PlatformIO + Arduino-Pico (earlephilhower) for RP2350. New FreeWili variant. Outputs UF2.

**Firmware #2 (WIO-E5 STM32WLE5JC):** Small custom firmware that acts as a UART-to-SX1262 bridge. Receives radio commands over UART, executes them on the internal SX1262 via SUBGHZSPI using RadioLib's STM32WL support. Flashed via SWD.

## Hardware Summary

### FreeWili 2 Rev 20 Key Components

| Component | Chip/Module | Interface | RP2350B GPIOs |
|-----------|-------------|-----------|---------------|
| Main MCU (display) | RP2350B | — | — |
| Main MCU (system) | RP2350B | — | — |
| LoRa Radio | WIO-E5 (STM32WLE5JC + SX1262) | UART | TX→23, RX→32 |
| Display | ST7789 TFT 480x320 | SPI 50MHz | MOSI=11, SCLK=10, CS=9, DC=8 |
| Backlight | AP2502 LED driver | PWM | EN=25 |
| Touch | FT6336U capacitive | I2C (0x38) | SDA=26, SCL=27 |
| Buttons | PIC16F19195 matrix | UART (0xC0 0xC5 protocol) | RX=38, TX=39 |
| Audio Codec | NAU88C10 | I2C (0x1A) + I2S | DOUT=4, BCLK=7, LRCK=6 |
| IO Expander (display) | PCAL6524 | I2C (0x23) | on I2C1 bus |
| IO Expander (main) | PCAL6524 | I2C (0x22) | on main I2C bus |
| Sub-GHz radio (legacy) | CC1101 | SPI | shared with LoRa via antenna switch |
| WiFi/BLE | ESP32-C5 | UART | connected via WIO-E5 LPUART |
| Flash | 16MB | QSPI | — |
| SRAM | 64Mb | SPI | — |

### WIO-E5 Connections to Display RP2350B

| WIO-E5 Pin | STM32WL Function | Signal Name | RP2350B GPIO |
|------------|------------------|-------------|--------------|
| PB6 | UART1_TX | LoRA_SPI_CS | 23 |
| PB7 | UART1_RX | LoRA_PB7 / GDO_1 | 32 |
| PB5 | GPIO | WIO_GPIO | (via IO expander) |
| PC1 | LPUART1_TX | WIO_Tx | (to ESP32-C5) |
| PD0 | LPUART1_RX | WIO_Rx | (to ESP32-C5) |
| PA13 | SWDIO | WIO_SWDIO | (SWD debug) |
| PA14 | SWCLK | WIO_SWCLK | (SWD debug) |
| RST | Reset | WIO_RST | (via PIC16) |
| RFIO | RF I/O | WIO_ANT | (antenna, switched via LoRA_1101_SEL) |

### PIC16 Button Protocol

- **UART format:** 0xC0 0xC5 [LSB] [MSB] — 16-bit bitmap
- **Bit mapping:**

| Bit | Button | Bit | Button |
|-----|--------|-----|--------|
| 0 | Grey | 7 | Right |
| 1 | Yellow | 8 | Up |
| 2 | Green | 9 | Left |
| 3 | Blue | 10 | Home |
| 4 | Red | 11 | OK |
| 5 | Center | 12 | Cancel |
| 6 | Down | 13 | AI |

- **Features:** Debouncing, long press (3000ms), double-click (200us window), haptic/sound feedback

## Component Design

### 1. Meshtastic Variant Configuration

**Directory:** `variants/rp2350/freewili/`

**variant.h pin definitions:**

| Meshtastic Define | Value | FreeWili Signal |
|-------------------|-------|-----------------|
| `ST7789_SDA` | 11 | LCD_MOSI_D |
| `ST7789_SCK` | 10 | LCD_SCLK_D |
| `ST7789_CS` | 9 | LCD_CS_D |
| `ST7789_RS` | 8 | LCD_DC_D |
| `ST7789_BACKLIGHT_EN` | 25 | BLACKLIGHT_EN |
| `TFT_BACKLIGHT_ON` | HIGH | — |
| `SPI_FREQUENCY` | 40000000 | 40MHz (conservative) |
| `SCREEN_WIDTH` | 480 | — |
| `SCREEN_HEIGHT` | 320 | — |
| `I2C_SDA` | 26 | I2C1_SDA |
| `I2C_SCL` | 27 | I2C1_SCL |
| `TOUCH_CS` / addr | 0x38 | FT6336U |
| `USE_UART_RADIO` | 1 | selects UARTRadioInterface |
| `UART_RADIO_TX_PIN` | 32 | RP2350 TX → WIO-E5 PB7 (UART1_RX) |
| `UART_RADIO_RX_PIN` | 23 | RP2350 RX ← WIO-E5 PB6 (UART1_TX) |
| `PIC_UART_RX_PIN` | 38 | PIC_UART_RX |
| `PIC_UART_TX_PIN` | 39 | PIC_UART_TX |
| `BUTTON_PIN` | -1 | no direct GPIO button |

**platformio.ini:**
- Extends `rp2350_base`
- Board: `rpipico2` (or custom board definition for 16MB flash)
- Build flags: `-I variants/rp2350/freewili`, `-D FREEWILI`, `-D PRIVATE_HW`
- Custom `meshtastic_hw_model`: `PRIVATE_HW`

### 2. WIO-E5 Radio Bridge Firmware

**Purpose:** Accept radio commands over UART1, execute them on the internal SX1262 via RadioLib's STM32WL support, report results/interrupts back.

**Toolchain:** STM32CubeIDE (STM32WL HAL) + RadioLib (STM32WLE5JC SUBGHZSPI support)

**UART Configuration:** UART1 (PB6 TX / PB7 RX) at 115200 baud

**Protocol:**

Frame format:
```
[SYNC1=0xAA] [SYNC2=0x55] [CMD] [LEN_HI] [LEN_LO] [PAYLOAD...] [CRC8]
```

Commands (RP2350 -> WIO-E5):

| CMD | Name | Payload |
|-----|------|---------|
| 0x01 | RADIO_CONFIGURE | frequency(4B), bandwidth(1B), sf(1B), cr(1B), power(1B), preamble(2B), syncword(2B) |
| 0x02 | RADIO_TX | packet data (up to 256 bytes) |
| 0x03 | RADIO_RX_START | timeout(4B) |
| 0x04 | RADIO_SLEEP | — |
| 0x05 | RADIO_STANDBY | — |
| 0x06 | RADIO_GET_STATUS | — |
| 0x07 | RADIO_SET_DIO | DIO2_as_RF_switch(1B), DIO3_TCXO_voltage(1B) |

Responses (WIO-E5 -> RP2350):

| CMD | Name | Payload |
|-----|------|---------|
| 0x81 | ACK | original_cmd(1B), status(1B) |
| 0x82 | RX_PACKET | rssi(2B), snr(1B), data... |
| 0x83 | TX_DONE | — |
| 0x84 | STATUS | state(1B), rssi(2B), snr(1B) |
| 0x8F | ERROR | error_code(1B) |

**Implementation notes:**
- RadioLib handles SUBGHZSPI, DIO1 interrupt, BUSY polling internally for STM32WL
- Bridge firmware is interrupt-driven: SX1262 DIO1 fires on TX_DONE/RX_DONE, firmware sends appropriate response frame
- No RTOS, no filesystem — bare-metal state machine
- Estimated size: 500-800 lines of C, well within 256KB flash

**Reference implementations:**
- RadioLib STM32WL support (merged Dec 2023, PR #649)
- MeshCore Companion Radio protocol (architectural reference)
- Meshtastic native WIO-E5 variant (PR #1631, TCXO config reference)

### 3. UARTRadioInterface (RP2350 Side)

**New class:** `src/mesh/UARTRadioInterface.cpp/h`

**Superclass:** `RadioInterface` (directly, NOT `RadioLibInterface`)

**Key methods:**

| Method | Implementation |
|--------|---------------|
| `init()` | Open UART at 115200, send RADIO_SET_DIO (TCXO, RF switch), send RADIO_CONFIGURE with channel params |
| `reconfigure()` | Re-send RADIO_CONFIGURE when channel/region changes |
| `send(MeshPacket*)` | Serialize packet, send RADIO_TX frame, wait for TX_DONE |
| `startReceive()` | Send RADIO_RX_START frame |
| `sleep()` | Send RADIO_SLEEP frame |
| `isActivelyReceiving()` | Track from bridge state responses |
| `getPacketTime()` | Calculate airtime from radio params (same math as SX1262Interface) |

**Integration:** Selected by `initLoRa()` factory function when `USE_UART_RADIO` is defined in variant.h.

**UART processing:** Called from the Meshtastic main loop. Parses incoming frames, dispatches RX_PACKET to `deliverToReceiver()`, handles TX_DONE to unblock send queue.

### 4. PIC16 Button Input Source

**New class:** `src/input/PICButtonInput.cpp/h`

**Superclass:** Implements Meshtastic's input source pattern, registers with `InputBroker`

**Button mapping:**

| PIC16 Button | Meshtastic InputEvent |
|-------------|----------------------|
| NAV_UP (bit 8) | `UP` |
| NAV_DOWN (bit 6) | `DOWN` |
| NAV_LEFT (bit 9) | `LEFT` |
| NAV_RIGHT (bit 7) | `RIGHT` |
| NAV_CENTER (bit 5) | `SELECT` |
| OK (bit 11) | `SELECT` |
| CANCEL (bit 12) | `CANCEL` |
| HOME (bit 10) | `BACK` |
| RED (bit 4) | `FN1` |
| BLUE (bit 3) | `FN2` |
| GREEN (bit 2) | `FN3` |
| YELLOW (bit 1) | `FN4` |
| GREY (bit 0) | `FN5` |
| AI (bit 13) | custom / `FN5` |

**UART parsing:** State machine matching the PIC protocol — wait for 0xC0, then 0xC5, then read 2 bytes (LSB, MSB). Compare with previous state to detect press/release edges. Emit InputEvent on press.

### 5. Display Configuration

**Library:** LovyanGFX (already used by Meshtastic for T-Deck and other TFT targets)

**Panel config for FreeWili:**
- Controller: `Panel_ST7789`
- Bus: `Bus_SPI` on SPI0 (MOSI=11, SCLK=10, CS=9, DC=8)
- Resolution: 480x320
- Color depth: 16-bit RGB565
- Backlight: `Light_PWM` on GPIO 25
- Touch: `Touch_FT5x06` on I2C (SDA=26, SCL=27, addr=0x38)

**Integration:** Configured in `TFTDisplay.cpp` under `#ifdef FREEWILI` guard, following the existing pattern used by T-Deck and other ST7789 targets.

**Note:** The 480x320 resolution is larger than T-Deck's 320x240. Meshtastic's UI renders through an `OLEDDisplay` abstraction. The extra pixels give more room — font scaling or layout adjustments can come later as polish.

### 6. Antenna Switching

The FreeWili 2 has a shared antenna path between CC1101 and LoRa, controlled by `LoRA_1101_SEL` (visible on schematic page 15).

- During Meshtastic init, set `LoRA_1101_SEL` to select the LoRa path
- This is likely controlled via the IO Expander Main (PCAL6524 at I2C 0x22) or directly from the Display RP2350B
- Must be set before any radio operations

## Build System

### Repository Structure

```
wiligo/
├── meshtastic-firmware/              # Meshtastic fork (submodule or full fork)
│   └── variants/rp2350/freewili/
│       ├── variant.h
│       └── platformio.ini
│   └── src/mesh/
│       └── UARTRadioInterface.cpp/h
│   └── src/input/
│       └── PICButtonInput.cpp/h
│
├── wio-e5-bridge/                    # WIO-E5 bridge firmware
│   ├── CMakeLists.txt or Makefile
│   ├── src/
│   │   ├── main.c
│   │   ├── uart_protocol.c/h
│   │   └── radio_bridge.c/h
│   └── build/
│       └── wio-e5-bridge.bin
│
└── docs/
    └── superpowers/specs/
        └── 2026-04-05-meshtastic-freewili-design.md
```

### Build Outputs

| Output | Target | Method |
|--------|--------|--------|
| `firmware-freewili.uf2` | Display RP2350B | `pio run -e freewili` |
| `wio-e5-bridge.bin` | WIO-E5 STM32WLE5JC | STM32CubeIDE build or `make` |

### Flashing

- **RP2350B:** Hold BOOTSEL, plug USB, drag UF2 onto mass storage drive
- **WIO-E5:** One-time flash via SWD (ST-Link or J-Link) through WIO_SWDIO/WIO_SWCLK pads

## Implementation Phases

### Phase 1 — Radio Bridge (highest risk, do first)
1. WIO-E5 bridge firmware (RadioLib + STM32WL SUBGHZSPI + UART protocol)
2. UARTRadioInterface on RP2350
3. Headless test: verify LoRa TX/RX between FreeWili and another Meshtastic node
4. Antenna switching (LoRA_1101_SEL)

### Phase 2 — Display & UI
5. LovyanGFX ST7789 panel configuration for 480x320
6. Verify LovyanGFX compiles on Arduino-Pico RP2350
7. Touch integration (FT6336U via LovyanGFX Touch_FT5x06)
8. Meshtastic Screen/UI rendering on FreeWili display

### Phase 3 — Input & Polish
9. PIC16 button InputBroker source
10. Button-to-function mapping and navigation
11. Audio notifications (NAU88C10 speaker for message alerts)
12. Battery monitoring (fuel gauge I2C)
13. UI scaling/font adjustments for 480x320

## Risk Areas

| Risk | Severity | Mitigation |
|------|----------|------------|
| LovyanGFX on RP2350 Arduino-Pico | Medium | Fallback: TFT_eSPI or adapt FreeWili's existing st7789 driver |
| RadioLib STM32WL TCXO config | Medium | Reference Meshtastic issue #5991 and PR #1631 for correct DIO3 voltage |
| WIO-E5 flash size (256KB) | Low | Bridge firmware is minimal (~5-10KB), no filesystem needed |
| 480x320 display layout | Low | Meshtastic UI scales; cosmetic adjustments can come later |
| SPI bus contention | None | Display on SPI0, radio on UART — no contention |
| UART latency for radio | None | 256B at 115200 = 22ms; LoRa airtime is 50-200ms+ |

## References

- **Meshtastic firmware:** https://github.com/meshtastic/firmware
- **LilyGo T-Deck variant:** `variants/esp32s3/t-deck/variant.h`
- **Meshtastic RP2350 variant:** `variants/rp2350/rpipico2/variant.h`
- **Meshtastic WIO-E5 native (PR #1631):** https://github.com/meshtastic/firmware/pull/1631
- **RadioLib STM32WL support (PR #649):** https://github.com/jgromes/RadioLib/pull/649
- **MeshCore Companion Radio:** https://github.com/meshcore-dev/MeshCore (protocol reference)
- **FreeWili 2 schematic:** `Documentation/FreeWili_2_20.pdf` (Rev 20, 3/11/2026)
- **FreeWili firmware:** `freewili-firmware/freewilimain/` (existing drivers)
- **WIO-E5 datasheet:** https://files.seeedstudio.com/products/317990687/res/LoRa-E5%20module%20datasheet_V1.1.pdf
