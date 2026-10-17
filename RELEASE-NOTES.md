# Release Notes

## 1.9 — Parallel BoltLZ compression

- OpenMP-enabled Linux and Windows CLI builds compress independent BoltLZ
  blocks in parallel on sufficiently large files. `-j N` sets the worker limit;
  `--single-thread` forces sequential compression. Small files stay on the
  sequential path.
- Parallel BoltLZ writes blocks in their original order and keeps the existing
  archive format and decoder compatibility.
- Windows installers leave long PATH values unchanged instead of risking
  truncation.
- Bolt Mini remains at version 1.3.

## 1.8 — Fluff compression improvements

- Fluff's encoder improves compression ratio and reduces encoder memory use.
- New archives use Fluff stream versions `0x09`/`0x89`. BoltPack 1.8 reads
  older Fluff streams, while earlier BoltPack versions cannot decode streams
  written by 1.8.
- The decoder continues to support Fluff streams `0x06`, `0x07`/`0x87`, and
  `0x08`/`0x88`.

## 1.7 — Compact BoltLite for constrained targets

- BoltLite uses a compact variable-form bitstream in archive format 5.
- The encoder workspace drops from 29,200 to 18,432 bytes; the decoder needs
  no codec scratch.
- 16-bit mini builds use native 16-bit match positions for blocks up to 32 KiB.
- Previous BoltPack binaries remain the way to extract format-4 archives.

## 1.6 — BoltLZ integration

- BoltLZ is the default method 2 codec, using Balanced mode.
- `-2` / `--boltlz` selects it; `-l 1` selects Fast and `-l 2` Balanced.
- Archives use format 4; readers reject every other archive version.
- Encoder workspace is cached per archive; decoder buffers require no padding.
- Full, mini, GUI and source-kit builds use BoltLZ, compiled with `-O2`.

## v1.5.0 — August 2026

**Version:** 1.5
**Status:** Stable

### License and dependency change

- **Relicensed from GPLv2 to the BSD 2-Clause "Simplified" License.** BoltPack
  code is now BSD-2-Clause; bundled third-party components retain their own
  license notices.

### What's new

- `creator_ver` in new archives: `0x01, 0x05`.
- Mini build (`bpmini`) bumped to 1.3.

---

## v1.4 — July 2026

**Version:** 1.4
**Status:** Stable

### Archive format

**Format version bumped to 3.** v3 archives are not readable by BoltPack 1.3
or earlier. v3 readers cannot read v2 archives (strict version check). The
`creator_ver` bytes in archives written by BoltPack 1.4 are `0x01, 0x04`.

### What's new

#### Archive format v3

- **Per-block CRC-32** — every compressed block now carries a 4-byte CRC-32
  field. The block header grows from 8 to 12 bytes (`comp_size + orig_size +
  block_crc`). Readers verify the CRC before decompressing; a mismatch is
  reported as `BP_ERR_CHECKSUM`. This makes corruption detectable at the
  earliest possible point in the read path rather than only at the whole-entry
  checksum.
- **`total_orig` in file-table header** — the 8-byte table header grows to 16
  bytes by appending the sum of all `original_size` values. Listing and info
  commands read total uncompressed size directly from the table header without
  scanning every entry record.
- **Unix permission bits in entry flags** — the entry `flags` field (previously
  reserved) now carries bit 15 (`BP_ENTRY_FLAG_MODE_VALID`) and bits 0–11
  (`BP_ENTRY_FLAG_MODE_MASK`) for the 12-bit Unix permission field
  (`st_mode & 0x0FFF`). On the Linux frontend, permissions are stored at create
  time and restored with `chmod` at extract time. Unknown flag bits are accepted
  silently for forward compatibility. New `bp_archive_add_entry_ex` and
  `bp_archive_pack_entry_ex` API variants let frontends set flags without
  modifying existing call sites.
- **`archive_size` is now normative for v3** — a v3 reader must verify that the
  stored `archive_size` matches the actual file size; mismatch → `BP_ERR_TRUNCATED`.
  Writers must store the correct total file size. (v2: informational only.)

#### CRC-32 as the only checksum

Adler-32 (`checksum_type = 0`) is fully retired. Writers have not emitted it
since v1.2, and v3 now makes `checksum_type = 1` (CRC-32) mandatory. Readers
encountering `checksum_type != 1` on a v3 archive return `BP_ERR_CORRUPT`.
The `BP_ENABLE_ADLER32` build flag is retained for unit-testing the function
itself.

#### zstd multi-threaded compression

The Linux CLI and Linux/Windows FLTK GUI frontends now run zstd compression
with multiple threads when `-j N` is given alongside `-Z`. The sequential
path is unchanged. No archive-format impact.

#### Mini build (bpmini 1.2)

