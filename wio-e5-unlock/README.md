# wio-e5-unlock

One-time RDP-Level-0 unlock + sanity-test helper for the Seeed WIO-E5 module
soldered to FreeWili 2 boards. Runs on the FreeWili Display RP2350B and drives
the WIO-E5 over UART1 (GPIO 40/23 via the PCAL6524 antenna mux).

Three modes, selected by setting one of the `*_ONLY` defines in `src/main.cpp`
to `1`:

- `SCOPE_TEST_ONLY` — tight `AT\r\n` loop at 9600 baud for verifying TX on a
  scope before committing to the unlock sequence.
- `BRIDGE_VERIFY_ONLY` — switch UART to 115200 8-N-1 and send framed
  `CMD_RADIO_GET_STATUS` requests in a loop, capturing ACK responses in SRAM
  globals for SWD inspection. Use this after flashing `../wio-e5-bridge`.
- Default (both 0) — full RDP unlock:
  1. Probe AT firmware (`AT\r\n` → `OK`)
  2. `AT+DFU=ON` → AT firmware soft-resets into the STM32 system bootloader
  3. Switch UART to 115200 8-E-1 (bootloader framing, AN3155/AN5482)
  4. `0x7F` sync → expect `0x79` ACK
  5. `0x91 0x6E` (Readout Unprotect + ~cmd checksum) → mass erase (~25 s)
     → RDP→Level 0, flash erased, chip resets back into bootloader

Status globals throughout `main.cpp` are SWD-readable for diagnosis.

## Build

```bash
mkdir build && cd build
cmake -G Ninja ..
ninja
```

## Flash

The output `.elf` flashes to the Display RP2350B via SWD on CMSIS-DAP
interface 0. Example with OpenOCD:

```bash
openocd -f interface/cmsis-dap.cfg \
        -c "cmsis-dap usb interface 0" \
        -f target/rp2350.cfg \
        -c "program build/wio-e5-unlock.elf verify reset exit"
```
