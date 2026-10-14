# BoltPack Specification v1.0

**Format Version:** 4 (only)
**Software Version:** 1.6
**Status:** Development
**Release Date:** 2026-07-04

> **Archive format 4.** Method 2 uses BoltLZ format 2. Readers accept only
> version 4 and reject every other archive version with `BP_ERR_VERSION`.
> The header, file-table, entry, and block CRC-32 fields are mandatory.

## 1. Introduction

BoltPack is a portable file archiver designed for maximum compatibility across vintage and modern computer systems — from IBM PC/DOS to contemporary Linux, Windows, and embedded platforms.

This specification defines the binary archive format (`.bolt` / `.blt` files), the mandatory decompression methods, and conformance requirements for any BoltPack implementation.

### 1.1 Scope

This document covers:

- **Archive format:** layout, header structures, entry records
- **Compression methods:** method definitions, block structure, encoding/decoding rules
- **Decompression:** mandatory algorithms, decoder specifications
- **Data integrity:** checksums, error detection, recovery
- **Portability:** byte order, character encoding, platform-independent data representation
- **Conformance:** requirements for readers, writers, and tools

Topics **not** covered here (see separate documentation):

- **Encoding strategies:** match finding algorithms, Huffman tree generation (implementation detail)
- **Performance tuning:** optimization techniques, memory trade-offs
- **CLI interface:** command syntax, option formats (platform-specific)
- **GUI/IDE integration:** Total Commander support, shell extensions

### 1.2 Normative References

- ANSI C99 / ISO C (for portable type definitions)
- RFC 1962 — The PPP Compression Control Protocol (LZW reference)
- RFC 1950 — ZLIB Compressed Data Format (Huffman coding patterns)
- BoltLZ format 2, defined in §3.3 (method 2)

### 1.3 Terminology

- **Archive:** a `.bolt` / `.blt` file containing zero or more entries (files, directories)
- **Entry:** a single file, symlink, or directory record in an archive
- **Block:** a variable-length chunk of compressed data (for large entries)
- **Payload:** the uncompressed content of an entry or block
- **Metadata:** modification time, permissions, platform origin (stored in entry headers)
- **Little-endian:** least-significant byte first (all multi-byte integers in this format)
- **UTF-8:** Unicode text encoding (all file names and paths in archives)

---

## 2. Archive Layer

### 2.1 Archive Layout

A BoltPack archive consists of three sequential sections:

```text
╔════════════════╦═════════════════════════════════╦════════════════╗
║Archive Header  ║          File Entries           ║ Footer Section ║
║  (32 bytes)    ║  (variable: entries + blocks)   ║  (~30+ bytes)  ║
╠════════════════╬═════════════════════════════════╬════════════════╣
│ Magic (BOLT)   │ Entry[0] Header + Compressed    │ File Table     │
│ Version        │   Data (blocks)                 │ Archive Footer │
│ File count     │ Entry[1] Header + Compressed    │                │
│ Creator info   │   Data (blocks)                 │                │
│ Archive size   │ ...                             │                │
│ Reserved       │ Entry[N] Header + Compressed    │                │
│                │   Data (blocks)                 │                │
╚════════════════╩═════════════════════════════════╩════════════════╝
```

**Offset discovery:** A reader MAY seek to the end of the file, read the 20-byte footer, extract `table_offset`, and jump directly to the file table for fast directory listing. **Readers MUST support sequential reading** (seeking not required) for streaming and embedded systems.

### 2.2 Archive Header (32 bytes)

| Offset | Size | Field            | Type   | Description                                                            |
| ------ | ---- | ---------------- | ------ | ---------------------------------------------------------------------- |
| 0      | 4    | `magic`          | u32 LE | `0x544C4F42` (bytes: `42 4F 4C 54` = "BOLT")                           |
| 4      | 2    | `format_version` | u16 LE | `0x0004`; readers reject every other archive version                 |
| 6      | 2    | `flags`          | u16 LE | Reserved; must be `0x0000`. Fail with `ERR_CORRUPT` if nonzero.        |
| 8      | 4    | `file_count`     | u32 LE | Number of entries (0 to 4,294,967,295)                                 |
| 12     | 1    | `creator_os`     | u8     | Platform that created the archive (see Section 2.6)                    |
| 13     | 1    | `creator_cpu`    | u8     | CPU architecture of creator (see Section 2.7)                          |
| 14     | 2    | `creator_ver`    | u8, u8 | BoltPack version (major, minor) at offset 14 and 15                    |
| 16     | 8    | `archive_size`   | u64 LE | Total size of `.bolt` file in bytes                                 |
| 24     | 4    | `table_crc`      | u32 LE | CRC-32 of the file-table region                                      |
| 28     | 4    | `header_crc`     | u32 LE | CRC-32 of header bytes `[0, 28)`                                     |

**Validation:**

- `magic != 0x544C4F42` → `ERR_NOT_ARCHIVE`
- `format_version != 4` → `BP_ERR_VERSION`
- `flags != 0x0000` → `ERR_CORRUPT`
- `header_crc != CRC-32(header[0..28))` → `ERR_CORRUPT`; after reading the
  file table, `table_crc != CRC-32(table region)` → `ERR_CORRUPT`.
- `archive_size != actual_file_size` → `ERR_TRUNCATED`. A reader MUST verify
  this after reading the footer. A writer MUST store the correct total size.

**Byte order:** All multi-byte integers are **little-endian** on all platforms. Decoders running on big-endian hosts (PowerPC, 68k, SPARC, Alpha) must byte-swap when reading. The swap cost is negligible (~10 lines of C89 code).

### 2.3 File Entries

After the header, entries are stored sequentially. Each entry consists of a variable-length header containing metadata, followed by compressed data (blocks or a single blob).

#### 2.3.1 Entry Header (variable size)

| Offset | Size | Field             | Type   | Description                                                               |
| ------ | ---- | ----------------- | ------ | ------------------------------------------------------------------------- |
| 0      | 2    | `header_size`     | u16 LE | Total entry header size (offset 0 to start of comp data)                  |
| 2      | 2    | `name_len`        | u16 LE | Length of file name in bytes (no null terminator; max 65535)              |
| 4      | N    | `name`            | bytes  | File name (UTF-8, forward-slash separated; path is relative)              |
| 4+N    | 8    | `original_size`   | u64 LE | Uncompressed size in bytes                                                |
| 12+N   | 8    | `compressed_size` | u64 LE | Compressed data size (all blocks combined)                                |
| 20+N   | 4    | `checksum`        | u32 LE | Checksum of uncompressed payload, per `checksum_type`                     |
| 24+N   | 1    | `method`          | u8     | Compression method (0=stored, 1=LZSS, 2=BoltLZ, 3=fluff, 4=zstd)          |
| 25+N   | 1    | `checksum_type`   | u8     | Checksum algorithm (1=CRC-32, the only value written; 0=Adler-32 retired) |
| 26+N   | 4    | `block_size`      | u32 LE | Max uncompressed block size; 0 = single block (no blocking)               |
| 30+N   | 1    | `entry_type`      | u8     | Entry type (0=data, 1=rsrc, 2=metadata, 3=symlink, 4=dir)                 |
| 31+N   | 1    | `platform`        | u8     | Metadata platform origin (OS code; same as `creator_os` table)            |
| 32+N   | 8    | `mtime`           | u64 LE | Modification time (Unix epoch seconds, UTC)                               |
| 40+N   | 2    | `flags`           | u16 LE | Entry flags (see below). Unknown bits MUST be accepted silently.          |

**Entry flags (format 4):**

| Bit(s) | Constant                   | Description                                                      |
| ------ | -------------------------- | ---------------------------------------------------------------- |
| 15     | `BP_ENTRY_FLAG_MODE_VALID` | Bits 0–11 contain valid Unix permission bits                     |
| 0–11   | `BP_ENTRY_FLAG_MODE_MASK`  | Unix mode bits (`st_mode & 0x0FFF`)                              |
| 12–14  | —                          | Reserved; writers MUST write zero; readers MUST accept any value |

When bit 15 is set, bits 0–11 encode the 12-bit Unix permission+type-low field
(`st_mode & 0x0FFF`). An extractor that supports POSIX permissions SHOULD apply
`chmod(path, flags & 0x0FFF)` after writing the extracted file.

**Key constraints:**

- `header_size` **MUST** equal `4 + name_len + 38` for format version 4.
- `name_len` MUST NOT be zero (empty names forbidden).
- `name` MUST be valid UTF-8; decoder MAY reject on invalid sequences.
- `name` MUST NOT contain `..` components or absolute paths (security check).
- `name_len > 2^16−1` → entry is invalid; skip with `ERR_CORRUPT`.
- If `entry_type == 4` (directory), `original_size` and `compressed_size` MUST both be zero.

**Entry type details:**

- **`0` (data):** Regular file or stream. Payload contains file content.
- **`1` (rsrc):** Mac OS resource fork. Payload contains Classic Mac resource data. Ignore on non-Mac platforms.
- **`2` (metadata):** Metadata entry (extended attributes, ACLs). Payload contains platform-specific data. Ignored by most extractors.
- **`3` (symlink):** Symbolic link. Payload contains the link target path (UTF-8, forward-slash separated, max 65535 bytes).
- **`4` (directory):** Directory record. `original_size` and `compressed_size` MUST be zero. Payload is absent. Platform-specific creation code applies (e.g., NTFS ACL, POSIX mode bits extracted from `mtime` / `flags`).

#### 2.3.2 Compressed Data Layout

The compressed payload immediately follows the header at byte offset `header_size`.

**Case 1: `block_size == 0` (single blob)**

The entire `compressed_size` bytes are treated as one uncompressed chunk. Decompress in-place to get `original_size` bytes.

**Case 2: `block_size > 0` (blocked)**

The payload is a sequence of 12-byte block headers followed by block data:

| Offset | Size | Field             | Type   | Description                                             |
| ------ | ---- | ----------------- | ------ | ------------------------------------------------------- |
| 0      | 4    | `block_comp_size` | u32 LE | Size of this block's compressed data                    |
| 4      | 4    | `block_orig_size` | u32 LE | Uncompressed size of this block (≤ `block_size`)        |
| 8      | 4    | `block_crc`       | u32 LE | CRC-32 of `block_data` (the compressed bytes); see §4.1 |
| 12     | M    | `block_data`      | bytes  | Compressed block data (M = `block_comp_size`)           |

The `block_crc` field covers the compressed bytes **before** decompression. A
reader MUST verify `block_crc` before decompressing; mismatch → `ERR_CHECKSUM`.

**Semantics:**

- All blocks EXCEPT the last have `block_orig_size == block_size`.
- The last block MAY have `block_orig_size < block_size`.
- `block_comp_size == block_orig_size` means the block payload is stored raw.
  Method 0 always uses raw blocks; methods 1-4 MAY also use raw blocks when a
  compression attempt fails the 99% threshold rule.
- Total uncompressed bytes from all blocks MUST equal `original_size`.
- Total compressed bytes from all blocks MUST equal `compressed_size`, including
  the 12-byte headers.
- Decompressor reads blocks sequentially until `original_size` bytes have been produced.

**Block size recommendations** (for implementers choosing a block size when creating archives):

| System RAM | Suggested `block_size` | Rationale                                 |
| ---------- | ---------------------- | ----------------------------------------- |
| ≥ 16 MB    | 262,144 (256 KB)       | Maximize for modern hardware              |
| 4–16 MB    | 65,536 (64 KB)         | Balance compression / speed               |
| 1–4 MB     | 32,768 (32 KB)         | Backward-compatible default               |
| 512 K–1 MB | 16,384 (16 KB)         | Reduced window for memory                 |
| < 512 KB   | 8,192 (8 KB)           | Minimal overhead; suitable for DOS, Atari |

A reader MAY accept any block size; it only needs to allocate `block_size + codec_overhead` working memory per block.

---

## 3. Compression Methods

### 3.1 Method 0: Stored (Uncompressed)

**ID:** `0`
**Decoder complexity:** Trivial (byte copy)

For data that does not compress well, method 0 stores payload verbatim:

- `block_comp_size == block_orig_size`
- Decompress by reading exactly `block_comp_size` bytes.

**Use case:** Already-compressed data (JPEG, PNG, ZIP, executable binaries); rarely saves space after compression attempt.

### 3.2 Method 1: LZSS (LZ77 Sliding Window)

**ID:** `1`
**Codec:** LZSS (Okumura / Haruhiko Okumura variant, 1989)
**Decompressor complexity:** ~60 lines of C89; 2 048 bytes RAM
**Status:** Implemented in `src/core/lzss.c`

#### 3.2.1 Algorithm Parameters (Format-Fixed)

| Parameter            | Value  | Notes                                            |
| -------------------- | ------ | ------------------------------------------------ |
| Ring buffer size (N) | 2 048  | 11-bit position field                            |
| Max match length (F) | 34     | 5-bit field stores length−3; range 3–34          |
| Min match length     | 3      | Shorter sequences encoded as literals            |
| Initial fill byte    | `0x20` | Space character                                  |
| Position bits        | 11     |                                                  |
| Length bits          | 5      |                                                  |
| Flag bits per byte   | 8      | Bit 0 = first symbol; `1` = literal, `0` = match |

#### 3.2.2 Token Encoding

The compressed stream is a sequence of **flag bytes** followed by symbol data.
Each flag byte covers exactly 8 symbols; bit 0 applies to the first symbol:

- **bit = `1`:** the next byte is a **literal** (copy verbatim).
- **bit = `0`:** the next 2 bytes are a **match token**:

```text
Byte 0 = position bits 10..3  (high 8 bits of 11-bit ring-buffer position)
Byte 1 = [position bits 2..0, high 3 bits] | [(match_len - 3), low 5 bits]
```

Decoder sample:

```c
pos  = (byte0 << 3) | (byte1 >> 5);  /* 11-bit ring buffer index */
len  = (byte1 & 0x1F) + 3;           /* decoded match length     */
for (j = 0; j < len; j++) {
    c = ring_buf[(pos + j) & (N - 1)];
    ring_buf[r] = c;
    output(c);
    r = (r + 1) & (N - 1);
}
```

#### 3.2.3 Decoder Reference Algorithm

```c
memset(ring_buf, 0x20, N);       /* fill with space         */
r = N - F;                       /* write position          */

while (output_bytes_remaining > 0) {
    /* read flag byte */
    flags = read_byte();
    for (mask = 1; mask < 256; mask <<= 1) {
        if (flags & mask) {
            /* literal */
            c = read_byte();
            ring_buf[r] = c; output(c);
            r = (r + 1) & (N - 1);
        } else {
            /* match token */
            b0 = read_byte(); b1 = read_byte();
            pos = (b0 << 3) | (b1 >> 5);
            len = (b1 & 0x1F) + 3;
            for (j = 0; j < len; j++) {
                c = ring_buf[(pos + j) & (N - 1)];
                ring_buf[r] = c; output(c);
                r = (r + 1) & (N - 1);
            }
        }
    }
}
```

#### 3.2.4 Encoder Constraints

- Output buffer MUST be at least `original_size + original_size / 8 + 16` bytes.
- Compressor requires `LZSS_WORK_SIZE` (14 900) bytes of working memory.
- On 16-bit platforms (DOS, Win16, OS/2 1.x, CP/M-86) the working memory
  MUST be allocated as `static` to avoid stack overflow.

#### 3.2.5 Compatibility

Method 1 archives produced by any conforming BoltPack implementation MUST be
decompressable by any other conforming implementation regardless of platform.

### 3.3 Method 2: BoltLZ

**Method ID:** `2`
**Codec:** BoltLZ format 2, original BSD-2-Clause C89 implementation
**Maximum uncompressed block size:** 262,144 bytes

Each compressed block MUST begin with byte `0x02`. The remaining stream is
an ordered sequence of tokens and their payloads. Both encoder modes use
this representation; the compression level is not stored in the stream.

| Token bits | Meaning                                                               |
| ---------- | --------------------------------------------------------------------- |
| 7          | Reuse the previous positive match distance when set                   |
| 6–4        | Literal length: values 0–6 are inline; 7 adds a ULEB128 extension     |
| 3–0        | Match length: values 0–14 mean 4–18 bytes; 15 adds an extension to 19 |

Payload order is: token, optional literal-length extension, literal bytes,
optional match-length extension, and explicit distance unless bit 7 is set.
Length extensions and distances MUST be canonical unsigned LEB128, with at
most three bytes. A terminal byte zero after an earlier byte is noncanonical;
a continuation bit on the third byte or a third-byte payload above 16 is
invalid. All decoded lengths MUST fit the declared output block.

An explicit match distance MUST be positive and no greater than the number
of bytes already produced. It becomes the previous distance for later reuse.
The previous distance is initially invalid and persists through literals.
Matches copy from already-produced output, allowing overlap byte by byte.

A final literal-only sequence MUST have bit 7 clear, low four bits zero,
and at least one literal. Its literals MUST consume the exact remaining
compressed input and complete the exact expected output. A block MAY end
immediately after a match. The one-byte stream `0x02` represents empty output;
archive empty entries use Stored and have no data blocks.

Readers MUST reject unknown block versions, noncanonical/truncated fields,
invalid distances, extra sequences, or input/output overruns. Decoders MUST
operate within the declared buffers without caller-supplied padding. The
reference implementation is `boltlz_decompress()` in `src/core/boltlz.c`.
The conservative encoder output bound is `n + floor(n / 63) + 16` bytes.

### 3.4 Method 3: Fluff (LZ77 + FSE/tANS)

**ID:** `3`
**Codec:** Fluff
**Decompressor complexity:** Moderate (~1000 lines of C89)
**Speed:** ~467 MB/s decompression (modern 64-bit system)
**Compression ratio:** ~29% on curated corpus (best ratio among the non-zstd
methods)

#### 3.4.1 Overview

Fluff is BoltPack's native higher-ratio codec. It combines two stages:

1. **LZ77 match finding** — 256 KB sliding window; matches 3–258 bytes; 3-entry
   repeat-distance ring
2. **FSE/tANS entropy coding** — Finite State Entropy using tANS tables of size
   `L = 1024` (lit/len) and `L = 512` (distance)

The codec is **not DEFLATE-compatible**. The reference implementation is
`src/core/fluff.c`.

Blocks carry a stream version byte. Current FSE/tANS blocks use the compact
version `0x08` (grouped fast-fluff batches use `0x88` on the first block of each
reset group and `0x08` on later blocks). The decoder also accepts the earlier
FSE/tANS versions `0x07`/`0x87` and the original adaptive range coder `0x06`
for backward compatibility with archives written by previous releases.

Version `0x08` differs from `0x07` only in the header encoding (compact count
tables and narrower scalar fields, §3.4.4); the FSE bitstream and raw extra-bit
stream that carry the LZ77 tokens are byte-for-byte identical between the two.

#### 3.4.2 Block Layout — Versions `0x07` and `0x87` (FSE/tANS, current)

A v0x07 or v0x87 Fluff block is a fixed-size byte-aligned header followed by two
backwards binary streams:

```text
┌──────────────────────────────────────────────────────────────────────────────────┐
│                         BYTE-ALIGNED HEADER (≥ 694 bytes)                        │
│  stream hdr  │  litlen counts  │  dist counts  │  states  │  lengths             │
│   (4 bytes)  │   (578 bytes)   │   (96 bytes)  │ (8 bytes)│  (8 bytes)           │
├──────────────┴─────────────────┴───────────────┴──────────┴──────────────────────┤
│  FSE bitstream   │  Raw extra-bits stream                                         │
│  (fse_len bytes) │  (raw_len bytes)                                               │
└────────────────────────────────────────────────────────────────────────────────────┘
```

Both bitstreams are written **backwards** by the encoder; the decoder reads them
from the end towards the start.

#### 3.4.3 Stream Header (4 bytes, byte-aligned)

| Byte | Value            | Field         | Description                                                                                                          |
| ---- | ---------------- | ------------- | -------------------------------------------------------------------------------------------------------------------- |
| 0    | `0x46`           | `magic[0]`    | ASCII `'F'`; invalid value → `ERR_CORRUPT`                                                                           |
| 1    | `0x4C`           | `magic[1]`    | ASCII `'L'`; invalid value → `ERR_CORRUPT`                                                                           |
| 2    | `0x08` or `0x88` | `version`     | `0x08` = compact FSE/tANS block; `0x88` = compact reset block; `0x07`/`0x87` = legacy FSE/tANS; `0x06` = range coder |
| 3    | `0x12`           | `window_bits` | `18` = 256 KB window (informational; always 18)                                                                      |

Encoders MUST write version `0x08` for normal blocks. Encoders using grouped
reset batches MAY write `0x88` on the first block of a batch and MUST write
`0x08` on later blocks within that batch. Decoders MUST accept `0x06`, `0x07`,
`0x87`, `0x08`, and `0x88`. Any other version value MUST be rejected with
`ERR_CORRUPT`.

#### 3.4.3a Reset Blocks and Grouped Fast-Fluff

Version `0x87` uses the same byte layout as version `0x07`, but with one extra
semantic rule: before decoding the block body, the decoder MUST reset the Fluff
history window and repeat-distance ring for the current entry.

This reset form is used by the optional grouped fast-fluff encoder on modern
OpenMP-enabled builds. It lets a compressor process block groups in parallel
without inventing a new archive method or a second FSE block layout.

#### 3.4.3b Cross-Block History and Raw-Block Fallback

Fluff maintains a 256 KB history window across blocks within the same entry.
Later blocks may therefore refer back to bytes from earlier blocks of the same
file.

If a Fluff block does not beat the archive layer's 99% threshold, the encoder
stores that block raw by writing it with `block_comp_size == block_orig_size`
while leaving the entry method as Fluff.

Decoders MUST mirror the encoder's history state in that case:

1. Copy the raw block bytes verbatim to the output.
1. Feed those same raw bytes into the Fluff history window before decoding the
   next block of the same entry.

The reference implementation performs this history advance with
`fluff_ctx_feed_raw()` in `src/core/fluff.c`.

#### 3.4.4 FSE Count Table Header (v0x07/v0x87 only)

Immediately after the 4-byte stream header, the block contains two count tables
in a flat byte-aligned format — no bitstream packing.

**Lit/len count table** (578 bytes):

- 289 entries × 2 bytes each, little-endian `uint16`.
- Entry `i` holds the normalized frequency count for lit/len symbol `i`.
- All counts sum exactly to `FSE_LL_L = 1024`.
- Symbols with count 0 are unused (decoder assigns no states).

**Distance count table** (96 bytes):

- 48 entries × 2 bytes each, little-endian `uint16`.
- All counts sum exactly to `FSE_D_L = 512`.

