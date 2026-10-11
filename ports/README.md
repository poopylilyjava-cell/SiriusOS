# Ports

Third-party software for Sirius, built by [jinx](https://github.com/Mintsuki/Jinx)
from the recipes in this directory.

```bash
./tools/ports.sh build zlib     # build a port and everything it needs
./tools/ports.sh add zlib       # ...and unpack it into /sysroot
./tools/ports.sh rebuild zlib   # any other jinx command passes straight through
```

After `add`, the port is in the sysroot `x86_64-sirius-gcc` searches, and a
program under `user/` can simply link it (`target_link_libraries(name.elf
PRIVATE z)`).

## How it fits together

jinx builds each package in a Debian container (pinned by `Jinxfile`) that
can see this directory and nothing else of the repository. Two consequences
shape the tree:

**The container has its own toolchain.** `host-recipes/binutils` and
`host-recipes/gcc` build the `x86_64-sirius` compiler a second time, inside
the container, from the same sources and the same patches as
`toolchain/build-sirius.sh`. The patches exist once, in the `patches/`
directory beside each recipe, and the script reads them from there. The
configure flags exist twice; a change to one belongs in the other.

**The C library's source is staged.** The Sirius half of mlibc lives in
`user/libc/sysdeps/sirius`, and the system call numbers in
`kernel/include/sirius` -- outside this directory. `tools/ports.sh` assembles
the complete, patched mlibc tree under `.staging/mlibc` before it runs jinx,
and the two mlibc recipes build that tree in place.

| Recipe | What |
|---|---|
| `host-recipes/binutils` | assembler and linker for `x86_64-sirius` |
| `host-recipes/gcc` | the compiler and libgcc |
| `recipes/mlibc-headers` | the C library's headers, which libgcc is built against |
| `recipes/mlibc` | the C library |
| `recipes/libstdcxx` | libstdc++, configured on its own against the installed mlibc |
| `recipes/zlib` | the first port |
| `recipes/libffi` | calls through signatures known only at run time; libwayland needs it |
| `host-recipes/wayland-scanner` | Wayland's protocol compiler, for the build machine; owns the Wayland source and its patch |
| `recipes/wayland` | libwayland-client and -server, and their test suite in `/usr/libexec/wayland-tests` |
| `recipes/wayland-protocols` | the XML of xdg-shell and the other protocols beyond the core |
| `recipes/pixman` | pixel compositing, under cairo |
| `recipes/libpng` | PNG images |
| `recipes/libxkbcommon` | keymaps: clients compile the one the compositor sends |
| `recipes/cairo` | 2D drawing, without FreeType or fontconfig |
| `recipes/weston-clients` | `weston-simple-shm` and `weston-terminal`, in `/usr/bin`: M8's exit test (ADR 0013) |

Autoconf recipes pass `--build="${SIRIUS_BUILD}"`. Without it `configure`
tests whether it is cross-compiling by running a program it just built, and a
Sirius executable does start on Linux -- then hangs.

## Adding a port

Create `recipes/<name>/recipe`; `recipes/zlib/recipe` is the model. It needs
`hostdeps="gcc binutils"`, `deps="mlibc"` (and `libstdcxx` for C++), and
whichever Debian packages its build tools come from in `imagedeps`. Programs
are statically linked, so configure for static libraries only. Executables a
port installs in `/usr/bin`, and its data in `/usr/share`, `tools/mkimage.sh`
puts on the disk (`/usr/share` by name: see its list).

Patches go in `recipes/<name>/patches/`. To write one, build the port once,
edit `sources/<name>-workdir/`, and run `./tools/ports.sh regen <name>`.

Build output lives in `toolchain/build/ports` (packages in `pkgs/` and
`host-pkgs/`); the downloaded sources and the container cache appear here, in
`sources/`, `host-sources/` and `.jinx-cache/`, all ignored by git.
