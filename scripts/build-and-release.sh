#!/bin/bash

# BoltPack: Build-to-Release Automation
# Usage: ./scripts/build-and-release.sh [platforms...]
# Example: ./scripts/build-and-release.sh linux arm64 amiga atari

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"
VERSION=$(cat VERSION 2>/dev/null | tr -d '[:space:]')
[ -n "$VERSION" ] || { echo "Error: VERSION file not found" >&2; exit 1; }

# Default platforms if none specified
if [[ $# -eq 0 ]]; then
    PLATFORMS=("linux" "linux-arm64" "musl" "riscv" "haiku" "wasm")
else
    PLATFORMS=("$@")
fi

print_step() {
    echo ""
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "▶ $1"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
}

print_success() {
    echo "✓ $1"
}

check_release_exists() {
    if ! ./scripts/release.sh status 2>/dev/null | grep -q "v${VERSION}"; then
        print_step "Creating draft release"
        ./scripts/release.sh create
    fi
}

print_step "BoltPack Build & Release"
echo "Platforms: ${PLATFORMS[*]}"

# Check prerequisites
if [[ -z "$GITHUB_TOKEN" ]]; then
    echo "⚠ Warning: GITHUB_TOKEN not set - packages will NOT be uploaded"
    echo "   Set it with: export GITHUB_TOKEN='ghp_xxxxxxxxxxxx'"
    SKIP_UPLOAD=true
fi

# Ensure release exists
if [[ -z "$SKIP_UPLOAD" ]]; then
    check_release_exists
fi

# Build specified platforms
print_step "Building binaries"
for platform in "${PLATFORMS[@]}"; do
    if make "$platform" 2>/dev/null; then
        print_success "Built $platform"
    else
        echo "⚠ Skipped $platform (toolchain not available)"
    fi
done

# Create distribution packages
print_step "Creating distribution packages"
if bash scripts/create-packages.sh; then
    print_success "Packages created"
else
    echo "✗ Package creation failed"
    exit 1
fi

# Generate download page
print_step "Generating download page"
if python3 scripts/generate-download-page.py; then
    print_success "Download page generated (dist/index.html)"
else
    echo "✗ Download page generation failed"
    exit 1
fi

# Upload to release (if token is set)
if [[ -z "$SKIP_UPLOAD" ]]; then
    print_step "Uploading to GitHub release"
    if ./scripts/release.sh upload; then
        print_success "Packages uploaded"
        echo ""
        ./scripts/release.sh status
        echo ""
        echo "View: https://github.com/Bzoink/boltpack/releases/tag/v${VERSION}"
    else
        echo "✗ Upload failed"
        exit 1
    fi
fi

print_step "Complete!"
echo ""
echo "Next steps:"
if [[ -z "$SKIP_UPLOAD" ]]; then
    echo "  1. Review release: https://github.com/Bzoink/boltpack/releases/tag/v${VERSION}"
    echo "  2. Add more platforms: ./scripts/build-and-release.sh amiga atari dos"
    echo "  3. When done: ./scripts/release.sh publish"
else
    echo "  1. Set GitHub token: export GITHUB_TOKEN='ghp_xxxxxxxxxxxx'"
    echo "  2. Re-run this script to upload: ./scripts/build-and-release.sh ${PLATFORMS[*]}"
fi
