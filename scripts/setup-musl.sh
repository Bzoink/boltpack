#!/usr/bin/env bash
set -euo pipefail

# Setup helper for musl cross/host build environment.

MUSL_CC=${MUSL_CC:-musl-gcc}

if ! command -v "$MUSL_CC" >/dev/null 2>&1; then
    echo "$MUSL_CC not found. Please install musl-tools or equivalent."
    exit 1
fi

make musl
