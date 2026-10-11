# Sirius OS

An x86_64 operating system written from scratch, whose desktop environment is a
Dear ImGui application acting as a Wayland compositor.

- **Kernel** — original, monolithic, C++20 freestanding, UEFI boot via Limine
- **Userland** — C++20 on mlibc + libstdc++, POSIX subset
- **Desktop** — `siriusd`: one ImGui process that owns the framebuffer, draws the
  shell, and serves the Wayland protocol to client applications

The full roadmap, architecture and risk analysis live in **[docs/PLAN.md](docs/PLAN.md)**.

## Status

**M8 complete: a desktop that is a Wayland compositor.** `make run` boots to it:
the kernel keeps the screen black, and the desktop comes up out of it behind
an animated loading screen, drawn with Dear ImGui like everything after it.

- **Panel** — a launcher, the open windows, and a clock that knows the time
- **Windows** — move, resize, close, minimize from the panel
- **Terminal** — a command line onto `/bin/sh`: pipes, redirection, `ls`,
  `cat`, `cp`, `ps`, `kill` and twenty more commands, history, Ctrl+C
- **Files** — browse, create and delete; double-click opens the **Editor**
- **System Monitor** — per-CPU load, memory, the process table, and a button
  to end a process
- **Settings** — theme, accent colour, text size, wallpaper, keyboard layout
  (US or German), pointer speed; saved to `/etc/siriusd.conf`

- **Wayland programs** — `weston-terminal` and `weston-simple-shm`, unchanged
  Linux programs built for Sirius, in windows the desktop decorates; started
  from the panel, moved, resized, closed like the rest

Everything on screen is drawn by one process, `siriusd`, with Dear ImGui into
the framebuffer through a software rasterizer; it sleeps when nothing changes.
The built-in applications are compiled into it; other programs are Wayland
clients, and `siriusd` is their compositor (ADR 0013).

Underneath: an original kernel (SMP, a VFS with FAT32 and tmpfs, signals,
unix sockets with descriptor passing, shared memory, epoll, pseudo-terminals),
mlibc and libstdc++ built by an `x86_64-sirius` GCC, and a jinx ports tree:
libwayland, cairo, pixman, libxkbcommon and the weston clients among it.
Processes can have threads (`pthread`), all on one CPU. Not there yet: dynamic
linking, USB, networking.

The first boot entry runs the system's own tests instead of the desktop:
`/bin/fstest` (101 checks), `/bin/cxxtest` (60), `/bin/ipctest` (102),
`/bin/threadtest` (39) and `/bin/ptytest` (30).

Next: **M9**, real hardware.

## Quick start

```bash
make deps          # prints the host packages to install
make submodules    # limine + limine-protocol + imgui
make toolchain     # builds x86_64-elf binutils + gcc  (~30 min, once)
make toolchain-sirius  # x86_64-sirius gcc, mlibc, libstdc++  (~30 min, once)
make kernel        # build the kernel
make userland      # build the mlibc programs
make run           # boot the desktop in QEMU under UEFI (ENTRY=1: the tests)
make test          # host-side unit tests
make harness       # the desktop in a window on this machine, no kernel involved
./tools/ports.sh add libffi wayland wayland-protocols pixman libpng libxkbcommon cairo weston-clients
                   # the Wayland programs (~20 min the first time); then make run again
```

The kernel needs only the first toolchain: without the second there is no
desktop, and `make run ENTRY=1` boots the kernel and the bring-up programs,
with `/bin/init` skipping the rest.

In the QEMU window, click to give it the mouse; Ctrl+Alt+G takes it back.
`make run` uses KVM when the host has it. Without it the desktop still runs,
at a fraction of the speed.

## Writing a program

```bash
x86_64-sirius-g++ -O2 -o hello hello.cpp     # a static executable for Sirius
```

To have it on the disk image, add it to `user/` with `sirius_program(name
sources...)`; `make run` then installs it as `/bin/name`. After changing the
C library's sysdeps or a system call number, `make libc` rebuilds mlibc and
the next `make userland` relinks everything against it.

