# Meshtastic FreeWili Port — Handoff Document

## Project Summary

Porting Meshtastic firmware to FreeWili 2 hardware (RP2350B Display CPU + WIO-E5 LoRa radio). Design spec at `docs/superpowers/specs/2026-04-05-meshtastic-freewili-design.md`, implementation plan at `docs/superpowers/plans/2026-04-05-meshtastic-freewili.md`.

## Current Status: Display + Radio UART initialized — touch and buttons pending

All code is written and compiles. The WIO-E5 bridge firmware builds. The Meshtastic firmware builds. But the ST7789 display doesn't render the Meshtastic UI.

## What Works (Proven)

1. **Pico SDK SPI1 works in `initVariant()`** — we can fill the screen blue/black using raw `spi_init(spi1, 40000000)` + `spi_write_blocking()` in the very early boot function `initVariant()`. This proves: IO expander I2C works, SPI buffer directions are correct, SCREEN_nRST release works, backlight works, ST7789 accepts commands.

2. **IO expander (PCAL6524 at I2C 0x23)** — must be initialized to set bidirectional buffer directions for SPI signals. Without this, SPI signals don't physically reach the display. Uses Pico SDK `i2c_init(i2c1, 400000)` on GPIO 26/27. Register addresses: Output=0x04/0x05/0x06, Config=0x0C/0x0D/0x0E.

3. **Display parameters** — ST7789, physical 320x480 portrait panel, used in 480x320 landscape via MADCTL=0x2C (SWAP_XY|RGB|HORIZ_ORDER). Need 480*480 pixel writes to fill screen (not 480*320). INVON required.

4. **WIO-E5 bridge firmware** builds and compiles at `wio-e5-bridge/`. Not yet flashed (RDP Level 1 blocks SWD programming — need STM32CubeProgrammer).

5. **Debug probe** — multiprobe firmware at `multiprobe/sw/debugprobe/build/debugprobe_on_pico.uf2`. Interface 0=Display RP2350B, Interface 1=Main RP2350B, Interface 2=WIO-E5. Flash command: `openocd -s scripts -c "adapter driver cmsis-dap; cmsis-dap backend usb_bulk; cmsis-dap usb interface 0; transport select swd; adapter speed 1000" -f target/rp2350.cfg -c "init; reset halt; program firmware.bin 0x10000000 verify reset exit"`

## Root Cause (Found 2026-04-06)

**The firmware was panicking at `Wire.setSDA(26)` in main.cpp before any display code ran.**

`variant.h` defined `I2C_SDA 26` / `I2C_SCL 27`, which main.cpp fed to `Wire` (the default Arduino I2C object). On the rpipico2 board, `Wire` is hardcoded to `i2c0` via `__WIRE0_DEVICE`. But GPIO 26/27 are physically wired to `i2c1` — they are NOT valid pins for i2c0. The Arduino-Pico framework's `Wire.setSDA(26)` validates pin-to-peripheral mapping and calls `panic()` when the pin doesn't match.

This explains everything:
- Display worked in `initVariant()` (runs before Wire setup)
- Display was "broken" later — because the firmware halted at `Wire.setSDA(26)`
- SPI1 reinit in `lateInitVariant()` "didn't work" — it never executed
- Register dump showed garbage SPI1 values — SPI1 was never initialized (only IO expander was set up)

### Fix Applied
1. **variant.h**: Changed `I2C_SDA`/`I2C_SCL` → `I2C_SDA1`/`I2C_SCL1` so main.cpp routes through `Wire1` (i2c1), matching the hardware
2. **main.cpp**: Guarded the LoRa SPI init block with `#if defined(FREEWILI)` skip — FreeWili uses UART radio, no SPI LoRa needed
3. **variant.h**: Updated `TOUCH_I2C_PORT` from 0 to 1 (Wire1)
4. Build compiles cleanly

### Previous debugging (context)
- LovyanGFX was bypassed with custom Pico SDK LGFX wrapper class (still in TFTDisplay.cpp)
- `HW_SPI1_DEVICE` was removed so Meshtastic's SPI init wouldn't touch SPI1
- LORA_CS was fixed from -1 to valid GPIO 5
- IO expander confirmed as PCAL6524 at I2C 0x23

### Display + radio confirmed working (2026-04-07)
Meshtastic UI renders on ST7789 — no faults displayed. Shows "UNSENT" status, firmware version 2.7.21.

### RP2350B board definition created
Custom `freewili2` board (boards/freewili2.json) with `PICO_RP2350A=0` in variant pins_arduino.h at `.platformio/packages/framework-arduinopico/variants/freewili2/`. Enables GPIO 0-47.

### UART radio: split-UART approach
GPIO 32 = UART0_TX, GPIO 23 = UART1_RX — different hardware UARTs. Solution: Serial1 (UART0) for TX, Serial2 (UART1) for RX. Controlled by `USE_SPLIT_UART_RADIO` define. SerialPIO was attempted but hit PIO allocation conflicts.

