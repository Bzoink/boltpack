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
[boltlz_m68k.S](../src/core/boltlz_m68k.S), 68000 assembly for the encoder
and decoder loops. It is off by default. A `-m68020` or later build also uses
unaligned long access there.

- The encoder loop (BALANCED and FAST) produces byte-identical output to
  the C loop.
- The decoder loop decodes a sequence only after validating all of its
  fields, and stops at the start of anything else: a malformed field, the
  last ten input bytes, the final literals. The C decoder continues from
  there, so results, error codes and output bytes match the C decoder.

Measured in Hatari on 384 KiB of mixed Silesia data (emulated time):

| CPU, build | Compress, default level | Compress, fast level | Decompress |
| --- | --- | --- | --- |
| 68000 8 MHz, assembly vs C | 1.86x faster | 1.18x | 2.04x (64 to 131 KB/s) |
| 68030 32 MHz (TT), 68000 assembly vs C | 1.71x | 1.09x | 1.77x |
| 68030 32 MHz (TT), 68020+ assembly vs 68000 C | 1.99x | 1.32x | 2.07x |

Data made of very long matches gains less (about 1.2x decoding), because
copying dominates there and the C code already copies with long moves.

Release packages ship the C build and, in an optimized folder, assembly
builds for the 68000 and for 68020 to 68060 (`make atari-optimized`,
`make amiga-optimized`). Classic Mac 68k ships them as separate packages
(`mac68k-optimized-68000.hqx`, `mac68k-optimized-68020.hqx`; `make
macos9-68k-optimized`).

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
