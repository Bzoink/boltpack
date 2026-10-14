# BoltPack Portability Guide

BoltPack enforces **strict ANSI C (C89/C90)** across the entire project —
compression layer, archiver core, and CLI frontend. This is a deliberate design
constraint, not a fallback. By restricting the codebase to the lowest common
denominator of C compilers, BoltPack can be compiled on virtually any system
with a C89 compiler and a standard C library.

This document covers portability constraints, supported environments, and the
toolchain landscape. Native per-platform build procedures live in `BUILD.md`.

## Target Environments

| System                                                 | CPU                        | OS                  | Compiler                                                |
| ------------------------------------------------------ | -------------------------- | ------------------- | ------------------------------------------------------- |
| Linux (x86_64/i686/ARM/ARM64/MIPS/PowerPC/RISC-V/S390) | multi                      | Linux (any)         | GCC, Clang, cross-toolchains                            |
| Modern Linux (x86)                                     | x86 / x64                  | Linux (any)         | GCC, Clang                                              |
| musl Linux                                             | x86 / x64                  | Linux (musl)        | musl-gcc                                                |
| RISC-V Linux                                           | RISC-V                     | Linux (riscv64)     | GCC (riscv64-unknown-linux-gnu)                         |
| ARM Linux                                              | ARMv7                      | Linux (armhf)       | arm-linux-gnueabi-gcc                                   |
| ARM64 Linux                                            | aarch64                    | Linux (arm64)       | aarch64-linux-gnu-gcc                                   |
| NeXTSTEP / OPENSTEP                                    | 68k / x86 / PPC / SPARC    | NeXTSTEP / OPENSTEP | GCC (old ports) / NeXT dev tools                        |
| QNX Neutrino                                           | x86 / x86_64 / ARM         | QNX                 | qcc (Watcom) / GCC ports                                |
| Plan 9                                                 | x86 / ARM / MIPS / PowerPC | Plan 9              | 9c / pcc                                                |
| Classic Mac OS                                         | 68k                        | Mac OS 7–9          | CodeWarrior 8.x / MPW (C89)                             |
| macOS Intel                                            | x86_64                     | macOS 10.6–12       | Clang/GCC                                               |
| macOS Apple Silicon                                    | ARM64                      | macOS 11+           | Clang / Rosetta for Intel binaries                      |
| Classic Amiga                                          | 68k                        | AmigaOS 2.x/3.x     | SAS/C, VBCC, GCC                                        |
| Atari TOS/MiNT                                         | 68k                        | Atari TOS / MiNT    | m68k-atari-mint-gcc                                     |
| AROS                                                   | x86_64                     | AROS                | aros-x86_64-gcc (AROS SDK)                              |
| MorphOS                                                | PPC                        | MorphOS             | ppc-morphos-gcc (MorphOS SDK)                           |
| RISC OS                                                | ARM                        | RISC OS             | GCCSDK (arm-unknown-riscos)                             |
| BeOS / Haiku                                           | x86 / x64                  | BeOS / Haiku        | GCC (Haiku pkgsrc/cross)                                |
| FreeBSD                                                | x86_64                     | FreeBSD             | Clang --target=x86_64-unknown-freebsd14                 |
| NetBSD                                                 | x86_64                     | NetBSD              | Clang --target=x86_64--netbsd                           |
| OpenBSD                                                | x86_64                     | OpenBSD             | Clang --target=x86_64--openbsd                          |
| Solaris/Illumos                                        | x86_64                     | Solaris / Illumos   | GCC (x86_64-pc-solaris2.11)                             |
| SPARC Solaris                                          | SPARC                      | Solaris 2.11        | GCC (sparc-sun-solaris2.11)                             |
| illumos                                                | x86_64                     | illumos             | GCC (x86_64-unknown-illumos)                            |
| IRIX                                                   | MIPS                       | IRIX                | GCC (mips-sgi-irix6.5)                                  |
| HP-UX                                                  | PA-RISC/IA64               | HP-UX               | GCC (hppa-hp-hpux or ia64-hp-hpux)                      |
| AIX                                                    | PowerPC                    | AIX                 | GCC (powerpc-ibm-aix)                                   |
| e2k (Elbrus)                                           | e2k                        | Linux (e2k)         | GCC (e2k-unknown-linux)                                 |
| Tru64 UNIX (OSF/1)                                     | Alpha                      | Tru64 UNIX          | GCC ports / aCC                                         |
| SCO OpenServer / UnixWare                              | x86                        | SCO UNIX            | GCC (old) / cc                                          |
| Hurd (GNU)                                             | x86 / x86_64               | GNU Hurd            | GCC (hurd ports)                                        |
| OpenVMS                                                | VAX/Alpha/Itanium/x86_64   | OpenVMS             | Native VSI C / DEC C; vax-dec-vms GCC exists (emulated) |
| z/OS (Mainframe)                                       | z/Architecture             | IBM z/OS            | IBM Open XL C/C++ (EBCDIC charset, generally native)    |
| OS/2                                                   | x86 (16/32)                | OS/2 Warp/ArcaOS    | Open Watcom v2 / os2-gcc                                |
| Sinclair QL / X68000                                   | 68k                        | QDOS / Human68k     | GCC (m68k-elf / m68k-x68k-human)                        |
| CP/M-86                                                | 8086                       | CP/M-86             | Aztec C / Turbo C (?)                                   |
| MS-DOS                                                 | 8086+                      | DOS                 | Turbo C, DJGPP, Watcom                                  |
| WebAssembly                                            | x86_64/RISCV?              | Node / Browser      | Emscripten (`emcc`)                                     |
| Windows 9x/NT                                          | x86                        | Win95/98/NT4/2000   | MSVC 6, Watcom, MinGW                                   |
| Windows GUI (FLTK) 32-bit                              | x86                        | Windows XP+         | MinGW-w64 (i686) + FLTK 1.4 cross-compiled              |
| Windows GUI (FLTK) 64-bit                              | x86_64                     | Windows Vista+      | MinGW-w64 (x86_64) + FLTK 1.4 cross-compiled            |
| Linux GUI (FLTK) x86-64                                | x86_64                     | Linux (any)         | GCC + FLTK 1.4 (system package)                         |