- Lazy allocation of the BoltLite work buffer (16-bit path, 29 200 bytes) and
  LZO work buffer (32-bit path, 64 KB): memory is malloc'd on the first
  create call and cached for process lifetime. Extract, list, test, and info
  operations no longer pay for compression scratch at startup.
- Per-entry extract buffers hoisted to once-per-run: two malloc/free pairs per
  run instead of two per entry, eliminating far-heap churn on DOS.
- DOS-16 and Win16 frontends: full list, info, test, extract, create, append,
  and delete command set aligned with the full build's CLI conventions.
- Watcom builds: `-zm` flag added for function-level segments so `option
  eliminate` can strip dead functions. linuxmini binary stripped of unwind
  tables, symbol tables, and build-id.

#### Correctness and internal quality

- **Append bug fix** — an off-by-one in `bp_archive_open_append` caused the
  last byte of the old file table to be included in the first new data block,
  corrupting archives created via append on v2 archives with certain table
  alignments.
- **Codec reentrancy** — Fluff, zstd, and miniLZO compression contexts are now
  fully allocated per-call; no static state remains in the codec layer, making
  the core safe for process-level concurrent use (one archive per thread).
- **Security fixes** — path traversal checks tightened; symlink target
  validation added (`bp_symlink_target_is_safe`); integer overflow guards added
  to block-size arithmetic in the decompression path.
- **Layering cleanup** — archiver internal helpers moved out of `archiver.h`
  into a private header; public surface area of `bp_archive_t` reduced.
- **zstd error strings** moved out of the vendored `lib/zstd/zstd.c` into the
  wrapper (`zstd_wrap.c`) so the vendored file tracks upstream cleanly.
- **stderr fallback removed from core** — the archiver core no longer writes to
  `stderr` directly; all diagnostics go through the `bp_log_fn` callback.
  Frontends that do not set a log callback silently discard diagnostic messages.

### Codec table

| Method | Name   | Intended use                                   |
| ------ | ------ | ---------------------------------------------- |
| 0      | Stored | Incompressible data or raw fallback            |
| 1      | BoltLite   | Small-memory and 16-bit compatible compression |

## v1.3 — July 2026

**Version:** 1.3
**Status:** Stable

### What's new

- **LAN send/receive** — transfer archives directly between machines on the
  local network without a shared filesystem or external service.
  - `boltpack listen` starts a receiver; `boltpack scan` lists active receivers.
  - `--send HOST` streams a newly created archive to a named or IP-addressed
    receiver immediately after the create step.
  - Discovery uses UDP broadcast so receivers are found by friendly name; no
    manual IP entry required on typical home/office networks.
  - Broadcast is sent on every network interface, fixing discovery when a
    multi-homed host or VPN adapter is present.
- **Receiver options:**
  - `--name NAME` — advertise a friendly name (listen) or identify the sender
    (send); defaults to the system hostname.
  - `--auto` / `--prompt` — accept all incoming transfers silently, or prompt
    before each one (default: prompt).
  - `--autoextract` — unpack the received archive into files immediately rather
    than saving the `.bolt` file.
  - `--autoverify` — re-read and verify the saved `.bolt` from disk after
    download completes.
  - `--timeout N` — scan wait time in seconds (default: 2).
  - `-o PATH` — save received archives (or extracted files) to a specific
    directory; the directory is created automatically if it does not exist.
- **GUI integration** (FLTK frontends, Linux and Windows):
  - Create dialog gains a **Send over network** action alongside the existing
    **Save** action; choosing it opens a receiver picker populated by a live
    LAN scan.
  - The receiver picker shows each host's friendly name, IP address, and
    accept mode.
- **Platform coverage** — LAN networking is enabled on all 32-bit and 64-bit
  targets that have a POSIX socket layer or WinSock2: Linux (x86-64, ARM,
  ARM64, RISC-V), musl static, macOS, Haiku, FreeBSD, NetBSD, illumos, GNU
  Hurd, Windows (CLI and FLTK GUI), and all tier-3/tier-4 cross targets.
  16-bit and bare-metal targets are not affected.
- **Bug fix** — `boltpack listen -o DIR` in save mode (no `--autoextract`) now
  creates the output directory if it does not already exist, rather than
  failing silently and discarding the received archive.

### Archive format

Format version remains **2** (no archive-format changes in this release). The
`creator_ver` bytes in archives written by BoltPack 1.3 are `0x01, 0x03`.

### Codec table

| Method | Name    | Intended use                                          |
| ------ | ------- | ----------------------------------------------------- |
| 0      | Stored  | Incompressible data or raw fallback                   |
| 1      | BoltLite    | Small-memory and 16-bit compatible compression        |
| 2      | miniLZO | Fast general-purpose compression; current default     |
| 3      | Fluff   | High compression ratio; all full-build targets        |
| 4      | zstd    | Best compression ratio; modern 32/64-bit targets only |

---

