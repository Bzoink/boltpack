#!/usr/bin/env python3
"""BoltPack Download Page Generator

Scans the dist/ directory for release packages and writes dist/index.html —
a plain HTML page (no CSS, no JavaScript) listing every available download
with system name, OS, architecture, description, filename, and file size.

Usage:
    python3 scripts/generate-download-page.py
    python3 scripts/generate-download-page.py --dist /path/to/dist --output /path/to/index.html
"""

import os
import sys
import hashlib
import re
import argparse
from datetime import date
from html import escape

# ============================================================================
# DEPENDENCY CHECK
# ============================================================================

def check_dependencies():
    """Verify Python version and required modules."""
    if sys.version_info < (3, 6):
        print("ERROR: Python 3.6 or later is required")
        print(f"  Current version: {sys.version}")
        sys.exit(1)
    
    print("All necessary commands are installed")

check_dependencies()

# ---------------------------------------------------------------------------
# Platform catalog
# Keys must match the platform segment in package filenames exactly.
# ---------------------------------------------------------------------------

PLATFORMS = {
    # --- Linux ---------------------------------------------------------------
    "linux": {
        "name": "Linux (x86-64)",
        "os": "Linux (glibc, any distro)",
        "arch": "x86-64",
        "description": (
            "64-bit Linux. Dynamically linked against glibc. "
            "Works on Ubuntu, Debian, Fedora, Arch, and most modern distros."
        ),
        "family": "Linux",
    },
    "linux-x86": {
        "name": "Linux (32-bit x86)",
        "os": "Linux (glibc, any distro)",
        "arch": "x86 32-bit",
        "description": (
            "32-bit Linux build. Runs on 32-bit x86 hosts and under WOW32 on 64-bit Linux."
        ),
        "family": "Linux",
    },
    "linux-arm32": {
        "name": "Linux (ARM 32-bit)",
        "os": "Linux (armhf)",
        "arch": "ARMv7 / armhf",
        "description": (
            "ARM hard-float 32-bit Linux. "
            "Suitable for Raspberry Pi 2/3 running a 32-bit OS, BeagleBone, and similar boards."
        ),
        "family": "Linux",
    },
    "linux-arm64": {
        "name": "Linux (ARM64)",
        "os": "Linux (aarch64)",
        "arch": "AArch64 / ARM64",
        "description": (
            "64-bit ARM Linux. "
            "Raspberry Pi 4/5 (64-bit OS), Apple Silicon in Linux mode, AWS Graviton, and similar."
        ),
        "family": "Linux",
    },
    "linux-ppc": {
        "name": "Linux (PowerPC)",
        "os": "Linux (ppc / ppc64)",
        "arch": "PowerPC 32/64-bit",
        "description": (
            "PowerPC Linux. Old iMac G4/G5, Pegasos, PowerPC servers running Linux."
        ),
        "family": "Linux",
    },
    "musl": {
        "name": "Linux (musl / static)",
        "os": "Linux (musl libc)",
        "arch": "x86-64",
        "description": (
            "Statically linked with musl libc. No glibc dependency — "
            "runs on any 64-bit Linux including Alpine, embedded, and containers without libc installed."
        ),
        "family": "Linux",
    },
    "riscv": {
        "name": "Linux (RISC-V 64)",
        "os": "Linux (riscv64)",
        "arch": "RISC-V 64-bit",
        "description": (
            "64-bit RISC-V Linux. SiFive Freedom boards, StarFive VisionFive, "
            "Kendryte K210, and QEMU RISC-V emulation."
        ),
        "family": "Linux",
    },
    "hurd": {
        "name": "GNU Hurd",
        "os": "GNU Hurd",
        "arch": "x86 / x86-64",
        "description": (
            "GNU Hurd microkernel operating system. Debian GNU/Hurd. "
            "Cross-compiled with GCC targeting i386-gnu or x86_64-gnu."
        ),
        "family": "Linux",
    },
    # --- Windows / DOS -------------------------------------------------------
    "win64": {
        "name": "Windows 64-bit",
        "os": "Windows Vista / 7 / 8 / 10 / 11",
        "arch": "x86-64",
        "description": (
            "64-bit Windows. Includes command-line tool (boltpack.exe) and "
            "Total Commander packer plugin (.wcx64). Built with MinGW-w64."
        ),
        "family": "Windows / DOS",
    },
    "win32": {
        "name": "Windows 32-bit",
        "os": "Windows 95 / 98 / NT / XP / Vista / 7+",
        "arch": "x86 32-bit",
        "description": (
            "32-bit Windows. Runs on both 32-bit and 64-bit Windows under WOW64. "
            "Includes boltpack.exe and Total Commander plugin (.wcx). Built with MinGW-w64 i686."
        ),
        "family": "Windows / DOS",
    },
    "totalcmd-plugin-win32": {
        "name": "Total Commander Plugin (32-bit)",
        "os": "Total Commander 32-bit",
        "arch": "x86 32-bit",
        "description": (
            "Installable Total Commander WCX plugin ZIP for 32-bit Total Commander. "
            "Open the ZIP inside Total Commander to install it. "
            "Includes the BoltPack WCX plugin and boltpack-config.exe."
        ),
        "family": "Windows Plugins",
    },
    "totalcmd-plugin-win64": {
        "name": "Total Commander Plugin (64-bit)",
        "os": "Total Commander 64-bit",
        "arch": "x86-64",
        "description": (
            "Installable Total Commander WCX plugin ZIP for 64-bit Total Commander. "
            "Open the ZIP inside Total Commander to install it. "
            "Includes the BoltPack WCX plugin and boltpack-config.exe."
        ),
        "family": "Windows Plugins",
    },
    "dos": {
        "name": "MS-DOS / FreeDOS",
        "os": "MS-DOS 5+, FreeDOS, DR-DOS",
        "arch": "8086+ with DPMI (32-bit flat)",
        "description": (
            "DOS binary built with DJGPP. Requires a DPMI host at run time "
            "(CWSDPMI.EXE, Windows DOS box, or DOSBox). "
            "Large memory model; supports files up to 2 GB on FAT16 volumes."
        ),
        "family": "Windows / DOS",
    },
    "os2": {
        "name": "OS/2 / ArcaOS",
        "os": "OS/2 Warp 4, eComStation, ArcaOS 5.x",
        "arch": "x86 32-bit",
        "description": (
            "IBM OS/2 and its descendants. Built with OpenWatcom v2 targeting OS/2 v2."
        ),
        "family": "Windows / DOS",
    },
    # --- macOS ---------------------------------------------------------------
    "macosx": {
        "name": "macOS (Intel)",
        "os": "macOS 10.6 Snow Leopard through macOS 15",
        "arch": "x86-64",
        "description": (
            "Intel macOS binary. Runs natively on Intel Macs (10.6+) and "
            "under Rosetta 2 on Apple Silicon."
        ),
        "family": "macOS",
    },
    "macos9": {
        "name": "Classic Mac OS",
        "os": "Mac OS 7 through Mac OS 9",
        "arch": "Motorola 68k",
        "description": (
            "Classic Mac OS 68k binary built with the Retro68 cross-compiler. "
            "Runs on Mac 512k, Plus, SE, Classic, LC, Quadra, and Centris. "
            "Requires System 7 or later."
        ),
        "family": "macOS",
    },
    "source-macosx-ppc": {
        "name": "macOS PowerPC Compile Package (CLI only)",
        "os": "Mac OS X 10.4 / 10.5",
        "arch": "PowerPC",
        "description": (
            "Source compile package for native G4/G5 builds. "
            "Includes root-level build-notes.md, the shared macOS native build helper, and main docs."
        ),
        "family": "Compile Packages",
    },
    "source-macosx-legacy-gui": {
        "name": "macOS Legacy Cocoa GUI Compile Package",
        "os": "Mac OS X 10.5 on PowerPC, 10.5-10.8 on Intel",
        "arch": "PowerPC / x86",
        "description": (
            "Source compile package for the separate pre-10.9 Cocoa app line. "
            "Includes root-level build-notes.md, legacy Cocoa GUI sources, the shared Cocoa implementation, and the macOS build helper."
        ),
        "family": "Compile Packages",
    },
    "source-macosx-intel": {
        "name": "macOS Intel Compile Package (CLI + modern Cocoa GUI)",
        "os": "Mac OS X / macOS",
        "arch": "x86-64",
        "description": (
            "Source compile package for native Intel Mac builds. "
            "Includes root-level build-notes.md, CLI and modern 10.9+ Cocoa GUI sources, and the shared macOS helper."
        ),
        "family": "Compile Packages",
    },
    "source-macosx-arm64": {
        "name": "macOS Apple Silicon Compile Package (CLI + modern Cocoa GUI)",
        "os": "macOS 11+",
        "arch": "ARM64",
        "description": (
            "Source compile package for native Apple Silicon builds. "
            "Includes root-level build-notes.md, CLI and modern 10.9+ Cocoa GUI sources, and the shared macOS helper."
        ),
        "family": "Compile Packages",
    },
    # --- Amiga / 68k ---------------------------------------------------------
    "amiga": {
        "name": "Classic Amiga (AmigaOS 2/3)",
        "os": "AmigaOS 2.x / 3.x",
        "arch": "Motorola 68k",
        "description": (
            "Amiga 500, 1200, 4000 and compatibles running AmigaOS 2.x or 3.x. "
            "Built with bebbo's amiga-gcc cross-compiler. "
            "Package includes the command-line tool, the GadTools GUI build, "
            "and Workbench .info icons for both executables, "
            "packed as an LHA archive."
        ),
        "family": "Amiga / 68k",
    },
    "amigaos4": {
        "name": "AmigaOS 4",
        "os": "AmigaOS 4.x",
        "arch": "PowerPC",
        "description": (
            "AmigaOne, Sam440/460, X5000, and PowerPC hardware running AmigaOS 4.x. "
            "Built with adtools ppc-amigaos-gcc."
        ),
        "family": "Amiga / 68k",
    },
    "aros": {
        "name": "AROS Research OS",
        "os": "AROS",
        "arch": "x86 / x86-64 / ARM",
        "description": (
            "AROS — open-source AmigaOS-compatible operating system. "
            "Built with the AROS SDK cross-compiler."
        ),
        "family": "Amiga / 68k",
    },
    "morphos": {
        "name": "MorphOS",
        "os": "MorphOS 3.x",
        "arch": "PowerPC",
        "description": (
            "MorphOS on PowerPC hardware (Pegasos II, efika MX, Mac mini G4, PowerBook). "
            "Built with the MorphOS SDK ppc-morphos-gcc."
        ),
        "family": "Amiga / 68k",
    },
    "atari": {
        "name": "Atari TOS / MiNT",
        "os": "Atari TOS, FreeMiNT",
        "arch": "Motorola 68k",
        "description": (
            "Atari ST, STE, TT, and Falcon running TOS or FreeMiNT. "
            "Built with m68k-atari-mint-gcc (Riviere toolchain). "
            "Package includes the command-line .TOS build and the GEM GUI .PRG build."
        ),
        "family": "Amiga / 68k",
    },
    "x68k": {
        "name": "Sharp X68000",
        "os": "Human68k",
        "arch": "Motorola 68k",
        "description": (
            "Sharp X68000 personal computer running Human68k OS. "
            "Built with m68k-x680x-human-gcc."
        ),
        "family": "Amiga / 68k",
    },
    "ql": {
        "name": "Sinclair QL",
        "os": "QDOS / Minerva / SMSQ/E",
        "arch": "Motorola 68008",
        "description": (
            "Sinclair QL, Q68, and compatible hardware running QDOS, Minerva, or SMSQ/E. "
            "Built with qcc or m68k-qdos-gcc from the QDOS toolchain."
        ),
        "family": "Amiga / 68k",
    },
    # --- Unix / BSD ----------------------------------------------------------
    "haiku": {
        "name": "Haiku / BeOS",
        "os": "Haiku R1 beta, BeOS",
        "arch": "x86-64",
        "description": (
            "Haiku open-source OS, spiritual successor to BeOS. "
            "Cross-compiled from Linux with x86_64-unknown-haiku-gcc."
        ),
        "family": "Unix / BSD",
    },
    "freebsd": {
        "name": "FreeBSD",
        "os": "FreeBSD 12+",
        "arch": "x86-64",
        "description": (
            "FreeBSD 12 and later. "
            "Cross-compiled with Clang targeting x86_64-unknown-freebsd."
        ),
        "family": "Unix / BSD",
    },
    "openbsd": {
        "name": "OpenBSD",
        "os": "OpenBSD 7.x",
        "arch": "x86-64",
        "description": (
            "OpenBSD. Cross-compilation is not practical; "
            "built natively on an OpenBSD host."
        ),
        "family": "Unix / BSD",
    },
    "netbsd": {
        "name": "NetBSD",
        "os": "NetBSD 9+",
        "arch": "x86-64",
        "description": (
            "NetBSD. Cross-compiled via the NetBSD build.sh cross-compilation framework."
        ),
        "family": "Unix / BSD",
    },
    "solaris": {
        "name": "Oracle Solaris",
        "os": "Solaris 10 / 11",
        "arch": "x86-64",
        "description": (
            "Oracle Solaris 10 or 11. "
            "Built with GCC targeting x86_64-pc-solaris2.11."
        ),
        "family": "Unix / BSD",
    },
    "illumos": {
        "name": "illumos (OpenIndiana / OmniOS)",
        "os": "illumos",
        "arch": "x86-64",
        "description": (
            "illumos-based distributions including OpenIndiana, OmniOS CE, and SmartOS. "
            "Built with GCC targeting x86_64-unknown-illumos."
        ),
        "family": "Unix / BSD",
    },
    "irix": {
        "name": "SGI IRIX",
        "os": "IRIX 6.5",
        "arch": "MIPS",
        "description": (
            "SGI IRIX 6.5 on MIPS hardware. "
            "Native build on SGI hardware with MIPSPro 7.4 or GCC 4.7."
        ),
        "family": "Unix / BSD",
    },
    "hpux": {
        "name": "HP-UX",
        "os": "HP-UX 11i",
        "arch": "PA-RISC / IA-64",
        "description": (
            "HP-UX 11i v1/v2/v3 on PA-RISC or Itanium hardware. "
            "Native build with GCC or HP aCC."
        ),
        "family": "Unix / BSD",
    },
    "aix": {
        "name": "IBM AIX",
        "os": "AIX 7.x",
        "arch": "PowerPC 64-bit",
        "description": (
            "IBM AIX on POWER servers. "
            "Native build with GCC from the AIX Toolbox for Open Source, or IBM XL C."
        ),
        "family": "Unix / BSD",
    },
    "source-openbsd": {
        "name": "OpenBSD Compile Package",
        "os": "OpenBSD",
        "arch": "x86-64",
        "description": (
            "Source compile package for native OpenBSD builds. "
            "Includes root-level build-notes.md, the shared native-Unix build helper, and main docs."
        ),
        "family": "Compile Packages",
    },
    "source-solaris": {
        "name": "Solaris Compile Package",
        "os": "Solaris 10 / 11",
        "arch": "x86-64",
        "description": (
            "Source compile package for native Solaris builds. "
            "Includes root-level build-notes.md, the shared native-Unix build helper, and main docs."
        ),
        "family": "Compile Packages",
    },
    "source-irix": {
        "name": "IRIX Compile Package",
        "os": "IRIX 6.5",
        "arch": "MIPS",
        "description": (
            "Source compile package for native IRIX builds. "
            "Includes root-level build-notes.md, the shared native-Unix build helper, and main docs."
        ),
        "family": "Compile Packages",
    },
    "source-hpux": {
        "name": "HP-UX Compile Package",
        "os": "HP-UX 11i",
        "arch": "PA-RISC / IA-64",
        "description": (
            "Source compile package for native HP-UX builds. "
            "Includes root-level build-notes.md, the shared native-Unix build helper, and main docs."
        ),
        "family": "Compile Packages",
    },
    "source-aix": {
        "name": "AIX Compile Package",
        "os": "AIX 7.x",
        "arch": "PowerPC 64-bit",
        "description": (
            "Source compile package for native AIX builds. "
            "Includes root-level build-notes.md, the shared native-Unix build helper, and main docs."
        ),
        "family": "Compile Packages",
    },
    "source-amigaos4": {
        "name": "AmigaOS 4 Compile Package",
        "os": "AmigaOS 4.x",
        "arch": "PowerPC",
        "description": (
            "Source compile package for AmigaOS 4.x SDK builds. "
            "Includes root-level build-notes.md, the shared special-SDK helper, Amiga wrapper sources, and main docs."
        ),
        "family": "Compile Packages",
    },
    "source-morphos": {
        "name": "MorphOS Compile Package",
        "os": "MorphOS 3.x",
        "arch": "PowerPC",
        "description": (
            "Source compile package for MorphOS SDK builds. "
            "Includes root-level build-notes.md, the shared special-SDK helper, Amiga wrapper sources, and main docs."
        ),
        "family": "Compile Packages",
    },
    "source-aros": {
        "name": "AROS Compile Package",
        "os": "AROS",
        "arch": "x86-64",
        "description": (
            "Source compile package for AROS SDK builds. "
            "Includes root-level build-notes.md, the shared special-SDK helper, Amiga wrapper sources, and main docs."
        ),
        "family": "Compile Packages",
    },
    "source-riscos": {
        "name": "RISC OS Compile Package",
        "os": "RISC OS 3+",
        "arch": "ARM",
        "description": (
            "Source compile package for GCCSDK or native RISC OS builds. "
            "Includes root-level build-notes.md, the shared special-SDK helper, and main docs."
        ),
        "family": "Compile Packages",
    },
    "source-ql": {
        "name": "Sinclair QL Compile Package",
        "os": "QDOS / SMSQ/E",
        "arch": "Motorola 68008",
        "description": (
            "Source compile package for QL SDK builds. "
            "Includes root-level build-notes.md, the shared special-SDK helper, and main docs."
        ),
        "family": "Compile Packages",
    },
    "source-x68k": {
        "name": "Sharp X68000 Compile Package",
        "os": "Human68k",
        "arch": "Motorola 68k",
        "description": (
            "Source compile package for Human68k SDK builds. "
            "Includes root-level build-notes.md, the shared special-SDK helper, and main docs."
        ),
        "family": "Compile Packages",
    },
    "tru64": {
        "name": "Tru64 UNIX (OSF/1)",
        "os": "Tru64 UNIX 5.x",
        "arch": "Alpha AXP",
        "description": (
            "Compaq / HP Tru64 UNIX on Alpha AXP hardware. "
            "Native build with GCC port or Compaq aCC."
        ),
        "family": "Unix / BSD",
    },
    "sco": {
        "name": "SCO OpenServer / UnixWare",
        "os": "SCO OpenServer 6, UnixWare 7",
        "arch": "x86 32-bit",
        "description": (
            "SCO OpenServer 6 or UnixWare 7. "
            "Native build with GCC (old) or SCO cc."
        ),
        "family": "Unix / BSD",
    },
    "nextstep": {
        "name": "NeXTSTEP / OPENSTEP",
        "os": "NeXTSTEP 3.x, OPENSTEP 4.x",
        "arch": "68k / x86 / SPARC / PA-RISC",
        "description": (
            "NeXT workstations and OPENSTEP ports. "
            "Native build with legacy GCC and NeXT developer tools."
        ),
        "family": "Unix / BSD",
    },
    "qnx": {
        "name": "QNX Neutrino",
        "os": "QNX Neutrino RTOS 7.x",
        "arch": "x86 / x86-64 / ARM",
        "description": (
            "QNX Neutrino real-time OS. "
            "Native build with qcc on a QNX development host."
        ),
        "family": "Unix / BSD",
    },
    "plan9": {
        "name": "Plan 9",
        "os": "Plan 9 from Bell Labs / 9front",
        "arch": "x86 / ARM / MIPS / PowerPC",
        "description": (
            "Plan 9 from Bell Labs and the 9front fork. "
            "Native build with 9c or pcc on a Plan 9 host."
        ),
        "family": "Unix / BSD",
    },
    "zos": {
        "name": "IBM z/OS (Mainframe)",
        "os": "IBM z/OS",
        "arch": "z/Architecture (S/390 64-bit)",
        "description": (
            "IBM mainframe z/OS. EBCDIC character set. "
            "Native build with IBM Open XL C/C++ on a z/OS host."
        ),
        "family": "Unix / BSD",
    },
    # --- Other ---------------------------------------------------------------
    "wasm": {
        "name": "WebAssembly",
        "os": "Node.js / Browser",
        "arch": "WebAssembly",
        "description": (
            "Runs in Node.js or any WebAssembly-capable browser. "
            "Built with Emscripten (emcc). "
            "Browser use requires MEMFS/IDBFS filesystem mounting."
        ),
        "family": "Other",
    },
    "riscos": {
        "name": "RISC OS",
        "os": "RISC OS 5.x",
        "arch": "ARM",
        "description": (
            "RISC OS on ARM hardware. Raspberry Pi with RISC OS, RiscPC, Iyonix. "
            "Built with GCCSDK (arm-unknown-riscos-gcc). "
            "Executable filetype must be set to 0xFF8 (Absolute)."
        ),
        "family": "Other",
    },
    "cpm86": {
        "name": "CP/M-86",
        "os": "CP/M-86",
        "arch": "8086",
        "description": (
            "CP/M-86 on early 8086 hardware. "
            "Built with Aztec C 86 or Turbo C."
        ),
        "family": "Other",
    },
}