## Practical Optimization Tiers

The core codec (`src/core/fluff.c`, `compress.c`) is written in portable C89. Frontends use the language supported by their platform implementation, including C++, Objective-C, and C# where applicable.
Performance-sensitive paths can be improved without abandoning portability by selecting wider word operations at compile time.
The tiers below map CPU families to what is safely available in a C89 translation unit.
Tier 1 (8-bit: Z80, 6502) is omitted — those platforms are too memory-constrained for the full window codec.

### Tier overview

| Tier | Representative CPUs                                       | Key properties                                                      | Safe optimization in `src/core/`                                               |
| ---- | --------------------------------------------------------- | ------------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| 2    | 68k, SPARC, MIPS (BE), PA-RISC, Alpha                     | Big- or mixed-endian; trap on unaligned access; 32-bit registers    | 32-bit word loads via `memcpy` into `unsigned int`; must byte-swap on BE       |
| 3    | x86, ARM 32 (LE), PowerPC 32, x86-64 in 32-bit mode       | Mostly little-endian; hardware handles unaligned; hardware multiply | Same `memcpy`-word-load pattern; unaligned access works but `memcpy` is safest |
| 4    | x86-64, ARM64, RISC-V 64, PPC64, SPARC64, z/Arch, Itanium | 64-bit registers; `unsigned long` = 64 bits (LP64)                  | 64-bit bit-reader accumulator; halves `flr_fill` call frequency                |

### Hot-path capabilities by tier

| Hot path         | Tier 2                                                  | Tier 3                                             | Tier 4                                                  |
| ---------------- | ------------------------------------------------------- | -------------------------------------------------- | ------------------------------------------------------- |
| Bit reader fill  | 4-byte `memcpy` load; byte-swap on big-endian platforms | Same — `memcpy` is guaranteed safe on all Tier 3   | 8-byte `memcpy` load; fills twice as many bits per call |
| Bit writer drain | 4-byte accumulator; drain when full                     | Same                                               | 8-byte accumulator; drains half as often                |
| Hash 4-byte load | `memcpy` into `unsigned int`; byte-swap on BE           | Same                                               | Same — hash is already 32-bit; no change needed         |
| Match comparison | `fl_byte_at()` per byte (history safety required)       | In-block fast path: `memcmp` on `cur_src` directly | Same as Tier 3                                          |

### Compile-time tier detection

The C89-legal macro `#if (ULONG_MAX > 0xFFFFFFFFUL)` selects the Tier 4 path when `unsigned long` is 64 bits (LP64).
It is already used in `bp_types.h` for `bp_off_t` and may be reused for any Tier 4 code path.

