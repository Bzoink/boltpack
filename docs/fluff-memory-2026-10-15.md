# Fluff allocation investigation — 2026-10-15

The retained changes reduce encoder allocations at 256 KiB blocks from
**3,495,416 to 2,641,360 bytes**, saving **854,056 bytes (24.43%)** per
encoder. The 256 KiB history, match tables, parsing and encoded bytes are
unchanged. No additional format change is needed.

## Where the memory goes

Requested bytes on the 64-bit test build, excluding caller input/output,
allocator overhead, stack and process/runtime memory:

| Allocation | Before | Retained |
| --- | ---: | ---: |
| Context, including 256 KiB history and ANS tables | 282,072 | 282,048 |
| Four-byte hash heads: 131,072 × 4 | 524,288 | 524,288 |
| Per-position chain: 262,144 × 4 | 1,048,576 | 1,048,576 |
| Three-byte hash: 65,536 × 4 | 262,144 | 262,144 |
| Token buffer: 2 × block size + 16 | 524,304 | 524,304 |
| Token type index: block size + 16 | 262,160 | 0 |
| Literal/length stream scratch: compress_bound | 295,936 | 0 |
| Distance/raw stream scratch: compress_bound | 295,936 | 0 |
| **Persistent encoder total** | **3,495,416** | **2,641,360** |
| **Peak during fixed-size encoding, including temporaries** | **3,499,776** | **2,645,720** |

Persistent scratch drops from 1,378,336 to 524,304 bytes (62.0%). There are
five retained heap allocations per encoder, previously eight. Normalization
and ANS table construction still make eight small temporary allocations per
compressed block, with at most 4,360 bytes live simultaneously. They are a
possible allocator-traffic optimization, but not a material part of the byte
footprint. The remaining match tables account for 69.5% of retained memory.

Scratch grows to the largest block seen and stays allocated until context
destruction. Growth now allocates the replacement before freeing the old
buffer, preserving usable scratch on allocation failure. Consequently,
growing from 262,143 to 262,144 input bytes briefly requests **3,165,662
bytes**; this exceeds the fixed-size peak above. All tracked allocations
return to zero after context destruction, including failure paths.

A decode-only context requests 282,048 bytes, plus at most 5,516 bytes during
table construction: it never allocates match tables or encoder scratch.
Parallel encoders get the scratch savings per worker that compresses blocks.
The fast context currently allocates match tables for all configured workers
during setup, including workers that may remain idle; archive batch input and
output slabs are additional allocations outside this accounting.

