#!/usr/bin/env bash
#
# Put Sirius on a USB stick, to boot a real machine from it (M9).
#
#   make usb                              build build/sirius-usb.img
#   ./tools/write-usb.sh /dev/sdX         ...and write it to the stick at /dev/sdX
#
# EVERYTHING ON THE STICK IS ERASED. The script refuses anything that does
# not look like a removable USB disk, anything mounted, and the disk the
# running system lives on, and asks for the device name to be typed back
# before it writes. Writing needs root: it runs dd through sudo.
#
# The image boots, UEFI or BIOS, into the "live" entry: the root filesystem
# is loaded into memory by the bootloader, so nothing on the machine's own
# disks is read or written. Choose "live, with the kernel log on screen"
# from the boot menu (press a key while it counts down) to see what the
# kernel is doing.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-$ROOT/build/sirius-usb.img}"

die() { printf '\033[1;31merror: %s\033[0m\n' "$*" >&2; exit 1; }
log() { printf '\033[1;34m==> %s\033[0m\n' "$*"; }

DEVICE="${1:-}"
[ -n "$DEVICE" ] || { sed -n '2,17p' "$0"; echo; echo "Removable disks here:"; lsblk -d -o NAME,SIZE,TRAN,RM,MODEL | awk 'NR == 1 || $4 == 1'; exit 2; }
[ -f "$IMAGE" ] || die "no image at $IMAGE: run 'make usb' first"
[ -b "$DEVICE" ] || die "$DEVICE is not a block device"

name="$(basename "$(readlink -f "$DEVICE")")"
[ -e "/sys/block/$name" ] || die "$DEVICE is a partition, not a whole disk: name the disk (/dev/sdX, not /dev/sdX1)"
transport="$(lsblk -d -n -o TRAN "$DEVICE" | tr -d ' ')"
removable="$(cat "/sys/block/$name/removable")"
[ "$transport" = usb ] || [ "$removable" = 1 ] || die "$DEVICE is neither on USB nor removable (transport '$transport'): refusing"
if lsblk -n -o MOUNTPOINTS "$DEVICE" | grep -q '[^[:space:]]'; then
    die "something on $DEVICE is mounted: unmount it first"
fi
root_disk="$(lsblk -n -o PKNAME "$(findmnt -n -o SOURCE /)" 2>/dev/null || true)"
[ "$root_disk" != "$name" ] || die "$DEVICE holds the running system's root filesystem"

image_bytes="$(stat -c %s "$IMAGE")"
device_bytes="$(lsblk -d -n -b -o SIZE "$DEVICE" | tr -d ' ')"
[ "$device_bytes" -ge "$image_bytes" ] || die "$DEVICE is too small: $((image_bytes >> 20)) MiB needed"

echo
lsblk -o NAME,SIZE,TRAN,MODEL,LABEL "$DEVICE"
echo
printf 'Everything on %s (%s, %s GiB) will be erased.\n' "$DEVICE" "$(lsblk -d -n -o MODEL "$DEVICE" | sed 's/ *$//')" \
    "$((device_bytes >> 30))"
read -r -p "Type the device name ($name) to go on: " answer
[ "$answer" = "$name" ] || die "not confirmed: nothing written"

log "writing $(basename "$IMAGE") to $DEVICE"
sudo dd if="$IMAGE" of="$DEVICE" bs=4M conv=fsync status=progress
sync
log "done: boot the machine from the stick (on most laptops, F12 or Esc at power on picks the boot device)"