# Display order for the family sections
FAMILY_ORDER = [
    "Linux",
    "Windows / DOS",
    "Windows Plugins",
    "macOS",
    "Compile Packages",
    "Amiga / 68k",
    "Unix / BSD",
    "Other",
]

# Known package extensions, longest first so .tar.gz is matched before .tar
EXTENSIONS = (".tar.gz", ".tar.bz2", ".tar.xz", ".tar.Z", ".zip", ".lha", ".tar")

# Known variant suffixes that can trail the cpu field
VARIANT_LABELS = {
    "uqlx": "UQLX container for emulator",
}


def format_size(n_bytes):
    """Return a human-readable file size string."""
    for unit in ("B", "KB", "MB", "GB"):
        if n_bytes < 1024:
            return "%d %s" % (n_bytes, unit)
        n_bytes //= 1024
    return "%d GB" % n_bytes


def strip_extension(filename):
    """Strip the package extension and return (stem, ext)."""
    for ext in EXTENSIONS:
        if filename.endswith(ext):
            return filename[: -len(ext)], ext
    return filename, ""


def parse_package_name(filename):
    """Parse a package filename into components.

    Expected format: boltpack-{VERSION}-{PLATFORM}[-{CPU}][.ext]
    Returns a dict with keys: version, platform, cpu, variant, ext, raw
    or None if the filename does not match.
    """
    stem, ext = strip_extension(filename)

    if not stem.startswith("boltpack-"):
        return None

    # Everything after "boltpack-"
    rest = stem[len("boltpack-"):]

    # First token before '-' is the version
    dash = rest.find("-")
    if dash < 0:
        return None
    version = rest[:dash]
    if not version:
        # Empty version (e.g. boltpack--riscos-ARM.zip)
        return None
    after_version = rest[dash + 1:]  # e.g. "amiga-m68k" or "linux-arm64-ARM"

    # Match the longest platform key that is a prefix of after_version
    # followed by either end-of-string or '-'.
    sorted_keys = sorted(PLATFORMS.keys(), key=len, reverse=True)
    matched_platform = None
    cpu_part = ""
    for key in sorted_keys:
        if after_version == key:
            matched_platform = key
            cpu_part = ""
            break
        if after_version.startswith(key + "-"):
            matched_platform = key
            cpu_part = after_version[len(key) + 1:]
            break

    if matched_platform is None:
        return None

    # Detect known variant suffix in cpu_part (e.g. "M68K-uqlx")
    variant = None
    cpu = cpu_part
    for v in VARIANT_LABELS:
        suffix = "-" + v
        if cpu_part.endswith(suffix):
            variant = v
            cpu = cpu_part[: -len(suffix)]
            break

    return {
        "version": version,
        "platform": matched_platform,
        "cpu": cpu,
        "variant": variant,
        "ext": ext,
        "raw": filename,
    }


