#!/usr/bin/env bash
set -euo pipefail

# Setup helper for Sinclair QL cross-compilation.

if [ -z "${QL_CC:-}" ]; then
    if command -v qcc >/dev/null 2>&1; then
        QL_CC=qcc
    elif command -v m68k-qdos-gcc >/dev/null 2>&1; then
        QL_CC=m68k-qdos-gcc
    else
        echo "No Sinclair QL compiler found (tried qcc and m68k-qdos-gcc)." >&2
        exit 1
    fi
fi

if ! command -v "$QL_CC" >/dev/null 2>&1; then
    echo "$QL_CC not found. Please install qcc or m68k-qdos-gcc." >&2
    exit 1
fi

make ql QL_CC="$QL_CC"
