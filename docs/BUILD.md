# BoltPack Native Build Guide

BoltPack has native frontends for dozens of operating systems and CPU families.
This guide covers repository build targets and the source kits used when a
native toolchain is required.

## Quick Start

### Linux build

From the full repository checkout, the default `make` target builds the Linux
CLI. Select a named target (for example `make arm64` or `make haiku`) for a
cross-build when its toolchain is installed.

```bash
tar xzf boltpack-source.tar.gz
cd boltpack
make
```

### OpenMP-Enabled Parallel Compression Builds

On Linux hosts with OpenMP available, these targets enable the optional
parallel compression paths. The `BOLTPACK_FAST_FLUFF` build switch controls
parallel Fluff and BoltLZ; supported CLI builds can also parallelize independent
zstd files:

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

`make modern` builds the Linux x86-64 CLI with OpenMP enabled. `make arm64`
does the same for the dedicated Linux ARM64 CLI build. On supported Linux and
Windows CLI builds, BoltLZ, Fluff, and zstd automatically use multiple workers
for sufficiently large workloads. `--single-thread` forces serial compression;
`boltpack help advanced` lists `-j N` for the worker count and `-g N` for Fluff
grouping.

`make win32` and `make win64` build the Windows CLI with the same OpenMP
support. Builds without OpenMP keep compression serial and omit threading
controls from advanced CLI help.

The Linux/Windows FLTK, macOS Cocoa, and Haiku GUI targets surface parallel
Fluff as a `Use Multi-threaded Fluff` toggle. Their OpenMP builds also provide
the parallel BoltLZ path where enabled by the frontend. The macOS GUI target
requires an osxcross or native macOS toolchain with OpenMP support, and
`make haiku-gui` still requires the Haiku cross SDK.

For the generic Makefile build, set `ENABLE_FAST_FLUFF=0` to omit the C11/
OpenMP encoder and `ENABLE_ZSTD=0` to omit the C99 zstd library. These switches
do not apply to target-specific recipes that explicitly set their feature
flags; check the target recipe when building outside the generic Linux CLI
target.

### Other platforms

Platform-specific targets and native build instructions are listed in
[`frontends.md`](frontends.md) and [`portability.md`](portability.md). No
`build/Makefile.*` files are shipped for Plan 9, z/OS, or OpenVMS.

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

The modern Intel and Apple Silicon Cocoa GUI builds compile
`src/core/fluff_fast.c` with
`-DBOLTPACK_FAST_FLUFF -fopenmp`, so the `Use Multi-threaded Fluff` toggle is
available when the host toolchain provides OpenMP. The same OpenMP build also
enables parallel BoltLZ in frontends that set its worker policy.

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

The package includes `src/core`, the PowerPC frontend
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
```

Run each command on its matching operating system. The helper refuses to use
a host compiler to produce an executable labeled for another Unix system.

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

### POSIX Frontends

The root Makefile does not detect the host OS. Select the frontend explicitly with `FRONTEND` and set its output directory:

#### OpenBSD (x86_64)

**Prerequisites:**

- Clang or GCC (built-in, no installation needed)

**Build:**

```bash
cd boltpack
make FRONTEND=openbsd BUILDDIR=build/openbsd TARGET=build/openbsd/boltpack
./build/openbsd/boltpack --help
```

**Install manually:**

```bash
sudo install -m 755 build/openbsd/boltpack /usr/local/bin/boltpack
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
make FRONTEND=solaris BUILDDIR=build/solaris TARGET=build/solaris/boltpack
./build/solaris/boltpack --help
```

**For SPARC Solaris:** set `CC` to the SPARC cross-compiler explicitly; the build does not detect the host architecture.

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
make FRONTEND=irix BUILDDIR=build/irix TARGET=build/irix/boltpack
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
make FRONTEND=hpux BUILDDIR=build/hpux TARGET=build/hpux/boltpack
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
make FRONTEND=aix BUILDDIR=build/aix TARGET=build/aix/boltpack
./build/aix/boltpack --help
```

---

#### Tru64 UNIX (OSF/1) (Alpha)

**Prerequisites:**

- GCC ports (if available on your system)

**Build:**

```bash
cd boltpack
make FRONTEND=tru64 BUILDDIR=build/tru64 TARGET=build/tru64/boltpack
./build/tru64/boltpack --help
```

---

#### SCO OpenServer / UnixWare (x86)

**Prerequisites:**

- GCC or native cc

**Build:**

```bash
cd boltpack
make FRONTEND=sco BUILDDIR=build/sco TARGET=build/sco/boltpack
./build/sco/boltpack --help
```

---

#### NeXTSTEP / OPENSTEP (68k / x86 / PPC / SPARC)

**Prerequisites:**

- GCC (old ports may be available; NeXT dev tools if not)

**Build:**

```bash
cd boltpack
make FRONTEND=nextstep BUILDDIR=build/nextstep TARGET=build/nextstep/boltpack
./build/nextstep/boltpack --help
```

---

#### QNX Neutrino (x86 / x86_64 / ARM)

**Prerequisites:**

- qcc (Watcom) or GCC ports

**Build:**

```bash
cd boltpack
make FRONTEND=qnx BUILDDIR=build/qnx TARGET=build/qnx/boltpack
./build/qnx/boltpack --help
```

---

#### GNU Hurd (x86 / x86_64)

**Prerequisites:**

- GCC with Hurd support (i386-gnu or x86_64-gnu)

**Build:**

```bash
cd boltpack
make FRONTEND=hurd BUILDDIR=build/hurd TARGET=build/hurd/boltpack
./build/hurd/boltpack --help
```

---

### Platforms without a build target

Plan 9, z/OS, and OpenVMS frontends are present, but this repository does not
ship platform Makefiles or a complete documented manual link command for them.
The build commands in this guide apply only to targets defined by the root
`Makefile`.
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
- Check the target-specific compiler flags in the root `Makefile`

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

| Build request | Command | Result |
| ------------- | ------- | ------ |
| Default host build | `make` | Builds the default Linux CLI target |
| Cross-build | `make <target>` | Builds a named target listed by `make help`; requires its toolchain |

---

## Customization

To override compiler or flags:

```bash
# Use a specific compiler
CC=gcc-9 make

# Add extra flags
CFLAGS="-ansi -pedantic -Wall -Wextra -O3" make

# Build with a selected compiler
CC=clang make
```

---

## Adding a New Platform

To add support for a new platform:

1. Add a target to the root `Makefile` and adjust `CC`, `CFLAGS`, and `LDFLAGS`
3. Create `src/frontend/PLATFORM/main.c` (copy from similar OS, adapt I/O)
4. Add the target to `make help`
5. Document the platform in this file (`docs/BUILD.md`)

---

## For More Help

- Check `docs/portability.md` for detailed info on supported platforms
- Check `docs/architecture.md` for build system overview
- Check `src/frontend/PLATFORM/main.c` to understand platform-specific code
- Run `make help` for summary of all build targets
