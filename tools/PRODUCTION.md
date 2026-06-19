# PRODUCTION.md - FreeWili 2 Flash Workflow Runbook

Operator runbook for the production flash bench. Walk this top-to-bottom on a
fresh host. Every command is copy/paste; no judgment calls until section 4
(exit code matrix). Assumes `v0.2.1` artifacts; bump the tag where noted when
a newer release ships.

---

## 1. Hardware checklist

On the bench, every station:

- **FreeWili 2 board** - Display board + Father board mated. Both screws
  snug; the inter-board flex must seat flat.
- **Type-C USB cable** - data-capable (NOT charge-only). 1 m or shorter
  preferred; long cables drop SWD packets.
- **Host PC** - Windows 10/11 with PowerShell 7+ (`pwsh`), or Linux with
  Bash 4+. Native PowerShell 5.1 (`powershell.exe`) is NOT supported by
  `flash-board.ps1`.
- **Pico SDK** installed at `~/.pico-sdk/` - provides the OpenOCD binary
  with CMSIS-DAP multi-interface support. PlatformIO's bundled OpenOCD is
  too old. See section 2 for install.
- **`tools/artifacts/`** pre-populated with the current release ELFs
  (`wio-e5-bridge.elf`, `meshtastic-freewili.elf`), OR `gh` CLI installed
  and authenticated so the script can fetch them.

---

## 2. First-time setup (once per host PC)

### 2a. Install the Pico SDK

The flasher only needs OpenOCD out of the SDK, but the full installer is
the path of least resistance.

- **Linux:** run the official one-line installer from the Raspberry Pi
  Foundation:
  <https://github.com/raspberrypi/pico-setup-linux>. Drops OpenOCD at
  `~/.pico-sdk/openocd/<ver>/openocd`.
