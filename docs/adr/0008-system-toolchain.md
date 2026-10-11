# ADR 0008 — The x86_64-sirius toolchain: static programs on mlibc, sysdeps in-tree

**Status:** accepted · 2026-10-10 · **milestone:** M4

## Context
M4 gives userland a real compiler and C library: an `x86_64-sirius` GCC, mlibc
on the Sirius system calls (ADR 0002), and libstdc++ on top. The exit test is
a C++ program using `std::vector`, `std::string` and iostreams, running on the
OS. Everything after this is written against a hosted toolchain, so the shape
chosen here is what every later program is built on.

## Decisions

### A real OS target, not a bare-metal compiler with flags
binutils and GCC learn the triple `x86_64-sirius` (the patches under `ports/`). The
compiler then knows its own sysroot, start files, default libraries and
`__sirius__`, and `x86_64-sirius-g++ hello.cpp` works with no flags at all --
which is what CMake, meson and every ported `configure` script assume.

*Rejected:* driving `x86_64-elf-gcc` with `-nostdlib` and a spec file. It
works for one program and fails for the first third-party build system, and
libstdc++ cannot be configured for a target that has no name.

### Every program is statically linked
`LINK_SPEC` passes `-static` unconditionally and there is no dynamic linker.
The kernel loads `ET_EXEC` images only; shared objects need a loader that maps
files, relocates, and a kernel that can share those mappings, none of which a
desktop with a handful of programs needs yet.

The cost is size: a C program is ~850 KiB of mlibc, a C++ one with iostreams
~1.6 MiB, stripped. `tools/mkimage.sh` strips programs on their way to the
disk and the build tree keeps the debug information.

### The linker separates code from read-only data
ld is configured with `--enable-separate-code`, so text, `.rodata` and data
get a segment each and the kernel maps them r-x, r-- and rw-. For an OS it
does not know, ld would otherwise put `.rodata` (and the ELF headers) in the
executable segment.

### The sysdeps live in this repository
`user/libc/sysdeps/sirius` is the whole port: `sysdeps.cpp` (one function per
kernel service), `crt1.S`/`crti.S`/`crtn.S`, and the list of what is
implemented. `toolchain/build-sirius.sh` copies it into the mlibc tree
together with `kernel/include/sirius/`, so the system call numbers have one
definition, shared by the kernel, the C library and (as `<sirius/syscall.h>`)
programs. The mlibc patch itself is two lines of `meson.build`.

*Rejected:* keeping the sysdeps as a patch against mlibc. They change with
every system call the kernel gains; that is source code, and it should be
reviewed, grepped and bisected like the rest.

### mlibc uses Linux's ABI headers
`abi-bits/` are mlibc's `abis/linux` headers, unchanged. The kernel already
uses Linux's errno values, open flags, mode bits and wait statuses (ADR 0007),
so most calls pass arguments and results straight through. The exceptions are
translated in the sysdeps: `struct stat` and `struct dirent` have their own,
smaller kernel layouts.

### What the kernel had to learn
- **`tcb_set`** sets the FS base, where the C library keeps the thread
  control block and thread-local storage. It is per thread, loaded on every
  switch to a user thread, inherited by `fork` and cleared by `execve`. Ring 3
  cannot change it behind the kernel's back (CR4.FSGSBASE stays off), so
  nothing has to be saved on the way out.
- **`clock_get`** reads the time since boot, at tick resolution.
- **`dup` and `dup2`**: mlibc's BSD option refuses to build without them.
- **An auxiliary vector** on the entry stack: `AT_PHDR`, `AT_PHENT`,
  `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`, `AT_EXECFN`. mlibc's startup finds the
  program headers -- and with them the TLS template -- through it.

### What is deliberately missing
A libc function whose sysdep is not implemented fails with `ENOSYS`. That is
how the things the kernel does not have show up, rather than as link errors:
signals, pipes, sockets, `poll`, terminals, timestamps, permissions.

A process has one thread, so the futex sysdeps do not enter the kernel: a
lock is never contended, and a wait that could only be ended by another
thread is reported as the deadlock it would be.

### libstdc++ is built last, against the installed mlibc
The order is binutils, mlibc headers, GCC and libgcc, mlibc, libstdc++.
libstdc++'s configure runs real link tests against the C library rather than
guessing, so what it enables is what mlibc has. Its `crossconfig.m4` needs a
`sirius` case, which means regenerating `configure` with exactly autoconf
2.69; the build script builds that autoconf itself.

### Two build trees
The kernel and the bring-up programs use `x86_64-elf-gcc`; userland uses
`x86_64-sirius-gcc`. One CMake build tree has one compiler, so `user/` is a
project of its own (`make userland`, in `build/userland`). `/bin/init` is
still a bring-up program, and runs the M4 test if the image carries it: the
kernel can be built and booted without the 20-minute system toolchain.

### Ports are built by jinx, with a toolchain of its own
`ports/` is a jinx tree: each package is built from a shell recipe inside a
pinned Debian container, so a port builds the same on any host. The container
cannot use the compiler in `toolchain/local` -- it sees only `ports/`, and
runs a different distribution -- so `host-recipes/` build binutils and GCC a
second time, and `recipes/` build mlibc and libstdc++ for that compiler's
sysroot. Both builds come from one set of patches (they live beside the jinx
recipes, and `toolchain/build-sirius.sh` reads them there) and one mlibc
tree, which `tools/ports.sh` stages into `ports/` because the sysdeps and the
kernel's ABI headers live outside it.

`tools/ports.sh add <port>` unpacks a built port into `/sysroot`, where the
everyday compiler links it. The two compilers are the same GCC with the same
configuration, so their objects mix; zlib built in the container is linked
by the host's `x86_64-sirius-gcc` and runs on the OS.

*Rejected:* building the everyday toolchain with jinx too. Then every edit
to a sysdep would go through a container and a package install; the script
rebuilds mlibc in seconds, in place. *Also rejected:* a ports tool of our
own. The recipes are a few lines each because jinx already handles fetching,
checksums, patches, dependency order and packaging.

## Consequences
- The toolchain's configure flags are written twice, in the build script and
  in the jinx host recipes. A change to one belongs in the other.
- An executable must fit in 4 MiB on disk: `execve` reads it whole into one
  kernel allocation, and the buddy allocator's largest block is 4 MiB. Fine
  for M4; siriusd with Dear ImGui linked in will be close to it, and the
  loader should read segments from the file instead.
- `CLOCK_REALTIME` counts from boot: there is no RTC driver.
- Changing the system call ABI means `make libc` and relinking userland.
  Programs are static, so an old binary keeps the numbers it was built with:
  a number, once used, is never reused.
- The GCC patch is carried across GCC upgrades by hand. It is small -- six
  files, one of them new -- and the bare-metal toolchain shares the tree.
- mlibc's bundled copy of GCC's intrinsics headers is older than GCC 16 and
  no longer compiles with it; a second mlibc patch prefers the compiler's own.
