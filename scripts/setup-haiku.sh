#!/usr/bin/env bash
set -euo pipefail

# Setup helper for Haiku cross-compilation environment.

HAIKU_CC=${HAIKU_CC:-x86_64-unknown-haiku-gcc}

echo "Haiku build helper"

if ! command -v "$HAIKU_CC" >/dev/null 2>&1; then
    echo "$HAIKU_CC not found. Please install Haiku cross toolchain (buildtools)."
    exit 1
fi

make haiku
