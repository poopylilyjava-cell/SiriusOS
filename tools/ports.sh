#!/usr/bin/env bash
#
# Run jinx on the ports tree (ports/).
#
#   ./tools/ports.sh build zlib          build a port, and whatever it needs
#   ./tools/ports.sh add zlib            ...and unpack it into /sysroot, where
#                                        x86_64-sirius-gcc finds it
#   ./tools/ports.sh <jinx command...>   anything else goes to jinx as it is:
#                                        rebuild, regen, dry-run, run-in, ...
#
# jinx builds every package inside a Debian container, from recipes in
# ports/. It cannot see the rest of the repository from in there, so this
# wrapper first stages what the recipes need from outside: the mlibc tree
# with the Sirius sysdeps and the kernel's ABI headers in it. The build
# directory is toolchain/build/ports, where `make clean` leaves it alone --
# it holds a second toolchain, built in the container.
#
# The first run downloads the container image and builds binutils, GCC,
# mlibc and libstdc++ in it: allow twenty minutes and a few GiB.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT/toolchain/versions.env"

PORTS="$ROOT/ports"
STAGING="$PORTS/.staging"
BUILD="${PORTS_BUILD:-$ROOT/toolchain/build/ports}"
SYSROOT="$ROOT/sysroot"
JINX_SRC="$ROOT/toolchain/src/jinx"
JINX="$JINX_SRC/jinx"

log() { printf '\033[1;34m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }

[ $# -gt 0 ] || { sed -n '2,19p' "$0"; exit 2; }

for t in git rsync meson wget zstd perl unshare; do
    command -v "$t" >/dev/null || die "missing host tool: $t"
done

# --- jinx itself, at the pinned commit ---------------------------------------
if [ ! -x "$JINX" ] || [ "$(git -C "$JINX_SRC" rev-parse HEAD 2>/dev/null)" != "$JINX_COMMIT" ]; then
    log "fetching jinx $JINX_COMMIT"
    rm -rf "$JINX_SRC"
    git clone -q https://github.com/Mintsuki/Jinx.git "$JINX_SRC"
    git -C "$JINX_SRC" -c advice.detachedHead=false checkout -q "$JINX_COMMIT"
fi

# --- stage the sources that live outside ports/ -------------------------------
# The mlibc recipes build this tree in place, so a change to the sysdeps
# reaches the next `build mlibc` without bumping anything.
"$ROOT/toolchain/build-sirius.sh" --sources >/dev/null
mkdir -p "$STAGING"
# The subprojects meson downloads are kept out of the sync, so that the
# staged copy keeps its own once it has them; packagefiles/ is mlibc's.
rsync -a --delete --exclude .git \
    --include 'subprojects/packagefiles/***' --exclude 'subprojects/*/' \
    "$ROOT/toolchain/src/mlibc-$MLIBC_VERSION/" "$STAGING/mlibc/"
# Meson would fetch them during configure, but the container has no network
# then.
if [ ! -f "$STAGING/mlibc-subprojects.fetched" ]; then
    log "fetching mlibc's subprojects"
    for subproject in frigg libsmarter freestnd-c-hdrs freestnd-cxx-hdrs; do
        rm -rf "$STAGING/mlibc/subprojects/$subproject"
    done
    meson subprojects download --sourcedir "$STAGING/mlibc" \
        frigg libsmarter freestnd-c-hdrs freestnd-cxx-hdrs >/dev/null
    for subproject in frigg libsmarter freestnd-c-hdrs freestnd-cxx-hdrs; do
        [ -f "$STAGING/mlibc/subprojects/$subproject/meson.build" ] ||
            die "mlibc subproject $subproject did not download"
    done
    touch "$STAGING/mlibc-subprojects.fetched"
fi

# --- the build directory --------------------------------------------------------
mkdir -p "$BUILD"
cd "$BUILD"
[ -f .jinx-parameters ] || "$JINX" init "$PORTS" ARCH=x86_64

# --- add: build, then unpack into the sysroot -----------------------------------
# Only the named packages are unpacked. Their dependencies on mlibc and
# libstdc++ are already satisfied there, by toolchain/build-sirius.sh; jinx's
# own `install` would lay the container-built copies over them.
if [ "$1" = add ]; then
    shift
    [ $# -gt 0 ] || die "add: name at least one port"
    "$JINX" build "$@"
    for name in "$@"; do
        package="$(find pkgs -maxdepth 1 -name "$name-[0-9]*.xbps" | sort | tail -1)"
        [ -n "$package" ] || die "add: no package was built for $name"
        log "unpacking $(basename "$package") into $SYSROOT"
        # An XBPS package is a zstd-compressed tar; the two plists at its
        # root are the package manager's metadata, not files to install.
        zstdcat "$package" | tar -x -C "$SYSROOT" --exclude=./props.plist --exclude=./files.plist
    done
    exit 0
fi

exec "$JINX" "$@"