**Validation:** reject any count table where the sum of non-zero entries does not
equal the required `L` value → `ERR_CORRUPT`.

#### 3.4.5 FSE State Fields (v0x07/v0x87 only)

After the count tables, four 4-byte little-endian `uint32` fields appear:

| Field      | Size | Description                                     |
| ---------- | ---- | ----------------------------------------------- |
| `ll_state` | 4    | Initial ANS state for the lit/len decode table  |
| `d_state`  | 4    | Initial ANS state for the distance decode table |
| `fse_len`  | 4    | Byte length of the FSE bitstream that follows   |
| `raw_len`  | 4    | Byte length of the raw extra-bits stream        |

**Total fixed header size:** 4 + 578 + 96 + 16 = **694 bytes**.

#### 3.4.6 FSE Bitstreams (v0x07/v0x87 only)

Immediately after the fixed header, the block contains two concatenated streams:

1. **FSE bitstream** (`fse_len` bytes) — carries the ANS-coded sequence of
   lit/len and distance symbol decisions. The encoder wrote this stream
   backwards (last token first); the decoder reads it from the end.
2. **Raw extra-bits stream** (`raw_len` bytes) — carries all length and
   distance extra bits that are not entropy-coded. Also written and read
   backwards.

**FSE decoding step (per symbol):**

```c
/* Read next FSE symbol from the LL table. */
sym     = ll_dtab[ll_state].symbol;
nbits   = ll_dtab[ll_state].num_bits;
new_base= ll_dtab[ll_state].new_base;
ll_state = new_base + read_bits_backwards(fse_bits, nbits);
```

The distance symbol is decoded from `d_dtab` by the same pattern.

**FSE table construction** follows the standard tANS spread algorithm: each
symbol `s` with normalized count `c_s` receives `c_s` states spread uniformly
across the table of size `L`.

#### 3.4.6a Block Layout — Versions `0x08` and `0x88` (compact FSE/tANS, current)

A v0x08/v0x88 block has the same structure as v0x07 — 4-byte stream header,
count tables, scalar fields, then the two backwards bitstreams — and the FSE
bitstream and raw extra-bits stream (§3.4.6) are **identical** to what v0x07
would produce for the same input. Only the header encoding is more compact, so
the fixed 694-byte header of v0x07 becomes a variable header that is typically
150–350 bytes smaller. `0x88` carries the same context-reset semantics as `0x87`
(§3.4.3a).

**Compact count tables.** Each table (lit/len, then distance) replaces the flat
`uint16` array with a left-to-right token stream over its `nsym` entries:

- A non-zero count `c` (`1 ≤ c ≤ L`) is written as `varint(c)`.
- A run of `k ≥ 1` consecutive zero counts is written as a `0x00` marker byte
  followed by `varint(k)`.

`varint` is LSB-first base-128 (LEB128). A varint of any value `≥ 1` has a
non-zero first byte, so the `0x00` zero-run marker is unambiguous. The decoder
reconstructs `nsym` entries and MUST reject the table (→ `ERR_CORRUPT`) if a
zero run overflows `nsym`, a value exceeds `L`, a varint is truncated, or the
final entries do not sum to exactly `L`.

**Compact scalar fields.** The four trailing fields shrink (R3):

| Field      | v0x07 | v0x08 | Description                                |
| ---------- | ----- | ----- | ------------------------------------------ |
| `ll_state` | 4     | 2     | Initial ANS state, lit/len (`< 1024`)      |
| `d_state`  | 4     | 2     | Initial ANS state, distance (`< 512`)      |
| `fse_len`  | 4     | 3     | FSE bitstream byte length (block ≤ 256 KB) |
| `raw_len`  | 4     | 3     | Raw extra-bits byte length                 |

All are little-endian. Total scalar tail: 10 bytes (vs. 16 in v0x07).

#### 3.4.7 Block Layout — Version `0x06` (Range Coder, legacy read-only)

A v0x06 block is a 4-byte byte-aligned stream header followed by a single
continuous bit stream (no tree or count table header):

```text
┌──────────────┬────────────────────────────────────────────────────┐
│ stream hdr   │  continuous adaptive range-coder bitstream         │
│  (4 bytes)   │  (tokens until EOB)                                │
└──────────────┴────────────────────────────────────────────────────┘
```

The range coder uses:

- A 9-bit binary tree model for lit/len symbols (512 probability nodes,
  `FL_LL_BITS = 9`).
- A 6-bit binary tree model for distance symbols (64 probability nodes,
  `FL_D_BITS = 6`).
- 12-bit fixed-point probability (`FL_PROB_MAX = 4096`); initial value 2048
  (50/50). Each decoded bit updates its node's probability with:
  `p' = p + (direction × (4096 − p) >> adapt_shift)`.
- No tree header or count tables are stored; the adaptive model rebuilds itself
  from the 50/50 prior at the start of each block.

Decoders MUST support v0x06 for reading. Encoders MUST NOT produce v0x06.

#### 3.4.8 Lit/Len Symbol Alphabet (289 symbols)

| Symbols | Meaning                |
| ------- | ---------------------- |
| 0–255   | Literal byte value     |
| **256** | **End-of-block (EOB)** |
| 257–285 | Match length codes     |
| 286–288 | Repeat-distance match  |

**Match length codes** — `length = base + read_bits(extra_bits)`:

| Symbol | Base | Extra bits | Symbol | Base | Extra bits |
| ------ | ---- | ---------- | ------ | ---- | ---------- |
| 257    | 3    | 0          | 272    | 31   | 2          |
| 258    | 4    | 0          | 273    | 35   | 3          |
| 259    | 5    | 0          | 274    | 43   | 3          |
| 260    | 6    | 0          | 275    | 51   | 3          |
| 261    | 7    | 0          | 276    | 59   | 3          |
| 262    | 8    | 0          | 277    | 67   | 4          |
| 263    | 9    | 0          | 278    | 83   | 4          |
| 264    | 10   | 0          | 279    | 99   | 4          |
| 265    | 11   | 1          | 280    | 115  | 4          |
| 266    | 13   | 1          | 281    | 131  | 5          |
| 267    | 15   | 1          | 282    | 163  | 5          |
| 268    | 17   | 1          | 283    | 195  | 5          |
| 269    | 19   | 2          | 284    | 227  | 5          |
| 270    | 23   | 2          | 285    | 258  | 0          |
| 271    | 27   | 2          |        |      |            |

Symbol 285 encodes length 258 exactly (no extra bits). Symbol 284 with extra=31
also produces 258; a conformant encoder uses symbol 285 for that length.

Symbols 286–288 are repeat-distance matches. The symbol selects which recent distance
to reuse:

| Symbol | Meaning                    |
| ------ | -------------------------- |
| 286    | reuse most-recent distance |
| 287    | reuse second-most-recent   |
| 288    | reuse third-most-recent    |

Symbols 286–288 are each followed by a 5-bit length-code index into the same table
above, plus any length extra bits.

#### 3.4.8a Repeat-Distance Ring State

Fluff tracks three recent distances:

- `rep0` — most recent distance
- `rep1` — second-most-recent distance
- `rep2` — third-most-recent distance

After every regular match or repeat-distance match, the matched distance is
applied to the ring with move-to-front semantics:

- If `distance == rep0` or `distance == 0`, the ring is unchanged.
- If `distance == rep1`, then `rep0` and `rep1` are swapped.
- If `distance == rep2`, then `rep2` becomes `rep0`, old `rep0` becomes
  `rep1`, and old `rep1` becomes `rep2`.
- Otherwise, `distance` becomes the new `rep0`, old `rep0` becomes `rep1`, and
  old `rep1` becomes `rep2`.