These are allocation counts, not measured RSS. A DEFLATE 32 KiB dictionary is
also a history size rather than its complete encoder allocation. The separate
[window experiment](fluff-experiments-2026-10-15.md#history-window-follow-up)
measured useful ratio gains from retaining Fluff's larger history.

## How the buffers were removed

Each internal token now ends with its type, so the encoder can walk backwards
without building a side index. This also removes a forward scan. The count
tables and fixed header tail are written before entropy coding; the
literal/length stream then goes directly into the caller's output buffer,
and the states and stream lengths are patched into the reserved tail.

The distance/raw stream grows downwards from the end of the token buffer as
tokens are consumed backwards. A normal token consumes six bytes before
emitting at most 31 bits, a repeat consumes three before at most ten bits,
and a literal consumes two and emits no raw bits. Even allowing seven pending
bits, no token emits more bytes than it consumes. Token fields are loaded
before writing raw bits, and the allocation has another 16 bytes for slack
and the final sentinel. Thus raw writes cannot reach unread tokens. The final
copy reverses the stored bytes back into the existing stream order.

The investigation also found an existing allocator ownership bug: match
tables bypassed the custom allocation hook through system `calloc`, while
destruction used the custom free hook. The retained code routes zeroed table
allocations through the hook when installed, preserving system `calloc` when
using the default allocator.

## Paired encoding measurements

All runs use 256 KiB blocks, persistent per-file contexts, one warmup per file,
and three alternating pairs in one process, pinned to CPU 0 on the Intel
Core i5-10500T. Portable builds use Clang 19.1.7; modern builds use GCC 14.2.0
with SIMD enabled and sequential compression. Flags are `-O3 -march=native`.
Allocation, input I/O, reset and byte verification are outside encoding time.

| Profile | Level | Encoded-byte change | Median paired encoding-time change |
| --- | ---: | ---: | ---: |
| Portable, Silesia | 1 | 0 | −2.87% |
| Portable, Silesia | 8 | 0 | −1.08% |
| Modern SIMD, Silesia | 8 | 0 | +0.23% |
| Portable, Calgary + Canterbury + large-fixed-3 | 8 | 0 | +0.19% |

Every file, level and pair matches the baseline's encoded length and
fingerprint, and every block verifies against its input. Level-8 totals remain
62,451,851 bytes for Silesia and 4,566,318 for the 28-file holdout corpus.
The largest Silesia per-file median paired encoding increase is 4.82%; the
holdout's largest is 8.24% on `world192.txt`. The largest aggregate paired
increase among these four comparisons is 2.63%. This is a shared machine
using the powersave governor, so sub-percent differences are not evidence of
a repeatable speed change.

A targeted recheck of the holdout outliers (`world192.txt`, `obj2`, and
`grammar.lsp`) uses 15 alternating pairs on CPU 8. At level 8, their median
paired encoding changes are **+0.43%, −1.60%, and −5.57%**, respectively;
at level 1 they are +1.01%, −1.88%, and −3.43%. Thus the short-run text
regression does not reproduce. Individual samples remain noisy, especially
on the 3.7 KiB Lisp file; full samples are retained with the report.

Short decoder samples are noisy, including interruptions that inflate the
aggregate SIMD result. The decoder's normalized generated instruction sequence
is identical apart from addresses and padding. A separate decoder harness
repeats each timed decode ten times, using five alternating pairs on CPU 8;
compression and verification remain outside that timing. Its median paired
aggregate change is **−2.96%**, with all five pairs between −4.80% and −2.44%.
This follow-up does not show a decoding regression. Since decoding logic is
unchanged, it is treated as a preservation check rather than an algorithmic
decoder speedup.

## Smaller match-table experiments

Halving the head tables while retaining the full history and chain has very
little size cost at level 8 on Silesia:

| Table change | Additional memory saved | Encoded bytes | Size change |
| --- | ---: | ---: | ---: |
| Main hash: 17 → 16 bits | 262,144 | 62,453,173 | +0.00212% |
| Three-byte hash: 16 → 15 bits | 131,072 | 62,452,114 | +0.00042% |
| Both | 393,216 | 62,453,439 | +0.00254% |

These are separate parser experiments: collisions change candidate selection.
All screened blocks roundtrip correctly. Unlike the buffer cleanup, these
variants do not preserve the exact output. Screening timings are unpaired and
are not used to infer speed gains.

A controlled follow-up compares the retained buffer implementation against
halving both tables, with two alternating pairs, one warmup and Clang on CPU 8.
It adds **6.59% encoding time at level 1** (pairs +6.56% and +6.63%) and
**4.98% at level 8** (pairs +2.91% and +7.05%). Level-1 output also grows
64,143 bytes, or 0.0992%. Although this would reduce persistent encoder memory
further to 2,248,144 bytes (2.14 MiB), it exceeds the proposed small encoding
budget. The table sizes are therefore unchanged. The isolated three-byte
hash reduction remains a separate experiment without paired speed validation.

## Reproduction

The [experiment sources and instructions](../test/experiments/fluff-memory-2026-10-15/README.md)
include the allocation tracer and a reversible patch to reconstruct the
pre-memory-change, shifted-ANS baseline. The baseline source SHA-256 is
`f52cd6e1abe45314d2ddbf455255255fdc1efa6678056c7c22132826233df76717e`.

The allocation tracer includes both fixed-block and worst-case scratch-growth
examples. Codec tests cover allocator ownership, allocation failure cleanup,
growth failure preserving the old buffer, and reuse for smaller blocks, in
addition to the existing all-level roundtrip, history, bounds and legacy
format checks.

Validation passed all 195 normal unit checks and the same 195 under
AddressSanitizer/UBSan, 96 CLI integration checks, and twelve mixed-data
roundtrips covering threads 1/2/4 and group sizes 1/2/4/8. The final expanded
allocator-failure test also passed separately in normal and sanitizer builds.
LeakSanitizer was disabled because of the environment's ptrace restriction;
requested-byte accounting and injected-failure ownership checks verify that
all tracked codec allocations are released. The parse-trace build also passes.

[Requested allocations](benchmarks/fluff-memory-2026-10-15-allocations.csv),
[paired samples](benchmarks/fluff-memory-2026-10-15-samples.csv),
[longer decoder samples](benchmarks/fluff-memory-2026-10-15-decode.csv), and
[manifests and summaries](benchmarks/fluff-memory-2026-10-15.json) preserve
the measurements and source/corpus fingerprints.
The [smaller-table paired samples](benchmarks/fluff-memory-2026-10-15-half-samples.csv)
record the rejected tradeoff.
The [targeted encoding samples](benchmarks/fluff-memory-2026-10-15-text-samples.csv)
record the longer holdout recheck.
