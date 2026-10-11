#!/usr/bin/env bash
#
# Build a hybrid GPT disk image: UEFI (ESP + BOOTX64.EFI) and legacy BIOS,
# plus the root filesystem. Runs entirely unprivileged -- mtools writes into
# each FAT partition by byte offset, so no loop device and no sudo.
#
#   partition 1  BIOSBOOT     limine's BIOS stage 2
#   partition 2  ESP          FAT32: limine, the kernel, boot-module programs,
#                             and a copy of the root filesystem (rootfs.img)
#   partition 3  SIRIUS-ROOT  FAT32: the root filesystem, mounted at /
#
# The root filesystem is built once, as a file, and goes both into its
# partition and onto the ESP: booted from a USB stick, where there is no
# driver for the disk the system came from, the bootloader loads the copy
# into memory and the kernel mounts it from there (limine.conf's "live"
# entries).
#
# Usage: ./tools/mkimage.sh [kernel.elf] [out.img]
# The bring-up programs are taken from $USER_DIR (default build/user/*.elf)
# and installed both in /boot on the ESP, where limine.conf's module_path
# finds them, and in /bin on the root. The mlibc userland is taken from
# $USERLAND_DIR (default build/userland/bin/*.elf) and installed, stripped,
# in /bin; it is simply left out if it has not been built. rootfs/ is copied
# onto the root as-is. If the wayland port has been added to the sysroot
# (./tools/ports.sh add wayland), its test suite is installed too, in
# /usr/libexec/wayland-tests, for /bin/wltest to run. So are the programs
# ports put in the sysroot's /usr/bin (the weston clients), and the data in
# /usr/share that they read at run time.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL="${1:-$ROOT/build/kernel/sirius.elf}"
IMAGE="${2:-$ROOT/build/sirius.img}"
ROOT_MB="${ROOT_MB:-96}"
USER_DIR="${USER_DIR:-$ROOT/build/user}"
USERLAND_DIR="${USERLAND_DIR:-$ROOT/build/userland/bin}"
STRIP="$ROOT/toolchain/local/bin/x86_64-sirius-strip"

# GPT layout. The 2MiB BIOS boot partition holds limine's stage 2: installing
# the legacy BIOS stage to a GPT disk fails without it, because there is no
# post-MBR gap on GPT to embed into.
BIOSBOOT_START_MB=1
BIOSBOOT_END_MB=3
ESP_START_MB=$BIOSBOOT_END_MB
# The ESP holds a copy of the root filesystem, and the rest beside it.
ESP_END_MB=$((ESP_START_MB + ROOT_MB + 40))
ROOT_END_MB=$((ESP_END_MB + ROOT_MB))
SIZE_MB=$((ROOT_END_MB + 1))     # and the backup GPT after it