This is the state transition implemented by `fl_update_rep_ring()` in the
reference codec.

#### 3.4.9 Distance Symbol Alphabet (up to 48 symbols)

BoltPack v1.0 Fluff streams keep the DEFLATE layout for short distances, then use
denser four-way buckets for 513–32768 to cut inline distance extra bits.

`distance = base + read_bits(extra_bits)` (same for v0x06, v0x07, and v0x87):

| Symbol | Base | Extra bits | Symbol | Base   | Extra bits |
| ------ | ---- | ---------- | ------ | ------ | ---------- |
| 0      | 1    | 0          | 24     | 1537   | 8          |
| 1      | 2    | 0          | 25     | 1793   | 8          |
| 2      | 3    | 0          | 26     | 2049   | 9          |
| 3      | 4    | 0          | 27     | 2561   | 9          |
| 4      | 5    | 1          | 28     | 3073   | 9          |
| 5      | 7    | 1          | 29     | 3585   | 9          |
| 6      | 9    | 2          | 30     | 4097   | 10         |
| 7      | 13   | 2          | 31     | 5121   | 10         |
| 8      | 17   | 3          | 32     | 6145   | 10         |
| 9      | 25   | 3          | 33     | 7169   | 10         |
| 10     | 33   | 4          | 34     | 8193   | 11         |
| 11     | 49   | 4          | 35     | 10241  | 11         |
| 12     | 65   | 5          | 36     | 12289  | 11         |
| 13     | 97   | 5          | 37     | 14337  | 11         |
| 14     | 129  | 6          | 38     | 16385  | 12         |
| 15     | 193  | 6          | 39     | 20481  | 12         |
| 16     | 257  | 7          | 40     | 24577  | 12         |
| 17     | 385  | 7          | 41     | 28673  | 12         |
| 18     | 513  | 7          | 42     | 32769  | 13         |
| 19     | 641  | 7          | 43     | 40961  | 13         |
| 20     | 769  | 7          | 44     | 49153  | 14         |
| 21     | 897  | 7          | 45     | 65537  | 15         |
| 22     | 1025 | 8          | 46     | 98305  | 15         |
| 23     | 1281 | 8          | 47     | 131073 | 17         |

The encoder always selects the symbol with the highest base ≤ distance. Distances greater
than 262144 (the window size) or greater than the total bytes decompressed so far in the
file are invalid → `ERR_CORRUPT`.

#### 3.4.10 Token Stream

After the code-length stream, the bitstream contains tokens until end-of-block:

```text
block           ::= token* EOB
token           ::= literal | match | rep_match
literal         ::= sym(0..255)
match           ::= sym(257..285)  length_extra  dist_sym  dist_extra
rep_match       ::= sym(286..288)  len_idx(5 bits)  length_extra
EOB             ::= sym(256)
```

**Decoding a regular match:**

1. Decode a lit/len symbol. If 257–285, it is a length code.
2. Compute `length = LEN_BASE[sym − 257] + read_bits(LEN_EXTRA[sym − 257])`.
3. Decode a distance symbol from the distance tree.
4. Compute `distance = DIST_BASE[dist_sym] + read_bits(DIST_EXTRA[dist_sym])`.
5. Validate distance (see §3.4.9). Copy `length` bytes from `distance` bytes behind the
   current output position, using overlap-safe copy (see §3.4.11).

**Decoding a repeat-distance match:**

1. Decode lit/len symbol 286, 287, or 288.
2. Read a 5-bit length-code index and decode the match length using the same length table.
3. Reuse the most-recent, second-most-recent, or third-most-recent match distance.
4. Validate distance and copy as for a regular match.
5. Update the repeat-distance ring using the matched distance and the rule in §3.4.8a.

#### 3.4.11 Decompression Algorithm

`fluff_decompress_block_ctx()` dispatches on the stream version byte:

```c
/* 1. Validate stream header */
if (src[0] != 0x46 || src[1] != 0x4C)
    return ERR_CORRUPT;
version = src[2];
if (version != 0x07 && version != 0x87 && version != 0x06)
    return ERR_CORRUPT;

if (version == 0x07 || version == 0x87)
    return fl_decompress_fse(src, src_len, dst, dst_len, ctx);
else
    return fl_decompress_rc(src, src_len, dst, dst_len, ctx);
```

**Versions `0x07` and `0x87` (FSE/tANS) — `fl_decompress_fse()`:**

If the stream header carries version `0x87`, the decoder MUST reset the Fluff
history window and repeat-distance ring before reading the rest of the block.

```c
/* 2. Parse fixed header */
hdr = src + 4;
read_counts(hdr,           ll_norm, FLUFF_NUM_LITLEN);   /* 578 bytes */
read_counts(hdr + 578,     d_norm,  FLUFF_NUM_DIST);     /*  96 bytes */
ll_state = read_u32_le(hdr + 674);
d_state  = read_u32_le(hdr + 678);
fse_len  = read_u32_le(hdr + 682);
raw_len  = read_u32_le(hdr + 686);
fse_data = src + 694;
raw_data = fse_data + fse_len;

/* 3. Build decode tables */
fse_build_dtab(ll_norm, FLUFF_NUM_LITLEN, ll_dtab, FSE_LL_BITS);
fse_build_dtab(d_norm,  FLUFF_NUM_DIST,   d_dtab,  FSE_D_BITS);

/* 4. Decode token stream forwards */
for (;;) {
    sym = ll_dtab[ll_state].symbol;
    advance_ll_state(ll_dtab, &ll_state, fse_data, fse_len, &fse_pos);
    if (sym < 256) {
        output[out_pos++] = (unsigned char)sym;           /* literal */
    } else if (sym == 256) {
        break;                                            /* end-of-block */
    } else if (sym >= 286 && sym <= 288) {                /* repeat-distance */
        idx    = read_bits_raw(raw_data, raw_len, &raw_pos, 5);
        length = LEN_BASE[idx] + read_bits_raw(raw_data, raw_len, &raw_pos,
                                               LEN_EXTRA[idx]);
        dist   = recent_distance(sym, rep0, rep1, rep2);
        copy_lz77_overlap(output, out_pos, dist, length);
        out_pos += length;
    } else {                                              /* match 257..285 */
        idx    = sym - 257;
        length = LEN_BASE[idx] + read_bits_raw(raw_data, raw_len, &raw_pos,
                                               LEN_EXTRA[idx]);
        d_sym  = d_dtab[d_state].symbol;
        advance_d_state(d_dtab, &d_state, fse_data, fse_len, &fse_pos);
        dist   = DIST_BASE[d_sym] + read_bits_raw(raw_data, raw_len, &raw_pos,
                                                  DIST_EXTRA[d_sym]);
        copy_lz77_overlap(output, out_pos, dist, length);
        out_pos += length;
    }
    update_rep_ring(&rep0, &rep1, &rep2, dist);
}

if (out_pos != block_orig_size)
    return ERR_CORRUPT;
```

**Version `0x06` (range coder) — `fl_decompress_rc()`:**

```c
/* 2. Initialize adaptive probability model */
init_rc_model(&ll_model, FL_LL_NODES, FL_PROB_HALF);
init_rc_model(&d_model,  FL_D_NODES,  FL_PROB_HALF);
rc_init_decoder(&rc, src + 4, src_len - 4);

/* 3. Decode token stream */
for (;;) {
    sym = rc_decode_symbol(&rc, &ll_model, FL_LL_BITS);
    if (sym < 256) {
        output[out_pos++] = (unsigned char)sym;
    } else if (sym == 256) {
        break;
    } else if (sym >= 286 && sym <= 288) {
        idx    = rc_read_bits(&rc, 5);
        length = LEN_BASE[idx] + rc_read_bits(&rc, LEN_EXTRA[idx]);
        dist   = recent_distance(sym, rep0, rep1, rep2);
        copy_lz77_overlap(output, out_pos, dist, length);
        out_pos += length;
    } else {
        idx    = sym - 257;
        length = LEN_BASE[idx] + rc_read_bits(&rc, LEN_EXTRA[idx]);
        d_sym  = rc_decode_symbol(&rc, &d_model, FL_D_BITS);
        dist   = DIST_BASE[d_sym] + rc_read_bits(&rc, DIST_EXTRA[d_sym]);
        copy_lz77_overlap(output, out_pos, dist, length);
        out_pos += length;
    }
    update_rep_ring(&rep0, &rep1, &rep2, dist);
}

if (out_pos != block_orig_size)
    return ERR_CORRUPT;
```

**Overlap-safe LZ77 copy** (required when `distance < length`):

```c
/* Periodic-repeat expansion: copy 'distance' bytes at a time,
 * doubling step each round so earlier output fills the buffer. */
void copy_lz77_overlap(unsigned char *buf,
                       unsigned int pos,
                       unsigned int distance,
                       unsigned int length) {
    unsigned char *from = buf + pos - distance;
    unsigned char *to   = buf + pos;
    if (distance >= length) {
        memcpy(to, from, length);               /* no overlap: one copy */
    } else {
        unsigned int step = distance, rem = length;
        while (rem >= step) {
            memcpy(to, from, step);
            to += step; rem -= step; step += step;
        }
        if (rem > 0) memcpy(to, from, rem);
    }
}
```

#### 3.4.11.1 Decompression Validation

In addition to the token-decoding rules above, a conformant decoder MUST
validate:

- repeat-distance length-code indices are in the range 0–28
- distances satisfy `1 <= distance <= min(262144, bytes_decompressed_so_far)`
- output never grows beyond `block_orig_size`
- for v0x07/v0x87: litlen count sum equals `FSE_LL_L` (1024) and dist count sum equals
  `FSE_D_L` (512)
- the FSE or RC bitstream does not terminate before the EOB symbol is decoded

#### 3.4.12 Decompression Requirements

- MUST reject blocks where `magic[0] != 0x46` or `magic[1] != 0x4C` →
  `ERR_CORRUPT`.
- MUST reject blocks where `version` is not `0x06`, `0x07`, or `0x87` →
  `ERR_CORRUPT`.
- For v0x07/v0x87: MUST reject any count table whose entries do not sum to the
  required `L` value → `ERR_CORRUPT`.
- MUST support overlapping copies (`distance < length`) via the periodic-repeat
  doubling algorithm shown in §3.4.11.
- MUST reject distances exceeding the number of bytes decompressed so far in the
  file → `ERR_CORRUPT`.
- MUST reject distances exceeding 262144 (the window size) → `ERR_CORRUPT`.
- MUST reject repeat-distance length-code indices outside 0–28 → `ERR_CORRUPT`.
- MUST verify that total decompressed output equals `block_orig_size` →
  `ERR_CORRUPT`.

The reference implementation is `src/core/fluff.c` (`fluff_decompress_block_ctx`).

---

### 3.5 Method 4: zstd (Zstandard)

**ID:** `4`
**Codec:** Zstandard 1.5.x (Meta Platforms / Yann Collet)
**Library:** `lib/zstd/zstd.c` (single-file amalgamation, C99)
**Availability:** Modern 32/64-bit targets only (`BOLTPACK_ZSTD` build flag)
**Speed:** ~1700 MB/s decompression (modern 64-bit system)
**Compression ratio:** ~27% on curated corpus (best ratio codec in BoltPack)

#### 3.5.1 Overview

Method 4 stores each block as a self-contained zstd **frame** produced by
`ZSTD_compressCCtx()`. Each block is independent — no cross-block dictionary
state is maintained at the BoltPack level. The block size is fixed at 256 KB
for zstd archives.

Compression levels map from the BoltPack GUI/CLI 1–9 scale to zstd native
levels as follows:

| BP level | zstd native level | Notes             |
| -------- | ----------------- | ----------------- |
| 1        | 1                 | Fastest           |
| 2        | 2                 |                   |
| 3        | 3                 | Default           |
| 4        | 5                 |                   |
| 5        | 7                 |                   |
| 6        | 9                 |                   |
| 7        | 12                |                   |
| 8        | 15                |                   |
| 9        | 19                | Maximum (slowest) |

#### 3.5.2 Decompression Requirements

- Each block MUST be decompressed with `ZSTD_decompressDCtx()` (or equivalent).
- Decompressed size MUST equal `block_orig_size`; mismatch → `ERR_CORRUPT`.
- Implementations on vintage/16-bit targets that do not link `lib/zstd/`
  MUST return `ERR_UNSUPPORTED_METHOD` when encountering method 4.

#### 3.5.3 Compatibility

Method 4 archives are readable by any modern build of BoltPack (Linux, macOS,
Windows 32/64-bit, ARM64, RISC-V). Vintage targets (DOS 16-bit, Win16, Amiga,
Atari ST, Classic Mac 68 K) return `ERR_UNSUPPORTED_METHOD`.

The zstd frame format is documented at <https://github.com/facebook/zstd/blob/dev/doc/zstd_compression_format.md>.

---

## 4. Data Integrity

### 4.1 CRC-32 Checksum (checksum_type = 1) — current

**Checksum type:** `1` (CRC-32)
**Polynomial:** IEEE 802.3 reflected `0xEDB88320` (zlib-compatible)
**Detection:** All 1- and 2-bit errors, all odd numbers of bit errors, all
bursts ≤ 32 bits; longer bursts with probability 1 − 2⁻³²

CRC-32 is the **only** payload checksum current writers emit. Writers MUST
write `checksum_type = 1`; any other value → `ERR_CORRUPT` on read. Seed
the running value with `0`, fold each uncompressed block in stream order, and
compare the final value against the entry's `checksum` field. The archive
**header**, **file table**, and each **block** are protected by CRC-32
(§2.2, §2.3.2).

**Validation:**

- If computed checksum ≠ stored checksum → `ERR_CORRUPT`
- Decompression MUST NOT halt early; always validate checksum.

### 4.1a Adler-32 Checksum (checksum_type = 0) — retired

Earlier BoltPack versions used Adler-32 (RFC 1950) as the payload checksum.
It has been **retired**: current writers never emit `checksum_type = 0`, and
the reference implementation compiles the Adler-32 code only under the
`BP_ENABLE_ADLER32` build flag (used by its unit tests). The `checksum_type`
field is retained so a future algorithm can be assigned a new value without a
format break. For reference, Adler-32 is defined as:

```text
A = (sum of uncompressed bytes + 1) mod 65521
B = (sum of (65521 − position + byte_i)) mod 65521
Adler32 = (B << 16) | A
```

A reader MAY still support `checksum_type = 0` for compatibility, but is not
required to, and no conforming writer of this generation produces it.

### 4.2 Decompression Integrity Checks

Decompressor MUST perform the following checks during and after decompression:

1. **Format validation:** Method ID is one of {0, 1, 2, 3, 4}.
1. **Size tracking:** Decompressed output equals `block_orig_size` for each block.
1. **Distance validity:** Match distances do not exceed available history.
1. **End-of-block marker:** Fluff blocks terminate with symbol 256.
1. **Checksum validation:** CRC-32 of uncompressed payload matches entry header.

Failure of any check → `ERR_CORRUPT`; abort extraction.

---

## 5. File Table and Footer

### 5.1 File Table (Directory)

**Purpose:** Rapid directory listing without scanning entire archive.

**Offset:** Stored in footer; typically at end of archive before footer.

**Table header (16 bytes):**

| Offset | Size | Field         | Type   | Description                                  |
| ------ | ---- | ------------- | ------ | -------------------------------------------- |
| 0      | 4    | `table_magic` | u32 LE | `0x544C4254` (bytes: `54 42 4C 54` = "TBLT") |
| 4      | 4    | `entry_count` | u32 LE | Number of directory entries                  |
| 8      | 8    | `total_orig`  | u64 LE | Sum of `original_size` across all entries    |

The `total_orig` field allows readers to display total uncompressed size without
scanning each entry record. Writers MUST store the correct sum; readers SHOULD
use it in preference to iterating records.

Followed by `entry_count` records:

| Offset | Size | Field             | Type   | Description                             |
| ------ | ---- | ----------------- | ------ | --------------------------------------- |
| 0      | 2    | `name_len`        | u16 LE | Length of file name                     |
| 2      | N    | `name`            | bytes  | File name (UTF-8, forward-slash sep)    |
| 2+N    | 8    | `original_size`   | u64 LE | Uncompressed size                       |
| 10+N   | 8    | `compressed_size` | u64 LE | Compressed size                         |
| 18+N   | 1    | `entry_type`      | u8     | Entry type (0–4)                        |
| 19+N   | 1    | `platform`        | u8     | Metadata platform origin                |
| 20+N   | 8    | `mtime`           | u64 LE | Modification time (Unix epoch)          |
| 28+N   | 8    | `offset`          | u64 LE | Byte offset of entry from archive start |
| 36+N   | 1    | `method`          | u8     | Compression method (0–4)                |
| 37+N   | 2    | `header_size`     | u16 LE | Entry header size                       |
| 39+N   | 4    | `checksum`        | u32 LE | Entry payload checksum                  |

The record tail is 41 bytes. Its final three fields duplicate values from
the entry header so listing, info, delete, and copy operations need not seek
to each entry to recover them.

**Validation:**

- `table_magic != 0x544C4254` → `ERR_CORRUPT`
- `entry_count != archive_header.file_count` → `ERR_CORRUPT`
- The whole file-table region is covered by the header's `table_crc` (§2.2).

### 5.2 Archive Footer (20 bytes)

**Location:** Last 20 bytes of file.

**Format:**

| Offset | Size | Field          | Type   | Description                                     |
| ------ | ---- | -------------- | ------ | ----------------------------------------------- |
| 0      | 8    | `table_offset` | u64 LE | Byte offset of file table from start of archive |
| 8      | 8    | `table_size`   | u64 LE | Size of file table section in bytes             |
| 16     | 4    | `footer_magic` | u32 LE | `0x424F4C54` (bytes: `54 4C 4F 42` = "TLOB")    |

**Validation:**

- `footer_magic != 0x424F4C54` → `ERR_CORRUPT`
- `table_offset + table_size + 20 != file_size` → `ERR_CORRUPT`

---

## 6. Creator Identification

### 6.1 Creator OS Codes

