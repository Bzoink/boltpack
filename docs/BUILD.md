# BoltPack Native Build Guide

BoltPack can be compiled natively on 13 different operating systems. This source package contains everything needed to build on any of them.

## Quick Start

### For Full Source Trees (Auto-detected)

On OpenBSD, Solaris, IRIX, HP-UX, AIX, Tru64, SCO, NeXTSTEP, QNX, or GNU Hurd:

```bash
tar xzf boltpack-source.tar.gz
cd boltpack
make
```

The Makefile automatically detects your OS and uses the appropriate compiler
when you are building from the full repository checkout.

### Linux-Hosted Fast-Fluff Targets

On Linux hosts with OpenMP available, the repository also provides modern
targets that enable the optional parallel Fluff encoder
(`BOLTPACK_FAST_FLUFF`) for method `3`:

```bash
make modern
make arm64
make win32
make win64
make linux-gui
make win32-fltk-gui
make win64-fltk-gui
make haiku-gui
make macosx-gui
```

`make modern` builds the Linux x86-64 CLI with fast-fluff enabled. `make arm64`
does the same for the dedicated Linux ARM64 CLI build. On those Linux-family
CLI builds, selecting Fluff now auto-enables multi-processor compression for
larger workloads by default; `--single-thread` forces the sequential path, and
`boltpack help advanced` exposes the expert `-j N` / `-g N` overrides.

`make win32` and `make win64` now build the Windows CLI with the same
fast-fluff support. On those Windows CLI builds, selecting Fluff now
auto-enables multi-processor compression for larger workloads by default;
`--single-thread` forces the sequential path, and `boltpack help advanced`
exposes the expert `-j N` / `-g N` overrides.

The Linux/Windows FLTK, macOS Cocoa, and Haiku GUI targets surface the same
feature as a `Use Multi-threaded Fluff` toggle when Fluff is selected. The
macOS GUI target still requires an osxcross or native macOS toolchain with
OpenMP support, and `make haiku-gui` still requires the Haiku cross SDK.

### For Exotic Systems (Explicit Build Required)

For Plan 9, z/OS, and OpenVMS, explicitly specify the Makefile:

```bash
# Plan 9 (uses mk, not make)
mk -f build/Makefile.plan9

# z/OS (uses IBM Open XL C)
make -f build/Makefile.zos

# OpenVMS (uses DEC C / VSI C)
make -f build/Makefile.openvms
```

### For Compile Packages

When a target is not part of the aggregate Linux-hosted build workflow, create
compile packages from the main tree:

```bash
bash scripts/create-source-kits.sh
```

You can also create one family at a time:

```bash
bash scripts/create-source-kits.sh macos
bash scripts/create-source-kits.sh classic-mac
bash scripts/create-source-kits.sh native-unix
bash scripts/create-source-kits.sh special-sdk
```

These packages are emitted directly under `dist/` with names like:

- `dist/boltpack-<version>-source-macosx-ppc.tar.gz`
- `dist/boltpack-<version>-source-macos9-ppc.tar.gz`
- `dist/boltpack-<version>-source-openbsd.tar.gz`

Each compile package includes the matching family helper plus the main
documentation files `docs/BUILD.md`, `docs/frontends.md`, and
`docs/portability.md`. There is no separate platform-specific README.
Depending on the family, the helper lives under `scripts/macos/`,
`scripts/native-unix/`, or `scripts/special-sdk/`. The Classic Mac OS 9 kit
ships with build notes only because the expected workflow is still a native
CodeWarrior project on the target Mac.

#### macOS compile packages

When the practical workflow is to carry source to a Mac and build there
natively, create the target-specific macOS kits from the main tree:

```bash
bash scripts/create-source-kits.sh macos
```

This creates:

- `dist/boltpack-<version>-source-macosx-ppc.tar.gz`
- `dist/boltpack-<version>-source-macosx-legacy-gui.tar.gz`
- `dist/boltpack-<version>-source-macosx-intel.tar.gz`
- `dist/boltpack-<version>-source-macosx-arm64.tar.gz`

Each source kit includes the shared helper `scripts/macos/build-native.sh`, a
root-level `build-notes.md`, and the main documentation files. There is no
separate macOS kit README. The pre-10.9 Cocoa app lives in the separate
`source-macosx-legacy-gui` kit. The modern 10.9+ Cocoa GUI sources remain in
`source-macosx-intel` and `source-macosx-arm64`.

Build on the Mac with:

```sh
sh scripts/macos/build-native.sh ppc
sh scripts/macos/build-native.sh legacy-ppc
sh scripts/macos/build-native.sh legacy-intel
sh scripts/macos/build-native.sh intel
sh scripts/macos/build-native.sh arm64
sh scripts/macos/build-native.sh all
sh scripts/macos/build-native.sh legacy-all
```

Target summary:

- `ppc`: PowerPC Mac OS X 10.4 or 10.5. Produces
  `build/macosx-ppc/boltpack`. CLI only; no Cocoa GUI sources are included in
  that kit.
- `legacy-ppc`: PowerPC Mac OS X 10.5. Produces
  `build/macosx-legacy-gui-ppc/BoltPack.app` from the legacy Cocoa frontend.
- `legacy-intel`: Intel Mac OS X 10.5 through 10.8. Produces
  `build/macosx-legacy-gui-intel/BoltPack.app`. The default build uses `i386`
  so one app can run across the full 10.5-10.8 Intel range.
- `intel`: Intel Mac OS X / macOS 10.9+. Produces
  `build/macosx-intel/boltpack` and `build/macosx-gui-intel/BoltPack.app`. Set
  `BUILD_GUI=0` to skip the modern GUI and build only the CLI.
- `arm64`: Apple Silicon macOS 11+. Produces
  `build/macosx-arm64/boltpack` and `build/macosx-gui-arm64/BoltPack.app`.
- `all`: Builds every target in sequence. This is mainly useful on newer Macs
  with a toolchain that supports the required `-arch` values.
- `legacy-all`: Builds both legacy GUI targets in sequence.

The modern `intel` and `arm64` GUI builds also compile `src/core/fluff_fast.c`
with `-DBOLTPACK_FAST_FLUFF -fopenmp`, so the Cocoa `Use Multi-threaded Fluff`
toggle is active whenever the host toolchain provides OpenMP.

Useful environment overrides:

- `CC` chooses the C compiler for CLI builds.
- `OBJC` chooses the Objective-C compiler for Cocoa GUI builds.
- `CFLAGS` adds extra CLI compiler flags.
- `GUI_CFLAGS` adds extra Cocoa GUI compiler flags.
- `LEGACY_GUI_CFLAGS` adds extra legacy Cocoa GUI compiler flags.
- `LEGACY_INTEL_ARCH` chooses the Intel arch for the legacy GUI build.
- `BUILD_GUI=0` skips the Cocoa GUI on Intel or ARM64 builds.

On older Macs, use the matching native target directly instead of `all`.

#### Classic Mac OS compile packages

Classic Mac OS 9 PowerPC now has a separate native source kit for CodeWarrior
builds inside the target environment:

```bash
bash scripts/create-source-kits.sh classic-mac
```

This creates:

- `dist/boltpack-<version>-source-macos9-ppc.tar.gz`

The package includes `src/core`, `lib/fastlz`, the PowerPC frontend
under `src/frontend/macos9/`, the matching Classic Mac resource source
`src/frontend/macos9/boltpack.r`, and a root-level `build-notes.md` with the
CodeWarrior project settings and library list.

Unpack it on Linux if convenient, then copy the extracted tree to a FAT
transfer image for Mac OS 9. Since the package contains plain source and Rez
files, it does not rely on HFS-specific metadata during transfer.

#### Native-Unix compile packages

OpenBSD, Solaris, IRIX, HP-UX, and AIX now use native compile packages instead
of participating in `./build.sh all`.

Create them with:

```bash
bash scripts/create-source-kits.sh native-unix
```

This creates:

