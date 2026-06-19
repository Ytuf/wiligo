#!/usr/bin/env bash
# flash-board.sh - Linux/macOS port of flash-board.ps1
#
# Bring a fresh FreeWili 2 from blank to running Meshtastic in one shot.
#
# Sequence (all SWD over the on-board RP2040 multiprobe / CMSIS-DAP):
#   1. RDP-unlock the WIO-E5 over SWD (iface 2) — skipped if RDP already 0xAA.
#   2. Flash wio-e5-bridge to the WIO-E5 (iface 2).
#   3. Flash meshtastic-freewili to the Display RP2350B (iface 0).
#
# Requires:
#   - openocd in PATH (or PICO_OPENOCD_DIR pointing at a Pico-SDK install)
#   - arm-none-eabi-nm in PATH (for symbol lookup in the sanity check)
#   - lsusb in PATH (for multiprobe enumeration)
#
# Exit codes:
#   0  success
#   1  generic / usage error
#   2  OpenOCD not found / toolchain missing
#   3  artifact (ELF) missing
#   4  multiprobe pre-flight failed (CMSIS-DAP interfaces not enumerated)
#   5  RDP unlock failed (or RDP Level 2 — permanently locked)
#   6  bridge flash failed after retries
#   7  meshtastic flash failed / post-flash sanity check failed
#
# Every fatal error logs a greppable "[E<code>] <message>" line to stderr.

set -o pipefail

# ---------------------------------------------------------------------------
# Defaults & globals
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ARTIFACTS_DIR="$SCRIPT_DIR/artifacts"
USE_LOCAL_BUILDS=0
FORCE_UNLOCK=0
SKIP_MESHTASTIC=0
SKIP_SANITY_CHECK=0
LOOP_MODE=0
JSON_LOG=""
SERIAL=""

OCD_BIN=""
OCD_SCRIPTS=""

# JSON status accumulators
STATUS_STAGE="init"
STATUS_RESULT="pending"
STATUS_ERROR=""
STATUS_EXIT=1
STATUS_SERIAL=""
STATUS_BRIDGE_ELF=""
STATUS_MESH_ELF=""
STATUS_RDP_BEFORE=""
STATUS_RDP_AFTER=""
STATUS_UART_COUNT=""
STATUS_BRIDGE_ATTEMPTS=0
STATUS_TIMESTAMP_START="$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)"

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------

log()       { printf '%s\n' "$*"; }
log_cyan()  { printf '==> %s\n' "$*"; }
log_warn()  { printf '    %s\n' "$*" >&2; }
log_err()   { # log_err <code> <message>
    local code="$1"; shift
    printf '[E%s] %s\n' "$code" "$*" >&2
}

# ---------------------------------------------------------------------------
# Usage
# ---------------------------------------------------------------------------

usage() {
    cat <<EOF
flash-board.sh - bring a fresh FreeWili 2 from blank to running Meshtastic

Usage: flash-board.sh [options]

Options:
  --artifacts-dir DIR    Where release ELFs live (default: tools/artifacts/)
  --use-local-builds     Ignore artifacts dir; pull freshest ELF from each
                         subproject's build dir
  --force-unlock         Run SWD unlock even if RDP already reads 0xAA
  --skip-meshtastic      Skip step 3 (Display CPU flash)
  --skip-sanity-check    Skip post-flash mdw read of g_uart_byte_count
  --serial XXX           Match a specific CMSIS-DAP probe serial (filters
                         lsusb output and passes 'adapter serial XXX' to
                         OpenOCD)
  --json-log PATH        Write final JSON status to PATH
  --loop                 After completing one board, poll lsusb for a serial
                         change and run again on the next probe plugged in
  -h, --help             Show this help and exit

Exit codes:
  0 success            5 RDP unlock failed / Level 2
  1 usage / generic    6 bridge flash failed
  2 toolchain missing  7 meshtastic flash or sanity check failed
  3 artifact missing
  4 pre-flight failed (CMSIS-DAP interfaces not enumerated)
EOF
}