## v1.2 — June 2026

**Version:** 1.2
**Status:** Stable

### Changes

- **Archive format version 2** — `BP_FORMAT_VERSION` bumped from 1 to 2. All
  version checks are now strict equality: archives from v1.0 and v1.1 are
  rejected with `BP_ERR_VERSION`, and v1.2 archives are rejected by older
  readers. No backward-compatibility layer.
- **Improved BoltLite codec** (method 1):
  - Sliding window expanded from 2 048 to 4 096 bytes, improving compression
    ratio on most real-world files.
  - Back-distance encoding replaces the old absolute ring-buffer position.
    The decoder now reads matches directly from the output buffer; the scratch
    buffer for decompression is eliminated entirely (was 2 KB minimum).
  - Token format is now a 12+4 bit back-distance/length split (was 11+5
    absolute-position/length). Back-distance range 1–4 096, match length 3–18.
  - Lazy matching and BST-based encoder retained; encoder scratch grows from
    14 900 to 29 200 bytes (fits comfortably in the 16-bit far data segment).
- **Mini build** (`bpmini`) version bumped to 1.1. Shares the improved BoltLite
  codec; decoder scratch eliminated on 16-bit targets as well.
- Archive format version is now **2**; `creator_ver` in new archives: `0x01, 0x02`.

### Codec table

| Method | Name    | Intended use                                          |
| ------ | ------- | ----------------------------------------------------- |
| 0      | Stored  | Incompressible data or raw fallback                   |
| 1      | BoltLite    | Small-memory and 16-bit compatible compression        |
| 2      | miniLZO | Fast general-purpose compression; current default     |
| 3      | Fluff   | High compression ratio; all full-build targets        |
| 4      | zstd    | Best compression ratio; modern 32/64-bit targets only |

---

## v1.1 — June 2026

**Version:** 1.1  
**Status:** Stable

### What's new

- **Method 4: zstd (Zstandard)** — best compression ratio in BoltPack, enabled
  by default on all modern 32/64-bit targets (Linux, macOS, Windows, ARM64,
  RISC-V). Vintage/16-bit targets return `BP_ERR_UNSUPPORTED_METHOD` when
  encountering a method-4 archive.
- New CLI flag `-Z` / `--zstd` selects zstd; combines with `-l 1`–`-l 9` to
  map to zstd native levels 1, 2, 3, 5, 7, 9, 12, 15, 19.
- Block size auto-set to 256 KB when `-Z` is used without `-b`.
- GUI compression dialog (FLTK frontends) shows a "zstd" radio button on builds
  with `BOLTPACK_ZSTD`, with Fast / Normal / Best level selector.
- zstd 1.5.7 vendored as `lib/zstd/` (single-file amalgamation, BSD 2-Clause).

### Archive format

Format version remains **1** (archive-level major version unchanged). The new
method value `4` fits within the existing single-byte `method` field and is
defined in the v1.0 specification. A BoltPack 1.0 reader encountering a method-4
archive returns the already-defined `BP_ERR_UNSUPPORTED_METHOD` error code.

The `creator_ver` bytes in new archives written by BoltPack 1.1 are
`0x01, 0x01`.

### Compression summary

| Method | Name    | Intended use                                          |
| ------ | ------- | ----------------------------------------------------- |
| 0      | Stored  | Incompressible data or raw fallback                   |
| 1      | BoltLite    | Small-memory and 16-bit compatible compression        |
| 2      | miniLZO | Fast general-purpose compression; current default     |
| 3      | Fluff   | High compression ratio; all full-build targets        |
| 4      | zstd    | Best compression ratio; modern 32/64-bit targets only |

---

## v1.0 — April 2026

**Version:** 1.0  
**Status:** Stable

## Highlights

BoltPack 1.0 is the first stable release of the archive format and reference
implementation.

- Stable archive format version 1 with little-endian headers, UTF-8 archive
  paths, Adler-32 checksums, and block-based storage.
- Four archive methods:
  - Method 0: Stored
  - Method 1: BoltLite
  - Method 2: miniLZO
  - Method 3: Fluff
- Full frontend and core support for modern and vintage targets, including the
  16-bit mini build path.
- FLTK GUI frontend for Linux x86-64, Windows 32-bit, and Windows 64-bit.
- Windows Total Commander WCX plugin plus the `boltpack-config.exe`
  configuration utility.
- Canonical documentation set for format, algorithms, architecture,
  portability, and frontend behavior.

## Compression Summary

BoltPack 1.0 ships the following compression choices:

| Method | Name    | Intended use                                      |
| ------ | ------- | ------------------------------------------------- |
| 0      | Stored  | Incompressible data or raw fallback               |
| 1      | BoltLite    | Small-memory and 16-bit compatible compression    |
| 2      | miniLZO | Fast general-purpose compression; current default |
| 3      | Fluff   | Best compression ratio in the full build          |

