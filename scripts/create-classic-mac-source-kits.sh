#!/bin/bash
# Create Classic Mac OS source kits for native PPC builds on Mac OS 9.
# Usage: bash scripts/create-classic-mac-source-kits.sh [ppc|all]

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
Usage: bash scripts/create-classic-mac-source-kits.sh [ppc|all]

Creates source-only tar.gz kits under dist/ for native builds on:
    ppc     Classic Mac OS 9 PowerPC (CodeWarrior Pro 8)
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

    cat >"$kitdir/build-notes.md" <<'EOF'
# Build Notes

This source kit is for native BoltPack builds on Classic Mac OS 9 PowerPC.

## Supported Toolchains

- CodeWarrior Pro 8 (Metrowerks), PowerPC C target
- MPW 3.5 with MrC, PPCLink, and Rez
- Classic Mac OS 9.1 or 9.2.x on a G3 or G4

## Files To Add To The Project

- src/core/archiver.c
- src/core/compress.c
- src/core/lzss.c
- src/core/fluff.c
- src/core/bp_codepage.c
- lib/fastlz/fastlz.c
- src/frontend/macos9/main.c
- src/frontend/macos9/boltpack.r

## Include Paths

- src/core
- lib/fastlz

## Common Libraries

- InterfaceLib
- AppearanceLib
- NavigationLib
- MathLib

## CodeWarrior Build

Use a native PPC application target in CodeWarrior Pro 8.

### Required Libraries

- MSL C.PPC.Lib

### Target Settings

- File type: APPL
- Creator: BoPk
- Preferred heap: 2048 KB
- Minimum heap: 1536 KB
- Stack: 256 KB
- Processor: 750 / G3 scheduling
- C dialect: ANSI C, ANSI Strict off

## MPW Build

Use MPW 3.5 with the PowerPC MrC compiler, PPCLink, and Rez. A practical
workflow is:

1. Open the extracted source kit in MPW Shell.
2. Add `src:core` and `lib:fastlz` to the MrC include search path.
3. Compile each C file listed above with MrC for PowerPC output.
4. Compile `src:frontend:macos9:boltpack.r` with Rez.
5. Link the resulting objects with PPCLink against the common libraries above
     plus the MPW PPC ANSI C runtime shipped with your MPW release.
6. Merge the compiled resource fork into the linked `BoltPack` application.
7. Set the output file type to `APPL`, creator to `BoPk`, preferred heap to
     2048 KB, minimum heap to 1536 KB, and stack to 256 KB.

The exact runtime object and library names differ slightly between MPW
releases, so use the standard PPC C runtime from your installed MPW tree.

### Example MPW Compile Sequence

The commands below are a starting point. Adjust the runtime library names and
library paths to match your MPW installation.

```text
NewFolder obj

MrC -opt peep -i src:core -i lib:fastlz -o obj:archiver.o src:core:archiver.c
MrC -opt peep -i src:core -i lib:fastlz -o obj:compress.o src:core:compress.c
MrC -opt peep -i src:core -i lib:fastlz -o obj:lzss.o src:core:lzss.c
MrC -opt peep -i src:core -i lib:fastlz -o obj:fluff.o src:core:fluff.c
MrC -opt peep -i src:core -i lib:fastlz -o obj:bp_codepage.o src:core:bp_codepage.c
MrC -opt peep -i src:core -i lib:fastlz -o obj:fastlz.o lib:fastlz:fastlz.c
MrC -opt peep -i src:core -i lib:fastlz -o obj:main.o src:frontend:macos9:main.c
Rez src:frontend:macos9:boltpack.r -o obj:boltpack.rsrc

PPCLink -o BoltPack obj:archiver.o obj:compress.o obj:lzss.o obj:fluff.o \
        obj:bp_codepage.o obj:fastlz.o obj:main.o \
        InterfaceLib AppearanceLib NavigationLib MathLib <MPW PPC C runtime>
```

## Resources

Compile src/frontend/macos9/boltpack.r with Rez and add the resulting resource
fork to the application target. The PPC frontend expects menus, dialogs, and
the logo picture from that resource file.

## Notes

- No CodeWarrior .mcp project is bundled; create one native PPC application
    target or MPW build worksheet and add the files above.
- The frontend uses Navigation Services when present, but runtime-checks it,
  so older installs still run without folder-picking support.
- Copy the extracted source tree to a FAT transfer image from Linux if you do
  not want to unpack the tarball directly on Mac OS 9.
EOF
}

stage_common() {
    local kitdir="$1"

    copy_tree "$PROJROOT/src/core" "$kitdir/src/core"
    copy_tree "$PROJROOT/lib/fastlz" "$kitdir/lib/fastlz"
    copy_file "$PROJROOT/LICENSE.md" "$kitdir/LICENSE.md"
    copy_tree "$PROJROOT/src/frontend/macos9" "$kitdir/src/frontend/macos9"
    rm -rf "$kitdir/src/frontend/macos9/68k"
    copy_file "$PROJROOT/docs/BUILD.md" "$kitdir/docs/BUILD.md"
    copy_file "$PROJROOT/docs/frontends.md" "$kitdir/docs/frontends.md"
    copy_file "$PROJROOT/docs/portability.md" "$kitdir/docs/portability.md"
    copy_file "$PROJROOT/RELEASE-NOTES.md" "$kitdir/RELEASE-NOTES.md"
}

create_kit() {
    local kit_name="boltpack-$VERSION-source-macos9-ppc"
    local kitdir="$TMPROOT/$kit_name"
    local pkg_path="$DISTDIR/$kit_name.tar.gz"

    rm -rf "$kitdir"
    mkdir -p "$kitdir"

    stage_common "$kitdir"
    write_build_notes "$kitdir"

    cat >"$kitdir/build_date.md" <<EOF
# Package Build Date

Kit:     $kit_name
Created: $(date '+%Y-%m-%d %H:%M:%S %Z')
EOF

    mkdir -p "$DISTDIR"
    tar czf "$pkg_path" -C "$TMPROOT" "$kit_name"
    echo "Created: $pkg_path"
}

case "${1:-all}" in
    ppc|all)
        create_kit
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac
