# Meshtastic FreeWili Port — Handoff Document

## Project Summary

Porting Meshtastic firmware to FreeWili 2 hardware (RP2350B Display CPU + WIO-E5 LoRa radio). Design spec at `docs/superpowers/specs/2026-04-05-meshtastic-freewili-design.md`, implementation plan at `docs/superpowers/plans/2026-04-05-meshtastic-freewili.md`.

## Current Status: Display working, touch input blocked on I2C NACK

Meshtastic UI renders on ST7789 with no faults. UART radio initialized (split across Serial1/Serial2). Touch controller (FT5316 at 0x38) confirmed present but NACKs during polling despite TOUCH_INT gating. PIC buttons disabled.

## What Works

1. **ST7789 display** — Meshtastic UI renders in 480x320 landscape. Custom Pico SDK LGFX wrapper in TFTDisplay.cpp manages SPI1 directly (LovyanGFX bypassed).

2. **RP2350B board definition** — Custom `freewili2` board (`boards/freewili2.json`) with `PICO_RP2350A=0`. Variant at `.platformio/packages/framework-arduinopico/variants/freewili2/pins_arduino.h`. Enables GPIO 0-47.

3. **IO expander (PCAL6524 at I2C1 0x23)** — Initialized in `initVariant()` via Pico SDK. Sets SPI buffer directions, SCREEN_nRST, backlight GPIO direction.

4. **Wire routed to i2c1** — `-D__WIRE0_DEVICE=i2c1` build flag. `I2C_SDA=26`, `I2C_SCL=27`.

5. **Split-UART radio** — GPIO 32 (UART0_TX via Serial1) + GPIO 23 (UART1_RX via Serial2). Controlled by `USE_SPLIT_UART_RADIO`. WIO-E5 bridge not yet flashed.

6. **SPI0 LoRa init guarded** — `#if defined(FREEWILI)` skip in main.cpp prevents unnecessary SPI0 init.

## Bugs Found and Fixed (2026-04-06/07)

| # | Bug | Root Cause | Fix |
|---|-----|-----------|-----|
| 1 | `Wire.setSDA(26)` panic | GPIO 26/27 = i2c1, Wire = i2c0 | `-D__WIRE0_DEVICE=i2c1` build flag |
| 2 | I2C hang on default pins 4/5 | `HAS_WIRE` fallback with no devices | Define `I2C_SDA`/`I2C_SCL` to prevent fallback |
| 3 | `Serial1.setRX(38)` panic | Pin 38 exceeds RP2350A limit | Created RP2350B board (`PICO_RP2350A=0`) |
| 4 | LittleFS flash assert | Board JSON had 32MB, LFS offset OOB | Set `maximum_size` to 4MB |
| 5 | `Serial2.setTX(32)` panic | GPIO 32 = UART0_TX, not UART1 | Split UART: Serial1 TX + Serial2 RX |
| 6 | PIC `Serial1.setRX(38)` panic | Pin 38 = UART1_RX, not UART0 | Disabled PIC buttons (needs SerialPIO) |

## Touch Screen — Current Blocker

### What we know from schematic (`../../Documentation/FreeWili_2_20.pdf`)
- **FT5316** (FT5x06 family, chip ID 0x11) at I2C address 0x38
- On **Display CPU's I2C1 bus** (GPIO 26/27) — **NOT shared** with Main CPU
- Main CPU has separate I2C1_M bus (SDAM1/SCLM1) with NS2009 at 0x2A
- **TOUCH_INT = GPIO31** on Display RP2350B (page 3 of schematic)
- SCREEN_nRST (IO expander controlled) may also reset the touch controller

### What we tried
1. **Direct I2C polling in getTouch()** — reads registers 0x02-0x06 from 0x38. Gets NACK (error 2) during normal operation.
2. **lateInitVariant() probe** — ACKs successfully, chip ID register 0xA8 returns 0x11 (FT5316). Touch controller IS alive at this point.
3. **TOUCH_INT gating** — configured GPIO31 as input with pull-up, only read I2C when INT is LOW. Still no touch response from UI.
4. **Backlight flicker test** — `sleep_ms(30)` in getTouch with gpio_put backlight toggle. Worked a couple of times then stopped (sleep blocking killed I2C).

### What to investigate next
- **Is TOUCH_INT actually going LOW when touched?** Read GPIO31 via GDB while touching. If it stays HIGH, the touch controller may need additional initialization or a hardware reset toggle.
- **Check the original FreeWili firmware's touch init sequence** in `../freewili-firmware/freewilimain/` — there may be a required power-on or reset sequence.
- **I2C bus recovery** — the repeated NACK polling may be locking the I2C bus. Try `i2c_deinit(i2c1) + i2c_init(i2c1, ...)` before touch reads.
- **Verify Wire.requestFrom works on i2c1** — the lateInitVariant probe works using Wire.beginTransmission/endTransmission, but getTouch also uses Wire.requestFrom which may behave differently.
- **Try reading touch data in lateInitVariant style** (single probe, not continuous polling) to confirm the data path works end-to-end.

## Key Files