The archive layer applies the 99% threshold per block, so a block may be stored
raw even when the entry method remains 1, 2, or 3.

## CLI Notes

Current CLI frontends use these method-selection conventions unless a specific
platform documents a smaller subset:

- no compression flag: miniLZO default
- `-0`: stored
- `-1`: BoltLite
- `-F`: Fluff

Typical usage:

```sh
boltpack c archive.bolt files...
boltpack c -0 archive.bolt files...
boltpack c -1 archive.bolt files...
boltpack c -F archive.bolt files...

boltpack x archive.bolt
boltpack x -o outdir archive.bolt
boltpack l archive.bolt
boltpack i archive.bolt
boltpack t archive.bolt
```

## Platform Scope

BoltPack targets modern Unix-like systems, Windows, macOS, classic Mac OS,
Amiga, Atari, DOS, RISC OS, Haiku, Plan 9, and several additional native or
cross-compiled targets. The project is structured so the portable core remains
strict ANSI C89 while platform-specific I/O and UI concerns stay inside the
frontends.

## Documentation

The product documentation is centered on these canonical files under `docs/`:

- `specification-1.0.md`
- `format.md`
- `algorithms.md`
- `architecture.md`
- `portability.md`
- `frontends.md`

Support documentation remains available where it adds value without duplicating
the canonical set:

- `docs/BUILD.md`
- `docs/mini.md`
- `docs/README.md`

Process documentation now lives outside `docs/`, primarily in `dev/`.

## Validation

Relevant repository checks for the stable release include:

```sh
make
python3 test/check_boltpack.py
python3 test/test_roundtrip.py
python3 test/test_fluff.py
python3 test/test_format.py
```

## Known Constraints

- No multithreading.
- No archive encryption.
- No solid archive mode.
- Frontends may expose only a subset of supported methods where platform
  constraints require it.

## Mac OS 9 68k Fixes

- The `macos9-68k` HQX package now writes classic BinHex CRC values using raw
  `crc_hqx(...)` results with no two-zero-byte finalization. That matches the
  de facto behavior used by classic decoders and fixes the deterministic `bad
crc` failures seen with the previous flushed-CRC variant.
- The Retro68 Mac OS 9 68k frontend now builds against the shared core archive
  and compression headers directly instead of relying on drift-prone local
  copies. The stale local header/types had diverged far enough to produce bogus
  `BP_ERR_IO` failures when creating directory entries in new archives.
- Mac 68k diagnostic logging now preserves the sign of BoltPack error codes, so
  `-1` is logged as `-1` instead of `1`.

## Version History

### 1.4

- Archive format version bumped to 3 (strict; v1.3 and earlier archives rejected).
- Per-block CRC-32 in block headers (12-byte v3 block headers).
- 16-byte file-table header with `total_orig` sum field.
- Unix permission bits in entry `flags`; stored and restored on Linux.
- `archive_size` field is normative for v3 (reader must verify).
- Adler-32 fully retired; `checksum_type = 1` (CRC-32) mandatory in v3.
- zstd multi-threaded compression on Linux CLI and FLTK GUI.
- Mini build bumped to 1.2: lazy work-buffer allocation, hoisted extract buffers, DOS/Win16 frontend completions.
- Append bug fix, codec reentrancy, security fixes, layering cleanup.
- `creator_ver` in new archives: `0x01, 0x04`.

### 1.3

- LAN send/receive added (`listen`, `scan`, `--send`). Enabled on all 32-bit+ POSIX and Windows targets.
- FLTK GUI gains Send over network action on Linux and Windows.
- `creator_ver` in new archives: `0x01, 0x03`.

### 1.2

- Archive format version bumped to 2 (strict; v1.0/v1.1 archives rejected).
- BoltLite codec improved: 4 096-byte sliding window, back-distance encoding, decoder scratch eliminated.
- Mini build bumped to 1.1; shares improved BoltLite codec.
- `creator_ver` in new archives: `0x01, 0x02`.

### 1.1

- Method 4 (zstd 1.5.7) added. Enabled by default on modern targets.
- GUI compression dialogs updated with zstd option.
- `creator_ver` in new archives: `0x01, 0x01`.

### 1.0

- First stable BoltPack archive format release.
- Stable four-method numbering finalized as stored, BoltLite, miniLZO, Fluff.
- Fluff v0x05 documented as the current native high-ratio codec.
- FLTK GUI frontend ships for Linux x86-64, Windows 32-bit, and Windows 64-bit.
- Pre-built binaries available for 18 platforms including Amiga 68k, Atari TOS,
  Classic Mac OS 9 68k, Haiku, MS-DOS (16-bit and 32-bit), GNU Hurd, WebAssembly,
  and all major Linux ABI variants.
- Documentation consolidated around canonical product docs plus narrow support
  docs.