- `dist/boltpack-<version>-source-openbsd.tar.gz`
- `dist/boltpack-<version>-source-solaris.tar.gz`
- `dist/boltpack-<version>-source-irix.tar.gz`
- `dist/boltpack-<version>-source-hpux.tar.gz`
- `dist/boltpack-<version>-source-aix.tar.gz`

Each package also includes a root-level `build-notes.md` with the matching
toolchain requirements and build command.

Build on the target host with:

```sh
sh scripts/native-unix/build-native.sh openbsd
sh scripts/native-unix/build-native.sh solaris
sh scripts/native-unix/build-native.sh irix
sh scripts/native-unix/build-native.sh hpux
sh scripts/native-unix/build-native.sh aix
sh scripts/native-unix/build-native.sh all
```

Useful environment overrides:

- `CC` chooses the C compiler.
- `CFLAGS` adds extra compiler flags.

The default flags are aimed at GCC/Clang-style compilers. On vendor compiler
setups, override both `CC` and `CFLAGS` explicitly.

#### Special-SDK compile packages

AmigaOS 4.x, MorphOS, AROS, RISC OS, Sinclair QL, and Sharp X68000 now use
compile packages instead of participating in `./build.sh all`.

Create them with:

```bash
bash scripts/create-source-kits.sh special-sdk
```

This creates:

- `dist/boltpack-<version>-source-amigaos4.tar.gz`
- `dist/boltpack-<version>-source-morphos.tar.gz`
- `dist/boltpack-<version>-source-aros.tar.gz`
- `dist/boltpack-<version>-source-riscos.tar.gz`
- `dist/boltpack-<version>-source-ql.tar.gz`
- `dist/boltpack-<version>-source-x68k.tar.gz`

Each package also includes a root-level `build-notes.md` with the matching
toolchain requirements and build command.

Build from the extracted kit with:

```sh
sh scripts/special-sdk/build-sdk.sh amigaos4
sh scripts/special-sdk/build-sdk.sh morphos
sh scripts/special-sdk/build-sdk.sh aros
sh scripts/special-sdk/build-sdk.sh riscos
sh scripts/special-sdk/build-sdk.sh ql
sh scripts/special-sdk/build-sdk.sh x68k
sh scripts/special-sdk/build-sdk.sh all
```

Useful environment overrides:

- `CC` chooses the compiler for the selected target.
- `CFLAGS` adds extra compiler flags for the selected target.

The helper follows the current project recipes instead of a simplified generic
front-end build. In particular:

- `amigaos4`, `morphos`, and `aros` kits also include `src/frontend/amiga/`
  because the wrapper `main.c` files include it directly.
- `ql` prefers `qcc` and falls back to `m68k-qdos-gcc` if that is how the SDK
  is exposed.
- `riscos` outputs `build/riscos/boltpack,ff8` and `x68k` outputs
  `build/x68k/boltpack.x`, matching the existing platform conventions.

---

## Detailed Platform Guide

### Auto-Detectable Systems

These platforms have official Makefiles and will compile with a simple `make` command.

#### OpenBSD (x86_64)

**Prerequisites:**

- Clang or GCC (built-in, no installation needed)

**Build:**

```bash
cd boltpack
make
./build/openbsd/boltpack --help
```

**Install:**

```bash
make install
```

---

#### Solaris / Illumos (x86_64, SPARC)

**Prerequisites:**

- GCC (install via pkgutil or IPS)

```bash
pkgutil -i gcc        # or: pkg install developer/gcc
```

**Build:**

```bash
cd boltpack
make
./build/solaris/boltpack --help
```

**For SPARC Solaris:**

- Should auto-detect and use GCC for SPARC architecture

---

#### IRIX (MIPS)

**Prerequisites:**

- GCC cross-compiler for MIPS (mips-sgi-irix6.5)

```bash
# On IRIX natively, or cross from Linux:
# sudo apt install gcc-mips-sgi-irix6
```

**Build:**

```bash
cd boltpack
make
./build/irix/boltpack --help
```

---

#### HP-UX (PA-RISC / IA64)

**Prerequisites:**

- GCC (install from HP Porting Centre)

```bash
# Or from AIX ports if available
```

