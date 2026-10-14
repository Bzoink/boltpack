#!/bin/bash
# Create native compile packages for Unix targets that should be built on their
# own host systems rather than by the aggregate Linux cross-build workflow.
# Usage: bash scripts/create-native-unix-source-kits.sh [openbsd|solaris|irix|hpux|aix|all]

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
Usage: bash scripts/create-native-unix-source-kits.sh [openbsd|solaris|irix|hpux|aix|all]

Creates source-only tar.gz native compile packages under dist/ for:
  openbsd  OpenBSD
  solaris  Oracle Solaris
  irix     SGI IRIX
  hpux     HP-UX
  aix      IBM AIX
  all      Create all packages (default)
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

    case "$platform" in
        openbsd)
            label="OpenBSD"
            build_cmd="sh scripts/native-unix/build-native.sh openbsd"
            output_path="build/openbsd/boltpack"
            ;;
        solaris)
            label="Oracle Solaris"
            build_cmd="sh scripts/native-unix/build-native.sh solaris"
            output_path="build/solaris/boltpack"
            ;;
        irix)
            label="SGI IRIX"
            build_cmd="sh scripts/native-unix/build-native.sh irix"
            output_path="build/irix/boltpack"
            ;;
        hpux)
            label="HP-UX"
            build_cmd="sh scripts/native-unix/build-native.sh hpux"
            output_path="build/hpux/boltpack"
            ;;
        aix)
            label="IBM AIX"
            build_cmd="sh scripts/native-unix/build-native.sh aix"
            output_path="build/aix/boltpack"
            ;;
        *)
            return 1
            ;;
    esac

    cat >"$kitdir/build-notes.md" <<EOF
# Build Notes

This source kit is for native BoltPack CLI builds on $label.

## Requirements

- A native C compiler installed on the target host. The helper script checks cc, gcc, and clang unless CC is set explicitly.
- Standard system headers and libc development files for that platform.

## Build

Run from the root of this extracted source kit:

    $build_cmd

## Output

- CLI: $output_path

## Notes

- Override the compiler with CC=... if the default compiler lookup is not correct for your host.
- Add extra compiler flags with CFLAGS=... if you need vendor-specific or local tuning options.
- These native-Unix kits are CLI only; there is no GUI frontend in this package.
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
    copy_file "$PROJROOT/scripts/native-unix/build-native.sh" \
        "$kitdir/scripts/native-unix/build-native.sh"
    chmod +x "$kitdir/scripts/native-unix/build-native.sh"
}

create_kit() {
    local platform="$1"
    local frontend="$2"
    local kit_name="boltpack-$VERSION-source-$platform"
    local kitdir="$TMPROOT/$kit_name"
    local pkg_path="$DISTDIR/$kit_name.tar.gz"

    rm -rf "$kitdir"
    mkdir -p "$kitdir/scripts/native-unix"
    stage_common "$kitdir" "$frontend"
    write_build_notes "$kitdir" "$platform"
    mkdir -p "$DISTDIR"
    tar czf "$pkg_path" -C "$TMPROOT" "$kit_name"
    echo "Created: $pkg_path"
}

case "${1:-all}" in
    openbsd)
        create_kit "openbsd" "openbsd"
        ;;
    solaris)
        create_kit "solaris" "solaris"
        ;;
    irix)
        create_kit "irix" "irix"
        ;;
    hpux)
        create_kit "hpux" "hpux"
        ;;
    aix)
        create_kit "aix" "aix"
        ;;
    all)
        create_kit "openbsd" "openbsd"
        create_kit "solaris" "solaris"
        create_kit "irix" "irix"
        create_kit "hpux" "hpux"
        create_kit "aix" "aix"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
