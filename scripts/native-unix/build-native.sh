#!/bin/sh
# Build BoltPack natively on a Unix host that is not part of the aggregate
# Linux-hosted cross-build workflow.
# Usage: sh scripts/native-unix/build-native.sh [openbsd|solaris|irix|hpux|aix|all]

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

usage() {
    cat <<EOF
Usage: sh scripts/native-unix/build-native.sh [openbsd|solaris|irix|hpux|aix|all]

Targets:
  openbsd  Build the OpenBSD CLI frontend natively
  solaris  Build the Solaris CLI frontend natively
  irix     Build the IRIX CLI frontend natively
  hpux     Build the HP-UX CLI frontend natively
  aix      Build the AIX CLI frontend natively
  all      Build every native-Unix target in sequence

Environment:
  CC      C compiler to use (defaults to cc, gcc, or clang)
  CFLAGS  Extra compiler flags (defaults assume GCC/Clang-like compilers)

Notes:
  - The default CFLAGS are aimed at GCC/Clang ports on these systems.
  - For vendor compilers, override CC and CFLAGS explicitly.
EOF
}

find_c_compiler() {
    if [ -n "${CC:-}" ]; then
        echo "$CC"
    elif command -v cc >/dev/null 2>&1; then
        echo "cc"
    elif command -v gcc >/dev/null 2>&1; then
        echo "gcc"
    elif command -v clang >/dev/null 2>&1; then
        echo "clang"
    else
        echo "No C compiler found. Set CC or install cc/gcc/clang." >&2
        exit 1
    fi
}

build_target() {
    target="$1"
    label="$2"
    outdir="$3"

    mkdir -p "$outdir"

    echo "Building $label with $COMPILER..."

    "$COMPILER" $CLI_CFLAGS \
        -Isrc/core  \
        src/core/archiver.c src/core/compress.c src/core/lzss.c \
        src/core/fluff.c src/core/bp_codepage.c \
        src/core/boltlz.c \
        "src/frontend/$target/main.c" \
        -o "$outdir/boltpack"

    echo "Built: $outdir/boltpack"
}

COMPILER=$(find_c_compiler)
CLI_CFLAGS="${CFLAGS:--ansi -pedantic -Wall -Wextra -Wno-unused-function -O2}"

case "${1:-}" in
    openbsd)
        build_target openbsd "OpenBSD CLI" "${OPENBSD_OUTDIR:-build/openbsd}"
        ;;
    solaris)
        build_target solaris "Solaris CLI" "${SOLARIS_OUTDIR:-build/solaris}"
        ;;
    irix)
        build_target irix "IRIX CLI" "${IRIX_OUTDIR:-build/irix}"
        ;;
    hpux)
        build_target hpux "HP-UX CLI" "${HPUX_OUTDIR:-build/hpux}"
        ;;
    aix)
        build_target aix "AIX CLI" "${AIX_OUTDIR:-build/aix}"
        ;;
    all)
        build_target openbsd "OpenBSD CLI" "${OPENBSD_OUTDIR:-build/openbsd}"
        build_target solaris "Solaris CLI" "${SOLARIS_OUTDIR:-build/solaris}"
        build_target irix "IRIX CLI" "${IRIX_OUTDIR:-build/irix}"
        build_target hpux "HP-UX CLI" "${HPUX_OUTDIR:-build/hpux}"
        build_target aix "AIX CLI" "${AIX_OUTDIR:-build/aix}"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac