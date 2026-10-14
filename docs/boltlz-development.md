# BoltLZ development guide

BoltLZ is BoltPack's default general-purpose codec: archive method 2, raw
block format 2, archive format 5. Balanced is the default. `-2` or `--boltlz`
selects the codec, `-l 1` selects Fast, and `-l 2` selects Balanced. Levels
3–9 also map to Balanced. Readers accept only archive format 5.

The [design document](boltlz-design.md) explains the encoder and selected
optimizations. The [specification](specification-1.0.md) defines the complete
stream and archive layouts. The existing ratio/speed defaults were retained
during integration; no match-finding or parsing tradeoff was changed.

## Interfaces and archive integration

- [Core codec](../src/core/boltlz.c) and [block API](../src/core/boltlz.h):
  C89, no hidden allocation, maximum 256 KiB block, exact decoded length.
- [Compression wrapper](../src/core/compress.c): default Balanced, explicit
  modes, destination capacity supplied through `dst_len`.
- [Archive writer](../src/core/archiver.c): reusable input/output buffers and
  a cached 64 KiB encoder workspace, freed by `bp_archive_close`.
- [Mini adapter](../src/core/mini/boltlz_decomp.c): shared decoder-only build
  on 16-bit targets, within the existing 32 KiB mini block limit.

Input and output buffers must be disjoint. Work must be aligned for
`bp_uint32`; `malloc` supplies suitable alignment. The encoder may write
unused bytes within supplied capacity, and only the returned encoded prefix
is valid output. The decoder needs no padding or workspace. Format fields,
distances, truncation, and output bounds are validated.

The archive retains the 1% stored-block fallback and verifies CRC-32 for
both encoded blocks and reconstructed file data. Empty entries are Stored.
CLI and GUI defaults, method labels, source kits and platform build recipes
use BoltLZ. All GCC/Clang codec builds use `-O2`.

## Checks

```sh
make unit-test
make unit-test-asan
make boltlz-test
make boltlz-test-asan
python3 test/test_bench_boltlz.py
python3 test/validate_boltlz.py
python3 test/check_boltpack.py --boltpack build/linux/boltpack
```

The codec suite includes an independent scalar decoder, golden/malformed
streams, exact capacities, overlapping and long-distance matches, 64 KiB
table-width transitions, and 2,048 unaligned-buffer round trips. Archive tests
cover headers, listing, integrity, checksums, and method dispatch. The
platform runner checks default and portable native builds and distinguishes
cross compilation from target execution.

## Benchmarks and experiments

```sh
python3 test/bench_boltlz.py --cc clang --rounds 5 \
    --block-sizes 32768,65536,262144 \
    --codecs boltlz-fast,boltlz-balanced,lz4,boltlz-loop-before-fast,boltlz-loop-before-balanced \
    --require-identical

python3 test/prepare_boltlz_holdout.py --download
python3 test/prepare_boltlz_holdout.py --check-only
python3 test/experiment_boltlz.py --help
```

The driver uses native timing, per-file medians and total inferred time for
aggregate throughput. It alternates codec/decoder order, verifies exact
round trips outside timing, snapshots the executable by hash, and rejects
concurrent benchmarks. Reports record source hashes, flags, input hashes,
raw measurements, spread, and stored-fallback sizes. Allocation and I/O are
excluded. The automatic development data are not an untouched holdout.

[LZ4 1.10.0](../test/reference/lz4/README.md) is pinned and unmodified.
It uses C99 to enable upstream inline paths, with the same `-O2` optimization
level as BoltLZ. The benchmark includes default LZ4 and HC level 9.

Frozen BoltLZ checkpoints preserve the algorithm before encoder, copy, and
loop optimizations. The [loop checkpoint](../test/reference/boltlz-loop-baseline.md)
is the current encoder-identity and paired-decoder reference. Identification
comments were cleaned up during integration and manifest file hashes were
updated; the frozen algorithms and configurations remain unchanged.

The selected optimizations improved speed without changing compressed
bytes. Optional no-lazy parsing remains disabled because its compression
loss was larger than desired. Pipeline and broader decoder-loop experiments
remain disabled because their measured speed was lower.

## Independent evaluation

The [2026-10-14 report](boltlz-validation-2026-10-14.md) records Canterbury,
Calgary and a fixed three-file Large subset. Balanced produced smaller
output than LZ4 on every evaluated file/block combination. Aggregate savings
ranged from 7% to 21%. Fast has measured exceptions, including larger output
on the Large subset at 256 KiB. These corpora are now validation data.

## Fuzzing

```sh
make boltlz-fuzz CC=clang
python3 test/seed_boltlz_fuzz.py
mkdir -p build/boltlz/fuzz-validation-artifacts
build/boltlz/fuzz_boltlz -max_total_time=300 -max_len=262144 \
    -seed=20261014 -timeout=20 -rss_limit_mb=2048 \
    -artifact_prefix=build/boltlz/fuzz-validation-artifacts/ \
    build/boltlz/fuzz-validation-corpus
```

The fuzz target checks both encoder modes, byte identity, independent scalar
and frozen optimized decoding, malformed streams, full 256 KiB blocks, and
exact/one-byte-short capacities. It uses exact positive allocations so
sanitizer redzones expose accesses beyond the declared limits. The earlier
five-minute full-block run passed 82,535 executions with ASan, UBSan and
LeakSanitizer. Runtime tests on other architectures and longer campaigns
remain useful; cross-compilation checks alone do not establish performance.
