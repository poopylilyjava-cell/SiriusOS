#!/usr/bin/env bash
#
# The M7 exit test: boot to the desktop and use it, from outside. QEMU's
# monitor plays the user with the PS/2 mouse and keyboard:
#
#   - launch three applications from the panel (Terminal, Files, Monitor)
#   - resize one window, move another, close a third
#   - type commands into the terminal and see that the shell ran them
#
#   make image ENTRY=9 && ./tools/check-desktop.sh [screenshot.png]
#
# siriusd describes its windows on the serial console once a second, which is
# how this script knows where to click and whether it worked. One command
# typed into the terminal writes to /dev/console, so its output shows up on
# the serial console too.
#
# The cost of a frame is held to MIN_FPS (default 30) only when QEMU can use
# KVM. Under emulation the number says how fast the host emulates SSE2, not
# how fast the rasterizer is; it is printed either way.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/desktop.png}"
LOG="${OUT%.png}.log"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"

log()  { printf '\033[1;34m==> %s\033[0m\n' "$*"; }
fail() { printf 'desktop test FAILED: %s\n' "$*" >&2; [ -f "$LOG" ] && grep -a '^\[siriusd\]' "$LOG" | tail -n 4 >&2; exit 1; }

command -v python3 >/dev/null || fail "missing host tool: python3"

accel=()
if [ -w /dev/kvm ] && [ "${NO_KVM:-0}" != 1 ]; then
    accel=(--kvm)
    MIN_FPS="${MIN_FPS:-30}"
else
    MIN_FPS="${MIN_FPS:-1}"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; [ -n "${QEMU_PID:-}" ] && kill "$QEMU_PID" 2>/dev/null || true' EXIT
rm -f "$LOG"

log "booting the desktop${accel[0]:+ with KVM}"
"$ROOT/tools/run-qemu.sh" --headless "${accel[@]}" --monitor "$WORK/monitor.sock" --log "$LOG" >/dev/null 2>&1 &
QEMU_PID=$!

status_lines() { local n; n="$(grep -ac '^\[siriusd\] .* windows' "$LOG" 2>/dev/null)" || n=0; echo "$n"; }
for _ in $(seq 1 "$BOOT_TIMEOUT"); do
    kill -0 "$QEMU_PID" 2>/dev/null || fail "qemu exited early"
    [ "$(status_lines)" -ge 2 ] && break
    command sleep 1
done
grep -aq '^\[siriusd\] display ' "$LOG" 2>/dev/null || fail "siriusd did not open the display within ${BOOT_TIMEOUT}s"
[ "$(status_lines)" -ge 2 ] || fail "siriusd drew no frames within ${BOOT_TIMEOUT}s"

log "launching, resizing, moving, closing, typing"
python3 - "$WORK/monitor.sock" "$LOG" "$WORK/shot.ppm" <<'PYEOF' || fail "see above"
import re, socket, sys, time

sock_path, log_path, shot = sys.argv[1:4]
monitor_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
monitor_socket.connect(sock_path)
time.sleep(0.5)
monitor_socket.recv(65536)                      # monitor banner

def monitor(command, wait=0.12):
    monitor_socket.sendall((command + "\n").encode())
    time.sleep(wait)

def status():
    """The last complete status line, parsed."""
    lines = [l for l in open(log_path, errors="replace").read().split("\n") if l.startswith("[siriusd]") and " windows" in l]
    line = lines[-1]
    mouse = re.search(r"mouse at (-?\d+),(-?\d+)", line)
    windows = {}
    for name, x, y, w, h, focused in re.findall(r"(\w+#\d+) at (-?\d+),(-?\d+) (\d+)x(\d+)( focused)?", line):
        windows[name] = dict(x=int(x), y=int(y), w=int(w), h=int(h), focused=bool(focused))
    return dict(
        count=int(re.search(r"(\d+) windows", line).group(1)),
        mouse=(int(mouse.group(1)), int(mouse.group(2))),
        launchers={name: (int(x), int(y)) for name, x, y in re.findall(r" (\w+)@(\d+),(\d+)", line)},
        windows=windows,
        frame_ms=float(re.search(r"frame ([\d.]+) ms", line).group(1)),
        drawn=int(re.search(r"(\d+) drawn", line).group(1)),
        seen=len(lines),
    )

def settle():
    """Wait for a status line written after now."""
    seen = status()["seen"]
    for _ in range(100):
        time.sleep(0.1)
        if status()["seen"] > seen + 1:
            return status()
    sys.exit("siriusd stopped reporting")

def move_to(x, y):
    """The mouse is a relative device: steer it by what siriusd says its position is."""
    for _ in range(6):
        cx, cy = settle()["mouse"]
        if (cx, cy) == (x, y):
            return
        dx, dy = x - cx, y - cy
        while dx or dy:
            sx = max(-40, min(40, dx)); sy = max(-40, min(40, dy))
            monitor(f"mouse_move {sx} {sy}", 0.03)
            dx -= sx; dy -= sy
    sys.exit(f"the pointer would not go to {x},{y}: it is at {status()['mouse']}")

def click(x, y):
    move_to(x, y)
    monitor("mouse_button 1", 0.15)
    monitor("mouse_button 0", 0.3)

def drag(x, y, dx, dy):
    move_to(x, y)
    monitor("mouse_button 1", 0.3)
    remaining_x, remaining_y = dx, dy
    while remaining_x or remaining_y:
        sx = max(-20, min(20, remaining_x)); sy = max(-20, min(20, remaining_y))
        monitor(f"mouse_move {sx} {sy}", 0.05)
        remaining_x -= sx; remaining_y -= sy
    time.sleep(0.3)
    monitor("mouse_button 0", 0.3)

KEYS = {" ": "spc", "-": "minus", "/": "slash", ".": "dot", ">": "shift-dot", "|": "shift-backslash", "\n": "ret",
        ";": "semicolon", "_": "shift-minus", "=": "equal", "&": "shift-7"}
def type_text(text):
    for c in text:
        monitor("sendkey " + KEYS.get(c, c), 0.12)

def expect(condition, what):
    if not condition:
        sys.exit(what + "\n    " + repr(status()))
    print("    ok: " + what)

# --- launch three applications -------------------------------------------------------
state = settle()
expect(state["count"] == 0 and len(state["launchers"]) >= 4, "the desktop is up, with a panel and no windows")
expect(state["drawn"] <= 12, f"left alone it sleeps: {state['drawn']} frames drawn in the last second")
for name in ("Terminal", "Files", "Monitor"):
    click(*state["launchers"][name])
state = settle()
expect(state["count"] == 3 and {"Terminal#1", "Files#2", "Monitor#3"} <= set(state["windows"]), "three applications launched from the panel")

# --- resize the monitor by its corner ---------------------------------------------------
m = state["windows"]["Monitor#3"]
drag(m["x"] + m["w"] - 3, m["y"] + m["h"] - 3, 60, 40)
after = settle()["windows"]["Monitor#3"]
expect(after["w"] == m["w"] + 60 and after["h"] == m["h"] + 40 and (after["x"], after["y"]) == (m["x"], m["y"]),
       f"a window resized by its corner: {m['w']}x{m['h']} to {after['w']}x{after['h']}")

# --- move it by its title bar, out of the way ---------------------------------------------
m = after
drag(m["x"] + 120, m["y"] + 10, 820, 60)
after = settle()["windows"]["Monitor#3"]
expect((after["x"], after["y"]) == (m["x"] + 820, m["y"] + 60) and (after["w"], after["h"]) == (m["w"], m["h"]),
       f"a window moved by its title bar: {m['x']},{m['y']} to {after['x']},{after['y']}")

# --- close the file manager with its close button -------------------------------------------
f = settle()["windows"]["Files#2"]
click(f["x"] + f["w"] - 14, f["y"] + 11)
state = settle()
expect(state["count"] == 2 and "Files#2" not in state["windows"], "a window closed by its close button")

# --- the terminal: focus it and run commands ---------------------------------------------------
t = state["windows"]["Terminal#1"]
click(t["x"] + 200, t["y"] + t["h"] - 20)        # its command line
type_text("echo m7-terminal-ok > /dev/console\n")
type_text("mkdir /tmp/m7; echo made by the shell > /tmp/m7/note; cat /tmp/m7/note | wc > /dev/console\n")
type_text("ls /bin\n")
for _ in range(50):
    text = open(log_path, errors="replace").read()
    if "m7-terminal-ok" in text and re.search(r"^1 4 18$", text, re.M):
        break
    time.sleep(0.2)
expect("m7-terminal-ok" in text, "a command typed into the terminal ran: its output reached the console")
expect(re.search(r"^1 4 18$", text, re.M), "a pipeline with redirection ran: mkdir, echo > file, cat | wc")
state = settle()
expect(state["windows"]["Terminal#1"]["focused"], "the terminal has the focus")
time.sleep(1.5)
state = settle()
expect(state["drawn"] <= 25, f"and with a terminal waiting for input it still sleeps: {state['drawn']} frames in a second")

monitor(f"screendump {shot}", 1.5)
print("FRAME_MS %.2f" % state["frame_ms"])
monitor("quit", 0.2)
PYEOF
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

! grep -aq "KERNEL PANIC" "$LOG" || fail "the kernel panicked"
! grep -aq "siriusd': killed" "$LOG" || fail "siriusd was killed"
[ -s "$WORK/shot.ppm" ] || fail "screendump produced nothing"

# What a frame costs, from the last status line: the desktop only draws when
# something changes, so the rate it could sustain is 1000 / that.
frame_ms="$(grep -a '^\[siriusd\] .* windows' "$LOG" | tail -n 1 | sed -n 's/.*frame \([0-9.]*\) ms.*/\1/p')"
python3 - "$frame_ms" "$MIN_FPS" <<'PYEOF' || fail "frames cost too much"
import sys
ms, minimum = float(sys.argv[1]), float(sys.argv[2])
fps = 1000.0 / ms if ms > 0 else 0.0
print(f"    a frame takes {ms:.2f} ms: {fps:.0f} fps possible")
sys.exit(0 if fps >= minimum else f"{fps:.0f} fps is below the required {minimum:.0f}")
PYEOF

if command -v magick >/dev/null; then magick "$WORK/shot.ppm" "$OUT"
elif command -v convert >/dev/null; then convert "$WORK/shot.ppm" "$OUT"
else cp "$WORK/shot.ppm" "${OUT%.png}.ppm"; fi

printf 'desktop test ok: three applications launched, windows resized, moved and closed, commands run in the terminal (screenshot: %s)\n' "$OUT"
