#!/usr/bin/env bash
#
# Half of the M6 exit test: libwayland's own test suite, run on Sirius by
# /bin/wltest. Boots the "wayland test suite" entry and checks the serial log
# -- or, given a log, only checks it.
#
#   ./tools/ports.sh add libffi wayland          once: build the port, put it in the sysroot
#   make image ENTRY=10 && ./tools/check-wayland.sh
#   ./tools/check-wayland.sh some-earlier.log
#
# Every test must pass except the ones listed in KNOWN below, each with the
# kernel feature it is waiting for. A listed test that starts passing is
# reported too: the list should shrink, not rot.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="${1:-}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-600}"

# Tests that cannot pass yet, each waiting for a kernel feature. None at the
# moment: the last four needed threads.
KNOWN=""

fail() { printf 'wayland test FAILED: %s\n' "$*" >&2; exit 1; }

if [ -z "$LOG" ]; then
    LOG="$ROOT/build/wayland-tests.log"
    rm -f "$LOG"
    accel=()
    [ -w /dev/kvm ] && [ "${NO_KVM:-0}" != 1 ] && accel=(--kvm)
    printf '\033[1;34m==> running the suite in QEMU (up to %ss)\033[0m\n' "$BOOT_TIMEOUT"
    "$ROOT/tools/run-qemu.sh" --headless "${accel[@]}" --log "$LOG" >/dev/null 2>&1 &
    QEMU_PID=$!
    trap 'kill "$QEMU_PID" 2>/dev/null || true' EXIT
    for _ in $(seq 1 "$BOOT_TIMEOUT"); do
        kill -0 "$QEMU_PID" 2>/dev/null || break
        grep -aq '^\[wltest\] [0-9]* of [0-9]* test programs passed' "$LOG" 2>/dev/null && break
        grep -aq 'KERNEL PANIC\|\[wltest\] no test suite' "$LOG" 2>/dev/null && break
        command sleep 1
    done
    kill "$QEMU_PID" 2>/dev/null || true
    trap - EXIT
fi

[ -f "$LOG" ] || fail "no log at $LOG"
! grep -aq "KERNEL PANIC" "$LOG" || fail "the kernel panicked"
! grep -aq '\[wltest\] no test suite' "$LOG" || fail "the image has no test suite: ./tools/ports.sh add libffi wayland, then rebuild the image"
grep -aq '^\[wltest\] [0-9]* of [0-9]* test programs passed' "$LOG" || fail "the suite did not run to the end"

# Each test reports itself as:  test "name":<tab>exit status N, pass.  (or: signal N, fail.)
results="$(grep -ao 'test "[a-z0-9_]*":.*\(pass\|fail\)\.' "$LOG" | sed 's/test "\([a-z0-9_]*\)".* \(pass\|fail\)\./\1 \2/')"
total="$(printf '%s\n' "$results" | grep -c . || true)"
passed="$(printf '%s\n' "$results" | grep -c ' pass$' || true)"
failed="$(printf '%s\n' "$results" | awk '$2 == "fail" { print $1 }' | sort)"

unexpected=""
for name in $failed; do
    case " $KNOWN " in *" $name "*) ;; *) unexpected="$unexpected $name" ;; esac
done
fixed=""
for name in $KNOWN; do
    printf '%s\n' "$failed" | grep -qx "$name" || fixed="$fixed $name"
done

printf '    %s tests in %s programs: %s passed, %s failed\n' "$total" \
    "$(grep -ac '^\[wltest\] running ' "$LOG")" "$passed" "$((total - passed))"
[ "$total" -ge 100 ] || fail "only $total tests ran"
[ -z "$unexpected" ] || fail "unexpected failures:$unexpected"
[ -z "$fixed" ] || fail "these are listed as known failures but passed -- take them off the list:$fixed"

if [ "$passed" = "$total" ]; then
    printf 'wayland test ok: all %s pass\n' "$total"
else
    printf 'wayland test ok: %s of %s pass; the other %s are known (%s)\n' "$passed" "$total" "$((total - passed))" \
        "$(printf '%s' "$KNOWN" | sed 's/_tst//g; s/ /, /g')"
fi
