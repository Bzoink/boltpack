#!/bin/bash
# Create portable macOS source kits for native compilation on target Macs.
# Usage: bash scripts/create-macos-source-kits.sh [ppc|legacy|intel|arm64|all]

set -eo pipefail

PROJROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISTDIR="$PROJROOT/dist"
TMPROOT="$(mktemp -d)"

cleanup() {
    rm -rf "$TMPROOT"
}
trap cleanup EXIT

VERSION=$(sed -n \
    -e 's/^\*\*Version:\*\*[[:space:]]*\([^[:space:]]*\).*/\1/p' \
    -e 's/.*Version:[[:space:]]*\([^[:space:]]*\).*/\1/p' \
    "$PROJROOT/RELEASE-NOTES.md" | head -1)
[ -n "$VERSION" ] || VERSION="1.0"

usage() {
    cat <<EOF
Usage: bash scripts/create-macos-source-kits.sh [ppc|legacy|intel|arm64|all]

Creates source-only tar.gz kits under dist/ for native builds on:
    ppc     PowerPC Mac OS X 10.4/10.5 (CLI only)
    legacy  Legacy Cocoa GUI kit for PowerPC 10.5 and Intel 10.5-10.8
    intel   Intel macOS (CLI + modern Cocoa GUI)
    arm64   Apple Silicon macOS (CLI + modern Cocoa GUI)
    all     Create all kits (default)
EOF
}

copy_tree() {
    local src="$1"
    local dst="$2"
    mkdir -p "$(dirname "$dst")"
    cp -R "$src" "$dst"
}

copy_file() {
    local src="$1"
    local dst="$2"
    mkdir -p "$(dirname "$dst")"
    cp "$src" "$dst"
}

write_build_notes() {
    local kitdir="$1"
    local kit_id="$2"
    local build_cmd=""
    local target_label=""
    local requirements=""
    local outputs=""
    local notes=""

    case "$kit_id" in
        macosx-ppc)
            target_label="PowerPC Mac OS X 10.4/10.5"
            build_cmd="sh scripts/macos/build-native.sh ppc"
            requirements="- Apple Developer Tools or Xcode with a native PowerPC C compiler installed.\n- One of clang, cc, or gcc available in PATH."
            outputs="- CLI: build/macosx-ppc/boltpack"
            notes="- This kit is CLI only; it does not include a GUI frontend.\n- The separate source-macosx-legacy-gui kit carries the pre-10.9 Cocoa GUI sources for PowerPC 10.5 and Intel 10.5-10.8.\n- Override the compiler with CC=... if the default tool lookup is not correct.\n- Add extra compiler flags with CFLAGS=... if your local setup needs them."
            ;;
        macosx-legacy-gui)
            target_label="PowerPC Mac OS X 10.5 and Intel Mac OS X 10.5-10.8"
            build_cmd="sh scripts/macos/build-native.sh legacy-ppc\n    sh scripts/macos/build-native.sh legacy-intel"
            requirements="- Apple Developer Tools or Xcode with a native Objective-C compiler installed.\n- Cocoa headers and frameworks from Mac OS X 10.5 or later.\n- For Intel, the default legacy build is i386 so one app can run on 10.5 through 10.8."
            outputs="- Legacy PowerPC Cocoa GUI app: build/macosx-legacy-gui-ppc/BoltPack.app\n- Legacy Intel Cocoa GUI app: build/macosx-legacy-gui-intel/BoltPack.app"
            notes="- This kit is for the separate pre-10.9 Cocoa app line.\n- GUI builds now produce Finder-launchable .app bundles.\n- Override the compiler with OBJC=... if the default tool lookup is not correct.\n- Override the default Intel arch with LEGACY_INTEL_ARCH=x86_64 only if you explicitly want a 64-bit-only Intel build.\n- Add extra Objective-C flags with LEGACY_GUI_CFLAGS=... if your local setup needs them."
            ;;
        macosx-intel)
            target_label="Intel Mac OS X / macOS"
            build_cmd="sh scripts/macos/build-native.sh intel"
            requirements="- Xcode or Apple command line developer tools installed.\n- A C compiler for the CLI build and an Objective-C compiler for the Cocoa GUI."
            outputs="- CLI: build/macosx-intel/boltpack\n- Cocoa GUI app: build/macosx-gui-intel/BoltPack.app"
            notes="- This kit carries the modern 10.9+ Cocoa GUI sources, not the pre-10.9 legacy app.\n- Use the separate source-macosx-legacy-gui kit for Intel Mac OS X 10.5-10.8 GUI builds.\n- GUI builds now produce Finder-launchable .app bundles.\n- Set BUILD_GUI=0 to skip the modern Cocoa GUI build.\n- Override the CLI compiler with CC=... and the Objective-C compiler with OBJC=... if needed.\n- Add extra flags with CFLAGS=... and GUI_CFLAGS=...."
            ;;
        macosx-arm64)
            target_label="Apple Silicon macOS 11+"
            build_cmd="sh scripts/macos/build-native.sh arm64"
            requirements="- Xcode or Apple command line developer tools installed.\n- A C compiler for the CLI build and an Objective-C compiler for the Cocoa GUI."
            outputs="- CLI: build/macosx-arm64/boltpack\n- Cocoa GUI app: build/macosx-gui-arm64/BoltPack.app"
            notes="- This kit carries the modern 10.9+ Cocoa GUI sources.\n- GUI builds now produce Finder-launchable .app bundles.\n- Set BUILD_GUI=0 to skip the Cocoa GUI build.\n- Override the CLI compiler with CC=... and the Objective-C compiler with OBJC=... if needed.\n- Add extra flags with CFLAGS=... and GUI_CFLAGS=...."
            ;;
        *)
            return 1
            ;;
    esac

    build_cmd=$(printf '%b' "$build_cmd")
    requirements=$(printf '%b' "$requirements")
    outputs=$(printf '%b' "$outputs")
    notes=$(printf '%b' "$notes")

    cat >"$kitdir/build-notes.md" <<EOF
# Build Notes

This source kit is for native BoltPack builds on $target_label.

## Requirements

$requirements

## Build

Run from the root of this extracted source kit:

    $build_cmd

## Output

$outputs

## Notes

$notes
EOF
}

stage_common() {
    local kitdir="$1"

    copy_tree "$PROJROOT/src/core" "$kitdir/src/core"
    copy_tree "$PROJROOT/lib/minilzo-2.10" "$kitdir/lib/minilzo-2.10"
    copy_tree "$PROJROOT/src/frontend/macosx" "$kitdir/src/frontend/macosx"
    copy_file "$PROJROOT/resources/pics/boltpack_icon.png" \
        "$kitdir/resources/pics/boltpack_icon.png"
    if [ -f "$PROJROOT/resources/pics/boltpack_icon.icns" ]; then
        copy_file "$PROJROOT/resources/pics/boltpack_icon.icns" \
            "$kitdir/resources/pics/boltpack_icon.icns"
    fi
    copy_file "$PROJROOT/docs/BUILD.md" "$kitdir/docs/BUILD.md"
    copy_file "$PROJROOT/docs/frontends.md" "$kitdir/docs/frontends.md"
    copy_file "$PROJROOT/docs/portability.md" "$kitdir/docs/portability.md"
    copy_file "$PROJROOT/RELEASE-NOTES.md" "$kitdir/RELEASE-NOTES.md"
}

stage_modern_gui() {
    local kitdir="$1"

    copy_tree "$PROJROOT/src/frontend/macosx_gui" "$kitdir/src/frontend/macosx_gui"
    copy_file "$PROJROOT/src/frontend/gui_common/help_text.h" \
        "$kitdir/src/frontend/gui_common/help_text.h"
}

stage_legacy_gui() {
    local kitdir="$1"

    copy_tree "$PROJROOT/src/frontend/macosx_legacy_gui" \
        "$kitdir/src/frontend/macosx_legacy_gui"
    copy_tree "$PROJROOT/src/frontend/macosx_gui" \
        "$kitdir/src/frontend/macosx_gui"
    copy_file "$PROJROOT/src/frontend/gui_common/help_text.h" \
        "$kitdir/src/frontend/gui_common/help_text.h"
}

create_kit() {
    local kit_id="$1"
    local gui_variant="$2"
    local kit_name="boltpack-$VERSION-source-$kit_id"
    local kitdir="$TMPROOT/$kit_name"
    local pkg_path="$DISTDIR/$kit_name.tar.gz"

    rm -rf "$kitdir"
    mkdir -p "$kitdir/scripts/macos"

    stage_common "$kitdir"
    if [ "$gui_variant" = "modern" ]; then
        stage_modern_gui "$kitdir"
    elif [ "$gui_variant" = "legacy" ]; then
        stage_legacy_gui "$kitdir"
    fi

    copy_file "$PROJROOT/scripts/macos/build-native.sh" \
        "$kitdir/scripts/macos/build-native.sh"
    chmod +x "$kitdir/scripts/macos/build-native.sh"
    write_build_notes "$kitdir" "$kit_id"

    cat >"$kitdir/build_date.md" <<EOF
# Package Build Date

Kit:     $kit_name
Created: $(date '+%Y-%m-%d %H:%M:%S %Z')
EOF

    mkdir -p "$DISTDIR"
    tar czf "$pkg_path" -C "$TMPROOT" "$kit_name"
    echo "Created: $pkg_path"
}

TARGET="${1:-all}"

case "$TARGET" in
    ppc)
        create_kit "macosx-ppc" "none"
        ;;
    legacy)
        create_kit "macosx-legacy-gui" "legacy"
        ;;
    intel)
        create_kit "macosx-intel" "modern"
        ;;
    arm64)
        create_kit "macosx-arm64" "modern"
        ;;
    all)
        create_kit "macosx-ppc" "none"
        create_kit "macosx-legacy-gui" "legacy"
        create_kit "macosx-intel" "modern"
        create_kit "macosx-arm64" "modern"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac