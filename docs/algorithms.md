# BoltPack Compression Algorithms v1.1

**Status:** Stable
**Scope:** Current codec implementations shipped in BoltPack v1.1

This document describes the algorithms that are actually implemented in the
current codebase. It intentionally covers only the kept v1.1 codecs and their
current behavior. Retired prototypes, intermediate Fluff stream layouts, and
implementation-plan history are excluded.

For the on-disk archive format, see [specification-1.0.md](specification-1.0.md).

## 1. Methods at a Glance

| Method | Name   | Implementation                 | Primary role                                    |
| ------ | ------ | ------------------------------ | ----------------------------------------------- |
| 0      | Stored | `src/core/archiver.c`          | Fallback for incompressible data                |
| 1      | LZSS   | `src/core/lzss.c`              | Small-memory LZ77, especially for mini builds   |
| 2      | boltlz | `src/core/compress.c` wrapper  | Fast general-purpose compression                |
| 3      | Fluff  | `src/core/fluff.c`             | Highest-ratio codec in the full build           |
| 4      | zstd   | `src/core/zstd_wrap.c` wrapper | Best-ratio codec; modern 32/64-bit targets only |

The full archiver can read and write all five methods on modern targets.
Constrained frontends (DOS 16-bit, Win16, Amiga, Atari, Classic Mac 68 K) may
reject methods they do not ship and return `ERR_METHOD`.

## 2. Method 0: Stored

Stored mode copies bytes verbatim into the archive and copies them back out at
extract time.

Compression behavior:

1. Write the input bytes unchanged.
1. Set `compressed_size == original_size`.
1. Skip codec-specific working memory.

Decompression behavior:

1. Verify that the stored byte count matches the expected output size.
1. Copy the payload directly.

BoltPack's archive layer uses a 99% threshold when evaluating compressed
blocks. If a codec saves less than 1%, the block is stored uncompressed
instead.

## 3. Method 1: LZSS

### 3.1 Overview

BoltPack method 1 is an Okumura-style LZSS codec implemented in
`src/core/lzss.c` and declared in `src/core/lzss.h`.

Its defining traits are:

1. A small 2048-byte ring buffer.
1. Two-byte match tokens with an 11-bit position and 5-bit length field.
1. One flag byte per 8 symbols, with bit 0 describing the first symbol.
1. A fixed 14,900-byte scratch buffer that works even on 16-bit targets.

### 3.2 Format-Fixed Parameters

| Parameter            | Value  | Meaning                                     |
| -------------------- | ------ | ------------------------------------------- |
| Ring buffer size     | 2048   | 11-bit history window                       |
| Maximum match length | 34     | Encoded as `(len - 3)` in 5 bits            |
| Minimum match length | 3      | Shorter runs become literals                |
| Fill byte            | `0x20` | Initial ring-buffer contents                |
| Flag packing         | 8 bits | Bit 0 is the first symbol in each flag byte |
| Work buffer          | 14900  | `LZSS_WORK_SIZE`                            |

The token layout is:

```text
flag bit = 1  -> literal byte follows
flag bit = 0  -> two-byte match token follows

match byte 0 = position bits 10..3
match byte 1 = position bits 2..0 in the upper 3 bits,
               match length minus 3 in the lower 5 bits
```

### 3.3 Current Compressor Behavior

The current compressor matches the code in `src/core/lzss.c`:

1. The caller supplies `LZSS_WORK_SIZE` bytes of scratch.
1. The work buffer is carved into the ring buffer plus three binary-search-tree
   arrays: `lson`, `rson`, and `dad`.
1. The ring buffer is prefilled with `0x20`, and the initial write position is
   `N - F`.
1. Match finding uses Okumura's binary-search-tree insertion routine.
1. The encoder performs a one-step lazy check: when the current match is not a
   perfect `F`-byte match, it peeks at `r + 1` and emits a literal now if the
   next position yields a longer match.
1. Tree search stops early once a match of `GOOD_ENOUGH = F - 2` is found,
   which avoids extra traversal chasing the rare longest matches.

This is the current kept implementation and the behavior the documentation
covers for v1.0.

### 3.4 Current Decompressor Behavior

The decompressor is deliberately small and simple:

1. It uses the ring-buffer portion of the caller-provided work area.
1. It refills the ring with `0x20` before decoding.
1. It reads one flag byte at a time, consuming 8 symbols per flag byte.
1. Literal bits copy one byte from input to output and into the ring buffer.
1. Match bits decode `(pos, len)` and replay bytes from the ring buffer,
   naturally supporting overlap.
1. The outer loop stops when exactly `dst_len` bytes have been produced.

### 3.5 Bounds and Memory

The current documented bound matches the implementation:

```text
worst_case = src_len + src_len / 8 + 16
```

The public API asks for `LZSS_WORK_SIZE` bytes for both compression and
Decompression. In practice the decoder uses only the ring-buffer portion, but
the fixed-size API keeps call sites simple across all targets.

## 4. Method 2: BoltLZ

Method 2 uses the original C89 implementation in `src/core/boltlz.c`, with
archive wrappers in `src/core/compress.c`. The default is Balanced; `-l 1`
selects Fast, and `-l 2` selects Balanced. Levels 3–9 also select Balanced.
Both modes use the same format-2 block decoder and a maximum 256 KiB block.

Fast searches one hash candidate with adaptive skipping. Balanced searches
two candidates plus the previous distance and uses bounded lazy matching.
Both extend matches backwards through pending literals. The byte-oriented
stream has combined literal/match tokens, canonical ULEB128 extensions,
and a repeated-distance bit. The [normative specification](specification-1.0.md)
defines the complete representation.

The core codec allocates no memory. The archive writer caches a 64 KiB
workspace that supports both modes, avoiding per-block allocation. Fast
needs 32 KiB workspace when used independently. Blocks up to 64 KiB use
packed 16-bit entries internally; larger blocks use 32-bit entries. The
public workspace sizes remain the worst case. Decompression needs no
workspace beyond the declared input and output buffers, and accepts
unaligned buffers without requiring padding.

Each block is compressed independently. The archive stores blocks verbatim
when compression saves less than 1%. Input capacity and exact output length
are checked, and archives carry file and block CRC-32 checksums. All
GCC/Clang codec builds use `-O2`.

16-bit mini builds include the same decoder through the decoder-only build
path and accept blocks within their existing 32 KiB memory limit. They
continue to create Stored or LZSS archives. Other mini builds support
BoltLZ Balanced compression.

## 5. Method 3: Fluff

### 5.1 Overview

Fluff is BoltPack's ratio-oriented codec. The current implementation is in
`src/core/fluff.c` and `src/core/fluff.h`.

The kept v1.0 implementation combines:

1. A 256 KB LZ77 window.
1. A hybrid hash-chain match finder.
1. A three-entry repeat-distance ring.
1. A rep-aware greedy parser with lazy lookahead.
1. FSE/tANS entropy coding (current encoder family; normal blocks use stream
   version `0x08`, grouped reset blocks use `0x88`).

The encoder writes the compact `0x08` block format. The decoder also accepts
the earlier `0x07`/`0x87` FSE/tANS blocks and the original adaptive range coder
format (stream version `0x06`) for backward compatibility with archives written
by previous releases.

**Stream version `0x08` (compact FSE)** differs from `0x07` only in the block
header — the LZ77 token streams are unchanged:

- **Count tables** are stored with a varint + zero-run code instead of a raw
  `289×u16` + `48×u16` dump (saves several hundred bytes per block; the win is
  proportionally largest on small blocks and many-small-file archives).
- **Header scalar fields** shrink: the two FSE initial states become `u16`
  (state `< 1024`) and the two stream lengths become `u24` (block `≤ 256 KB`),
  saving 6 bytes per block.

Rep-match lengths keep the `0x07` flat 5-bit raw index. (Entropy-coding them
through the litlen FSE table was evaluated and measured net-negative on every
corpus file, so it was not adopted.)

### 5.2 Format-Fixed Parameters

| Parameter               | Value           | Notes                                                                                                                                                                      |
| ----------------------- | --------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Stream version          | `0x08` / `0x88` | `0x08` = current compact FSE/tANS block; `0x88` = compact reset block used by grouped fast-fluff; decoder also accepts legacy `0x07`/`0x87` (FSE) and `0x06` (range coder) |
| Window bits             | 18              | 256 KB window                                                                                                                                                              |
| Window size             | 262144          | `1 << 18`                                                                                                                                                                  |
| Minimum match           | 3               | Shared by normal and repeat matches                                                                                                                                        |
| Maximum match           | 258             | DEFLATE-style upper bound                                                                                                                                                  |
| Main hash table         | 131072          | 17-bit 4-byte hash                                                                                                                                                         |
| Sub-hash table          | 65536           | 3-byte recent-position table                                                                                                                                               |
| Default chain depth     | 256             | Tuned by compression level                                                                                                                                                 |
| Good-match early stop   | 64              | Stop deeper chain search at large matches                                                                                                                                  |
| Lit/len alphabet        | 289             | Literals, EOB, match lengths, rep-match symbols                                                                                                                            |
| Distance alphabet       | 48              | Current v1.0 distance buckets                                                                                                                                              |
| Repeat-distance symbols | 286-288         | Reuse `rep0`, `rep1`, `rep2`                                                                                                                                               |

### 5.2a Modern Fast-Fluff Path

Modern OpenMP-enabled builds may also use `src/core/fluff_fast.c` as an
alternate encoder for method `3`. It keeps Fluff's on-disk format compatible
with the strict-C89 decoder, but compresses blocks in parallel and groups
consecutive blocks into shared-history batches (default group size `4`) so the
ratio loss from resetting history stays bounded.

The first block in each batch uses stream version `0x87`, which tells the
decoder to reset the Fluff context before decoding that block. Later blocks in
the same batch use the ordinary `0x07` header and can still refer back to the
earlier blocks in that batch.

### 5.3 Current Encoder Pipeline

The current Fluff encoder is best understood as the following concrete stages.

1. **Persistent context and cross-block history.**
   `fluff_ctx_t` keeps the ring buffer, hash tables, and file position across
   blocks in the same file so later blocks can refer back to earlier ones.
1. **Hybrid match finding.**
   `fl_find_match_hyb()` searches a 4-byte hash chain and also probes a dense
   3-byte sub-hash table. The sub-hash does not replace the main chain; it
   seeds the rejection threshold so the chain search starts from a stronger
   minimum match length.
1. **Distance-aware short-match filtering.**
   The encoder rejects matches that expand the stream: `len <= 3` at
   `dist > 512`, and `len <= 4` at `dist > 49152`.
1. **Repeat-distance scoring.**
   `fl_apply_rep_ring()` tests `rep0`, `rep1`, and `rep2` against the current
   normal match. `fl_match_savings_est()` compares the estimated bit cost of a
   normal match against the cheaper rep-token form.
1. **Rep-aware greedy parsing with lazy lookahead.**
   `fl_parse_greedy()` evaluates the current position, then checks `i + 1`. If
   the lazy candidate wins by more than 8 bits, it may also evaluate `i + 2`
   before committing. This is the current kept parser. The discarded parser
   experiments and profiling-only heuristics are not part of the v1.0
   algorithm description.
1. **FSE/tANS emission.**
   After tokenization, the encoder normalizes lit/len and distance symbol
   frequencies to power-of-two table sizes (`L = 1024` for lit/len,
   `L = 512` for distance). It then encodes tokens backwards through the ANS
   state machine, writes count tables and initial states into the fixed block
   header, and appends the backwards FSE bitstream and backwards raw extra-bits
   bitstream.

### 5.4 Current Decoder Pipeline

`fluff_decompress_block_ctx()` dispatches on the stream version byte:

**Version `0x87` (FSE/tANS reset block) — grouped fast-fluff format:**

1. Validate the four-byte `F`, `L`, `0x87`, `18` stream header.
1. Reset the Fluff history window and repeat-distance state for the current
   context.
1. Decode the remainder of the block exactly as version `0x07`.

**Version `0x07` (FSE/tANS) — current format:**

1. Validate the four-byte `F`, `L`, `0x07`, `18` stream header.
1. Read count tables from the fixed-size header (578 bytes lit/len + 96 bytes
   dist).
1. Build FSE decode tables from the count tables.
1. Read initial ANS states and bitstream lengths from the header.
1. Decode tokens forwards by advancing through the ANS decode table and reading
   backwards from the pre-written bitstreams.
1. For each token: emit literal, or compute match length + distance (or
   reuse a rep distance), then copy via overlap-safe LZ77.
1. Bulk-copy the finished block into the ring buffer so the next block can use
   cross-block references.

**Version `0x06` (range coder) — legacy read-only:**

1. Validate the four-byte `F`, `L`, `0x06`, `18` stream header.
1. Initialize adaptive probability tables for the 9-bit lit/len binary tree
   (512 nodes) and the 6-bit distance binary tree (64 nodes) at 50/50 (`p = 2048`).
1. Decode tokens from a single continuous bitstream, updating probabilities
   after each decoded bit.
1. Perform overlap-safe LZ77 copy for all back-references.
1. Bulk-copy the finished block into the ring buffer.

### 5.5 Stored-Fallback Synchronization

One implementation detail matters for correctness: when the archive layer tests
Fluff for a block and then stores that block uncompressed because it misses the
99% threshold, the decompressor still has to advance the Fluff history window.

That is why the current code includes `fluff_ctx_feed_raw()`: it keeps the
Decoder's ring buffer synchronized with stored fallback blocks inside an
otherwise Fluff-compressed file.

### 5.6 Repeat-Distance Ring State

Fluff updates its three-entry repeat-distance ring after every emitted or
decoded match, including repeat-distance matches.

The update rule is move-to-front rather than simple append:

1. Reusing `rep0` leaves the ring unchanged.
1. Reusing `rep1` promotes it to `rep0` and demotes the old `rep0` to `rep1`.
1. Reusing `rep2` promotes it to `rep0`, shifts the old `rep0` to `rep1`, and
   shifts the old `rep1` to `rep2`.
1. A new distance not already in the ring becomes `rep0`, and the previous
   `rep0` and `rep1` slide down to `rep1` and `rep2`.

This is the behavior implemented by `fl_update_rep_ring()` and is shared by the
encoder and decoder. The repeat-distance tokens (286-288) therefore do more
than shorten the token stream: they also refresh the reused distance for future
matches.

### 5.7 Indicative Current Performance

At 256 KB blocks, the kept current implementation is roughly in this range on a
modern x86-64 Linux system:

| Corpus  | Ratio  | Compress  | Decompress |
| ------- | ------ | --------- | ---------- |
| Silesia | 29.45% | 15.2 MB/s | 466.9 MB/s |
| Curated | 28.35% | 15.7 MB/s | 438.3 MB/s |

These numbers describe the current reference implementation. They are useful
for orientation, not as a wire-format guarantee. Fast-fluff modern builds trade
a small amount of ratio for much higher compression throughput, but they still
write method `3` archives rather than introducing a separate archive method.

## 6. Method 4: zstd (Zstandard)

zstd is BoltPack’s highest-ratio codec and the default recommendation for
modern 64-bit targets when maximum compression is needed.

### 6.1 Overview

