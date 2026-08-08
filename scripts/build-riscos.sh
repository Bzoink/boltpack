#!/bin/bash
# BoltPack RISC OS Build and Package
# 
# Builds RISC OS binary and creates distribution package in one command.
# 
# Prerequisites: GCCSDK must be installed on host (see RISC-OS-BUILD-SETUP.md)
# Usage: bash dev/build-riscos.sh

set -e

GCCSDK_PATH="${GCCSDK_PATH:-/opt/gccsdk}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=========================================="
echo "BoltPack RISC OS Build + Package"
echo "=========================================="

# Check for GCCSDK
if [ ! -d "$GCCSDK_PATH" ]; then
    echo ""
    echo "❌ ERROR: GCCSDK not found at: $GCCSDK_PATH"
    echo ""
    echo "SETUP INSTRUCTIONS:"
    echo "==================="
    echo "1. Download GCCSDK from:"
    echo "   https://www.gccsdk.riscos.info/downloads/"
    echo ""
    echo "2. Extract to /opt/gccsdk or custom path"
    echo ""
    echo "3. Verify installation:"
    echo "   /opt/gccsdk/cross/bin/arm-unknown-riscos-gcc --version"
    echo ""
    echo "4. Then run this script again (or with custom path):"
    echo "   GCCSDK_PATH=/path/to/gccsdk bash dev/build-riscos.sh"
    echo ""
    exit 1
fi

if [ ! -f "$GCCSDK_PATH/cross/bin/arm-unknown-riscos-gcc" ]; then
    echo ""
    echo "❌ ERROR: GCCSDK compiler not found at:"
    echo "   $GCCSDK_PATH/cross/bin/arm-unknown-riscos-gcc"
    echo ""
    echo "Check your GCCSDK installation."
    exit 1
fi

# Verify GCCSDK works
echo ""
echo "✓ GCCSDK found:"
"$GCCSDK_PATH/cross/bin/arm-unknown-riscos-gcc" --version 2>&1 | head -1

# Build RISC OS binary
echo ""
echo "=========================================="
echo "Building RISC OS binary..."
echo "=========================================="
cd "$PROJECT_ROOT"

# Use GCCSDK from host directly (no Docker for now)
export PATH="$GCCSDK_PATH/cross/bin:$PATH"

make riscos

if [ ! -f "build/riscos/boltpack,ff8" ] && [ ! -f "build/riscos/boltpack" ]; then
    echo ""
    echo "❌ Build failed: Binary not found in build/riscos/"
    echo ""
    exit 1
fi

# Find the binary (might be boltpack or boltpack,ff8)
if [ -f "build/riscos/boltpack,ff8" ]; then
    BINARY="build/riscos/boltpack,ff8"
elif [ -f "build/riscos/boltpack" ]; then
    BINARY="build/riscos/boltpack"
fi

echo ""
echo "✓ Binary created:"
ls -lh "$BINARY"

# Create distribution package
echo ""
echo "=========================================="
echo "Creating distribution package..."
echo "=========================================="

bash scripts/create-packages.sh riscos

if [ ! -f "dist/boltpack-"*"-riscos-"*.zip ]; then
    echo ""
    echo "❌ Packaging failed: No package found in dist/"
    echo ""
    exit 1
fi

# Show final result
echo ""
echo "=========================================="
echo "✓ SUCCESS!"
echo "=========================================="
echo ""
echo "Binary:  $BINARY"
echo "Package: $(ls -1 dist/boltpack-*-riscos-*.zip)"
echo ""
echo "Ready to distribute!"
