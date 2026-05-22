# wio-e5-bridge

UART-to-LoRa bridge firmware for the Seeed WIO-E5 module (STM32WLE5JC). Runs on the WIO-E5 alongside the FreeWili 2 Display RP2350B, which speaks a framed UART protocol (see `../meshtastic-firmware/src/mesh/UARTRadioProtocol.h`) to drive the internal SX1262.

## Prerequisites

- PlatformIO Core (`pip install platformio` or via VS Code extension)
- FreeWili 2 with multiprobe firmware on the debug RP2040 (WIO-E5 SWD on CMSIS-DAP interface 2)
- WIO-E5 must be at RDP Level 0 — see `../wio-e5-unlock/` if it's still locked

## Build

```bash
pio run -e wio-e5-bridge
```
Output: `.pio/build/wio-e5-bridge/firmware.bin`

## Flash

```bash
pio run -e wio-e5-bridge --target upload
```
Uses CMSIS-DAP iface 2 via the FreeWili multiprobe (configured in `platformio.ini`).

## Debug (GDB over OpenOCD)

```bash
pio debug -e wio-e5-bridge
```

## Manual SWD inspection

```bash
OPENOCD="C:/Users/benki/.pico-sdk/openocd/0.12.0+dev/openocd.exe"
SCRIPTS="C:/Users/benki/.pico-sdk/openocd/0.12.0+dev/scripts"

"$OPENOCD" -s "$SCRIPTS" \
  -c "adapter driver cmsis-dap; cmsis-dap usb interface 2; transport select swd; adapter speed 500" \
  -f target/stm32wlx.cfg -c "init; halt; <your commands>; shutdown"
```

## Source layout

- `src/main.cpp` — UART command dispatcher + bridge loop
- `src/uart_protocol.h/c` — frame parser/builder (CRC8, AN3155-style framing)
- `src/radio_bridge.h/cpp` — RadioLib STM32WLx wrapper (configure / TX / RX / sleep)

## Wiring

UART1 on WIO-E5 (PB6 TX / PB7 RX, 115200 8-N-1) is routed through the FreeWili antenna mux (PCAL6524 `LoRA_1101_SEL`) to RP2350B GPIO 40 (TX) / GPIO 23 (RX).