zstd is vendored as a single-file amalgamation in `lib/zstd/zstd.c` (zstd 1.5.7,
BSD 2-Clause). The thin C89-compatible wrapper is `src/core/zstd_wrap.c` /
`src/core/zstd_wrap.h`. The amalgamation is compiled with `-std=c99`; the wrapper
calls it through opaque `ZSTD_CCtx *` / `ZSTD_DCtx *` contexts, keeping the
C99 requirement entirely inside `lib/` and `src/core/zstd_wrap.c`.

Each block is a self-contained zstd frame. No cross-block dictionary state is
maintained; all blocks use the same compression level set by `bp_zstd_set_level()`.

### 6.2 Compression Level Mapping

| BP level (1–9) | zstd native level | GUI label        |
| -------------- | ----------------- | ---------------- |
| 1              | 1                 | Fast             |
| 2              | 2                 |                  |
| 3              | 3                 | Normal (default) |
| 4              | 5                 |                  |
| 5              | 7                 |                  |
| 6              | 9                 |                  |
| 7              | 12                |                  |
| 8              | 15                |                  |
| 9              | 19                | Best / Ultra     |

The default block size for zstd archives is 256 KB (same as Fluff).

### 6.3 Availability

Method 4 requires the `BOLTPACK_ZSTD` compile-time flag, which is enabled by
default for the Linux, macOS, Windows 32/64-bit, ARM64, and RISC-V targets.
Vintage targets (DOS 16-bit, Win16, Amiga 68 K, Atari ST, Classic Mac 68 K)
do not link `lib/zstd/` and return `BP_ERR_UNSUPPORTED_METHOD` for method 4.

### 6.4 Memory Profile

| Side          | Working memory                                         |
| ------------- | ------------------------------------------------------ |
| Compression   | `ZSTD_CCtx` heap context (∼ few hundred KB at level 3) |
| Decompression | `ZSTD_DCtx` heap context (∼ 128 KB)                    |

Both contexts are created lazily on first use and released by `bp_zstd_cleanup()`.

## 7. Memory Profile

| Method | Compression-side working memory                              | Decompression-side working memory                         |
| ------ | ------------------------------------------------------------ | --------------------------------------------------------- |
| 0      | No codec scratch beyond the caller’s buffers                 | No codec scratch beyond the caller’s buffers              |
| 1      | Fixed 14,900-byte work buffer                                | Same API-sized work buffer; decoder uses the ring portion |
| 2      | 32 KiB Fast / 64 KiB Balanced heap workspace                 | No codec workspace                                        |
| 3      | `fluff_ctx_t` is about 2 MB plus per-block token buffers     | Current decoder also reuses `fluff_ctx_t` for block state |
| 4      | `ZSTD_CCtx` heap context (∼ few hundred KB, level-dependent) | `ZSTD_DCtx` heap context (∼ 128 KB)                       |

Fluff’s larger context is the trade-off for its larger window, deeper search,
and cross-block history.

## 8. Validation

The current implementations are covered by the repository test suite in `test/`.
Relevant checks include:

1. `test/check_boltpack.py` for end-to-end archive behavior.
1. `test/test_roundtrip.py` for cross-method round-trips.
1. `test/test_fluff.py` for Fluff-specific regression coverage.
1. `test/test_format.py` for format validation.
1. `test/test_filenames.py` for path and encoding behavior.

The v1.0 codec documentation is meant to track the code that ships. Historical
planning material and discarded codec variants are intentionally left out.

## 9. References

1. Haruhiko Okumura, original LZSS design and binary-search-tree matcher.
1. J. Duda, “Asymmetric numeral systems,” arXiv:0902.0271, 2009 — theoretical
   basis for the FSE/tANS entropy coder in Fluff.
1. zstd 1.5.7, vendored in `lib/zstd/`; by Meta Platforms / Yann Collet;
   BSD 2-Clause license.
1. [specification-1.0.md](specification-1.0.md) for the exact BoltPack wire
   format.

**Last reviewed:** June 2026
