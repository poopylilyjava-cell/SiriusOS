#!/usr/bin/env bash
#
# Boot the image headlessly and capture the framebuffer to a PNG.
#
#   ./tools/screenshot.sh [out.png] [seconds-to-wait]
#
# Drives QEMU's monitor over a unix socket, so it needs no display and works in
# CI. tools/check-desktop.sh does the same for the desktop, and drives it too.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-$ROOT/build/sirius.img}"
OUT="${1:-$ROOT/build/screenshot.png}"
WAIT="${2:-6}"

# shellcheck source=tools/ovmf.sh
. "$ROOT/tools/ovmf.sh"

log() { printf '\033[1;34m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }

[ -f "$IMAGE" ] || die "no image at $IMAGE -- run tools/mkimage.sh"
find_ovmf || exit 1
command -v magick >/dev/null || command -v convert >/dev/null || die "need ImageMagick to convert the PPM"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; [ -n "${QEMU_PID:-}" ] && kill "$QEMU_PID" 2>/dev/null || true' EXIT

VARS="$WORK/OVMF_VARS.fd"
cp "$OVMF_VARS" "$VARS"

log "booting headless"
qemu-system-x86_64 \
    -M q35 -m 2G -smp 4 -cpu qemu64 \
    -drive "file=$IMAGE,format=raw,if=none,id=disk0" \
    -device ahci,id=ahci -device ide-hd,drive=disk0,bus=ahci.0 \
    -drive "if=pflash,unit=0,format=raw,readonly=on,file=$OVMF_CODE" \
    -drive "if=pflash,unit=1,format=raw,file=$VARS" \
    -display none -serial "file:$WORK/serial.log" \
    -monitor "unix:$WORK/monitor.sock,server,nowait" \
    -no-reboot &
QEMU_PID=$!

log "waiting ${WAIT}s for the kernel to draw"
for _ in $(seq 1 "$WAIT"); do
    kill -0 "$QEMU_PID" 2>/dev/null || die "qemu exited early; serial log:$(printf '\n'; cat "$WORK/serial.log")"
    command sleep 1
done

log "capturing framebuffer"
python3 - "$WORK/monitor.sock" "$WORK/shot.ppm" <<'PYEOF'
import socket, sys, time

sock_path, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
time.sleep(0.5)
s.recv(65536)                                  # monitor banner
s.sendall(f"screendump {out}\n".encode())
time.sleep(1.5)
s.sendall(b"quit\n")
s.close()
PYEOF

[ -s "$WORK/shot.ppm" ] || die "screendump produced nothing"

if command -v magick >/dev/null; then magick "$WORK/shot.ppm" "$OUT"; else convert "$WORK/shot.ppm" "$OUT"; fi
log "wrote $OUT ($(du -h "$OUT" | cut -f1))"

# The serial log is usually wanted alongside the picture.
cp "$WORK/serial.log" "${OUT%.png}.log" 2>/dev/null || true
