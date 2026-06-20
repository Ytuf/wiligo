# wiligo

[Meshtastic](https://meshtastic.org/) port for the
[FreeWili 2](https://freewili.com/) - RP2350B + ST7789 touchscreen + Seeed
WIO-E5 LoRa module.

## Layout

| Directory | What it is |
|---|---|
| [`meshtastic-firmware/`](https://github.com/Ytuf/firmware) | Git submodule. Fork of [`meshtastic/firmware`](https://github.com/meshtastic/firmware) on the `freewili-port` branch - adds the `freewili` variant, on-device DM / channel editor, touch + virtual keyboard, audio (bit-bang I²S to NAU88C10), WS2812 indicator LEDs, haptic feedback. Runs on the Display RP2350B. |
| `wio-e5-bridge/` | UART-to-LoRa bridge firmware for the WIO-E5 (STM32WLE5JC). Speaks a small framed protocol back to the Display CPU; uses RadioLib for the actual SX1262. |
| `wio-e5-unlock/` | One-time helper that drives the WIO-E5 system bootloader from the Display CPU to clear RDP Level 1 → 0 (factory-locked modules ship locked). Also has a bridge-verify mode for sanity-checking `wio-e5-bridge` after flash. |
| `tools/` | `flash-board.ps1` turnkey flasher + release-artifact drop point. |

## Clone

```bash
git clone --recurse-submodules https://github.com/Ytuf/wiligo.git
```

If you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

## Build / flash

One-shot from release artifacts:

```powershell
gh release download v0.2.2 --repo Ytuf/wiligo --dir tools/artifacts --pattern '*.elf'
pwsh tools/flash-board.ps1
```

That chains RDP unlock → WIO-E5 bridge → Display Meshtastic, all over the
on-board RP2040 multiprobe (CMSIS-DAP). See `tools/README.md` for flags
(`-ForceUnlock`, `-UseLocalBuilds`, `-SkipMeshtastic`) and each subdirectory
README for per-project build instructions.

## What works in v0.2.2

- **LoRa bridge** via WIO-E5. Defaults to US LongFast slot 19 (906.875 MHz).
  You can pick a different slot in the UI but it only persists for one boot
  (see "Known caveats" below).
- **PKI DMs** after a fresh peer NodeInfo exchange.
- **Broadcasts** on the Primary LongFast channel.
- **Touch + virtual keyboard**, hardware buttons (Home → home frame,
  AI → messages), haptic, WS2812 indicator LEDs (TX/RX/msg blinks).
- **Audio bit-bang** feedback (UI click / notification beeps).
- **Battery fuel gauge** readback at I²C 0x55.

## Not yet

- Persistent settings — gated on the SD UART API. See "Known caveats."
- Real wall-clock RTC — gated on the RTC UART API.
- Onboard sensors (BMI323 / BMM350 / SHT40 / OPT4001) exposed as Telemetry.
- OTA update path.
- Speaker output / mesh audio.
- tzdef display.

## Known caveats

### Persistence is stubbed

The Meshtastic firmware on the Display RP2350B has a persistence stub at
`meshtastic-firmware/src/mesh/NodeDB.cpp:1439` — `saveProto()` returns `true`
without writing (RP2350 SMP + arduino-pico LittleFS hang on flash write).
Every boot starts from defaults. Workarounds shipped in v0.2.2:

- **Build-time hardcoded PKI keypair** (`FW2_FIXED_PRIVKEY` in `NodeDB.cpp`)
  so reboots don't churn the pubkey — peers (especially LilyGo T-Deck class
  clients with no "forget node" UI) lock the first pubkey they see and
  reject all future NodeInfo otherwise.
- **Hardcoded default `channel_num = 19`** so reboots tune to 906.875 MHz
  US LongFast instead of the slot-0 → slot-17 derivation.
- **`nodeNum ^= 0xFEED0001`** so peers see this firmware as a distinct node
  ID family.
- **Region defaults to `UNSET`** but the radio paths now compute frequency
  directly without relying on `LocalConfig.lora.region`.

Full SD-backed persistence is the upcoming SD UART API work.

### The shipped PKI privkey is PUBLIC

`FW2_FIXED_PRIVKEY` lives in this repo. **Anyone running this firmware can
decrypt DMs sent to any other instance of it.** Fine for development and
benchtop testing. Do not deploy in adversarial environments without
rotating the constant in `NodeDB.cpp`.

## Audit status

Full firmware audit completed for v0.2.2: 27 confirmed bugs, ~13 fixed in
this release. The remainder are blocked on the SD and RTC UART APIs and
ship as known issues against v0.3.x.
