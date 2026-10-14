# BoltLZ design

BoltLZ is an original general-purpose, byte-oriented LZ77 codec implemented
in portable C89. Its two encoder modes share one bounds-checked decoder.
The default Balanced mode favours compressed size; Fast reduces search work.
The archive integration uses method 2 in archive format 4.

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
