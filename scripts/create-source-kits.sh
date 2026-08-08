#!/bin/bash
# Create compile packages for targets that are not part of the main
# Linux-hosted aggregate build workflow.
# Usage: bash scripts/create-source-kits.sh [macos|classic-mac|native-unix|special-sdk|all]

set -eo pipefail

PROJROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    cat <<EOF
Usage: bash scripts/create-source-kits.sh [macos|classic-mac|native-unix|special-sdk|all]

Creates compile packages under dist/ for targets that should be built outside
the aggregate Linux cross-build path.

Targets:
  macos        Create macOS native compile packages
    classic-mac  Create Classic Mac OS native compile packages
  native-unix  Create OpenBSD/Solaris/IRIX/HP-UX/AIX native compile packages
    special-sdk  Create AmigaOS4/MorphOS/AROS/RISC OS/QL/X68000 compile packages
    all          Create all compile packages (default)
EOF
}

run_script() {
    local script_path="$1"
    if [ ! -x "$script_path" ]; then
        chmod +x "$script_path"
    fi
    bash "$script_path"
}

case "${1:-all}" in
    macos)
        run_script "$PROJROOT/scripts/create-macos-source-kits.sh"
        ;;
    classic-mac)
        run_script "$PROJROOT/scripts/create-classic-mac-source-kits.sh"
        ;;
    native-unix)
        run_script "$PROJROOT/scripts/create-native-unix-source-kits.sh"
        ;;
    special-sdk)
        run_script "$PROJROOT/scripts/create-special-sdk-source-kits.sh"
        ;;
    all)
        run_script "$PROJROOT/scripts/create-macos-source-kits.sh"
        run_script "$PROJROOT/scripts/create-classic-mac-source-kits.sh"
        run_script "$PROJROOT/scripts/create-native-unix-source-kits.sh"
        run_script "$PROJROOT/scripts/create-special-sdk-source-kits.sh"
        ;;
    -h|--help|help)
        usage
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac