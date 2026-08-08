#!/usr/bin/env bash
set -euo pipefail

# Setup helper for E2K (Elbrus) cross-compilation environment.

E2K_CC=${E2K_CC:-e2k-unknown-linux-gcc}

if ! command -v "$E2K_CC" >/dev/null 2>&1; then
    echo "$E2K_CC not found. Please install E2K/Elbrus cross toolchain."
    exit 1
fi

make e2k
