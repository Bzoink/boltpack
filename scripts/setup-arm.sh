#!/usr/bin/env bash
set -euo pipefail

# Setup helper for ARM / AArch64 cross build.

ARM_CC=${ARM_CC:-arm-linux-gnueabi-gcc}
ARM64_CC=${ARM64_CC:-aarch64-linux-gnu-gcc}

echo "ARM build helper"

if command -v "$ARM_CC" >/dev/null 2>&1; then
    echo "Found $ARM_CC, building arm"
    make arm
else
    echo "$ARM_CC not found, skipping arm target."
fi

if command -v "$ARM64_CC" >/dev/null 2>&1; then
    echo "Found $ARM64_CC, building arm64"
    make arm64
else
    echo "$ARM64_CC not found, skipping arm64 target."
fi
