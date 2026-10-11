#!/usr/bin/env bash
#
# Build the bare-metal x86_64-elf cross toolchain used for the Sirius kernel.
#
# This is the M0 toolchain: binutils + GCC with no libc, no headers, and a
# libgcc variant built without the red zone (kernel interrupt handlers cannot
# tolerate the System V red zone).
#
# The separate x86_64-sirius *system* toolchain (mlibc + libstdc++, for all
# userland) is built by toolchain/build-sirius.sh.
#
# Usage:  ./toolchain/build-cross.sh [-j N] [--clean] [--force]
# Resumable: each stage drops a stamp file, so a re-run skips finished work.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/toolchain/versions.env"

TARGET="${TARGET:-x86_64-elf}"
PREFIX="$ROOT/toolchain/local"
SRC="$ROOT/toolchain/src"
BUILD="$ROOT/toolchain/build"
STAMPS="$BUILD/.stamps"
JOBS="$(nproc)"
FORCE=0

while [ $# -gt 0 ]; do
    case "$1" in
        -j) JOBS="$2"; shift 2 ;;
        -j*) JOBS="${1#-j}"; shift ;;
        --clean) echo ">> removing $BUILD and $PREFIX"; rm -rf "$BUILD" "$PREFIX"; shift ;;
        --force) FORCE=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ "$FORCE" = 1 ] && rm -rf "$STAMPS"

log()   { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
info()  { printf '    %s\n' "$*"; }
die()   { printf '\n\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }
done_stage() { [ -f "$STAMPS/$1" ]; }
mark()  { mkdir -p "$STAMPS"; touch "$STAMPS/$1"; }

# --- preflight -------------------------------------------------------------
log "preflight"
missing=()
for t in curl tar make gcc g++ bison flex makeinfo; do
    command -v "$t" >/dev/null || missing+=("$t")
done
[ ${#missing[@]} -eq 0 ] || die "missing host tools: ${missing[*]}"
info "target      $TARGET"
info "prefix      $PREFIX"
info "binutils    $BINUTILS_VERSION"
info "gcc         $GCC_VERSION"
info "jobs        $JOBS"

mkdir -p "$SRC" "$BUILD" "$PREFIX"

fetch() { # fetch <url> <file>
    if [ -f "$SRC/$2" ]; then info "have $2"; return; fi
    info "downloading $2"
    curl -fL --retry 3 --progress-bar -o "$SRC/$2.part" "$1"
    mv "$SRC/$2.part" "$SRC/$2"
}

# --- binutils --------------------------------------------------------------
if done_stage binutils; then
    log "binutils $BINUTILS_VERSION (already built)"
else
    log "binutils $BINUTILS_VERSION"
    fetch "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
          "binutils-$BINUTILS_VERSION.tar.xz"
    [ -d "$SRC/binutils-$BINUTILS_VERSION" ] || \
        tar -C "$SRC" -xf "$SRC/binutils-$BINUTILS_VERSION.tar.xz"

    rm -rf "$BUILD/binutils"; mkdir -p "$BUILD/binutils"
    ( cd "$BUILD/binutils"
      "$SRC/binutils-$BINUTILS_VERSION/configure" \
          --target="$TARGET" --prefix="$PREFIX" --with-sysroot \
          --disable-nls --disable-werror --enable-gold=no
      make -j"$JOBS"
      make install )
    mark binutils
fi

# --- gcc -------------------------------------------------------------------
if done_stage gcc; then
    log "gcc $GCC_VERSION (already built)"
else
    log "gcc $GCC_VERSION"
    fetch "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
          "gcc-$GCC_VERSION.tar.xz"
    [ -d "$SRC/gcc-$GCC_VERSION" ] || tar -C "$SRC" -xf "$SRC/gcc-$GCC_VERSION.tar.xz"

    ( cd "$SRC/gcc-$GCC_VERSION" && ./contrib/download_prerequisites )

    # Kernel code must not use the SysV red zone: an interrupt pushes a frame
    # over it. Teach GCC a -mno-red-zone multilib so libgcc has a safe variant.
    #
    # Both steps below are idempotent, and the check is deliberately scoped to
    # the x86_64-*-elf* block: upstream config.gcc already names
    # i386/t-x86_64-elf from the *fuchsia* target, so a bare substring grep
    # reports success on an unpatched tree.
    gccsrc="$SRC/gcc-$GCC_VERSION"
    cfg="$gccsrc/gcc/config.gcc"

    cat > "$gccsrc/gcc/config/i386/t-x86_64-elf" <<'MULTILIB'
MULTILIB_OPTIONS += mno-red-zone
MULTILIB_DIRNAMES += no-red-zone
MULTILIB

    # Is t-x86_64-elf already named inside the x86_64-*-elf*) case arm?
    elf_block_patched() {
        awk '
            /^x86_64-\*-elf\*\)$/          { inblock = 1; next }
            inblock && /t-x86_64-elf/        { found = 1 }
            inblock && /^[[:space:]]*;;$/    { inblock = 0 }
            END                              { exit !found }
        ' "$cfg"
    }

    if elf_block_patched; then
        info "config.gcc already carries the red-zone multilib"
    else
        info "patching config.gcc: -mno-red-zone multilib for x86_64-*-elf*"
        awk '
            { print }
            /^x86_64-\*-elf\*\)$/ { print "\ttmake_file=\"${tmake_file} i386/t-x86_64-elf\"" }
        ' "$cfg" > "$cfg.new"
        mv "$cfg.new" "$cfg"
        elf_block_patched || die "could not patch config.gcc -- the x86_64-*-elf*) case arm was not found"
    fi

    export PATH="$PREFIX/bin:$PATH"
    command -v "$TARGET-as" >/dev/null || die "$TARGET-as not found; binutils stage did not install"

    rm -rf "$BUILD/gcc"; mkdir -p "$BUILD/gcc"
    ( cd "$BUILD/gcc"
      "$SRC/gcc-$GCC_VERSION/configure" \
          --target="$TARGET" --prefix="$PREFIX" \
          --disable-nls --disable-shared --disable-bootstrap \
          --without-headers --enable-languages=c,c++
      make -j"$JOBS" all-gcc
      make -j"$JOBS" all-target-libgcc
      make install-gcc
      make install-target-libgcc )
    mark gcc
fi

# --- verify ----------------------------------------------------------------
log "verify"
export PATH="$PREFIX/bin:$PATH"
"$TARGET-gcc" --version | head -1
"$TARGET-ld"  --version | head -1
info "libgcc variants:"
"$TARGET-gcc" -print-multi-lib | sed 's/^/      /'
# Ask the built compiler what it would actually link. This is the only check
# that cannot be fooled by a patch that appeared to apply but did not.
if "$TARGET-gcc" -mno-red-zone -print-libgcc-file-name | grep -q no-red-zone; then
    info "red-zone-free libgcc: present"
else
    die "red-zone-free libgcc missing -- the config.gcc patch did not take"
fi

log "done"
echo "    source toolchain/env.sh   # bash"
echo "    fish_add_path $PREFIX/bin # fish"
