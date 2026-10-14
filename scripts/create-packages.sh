#!/bin/bash
# BoltPack Distribution Package Creator
# Generates platform-standard packages from built binaries
# Usage: bash create-packages.sh [platform]
#        bash create-packages.sh              (all platforms)

set -eo pipefail

PROJROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILDDIR="$PROJROOT/build"
INSTALLDIR="$PROJROOT/staging"
DISTDIR="$PROJROOT/dist"
TEMPLATESDIR="$PROJROOT/templates"
ENCODINGSDIR="$PROJROOT/encodings"
TOTALCMD_PLUGIN_README="$PROJROOT/packaging/totalcmd-plugin/README.txt"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'  # No Color

# ============================================================================
# DEPENDENCY CHECK
# ============================================================================

check_dependencies() {
    local missing=()
    local optional=()

    # Required commands
    if ! command -v tar >/dev/null 2>&1; then
        missing+=("tar:GNU tar - used to create .tar and .tar.gz packages")
    fi
    
    if ! command -v zip >/dev/null 2>&1; then
        missing+=("zip:Info-ZIP - used to create .zip packages")
    fi

    # Optional but useful
    if ! command -v iconv >/dev/null 2>&1; then
        optional+=("iconv:GNU libiconv - used for character encoding conversion (will fall back to UTF-8)")
    fi
    
    if ! command -v lha >/dev/null 2>&1; then
        optional+=("lha:LHA archiver - used for Amiga packages (will fall back to .zip)")
    fi

    # Report results
    if [ ${#missing[@]} -gt 0 ]; then
        echo -e "${RED}[ERROR] Missing required commands:${NC}"
        for item in "${missing[@]}"; do
            cmd="${item%%:*}"
            desc="${item#*:}"
            echo -e "${RED}  • $cmd: $desc${NC}"
        done
        echo ""
        return 1
    fi

    if [ ${#optional[@]} -gt 0 ]; then
        echo -e "${YELLOW}[WARN] Optional commands not found:${NC}"
        for item in "${optional[@]}"; do
            cmd="${item%%:*}"
            desc="${item#*:}"
            echo -e "${YELLOW}  • $cmd: $desc${NC}"
        done
        echo ""
    fi

    echo -e "${GREEN}All necessary commands are installed${NC}"
    return 0
}

check_dependencies || exit 1

# Version: single source of truth from VERSION at the repo root
VERSION=$(cat "$PROJROOT/VERSION" 2>/dev/null | tr -d '[:space:]')
[ -n "$VERSION" ] || { printf 'Error: VERSION file not found at %s/VERSION\n' "$PROJROOT" >&2; exit 1; }
BUILD_DATE=$(date '+%Y-%m-%d')

# ============================================================================
# PLATFORM MAPPINGS
# Format: platform:format:encoding:template:binary_pattern:cpu
# ============================================================================

declare -A PLATFORM_FORMAT=(
    [linux]="tar.gz"
    [linux-static]="tar.gz"
    [linux32mini]="tar.gz"
    [linux-arm32]="tar.gz"
    [linux-arm64]="tar.gz"
    [linux-ppc]="tar.gz"
    [linux-x86]="tar.gz"
    [musl]="tar.gz"
    [riscv]="tar.gz"
    [wasm]="zip"
    [macosx]="tar.gz"
    [haiku]="tar.gz"
    [freebsd]="tar.gz"
    [openbsd]="tar.gz"
    [netbsd]="tar.gz"
    [hurd]="tar.gz"
    [solaris]="tar.gz"
    [illumos]="tar.gz"
    [irix]="tar.gz"
    [hpux]="tar.gz"
    [aix]="tar.gz"
    [tru64]="tar.gz"
    [sco]="tar.gz"
    [zos]="zip"
    [dos]="zip"
    [dos16]="zip"
    [win16]="zip"
    [os2-16]="zip"
    [win32]="zip"
    [win32-fltk-gui]="zip"
    [win64]="zip"
    [win64-fltk-gui]="zip"
    [linux-gui]="tar.gz"
    [os2]="zip"
    [atari]="zip"
    [amiga]="lha"
    [aros]="zip"
    [amigaos4]="zip"
    [morphos]="zip"
    [macos9]="zip"
    [macos9-68k]="hqx"
    [x68k]="zip"
    [nextstep]="tar.gz"
    [plan9]="tar"
    [ql]="zip"
    [qnx]="tar.gz"
    [riscos]="zip"
)

declare -A PLATFORM_ENCODING=(
    [linux]="UTF-8"
    [linux-static]="UTF-8"
    [linux32mini]="UTF-8"
    [linux-arm32]="UTF-8"
    [linux-arm64]="UTF-8"
    [linux-ppc]="UTF-8"
    [linux-x86]="UTF-8"
    [musl]="UTF-8"
    [riscv]="UTF-8"
    [wasm]="UTF-8"
    [macosx]="UTF-8"
    [haiku]="UTF-8"
    [freebsd]="UTF-8"
    [openbsd]="UTF-8"
    [netbsd]="UTF-8"
    [hurd]="UTF-8"
    [solaris]="UTF-8"
    [illumos]="UTF-8"
    [irix]="ISO-8859-1"
    [hpux]="UTF-8"
    [aix]="UTF-8"
    [tru64]="UTF-8"
    [sco]="ISO-8859-1"
    [zos]="UTF-8"
    [dos]="ISO-8859-1"
    [dos16]="ISO-8859-1"
    [win16]="ISO-8859-1"
    [os2-16]="ISO-8859-1"
    [win32]="UTF-8"
    [win32-fltk-gui]="UTF-8"
    [win64]="UTF-8"
    [win64-fltk-gui]="UTF-8"
    [linux-gui]="UTF-8"
    [os2]="UTF-8"
    [atari]="ISO-8859-1"
    [amiga]="ISO-8859-1"
    [aros]="UTF-8"
    [amigaos4]="UTF-8"
    [morphos]="UTF-8"
    [macos9]="ISO-8859-1"
    [macos9-68k]="ISO-8859-1"
    [x68k]="ISO-8859-1"
    [nextstep]="ISO-8859-1"
    [plan9]="UTF-8"
    [ql]="ISO-8859-1"
    [qnx]="UTF-8"
    [riscos]="ISO-8859-1"
)

declare -A PLATFORM_TEMPLATE=(
    [linux]="README-linux.txt"
    [linux-static]="README-linux.txt"
    [linux32mini]="README-linux.txt"
    [linux-arm32]="README-linux.txt"
    [linux-arm64]="README-linux.txt"
    [linux-ppc]="README-linux.txt"
    [linux-x86]="README-linux.txt"
    [musl]="README-linux.txt"
    [riscv]="README-linux.txt"
    [wasm]="README-generic.txt"
    [macosx]="README-generic.txt"
    [haiku]="README-generic.txt"
    [freebsd]="README-generic.txt"
    [openbsd]="README-generic.txt"
    [netbsd]="README-generic.txt"
    [hurd]="README-generic.txt"
    [solaris]="README-generic.txt"
    [illumos]="README-generic.txt"
    [irix]="README-generic.txt"
    [hpux]="README-generic.txt"
    [aix]="README-generic.txt"
    [tru64]="README-generic.txt"
    [sco]="README-generic.txt"
    [zos]="README-generic.txt"
    [dos]="README-windows.txt"
    [dos16]="README-windows.txt"
    [win16]="README-windows.txt"
    [os2-16]="README-windows.txt"
    [win32]="README-windows.txt"
    [win32-fltk-gui]="README-windows.txt"
    [win64]="README-windows.txt"
    [win64-fltk-gui]="README-windows.txt"
    [linux-gui]="README-linux.txt"
    [os2]="README-windows.txt"
    [atari]="README-generic.txt"
    [amiga]="README-amiga.txt"
    [aros]="README-generic.txt"
    [amigaos4]="README-generic.txt"
    [morphos]="README-generic.txt"
    [macos9]="README-generic.txt"
    [macos9-68k]="README-generic.txt"
    [x68k]="README-generic.txt"
    [nextstep]="README-generic.txt"
    [plan9]="README-generic.txt"
    [ql]="README-ql.txt"
    [qnx]="README-generic.txt"
    [riscos]="README-riscos.txt"
)

# ============================================================================
# UTILITY FUNCTIONS
# ============================================================================

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

# Check if command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Get CPU architecture from platform name
get_cpu_from_platform() {
    local platform="$1"
    case "$platform" in
        ql)                                     echo "M68K" ;;
        amiga|atari|x68k|macos9|macos9-68k)  echo "m68k" ;;
        linux-arm32)                           echo "arm" ;;
        linux-arm64|aros|amigaos4|riscos)      echo "ARM" ;;
        linux-ppc|aix|hpux)                    echo "PPC" ;;
        linux-x86|linux32mini|dos|dos16|win16|os2-16|win32|win32-fltk-gui|os2|sco)  echo "x86" ;;
        riscv)                                 echo "riscv64" ;;
        wasm)                                  echo "wasm32" ;;
        linux|linux-static|linux-gui|musl|haiku|freebsd|netbsd|openbsd|macosx|solaris|illumos|hurd|zos|win64|win64-fltk-gui|irix|tru64|qnx|plan9|nextstep|zos)  echo "x86-64" ;;
        *)                                     echo "unknown" ;;
    esac
}

