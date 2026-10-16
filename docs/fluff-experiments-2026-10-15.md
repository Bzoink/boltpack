# Fluff compression experiments — 2026-10-15

The retained experiment reduces level-8 Silesia output from **64,342,839 to
62,451,851 bytes (2.94% smaller)**. It changes the ANS state layout rather
than the LZ77 parse. The final modern benchmark also includes a decoder refill
optimization. Results below are direct codec measurements; archive metadata
and stored fallback are excluded.

## Retained change

The earlier ANS transitions were invertible, but partitioned the stored state
interval differently from the probability model used for normalization. The
new mapping uses virtual states in `[L, 2L)`, storing `state - L`. Both the
literal/length and distance alphabets use the new mapping. The exact equations
and legacy mapping are in [the format specification](specification-1.0.md#346b-versions-0x09-and-0x89-shifted-state-fse).

The match finder, parse decisions, search depths, window, repeat ring, token
alphabets, normalization and table sizes are unchanged. The encoder computes
the inverse transition without a conditional branch. The decoder caches
bit-width parameters once per symbol and refills three bytes together when
its 32-bit accumulator has room. Tails use the original byte refill.

The encoder writes **`0x09` / `0x89`**. These have the same compact header as
`0x08` / `0x88`, with different entropy payload semantics. Existing decoders
reject the new streams. The new decoder preserves `0x07`/`0x87` and
`0x08`/`0x88` mappings and retains the `0x06` range decoder. Cold parallel
groups write `0x89`; groups with preloaded history keep `0x09`.

At this stage, `sizeof(fluff_ctx_t)` remained **282,072 bytes** on the
64-bit build. Decoder table construction used **5,516 temporary bytes**,
2,312 more than the earlier implementation; table sizes and encoder working
memory were unchanged. The [subsequent allocation work](fluff-memory-2026-10-15.md)
reduces encoder scratch. These are requested-byte counts, not process RSS.

## Paired measurements

| Profile | Level | Encoded bytes | Encode time | Decode time |
| --- | ---: | ---: | ---: | ---: |
| Portable, Silesia | 1 | -3.03% | +0.4% | — |
| Portable, Silesia | 5 | -2.95% | -0.5% | — |
| Portable, Silesia | 8 | -2.94% | +0.2% | — |
| Modern SIMD, Silesia | 8 | -2.94% | -0.9% | -8.8% |
| Portable, holdout | 8 | -3.95% | -0.1% | -8.8% |

Negative changes mean fewer bytes or less time. Time changes are medians of
paired aggregate ratios. The worst per-file median paired encoding increase
was 1.30% on portable Silesia, 0.23% on modern Silesia, and 3.10% on the holdout.

Silesia contains twelve files, 211,938,580 input bytes and 815 blocks at 256 KiB.
The untouched local holdout contains Calgary, Canterbury and the three large
fixed files. Its files were first benchmarked after the implementation was
selected; no tuning used their results. Corpus hashes and aggregate results
are in [the JSON record](benchmarks/fluff-2026-10-15.json), with
[per-file timings in CSV](benchmarks/fluff-2026-10-15-files.csv).

The portable encoding run preceded only the final decoder refill change;
source comparison confirms all code outside `fse_bits_read` is identical.
Its decoder timings are preserved as historical data, not final decoder
performance claims. Modern and holdout runs use the final implementation.
Modern here means SIMD with sequential encoding, separately from threaded
archive correctness checks.

Measurements use five alternating pairs per file, a warmup of each codec,
preloaded input, persistent encoder/decoder contexts, and CPU affinity on an
Intel Core i5-10500T. Reset time is recorded separately; allocation, input I/O,
verification and deterministic-output checks are outside timed loops.
Portable builds use Clang 19.1.7, strict C89, `-O3 -march=native`; modern builds
use GCC 14.2.0 with C11, `-O3 -march=native -fopenmp`.

CPU frequency was not locked. There is visible frequency drift in the samples;
per-file comparisons use the median of paired time ratios rather than the
ratio of separate medians. Aggregate paired changes compare sums of each
pair's per-file times. The JSON also retains the ratio of aggregate medians
and variation. These are local measurements, not cross-platform guarantees.

The small-input check used 18 synthetic text/random inputs from 0 to 65,536
bytes and 50 alternating pairs. With already allocated contexts, total
reset-plus-encode time was essentially unchanged (median paired change
−0.42%). Encoded bytes fell from 90,389 to 90,019. This excludes initial
context allocation and filesystem overhead; it is not an end-to-end startup
latency measurement. The worst per-file paired median encoding increase was
3.29%. Short timings have visibly greater variation.

## Other experiments

All screened variants round-tripped every Silesia block with their own matching
decoder. These are independent experiments at level 8, not combinations with
the retained coder:

| Experiment | Change in compressed bytes |
| --- | ---: |
| Nearby sub-hash alternative | −0.020% |
| Backward extension over preceding literals | −0.091% |
| Adaptive symbol costs and literal-span estimates | −0.120% |
| Separate repeat-length FSE table | −0.507% |
| Literal/length table log 9 | −0.406% |
| Literal/length table log 11 / 12 | +0.803% / +1.376% |
| Longer matches, best tested variant | −0.041% |
| Marginal normalization approximation | −0.063% |
| New ANS state layout | **−2.939%** |

Simple literal-price, normal-match-overhead and lazy-margin adjustments were
also screened. Their changes ranged from −0.012% to +0.166%. The adaptive
prototype added work for a small size gain; the format alternatives offered
less benefit than the retained state mapping. No speed acceptance claim is
based on screening timings. Exact sizes, source hashes, recipes and patches
are retained in [the experiment directory](../test/experiments/fluff-2026-10-15/README.md).
The larger tables and other mechanisms were not retested in combination with
the new ANS mapping.

## Correctness

- Exhaustive ANS inverse checks for every state and every two-symbol count
  distribution at both table sizes, including powers of two and singletons.
- All nine levels: empty/tiny blocks, length boundaries, random data, runs,
  near-matches, exact allocation bounds and canaries.
- Persistent history through window wrap, compressed/stored/compressed
  sequences, short tails, and explicit new reset blocks.
- Frozen two-block `0x07`/`0x08` fixtures and `0x87`/`0x88` reset variants.
- Batched reader equivalence against the byte reader for widths 0–17, short
  streams, tails and zero padding; truncated and mutated new streams under
  ASan/UBSan.
- Every timed block byte-verified. The candidate decoder also verifies the
  frozen baseline's output. Repeated encoded FNV hashes must agree.
- Full unit suites, sanitizer suites, parse-dump build and 96 CLI integration
  checks, including a two-thread grouped archive roundtrip. Additional mixed
  compressed/stored roundtrips cover group sizes 1/2/4/8 and thread counts
  1/2/4.

Sanitizers used `ASAN_OPTIONS=detect_leaks=0`: LeakSanitizer cannot run under
ptrace in this environment. This was not a long decoder fuzz campaign or a
32-bit/big-endian portability run.

## History-window follow-up

The extra history provides measurable compression value. A separate level-8
experiment capped ordinary match distance at 32/64/128 KiB and the existing
256 KiB-minus-one maximum. Repeat distances originate from those capped
matches. All variants retain the same allocations, hash tables, block size,
distance alphabet and decoder; this measures the value of available history,
not the speed or memory footprint of a physically smaller implementation.
Every block was decoded and byte-compared. The full-window output hashes
match the preceding benchmark.

| Maximum match distance | Silesia encoded bytes | Other corpus encoded bytes |
| --- | ---: | ---: |
| 32 KiB | 66,898,544 | 4,893,110 |
| 64 KiB | 65,141,026 | 4,747,927 |
| 128 KiB | 63,831,057 | 4,648,147 |
| 256 KiB minus one | 62,451,851 | 4,566,318 |

The full window makes output **6.65% smaller on Silesia and 6.68% smaller on
the other corpus** than the 32 KiB reach. The last doubling, 128 to 256 KiB,
alone saves 2.16% and 1.76%, respectively. With the full window, 39.1% of
Silesia matches have distances above 32 KiB, covering 35.9% of input bytes.
Individual file benefits vary: XML saves 17.4%, whereas `sao` saves 1.8%.
Some small or highly repetitive files in the other corpus slightly worsen.
This is a comparison within Fluff, not a compression comparison with DEFLATE.
Windows above 256 KiB have not been measured.

The memory distinction matters: 32 KiB is a history size. At 256 KiB blocks,
the encoder before the [allocation changes](fluff-memory-2026-10-15.md)
requested approximately **3.33 MiB** internally:
0.269 MiB for its context (including history), 1.75 MiB for match tables,
and 1.314 MiB for token/stream scratch. Caller buffers, allocator overhead,
and transient table-building allocations are additional. Decode-only contexts
request approximately 276 KiB plus temporary table-building scratch. Reducing
the physical history and its per-position chain from 256 to 32 KiB could save
about 1.09 MiB per encoder, with the other allocations held fixed; doing so
needs implementation work and was not done in this reach-only experiment.

[Per-file measurements](benchmarks/fluff-window-2026-10-15.csv) and
[the manifest and totals](benchmarks/fluff-window-2026-10-15.json) record the
experiment. Local harness and variant sources are under
`build/fluff-window-2026-10-15/`. To reproduce the ratio comparison with the
versioned benchmark runner, copy `src/core/fluff.c`, replace
`FL_MIN(cur_abs, FLUFF_WINDOW_SIZE - 1)` in `fl_find_match_hyb` with
`FL_MIN(cur_abs, 32768U)`, and pass that copy as `--baseline-source` to
`test/bench_fluff.py`, with the current source as candidate, level 8 and
256 KiB blocks. This leaves production settings unchanged.

## Reproduce

The baseline is revision `f627bac5e77da844a863a99a41bdfd8eb3c3d0ee`, matching
the review's source SHA-256. From the repository root:

```sh
mkdir -p build
git show f627bac5:src/core/fluff.c > build/fluff-ab-baseline.c
python3 test/bench_fluff.py --baseline-source build/fluff-ab-baseline.c \
  --output build/fluff-ab-portable --cc clang --pairs 5 --levels 1,5,8 \
  test/corpus/silesia/*
python3 test/bench_fluff.py --baseline-source build/fluff-ab-baseline.c \
  --output build/fluff-ab-modern --cc gcc --modern --pairs 5 --levels 8 \
  test/corpus/silesia/*
make unit-test
ASAN_OPTIONS=detect_leaks=0 make unit-test-asan
make modern CC=gcc
python3 test/check_boltpack.py --boltpack build/linux/boltpack
```

The runner freezes both sources, links independently named baseline functions,
alternates codecs within each file, and writes `manifest.json`, `samples.csv`
and `summary.json`. Full local artifacts are under the Git-ignored
`build/fluff-experiment-2026-10-15/`; the versioned record contains the final
summaries, source/binary/corpus hashes, and per-file variation.
