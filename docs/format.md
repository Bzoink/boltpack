# BoltPack Archive Format (`.bolt` / `.blt`)

**Format version:** 1  
**Byte order:** Little-endian (all multi-byte integers, on all architectures)  
**Status:** Stable overview

## Overview

A `.bolt` file consists of three sections laid out sequentially:

```text
┌──────────────────┐
│   Archive Header │  Fixed size
├──────────────────┤
│   File Entry 1   │  Entry header + compressed data
│   File Entry 2   │
│   ...            │
│   File Entry N   │
├──────────────────┤
│   File Table     │  Directory of all entries (for fast listing)
├──────────────────┤
│   Archive Footer │  Points back to file table
└──────────────────┘
```

The footer at the end points to the file table, allowing the archiver to read the directory without scanning the entire file.

This document is the concise format overview. For the normative field rules,
decoder requirements, and method-specific wire details, see
`specification-1.0.md`. For the current codec behavior behind those method
IDs, see `algorithms.md`.

## Archive Header (32 bytes)

| Offset | Size | Field          | Description                                                          |
| ------ | ---- | -------------- | -------------------------------------------------------------------- |
| 0      | 4    | `magic`        | `42 4F 4C 54` ("BOLT")                                               |
| 4      | 2    | `version`      | Format version (currently `0x0001`)                                  |
| 6      | 2    | `flags`        | Reserved, must be `0x0000`                                           |
| 8      | 4    | `file_count`   | Number of file entries (max ~4 billion)                              |
| 12     | 1    | `creator_os`   | OS that created this archive (see table)                             |
| 13     | 1    | `creator_cpu`  | CPU architecture of creator (see table)                              |
| 14     | 2    | `creator_ver`  | BoltPack version that created the archive (major.minor, 1 byte each) |
| 16     | 8    | `archive_size` | Total archive size in bytes                                          |
| 24     | 8    | `reserved`     | Reserved, must be zero                                               |

## File Entry

Each file entry is stored sequentially after the header. It consists of an entry header followed by the compressed data.

### Entry Header (variable size)

| Offset | Size | Field             | Description                                                                                                              |
| ------ | ---- | ----------------- | ------------------------------------------------------------------------------------------------------------------------ |
| 0      | 2    | `header_size`     | Total entry header size in bytes (offset 0 to start of compressed data)                                                  |
| 2      | 2    | `name_len`        | Length of file name in bytes (no null terminator)                                                                        |
| 4      | N    | `name`            | File name (UTF-8, forward slashes for path sep)                                                                          |
| 4+N    | 8    | `original_size`   | Uncompressed file size in bytes                                                                                          |
| 12+N   | 8    | `compressed_size` | Compressed data size in bytes (all blocks combined)                                                                      |
| 20+N   | 4    | `checksum`        | Adler-32 of the **uncompressed** data (all blocks)                                                                       |
| 24+N   | 1    | `method`          | Compression method: `0` = stored, `1` = LZSS LZ77, `2` = fastlz, `3` = fluff LZ77+FSE, `4` = zstd (modern targets only) |
| 25+N   | 1    | `checksum_type`   | Checksum algorithm: `0` = Adler-32                                                                                       |
| 26+N   | 4    | `block_size`      | Max uncompressed block size (0 = single block)                                                                           |
| 30+N   | 1    | `entry_type`      | `0` = data, `1` = Mac resource fork, `2` = metadata, `3` = symlink, `4` = directory                                      |
| 31+N   | 1    | `platform`        | Origin platform (uses `creator_os` values, see table)                                                                    |
| 32+N   | 8    | `mtime`           | Modification time (Unix epoch, seconds since 1970-01-01 UTC, little-endian uint64)                                       |
| 40+N   | 2    | `flags`           | Reserved, must be `0x0000`                                                                                               |

