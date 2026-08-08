#!/bin/sh
# Build BoltPack natively on macOS / Mac OS X.
# Usage: sh scripts/macos/build-native.sh [ppc|legacy-ppc|legacy-intel|intel|arm64|all|legacy-all]

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

BUNDLE_VERSION=$(sed -n \
    -e 's/^\*\*Version:\*\*[[:space:]]*\([^[:space:]]*\).*/\1/p' \
    -e 's/.*Version:[[:space:]]*\([^[:space:]]*\).*/\1/p' \
    "$ROOT/RELEASE-NOTES.md" | head -1)
[ -n "$BUNDLE_VERSION" ] || BUNDLE_VERSION="1.0"

usage() {
    cat <<EOF
Usage: sh scripts/macos/build-native.sh [ppc|legacy-ppc|legacy-intel|intel|arm64|all|legacy-all]

Targets:
    ppc           PowerPC Mac OS X CLI build
    legacy-ppc    PowerPC Mac OS X 10.5 legacy Cocoa GUI build
    legacy-intel  Intel Mac OS X 10.5-10.8 legacy Cocoa GUI build
    intel         Intel macOS CLI build, plus modern Cocoa GUI when available
    arm64         Apple Silicon macOS CLI build, plus modern Cocoa GUI when available
    all           Build the modern targets in sequence
    legacy-all    Build both legacy GUI targets in sequence

Environment:
    CC                C compiler for CLI builds
    OBJC              Objective-C compiler for Cocoa GUI builds
    CFLAGS            Extra CLI compiler flags
    GUI_CFLAGS        Extra modern Cocoa GUI compiler flags
    LEGACY_GUI_CFLAGS Extra legacy Cocoa GUI compiler flags
    LEGACY_INTEL_ARCH Intel arch for legacy GUI build (default: i386)
    BUILD_GUI         Set to 0 to skip the Cocoa GUI for intel/arm64

Notes:
    - The modern Cocoa GUI is only built when src/frontend/macosx_gui/main.m is present.
    - The legacy Cocoa GUI is only built when src/frontend/macosx_legacy_gui/main.m is present.
    - In CLI-only source kits such as source-macosx-ppc, GUI targets are unavailable.
    - GUI targets now produce BoltPack.app bundles for Finder launch.
    - legacy-intel defaults to i386 so one build can run across Mac OS X 10.5-10.8.
    - The all target assumes the local toolchain can emit the requested -arch values.
        On older Macs, use the matching native target instead.
EOF
}

find_c_compiler() {
    if [ -n "${CC:-}" ]; then
        echo "$CC"
    elif command -v clang >/dev/null 2>&1; then
        echo "clang"
    elif command -v cc >/dev/null 2>&1; then
        echo "cc"
    elif command -v gcc >/dev/null 2>&1; then
        echo "gcc"
    else
        echo "No C compiler found. Set CC or install clang/cc/gcc." >&2
        exit 1
    fi
}

find_objc_compiler() {
    if [ -n "${OBJC:-}" ]; then
        echo "$OBJC"
    else
        echo "$COMPILER"
    fi
}

prepare_app_bundle() {
    bundle_dir="$1"
    bundle_id="$2"
    min_os="$3"

    rm -rf "$bundle_dir"
    mkdir -p "$bundle_dir/Contents/MacOS" "$bundle_dir/Contents/Resources"

    cat >"$bundle_dir/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>English</string>
    <key>CFBundleExecutable</key>
    <string>BoltPack</string>
    <key>CFBundleIdentifier</key>
    <string>$bundle_id</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleIconFile</key>
    <string>boltpack_icon.icns</string>
    <key>CFBundleName</key>
    <string>BoltPack</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$BUNDLE_VERSION</string>
    <key>CFBundleVersion</key>
    <string>$BUNDLE_VERSION</string>
    <key>LSMinimumSystemVersion</key>
    <string>$min_os</string>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
</dict>
</plist>
EOF

    if [ -f "$ROOT/resources/pics/boltpack_icon.icns" ]; then
        cp "$ROOT/resources/pics/boltpack_icon.icns" \
           "$bundle_dir/Contents/Resources/boltpack_icon.icns"
    fi

    printf 'APPL????' > "$bundle_dir/Contents/PkgInfo"
}