```c
#if (ULONG_MAX > 0xFFFFFFFFUL)
    /* Tier 4: 64-bit accumulator available */
#else
    /* Tier 2/3: 32-bit accumulator */
#endif
```

No runtime detection is needed — the compiler resolves the branch at build time on every supported platform.

## Coding Constraints

### Language: ANSI C (C89/C90)

All code in the **compression** and **archiver** layers must compile as strict C89. This means:

- **No C99 features**: no `//` comments, no mixed declarations and code, no `<stdint.h>`, no `<stdbool.h>`, no variable-length arrays, no designated initializers.
- **No `long long`**: use `unsigned long` (at least 32 bits per C89). BoltLZ uses 32-bit block lengths with a maximum of 256 KiB.
- **Declare all variables at the top** of each block.
- **No compiler extensions** (`__attribute__`, `__declspec`, `#pragma` packing) in core code. Use these only in platform-specific frontend code, guarded by `#ifdef`.

**Exception:** `bp_off_t` in `bp_types.h` uses `__int64` (MSVC) or `__extension__ long long` (GCC/Clang) on 32-bit hosts to support files larger than 2 GB. On LP64 hosts, `long` is already 64 bits. BoltLZ also uses guarded compiler intrinsics with portable fallbacks. The offset type is justified by the impossibility of expressing a 64-bit signed offset in portable C89 on ILP32 platforms.

**Third-party C99 exemption:** `lib/zstd/` is compiled as C99 (or later)
because that library uses features unavailable in C89. It is isolated behind a
thin C89-compatible wrapper (`src/core/zstd_wrap.h` / `src/core/zstd_wrap.c`).
BoltLZ in `src/core/boltlz.c` is an original C89 implementation and compiles with the same
`-ansi -pedantic` flags as the core. All code in `src/core/` and
`src/frontend/` must remain strict C89.

**Availability of method 4 (zstd):** The zstd codec (`lib/zstd/`) is linked only
when the `BOLTPACK_ZSTD` preprocessor flag is set. Modern targets (Linux, macOS,
Windows 32/64-bit, ARM64, RISC-V) enable it by default. Vintage/constrained
targets (DOS 16-bit, Win16, Amiga 68 K, Atari ST, Classic Mac 68 K) do **not**
define `BOLTPACK_ZSTD` and will return `BP_ERR_UNSUPPORTED_METHOD` when
encountering a method-4 archive.

### Integer sizes

Do not assume sizes beyond the C89 guarantees:

| Type            | Minimum guaranteed |
| --------------- | ------------------ |
| `char`          | 8 bits             |
| `short`         | 16 bits            |
| `int`           | 16 bits            |
| `long`          | 32 bits            |
| `unsigned long` | 32 bits            |

For the archive format, use explicit-width types in a portability header:

```c
/* bp_types.h — portable fixed-width types for BoltPack */
typedef unsigned char      bp_uint8;
typedef unsigned short     bp_uint16;   /* assumes short >= 16 bits (always true) */
typedef unsigned int       bp_uint32;   /* assumes int >= 16 bits; usually 32 bits on modern platforms */

/* 64-bit value as a pair of 32-bit halves (C89 has no uint64_t / long long) */
typedef struct {
    bp_uint32 lo;
    bp_uint32 hi;
} bp_uint64;
```

### Byte order

The archive uses little-endian storage. Provide read/write helpers:

```c
static void bp_write_le16(unsigned char *p, bp_uint16 v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void bp_write_le32(unsigned char *p, bp_uint32 v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void bp_write_le64(unsigned char *p, bp_uint64 v) {
    bp_write_le32(p, v.lo);
    bp_write_le32(p + 4, v.hi);
}
```

Corresponding `bp_read_le16`, `bp_read_le32`, `bp_read_le64` helpers reverse the process.

These work correctly on both big-endian (PowerPC, 68k) and little-endian (x86) hosts with no platform-specific code.

### File I/O

- The archiver core **never** calls `fopen`, `fread`, etc. directly. It uses the `bp_io_t` callback interface (see [Architecture](architecture.md)).
- The CLI frontend provides `bp_io_t` implementations using `<stdio.h>` (`fopen`, `fread`, `fwrite`, `fseek`, `ftell`).
- On classic Mac OS, a different frontend would use `FSOpen`, `FSRead`, `FSWrite`, etc.
- On Amiga, a frontend would use `Open()`, `Read()`, `Write()` from `dos.library`.

