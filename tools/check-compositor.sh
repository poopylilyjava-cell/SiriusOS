#!/usr/bin/env bash
#
# The M8 exit test: Wayland clients in windows the desktop decorates. Boots
# the desktop and, through QEMU's monitor, uses it:
#
#   - start weston-simple-shm from the panel: a window appears, and keeps
#     animating -- frames go on being drawn with nobody touching anything
#   - start weston-terminal from the panel, click into it, type commands;
#     its shell runs them (one writes to /dev/console, which is how this
#     script sees it)
#   - move the terminal by its title bar and resize it by its corner: the
#     client follows the new size
#   - close simple-shm with its close button: the client is asked, quits,
#     and its window goes
#
#   ./tools/ports.sh add libffi wayland wayland-protocols pixman libpng libxkbcommon cairo weston-clients
#   make image ENTRY=9 && ./tools/check-compositor.sh [screenshot.png]
#
# Like check-desktop.sh it learns where things are from siriusd's status
# line on the serial console.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$ROOT/build/compositor.png}"
LOG="${OUT%.png}.log"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}"

log()  { printf '\033[1;34m==> %s\033[0m\n' "$*"; }
fail() { printf 'compositor test FAILED: %s\n' "$*" >&2; [ -f "$LOG" ] && grep -a '^\[siriusd\]' "$LOG" | tail -n 4 >&2; exit 1; }

command -v python3 >/dev/null || fail "missing host tool: python3"
accel=()
[ -w /dev/kvm ] && [ "${NO_KVM:-0}" != 1 ] && accel=(--kvm)

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
[ "$(status_lines)" -ge 2 ] || fail "siriusd drew no frames within ${BOOT_TIMEOUT}s"
grep -aq '^\[siriusd\] wayland: listening on' "$LOG" || fail "siriusd has no compositor (built without the wayland port?)"

log "starting clients, typing, moving, resizing, closing"
python3 - "$WORK/monitor.sock" "$LOG" "$WORK/shot.ppm" <<'PYEOF' || fail "see above"
import re, socket, sys, time

sock_path, log_path, shot = sys.argv[1:4]
monitor_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
monitor_socket.connect(sock_path)
time.sleep(0.5)
monitor_socket.recv(65536)

def monitor(command, wait=0.12):
    monitor_socket.sendall((command + "\n").encode())
    time.sleep(wait)

def text():
    return open(log_path, errors="replace").read()

def status():
    lines = [l for l in text().split("\n") if l.startswith("[siriusd]") and " windows" in l]
    line = lines[-1]
    windows = {}
    for name, x, y, w, h, focused in re.findall(r"(\w+#\d+) at (-?\d+),(-?\d+) (\d+)x(\d+)( focused)?", line):
        windows[name] = dict(x=int(x), y=int(y), w=int(w), h=int(h), focused=bool(focused))
    clients = re.search(r"(\d+) clients", line)
    mouse = re.search(r"mouse at (-?\d+),(-?\d+)", line)
    return dict(
        windows=windows,
        clients=int(clients.group(1)) if clients else -1,
        mouse=(int(mouse.group(1)), int(mouse.group(2))),
        launchers={name: (int(x), int(y)) for name, x, y in re.findall(r" (\w+)@(\d+),(\d+)", line)},
        drawn=int(re.search(r"(\d+) drawn", line).group(1)),
        seen=len(lines),
    )

def settle():
    seen = status()["seen"]
    for _ in range(100):
        time.sleep(0.1)
        if status()["seen"] > seen + 1:
            return status()
    sys.exit("siriusd stopped reporting")

def wait_for(predicate, what, seconds=30):
    deadline = time.time() + seconds
    while time.time() < deadline:
        state = settle()
        if predicate(state):
            return state
    sys.exit("timed out waiting for " + what + "\n    " + repr(status()))

def window(state, prefix):
    for name, w in state["windows"].items():
        if name.startswith(prefix + "#"):
            return name, w
    return None, None

def move_to(x, y):
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
    while dx or dy:
        sx = max(-20, min(20, dx)); sy = max(-20, min(20, dy))
        monitor(f"mouse_move {sx} {sy}", 0.05)
        dx -= sx; dy -= sy
    time.sleep(0.3)
    monitor("mouse_button 0", 0.3)

KEYS = {" ": "spc", "-": "minus", "/": "slash", ".": "dot", ">": "shift-dot", "\n": "ret", "_": "shift-minus", ";": "semicolon"}
def type_text(s):
    for c in s:
        monitor("sendkey " + KEYS.get(c, c), 0.12)