**Build:**

```bash
cd boltpack
make
./build/hpux/boltpack --help
```

---

#### AIX (PowerPC)

**Prerequisites:**

- GCC (from AIX Toolbox for Open Source)

```bash
rpm -i gcc-*.rpm
```

**Build:**

```bash
cd boltpack
make
./build/aix/boltpack --help
```

---

#### Tru64 UNIX (OSF/1) (Alpha)

**Prerequisites:**

- GCC ports (if available on your system)

**Build:**

```bash
cd boltpack
make
./build/tru64/boltpack --help
```

---

#### SCO OpenServer / UnixWare (x86)

**Prerequisites:**

- GCC or native cc

**Build:**

```bash
cd boltpack
make
./build/sco/boltpack --help
```

---

#### NeXTSTEP / OPENSTEP (68k / x86 / PPC / SPARC)

**Prerequisites:**

- GCC (old ports may be available; NeXT dev tools if not)

**Build:**

```bash
cd boltpack
make
./build/nextstep/boltpack --help
```

---

#### QNX Neutrino (x86 / x86_64 / ARM)

**Prerequisites:**

- qcc (Watcom) or GCC ports

**Build:**

```bash
cd boltpack
make
./build/qnx/boltpack --help
```

---

#### GNU Hurd (x86 / x86_64)

**Prerequisites:**

- GCC with Hurd support (i386-gnu or x86_64-gnu)

**Build:**

```bash
cd boltpack
make
./build/hurd/boltpack --help
```

---

### Exotic Systems (Manual/Explicit Build)

These systems use non-standard build tools and require explicit Makefile specification.

#### Plan 9 (x86 / ARM / MIPS / PowerPC)