For format version 1, `header_size` is always `42 + N` (where N = `name_len`). See [Forward Compatibility](#forward-compatibility-header_size) for the rationale.

### Compressed Data (Block-Based)

If `block_size` is `0`, the compressed data is a single blob of `compressed_size` bytes (simple mode, for small files).

If `block_size` is nonzero, the data consists of a sequence of **compressed blocks**, each preceded by a small block header:

| Offset | Size | Field             | Description                                      |
| ------ | ---- | ----------------- | ------------------------------------------------ |
| 0      | 4    | `block_comp_size` | Compressed size of this block                    |
| 4      | 4    | `block_orig_size` | Uncompressed size of this block (≤ `block_size`) |
| 8      | ...  | `block_data`      | Compressed block data                            |

The last block may have `block_orig_size < block_size`. The decompressor reads blocks sequentially until `original_size` bytes have been produced.

**Recommended block sizes:**

| System memory | Suggested `block_size` |
| ------------- | ---------------------- |
| ≥ 16 MB       | 262,144 (256 KB)       |
| 4–16 MB       | 65,536 (64 KB)         |
| 1–4 MB        | 32,768 (32 KB)         |
| 512 KB–1 MB   | 16,384 (16 KB)         |
| < 512 KB      | 8,192 (8 KB)           |

The compressor chooses a block size at archive creation time. Any decompressor can handle any block size, since it only needs to allocate `block_size + overhead` for one block at a time.

If a block has `block_comp_size == block_orig_size`, that block payload is raw.
Method 0 always uses raw payloads, and methods 1-3 may also emit a raw block as
a per-block fallback when compression fails the 99% threshold rule.

If `method` is `0` (stored) and `block_size` is `0`, the data is uncompressed and `compressed_size == original_size`.
If `method` is `0` (stored) and `block_size` is nonzero, each block's `block_comp_size == block_orig_size` and the data is uncompressed.

## File Table

The file table is a compact directory written after all file entries, allowing quick listing without reading all entry headers scattered through the file.

| Offset | Size | Field         | Description                                        |
| ------ | ---- | ------------- | -------------------------------------------------- |
| 0      | 4    | `table_magic` | `54 42 4C 54` ("TBLT")                             |
| 4      | 4    | `entry_count` | Number of entries (must match header `file_count`) |

Followed by `entry_count` records:

| Offset | Size | Field             | Description                                                                     |
| ------ | ---- | ----------------- | ------------------------------------------------------------------------------- |
| 0      | 2    | `name_len`        | Length of file name                                                             |
| 2      | N    | `name`            | File name                                                                       |
| 2+N    | 8    | `original_size`   | Uncompressed size                                                               |
| 10+N   | 8    | `compressed_size` | Compressed size                                                                 |
| 18+N   | 1    | `entry_type`      | `0` = data, `1` = resource fork, `2` = metadata, `3` = symlink, `4` = directory |
| 19+N   | 1    | `platform`        | Origin platform                                                                 |
| 20+N   | 8    | `mtime`           | Modification time (Unix epoch, seconds, uint64 LE)                              |
| 28+N   | 8    | `offset`          | Byte offset of the file entry from start of archive                             |

## Archive Footer (20 bytes)

| Offset | Size | Field          | Description                                         |
| ------ | ---- | -------------- | --------------------------------------------------- |
| 0      | 8    | `table_offset` | Byte offset of the file table from start of archive |
| 8      | 8    | `table_size`   | Size of the file table in bytes                     |
| 16     | 4    | `footer_magic` | `54 4C 4F 42` ("TLOB" — "BOLT" reversed)            |

## Design Notes

### Format Version and Compatibility

The `version` field in the archive header is the **format version**, not the BoltPack software version. It changes only when the binary layout changes in an incompatible way.

**Rules:**

- If `archive.version > reader.version` → **refuse to open** with `BP_ERR_VERSION`. Do not attempt partial extraction. BoltPack is open source (BSD-2-Clause) — users can always obtain a newer version.
- If `archive.version <= reader.version` → **extract normally**. The reader must support all format versions up to and including its own.
- If `archive.version == 0` or `archive.version > 0xFFFE` → **reject as corrupt** (`BP_ERR_CORRUPT`). Version 0 is never valid.

Currently the only format version is `1`. The `version` field is 2 bytes (max `0xFFFF`), allowing 65,534 future revisions — far more than will ever be needed.

The `creator_ver` field is separate — it records the BoltPack _software_ version that created the archive and is purely informational. A reader never makes decisions based on `creator_ver`.

### Endianness

**Little-endian** is the canonical byte order for all multi-byte integers in the format. Big-endian hosts (PowerPC, 68k, SPARC) byte-swap when reading and writing using trivial helper functions. This is the same convention used by ZIP, PNG, and most modern formats. It avoids any dependency on `htonl`/`ntohl` (unavailable on all targets). The swap code is ~10 lines of C89.

### File Extension

- **`.bolt`** — preferred extension on systems that support long file names.
- **`.blt`** — short alternative for systems restricted to three-character extensions (DOS 8.3, Atari TOS, early Windows).

Both are equivalent. BoltPack accepts either extension (and any other — identification is by magic bytes, not extension). Neither collides with any mainstream file format.

### Magic Identification

The first four bytes of every `.bolt` file are `42 4F 4C 54` ("BOLT"). This is sufficient for `file(1)` identification on Unix systems. A magic definition file is provided at `magic/boltpack.magic` in the repository.

To register system-wide:

```bash
sudo cp magic/boltpack.magic /etc/magic.d/   # or append to /usr/share/misc/magic
sudo update-mime-database /usr/share/mime     # if using shared-mime-info
```

Per-user (no root):

```bash
mkdir -p ~/.local/share/magic
cp magic/boltpack.magic ~/.local/share/magic/
```

Expected output:

```bash
$ file myarchive.bolt
myarchive.bolt: BoltPack archive, version 1, creator OS 1
```

### Creator identification

The archive header records what OS, CPU, and BoltPack version created the archive. This is informational — it does **not** affect extraction behavior. Any BoltPack on any platform can extract any archive. The creator fields help with:

- Diagnostics ("this archive was made on an Amiga 68k, why does it have resource forks?" — it doesn't)
- Listing (`boltpack l` can show "Created by BoltPack 1.0 on Mac OS / PowerPC")
- Future compatibility decisions

#### `creator_os` values

| Value  | OS                        | Description                                                                                |
| ------ | ------------------------- | ------------------------------------------------------------------------------------------ |
| `0x00` | Unknown / generic         | Fallback when the host OS cannot be identified at compile time                             |
| `0x01` | Linux                     | All distributions and kernel versions; typically x86, x86-64, ARM, PowerPC, MIPS           |
| `0x02` | Mac OS (classic, 1–9)     | Pre-Unix Apple OS (1984–2001); 68k and PowerPC; resource-fork and Type/Creator dependent   |
| `0x03` | Mac OS X / macOS          | Unix-based Apple OS (2001–present); PowerPC, x86, x86-64, ARM64                            |
| `0x04` | AmigaOS (1.x–3.x)         | Original Commodore Amiga OS (1985–1994); 68k only; protection bits and file comments       |
| `0x05` | AmigaOS 4.x               | Post-Commodore continuation (2004–present); PowerPC only (AmigaOne, Sam4x0, X5000)         |
| `0x06` | MorphOS                   | Amiga-compatible OS (2000–present); PowerPC (Pegasos, Mac mini G4, Efika, PowerBook G4)    |
| `0x07` | AROS                      | Open-source Amiga-compatible OS (1995–present); x86, 68k, ARM, PowerPC                     |
| `0x08` | MS-DOS                    | Microsoft disk operating system (1981–2000); 8086/8088 through Pentium; FAT attributes     |
| `0x09` | Windows (9x/NT/2000+)     | All desktop Windows versions; x86, x86-64, ARM64; FAT/NTFS attributes                      |
| `0x0A` | FreeBSD                   | BSD Unix derivative (1993–present); x86, x86-64, ARM64, PowerPC, MIPS, RISC-V              |
| `0x0B` | NetBSD                    | Highly portable BSD (1993–present); runs on 60+ architectures including VAX, Alpha, SPARC  |
| `0x0C` | OpenBSD                   | Security-focused BSD (1995–present); x86, x86-64, ARM64, SPARC64, PowerPC, MIPS64          |
| `0x0D` | Solaris / SunOS           | Sun/Oracle Unix (1982–present); SPARC and x86/x86-64                                       |
| `0x0E` | IRIX                      | SGI Unix (1988–2006); MIPS only (Indy, Indigo, O2, Octane, Origin)                         |
| `0x0F` | HP-UX                     | Hewlett-Packard Unix (1984–present); PA-RISC and Itanium                                   |
| `0x10` | AIX                       | IBM Unix (1986–present); PowerPC / POWER only (RS/6000, pSeries)                           |
| `0x11` | Atari TOS / MiNT          | Atari ST/TT/Falcon OS (1985–1993); 68k; TOS uses FAT-like attributes, MiNT adds Unix perms |
| `0x12` | RISC OS                   | Acorn/RISC OS Ltd (1987–present); ARM only; filetype in load address, no file extensions   |
| `0x13` | OS/2                      | IBM/Microsoft OS (1987–2001, eComStation/ArcaOS continuing); x86; FAT/HPFS attributes      |
| `0x14` | BeOS / Haiku              | Media-oriented OS (1995–2001, Haiku 2001–present); x86, PowerPC (BeOS), x86-64 (Haiku)     |
| `0x15` | OpenVMS                   | DEC/HP OpenVMS (VAX/Alpha/Itanium/x86_64)                                                  |
| `0x16` | z/OS                      | IBM mainframe Unix-like (z/Architecture)                                                   |
| `0x17` | WebAssembly               | Wasm modules (Node.js / browser runtime)                                                   |
| `0x18` | NeXTSTEP / OPENSTEP       | NeXT OS (68k/x86/PPC/SPARC)                                                                |
| `0x19` | QNX Neutrino              | QNX RTOS (x86/x86_64/ARM)                                                                  |
| `0x1A` | Plan 9                    | Plan 9 from Bell Labs (x86/ARM/MIPS/PowerPC)                                               |
| `0x1B` | Tru64 UNIX (OSF/1)        | Digital/Compaq/HP UNIX on Alpha                                                            |
| `0x1C` | SCO OpenServer / UnixWare | SCO UNIX on x86 (legacy)                                                                   |
| `0x1D` | CP/M-86                   | Vintage 8086 CP/M-86 with Aztec C / Turbo C                                                |
| `0x1E` | Sinclair QL               | Sinclair QL (QDOS), 68008                                                                  |
| `0x20` | GNU Hurd                  | GNU's Hurd microkernel OS (x86, x86-64); POSIX-compatible API; GCC ports                   |
| `0x21` | Sharp X68000              | Sharp X68000 (Human68k), 68000                                                             |

Values `0x80`–`0xFF` are reserved for user/private use. Third-party ports or forks may claim a private value without coordinating upstream.

#### `creator_cpu` values

| Value  | Architecture              |
| ------ | ------------------------- |
| `0x00` | Unknown / generic         |
| `0x01` | x86 (i386, i486, Pentium) |
| `0x02` | x86-64 (AMD64)            |
| `0x03` | PowerPC (32-bit)          |
| `0x04` | PowerPC (64-bit)          |
| `0x05` | Motorola 68k              |
| `0x06` | ARM (32-bit)              |
| `0x07` | ARM64 / AArch64           |
| `0x08` | MIPS (32-bit)             |
| `0x09` | MIPS (64-bit)             |
| `0x0A` | SPARC                     |
| `0x0B` | Alpha                     |
| `0x0C` | PA-RISC                   |
| `0x0D` | RISC-V (32-bit)           |
| `0x0E` | RISC-V (64-bit)           |
| `0x0F` | SuperH (SH)               |
| `0x10` | 6502 / 65C816             |
| `0x11` | Z80                       |
| `0x12` | 8086 / 8088 (16-bit x86)  |
| `0x13` | e2k (Elbrus)              |
| `0x14` | z/Architecture            |
| `0x15` | SPARC64                   |
| `0x16` | Itanium (IA-64)           |

Values `0x80`–`0xFF` are reserved for user/private use.

#### `creator_ver`

Two bytes: `major` (offset 14) and `minor` (offset 15). BoltPack 1.0 stores `0x01, 0x00`. BoltPack 1.1 stores `0x01, 0x01`. BoltPack 1.2 stores `0x01, 0x02`.

#### Per-entry `platform` field

Note: the per-entry `platform` field in file entries serves a **different purpose** — it indicates what platform the file's metadata (`entry_type=2`) should be interpreted for. It uses the same values as `creator_os`.

An archive created on Linux (`creator_os=0x01`) can contain Mac entries with `platform=0x02` (e.g., if someone packs Mac files transferred to Linux). The `creator_os` says where the packing happened; `platform` says what the metadata means.

### 64-bit sizes and offsets

All file sizes and archive offsets are stored as **8-byte (64-bit) little-endian unsigned integers**. This allows individual files and archives up to 2^64 bytes (~16 exabytes).

In C89 (which has no `uint64_t` or `long long`), 64-bit values are handled as 8 raw bytes via read/write helpers, and as a `{ unsigned long lo; unsigned long hi; }` struct for arithmetic. Only basic operations are needed: addition, comparison, and conversion to/from 32-bit for platforms where files cannot exceed 4 GB.

On vintage platforms, `fseek()`/`ftell()` use `long` (32-bit), limiting practical I/O to ~2 GB. The format still supports larger values so that:

- Archives created on modern systems are valid
- Vintage tools can cleanly reject files that exceed their platform limits rather than silently corrupting

### File names and paths

- **UTF-8 encoding**, regardless of host platform. The frontend converts to/from native encoding (MacRoman on classic Mac OS, local code page on DOS/Windows, Shift-JIS on Japanese systems, etc.).
- **Forward slashes** (`/`) as path separators, regardless of host convention.
- **No leading slash** — all paths are relative.
- **No `..` components** — paths must not escape the extraction root.
- **Maximum 65,535 bytes** per path (2-byte `name_len`). Well beyond any real-world limit (Linux PATH_MAX is 4,096).

### Checksum

**Adler-32** is used for file integrity checking. It is:

- Very fast (faster than CRC-32)
- Adequate for detecting accidental corruption

The `checksum_type` field allows a future format version to add stronger algorithms (e.g., CRC-32) without breaking backward compatibility.

### File count

The 4-byte `file_count` supports up to 4,294,967,295 files per archive. In practice, the file table size is the limiting factor on vintage systems (each table entry is ~30+ bytes, so 1 million files ≈ 30 MB of table).

### Modification timestamp

**`mtime`** stores the file's last modification time as a `uint64` (little-endian) in **Unix epoch** (seconds since 1970-01-01 00:00:00 UTC).

The field is 8 bytes on disk for future-proofing. 32-bit platforms that only support `time_t` as a 32-bit value write the timestamp in the low 4 bytes and zero the high 4. This covers dates up to 2106. The full 64-bit range extends to ~584 billion years, far exceeding any practical need.

The file table stores `mtime` as 8 bytes (uint64, little-endian), identical in size to the entry header. Note: many existing frontends currently populate and restore only the low 32 bits — they call `bp_u64_from_u32((bp_uint32)st.st_mtime)` when packing and use `entry->mtime.lo` when restoring via `utime()` or platform-specific APIs. Readers should use the full 64-bit `mtime` when present; frontends may be updated to preserve full 64-bit timestamps where supported.

The modification timestamp is stored in the **entry header** (not in platform metadata) because it is universal — every platform has a modification date.

Each frontend converts to/from the native time format:

| Platform       | Native format                        | Conversion                 |
| -------------- | ------------------------------------ | -------------------------- |
| Unix / Linux   | `time_t` (Unix epoch)                | Direct — no conversion     |
| Classic Mac OS | Seconds since 1904-01-01             | Subtract 2,082,844,800     |
| AmigaOS        | Days/minutes/ticks since 1978-01-01  | Convert to/from Unix epoch |
| DOS / Windows  | Packed FAT date/time                 | Convert to/from Unix epoch |
| RISC OS        | 5-byte centiseconds since 1900-01-01 | Convert to/from Unix epoch |
| Atari TOS      | Same as DOS FAT format               | Convert to/from Unix epoch |

A value of `0` means "unknown" — the frontend should leave the file's date as-is (typically the extraction time).

Creation date is not stored in the entry header. For classic Mac OS, the creation date is preserved in the Mac-specific metadata entry (`entry_type=2`).

### Resource Forks (Classic Mac OS)

Classic Mac OS files have **two forks**: a data fork and a resource fork. Mac applications, extensions, and many documents are useless without their resource fork. Archivers that ignore resource forks destroy Mac software.

BoltPack handles this with the `entry_type` field:

| `entry_type` | Meaning                                                                                 |
| ------------ | --------------------------------------------------------------------------------------- |
| `0`          | **Data** — the file's primary content (data fork on Mac, the entire file on other OSes) |
| `1`          | **Resource fork** — Mac resource fork for the preceding data entry                      |
| `2`          | **Metadata** — platform-specific metadata blob (see below)                              |
| `3`          | **Symlink** — symbolic link (data is the target path)                                   |

A classic Mac OS file is stored as **two or three consecutive entries**:

1. `entry_type=0` — the data fork (file name: `path/to/MyApp`)
2. `entry_type=1` — the resource fork (file name: `path/to/MyApp`, same name)
3. `entry_type=2` — Finder metadata (file name: `path/to/MyApp`, same name) — optional

The entries share the **same file name**. The `entry_type` field discriminates them. This design means:

- The **archiver core** needs no knowledge of what a resource fork is — it's just bytes with a type tag.
- The **Mac frontend** reads both forks from disk (via `FSRead` on the data fork, opening the resource fork with `OpenRF` or `FSpOpenRF`), adds them as separate entries, and reverses the process on extraction.
- A **Linux frontend** extracting a Mac archive can write **`filename`** (data fork) and **`filename.rsrc`** (resource fork) as separate files, or optionally recombine them using a library.
- The file table includes all entries, so `boltpack l` shows which files have resource forks.

### Platform Metadata (entry_type = 2)

Optional metadata entries store platform-specific file attributes. The `platform` field in the entry header uses `creator_os` values (see table) and determines how the metadata blob is interpreted.

Platforms are grouped into families that share the same metadata format:

#### Unix family (platform = `0x01` Linux, `0x03` macOS, `0x0A`–`0x10` BSDs/Solaris/IRIX/HP-UX/AIX, `0x18` NeXTSTEP/OPENSTEP, `0x19` QNX Neutrino, `0x1A` Plan 9, `0x1B` Tru64, `0x1C` SCO OpenServer, `0x20` Hurd)

| Offset | Size | Field  | Description                                   |
| ------ | ---- | ------ | --------------------------------------------- |
| 0      | 2    | `mode` | File permission bits (low 9 bits of `mode_t`) |

The 9-bit mode encodes `rwxrwxrwx` (owner/group/other):

| Bits | Mask    | Meaning   |
| ---- | ------- | --------- |
| 8–6  | `0x1C0` | Owner rwx |
| 5–3  | `0x038` | Group rwx |
| 2–0  | `0x007` | Other rwx |

Security bits (setuid `0x800`, setgid `0x400`, sticky `0x200`) are **not stored**. BoltPack is for transferring programs and data, not for system security. The user can set security attributes after extraction if needed.

**Why this matters:** Without the execute bit, extracted binaries and scripts fail with "Permission denied". This is the single most critical metadata for any Unix-like system. User/group ownership is not stored — the extracting user becomes the owner.

**Minimum for programs to work:** The frontend must restore at least the owner execute bit (`0x040`) for files that had it when archived. A simple frontend can apply `mode & 0x1FF` via `chmod()`.

#### Classic Mac OS (platform = `0x02`)

| Offset | Size | Field          | Description                     |
| ------ | ---- | -------------- | ------------------------------- |
| 0      | 4    | `type`         | Finder Type code (e.g., `APPL`) |
| 4      | 4    | `creator`      | Finder Creator code             |
| 8      | 2    | `finder_flags` | Finder flags                    |
| 10     | 4    | `crdate`       | Creation date (Mac epoch)       |
| 14     | 4    | `mdate`        | Modification date (Mac epoch)   |

**Why this matters:** Without Type/Creator, the Finder doesn't know what application opens the file, and applications won't have the correct icon or be launchable by double-click. Without the resource fork (`entry_type=1`), the application literally won't run.

#### AmigaOS family (platform = `0x04` AmigaOS 1-3, `0x05` AmigaOS 4, `0x06` MorphOS, `0x07` AROS)

| Offset | Size | Field         | Description                        |
| ------ | ---- | ------------- | ---------------------------------- |
| 0      | 4    | `protection`  | AmigaOS protection bits (HSPARWED) |
| 4      | 1    | `comment_len` | Length of file comment             |
| 5      | N    | `comment`     | AmigaOS file comment               |

**Why this matters:** The Script (`S`) protection bit determines whether a file can be executed via the CLI. The Pure (`P`) bit affects whether the system can make the program resident. Without correct protection bits, scripts won't run and programs may behave unexpectedly.

#### DOS / Windows / OS/2 family (platform = `0x08` DOS, `0x09` Windows, `0x13` OS/2)

| Offset | Size | Field        | Description                 |
| ------ | ---- | ------------ | --------------------------- |
| 0      | 1    | `attributes` | File attributes (RHSA bits) |

| Bit  | Meaning   |
| ---- | --------- |
| 0x01 | Read-only |
| 0x02 | Hidden    |
| 0x04 | System    |
| 0x20 | Archive   |

**Why this matters for programs:** Mostly cosmetic — DOS/Windows programs run based on extension (.COM, .EXE, .BAT), not attributes. But System and Hidden flags matter for OS files, and Read-only prevents accidental overwrites. Good to preserve.

#### Atari TOS / MiNT (platform = `0x11`)

| Offset | Size | Field        | Description                                            |
| ------ | ---- | ------------ | ------------------------------------------------------ |
| 0      | 1    | `attributes` | TOS file attributes (same as DOS: RHSA)                |
| 1      | 2    | `mode`       | MiNT Unix permissions (optional, `0x0000` if pure TOS) |

**Why this matters:** TOS programs are identified by extension (.PRG, .TOS, .APP, .TTP, .GTP) — no execute bit needed. But under MiNT (the multitasking extension), Unix permissions apply and the execute bit matters. Storing both covers both environments.

#### RISC OS (platform = `0x12`)

| Offset | Size | Field        | Description                       |
| ------ | ---- | ------------ | --------------------------------- |
| 0      | 4    | `load_addr`  | Load address / filetype encoding  |
| 4      | 4    | `exec_addr`  | Exec address / timestamp          |
| 8      | 4    | `attributes` | RISC OS file attributes (RWLR/rw) |

**Why this matters:** RISC OS does **not** use file extensions to identify file types. The 12-bit filetype is encoded in the top 12 bits of `load_addr` (when bits 20–31 are `0xFFF`). Without the correct filetype, the Filer won't know what the file is, and executables (filetype `0xFF8` = Absolute) won't run. This is as critical as resource forks on Mac.

#### BeOS / Haiku (platform = `0x14`)

| Offset | Size | Field       | Description                                            |
| ------ | ---- | ----------- | ------------------------------------------------------ |
| 0      | 2    | `mode`      | Unix permissions (same as Unix family)                 |
| 2      | 1    | `mime_len`  | Length of MIME type string                             |
| 3      | N    | `mime_type` | MIME type (e.g., `application/x-vnd.Be-elfexecutable`) |

**Why this matters:** BeOS/Haiku are Unix-like underneath, so the execute bit is needed for binaries. The MIME type determines file associations in Tracker (the file manager). BFS supports arbitrary named attributes, but MIME type is the most important one.

#### Platforms with no metadata needed

Some platforms in the `creator_os` list don't require platform-specific metadata for programs to work. They either fall under an existing family above or have no file attributes meaningful to preserve cross-platform:

| `creator_os` | Platform     | Notes                                                                                            |
| ------------ | ------------ | ------------------------------------------------------------------------------------------------ |
| `0x15`       | OpenVMS      | Record-structured files are flattened on extraction; no standard portable metadata format        |
| `0x16`       | z/OS         | EBCDIC data stored as raw bytes; code-page conversion is a frontend concern, not a metadata blob |
| `0x17`       | WebAssembly  | No persistent file system; in-memory FS state is not meaningful to transfer between hosts        |
| `0x1D`       | CP/M-86      | Read-only is the only useful attribute; too minor to warrant a dedicated format                  |
| `0x1E`       | Sinclair QL  | QL has a simple protection byte; no separate blob needed                                         |
| `0x21`       | Sharp X68000 | Human68k uses DOS-like attributes; no separate blob needed                                       |

No `entry_type=2` blob format is defined for these. A frontend may safely skip all `entry_type=2` entries whose `platform` field it does not recognise.

---

Frontends on platforms that don't understand the metadata simply skip `entry_type=2` entries. No data is lost — the file data and resource fork are separate entries.

### Other notes

- **Directories** are stored as `entry_type=4` entries. The entry `name` is the directory path (with trailing `/`). `original_size=0`, `compressed_size=0`, no data blocks. Directory entries carry metadata (permissions, timestamps) just like files. Non-empty directories are also stored as entries so their metadata is preserved — even if extracting files would create the directory implicitly.
- **Empty directories** are preserved. The frontend emits a `entry_type=4` entry for every directory it encounters during recursion, empty or not.
- **Empty files** (0 bytes) are valid. `original_size=0`, `compressed_size=0`, `checksum` is the Adler-32 of zero bytes (`1`), no compressed blocks follow the entry header.
- **Compression method field** allows future addition of alternative compressors without breaking format compatibility.
- **Stored (method 0)** is used when compression does not reduce size (incompressible data). The archiver compresses each block, compares sizes, and falls back to stored if the compressed block is not smaller than the original.
- **Checksums are per-file, not per-block.** The `checksum` field in the entry header covers the entire uncompressed file data across all blocks. A corrupt block is detectable at the file level but the specific bad block is not pinpointable from the checksum alone. Block-level checksums were considered but omitted to keep the format simple and block headers small (8 bytes). The fastlz and fluff decompressors still detect many corruption cases through internal consistency checks.
- **Archive creation requires seekable output.** The header contains `archive_size` and the footer contains `table_offset`, both unknown until all entries are written. The archiver seeks back to patch the header after finalizing. This means archives cannot be created on non-seekable outputs (pipes, stdout). Extraction and listing do support sequential reading — the footer is read first (seek to end minus 20 bytes), then the file table, then individual entries by offset.

### Forward Compatibility (`header_size`)

The `header_size` field at the start of every entry header enables **forward compatibility without a format version bump**. A reader that doesn't recognize new fields added after `flags` can still:

1. Read `header_size` (2 bytes at offset 0)
2. Parse the fields it understands
3. Skip to `entry_start + header_size` to find the compressed data

This means a future BoltPack version could append new fields to the entry header (e.g., a creation timestamp, an ACL blob) and older readers would simply ignore them — they'd still extract the file correctly because `header_size` tells them exactly where the data begins.

**Rules:**

- Writers must set `header_size` to the actual total size of the entry header they write.
- Readers must use `header_size` (not a hardcoded value) to locate the compressed data.
- If `header_size` is smaller than the minimum expected for the format version, reject as corrupt.
- Unknown trailing bytes between known fields and `entry_start + header_size` are silently ignored.

### Symbolic Links (entry_type = 3)

A symlink entry stores a symbolic link. The entry `name` is the link path (where the link lives), and the entry's **data payload** is the link target path (what it points to), stored as raw UTF-8 bytes with no null terminator.

- `original_size` and `compressed_size` equal the byte length of the target path
- `method` is always `0` (stored) — symlink targets are too short to benefit from compression
- `checksum` covers the target path bytes
- `block_size` is `0`

The target path uses the same conventions as file names: UTF-8, forward slashes. It may be relative (`../lib/libfoo.so.2`) or absolute (`/usr/bin/python3`), preserving what the original symlink pointed to.

**Cross-platform extraction:**

| Extracting on        | Behavior                                                  |
| -------------------- | --------------------------------------------------------- |
| Linux / macOS / BSDs | Create symlink via `symlink()`                            |
| AmigaOS 2+           | Create soft link via `MakeLink()`                         |
| Haiku                | Create symlink via `symlink()`                            |
| Classic Mac OS       | Skip with message (or create Finder alias, optional)      |
| RISC OS              | Skip with message                                         |
| DOS / Windows 9x     | Skip with message (no symlink support)                    |
| Atari TOS            | Skip with message                                         |
| NeXTSTEP / OPENSTEP  | Create symlink via `symlink()`                            |
| QNX Neutrino         | Create symlink via `symlink()`                            |
| Plan 9               | Skip with message (Plan 9 uses bind mounts, not symlinks) |
| Sinclair QL          | Skip with message (QDOS has no symlinks)                  |
| Sharp X68000         | Skip with message (Human68k has no symlinks)              |