## Boot entries

`boot/limine.conf` defines several entries; `DEFAULT_ENTRY=N` picks one without
editing the file, which is how the fault tests run unattended:

```bash
make image ENTRY=9 && ./tools/check-desktop.sh              # the desktop, driven and checked from outside
make image ENTRY=9 && ./tools/check-compositor.sh           # Wayland clients on it (needs the ports above)
make image ENTRY=10 && ./tools/check-wayland.sh             # libwayland's test suite (needs the wayland port)
DEFAULT_ENTRY=3 ./tools/mkimage.sh && ./tools/run-qemu.sh   # force a #PF
DEFAULT_ENTRY=4 ./tools/mkimage.sh && ./tools/run-qemu.sh   # force a #DF
DEFAULT_ENTRY=7 ./tools/mkimage.sh && ./tools/run-qemu.sh   # misbehaving user programs
DEFAULT_ENTRY=8 ./tools/mkimage.sh && ./tools/run-qemu.sh   # the M2 two-ticker test
./tools/screenshot.sh build/shot.png                        # headless capture
```

Fish users: `toolchain/env.sh` is bash. Add the toolchain to your path with
`fish_add_path (pwd)/toolchain/local/bin`.

## Layout

| Path | What |
|---|---|
| `kernel/` | the freestanding kernel — `arch/`, `mm/`, `sched/`, `exec/`, `fs/`, `block/`, `drivers/`, `klib/`; `include/` holds the user-visible ABI |
| `user/bringup/` | static programs (`init`, `fstest`, …) built with the kernel compiler and no C library |
| `user/libc/` | the Sirius half of mlibc: the sysdeps that turn libc calls into system calls |
| `user/tests/` | programs that test the system from inside it (`cxxtest`, `ipctest`, `threadtest`, `ptytest`, `wltest`) |
| `sysroot/` | (generated) mlibc, libstdc++ and their headers, as `x86_64-sirius-gcc` sees them |
| `rootfs/` | files copied onto the root partition; the programs land in `/bin` |
| `user/siriusd/` | the desktop: `backend/` (the two files that are the Dear ImGui port), `shell/` (panel, windows, settings), `apps/` (terminal, files, editor, monitor), `wayland/` (the compositor) |
| `user/apps/` | programs that run in a terminal: `sh`, with its commands built in |
| `user/libsirius/` | C++ wrappers for what POSIX has no call for: system and process information |
| `tests/host_harness/` | the same desktop in an SDL window on the build machine |
| `toolchain/` | build scripts for both compilers, and pinned versions |
| `ports/` | jinx recipes for third-party software, and the patches that teach binutils, GCC and mlibc about Sirius |
| `tools/` | image builder, QEMU runner, gdb helpers |
| `tests/` | host-side unit tests + QEMU integration harness |
| `docs/adr/` | architecture decision records |

## Development loop

```bash
make run                     # build + boot
make debug                   # boot halted with a gdb stub on :1234
make gdb                     # attach, with a `pagewalk <va>` helper
make test                    # host-side unit tests (asan + ubsan)
make harness                 # run siriusd's rendering and input code on this machine
make libc                    # rebuild mlibc after touching user/libc or the syscall ABI
./tools/ports.sh add zlib    # build a port with jinx and unpack it into the sysroot
./tools/ports.sh add libffi wayland   # libwayland, and its tests into the next image
```

Panic backtraces print raw addresses; pipe a log through `tools/symbolize.sh`
to resolve them to file and line. `build/qemu.log` records guest errors and interrupts — read it first when the
machine triple-faults. `build/kernel/sirius.map` resolves addresses to symbols.

## Conventions

- One commit, one subsystem. This repository gets bisected constantly.
- Every subsystem gets logging before it gets features.
- Anything that can be tested on the host is tested on the host.
- Irreversible decisions get an ADR in `docs/adr/` naming the rejected alternatives.