create_totalcmd_plugin_package() {
    local platform="$1"
    local plugin_name=""
    local plugin_inf_name=""
    local description=""
    local pkg_name=""
    local pkg_path=""
    local plugin_path=""
    local config_path="$BUILDDIR/$platform/boltpack-config.exe"
    local tmpdir=""

    case "$platform" in
        win32)
            plugin_name="boltpack.wcx"
            plugin_inf_name="boltpack.wcx"
            description="BoltPack archive support v$VERSION (32-bit)"
            pkg_name="boltpack-$VERSION-totalcmd-plugin-win32.zip"
            ;;
        win64)
            plugin_name="boltpack.wcx64"
            plugin_inf_name="boltpack.wcx"
            description="BoltPack archive support v$VERSION (64-bit)"
            pkg_name="boltpack-$VERSION-totalcmd-plugin-win64.zip"
            ;;
        *)
            return 0
            ;;
    esac

    plugin_path="$BUILDDIR/$platform/$plugin_name"
    pkg_path="$DISTDIR/$pkg_name"

    if [ ! -f "$plugin_path" ]; then
        log_error "Total Commander plugin not found: $plugin_path"
        return 1
    fi
    if [ ! -f "$config_path" ]; then
        log_error "Total Commander config tool not found: $config_path"
        return 1
    fi
    if [ ! -f "$TOTALCMD_PLUGIN_README" ]; then
        log_error "Total Commander plugin README not found: $TOTALCMD_PLUGIN_README"
        return 1
    fi

    mkdir -p "$DISTDIR"
    tmpdir=$(mktemp -d)

    cp "$plugin_path" "$tmpdir/$plugin_name"
    cp "$config_path" "$tmpdir/boltpack-config.exe"
    cp "$TOTALCMD_PLUGIN_README" "$tmpdir/README.txt"
    cp "$PROJROOT/LICENSE.md" "$tmpdir/LICENSE"

    cat > "$tmpdir/pluginst.inf" <<EOF