build_cli() {
    arch_flag="$1"
    creator_cpu="$2"
    outdir="$3"
    label="$4"

    mkdir -p "$outdir"

    echo "Building $label CLI with $COMPILER..."

    "$COMPILER" $CLI_CFLAGS \
        -arch "$arch_flag" \
        -DBP_CREATOR_CPU=$creator_cpu \
        -Isrc/core -Ilib/minilzo-2.10 \
        src/core/archiver.c src/core/compress.c src/core/lzss.c \
        src/core/fluff.c src/core/bp_codepage.c \
        lib/minilzo-2.10/minilzo.c \
        src/frontend/macosx/main.c \
        -o "$outdir/boltpack"

    echo "Built: $outdir/boltpack"
}

build_gui() {
    arch_flag="$1"
    creator_cpu="$2"
    outdir="$3"
    label="$4"
    bundle_id="$5"
    min_os="$6"
    bundle_dir="$outdir/BoltPack.app"
    bundle_exec="$bundle_dir/Contents/MacOS/BoltPack"
    fast_fluff_obj="$outdir/fluff_fast.o"

    if [ "$BUILD_GUI" = "0" ]; then
        echo "Skipping $label Cocoa GUI because BUILD_GUI=0."
        return 0
    fi

    if [ ! -f src/frontend/macosx_gui/main.m ]; then
        echo "Skipping $label Cocoa GUI because this source kit does not include macosx_gui (for example, source-macosx-ppc is CLI only)."
        return 0
    fi

    mkdir -p "$outdir"
    prepare_app_bundle "$bundle_dir" "$bundle_id" "$min_os"

    echo "Building $label Cocoa GUI with $OBJC_COMPILER..."

    "$COMPILER" -Wall -Wextra -Wno-unused-function -O3 -std=c11 \
        -arch "$arch_flag" \
        -DBOLTPACK_FAST_FLUFF -fopenmp \
        -Isrc/core \
        -c src/core/fluff_fast.c \
        -o "$fast_fluff_obj"

    "$OBJC_COMPILER" $GUI_CFLAGS \
        -arch "$arch_flag" \
        -DBOLTPACK_FAST_FLUFF \
        -DBP_CREATOR_CPU=$creator_cpu \
        -Isrc/core -Ilib/minilzo-2.10 -Isrc/frontend/gui_common \
        src/core/archiver.c src/core/compress.c src/core/lzss.c \
        src/core/fluff.c src/core/bp_codepage.c \
        lib/minilzo-2.10/minilzo.c \
        "$fast_fluff_obj" \
        src/frontend/macosx_gui/main.m \
        -framework Cocoa -fopenmp \
        -o "$bundle_exec"

    echo "Built: $bundle_dir"
}

legacy_intel_cpu() {
    case "${LEGACY_INTEL_ARCH}" in
        i386)
            echo "BP_CPU_X86"
            ;;
        x86_64)
            echo "BP_CPU_X86_64"
            ;;
        *)
            echo "Unsupported LEGACY_INTEL_ARCH: ${LEGACY_INTEL_ARCH}" >&2
            exit 1
            ;;
    esac
}

build_legacy_gui() {
    arch_flag="$1"
    creator_cpu="$2"
    outdir="$3"
    label="$4"
    bundle_id="$5"
    min_os="$6"
    bundle_dir="$outdir/BoltPack.app"
    bundle_exec="$bundle_dir/Contents/MacOS/BoltPack"
    fast_fluff_obj="$outdir/fluff_fast.o"

    if [ ! -f src/frontend/macosx_legacy_gui/main.m ]; then
        echo "Cannot build $label because this source kit does not include macosx_legacy_gui." >&2
        return 1
    fi

    mkdir -p "$outdir"
    prepare_app_bundle "$bundle_dir" "$bundle_id" "$min_os"

    echo "Building $label with $OBJC_COMPILER..."

    "$COMPILER" -Wall -Wextra -Wno-unused-function -O3 -std=c11 \
        -arch "$arch_flag" \
        -DBOLTPACK_FAST_FLUFF -fopenmp \
        -Isrc/core \
        -c src/core/fluff_fast.c \
        -o "$fast_fluff_obj"

    "$OBJC_COMPILER" $LEGACY_GUI_CFLAGS \
        -arch "$arch_flag" \
        -DBOLTPACK_FAST_FLUFF \
        -DBP_CREATOR_CPU=$creator_cpu \
        -Isrc/core -Ilib/minilzo-2.10 -Isrc/frontend/gui_common \
        src/core/archiver.c src/core/compress.c src/core/lzss.c \
        src/core/fluff.c src/core/bp_codepage.c \
        lib/minilzo-2.10/minilzo.c \
        "$fast_fluff_obj" \
        src/frontend/macosx_legacy_gui/main.m \
        -framework Cocoa -framework Carbon -fopenmp \
        -o "$bundle_exec"

    echo "Built: $bundle_dir"
}

