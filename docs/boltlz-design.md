# BoltLZ design

BoltLZ is an original general-purpose, byte-oriented LZ77 codec implemented
in portable C89. Its two encoder modes share one bounds-checked decoder.
The default Balanced mode favours compressed size; Fast reduces search work.
Archive method 2 was introduced in format 5 and remains available in current
format 6.

## Representation

A compressed block begins with format byte `0x02`. Tokens combine literal
and match lengths, with canonical ULEB128 extensions and a flag to reuse the
previous match distance. There is no entropy-coding stage. The decoder can
reconstruct a block without encoder state or heap scratch memory.
The [archive specification](specification-1.0.md) defines the bitstream.

## Match finding and parsing

Both modes hash four-byte prefixes into 8,192 buckets and extend matches
backward through pending literals. Fast checks one candidate and skips
positions adaptively after misses. Balanced checks two candidates, the
previous distance, and bounded lazy matching at the next position.
The cost model accounts for token and variable-distance overhead.

Positions are stored as position-plus-one with zero meaning no candidate.
Blocks up to 64 KiB use packed 16-bit entries; larger blocks use 32-bit
entries. Workspace requirements are 32 KiB for Fast and 64 KiB for Balanced.
The archive caches enough workspace for both modes and reuses it across
blocks and files. Each block resets its match history.

## Speed decisions

Selected optimizations include native word match counting through `memcpy`,
bounded literal copies, overlap-specific match copies, a common short-token
decoder loop, encoder specialization, packed position tables, and a single
encoder capacity decision. No caller padding or unaligned pointer casts are
required. Search pipelining and broader decoder-loop experiments measured
slower and are disabled. These generic optimizations preserve encoded bytes.

GCC/Clang builds use `-O2`. Guarded intrinsics have portable fallbacks, and
memory-limited builds can exclude the encoder using `BOLTLZ_DECODE_ONLY`.

## Motorola 68000 family

The 68000 has no 32-bit multiply and traps on word or long access at an
odd address. On m68k compilers, `BOLTLZ_M68K` is automatic and selects aligned
or byte-assembled loads, longword matching where alignment permits, one hash
per position, and the exact-copy decoder where the compiler can inline it.
The encoder folds the two halves of the four-byte prefix into a word.
The 68000 and 68008 use a shift, add, and rotate to mix that word. Builds
targeting the 68020 or later use one `MULU.W`. Hash selection can change
which matches the encoder finds. All builds emit the same BoltLZ block format.

On an emulated 8 MHz Atari STE, the new hash improved assembly encoding
throughput by 6.1% in Fast mode and 3.3% in Balanced mode across 21 mixed
32 KiB blocks. With GCC 4.6.4, the C encoder gained 2.6% in Fast mode and
lost 1.8% in Balanced mode on the same blocks. Across Silesia, Calgary,
Canterbury, and the fixed three-file Large subset, aggregate payload size
changed by less than 0.02% in either mode at 16, 32, 64, or 256 KiB. The
largest increase for an individual file was 1.17% for Calgary's `geo` at
256 KiB in Balanced mode.

`BOLTLZ_M68K_ASM=1` (`make atari amiga macos9-68k BOLTLZ_M68K_ASM=1`) adds
[boltlz_m68k.S](../src/core/boltlz_m68k.S), 68000 assembly for the encoder
and decoder loops. It is off by default. A `-m68020` or later build also uses
unaligned long access there.

- The encoder loop (BALANCED and FAST) produces byte-identical output to
  the C loop for the same CPU target.
- The decoder loop decodes a sequence only after validating all of its
  fields, and stops at the start of anything else: a malformed field, the
  last ten input bytes, the final literals. The C decoder continues from
  there, so results, error codes and output bytes match the C decoder.

The original assembly speedups were measured in Hatari on 384 KiB of mixed
Silesia data before the shift-and-add hash was introduced.

| CPU, build | Compress, default level | Compress, fast level | Decompress |
| --- | --- | --- | --- |
| 68000 8 MHz, assembly vs C | 1.86x faster | 1.18x | 2.04x (64 to 131 KB/s) |
| 68030 32 MHz (TT), 68000 assembly vs C | 1.71x | 1.09x | 1.77x |
| 68030 32 MHz (TT), 68020+ assembly vs 68000 C | 1.99x | 1.32x | 2.07x |

Data made of very long matches gains less (about 1.2x decoding), because
copying dominates there and the C code already copies with long moves.

Release packages ship the assembly builds as the programs, with the plain C
build in a `C_BUILD` folder, and for the Atari, Amiga and Classic Mac 68k a
second assembly build for 68020 to 68060 (`make atari-optimized`, `make
amiga-optimized`, `make macos9-68k-optimized`).

`BOLTLZ_M68K_ASM=1` also adds [crc32_m68k.S](../src/core/crc32_m68k.S) and
`-DBP_CRC32_M68K_ASM=1`: a CRC-32 that reads four bytes per round through
byte-swapped slicing-by-4 tables, 64 cycles per byte on a 68000 against 135
for the C loop, with the same results. In the Amiga CLI most of the time
outside the codec is this checksum (the block CRC and the entry CRC), so it
helps extraction more than compression. The Sinclair QL and the Sharp X68000 ship one 68000
build in `Optimized/68000` (`make ql-optimized`, `make x68k-optimized`).

The QL's c68 toolchain cannot assemble GNU syntax, and the code has no
relocations, so [boltlz_m68k_c68.s](../src/core/boltlz_m68k_c68.s) holds the
GNU-assembled 68000 machine code as `as68` data words. Regenerate it with
`scripts/gen_boltlz_m68k_c68.py` after changing `boltlz_m68k.S`; `make
boltlz-m68k-c68-sync-test` (part of `make test`) fails if it is stale. c68 also
keeps decoder mode 2, because it does not inline the copy helpers mode 1 uses.
At original QL speed (sQLux with 68008 timing, 32 KiB of mixed data):

| QL build | Compress, default level | Compress, fast level | Decompress |
| --- | --- | --- | --- |
| 1.11 (generic C) | 0.56 KB/s | 1.1 KB/s | 14 KB/s |
| 68000 C path | 1.25 KB/s | 2.2 KB/s | 14 KB/s |
| Assembly | about 7 KB/s | about 14 KB/s | about 49 KB/s |

`make ql-sqlux-boltlz-asm-test` runs the same check program as the Atari test
in sQLux, with and without the assembly.

`make atari-hatari-boltlz-asm-test HATARI_TOS=... [EMUTOS_ROM=...]` checks
the assembly against the C code over generated inputs (sizes 0 to 256 KiB,
odd addresses, both levels) and 540 corrupted streams (same result codes
and output bytes), on TOS and, with EmuTOS, for 68030 builds on a TT.

## Evaluation

Use the [benchmark driver](../test/bench_boltlz.py) to compare BoltLZ with
pinned LZ4 1.10.0 and frozen BoltLZ checkpoints. It records raw repeated
measurements, compiler flags, source and corpus hashes, exact round trips,
and paired decoder timings. Independent datasets are described in the
[validation report](boltlz-validation-2026-10-14.md).

Ratio changes must be evaluated per file and in aggregate. Throughput and
compressed byte count are separate measures; improvements in one do not
establish an improvement in the other. Hardware/runtime validation and
sustained fuzzing remain necessary for wider platform claims.
