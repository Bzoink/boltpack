#!/usr/bin/env bash
set -euo pipefail

# Setup helper for Illumos cross-compilation environment.

ILLUMOS_CC=${ILLUMOS_CC:-x86_64-unknown-illumos-gcc}

if ! command -v "$ILLUMOS_CC" >/dev/null 2>&1; then
    echo "$ILLUMOS_CC not found. Please install illumos cross toolchain."
    exit 1
fi

make illumos
