#!/usr/bin/env bash
set -euo pipefail

# Setup helper for OS/2 cross-compilation environment.

OS2_CC=${OS2_CC:-os2-gcc}

if ! command -v "$OS2_CC" >/dev/null 2>&1; then
    echo "$OS2_CC not found. Please install OS/2 cross toolchain (OpenWatcom or os2-gcc)."
    exit 1
fi

make os2
