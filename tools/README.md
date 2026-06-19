# tools

## flash-board.ps1

Chains the three steps for a FreeWili 2 from blank to running mesh:

1. **RDP unlock the WIO-E5 over SWD** (CMSIS-DAP iface 2). Reads `FLASH_OPTR`
   (`0x58004020`); if the low byte is already `0xAA` (RDP Level 0), unlock is
   skipped. Otherwise runs `stm32wlx unlock 0` + `stm32wlx option_load 0` which
   writes `RDP=0xAA` to the option byte, triggers `OBL_LAUNCH`, and the chip
   mass-erases + reboots at RDP 0 (~25 s).
2. **`wio-e5-bridge.elf`** → WIO-E5 (iface 2).
3. **`meshtastic-freewili.elf`** → Display RP2350B (iface 0).

### Prerequisites

- **Pico SDK OpenOCD** at `~/.pico-sdk/openocd/<ver>/openocd.exe` (override
  with `PICO_OPENOCD_DIR`). PlatformIO's bundled OpenOCD is too old for
  `cmsis-dap usb interface N`.
- **Release artifacts** in `tools/artifacts/` — fetch with:
  ```powershell
  gh release download v0.2.0 --repo Ytuf/wiligo --dir tools/artifacts --pattern '*.elf'
  ```

### Usage

The script is idempotent: on an already-unlocked board (FLASH_OPTR low byte =
`0xAA`) step 1 auto-skips and goes straight to bridge + meshtastic flash. Pass
`-ForceUnlock` to redo the mass erase anyway.

```powershell
# Fresh board, full sequence (idempotent - unlock is skipped if not needed)
pwsh tools/flash-board.ps1

# Force a fresh mass erase even if RDP already reads 0xAA
pwsh tools/flash-board.ps1 -ForceUnlock

# Local development - use the freshest ELF in each subproject's build dir
pwsh tools/flash-board.ps1 -UseLocalBuilds

# Only set up the WIO-E5; leave the Display CPU alone
pwsh tools/flash-board.ps1 -SkipMeshtastic
```

### Debug

If `stm32wlx unlock 0` fails or `option_load 0` doesn't actually mass-erase,
check the FLASH_OPTR value directly:

```bash
openocd -f interface/cmsis-dap.cfg -c "cmsis-dap usb interface 2" \
        -f target/stm32wlx.cfg -c "init; mdw 0x58004020 1; exit"
```

Low byte = RDP byte. `0xAA` = unlocked, `0xCC` = Level 2 (permanently locked,
unrecoverable), anything else = Level 1 (recoverable via the script).

If the WIO-E5 falls off the SWD bus after a bad option-byte sequence, see
`../wio-e5-unlock/README.md` for the optional recovery helper — it pulses the
WIO_RST line via the PIC's UART command channel to bring the chip back.
