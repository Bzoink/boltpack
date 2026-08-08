#!/usr/bin/env bash
set -euo pipefail

# Setup helper for RISC-V cross-compilation environment.

RISCV_CC=${RISCV_CC:-riscv64-unknown-linux-gnu-gcc}

if ! command -v "$RISCV_CC" >/dev/null 2>&1; then
    echo "$RISCV_CC not found. Please install RISC-V cross toolchain."
    exit 1
fi

make riscv
