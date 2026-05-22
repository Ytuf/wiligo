# Meshtastic FreeWili 2 Port — Session Handoff

Date: 2026-05-22. Updates the 2026-05-21 handoff. Audio bring-up session:
fixed pin assignment (DOUT is GPIO 5, not 4 — codec ADC OUT is on GPIO 4),
discovered RP2350 PADS_BANK0 bit 8 is ISO (pad isolation), learned that
PIO1 SM2 pindirs setting silently fails on this SDK fork (PIO0 SM1 works
for pindirs but the codec still won't lock to the PIO-generated I2S
stream), and finally landed **audible audio via CPU bit-bang through SIO**.
Audio is now wired to text-message arrival (low tone) and the green button
(click).

If you're a fresh Claude reading this — start here, then check
`git log --oneline -30` to see what's actually committed vs uncommitted.
Touch hardware via SWD; never assume a reflash "just works" without
re-checking GPIO_STATUS (bit 13 = OETOPAD, bit 9 = OUTTOPAD).

---

## TL;DR

The device is fully usable for end-to-end Meshtastic mesh communication +
**audible audio**. Bit-bang I2S via SIO drives the NAU88C10 codec; you
hear an 880 Hz tone on incoming text messages and a 2 kHz click on the
green button.

Open: PIO path is parked. PIO0 SM1 drives the pads correctly (SWD verified
OETOPAD + IRQTOPROC), but the codec doesn't lock to the PIO-generated
stream for unknown reasons (sys_clk = 200 MHz on stock vs 150 MHz here is
a candidate; entry-point timing is another). Bit-bang costs CPU during
playback (~250 ms blocking for the longest preset tone), which is fine
for short event tones but unsuitable for any future continuous audio
(RTTTL, TTS). See [[freewili-audio-bitbang-works-pio1-doesnt]] memory.

---

## Repo layout / paths

- `wiligo/` — this repo. Meshtastic port lives in nested submodule
  `meshtastic-firmware/` (fork of upstream); STM32WL UART bridge in
  `wio-e5-bridge/`.
- `../freewili-firmware/freewilimain/` — stock FreeWili firmware. **Reference
  source of truth** for hardware quirks (codec init, I2S PIO program,
  IOExpander pinout). When in doubt about how the hardware works, read
  stock first.
- `Documentation/FreeWili_2_20.pdf` — board schematic. Use
  `pdftotext -layout` from `/mingw64/bin` to read.

## Build / flash quick reference

```sh
OPENOCD="/c/Users/benki/.pico-sdk/openocd/0.12.0+dev/openocd.exe"
SCRIPTS="/c/Users/benki/.pico-sdk/openocd/0.12.0+dev/scripts"

# Display (RP2350, cmsis-dap interface 0)
ELF=meshtastic-firmware/.pio/build/freewili/firmware-freewili-2.7.21.0fc4151.elf
cd meshtastic-firmware && pio run -e freewili
"$OPENOCD" -s "$SCRIPTS" \
  -c "adapter driver cmsis-dap; cmsis-dap backend usb_bulk; \
      cmsis-dap usb interface 0; transport select swd; adapter speed 1000" \
  -f target/rp2350.cfg \
  -c "init; reset halt; program $ELF verify; reset run; exit"

# Bridge (STM32WLx, cmsis-dap interface 2) — rarely needs reflashing now
cd wio-e5-bridge && pio run -t upload
```

Memory-rules to obey:
- **Always `.elf` to OpenOCD program**, never `.bin` — STM32WL will silently
  write to address 0 otherwise.
- **End RP2350 sessions with `reset run`**, never bare `resume` — SMP resume
  leaves both cores halted.

---

## What works (committed/uncommitted, all on FW2 today)

### Bridge (wio-e5-bridge/) — done last session
- Wio-E5 RF switch driven via PA4/PA5 (`rfswitch_pins`), not DIO2. Without
  this, TX worked by accident but RX never reached the receiver.
- TCXO at 1.7 V, TX/RX IRQ disambiguation on DIO1, auto RX re-arm after TX,
  RX boosted gain, calibrateImage, power clamp ≤22 dBm, big-endian
  freq/preamble/syncword read, syncword `>>8` fix, Heltec 0x8B5 RX
  sensitivity patch, robust check_rx error recovery.

