#!/usr/bin/env bash
set -euo pipefail

# Setup helper for IRIX cross-compilation environment.

IRIX_CC=${IRIX_CC:-mips-sgi-irix6.5-gcc}

echo "IRIX build helper"

if ! command -v "$IRIX_CC" >/dev/null 2>&1; then
    echo "$IRIX_CC not found. Please install IRIX cross toolchain."
    exit 1
fi

make irix
