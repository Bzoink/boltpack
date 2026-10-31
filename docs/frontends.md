# BoltPack Frontends

BoltPack separates platform-specific code from the archive core. Each **frontend** adapts the archiver to a specific operating system by implementing file I/O callbacks (`bp_io_t`), directory recursion, text encoding conversion, and platform metadata handling. The archiver core never touches the file system directly — all platform interaction goes through the frontend.

This document covers all frontends: their design, platform-specific concerns, and build instructions.

## Frontend Responsibilities

Every frontend must:

1. **Implement `bp_io_t` callbacks** — wrap the platform's file read/write/seek/tell into the abstract I/O interface
2. **Recurse directories** — walk directory trees and feed entries to the archiver one at a time (depth-first: directory entry first, then children)
3. **Handle extraction output** — implement `bp_extract_fn` and `bp_extract_done_fn` callbacks to create files, directories, and apply metadata
4. **Convert text encoding** — translate file names between the platform's native encoding and UTF-8 (the archive's canonical encoding)
5. **Preserve platform metadata** — archive Mac resource forks, Unix permissions, Amiga protection bits, etc. as typed entries; skip foreign metadata gracefully on extraction
6. **Report progress** — implement `bp_progress_fn` to display operation status (CLI output, GUI progress bar, etc.)
7. **Handle errors** — translate `BP_ERR_*` codes into user-visible messages

### Compression Method Conventions

The current CLI frontends follow one method-selection convention unless a
platform explicitly documents a smaller subset:

- no compression flag: boltlz default
- `-0`: stored
- `-1`: BoltLite
- `-F`: Fluff

Mini frontends keep the same numbering but only expose the methods they ship.

## Source Layout

```text
src/frontend/
    linux/main.c        Linux CLI (POSIX, x86-64)   — also base for arm, arm64, riscv, musl, wasm, e2k
    win/main.c          Windows CLI (MinGW, win32/win64)
    dos/main.c          MS-DOS CLI (DJGPP / Turbo C)
    cpm86/main.c        CP/M-86 CLI mini (Aztec C 86 / Turbo C, BPMINI.CMD)
    os2/main.c          OS/2 CLI (Open Watcom / GCC+EMX)
    amiga/main.c        AmigaOS 2.x/3.x CLI (m68k-amigaos-gcc)
    amigaos4/main.c     AmigaOS 4.x CLI (ppc-amigaos-gcc)             — thin Amiga wrapper
    morphos/main.c      MorphOS CLI (ppc-morphos-gcc)                  — thin Amiga wrapper
    aros/main.c         AROS CLI (aros-x86_64-gcc)                     — thin Amiga wrapper
    atari/main.c        Atari TOS/MiNT CLI (m68k-atari-mint-gcc, .TOS)
    haiku/main.c        Haiku / BeOS CLI (x86_64-unknown-haiku-gcc)
    riscos/main.c       RISC OS CLI (arm-unknown-riscos-gcc, ,ff8)
    macos9/main.c       Mac OS 9 GUI (CodeWarrior Pro 8, PPC)
    macosx/main.c       macOS / Mac OS X CLI (Apple Clang)
    plan9/main.c        Plan 9 CLI (8c/6c/5c, Plan 9 native)
    ql/main.c           Sinclair QL CLI (m68k-qdos-gcc, QDOS)
    x68k/main.c         Sharp X68000 CLI (m68k-x680x-human-gcc, Human68k)
    freebsd/main.c      FreeBSD CLI                                    — thin POSIX wrapper
    netbsd/main.c       NetBSD CLI                                     — thin POSIX wrapper
    openbsd/main.c      OpenBSD CLI                                    — thin POSIX wrapper
    solaris/main.c      Solaris CLI                                    — thin POSIX wrapper
    illumos/main.c      illumos CLI                                    — thin POSIX wrapper
    hpux/main.c         HP-UX CLI                                      — thin POSIX wrapper
    aix/main.c          AIX CLI                                        — thin POSIX wrapper
    irix/main.c         IRIX CLI                                       — thin POSIX wrapper
    hurd/main.c         GNU Hurd CLI                                   — thin POSIX wrapper
    tru64/main.c        Tru64 UNIX CLI                                 — thin POSIX wrapper
    sco/main.c          SCO OpenServer CLI                             — thin POSIX wrapper
    qnx/main.c          QNX Neutrino CLI                               — thin POSIX wrapper
    nextstep/main.c     NeXTSTEP / OPENSTEP CLI                       — thin POSIX wrapper
    zos/main.c          z/OS (USS) CLI                                 — thin POSIX wrapper
    gui_common/         Shared GUI constants and help text
        help_text.h     App name, version, copyright, help text
    linux_gui/main.cpp  Linux GUI (FLTK 1.3, C++)
    win_gui_fltk/main.cpp Windows NT GUI (FLTK 1.4, C++; UTF-16 filesystem APIs)
    win_gui/            Windows NT GUI (Win32 API, C89; Unicode filesystem paths)
        main.c          Main window, menus, ListView, drag-and-drop
        resource.h      Resource identifiers
        resource.rc     Menus, dialogs, accelerators, version info
    win_gui_net/        Windows .NET GUI (C# WinForms, .NET 4.8)
        MainForm.cs     Main window, menus, ListView, drag-and-drop
        NativeMethods.cs P/Invoke declarations for boltpack_core.dll
        ProgressForm.cs Progress dialog
        AboutForm.cs    About dialog
        DocForm.cs      Documentation viewer
        Program.cs      Entry point
        BoltPack.csproj .NET project file
    macosx_gui/main.m   macOS GUI shared implementation (Cocoa AppKit, Objective-C)
    macosx_legacy_gui/main.m
              macOS legacy GUI wrapper (Cocoa AppKit, Objective-C)
    atari_gui/main.c    Atari ST GUI (GEM/AES + VDI, C89)
    amiga_gui/main.c    AmigaOS GUI (GadTools + ASL, C89)
    haiku_gui/main.cpp  Haiku GUI (BeAPI, C++)
```

CLI frontends are each a single `.c` file that includes `core/archiver.h` and `core/compress.h`.
GUI frontends may span multiple files and use platform-native toolkits.

---

## Linux Frontend

**File:** `src/frontend/linux/main.c`
**Compiler:** GCC or Clang
**Interface:** Command-line

### Build

```sh
make FRONTEND=linux
```

Or directly:

```sh
gcc -ansi -pedantic -Wall -Wextra -O2 \
     \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c \
    src/frontend/linux/main.c src/core/boltlz.c \
    -o boltpack
```

### bp_io_t Implementation

Wraps standard C `<stdio.h>` functions:

| Callback | Implementation              |
| -------- | --------------------------- |
| `read`   | `fread(buf, 1, count, fp)`  |
| `write`  | `fwrite(buf, 1, count, fp)` |
| `seek`   | `fseek(fp, offset, origin)` |
| `tell`   | `ftell(fp)`                 |

The `handle` field points to a `FILE *`.

### Directory Recursion

Uses POSIX `opendir()` / `readdir()` / `closedir()` with `stat()` for file metadata. Depth-first walk: emits `BP_TYPE_DIR` for each directory, then recurses into contents. Symlinks are archived as `BP_TYPE_SYMLINK` entries with the link target stored as the entry's data.

### Platform Metadata

- Archives Unix permissions in the `platform` field with `BP_OS_LINUX`
- Sets `mtime` from `stat.st_mtime`
- On extraction, restores permissions with `chmod()` and modification time with `utime()`

Before creating the extraction root, Linux scans the file table and rejects
unsafe or overlong components, duplicate projected paths, and file/directory
prefix conflicts. The preflight is bounded to 1,048,576 names and 64 MiB of
path keys. Extraction then uses directory-relative no-follow operations and
stages regular files beside their destinations.

### Text Encoding

Assumes the host uses UTF-8 natively (standard on modern Linux). No conversion needed.

### Linux: Commands

| Command         | Description                      |
| --------------- | -------------------------------- |
| `create` / `c`  | Create archive (recursive)       |
| `add` / `a`     | Add files to existing archive    |
| `extract` / `x` | Extract archive                  |
| `list` / `l`    | List archive contents            |
| `test` / `t`    | Test archive integrity           |
| `info` / `i`    | Show archive info                |
| `delete` / `d`  | Delete files from archive        |
| `help`          | Show the default simplified help |

### Common Options

| Flag              | Description                                   |
| ----------------- | --------------------------------------------- |
| `-b SIZE`         | Block size: `8k`, `16k`, `32k`, `64k`, `256k` |
| `-0`              | Store only (no compression)                   |
| `-F`              | Use Fluff LZ77+FSE/tANS compression           |
| `-o PATH`         | Extract to directory                          |
| `-f`              | Force overwrite                               |
| `-n`              | Dry run                                       |
| `-q` / `-qq`      | Quiet / silent                                |
| `--single-thread` | Force sequential compression on modern builds  |
| `--interchange-profile-1` | Create or append using mini-reader limits: BoltLite, 16 KiB blocks, 65,535 entries, 4 GiB minus one byte per entry, and 2 GiB minus one byte total |

The profile option is valid only for `create` and `add`. Append checks the
existing archive against the profile before modifying it. It refuses codec,
block-size, or level overrides that conflict with the selected profile. It
uses BoltLite by default; add `-0` to select Stored.

### Advanced Help

The default `boltpack help` page hides expert tuning and debug switches.
Run `boltpack help advanced` to show the full Linux-family CLI surface,
including `debug-fluff` / `T`, compression-method tuning, and the explicit
parallel-compression controls below.

| Flag   | Description                                                                               |
| ------ | ----------------------------------------------------------------------------------------- |
| `-1`   | Use BoltLite compression                                                                      |
| `-2`   | Use boltlz compression explicitly (the default method)                                    |
| `-l N`, `--level N` | Exact level: BoltLZ 1-2; Fluff/zstd 1-9                                     |
| `--light` | Fast preset: BoltLZ 1; Fluff/zstd 1                                                   |
| `--standard` | Default preset: BoltLZ 2; Fluff 8; zstd 3                                          |
| `--best` | Highest preset: BoltLZ 2; Fluff/zstd 9                                                 |
| `-j N` | Use up to `N` worker threads for BoltLZ, Fluff, or zstd (OpenMP CLI builds only)            |
| `-g N` | Group `N` Fluff blocks per shared-history batch before reset (fast-fluff CLI builds only) |

`-j` and `-g` are exposed by OpenMP-enabled CLI builds such as
`make modern`, the dedicated `make arm64` target, and the Windows
`make win32` / `make win64` builds. Builds without OpenMP reject those flags
instead of silently ignoring them.

On OpenMP-enabled Linux and Windows CLI builds, BoltLZ, Fluff, and Zstandard
use an automatic multi-processor policy for larger workloads. BoltLZ
parallelizes independent archive blocks without changing their encoded
representation; a single file uses that path only when it has at least two
blocks per worker. Use `--single-thread` to opt out, or `-j N` from advanced
help to choose an explicit worker count. `-j 1` also selects the sequential
path. These controls appear only in builds with the parallel feature enabled.

---

## Windows Frontend (new)

**File:** `src/frontend/win/main.c`  
**Compiler:** MinGW cross-compiler (x86 and x86_64)  
**Interface:** Command-line

### Windows: Build

```sh
make win32   # i686-w64-mingw32-gcc, output build/win32/boltpack.exe
make win64   # x86_64-w64-mingw32-gcc, output build/win64/boltpack.exe
```

Alternatively:

```sh
make FRONTEND=win CC=gcc
```

The Win frontend is a near clone of `src/frontend/dos/main.c` with `BP_OS_WINDOWS` recorder metadata and Windows-compatible path handling (backslash + forward slash support).

The `win32` and `win64` CLI builds now also expose the simplified modern CLI
surface used by the Linux-family frontends: long-form commands such as
`create`, `add`, `extract`, `list`, `test`, `info`, and `delete`, plus a
basic `boltpack help` page that hides expert compression tuning. Run
`boltpack help advanced` to show the full Windows CLI surface.

`--interchange-profile-1` is available for `create` and `add`. It selects
BoltLite with 16 KiB blocks by default, or Stored with `-0`, and enforces the
mini-compatible entry-count, per-entry, block, and serialized archive limits.
Explicit codec, block-size, or level settings that conflict with the profile
are refused. Appending also checks that the existing archive already conforms.
The Win64 CLI creates a Profile 1 archive under Wine that is byte-checked as
BoltLite/16 KiB and accepted by both Linux full and mini readers. Native Windows
profile/runtime behavior remains unqualified. Append and delete refuse read-only, multiply linked, and reparse-point archive
targets before editing. The Win64 CLI also passes a Wine runtime fixture for supplementary-Unicode paths, corrupt-output
preservation, portable file/directory FAT flags and timestamps, append, and
delete. Extraction preflights the full archive name table before creating the
output root; Linux-created fixtures confirm reserved Windows names,
trailing dots/spaces, case-insensitive aliases, and file/directory-prefix
conflicts in either archive order are refused without output changes. A Linux
CLI and Win64 Wine also
transfer v6 archives both ways, with bytes verified and
unsupported foreign metadata reported as partial. Append/delete now reapply the
archive's supported native file-attribute bits after `ReplaceFile` and roll back
if attribute restoration fails. Wine confirms read-only archive edits preserve
bytes and READONLY/HIDDEN/SYSTEM flags with no stage left behind, and confirms
HIDDEN/SYSTEM survive successful append and delete. Wine does not expose host POSIX symlinks as Win32 reparse points, and it
does not qualify native ACL/stream merging or recovery behavior; those still
require native Windows testing.

On OpenMP-enabled Windows CLI builds, BoltLZ, Fluff, and zstd use multiple
workers automatically for sufficiently large workloads. Use
`--single-thread` to force serial compression, or `-j N` to set the worker
count. `-g N` tunes Fluff's grouped-block size. The build-aware help omits
these controls when OpenMP support is absent.

---

## Total Commander WCX Plugin

**File:** `src/frontend/win_wcx/boltpack_wcx.c`  
**Compiler:** MinGW cross-compiler (i686 and x86_64)  
**Interface:** Total Commander WCX packer plugin API (DLL)

