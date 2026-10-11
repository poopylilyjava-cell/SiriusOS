# Sirius OS — Project Plan

A from-scratch x86_64 operating system whose desktop environment is a Dear ImGui
application acting as a Wayland compositor.

- **Kernel:** original, monolithic, C++20 freestanding, x86_64, UEFI boot
- **Userland:** C++20, mlibc + libstdc++, POSIX subset
- **DE:** `siriusd` — single ImGui process that owns the framebuffer, draws the
  shell, and speaks the Wayland protocol to client applications
- **Target hardware:** QEMU first, then one real laptop (single reference machine)

---

## 0. The decision that shapes everything

Dear ImGui is an immediate-mode UI library with **no OS dependencies**: it emits
vertex buffers and asks for input state. That makes it an unusually good fit for
a hobby OS — it needs no GTK, no Qt, no font server, no toolkit. The entire
porting burden is two files you write yourself:

| File | Responsibility |
|---|---|
| `imgui_impl_sirius.cpp` | Platform backend: feed `ImGuiIO` from kernel input events, provide clipboard + cursor + time |
| `imgui_impl_softrast.cpp` | Renderer backend: rasterize `ImDrawData` (clipped, textured triangles) into a framebuffer |

Everything else in ImGui compiles freestanding-ish already. **Write these two
files portable from day one** — they are the only ImGui-specific code in the
project, and they must run identically against a Linux host window (for fast
iteration) and against the real kernel.

The Wayland half is the opposite: it is *not* portable, and it is the single
largest risk in this project. See §6.

---

