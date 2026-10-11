#!/usr/bin/env bash
#
# Build the x86_64-sirius system toolchain: the compiler for all of userland.
#
#   binutils + GCC, taught the `sirius` target by the patches in ports/
#   mlibc, with the Sirius sysdeps from user/libc/sysdeps/sirius
#   libstdc++, built against that mlibc
#
# The tools go next to the kernel's x86_64-elf ones in toolchain/local; the C
# library, its headers and libstdc++ go into the sysroot at /sysroot, which
# the compiler searches by default. The order is the usual bootstrap:
#
#   binutils -> libc headers -> gcc + libgcc -> libc -> libstdc++
#
# libgcc needs the C library's headers to build, the C library needs the
# compiler, and libstdc++ needs both.
#
# Usage:  ./toolchain/build-sirius.sh [-j N] [--force] [--libc | --sources]
#   --libc     rebuild and reinstall only mlibc (after editing the sysdeps)
#   --sources  only fetch and patch the sources (tools/ports.sh uses this)
# Resumable: each stage drops a stamp file, so a re-run skips finished work.
# mlibc itself is always rebuilt; ninja makes that a no-op when it is current.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/toolchain/versions.env"

TARGET=x86_64-sirius
PREFIX="$ROOT/toolchain/local"
SYSROOT="$ROOT/sysroot"
SRC="$ROOT/toolchain/src"
BUILD="$ROOT/toolchain/build"
STAMPS="$BUILD/.stamps"
# The patches sit where jinx looks for them, beside the recipe that owns the
# source, so the ports tree and this script build the same thing.
BINUTILS_PATCHES="$ROOT/ports/host-recipes/binutils/patches"
GCC_PATCHES="$ROOT/ports/host-recipes/gcc/patches"
MLIBC_PATCHES="$ROOT/ports/recipes/mlibc/patches"
HOST_TOOLS="$BUILD/host-tools"
JOBS="$(nproc)"
FORCE=0
LIBC_ONLY=0
SOURCES_ONLY=0

while [ $# -gt 0 ]; do
    case "$1" in
        -j) JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        --force) FORCE=1; shift ;;
        --libc) LIBC_ONLY=1; shift ;;
        --sources) SOURCES_ONLY=1; shift ;;
        -h|--help) sed -n '2,23p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ "$FORCE" = 1 ] && rm -f "$STAMPS"/sirius-*

