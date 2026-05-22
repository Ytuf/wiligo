# wiligo

[Meshtastic](https://meshtastic.org/) port for the
[FreeWili 2](https://freewili.com/) — RP2350B + ST7789 touchscreen + Seeed
WIO-E5 LoRa module.

## Layout

| Directory | What it is |
|---|---|
| [`meshtastic-firmware/`](https://github.com/Ytuf/firmware) | Git submodule. Fork of [`meshtastic/firmware`](https://github.com/meshtastic/firmware) on the `freewili-port` branch — adds the `freewili` variant, on-device DM / channel editor, touch + virtual keyboard, audio (bit-bang I²S to NAU88C10), WS2812 indicator LEDs, haptic feedback. Runs on the Display RP2350B. |
| `wio-e5-bridge/` | UART-to-LoRa bridge firmware for the WIO-E5 (STM32WLE5JC). Speaks a small framed protocol back to the Display CPU; uses RadioLib for the actual SX1262. |
| `wio-e5-unlock/` | One-time helper that drives the WIO-E5 system bootloader from the Display CPU to clear RDP Level 1 → 0 (factory-locked modules ship locked). Also has a bridge-verify mode for sanity-checking `wio-e5-bridge` after flash. |

## Clone

```bash
git clone --recurse-submodules https://github.com/Ytuf/wiligo.git
```

If you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

## Build / flash

Each subdirectory has its own README with build + flash instructions.
The FreeWili 2 has an on-board RP2040 multiprobe (CMSIS-DAP) exposing three
SWD interfaces — Display CPU on iface 0, Main CPU on iface 1, WIO-E5 on
iface 2. PlatformIO scripts in each subproject target the appropriate
interface automatically.

## Hardware reference

`Documentation/FreeWili_2_20.pdf` (board schematic; not in this repo) is the
authoritative source for pin assignments. The relevant pinout summary lives
in `meshtastic-firmware/variants/rp2350/freewili/variant.h`.