## 1. Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Apps: terminal, file manager, text editor, settings        │
│  (native ImGui clients + Wayland clients)                   │
├─────────────────────────────────────────────────────────────┤
│  siriusd — ImGui shell + Wayland compositor                 │
│    · ImGui core + imgui_impl_sirius + imgui_impl_softrast   │
│    · wl_compositor / wl_shm / wl_seat / xdg_shell           │
│    · window manager, panel, launcher, decorations           │
├─────────────────────────────────────────────────────────────┤
│  libsirius (C++) · mlibc (libc) · libstdc++ · libwayland    │
├─────────────────────────────────────────────────────────────┤
│  syscall boundary (SYSCALL/SYSRET, POSIX subset)            │
├─────────────────────────────────────────────────────────────┤
│  Kernel                                                     │
│    sched · vmm/pmm/heap · vfs · ipc(unix sockets, shm,      │
│    epoll) · signals · drivers(fb, ps2, pci, ahci, xhci,     │
│    virtio) · acpi · smp                                     │
├─────────────────────────────────────────────────────────────┤
│  Limine (UEFI bootloader): framebuffer, memmap, SMP, HHDM   │
└─────────────────────────────────────────────────────────────┘
```

**Key architectural commitments**

1. **Limine, not GRUB.** Limine hands you a linear framebuffer, a memory map, a
   higher-half direct map and APs already parked — GRUB/multiboot2 gives you
   less and costs more setup. Non-negotiable time saver.
2. **Single compositor process owns the framebuffer.** No kernel-side window
   system. The kernel exposes a framebuffer device and an input event stream;
   all policy lives in `siriusd`. This keeps the kernel small and lets you
   iterate the DE without rebuilding the kernel.
3. **Software rasterization until it hurts.** ImGui draw lists are flat/textured
   triangles with scissor rects — perfectly tractable in software. A tuned SSE2
   rasterizer sustains 60 fps of UI at 1080p. Do not write a GPU driver to get
   a desktop; write one later to make it fast.
4. **mlibc, not musl.** mlibc is explicitly designed for non-Linux targets with a
   clean `sysdeps/` porting layer; musl assumes Linux syscall semantics and
   fights you. mlibc also already carries the `libwayland` porting experience.
5. **Kernel C++ is restricted C++:** `-fno-exceptions -fno-rtti -fno-threadsafe-statics`,
   freestanding, no `std::` containers — a small in-tree `klib` instead.

---

## 2. Repository layout

```
os/
├─ kernel/           # freestanding C++20 kernel
│  ├─ arch/x86_64/   # gdt, idt, paging, apic, smp, syscall entry
│  ├─ mm/            # pmm, vmm, heap, address spaces
│  ├─ sched/         # threads, processes, scheduler, signals
│  ├─ fs/            # vfs, tmpfs, devfs, fat32, ext2
│  ├─ ipc/           # unix sockets, pipes, shm, epoll, fd passing
│  ├─ drivers/       # fb, ps2, pci, ahci, xhci, virtio, rtc, hpet
│  └─ klib/          # printf, string, vector, hashmap, lock, list
├─ user/
│  ├─ libsirius/     # C++ system library (ipc, gfx, event loop)
│  ├─ siriusd/       # the ImGui DE / Wayland compositor
│  │  ├─ backend/    # imgui_impl_sirius.cpp, imgui_impl_softrast.cpp
│  │  ├─ shell/      # panel, launcher, wm, decorations, theme
│  │  └─ wayland/    # protocol server implementation
│  ├─ apps/          # terminal, files, editor, settings, monitor
│  └─ init/          # pid 1, service supervision
├─ ports/            # jinx recipes: mlibc, gcc, libwayland, zlib, ...
├─ toolchain/        # cross-compiler build scripts
├─ third_party/      # imgui, limine, freetype/stb_truetype
├─ tests/            # host-side unit tests + QEMU integration harness
├─ tools/            # debug scripts, gdb helpers, image builder
└─ docs/
```

---

## 3. Toolchain and build

**Missing on this machine — install first:**
`nasm`, `cmake`, `ninja`, `xorriso`, `mtools`, `parted`, `gdb`, `meson`

**Two compilers, both built locally under `toolchain/`:**

| Toolchain | Triple | Used for |
|---|---|---|
| Bare-metal cross | `x86_64-elf-gcc` | the kernel (no libc) |
| System toolchain | `x86_64-sirius-gcc` | all userland (mlibc + libstdc++) |

The second requires teaching binutils and GCC a `sirius` target triple — a small
patch set against `config.sub`, `gcc/config.gcc`, and a new `gcc/config/sirius.h`.
Budget a full weekend for this; it is the classic hobby-OS wall and it blocks all
C++ userland, so do it early (M4), not late.

- **Build system:** CMake + Ninja for kernel and userland; **jinx** for the
  ports tree (simpler than xbstrap, and the recipes are shell). The system
  toolchain is built by `toolchain/build-sirius.sh` for everyday use and
  again by jinx inside its container, from the same patches (ADR 0008).
- **Image:** `tools/mkimage.sh` builds a GPT + FAT32 ESP disk image with Limine.
- **Run:** `qemu-system-x86_64 -M q35 -m 2G -smp 4 -serial stdio -s -bios OVMF.fd`
- **Debug:** QEMU gdb stub + `tools/gdbinit` with symbol loading and page-table
  walk helpers; serial `kprintf` from the first line of kernel code.

---

## 4. Milestones

Durations assume steady part-time work. They are ordering guidance, not promises.

### M0 — Skeleton boots ✅ complete
Limine boots a higher-half C++ kernel. Serial `kprintf`. GDT, IDT, exception
handlers with register dumps. Parse the Limine memmap and framebuffer response.
**Exit test:** a red rectangle drawn to the framebuffer in QEMU, and a triple
fault produces a readable stack trace on serial.

### M1 — Memory ✅ complete
Bitmap PMM → buddy allocator. 4-level VMM with per-process address spaces,
demand paging, CoW. Slab kernel heap. Global `new`/`delete`.
**Exit test:** allocate/free 100k random-sized objects across 1000 iterations
with an in-kernel stress test and a leak counter that returns to zero.

### M2 — Execution ✅ complete
Threads, processes, preemptive round-robin scheduler on the APIC timer. SMP
bring-up of APs, per-CPU runqueues, spinlocks and a mutex. `SYSCALL`/`SYSRET`
entry, first syscalls (`write`, `exit`, `yield`). ELF64 loader. Ring 3.
**Exit test:** two userspace ELF binaries printing interleaved output from
different cores.

### M3 — Files and devices ✅ complete
VFS with mounts and dentry cache. tmpfs, devfs. PCI enumeration, AHCI driver,
FAT32 read/write. `open/read/write/close/stat/mmap/fork/execve/waitpid`.
**Exit test:** a static userspace program reads a file from disk and `fork`s a
child that `execve`s another binary.

### M4 — The toolchain wall ✅ complete
Port mlibc onto the Sirius syscall layer. Build `x86_64-sirius` binutils + GCC.
Build libstdc++ against mlibc. Bring up a `ports/` tree with jinx.
**Exit test:** `x86_64-sirius-g++` compiles a C++ program using `std::vector`,
`std::string` and iostreams that runs on the OS.

> Everything after this point is ordinary application development. Everything
> before it is kernel archaeology. Do not let M4 slip.

### M5 — ImGui on bare metal ✅ complete
Write `imgui_impl_softrast.cpp` (clipped textured triangle rasterizer, SSE2) and
`imgui_impl_sirius.cpp`. PS/2 keyboard + mouse drivers feeding an event device.
Font atlas via stb_truetype.
**Exit test:** `ImGui::ShowDemoWindow()` running fullscreen on real framebuffer
with working mouse and keyboard, ≥30 fps at 1920×1080.

*Develop this milestone's code on Linux first* against an SDL host harness, so
the rasterizer and platform backend are already correct before the kernel has to
run them. `tests/host_harness/` exists for exactly this.

### M6 — IPC for Wayland ✅ complete
Unix domain sockets **with `SCM_RIGHTS` fd passing** — this is the hard,
mandatory piece. `memfd_create`-style anonymous shared memory. `poll`/`epoll`.
Signals. Pipes. A process that survives `dup2` plumbing.
**Exit test:** a ported libwayland's own test suite links and passes; two
processes exchange a shared-memory buffer by fd over a unix socket and both mmap
it coherently.

*Status:* both halves pass: `/bin/ipctest`, and all 141 tests of libwayland's
suite (`tools/check-wayland.sh`). The last four needed threads, which were not
on this milestone's list and came after M7 (ADR 0012).

### M7 — Shell without Wayland ✅ complete
`siriusd` becomes the real DE: panel, clock, app launcher, window manager for
*in-process* ImGui windows, theme, settings. Native apps (terminal, file manager,
system monitor) run as ImGui panels inside `siriusd`.
**Exit test:** boot to desktop, launch three apps, move/resize/close them, open a
terminal and run commands. **This is a usable operating system.** Ship it, take
screenshots, and only then start §6.

### M8 — Wayland compositor ✅ complete
Port libwayland (or implement `wl_*` server-side from the XML protocol specs —
seriously consider this; it's ~4k lines and avoids the port). Implement
`wl_compositor`, `wl_surface`, `wl_shm`, `wl_seat`, `wl_output`, `xdg_shell`.
Client shm buffers become ImGui textures drawn via `ImDrawList::AddImage()`
inside ImGui-decorated windows. Input routing: ImGui owns the pointer until it
enters a client surface, then events forward as `wl_pointer`/`wl_keyboard`.
**Exit test:** `weston-simple-shm` and `weston-terminal` render and accept input
inside a window your ImGui shell decorates.

*Status:* `tools/check-compositor.sh` passes: both clients start from the
panel, simple-shm animates, commands typed in weston-terminal run, its window
moves and resizes, simple-shm closes by its close button. libwayland-server
is the protocol layer (the port from M6), not a marshaller of our own; and
weston-terminal's `forkpty()` brought pseudo-terminals into the kernel
(ADR 0013).

### M9 — Real hardware (4–8 weeks)
xHCI + USB HID (PS/2 does not exist on your laptop). NVMe. ACPI shutdown,
poweroff, reboot. Real-hardware framebuffer via UEFI GOP. Bootable USB installer.
**Exit test:** boots from USB on the reference laptop, desktop usable with the
built-in keyboard and trackpad.

*The reference laptop:* a GIGABYTE AERO X16 1WH -- AMD Ryzen AI 7 350
(Krackan), keyboard on USB (xHCI), an Elan touchpad on I2C (HID over I2C,
AMD's DesignWare controllers), a Kingston NVMe SSD, Realtek RTL8852CE Wi-Fi
and RTL8111/8168 Ethernet, UEFI only.

### M10 — Networking, wired (6–10 weeks)
A network stack before any radio: Wi-Fi is only a link layer, and everything
above it -- IP, TCP, DHCP, DNS, sockets -- is built and debugged far more
easily over a cable and in QEMU.

- **Drivers:** virtio-net first (QEMU, testable in CI), then the reference
  laptop's own wired port, a Realtek RTL8111/8168 (`10ec:8168`): one TX and
  one RX descriptor ring, MSI, no firmware needed for basic operation.
- **The stack:** Ethernet, ARP, IPv4, ICMP, UDP, TCP (retransmission,
  windows; congestion control kept simple). The decision for the ADR is
  writing it versus porting **lwIP** (BSD-licensed, built to be embedded,
  needs only threads, semaphores and mailboxes from its host); porting is
  weeks, writing is months.
- **Sockets:** `AF_INET` stream and datagram sockets as vnodes, like the unix
  sockets, so `poll`/`epoll` work unchanged; mlibc's network sysdeps.
- **Configuration:** a DHCP client (a small daemon in user space), DNS
  through mlibc's resolver and `/etc/resolv.conf`.
- **Desktop:** connection state in the panel.

**Exit test:** in QEMU (user networking) and on the laptop by cable, the
desktop gets an address by DHCP, `ping` reaches the gateway, and a program
fetches a web page by name over HTTP.

### M11 — Wi-Fi (3–6 months)
The reference laptop's radio is a **Realtek RTL8852CE** (`10ec:c852`, PCIe,
802.11ax, 2.4/5/6 GHz; Linux's `rtw89` driver). Station mode only: join an
access point, nothing else.

1. **Firmware.** The chip runs Realtek's firmware (`rtw89/rtw8852c_fw.bin`
   in linux-firmware, redistributable under its own licence). It goes on
   the root filesystem under `/lib/firmware`, copied by `tools/mkimage.sh`
   with its licence, and the kernel learns to read a file for a driver.
2. **The PCIe driver.** Power-on sequence, firmware download, TX/RX DMA
   rings, MSI, then the MAC, baseband and RF initialisation -- the bulk of
   it register tables. `rtw89` is dual-licensed GPL-2.0 **or BSD-3-Clause**,
   so it can be read and its tables ported under BSD terms. Start on
   2.4/5 GHz with 20 MHz channels and legacy/HT rates; wider channels, HE
   and 6 GHz once that works.
3. **802.11 MAC layer** (what Linux calls mac80211): scanning (passive on
   DFS channels, probe requests elsewhere), open authentication,
   association, capabilities negotiation, data frames to and from Ethernet
   frames (802.11 header, LLC/SNAP), so the M10 stack sees one more
   Ethernet-like interface.
4. **Security:** WPA2-Personal first -- PBKDF2-SHA1 for the PSK, the EAPOL
   4-way handshake, PTK/GTK derivation, keys handed to the chip's CCMP
   engine. WPA3-SAE (Dragonfly over P-256) second: most routers run WPA2/WPA3
   transition mode, which takes WPA2. The handshake runs in a user-space
   daemon (`wifid`, a small supplicant) over a control interface to the
   kernel, with crypto from a ported library (BearSSL or Mbed TLS, which
   TLS will want anyway).
5. **Regulatory:** a country setting and its channel list; no transmitting
   where scanning must be passive.
6. **Desktop:** a network list in the panel, a password prompt, saved
   networks in `/etc`.

*Testing without the chip:* QEMU emulates no Wi-Fi hardware. Layers 3-5 get
a software radio pair in the kernel (like Linux's `mac80211_hwsim`) and an
in-kernel access point on its other end, so scanning, association and the
handshake run in QEMU and CI; the crypto is unit-tested on the host against
RFC 6070 (PBKDF2) and IEEE 802.11's handshake test vectors. Only layers 1-2
need the laptop.

**Exit test:** on the reference laptop, the desktop lists nearby networks,
joins a WPA2-Personal network with a typed password, gets an address by
DHCP, and fetches a web page -- with the cable unplugged.

### M12 — Beyond (open-ended)
virtio-gpu / a real GPU driver for accelerated ImGui rendering · TLS and a
web client · audio · a package manager · installing to the NVMe beside
another system · self-hosting (build Sirius on Sirius).

---

## 5. Cross-cutting engineering practice

- **Serial-first debugging.** Every subsystem gets a `LOG_*` category with
  runtime-toggleable levels before it gets features.
- **Host-testable by default.** PMM, VMM page-table math, VFS path resolution,
  the rasterizer, and the Wayland protocol marshalling all compile and unit-test
  on Linux. Only the parts that genuinely touch hardware require QEMU.
- **CI from M1.** GitHub Actions: build kernel + userland, boot in QEMU headless,
  assert on serial output markers, fail on timeout. A boot regression caught a
  week later costs a day to bisect.
- **Kernel sanitizers.** Guard pages on the slab allocator, poison freed memory,
  and a stack-canary check — a `-fsanitize=undefined` build config too.
- **One commit, one subsystem.** You will bisect this repo constantly.
- **`docs/adr/`** — record each irreversible decision (bootloader, libc, ABI,
  protocol) with the alternatives you rejected and why.

---

## 6. Risks, honestly

| Risk | Severity | Mitigation |
|---|---|---|
| **wlroots is unusable here.** It hard-depends on DRM/KMS, libinput, udev, seatd — Linux-only subsystems you do not have. | High | Never plan around wlroots. Implement the Wayland server protocol directly (M8), or port only libwayland's core marshalling. |
| **`SCM_RIGHTS` fd passing is deep kernel work** and Wayland cannot function without it. | High | M6 is scheduled before any Wayland work and has its own exit test. If M6 stalls, M7 still ships a real desktop. |
| **The `x86_64-sirius` GCC port (M4)** blocks all C++ userland including ImGui itself. | High | Scheduled early. Fallback: static-link with a bare-metal toolchain and a stub libc for M5 only. |
| **Software rasterization too slow** at high resolution. | Medium | Dirty-rect damage tracking, render at 1280×720 upscaled, then virtio-gpu 2D blits, then a GPU driver. ImGui's vertex volume is small; fill rate is the cost. |
| **Scope collapse** — this is a multi-year project and M8 alone can consume a year. | High | M7 is deliberately a complete, shippable desktop with zero Wayland. Treat M8 as a separate project that starts only after M7 ships. |
| **Real hardware is a different OS** than QEMU (USB, ACPI quirks, no PS/2). | Medium | Pick exactly one reference laptop at M0 and check its chipset. Do not chase broad hardware support. |
| **Wi-Fi is the largest driver in the project.** The RTL8852CE's Linux driver is tens of thousands of lines, most of them chip-specific initialisation, and it cannot be exercised in QEMU. | High | Networking over a cable first (M10), so only the radio itself is new in M11. A software radio for everything above the driver. Fallback: a USB Wi-Fi adapter with a simpler, well-documented chip on the xHCI driver, or the wired port alone. |
| **WPA3-only networks.** SAE is far harder than WPA2's handshake. | Medium | WPA2 first; most routers offer transition mode. SAE as its own step, on a ported crypto library. |
| **Burnout after M2** — the classic hobby-OS death point, where there's much work and nothing to see. | High | M5 deliberately front-loads the visual payoff: a graphical ImGui demo on your own kernel before the boring plumbing of M6. |

---

## 7. What to do first

1. Install the missing tools (`nasm cmake ninja xorriso mtools parted gdb meson`).
2. `git init`, create the directory skeleton from §2, add `.gitignore`.
3. Vendor Limine and Dear ImGui into `third_party/`.
4. Build `x86_64-elf` binutils + GCC into `toolchain/`.
5. Write `kernel/arch/x86_64/boot.cpp` + a linker script until serial says hello.
6. Stand up the QEMU run script and the GitHub Actions boot test **on day one** —
   before there is anything to regress.

Then M0.