[plugininstall]
description=$description
type=wcx
file=$plugin_inf_name
defaultdir=plugins\wcx\boltpack
defaultextension=bolt,blt
EOF

    rm -f "$pkg_path"
    if ! (cd "$tmpdir" && zip -rq "$pkg_path" .); then
        rm -rf "$tmpdir"
        log_error "zip creation failed for Total Commander plugin package: $platform"
        return 1
    fi

    rm -rf "$tmpdir"
    log_info "✓ Created: $pkg_name ($(du -h "$pkg_path" | cut -f1))"
    return 0
}

# ============================================================================
# HFS DISK IMAGE HELPER
# Creates an 800K HFS floppy image containing the Classic Mac 68k binary.
# Requires: hformat hcopy hmount humount (hfsutils) and python3.
# The resource fork is wrapped in MacBinary II so hcopy -m preserves it.
# ============================================================================

create_binhex() {
    local rsrc_file="$1"   # .rsrc/BoltPack from Retro68 output
    local outfile="$2"     # output .hqx path

    if ! command_exists python3; then
        log_error "python3 not found — required for BinHex creation"
        return 1
    fi

    # Write the Python script to a temp file so we can pass arguments to it
    local py_tmp
    py_tmp=$(mktemp --suffix=.py)
    cat > "$py_tmp" << 'PYEOF'
import binascii, struct, sys, os

rsrc_path = sys.argv[1]
out_path  = sys.argv[2]

try:
    with open(rsrc_path, 'rb') as f:
        rsrc = f.read()
except IOError as e:
    print('ERROR: ' + str(e), file=sys.stderr)
    sys.exit(1)

# Read the data fork (sits next to .rsrc/<name> at ../<name>)
data_fork_path = os.path.join(os.path.dirname(os.path.dirname(rsrc_path)),
                               os.path.basename(rsrc_path))
try:
    with open(data_fork_path, 'rb') as f:
        data_fork = f.read()
except IOError:
    data_fork = b''   # fall back to empty data fork

# ---- CRC-16/CCITT used by BinHex (use binascii reference) ----------------
def crc16(data, crc=0):
    return binascii.crc_hqx(data, crc)

# ---- BinHex 4.0 RLE (matches CPython binascii.rlecode_hqx) ---------------
# 0x90 bytes are always escaped as 0x90 0x00 individually.
# Runs of other bytes are only RLE-compressed when > 3 bytes long.
def rle_encode(data):
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        ch = data[i]
        if ch == 0x90:
            out.append(0x90)
            out.append(0)
            i += 1
        else:
            j = i + 1
            while j < n and data[j] == ch and j < i + 255:
                j += 1
            run = j - i
            if run > 3:
                out.append(ch)
                out.append(0x90)
                out.append(run)
                i = j
            else:
                out.append(ch)
                i += 1
    return bytes(out)

# ---- BinHex 6-bit encoding -----------------------------------------------
HQXCHARS = b'!"#$%&\'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr'

def b64_encode(data):
    bits = 0
    nbits = 0
    out = bytearray()
    for b in data:
        bits = (bits << 8) | b
        nbits += 8
        while nbits >= 6:
            nbits -= 6
            out.append(HQXCHARS[(bits >> nbits) & 0x3F])
    if nbits:
        out.append(HQXCHARS[(bits << (6 - nbits)) & 0x3F])
    return bytes(out)

# ---- Build BinHex header -------------------------------------------------
# name_len(1) + name(N) + version(1=0) + type(4) + creator(4)
#   + finder_flags(2) + data_len(4) + rsrc_len(4)
name     = b'BoltPack'
ftype    = b'APPL'
creator  = b'BoPk'      # must match kCreatorCode in main.c
data_len = len(data_fork)
rsrc_len = len(rsrc)

hdr  = struct.pack('>B', len(name)) + name
hdr += b'\x00'          # version = 0
hdr += ftype + creator
hdr += struct.pack('>HII', 0, data_len, rsrc_len)
# Classic BinHex decoders expect the raw crc_hqx value with no extra flush.
hdr_crc  = struct.pack('>H', crc16(hdr))
data_crc = struct.pack('>H', crc16(data_fork))
rsrc_crc = struct.pack('>H', crc16(rsrc))

raw     = hdr + hdr_crc + data_fork + data_crc + rsrc + rsrc_crc
encoded = b64_encode(rle_encode(raw))

# ---- Write .hqx file (Mac \r line endings, 64 chars/line) ----------------
out_lines = []
i = 0
while i < len(encoded):
    out_lines.append(encoded[i:i+64])
    i += 64

with open(out_path, 'wb') as f:
    f.write(b'(This file must be converted with BinHex 4.0)\r\r:')
    f.write(b'\r'.join(out_lines))
    f.write(b':\r')

print('OK')
PYEOF
    python3 "$py_tmp" "$rsrc_file" "$outfile"
    local rc=$?
    rm -f "$py_tmp"
    [ $rc -eq 0 ] && [ -f "$outfile" ] && return 0 || return 1
}

