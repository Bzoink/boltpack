# BoltLZ independent validation, 2026-10-14

The current defaults were retained throughout this evaluation. Balanced mode
continues to offer smaller output than LZ4, while LZ4 retains higher codec
throughput. Fast mode has a useful speed/size tradeoff with data-dependent exceptions.

## Dataset selection and preservation

The encoder, decoder, configuration, type definitions, and reference sources
were hashed before the first measurements. Their hashes in the
[evaluation manifest](../test/reference/boltlz-holdout.json) identified the
pre-integration sources and the recorded pre-integration benchmark reports. No codec tuning followed
inspection of these results.

Data came from the [official Canterbury distribution](https://corpus.canterbury.ac.nz/descriptions/):

| Selection                                                 | Files | Input bytes |
| --------------------------------------------------------- | ----: | ----------: |
| Canterbury                                                | 11    | 2,810,784   |
| Calgary, published 14-file selection                      | 14    | 3,141,622   |
| Fixed Large subset: `E.coli`, `bible.txt`, `world192.txt` | 3     | 11,159,482  |

The Calgary archive also contains `paper3` through `paper6`; these were
excluded before measurement to match the author's documented selection.
The Large collection can grow, so its results refer specifically to the
three named files. Corpus files and downloaded archives stay under ignored
`test/corpus`; only names, sizes, provenance, and SHA-256 hashes are vendored.

These data had not previously been used in BoltLZ tuning. They now form a
validation set; future experiments cannot describe them as an untouched
holdout. Canterbury's `ptt5` and Calgary's `pic` are byte-identical. The
datasets are reported separately rather than combined into an overall score.
None of the files is an exact duplicate of a Silesia file, although historical
corpora can share content, genres, and statistical characteristics. They do
not establish coverage of every modern application workload.

## Measurement method

All codecs used `-O2`, Clang 19.1.7, one thread, and independent 32/64/256 KiB
blocks on the Intel Core i5-10500T. BoltLZ used C89 flags; pinned
LZ4 1.10.0 used C99 with the same optimization level. Each file/block/codec
combination had five rounds, with at least 30 ms per timed batch. Aggregate
rates are total bytes divided by the time inferred from each file's median
throughput. CPU frequency was uncontrolled.

Loading, allocation, process startup, filesystem I/O, archive headers, and
checksums are excluded. Small inputs fit in cache. The reported fractions
are raw codec output, with smaller fractions representing better compression.
Every block was checked for exact round-trip reconstruction and byte-for-byte
encoder agreement with the frozen loop baseline. Paired decoder timings used
the same compressed blocks. All 2,520 raw samples passed their checks.

Raw reports are under `build/boltlz/reports/o2-holdout-{canterbury,calgary,large-fixed-3}.json`
and corresponding `.md` files. The JSON retains every sample, corpus and
source hashes, compiler flags, and immutable benchmark binary hash.

## Results at 32 KiB

| Dataset            | Codec           | Output/input | Encode MiB/s | Decode MiB/s |
| ------------------ | --------------- | -----------: | -----------: | -----------: |
| Canterbury         | BoltLZ Fast     | 40.29%       | 332          | 1487         |
| Canterbury         | BoltLZ Balanced | 36.34%       | 140          | 1622         |
| Canterbury         | LZ4 default     | 44.85%       | 511          | 2955         |
| Calgary            | BoltLZ Fast     | 52.25%       | 258          | 1340         |
| Calgary            | BoltLZ Balanced | 48.84%       | 117          | 1411         |
| Calgary            | LZ4 default     | 53.16%       | 425          | 2924         |
| Large fixed subset | BoltLZ Fast     | 56.10%       | 264          | 835          |
| Large fixed subset | BoltLZ Balanced | 51.67%       | 115          | 968          |
| Large fixed subset | LZ4 default     | 60.10%       | 419          | 2042         |

At 32 KiB, Balanced produces 19.0%, 8.1%, and 14.0% fewer compressed bytes
than LZ4 on Canterbury, Calgary, and the fixed Large subset, respectively.
Fast produces 10.2%, 1.7%, and 6.6% fewer compressed bytes. These percentages
compare compressed byte counts, not percentage points of the input fraction.

Balanced's compression throughput is about 27% of LZ4's on these datasets;
its decompression throughput ranges from 47% to 55% of LZ4's. Fast reaches
61% to 65% of LZ4's compression throughput and 41% to 50% of its decompression
throughput. The ratio advantage therefore carries a measurable CPU cost.

## Block size and individual-file exceptions

| Selection          | Block   | Fast size delta vs LZ4 | Balanced size delta vs LZ4 |
| ------------------ | ------- | ---------------------: | -------------------------: |
| Canterbury         | 32 KiB  | -10.17%                | -18.98%                    |
| Canterbury         | 64 KiB  | -9.60%                 | -19.19%                    |
| Canterbury         | 256 KiB | -10.86%                | -21.12%                    |
| Calgary            | 32 KiB  | -1.71%                 | -8.13%                     |
| Calgary            | 64 KiB  | -1.04%                 | -8.29%                     |
| Calgary            | 256 KiB | -3.80%                 | -11.80%                    |
| Large fixed subset | 32 KiB  | -6.65%                 | -14.03%                    |
| Large fixed subset | 64 KiB  | -6.30%                 | -14.44%                    |
| Large fixed subset | 256 KiB | +2.69%                 | -7.26%                     |

Balanced beats LZ4's compressed size on each of the 28 dataset members at all
three block sizes; one fax file occurs in two datasets. Fast has several
individual regressions. At 32 KiB it produces 0.74% more bytes on Calgary's
`bib` and 0.83% more on `geo`. At 256 KiB it produces 7.88% more on `E.coli`
and 1.29% more on `bible.txt`; those files reverse the fixed Large subset's
aggregate advantage over LZ4. Larger blocks do not automatically improve the
comparison with another codec, even when BoltLZ's own output gets smaller.

## Correctness and portability

The unit suite now has 31 checks. Two new checks cover 2,048 round trips with
source, compressed input/output, and decoded output offsets from zero through
seven bytes, including 64 KiB table-width transitions and the full 256 KiB
block. Allocations end at the declared positive buffer limits, and prefix
guards check stores before each buffer. A separate scalar decoder verifies
the reconstructed bytes.

The existing independent scalar decoder was extracted into a
[shared validation header](../test/reference/boltlz_scalar_decoder.h).
Both unit tests and fuzzing now use its bytewise parser/copy logic, without
sharing production decoder helpers. The frozen optimized decoder still
checks precise error codes. This adds an independent acceptance/output check
to fuzzing instead of relying exclusively on two related optimized decoders.

The [platform validation runner](../test/validate_boltlz.py) passed the full
suite with both Clang and GCC, with default and portable controls: four native
configurations, all using `-O2 -Werror`. The portable configuration disables
native word counting, encoder specialization, short/wide copies, the fast
decoder, narrow tables, and the full-capacity shortcut.

It also compiled default and portable codec objects for AArch64 little/big
endian, ARMv7 little/big endian, PowerPC32, m68k, i686, and MSP430: sixteen
cross-compilation checks. MSP430 uses `SIZEOF_INT=2` for the existing 16-bit
integer type override. These probes use Clang freestanding headers plus
minimal `memcpy`/`memset` declarations. They validate compilation and code
generation, rather than linking against target libraries or running the
codec. Target runtime correctness, 16-bit memory limits, and ARM throughput
remain unverified.

AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer passed all 31
unit checks. The eight benchmark-driver regression checks also passed.

## Longer fuzzing

The [fuzz target](../test/fuzz_boltlz.c) now accepts the full 256 KiB block
range. It copies encoder input into exact allocations and compares decoding
against both independent scalar and frozen optimized references. Three-byte
expected-length prefixes allow malformed streams to request outputs up to
256 KiB. A subset of inputs also checks compression into the exact encoded
size and rejection with one byte less, exercising the checked-capacity path.

The [seed generator](../test/seed_boltlz_fuzz.py) adds raw full blocks,
65,535/65,536/65,537-byte inputs, long explicit/repeated distances, and malformed
distance/truncation cases. The five-minute run completed 82,535 executions in
301 seconds with `-O2`, ASan, UBSan, and LeakSanitizer, with a maximum input
size of 262,144 bytes. It found no reference differences or sanitizer errors,
and produced no failure artifacts. This is an execution count, not a count
of unique inputs or proof that all malformed streams have been covered.
Its build provenance, completion count, log, evolving corpus, and artifact
directory are retained under `build/boltlz/validation-2026-10-14` and
`build/boltlz/fuzz-validation-corpus`. The round's structured summary is
`build/boltlz/validation-2026-10-14/results.json`.

## Reproduction

```sh
# Download missing official archives; all archives and selected files are
# verified against test/reference/boltlz-holdout.json.
python3 test/prepare_boltlz_holdout.py --download
python3 test/prepare_boltlz_holdout.py --check-only

for dataset in canterbury calgary large-fixed-3; do
    python3 test/bench_boltlz.py \
        --corpus "test/corpus/boltlz-holdout-2026-10-14/$dataset" \
        --split validation --block-sizes 32768,65536,262144 \
        --seconds 0.03 --rounds 5 --cc clang --require-identical \
        --codecs boltlz-fast,boltlz-balanced,lz4,boltlz-loop-before-fast,boltlz-loop-before-balanced \
        --output "build/boltlz/reports/o2-validation-$dataset"
done

python3 test/validate_boltlz.py
make boltlz-test-asan CC=clang
python3 test/test_bench_boltlz.py

make boltlz-fuzz CC=clang
python3 test/seed_boltlz_fuzz.py
mkdir -p build/boltlz/fuzz-validation-artifacts
build/boltlz/fuzz_boltlz -max_total_time=300 -max_len=262144 \
    -seed=20261014 -timeout=20 -rss_limit_mb=2048 \
    -artifact_prefix=build/boltlz/fuzz-validation-artifacts/ \
    build/boltlz/fuzz-validation-corpus
```

To avoid contaminating timing, benchmark processes run sequentially and
finish before compiler-matrix checks and fuzzing. Initial reports record the
first evaluation as `holdout`; reproduction uses `validation`.

## Assessment and remaining work

The results support keeping the current defaults. Balanced's size advantage
extends beyond Silesia, and Fast's exceptions now have explicit measurements.
Further optimizations should be evaluated as experiments against these frozen
defaults, with ratio regressions reported per file as well as in aggregate.

Real target runtime tests, ARM64 hardware measurements, additional modern
application datasets, longer fuzz campaigns, and archive-layer integration
remain separate work. BoltLZ is integrated as archive method 2 in format 4; this report records the preceding standalone validation.