### Display CPU (meshtastic-firmware/) — this session
1. **Display LoRa freq hardcode typo** — was 0x36092818 (906.569 MHz),
   corrected to literal `906875000u` (906.875 MHz, slot 19).
2. **UART RX ISR + 1 KB ring buffer** for the bridge link
   (`UARTRadioInterface.cpp`). Old poll-only RX dropped almost all bytes of
   any frame larger than 32 (FIFO depth), which is why names never
   populated and DMs never delivered. Counters: `g_rsp_rx_count` /
   `g_uart_isr_count` / `g_uart_byte_count`.
3. **Home-menu navigation entries**: Nodes / LoRa / Settings / Channels /
   Identity / Edit Channels — exposed the giant existing menu tree that
   upstream Meshtastic already had but FW2's homeBaseMenu didn't surface.
4. **DM-from-cold via NodePicker** — added "Send Message" entry to
   `manageNodeMenu` (upstream didn't have one).
5. **Identity editor** — name editors that propagate to NodeDB via
   `freewili_push_owner_to_nodedb()` (otherwise long_name read from
   ourNode->user.long_name stayed stale).
6. **Channel picker + on-device channel editor** (`channelEditorMenu` →
   name → passphrase → role). ASCII passphrase pads to 16-byte PSK.
   Persistence still no-op'd, so settings revert on reboot.
7. **Reply / freetext launch race fix** — every banner-callback that wanted
   to launch the popup keyboard goes through a deferred `FreetextLaunchMenu`
   dispatch with `pendingFreetextDest` / `pendingFreetextChannel` static
   state. Calling `showTextInput` directly from a banner callback races
   with the banner's own cleanup and silently kills the popup.
8. **OnScreenKeyboardModule UAF fix** — `stop()` deletes the VirtualKeyboard
   from inside its own callback chain; deferred via `s_pendingDestroyKeyboard`
   reaped at next `start()`. Cancel/submit no longer hardfaults.
9. **Tap-to-type** — `VirtualKeyboard::selectKeyAt(x,y)` caches the draw-
   time grid layout and maps touchscreen taps to a key. Touch USER_PRESS
   with non-zero touch coords is intercepted before the "move cursor right"
   default.
10. **Touch coordinate calibration** — old code assumed `rawX` was on a
    0..479 axis and scaled by 320/480; actual FT5316 reports `rawX` on
    0..319 (inverted = screen Y) and `rawY` on 0..479 (= screen X).
    Verified with corner taps, new transform: `x=rawY; y=319-rawX`.
11. **Input direction split** — touch swipe and dpad share INPUT_BROKER_LEFT/
    RIGHT but the user wants them mapped to opposite directions. Fixed at
    the touch driver (`TouchScreenImpl1.cpp`): on FREEWILI, TOUCH_ACTION_LEFT
    emits INPUT_BROKER_RIGHT (and vice versa). Dpad and keyboard cursor
    keep the natural arrow-points-direction mapping.
12. **Node-list auto-rotation disabled** — each Distance / Hops-Signal /
    Last-Heard variant is its own swipeable frame instead of a 3-second
    flipping single slot. Plumbed through `hiddenFrames` for FREEWILI.
13. **Two big freeze fixes**:
    - **ScanI2C 0x55 hang**: TDECK_KB_ADDR probe at I2C 0x55 hits the
      BQ27441 fuel gauge with the wrong sub-register and the I2C peripheral
      never gets ACKed, hangs forever. setup() never finishes, screen
      stays black. Skipped 0x55 in scan loop for FREEWILI.
    - **Multi-sec stalls in main loop**: traced to FT5316 touch poll and
      then BQ27441 read both blocking for seconds on bus contention. Both
      now use raw pico-sdk `i2c_*_blocking_until` with 2 ms / 5 ms
      timeouts; the BQ27441 path also dropped from 5 retries to 2.
    - Phase markers added throughout `loop()`: `g_loop_phase`,
      `g_phase_max_ms[16]`, `g_loop_max_gap_ms` — if a new stall happens,
      these pinpoint which phase blocks.

### SWD-readable diagnostics (Display CPU)
Many counters across the firmware; key ones:
- `g_loop_iter_count`, `g_loop_max_gap_ms`, `g_loop_gap_at_ms`,
  `g_loop_max_gap_phase`, `g_phase_max_ms[N]` — main-loop liveness/timing.
- `g_processUART_count`, `g_uart_isr_count`, `g_uart_byte_count`,
  `g_uart_ring_drops`, `g_uart_crc_fail_count` — UART chain to bridge.
- `g_rsp_rx_count`, `g_rsp_rx_lastlen`, `g_rsp_rx_deliver_count` —
  RX-from-bridge.
- `g_updateUser_count`, `g_updateUser_changed` — NodeInfo decoding.
- `g_touch_raw_x/y`, `g_touch_screen_x/y` — last touch coords.
- `g_audio_*` — see audio section.
- `g_scan_*` — I2C scan progress (in case another address hangs).

Refresh symbol addresses with
`arm-none-eabi-nm meshtastic-firmware/.pio/build/freewili/firmware-*.elf | grep g_`.

---

## What's still broken / open

### Audio — WORKING via bit-bang (PIO parked) — `freewili_audio.cpp`

**Audible.** Test path: power on the board. Green button → 2 kHz click.
Send a text DM to the unit → 880 Hz tone fires alongside the blue LED blink.

**Implementation:** CPU bit-bang via SIO toggling on GPIO 5/6/7. 2 µs busy-wait
between BCLK edges. Pattern lifted from stock's `badge_bitbangi2s`
(`freewili-firmware/freewilimain/rmpLib/rpI2S.cpp:153-192`).

**Three bug fixes were required to get here**:

1. **Pin assignment**: We were driving GPIO 4 as DOUT. The schematic /
   stock firmware (`FW2Display_pin_definitions.h: SPK_DOUT=4, SPK_DIN=5`)
   shows GPIO 4 is the codec's ADC OUT (mic → MCU) and GPIO 5 is the
   codec's DAC IN (MCU → speaker). The MCU's playback data line is **5**.
2. **RP2350 PADS_BANK0 bit 8 is ISO** (pad isolation), not OD as on
   RP2040. After `pio_gpio_init`, GPIO 6/7 had ISO=1 (verified PADS=0x116),
   keeping the pad disconnected from chip logic. Explicit clear required:
   `PAD = (*PAD & ~((1<<7)|(1<<8))) | (1<<6)` (IE=1, OD=0, ISO=0).
3. **PIO pindirs are unreliable on PIO1** — `pio_sm_set_pindirs_with_mask`
   and every hand-rolled SET-PINDIRS variant leave the SM's pindirs at 0
   on PIO1 SM2, so the PIO's OUT signal is masked at the pin matrix even
   with IO_BANK0 OEOVER forced to ENABLE. **PIO0 SM1 does set pindirs
   correctly via the same SDK call** (SWD verified OETOPAD asserted, IRQ
   firing on transitions), but the codec still won't lock to the
   PIO-generated stream from PIO0. Mystery deferred.

### Audio open work

- **PIO investigation**: PIO0 SM1 drives pads correctly but codec
  silent. Candidates: stock runs `set_sys_clock_khz(200'000)` while
  we're at 150 MHz; stock enters program at `offset + 7` (`set x, 14
  side 3`) so X is pre-init, our SDK `pio_sm_init` enters at `s_offset`
  with X uninitialized → first frame garbled, codec may lock to garbage;
  stock uses DMA ping-pong, we'd be using CPU push. Fix in this priority.
  See `[[freewili-audio-bitbang-works-pio1-doesnt]]` memory.
- **CPU cost**: each event tone (20–80 ms typical) busy-waits the CPU.
  Fine for clicks/dings but unsuitable for any continuous audio (RTTTL,
  TTS). When PIO investigation lands, switch the playback function back.
  `ensure_i2s_started` is already built for PIO0 SM1 — just swap the body
  of `freewili_audio_tone` from bit-bang back to `pio_sm_put` and verify.
- **TX event tone**: not wired. The two `freewili_led_pulse_all` calls in
  `UARTRadioInterface.cpp` (TX/RX) fire on every bridge frame which is
  too noisy for audio. If user wants a TX tone, hook it at a higher level
  (e.g., once per send-text from `TextMessageModule` or the send path).

### Speaker — physical setup confirmed

User has the CN37 codec→speaker jumper installed; power jumper connected;
audio audible via bit-bang. Stock firmware also plays sound on this board.

### Persistence

`saveProto` / `saveToDisk` are no-op'd on FREEWILI to avoid the SMP
flash-write hang. Every configuration change (identity, channels, region,
preset, role, frame visibility) reverts on reboot. Re-enabling persistence
needs the flash-write hang fixed first — that's a deeper investigation.
Mention it explicitly to the user before they spend time configuring; they
already know but new students won't.

### Other items the user flagged but didn't prioritize today

- Button-semantics doc + audit (`task #24`) — write up what every button
  does on every screen, get user sign-off, then a code pass to align.
- Watchdog — earlier attempt at `rp2040.wdt_begin(8000)` in `setup()`
  caused a boot loop because the I2C scan exceeds 8 s. If we want a WDT,
  it must arm AFTER first `loop()` iteration. The hang we were trying to
  protect against (0x55 BQ27441) is fixed at the source now, so a WDT
  is less urgent.

---

## User context / collaboration preferences

- **Verify before claiming.** Halt via SWD and read counters; don't infer.
  The user is fed up with claims that turn out wrong.
- **Use `pio_sm_*` direct register writes** if the SDK helpers seem to
  silently not apply (we hit this with PINCTRL today).
- **Don't reflash speculatively.** User has many devices in a teaching
  context and wants the minimum number of flashes per change.
- **Match the student's environment** — don't suggest enabling Windows
  long-paths or Dev Mode; user is a teacher and shares the student
  friction.
- The user is comfortable being told "I don't know" or "let's stop and
  come back with a scope." Honesty > guessing.
- When in doubt about hardware, the stock FreeWili firmware
  (`../freewili-firmware/freewilimain/`) is authoritative. Dispatch the
  Explore agent to extract exact code, don't paraphrase.

---

## File map (changes that aren't a one-liner)

```
meshtastic-firmware/  (nested git repo)
  src/main.cpp                      — loop phase markers, gap tracker
  src/mesh/UARTRadioInterface.cpp   — RX ISR + ring buffer, freq hardcode
  src/mesh/UARTRadioInterface.h     — (small)
  src/mesh/NodeDB.cpp               — updateUser counter, freewili_push helper
  src/mesh/Channels.h               — (uses existing API)
  src/detect/ScanI2CTwoWire.cpp     — skip 0x55 on FREEWILI, scan trace
  src/graphics/draw/MenuHandler.cpp — Identity, channel editor, freetext
                                      deferred launch, NodePicker SendMessage
  src/graphics/draw/MenuHandler.h   — many new menu enum values
  src/graphics/draw/NodeListRenderer.cpp — auto-rotation disabled on FREEWILI
  src/graphics/Screen.cpp           — split list frames, swipe direction
  src/graphics/Screen.h             — hiddenFrames split for FREEWILI
  src/graphics/TFTDisplay.cpp       — touch calibration, raw pico-sdk I2C
                                      with tight timeout
  src/graphics/VirtualKeyboard.cpp  — selectKeyAt + layout cache
  src/graphics/VirtualKeyboard.h    — (header)
  src/input/PICButtonInput.cpp      — green-button → freewili_audio_play_click
  src/input/TouchScreenImpl1.cpp    — FREEWILI swipe-direction swap
  src/modules/OnScreenKeyboardModule.cpp — deferred-delete UAF fix
  src/modules/CannedMessageModule.cpp — freetext path through showTextInput
  src/modules/TextMessageModule.cpp — RX text msg → blue LED + 880 Hz tone
  src/platform/extra_variants/freewili/variant.cpp   — touch globals,
                                                       audio_init, BQ27441
                                                       I2C tightened
  src/platform/extra_variants/freewili/freewili_audio.cpp/.h — codec init
                                                       (NAU88C10 over I2C),
                                                       CPU-bit-bang I2S
                                                       playback via SIO,
                                                       PIO0 SM1 setup
                                                       function preserved
                                                       for future PIO
                                                       investigation
  variants/rp2350/freewili/variant.h — I2S_DOUT = 5 (was 4)

wio-e5-bridge/  (in main repo)
  src/radio_bridge.cpp              — last session; RF switch fix, etc.

docs/
  HANDOFF.md                        — this file
```

---

## Recommendation

Audio + UI work from this session can ship together:

1. **Meshtastic UI + bridge plumbing** — UI menus, freeze fixes, audio
   bit-bang + event wiring. Audio path is audible end-to-end so this is
   shippable as a unit.
2. **PIO investigation** — left in place but unused (`ensure_i2s_started`
   builds a PIO0 SM1 setup that drives pads correctly but doesn't produce
   audible codec output). Don't delete; it's the starting point for the
   next session's PIO work. See the audio open work section above.
