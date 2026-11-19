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
slower and are disabled. The selected speed changes preserve encoded bytes.

GCC/Clang builds use `-O2`. Guarded intrinsics have portable fallbacks, and
memory-limited builds can exclude the encoder using `BOLTLZ_DECODE_ONLY`.

## Motorola 68000 family

The 68000 has no 32-bit multiply and traps on word or long access at an
odd address, so on m68k compilers `BOLTLZ_M68K` (automatic) replaces the
`memcpy` loads with aligned or byte-assembled loads, hashes with one 16x16
`MULU.W`, compares match lengths a long at a time when both sides share
parity, hashes each position once and selects the exact-copy decoder. The
hash differs from other CPUs, so m68k output differs slightly (up to 0.25%
larger); the format is unchanged. Other CPUs compile to the same code.

`BOLTLZ_M68K_ASM=1` (`make atari amiga macos9-68k BOLTLZ_M68K_ASM=1`) adds
[boltlz_m68k.S](../src/core/boltlz_m68k.S), the BALANCED and FAST encoder
loop in 68000 assembly with byte-identical output to the C loop. A
`-m68020` or later build also uses unaligned long access there. It is off by
default. Measured in Hatari (emulated time, compression only):

| CPU, build | Default level | Fast level |
| --- | --- | --- |
| 68000 8 MHz, assembly vs C | 1.9-2.0x faster | 1.2-1.5x faster |
| 68030 32 MHz (TT), 68000 assembly vs C | 1.7x | 1.1x |
| 68030 32 MHz (TT), 68020+ assembly vs 68000 C | 2.0x | 1.3x |

`make atari-hatari-boltlz-asm-test HATARI_TOS=... [EMUTOS_ROM=...]` checks
that the assembly output is identical to the C output over generated inputs
(sizes 0 to 256 KiB, odd addresses, both levels), on TOS and, with EmuTOS,
for 68030 builds on a TT. Decoding is still C.

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
