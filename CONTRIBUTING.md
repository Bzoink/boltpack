# Contributing to BoltPack

Thanks for your interest in BoltPack. Bug reports, portability fixes, and new
platform frontends are all welcome.

BoltPack has one unusual constraint that shapes almost every contribution: the
archive core must compile as **strict ANSI C89** on compilers from the 1980s and
1990s. Please read [Core Language Rules](#core-language-rules) before touching
anything under `src/core/`.

## Reporting Bugs

Open an issue with:

- **Platform and toolchain** — OS, version, CPU, compiler and compiler version
- **BoltPack version** — shown in the `boltpack help` banner, or the release you
  downloaded
- **Exact command line** and the full output
- **A minimal archive or input file** that reproduces the problem, if possible

Archive corruption and decompression bugs are the highest-priority class of
issue. If you have an archive that fails to extract, please attach it (or a
reduced version) rather than only describing the symptom.

Do **not** open a public issue for a security vulnerability — see
[SECURITY.md](SECURITY.md).

## Building

```sh
make            # Linux x86-64 CLI at build/linux/boltpack
make modern     # OpenMP-enabled fast-fluff encoder
make debug      # debug symbols + AddressSanitizer + UBSanitizer
make clean
```

See [docs/BUILD.md](docs/BUILD.md) for cross targets (`make win32`, `make
win64`, `make musl`, `make wasm`, `make cross-all`) and native builds on other
platforms.

## Testing

Run the tests before submitting. `make` first — the integration suite drives the
built CLI binary.

```sh
make test                                    # unit tests + CLI integration tests
make unit-test                               # unit tests only (fast)
make unit-test-asan                          # unit tests under AddressSanitizer
python3 test/check_boltpack.py --verbose     # integration tests, verbose
python3 test/bench_corpus.py                 # compression corpus benchmark
make fuzz                                    # fuzz the ASan build
```

What to add with your change:

- **Core behavior** — a focused C unit test in `test/unit/test_<area>.c`
- **User-visible archive or CLI behavior** — a round-trip case in
  `test/check_boltpack.py`
- **Format changes** — a fixture under `test/fixtures/` so older stream
  versions keep decoding

Use `make unit-test-asan` or `make debug` for anything memory-sensitive.

## Core Language Rules

These are non-negotiable for `src/core/` and `lib/`. They exist because the core
is compiled by Aztec C, Lattice C, Turbo C, SAS/C, MPW C, and other pre-C99
compilers.

- `/* */` comments only — no `//`
- Declare all variables at the top of a block; no mixed declarations and code
- No `<stdint.h>`, `<stdbool.h>`, `_Bool`, `long long`, or variable-length arrays
- No compiler extensions — no `__attribute__`, `__declspec`, or `#pragma pack`
- Use the sized types from `bp_types.h` (`bp_uint8`, `bp_uint16`, `bp_uint32`,
  `bp_uint64`) — never raw `int`/`long` for sized data
- `bp_uint32` is `unsigned int`, not `unsigned long` — this matters for correct
  LP64 arithmetic
- Return `0` on success and non-zero on error; check every return value
- The core never calls `fopen`/`fread` directly — it goes through the `bp_io_t`
  callbacks so non-POSIX platforms can supply their own I/O

There is exactly one sanctioned exception: `bp_off_t` in `bp_types.h` uses
`__extension__ typedef long long` (GCC/Clang) and `__int64` (MSVC) to express a
64-bit file offset on ILP32 platforms. See [docs/portability.md](docs/portability.md)
for the rationale.

Frontend code under `src/frontend/<platform>/` may use platform APIs freely, but
those dependencies must not leak into the core. There is no multithreading in
the core; `fluff_fast.c` (OpenMP) is a frontend-layer concern.

## Style

- `snake_case` for C functions, types, and filenames
- Every source file opens with the standard header comment:

  ```c
  /*
   * archiver.c -- archive format read/write for BoltPack
   *
   * ANSI C (C89/C90) only.
   *
   * Copyright (C) 2026 Tony Mattsson
   * Licensed under the GNU General Public License v2
   */
  ```

- Match the surrounding code's naming, comment density, and idiom

Markdown under `docs/` and `.github/` must pass markdownlint, and all fenced
code blocks need a language tag:

```sh
prettier --write <file.md>
markdownlint <file.md>
```

## Adding a Platform Frontend

New platforms are the most welcome kind of contribution.
[docs/frontends.md](docs/frontends.md) is the guide — it covers the frontend
contract, the `bp_io_t` callbacks you must supply, encoding conversion via
`bp_codepage.c`, and byte-swapping on big-endian hosts. Archive paths are always
stored as UTF-8; the frontend converts to and from the native encoding.

Start from `templates/` and an existing frontend close to your target.

## Pull Requests

- One logical change per PR
- Short, sentence-case commit subjects (`Fix append bug`)
- Explain portability, archive format, or compatibility impact in the commit body
- List the commands you tested, and name any cross-platform target you could
  **not** test for lack of a toolchain or emulator — that is expected and fine,
  just say so
- Include screenshots for GUI changes
- Update the relevant docs in the same PR; format changes must also update
  [docs/specification-1.0.md](docs/specification-1.0.md)

CI builds and tests the Linux x86-64 and musl static targets on every push. It
cannot cover the vintage platforms, so portability review is done by reading.

## License

BoltPack is licensed under the **GNU General Public License v2**. By submitting
a contribution you agree to license it under the same terms. Contributors are
credited in [CONTRIBUTORS.md](CONTRIBUTORS.md).
