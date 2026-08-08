#!/usr/bin/env bash
set -euo pipefail

# Setup helper for Sinclair QL / X68000 cross-compilation environment.

QL_CC=${QL_CC:-m68k-x680x-human-gcc}

if ! command -v "$QL_CC" >/dev/null 2>&1; then
    echo "$QL_CC not found. Please install the m68k-x680x-human cross toolchain."
    exit 1
fi

make ql
