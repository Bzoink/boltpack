#!/bin/bash
# BoltPack build script
# Usage: ./build.sh [--verbose] <target>
#        VERBOSE=1 ./build.sh <target>

set -eo pipefail

# ============================================================================
# DEPENDENCY CHECK
# ============================================================================

check_dependencies() {
    local missing=()
    local optional=()

    # Required commands
    if ! command -v make >/dev/null 2>&1; then
        missing+=("make:GNU Make - used to compile binaries")
    fi

    # Optional but needed for full workflow
    if ! command -v python3 >/dev/null 2>&1; then
        optional+=("python3:Python 3.x - needed for ./build.sh download-page (webpage generation)")
    fi

    # Report results
    if [ ${#missing[@]} -gt 0 ]; then
        echo "================================"
        echo "ERROR: Missing required commands"
        echo "================================"
        for item in "${missing[@]}"; do
            cmd="${item%%:*}"
            desc="${item#*:}"
            echo "  • $cmd: $desc"
        done
        echo ""
        exit 1
    fi

    if [ ${#optional[@]} -gt 0 ]; then
        echo "================================"
        echo "WARN: Optional commands not found"
        echo "================================"
        for item in "${optional[@]}"; do
            cmd="${item%%:*}"
            desc="${item#*:}"
            echo "  • $cmd: $desc"
        done
        echo ""
    fi

    echo "All necessary commands are installed"
}

check_dependencies

# Check for --verbose flag and strip it from arguments
VERBOSE="${VERBOSE:-}"
if [ "$1" = "--verbose" ] || [ "$1" = "-v" ]; then
    VERBOSE=1
    shift
fi

# ---- target definitions -------------------------------------------------
# Each target: frontend directory name, CC, CFLAGS additions, LDFLAGS, binary suffix

build_target() {
    target_name="$1"
    compiler="$2"
    extra_cflags="$3"
    extra_ldflags="$4"
    suffix="$5"
    frontend="${6:-$target_name}"

    if [ ! -d "src/frontend/$frontend" ]; then
        echo "  SKIP  $target_name (no frontend source yet)"
        return 0
    fi

    # Check if compiler is available
    if ! command -v "$compiler" >/dev/null 2>&1; then
        echo "  SKIP  $target_name ($compiler not found)"
        return 0
    fi

    base_cflags="${7:--ansi -pedantic} -Wall -Wextra -Wno-unused-function -O2"
    extra_make_vars="$8"

    echo "  BUILD $target_name ($compiler)"
    if [ -n "$VERBOSE" ]; then
        if make FRONTEND="$frontend" CC="$compiler" \
             CFLAGS="$base_cflags $extra_cflags" \
             LDFLAGS="$extra_ldflags" \
             BUILDDIR="build/$target_name" \
             TARGET="build/$target_name/boltpack$suffix" \
             $extra_make_vars \
             2>&1 | sed 's/^/        /'; then
            echo "  OK    build/$target_name/boltpack$suffix"
            return 0
        else
            echo "  FAIL  build/$target_name/boltpack$suffix"
            return 1
        fi
    else
        if make FRONTEND="$frontend" CC="$compiler" \
             CFLAGS="$base_cflags $extra_cflags" \
             LDFLAGS="$extra_ldflags" \
             BUILDDIR="build/$target_name" \
             TARGET="build/$target_name/boltpack$suffix" \
             $extra_make_vars \
             >/dev/null 2>&1; then
            echo "  OK    build/$target_name/boltpack$suffix"
            return 0
        else
            echo "  FAIL  build/$target_name/boltpack$suffix"
            return 1
        fi
    fi
}

# Wrapper for standalone Makefile targets that don't use the FRONTEND pattern.
# build_make_target <target> <compiler> <expected-output>
build_make_target() {
    target_name="$1"
    compiler="$2"
    output="$3"

    if ! command -v "$compiler" >/dev/null 2>&1; then
        echo "  SKIP  $target_name ($compiler not found)"
        return 0
    fi

    echo "  BUILD $target_name ($compiler)"
    if [ -n "$VERBOSE" ]; then
        if make "$target_name" 2>&1 | sed 's/^/        /'; then
            echo "  OK    $output"
            return 0
        else
            echo "  FAIL  $target_name"
            return 1
        fi
    else
        if make "$target_name" >/dev/null 2>&1; then
            echo "  OK    $output"
            return 0
        else
            echo "  FAIL  $target_name"
            return 1
        fi
    fi
}

# ---- targets -------------------------------------------------------------

do_linux() {
    build_target linux cc "" "-fuse-ld=bfd" "" linux "-ansi -pedantic" "ENABLE_FAST_FLUFF=1"
}

do_linux_static() {
    build_target linux cc "" "-fuse-ld=bfd -static" ""
    # Rename to distinguish from dynamic build
    if [ -f build/linux/boltpack ]; then
        cp build/linux/boltpack build/linux/boltpack-static
        echo "  OK    build/linux/boltpack-static"
    fi
}

do_linux_i386() {
    build_target linux gcc "-m32" "-fuse-ld=bfd -m32" ""
    if [ -f build/linux/boltpack ]; then
        cp build/linux/boltpack build/linux/boltpack-i386
        echo "  OK    build/linux/boltpack-i386"
    fi
}

do_linux32mini() {
    if ! printf 'int main(void){return 0;}\n' | gcc -m32 -x c -o /dev/null - >/dev/null 2>&1; then
        echo "  SKIP  linux32mini (gcc -m32 not usable)"
        return 0
    fi

    echo "  BUILD linux32mini (32-bit Linux bpmini -- mini archiver)"
    if [ -n "$VERBOSE" ]; then
        if make linuxmini 2>&1 | sed 's/^/        /'; then
            echo "  OK    build/linux32mini/bpmini"
            return 0
        else
            echo "  FAIL  linux32mini"
            return 1
        fi
    else
        if make linuxmini >/dev/null 2>&1; then
            echo "  OK    build/linux32mini/bpmini"
            return 0
        else
            echo "  FAIL  linux32mini"
            return 1
        fi
    fi
}

do_dos_djgpp() {
    if [ -d /usr/local/djgpp/bin ]; then
        PATH="/usr/local/djgpp/bin:$PATH"
        export PATH
    fi
    build_target dos i586-pc-msdosdjgpp-gcc "" "" ".exe" dos "-std=gnu89" "ENABLE_ZSTD=0"
}

do_win32() {
    if ! command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
        echo "  SKIP  win32 (i686-w64-mingw32-gcc not found)"
        return 0
    fi
    echo "  BUILD win32 (boltpack + Total Commander plugin)"
    if [ -n "$VERBOSE" ]; then
        if make win32 2>&1 | sed 's/^/        /'; then
            echo "  OK    build/win32/boltpack.exe + boltpack.wcx"
            return 0
        else
            echo "  FAIL  win32"
            return 1
        fi
    else
        if make win32 >/dev/null 2>&1; then
            echo "  OK    build/win32/boltpack.exe + boltpack.wcx"
            return 0
        else
            echo "  FAIL  win32"
            return 1
        fi
    fi
}

do_win64() {
    if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        echo "  SKIP  win64 (x86_64-w64-mingw32-gcc not found)"
        return 0
    fi
    echo "  BUILD win64 (boltpack + Total Commander plugin)"
    if [ -n "$VERBOSE" ]; then
        if make win64 2>&1 | sed 's/^/        /'; then
            echo "  OK    build/win64/boltpack.exe + boltpack.wcx64"
            return 0
        else
            echo "  FAIL  win64"
            return 1
        fi
    else
        if make win64 >/dev/null 2>&1; then
            echo "  OK    build/win64/boltpack.exe + boltpack.wcx64"
            return 0
        else
            echo "  FAIL  win64"
            return 1
        fi
    fi
}

do_win32_fltk_gui() {
    if [ ! -f "compilation/fltk-win32/lib/libfltk.a" ]; then
        echo "  SKIP  win32-fltk-gui (FLTK not built; run 'make fltk-win32' first)"
        return 0
    fi
    echo "  BUILD win32-fltk-gui (FLTK GUI for Windows 32-bit)"
    if [ -n "$VERBOSE" ]; then
        if make win32-fltk-gui 2>&1 | sed 's/^/        /'; then
            echo "  OK    build/win32-fltk-gui/boltpack-gui.exe"
            return 0
        else
            echo "  FAIL  win32-fltk-gui"
            return 1
        fi
    else
        if make win32-fltk-gui >/dev/null 2>&1; then
            echo "  OK    build/win32-fltk-gui/boltpack-gui.exe"
            return 0
        else
            echo "  FAIL  win32-fltk-gui"
            return 1
        fi
    fi
}

do_win64_fltk_gui() {
    if [ ! -f "compilation/fltk-win64/lib/libfltk.a" ]; then
        echo "  SKIP  win64-fltk-gui (FLTK not built; run 'make fltk-win64' first)"
        return 0
    fi
    echo "  BUILD win64-fltk-gui (FLTK GUI for Windows 64-bit)"
    if [ -n "$VERBOSE" ]; then
        if make win64-fltk-gui 2>&1 | sed 's/^/        /'; then
            echo "  OK    build/win64-fltk-gui/boltpack-gui.exe"
            return 0
        else
            echo "  FAIL  win64-fltk-gui"
            return 1
        fi
    else
        if make win64-fltk-gui >/dev/null 2>&1; then
            echo "  OK    build/win64-fltk-gui/boltpack-gui.exe"
            return 0
        else
            echo "  FAIL  win64-fltk-gui"
            return 1
        fi
    fi
}

do_linux_gui() {
    if ! command -v fltk-config >/dev/null 2>&1; then
        echo "  SKIP  linux-gui (fltk-config not found; install FLTK development package)"
        return 0
    fi
    echo "  BUILD linux-gui (FLTK GUI for Linux x86-64)"
    if [ -n "$VERBOSE" ]; then
        if make linux-gui 2>&1 | sed 's/^/        /'; then
            echo "  OK    build/linux-gui/boltpack-gui"
            return 0
        else
            echo "  FAIL  linux-gui"
            return 1
        fi
    else
        if make linux-gui >/dev/null 2>&1; then
            echo "  OK    build/linux-gui/boltpack-gui"
            return 0
        else
            echo "  FAIL  linux-gui"
            return 1
        fi
    fi
}

do_arm32() {
    if ! printf 'int main(void){return 0;}\n' | arm-linux-gnueabihf-gcc -x c -o /dev/null - >/dev/null 2>&1; then
        echo "  SKIP  linux-arm32 (arm-linux-gnueabihf-gcc not usable)"
        return 0
    fi
    build_make_target arm arm-linux-gnueabihf-gcc build/linux-arm32/boltpack
}

do_arm64() {
    if ! printf 'int main(void){return 0;}\n' | aarch64-linux-gnu-gcc -x c -o /dev/null - >/dev/null 2>&1; then
        echo "  SKIP  linux-arm64 (aarch64-linux-gnu-gcc not usable)"
        return 0
    fi
    build_make_target arm64 aarch64-linux-gnu-gcc build/linux-arm64/boltpack
}

do_amiga_m68k() {
    if [ ! -d "src/frontend/amiga" ]; then
        echo "  SKIP  amiga (no frontend source yet)"
        return 0
    fi

    if ! command -v m68k-amigaos-gcc >/dev/null 2>&1; then
        echo "  SKIP  amiga (m68k-amigaos-gcc not found)"
        return 0
    fi

    echo "  BUILD amiga (m68k-amigaos-gcc)"
    if [ -n "$VERBOSE" ]; then
        if make amiga FLUFF_NATIVE_FLAGS= 2>&1 | sed 's/^/        /'; then
            echo "  OK    build/amiga/boltpack"
            echo "  OK    build/amiga/boltpack-gui"
            return 0
        else
            echo "  FAIL  amiga"
            return 1
        fi
    else
        if make amiga FLUFF_NATIVE_FLAGS= >/dev/null 2>&1; then
            echo "  OK    build/amiga/boltpack"
            echo "  OK    build/amiga/boltpack-gui"
            return 0
        else
            echo "  FAIL  amiga"
            return 1
        fi
    fi
}

do_atari() {
    if [ -d /usr/m68k-atari-mint/bin ]; then
        PATH="/usr/m68k-atari-mint/bin:$PATH"
        export PATH
    fi

    if ! command -v m68k-atari-mint-gcc >/dev/null 2>&1; then
        echo "  SKIP  atari (m68k-atari-mint-gcc not found)"
        return 0
    fi

    echo "  BUILD atari (m68k-atari-mint-gcc)"
    if [ -n "$VERBOSE" ]; then
        if make atari FLUFF_NATIVE_FLAGS= 2>&1 | sed 's/^/        /'; then
            echo "  OK    build/atari/boltpack.tos"
            echo "  OK    build/atari/boltpack.prg"
            return 0
        else
            echo "  FAIL  atari"
            return 1
        fi
    else
        if make atari FLUFF_NATIVE_FLAGS= >/dev/null 2>&1; then
            echo "  OK    build/atari/boltpack.tos"
            echo "  OK    build/atari/boltpack.prg"
            return 0
        else
            echo "  FAIL  atari"
            return 1
        fi
    fi
}

do_macos9_68k() {
    local cc="/usr/retro68/bin/m68k-apple-macos-gcc"
    if [ ! -x "$cc" ]; then
        echo "  SKIP  macos9-68k ($cc not found)"
        return 0
    fi
    echo "  BUILD macos9-68k ($cc)"
    if [ -n "$VERBOSE" ]; then
        if make macos9-68k 2>&1 | sed 's/^/        /'; then
            echo "  OK    build/macos9-68k/.rsrc/BoltPack"
            return 0
        else
            echo "  FAIL  macos9-68k"
            return 1
        fi
    else
        if make macos9-68k >/dev/null 2>&1; then
            echo "  OK    build/macos9-68k/.rsrc/BoltPack"
            return 0
        else
            echo "  FAIL  macos9-68k"
            return 1
        fi
    fi
}


do_wasm() {
    build_make_target wasm emcc build/wasm/boltpack.js
}

do_haiku() {
    build_make_target haiku x86_64-unknown-haiku-gcc build/haiku/boltpack
}

do_riscos() {
    build_make_target riscos arm-unknown-riscos-gcc "build/riscos/boltpack,ff8"
}

do_aros() {
    build_make_target aros aros-x86_64-gcc build/aros/boltpack
}

do_amigaos4() {
    build_make_target amigaos4 ppc-amigaos-gcc build/amigaos4/boltpack
}

do_morphos() {
    build_make_target morphos ppc-morphos-gcc build/morphos/boltpack
}

do_freebsd() {
    local crt
    crt=$(clang -target x86_64-unknown-freebsd14 -print-file-name=crtbegin.o 2>/dev/null)
    if [ "$crt" = "crtbegin.o" ]; then
        echo "  SKIP  freebsd (no FreeBSD sysroot found)"
        return 0
    fi
    build_make_target freebsd clang build/freebsd/boltpack
}

do_netbsd() {
    local crt
    crt=$(clang -target x86_64--netbsd -print-file-name=crt0.o 2>/dev/null)
    if [ "$crt" = "crt0.o" ]; then
        echo "  SKIP  netbsd (no NetBSD sysroot found)"
        return 0
    fi
    build_make_target netbsd clang build/netbsd/boltpack
}

do_openbsd() {
    local crt
    crt=$(clang -target x86_64--openbsd -print-file-name=crt0.o 2>/dev/null)
    if [ "$crt" = "crt0.o" ]; then
        echo "  SKIP  openbsd (no OpenBSD sysroot found)"
        return 0
    fi
    build_make_target openbsd clang build/openbsd/boltpack
}

do_macosx() {
    build_make_target macosx arm64-apple-macosx11-clang build/macosx/boltpack
}

do_solaris() {
    build_make_target solaris x86_64-pc-solaris2.11-gcc build/solaris/boltpack
}

do_illumos() {
    build_make_target illumos x86_64-unknown-illumos-gcc build/illumos/boltpack
}

do_irix() {
    build_make_target irix mips-sgi-irix6.5-gcc build/irix/boltpack
}

do_hpux() {
    build_make_target hpux hppa-hp-hpux-gcc build/hpux/boltpack
}

do_aix() {
    build_make_target aix powerpc-ibm-aix-gcc build/aix/boltpack
}

do_riscv() {
    build_make_target riscv riscv64-linux-gnu-gcc build/riscv/boltpack
}

do_musl() {
    build_make_target musl musl-gcc build/musl/boltpack
}

do_hurd() {
    build_make_target hurd gcc build/hurd/boltpack
}

do_e2k() {
    build_make_target e2k e2k-unknown-linux-gcc build/e2k/boltpack
}

do_os2() {
    build_make_target os2 os2-gcc build/os2/boltpack.exe
}

do_ql() {
    build_make_target ql m68k-qdos-gcc build/ql/boltpack
}

do_x68k() {
    build_make_target x68k m68k-x680x-human-gcc build/x68k/boltpack.x
}

# ---- 16-bit targets (Open Watcom) ----------------------------------------
# Requires Open Watcom 2.0 at /opt/watcom (or wcc on PATH).
# The mini archiver (src/core/mini/) is used instead of the full archiver.
# miniLZO is excluded on 16-bit; stored-only compression is used.

_check_watcom() {
    if command -v wcc >/dev/null 2>&1; then
        return 0
    fi
    if [ -x /opt/watcom/binl64/wcc ]; then
        PATH="/opt/watcom/binl64:$PATH"
        export PATH
        return 0
    fi
    return 1
}

_do_watcom_target() {
    local target="$1" output="$2"
    echo "  BUILD $target (Open Watcom 16-bit)"
    if [ -n "$VERBOSE" ]; then
        if make "$target" 2>&1 | sed 's/^/        /'; then
            echo "  OK    $output"
            return 0
        else
            echo "  FAIL  $target"
            return 1
        fi
    else
        if make "$target" >/dev/null 2>&1; then
            echo "  OK    $output"
            return 0
        else
            echo "  FAIL  $target"
            return 1
        fi
    fi
}

do_dos16() {
    if ! _check_watcom; then
        echo "  SKIP  dos16 (Open Watcom not found)"
        return 0
    fi
    _do_watcom_target dos16 build/dos16/bpmini.exe
}

do_win16() {
    if ! _check_watcom; then
        echo "  SKIP  win16 (Open Watcom not found)"
        return 0
    fi
    _do_watcom_target win16 build/win16/bpmini.exe
}

do_os2_16() {
    if ! _check_watcom; then
        echo "  SKIP  os2-16 (Open Watcom not found)"
        return 0
    fi
    _do_watcom_target os2-16 build/os2-16/bpmini.exe
}

# ---- Fluff optimization tier builds ----------------------------------------
# Tier detection is automatic at compile time via ULONG_MAX macro check.
# These targets build the Linux binary with explicit tier selection for
# benchmarking and optimization validation.

do_linux_tier3() {
    echo "  BUILD linux-tier3 [Tier 3: 32-bit fluff optimization]"
    if [ -n "$VERBOSE" ]; then
        if make tier3 2>&1 | sed 's/^/        /'; then
            echo "  OK    build/linux/boltpack-tier3"
            return 0
        else
            echo "  FAIL  linux-tier3"
            return 1
        fi
    else
        if make tier3 >/dev/null 2>&1; then
            echo "  OK    build/linux/boltpack-tier3"
            return 0
        else
            echo "  FAIL  linux-tier3"
            return 1
        fi
    fi
}

do_linux_tier4() {
    echo "  BUILD linux-tier4 [Tier 4: 64-bit fluff optimization]"
    if [ -n "$VERBOSE" ]; then
        if make tier4 2>&1 | sed 's/^/        /'; then
            echo "  OK    build/linux/boltpack-tier4"
            return 0
        else
            echo "  FAIL  linux-tier4"
            return 1
        fi
    else
        if make tier4 >/dev/null 2>&1; then
            echo "  OK    build/linux/boltpack-tier4"
            return 0
        else
            echo "  FAIL  linux-tier4"
            return 1
        fi
    fi
}

do_all_tiers() {
    echo "Building all fluff optimization tiers for Linux..."
    echo ""
    do_linux_tier3 || true
    do_linux_tier4 || true
    echo ""
    echo "Fluff tier binaries built:"
    ls -lh build/linux/boltpack-tier* 2>/dev/null || true
    echo ""
    echo "To benchmark: python3 test/bench_corpus.py"
}

do_all() {
    echo "Building aggregate Linux-hosted targets..."
    echo ""
    do_linux       || true
    do_linux_static || true
    do_linux_i386  || true
    do_linux32mini || true
    do_arm32       || true
    do_arm64       || true
    do_riscv       || true
    do_musl        || true
    do_hurd        || true
    do_dos_djgpp   || true
    do_win32       || true
    do_win64       || true
    do_win32_fltk_gui || true
    do_win64_fltk_gui || true
    do_linux_gui   || true
    do_os2         || true
    do_freebsd     || true
    do_netbsd      || true
    do_illumos     || true
    do_haiku       || true
    do_amiga_m68k  || true
    do_atari       || true
    do_macos9_68k  || true
    do_e2k         || true
    do_wasm        || true
    do_dos16       || true
    do_win16       || true
    do_os2_16      || true
    echo ""
    echo "Done. Built binaries:"
    find build -name 'boltpack*' -type f 2>/dev/null | sort | while read f; do
        size=$(ls -lh "$f" | awk '{print $5}')
        printf "  %-40s %s\n" "$f" "$size"
    done
}

do_win_fltk_installers() {
    local built_any=0

    if ! command -v makensis >/dev/null 2>&1; then
        echo "  SKIP  Windows FLTK installers (makensis not found; install NSIS)"
        return 0
    fi

    # win64 FLTK installer
    if [ -f "compilation/fltk-win64/lib/libfltk.a" ] && \
       [ -f "staging/win64/package/README.txt" ]; then
        echo "  BUILD Windows installer (win64 FLTK)"
        if [ -n "$VERBOSE" ]; then
            if make win64-fltk-installer 2>&1 | sed 's/^/        /'; then
                echo "  OK    build/win64-fltk-gui/boltpack-*-win64-setup.exe"
                built_any=1
            else
                echo "  FAIL  win64 FLTK installer"
            fi
        else
            if make win64-fltk-installer >/dev/null 2>&1; then
                echo "  OK    build/win64-fltk-gui/boltpack-*-win64-setup.exe"
                built_any=1
            else
                echo "  FAIL  win64 FLTK installer"
            fi
        fi
    else
        if [ ! -f "compilation/fltk-win64/lib/libfltk.a" ]; then
            echo "  SKIP  win64 FLTK installer (FLTK not built; run 'make fltk-win64' first)"
        else
            echo "  SKIP  win64 FLTK installer (staging/win64/package not ready)"
        fi
    fi

    # win32 FLTK installer
    if [ -f "compilation/fltk-win32/lib/libfltk.a" ] && \
       [ -f "staging/win32/package/README.txt" ]; then
        echo "  BUILD Windows installer (win32 FLTK)"
        if [ -n "$VERBOSE" ]; then
            if make win32-fltk-installer 2>&1 | sed 's/^/        /'; then
                echo "  OK    build/win32-fltk-gui/boltpack-*-win32-setup.exe"
                built_any=1
            else
                echo "  FAIL  win32 FLTK installer"
            fi
        else
            if make win32-fltk-installer >/dev/null 2>&1; then
                echo "  OK    build/win32-fltk-gui/boltpack-*-win32-setup.exe"
                built_any=1
            else
                echo "  FAIL  win32 FLTK installer"
            fi
        fi
    else
        if [ ! -f "compilation/fltk-win32/lib/libfltk.a" ]; then
            echo "  SKIP  win32 FLTK installer (FLTK not built; run 'make fltk-win32' first)"
        else
            echo "  SKIP  win32 FLTK installer (staging/win32/package not ready)"
        fi
    fi

    return 0
}

do_binary_packages() {
    echo "Creating distribution packages from compiled binaries..."
    echo ""

    # Check if any binaries exist
    if ! find build -name "boltpack*" -type f 2>/dev/null | grep -q .; then
        echo "  WARN  No compiled binaries found in build/"
        echo "        Run ./build.sh all first to build aggregate cross-build targets"
        return 2
    fi

    # Clear old install and dist directories
    echo "  CLEAN Removing old packages and staging..."
    rm -rf dist/* 2>/dev/null || true
    mkdir -p dist

    echo "  RUN   scripts/create-packages.sh"
    if [ -n "$VERBOSE" ]; then
        bash scripts/create-packages.sh 2>&1 | sed 's/^/        /'
    else
        bash scripts/create-packages.sh >/dev/null 2>&1
    fi

    if [ $? -eq 0 ]; then
        echo "  OK    Packages created"
        echo ""
        echo "Distribution packages built:"
        ls -lh dist/ 2>/dev/null | tail -n +2 | while read -r line; do
            printf "        %s\n" "$line"
        done
    else
        echo "  FAIL  Package creation failed"
        return 1
    fi

    echo ""
    do_win_fltk_installers
}

do_compile_packages() {
    echo "Creating compile packages for non-aggregate targets..."
    echo ""

    echo "  RUN   scripts/create-source-kits.sh"
    if [ -n "$VERBOSE" ]; then
        bash scripts/create-source-kits.sh 2>&1 | sed 's/^/        /'
    else
        bash scripts/create-source-kits.sh >/dev/null 2>&1
    fi

    if [ $? -eq 0 ]; then
        echo "  OK    Compile packages created"
    else
        echo "  FAIL  Compile package creation failed"
        return 1
    fi
}

do_packages() {
    local status=0
    local had_binary_packages=1
    local binary_status=0

    echo "Creating all distribution packages..."
    echo ""

    # Clear old install and dist directories once for both binary and compile packages.
    echo "  CLEAN Removing old packages and staging..."
    rm -rf dist/* 2>/dev/null || true
    mkdir -p dist

    if do_binary_packages; then
        had_binary_packages=0
    else
        binary_status=$?
        echo ""
        if [ $binary_status -eq 2 ]; then
            echo "  NOTE  Continuing with compile packages only."
        else
            status=1
            echo "  WARN  Binary packaging failed; compile packages will still be attempted."
        fi
    fi

    echo ""
    if ! do_compile_packages; then
        status=1
    fi

    if [ $binary_status -eq 2 ] && [ $status -eq 0 ]; then
        echo ""
        echo "Package creation complete: compile packages created, binary packages skipped."
        return 0
    fi

    return $status
}

do_download_page() {
    echo "Generating download webpage..."
    echo ""

    # Check if dist directory exists with packages
    if [ ! -d dist ] || [ -z "$(ls -A dist 2>/dev/null)" ]; then
        echo "  WARN  No packages found in dist/"
        echo "        Run ./build.sh packages first"
        return 1
    fi

    echo "  RUN   scripts/generate-download-page.py"
    if [ -n "$VERBOSE" ]; then
        python3 scripts/generate-download-page.py 2>&1 | sed 's/^/        /'
    else
        python3 scripts/generate-download-page.py >/dev/null 2>&1
    fi

    if [ $? -eq 0 ]; then
        if [ -f dist/index.html ]; then
            echo "  OK    dist/index.html"
        else
            echo "  WARN  generate-download-page.py ran but no index.html created"
            return 1
        fi
    else
        echo "  FAIL  Download page generation failed"
        return 1
    fi
}

do_all_with_packages() {
    echo "========================================================================"
    echo "Full build and packaging workflow:"
    echo "  1. Build aggregate Linux-hosted targets"
    echo "  2. Create distribution packages"
    echo "  3. Generate download webpage"
    echo "========================================================================"
    echo ""

    # Step 1: Build all
    do_all
    echo ""

    # Step 2: Create packages
    do_packages
    if [ $? -ne 0 ]; then
        echo ""
        echo "Package creation failed. Aborting."
        return 1
    fi
    echo ""

    # Step 3: Generate download page
    do_download_page
    if [ $? -ne 0 ]; then
        echo ""
        echo "Download page generation failed."
        return 1
    fi

    echo ""
    echo "========================================================================"
    echo "Build and packaging complete!"
    echo ""
    echo "Outputs:"
    echo "  Binaries:   build/<platform>/boltpack*"
    echo "  Packages:   dist/*.tar.gz, dist/*.zip, etc."
    echo "  Webpage:    dist/index.html"
    echo "======================================================================="
}

do_clean() {
    echo "Cleaning build/"
    make clean
    if [ -d staging ] && [ -n "$(ls -A staging 2>/dev/null)" ]; then
        echo "Cleaning staging/"
        rm -rf staging/*/
    fi
    if [ -d dist ] && [ -n "$(ls -A dist 2>/dev/null)" ]; then
        echo "Cleaning dist/"
        rm -rf dist/*
    fi
    if [ -d compilation ] && [ -n "$(ls -A compilation 2>/dev/null)" ]; then
        echo "Cleaning compilation/"
        rm -rf compilation/*/
    fi
}

do_help() {
    cat <<'EOF'
BoltPack build script

Usage: ./build.sh [--verbose] <target>
       VERBOSE=1 ./build.sh <target>

Options:
  --verbose, -v  Show full compiler output (default: quiet summary)
  VERBOSE=1      Environment variable to enable verbose mode

Targets:
  linux          Build native Linux binary (default)
  linux-static   Build statically linked Linux binary
  linux-i386     Build 32-bit Linux binary
  linux-tier3    Build Linux binary with Tier 3 (32-bit) fluff optimization
  linux-tier4    Build Linux binary with Tier 4 (64-bit) fluff optimization
  linux-gui      Build Linux FLTK GUI (requires FLTK development package)
  linux32mini    Build 32-bit bpmini Linux binary (gcc -m32 required)
  musl           Build with musl-gcc (static musl libc)
  hurd           Build for GNU Hurd

  arm32|arm      Cross-compile for ARM 32-bit Linux (RPi etc.)
  arm64|aarch64  Cross-compile for ARM 64-bit Linux (RPi 3/4/5)
  riscv          Cross-compile for RISC-V 64 Linux
  e2k            Cross-compile for Elbrus (E2K) Linux

  dos            Cross-compile for DOS (DJGPP with go32 extender)
  win32          Cross-compile for Windows 32-bit (MinGW), includes Total Commander WCX plugin
  win64          Cross-compile for Windows 64-bit (MinGW), includes Total Commander WCX plugin
  win32-fltk-gui  Cross-compile Windows 32-bit FLTK GUI (requires 'make fltk-win32' first)
  win64-fltk-gui  Cross-compile Windows 64-bit FLTK GUI (requires 'make fltk-win64' first)
  win-fltk-installers  Build NSIS installers for win32/win64 (requires FLTK GUIs + staging dirs)
  os2            Cross-compile for OS/2 (EMX/GCC)

    macosx         Expert/manual macOS build target; compile package preferred
  freebsd        Cross-compile for FreeBSD x86_64 (clang + FreeBSD sysroot)
  netbsd         Cross-compile for NetBSD x86_64 (clang + NetBSD sysroot)
    openbsd        Expert/manual OpenBSD target; compile package preferred
  haiku          Cross-compile for Haiku x86_64
    solaris        Expert/manual Solaris target; compile package preferred
  illumos        Cross-compile for illumos x86_64
    irix           Expert/manual IRIX target; compile package preferred
    hpux           Expert/manual HP-UX target; compile package preferred
    aix            Expert/manual AIX target; compile package preferred

    amiga          Cross-compile Amiga CLI and GadTools GUI (68k)
    amigaos4       Expert/manual AmigaOS 4 PPC target; compile package preferred
    morphos        Expert/manual MorphOS PPC target; compile package preferred
    aros           Expert/manual AROS x86_64 target; compile package preferred
    atari          Cross-compile Atari CLI (.tos) and GEM GUI (.prg)
    riscos         Expert/manual RISC OS ARM target; compile package preferred
    ql             Expert/manual Sinclair QL target; compile package preferred
    x68k           Expert/manual Sharp X68000 target; compile package preferred

  wasm           Cross-compile to WebAssembly via Emscripten (emcc)

16-bit mini archiver targets (Open Watcom 2.0 at /opt/watcom):
  dos16          16-bit DOS (8086 compact model)
  win16          16-bit Windows 3.x (8086 compact model)
  os2-16         16-bit OS/2 1.x (8086 compact model)

  all-tiers      Build fluff optimization tiers (tier3 + tier4) for Linux
    all            Build aggregate Linux-hosted targets only (skips missing compilers)

Workflow Commands:
  all-with-packages
                 Full workflow: build all + create packages + generate webpage
                 (clears dist/ and staging/ first)
    binary-packages
                                 Create distribution packages from compiled binaries only
    compile-packages
                                 Create compile packages for non-aggregate targets
    source-kits     Alias for compile-packages
    packages        Create both binary packages and compile packages
                                 (binary packages are skipped if no compiled binaries exist)
  download-page  Generate HTML download webpage (requires packages in dist/)

Maintenance:
  clean          Remove all build outputs (build/, staging/, dist/, compilation/)
  help           Show this message

Output goes to:
  Binaries:  build/<target>/boltpack[.exe/.js/...]
    Packages:  dist/*.tar.gz, dist/*.zip, etc.
  Webpage:   dist/index.html

Missing cross-compilers are silently skipped.

Targets that require native hosts or niche SDK setups remain available as
direct expert commands but are removed from the aggregate all workflow in
favor of compile packages.

Fluff Optimization Tiers:
  Tier 3 (32-bit): For x86, ARM 32-bit, and other 32-bit systems.
                   Uses 32-bit fluff bit-reader/writer accumulator.
  Tier 4 (64-bit): For x86-64, ARM64, RISC-V 64, and other 64-bit systems.
                   Uses 64-bit fluff accumulator for better performance.
EOF
}

# ---- main ----------------------------------------------------------------

if [ $# -eq 0 ]; then
    do_help
    exit 0
fi

case "$1" in
    linux)        do_linux ;;
    linux-static) do_linux_static ;;
    linux-i386)   do_linux_i386 ;;
    linux-tier3)  do_linux_tier3 ;;
    linux-tier4)  do_linux_tier4 ;;
    linux-gui)    do_linux_gui ;;
    arm32|arm)    do_arm32 ;;
    arm64|aarch64) do_arm64 ;;
    riscv)        do_riscv ;;
    musl)         do_musl ;;
    hurd)         do_hurd ;;
    e2k)          do_e2k ;;
    dos)          do_dos_djgpp ;;
    linux32mini)  do_linux32mini ;;
    win32)        do_win32 ;;
    win64)        do_win64 ;;
    win32-fltk-gui) do_win32_fltk_gui ;;
    win64-fltk-gui) do_win64_fltk_gui ;;
    win32-fltk-installer|win32-installer) do_win_fltk_installers ;;
    win64-fltk-installer|win64-installer) do_win_fltk_installers ;;
    win-fltk-installers|win-installers) do_win_fltk_installers ;;
    os2)          do_os2 ;;
    macosx)       do_macosx ;;
    freebsd)      do_freebsd ;;
    netbsd)       do_netbsd ;;
    openbsd)      do_openbsd ;;
    haiku)        do_haiku ;;
    solaris)      do_solaris ;;
    illumos)      do_illumos ;;
    irix)         do_irix ;;
    hpux)         do_hpux ;;
    aix)          do_aix ;;
    amiga)        do_amiga_m68k ;;
    amigaos4)     do_amigaos4 ;;
    morphos)      do_morphos ;;
    aros)         do_aros ;;
    atari)        do_atari ;;
    macos9-68k)   do_macos9_68k ;;
    riscos)       do_riscos ;;
    ql)           do_ql ;;
    x68k)         do_x68k ;;
    wasm)         do_wasm ;;
    dos16)        do_dos16 ;;
    win16)        do_win16 ;;
    os2-16)       do_os2_16 ;;
    all-tiers)    do_all_tiers ;;
    all)          do_all ;;
    all-with-packages|all-packages|release) do_all_with_packages ;;
    binary-packages) do_binary_packages ;;
    compile-packages|source-kits) do_compile_packages ;;
    packages)     do_packages ;;
    download-page) do_download_page ;;
    clean)        do_clean ;;
    help|--help|-h) do_help ;;
    *)
        echo "Unknown target: $1"
        echo "Run ./build.sh help for available targets."
        exit 1
        ;;
esac