build_target() {
    case "$1" in
        ppc)
            build_cli ppc BP_CPU_PPC32 "${PPC_OUTDIR:-build/macosx-ppc}" \
                "PowerPC Mac OS X"
            ;;
        legacy-ppc)
            build_legacy_gui ppc BP_CPU_PPC32 \
                "${LEGACY_PPC_GUI_OUTDIR:-build/macosx-legacy-gui-ppc}" \
                "PowerPC Mac OS X 10.5 legacy Cocoa GUI" \
                "com.boltpack.BoltPackLegacyPPC" "10.5"
            ;;
        legacy-intel)
            build_legacy_gui "${LEGACY_INTEL_ARCH}" "$(legacy_intel_cpu)" \
                "${LEGACY_INTEL_GUI_OUTDIR:-build/macosx-legacy-gui-intel}" \
                "Intel Mac OS X 10.5-10.8 legacy Cocoa GUI" \
                "com.boltpack.BoltPackLegacyIntel" "10.5"
            ;;
        intel)
            build_cli x86_64 BP_CPU_X86_64 \
                "${INTEL_CLI_OUTDIR:-build/macosx-intel}" "Intel macOS"
            build_gui x86_64 BP_CPU_X86_64 \
                "${INTEL_GUI_OUTDIR:-build/macosx-gui-intel}" "Intel macOS" \
                "com.boltpack.BoltPackIntel" "10.9"
            ;;
        arm64)
            build_cli arm64 BP_CPU_ARM64 \
                "${ARM64_CLI_OUTDIR:-build/macosx-arm64}" "Apple Silicon macOS"
            build_gui arm64 BP_CPU_ARM64 \
                "${ARM64_GUI_OUTDIR:-build/macosx-gui-arm64}" \
                "Apple Silicon macOS" \
                "com.boltpack.BoltPackARM64" "11.0"
            ;;
        *)
            echo "Unknown target: $1" >&2
            usage >&2
            return 1
            ;;
    esac
}

COMPILER=$(find_c_compiler)
OBJC_COMPILER=$(find_objc_compiler)
CLI_CFLAGS="${CFLAGS:--ansi -pedantic -Wall -Wextra -O2}"
GUI_CFLAGS="${GUI_CFLAGS:--ObjC -fobjc-arc -Wall -Wextra -Wno-unused-function -O2}"
LEGACY_GUI_CFLAGS="${LEGACY_GUI_CFLAGS:--ObjC -Wall -Wextra -Wno-unused-function -O2 -mmacosx-version-min=10.5}"
LEGACY_INTEL_ARCH="${LEGACY_INTEL_ARCH:-i386}"
BUILD_GUI="${BUILD_GUI:-1}"
TARGET="${1:-}"

case "$TARGET" in
    ppc|legacy-ppc|legacy-intel|intel|arm64)
        build_target "$TARGET"
        ;;
    all)
        status=0
        for target in ppc intel arm64; do
            if ! build_target "$target"; then
                status=1
                echo "Failed target: $target" >&2
            fi
        done
        exit "$status"
        ;;
    legacy-all)
        status=0
        for target in legacy-ppc legacy-intel; do
            if ! build_target "$target"; then
                status=1
                echo "Failed target: $target" >&2
            fi
        done
        exit "$status"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac