#!/usr/bin/env bash
#
# Resolve kernel addresses to file:line. Pipe a panic backtrace into it, or
# pass addresses as arguments:
#
#   ./tools/run-qemu.sh --log boot.log; ./tools/symbolize.sh < boot.log
#   ./tools/symbolize.sh 0xffffffff80001e4f

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL="${KERNEL:-$ROOT/build/kernel/sirius.elf}"
ADDR2LINE="$ROOT/toolchain/local/bin/x86_64-elf-addr2line"

[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL" >&2; exit 1; }

resolve() { "$ADDR2LINE" -e "$KERNEL" -f -C -p "$1" 2>/dev/null || echo "?"; }

if [ $# -gt 0 ]; then
    for addr in "$@"; do printf '%s  %s\n' "$addr" "$(resolve "$addr")"; done
    exit 0
fi

# Rewrite any 0x-prefixed kernel address found on stdin, leaving the rest alone.
while IFS= read -r line; do
    addr="$(grep -oE '0xffffffff[0-9a-f]{8}' <<<"$line" | head -1 || true)"
    if [ -n "$addr" ]; then
        printf '%s\n        %s\n' "$line" "$(resolve "$addr")"
    else
        printf '%s\n' "$line"
    fi
done
