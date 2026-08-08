#!/usr/bin/env bash
set -euo pipefail

# Setup helper for HP-UX cross-compilation environment.

HPUX_CC=${HPUX_CC:-hppa-hp-hpux-gcc}

echo "HP-UX build helper"

if ! command -v "$HPUX_CC" >/dev/null 2>&1; then
    echo "$HPUX_CC not found. Please install HP-UX cross toolchain."
    exit 1
fi

make hpux