log()   { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
info()  { printf '    %s\n' "$*"; }
die()   { printf '\n\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }
done_stage() { [ -f "$STAMPS/sirius-$1" ]; }
mark()  { mkdir -p "$STAMPS"; touch "$STAMPS/sirius-$1"; }

BINUTILS_SRC="$SRC/binutils-$BINUTILS_VERSION"
GCC_SRC="$SRC/gcc-$GCC_VERSION"
MLIBC_SRC="$SRC/mlibc-$MLIBC_VERSION"

# --- preflight -------------------------------------------------------------
log "preflight"
missing=()
for t in curl tar make gcc g++ bison flex makeinfo patch git meson ninja rsync; do
    command -v "$t" >/dev/null || missing+=("$t")
done
[ ${#missing[@]} -eq 0 ] || die "missing host tools: ${missing[*]}"
info "target      $TARGET"
info "prefix      $PREFIX"
info "sysroot     $SYSROOT"
info "binutils    $BINUTILS_VERSION"
info "gcc         $GCC_VERSION"
info "mlibc       $MLIBC_VERSION"
info "jobs        $JOBS"

mkdir -p "$SRC" "$BUILD" "$PREFIX" "$SYSROOT/usr/include" "$SYSROOT/usr/lib"
export PATH="$PREFIX/bin:$PATH"

fetch() { # fetch <url> <file>
    if [ -f "$SRC/$2" ]; then info "have $2"; return; fi
    info "downloading $2"
    curl -fL --retry 3 --progress-bar -o "$SRC/$2.part" "$1"
    mv "$SRC/$2.part" "$SRC/$2"
}

# Apply every patch in a directory, skipping the ones already in the tree: a
# patch that reverses cleanly has been applied. The source trees are shared
# with build-cross.sh and survive across runs, so this must be idempotent.
apply_patches() { # apply_patches <source dir> <patch dir>
    local p
    for p in "$2"/*.patch; do
        [ -f "$p" ] || continue
        if patch -d "$1" -p1 -R -s -f --dry-run < "$p" >/dev/null 2>&1; then
            info "have $(basename "$p")"
        else
            info "applying $(basename "$p")"
            patch -d "$1" -p1 -s < "$p" || die "$(basename "$p") does not apply to $1"
        fi
    done
}

# --- sources ---------------------------------------------------------------
log "sources"
fetch "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" "binutils-$BINUTILS_VERSION.tar.xz"
[ -d "$BINUTILS_SRC" ] || tar -C "$SRC" -xf "$SRC/binutils-$BINUTILS_VERSION.tar.xz"
apply_patches "$BINUTILS_SRC" "$BINUTILS_PATCHES"

fetch "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" "gcc-$GCC_VERSION.tar.xz"
[ -d "$GCC_SRC" ] || tar -C "$SRC" -xf "$SRC/gcc-$GCC_VERSION.tar.xz"
[ -d "$GCC_SRC/gmp" ] || ( cd "$GCC_SRC" && ./contrib/download_prerequisites )
apply_patches "$GCC_SRC" "$GCC_PATCHES"

if [ ! -d "$MLIBC_SRC" ]; then
    info "cloning mlibc $MLIBC_VERSION"
    git clone -q --depth 1 --branch "v$MLIBC_VERSION" https://github.com/managarm/mlibc "$MLIBC_SRC"
fi
apply_patches "$MLIBC_SRC" "$MLIBC_PATCHES"

# The GCC patch touches libstdc++'s crossconfig.m4, so its configure script
# has to be regenerated -- and GCC insists on exactly the autoconf it was
# released with. That one is built here rather than asked of the host.
if ! grep -q 'sirius' "$GCC_SRC/libstdc++-v3/configure"; then
    if [ ! -x "$HOST_TOOLS/bin/autoconf" ]; then
        log "autoconf $AUTOCONF_VERSION (for libstdc++'s configure)"
        fetch "https://ftp.gnu.org/gnu/autoconf/autoconf-$AUTOCONF_VERSION.tar.xz" "autoconf-$AUTOCONF_VERSION.tar.xz"
        [ -d "$SRC/autoconf-$AUTOCONF_VERSION" ] || tar -C "$SRC" -xf "$SRC/autoconf-$AUTOCONF_VERSION.tar.xz"
        rm -rf "$BUILD/autoconf"; mkdir -p "$BUILD/autoconf"
        ( cd "$BUILD/autoconf"
          "$SRC/autoconf-$AUTOCONF_VERSION/configure" --prefix="$HOST_TOOLS" >/dev/null
          make -j"$JOBS" >/dev/null 2>&1
          make install >/dev/null 2>&1 )
    fi
    info "regenerating libstdc++-v3/configure"
    ( cd "$GCC_SRC/libstdc++-v3" && PATH="$HOST_TOOLS/bin:$PATH" autoconf && rm -rf autom4te.cache )
    grep -q 'sirius' "$GCC_SRC/libstdc++-v3/configure" || die "libstdc++-v3/configure did not pick up the sirius target"
fi

# The Sirius half of mlibc lives in this repository, not in a patch: copy it
# into place, give it mlibc's Linux ABI headers, and the kernel's own.
sync_sysdeps() {
    local dest="$MLIBC_SRC/sysdeps/sirius" h
    rsync -a --delete --exclude 'include/abi-bits' --exclude 'include/sirius' \
        "$ROOT/user/libc/sysdeps/sirius/" "$dest/"
    mkdir -p "$dest/include/abi-bits" "$dest/include/sirius"
    for h in "$MLIBC_SRC"/abis/linux/*.h; do
        ln -sf "../../../../abis/linux/$(basename "$h")" "$dest/include/abi-bits/$(basename "$h")"
    done
    rsync -a --delete "$ROOT/kernel/include/sirius/" "$dest/include/sirius/"
}

# Meson needs to be told about the target; the compilers are named by full
# path so that nothing depends on the caller's PATH.
write_cross_file() { # write_cross_file <file> <with compilers: 0|1>
    {
        if [ "$2" = 1 ]; then
            echo "[binaries]"
            echo "c = '$PREFIX/bin/$TARGET-gcc'"
            echo "cpp = '$PREFIX/bin/$TARGET-g++'"
            echo "ar = '$PREFIX/bin/$TARGET-ar'"
            echo "strip = '$PREFIX/bin/$TARGET-strip'"
            echo
        fi
        echo "[host_machine]"
        echo "system = 'sirius'"
        echo "cpu_family = 'x86_64'"
        echo "cpu = 'x86_64'"
        echo "endian = 'little'"
    } > "$1"
}

MLIBC_OPTIONS=( --prefix=/usr --libdir=lib -Ddefault_library=static )

build_libc() {
    log "mlibc $MLIBC_VERSION"
    command -v "$TARGET-gcc" >/dev/null || die "$TARGET-gcc not found; build the compiler first"
    sync_sysdeps
    write_cross_file "$BUILD/sirius.cross" 1
    if [ ! -f "$BUILD/mlibc/build.ninja" ]; then
        rm -rf "$BUILD/mlibc"
        meson setup "$BUILD/mlibc" "$MLIBC_SRC" --cross-file "$BUILD/sirius.cross" "${MLIBC_OPTIONS[@]}"
    fi
    ninja -C "$BUILD/mlibc" -j "$JOBS"
    DESTDIR="$SYSROOT" ninja -C "$BUILD/mlibc" install >/dev/null
    info "installed into $SYSROOT/usr"
}

if [ "$SOURCES_ONLY" = 1 ]; then
    sync_sysdeps
    info "sources ready in $SRC"
    exit 0
fi

if [ "$LIBC_ONLY" = 1 ]; then
    build_libc
    exit 0
fi

# --- binutils --------------------------------------------------------------
# --enable-separate-code gives text, read-only data and writable data a
# segment each, so the kernel can map them r-x, r-- and rw-. Left to its
# default for an unknown OS, ld would put .rodata in the executable segment.
if done_stage binutils; then
    log "binutils $BINUTILS_VERSION (already built)"
else
    log "binutils $BINUTILS_VERSION"
    rm -rf "$BUILD/sirius-binutils"; mkdir -p "$BUILD/sirius-binutils"
    ( cd "$BUILD/sirius-binutils"
      "$BINUTILS_SRC/configure" \
          --target="$TARGET" --prefix="$PREFIX" --with-sysroot="$SYSROOT" \
          --disable-nls --disable-werror --enable-gold=no \
          --enable-default-execstack=no --enable-separate-code
      make -j"$JOBS"
      make install )
    mark binutils
fi

# --- libc headers ----------------------------------------------------------
if done_stage libc-headers; then
    log "mlibc headers (already installed)"
else
    log "mlibc headers"
    sync_sysdeps
    write_cross_file "$BUILD/sirius-headers.cross" 0
    rm -rf "$BUILD/mlibc-headers"
    meson setup "$BUILD/mlibc-headers" "$MLIBC_SRC" --cross-file "$BUILD/sirius-headers.cross" \
        "${MLIBC_OPTIONS[@]}" -Dheaders_only=true
    DESTDIR="$SYSROOT" ninja -C "$BUILD/mlibc-headers" install >/dev/null
    mark libc-headers
fi

# --- gcc and libgcc --------------------------------------------------------
if done_stage gcc; then
    log "gcc $GCC_VERSION (already built)"
else
    log "gcc $GCC_VERSION"
    command -v "$TARGET-as" >/dev/null || die "$TARGET-as not found; binutils stage did not install"
    rm -rf "$BUILD/sirius-gcc"; mkdir -p "$BUILD/sirius-gcc"
    ( cd "$BUILD/sirius-gcc"
      "$GCC_SRC/configure" \
          --target="$TARGET" --prefix="$PREFIX" --with-sysroot="$SYSROOT" \
          --enable-languages=c,c++ --enable-threads=posix --enable-initfini-array \
          --disable-nls --disable-shared --disable-multilib --disable-bootstrap
      make -j"$JOBS" all-gcc
      make -j"$JOBS" all-target-libgcc
      make install-gcc
      make install-target-libgcc )
    mark gcc
fi

# --- libc ------------------------------------------------------------------
build_libc

# --- libstdc++ -------------------------------------------------------------
if done_stage libstdcxx; then
    log "libstdc++ (already built)"
else
    log "libstdc++"
    ( cd "$BUILD/sirius-gcc"
      make -j"$JOBS" all-target-libstdc++-v3
      make install-target-libstdc++-v3 )
    mark libstdcxx
fi

# --- verify ----------------------------------------------------------------
log "verify"
"$TARGET-gcc" --version | head -1
"$TARGET-ld"  --version | head -1
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cat > "$WORK/hello.cpp" <<'PROGRAM'
#include <iostream>
#include <string>
#include <vector>
int main() {
    std::vector<std::string> words{"hello", "from", "sirius"};
    for (const auto& word : words) std::cout << word << '\n';
}
PROGRAM
"$TARGET-g++" -O2 -o "$WORK/hello" "$WORK/hello.cpp" || die "$TARGET-g++ cannot link a C++ program"
"$TARGET-readelf" -h "$WORK/hello" | grep -q 'EXEC (Executable file)' || die "expected a static ET_EXEC executable"
info "a C++ program using iostream, string and vector links"

log "done"
echo "    source toolchain/env.sh   # bash"
echo "    fish_add_path $PREFIX/bin # fish"
