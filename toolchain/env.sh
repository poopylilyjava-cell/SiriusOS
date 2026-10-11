#!/usr/bin/env bash
# Source this to put the Sirius cross toolchain on PATH:
#   bash:  source toolchain/env.sh
#   fish:  set -gx PATH (pwd)/toolchain/local/bin $PATH
_SIRIUS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export SIRIUS_ROOT="$_SIRIUS_ROOT"
export SIRIUS_TOOLCHAIN="$_SIRIUS_ROOT/toolchain/local"
export PATH="$SIRIUS_TOOLCHAIN/bin:$PATH"
echo "sirius: toolchain on PATH ($SIRIUS_TOOLCHAIN)"