create_macbinary() {
    local rsrc_file="$1"   # .rsrc/BoltPack from Retro68 output
    local outfile="$2"     # output .bin path

    if ! command_exists python3; then
        log_error "python3 not found — required for MacBinary II creation"
        return 1
    fi

    python3 << PYEOF
import struct, sys
rsrc_path = "$rsrc_file"
out_path  = "$outfile"
try:
    with open(rsrc_path, 'rb') as f:
        rsrc = f.read()
except IOError as e:
    print('ERROR: ' + str(e), file=sys.stderr)
    sys.exit(1)
name    = b'BoltPack'
ftype   = b'APPL'
creator = b'BoPk'
h = bytearray(128)
h[0] = 0
h[1] = len(name)
h[2:2+len(name)] = name
h[65:69] = ftype
h[69:73] = creator
struct.pack_into('>I', h, 83, 0)          # data fork length = 0
struct.pack_into('>I', h, 87, len(rsrc))  # resource fork length
h[122] = 129   # MacBinary II version
h[123] = 129   # minimum MacBinary II version
crc = 0
for b in h[:124]:
    crc ^= b << 8
    for _ in range(8):
        crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
    crc &= 0xFFFF
struct.pack_into('>H', h, 124, crc)
pad = lambda d: d + b'\x00' * ((128 - len(d) % 128) % 128)
with open(out_path, 'wb') as f:
    f.write(bytes(h))
    f.write(pad(b''))   # empty data fork (padded)
    f.write(pad(rsrc))  # resource fork (padded)
print('OK')
PYEOF
    [ -f "$outfile" ] && return 0 || return 1
}