| Code          | OS / Platform                        |
| ------------- | ------------------------------------ |
| `0x00`        | Unknown / generic                    |
| `0x01`        | Linux                                |
| `0x02`        | Mac OS (classic 1–9)                 |
| `0x03`        | Mac OS X / macOS                     |
| `0x04`        | AmigaOS (1.x–3.x)                    |
| `0x05`        | AmigaOS 4.x                          |
| `0x06`        | MorphOS                              |
| `0x07`        | AROS                                 |
| `0x08`        | MS-DOS                               |
| `0x09`        | Windows (9x/NT/2000+/(NT/ARM64)      |
| `0x0A`        | FreeBSD                              |
| `0x0B`        | NetBSD                               |
| `0x0C`        | OpenBSD                              |
| `0x0D`        | Solaris / SunOS                      |
| `0x0E`        | IRIX                                 |
| `0x0F`        | HP-UX                                |
| `0x10`        | AIX                                  |
| `0x11`        | Atari TOS / MiNT                     |
| `0x12`        | RISC OS (Acorn)                      |
| `0x13`        | OS/2                                 |
| `0x14`        | BeOS / Haiku                         |
| `0x15`        | OpenVMS                              |
| `0x16`        | z/OS (IBM mainframe)                 |
| `0x17`        | WebAssembly (Node.js / browser)      |
| `0x18`        | NeXTSTEP / OPENSTEP                  |
| `0x19`        | QNX Neutrino                         |
| `0x1A`        | Plan 9                               |
| `0x1B`        | Tru64 UNIX (OSF/1)                   |
| `0x1C`        | SCO OpenServer / UnixWare            |
| `0x1D`        | CP/M-86                              |
| `0x1E`        | Sinclair QL (QDOS)                   |
| `0x1F`        | Windows 3.1 (Win16)                  |
| `0x20`        | GNU Hurd                             |
| `0x21`        | Sharp X68000                         |
| `0x80`–`0xFF` | Reserved for private/user extensions |

Decoders MAY display `creator_os` for diagnostics. Extraction behavior is platform-independent.

### 6.2 Creator CPU Codes

| Code          | Architecture             |
| ------------- | ------------------------ |
| `0x00`        | Unknown / generic        |
| `0x01`        | x86 (i386+)              |
| `0x02`        | x86-64 (AMD64)           |
| `0x03`        | PowerPC (32-bit)         |
| `0x04`        | PowerPC (64-bit)         |
| `0x05`        | Motorola 68k             |
| `0x06`        | ARM (32-bit)             |
| `0x07`        | ARM64 / AArch64          |
| `0x08`        | MIPS (32-bit)            |
| `0x09`        | MIPS (64-bit)            |
| `0x0A`        | SPARC                    |
| `0x0B`        | Alpha                    |
| `0x0C`        | PA-RISC                  |
| `0x0D`        | RISC-V (32-bit)          |
| `0x0E`        | RISC-V (64-bit)          |
| `0x0F`        | SuperH (SH)              |
| `0x10`        | 6502 / 65C816            |
| `0x11`        | Z80                      |
| `0x12`        | 8086 / 8088 (16-bit)     |
| `0x13`        | e2k (Elbrus)             |
| `0x14`        | z/Architecture           |
| `0x15`        | SPARC64                  |
| `0x16`        | Itanium (IA-64)          |
| `0x80`–`0xFF` | Reserved for private use |

---

## 7. Byte Order and Portability

### 7.1 Canonical Byte Order

**All multi-byte integers are little-endian** (least-significant byte first).

**Examples:**

- `0x12345678` stored as: `78 56 34 12` (in memory order)
- `0xDEADBEEF` stored as: `EF BE AD DE`

### 7.2 Byte-Swapping for Big-Endian Hosts

On big-endian platforms (PowerPC, 68k, SPARC, Alpha), decoders MUST byte-swap when reading:

**C implementation (ANSI C89):**

```c
typedef unsigned char      bp_uint8;
typedef unsigned short     bp_uint16;
typedef unsigned int       bp_uint32;

/* Read 16-bit little-endian */
bp_uint16 read_le16(const bp_uint8 *p) {
    return (p[0]) | (p[1] << 8);
}

/* Read 32-bit little-endian */
bp_uint32 read_le32(const bp_uint8 *p) {
    return (p[0]) | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

/* Read 64-bit little-endian (returns structure) */
typedef struct {
    bp_uint32 lo;
    bp_uint32 hi;
} bp_uint64;

bp_uint64 read_le64(const bp_uint8 *p) {
    bp_uint64 result;
    result.lo = read_le32(p);
    result.hi = read_le32(p + 4);
    return result;
}
```

The swap cost is negligible; byte-order detection is automatic via compile-time checks or CPU detection.

### 7.3 Character Encoding

**File names and paths:** UTF-8 (Unicode)
**Encoding flow:**

1. **Writer:** Convert native encoding (MacRoman, Shift-JIS, EBCDIC, etc.) to UTF-8.
2. **Archive:** Store UTF-8 paths verbatim.
3. **Reader:** Convert UTF-8 back to native encoding.

On Linux/Unix/Windows, UTF-8 is native; no conversion needed in typical cases.

---

## 8. Error Handling and Status Codes

### 8.1 Standard Error Codes

The reference implementation defines error codes as negative integers in
`src/core/bp_types.h`. Zero is success; any non-zero value is an error.

| Code | Name                        | Meaning                                 |
| ---- | --------------------------- | --------------------------------------- |
| 0    | `BP_OK`                     | Operation completed successfully        |
| -1   | `BP_ERR_IO`                 | I/O error (read/write/seek failed)      |
| -2   | `BP_ERR_NOMEM`              | `malloc()` returned NULL                |
| -3   | `BP_ERR_MAGIC`              | Bad magic bytes (not a `.bolt` file)    |
| -4   | `BP_ERR_VERSION`            | Archive format version > reader version |
| -5   | `BP_ERR_CHECKSUM`           | CRC-32 mismatch on decompressed data    |
| -6   | `BP_ERR_CORRUPT`            | Structural corruption                   |
| -7   | `BP_ERR_TRUNCATED`          | Unexpected end of file                  |
| -8   | `BP_ERR_OVERFLOW`           | File or archive exceeds platform limits |
| -9   | `BP_ERR_NAME_TOO_LONG`      | File name exceeds 65 535 bytes          |
| -10  | `BP_ERR_UNSUPPORTED_METHOD` | Unknown compression method              |
| -11  | `BP_ERR_COMPRESS`           | Compression backend call failed         |
| -12  | `BP_ERR_DECOMPRESS`         | Decompression backend call failed       |
| -13  | `BP_ERR_ABORTED`            | User cancelled via progress callback    |
| -14  | `BP_ERR_NOT_FOUND`          | Named entry not found in archive        |
| -15  | `BP_ERR_INVALID_ARG`        | NULL pointer or invalid parameter       |

---

## 9. Conformance and Testing

### 9.1 Decoder Conformance

Every BoltPack reader **MUST:**

1. Validate archive header magic and version.
2. Support **method 0** (stored).
3. Return `ERR_METHOD` for any compressed method it does not implement.
4. Compute and validate CRC-32 checksums.
5. Reject archives with format version > reader version.
6. Handle zero-length entries, empty directories, and symlinks.
7. Support both **blocked** and **single-blob** compressed data.
8. Restore modification times (mtime) during extraction.
9. Report clear error messages for all error conditions.

Full v1.0 implementations SHOULD support all compressed methods:

1. **Method 1** — LZSS
2. **Method 2** — BoltLZ
3. **Method 3** — Fluff
4. **Method 4** — zstd (modern 32/64-bit targets only; return `ERR_METHOD` on vintage/16-bit builds)
5. **Method 4** — zstd (modern builds)

### 9.2 Encoder Conformance

Every BoltPack writer **SHOULD:**

1. Set archive header magic to `0x544C4F42` ("BOLT").
2. Set format version to `0x0001`.
3. Set creator fields accurately (OS, CPU, BoltPack version).
4. Choose compression method based on file characteristics:
   - Method 3 (fluff) for text, source code, and other ratio-sensitive data.
   - Method 2 (BoltLZ) for binary data and fast general-purpose compression.
   - Method 1 (LZSS) for constrained-memory or 16-bit targets.
   - Method 0 (stored) if compression saves < 1% (see Section 9.3).
5. Compute correct CRC-32 checksum.
6. Choose appropriate block size based on available memory (see Section 2.3.2).
7. Build file table and footer with correct offsets.
8. Preserve file metadata (mtime, entry type, platform codes).

### 9.3 Compression Method Selection Heuristic

The **99% threshold rule** is recommended for encoder implementations:

```c
compressed_size = compress_block(original_data, block_size)

if (compressed_size > (original_size * 99 / 100)) {
    /* Savings less than 1%; store uncompressed instead */
    use_method_0();
} else {
    /* Worthwhile compression savings */
    use_selected_compressed_method();
}
```

This heuristic avoids decompression overhead for poorly-compressible data.

---

## 10. Format Extensibility and Future Compatibility

### 10.1 Forward Compatibility

To support future enhancements without breaking existing readers:

1. **Entry header**: The `header_size` field enables new fields after existing data. Readers MAY skip unknown fields by advancing to `header_size`.
2. **Archive header**: The `flags` field is reserved for future options (currently must be zero).
3. **Compression methods**: New methods (3, 4, 5, ...) MAY be assigned; old readers will error with `ERR_METHOD`.
4. **Checksum types**: `checksum_type` allows future algorithms (1 = CRC-32, 2 = SHA-256, etc.).
5. **Entry flags**: The `flags` field in entry headers is reserved; new bits may be assigned without breaking old readers.

### 10.2 Archive version policy

Writers MUST emit archive version 4. Readers MUST reject every other version
with `BP_ERR_VERSION` before interpreting compressed blocks or file-table
records. New archives use the 16-byte table header, 41-byte record tails,
and 12-byte CRC-protected block headers described above. No conversion or
compatibility decoding is performed.

## 11. Reference Implementations

### 11.1 Decompression Library

A portable C89 decompression library is provided in the BoltPack repository:

- **File:** `src/core/archiver.c`
- **API:** `bp_archive_unpack()`, `bp_archive_unpack_safe()`
- **Required headers:** `src/core/archiver.h`, `src/core/bp_types.h`

**Usage:**

```c
#include "src/core/archiver.h"

int result = bp_archive_unpack(
    input_file_handle,
    output_directory,
    progress_callback  /* optional */
);

if (result != 0) {
    /* handle error */
    printf("Extraction failed: %s\n", bp_strerror(result));
}
```

The library is self-contained (includes LZSS, BoltLZ, and fluff codecs) and compiles with strict ANSI C89.

### 11.2 Repository Test Coverage

The v1.0 repository ships test programs and regression suites in `test/` rather than
a separate `test/vectors/` directory. Relevant coverage includes:

1. `test/check_boltpack.py` — end-to-end archive creation and extraction checks.
2. `test/test_fluff.py` — Fluff block round-trip, stored fallback, and corruption tests.
3. `test/test_roundtrip.py` — multi-method archive round-trips.
4. `test/test_format.py` — format and header validation.
5. `test/test_filenames.py` — UTF-8 and path handling.

---

## A. Glossary

- **Archive:** A `.bolt` file containing entries.
- **Block:** A variable-length chunk of compressed data (8 KB to 256 KB).
- **Creator code:** OS or CPU identifier in archive header (informational).
- **Entry:** A file, directory, symlink, or metadata record.
- **Fluff:** BoltPack's native LZ77+Huffman codec (method 3).
- **Little-endian:** Least-significant byte first; canonical byte order for this format.
- **Metadata:** File creation time, platform origin, entry type.
- **Method:** Compression algorithm (0=stored, 1=LZSS, 2=BoltLZ, 3=fluff).
- **Payload:** Uncompressed content of an entry.
- **UTF-8:** Unicode text encoding for file names and paths.

---

## B. References

- **RFC 1950:** ZLIB Compressed Data Format Specification (Huffman coding, Adler-32)
- **ANSI C89/C90:** ISO C language standard (for portable type definitions)
- **ZIP format:** PKWARE AppNote (for design inspiration and compatibility lessons)

---

**Document version:** 1.1
**Last updated:** June 2026
**Status:** Final (Stable release)