# ---------------------------------------------------------------------------
# Arg parsing
# ---------------------------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        --artifacts-dir)
            [ $# -lt 2 ] && { log_err 1 "--artifacts-dir requires an argument"; exit 1; }
            ARTIFACTS_DIR="$2"; shift 2 ;;
        --use-local-builds)   USE_LOCAL_BUILDS=1; shift ;;
        --force-unlock)       FORCE_UNLOCK=1; shift ;;
        --skip-meshtastic)    SKIP_MESHTASTIC=1; shift ;;
        --skip-sanity-check)  SKIP_SANITY_CHECK=1; shift ;;
        --loop)               LOOP_MODE=1; shift ;;
        --serial)
            [ $# -lt 2 ] && { log_err 1 "--serial requires an argument"; exit 1; }
            SERIAL="$2"; shift 2 ;;
        --json-log)
            [ $# -lt 2 ] && { log_err 1 "--json-log requires an argument"; exit 1; }
            JSON_LOG="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *)
            log_err 1 "Unknown argument: $1"
            usage >&2
            exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# JSON status output
# ---------------------------------------------------------------------------

# Escape a string for JSON output.
json_escape() {
    local s="$1"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    s="${s//$'\n'/\\n}"
    s="${s//$'\r'/\\r}"
    s="${s//$'\t'/\\t}"
    printf '%s' "$s"
}

write_json_log() {
    [ -z "$JSON_LOG" ] && return 0
    local ts_end
    ts_end="$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)"
    {
        printf '{\n'
        printf '  "stage": "%s",\n'           "$(json_escape "$STATUS_STAGE")"
        printf '  "result": "%s",\n'          "$(json_escape "$STATUS_RESULT")"
        printf '  "exit_code": %s,\n'         "$STATUS_EXIT"
        printf '  "error": "%s",\n'           "$(json_escape "$STATUS_ERROR")"
        printf '  "serial": "%s",\n'          "$(json_escape "$STATUS_SERIAL")"
        printf '  "bridge_elf": "%s",\n'      "$(json_escape "$STATUS_BRIDGE_ELF")"
        printf '  "mesh_elf": "%s",\n'        "$(json_escape "$STATUS_MESH_ELF")"
        printf '  "rdp_before": "%s",\n'      "$(json_escape "$STATUS_RDP_BEFORE")"
        printf '  "rdp_after": "%s",\n'       "$(json_escape "$STATUS_RDP_AFTER")"
        printf '  "uart_byte_count": "%s",\n' "$(json_escape "$STATUS_UART_COUNT")"
        printf '  "bridge_attempts": %s,\n'   "$STATUS_BRIDGE_ATTEMPTS"
        printf '  "started_at": "%s",\n'      "$STATUS_TIMESTAMP_START"
        printf '  "ended_at": "%s"\n'         "$ts_end"
        printf '}\n'
    } > "$JSON_LOG" 2>/dev/null || log_warn "Could not write JSON log to $JSON_LOG"
}

# Single exit funnel — always writes JSON before exiting.
finish() {
    local code="$1"
    local err_msg="${2:-}"
    STATUS_EXIT="$code"
    if [ "$code" -eq 0 ]; then
        STATUS_RESULT="success"
    else
        STATUS_RESULT="failure"
        [ -n "$err_msg" ] && STATUS_ERROR="$err_msg"
    fi
    write_json_log
    exit "$code"
}

# ---------------------------------------------------------------------------
# OpenOCD discovery
# ---------------------------------------------------------------------------

find_openocd() {
    # 1. Explicit override
    if [ -n "${PICO_OPENOCD_DIR:-}" ]; then
        if [ -x "$PICO_OPENOCD_DIR/openocd" ]; then
            OCD_BIN="$PICO_OPENOCD_DIR/openocd"
            OCD_SCRIPTS="$PICO_OPENOCD_DIR/scripts"
            return 0
        fi
        # PICO_OPENOCD_DIR may be the parent of versioned dirs
        local picked
        picked="$(ls -1 "$PICO_OPENOCD_DIR" 2>/dev/null | sort | tail -1)"
        if [ -n "$picked" ] && [ -x "$PICO_OPENOCD_DIR/$picked/openocd" ]; then
            OCD_BIN="$PICO_OPENOCD_DIR/$picked/openocd"
            OCD_SCRIPTS="$PICO_OPENOCD_DIR/$picked/scripts"
            return 0
        fi
    fi

    # 2. Pico SDK default location
    local base="$HOME/.pico-sdk/openocd"
    if [ -d "$base" ]; then
        if [ -x "$base/openocd" ]; then
            OCD_BIN="$base/openocd"
            OCD_SCRIPTS="$base/scripts"
            return 0
        fi
        local picked
        picked="$(ls -1 "$base" 2>/dev/null | sort | tail -1)"
        if [ -n "$picked" ] && [ -x "$base/$picked/openocd" ]; then
            OCD_BIN="$base/$picked/openocd"
            OCD_SCRIPTS="$base/$picked/scripts"
            return 0
        fi
    fi

    # 3. PATH fallback (any modern openocd)
    if command -v openocd >/dev/null 2>&1; then
        OCD_BIN="$(command -v openocd)"
        # Try common script-dir locations
        for d in /usr/share/openocd/scripts /usr/local/share/openocd/scripts \
                 /opt/homebrew/share/openocd/scripts; do
            if [ -d "$d" ]; then OCD_SCRIPTS="$d"; return 0; fi
        done
        OCD_SCRIPTS=""
        return 0
    fi

    return 1
}

# ---------------------------------------------------------------------------
# Multiprobe enumeration (lsusb)
# ---------------------------------------------------------------------------

# Print one serial per line; respects $SERIAL filter.
enumerate_probes() {
    if ! command -v lsusb >/dev/null 2>&1; then
        return 0
    fi
    local serials
    serials="$(lsusb -d 2e8a:000c -v 2>/dev/null | grep iSerial | awk '{print $3}')"
    if [ -n "$SERIAL" ]; then
        printf '%s\n' "$serials" | grep -Fx -- "$SERIAL" || true
    else
        printf '%s\n' "$serials"
    fi
}

# Verify probe + 3 CMSIS-DAP interfaces show up.
preflight_check() {
    if ! command -v lsusb >/dev/null 2>&1; then
        log_warn "lsusb not found — skipping pre-flight enumeration check"
        return 0
    fi
    local count
    count="$(enumerate_probes | grep -c .)" || count=0
    if [ "$count" -eq 0 ]; then
        return 1
    fi
    # Confirm 3 interfaces (CMSIS-DAP v2 typically exposes 3 USB interfaces)
    local ifaces
    ifaces="$(lsusb -d 2e8a:000c -v 2>/dev/null | grep -c 'bInterfaceNumber')"
    if [ "$ifaces" -lt 3 ]; then
        log_warn "Multiprobe shows only $ifaces interfaces (expected >= 3)"
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# Artifact resolution
# ---------------------------------------------------------------------------

# resolve_artifact <release-name> <local-glob>
resolve_artifact() {
    local release="$1"
    local glob="$2"

    if [ "$USE_LOCAL_BUILDS" -ne 1 ]; then
        local p="$ARTIFACTS_DIR/$release"
        if [ -f "$p" ]; then
            printf '%s\n' "$p"
            return 0
        fi
    fi

    # shellcheck disable=SC2086
    local hit
    hit="$(ls -1t $REPO_ROOT/$glob 2>/dev/null | head -1)"
    if [ -n "$hit" ] && [ -f "$hit" ]; then
        printf '%s\n' "$hit"
        return 0
    fi
    return 1
}

# ---------------------------------------------------------------------------
# OpenOCD invocation helpers
# ---------------------------------------------------------------------------

# build_serial_cmds — echoes -c args to filter by --serial (if set).
serial_cmds() {
    [ -n "$SERIAL" ] && printf -- '-c\nadapter serial %s\n' "$SERIAL"
}

# invoke_openocd <interface> <target_cfg> <elf_path> [--rp2350]
invoke_openocd() {
    local iface="$1" cfg="$2" elf="$3" rp2350=0
    [ "${4:-}" = "--rp2350" ] && rp2350=1

    local program_cmd
    if [ "$rp2350" -eq 1 ]; then
        program_cmd="program \"$elf\" verify"
    else
        program_cmd="program \"$elf\" verify reset exit"
    fi

    local -a args=(
        -s "$OCD_SCRIPTS"
        -c 'adapter driver cmsis-dap'
    )
    [ -n "$SERIAL" ] && args+=(-c "adapter serial $SERIAL")
    args+=(
        -c "cmsis-dap usb interface $iface"
        -c 'transport select swd'
        -c 'adapter speed 500'
        -f "$cfg"
        -c "$program_cmd"
    )
    if [ "$rp2350" -eq 1 ]; then
        args+=(-c 'reset run' -c 'shutdown')
    fi

    log "    > $OCD_BIN ${args[*]}"
    "$OCD_BIN" "${args[@]}"
}

# Read FLASH_OPTR (0x58004020); echo the low (RDP) byte in hex, e.g. "aa".
# Empty stdout on failure.
read_rdp_byte() {
    local tmp
    tmp="$(mktemp)"
    local -a args=(
        -s "$OCD_SCRIPTS"
        -c 'adapter driver cmsis-dap'
    )
    [ -n "$SERIAL" ] && args+=(-c "adapter serial $SERIAL")
    args+=(
        -c 'cmsis-dap usb interface 2'
        -c 'transport select swd'
        -c 'adapter speed 500'
        -f target/stm32wlx.cfg
        -c 'init'
        -c 'mdw 0x58004020 1'
        -c 'exit'
        -l "$tmp"
    )
    "$OCD_BIN" "${args[@]}" >/dev/null 2>&1 || true
    local line
    line="$(grep '^0x58004020:' "$tmp" 2>/dev/null | head -1)"
    rm -f "$tmp"
    [ -z "$line" ] && return 1
    # line like:  0x58004020: deadbeaa
    local word="${line##*: }"
    word="${word// /}"
    printf '%s\n' "${word: -2}"
}

# Unlock the WIO-E5 (stm32wlx unlock 0 + option_load 0).
unlock_wio_e5() {
    local -a args=(
        -s "$OCD_SCRIPTS"
        -c 'adapter driver cmsis-dap'
    )
    [ -n "$SERIAL" ] && args+=(-c "adapter serial $SERIAL")
    args+=(
        -c 'cmsis-dap usb interface 2'
        -c 'transport select swd'
        -c 'adapter speed 500'
        -f target/stm32wlx.cfg
        -c 'init'
        -c 'reset halt'
        -c 'stm32wlx unlock 0'
        -c 'stm32wlx option_load 0'
        -c 'exit'
    )
    "$OCD_BIN" "${args[@]}" 2>&1 | sed 's/^/    /'
    return ${PIPESTATUS[0]}
}

# Post-flash sanity check: read g_uart_byte_count via mdw and echo the hex word.
# Returns 0 if non-zero, 1 otherwise.
sanity_check_uart_counter() {
    local elf="$1"
    if ! command -v arm-none-eabi-nm >/dev/null 2>&1; then
        log_warn "arm-none-eabi-nm not found — cannot resolve g_uart_byte_count, skipping"
        return 0
    fi
    local addr
    addr="$(arm-none-eabi-nm "$elf" 2>/dev/null | awk '$3=="g_uart_byte_count"{print "0x"$1; exit}')"
    if [ -z "$addr" ]; then
        log_warn "g_uart_byte_count symbol not found in $elf, skipping sanity check"
        return 0
    fi

    local tmp
    tmp="$(mktemp)"
    local -a args=(
        -s "$OCD_SCRIPTS"
        -c 'adapter driver cmsis-dap'
    )
    [ -n "$SERIAL" ] && args+=(-c "adapter serial $SERIAL")
    args+=(
        -c 'cmsis-dap usb interface 0'
        -c 'transport select swd'
        -c 'adapter speed 500'
        -f target/rp2350.cfg
        -c 'init'
        -c "mdw $addr 1"
        -c 'exit'
        -l "$tmp"
    )
    "$OCD_BIN" "${args[@]}" >/dev/null 2>&1 || true
    local line word
    line="$(grep -i "^${addr,,}:" "$tmp" 2>/dev/null | head -1)"
    [ -z "$line" ] && line="$(grep "^0x" "$tmp" 2>/dev/null | head -1)"
    rm -f "$tmp"
    if [ -z "$line" ]; then
        log_warn "Could not read $addr (g_uart_byte_count) — sanity check inconclusive"
        STATUS_UART_COUNT="unreadable"
        return 1
    fi
    word="${line##*: }"
    word="${word// /}"
    STATUS_UART_COUNT="0x$word"
    # Nonzero counter = bridge UART is alive.
    if [ "$word" = "00000000" ]; then
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# Main flash sequence (single board)
# ---------------------------------------------------------------------------

flash_one_board() {
    # ---- Pre-flight ----------------------------------------------------
    STATUS_STAGE="preflight"
    log_cyan "Pre-flight: enumerating CMSIS-DAP multiprobe(s)..."
    if ! preflight_check; then
        log_err 4 "Pre-flight failed: CMSIS-DAP multiprobe (2e8a:000c) not enumerated, or fewer than 3 interfaces visible"
        finish 4 "preflight: probe not visible"
    fi
    local probe_serial
    probe_serial="$(enumerate_probes | head -1)"
    STATUS_SERIAL="$probe_serial"
    [ -n "$probe_serial" ] && log "    probe serial: $probe_serial"

    # ---- Step 1: RDP unlock --------------------------------------------
    STATUS_STAGE="unlock"
    log ""
    log_cyan "Step 1/3: check + unlock WIO-E5 RDP over SWD (iface 2)"
    local rdp
    rdp="$(read_rdp_byte)"
    if [ -z "$rdp" ]; then
        log_err 5 "Couldn't read WIO-E5 FLASH_OPTR over SWD (iface 2). Check the multiprobe + power."
        finish 5 "RDP read failed"
    fi
    STATUS_RDP_BEFORE="0x$rdp"
    log "    FLASH_OPTR low byte (RDP) = 0x$rdp"

    if [ "$rdp" = "cc" ]; then
        log_err 5 "WIO-E5 is at RDP Level 2 (0xCC) - permanently locked, cannot recover."
        finish 5 "RDP Level 2 (0xCC) - unrecoverable"
    fi

    if [ "$rdp" != "aa" ] || [ "$FORCE_UNLOCK" -eq 1 ]; then
        log "    Running stm32wlx unlock 0 + option_load 0 (mass erase + reboot, ~25 s)..."
        if ! unlock_wio_e5; then
            log_err 5 "stm32wlx unlock 0 / option_load 0 failed"
            finish 5 "unlock command failed"
        fi
        log "    Waiting 30 s for mass erase to complete..."
        sleep 30
        local rdp2
        rdp2="$(read_rdp_byte || true)"
        STATUS_RDP_AFTER="0x${rdp2:-?}"
        if [ "$rdp2" != "aa" ]; then
            log_err 5 "Post-unlock RDP read = 0x${rdp2:-?} (expected 0xaa)"
            finish 5 "post-unlock RDP not 0xAA"
        fi
    else
        STATUS_RDP_AFTER="0x$rdp"
        log "    Already at RDP Level 0 - skipping unlock."
    fi

    # ---- Step 2: bridge flash (with retries) ---------------------------
    STATUS_STAGE="bridge_flash"
    log ""
    log_cyan "Step 2/3: flash WIO-E5 bridge firmware (iface 2)"
    local rc=1 i
    for i in 1 2 3; do
        STATUS_BRIDGE_ATTEMPTS="$i"
        if [ "$i" -gt 1 ]; then
            log "    Attempt $i/3 after 10 s wait..."
            sleep 10
        fi
        if invoke_openocd 2 target/stm32wlx.cfg "$STATUS_BRIDGE_ELF"; then
            rc=0
            break
        fi
        rc=$?
    done
    if [ "$rc" -ne 0 ]; then
        log_err 6 "Bridge flash failed after 3 attempts (last rc=$rc)"
        finish 6 "bridge flash failed"
    fi

    # ---- Step 3: meshtastic flash --------------------------------------
    if [ "$SKIP_MESHTASTIC" -ne 1 ]; then
        STATUS_STAGE="meshtastic_flash"
        log ""
        log_cyan "Step 3/3: flash meshtastic firmware to Display CPU (iface 0)"
        if ! invoke_openocd 0 target/rp2350.cfg "$STATUS_MESH_ELF" --rp2350; then
            log_err 7 "Meshtastic flash failed"
            finish 7 "meshtastic flash failed"
        fi

        # ---- Post-flash sanity check -----------------------------------
        if [ "$SKIP_SANITY_CHECK" -ne 1 ]; then
            STATUS_STAGE="sanity_check"
            log ""
            log_cyan "Post-flash sanity check: reading g_uart_byte_count..."
            # Wait briefly for firmware to boot and start counting.
            sleep 3
            if ! sanity_check_uart_counter "$STATUS_MESH_ELF"; then
                log_err 7 "Sanity check: g_uart_byte_count = ${STATUS_UART_COUNT:-?} (expected non-zero)"
                finish 7 "sanity check failed - UART counter is zero"
            fi
            log "    g_uart_byte_count = $STATUS_UART_COUNT (OK)"
        fi
    fi

    STATUS_STAGE="done"
    log ""
    log "DONE. Board is up."
    return 0
}

# ---------------------------------------------------------------------------
# Top-level driver
# ---------------------------------------------------------------------------

main() {
    # Toolchain checks
    log_cyan "Locating OpenOCD..."
    if ! find_openocd; then
        log_err 2 "OpenOCD not found. Install the Pico SDK (with bundled OpenOCD) or set PICO_OPENOCD_DIR, or put 'openocd' in PATH."
        finish 2 "OpenOCD not found"
    fi
    log "    $OCD_BIN"
    [ -n "$OCD_SCRIPTS" ] && log "    scripts: $OCD_SCRIPTS"

    # Resolve artifacts
    log_cyan "Locating artifacts..."
    local bridge mesh
    bridge="$(resolve_artifact 'wio-e5-bridge.elf' 'wio-e5-bridge/.pio/build/wio-e5-bridge/firmware.elf')" || true
    if [ -z "$bridge" ]; then
        log_err 3 "Missing wio-e5-bridge.elf (try --use-local-builds or drop the release ELF in $ARTIFACTS_DIR)"
        finish 3 "missing wio-e5-bridge.elf"
    fi
    STATUS_BRIDGE_ELF="$bridge"
    log "    bridge:     $bridge"

    if [ "$SKIP_MESHTASTIC" -ne 1 ]; then
        mesh="$(resolve_artifact 'meshtastic-freewili.elf' 'meshtastic-firmware/.pio/build/freewili/firmware-freewili-*.elf')" || true
        if [ -z "$mesh" ]; then
            log_err 3 "Missing meshtastic-freewili.elf (try --use-local-builds or drop the release ELF in $ARTIFACTS_DIR)"
            finish 3 "missing meshtastic-freewili.elf"
        fi
        STATUS_MESH_ELF="$mesh"
        log "    meshtastic: $mesh"
    fi

    # USB retry loop: probe enumeration sometimes lags 0-20 s after plug-in.
    local tries=0
    while [ "$tries" -lt 3 ]; do
        if preflight_check; then break; fi
        tries=$((tries + 1))
        if [ "$tries" -lt 3 ]; then
            log_warn "Multiprobe not yet visible; retrying in 10 s ($tries/3)..."
            sleep 10
        fi
    done

    if [ "$LOOP_MODE" -ne 1 ]; then
        flash_one_board
        finish 0
    fi

    # ---- Loop mode -----------------------------------------------------
    log_cyan "Loop mode: flashing each new probe serial seen on lsusb. Ctrl-C to stop."
    local last_serial=""
    while :; do
        local cur
        cur="$(enumerate_probes | head -1)"
        if [ -n "$cur" ] && [ "$cur" != "$last_serial" ]; then
            log ""
            log_cyan "New probe detected: $cur"
            # Reset per-board status fields
            STATUS_STAGE="init"
            STATUS_RESULT="pending"
            STATUS_ERROR=""
            STATUS_SERIAL="$cur"
            STATUS_RDP_BEFORE=""
            STATUS_RDP_AFTER=""
            STATUS_UART_COUNT=""
            STATUS_BRIDGE_ATTEMPTS=0
            STATUS_TIMESTAMP_START="$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)"

            if flash_one_board; then
                STATUS_RESULT="success"
                STATUS_EXIT=0
                write_json_log
                log "    Board $cur done. Unplug and plug next board."
            else
                # flash_one_board calls finish() on error which would exit;
                # if we reach here, treat as success.
                :
            fi
            last_serial="$cur"
        fi
        sleep 2
    done
}

main "$@"
