# Meshtastic FreeWili Port — Handoff Document

## Project Summary

Porting Meshtastic firmware to FreeWili 2 hardware (RP2350B Display CPU + WIO-E5 LoRa radio). Design spec at `docs/superpowers/specs/2026-04-05-meshtastic-freewili-design.md`, implementation plan at `docs/superpowers/plans/2026-04-05-meshtastic-freewili.md`.

## Current Status: BLOCKED on display bring-up

All code is written and compiles. The WIO-E5 bridge firmware builds. The Meshtastic firmware builds. But the ST7789 display doesn't render the Meshtastic UI.

## What Works (Proven)

1. **Pico SDK SPI1 works in `initVariant()`** — we can fill the screen blue/black using raw `spi_init(spi1, 40000000)` + `spi_write_blocking()` in the very early boot function `initVariant()`. This proves: IO expander I2C works, SPI buffer directions are correct, SCREEN_nRST release works, backlight works, ST7789 accepts commands.

2. **IO expander (PCAL6524 at I2C 0x23)** — must be initialized to set bidirectional buffer directions for SPI signals. Without this, SPI signals don't physically reach the display. Uses Pico SDK `i2c_init(i2c1, 400000)` on GPIO 26/27. Register addresses: Output=0x04/0x05/0x06, Config=0x0C/0x0D/0x0E.

3. **Display parameters** — ST7789, physical 320x480 portrait panel, used in 480x320 landscape via MADCTL=0x2C (SWAP_XY|RGB|HORIZ_ORDER). Need 480*480 pixel writes to fill screen (not 480*320). INVON required.

4. **WIO-E5 bridge firmware** builds and compiles at `wio-e5-bridge/`. Not yet flashed (RDP Level 1 blocks SWD programming — need STM32CubeProgrammer).

5. **Debug probe** — multiprobe firmware at `multiprobe/sw/debugprobe/build/debugprobe_on_pico.uf2`. Interface 0=Display RP2350B, Interface 1=Main RP2350B, Interface 2=WIO-E5. Flash command: `openocd -s scripts -c "adapter driver cmsis-dap; cmsis-dap backend usb_bulk; cmsis-dap usb interface 0; transport select swd; adapter speed 1000" -f target/rp2350.cfg -c "init; reset halt; program firmware.bin 0x10000000 verify reset exit"`

## The Core Problem

**SPI1 works in `initVariant()` but is permanently broken by the time `TFTDisplay::connect()` runs.** Even full `spi_deinit(spi1)` + `spi_init(spi1, 40MHz)` + `gpio_set_function(10/11, GPIO_FUNC_SPI)` in `lateInitVariant()` does NOT restore SPI1 functionality. Register dump showed SPI1 SSPCR0=0x65 (wrong format: FRF=TI_SS, SPO=1, DSS=6-bit) and SSPCR1 with SSE=0 (SPI disabled).

### What we tried and ruled out:
- LovyanGFX: bypassed entirely with custom Pico SDK LGFX wrapper class — still doesn't work
- `HW_SPI1_DEVICE`: removed, so Meshtastic's SPI init uses SPI0 (dummy pins 2/3/4/5) and doesn't touch SPI1
- RP2350A vs RP2350B: tried `olimex_pico2bb48` (RP2350B) but `initVariant()` stopped being called entirely (no backlight, no pixel noise)
- LORA_CS=-1 causing `pinMode(-1, OUTPUT)`: fixed to valid GPIO
- Wrong IO expander type (PCAL6416A vs PCAL6524): confirmed PCAL6524 registers are correct (firmware code uses same addresses and works)
- SPI host number (0 vs 1): confirmed SPI1 is correct for GPIO 8-11
- Pin function conflicts: GPIO register dump shows correct FUNCSEL (10/11=SPI, 8/9=SIO)
- GPIO pad control: output enabled, 4mA drive, normal settings

### What we haven't tried:
- **Binary search** for what breaks SPI1: insert the green-fill test at progressively earlier points in main.cpp (between Wire.begin and screen->setup) to find the EXACT line that breaks SPI1
- **Check if `SPI.begin(false)` on SPI0 with dummy pins 2/3/4 somehow resets SPI1** — on RP2350, spi_init() calls spi_reset() which might affect RESETS register for both SPI instances?
- **Check the RP2350 RESETS register** (0x40020000) to see if SPI1 gets reset/unreset properly
- **Use the original FreeWili st7789 driver** directly instead of LovyanGFX or our wrapper — port the `st7789.cpp` from `freewili-firmware/freewilimain/rmpLib/` into Meshtastic
- **Disable ALL Meshtastic SPI init** (guard the SPI.begin block in main.cpp with `#ifndef FREEWILI`)
- **Run display on Core 1** like the original FreeWili firmware does (Core 0=events, Core 1=display)

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

1. **Find what breaks SPI1**: Add the proven blue-fill test code at successive points in main.cpp setup() to binary-search the exact line. The code that works in initVariant():
```cpp
#include "hardware/spi.h"
#include "hardware/gpio.h"
spi_init(spi1, 40000000);
gpio_set_function(10, GPIO_FUNC_SPI);
gpio_set_function(11, GPIO_FUNC_SPI);
gpio_init(9); gpio_set_dir(9, GPIO_OUT); gpio_put(9, 1);
gpio_init(8); gpio_set_dir(8, GPIO_OUT); gpio_put(8, 1);
gpio_init(25); gpio_set_dir(25, GPIO_OUT); gpio_put(25, 1);
// Then ST7789 init + blue fill (see variant.cpp history)
```

2. **Or bypass the problem entirely**: Guard ALL SPI init in main.cpp with `#if !defined(FREEWILI)` and manage SPI1 exclusively via Pico SDK in the FREEWILI display code.

3. **WIO-E5 programming**: Install STM32CubeProgrammer for RDP Level 1 unlock. OpenOCD/pyOCD cannot halt the CPU due to factory firmware protection.
