# Release Notes

## 1.12 — Archive format 7, volume sets and optimal Fluff level 9

- Bolt Mini is bumped to version 1.4: its core reads and writes archive
  format 7 (see below), so mini archives no longer exchange with format 6
  frontends.

- Fluff levels 1 and 2 use a new fast parser. Level 1 encodes about twice as
  fast as before (54 MB/s against 27 MB/s on a five-file Silesia sample) and
  compresses about 1.3 points worse (30.9% against 29.5%); level 2 matches the
  old level 1's ratio at 1.4 times its speed. Levels 3-8, including the default
  level 8, produce the same bytes as before, and the decoder and the format are
  unchanged.
- Fluff level 9 (`--best`) uses a price-based optimal parser in builds with the
  fast-Fluff encoder. It finds the cheapest sequence of literals, matches and
  repeat distances over a 4096-byte window, pricing each token from the FSE
  tables (the previous block's, refined with the current block's counts). On
  the Silesia corpus the output is 3.8% smaller (59,990,583 bytes against
  62,350,401 with one thread, 256 KiB blocks); encoding takes about twice as
  long with one thread and 1.65 times as long with the automatic threads.
  Decoding is unchanged, the format is unchanged and older decoders read the
  archives. Other builds, including the retro ones, keep the previous level 9
  parser; `-DFLUFF_NO_OPTIMAL` disables it.
- `-l N` and the `--light`, `--standard` and `--best` presets are honoured when
  Fluff compresses with threads (`-j`, and the automatic parallel mode for
  large files); the parallel encoder had been using level 8 whatever was asked.

- The mini core (`src/core/mini/archiver.c`) reads and writes archive format
  7, so the 16-bit DOS, Win16, OS/2 16-bit, CP/M-86 and Linux mini CLIs
  exchange archives with the Linux and DOS CLIs again, and no longer with
  format 6 frontends. The 16-bit DOS CLI also writes and reads volume sets:
  `bpmini c -v 1.44M --pause A:\BACKUP.BLT DOCS` writes `BACKUP.000`,
  `BACKUP.001`, ... `BACKUP.BLT`, and `l`, `t`, `i` and `x` accept any volume,
  asking for missing disks at the console. The other mini CLIs read and write
  single-volume archives and report a volume set as such.
- 16-bit DOS and Win16 mini CLIs: files of 32 KiB or more can be archived with
  the default 32 KiB blocks. A full block made the 16-bit `read()` count
  negative, so `bpmini c` failed with "I/O error" on any such file.
- Format 7 writer: an entry that fits in one block is compressed before its
  header is written, recorded as Stored when compression does not help, and
  given the smallest standard block size that holds it. Small files in an
  archive made with the 64-bit default (Fluff, 256 KiB blocks) can therefore
  be extracted by the mini CLIs, which have no Fluff and allow 32 KiB blocks.
  Larger entries keep the requested method and block size.
- DOS CLI: with long file names (LFN) enabled, extraction no longer fails with
  "staged output path changed" and no longer leaves `BPxxxxxx.TMP` files
  behind, and add/delete no longer report "archive changed while opening".
  DJGPP invents inode numbers when DOS gives no starting cluster, and an open
  handle and its path can then report different inodes and creation times.
  The checks now compare the stage path with itself, verify its size, and
  ignore invented inode numbers and the creation time where a handle is
  compared with a path.
- DOS CLI: extraction on a DOS without long file names no longer refuses
  lower-case 8.3 names. The DOS CLI stores names in lower case there (as
  DJGPP reports them), so it could not extract its own archives; `-o` with a
  lower-case directory name was refused for the same reason.
- Multi-volume archives (archive format 7: the Linux CLI, the POSIX CLIs built
  from it, and the 32-bit DOS CLI). `boltpack create -v 1.44M backup.bolt docs`
  splits an archive over
  volumes of at most the given size, named `backup.000`, `backup.001`, ... with
  the last one `backup.bolt`, for diskettes, CDs or other media. Sizes use
  K, M, G (x1000) or Ki, Mi, Gi (x1024), so `1.44M`, `720K` and `700M` fit
  their media. `--pause` asks for new media before each volume. The set is
  written in one pass with no temporary copy, so it can go straight onto
  removable media. List, test, extract and info accept any volume of a set
  and ask for missing ones when run from a terminal; otherwise extraction
  checks that every volume is present before writing anything. Each volume
  records its set and number, so a wrong or damaged disk is recognised when it
  is opened. A block at the end of a volume is cut to fill it, so volumes are
  full to within a few hundred bytes and the block size is independent of the
  volume size. Add and delete work on single-volume archives only. On DOS a
  diskette set is typically `boltpack c -v 1.44M --pause A:\BACKUP.BLT DOCS`,
  giving `BACKUP.000`, `BACKUP.001`, ... `BACKUP.BLT`; DOS volumes are limited
  to 2 GiB.
- Archive format 7 ([specification 2.0](docs/specification-2.0.md)) replaces
  format 6 in these CLIs, for single-volume archives too. They read and write
  format 7 only. Every CLI and GUI now uses the format 7 core,
  `src/core/archiver.c`; the format 6 core was removed (archives from older
  releases need an older release to extract). The Windows, macOS, Haiku,
  OS/2, Plan 9, Amiga, Atari, X68000, QL, RISC OS, Win16 and Linux mini CLIs
  also take `-v SIZE` and `--pause`; the GUIs read one volume at a time and
  tell the user to use the CLI for a set.

- Amiga CLI and GUI: when `BP_TZ_OFFSET` is not set, the time zone now comes
  from the Locale preferences (AmigaOS 2.1 and later), so file times are
  stored instead of being recorded as unknown. `locale.library` is opened only
  when needed, so AmigaOS 2.04 is still supported.
- Amiga GUI: fixed a garbled window title after opening an archive; protection
  bit and comment metadata entries are no longer listed as extra files; the
  progress counter no longer exceeds the file total; the partial-preservation
  requester now says which information was not kept.
- Archive information in the Amiga, Linux and FLTK (Linux/Windows) frontends
  no longer counts metadata entries as files. It reports them separately, for
  example `22 (11 files, 0 directories, 11 metadata)`. The Linux CLI list
  summary and the FLTK status bar count files the same way, and the FLTK GUIs
  show `-` instead of 1970-01-01 for unknown modification times. The core adds
  `bp_archive_info_ex()`, which also returns the number of metadata entries;
  `bp_archive_info_t` is unchanged.
- Atari GUI: Create no longer fails on Hatari with a Windows host GEMDOS
  drive. The unfinished file's date is now checked after it is closed, because
  such drives may report a stale date while the file is open. If Create still
  fails, the alert names the unfinished file that was kept. Long alerts are
  wrapped to fit the GEM alert box, the window title no longer uses a
  temporary buffer, and the progress counter and archive information count
  files instead of entries, so metadata is no longer counted as files.
- Release packages: text files are converted for the target system. DOS,
  Windows, OS/2 and Atari packages use CRLF line endings; DOS and OS/2 use code
  page 437 and the Atari package plain ASCII, with characters the target lacks
  transliterated (an em dash becomes `--`) instead of the whole file staying
  UTF-8. Packages for 8.3 systems (DOS, Windows 3.x, 16-bit OS/2, Atari) name
  the release notes `RELNOTES.TXT`. The Atari package has its own README, and
  the generic README no longer lists options that do not exist. The Windows
  CLI packages no longer contain a stray object file, and the WebAssembly
  package now includes `boltpack.wasm`, without which it could not run.
- BoltLZ is faster on Motorola 68000-family builds (Atari, Amiga, Classic Mac
  68k, X68000). On an emulated 8 MHz 68000, default-level compression is 1.9x
  faster (3.3 to 6.5 KB/s on mixed Silesia data), fast-level compression 2.8x
  and decompression 1.4x. The encoder avoids the 68000's missing 32-bit
  multiply and odd-address word access, and hashes each position once.
  Compressed output is up to 0.25% larger; the archive format is unchanged
  and other CPUs compile to the same code as before.
- Optional hand-written 68000 assembly for the BoltLZ encoder and decoder
  loops (`make atari amiga macos9-68k BOLTLZ_M68K_ASM=1`). Its output, result
  codes and error handling are identical to the C code. On an emulated 8 MHz
  68000, default-level compression is a further 1.9x faster and
  decompression 2.0x (64 to 131 KB/s on mixed data); on a 32 MHz 68030 1.7x
  and 1.8x, and 2.0x and 2.1x with a `-m68020` build.
  `make atari-hatari-boltlz-asm-test` compares it with the C code in Hatari.
  Off by default in `make`; the release packages ship it (see below).
- The assembly builds are the default in the Atari, Amiga, Sinclair QL, Sharp
  X68000 and Classic Mac 68k packages. The programs in the package root are
  the 68000 assembly builds, which run on every CPU of the family. The Atari
  and Amiga packages add a `68020` folder with both programs built for 68020
  to 68060 (they do not start on a 68000), and every package keeps the plain C
  build in a `C_BUILD` folder as a fallback; archives are identical with all
  of them. Built with `make atari-optimized`, `amiga-optimized`,
  `macos9-68k-optimized`, `ql-optimized` and `x68k-optimized`.
- The 16-bit DOS, Windows 3.x and OS/2 1.x packages likewise ship the build
  with the 8086 assembly BoltLite decoder as `bpmini.exe`, which extracts and
  tests faster, with the plain C build in `C_BUILD`
  (`make dos16-optimized win16-optimized os2-16-optimized`); archives are
  identical. The new RISC OS package (`boltpack-VERSION-riscos-ARM.zip`) does
  the same with the ARM assembly BoltLZ match scan (`make riscos-optimized`),
  about three times faster at compression in the RPCEmu emulator. It was
  tested there (RISC OS 5.30: create, test and extract with both builds,
  archives exchanged with the Linux CLI) but not on real hardware.
- RISC OS: the plain C build (`make riscos`) crashed on start with
  "undefined instruction" because it lacked the ARM flags the optimized build
  used (`-marm -mcpu=arm610 -mlibscl`); both builds now share them.
- Classic Mac 68k has three packages: `boltpack-VERSION-mac68k.hqx` is the
  68000 assembly build for any 68k Mac, `mac68k-68020.hqx` the build for 68020
  and later, and `mac68k-c.hqx` the plain C build. The download page now lists
  the Mac 68k packages, which it previously skipped (`.hqx` and the `mac68k`
  name were not recognized).
- Sinclair QL: the c68 compiler's `MC68000` macro now selects the 68000
  BoltLZ code, which the QL build had missed. At original QL speed (sQLux
  68008 timing) BoltLZ compression is 2.2x faster (0.56 to 1.25 KB/s on mixed
  data); decompression is unchanged, as c68 keeps the default decoder.
- Sinclair QL: the package's `boltpack` is an assembly-optimized build (the
  plain C build is `C_BUILD/boltpack`, `c_build_boltpack` in the UQLX
  container), built with `make ql-optimized`. c68's assembler cannot read GNU syntax, so
  `src/core/boltlz_m68k_c68.s` carries the same position-independent machine
  code as data words; `scripts/gen_boltlz_m68k_c68.py` regenerates it and
  `make boltlz-m68k-c68-sync-test` checks it. At original QL speed BoltLZ
  compresses at about 7 KB/s and decompresses at about 49 KB/s (from 0.56
  and 14 KB/s in 1.11). `make ql-sqlux-boltlz-asm-test` compares it with the
  C build in sQLux; its compressed output is identical to the Atari builds.
- Sharp X68000: built for the first time with a real Human68k toolchain
  (elf2x68k, `m68k-xelf-gcc`, now the Makefile default) and run in the run68
  emulator: create, test, list and extraction work and Linux verifies the
  archives. Fixed a missing `fileno` prototype and a progress message that
  reported every file as "(stored)" because it read the FAT metadata entry
  instead of the file's. Its 68000 BoltLZ C path is active automatically.
  `scripts/install-x68k-tools.sh` installs the toolchain and a run68 with a
  fix for its 16-bit `CMPA.W` comparison
  (`scripts/patches/run68x-cmpa-w.patch`), which had made correct compiled
  code appear to fail. Not yet run on a real X68000.
- Sharp X68000: the package has an X68000 README, ASCII text with CRLF line
  endings. Its `boltpack.x` is the assembly-optimized build (`make
  x68k-optimized`; the C build is in `C_BUILD`), whose output in run68 is
  identical to the Atari builds.
  `make x68k-run68-boltlz-asm-test` compares it with the C build.

## 1.11 — Native metadata preservation and usable application transfers

- Archive format remains at version 6. Bolt Mini remains at version 1.3;
  no mini codec or format changes.

- Added a usable round-trip acceptance gate (`make usable-roundtrip-test`),
  with explicit pass/fail/unavailable reporting and retained evidence. It runs
  restored Linux applications with all codecs, a DOS16 program on FAT12, and
  optional Atari and Amiga application checks through existing emulator runners.
  The combined 2026-11-13 run passed Linux, DOS16, Atari TOS 1.62 and Amiga
  checks on the host and in emulators; physical hardware remains unqualified.

- Added same-system transfer and hardware-testing guides describing what each
  frontend preserves, known limitations and how to verify native systems.

- 64-bit Linux, Windows, macOS, Haiku and Plan 9 CLI builds, and the POSIX
  wrappers built on the Linux frontend, now default to Fluff with 256 KiB
  blocks. 32-bit builds keep BoltLZ. `-2` selects BoltLZ and `-b` sets the block
  size. Target profiles that exclude Fluff keep their own codec rules.
- Linux and Windows FLTK GUIs enable multithreaded Fluff by default on
  systems with more than one hardware thread. Saved settings take precedence.