log() { printf '\033[1;34m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }

for t in parted mformat mmd mcopy; do
    command -v "$t" >/dev/null || die "missing host tool: $t (pacman -S parted mtools)"
done
[ -f "$KERNEL" ] || die "kernel not found: $KERNEL  (build it first)"

LIMINE_DIR="$ROOT/third_party/limine"
[ -f "$LIMINE_DIR/BOOTX64.EFI" ] || die "limine submodule missing: git submodule update --init"

# The binary branch ships the host installer as source; build it once.
if [ ! -x "$LIMINE_DIR/limine" ]; then
    log "building the limine host tool"
    make -C "$LIMINE_DIR" >/dev/null
fi

mkdir -p "$(dirname "$IMAGE")"

log "creating ${SIZE_MB}MiB GPT image -> $IMAGE"
rm -f "$IMAGE"
truncate -s "${SIZE_MB}M" "$IMAGE"
parted -s "$IMAGE" mklabel gpt
parted -s "$IMAGE" mkpart BIOSBOOT "${BIOSBOOT_START_MB}MiB" "${BIOSBOOT_END_MB}MiB"
parted -s "$IMAGE" set 1 bios_grub on
parted -s "$IMAGE" mkpart ESP fat32 "${ESP_START_MB}MiB" "${ESP_END_MB}MiB"
parted -s "$IMAGE" set 2 esp on
parted -s "$IMAGE" mkpart SIRIUS-ROOT fat32 "${ESP_END_MB}MiB" "${ROOT_END_MB}MiB"

# Each partition's exact extent, in 512-byte sectors, as parted laid it out.
# mformat must be told the size: given only an offset, it would format to the
# end of the image, straight across the next partition.
extent() { parted -s -m "$IMAGE" unit s print | awk -F: -v n="$1" '$1 == n { sub("s","",$2); sub("s","",$4); print $2, $4 }'; }
read -r ESP_START ESP_SECTORS <<<"$(extent 2)"
read -r ROOT_START ROOT_SECTORS <<<"$(extent 3)"
ESP="$IMAGE@@$((ESP_START * 512))"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
ROOTFS="$WORK/rootfs.img"
truncate -s "$((ROOT_SECTORS * 512))" "$ROOTFS"

log "formatting ESP ($((ESP_SECTORS / 2048)) MiB) and root ($((ROOT_SECTORS / 2048)) MiB)"
mformat -i "$ESP" -F -T "$ESP_SECTORS" -v SIRIUS ::
mformat -i "$ROOTFS" -F -T "$ROOT_SECTORS" -v SIRIUSROOT ::     # a file for now: copied into place at the end

log "populating ESP"
mmd   -i "$ESP" ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine
mcopy -i "$ESP" "$KERNEL"                      ::/boot/sirius.elf
for program in "$USER_DIR"/*.elf; do
    [ -f "$program" ] || continue
    mcopy -i "$ESP" "$program" "::/boot/$(basename "$program")"
done
# DEFAULT_ENTRY selects a boot menu entry by 1-based index, so the fault-test
# entries can be booted unattended (CI, scripted runs) without editing the file.
# MENU_TIMEOUT shows the menu for that many seconds (the file says 0: none),
# for a USB stick on a machine where the entry may need choosing.
CONF="$ROOT/boot/limine.conf"
if [ -n "${DEFAULT_ENTRY:-}" ] || [ -n "${MENU_TIMEOUT:-}" ]; then
    CONF="$WORK/limine.conf"
    [ -z "${DEFAULT_ENTRY:-}" ] || printf 'default_entry: %s\n' "$DEFAULT_ENTRY" > "$CONF"
    sed "s/^timeout: .*/timeout: ${MENU_TIMEOUT:-0}/" "$ROOT/boot/limine.conf" >> "$CONF"
    [ -z "${DEFAULT_ENTRY:-}" ] || log "default boot entry: $DEFAULT_ENTRY"
fi
mcopy -i "$ESP" "$CONF"                        ::/boot/limine/limine.conf
mcopy -i "$ESP" "$LIMINE_DIR/limine-bios.sys"  ::/boot/limine/limine-bios.sys
mcopy -i "$ESP" "$LIMINE_DIR/BOOTX64.EFI"      ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$ESP" "$LIMINE_DIR/BOOTIA32.EFI"     ::/EFI/BOOT/BOOTIA32.EFI

log "populating the root filesystem"
mmd -i "$ROOTFS" ::/bin ::/dev ::/tmp
# rootfs/ minus its README, which documents the tree rather than belonging in it.
for entry in "$ROOT/rootfs"/*; do
    [ "$(basename "$entry")" = README ] && continue
    mcopy -s -i "$ROOTFS" "$entry" ::/
done
for program in "$USER_DIR"/*.elf; do
    [ -f "$program" ] || continue
    mcopy -i "$ROOTFS" "$program" "::/bin/$(basename "$program" .elf)"
done
# Debug information stays in the build tree: a static C++ program is several
# times larger with it, and the kernel reads an executable whole.
userland=0
for program in "$USERLAND_DIR"/*.elf; do
    [ -f "$program" ] || continue
    stripped="$(mktemp)"
    "$STRIP" -o "$stripped" "$program"
    mcopy -i "$ROOTFS" "$stripped" "::/bin/$(basename "$program" .elf)"
    rm -f "$stripped"
    userland=$((userland + 1))
done
# Directories under /usr, each made once: mmd refuses one that exists.
declare -A made=()
usr_dir() {
    local path="" part
    IFS=/ read -ra parts <<<"${1#/}"
    for part in "${parts[@]}"; do
        path="$path/$part"
        [ -n "${made[$path]:-}" ] || { mmd -i "$ROOTFS" "::$path"; made[$path]=1; }
    done
}

WAYLAND_TESTS="$ROOT/sysroot/usr/libexec/wayland-tests"
if [ -d "$WAYLAND_TESTS" ]; then
    usr_dir /usr/libexec/wayland-tests
    mcopy -i "$ROOTFS" "$WAYLAND_TESTS"/* ::/usr/libexec/wayland-tests/
    log "installed libwayland's test suite ($(find "$WAYLAND_TESTS" -type f | wc -l) programs)"
fi

# Programs from the ports: the executables in the sysroot's /usr/bin (not the
# scripts some libraries put there for building against them), stripped
# like the rest.
ported=0
for program in "$ROOT/sysroot/usr/bin"/*; do
    [ -f "$program" ] && [ "$(head -c 4 "$program" | od -An -c | tr -d ' ')" = '177ELF' ] || continue
    usr_dir /usr/bin
    stripped="$(mktemp)"
    "$STRIP" -o "$stripped" "$program"
    mcopy -i "$ROOTFS" "$stripped" "::/usr/bin/$(basename "$program")"
    rm -f "$stripped"
    ported=$((ported + 1))
done
# And what they read at run time.
for data in weston; do
    [ -d "$ROOT/sysroot/usr/share/$data" ] || continue
    usr_dir "/usr/share/$data"
    mcopy -s -i "$ROOTFS" "$ROOT/sysroot/usr/share/$data"/* "::/usr/share/$data/"
done
[ "$ported" -eq 0 ] || log "installed $ported program(s) from the ports in /usr/bin"

if [ "$userland" -gt 0 ]; then
    log "installed $userland mlibc program(s) from $USERLAND_DIR"
else
    log "no mlibc userland in $USERLAND_DIR (make userland) -- the image has the bring-up programs only"
fi

log "copying the root filesystem into its partition, and onto the ESP"
dd if="$ROOTFS" of="$IMAGE" bs=512 seek="$ROOT_START" conv=notrunc status=none
mcopy -i "$ESP" "$ROOTFS" ::/boot/rootfs.img

log "installing the BIOS stage"
"$LIMINE_DIR/limine" bios-install "$IMAGE" >/dev/null

log "ok: $IMAGE ($(du -h "$IMAGE" | cut -f1))"