def expect(condition, what):
    if not condition:
        sys.exit(what + "\n    " + repr(status()))
    print("    ok: " + what)

state = settle()
expect("Wayland_Demo" in state["launchers"] and "Wayland_Terminal" in state["launchers"],
       "the panel offers the installed Wayland programs")
expect(state["clients"] == 0, "no clients yet")

# --- weston-simple-shm ---------------------------------------------------------------
click(*state["launchers"]["Wayland_Demo"])
state = wait_for(lambda s: window(s, "simple_shm")[0] is not None, "simple-shm's window")
name, shm = window(state, "simple_shm")
expect(state["clients"] == 1 and shm["w"] >= 250 and shm["h"] >= 250, f"weston-simple-shm has a window: {name} {shm['w']}x{shm['h']}")
move_to(shm["x"] + shm["w"] + 300, shm["y"] + shm["h"] + 200)     # out of its way, and then leave it alone
time.sleep(2.5)
state = settle()
expect(state["drawn"] >= 20, f"and animates by itself: {state['drawn']} frames drawn in the last second")

# --- weston-terminal ---------------------------------------------------------------------
click(*state["launchers"]["Wayland_Terminal"])
state = wait_for(lambda s: window(s, "Wayland_Terminal")[0] is not None, "weston-terminal's window")
name, term = window(state, "Wayland_Terminal")
expect(state["clients"] == 2, f"weston-terminal has a window: {name} at {term['x']},{term['y']} {term['w']}x{term['h']}")
time.sleep(1.0)
click(term["x"] + term["w"] // 2, term["y"] + term["h"] // 2)
state = wait_for(lambda s: window(s, "Wayland_Terminal")[1]["focused"], "the terminal to take the focus", 10)
type_text("echo m8-wayland-ok > /dev/console\n")
type_text("mkdir /tmp/m8; echo typed in a wayland window > /tmp/m8/note; cat /tmp/m8/note > /dev/console\n")
for _ in range(60):
    if "m8-wayland-ok" in text() and "typed in a wayland window" in text():
        break
    time.sleep(0.2)
expect("m8-wayland-ok" in text(), "keys typed in weston-terminal reach its shell, through a pseudo-terminal")
expect("typed in a wayland window" in text(), "and the shell runs a pipeline of commands")

# --- move and resize it -----------------------------------------------------------------------
_, term = window(settle(), "Wayland_Terminal")
drag(term["x"] + 60, term["y"] + 10, 200, 80)
_, after = window(settle(), "Wayland_Terminal")
expect((after["x"], after["y"]) == (term["x"] + 200, term["y"] + 80), f"moved by its title bar to {after['x']},{after['y']}")
term = after
drag(term["x"] + term["w"] - 3, term["y"] + term["h"] - 3, -120, -60)
time.sleep(1.5)
_, after = window(settle(), "Wayland_Terminal")
expect(after["w"] < term["w"] and after["h"] < term["h"] and abs(after["w"] - (term["w"] - 120)) <= 20 and abs(after["h"] - (term["h"] - 60)) <= 20,
       f"resized by its corner, and the client drew at the new size: {term['w']}x{term['h']} to {after['w']}x{after['h']}")

# --- close simple-shm ---------------------------------------------------------------------------
_, shm = window(settle(), "simple_shm")
click(shm["x"] + shm["w"] - 14, shm["y"] + 11)
state = wait_for(lambda s: window(s, "simple_shm")[0] is None, "simple-shm to close", 15)
expect(state["clients"] == 1, "closed by its close button: the client quit and its window went")

monitor(f"screendump {shot}", 1.5)
monitor("quit", 0.2)
PYEOF
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=""

! grep -aq "KERNEL PANIC" "$LOG" || fail "the kernel panicked"
! grep -aq "siriusd': killed" "$LOG" || fail "siriusd was killed"
! grep -aq "weston-[a-z-]*': killed" "$LOG" || fail "a client was killed"
[ -s "$WORK/shot.ppm" ] || fail "screendump produced nothing"
if command -v magick >/dev/null; then magick "$WORK/shot.ppm" "$OUT"
elif command -v convert >/dev/null; then convert "$WORK/shot.ppm" "$OUT"
else cp "$WORK/shot.ppm" "${OUT%.png}.ppm"; fi

printf 'compositor test ok: weston-simple-shm and weston-terminal ran in decorated windows and took input (screenshot: %s)\n' "$OUT"