create_hfs_image() {
    local rsrc_file="$1"   # .rsrc/BoltPack from Retro68 output
    local stagindir="$2"   # staging dir already populated with README/LICENSE
    local imgpath="$3"     # output .img path
    local label="BoltPack"

    # Dependency check — exit immediately so the user knows what to install
    for tool in hformat hcopy hmount humount; do
        if ! command_exists "$tool"; then
            log_error "$tool not found — install hfsutils to build HFS disk images"
            exit 1
        fi
    done
    if ! command_exists python3; then
        log_error "python3 not found — required for MacBinary II creation"
        exit 1
    fi

    local tmpdir
    tmpdir=$(mktemp -d)
    local macbin="$tmpdir/BoltPack.bin"

    # Build a MacBinary II file from the Retro68 resource fork.
    # The data fork is intentionally empty (Classic Mac apps live in the rsrc fork).
    python3 << PYEOF
import struct
rsrc_path = "$rsrc_file"
out_path  = "$macbin"
with open(rsrc_path, 'rb') as f:
    rsrc = f.read()
name    = b'BoltPack'
ftype   = b'APPL'
creator = b'BoPk'
h = bytearray(128)
h[0] = 0
h[1] = len(name)
h[2:2+len(name)] = name
h[65:69] = ftype
h[69:73] = creator
struct.pack_into('>I', h, 83, 0)          # data fork length
struct.pack_into('>I', h, 87, len(rsrc))  # resource fork length
h[122] = 129   # MacBinary II version
h[123] = 129   # minimum MacBinary II version
crc = 0
for b in h[:124]:
    crc ^= b << 8
    for _ in range(8):
        crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
    crc &= 0xFFFF
struct.pack_into('>H', h, 124, crc)
pad = lambda d: d + b'\x00' * ((128 - len(d) % 128) % 128)
with open(out_path, 'wb') as f:
    f.write(bytes(h))
    f.write(pad(rsrc))
PYEOF
    if [ ! -f "$macbin" ]; then
        log_error "MacBinary II creation failed"
        rm -rf "$tmpdir"
        return 1
    fi

    # 800K HFS floppy (1600 × 512-byte sectors)
    dd if=/dev/zero of="$imgpath" bs=512 count=1600 2>/dev/null
    hformat -l "$label" "$imgpath" > /dev/null
    hmount "$imgpath" > /dev/null

    # Copy the application (resource fork preserved via MacBinary II)
    hcopy -m "$macbin" : || {
        humount "$imgpath" > /dev/null 2>&1
        rm -rf "$tmpdir"
        log_error "hcopy failed for $imgpath"
        return 1
    }

    # Copy plain-text files into the image
    for txtfile in "$stagindir/README.txt" "$stagindir/LICENSE" "$stagindir/RELEASE-NOTES.txt"; do
        [ -f "$txtfile" ] && hcopy -t "$txtfile" : 2>/dev/null || true
    done

    humount "$imgpath" > /dev/null
    rm -rf "$tmpdir"
    return 0
}

# ============================================================================
# PACKAGE CREATION
# ============================================================================

create_ql_uqlx_package() {
    local stagindir="$1"
    local binary_name="$2"
    local cpu="$3"
    local store_root="$INSTALLDIR/ql/uqlx"
    local store_name="boltpack_uqlx"
    local store_dir="$store_root/$store_name"
    local readme_template="$TEMPLATESDIR/README-ql-uqlx.txt"
    local pkg_name="boltpack-$VERSION-ql-$cpu-uqlx.tar.gz"
    local pkg_path="$DISTDIR/$pkg_name"

    if ! command_exists qcp || ! command_exists qls; then
        log_warn "qcp/qls not found, skipping QL UQLX container package"
        return 0
    fi

    if [ ! -f "$readme_template" ]; then
        log_warn "QL UQLX template not found, skipping QL UQLX container package"
        return 0
    fi

    mkdir -p "$DISTDIR"
    rm -rf "$store_root"
    mkdir -p "$store_dir"
    : > "$store_dir/.-UQLX-"

    qcp "$stagindir/$binary_name" "$store_dir/$binary_name" || {
        log_error "qcp failed while creating QL UQLX container package"
        return 1
    }

    {
        sed "s|{VERSION}|$VERSION|g; s|{BUILD_DATE}|$BUILD_DATE|g; s|{ARCH}|$cpu|g; s|{BINARY_SIZE}|~50 KB|g" "$readme_template"
    } > "$store_dir/readme_txt"

    cp "$stagindir/LICENSE" "$store_dir/license_txt"
    if [ -f "$stagindir/RELEASE-NOTES.txt" ]; then
        cp "$stagindir/RELEASE-NOTES.txt" "$store_dir/release_notes_txt"
    fi

    qls "$store_dir" >/dev/null || {
        log_error "qls validation failed for QL UQLX container package"
        return 1
    }

    cd "$store_root"
    tar czf "$pkg_path" "$store_name" || {
        log_error "tar.gz creation failed for QL UQLX container package"
        return 1
    }
    cd - > /dev/null

    if [ ! -f "$pkg_path" ]; then
        log_error "Package file not created: $pkg_path"
        return 1
    fi

    local size=$(du -h "$pkg_path" | cut -f1)

    log_info "✓ Created: $pkg_name ($size)"
    echo "$pkg_path"

    return 0
}