**Compiler:** 9c (Plan 9 C compiler) or pcc  
**Build Tool:** mk (Plan 9's make replacement)

**Build on Plan 9:**

```sh
cd /boltpack                    # or appropriate path
mk -f build/Makefile.plan9
```

**Notes:**

- Plan 9 uses `mk`, not `make`
- Makefile syntax differs from POSIX make
- `build/Makefile.plan9` is approximate; may need adjustment
- No standard autodetection (different shell environment)

---

#### z/OS Mainframe (z/Architecture)

**Compiler:** IBM Open XL C/C++  
**Character Encoding:** EBCDIC (native) / ASCII conversion required

**Build on z/OS:**

```plaintext
# Using MMS (Make replacement on z/OS)
mms /DESCRIP=build/Makefile.zos

# Or manual compilation:
xlc -qlanglvl=extended -qansi -O2 -c src/core/archiver.c
xlc -qlanglvl=extended -qansi -O2 -c src/core/fluff.c
# ... (repeat for all source files)
# Then link: ...
```

**Notes:**

- z/OS is EBCDIC-based; ASCII files must be converted
- Requires mainframe-specific `src/frontend/zos/main.c`
- Build on real z/OS host or Hercules emulator
- Binary will not be portable to non-mainframe systems

---

#### OpenVMS (VAX / Alpha / Itanium / x86_64)

**Compiler:** DEC C or VSI C  
**Build Tool:** MMS (OpenVMS Make variant)

**Build on OpenVMS:**

```plaintext
$ MMS /DESCRIP=build:Makefile.openvms

or manually:
$ CC /STANDARD=RELAXED_ANSI /OPTIMIZE=LEVEL=2 src:core:archiver.c
$ CC /STANDARD=RELAXED_ANSI /OPTIMIZE=LEVEL=2 src:core:fluff.c
$ ...
$ LINK archiver.obj, fluff.obj, ... /EXE=boltpack.exe
```

**Notes:**

- OpenVMS uses different file naming conventions: `[DIR.SUBDIR]FILE.EXT`
- Use `/SWITCHES` instead of `-flags`
- Linker command quite different (LINK instead of ld)
- MMS syntax differs from Unix make
- Binary will be OpenVMS-specific

---

## Troubleshooting

### Common Issues

#### "cc: command not found"

- Your system doesn't have a C compiler installed
- Check the Prerequisites section for your OS
- Install GCC or Clang as recommended

#### "Can't find src/core/\*.c"

- Make sure you extracted the source tarball completely
- Check: `ls -la src/core/` should show at least 5+ .c files

#### Compilation errors about ANSI compliance

- Most likely: wrong flags for your compiler
- Try: `CC=gcc make` or `CC=clang make`
- Check the appropriate `build/Makefile.PLATFORM` for correct flags

#### "No such file or directory: src/frontend/PLATFORM/main.c"

- The frontend for your OS may not exist
- Fall back to a generic frontend (if available)
- Or create a simple main.c based on an existing one

#### Linker errors (undefined references)

- Missing system libraries
- On Solaris: add `-lsocket -lnsl` (already in Makefile.solaris)
- On AIX: may need additional AIX-specific libraries
- See `LDFLAGS` in the appropriate Makefile

### Platform-Specific Notes

**OpenBSD:** Uses strict pledge(2) security; some operations may need audit

**Solaris:** May need `/usr/ccs/bin` in PATH for `make` and `ar`

**IRIX:** Some GCC versions have issues with MIPS; try native `cc` if available

**AIX:** Very picky about symbol visibility; may need `#pragma` in frontend code

**z/OS:** Requires EBCDIC conversion utilities; see `src/frontend/zos/main.c`

**OpenVMS:** File naming and linker syntax completely different; expert knowledge needed

---

## Building a Distribution Package

After successful compilation:

```bash
# Copy binary and docs
mkdir -p dist/
cp build/PLATFORM/boltpack dist/
cp docs/README.md dist/
cp docs/format.md dist/

# Create archive
tar czf boltpack-PLATFORM.tar.gz dist/
```

---

## Quick Platform Reference

| Platform    | Auto? | Build Command                    | Binary                       |
| ----------- | ----- | -------------------------------- | ---------------------------- |
| OpenBSD     | ✅    | `make`                           | `build/openbsd/boltpack`     |
| Solaris     | ✅    | `make`                           | `build/solaris/boltpack`     |
| IRIX        | ✅    | `make`                           | `build/irix/boltpack`        |
| HP-UX       | ✅    | `make`                           | `build/hpux/boltpack`        |
| AIX         | ✅    | `make`                           | `build/aix/boltpack`         |
| Tru64       | ✅    | `make`                           | `build/tru64/boltpack`       |
| SCO         | ✅    | `make`                           | `build/sco/boltpack`         |
| NeXTSTEP    | ✅    | `make`                           | `build/nextstep/boltpack`    |
| QNX         | ✅    | `make`                           | `build/qnx/boltpack`         |
| GNU Hurd    | ✅    | `make`                           | `build/hurd/boltpack`        |
| **Plan 9**  | ❌    | `mk -f build/Makefile.plan9`     | `build/plan9/boltpack`       |
| **z/OS**    | ❌    | `make -f build/Makefile.zos`     | `build/zos/boltpack`         |
| **OpenVMS** | ❌    | `make -f build/Makefile.openvms` | `build/openvms/boltpack.exe` |

---

## Customization

To override compiler or flags:

```bash
# Use a specific compiler
CC=gcc-9 make

# Add extra flags
CFLAGS="-ansi -pedantic -Wall -Wextra -O3" make

# For explicit builds
CC=clang make -f build/Makefile.openbsd
```

---

## Adding a New Platform

To add support for a new platform:

1. Create `build/Makefile.PLATFORM` (copy from `build/Makefile.generic` or existing platform)
2. Adjust `CC`, `CFLAGS`, `LDFLAGS` for your platform
3. Create `src/frontend/PLATFORM/main.c` (copy from similar OS, adapt I/O)
4. Update `Makefile.native` to add auto-detection (if `uname -s` gives a recognisable string)
5. Document the platform in this file (`docs/BUILD.md`)

---

## For More Help

- Check `docs/portability.md` for detailed info on supported platforms
- Check `docs/architecture.md` for build system overview
- Check `src/frontend/PLATFORM/main.c` to understand platform-specific code
- Run `make help` for summary of all build targets