### Meshtastic Fork (meshtastic-firmware/)
- `boards/freewili2.json` — custom RP2350B board definition
- `variants/rp2350/freewili/variant.h` — pin defs, FREEWILI defines, USE_SPLIT_UART_RADIO, SCREEN_TOUCH_INT=31
- `variants/rp2350/freewili/platformio.ini` — board=freewili2, `-D__WIRE0_DEVICE=i2c1`
- `src/platform/extra_variants/freewili/variant.cpp` — initVariant() (IO expander), lateInitVariant() (TOUCH_INT GPIO setup)
- `src/graphics/TFTDisplay.cpp` — `#elif defined(FREEWILI)`: custom Pico SDK LGFX + FT5316 touch reads + hasTouch/getTouch
- `src/mesh/UARTRadioInterface.cpp` — split-UART radio (RADIO_TX_SERIAL / RADIO_RX_SERIAL macros)
- `src/input/PICButtonInput.cpp` — SerialPIO for PIC buttons (disabled, PIO conflict)
- `src/main.cpp` — SPI init guarded with `#if defined(FREEWILI)` skip

### Framework variant (outside repo — manual install required)
- `.platformio/packages/framework-arduinopico/variants/freewili2/pins_arduino.h` — `PICO_RP2350A=0`

### WIO-E5 Bridge (wio-e5-bridge/)
- `src/main.cpp` — UART command dispatcher
- `src/radio_bridge.h/cpp` — RadioLib STM32WL SX1262 wrapper
- `src/uart_protocol.h/c` — frame parser/builder

### Hardware Reference
- Schematic: `../../Documentation/FreeWili_2_20.pdf`
- FreeWili firmware: `../freewili-firmware/freewilimain/` — working touch code to reference
- PIC16 firmware: `../freewili-firmware/fw2_pic16/barebones-firmware.X/`

## Hardware Summary

| Component | Detail |
|-----------|--------|
| Display CPU | RP2350B, SWD on debug probe interface 0 |
| Display | ST7789 480x320 (320x480 portrait + SWAP_XY), SPI1: SCK=10, MOSI=11, CS=9, DC=8 |
| Backlight | GPIO 25 via AP2502, needs IO expander GPIO25_RP_DIR=1 |
| IO Expander | PCAL6524 at I2C1 0x23 (GPIO 26 SDA, 27 SCL), controls SPI buffer dirs + SCREEN_nRST |
| Touch | FT5316 (FT5x06 family) at I2C1 0x38, TOUCH_INT=GPIO31 — Display CPU bus only |
| Radio | WIO-E5 via split UART: TX=GPIO32 (Serial1/UART0), RX=GPIO23 (Serial2/UART1) |
| Buttons | PIC16F19195 via UART: TX=38, RX=39 (UART1 pins, needs SerialPIO) |
| Main CPU | RP2350B running original FreeWili firmware, separate I2C1_M bus |
| Debug boot | Hold HOME+OK+AI to enter debug RP2040 bootloader |

## OpenOCD Commands

```bash
OPENOCD="/c/Users/benki/.pico-sdk/openocd/0.12.0+dev/openocd.exe"
SCRIPTS="/c/Users/benki/.pico-sdk/openocd/0.12.0+dev/scripts"

# Flash Display RP2350B (use Windows-style path for firmware!)
"$OPENOCD" -s "$SCRIPTS" -c "adapter driver cmsis-dap; cmsis-dap backend usb_bulk; cmsis-dap usb interface 0; transport select swd; adapter speed 1000" -f target/rp2350.cfg -c "init; reset halt; program C:/Users/benki/path/to/firmware.bin 0x10000000 verify reset exit"

# GDB debug
"$OPENOCD" -s "$SCRIPTS" -c "adapter driver cmsis-dap; cmsis-dap backend usb_bulk; cmsis-dap usb interface 0; transport select swd; adapter speed 1000" -f target/rp2350.cfg -c "init" &
arm-none-eabi-gdb firmware.elf -ex "target remote localhost:3333" -ex "break panic" -ex "monitor reset halt" -ex "continue"
```

**Important:** OpenOCD `program` command requires Windows-style paths (`C:/...`), not Unix-style (`/c/...`).

## Build Commands

```bash
# Meshtastic firmware
cd meshtastic-firmware && pio run -e freewili
# Output: .pio/build/freewili/firmware-freewili-*.bin
# Note: filename includes git hash, changes after commits

# WIO-E5 bridge
cd wio-e5-bridge && pio run -e wio-e5-bridge
# Output: .pio/build/wio-e5-bridge/firmware.bin
```

## Recommended Next Steps

1. **Fix touch I2C NACK** — The FT5316 responds in lateInitVariant but NACKs during getTouch polling. Check the original FreeWili firmware's touch init sequence. May need hardware reset via IO expander or specific register writes. Verify TOUCH_INT (GPIO31) goes LOW when screen is touched.

2. **PIC buttons** — GPIO 38/39 are UART1 pins. Both hardware UARTs are used by radio. SerialPIO hit PIO allocation conflicts. Options: (a) investigate what uses PIO and resolve conflict, (b) bit-bang 9600 baud in 20ms polling loop, (c) use the original FreeWili firmware's PIC UART approach.

3. **WIO-E5 bridge firmware** — Install STM32CubeProgrammer for RDP Level 1 unlock. Flash `wio-e5-bridge/` firmware. Radio UART is initialized but has no peer.

4. **Touch coordinate mapping** — Once touch works: panel is 320x480 portrait, display is 480x320 landscape. Current mapping: `screenX = rawY, screenY = 319 - rawX`. May need adjustment based on actual touch data.
