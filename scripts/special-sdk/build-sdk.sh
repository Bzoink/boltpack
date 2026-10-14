#!/bin/sh
# Build BoltPack with specialized SDK or niche cross-toolchain setups that are
# intentionally kept out of the aggregate Linux-hosted build workflow.
# Usage: sh scripts/special-sdk/build-sdk.sh [amigaos4|morphos|aros|riscos|ql|x68k|all]

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

usage() {
    cat <<EOF
Usage: sh scripts/special-sdk/build-sdk.sh [amigaos4|morphos|aros|riscos|ql|x68k|all]

Targets:
  amigaos4  Build the AmigaOS 4.x wrapper frontend
  morphos   Build the MorphOS wrapper frontend
  aros      Build the AROS wrapper frontend
  riscos    Build the RISC OS CLI frontend
  ql        Build the Sinclair QL CLI frontend
  x68k      Build the Sharp X68000 CLI frontend
  all       Build every special-SDK target in sequence

Environment:
  CC      Compiler override for the selected target
  CFLAGS  Extra compiler flags for the selected target

Notes:
  - The AmigaOS 4.x, MorphOS, and AROS wrappers include src/frontend/amiga/.
  - ql prefers qcc and falls back to m68k-qdos-gcc if needed.
  - all is best-effort across heterogeneous SDKs and toolchains.
EOF
}

pick_compiler() {
    if [ -n "${CC:-}" ]; then
        echo "$CC"
        return 0
    fi

    while [ $# -gt 0 ]; do
        if command -v "$1" >/dev/null 2>&1; then
            echo "$1"
            return 0
        fi
        shift
    done

    return 1
}

build_with_sources() {
    compiler="$1"
    cflags="$2"
    outdir="$3"
    output="$4"
    label="$5"
    shift 5

    mkdir -p "$outdir"

    echo "Building $label with $compiler..."
    if "$compiler" $cflags \
        -Isrc/core  \
        "$@" \
        -o "$outdir/$output"; then
        echo "Built: $outdir/$output"
    else
        echo "Failed: $label" >&2
        return 1
    fi
}

build_amigaos4() {
    compiler=$(pick_compiler ppc-amigaos-gcc) || {
        echo "No compiler found for AmigaOS 4.x (tried ppc-amigaos-gcc)." >&2
        return 1
    }
    cflags="${CFLAGS:--athread=native -ansi -Wall -Wextra -Wno-unused-function -O2}"

    build_with_sources "$compiler" "$cflags" "${AMIGAOS4_OUTDIR:-build/amigaos4}" \
        "boltpack" "AmigaOS 4.x CLI" \
        src/core/archiver.c src/core/compress.c src/core/boltlite.c \
        src/core/fluff.c src/core/bp_codepage.c \
        src/frontend/amigaos4/main.c src/core/boltlz.c
}

build_morphos() {
    compiler=$(pick_compiler ppc-morphos-gcc) || {
        echo "No compiler found for MorphOS (tried ppc-morphos-gcc)." >&2
        return 1
    }
    cflags="${CFLAGS:--noixemul -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2}"

    build_with_sources "$compiler" "$cflags" "${MORPHOS_OUTDIR:-build/morphos}" \
        "boltpack" "MorphOS CLI" \
        src/core/archiver.c src/core/compress.c src/core/boltlite.c \
        src/core/fluff.c src/core/bp_codepage.c \
        src/frontend/morphos/main.c src/core/boltlz.c
}

build_aros() {
    compiler=$(pick_compiler aros-x86_64-gcc) || {
        echo "No compiler found for AROS (tried aros-x86_64-gcc)." >&2
        return 1
    }
    cflags="${CFLAGS:--ansi -pedantic -Wall -Wextra -Wno-unused-function -O2}"

    build_with_sources "$compiler" "$cflags" "${AROS_OUTDIR:-build/aros}" \
        "boltpack" "AROS CLI" \
        src/core/archiver.c src/core/compress.c src/core/boltlite.c \
        src/core/fluff.c src/core/bp_codepage.c \
        src/frontend/aros/main.c src/core/boltlz.c
}

build_riscos() {
    compiler=$(pick_compiler arm-unknown-riscos-gcc arm-unknown-riscos-cc) || {
        echo "No compiler found for RISC OS (tried arm-unknown-riscos-gcc, arm-unknown-riscos-cc)." >&2
        return 1
    }
    cflags="${CFLAGS:--ansi -pedantic -Wall -Wextra -Wno-unused-function -O2}"

    build_with_sources "$compiler" "$cflags" "${RISCOS_OUTDIR:-build/riscos}" \
        "boltpack,ff8" "RISC OS CLI" \
        src/core/archiver.c src/core/compress.c src/core/boltlite.c \
        src/core/fluff.c src/core/bp_codepage.c \
        src/frontend/riscos/main.c src/core/boltlz.c
}

build_ql() {
    compiler=$(pick_compiler qcc m68k-qdos-gcc) || {
        echo "No compiler found for Sinclair QL (tried qcc, m68k-qdos-gcc)." >&2
        return 1
    }
    cflags="${CFLAGS:--O}"
    outdir="${QL_OUTDIR:-build/ql}"

    mkdir -p "$outdir"

    echo "Building Sinclair QL CLI with $compiler..."
    if (
        cd src/frontend/ql &&
        "$compiler" $cflags \
            -I../../core  \
            ../../core/archiver.c \
            ../../core/compress.c \
            ../../core/boltlite.c \
            ../../core/fluff.c \
            ../../core/bp_codepage.c \
            main.c \
            ../../core/boltlz.c \
            -o "../../../$outdir/boltpack"
    ); then
        echo "Built: $outdir/boltpack"
    else
        echo "Failed: Sinclair QL CLI" >&2
        return 1
    fi
}

build_x68k() {
    compiler=$(pick_compiler m68k-x680x-human-gcc) || {
        echo "No compiler found for Sharp X68000 (tried m68k-x680x-human-gcc)." >&2
        return 1
    }
    cflags="${CFLAGS:--ansi -pedantic -Wall -Wextra -Wno-unused-function -O2}"

    build_with_sources "$compiler" "$cflags" "${X68K_OUTDIR:-build/x68k}" \
        "boltpack.x" "Sharp X68000 CLI" \
        src/core/archiver.c src/core/compress.c src/core/boltlite.c \
        src/core/fluff.c \
        src/core/bp_codepage.c \
        src/frontend/x68k/main.c src/core/boltlz.c
}

build_target() {
    case "$1" in
        amigaos4)
            build_amigaos4
            ;;
        morphos)
            build_morphos
            ;;
        aros)
            build_aros
            ;;
        riscos)
            build_riscos
            ;;
        ql)
            build_ql
            ;;
        x68k)
            build_x68k
            ;;
        *)
            echo "Unknown target: $1" >&2
            usage >&2
            return 1
            ;;
    esac
}

TARGET="${1:-}"

case "$TARGET" in
    amigaos4|morphos|aros|riscos|ql|x68k)
        build_target "$TARGET"
        ;;
    all)
        status=0
        for target in amigaos4 morphos aros riscos ql x68k; do
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