### Memory allocation

- Use `malloc` / `free` from `<stdlib.h>` (available on all targets).
- Keep allocations small and predictable. Full builds use a 32/64 KiB BoltLZ compression workspace; mini builds compress with BoltLite and 16-bit mini builds link BoltLZ decoding only.
- Avoid deep recursion — stack sizes on vintage systems may be small (as low as 4–8 KB on some platforms).

### String handling

- Use standard `<string.h>` functions: `memcpy`, `memset`, `memcmp`, `strlen`, `strncpy`.
- boltlz provides its own portable byte-by-byte copy path as a fallback.
- File names in the archive are UTF-8. Frontends convert to/from native encoding (e.g., MacRoman on classic Mac OS).

## Build Procedure Boundaries

This guide intentionally avoids repeating the native build cookbook. Use the
following companion documents and entry points instead:

- `BUILD.md` for native per-platform build procedures and prerequisites
- `build.sh <target>` or `make <target>` for the main workspace entry points
- `scripts/create-source-kits.sh` for native compile packages that are no
  longer part of the aggregate Linux-hosted build path
- `scripts/setup-*.sh` for host-side cross-toolchain setup helpers
- `../dev/README.md` for Docker-based build environments

## Cross-Compilation Reference

BoltPack targets 32 OS families. Many can be cross-compiled from a modern Linux host using GCC-based toolchains. The table below maps each `creator_os` target to its cross-compilation status and recommended tools.

### Linux-hosted build paths

```text
creator_os  Target                Linux build path      Cross toolchain                     Recommended native compiler
----------  --------------------  --------------------  ----------------------------------  ---------------------------------
0x01        Linux                 Native                —                                   GCC, Clang
0x02        Mac OS classic (1–9)  Yes (68k only)        Retro68                             CodeWarrior Pro, MPW
0x03        Mac OS X / macOS      Compile package       osxcross (secondary path)           Xcode (Clang), GCC
0x04        AmigaOS 1.x–3.x       Yes                   amiga-gcc (bebbo's fork)            SAS/C 6.x, VBCC
0x05        AmigaOS 4.x           Compile package       adtools (ppc-amigaos)               GCC (native on OS4)
0x06        MorphOS               Compile package       MorphOS SDK (ppc-morphos)           GCC (native on MorphOS)
0x07        AROS                  Compile package       AROS SDK (aros-x86_64)              GCC (native on AROS)
0x08        MS-DOS                Yes                   DJGPP cross (build-djgpp)           Turbo C, Watcom, DJGPP
0x09        Windows               Yes                   mingw-w64 (x86_64-w64-mingw32)      MSVC, OpenWatcom, MinGW
0x0A        FreeBSD               Yes                   FreeBSD cross-tools or Clang target GCC, Clang (native)
0x0B        NetBSD                Yes                   NetBSD build.sh (cross framework)   GCC (native)
0x0C        OpenBSD               Compile package       —                                   GCC, Clang (native)
0x0D        Solaris / SunOS       Compile package       —                                   GCC, Sun Studio
0x0E        IRIX                  Compile package       —                                   MIPSPro, GCC (native)
0x0F        HP-UX                 Compile package       —                                   GCC, HP aCC (native)
0x10        AIX                   Compile package       —                                   GCC, IBM XL C (native)
0x11        Atari TOS / MiNT      Yes                   m68k-atari-mint-gcc (Riviere)       Pure C, Lattice C, GCC under MiNT
0x12        RISC OS               Compile package       GCCSDK (arm-unknown-riscos)         GCC (native), Norcroft
0x13        OS/2                  Partial               OpenWatcom (cross-capable)          GCC/kLIBC, OpenWatcom (native)
0x14        BeOS / Haiku          Yes                   Haiku cross-tools                   GCC (native on Haiku)
0x15        OpenVMS               No                    —                                   DEC/VSI C (native)
0x16        z/OS                  No                    —                                   IBM Open XL C (native, EBCDIC)
0x17        WebAssembly           Yes                   Emscripten (emcc)                   —
0x18        NeXTSTEP / OPENSTEP   No (practical)        —                                   GCC ports / NeXT dev tools
0x19        QNX Neutrino          No (practical)        —                                   qcc (QNX host)
0x1A        Plan 9                No                    —                                   9c / pcc (native)
0x1B        Tru64 UNIX            No                    —                                   GCC ports / aCC (native)
0x1C        SCO OpenServer        No                    —                                   GCC (old) / cc (native)
0x1D        CP/M-86               No                    —                                   Aztec C / Turbo C (DOSBox/native)
0x1E        Sinclair QL / X68000  Compile package       qcc / m68k-x680x-human-gcc          GCC (cross/emulated)
0x20        GNU Hurd              Yes                   GCC (i386-gnu or x86_64-gnu cross)  GCC (native on Hurd)
```