- New native attribute container (metadata namespace `0xFD`, see the
  specification section 2.3.1.2). The macOS CLI stores extended attributes
  other than the resource fork and Finder info, such as Finder tags and
  download URLs, and restores them on macOS. Other readers skip the entry and
  report it as unsupported metadata; archive edits keep it. Archive format 6 is
  unchanged. The container has been exercised only on a Linux host with a
  shim, not on a Mac.
- The macOS CLI reports every attribute, resource fork and Finder info it
  cannot store or restore, with the item, the reason and `errno`, plus a
  one-line summary. `BOLTPACK_DEBUG=1` traces each attribute. Directory Finder
  info is now restored, and a partial result caused only by the Classic
  creation date not being restored is explained.
- Linux: `create` and `add` now store `user.*` extended attributes and POSIX ACLs
  in the native attribute container and record hard links; `extract` restores
  them (hard links only to an identical file it extracted itself). `--security-attrs`
  adds `security.*` and `trusted.*` (file capabilities, SELinux labels). Items that
  cannot be archived (FIFOs, sockets, devices) are now named in a warning instead of
  being skipped silently. Target profiles and Interchange Profile 1 carry none of
  this.
- The Linux FLTK GUI now preserves Linux extended attributes, POSIX ACLs,
  hard links, modes and symbolic links during creation and extraction,
  including selected and flattened extraction. Security attributes are
  controlled by a session-only option.
- Sinclair QL: a file's QDOS type and dataspace (what an executable needs to run)
  are now stored in a small `BP_OS_QL` metadata entry and restored on extraction.
  The QL tool also could not archive any file that has a header on sQLux (its size
  check counted the host file's header bytes and the create aborted); that is fixed.
  Verified in sQLux, not on a real QL.
- Windows CLI: directory symlinks and junctions are no longer followed. A link
  that pointed back up the tree made the whole create fail, and a link to a folder
  outside the tree archived that folder's files. Each link is now reported and
  skipped.
- The Atari CLI and GEM GUI now accept Atari ST accented letters in names (for
  example `café.txt`), which TOS stores as exact bytes; before, extraction refused
  them, so a file with an accented name archived on one Atari could not be
  extracted on another. Names other systems produce for an Atari keep the
  stricter ASCII rule. Spaces remain refused because the Atari C library cuts
  names at a space. Checked on TOS 1.06, 1.62 and 2.05 in Hatari.
- The Atari CLI now extracts archives that lack explicit directory entries, such
  as `boltpack c a.blt dir/file` made on Linux; it used to fail with "cannot
  inspect destination".
- QNX, HP-UX, IRIX, NeXTSTEP, SCO, Tru64 and pre-7.3 AIX can extract again.
  Their wrappers extract by pathname, which cannot be made race-safe, so they
  only write into a new directory they create private to the user (`-o DIR`,
  which must not exist) and open it to its normal permissions afterwards. The
  shared source also no longer needs the `openat()` family to compile in that
  mode. Verified on a Linux host only; none of these systems has been built or
  run.
- The NeXTSTEP wrapper no longer requires `statvfs()` for the temporary-space
  check, allowing builds on systems without that interface.
- The macOS source kits now include the shared headers the CLI needs; the
  previous kits stopped at the first `#include`.
- Source kits now include `VERSION`, standalone builds report 1.11, and
  macOS app bundles read their version from `VERSION` instead of an older
  entry in the release notes.
- The macOS CLI now builds for Mac OS X 10.5 and earlier PowerPC systems in
  principle: `src/frontend/posix/legacy_at.h` emulates the `openat()` family
  and `futimens()` with `fchdir()` on single path components, keeping
  extraction anchored to held directory descriptors. The `ppc` kit build
  enables it. It is verified on a Linux host only, not yet on a PowerPC Mac.
- Finder info records written by the macOS CLI now use the same 10-byte layout
  as the Mac OS 9 frontend, so type and creator codes transfer between the two.
  The macOS reader still accepts the older 18-byte records.

## 1.10 — Target profile parity and zstd removal

- Windows and macOS CLIs now support the shared Linux full, Linux mini,
  generic POSIX, Plan 9, and Haiku BFS target profiles, including automatic
  aliases and name reports.
- The Linux and Windows FLTK GUIs expose a single Compatibility Profile
  dropdown with `None`, `Interchange Profile 1`, and all target profiles,
  plus automatic aliases; the last selection is remembered.
- Method 4 (zstd) has been removed. Use Fluff for maximum ratio. Readers
  reject method-4 archives with `BP_ERR_UNSUPPORTED_METHOD`.
- Removing zstd substantially reduces the modern CLI and GUI binary sizes.
- Bolt Mini remains at version 1.3; no mini codec or format changes.

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