def scan_dist(dist_dir):
    """Scan dist_dir and return a list of parsed package dicts, sorted by family then name."""
    packages = []
    skipped = []

    try:
        entries = sorted(os.listdir(dist_dir))
    except OSError as exc:
        print("ERROR: Cannot read dist directory: %s" % exc, file=sys.stderr)
        sys.exit(1)

    for entry in entries:
        if entry == "index.html":
            continue
        full = os.path.join(dist_dir, entry)
        if not os.path.isfile(full):
            continue

        info = parse_package_name(entry)
        if info is None:
            skipped.append(entry)
            continue

        info["size"] = os.path.getsize(full)
        
        # Calculate SHA1 checksum
        sha1_hash = hashlib.sha1()
        try:
            with open(full, "rb") as f:
                for chunk in iter(lambda: f.read(8192), b""):
                    sha1_hash.update(chunk)
            info["sha1"] = sha1_hash.hexdigest()
        except OSError:
            info["sha1"] = ""
        
        packages.append(info)

    if skipped:
        for name in skipped:
            print("WARNING: skipping unrecognised file: %s" % name, file=sys.stderr)

    # Sort: family order first, then platform name, then variant
    family_index = {f: i for i, f in enumerate(FAMILY_ORDER)}

    def sort_key(p):
        plat = PLATFORMS.get(p["platform"], {})
        fam = plat.get("family", "Other")
        return (family_index.get(fam, 99), plat.get("name", p["platform"]), p["variant"] or "")

    packages.sort(key=sort_key)
    return packages