- **Windows:** install the **Raspberry Pi Pico** VS Code extension; on
  first launch it offers to install the SDK + OpenOCD into
  `%USERPROFILE%\.pico-sdk\`. Confirm `~/.pico-sdk/openocd/<ver>/openocd.exe`
  exists when it's done.

Override the search path with `PICO_OPENOCD_DIR` if the SDK lives elsewhere.

### 2b. Install PowerShell 7+ (Windows) or Bash 4+ (Linux)

- **Windows:** `winget install Microsoft.PowerShell` then verify with
  `pwsh -v` (must be 7.x or higher).
- **Linux:** Bash 4+ ships on every supported distro; verify with
  `bash --version`.

### 2c. Install + auth the GitHub CLI (optional, for artifact fetch)

```powershell
winget install GitHub.cli            # Windows
sudo apt install gh                  # Debian / Ubuntu
gh auth login                        # one-time browser auth
```

### 2d. Download release artifacts

```powershell
gh release download v0.2.1 --repo Ytuf/wiligo --dir tools/artifacts --pattern '*.elf'
```

Expected files in `tools/artifacts/` after this:

- `wio-e5-bridge.elf`
- `meshtastic-freewili.elf`

### 2e. Smoke-test the script

```powershell
pwsh tools/flash-board.ps1 -h        # Windows
bash tools/flash-board.sh -h         # Linux
```

You should see the synopsis from the script header. If you get
`OpenOCD not found at ...`, fix step 2a before continuing.

---

## 3. Per-board flash workflow

### Run sheet (single board)

1. Plug the board into the host via Type-C.
2. **Wait 3 seconds** for USB enumeration to settle. (The multiprobe
   advertises three CDC interfaces; OpenOCD will fail to open iface 2 if
   you race it.)
3. Run:

   ```powershell
   pwsh tools/flash-board.ps1 -JsonLog flash.log.json
   ```

4. Watch for the stage banners. Expected sequence:

   ```
   [unlock]     FLASH_OPTR = 0x..., RDP byte = 0xXX -> unlock | skip
   [bridge]     program wio-e5-bridge.elf verify reset exit -> OK
   [meshtastic] program meshtastic-freewili.elf verify -> OK
   [sanity]     bridge ACK round-trip OK
   [PASS]
   ```

5. Total wall time: **~60 s** fresh board (RDP unlock costs ~30 s mass
   erase). **~30 s** on a re-flash (unlock auto-skips when
   `FLASH_OPTR & 0xff == 0xAA`).

### Multi-board station (specify serial)

When more than one FreeWili 2 is plugged into the same host, pin the
script to a specific multiprobe by its USB serial:

```powershell
pwsh tools/flash-board.ps1 -Serial E664B49... -JsonLog flash.log.json
```

See section 5 for how to enumerate serials.

### Loop mode (auto-flash each new board)

For a single-tech bench running boards through one at a time:

```powershell
pwsh tools/flash-board.ps1 -Loop -JsonLog flash.log.json
```

Polls for a new multiprobe enumeration; flashes it; waits for unplug;
repeats. Ctrl+C to stop.

---

## 4. Exit code matrix

The script returns the code in `$LASTEXITCODE` (PowerShell) / `$?` (Bash)
and writes the same code to the `result.code` field of the JSON log.

| Code | Meaning | Action |
|---|---|---|
| 0 | Pass | Box it up. |
| 1 | Prereq missing | Run `pwsh tools/flash-board.ps1 -h`; check OpenOCD path + that both ELFs are in `tools/artifacts/`. |
| 2 | Hardware not detected | Unplug + replug USB. Try a different cable. If still nothing, swap to a known-good port. |
| 3 | Unlock failed | Power-cycle the board, re-run. If 3 boards in a row fail at unlock, the multiprobe firmware on the RP2040 is wrong - pause the line and reflash the multiprobe (see section 9). |
| 4 | Bridge flash failed | Power-cycle + re-run. Likely a transient SWD glitch; the script auto-retries 3x before bailing. |
| 5 | Meshtastic flash failed | Power-cycle + re-run. If it fails twice on the same board, the Display CPU (RP2350B) may be damaged - tag the unit for rework. |
| 6 | Sanity check failed | Firmware flashed but didn't boot. Check that the Display CPU is getting power (D2 LED steady). If D2 is dark, it's a power-rail issue, not a firmware issue. |
| 7 | SWD wedge | Power-cycle and run again. If twice in a row on the same board, file a bug with the JSON log attached. |

---

## 5. Multi-board enumeration

When >1 board is plugged in, you need the CMSIS-DAP serial to disambiguate.

### Windows

```powershell
Get-PnpDevice -Class USB | Where-Object FriendlyName -match 'CMSIS-DAP'
```

The `InstanceId` field carries the serial (last `&`-delimited token).

### Linux

```bash
lsusb -d 2e8a:000c -v | grep iSerial
```

(USB VID/PID `2e8a:000c` is the Raspberry Pi multiprobe.)

Pass it via `-Serial` (PowerShell) / `--serial` (Bash):

```powershell
pwsh tools/flash-board.ps1 -Serial E664B49C81B7B92E -JsonLog flash.log.json
```

---

## 6. Verifying a flashed board (QC bench)

1. Leave the board plugged in. Open a serial terminal at **115200 8-N-1**
   on the multiprobe iface 0 CDC port. (On Windows it shows up as the
   highest-numbered COM of the three CDCs; on Linux it's
   `/dev/ttyACM<N>` - the lowest-numbered of the three.)
2. Press the reset button or unplug/replug.
3. Look for the Meshtastic boot banner:

   ```
   //\ E S H T /\ S T I C
   ...
   INFO  | Hardware: freewili
   INFO  | Firmware version: 2.x.x.<sha>
   ```

   The word **FreeWili** appears in the variant string.
4. Verify the on-board indicator LED chain blinks on power-up. The
   TX/RX/msg pattern fires within ~5 s of boot: a quick green flash on
   the radio TX/RX channels means the WIO-E5 bridge is up and ACKing.
5. Optional: touch the screen - tapping should trigger a haptic buzz and
   a soft UI click on the bit-bang speaker line.

If steps 3 and 4 both pass, the board is good.

---

## 7. Updating to a new firmware version

When v0.2.2 (or whatever ships next) is released:

```powershell
gh release download v0.2.2 --repo Ytuf/wiligo --dir tools/artifacts --clobber --pattern '*.elf'
```

The `--clobber` flag overwrites the existing ELFs. Then re-run
`flash-board.ps1` on each board exactly as in section 3. Already-unlocked
boards skip step 1 and complete in **~20 s** (just the bridge + Display
re-flash).

No need to clear `tools/artifacts/` between versions; the `--clobber`
overwrite is enough.

---

## 8. Per-unit PKI keypair (`FW2_PER_UNIT_KEYPAIR`)

### What it is

The default `freewili` PlatformIO env ships with a **PUBLIC, hardcoded
PKI privkey** baked into the firmware (`FW2_FIXED_PRIVKEY` in
`meshtastic-firmware/src/mesh/NodeDB.cpp`). This is a dev convenience -
it works around the persistence stub (see section 10) so reboots don't
churn the pubkey and break already-paired peers.

**Consequence:** anyone running the default firmware can decrypt DMs
sent to any other unit running the default firmware. Acceptable for
benchtop / customer-demo loads; **not** acceptable for fielded units.

### The production build

`platformio.ini` defines a second env: **`freewili-production`**. This
build sets `FW2_PER_UNIT_KEYPAIR=1`, which derives each unit's privkey
deterministically from the RP2350 chip unique ID at first boot. Every
board out of the production line therefore has its own keypair.

### When to switch

**Always** for boards going to customers. **Never** for dev kits or
internal test units that need to interop on the shared-key dev mesh.

### How to switch

```powershell
gh release download v0.2.1 --repo Ytuf/wiligo --dir tools/artifacts --clobber --pattern 'meshtastic-freewili-production.elf'
```

Then rename or symlink so `flash-board.ps1` picks it up:

```powershell
Copy-Item tools/artifacts/meshtastic-freewili-production.elf tools/artifacts/meshtastic-freewili.elf -Force
```

(A future release of the script will accept a `-ProductionBuild` flag
that does this automatically. Until then, the rename is the
operator-facing knob.)

### Verifying the production build flashed

Connect SWD via OpenOCD on iface 0 and dump the pubkey region of
LittleFS-backed config, OR observe two freshly-flashed boards on a serial
console: each should print a different `config.security.public_key.bytes`
in its boot log. If both print the same key, you flashed the dev build.

---

## 9. Recovery procedures

### WIO-E5 falls off the SWD bus

Symptom: step 1 (unlock) reads back `0x00000000` or hangs on
`stm32wlx unlock 0`.

1. Unplug, wait 5 s, replug.
2. If still wedged, flash `wio-e5-unlock.elf` to the Display CPU first:

   ```powershell
   pwsh tools/flash-board.ps1 -SkipMeshtastic -RecoveryUnlock
   ```

   This Display-CPU helper pulses the WIO_RST line via the PIC's UART
   command channel and re-asserts the system bootloader entry sequence.
3. Wait 5 s. Re-run `flash-board.ps1` normally.

### Multiprobe firmware corrupted

Symptom: OpenOCD reports "no CMSIS-DAP compatible device found" even
though the board enumerates as USB. Or step 3 (unlock) consistently
fails across **multiple** known-good boards.

**Pause the production line.** The multiprobe (the on-board RP2040 that
exposes CMSIS-DAP + 3x CDC) needs to be reflashed:

1. Hold the **BOOTSEL** button on the multiprobe RP2040 while plugging
   in USB.
2. The board enumerates as `RPI-RP2` mass-storage.
3. Drag the latest **`multiprobe.uf2`** from
   `Ytuf/freewili-multiprobe` releases onto the mass-storage volume.
4. Board reboots. Verify with `lsusb` / Device Manager that CMSIS-DAP +
   3 CDCs come back.

### Display CPU bricked

Rare. Symptom: step 5 (meshtastic flash) returns exit 5 on multiple
re-attempts, AND the D2 power LED is dark or flickering.

1. Tag the unit for rework - this is a board-level fault, not a
   flash-station issue.
2. If you need to recover it at the bench: picotool over the BOOTSEL
   path requires direct USB access to the Display CPU, which the FreeWili
   2 does **not** expose - the Display RP2350B has no USB pinout to the
   host. Recovery requires desktop-bench SWD with an external probe on
   the Display CPU's SWCLK/SWDIO test pads (board-specific; see schematic
   pp. 11-12).

---

## 10. Known limitations (v0.2.1)

Carry this list into every customer demo. These are tracked against
later releases; they are not flash-station issues.

- **Persistence is stubbed.** `saveProto()` in
  `meshtastic-firmware/src/mesh/NodeDB.cpp:1439` returns `true` without
  writing. Every reboot starts from defaults. Blocked on the SD UART API.
- **The shipped PKI privkey is PUBLIC** on the default build. Use the
  `freewili-production` build (section 8) for any unit leaving the lab.
- **No OTA path.** All firmware updates are SWD via the multiprobe.
- **No onboard-sensor telemetry.** BMI323 / BMM350 / SHT40 / OPT4001 are
  not yet exposed as Telemetry packets.
- **No real wall-clock RTC.** Blocked on the RTC UART API.
- **No speaker output / mesh audio.** Bit-bang feedback chirps only.
- **No tzdef display.**
- **One-boot channel persistence.** UI lets the operator pick a
  different LongFast slot but it reverts to slot 19 (906.875 MHz US) on
  reboot.

Audit status: 27 confirmed bugs from the v0.2.0 audit, ~13 fixed in
v0.2.1. The rest are blocked on the SD / RTC APIs and ride as known
issues against v0.3.x.
