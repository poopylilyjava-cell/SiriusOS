#!/usr/bin/env bash
#
# Check the root filesystem a boot has written to, with tools that are not
# Sirius's own: fsck.fat must find it clean, and mtools must read back the
# file /bin/init wrote during the M3 exit test.
#
#   ./tools/check-disk.sh [image]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${1:-$ROOT/build/sirius.img}"
EXPECTED="This file was written by Sirius OS from user space."

fail() { printf 'disk check FAILED: %s\n' "$*" >&2; exit 1; }
for t in parted mtype fsck.fat; do command -v "$t" >/dev/null || fail "missing host tool: $t"; done

# The root is the partition named SIRIUS-ROOT.
read -r START SECTORS <<<"$(parted -s -m "$IMAGE" unit s print |
    awk -F: '$6 == "SIRIUS-ROOT" || $7 == "SIRIUS-ROOT" { sub("s","",$2); sub("s","",$4); print $2, $4 }')"
[ -n "${START:-}" ] || fail "no SIRIUS-ROOT partition in $IMAGE"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
dd if="$IMAGE" of="$WORK/root.img" bs=512 skip="$START" count="$SECTORS" status=none

fsck.fat -n "$WORK/root.img" > "$WORK/fsck.log" 2>&1 || { cat "$WORK/fsck.log"; fail "fsck.fat reports errors"; }
CONTENTS="$(mtype -i "$WORK/root.img" "::/Written by Sirius.txt" 2>/dev/null)" || fail "the file init wrote is missing"
[ "$CONTENTS" = "$EXPECTED" ] || fail "the file init wrote reads back as: $CONTENTS"

printf 'disk check ok: fsck.fat clean, "/Written by Sirius.txt" intact\n'
