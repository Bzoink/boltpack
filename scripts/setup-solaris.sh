#!/usr/bin/env bash
set -euo pipefail

# Setup helper for Solaris/Illumos cross-compilation environment.

SOLARIS_CC=${SOLARIS_CC:-x86_64-pc-solaris2.11-gcc}

echo "Solaris/Illumos build helper"

if ! command -v "$SOLARIS_CC" >/dev/null 2>&1; then
    echo "$SOLARIS_CC not found. Please install Solaris cross toolchain."
    exit 1
fi

make solaris