**Summary:** 17 of 32 targets still have some viable Linux-hosted cross-compilation path, and OS/2 has partial cross support via OpenWatcom. The aggregate `build.sh all` workflow is intentionally narrower: compile packages are now preferred for macOS, OpenBSD, Solaris, IRIX, HP-UX, AIX, AmigaOS 4.x, MorphOS, AROS, RISC OS, Sinclair QL, and Sharp X68000. The remaining native-only systems (NeXTSTEP, QNX, Plan 9, Tru64, SCO, OpenVMS, z/OS, CP/M-86, and similar) are still documented as native builds or emulator-assisted builds.

### Build Entry Points

After the required toolchain is installed, use `build.sh <target>` or the
matching `make <target>` rule from the project root for the regular buildable
targets. For systems that have been moved out of the aggregate workflow,
prefer `scripts/create-source-kits.sh` and the bundled family helper scripts
instead of `build.sh all`. Direct expert `build.sh <target>` commands remain
available for those systems. The exact build procedures, prerequisite
packages, and platform-specific caveats live in `BUILD.md`; this section stays
limited to which cross paths are realistic.

When cross-compiling, validate the produced binary under an emulator, VM, or
real hardware.

### Cross-toolchain details

#### Retro68 — Classic Mac OS 68k

Cross-compiles C/C++ for 68k Macs (System 6 through Mac OS 9 on 68k). Does **not** target PowerPC classic Mac OS — CodeWarrior remains the only practical option for PPC Mac OS 9.

```sh
# Build Retro68 from source (requires CMake, Boost)
git clone https://github.com/autc04/Retro68.git
cd Retro68 && mkdir build && cd build
../build-toolchain.sh
# Produces m68k-apple-macos-gcc
```

Outputs 68k Mach-O or CODE resources. The Mac frontend would need significant adaptation (Toolbox calls, resource fork I/O). BoltPack's C89 core compiles without changes.

#### amiga-gcc — AmigaOS 68k (bebbo's fork)

The most actively maintained 68k AmigaOS cross-compiler. Includes newlib, clib2, and full AmigaOS NDK headers.

```sh
# Build from source
git clone https://github.com/bebbo/amiga-gcc.git
cd amiga-gcc && make all
# Produces m68k-amigaos-gcc
```

```sh
m68k-amigaos-gcc -noixemul -ansi -pedantic -Wall -O2 \
    -Isrc/core  \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/amiga/main.c src/core/boltlz.c \
    -o boltpack
```

`-noixemul` links against native AmigaOS clib instead of ixemul.library (Unix emulation layer).

#### adtools — AmigaOS 4 PowerPC

Targets AmigaOS 4.x on PowerPC hardware (AmigaOne, Sam440/460, X5000).

```sh
# Clone and build following adtools instructions
git clone https://github.com/sba1/adtools.git
# Produces ppc-amigaos-gcc
```

#### MorphOS SDK

The MorphOS SDK is distributed by the MorphOS team. Includes a ppc-morphos GCC cross-compiler.

```sh
# Download from https://www.morphos-team.net/
# Unpack and set PATH to SDK/bin
ppc-morphos-gcc -noixemul -ansi -pedantic -Wall -O2 \
    -Isrc/core  \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/morphos/main.c src/core/boltlz.c \
    -o boltpack
```

#### AROS SDK

AROS provides hosted (Linux-based) and native cross-compilation.

```sh
# AROS build system generates the cross-compiler
git clone https://github.com/aros-development-team/AROS.git
cd AROS && ./configure --target=pc-i386
make crosstools
# Produces i386-aros-gcc (or m68k-aros-gcc, etc.)
```

#### DJGPP cross — MS-DOS

andrewwutw's build-djgpp provides a modern cross-DJGPP toolchain for Linux.

