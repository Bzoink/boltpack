#!/usr/bin/env bash
set -euo pipefail

# Setup helper for AIX cross-compilation environment.

AIX_CC=${AIX_CC:-powerpc-ibm-aix-gcc}

echo "AIX build helper"

if ! command -v "$AIX_CC" >/dev/null 2>&1; then
    echo "$AIX_CC not found. Please install AIX cross toolchain."
    exit 1
fi

make aix
