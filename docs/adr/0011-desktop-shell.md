# ADR 0011 — The shell: applications inside the desktop, a terminal without ttys, drawing on demand

**Status:** accepted · 2026-10-10 · **milestone:** M7

## Context
M7 turns `siriusd` from a demo into a desktop: a panel with a launcher, a task
list and a clock; windows that move, resize and close; a terminal, a file
manager, a system monitor, settings. The exit test is to boot, launch three
applications, rearrange them, and run commands in a terminal.

## Decisions

### Applications are objects inside siriusd
Each is a class with a `Draw()` (`user/siriusd/shell/app.h`); the desktop owns
its window. Dear ImGui is the window manager: title bars, moving, resizing,
stacking and focus are its own, and the panel is its main menu bar, which is
what keeps windows from going under it. A crash in an application is a crash
of the desktop, and one slow `Draw()` stalls every window.

That is the plan's deliberate order: the desktop is useful before there is a
protocol for windows that belong to other processes. M8's Wayland clients
will be a second kind of window beside these, not a replacement.

### The terminal talks to a shell through pipes
There are no ttys in the kernel, so the terminal is not a VT100. It runs
`/bin/sh -i` with three pipes, shows what comes out, and sends what is typed a
line at a time; line editing and history happen in the input field, before
the shell sees anything. Ctrl+C signals the shell, which passes it to what it
is running. It understands newline, carriage return, backspace, tab and
"clear the screen", and drops every other escape sequence.

The cost: nothing that wants a real terminal can run in it -- no editor that
draws its own screen, no `isatty`, no job control. Programs see a pipe and
buffer their output accordingly, so output arrives when they flush or exit.
A pseudo-terminal is the fix, and belongs with the first program that needs
one.

*Rejected:* a pty in the kernel now. A line discipline, sessions, process
groups and `SIGTTOU` are a milestone of their own, for no visible gain while
the only client is a shell.

### One shell binary carries the commands
`sh` has `ls`, `cat`, `cp`, `ps` and twenty more compiled in, and runs them in
a forked child so that pipes and redirection work as they would for separate
programs. A static executable is a megabyte of C library; twenty-five of them
would be twenty-five megabytes of the same code on a FAT partition. Anything
not built in is executed from `/bin`.

### The desktop draws when something changes
The main loop sleeps in `poll()` on the input device and the terminals' pipes,
with a timeout: a few milliseconds while the user is doing something, a
quarter of a second otherwise -- enough for the clock and the monitor's
graphs. An idle desktop draws about four frames a second instead of two
thousand, and the CPU it ran on is idle.

The whole screen is still redrawn each time. The wallpaper is rendered once
into a buffer and copied in place of clearing the frame.

### The kernel tells the time and describes itself
- The **CMOS real-time clock** is read once at boot; `CLOCK_REALTIME` is that
  plus the monotonic clock. There are no time zones: the panel shows whatever
  the machine's clock is set to.
- **`sysinfo`** and **`proclist`** report memory, per-CPU tick counts and the
  process table. CPU time is sampled: each timer tick is charged to whatever
  it interrupted. `libsirius` wraps them for C++, with stand-in data when
  built for Linux.

### Settings are a text file
`/etc/siriusd.conf`, lines of `key = value`, written when the user presses
Save. Theme, accent colour, text size, wallpaper, keyboard layout (US or
German, with AltGr as a third level), pointer speed.

## Consequences
- No drag and drop between applications, no clipboard beyond the process, no
  notifications: there is one process, so "between applications" is a
  function call nobody has written yet.
- Window positions are not remembered across boots.
- `mv` is a copy and a delete: the kernel has no `rename`.
- The file manager cannot rename or copy, for the same reason and for want
  of a design for long operations in a single-threaded desktop.
- Under QEMU without KVM a frame with several large windows takes tens of
  milliseconds: the rasterizer redraws every pixel of every overlapping
  window. Occlusion or damage tracking is the next rendering work.
- `siriusd` is pid 1. If it exits, so does the session; there is no login,
  and no init to restart it.