create_package() {
    local platform="$1"
    local binary="$2"
    local format="${PLATFORM_FORMAT[$platform]}"
    local encoding="${PLATFORM_ENCODING[$platform]}"
    local template="${PLATFORM_TEMPLATE[$platform]}"
    local cpu=$(get_cpu_from_platform "$platform")
    
    if [ -z "$format" ]; then
        log_warn "Unknown platform: $platform (skipping)"
        return 1
    fi
    
    log_info "Creating package for $platform ($cpu)..."
    
    # Create staging directory
    local stagindir="$INSTALLDIR/$platform/package"
    mkdir -p "$stagindir"
    rm -rf "$stagindir"/*  # Clean previous
    mkdir -p "$stagindir"
    
    # Copy binary/binaries
    local binary_name=$(basename "$binary")
    if [ "$platform" = "amiga" ]; then
        local amiga_cli="$BUILDDIR/$platform/boltpack"
        local amiga_gui="$BUILDDIR/$platform/boltpack-gui"
        local amiga_icon_script="$PROJROOT/scripts/make_amiga_info_icons.py"

        if [ ! -f "$amiga_cli" ]; then
            log_error "Binary not found: $amiga_cli"
            return 1
        fi
        if [ ! -f "$amiga_gui" ]; then
            log_error "Binary not found: $amiga_gui"
            return 1
        fi

        cp "$amiga_cli" "$stagindir/boltpack"
        cp "$amiga_gui" "$stagindir/boltpack-gui"
        chmod +x "$stagindir/boltpack" 2>/dev/null || true
        chmod +x "$stagindir/boltpack-gui" 2>/dev/null || true

        if ! command_exists python3; then
            log_error "python3 not found — required to generate Amiga Workbench icons"
            return 1
        fi
        if [ ! -f "$amiga_icon_script" ]; then
            log_error "Amiga icon generator not found: $amiga_icon_script"
            return 1
        fi
        if ! python3 "$amiga_icon_script" "$stagindir" >/dev/null; then
            log_error "Amiga Workbench icon generation failed"
            return 1
        fi

        binary_name="boltpack"
    elif [ "$platform" = "atari" ]; then
        local atari_cli="$BUILDDIR/$platform/boltpack.tos"
        local atari_gui="$BUILDDIR/$platform/boltpack.prg"

        if [ ! -f "$atari_cli" ]; then
            log_error "Binary not found: $atari_cli"
            return 1
        fi
        if [ ! -f "$atari_gui" ]; then
            log_error "Binary not found: $atari_gui"
            return 1
        fi

        cp "$atari_cli" "$stagindir/boltpack.tos"
        cp "$atari_gui" "$stagindir/boltpack.prg"
        chmod +x "$stagindir/boltpack.tos" 2>/dev/null || true
        chmod +x "$stagindir/boltpack.prg" 2>/dev/null || true
        binary_name="boltpack.tos"
    else
        if [ ! -f "$binary" ]; then
            log_error "Binary not found: $binary"
            return 1
        fi
        cp "$binary" "$stagindir/$binary_name"
        chmod +x "$stagindir/$binary_name" 2>/dev/null || true
    fi

    # For Windows platforms, copy all binaries (exe, wcx, config exe)
    if [ "$platform" = "win32" ] || [ "$platform" = "win64" ]; then
        for extra in "$BUILDDIR/$platform"/boltpack*.exe \
                     "$BUILDDIR/$platform"/boltpack*.wcx \
                     "$BUILDDIR/$platform"/boltpack*.wcx64; do
            if [ -f "$extra" ] && [ "$extra" != "$binary" ]; then
                cp "$extra" "$stagindir/"
            fi
        done
    fi
    
    # Copy the complete project and third-party license bundle.
    cp "$PROJROOT/LICENSE.md" "$stagindir/LICENSE"
    
    # Copy/process README
    local template_file="$TEMPLATESDIR/$template"
    if [ ! -f "$template_file" ]; then
        log_warn "Template not found: $template, using generic"
        template_file="$TEMPLATESDIR/README-generic.txt"
    fi
    
    # Create platform-specific README with variable substitution
    {
        sed "s|{VERSION}|$VERSION|g; s|{BUILD_DATE}|$BUILD_DATE|g; s|{ARCH}|$cpu|g; s|{BINARY_SIZE}|~50 KB|g" "$template_file"
    } > "$stagindir/README.txt"
    
    # Copy RELEASE-NOTES
    if [ -f "$PROJROOT/RELEASE-NOTES.md" ]; then
        cp "$PROJROOT/RELEASE-NOTES.md" "$stagindir/RELEASE-NOTES.txt"
    fi
    
    # Recode README to target encoding if not UTF-8
    if [ "$encoding" != "UTF-8" ] && command_exists iconv; then
        # Try to convert using iconv
        if iconv -f UTF-8 -t "$encoding" "$stagindir/README.txt" > "$stagindir/README.txt.tmp" 2>/dev/null; then
            mv "$stagindir/README.txt.tmp" "$stagindir/README.txt"
        else
            rm -f "$stagindir/README.txt.tmp"
            log_warn "Failed to recode README to $encoding, keeping UTF-8"
        fi
    fi
    
    # Create final package
    # For platforms where the CPU is already implied by the platform name,
    # omit the redundant CPU suffix.
    local pkg_name
    case "$platform" in
        macos9-68k)  pkg_name="boltpack-$VERSION-mac68k.$format" ;;
        *)           pkg_name="boltpack-$VERSION-$platform-$cpu.$format" ;;
    esac
    local pkg_path="$DISTDIR/$pkg_name"
    
    mkdir -p "$DISTDIR"
    
    case "$format" in
        tar.gz)
            tar czf "$pkg_path" -C "$stagindir" . 2>/dev/null || {
                log_error "tar.gz creation failed for $platform"
                return 1
            }
            ;;
        tar)
            tar cf "$pkg_path" -C "$stagindir" . 2>/dev/null || {
                log_error "tar creation failed for $platform"
                return 1
            }
            ;;
        zip)
            if ! command_exists zip; then
                log_error "zip command not found (install zip package)"
                return 1
            fi
            (cd "$stagindir" && zip -rq "$pkg_path" .) || {
                log_error "zip creation failed for $platform"
                return 1
            }
            ;;
        lha)
            if command_exists lha && (cd "$stagindir" && lha a "$pkg_path" * >/dev/null 2>&1); then
                :
            else
                log_warn "lha write support not available, using zip instead for $platform"
                format="zip"
                pkg_name="boltpack-$VERSION-$platform-$cpu.zip"
                pkg_path="$DISTDIR/$pkg_name"
                (cd "$stagindir" && zip -rq "$pkg_path" .) || {
                    log_error "zip creation failed for $platform"
                    return 1
                }
            fi
            ;;
        dmg)
            log_warn "DMG creation not yet implemented, using tar.gz for $platform"
            pkg_name="boltpack-$VERSION-$platform-$cpu.tar.gz"
            pkg_path="$DISTDIR/$pkg_name"
            tar czf "$pkg_path" -C "$stagindir" . || {
                log_error "tar.gz creation failed for $platform"
                return 1
            }
            ;;
        hqx)
            create_binhex \
                "$BUILDDIR/macos9-68k/.rsrc/BoltPack" \
                "$pkg_path" || return 1
            ;;
        bin)
            create_macbinary \
                "$BUILDDIR/macos9-68k/.rsrc/BoltPack" \
                "$pkg_path" || return 1
            ;;
        img)
            create_hfs_image \
                "$BUILDDIR/macos9-68k/.rsrc/BoltPack" \
                "$stagindir" \
                "$pkg_path" || return 1
            ;;
        *)
            log_error "Unknown format: $format"
            return 1
            ;;
    esac
    
    # Verify package was created
    if [ ! -f "$pkg_path" ]; then
        log_error "Package file not created: $pkg_path"
        return 1
    fi
    
    local size=$(du -h "$pkg_path" | cut -f1)

    if [ "$platform" = "win32" ] || [ "$platform" = "win64" ]; then
        create_totalcmd_plugin_package "$platform" || return 1
    fi

    log_info "✓ Created: $pkg_name ($size)"
    echo "$pkg_path"  # Return path for summary

    if [ "$platform" = "ql" ]; then
        create_ql_uqlx_package "$stagindir" "$binary_name" "$cpu" || return 1
    fi
    
    return 0
}

# ============================================================================
# MAIN
# ============================================================================

main() {
    local target_platform="${1:-all}"
    
    echo "=========================================="
    echo "BoltPack Distribution Package Creator"
    echo "=========================================="
    echo "Version: $VERSION"
    echo "Build date: $BUILD_DATE"
    echo ""
    
    # Check prerequisites
    if [ ! -d "$BUILDDIR" ]; then
        log_error "Build directory not found: $BUILDDIR"
        echo "First, build binaries with: make <platform> or docker compose"
        exit 1
    fi
    
    # Find all platforms with binaries
    local platforms_found=()
    local packages_created=()
    local packages_skipped=()
    
    if [ "$target_platform" = "all" ]; then
        # Auto-detect all platforms with binaries
        for platform_dir in "$BUILDDIR"/*; do
            if [ -d "$platform_dir" ]; then
                local platform=$(basename "$platform_dir")
                # Build output folders and scratch tests are not release platforms.
                case " ${!PLATFORM_FORMAT[*]} " in
                    *" $platform "*) ;;
                    *) continue ;;
                esac
                # Skip directories that don't have binaries
                if ls "$platform_dir"/boltpack* >/dev/null 2>&1 || \
                   ls "$platform_dir"/bpmini* >/dev/null 2>&1 || \
                   [ -f "$platform_dir/.rsrc/BoltPack" ]; then
                    platforms_found+=("$platform")
                fi
            fi
        done
    else
        # Single platform specified
        if [ -d "$BUILDDIR/$target_platform" ]; then
            platforms_found=("$target_platform")
        else
            log_error "Platform directory not found: $BUILDDIR/$target_platform"
            exit 1
        fi
    fi
    
    if [ ${#platforms_found[@]} -eq 0 ]; then
        log_warn "No platforms with built binaries found"
        echo "Build some packages first with: make <platform>"
        exit 1
    fi
    
    echo "Found $(echo ${#platforms_found[@]}) platform(s) with binaries"
    echo ""
    
    # Create packages
    for platform in "${platforms_found[@]}"; do
        # Find the binary
        local binary=""
        if [ "$platform" = "macos9-68k" ] && [ -f "$BUILDDIR/$platform/.rsrc/BoltPack" ]; then
            binary="$BUILDDIR/$platform/.rsrc/BoltPack"
        elif [ "$platform" = "amiga" ]; then
            if [ -f "$BUILDDIR/$platform/boltpack" ]; then
                binary="$BUILDDIR/$platform/boltpack"
            elif [ -f "$BUILDDIR/$platform/boltpack-gui" ]; then
                binary="$BUILDDIR/$platform/boltpack-gui"
            fi
        elif [ "$platform" = "atari" ]; then
            if [ -f "$BUILDDIR/$platform/boltpack.tos" ]; then
                binary="$BUILDDIR/$platform/boltpack.tos"
            elif [ -f "$BUILDDIR/$platform/boltpack.prg" ]; then
                binary="$BUILDDIR/$platform/boltpack.prg"
            fi
        else
            for candidate in "$BUILDDIR/$platform"/boltpack* "$BUILDDIR/$platform"/bpmini*; do
                if [ -f "$candidate" ]; then
                    binary="$candidate"
                    break
                fi
            done
        fi
        
        if [ -z "$binary" ]; then
            packages_skipped+=("$platform (no binary found)")
            continue
        fi
        
        if create_package "$platform" "$binary"; then
            packages_created+=("$platform")
        else
            packages_skipped+=("$platform (package creation failed)")
        fi
    done
    
    # Summary
    echo ""
    echo "=========================================="
    echo "SUMMARY"
    echo "=========================================="
    
    if [ ${#packages_created[@]} -gt 0 ]; then
        echo -e "${GREEN}Created packages:${NC}"
        for pkg in "${packages_created[@]}"; do
            echo "  ✓ $pkg"
        done
    fi
    
    if [ ${#packages_skipped[@]} -gt 0 ]; then
        echo -e "${YELLOW}Skipped:${NC}"
        for pkg in "${packages_skipped[@]}"; do
            echo "  ⊘ $pkg"
        done
    fi
    
    echo ""
    echo "Output directory: $DISTDIR"
    echo ""
    
    # List all packages
    if [ -d "$DISTDIR" ] && ls "$DISTDIR"/*.{tar.gz,zip,lha,tar,dmg} >/dev/null 2>&1; then
        echo "Final packages:"
        ls -lh "$DISTDIR"/*.{tar.gz,zip,lha,tar,dmg} 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'
    fi
    
    echo ""
    echo "Complete!"
}

main "$@"
