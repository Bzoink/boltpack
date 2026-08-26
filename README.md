# BoltPack

![BoltPack](resources/pics/boltpack_logo_wb.png)

[![CI](https://github.com/Bzoink/boltpack/actions/workflows/ci.yml/badge.svg)](https://github.com/Bzoink/boltpack/actions/workflows/ci.yml)
[![License: BSD-2-Clause](https://img.shields.io/badge/License-BSD--2--Clause-blue.svg)](LICENSE.md)
[![Latest Release](https://img.shields.io/github/v/release/Bzoink/boltpack?label=download)](https://github.com/Bzoink/boltpack/releases/latest)

Lightweight file archiver designed for maximum portability — from an 8088 PC
running DOS to a modern x86-64 Linux box, including AmigaOS, Atari TOS,
Classic Mac OS, RISC OS, Haiku, Plan 9, z/OS, and many more.

The archive core is strict ANSI C89 and compiles on virtually any system with a
C compiler. Platform-specific concerns (file I/O, encoding, GUI) live in
isolated frontends.

BoltPack is intended to run on real vintage hardware as well as modern systems,
to ease data transfer and archival between platforms.

> **On AI assistance:** this project was written with substantial help from AI
> tooling. Without it, the 40-platform scope would not have been reachable for a
> single author. The archive format, architecture, and portability rules are the
> author's; much of the implementation work was AI-assisted.

## Downloads

Pre-built binaries for the current release (**v1.4**) are below. Every release
also ships `SHA256SUMS.txt` for verification.

| Platform                      | Download                                                                                                                                   |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| Linux x86-64                  | [boltpack-1.4-linux-x86-64.tar.gz](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-linux-x86-64.tar.gz)             |
| Linux x86-64 static (musl)    | [boltpack-1.4-musl-x86-64.tar.gz](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-musl-x86-64.tar.gz)               |
| Linux x86-64 GUI (FLTK)       | [boltpack-1.4-linux-gui-x86-64.tar.gz](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-linux-gui-x86-64.tar.gz)     |
| Linux ARM64                   | [boltpack-1.4-linux-arm64-ARM.tar.gz](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-linux-arm64-ARM.tar.gz)       |
| Linux ARM32                   | [boltpack-1.4-linux-arm32-arm.tar.gz](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-linux-arm32-arm.tar.gz)       |
| Linux RISC-V 64               | [boltpack-1.4-riscv-riscv64.tar.gz](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-riscv-riscv64.tar.gz)           |
| GNU Hurd x86-64               | [boltpack-1.4-hurd-x86-64.tar.gz](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-hurd-x86-64.tar.gz)               |
| Windows 64-bit installer      | [boltpack-1.4-win64-setup.exe](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-win64-setup.exe)                     |
| Windows 64-bit CLI            | [boltpack-1.4-win64-x86-64.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-win64-x86-64.zip)                   |
| Windows 64-bit GUI (FLTK)     | [boltpack-1.4-win64-fltk-gui-x86-64.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-win64-fltk-gui-x86-64.zip) |
| Windows 32-bit installer      | [boltpack-1.4-win32-setup.exe](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-win32-setup.exe)                     |
| Windows 32-bit CLI            | [boltpack-1.4-win32-x86.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-win32-x86.zip)                         |
| Windows 32-bit GUI (FLTK)     | [boltpack-1.4-win32-fltk-gui-x86.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-win32-fltk-gui-x86.zip)       |
| Total Commander plugin 64-bit | [boltpack-1.4-totalcmd-plugin-win64.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-totalcmd-plugin-win64.zip) |
| Total Commander plugin 32-bit | [boltpack-1.4-totalcmd-plugin-win32.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-totalcmd-plugin-win32.zip) |
| Windows 3.x 16-bit (mini)     | [boltpack-1.4-win16-x86.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-win16-x86.zip)                         |
| MS-DOS 16-bit (mini)          | [boltpack-1.4-dos16-x86.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-dos16-x86.zip)                         |
| OS/2 1.x 16-bit (mini)        | [boltpack-1.4-os2-16-x86.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-os2-16-x86.zip)                       |
| Amiga 68k (CLI + GUI)         | [boltpack-1.4-amiga-m68k.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-amiga-m68k.zip)                       |
| Atari TOS + GEM GUI           | [boltpack-1.4-atari-m68k.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-atari-m68k.zip)                       |
| Classic Mac OS 68k GUI        | [boltpack-1.4-mac68k.hqx](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-mac68k.hqx)                               |
| WebAssembly                   | [boltpack-1.4-wasm-wasm32.zip](https://github.com/Bzoink/boltpack/releases/download/v1.4/boltpack-1.4-wasm-wasm32.zip)                     |

Source kits for platforms that require a native build (macOS, Solaris, IRIX,
HP-UX, AIX, OpenBSD, RISC OS, MorphOS, AROS, Sinclair QL, Sharp X68000, Haiku,
and others) are attached to the same
[release](https://github.com/Bzoink/boltpack/releases/latest).

Verify a download against the release checksums:

```sh
sha256sum -c SHA256SUMS.txt --ignore-missing
```

## Quick Start

```sh
make
./build/linux/boltpack c archive.bolt myfiles/    # create
./build/linux/boltpack l archive.bolt             # list
./build/linux/boltpack x archive.bolt             # extract
```

For maximum compression on a modern machine:

```sh
make modern
./build/linux/boltpack c -F archive.bolt myfiles/
```

Run `boltpack help` for the full command list, or `boltpack help advanced` for
expert tuning switches.

## Compression Methods

| Flag | Method  | ID  | Description                                               |
| ---- | ------- | --- | --------------------------------------------------------- |
| `-2` | fastlz  | 2   | Default — fast general-purpose compression                |
| `-0` | Stored  | 0   | No compression; raw byte copy                             |
| `-1` | LZSS    | 1   | Small-memory LZ77; suited for 16-bit targets              |
| `-F` | Fluff   | 3   | High compression ratio; LZ77 + FSE/tANS                   |
| `-Z` | zstd    | 4   | Best compression ratio; modern 32/64-bit only; zstd 1.5.7 |

fastlz accepts levels 1–2 via `-l N`; omitting `-l` auto-selects its level.
Fluff and zstd support levels 1–9. When `-F` or `-Z` is used without `-b`, block
size is set to 256 KB automatically.

On OpenMP-enabled modern builds, selecting Fluff auto-enables multi-processor
compression for larger workloads. Use `--single-thread` to force the sequential
path; `-j N` and `-g N` tune thread count and grouping (see
`boltpack help advanced`). The Linux/Windows FLTK, macOS Cocoa, and Haiku GUIs
expose the same feature as a **Use Multi-threaded Fluff** toggle.

## Platform Support

BoltPack targets over 40 platforms across 8 CPU families:

**Linux** · **Windows** · **macOS** · **Classic Mac OS** · **AmigaOS** ·
**Atari TOS/MiNT** · **MS-DOS** · **CP/M-86** · **OS/2** · **RISC OS** ·
**Haiku/BeOS** · **Plan 9** · **FreeBSD** · **NetBSD** · **OpenBSD** ·
**Solaris/Illumos** · **IRIX** · **HP-UX** · **AIX** · **Tru64** ·
**NeXTSTEP/OPENSTEP** · **QNX** · **Hurd** · **OpenVMS** · **z/OS** ·
**MorphOS** · **AROS** · **AmigaOS 4** · **Sinclair QL** · **Sharp X68000** ·
**WebAssembly** · and more

See [docs/portability.md](docs/portability.md) for the full platform table, CPU
families, compiler requirements, and C89 coding constraints.

## Building

```sh
make            # Linux x86-64 (default)
make modern     # OpenMP-enabled Fluff encoder
make arm64      # Linux ARM64
make musl       # static binary (musl libc)
make linux-gui  # FLTK GUI
make win32      # Windows 32-bit (cross-compiled from Linux)
make win64      # Windows 64-bit (cross-compiled from Linux)
make cross-all  # all cross-compilation targets
make clean
```

See [docs/BUILD.md](docs/BUILD.md) for native build instructions across all
platforms and cross-compilation details.

## Testing

```sh
make test                                    # unit tests + CLI integration tests
make unit-test                               # unit tests only
python3 test/check_boltpack.py --verbose     # integration tests, verbose
```

## Documentation

| File                                                   | Purpose                                                   |
| ------------------------------------------------------ | --------------------------------------------------------- |
| [docs/architecture.md](docs/architecture.md)           | Layer boundaries, core interfaces, and error codes        |
| [docs/specification-1.0.md](docs/specification-1.0.md) | Normative archive and codec rules                         |
| [docs/format.md](docs/format.md)                       | Concise `.bolt` / `.blt` archive format overview          |
| [docs/algorithms.md](docs/algorithms.md)               | LZSS, fastlz, and Fluff implementation details           |
| [docs/portability.md](docs/portability.md)             | Supported platforms, C89 constraints, and toolchain notes |
| [docs/frontends.md](docs/frontends.md)                 | Frontend responsibilities and platform integration guide  |
| [docs/BUILD.md](docs/BUILD.md)                         | Native build procedures and platform-specific caveats     |
| [docs/mini.md](docs/mini.md)                           | 16-bit mini variant architecture and capability limits    |

[docs/README.md](docs/README.md) is the documentation index.
[RELEASE-NOTES.md](RELEASE-NOTES.md) records what changed in each version.

## Repository Layout

```text
src/core/        Archive core + compression layer (strict C89)
src/core/mini/   16-bit mini variant core
src/frontend/    One frontend per platform
lib/             Vendored fastlz 0.5.0 and zstd 1.5.7
docs/            Full documentation set
test/            Unit tests, integration tests, and fixtures
scripts/         Build, packaging, and release scripts
packaging/       Installer and package definitions
encodings/       Codepage conversion tables
magic/           file(1) magic definitions for .bolt archives
templates/       SDK and frontend templates
```

## Contributing

Bug reports, portability fixes, and new platform frontends are welcome. See
[CONTRIBUTING.md](CONTRIBUTING.md) for build and test steps, the non-negotiable
C89 rules for `src/core/`, and pull request expectations.

To report a security issue, see [SECURITY.md](SECURITY.md) — please do not open
a public issue for vulnerabilities.

## Credits

See [CONTRIBUTORS.md](CONTRIBUTORS.md) for the project author and attributions
for the third-party algorithms and libraries incorporated into BoltPack.

## License

BoltPack is licensed under the **BSD 2-Clause "Simplified" License** — see
[LICENSE.md](LICENSE.md).

This distribution includes **fastlz 0.5.0** by Ariya Hidayat, licensed under
the MIT license (`lib/fastlz/LICENSE`), and **zstd 1.5.7** by Meta Platforms /
Yann Collet under the BSD 2-Clause license (`lib/zstd/`).