### Still disabled
- **PIC16 buttons** — GPIO 38/39 are UART1 pins, but both hardware UARTs are used (Serial1 for radio TX, Serial2 for radio RX). Needs SerialPIO but PIO allocation conflicts with something. Investigate PIO usage in framework.
- **Touch screen** — FT6336U at I2C 0x38 is detected by scanner, but custom LGFX class has `getTouch()` returning false. Need to integrate FT6336U touch input (either via Meshtastic's existing touch support or custom driver).

## Key Files

### Meshtastic Fork (meshtastic-firmware/)
- `variants/rp2350/freewili/variant.h` — pin defs, FREEWILI/PRIVATE_HW defines, USE_UART_RADIO, no HW_SPI1_DEVICE
- `variants/rp2350/freewili/platformio.ini` — extends rp2350_base, board=rpipico2 (need RP2350B)
- `src/platform/extra_variants/freewili/variant.cpp` — initVariant() (IO expander), lateInitVariant()
- `src/graphics/TFTDisplay.cpp` — search for `#elif defined(FREEWILI)` — custom Pico SDK LGFX wrapper (NOT LovyanGFX)
- `src/mesh/UARTRadioInterface.h/cpp` — UART radio for WIO-E5 bridge
- `src/mesh/UARTRadioProtocol.h` — shared protocol constants
- `src/input/PICButtonInput.h/cpp` — PIC16 button input via UART
- `src/main.cpp` — lines 770-780: SPI init (SPI0 with dummy pins now), line 815: Screen creation, line 942: screen->setup(), line 956: lateInitVariant()

### WIO-E5 Bridge (wio-e5-bridge/)
- `src/main.cpp` — UART command dispatcher
- `src/radio_bridge.h/cpp` — RadioLib STM32WL SX1262 wrapper
- `src/uart_protocol.h/c` — frame parser/builder

### Hardware Reference
- Schematic: `../../Documentation/FreeWili_2_20.pdf` (rendered PNGs at /tmp/freewili_schematic/)
- FreeWili firmware: `../freewili-firmware/freewilimain/` — working display code in `Fw2Display.cpp`, `rmpLib/st7789.cpp`, `rmpLib/fw2IOExpanderDisplay.cpp`
- PIC16 firmware: `../freewili-firmware/fw2_pic16/barebones-firmware.X/`

## Hardware Summary

| Component | Detail |
|-----------|--------|
| Display CPU | RP2350B, SWD on debug probe interface 0 |
| Display | ST7789 480x320 (320x480 portrait + SWAP_XY), SPI1: SCK=10, MOSI=11, CS=9, DC=8 |
| Backlight | GPIO 25 via AP2502, needs IO expander GPIO25_RP_DIR=1 |
| IO Expander | PCAL6524 at I2C1 0x23 (GPIO 26 SDA, 27 SCL), controls SPI buffer dirs + SCREEN_nRST |
| Touch | FT6336U at I2C 0x38 (or NS2009 at 0x49 per schematic) |
| Radio | WIO-E5 (STM32WLE5JC + SX1262) via UART1: TX=PB6→GPIO23, RX=PB7→GPIO32 |
| Buttons | PIC16F19195 via UART: RX=38, TX=39, protocol 0xC0 0xC5 + 16-bit bitmap |
| Main CPU | RP2350B running original FreeWili firmware, controls SCREEN_SEL mux via Main IO Expander (0x22) |
| Debug boot | Hold HOME+OK+AI to enter debug RP2040 bootloader |

## OpenOCD Commands

```bash
OPENOCD="/c/Users/benki/.pico-sdk/openocd/0.12.0+dev/openocd.exe"
SCRIPTS="/c/Users/benki/.pico-sdk/openocd/0.12.0+dev/scripts"

# Flash Display RP2350B
"$OPENOCD" -s "$SCRIPTS" -c "adapter driver cmsis-dap; cmsis-dap backend usb_bulk; cmsis-dap usb interface 0; transport select swd; adapter speed 1000" -f target/rp2350.cfg -c "init; reset halt; program C:/Users/benki/freewili.bin 0x10000000 verify reset exit"

# Read registers for debug
"$OPENOCD" -s "$SCRIPTS" -c "adapter driver cmsis-dap; cmsis-dap backend usb_bulk; cmsis-dap usb interface 0; transport select swd; adapter speed 1000" -f target/rp2350.cfg -c "init; halt; reg pc; mdw 0xE000ED28 4; resume; exit"
```

## Build Commands

```bash
# Meshtastic firmware
cd meshtastic-firmware && pio run -e freewili
# Output: .pio/build/freewili/firmware-freewili-*.bin

# WIO-E5 bridge
cd wio-e5-bridge && pio run -e wio-e5-bridge
# Output: .pio/build/wio-e5-bridge/firmware.bin
```

## Recommended Next Steps

1. **Touch screen integration**: FT6336U at I2C 0x38 is on Wire (i2c1). Custom LGFX class needs `getTouch()` implemented, or wire into Meshtastic's `kbI2cAddress`/`INPUTDRIVER_ENCODER_TYPE` touch system. Check how other TFT variants (T-Deck, T-Watch) handle touch.

2. **PIC buttons via SerialPIO**: Resolve PIO allocation conflict. Check what else uses PIO (Neopixel? I2S? Tone?). May need to disable conflicting PIO users or pre-allocate PIO programs. Alternatively, bit-bang the 9600 baud PIC UART in the polling loop (20ms period = plenty of time).

3. **WIO-E5 bridge firmware**: Install STM32CubeProgrammer for RDP Level 1 unlock, flash wio-e5-bridge firmware. Until then, radio UART is initialized but has no peer.

4. **SCREEN_SEL mux**: The Main CPU RP2350B controls a display mux (SCREEN_SEL) via its IO expander at 0x22. May need the main CPU to release the display. Currently works because debug boot likely defaults mux correctly.
