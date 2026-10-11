#!/usr/bin/env bash
# Attach gdb to a QEMU started with tools/run-qemu.sh --debug
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec gdb -x "$ROOT/tools/gdbinit" "${1:-$ROOT/build/kernel/sirius.elf}"
