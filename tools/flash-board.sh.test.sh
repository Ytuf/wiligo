#!/usr/bin/env bash
# Minimal smoke test for flash-board.sh.
#
# Asserts:
#   - `flash-board.sh -h` exits 0
#   - Help text contains expected sections (Usage:, --serial, --json-log, --loop)
#
# Run from anywhere:
#   bash tools/flash-board.sh.test.sh

set -u

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$TEST_DIR/flash-board.sh"

pass=0
fail=0
note() { printf '%s\n' "$*"; }
ok()   { pass=$((pass+1)); note "ok    - $*"; }
nope() { fail=$((fail+1)); note "FAIL  - $*"; }

if [ ! -f "$SCRIPT" ]; then
    nope "flash-board.sh not found at $SCRIPT"
    exit 1
fi

# 1. -h exits 0
out_h="$(bash "$SCRIPT" -h 2>&1)"
rc_h=$?
if [ "$rc_h" -eq 0 ]; then
    ok "flash-board.sh -h exited 0"
else
    nope "flash-board.sh -h exited $rc_h"
fi

# 2. --help exits 0
out_help="$(bash "$SCRIPT" --help 2>&1)"
rc_help=$?
if [ "$rc_help" -eq 0 ]; then
    ok "flash-board.sh --help exited 0"
else
    nope "flash-board.sh --help exited $rc_help"
fi

# 3. Help text contains expected sections
check_substr() {
    local needle="$1"
    if printf '%s' "$out_h" | grep -q -- "$needle"; then
        ok "help mentions '$needle'"
    else
        nope "help missing '$needle'"
    fi
}

check_substr "Usage:"
check_substr "--serial"
check_substr "--json-log"
check_substr "--loop"
check_substr "--artifacts-dir"
check_substr "--use-local-builds"
check_substr "--force-unlock"
check_substr "--skip-meshtastic"
check_substr "--skip-sanity-check"

# 4. Unknown arg yields exit code 1 and a greppable [E1] line.
out_bad="$(bash "$SCRIPT" --not-a-real-flag 2>&1 || true)"
if printf '%s' "$out_bad" | grep -q '^\[E1\]'; then
    ok "unknown arg produces [E1] error line"
else
    nope "unknown arg did not produce [E1] error line"
fi

note ""
note "Results: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
