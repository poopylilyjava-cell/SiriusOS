#!/usr/bin/env bash
#
# Boot the Sirius disk image in QEMU.
#
#   ./tools/run-qemu.sh              UEFI, graphical, serial on stdio
#   ./tools/run-qemu.sh --bios       legacy BIOS instead of UEFI
#   ./tools/run-qemu.sh --debug      halt at reset, wait for gdb on :1234
#   ./tools/run-qemu.sh --headless   no window (CI); serial only
#   ./tools/run-qemu.sh --kvm        hardware virtualisation, if the host has it
#   ./tools/run-qemu.sh --monitor S  QEMU's monitor on the unix socket S
#   ./tools/run-qemu.sh --nvme       the disk on NVMe rather than SATA, as on a laptop
#   ./tools/run-qemu.sh --log out.txt
#   QEMU_EXTRA="-machine pit=off" ./tools/run-qemu.sh   more QEMU arguments, as they are
#
# In --debug mode, attach with:  ./tools/gdb.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-$ROOT/build/sirius.img}"
MEM="${MEM:-2G}"
SMP="${SMP:-4}"

# shellcheck source=tools/ovmf.sh
. "$ROOT/tools/ovmf.sh"

USE_UEFI=1; DEBUG=0; HEADLESS=0; LOGFILE=""; KVM=0; MONITOR=""; NVME=0
while [ $# -gt 0 ]; do
    case "$1" in
        --bios)     USE_UEFI=0; shift ;;
        --debug)    DEBUG=1; shift ;;
        --headless) HEADLESS=1; shift ;;
        --log)      LOGFILE="$2"; shift 2 ;;
        --kvm)      KVM=1; shift ;;
        --monitor)  MONITOR="$2"; shift 2 ;;
        --nvme)     NVME=1; shift ;;
        -h|--help)  sed -n '2,19p' "$0"; exit 0 ;;
        *) break ;;
    esac
done

[ -f "$IMAGE" ] || { echo "error: no image at $IMAGE -- run tools/mkimage.sh" >&2; exit 1; }

# Commas are QEMU's own argument syntax here, not element separators.
# shellcheck disable=SC2054
args=(
    -M q35
    -m "$MEM"
    -smp "$SMP"
    -cpu qemu64,+rdtscp,+invtsc
    -drive "file=$IMAGE,format=raw,if=none,id=disk0"
    -device virtio-net-pci
    -rtc base=localtime
    # Fault triage: log guest resets and triple faults instead of silently looping.
    -no-reboot -no-shutdown
    -d guest_errors,int -D "$ROOT/build/qemu.log"
)

if [ "$NVME" = 1 ]; then
    # NVME_OPTIONS: more of the device's properties, e.g. logical_block_size=4096
    args+=( -device "nvme,drive=disk0,serial=sirius0${NVME_OPTIONS:+,$NVME_OPTIONS}" )
else
    args+=( -device ahci,id=ahci -device ide-hd,drive=disk0,bus=ahci.0 )
fi

if [ "$USE_UEFI" = 1 ]; then
    find_ovmf || exit 1
    # NVRAM must be writable and per-run, so boot entries don't leak between runs.
    VARS="$ROOT/build/OVMF_VARS.fd"
    mkdir -p "$ROOT/build"
    [ -f "$VARS" ] || cp "$OVMF_VARS" "$VARS"
    args+=( -drive "if=pflash,unit=0,format=raw,readonly=on,file=$OVMF_CODE"
            -drive "if=pflash,unit=1,format=raw,file=$VARS" )
fi

# Emulation (TCG) is the default: it is what CI has, and it interleaves the
# CPUs deterministically enough to debug. It is also far too slow to judge
# the desktop's frame rate by; that needs --kvm.
if [ "$KVM" = 1 ]; then
    if [ -w /dev/kvm ]; then
        args+=( -accel kvm )
    else
        echo ">> --kvm: /dev/kvm is not usable here; falling back to emulation" >&2
    fi
fi

[ -n "$MONITOR" ] && args+=( -monitor "unix:$MONITOR,server,nowait" )

if [ "$HEADLESS" = 1 ]; then
    args+=( -display none )
else
    # shellcheck disable=SC2054
    args+=( -display gtk,show-cursor=on )
fi

if [ -n "$LOGFILE" ]; then
    args+=( -serial "file:$LOGFILE" )
else
    args+=( -serial stdio )
fi

[ "$DEBUG" = 1 ] && args+=( -s -S ) && echo ">> gdb stub on :1234, cpu halted -- run tools/gdb.sh"

mkdir -p "$ROOT/build"
# shellcheck disable=SC2086  # QEMU_EXTRA is words, split on purpose
exec qemu-system-x86_64 "${args[@]}" ${QEMU_EXTRA:-}
