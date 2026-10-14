#!/bin/bash
# Create compile packages for targets that depend on niche SDKs or specialized
# cross-toolchains and are not part of the aggregate Linux-hosted build path.
# Usage: bash scripts/create-special-sdk-source-kits.sh [amigaos4|morphos|aros|riscos|ql|x68k|all]

set -eo pipefail

PROJROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISTDIR="$PROJROOT/dist"
TMPROOT="$(mktemp -d)"

cleanup() {
    rm -rf "$TMPROOT"
}
trap cleanup EXIT

VERSION=$(tr -d '[:space:]' < "$PROJROOT/VERSION")
[ -n "$VERSION" ] || VERSION="1.0"

usage() {
    cat <<EOF
Usage: bash scripts/create-special-sdk-source-kits.sh [amigaos4|morphos|aros|riscos|ql|x68k|all]

Creates source-only tar.gz compile packages under dist/ for:
  amigaos4  AmigaOS 4.x PPC
  morphos   MorphOS PPC
  aros      AROS x86-64
  riscos    RISC OS ARM
  ql        Sinclair QL / SMSQ-E
  x68k      Sharp X68000 / Human68k
  all       Create all packages (default)
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
    local platform="$2"
    local label=""
    local build_cmd=""
    local output_path=""
    local requirements=""
    local notes=""

    case "$platform" in
        amigaos4)
            label="AmigaOS 4.x"
            build_cmd="sh scripts/special-sdk/build-sdk.sh amigaos4"
            output_path="build/amigaos4/boltpack"
            requirements="- adtools or another SDK that provides ppc-amigaos-gcc."
            notes="- This wrapper build also includes the shared src/frontend/amiga support code.\n- Override the compiler with CC=... or add flags with CFLAGS=... if needed."
            ;;
        morphos)
            label="MorphOS"
            build_cmd="sh scripts/special-sdk/build-sdk.sh morphos"
            output_path="build/morphos/boltpack"
            requirements="- MorphOS SDK or another setup that provides ppc-morphos-gcc."
            notes="- Override the compiler with CC=... or add flags with CFLAGS=... if needed."
            ;;
        aros)
            label="AROS x86-64"
            build_cmd="sh scripts/special-sdk/build-sdk.sh aros"
            output_path="build/aros/boltpack"
            requirements="- AROS toolchain that provides aros-x86_64-gcc."
            notes="- Override the compiler with CC=... or add flags with CFLAGS=... if needed."
            ;;
        riscos)
            label="RISC OS"
            build_cmd="sh scripts/special-sdk/build-sdk.sh riscos"
            output_path="build/riscos/boltpack,ff8"
            requirements="- GCCSDK or another setup that provides arm-unknown-riscos-gcc or arm-unknown-riscos-cc."
            notes="- Override the compiler with CC=... or add flags with CFLAGS=... if needed."
            ;;
        ql)
            label="Sinclair QL / SMSQ-E"
            build_cmd="sh scripts/special-sdk/build-sdk.sh ql"
            output_path="build/ql/boltpack"
            requirements="- qcc or m68k-qdos-gcc installed and available in PATH."
            notes="- The helper builds from inside src/frontend/ql to match the expected QL toolchain workflow.\n- Override the compiler with CC=... or add flags with CFLAGS=... if needed."
            ;;
        x68k)
            label="Sharp X68000 / Human68k"
            build_cmd="sh scripts/special-sdk/build-sdk.sh x68k"
            output_path="build/x68k/boltpack.x"
            requirements="- m68k-x680x-human-gcc installed and available in PATH."
            notes="- Override the compiler with CC=... or add flags with CFLAGS=... if needed."
            ;;
        *)
            return 1
            ;;
    esac

    requirements=$(printf '%b' "$requirements")
    notes=$(printf '%b' "$notes")

    cat >"$kitdir/build-notes.md" <<EOF
# Build Notes

This source kit is for native or SDK-based BoltPack CLI builds on $label.

## Requirements

$requirements

## Build

Run from the root of this extracted source kit:

    $build_cmd

## Output

- CLI: $output_path

## Notes

$notes
EOF
}

stage_common() {
    local kitdir="$1"
    local frontend="$2"

    copy_tree "$PROJROOT/src/core" "$kitdir/src/core"
    copy_file "$PROJROOT/LICENSE.md" "$kitdir/LICENSE.md"
    copy_tree "$PROJROOT/src/frontend/$frontend" "$kitdir/src/frontend/$frontend"
    copy_file "$PROJROOT/docs/BUILD.md" "$kitdir/docs/BUILD.md"
    copy_file "$PROJROOT/docs/frontends.md" "$kitdir/docs/frontends.md"
    copy_file "$PROJROOT/docs/portability.md" "$kitdir/docs/portability.md"
    copy_file "$PROJROOT/RELEASE-NOTES.md" "$kitdir/RELEASE-NOTES.md"
    copy_file "$PROJROOT/scripts/special-sdk/build-sdk.sh" \
        "$kitdir/scripts/special-sdk/build-sdk.sh"
    chmod +x "$kitdir/scripts/special-sdk/build-sdk.sh"
}

stage_amiga_wrapper_support() {
    local kitdir="$1"

    copy_tree "$PROJROOT/src/frontend/amiga" "$kitdir/src/frontend/amiga"
}

create_kit() {
    local platform="$1"
    local frontend="$2"
    local kit_name="boltpack-$VERSION-source-$platform"
    local kitdir="$TMPROOT/$kit_name"
    local pkg_path="$DISTDIR/$kit_name.tar.gz"

    rm -rf "$kitdir"
    mkdir -p "$kitdir/scripts/special-sdk"
    stage_common "$kitdir" "$frontend"

    case "$platform" in
        amigaos4|morphos|aros)
            stage_amiga_wrapper_support "$kitdir"
            ;;
    esac

    write_build_notes "$kitdir" "$platform"

    mkdir -p "$DISTDIR"
    tar czf "$pkg_path" -C "$TMPROOT" "$kit_name"
    echo "Created: $pkg_path"
}

case "${1:-all}" in
    amigaos4)
        create_kit "amigaos4" "amigaos4"
        ;;
    morphos)
        create_kit "morphos" "morphos"
        ;;
    aros)
        create_kit "aros" "aros"
        ;;
    riscos)
        create_kit "riscos" "riscos"
        ;;
    ql)
        create_kit "ql" "ql"
        ;;
    x68k)
        create_kit "x68k" "x68k"
        ;;
    all)
        create_kit "amigaos4" "amigaos4"
        create_kit "morphos" "morphos"
        create_kit "aros" "aros"
        create_kit "riscos" "riscos"
        create_kit "ql" "ql"
        create_kit "x68k" "x68k"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
