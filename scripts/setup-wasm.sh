#!/usr/bin/env bash
set -euo pipefail

# Setup helper for WebAssembly build using Emscripten.
# Requires emsdk and `emcc` in PATH.

WASM_CC=${WASM_CC:-emcc}

echo "WebAssembly build helper"

if ! command -v "$WASM_CC" >/dev/null 2>&1; then
    echo "$WASM_CC not found. Please install Emscripten (https://emscripten.org/docs/getting_started/downloads.html)."
    exit 1
fi

make wasm