def html_escape(s):
    return escape(s, quote=True)


def render_html(packages, version_str):
    """Return the complete HTML page as a string."""
    today = date.today().strftime("%Y-%m-%d")
    total = len(packages)

    # Group by family
    groups = {}
    for p in packages:
        plat = PLATFORMS.get(p["platform"], {})
        fam = plat.get("family", "Other")
        groups.setdefault(fam, []).append(p)

    lines = []

    lines.append("<!DOCTYPE html>")
    lines.append('<html lang="en">')
    lines.append("<head>")
    lines.append('<meta charset="UTF-8">')
    lines.append("<title>BoltPack Downloads</title>")
    lines.append("</head>")
    lines.append("<body>")
    lines.append("<h1>BoltPack Downloads</h1>")
    lines.append("<p>")
    if version_str:
        lines.append("  Version: <strong>%s</strong><br>" % html_escape(version_str))
    lines.append("  Generated: %s<br>" % html_escape(today))
    lines.append("  %d package(s) available." % total)
    lines.append("</p>")
    lines.append("<hr>")

    for family in FAMILY_ORDER:
        if family not in groups:
            continue

        lines.append("<h2>%s</h2>" % html_escape(family))
        lines.append(
            '<table border="1" cellpadding="6" cellspacing="0">'
        )
        lines.append("<thead>")
        lines.append("<tr>")
        lines.append("  <th>System</th>")
        lines.append("  <th>Operating System</th>")
        lines.append("  <th>Architecture</th>")
        lines.append("  <th>Description</th>")
        lines.append("  <th>Download</th>")
        lines.append("  <th>Size</th>")
        lines.append("</tr>")
        lines.append("</thead>")
        lines.append("<tbody>")

        for p in groups[family]:
            plat = PLATFORMS.get(p["platform"], {})
            system_name = plat.get("name", p["platform"])
            os_name     = plat.get("os", "")
            arch        = plat.get("arch", p["cpu"])
            desc        = plat.get("description", "")

            # Append variant note if present
            if p["variant"] and p["variant"] in VARIANT_LABELS:
                system_name = "%s (%s)" % (system_name, VARIANT_LABELS[p["variant"]])

            size_str = format_size(p["size"])
            filename = p["raw"]

            lines.append("<tr>")
            lines.append("  <td>%s</td>" % html_escape(system_name))
            lines.append("  <td>%s</td>" % html_escape(os_name))
            lines.append("  <td>%s</td>" % html_escape(arch))
            lines.append("  <td>%s</td>" % html_escape(desc))
            lines.append(
                '  <td><a href="%s">%s</a></td>'
                % (html_escape(filename), html_escape(filename))
            )
            lines.append("  <td>%s</td>" % html_escape(size_str))
            lines.append("</tr>")

        lines.append("</tbody>")
        lines.append("</table>")
        lines.append("<br>")

    lines.append("<hr>")
    lines.append("<h2>SHA1 Checksums</h2>")
    lines.append("<pre>")
    for p in packages:
        filename = p["raw"]
        sha1 = p.get("sha1", "")
        if sha1:
            lines.append("%s  %s" % (sha1, html_escape(filename)))
        else:
            lines.append("(no checksum)  %s" % html_escape(filename))
    lines.append("</pre>")
    
    lines.append("<p><small>Page generated %s.</small></p>" % html_escape(today))
    lines.append("</body>")
    lines.append("</html>")

    return "\n".join(lines) + "\n"