```sh
# Automated build script
git clone https://github.com/andrewwutw/build-djgpp.git
cd build-djgpp && ./build-djgpp.sh 12.2.0
# Produces i586-pc-msdosdjgpp-gcc
```

```sh
i586-pc-msdosdjgpp-gcc -ansi -pedantic -Wall -O2 \
    -Isrc/core  \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/dos/main.c src/core/boltlz.c \
    -o boltpack.exe
```

Requires a DPMI host at runtime (CWSDPMI or Windows DOS box). Supports 32-bit flat memory — no 64 KB segment issues.

#### mingw-w64 — Windows

Available as a standard package on most Linux distributions.

```sh
# Debian/Ubuntu
sudo apt install gcc-mingw-w64-x86-64
# Fedora
sudo dnf install mingw64-gcc
```

```sh
x86_64-w64-mingw32-gcc -ansi -pedantic -Wall -O2 \
    -Isrc/core  \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/win/main.c src/core/boltlz.c \
    -o boltpack.exe
```

For 32-bit Windows (9x/NT/2000), use `i686-w64-mingw32-gcc` instead.

#### m68k-atari-mint-gcc — Atari TOS / MiNT

Vincent Rivière's cross-toolchain is the standard for Atari development from Linux.

```sh
# Pre-built binaries or build from source
# https://vincent.riviere.free.fr/soft/m68k-atari-mint/
# Produces m68k-atari-mint-gcc
```

```sh
m68k-atari-mint-gcc -ansi -pedantic -Wall -O2 \
    -Isrc/core  \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/atari/main.c src/core/boltlz.c \
    -o boltpack.tos
```

BoltPack now ships the Atari command-line frontend as `.TOS`, while the GEM GUI build is packaged as `.PRG`.

#### GCCSDK — RISC OS

The GCCSDK project provides an ARM cross-compiler targeting RISC OS from Linux.

```sh
# Build the cross-compiler
svn co svn://svn.riscos.info/gccsdk/trunk/ gccsdk
cd gccsdk/gcc4 && ./build-world
# Produces arm-unknown-riscos-gcc
```

```sh
arm-unknown-riscos-gcc -ansi -pedantic -Wall -O2 \
    -Isrc/core  \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/riscos/main.c src/core/boltlz.c \
    -o boltpack
```

The GCCSDK linker produces RISC OS ELF or AIF executables. Filetype must be set to `0xFF8` (Absolute) for the Filer to recognise it as a program.

#### osxcross — macOS

Cross-compiles for macOS from Linux using a packaged macOS SDK.

```sh
# Requires an Xcode .xip or SDK extracted from macOS
git clone https://github.com/tpoechtrager/osxcross.git
cd osxcross && ./build.sh
# Produces o64-clang / x86_64-apple-darwin-gcc
```

For PowerPC Mac OS X (10.2–10.5), you need an older SDK (10.4u or 10.5) and `powerpc-apple-darwin` target. In practice this is fragile — native compilation on a PowerPC Mac is more reliable.

#### Haiku cross-tools

Haiku's build system can bootstrap a cross-compiler.

```sh
git clone https://review.haiku-os.org/haiku
git clone https://review.haiku-os.org/buildtools
cd haiku && ./configure --cross-tools-source ../buildtools --build-cross-tools x86_64
# Produces x86_64-unknown-haiku-gcc
```

#### OpenWatcom — OS/2 (and DOS/Windows)

Not GCC-based, but OpenWatcom is the most practical cross-compiler for OS/2. Also targets DOS (16-bit and 32-bit) and Windows from Linux.

```sh
# Pre-built from https://github.com/open-watcom/open-watcom-v2
# Set WATCOM environment variable
export WATCOM=/opt/watcom
export PATH=$WATCOM/binl64:$PATH
```

```sh
wcl386 -bt=os2v2 -ansi -ox \
    -Isrc/core  \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/os2/main.c src/core/boltlz.c \
    -fe=boltpack.exe
```

### Targets requiring native builds

These systems lack a practical GCC cross-compiler from Linux. Build natively:

| Target          | Recommended compiler    | Notes                                                                                              |
| --------------- | ----------------------- | -------------------------------------------------------------------------------------------------- |
| OpenBSD         | GCC or Clang (in base)  | Cross-build not officially supported; build in an OpenBSD VM                                       |
| Solaris / SunOS | GCC or Sun Studio       | pkgutil or IPS provides GCC; use a Solaris VM or zone                                              |
| IRIX            | MIPSPro (native) or GCC | GCC 4.7 is the last version known to work on IRIX; MIPSPro 7.4 preferred. No practical cross path. |
| HP-UX           | GCC or HP aCC           | HP ships Bundled GCC with HP-UX 11i; depots from HP-UX Porting Centre                              |
| AIX             | GCC or IBM XL C         | GCC from AIX Toolbox for Open Source (perzl.org archive or IBM RPMs)                               |