The plugin exports both the legacy ANSI entry points and the Unicode `W`
entry points supported since Total Commander 7.5 (packer interface 2.20; see
the [WCX Unicode support guide](https://ghisler.github.io/WCX-SDK/unicode_support.htm)).
The Unicode path covers opening and listing archives, extraction, packing,
deletion, and progress/volume callbacks. Paths enter as UTF-16, are represented
internally as UTF-8, and use wide Windows filesystem calls. The ANSI path is
retained for older hosts and rejects paths that do not round-trip through the
active ANSI codepage. Win32 and Win64 builds pass; runtime behavior in Total
Commander has not yet been qualified.

### Compression Method Selection

The WCX plugin **reads the compression method from the Windows registry**, allowing users to configure their preferred compression at the system level:

```text
HKEY_CURRENT_USER\Software\BoltPack\CompressionMethod (DWORD)
```

Valid values:

| Value | Method           | Description                          |
| ----- | ---------------- | ------------------------------------ |
| `0`   | Stored           | No compression                       |
| `1`   | BoltLite             | Small-memory LZ77                    |
| `2`   | boltlz (default) | Fast general-purpose compression     |
| `3`   | fluff            | LZ77 + FSE/tANS, best compression ratio |

### TC WCX: Build

```sh
make win32   # Builds boltpack.wcx and boltpack-config.exe for 32-bit
make win64   # Builds boltpack.wcx64 and boltpack-config.exe for 64-bit
make win32-totalcmd-plugin   # Builds an installable Total Commander plugin ZIP for 32-bit TC
make win64-totalcmd-plugin   # Builds an installable Total Commander plugin ZIP for 64-bit TC
```

This produces:

- **boltpack.wcx** / **boltpack.wcx64** — The plugin DLL for Total Commander
- **boltpack-config.exe** — Configuration utility (dialog box or CLI)
- **boltpack-<version>-totalcmd-plugin-win32.zip** / **boltpack-<version>-totalcmd-plugin-win64.zip** — Click-to-install plugin ZIP for Total Commander

The FLTK installer targets also generate the matching Total Commander plugin ZIP:

```sh
make win32-fltk-installer   # Also emits build/win32/boltpack-<version>-totalcmd-plugin-win32.zip
make win64-fltk-installer   # Also emits build/win64/boltpack-<version>-totalcmd-plugin-win64.zip
```

Release packaging also copies these installable plugin ZIPs into `dist/`, and
the generated download page lists them as separate Total Commander plugin
downloads.

### Installation

Preferred:

1. Open `boltpack-<version>-totalcmd-plugin-win32.zip` or `boltpack-<version>-totalcmd-plugin-win64.zip` inside Total Commander.
2. Accept the plugin installation prompt.
3. Run `boltpack-config.exe` to select your preferred compression method.

Manual alternative:

1. Copy `boltpack.wcx` (or `boltpack.wcx64`) to the Total Commander plugins directory.
2. Run `boltpack-config.exe` to select your preferred compression method.
3. Restart Total Commander.
4. Configure the plugin in **Configuration → Packer plugins**: select "boltpack" or add it manually.

### Configuration Utility

**boltpack-config.exe** provides both GUI and CLI modes:

**GUI mode** (no arguments):

```text
boltpack-config.exe
```

Opens a dialog box to choose compression method interactively.

**CLI mode**:

```text
boltpack-config.exe --get       # Show current setting
boltpack-config.exe --set 0|1|2|3 # Set compression method
boltpack-config.exe --help      # Show usage
```

### Features

- **Creates new archives** with the configured compression method
- **Appends to existing archives** (preserves their original compression)
- **Extracts archives** created by any BoltPack tool
- **Deletes files** from archives (rewrites modified archive)
- **Lists archive contents** with file info (name, date, size, method)
- **Handles directory metadata** and nested archive structures

### Advantages

- **Zero UI blocking** — Configuration is set once, not per-archive
- **Registry-based** — Settings survive TC restarts and are user-level (not global)
- **Full compatibility** — Archives created with any compression method can be opened, extracted, and modified by any frontend
- **Fluff support** — Unlike older WCX plugins, this version supports the fluff codec for best compression

---

## DOS Frontend

**File:** `src/frontend/dos/main.c`
**Compiler:** DJGPP (primary), Turbo C (with fallbacks)
**Interface:** Command-line

### DOS: Build

```sh
make FRONTEND=dos
```

This uses the host compiler to build `build/dos/boltpack`; it is useful for
checking the frontend's C89 source and host-side behavior, but it is not a DOS
binary. `make dos-host-test` runs its staged-extraction, projected-name, and
profile create/append fixtures. The optional mini-reader check is:

```sh
python3 test/check_dos_profile.py --boltpack build/dos-host-test/boltpack \
  --linux-boltpack build/linux/boltpack \
  --mini-boltpack build/linux32mini/bpmini
```

It runs the mini reader as a native 32-bit Linux process on this host. `make dos-djgpp` builds the DOS
executable when the `i586-pc-msdosdjgpp-gcc` toolchain is available. To run
the full frontend on DOSBox-X FAT12, provide `CWSDPMI.EXE` from DJGPP's
`csdpmi` package:

```sh
make CC=gcc CWSDPMI=/path/to/CWSDPMI.EXE dos-djgpp-dosboxx-fat12-test
```

This exercises full-DOS create, append, delete, test, and extract paths in both
directions with Linux and checks HIDDEN/SYSTEM/ARCHIVE preservation across
edits. Native DOS and other DOS filesystems remain separate runtime targets.

Cross-compile from Linux:

```sh
i586-pc-msdosdjgpp-gcc -ansi -pedantic -Wall -O2 \
     \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c \
    src/frontend/dos/main.c src/core/boltlz.c \
    -o boltpack.exe
```

### DOS: bp_io_t and Extraction

Archive input uses the `<stdio.h>` adapter. Extracted files use exclusive
same-directory 8.3-style stages opened with `O_CREAT|O_EXCL`; the frontend
checks the core's checksum status and the file descriptor close before
publishing. Forced replacement moves an existing regular file to a recovery
name and restores it if publication fails. A CP850 projected-path preflight
rejects control characters, DOS-forbidden filename punctuation, device names,
case aliases, and file/directory-prefix conflicts before creating the output
tree. With DJGPP, creation writes portable FAT metadata and extraction
restores known FAT flags, deferring directory flags until children finish.
Directory timestamp restoration is not supported on plain DOS and is reported
as partial. The host test build simulates FAT flags and does not validate the
DOS APIs. Resource forks and symlinks are reported as unsupported, as are
metadata records the frontend cannot apply.
Preflight checks the destination volume's long filename support with DJGPP.
When it is unavailable, every archive path component and output-root component
must fit a conservative 8.3 profile; names requiring generated aliases are
refused before output changes. Non-DJGPP DOS builds use this conservative
profile because they do not query volume-specific LFN support. The host fixture
simulates a no-LFN volume and checks both archive names and output roots.
The pathname-based adapter cannot prevent a concurrent parent-directory
substitution.

Create, add, and delete write a unique exclusive sibling stage. Add copies the
source archive to the stage before appending. Existing archives are retained
under a recovery name until the staged replacement succeeds; known hidden,
system, and archive flags are copied on DJGPP, and read-only or multiply linked
archive targets are refused. The host fixture injects write, close, and
publication failures and checks that original archive bytes survive.

Native DJGPP compilation and DOS runtime validation remain outstanding in
this environment because the cross-compiler and DOS runtime fixture are not
available.

### DOS: Directory Recursion

Uses `opendir()` / `readdir()` from `<dirent.h>` (DJGPP extension). Falls back to `findfirst()` / `findnext()` for Turbo C. Path limit: 260 bytes (DOS namespace). Symlinks not supported (DOS has no equivalent).

### DOS: Platform Metadata

- Archives as `BP_OS_DOS`
- Preserves modification time
- No permission bits (FAT has no Unix-style permissions)

### DOS: Text Encoding

Filenames are stored as-is (ASCII / codepage). Non-ASCII names may not round-trip correctly between DOS and other platforms — this is a known limitation of DOS's lack of Unicode support.

### Commands and Options

| Command | Description                   |
| ------- | ----------------------------- |
| `c`     | Create archive (recursive)    |
| `a`     | Add files to existing archive |
| `x`     | Extract archive               |
| `l`     | List archive contents         |
| `t`     | Test archive integrity        |
| `i`     | Show archive information      |
| `d`     | Delete files from archive     |

| Option    | Description                                   |
| --------- | --------------------------------------------- |
| `-b SIZE` | Block size: `8k`, `16k`, `32k`, `64k`, `256k` |
| `-0`      | Store only (no compression)                   |
| `--interchange-profile-1` | Create/add with Stored or BoltLite, 16 KiB blocks, and mini-compatible limits |
| `-F`      | Use fluff LZ77 + FSE/tANS compression            |
| `-1`      | Use BoltLite compression                          |
| `-q`      | Quiet (progress line only)                    |
| `-qq`     | Silent (errors only)                          |
| `-o PATH` | Extract to directory                          |
| `-f`      | Force overwrite                               |
| `-n`      | Dry run                                       |

The profile flag applies only to `c` and `a`. It selects BoltLite unless `-0`
is supplied, permits an explicit `-1` BoltLite selection, and refuses other
codecs or block sizes. Append validates the existing archive before changing
the staged copy.

---

## AmigaOS Frontend

**File:** `src/frontend/amiga/main.c`
**Compiler:** m68k-amigaos-gcc (bebbo's fork, cross-compile from Linux); SAS/C 6.x or VBCC (native)
**Interface:** Command-line
**Target:** AmigaOS 2.x/3.x, 68k (A500, A600, A1200, A2000, A3000, A4000)

### AmigaOS: Build

Cross-compile from Linux:

```sh
make amiga68k
```

For the combined Amiga release build and package staging:

```sh
make amiga
```

This produces both Amiga executables in `build/amiga/`:

- `boltpack` — command-line build
- `boltpack-gui` — GadTools GUI build

Or directly:

```sh
m68k-amigaos-gcc -noixemul -ansi -Wall -Wextra -O2 \
    -I src/core  \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/amiga/main.c src/core/boltlz.c \
    -o boltpack
```

`-noixemul` links against AmigaDOS's native clib (libnix) instead of ixemul.library (the Unix emulation layer). This produces a self-contained binary with no external library dependency beyond `dos.library` and `exec.library`, which are present on every Amiga.

`-pedantic` is intentionally omitted: AmigaOS NDK headers use K&R-style declarations that fail strict C89 pedantic checks.

**Toolchain setup (bebbo's amiga-gcc):**

```sh
git clone https://github.com/bebbo/amiga-gcc.git
cd amiga-gcc && make all
# adds m68k-amigaos-gcc to PATH
```

### AmigaOS: bp_io_t Implementation

Wraps standard C `<stdio.h>` functions (provided by libnix):

| Callback | Implementation              |
| -------- | --------------------------- |
| `read`   | `fread(buf, 1, count, fp)`  |
| `write`  | `fwrite(buf, 1, count, fp)` |
| `seek`   | `fseek(fp, offset, origin)` |
| `tell`   | `ftell(fp)`                 |

### AmigaOS: Directory Traversal and Safe Extraction

Uses POSIX `opendir()` / `readdir()` and `lstat()` for file type and size.
libnix maps these calls to AmigaDOS `Lock()`/`ExNext()`. Source walking is
depth-first with an explicit heap-backed directory stack, so C stack use does
not grow with source nesting. The CLI carries an 8 KiB AmigaDOS `$STACK` cookie
and explicitly expands the stack at startup through libnix, then restores it
on exit. Without the startup expansion, a shallow host-directory create
stalled with the 4 KiB Workbench default. FS-UAE host-directory testing passes
64-level create/extract with `--stack 0`; its repeated one-character directory
names stay inside the legacy path limit. This is emulator evidence for the
tested Workbench 2.04 setup, not a universal limit for every Amiga
configuration. The cookie is documented by the [AmigaOS stack-control
guide](https://wiki.amigaos.net/wiki/Controlling_Application_Stack).

Before extraction, the CLI checks projected path collisions using case folding
and rejects AmigaDOS `:` volume/device syntax. The bounded preflight index
accepts up to 1,024 primary entries and 32 KiB of projected path keys; exceeding
either limit refuses extraction before creating output. Existing parent
components are checked with `lstat` and symlink components are refused.
Extraction path scratch buffers are allocated outside callback stack frames.
These pathname checks cannot prevent a concurrent parent rename. Symlink
entries are not extracted and produce a partial result. Source soft-link
behavior is not qualified because the FS-UAE host-directory mount reports a
host symlink as a regular file; native soft-link handling also remains open.
For regular-file creation, the opened handle is checked against the initial
`lstat`, and handle/path identity plus size and timestamps are rechecked after
packing and native metadata capture. Directory identity and timestamps are
checked while the explicit traversal stack is active. The FS-UAE
`fsuae-source-race-test` replaces a source pathname after open and verifies the
staged archive is rejected without changing an existing archive. Path-based
directory walking can still miss a transient swap-and-restore race.

Regular files are written to an exclusive sibling staging file. The destination is published only after the core verifies the entry checksum and the staging descriptor closes successfully. Forced replacement moves the old regular file to a unique recovery name before publishing and restores it if publication fails. A process interruption between those renames can leave the original at the recovery path. Unsupported entry kinds and failed metadata restores produce a partial result.

### AmigaOS: Platform Metadata

The AmigaOS CLI is the primary frontend that makes BoltPack's `BP_TYPE_META` mechanism worthwhile. After every `BP_TYPE_DATA` and `BP_TYPE_DIR` entry, a `BP_TYPE_META` entry with `platform = BP_CREATOR_OS` is written immediately following it, carrying the subject's protection bits and comment. The same shared CLI source is used by AmigaOS 4.x, MorphOS, and AROS wrappers, so each records its own platform identifier. The GadTools GUI also captures native protection and comments for files and directories, including empty directories, and writes the matching metadata entries. GUI runtime behavior remains unqualified.

**Metadata payload layout:**

| Offset | Size | Field         | Description                                  |
| ------ | ---- | ------------- | -------------------------------------------- |
| 0      | 4    | `protection`  | Raw `fib_Protection` value (little-endian)   |
| 4      | 1    | `comment_len` | Length of comment (0–79)                     |
| 5      | N    | `comment`     | File comment bytes (ISO-8859-1; embedded NUL is invalid) |

**Protection bits (AmigaOS HSPARWED format):**

| Symbol | Bit | Meaning when set                                 |
| ------ | --- | ------------------------------------------------ |
| H      | 7   | Hold — script is currently resident              |
| S      | 6   | Script — file is an AmigaDOS script              |
| P      | 5   | Pure — file can be made resident with `resident` |
| A      | 4   | Archive — file modified since last backup        |
| R      | 3   | Read-protected _(inverted: 0 = readable)_        |
| W      | 2   | Write-protected _(inverted: 0 = writable)_       |
| E      | 1   | Execute-protected _(inverted: 0 = executable)_   |
| D      | 0   | Delete-protected _(inverted: 0 = deletable)_     |

Lower four bits (RWED) are inverted: a clear bit grants the permission. Upper four bits (HSPA) are direct flags: a set bit means the flag is active.

`amiga_get_meta()` uses `Lock()` + `Examine()` to read `fib_Protection` and `fib_Comment`. `amiga_apply_meta()` checks both `SetProtection()` and `SetComment()` results. Native metadata is applied only when its exact subject was successfully extracted; directory metadata and timestamps are deferred until all entries have been processed, so protection bits do not block child extraction. Malformed Amiga payloads fail as corrupt metadata. The CLI and GadTools GUI share a decoder that rejects embedded NUL bytes within the declared comment length instead of silently truncating the native comment.

Foreign native metadata, portable FAT envelopes, resource forks, and symlink entries are skipped and make the extraction result partial. Malformed payloads for this Amiga-family metadata type are rejected as corrupt, including when the subject was skipped. Amiga protection bits are not mapped from POSIX or FAT fields because their meanings differ. Extracted files without native Amiga metadata receive the filesystem's default permissions.

File and directory modification times use format-6 UTC seconds. Epoch zero is preserved. Negative or host-unrepresentable source times are warned about and recorded as unknown rather than wrapping into a different date.

### AmigaOS: Text Encoding

AmigaOS Western-European locale uses ISO-8859-1 (Latin-1). File names are converted to UTF-8 on archive and back to ISO-8859-1 on extraction via `bp_local_to_utf8()` / `bp_utf8_to_local()` with `BP_CP_8859_1`.

ISO-8859-1 maps bytes 0x80–0xFF directly to Unicode U+0080–U+00FF, so the conversion is lossless in both directions for any character in the Latin-1 range.

### AmigaOS: Block Size Auto-Detection

When no `-b` flag is given, the block size is chosen automatically based on available RAM:

```c
ULONG avail = AvailMem(MEMF_ANY | MEMF_LARGEST);
```

| Available RAM | Block size |
| ------------- | ---------- |
| ≥ 640 KB      | 256 KB     |
| ≥ 256 KB      | 64 KB      |
| ≥ 128 KB      | 32 KB      |
| ≥ 64 KB       | 16 KB      |
| < 64 KB       | 8 KB       |

An A500 with 512 KB chip + 512 KB fast RAM (1 MB total) will typically have ~600 KB free after AmigaOS and get 256 KB blocks. An A500 without a fastRAM expansion (~300 KB free) gets 64 KB blocks. The minimum 8 KB block works even on a stock A500 used as a dedicated BoltPack machine.

### AmigaOS: Path Handling

AmigaOS uses `:` as a volume/device separator (`DH0:Work/project`). The frontend strips everything up to and including the first `:` when computing archive entry names:

```text
DH0:Work/project  →  Work/project
Work/project      →  Work/project
myfile.c          →  myfile.c
```

Within archive entry names, forward slashes are always used as directory separators (matching the archive format spec). AmigaDOS accepts forward slashes as child separators in relative paths, so the same strings work on AmigaOS for filesystem operations without any conversion.

### AmigaOS: Commands and Options

| Command | Description                   |
| ------- | ----------------------------- |
| `c`     | Create archive (recursive)    |
| `a`     | Add files to existing archive |
| `x`     | Extract archive               |
| `l`     | List archive contents         |
| `t`     | Test archive integrity        |
| `i`     | Show archive information      |
| `d`     | Delete files from archive     |

| Option    | Description                                                                           |
| --------- | ------------------------------------------------------------------------------------- |
| `-b SIZE` | Block size: `8k`, `16k`, `32k`, `64k`, `256k` (default: auto-detected via `AvailMem`) |
| `-0`      | Store only (no compression)                                                           |
| `-F`      | Use fluff LZ77 + FSE/tANS compression                                                    |
| `-1`      | Use BoltLite compression                                                                  |
| `-q`      | Quiet (summary only)                                                                  |
| `-qq`     | Silent (errors only)                                                                  |
| `-o PATH` | Output directory for extraction                                                       |
| `-f`      | Force overwrite existing files                                                        |
| `-n`      | Dry run                                                                               |

---

## Atari TOS/MiNT Frontend

**File:** `src/frontend/atari/main.c`
**Compiler:** m68k-atari-mint-gcc (Vincent Rivière's cross-toolchain)
**Interface:** Command-line (`.TOS`)
**Target:** Atari ST/STE/Mega/TT/Falcon, TOS 1.04+

### Atari: Build

Cross-compile from Linux:

```sh
make atari
```

This produces both Atari executables in `build/atari/`:

- `boltpack.tos` — command-line build
- `boltpack.prg` — GEM GUI build

Or directly:

```sh
m68k-atari-mint-gcc -ansi -Wall -Wextra -O2 \
    -I src/core  \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/atari/main.c src/core/boltlz.c \
  -o build/atari/boltpack.tos
```

### Atari: Toolchain setup (Docker, Arch Linux)

Rivière's toolchain packages target Debian. A container is the cleanest path on Arch:

```dockerfile
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y wget ca-certificates && \
    wget -q http://vincent.riviere.free.fr/soft/m68k-atari-mint/packages/debian/vincent-riviere-keyring.deb && \
    dpkg -i vincent-riviere-keyring.deb && \
    echo "deb http://vincent.riviere.free.fr/soft/m68k-atari-mint/packages/debian bookworm main" \
         > /etc/apt/sources.list.d/mint.list && \
    apt-get update && apt-get install -y gcc-m68k-atari-mint && \
    rm -rf /var/lib/apt/lists/*
```

Build the container once, then cross-compile:

```sh
docker build -t boltpack-atari .
docker run --rm -v $(pwd):/src -w /src boltpack-atari make atari
```

The resulting `build/atari/boltpack.tos` and `build/atari/boltpack.prg` can be transferred to an Atari or tested under **Hatari** (`pacman -S hatari` on Arch).

### Atari: bp_io_t Implementation

Standard `<stdio.h>` wrappers provided by mintlib, which is **statically linked**. No MiNT runtime is required on the target — the binary runs on plain TOS 1.04+.

### Atari: Directory Recursion

Uses POSIX `opendir()` / `readdir()` / `stat()` from mintlib. Depth-first walk; directory entry emitted before its contents. Symlinks (available only under MiNT) are silently skipped.

### Atari: Source Change Checks

For each regular source file, the CLI computes a CRC-32 before opening the file
for archiving, compares that value with the archive writer's checksum, then
checks the current pathname's CRC-32 again after the source stream closes. This
detects content substitutions that preserve TOS size and timestamp fields. It
adds two full reads of each source in addition to the archive read. CRC
collisions, changes between the initial metadata check and baseline hash, and
changes after the final hash remain possible.

### Atari: Platform Metadata

After every `BP_TYPE_DATA` and `BP_TYPE_DIR` entry, a `BP_TYPE_META` entry with `platform = BP_OS_ATARI` is written. Its payload is exactly **1 byte**: the TOS attribute flags.

**TOS attribute byte:**

| Bit | Symbol      | Meaning            |
| --- | ----------- | ------------------ |
| 0   | `FA_RDONLY` | Read-only          |
| 1   | `FA_HIDDEN` | Hidden             |
| 2   | `FA_SYSTEM` | System             |
| 5   | `FA_ARCH`   | Archive (modified) |

Read via `Fattrib(path, 0, 0)`, restored via `Fattrib(path, 1, attr)`. On extraction from a non-Atari archive, `BP_TYPE_META` entries with a different `platform` are silently skipped.

### Atari: Text Encoding

The Atari ST uses its own proprietary 8-bit character set — not ISO-8859-1, not CP850. File names are converted to UTF-8 on archive and back to the Atari ST encoding on extraction via `bp_local_to_utf8()` / `bp_utf8_to_local()` with `BP_CP_ATARIST`. Characters outside the ST character set are replaced with `_` on extraction.

### Atari: Block Size Auto-Detection

When no `-b` flag is given, block size is chosen based on available RAM:

```c
long avail = (long)Malloc(-1L);  /* largest free block, no allocation */
```

| Available RAM | Block size |
| ------------- | ---------- |
| ≥ 640 KB      | 256 KB     |
| ≥ 256 KB      | 64 KB      |
| ≥ 128 KB      | 32 KB      |
| ≥ 64 KB       | 16 KB      |
| < 64 KB       | 8 KB       |

A 1 MB ST typically has ~600 KB free and gets 256 KB blocks. A stock 512 KB ST gets 16 KB blocks.

### Atari: Path Handling

Drive prefixes (`C:\`, `C:`) are stripped when computing archive entry names. Backslashes are normalised to forward slashes.
Create, add, and delete operate on exclusive sibling staging files. Add copies
existing entries into a new staged archive before appending; the original is
replaced only after successful finalization and close. Publication keeps a
recovery copy while replacing an existing archive. Archive targets must be
writable regular files; symbolic links and directories are refused. A crash
while writing leaves the old archive intact but may leave a stage file. A
crash between the two publication renames may leave the old archive at its
recovery name.

```text
C:\WORK\MYPROJ   →   WORK/MYPROJ
C:MYFILE.TXT     →   MYFILE.TXT
MYFILE.TXT       →   MYFILE.TXT
```

### Atari: Commands and Options

| Command | Description                   |
| ------- | ----------------------------- |
| `c`     | Create archive (recursive)    |
| `a`     | Add files to existing archive |
| `x`     | Extract archive               |
| `l`     | List archive contents         |
| `t`     | Test archive integrity        |
| `i`     | Show archive information      |
| `d`     | Delete files from archive     |

| Option    | Description                                                                             |
| --------- | --------------------------------------------------------------------------------------- |
| `-b SIZE` | Block size: `8k`, `16k`, `32k`, `64k`, `256k` (default: auto-detected via `Malloc(-1)`) |
| `--interchange-profile-1` | Enforce mini-compatible limits for create/add (BoltLite by default, 16 KiB blocks) |
| `-0`      | Store only (no compression)                                                             |
| `-F`      | Use fluff LZ77 + FSE/tANS compression                                                      |
| `-1`      | Use BoltLite compression                                                                    |
| `-q`      | Quiet (summary only)                                                                    |
| `-qq`     | Silent (errors only)                                                                    |
| `-o PATH` | Output directory for extraction                                                         |
| `-f`      | Force overwrite existing files                                                          |
| `-n`      | Dry run                                                                                 |

### Atari: Observed Compression Throughput

Measured on Atari systems using the shared compression core used by both the
CLI and GEM GUI frontends. These are practical reference values, not hard
guarantees:

| Machine        | RAM  | BoltLite     | boltlz  | Fluff    |
| -------------- | ---- | -------- | ------- | -------- |
| 68000 @ 8 MHz  | 4 MB | 1.1 kB/s | 27 kB/s | 1.2 kB/s |
| 68030 @ 16 MHz | 4 MB | 4.9 kB/s | 81 kB/s | 4.1 kB/s |

---

## Mac OS 9 Frontend

**File:** `src/frontend/macos9/main.c`
**Compiler:** CodeWarrior Pro 8 (Metrowerks)
**Target:** PowerPC (iMac G3, OS 9.2.1)
**Interface:** Graphical (Mac Toolbox)

This is a GUI frontend, unlike Linux and DOS which are command-line. Mac OS 9 has no command-line shell, so the frontend uses the standard Mac Toolbox event model with drag-and-drop as the primary interaction method.

### Mac OS 9: Build

Built natively on Mac OS 9 with either CodeWarrior Pro 8 or MPW 3.5.

1. Create a new PPC application target in CodeWarrior Pro 8
2. Add the frontend and core sources listed below
3. Add `src/frontend/macos9/boltpack.r` as the Rez resource source
4. Project → Make (⌘M)

If you want a ready-to-transfer source tree instead of copying files manually,
generate `dist/boltpack-<version>-source-macos9-ppc.tar.gz` with:

```sh
bash scripts/create-source-kits.sh classic-mac
```

For MPW 3.5, use the same source files and include paths, then:

1. Open the extracted kit in MPW Shell
2. Compile the C files with `MrC` for PowerPC output
3. Compile `src/frontend/macos9/boltpack.r` with `Rez`
4. Link the objects with `PPCLink` against `InterfaceLib`, `AppearanceLib`, `NavigationLib`, `MathLib`, and the MPW PPC C runtime
5. Merge the compiled resources into the final `BoltPack` application

The generated Classic Mac source kit includes `build-notes.md` with a sample
MPW command sequence alongside the CodeWarrior settings.

The Retro68 68k port is a separate target under
`src/frontend/macos9/68k/Source/main.c` with its own resources and packaging
flow. Its compression dialog offers Interchange Profile 1, which selects
BoltLite with 16 KiB blocks and the profile's mini-compatible entry-count,
entry-size, and serialized-size limits. The separate PowerPC frontend in
`src/frontend/macos9/main.c` asks whether each new archive should use the
default format or Interchange Profile 1. The profile uses BoltLite with 16 KiB
blocks and mini-compatible entry-count, entry-size, and serialized-size
limits; the default path remains BoltLZ.

The Retro68 build compiles the shared `src/core` implementation. The portable
68k source kit also carries local copies of the core headers and sources; keep
them synchronized with `src/core`. A stale local header/type copy caused 68k
ABI drift and showed up as false `BP_ERR_IO` failures when the GUI tried to
add directory entries during archive creation. The checked-in archive sources,
headers, and fixed-width type definitions are byte-for-byte synchronized.

**Project contents:**

| File                             | Role                                            |
| -------------------------------- | ----------------------------------------------- |
| `src/core/archiver.c`            | Archive format read/write                       |
| `src/core/compress.c`            | boltlz compression wrapper                      |
| `src/core/boltlite.c`                | BoltLite codec                                      |
| `src/core/fluff.c`               | fluff codec                                     |
| `src/core/bp_codepage.c`         | code page helpers                               |
| `src/core/boltlz.c`              | boltlz library                                  |
| `src/frontend/macos9/main.c`     | GUI frontend                                    |
| `src/frontend/macos9/boltpack.r` | Resources (icons, menus, dialogs, app identity) |

**Libraries (from CodeWarrior):**

| Library         | Purpose                                                             |
| --------------- | ------------------------------------------------------------------- |
| `InterfaceLib`  | Mac Toolbox calls                                                   |
| `MathLib`       | PPC math support                                                    |
| `MSL C.PPC.Lib` | Metrowerks Standard Library (C runtime)                             |
| `AppearanceLib` | Appearance Manager (progress bar control)                           |
| `NavigationLib` | Navigation Services (`NavChooseFolder`) — runtime-checked, optional |

**Target settings:**

| Setting        | Value                                                |
| -------------- | ---------------------------------------------------- |
| App name       | BoltPack                                             |
| Creator        | `'BoPk'`                                             |
| Type           | `'APPL'`                                             |
| Preferred heap | 2048 KB                                              |
| Minimum heap   | 1536 KB                                              |
| Stack          | 256 KB                                               |
| Processor      | Schedule for 750 (G3)                                |
| Optimization   | Level 2                                              |
| C dialect      | ANSI C, ANSI Strict off (Toolbox headers require it) |
| Access paths   | `src/core/`, `src/core/`                             |

### Resources (ResEdit)

All resources live in `BoltPack.rsrc`, created with ResEdit 2.1.3.

**Application identity:**

| Resource | ID       | Purpose                                                                                                      |
| -------- | -------- | ------------------------------------------------------------------------------------------------------------ |
| `vers`   | 128, 129 | Version info (app + file)                                                                                    |
| `BNDL`   | 128      | Bundle — links creator `'BoPk'` to icons and file types                                                      |
| `FREF`   | 128      | File reference for `'APPL'` (the application)                                                                |
| `FREF`   | 129      | File reference for `'BOLT'` (bolt archives)                                                                  |
| `PICT`   | 128      | 96×96 application logo used by `DrawDropTarget`                                                              |
| `BoPk`   | 0        | Zero-length signature resource                                                                               |
| `SIZE`   | -1       | MultiFinder size flags: `isHighLevelEventAware`, `acceptSuspendResumeEvents`. Preferred 2048K, minimum 1536K |

**Icons (full family for each):**

| ID  | Assignment            | Resources                                      |
| --- | --------------------- | ---------------------------------------------- |
| 128 | Application icon      | `ICN#`, `icl4`, `icl8`, `ics#`, `ics4`, `ics8` |
| 129 | Document icon (.bolt) | `ICN#`, `icl4`, `icl8`, `ics#`, `ics4`, `ics8` |

**Menus:**

| Resource | ID  | Contents                                                          |
| -------- | --- | ----------------------------------------------------------------- |
| `MBAR`   | 128 | Menu bar (IDs 128, 129, 130)                                      |
| `MENU`   | 128 | Apple menu: About BoltPack...                                     |
| `MENU`   | 129 | File: Open Archive…, Create Archive…, —, Preferences…, —, Quit ⌘Q |
| `MENU`   | 130 | Edit: Undo ⌘Z, —, Cut ⌘X, Copy ⌘C, Paste ⌘V, Clear                |

The Edit menu is required for Desk Accessory compatibility, even though BoltPack does not use text editing.

**Main window:**

| Resource | ID  | Description                                                                                             |
| -------- | --- | ------------------------------------------------------------------------------------------------------- |
| `WIND`   | 128 | 360×300 document window titled "BoltPack" with logo, mode controls, drop area, and destination checkbox |

**Dialogs:**

| Resource        | ID        | Purpose             | Key items                                                                      |
| --------------- | --------- | ------------------- | ------------------------------------------------------------------------------ |
| `DLOG` + `DITL` | 128       | Progress            | File name, per-file bar, file count, ratio, cancel, overall label, overall bar |
| `DLOG` + `DITL` | 129       | Mixed items chooser | "Unpack Archives" / "Pack Everything" / Cancel                                 |
| `DLOG` + `DITL` | 130       | Archive name input  | EditText + OK / Cancel                                                         |
| `DLOG` + `DITL` | 131       | Preferences         | Checkbox "Ask where to save extracted files" + OK / Cancel                     |
| `DLOG` + `DITL` | 132       | About               | Icon, version, credits, OK                                                     |
| `ALRT` + `DITL` | 128 / 133 | Error alert         | Error text + OK                                                                |

**Strings:**

| Resource | ID  | Contents                                                     |
| -------- | --- | ------------------------------------------------------------ |
| `STR#`   | 128 | Error messages (one per `BP_ERR_*` code)                     |
| `STR#`   | 129 | UI strings: drop-target prompt plus mode and checkbox labels |

**ResEdit wiring details:**

- `rLogoPict = 128` is the picture resource loaded by `DrawDropTarget()`.
- `STR# 129` uses index 1 for the drop-target text, 2 for `Expand`, 3 for
  `Compress`, and 4 for `Ask for destination`.
- Progress dialog items map directly to the constants in `main.c`:
  `kProgFileName=1`, `kProgBar=2`, `kProgFileCount=3`, `kProgRatio=4`,
  `kProgCancel=5`, `kProgOverallLabel=6`, `kProgOverallBar=7`.
- The main window also creates three runtime controls not stored as separate
  dialog resources: `gRadioExpand`, `gRadioCompress`, and `gAskDestCheck`.

### macOS 9: bp_io_t Implementation

Wraps the Mac File Manager instead of `<stdio.h>`:

| Callback | Implementation                                                                                                        |
| -------- | --------------------------------------------------------------------------------------------------------------------- |
| `read`   | `FSRead(refNum, &count, buf)`                                                                                         |
| `write`  | `FSWrite(refNum, &count, buf)`                                                                                        |
| `seek`   | `SetFPos(refNum, posMode, offset)` — maps `SEEK_SET`/`SEEK_CUR`/`SEEK_END` to `fsFromStart`/`fsFromMark`/`fsFromLEOF` |
| `tell`   | `GetFPos(refNum, &pos)`                                                                                               |

The `handle` field points to a struct containing the file's `short refNum`.

### User Interface Model

The PowerPC Mac OS 9 frontend is event-driven, modeled after StuffIt Expander.

**Primary interaction — drag and drop:**

| User action                           | Result                                                     |
| ------------------------------------- | ---------------------------------------------------------- |
| Drop `.bolt` archive(s)               | Extract all                                                |
| Drop file(s) or folder(s)             | Create archive                                             |
| Drop mix of archives and non-archives | Ask user via dialog: "Unpack Archives" / "Pack Everything" |
| Drop multiple items to pack           | Prompt for archive name                                    |
| Drop single item to pack              | Auto-name: `<item name>.bolt`                              |

**Drag targets:**

- **Main window** — Drag Manager (`InstallTrackingHandler` / `InstallReceiveHandler`), accepts `'hfs '` flavor
- **App icon in Finder** — Apple Event `'odoc'` handler, receives `AEDescList` of `FSSpec`s

Both paths feed into the same classify-and-act logic.

**File classification:**

- Finder file type == `'BOLT'` → archive
- Directory bit set in `ioFlAttrib` → folder
- Everything else → regular file

**Extraction location:**

- Default: extract to the same folder as the archive
- Optional: ask user via `NavChooseFolder` (Navigation Services)
- The "Ask where to save" preference is only available when Navigation Services >= 1.1 is present (OS 9.1+). Check at startup with `Gestalt(gestaltNavigationServicesVersion, &version)`. If unavailable, the preference checkbox is greyed out and extraction always goes to the same folder

**Progress dialog (movable modal):**

- Displays: current file name, "File X of Y", per-file progress bar,
  compression ratio, and overall progress
- Cancel button — `bp_progress_fn` returns non-zero → core returns `BP_ERR_ABORTED`
- Pumps events during operation via short `WaitNextEvent` calls to keep UI responsive

### Event Loop

Standard Mac Toolbox event loop:

```c
while (!gQuit) {
    WaitNextEvent(everyEvent, &event, 30, nil);
    switch (event.what) {
        case kHighLevelEvent:  AEProcessAppleEvent(&event);
        case mouseDown:        handle menus, window drag, close box
        case keyDown:          ⌘-key menu equivalents
        case updateEvt:        redraw drop target content
        case activateEvt:      window activate/deactivate
    }
}
```

### Initialization Sequence

1. `MaxApplZone()` + Toolbox inits (`InitGraf`, `InitFonts`, `InitWindows`, `InitMenus`, `TEInit`, `InitDialogs`, `InitCursor`)
2. `RegisterAppearanceClient()` for Appearance Manager controls
3. Install Apple Event handlers (required four: open app, open documents, print, quit)
4. Load menu bar from `MBAR` 128
5. Open main window from `WIND` 128
6. Install Drag Manager handlers on main window
7. Check `Gestalt(gestaltNavigationServicesVersion, ...)` — enable/disable folder picker preference
8. Load preferences from Preferences folder
9. Call `bp_compress_init()`
10. Enter event loop

### Apple Event Handlers

| Event                | Action                                                           |
| -------------------- | ---------------------------------------------------------------- |
| `kAEOpenApplication` | No-op (window already open)                                      |
| `kAEOpenDocuments`   | Extract `FSSpec` list → classify and act (same as drag-and-drop) |
| `kAEPrintDocuments`  | Return `errAEEventNotHandled`                                    |
| `kAEQuitApplication` | Set quit flag                                                    |

The `kAEOpenDocuments` handler is what makes "drag files onto app icon in Finder" work. It receives the same data as the Drag Manager, so both paths share the action logic.

### macOS 9: Directory Recursion

Uses `PBGetCatInfoSync` with `CInfoPBRec`:

- Set `ioVRefNum`, `ioDirID`, `ioFDirIndex` (1-based) to enumerate directory contents
- Check `ioFlAttrib & 0x10` for subdirectory flag
- Build paths with `:` separator (Mac convention), convert to `/` for archive entry names
- For each file, check resource fork size (`ioFlRLgLen`) — if > 0, add as separate `BP_TYPE_RSRC` entry
- Archive Finder info (type, creator, flags) as `BP_TYPE_META` entry

### macOS 9: Platform Metadata

The Mac OS 9 frontend archives three entry types per file:

| Entry type     | Content                                                                          |
| -------------- | -------------------------------------------------------------------------------- |
| `BP_TYPE_DATA` | Data fork contents                                                               |
| `BP_TYPE_RSRC` | Resource fork contents (only if non-empty)                                       |
| `BP_TYPE_META` | Finder info: file type (4 bytes), creator code (4 bytes), Finder flags (2 bytes) |
| `BP_TYPE_DIR`  | Directory entry                                                                  |

On extraction:

- `BP_TYPE_DATA` → open data fork via `FSpOpenDF`, write
- `BP_TYPE_RSRC` → open resource fork via `FSpOpenRF`, write
- `BP_TYPE_META` → apply via `FSpSetFInfo`
- `BP_TYPE_DIR` → create via `DirCreate`
- Unknown platform metadata from other OSes → silently skip

Platform tag: `creator_os = BP_OS_MACOS9`, `creator_cpu = BP_CPU_PPC32`.

### macOS 9: Text Encoding

Mac OS 9 uses MacRoman natively. The archive stores UTF-8.

- **Pack:** convert MacRoman filenames → UTF-8 (lookup table for chars 0x80–0xFF)
- **Extract:** convert UTF-8 → MacRoman (reverse lookup; unmappable characters replaced with `_`)

The conversion table covers the 128 high-byte MacRoman characters. ASCII (0x00–0x7F) passes through unchanged.

### Memory and Block Size

The iMac G3 has at least 32 MB RAM. The application requests a 2 MB preferred heap (1.5 MB minimum) via the `SIZE` resource.

- Default block size: `BP_BLOCK_64K` (65536 bytes)
- Maximum supported: `BP_BLOCK_256K` (262144 bytes)
- BoltLZ compression workspace: 32 KiB Fast or 64 KiB Balanced, heap allocated
- Total memory per operation: block buffer + output buffer ≈ block_size × 2 + 66
- At 256K blocks: ~512 KB — well within the 2 MB heap

Block size auto-detection: query available memory with `FreeMem()` or `TempFreeMem()` and select the largest block size that fits comfortably (leaving at least 512 KB free for Toolbox, dialogs, and Drag Manager).

### PowerPC Mac OS 9: Compression Method Selection

Unlike CLI frontends that expose command-line flags (`-F`, `-0`, `-1`), the
PowerPC Mac OS 9 GUI currently uses a simple function call:

```c
static bp_uint8 ChooseCompressionMethod(void)
{
    return BP_METHOD_BOLTLZ;  /* Currently: boltlz (default) */
}
```

This architecture allows future extension to show a dialog menu enabling users to select:

- **BoltLite** (method 1): Small-memory LZ77 for compatibility-focused archives
- **Fluff** (method 3): LZ77 + FSE/tANS compression (best ratio)
- **boltlz** (method 2): Fast, small blocks only (current default)
- **Stored** (method 0): No compression (for pre-compressed data)

The underlying archiver already supports all four methods. The PowerPC Mac OS 9
GUI still uses boltlz for its default format; its separate create-time dialog
offers Interchange Profile 1, which selects BoltLite. The Retro68 68k source
kit has a method dialog and an Interchange Profile 1 option; see the build
notes above.

### Preferences

Stored in the Preferences folder as "BoltPack Preferences":

```c
FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &vRefNum, &dirID)
```

Format: simple binary struct:

| Offset | Size | Field                                                               |
| ------ | ---- | ------------------------------------------------------------------- |
| 0      | 2    | Version (`2`)                                                       |
| 2      | 1    | Ask extract location (0 = same folder, 1 = ask via NavChooseFolder) |
| 3      | 1    | Mode (`0` = Expand, `1` = Compress)                                 |

Loaded at startup and migrated if the on-disk version does not match
`kPrefVersion`. The "Ask where to save" option is only functional when
Navigation Services is available; otherwise it is forced to 0 and the checkbox
is disabled.

### Error Handling

All `BP_ERR_*` codes are translated to human-readable messages from `STR#` 128 and displayed via the error alert (`ALRT` 128). The core never prints to stdout/stderr — all errors go through the alert mechanism.

---

## macOS / Mac OS X Frontend

**File:** `src/frontend/macosx/main.c`
**Compiler:** Apple Clang (Xcode) or `arm64-apple-macosx11-clang` cross-compiler
**Interface:** Command-line
**Target:** Apple Silicon macOS 11+ in the repository Makefile target; native
source kits also support Intel macOS 10.9+.

Native PowerPC Mac OS X 10.4/10.5 CLI builds are also a supported practical
workflow when built on the machine itself. In that case, compile with
`-DBP_CREATOR_CPU=BP_CPU_PPC32` so created archives are tagged correctly for the
host CPU.

### macOS: Build

```sh
make macosx
```

Or directly (ARM64):

```sh
arm64-apple-macosx11-clang -ansi -pedantic -Wall -Wextra -O2 \
    -I src/core  \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/macosx/main.c src/core/boltlz.c \
    -framework CoreFoundation \
    -o boltpack
```

For Intel Mac builds, add `-DBP_CREATOR_CPU=BP_CPU_X86_64`.

For native PowerPC Mac OS X 10.4/10.5 builds, add
`-DBP_CREATOR_CPU=BP_CPU_PPC32` and use a native compiler on the Mac.

The repository's `make macosx` CLI target uses the portable C89 codecs and
does not include OpenMP or zstd. It has no parallel compression controls and
does not list `-Z` in its help, though generic level and preset descriptions
still mention zstd. The modern Cocoa GUI has a separate OpenMP build path
described below.

### macOS: Native Compile Packages

To prepare portable source packages for native compilation on Macs without
assuming Linux cross-compilers, run:

```sh
bash scripts/create-source-kits.sh macos
```

This creates tar.gz compile packages under `dist/` for:

- `source-macosx-ppc` — PowerPC Mac OS X 10.4/10.5, CLI only
- `source-macosx-legacy-gui` — legacy Cocoa GUI kit for PowerPC 10.5 and Intel 10.5-10.8
- `source-macosx-intel` — Intel macOS, CLI plus modern Cocoa GUI
- `source-macosx-arm64` — Apple Silicon macOS, CLI plus modern Cocoa GUI

Each kit includes a shared native build helper at
`scripts/macos/build-native.sh`.
Each kit also carries a root-level `build-notes.md` plus the main build docs in
`docs/BUILD.md` and `docs/frontends.md` rather than a separate kit-specific
README. The pre-10.9 Cocoa app lives in the separate `source-macosx-legacy-gui`
kit. The modern 10.9+ Cocoa GUI sources remain in `source-macosx-intel` and
`source-macosx-arm64`.

Use it like this:

```sh
sh scripts/macos/build-native.sh ppc
sh scripts/macos/build-native.sh legacy-ppc
sh scripts/macos/build-native.sh legacy-intel
sh scripts/macos/build-native.sh intel
sh scripts/macos/build-native.sh arm64
sh scripts/macos/build-native.sh all
sh scripts/macos/build-native.sh legacy-all
```

The `all` option builds every target in sequence and is mainly useful on a Mac
toolchain that can emit more than one architecture. On older Macs, use the
matching native target directly.

Target-specific behavior:

- `ppc` is for G4/G5 PowerPC Macs on Mac OS X 10.4/10.5 and produces only
  `build/macosx-ppc/boltpack`; the Cocoa GUI is intentionally excluded.
- `legacy-ppc` is for Leopard PowerPC Macs and produces
  `build/macosx-legacy-gui-ppc/BoltPack.app` from the legacy Cocoa frontend.
- `legacy-intel` is for Intel Mac OS X 10.5 through 10.8 and produces
  `build/macosx-legacy-gui-intel/BoltPack.app`; the default build uses `i386`
  so a single Intel app can cover the full pre-10.9 range.
- `intel` is for Intel macOS 10.9+ and produces
  `build/macosx-intel/boltpack` plus `build/macosx-gui-intel/BoltPack.app`.
- `arm64` is for Apple Silicon and produces `build/macosx-arm64/boltpack` plus
  `build/macosx-gui-arm64/BoltPack.app`.
- On Intel Macs running 10.5 through 10.8, use `legacy-intel` for the GUI app
  or `BUILD_GUI=0` with `intel` if you only want the CLI.

### Native-Unix Compile Packages

OpenBSD, Solaris, IRIX, HP-UX, and AIX are no longer part of the aggregate
Linux-hosted `./build.sh all` path. They are available as native compile
packages instead.

Create them with:

```sh
bash scripts/create-source-kits.sh native-unix
```

Each package carries the shared native helper `scripts/native-unix/build-native.sh`,
a root-level `build-notes.md`, and the main build docs. Use it like this on the
target host:

```sh
sh scripts/native-unix/build-native.sh openbsd
sh scripts/native-unix/build-native.sh solaris
sh scripts/native-unix/build-native.sh irix
sh scripts/native-unix/build-native.sh hpux
sh scripts/native-unix/build-native.sh aix
```

The helper assumes a GCC/Clang-style compiler by default. For vendor compiler
setups, override `CC` and `CFLAGS` explicitly.

### macOS: bp_io_t Implementation

Identical to the Linux frontend: standard `<stdio.h>` wrappers (`fread`, `fwrite`, `fseeko`, `ftello`) via `FILE *`.

### macOS: Directory Recursion

POSIX `opendir()` / `readdir()` / `stat()` — identical to the Linux frontend. Symlinks are archived as `BP_TYPE_SYMLINK` entries with the link target stored as the entry's data.

### macOS: Platform Metadata

The macOS CLI can produce up to three supplementary entries after a file or
directory entry, in this order:

| Entry type                             | Condition               | Content                                                                           |
| -------------------------------------- | ----------------------- | --------------------------------------------------------------------------------- |
| `BP_TYPE_RSRC`                         | Resource fork non-empty and at most 16 MiB | Resource fork bytes (read via `XATTR_RESOURCEFORK_NAME` xattr) |
| `BP_TYPE_META` (`BP_OS_MACOSX = 0x03`) | Every data/dir entry    | 2-byte Unix permissions (low 9 bits of `mode_t`)                                  |
| `BP_TYPE_META` (`BP_OS_MACOS9 = 0x02`) | Finder info present     | 18-byte Classic Mac info: Type (4), Creator (4), flags (2), crdate (4), mdate (4) |

On extraction:

- `BP_TYPE_RSRC` → restored via `setxattr(path, XATTR_RESOURCEFORK_NAME, ...)`
- `BP_TYPE_META` (`BP_OS_MACOSX`) → `chmod()` restores Unix permissions after
  resource forks and Finder info; directory permissions are deferred until
  children finish
- `BP_TYPE_META` (`BP_OS_MACOS9`) → `setxattr(path, "com.apple.FinderInfo", ...)` restores 32-byte Finder info block
- Format-6 valid mtimes restore to files and symlinks; directory mtimes are
  deferred until extraction ends
- Archive creation stores checked 64-bit UTC mtimes, including epoch zero;
  negative or host-unrepresentable source times are encoded as unknown with a
  warning
- Classic Mac creation/modification dates in the 18-byte Finder record are
  not restored and cause a partial-preservation result. Portable FAT flags do
  not have a macOS mapping and are also reported as partial.
- Resource forks larger than 16 MiB or unavailable through the source xattr API
  are warned about and omitted; destination filesystems that reject resource
  fork xattrs return a partial-preservation result.

Regular files are written to exclusive sibling temporary files and published
only after the core verifies the checksum and the stream closes. Symlinks are
buffered, checked against the extraction root, staged beside the destination,
and then published. Existing path components are checked with `lstat()` to
reject symlinks and non-directories. Because these are pathname checks rather
than directory-relative operations, a concurrent process can still replace a
checked parent between validation and use. Extraction preflights projected
names before creating the output root: canonical Unicode-equivalent and
case-insensitive collisions, file/directory prefix conflicts, paths beyond
`PATH_MAX`, and components beyond `NAME_MAX` are refused. The 65,535-entry
preflight bound and conservative case folding also apply on case-sensitive
volumes. Unsupported or unrepresentable metadata produces a partial result.

Create, append, and delete write to an exclusive sibling archive stage and
replace the target with a same-filesystem rename after finalization and close.
Append and delete reject non-regular archive targets and detect a replaced or
changed target before publication. Existing permission bits are copied to the
replacement archive; ownership, ACLs, and extended attributes are not copied.
The macOS CLI still needs a macOS target build and runtime round trip.

### macOS: Text Encoding

macOS uses UTF-8 natively. No filename conversion is required; `bp_local_to_utf8()` and `bp_utf8_to_local()` are no-ops.

### macOS: Commands and Options

| Command         | Description                      |
| --------------- | -------------------------------- |
| `create` / `c`  | Create archive (recursive)       |
| `add` / `a`     | Add files to existing archive    |
| `extract` / `x` | Extract archive                  |
| `list` / `l`    | List archive contents            |
| `test` / `t`    | Test archive integrity           |
| `info` / `i`    | Show archive information         |
| `delete` / `d`  | Delete files from archive        |
| `help`          | Show the default simplified help |

### macOS: Common Options

| Option     | Description                                   |
| ---------- | --------------------------------------------- |
| `-b SIZE`  | Block size: `8k`, `16k`, `32k`, `64k`, `256k` |
| `--interchange-profile-1` | Create/add under the mini-compatible profile (BoltLite, 16 KiB by default) |
| `-0`       | Store only (no compression)                   |
| `-F`       | Use Fluff compression                         |
| `-o PATH`  | Extract to directory                          |
| `-f`       | Force overwrite                               |
| `-n`       | Dry run                                       |
| `-q`/`-qq` | Quiet / silent                                |

### macOS: Advanced Help

The default `boltpack help` page now hides the secondary compression selectors
and long-form aliases. Run `boltpack help advanced` to show the full standalone
macOS CLI surface.

| Option              | Description                            |
| ------------------- | -------------------------------------- |
| `-1`, `--boltlite`      | Use BoltLite compression                   |
| `-2`, `--boltlz`    | Use boltlz compression explicitly      |
| `--interchange-profile-1` | Create/add using mini-compatible limits; defaults to BoltLite and 16 KiB blocks |
| `-l N`, `--level N` | Exact level: BoltLZ 1-2; Fluff/zstd 1-9 |
| `--light`           | Fast preset: BoltLZ 1; Fluff/zstd 1    |
| `--standard`        | Default preset: BoltLZ 2; Fluff 8; zstd 3 |
| `--best`            | Highest preset: BoltLZ 2; Fluff/zstd 9 |
| `--store`           | Long-form alias for `-0`               |
| `--fluff`           | Long-form alias for `-F`               |
| `--block-size N`    | Long-form alias for `-b`               |
| `--output PATH`     | Long-form alias for `-o`               |
| `--force`           | Long-form alias for `-f`               |
| `--dry-run`         | Long-form alias for `-n`               |
| `--quiet`           | Long-form alias for `-q`               |
| `--silent`          | Long-form alias for `-qq`              |

---

## Haiku / BeOS Frontend

**File:** `src/frontend/haiku/main.c`
**Compiler:** GCC (ships with Haiku) or `x86_64-unknown-haiku-gcc` cross-compiler
**Interface:** Command-line
**Target:** Haiku R1 (x86_64 primary, x86 32-bit secondary); BeOS R5 (identical API)

### Haiku: Build

```sh
make haiku
```

Or directly (cross-compilation from Linux):

```sh
x86_64-unknown-haiku-gcc -ansi -pedantic -Wall -Wextra -O2 \
    -I src/core  \
    src/core/compress.c src/core/archiver.c src/core/boltlite.c src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/haiku/main.c src/core/boltlz.c \
    -o boltpack
```

The standard `make haiku` CLI target uses the serial C89 Fluff path and omits
zstd. Its help does not show the unavailable method-4 selector, though generic
level and preset descriptions still mention zstd.

### Haiku: bp_io_t Implementation

Standard `<stdio.h>` wrappers (`fread`, `fwrite`, `fseeko`, `ftello`) — identical to the Linux frontend.

### Haiku: Directory Recursion

POSIX `opendir()` / `readdir()` / `stat()` — identical to the Linux frontend. Symlinks are archived as `BP_TYPE_SYMLINK` entries.

### Haiku: Platform Metadata

After every `BP_TYPE_DATA` and `BP_TYPE_DIR` entry, a `BP_TYPE_META` entry
with `platform = BP_OS_BEOS` is written, carrying both the Unix mode and the
BeOS MIME type when present:

| Offset | Size | Field       | Description                                    |
| ------ | ---- | ----------- | ---------------------------------------------- |
| 0      | 2    | `mode`      | Unix permissions (low 9 bits of `mode_t`)      |
| 2      | 1    | `mime_len`  | Length of MIME type string (0–255)             |
| 3      | N    | `mime_type` | MIME type bytes from BFS `BEOS:TYPE` attribute |

Maximum serialized payload: 258 bytes (3-byte header plus the 255-byte
maximum representable MIME length). The frontend uses a 259-byte buffer for
the header plus a 256-byte temporary MIME string including its terminator.

MIME type is read via `fs_read_attr(fd, "BEOS:TYPE", ...)` on pack and written back via `fs_write_attr(...)` on extraction. If no `BEOS:TYPE` attribute exists, `mime_len` is 0 and no MIME type bytes follow.

Regular-file extraction writes to an exclusive sibling stage. The stage is
published only after the payload checksum and output close succeed; a forced
replacement uses `rename`, while extraction without `-f` uses an atomic
no-clobber link. File timestamps are applied to the stage before publication.
Haiku mode and MIME restoration happens after the verified data file is
published, and failures report partial. Symlink payloads are staged too and
their targets are checked before link publication. Existing symlink components
in the destination path are refused during directory creation; concurrent
parent changes can still race these pathname checks. Archive create, append,
and delete also use same-directory sibling stages. Append copies the original
archive before editing; publication checks that the original target identity
has not changed. Failed archive edits leave the original bytes intact. The CLI
preflights the complete archive with the bounded POSIX path checker and refuses
duplicate paths and file/directory prefix conflicts before creating the output
root. The host-shim `make haiku-host-stage-test` covers these failure paths,
verifies a path-conflict refusal, injects a failed append publication, checks
archive mode preservation across edits, and reads successful append/delete
archives with the Linux reader. Its `fs_attr` shim maps attributes to host
extended attributes and verifies file and directory MIME round trips, including
the 255-byte serialized limit. This does not qualify BFS attribute semantics,
case mapping, timestamp resolution, or native path races.

### Haiku: Text Encoding

Haiku uses UTF-8 natively. No filename conversion is required.

### Haiku: Commands and Options

| Command         | Description                      |
| --------------- | -------------------------------- |
| `create` / `c`  | Create archive (recursive)       |
| `add` / `a`     | Add files to existing archive    |
| `extract` / `x` | Extract archive                  |
| `list` / `l`    | List archive contents            |
| `test` / `t`    | Test archive integrity           |
| `info` / `i`    | Show archive information         |
| `delete` / `d`  | Delete files from archive        |
| `help`          | Show the default simplified help |

### Haiku: Common Options

| Option     | Description                                   |
| ---------- | --------------------------------------------- |
| `-b SIZE`  | Block size: `8k`, `16k`, `32k`, `64k`, `256k` |
| `-0`       | Store only (no compression)                   |
| `-F`       | Use Fluff compression                         |
| `--interchange-profile-1` | Create or append a broadly portable archive |
| `-o PATH`  | Extract to directory                          |
| `-f`       | Force overwrite                               |
| `-n`       | Dry run                                       |
| `-q`/`-qq` | Quiet / silent                                |

### Haiku: Advanced Help

The default `boltpack help` page now hides the secondary compression selectors
and long-form aliases. Run `boltpack help advanced` to show the full standalone
Haiku CLI surface.

| Option              | Description                            |
| ------------------- | -------------------------------------- |
| `-1`, `--boltlite`      | Use BoltLite compression                   |
| `-2`, `--boltlz`    | Use boltlz compression explicitly      |
| `-l N`, `--level N` | Exact level: BoltLZ 1-2; Fluff/zstd 1-9 |
| `--light`           | Fast preset: BoltLZ 1; Fluff/zstd 1    |
| `--standard`        | Default preset: BoltLZ 2; Fluff 8; zstd 3 |
| `--best`            | Highest preset: BoltLZ 2; Fluff/zstd 9 |
| `--store`           | Long-form alias for `-0`               |
| `--fluff`           | Long-form alias for `-F`               |
| `--block-size N`    | Long-form alias for `-b`               |
| `--output PATH`     | Long-form alias for `-o`               |
| `--force`           | Long-form alias for `-f`               |
| `--dry-run`         | Long-form alias for `-n`               |
| `--quiet`           | Long-form alias for `-q`               |
| `--silent`          | Long-form alias for `-qq`              |

---

## RISC OS Frontend

**File:** `src/frontend/riscos/main.c`
**Compiler:** `arm-unknown-riscos-gcc` (GCCSDK cross-compiler from Linux) or `arm-unknown-riscos-cc` (Norcroft native)
**Interface:** Command-line
**Target:** RISC OS 3 and later (ARM)
**Output:** `boltpack,ff8` (filetype 0xFF8 = Absolute executable)

### RISC OS: Build

Preferred release workflow: use the special-SDK compile package and run
`sh scripts/special-sdk/build-sdk.sh riscos`. The direct `make riscos` path
remains available from a full checkout for expert/manual toolchain setups.

Cross-compile from Linux:

```sh
make riscos
```

Or directly:

```sh
arm-unknown-riscos-gcc -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 \
    -DBP_CREATOR_OS=BP_OS_RISCOS -DBP_CREATOR_CPU=BP_CPU_ARM32 \
    -I src/core  \
    src/core/archiver.c src/core/compress.c src/core/boltlite.c \
    src/core/fluff.c src/core/bp_codepage.c \
    src/frontend/riscos/main.c src/core/boltlz.c \
    -o boltpack,ff8
```

The `,ff8` suffix embeds the RISC OS filetype (0xFF8 = Absolute) in the filename, which is the RISC OS convention for storing filetypes on non-RISC OS filesystems.

**GCCSDK setup:**

```sh
# Download from https://www.riscos.info/index.php/GCCSDK
# or install via the packaging at https://gccsdk.riscos.info/
```

### RISC OS: bp_io_t Implementation

This frontend does **not** use POSIX or `<stdio.h>` for archive file I/O. All I/O is performed via RISC OS kernel SWI calls using `<kernel.h>` and `<swis.h>`:

| Callback | Implementation                         |
| -------- | -------------------------------------- |
| `read`   | `OS_GBPB` reason 4 (read sequential)   |
| `write`  | `OS_GBPB` reason 2 (write sequential)  |
| `seek`   | `OS_Args` reason 1 (set file pointer)  |
| `tell`   | `OS_Args` reason 0 (read file pointer) |

Files are opened with `OS_Find` (reason 0x40 for read, 0x80 for write). The `handle` field holds the RISC OS file handle (a small integer, not a pointer). `printf` / `fprintf` are still used for status output, provided by the GCCSDK C runtime.

**SWIs used:**

| SWI            | Number | Purpose                              |
| -------------- | ------ | ------------------------------------ |
| `OS_Find`      | 0x0D   | Open / close files                   |
| `OS_GBPB`      | 0x0C   | Read / write / enumerate directory   |
| `OS_Args`      | 0x09   | Seek / tell / set EOF                |
| `OS_File`      | 0x08   | Stat / write catalogue info / delete |
| `OS_FSControl` | 0x19   | Reason 26: create directory          |

### RISC OS: Directory Recursion

Uses `OS_GBPB` reason 12 to enumerate directory entries in a depth-first walk. Directory entry is emitted before its contents. RISC OS has no symlinks.

### RISC OS: Platform Metadata

After every `BP_TYPE_DATA` and `BP_TYPE_DIR` entry, a `BP_TYPE_META` entry with `platform = BP_OS_RISCOS` is written. The payload is exactly 12 bytes:

| Offset | Size | Field        | Description                           |
| ------ | ---- | ------------ | ------------------------------------- |
| 0      | 4    | `load_addr`  | Load address or filetype-encoded word |
| 4      | 4    | `exec_addr`  | Exec address or timestamp low word    |
| 8      | 4    | `attributes` | RISC OS file attributes               |

**Filetype encoding:** When bits 20–31 of `load_addr` are `0xFFF`, the file carries a datestamp. Bits 19–8 of `load_addr` hold the 12-bit filetype number. The 40-bit centisecond timestamp is split across `load_addr` bits 7–0 (high 8 bits) and all 32 bits of `exec_addr` (low 32 bits). The RISC OS timestamp epoch is 1900-01-01; the offset to the Unix epoch is 220,898,880,000 centiseconds.

**Attribute bits:**

| Bit | Symbol      | Meaning                      |
| --- | ----------- | ---------------------------- |
| 0   | `OA_Read`   | Owner read                   |
| 1   | `OA_Write`  | Owner write                  |
| 2   | `OA_Locked` | Locked (no delete/overwrite) |
| 4   | `PA_Read`   | Public read                  |
| 5   | `PA_Write`  | Public write                 |

On extraction from a non-RISC OS archive, `BP_TYPE_META` entries with a different `platform` are silently skipped; extracted files receive default attributes (owner read/write).
The status-aware completion callback applies RISC OS metadata only after its
payload checksum succeeds. Malformed native payloads and `OS_File` restoration
failures return a partial result instead of being reported as successful.
Regular data outputs are written to a generated sibling stage. Resource-fork
entries are refused before output creation because no collision-safe RISC OS
sidecar convention is defined.
The stage is published only after checksum verification and a successful close.
Forced replacement renames the previous file to a recovery name first and
attempts to restore it if publishing the new file fails. These SWI rename and
recovery paths still require RISC OS runtime testing. The
`make riscos-stage-host-test` target exercises checksum and close failures, successful
publication, no-force refusal, replacement rollback, and refusal to apply
corrupt native metadata through a 32-bit host SWI shim. Output-root and
directory-creation SWI failures now stop or mark extraction partial; host-shim
coverage checks the file-blocks-directory case.

### RISC OS: Path Handling

RISC OS uses `.` as the directory separator. The frontend converts native
Latin-1 path bytes to UTF-8 and maps `.` → `/` when building archive entry
names; extraction converts UTF-8 back to Latin-1 and maps `/` → `.`. Since a
literal dot in an archive component aliases a RISC OS directory boundary, such
components are refused. The checked converters reject reserved RISC OS path
syntax and components over 10 bytes, and path joins refuse truncation. Before
creating the output root, extraction checks the whole archive against bounded
case-fold collision and file/directory-prefix rules. Resource-fork entries
are refused before writes because no collision-safe sidecar convention is
defined. Filing-system-specific collision behavior and extraction still need
target-runtime qualification; directories are created as streaming proceeds.
For normal extraction, all payload checksums are tested before output-root
creation, then checked again while each file is written to its stage.

```text
HostFS:$.Work.myproj   →   Work/myproj
@.myfile/txt           →   myfile/txt
```

### RISC OS: Text Encoding

RISC OS uses Latin-1 (ISO 8859-1). File names are converted to UTF-8 on archive and back on extraction via `bp_local_to_utf8()` / `bp_utf8_to_local()` with `BP_CP_8859_1`.

### RISC OS: Commands and Options

| Command | Description                   |
| ------- | ----------------------------- |
| `c`     | Create archive (recursive)    |
| `a`     | Add files to existing archive |
| `x`     | Extract archive               |
| `l`     | List archive contents         |
| `t`     | Test archive integrity        |
| `i`     | Show archive information      |
| `d`     | Delete files from archive     |

| Option    | Description                                   |
| --------- | --------------------------------------------- |
| `-b SIZE` | Block size: `8k`, `16k`, `32k`, `64k`, `256k` |
| `-0`      | Store only (no compression)                   |
| `-F`      | Use fluff LZ77 + FSE/tANS compression            |
| `-1`      | Use BoltLite compression                          |
| `-q`      | Quiet (progress line only)                    |
| `-qq`     | Silent (errors only)                          |
| `-o PATH` | Extract to directory                          |
| `-f`      | Force overwrite                               |
| `-n`      | Dry run                                       |

---

## OS/2 Frontend

**File:** `src/frontend/os2/main.c`
**Compiler:** Open Watcom v2 (`os2` target) or `os2-gcc` (GCC with EMX/kLIBC)
**Interface:** Command-line
**Target:** IBM OS/2 Warp 3/4 (32-bit), ArcaOS, eComStation

### OS/2: Build

```sh
make os2
```

Or directly (cross-compilation with `os2-gcc`):

```sh
os2-gcc -ansi -Wall -Wextra -O2 \
    -I src/core  \
    src/core/archiver.c src/core/compress.c src/core/bp_codepage.c \
    src/frontend/os2/main.c src/core/boltlz.c \
    -o build/os2/boltpack.exe
```

### OS/2: bp_io_t Implementation

Archive reads and in-place append use the C runtime `FILE *` wrappers. Archive
creation and deletion write to an exclusive sibling stage through OS/2 file
handles, flush and close it, then publish it. The frontend uses 32-bit
`bp_off_t`, so archives are limited to offsets below 2 GiB.

### OS/2: Directory Recursion

Uses the native OS/2 API via `<os2.h>` (`INCL_DOSFILEMGR`):

- `DosFindFirst(mask, &fh, MUST_HAVE_DIRECTORY, &fb, ...)` — open directory search
- `DosFindNext(fh, &fb, sizeof(fb), &count)` — enumerate entries
- `DosFindClose(fh)` — close handle

The `FILEFINDBUF3` buffer provides the filename, attribute byte, and packed date/time. Depth-first walk; directory entry emitted before its contents.

### HPFS Extended Attributes

OS/2's High Performance File System supports extended attributes (EAs). The frontend optionally archives the `.TYPE` EA, which stores the file's MIME-style type as an ASCII string:

- On pack: `DosQueryPathInfo(path, FIL_QUERYEASIZE, ...)` checks for EAs; if `.TYPE` is present it is read into the metadata payload using `EAT_ASCII` (value type 0xFFFD).
- On extract: `DosSetPathInfo(path, FIL_SETEAS, ...)` restores the `.TYPE` EA.

Set `OS2_EA_ENABLED` to `0` to disable EA operations (for FAT file systems or older OS/2 versions).

### OS/2: Platform Metadata

After every `BP_TYPE_DATA` and `BP_TYPE_DIR` entry, a `BP_TYPE_META` entry with `platform = BP_OS_OS2` is written. The payload is 1 byte: the FAT/HPFS attribute flags.

**Attribute byte:**

| Bit | Symbol          | Meaning                              |
| --- | --------------- | ------------------------------------ |
| 0   | `FILE_READONLY` | Read-only                            |
| 1   | `FILE_HIDDEN`   | Hidden                               |
| 2   | `FILE_SYSTEM`   | System                               |
| 5   | `FILE_ARCHIVED` | Archive (modified since last backup) |

### OS/2: Text Encoding

OS/2 uses CP850 (Western Europe) for filenames. File names are converted to UTF-8 on archive and back on extraction via `bp_local_to_utf8()` / `bp_utf8_to_local()` with `BP_CP_850`.

### OS/2: Path Handling

Drive prefixes (`C:\`, `C:`) are stripped. Backslashes are normalised to forward slashes.
Create and delete preserve an existing archive until the replacement stage is
complete. Publication temporarily moves the old archive to a recovery name;
if publishing the stage fails, the frontend attempts to restore that backup.
Append copies the archive to an exclusive sibling stage, appends there, and
publishes by a backup/recovery rename after checking the original status. It
retains OS/2 attribute flags and creation/access timestamps while updating the
last-write time. Handled append failures leave the original archive intact;
interruption between publication renames may leave the original at its recovery
name. These paths have Watcom cross-build coverage, but no OS/2 runtime round
trip has been performed.

### OS/2: Commands and Options

| Command | Description                   |
| ------- | ----------------------------- |
| `c`     | Create archive (recursive)    |
| `a`     | Add files to existing archive |
| `x`     | Extract archive               |
| `l`     | List archive contents         |
| `t`     | Test archive integrity        |
| `i`     | Show archive information      |
| `d`     | Delete files from archive     |

| Option    | Description                                   |
| --------- | --------------------------------------------- |
| `-b SIZE` | Block size: `8k`, `16k`, `32k`, `64k`, `256k` |
| `--interchange-profile-1` | Enforce mini-compatible limits for create/add (BoltLite by default, 16 KiB blocks) |
| `-0`      | Store only (no compression)                   |
| `-F`      | Use fluff LZ77 + FSE/tANS compression            |
| `-1`      | Use BoltLite compression                          |
| `-q`      | Quiet (progress line only)                    |
| `-qq`     | Silent (errors only)                          |
| `-o PATH` | Extract to directory                          |
| `-f`      | Force overwrite                               |
| `-n`      | Dry run                                       |

---

## Plan 9 Frontend

**File:** `src/frontend/plan9/main.c`
**Compiler:** `8c` (i386), `6c` (AMD64), `5c` (ARM) — Plan 9 native C compiler (or `9cc` on 9front)
**Interface:** Command-line
**Target:** Plan 9 Fourth Edition / 9front

### Plan 9: Build

Plan 9 uses a native build system; cross-compilation from Linux is not supported via the Makefile. Build on a Plan 9 or 9front system:

```text
6c -pFVw -D__PLAN9__ -DPLAN9_CPU_X86_64 \
    -I /amd64/include -I /sys/include -I src/core \
    src/core/archiver.c src/core/compress.c src/core/boltlite.c \
    src/core/fluff.c src/core/bp_codepage.c src/core/boltlz.c \
    src/frontend/plan9/main.c
6l -o boltpack *.6
```

The `-p` flag enables the full preprocessor, which this source needs for
conditional compilation. `-D__PLAN9__` selects the native Plan 9 libc path;
the native architecture include directory must precede the APE include path.
Use `8c` / `8l` with `-DPLAN9_CPU_X86 -I /386/include` for i386, or `5c` / `5l`
with `-DPLAN9_CPU_ARM32` and the matching ARM include directory. Plan 9's native
compiler accepts bare `-DNAME` defines rather than `-DNAME=value`; `archiver.c`
detects Plan 9 through `__PLAN9__` and uses the architecture marker to record
the CPU identity. The installed binary conventionally goes in
`/$cputype/boltpack`.

### Plan 9 C Dialect

This frontend uses Plan 9's own C dialect and standard library — it is **not** compatible with POSIX, C99, or any standard compiler. Key differences:

- `#include <u.h>` must come first; defines `vlong`, `ulong`, `uchar`, etc.
- `#include <libc.h>` provides the standard library (`read`, `write`, `seek`, `open`, `close`, `create`, `dirread`, `exits`, etc.)
- Entry point: `void main(int argc, char **argv)` — no `int` return value
- Exit: `exits(nil)` for success, `exits("reason")` for failure; never `exit()`
- I/O is file-descriptor based, not `FILE *`

### Plan 9: bp_io_t Implementation

Backed by Plan 9 file descriptors. A `p9_io_t` struct (with `bp_io_t` as the mandatory first member) holds the `int fd`:

| Callback | Implementation                                            |
| -------- | --------------------------------------------------------- |
| `read`   | `read(fd, buf, (long)count)`                              |
| `write`  | `write(fd, buf, (long)count)`                             |
| `seek`   | `seek(fd, (vlong)offset, origin)` — 0=start, 1=cur, 2=end |
| `tell`   | `seek(fd, 0LL, 1)` (seek by 0 from current = tell)        |

### Plan 9: Directory Recursion

Uses `dirread(fd)` to obtain a `Dir *` array for a directory opened with `open(path, OREAD)`. Each `Dir` entry provides `.name`, `.mode`, `.length`, and `.mtime`. Depth-first walk; directory entry emitted before its contents. Plan 9 has no symlinks (per-process namespace mounts are used instead).

### Plan 9: Platform Metadata

After every `BP_TYPE_DATA` and `BP_TYPE_DIR` entry, a `BP_TYPE_META` entry with `platform = BP_OS_PLAN9` is written. The payload is 2 bytes: the lower 16 bits of `Dir.mode`.

The low 9 bits of `Dir.mode` are the familiar owner/group/others rwx bits.
Archive creation stores these mode bits. Real extraction currently refuses
after archive preflight and before creating output: the frontend has only
pathname-based create/metadata operations and cannot safely anchor or publish
under a selected output directory. Dry-run, listing, integrity testing, create,
and append remain available. Earlier native mode/time restoration results are
historical and do not describe current extraction behavior. Re-enable real
extraction only after safe anchored staging/publication is implemented and
qualified.

### Plan 9: Text Encoding

Plan 9 was the first production operating system to adopt UTF-8, designed by Rob Pike and Ken Thompson in 1992. All filenames are UTF-8 natively; no conversion is required (`BP_CP_UTF8`).

### Plan 9: Commands and Options

| Command | Description                   |
| ------- | ----------------------------- |
| `c`     | Create archive (recursive)    |
| `a`     | Add files to existing archive |
| `x`     | Extract archive               |
| `l`     | List archive contents         |
| `t`     | Test archive integrity        |
| `i`     | Show archive information      |
| `d`     | Delete files from archive     |

`x` currently refuses real extraction for the safety reason above; `-n`
remains available for extraction dry-run.

| Option    | Description                                   |
| --------- | --------------------------------------------- |
| `-b SIZE` | Block size: `8k`, `16k`, `32k`, `64k`, `256k` |
| `-0`      | Store only (no compression)                   |
| `-F`      | Use fluff LZ77 + FSE/tANS compression            |
| `-1`      | Use BoltLite compression                          |
| `-q`      | Quiet (progress line only)                    |
| `-qq`     | Silent (errors only)                          |
| `-o PATH` | Extract to directory                          |
| `-f`      | Force overwrite                               |
| `-n`      | Dry run                                       |

---

## Sinclair QL Frontend

**File:** `src/frontend/ql/main.c`
**Compiler:** `qcc` or `m68k-qdos-gcc` (QDOS/SMSQ toolchain)
**Interface:** Command-line
**Target:** Sinclair QL / SMSQ/E, QPC2, Q68

### QL: Build

Preferred release workflow: use the special-SDK compile package and run
`sh scripts/special-sdk/build-sdk.sh ql`. The direct `make ql` path remains
available from a full checkout for expert/manual SDK setups.

```sh
make ql
```

Or directly:

```sh
mkdir -p build/ql
cd src/frontend/ql
qcc -O \
  -I../../core  \
  ../../core/archiver.c \
  ../../core/compress.c \
  ../../core/boltlite.c \
  ../../core/fluff.c \
  ../../core/bp_codepage.c \
  main.c \
  ../../core/boltlz.c \
  -o ../../../build/ql/boltpack
```

If your SDK exposes `m68k-qdos-gcc` directly instead of `qcc`, substitute that
driver in the same command.

### QL: bp_io_t Implementation

Standard ANSI C `<stdio.h>` wrappers (`fopen`, `fread`, `fwrite`, `fseek`, `ftell`).

### QL: Directory Recursion

QDOS does not provide a POSIX `opendir()`. Directories are traversed by opening a device path ending with `_` (e.g., `win1_mydir_`) with `fopen(path, "rb")` and reading 64-byte `qdirect_t` entries sequentially.

**`qdirect_t` structure (64 bytes, big-endian / M68k native):**

| Offset | Size | Field       | Description                                |
| ------ | ---- | ----------- | ------------------------------------------ |
| 0      | 4    | `d_length`  | File size in bytes                         |
| 4      | 1    | `d_access`  | Access flags (low 3 bits = protection)     |
| 5      | 1    | `d_type`    | File type (0=BASIC, 1=DATA, 2=CODE, 3=REL) |
| 6      | 4    | `d_datalen` | Dataspace for EXEC/REL files               |
| 10     | 4    | —           | Reserved                                   |
| 14     | 2    | `d_szname`  | Name length (0–36, big-endian)             |
| 16     | 36   | `d_name`    | Name (not NUL-terminated, space-padded)    |
| 52     | 4    | `d_update`  | Last update (seconds since 1961-01-01)     |
| 56     | 4    | `d_refdate` | Reference date (same epoch)                |
| 60     | 4    | `d_backup`  | Backup date (same epoch)                   |

No byte-swapping is needed when compiled with `m68k-qdos-gcc` because M68k is big-endian and matches QL hardware.

**QDOS timestamp epoch:** seconds since 1961-01-01. QDOS stores local wall
time and has no timezone field. Supply `--tz-offset MINUTES` on the BoltPack
command line when creating or extracting (for example, `60` for UTC+1 or
`-300` for UTC-5). The offset has no daylight-saving rules; update it when
the machine's effective offset changes. If omitted, zero is used to preserve
the previous UTC-clock assumption. Values outside `-1439..1439` or malformed
values are rejected: creation stores the source time as unknown, and
extraction reports partial for timestamps that cannot be restored. QDOS
dates outside its signed 32-bit range are also unknown/partial. `BP_TZ_OFFSET`
is accepted as a fallback on runtimes that provide C environment variables;
the QL command-line option works without that runtime support and takes
precedence.

**QDOS path conventions:**

| Raw path          | Archive entry name |
| ----------------- | ------------------ |
| `win1_mydir_file` | `mydir/file`       |
| `win1_file`       | `file`             |

Device prefix is stripped; directories identified by paths ending with `_`.

### QL: Platform Metadata

The QL has only a simple 3-bit protection field (`d_access` bits 0–2), which is too limited to be meaningful cross-platform. BoltPack omits that protection metadata — no `BP_TYPE_META` entries are written. Unsupported metadata entries from other platforms are skipped and make extraction report partial.

### QL: Text Encoding

Sinclair QL uses ISO-8859-1 (Latin-1) for filenames. File names are converted to UTF-8 on archive and back on extraction via `bp_local_to_utf8()` / `bp_utf8_to_local()` with `BP_CP_8859_1`.

### QL: Safe Extraction

Before creating the output root, extraction scans the full archive table. It
refuses DATA/DIR names that cannot be represented in Latin-1, exceed QDOS's
36-byte complete relative pathname limit, alias under ASCII or Latin-1
case-folding, or conflict as a file and descendant path. This follows the
documented QDOS case-insensitive naming and path-length behavior described in
[Directories](https://sinclairql.net/djw/docs/articles/Directories.pdf).
Regular file bytes are written to a same-directory stage created with
`O_CREAT|O_EXCL`, then published only after checksum verification and a
successful close. The stage and backup names shrink to fit the remaining bytes
in the 36-byte relative-path allowance. `-f` keeps a sibling backup until
publication succeeds. Host fixtures cover the path-width calculation,
stage-name collision preservation, preflight, checksum preservation, and
rollback recovery. Verified file timestamps are restored through QDOS
[`FS.DATE`](https://dilwyn.qlforum.co.uk/docs/ebooks/C68%20Documentation%20-%20Dave%20Walker.pdf);
directory dates are applied deepest-first after child extraction. Recursive
source enumeration carries file dates from qdirect records; a direct file
argument has no parent record and is stored with unknown time.
Unsupported filing-system date calls or timestamps beyond QDOS's signed range
report partial. The sQLux hostFS fixture verifies QDOS device-prefix
inheritance, underscore path projection, stage-collision handling, `-o`, nested
directories, byte-identical Linux-to-QL extraction, file and directory mtime
round trips with a UTC emulator clock, and successful forced replacement.
Native rollback, crash durability, offset behavior on physical QL clocks,
QDOS protection flags, and physical QL filing systems remain unqualified. The
UTC default and offset arithmetic are covered by the host time-conversion
fixture; sQLux continues to cover zero-offset file and directory timestamps.

### QL: Commands and Options

| Command | Description                   |
| ------- | ----------------------------- |
| `c`     | Create archive (recursive)    |
| `a`     | Add files to existing archive |
| `x`     | Extract archive               |
| `l`     | List archive contents         |
| `t`     | Test archive integrity        |
| `i`     | Show archive information      |
| `d`     | Delete files from archive     |

| Option    | Description                                   |
| --------- | --------------------------------------------- |
| `-b SIZE` | Block size: `8k`, `16k`, `32k`, `64k`, `256k` |
| `-0`      | Store only (no compression)                   |
| `-F`      | Use fluff LZ77 + FSE/tANS compression            |
| `-1`      | Use BoltLite compression                          |
| `-q`      | Quiet (progress line only)                    |
| `-qq`     | Silent (errors only)                          |
| `-o PATH` | Extract to directory                          |
| `-f`      | Force overwrite                               |
| `-n`      | Dry run                                       |

---

## Sharp X68000 (Human68k) Frontend

**File:** `src/frontend/x68k/main.c`
**Compiler:** `m68k-x680x-human-gcc` (GCC cross-compiler for Human68k)
**Interface:** Command-line
**Target:** Sharp X68000 running Human68k

### X68000: Build

Preferred release workflow: use the special-SDK compile package and run
`sh scripts/special-sdk/build-sdk.sh x68k`. The direct `make x68k` path
remains available from a full checkout for expert/manual SDK setups.

```sh
make x68k
```

Or directly:

```sh
m68k-x680x-human-gcc -ansi -pedantic -Wall -Wextra -Wno-unused-function -O2 \
    -I src/core  \
    src/core/archiver.c src/core/compress.c src/core/boltlite.c \
    src/core/bp_codepage.c \
    src/frontend/x68k/main.c src/core/boltlz.c \
    -o build/x68k/boltpack.x
```

### X68000: bp_io_t Implementation

Standard ANSI C `<stdio.h>` wrappers (`fopen`, `fread`, `fwrite`, `fseek`, `ftell`).

### X68000: Directory Recursion

Uses Human68k `_dos_files()` / `_dos_nfiles()` from `<sys/dos.h>`, analogous to MS-DOS `findfirst`/`findnext`. The find structure provides filename, attribute byte, and a packed DOS date/time word. Recursive path joins are checked against the frontend's 256-byte path limit. `create` and `add` walk the selected source trees before opening or editing the archive and refuse unsupported names or overlong descendants before modification.

**DOS packed date/time format** (same as MS-DOS, Win16, and OS/2):

- Date word: bits 15–9 = year − 1980, bits 8–5 = month, bits 4–0 = day
- Time word: bits 15–11 = hour, bits 10–5 = minute, bits 4–0 = second / 2

**File attribute bits:**

| Symbol      | Bit | Meaning            |
| ----------- | --- | ------------------ |
| `FA_RDONLY` | 0   | Read-only          |
| `FA_HIDDEN` | 1   | Hidden             |
| `FA_SYSTEM` | 2   | System             |
| `FA_LABEL`  | 3   | Volume label       |
| `FA_SUBDIR` | 4   | Directory          |
| `FA_ARCH`   | 5   | Archive (modified) |

### X68000: Path Handling

Human68k uses `\` as the path separator. The frontend strips drive prefixes and maps `\` → `/` for archive entry names. On extraction, `/` is mapped back to `\`. Before it creates the output root, extraction converts and indexes the complete primary-entry table, folds ASCII case, and refuses duplicate paths, file/directory prefix conflicts, and joined paths that exceed the 256-byte target buffer. The bounded index fails closed if it reaches its node, key-byte, or allocation limit. This host-shim check does not qualify non-ASCII Human68k collation or concurrent path changes.

```text
A:\WORK\MYFILE.TXT   →   WORK/MYFILE.TXT
MYFILE.TXT           →   MYFILE.TXT
```

### X68000: Platform Metadata

Human68k uses DOS-style attributes similar to Win16 and OS/2. BoltPack stores
the portable FAT attribute envelope for files and directories. File attributes
are applied after payload verification; directory attributes are deferred
until children are written. Foreign native metadata records are skipped and
reported as partial.

Human68k's packed file dates are local wall time, while format 6 stores UTC.
Set `BP_TZ_OFFSET` to the source/destination offset in minutes east of UTC
(for example, `60` for UTC+1) when creating or extracting. Without a valid
offset, creation records the time as unknown and extraction of a known time
reports partial. Regular-file date restoration uses `_dos_filedate` after
payload verification and before publication. DOS/FAT dates have two-second
resolution; odd-second timestamps round down and report partial. Directory
dates are not restored yet. These behaviors have host-stub coverage but still
need qualification on Human68k itself.

Create, append, and delete build a unique sibling archive stage. Append first
copies the original archive to that stage; all three operations publish only
after successful archive finalization and close. Replacements preserve the
target's portable FAT attributes and use a sibling backup so a failed stage
rename can restore the prior archive. Host-shim failure injection verifies
that failed create/add/delete edits preserve archive bytes, failed publication
rolls back, and temporary files are cleaned. The target's rename and exclusive
create semantics still need native qualification.

### X68000: Text Encoding

Human68k uses Shift-JIS for filenames. ASCII, half-width kana, and standard JIS X 0208 double-byte characters are converted to UTF-8 on archive and back on extraction via `bp_local_to_utf8()` / `bp_utf8_to_local()` with `BP_CP_SJIS`. Undefined pairs and CP932 vendor extensions are refused. Path scans skip valid Shift-JIS pairs, including trail byte `0x5c`, when locating separators. Unit tests round-trip all 6,879 mapped JIS X 0208 pairs; the X68000 host DOS-call shim also checks Linux extraction and malformed/unmapped names. The table is reproducible with `scripts/generate_sjis_table.py`. Native Human68k path and encoding behavior still needs target qualification.

### X68000: Commands and Options

Identical to the Linux frontend.

---

## AmigaOS-Family Thin Wrappers

AmigaOS 4.x, MorphOS, and AROS are each a thin wrapper around `src/frontend/amiga/main.c`. Each file defines `BP_CREATOR_OS` and `BP_CREATOR_CPU`, then `#include`s the AmigaOS 68k frontend directly. The underlying AmigaDOS API (`Lock`, `Examine`, `SetProtection`, `SetComment`, `MakeLink`, `opendir`/`readdir`) is identical across all three platforms.

| Frontend    | Source file                    | `BP_CREATOR_OS`   | CPU    | Compiler                    | Hardware                         |
| ----------- | ------------------------------ | ----------------- | ------ | --------------------------- | -------------------------------- |
| AmigaOS 4.x | `src/frontend/amigaos4/main.c` | `BP_OS_AMIGA_PPC` | PPC32  | `ppc-amigaos-gcc` (adtools) | PPC hardware (G4, 603e)          |
| MorphOS     | `src/frontend/morphos/main.c`  | `BP_OS_MORPHOS`   | PPC32  | `ppc-morphos-gcc -noixemul` | Pegasos, Efika, G4/G5 Mac        |
| AROS        | `src/frontend/aros/main.c`     | `BP_OS_AROS`      | X86_64 | `aros-x86_64-gcc`           | x86/x86-64/ARM (default: x86-64) |

Preferred release workflow: use the special-SDK compile package helper:
`sh scripts/special-sdk/build-sdk.sh amigaos4`,
`sh scripts/special-sdk/build-sdk.sh morphos`,
`sh scripts/special-sdk/build-sdk.sh aros`, or
`sh scripts/special-sdk/build-sdk.sh all`.

These compile packages also include a root-level `build-notes.md` with the
matching toolchain requirement summary and build command.

Direct expert commands from a full checkout remain `make amigaos4`,
`make morphos`, and `make aros`.

The compile packages for these wrappers also stage `src/frontend/amiga/`
because each wrapper `main.c` includes the shared Amiga frontend directly.

**AmigaOS 4.x:** Use the adtools cross-compiler (`https://github.com/sba1/adtools`). Add `-athread=native` to the link stage.

**MorphOS:** The `-noixemul` flag (same as the 68k frontend) links against the native clib instead of the ixemul.library Unix emulation layer, producing a self-contained binary.

**AROS:** Override CPU with `-DBP_CREATOR_CPU=BP_CPU_X86` (32-bit x86 port) or `-DBP_CREATOR_CPU=BP_CPU_ARM32` (ARM port) as needed.

---

## POSIX Thin-Wrapper Frontends

These frontends define `BP_CREATOR_OS` and `BP_CREATOR_CPU`, then `#include "../linux/main.c"` directly. All POSIX I/O, directory recursion, symlink handling, and `BP_TYPE_META` permission archiving are identical to the Linux frontend. No additional metadata handling is added.

Extraction uses directory-relative no-follow operations on Linux, FreeBSD,
NetBSD, OpenBSD, GNU Hurd, Solaris 11, illumos, and z/OS builds. The AIX wrapper
selects that path when the target compiler defines `_AIX73`; older or
unidentified AIX builds refuse extraction before output creation. QNX also
refuses extraction because its resource-manager model cannot guarantee safe
multi-component traversal. Other thin wrappers retain path-based checks and
require native build/runtime qualification.

| Frontend     | Source file                    | `BP_CREATOR_OS`  | Default CPU     | Compiler                     | Notes                                                 |
| ------------ | ------------------------------ | ---------------- | --------------- | ---------------------------- | ----------------------------------------------------- |
| FreeBSD      | `src/frontend/freebsd/main.c`  | `BP_OS_FREEBSD`  | `BP_CPU_X86_64` | `freebsd-gcc`                | Override CPU with `-DBP_CREATOR_CPU=` for other ports |
| NetBSD       | `src/frontend/netbsd/main.c`   | `BP_OS_NETBSD`   | `BP_CPU_X86_64` | `netbsd-gcc`                 | Many ports; override CPU as needed                    |
| OpenBSD      | `src/frontend/openbsd/main.c`  | `BP_OS_OPENBSD`  | `BP_CPU_X86_64` | `openbsd-gcc`                |                                                       |
| Solaris      | `src/frontend/solaris/main.c`  | `BP_OS_SOLARIS`  | `BP_CPU_X86_64` | `sparc-sun-solaris2-gcc`     | Override with `BP_CPU_SPARC` or `BP_CPU_SPARC64`      |
| HP-UX        | `src/frontend/hpux/main.c`     | `BP_OS_HPUX`     | `BP_CPU_PARISC` | `hppa-hp-hpux-gcc`           | Override with `BP_CPU_IA64` for Itanium               |
| AIX          | `src/frontend/aix/main.c`      | `BP_OS_AIX`      | `BP_CPU_PPC64`  | `powerpc-ibm-aix-gcc`        | `_AIX73` selects dirfd extraction; older/unidentified AIX refuses extraction; override with `BP_CPU_PPC32` for 32-bit AIX |
| IRIX         | `src/frontend/irix/main.c`     | `BP_OS_IRIX`     | `BP_CPU_MIPS64` | `mips-sgi-irix6.5-gcc`       | MIPS hardware only (R4000–R16000)                     |
| illumos      | `src/frontend/illumos/main.c`  | `BP_OS_SOLARIS`  | `BP_CPU_X86_64` | `x86_64-unknown-illumos-gcc` | OpenIndiana, OmniOS, SmartOS                          |
| GNU Hurd     | `src/frontend/hurd/main.c`     | `BP_OS_HURD`     | `BP_CPU_X86_64` | `hurd-gcc`                   | Override with `BP_CPU_X86` for 32-bit                 |
| Tru64        | `src/frontend/tru64/main.c`    | `BP_OS_TRU64`    | `BP_CPU_ALPHA`  | `alpha-dec-tru64-gcc`        | End-of-life; Alpha hardware only                      |
| SCO          | `src/frontend/sco/main.c`      | `BP_OS_SCO`      | `BP_CPU_X86`    | `sco-gcc`                    | End-of-life; x86 only                                 |
| QNX Neutrino | `src/frontend/qnx/main.c`      | `BP_OS_QNX`      | `BP_CPU_X86`    | `qcc` (QNX SDP)              | Override with `BP_CPU_X86_64` or `BP_CPU_ARM64`; listing/test/dry-run work, extraction is refused until safe traversal is available |
| NeXTSTEP     | `src/frontend/nextstep/main.c` | `BP_OS_NEXTSTEP` | `BP_CPU_M68K`   | NeXT `cc`                    | End-of-life; also x86/SPARC/PA-RISC                   |

**Build commands:** `make freebsd`, `make netbsd`, `make openbsd`, `make solaris`, `make hpux`, `make aix`, `make irix`, `make illumos`, `make hurd`, `make tru64`, `make sco`, `make qnx`

---

## z/OS (USS) Frontend

**File:** `src/frontend/zos/main.c`
**Compiler:** IBM Open XL C/C++ (`xlc`) or z/OS XL C/C++ (`cc`)
**Interface:** Command-line
**Target:** z/OS 3.1 with the June 2024 refresh or later (z/Architecture mainframe)

This is a thin wrapper around `src/frontend/linux/main.c`. z/OS 3.1 with the June 2024 refresh or later provides the POSIX.1-2008 directory-relative APIs used by extraction, including [`openat`](https://www.ibm.com/docs/en/zos/3.1.0?topic=functions-openat-open-file-relative-directory-file-descriptor), [`fstatat`](https://www.ibm.com/docs/en/zos/3.1.0?topic=lf-fstatat-fstatat64-get-file-status-information-relative-directory-file-descriptor), and [`fdopendir`](https://www.ibm.com/docs/en/zos/3.1.0?topic=functions-fdopendir-opens-directory-associated-file-descriptor). IBM added `fdopendir` in the z/OS 3.1 June 2024 refresh ([change summary](https://www.ibm.com/docs/en/zos/3.2.0?topic=sc-summary-changes-zos-31-174)). `_XPLATFORM_SOURCE` enables the `O_DIRECTORY` and `O_NOFOLLOW` flags, as documented in the [`open` reference](https://www.ibm.com/docs/en/zos/3.1.0?topic=functions-open-open-file). The USS subsystem also provides `lstat`, `readlink`, `opendir`/`readdir`, `symlink`, `fseeko`, and `ftello`.

**Important notes:**

- Archive files must be opened in binary mode (`"rb"` / `"wb"`) to prevent z/OS from performing automatic EBCDIC ↔ ASCII translation of archive bytes.
- The shared POSIX frontend passes filename bytes through without codepage conversion. The documented command below does not select an ASCII execution mode, and the z/OS filename encoding has not been qualified. Do not assume that this frontend preserves Unicode names or interoperates with other platforms.
- IBM's Enhanced ASCII mode has to be selected deliberately and its conversion applies only to the portable character subset; it is not general UTF-8 filename conversion. Any support claim needs a native build and USS round-trip test with an explicitly selected compiler/runtime mode. See IBM's [`-qascii` compiler option](https://www.ibm.com/docs/en/open-xl-c-cpp-zos/2.1.0?topic=options-qascii) and [Enhanced ASCII limitations](https://www.ibm.com/docs/en/zos/2.5.0?topic=pages-enhanced-ascii).
- Cross-compilation from Linux is not practical without a licensed IBM toolchain. Build natively on a z/OS USS host.

**Build (on a z/OS USS host):**

```sh
xlc -ansi -O2 \
    -I src/core  \
    src/core/archiver.c src/core/compress.c src/core/bp_codepage.c \
    src/frontend/zos/main.c src/core/boltlz.c \
    -o boltpack
```

---

## GUI Frontends

BoltPack provides native GUI frontends for seven platforms. Each GUI is a
separate executable that links the archiver core statically (or, for the
C# variant, dynamically via a shared DLL). All GUI frontends share a common
UI model:

- **Menu bar** with Archive (Open, Extract, Close, Create, Exit/Quit) and Help
  (Documentation, About) menus
- **File list** showing six columns: Name, Original Size, Packed Size, Ratio,
  Method, Date
- **Progress indicator** during extract and create operations with cancel support
- **Drag-and-drop** support (all platforms except Atari GEM)
- **Auto-classification** of dropped/opened files: `.bolt`/`.blt` files are
  opened as archives; other files and directories trigger archive creation

Shared constants (application name, version, copyright, and a documentation
help string) live in `src/frontend/gui_common/help_text.h`, included by every
GUI frontend.

### Shared DLL Wrapper

**File:** `src/core/boltpack_dll.c`

The C# WinForms GUI cannot link the archiver core statically. Instead, it
calls into `boltpack_core.dll` via P/Invoke. The DLL wrapper in
`boltpack_dll.c` exposes the archiver API as flat C functions with
`__declspec(dllexport)` linkage:

| DLL Function                 | Wraps                    |
| ---------------------------- | ------------------------ |
| `bp_dll_archive_info`        | `bp_archive_info`        |
| `bp_dll_archive_list`        | `bp_archive_list`        |
| `bp_dll_archive_extract_all` | `bp_archive_extract_all` |
| `bp_dll_archive_extract_all_ex` | `bp_archive_extract_all_ex` |
| `bp_dll_archive_create`      | `bp_archive_create`      |
| `bp_dll_archive_create_profile` | `bp_archive_create_profile` |
| `bp_dll_archive_add_entry`   | `bp_archive_add_entry`   |
| `bp_dll_archive_add_entry_ex` | `bp_archive_add_entry_ex` |
| `bp_dll_archive_add_fat_metadata` | portable FAT metadata writer |
| `bp_dll_archive_alloc/free`  | correctly sized managed archive context |
| `bp_dll_archive_finalize`    | `bp_archive_finalize`    |
| `bp_dll_archive_close`       | `bp_archive_close`       |
| `bp_dll_fopen`               | `fopen` with mode string |
| `bp_dll_fopen_w`             | `_wfopen` with Unicode paths |
| `bp_dll_fopen_w_exclusive`   | exclusive Unicode sibling-stage creation |
| `bp_dll_fclose`              | `fclose`                 |
| `bp_dll_fclose_checked`      | flush, commit, and checked close |
| `bp_dll_fseek_start`         | `fseek` with `SEEK_SET`  |
| `bp_dll_compress_init`       | `bp_compress_init`       |

The DLL also provides its own `bp_io_t` callback implementations
(`dll_file_read`, `dll_file_write`, `dll_file_seek`, `dll_file_tell`) that
wrap `<stdio.h>` functions, allowing the C# side to obtain a fully wired
`bp_io_t` from a `FILE *` handle.

### GUI Build Targets

All GUI frontends are built from the project root:

```sh
make gui-all          # Build all GUI frontends
make linux-gui        # Linux FLTK GUI only
make win32-fltk-gui   # Windows FLTK GUI only (32-bit)
make win64-fltk-gui   # Windows FLTK GUI only (64-bit)
make win32-fltk-installer  # Windows FLTK GUI installer + TC plugin ZIP (32-bit)
make win64-fltk-installer  # Windows FLTK GUI installer + TC plugin ZIP (64-bit)
make win32-gui        # Windows NT Win32 GUI (32-bit)
make win64-gui        # Windows NT Win32 GUI (64-bit)
make win32-gui-dll    # boltpack_core.dll for C# (32-bit)
make win64-gui-dll    # boltpack_core.dll for C# (64-bit)
make macosx-gui       # macOS Cocoa GUI (ARM64)
make atari-gui        # Atari GEM GUI only (build/atari/boltpack.prg)
make amiga            # Amiga CLI + GadTools GUI release build
make amiga-gui        # Amiga GadTools GUI only (build/amiga/boltpack-gui)
make haiku-gui        # Haiku BeAPI GUI (x86-64)
```

The C# WinForms project is built separately with `dotnet build`:

```sh
cd src/frontend/win_gui_net
dotnet build -c Release BoltPack.csproj
```

The resulting `BoltPack.exe` expects `boltpack_core.dll` in the same directory.

---

### Linux FLTK GUI

**File:** `src/frontend/linux_gui/main.cpp`
**Toolkit:** FLTK 1.4 (Fast Light Toolkit)
**Language:** C++
**Target:** Linux / X11

The Linux GUI uses FLTK 1.4 for a lightweight, dependency-minimal interface.

| Feature            | Implementation                                           |
| ------------------ | -------------------------------------------------------- |
| File list          | Custom `FileTable` widget (Fl_Widget subclass)           |
| Drag-and-drop      | `FL_PASTE` events; URI text parsing                      |
| File dialogs       | `fl_file_chooser()` (FLTK native)                        |
| Progress           | Modal window with `Fl_Progress` bar, file name, count    |
| Keyboard shortcuts | Ctrl+O, Ctrl+N, Ctrl+E, Ctrl+W, Ctrl+Q                   |
| Linking            | Static to archiver core; dynamic to FLTK (`fltk-config`) |

The file list is a custom widget with six columns, draggable header dividers,
alternating row backgrounds, a header row, mouse-wheel scrolling, and a
horizontal scrollbar for wide column layouts.

Compression defaults, window geometry, and custom column widths are stored in
`gui.conf` under `$XDG_CONFIG_HOME/boltpack/` or `~/.config/boltpack/`. Use
`Settings > Reset GUI Settings...` to restore the default window size,
position, compression method, and file-list columns.

When Fluff is selected in the create dialog, the GUI also exposes a
`Use Multi-threaded Fluff` checkbox. That preference is stored in `gui.conf`
as `fluff_mt`, and the standard `make linux-gui` build links the fast-fluff
OpenMP path so the checkbox calls `bp_set_fast_fluff()` during archive
creation instead of being a no-op.

The create dialog also offers Interchange Profile 1, stored in `gui.conf` as
`interchange_profile`. It writes BoltLite archives with 16 KiB blocks under
the profile's entry-count, entry-size, and total-archive limits, making them
compatible with the mini readers. Saved archives are written to a sibling
stage and replace the selected path only after finalization, flush, and close
succeed; an existing destination must be a regular file and remain unchanged
while creation runs. Replacement retains its owner, group, and permission bits;
ACLs and extended attributes are not copied to the staged file.

Build dependencies: `libfltk-dev`, `libm`.

`make CC=gcc linux-gui-interoperability-test` runs Xvfb interaction checks
through the production create, open, and extract dialogs. The CLI verifies and
extracts the GUI-created archive; the runner also extracts a nested file and
selects two entries with the same leaf name for flattened extraction. Each
restored payload is compared with the pinned size and SHA-256 values, and the
nested file's and directory's modes and timestamps are checked against the
manifest, including deferred directory metadata restoration. The test
also selects duplicate leaf names while a destination file of the same name
already exists and verifies that the old file survives while new outputs get
distinct names. It also cancels extraction of a large stored entry over an
existing file and verifies the old bytes survive with no leftover staging
files. A checksum-corrupted stored entry is also extracted over an existing
file; the test verifies the original survives and the stage is removed. The
test requires `xvfb-run` and `xdotool`.

---

### Windows FLTK GUI

**File:** `src/frontend/win_gui_fltk/main.cpp`
**Toolkit:** FLTK 1.4 (Fast Light Toolkit)
**Language:** C++
**Target:** Windows (MinGW-w64 cross-build, 32-bit and 64-bit)

This frontend is the newer cross-built Windows GUI. It shares the same broad UI
model as the Linux FLTK frontend, but uses native Windows dialogs and extra
Win32 integration for Explorer drag-and-drop, file association workflows, and
installer packaging.

| Feature            | Implementation                                                     |
| ------------------ | ------------------------------------------------------------------ |
| File list          | Custom `FileTable` widget (Fl_Widget subclass)                     |
| Drag-and-drop      | FLTK DND plus native `WM_DROPFILES` fallback for elevated launches |
| File dialogs       | `Fl_Native_File_Chooser` on top of native Windows dialogs          |
| Progress           | Modal FLTK progress window with cancel                             |
| Keyboard shortcuts | Ctrl+O, Ctrl+N, Ctrl+E, Ctrl+W, Ctrl+Q                             |
| Linking            | Static to archiver core and FLTK; MinGW-w64 Windows GUI executable |

Build targets:

```sh
make win32-fltk-gui
make win64-fltk-gui
make win32-fltk-installer
make win64-fltk-installer
```

Artifacts:

- `build/win32-fltk-gui/boltpack-gui.exe`
- `build/win64-fltk-gui/boltpack-gui.exe`
- `build/win32-fltk-gui/boltpack-<version>-win32-setup.exe`
- `build/win64-fltk-gui/boltpack-<version>-win64-setup.exe`

Like the Linux FLTK frontend, the Windows create dialog exposes a
`Use Multi-threaded Fluff` checkbox when Fluff is selected. The standard
`win32-fltk-gui`, `win64-fltk-gui`, and installer targets compile and link the
fast-fluff/OpenMP path, so the checkbox feeds `bp_set_fast_fluff()` rather
than only changing GUI state.

The create dialog also offers Interchange Profile 1, stored in `gui.conf` as
`interchange_profile`. It writes BoltLite archives with 16 KiB blocks under
the mini-compatible entry-count, entry-size, and serialized-size limits.
Archive creation writes a sibling stage and only replaces the selected file
after finalization, flush, and close succeed. Existing destinations are
rechecked before publication; Windows `ReplaceFile` preserves destination
metadata when replacing an existing archive.

#### Unsigned test builds on Windows

The FLTK GUI binaries and NSIS installers produced by the standard Makefile
targets are unsigned by default. On Windows 10 and Windows 11, freshly built
unsigned executables can trigger SmartScreen or Defender warnings even when the
binary is clean, especially when the file is new, has no download/signing
reputation, or is rebuilt frequently during development.

For local testing on your own systems, signing is not required. Rebuilding the
program changes the executable, so a later signing step would apply only to the
specific release artifact you intend to distribute. If BoltPack starts being
distributed to other users, sign the final `boltpack-gui.exe` and the final
setup EXE after it is built and before release.

---

### Windows Win32 GUI

**Files:** `src/frontend/win_gui/main.c`, `resource.h`, `resource.rc`
**Toolkit:** Win32 API + Common Controls
**Language:** C89
**Target:** Windows 95 and later

A pure Win32 GUI that compiles as ANSI C. No MFC, ATL, or C++ runtime
required.

| Feature            | Implementation                                              |
| ------------------ | ----------------------------------------------------------- |
| File list          | Win32 ListView in Details view (6 columns)                  |
| Drag-and-drop      | `WS_EX_ACCEPTFILES` + `WM_DROPFILES`                        |
| File dialogs       | `GetOpenFileName` / `GetSaveFileName` / `SHBrowseForFolder` |
| Progress           | Modeless dialog with progress bar control and cancel        |
| Keyboard shortcuts | Accelerator table: Ctrl+O, Ctrl+E, Ctrl+W, Ctrl+N           |
| Linking            | Static to archiver core                                     |

Resources (`resource.rc`) define the menu bar, accelerator table, About
dialog (220x120), progress dialog (280x100), documentation dialog
(resizable 340x280), version info block, and application icon.

Text encoding: UTF-8 to ANSI code page conversion for Windows 9x
compatibility.

The Archive menu's `Interchange Profile 1` check applies to files created
through the menu or drag-and-drop. The profile selects BoltLite with 16 KiB
blocks and enforces mini-reader entry-count, entry-size, and serialized-size
limits. Creation strictly round-trips source names through the active Windows
code page before writing UTF-8 archive names. It writes a same-directory stage
and publishes only after finalization, flush, commit, and close succeed. On
Windows versions with `ReplaceFile`, replacement keeps the destination's
attributes and ACLs; the compatibility fallback uses recovery renames. Source
reparse points are refused rather than followed into their targets. File-dialog
and drag-and-drop creation preflight case-insensitive duplicate names and
file/directory path conflicts, bounded at 65,535 primary entries/path nodes
and 16 MiB of path keys. Unicode normalization collisions and source-tree
changes during creation remain unqualified.

Link libraries: `comctl32`, `comdlg32`, `shell32`, `user32`, `gdi32`.

---

### Windows .NET WinForms GUI

**Directory:** `src/frontend/win_gui_net/`
**Toolkit:** WinForms (.NET Framework 4.8)
**Language:** C#
**Target:** Windows XP SP3+ (with .NET 4.8 runtime)

A managed C# GUI that calls the archiver through `boltpack_core.dll` via
P/Invoke. This provides a modern Windows experience with visual styles while
reusing the same C89 archiver core.

Archive names are decoded and encoded as strict UTF-8; disk paths use the
Unicode CRT entry points. Extraction preflights Windows path components,
reserved names, case-insensitive and normalization collisions, existing
reparse points, and file/directory-prefix conflicts. Regular files use
exclusive sibling stages and are published only after checksum verification
and successful close. The GUI restores representable UTC modification times
and portable FAT flags, defers directory metadata until children finish, and
reports unsupported links, resource forks, POSIX modes, and metadata as
partial. Archive creation also uses an exclusive sibling stage, captures
portable FAT flags and explicit timestamp validity, and replaces the selected
archive only after successful finalization and close. Path-based checks retain
a race with concurrent parent substitution.

The Archive menu's Interchange Profile 1 check applies to new archives. It
selects BoltLite with 16 KiB blocks and enforces mini-reader entry-count,
entry-size, and serialized-size limits through the DLL profile-create wrapper.
The archive is still published through the existing exclusive sibling stage.

The Win32 and Win64 DLL variants cross-build, but the .NET project has not been
compiled or run in this environment because no .NET compiler or Windows runtime
is available.

| File               | Purpose                                                 |
| ------------------ | ------------------------------------------------------- |
| `MainForm.cs`      | Main window, menu, ListView, drag-and-drop, pack/unpack |
| `NativeMethods.cs` | P/Invoke declarations, struct mirrors, constants        |
| `ProgressForm.cs`  | Modal progress dialog with bar and cancel               |
| `AboutForm.cs`     | About dialog                                            |
| `DocForm.cs`       | Documentation viewer (multiline TextBox)                |
| `Program.cs`       | Entry point; passes command-line args to `MainForm`     |
| `BoltPack.csproj`  | .NET 4.8 WinExe project                                 |

| Feature            | Implementation                                              |
| ------------------ | ----------------------------------------------------------- |
| File list          | `ListView` in Details view (6 columns)                      |
| Drag-and-drop      | `AllowDrop = true`; `DragEnter` / `DragDrop` events         |
| File dialogs       | `OpenFileDialog` / `SaveFileDialog` / `FolderBrowserDialog` |
| Progress           | Modal `ProgressForm` with `ProgressBar` and cancel          |
| Keyboard shortcuts | Ctrl+O, Ctrl+E, Ctrl+W, Ctrl+N, Ctrl+Q, F1                  |
| Linking            | Dynamic to `boltpack_core.dll` via P/Invoke                 |

P/Invoke mirror structs: `BpUint64`, `BpArchiveInfo`, `BpIo`, `BpEntry`, and
`BpProgress`. Delegate types include list, progress, status-aware extraction,
and managed output writing. All functions use the `Cdecl` calling convention.

---

### macOS Cocoa GUI

**File:** `src/frontend/macosx_gui/main.m`
**Toolkit:** Cocoa AppKit
**Language:** Objective-C (with ARC)
**Target:** macOS 10.9 Mavericks and later (Intel + Apple Silicon)

This frontend is not intended for PowerPC Mac OS X. For G4/G5 systems, use the
CLI frontend and the `macosx-ppc` native source kit instead.

| Feature            | Implementation                                                                               |
| ------------------ | -------------------------------------------------------------------------------------------- |
| File list          | `NSTableView` with resizable columns and horizontal scroll                                   |
| Drag-and-drop      | `NSDraggingDestination` protocol; `file://` URL pasteboard                                   |
| File dialogs       | `NSOpenPanel` / `NSSavePanel`                                                                |
| Progress           | Sheet-attached `BPProgressPanel` (`NSWindow`) with cancel                                    |
| Validation         | `bp_archive_test_progress()` with live entry progress                                        |
| Preferences        | `NSUserDefaults` for window frame, columns, compression, multi-threaded Fluff, and Interchange Profile 1 |
| Keyboard shortcuts | Cmd+O, Cmd+E, Cmd+W, Cmd+T, Cmd+I, Cmd+N, Cmd+Q                                              |
| Linking            | Static to archiver core                                                                      |

Classes:

- `BPAppDelegate` — application lifecycle, menu bar construction
- `BPMainWindow` — main window, table data source, drag destination
- `BPProgressPanel` — progress sheet with `NSProgressIndicator`

Extract callbacks are implemented as static C functions
(`cocoa_extract_open`, `cocoa_extract_done`, `cocoa_extract_progress`) to
avoid Objective-C block / C function pointer incompatibilities. A
`cocoa_extract_ctx` struct carries the destination path and progress panel
reference.

Recent parity updates brought this frontend in line with the Linux FLTK GUI:

- Archive menu now includes `Extract Selected`, `Validate`, and `Information`
- Settings menu now includes `Reset GUI Settings...`
- Create flow now asks `Files` vs `Folder` first and lets the user choose a
  compression method and level
- The compression sheet also persists a `Use Multi-threaded Fluff` checkbox
  when Fluff is selected
- The compression sheet offers a persisted Interchange Profile 1 checkbox;
  when selected, new archives use BoltLite with 16 KiB blocks and mini-reader
  entry-count, entry-size, and serialized-size limits
- Validation shows current entry name, current entry index, file/directory
  totals, and a scrollable results window

When that checkbox is enabled, the create path calls `bp_set_fast_fluff()`.
The native helper and direct modern GUI targets compile `src/core/fluff_fast.c`
with OpenMP support so the toggle is live on supported toolchains.

Built via `arm64-apple-macosx11-clang` with `-ObjC -fobjc-arc -framework Cocoa`.

### macOS Legacy Cocoa GUI

**Files:** `src/frontend/macosx_legacy_gui/main.m`, `src/frontend/macosx_gui/main.m`
**Toolkit:** Cocoa AppKit
**Language:** Objective-C (non-ARC legacy mode)
**Target:** Mac OS X 10.5 Leopard on PowerPC, Mac OS X 10.5 through 10.8 on Intel

This is a separate pre-10.9 app line built from the same shared implementation
file as the modern Cocoa frontend. The wrapper frontend defines
`BP_LEGACY_COCOA=1`, which switches the shared code to Leopard-era Objective-C
syntax and older AppKit APIs such as classic pasteboard access, legacy window
style constants, non-block completion handling, and manual memory management.

Use `sh scripts/macos/build-native.sh legacy-ppc` for Leopard PowerPC builds
and `sh scripts/macos/build-native.sh legacy-intel` for Intel 10.5-10.8 builds.

---

### Atari GEM GUI

**File:** `src/frontend/atari_gui/main.c`
**Toolkit:** GEM/AES (windowing) + VDI (drawing)
**Language:** C89
**Target:** Atari ST/STE/TT/Falcon, TOS 1.04+

The only GUI frontend without drag-and-drop support, because the GEM desktop
does not provide a standard drag-and-drop protocol.

| Feature            | Implementation                                           |
| ------------------ | -------------------------------------------------------- |
| File list          | Custom VDI text drawing (`v_gtext`) in a GEM window      |
| Drag-and-drop      | Not supported (use menus)                                |
| File dialogs       | `fsel_exinput()` (GEM file selector)                     |
| Progress           | In-window progress panel during operations               |
| Keyboard shortcuts | Ctrl+O, Ctrl+E, Ctrl+W, Ctrl+N, Ctrl+Q (scan code based) |
| Linking            | Static to archiver core; links `-lgem`                   |

The menu bar is built programmatically as an `OBJECT` tree (no RSC resource
file required). Menu items use `G_IBOX`, `G_TITLE`, `G_BOX`, and
`G_STRING` object types. The event loop uses `evnt_multi` with
`MU_MESAG | MU_KEYBD`.

The file list is drawn with VDI text calls at fixed column positions.
Line height is 16 pixels; entry storage is allocated dynamically up to 2048
rows. Scrolling is handled via `WM_ARROWED` messages.

Cross-compiled with `m68k-atari-mint-gcc`. The `-pedantic` flag is omitted
because GEM headers are not strict C89 compliant.

Archive creation strictly converts names to UTF-8, rejects names outside the
Atari 8.3 ASCII profile and case-folded or file/directory-prefix collisions
(bounded at 1,024 names and 32 KiB), and stages the complete archive beside
the destination. It publishes after finalization and close. Replacing an
existing archive preserves its non-read-only TOS attributes and refuses a
read-only target.
Created file and directory entries carry representable timestamps, native TOS
attributes, and the portable FAT fallback, including empty directories.

The Compression menu has an Interchange Profile 1 check item. Selecting it
uses BoltLite with 16 KiB blocks and enforces the format's mini-reader entry
count, entry-size, and serialized-size limits; selecting another codec returns
to unrestricted full-format creation.

Extraction preflights projected names and file/directory-prefix conflicts
before creating output. The index is capped at 1,024 names and 32 KiB of keys,
and full GEMDOS paths are limited to 125 bytes. It rejects unrepresentable or
non-8.3 names, attempts to check existing path components for symlinks, and stages regular
files beside their destinations until checksum verification and close
succeed. The open stage records device, size, and modification time; publication and
cleanup require the stage path to retain those values. File-time restoration
follows publication so FAT12 timestamp conversion cannot invalidate that check. Before
publication the frontend reopens the stage and verifies its exact archived length and
CRC. A same-size/same-mtime substituted file with different contents is refused; the
old destination is preserved and the substituted stage is left untouched. CRC
collisions and substitutions after the final content/path validation remain possible.
The Atari runtime follows host symlinks in Hatari host-drive mode: a TOS 1.62
probe reports `lstat("C:\\OUT")` as a directory and `opendir()` returns
`ENOSYS`, so the parent-chain symlink check is ineffective there. A production
GUI probe redirected `OUT` to a different directory while a 1 MiB stage was
open. The stage-path check refused publication and preserved both pre-existing
targets, but cleanup could not reach the stage through the new parent path; a
full-size `BP` stage remained in the moved directory
(`build/atari-gui-cancel-u4rjzscu`). Native TOS filesystems do not provide
POSIX symlinks; behavior on MiNT symlink filesystems remains unqualified.
Existing files are replaced
through a recovery rename. File times
and matching native TOS attributes are restored; portable FAT attributes are
the fallback, and directory timestamps and attributes are deferred until
children finish. Unsupported links, resource forks, foreign metadata, and
POSIX modes are reported as partial. These path checks retain a concurrent
parent-substitution race. `make atari-gui` passes a 68k cross-build with
warnings treated as errors. Hatari 2.5.0 runs the production GUI on TOS 1.06,
1.62, and 2.05 with FAT12 create/extract fixtures. Archive, Read-only, Hidden,
and System file flags round-trip. Directory timestamps remain at FAT defaults
and report partial on all three tested TOS versions. A hidden/system empty
directory's archived flags are not restored on TOS 1.06, 1.62, or 2.05, and
the GUI reports partial. On TOS 1.62, the GUI replaced two pre-existing files
with exact archive bytes on both FAT12 and GEMDOS host-directory targets. Active extraction cancellation on TOS 1.06, 1.62, and 2.05 preserves
a pre-existing target and removes its stage. Evidence is retained at
`build/atari-gui-cancel-n5_73lb5`, `build/atari-gui-cancel-l_60i0ez`, and
`build/atari-gui-cancel-dv4mcp6n`. On TOS 1.62 FAT12, a read-only existing
target causes the production GUI to report incomplete extraction while
preserving its bytes and READONLY flag and cleaning the stage; the standard
TOS 1.62 FAT12 create/extract suite passed after the stage-identity and
content guards were added (`build/atari-gui-create-5l4vrz2o`). The complete
production flow also passed on the GEMDOS host directory at
`build/atari-gui-create-1q0nwqmg`. The deterministic
`atari-gui-stage-substitution-test` on Hatari/TOS 1.62 replaced the open 1 MiB
stage with different bytes while preserving its size and mtime; the original
destination stayed byte-identical and the substituted stage was not deleted
(`build/atari-gui-cancel-xo5h9aof`). Evidence is
under `build/atari-gui-create-msxh3kx1/failed-replacement`. A separate Hatari process-termination fixture preserves the old destination
after 32 KiB reaches a sibling stage; a production retry restores the exact
16 MiB payload while leaving the old partial stage
(`build/atari-gui-cancel-jh47lf8m`). Interruption after moving the old target
and safe stale-stage cleanup remain open. These
runtime results and screenshots are retained in
`docs/interoperability-remaining-steps.md` and the referenced `build/` evidence.

---

### Amiga GadTools GUI

**File:** `src/frontend/amiga_gui/main.c`
**Toolkit:** Intuition + GadTools + ASL + Workbench
**Language:** C89
**Target:** AmigaOS 2.0+ (Kickstart 37+), 68k

| Feature            | Implementation                                     |
| ------------------ | -------------------------------------------------- |
| File list          | GadTools `LISTVIEW_KIND` with Exec `List` / `Node` |
| Drag-and-drop      | Workbench `AppWindow` via `AddAppWindowA`          |
| File dialogs       | ASL file requester (`AllocAslRequestTags`)         |
| Progress           | In-window status panel with core progress updates  |
| Keyboard shortcuts | Menu shortcuts: O, E, W, N, Q (right-Amiga key)    |
| Linking            | Static to archiver core; `-noixemul`               |

The file list displays entries in a formatted single-line layout with compact
sizes and packed-size percentage semantics. Archive creation now uses a
dedicated **Compression** menu with checkmarked Stored, BoltLite, boltlz, and
Fluff choices; Fluff selection is gated against the current largest free memory
block reported by `AvailMem(MEMF_ANY | MEMF_LARGEST)`.
The menu also offers an Interchange Profile 1 check item for new archives.
It selects BoltLite with 16 KiB blocks and enforces mini-reader entry-count,
entry-size, and serialized-size limits. Selecting a compression method turns
the profile off.

Workbench integration: creates an `AppWindow` for drag-and-drop from the
Workbench icon view. Files dropped on the application icon (via WBStartup
message handling) are also processed. `NameFromLock` resolves full paths
from Workbench `WBArg` entries.

GUI-only cross-build:

```sh
make amiga-gui
```

This emits `build/amiga/boltpack-gui` so it can coexist safely with the CLI
binary on Amiga's case-insensitive filesystem.

Libraries opened at startup: `GadToolsBase`, `AslBase`, `WorkbenchBase`
(optional — may not be available on minimal systems).

Cross-compiled with `m68k-amigaos-gcc -noixemul`. The `-pedantic` flag is
omitted because AmigaOS NDK headers use K&R-style declarations.

Extraction uses the status-aware core API and sibling staging for regular
files. Before writing, the GUI checks projected Latin-1 names for case-folded
collisions and file/directory-prefix conflicts (up to 1,024 entries and 32 KiB
of keys), rejects Amiga volume separators, and checks existing directory
components for symlinks. File and directory modification times are restored
when representable; directory timestamps and native protection are deferred
until children finish. Matching Amiga protection/comment records are restored
only for successfully extracted subjects. Unsupported symlinks, resource
forks, foreign native metadata, and portable FAT envelopes are reported as a
partial extraction. Names outside Latin-1 cannot be extracted by this GUI.
These pathname checks cannot prevent a concurrent parent rename. The GUI
cross-build is verified, but no Amiga runtime or emulator round trip is
available.

---

### Haiku BeAPI GUI

**File:** `src/frontend/haiku_gui/main.cpp`
**Toolkit:** BeAPI (BApplication, BWindow, BMenuBar, BColumnListView)
**Language:** C++
**Target:** Haiku R1 (x86-64)

| Feature            | Implementation                                             |
| ------------------ | ---------------------------------------------------------- |
| File list          | `BColumnListView` with 6 `BStringColumn` instances         |
| Drag-and-drop      | `B_SIMPLE_DATA` message; `entry_ref` resolution            |
| File dialogs       | `BFilePanel` (Tracker-integrated)                          |
| Progress           | Status bar updates                                         |
| Keyboard shortcuts | Alt+O, Alt+E, Alt+W, Alt+N, Alt+Q                          |
| Linking            | Static to archiver core; `-lbe -lcolumnlistview -ltracker` |

Classes:

- `BPApp` (`BApplication`) — main loop, `RefsReceived` for open-with
- `BPWindow` (`BWindow`) — menu bar, column list, file operations

Extract and create callbacks use C++ lambda functions. Directory recursion
for archive creation uses `BDirectory` / `BEntry` / `BPath`.

Application signature: `application/x-vnd.BoltPack`.

The Compression menu also includes a `Use Multi-threaded Fluff` toggle for
modern fast-fluff builds. When Fluff is the selected method, `DoCreate()` maps
that toggle to `bp_set_fast_fluff()`, and the `make haiku-gui` target links the
matching OpenMP fast-fluff object so the menu item changes archive creation
behavior rather than only UI state.

The Compression menu also offers Interchange Profile 1 for archive creation.
It selects BoltLite with 16 KiB blocks and mini-reader entry-count,
entry-size, and serialized-size limits. Selecting a method or level disables
the profile.

The build target uses the `x86_64-unknown-haiku-gcc/g++` toolchain. It is not
installed in this environment, so the current source changes have no Haiku
cross-build or runtime evidence. Extraction now validates the complete
primary-name table before writing, including ASCII case-folded duplicates and
file/directory prefix conflicts. It opens the selected destination as a root
directory descriptor, refuses symlinked archive-controlled parent components,
and writes each regular file to a same-directory stage before publishing after
checksum verification and successful close. File and deferred directory modes
and times are restored after extraction; unsupported entries and failed
metadata restoration report partial. `make CC=gcc haiku-gui-extract-host-test`
checks collision refusal, symlink-parent refusal, a core-driven valid extraction
and checksum-failure preservation, successful replacement, and stage cleanup
through the shared POSIX helpers.
Haiku BFS Unicode case mapping, native metadata, and target API/runtime behavior
still need qualification. The GUI now writes the same `BP_OS_BEOS` metadata
payload as the CLI (Unix mode plus optional `BEOS:TYPE`) after each regular
file and directory. On extraction it restores file metadata after checksum
verification and defers directory mode/MIME restoration until descendants are
created. Failed or corrupt metadata restoration is reported as partial. The
host helper test does not emulate BFS attributes, so native attribute
round-trips still need a Haiku filesystem/runtime check.

---

### GUI Frontend Comparison

| Frontend       | Toolkit        | Language    | Drag-and-Drop           | Progress Style  | Core Linking |
| -------------- | -------------- | ----------- | ----------------------- | --------------- | ------------ |
| Linux FLTK     | FLTK 1.3       | C++         | X11 paste events        | Modal window    | Static       |
| Win32          | Win32 API      | C89         | `WM_DROPFILES`          | Modeless dialog | Static       |
| WinForms .NET  | WinForms 4.8   | C#          | `AllowDrop` events      | Modal dialog    | DLL/P/Invoke |
| macOS Cocoa    | AppKit         | Objective-C | `NSDraggingDestination` | Sheet panel     | Static       |
| Atari GEM      | GEM/AES + VDI  | C89         | —                       | Main window     | Static       |
| Amiga GadTools | GadTools + ASL | C89         | `AppWindow`             | Window title    | Static       |
| Haiku BeAPI    | BeAPI          | C++         | `B_SIMPLE_DATA`         | Status bar      | Static       |

---

## Adding a New Frontend

To port BoltPack to a new platform:

1. Create `src/frontend/<platform>/main.c`
2. Implement `bp_io_t` callbacks using the platform's file I/O API
3. Implement directory recursion using the platform's directory enumeration API
4. Implement `bp_extract_fn` / `bp_extract_done_fn` callbacks for extraction
5. Implement `bp_progress_fn` for progress display (CLI output, GUI, or silent)
6. Handle text encoding conversion between the platform's native encoding and UTF-8
7. Archive platform-specific metadata as `BP_TYPE_META` entries with the appropriate `BP_OS_*` platform tag
8. Call `bp_compress_init()` once at startup
9. Use `bp_archive_create` / `bp_archive_add_entry` / `bp_archive_finalize` for packing
10. Use `bp_archive_extract_all_ex` for checksum-aware unpacking and cleanup

The core API is pure C89 and compiles on any platform with a C89 compiler. No porting of the core is needed.

## Frontend Comparison

| Frontend       | Interface         | I/O layer              | Directory API           | Text encoding      | Metadata                                     |
| -------------- | ----------------- | ---------------------- | ----------------------- | ------------------ | -------------------------------------------- |
| Linux          | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| macOS X        | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions, resource fork, Finder info |
| Haiku / BeOS   | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions + BFS MIME type             |
| RISC OS        | CLI               | `OS_GBPB` SWI          | `OS_GBPB` enumerate     | Latin-1 ↔ UTF-8    | Load/exec address, attribute word            |
| Windows 32/64  | CLI               | `<stdio.h>` + Win32    | `FindFirstFile`         | CP850/437 ↔ UTF-8  | FAT attributes                               |
| Windows 3.1    | CLI               | `_lopen` / `_lread`    | `_dos_findfirst`        | CP850 ↔ UTF-8      | FAT attributes                               |
| MS-DOS         | CLI               | `<stdio.h>`            | `opendir` / `findfirst` | CP850/437 ↔ UTF-8  | FAT attributes                               |
| OS/2           | CLI               | `<stdio.h>` (kLIBC)    | `DosFindFirst`          | CP850 ↔ UTF-8      | FAT/HPFS attributes, `.TYPE` EA              |
| AmigaOS 68k    | CLI               | `<stdio.h>` (libnix)   | `opendir` / `readdir`   | ISO-8859-1 ↔ UTF-8 | Protection bits + file comment               |
| AmigaOS 4.x    | CLI               | `<stdio.h>` (newlib)   | `opendir` / `readdir`   | ISO-8859-1 ↔ UTF-8 | Protection bits + file comment               |
| MorphOS        | CLI               | `<stdio.h>` (clib2)    | `opendir` / `readdir`   | ISO-8859-1 ↔ UTF-8 | Protection bits + file comment               |
| AROS           | CLI               | `<stdio.h>`            | `opendir` / `readdir`   | ISO-8859-1 ↔ UTF-8 | Protection bits + file comment               |
| Atari TOS/MiNT | CLI (`.TOS`)      | `<stdio.h>` (mintlib)  | `opendir` / `readdir`   | Atari ST ↔ UTF-8   | TOS attribute byte                           |
| Mac OS 9       | GUI (drag & drop) | Mac File Manager       | `PBGetCatInfoSync`      | MacRoman ↔ UTF-8   | Type, creator, Finder flags, timestamps      |
| Plan 9         | CLI               | Plan 9 `read`/`write`  | `dirread`               | UTF-8 native       | `Dir.mode` lower 16 bits                     |
| Sinclair QL    | CLI               | `<stdio.h>`            | `qdirect` struct read   | ISO-8859-1 ↔ UTF-8 | None                                         |
| Sharp X68000   | CLI               | `<stdio.h>`            | `_dos_files`            | Shift-JIS ↔ UTF-8  | None                                         |
| FreeBSD        | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| NetBSD         | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| OpenBSD        | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| Solaris        | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| illumos        | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| HP-UX          | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| AIX            | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| IRIX           | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| GNU Hurd       | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| Tru64 UNIX     | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| SCO OpenServer | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| QNX Neutrino   | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| NeXTSTEP       | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| z/OS (USS)     | CLI               | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | Unqualified; filename bytes pass through without conversion | Unix permissions |
| Linux FLTK     | GUI (FLTK)        | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| Win32 GUI      | GUI (Win32)       | UTF-16 Win32/CRT APIs  | `FindFirstFileW`        | UTF-8 ↔ UTF-16     | —                                            |
| Windows FLTK   | GUI (FLTK)        | UTF-16 Win32/CRT APIs  | FLTK chooser / wide APIs | UTF-8 ↔ UTF-16   | —                                            |
| WinForms .NET  | GUI (WinForms)    | DLL via P/Invoke       | `Directory.GetFiles`    | UTF-16 native      | —                                            |
| Total Commander| WCX plugin        | WCX host API           | Host supplies paths     | UTF-16 ↔ UTF-8 (`W` API); strict ACP legacy API | —                         |
| macOS Cocoa    | GUI (Cocoa)       | `<stdio.h>` (`fseeko`) | `opendir` / `readdir`   | UTF-8 native       | Unix permissions                             |
| Atari GEM      | GUI (GEM/VDI)     | `<stdio.h>` (mintlib)  | `opendir` / `readdir`   | Atari ST ↔ UTF-8   | TOS attribute byte                           |
| Amiga GadTools | GUI (GadTools)    | `<stdio.h>` (libnix)   | `opendir` / `readdir`   | ISO-8859-1 ↔ UTF-8 | Protection bits + file comment               |
| Haiku BeAPI    | GUI (BeAPI)       | `<stdio.h>` (`fseeko`) | `BDirectory`/`BEntry`   | UTF-8 native       | Unix permissions + BFS MIME type             |