def detect_version(dist_dir):
    """Return the newest version represented by package filenames in dist/."""
    try:
        versions = []
        for entry in os.listdir(dist_dir):
            info = parse_package_name(entry)
            if info and info["version"]:
                versions.append(info["version"])
        if versions:
            return max(set(versions), key=lambda v: tuple(
                int(part) for part in re.findall(r"\d+", v)
            ))
    except OSError:
        pass
    return ""


def main():
    parser = argparse.ArgumentParser(
        description="Generate a plain-HTML download page for BoltPack dist packages."
    )
    default_dist = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dist"
    )
    parser.add_argument(
        "--dist",
        default=default_dist,
        metavar="DIR",
        help="Path to the dist directory (default: %s)" % default_dist,
    )
    parser.add_argument(
        "--output",
        default=None,
        metavar="FILE",
        help="Output HTML file (default: <dist>/index.html)",
    )
    args = parser.parse_args()

    dist_dir = os.path.abspath(args.dist)
    output_file = args.output if args.output else os.path.join(dist_dir, "index.html")

    if not os.path.isdir(dist_dir):
        print("ERROR: dist directory not found: %s" % dist_dir, file=sys.stderr)
        sys.exit(1)

    packages = scan_dist(dist_dir)
    version_str = detect_version(dist_dir)

    if not packages:
        print("WARNING: no recognised packages found in %s" % dist_dir, file=sys.stderr)

    html = render_html(packages, version_str)

    with open(output_file, "w", encoding="utf-8") as f:
        f.write(html)

    print("Written: %s (%d package(s))" % (output_file, len(packages)))


if __name__ == "__main__":
    main()