For these targets, BoltPack's C89 strict core ensures the code compiles with minimal adjustments — the same source files, just a different `cc` command.

## 16-Bit Systems (8086/8088)

The 8086/8088 running MS-DOS is a supported target. BoltPack's C89 core compiles on 16-bit compilers (Turbo C, Microsoft C, Watcom) with the following considerations:

### What works without changes

- `bp_uint32` (`unsigned long`) — 32 bits on all 16-bit C89 compilers
- `bp_uint64` as `{lo, hi}` struct — just two `unsigned long` values
- All LE read/write helpers — operate at the byte level, no width assumptions
- boltlz — decompression uses byte indices and unsigned lengths that fit 16-bit `int` for the ≤32 KB mini blocks

### Memory model

The compiler must be set to **compact**, **large**, or **huge** memory model (far pointers for data). boltlz decompression needs no work buffer, so block-size + output buffers are the only data requirements.

| Memory model | Code pointers | Data pointers | Max single allocation | Suitable? |
| ------------ | ------------- | ------------- | --------------------- | --------- |
| Small        | 16-bit (near) | 16-bit (near) | 64 KB                 | No        |
| Medium       | 32-bit (far)  | 16-bit (near) | 64 KB                 | No        |
| Compact      | 16-bit (near) | 32-bit (far)  | 64 KB per allocation  | Marginal  |
| Large        | 32-bit (far)  | 32-bit (far)  | 64 KB per allocation  | **Yes**   |
| Huge         | 32-bit (far)  | 32-bit (far)  | >64 KB                | **Yes**   |

With large model and 8 KB blocks, total memory for compression is ~81 KB — fits easily in a 640 KB conventional memory system (~500 KB free after DOS).

### `int` is 16 bits

On 16-bit compilers, `sizeof(int)` is 2. Code must never use `int` for values that can exceed 32,767 (signed) or 65,535 (unsigned). BoltPack's convention of using `bp_uint32` (`unsigned long`) for all sizes and offsets avoids this. The error code enum uses negative values that fit in 16-bit `int` (down to -15).

### File I/O limits

`fseek()`/`ftell()` use `long` (32-bit) — max ~2 GB. This is not a practical constraint on DOS, where FAT16 files are limited to 2 GB and typical disks were 20–500 MB.

### Performance

An 8088 at 4.77 MHz running boltlz decompression will be slow but functional. boltlz's byte-level operations (no bit shifting, no Huffman) make it significantly faster than DEFLATE on hardware without a barrel shifter. Expect decompression throughput in the low KB/s range — useful for floppy-sized archives, not hard disk images.

## 8-Bit Systems — Not Supported

8-bit systems (6502-based: C64, Apple II, Atari 800; Z80-based: ZX Spectrum, CP/M; 6809-based: CoCo) are **not** BoltPack targets. The constraints are fundamental:

- **64 KB total address space** — even the mini build's program, BoltLite workspace, I/O buffers, and stack exceed practical 8-bit memory budgets.
- **No conformant C89 compilers** — cc65 (6502) is close but has `int` = 16 bits, `long` = 32 bits via software emulation at dozens of cycles per operation. SDCC (Z80) has similar limitations.
- **Arithmetic speed** — 32-bit `unsigned long` operations on a 1 MHz 8-bit CPU take hundreds of cycles per addition. Processing archive headers would be painfully slow.
- **No realistic file I/O** — most 8-bit systems lack a hierarchical filesystem. Disk I/O is often sector-level.

The `creator_cpu` table includes `6502/65C816` and `Z80` for identification purposes only (the `65C816` is a 16-bit extension used in the Apple IIgs and SNES, not in scope either). These values exist so that if someone puts a `.bolt` file on a disk alongside 8-bit software, the creator field doesn't misidentify the origin.

## Testing Portability

Before claiming a platform is supported, verify:

1. boltlz compress/decompress round-trips a known test block
2. Compress and decompress a known test block, verify checksum
3. Create a small archive on one platform, extract on another — byte-identical results
