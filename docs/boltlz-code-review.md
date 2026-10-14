# BoltLZ code review: speed first, compression second

Review date: 2026-10-13. The current codec has credible opportunities for
further optimization without changing its format or compressed output.
Start with encoder literal emission and the decoder's common paths, then
improve the match-search loop and table layout. Compression improvements
should follow, with an explicit limit on their speed cost.

Implementation follow-up: the first batch has now been implemented and
benchmarked. See [the development results](boltlz-development.md#benchmarks-and-experiments)
for the selected changes and rejected alternatives. This document preserves
the pre-change analysis; its source line numbers refer to the snapshot below.

This is an analysis, not an implemented optimization round. The codec and
its defaults were left unchanged. Measurements below count actual operations
and encoded fields; they do not predict percentage throughput improvements.

The review covers [the codec](../src/core/boltlz.c),
[its configuration](../src/core/boltlz_config.h), the existing `-O2` object
code, and [the development results](boltlz-development.md). The source hash
matches the latest full-corpus benchmark:
`f7bb386375ccf6aaaf1ab201db7007adc7fa211372dee8293517509b327a0b01`.

The existing 32 KiB results on an Intel i5-10500T with Clang 19.1.7 are:

| Codec           | Encoded fraction | Encode MiB/s | Decode MiB/s |
| --------------- | ---------------: | -----------: | -----------: |
| BoltLZ fast     | 48.46%           | 274          | 1364         |
| BoltLZ balanced | 45.00%           | 122          | 1422         |
| LZ4 default     | 49.93%           | 526          | 2937         |

These are the existing measurements, not newly timed results. There is still
a substantial speed gap to LZ4, particularly in balanced compression. The
review does not establish that all of that gap can be removed while keeping
BoltLZ's compression and format.

A temporary instrumented build counted encoder operations and parsed its
output for all twelve Silesia files, totaling 211,938,580 input bytes per
configuration. Both modes were checked at 32/64/256 KiB. Every block's encoded
bytes were compared with an unmodified `-O2` encoder and decompressed back to
the original bytes. All six encoded totals and all corpus hashes match the
existing benchmark. Encoded byte accounting also balances exactly.

The reproducible analysis script and per-file raw counts are local artifacts:

- [Analysis script](../build/boltlz/analysis/source-review-2026-10-13/analyze.py)
- [Operation and token counts](../build/boltlz/analysis/source-review-2026-10-13/counts.json)
- [Existing timing report](../build/boltlz/reports/o2-speed-full.md)

Build artifacts are ignored by Git. Re-run the script from this checkout
with `python3 build/boltlz/analysis/source-review-2026-10-13/analyze.py`.
It uses `-O2` and does not alter the codec sources. Hardware performance
counters were not collected; the `perf` executable is unavailable here.
Consequently, cache misses, branch misses, and CPU cycle attribution remain
unmeasured.

Percentages in the next table use match sequences as their denominator,
except the final row, which uses total decompressed bytes. Extension rows
overlap when a sequence extends both lengths.

| Property                                           | Fast 32 KiB | Balanced 32 KiB | Fast 256 KiB | Balanced 256 KiB |
| -------------------------------------------------- | ----------: | --------------: | -----------: | ---------------: |
| Current decoder fast path                          | 82.04%      | 78.27%          | 79.02%       | 72.22%           |
| Fast path rejected solely by a three-byte distance | 1.25%       | 2.01%           | 5.82%        | 9.79%            |
| Extended literal length                            | 9.16%       | 10.02%          | 7.50%        | 7.98%            |
| Extended match length                              | 8.12%       | 10.46%          | 8.32%        | 10.81%           |
| Reuse of previous distance                         | 3.95%       | 9.23%           | 3.26%        | 8.27%            |
| Match distance below 16                            | 3.38%       | 2.70%           | 2.93%        | 2.29%            |
| Output bytes produced by current fast path         | 49.61%      | 45.67%          | 48.00%       | 42.20%           |

A high fraction of fast-path sequences does not mean the general path is
unimportant: it produces roughly half the output bytes. Conversely, byte
share is not CPU time; long copies can be relatively cheap per byte.

The following speed experiments are ordered by their measured opportunity
and implementation scope.

1. **Specialize short literal emission in the encoder.**

   `bl_compress_impl` copies every nonempty literal run using variable-length
   `memcpy` at lines 431–433. The inspected `-O2` object retains library calls.
   Across these configurations, 94.8–96.3% of nonempty literal runs contain
   at most 16 bytes: approximately seven million short copies per corpus
   pass. Try an exact, bounded first/last-copy helper for these runs and keep
   `memcpy` for larger ranges. The decoder already has this style of helper;
   the encoder does not use it. Test the helper independently because the
   earlier exact chunk/tail decoder implementation was slower.

   This can preserve every encoded byte and the existing exact-capacity
   contract. Do not replace it with an unconditional wide store beyond the
   checked sequence capacity.

2. **Widen decoder fast-path coverage and reduce unnecessary stores.**

   `boltlz_decompress`, lines 630–678, only admits inline lengths and a repeat
   or one-/two-byte explicit distance. Add bounded three-byte distance
   parsing first. At 256 KiB it could admit another 5.82% of fast-mode and
   9.79% of balanced-mode match sequences, with the existing length/margin
   requirements. That is coverage headroom, not an equivalent speed gain.
   At 32 KiB the opportunity is smaller, so avoid slowing the common cases.

   Next try a path for a single-byte length extension: about 98% of extended
   matches have such an extension. A value fitting one byte can still
   describe a match up to 146 bytes, so copying needs its own validated
   bounds and overlap handling. The current fixed 32-byte output margin is
   insufficient for these longer matches.

   In the existing fast path, 54–64% of sequences have zero literals, but
   they still perform a 16-byte literal copy. About 80–83% have matches of
   at most eight bytes, while the distance-at-least-16 branch always writes
   eighteen match bytes. Try a zero-literal path and an eight-byte-match
   class separately. Extra branches may cost more than the stores they save;
   the previous wider-copy experiment is evidence that this tradeoff matters.

   Preserve canonical ULEB128 validation, positive/backward distances,
   exact-sized buffers, overlapping-match semantics, and end-of-block rules.
   Retain the general path for exceptional fields and tails.

3. **Flatten balanced candidate evaluation and pipeline failed searches.**

   `bl_find`, lines 197–250, evaluates repeat, primary, and secondary entries
   through a generic array/loop. Despite forced inlining and constant-level
   specialization, the `-O2` object still contains that loop, stack-backed
   entries, and register spills. At 32 KiB it executes 232.6 million candidate
   visits and 51.4 million match counts, versus fast mode's 17.8 million
   match counts. Use explicit candidate evaluations with the same ordering,
   duplicate handling, scoring, and ties; compare code size as well as speed.

   Separately, 66–71% of main search positions find no usable match. A tight
   miss loop could calculate the next safe input prefix/hash while the
   current table/reference load is pending. Preserve position skipping and
   insertion order, especially when consecutive positions hash to the same
   bucket. This overlaps work across iterations; it is different from the
   already-tested `BOLTLZ_HASH_CACHE`, which merely shared computation within
   one iteration and provided no useful gain.

4. **Use narrower positions for blocks up to 64 KiB.**

   `boltlz_work_size` and `bl_insert_hash` currently use 32-bit entries for
   every block. In a block of at most 65,536 bytes, a valid four-byte hash
   position is at most 65,532, so the stored `position + 1` is at most 65,533.
   It fits in a 16-bit entry while preserving zero as the empty sentinel.
   Keeping 8,192 buckets would halve active fast/balanced tables from
   32/64 KiB to 16/32 KiB and halve table clearing for these blocks.

   Retain the 32-bit path for larger blocks and platforms without a suitable
   16-bit type. The current workspace API takes only a level; it can keep
   returning the largest required allocation while the small-block path
   uses less of it. Test interleaved primary/secondary entries separately for
   balanced mode. This is not the earlier smaller-hash-table experiment:
   narrower entries can preserve all candidates and encoded bytes.

   Also return the final literal sequence before clearing the table for
   inputs shorter than four bytes. That helps tiny inputs, not the full-block
   Silesia workload. Epoch-tagged tables are a later option; their additional
   lookup state may outweigh savings over optimized `memset`.

5. **Carry a winning lookahead result into the next iteration.**

   At line 409, a better `next` match advances `pos` and restarts the loop,
   which recomputes that same match. No table or last-distance state changes
   between these evaluations under the current defaults. Carry the match
   forward, then perform the next iteration's insertion and lookahead in
   their original order.

   This removes 1.58–1.77 million repeated searches per corpus pass, but only
   2.03–2.51% of total balanced matcher calls. It is a bounded opportunity,
   not a solution to balanced mode's full speed gap. Eliminating lookahead
   entirely previously improved speed but increased compressed bytes by
   3.0–3.7%, with larger individual-file regressions.

6. **Specialize field writers and the adequately sized output case.**

   `bl_write_var`, lines 90–96, still loops even though encoded fields need
   at most three bytes. Two-byte explicit distances alone occur in 67–76%
   of all match sequences. Try direct one-/two-/three-byte emission with
   identical encoding and exact writes. The existing `BOLTLZ_CHEAP_COST`
   optimizes byte counting, not this writer.

   A further experiment can select a writer once when destination capacity
   is at least `boltlz_compress_bound(src_len)`, after proving that bound
   covers every emission. Keep the checked path for smaller buffers. This
   could reduce repeated capacity checks; its importance needs timing.

Native-word counting, backward match extension, cheap cost calculation,
constant-level specialization, short-period copying, and wide decoder copies
are already implemented. Recommending these again as new work would miss
the current state. SIMD matching, more forced inlining, branch hints, and
parallel blocks are lower priorities until measurements identify a need.
Parallel blocks improve aggregate throughput but do not improve single-thread
codec speed or compression ratio.

For compression ratio, start with better use of the existing format. Encoder
search and parsing changes can reduce output without adding decoder features.
They may still slow decoding by producing different sequence distributions,
so decoder performance must also be measured.

| Experiment                              | Reason to try it                                                                                                   | Main cost or qualification                                                                                                     |
| --------------------------------------- | ------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------ |
| Selective extra history insertion       | After a match, only the last two positions are inserted; useful interior positions are skipped                     | Try one or two selected positions before a full chain; extra writes and collisions can lose as well as gain matches            |
| Better parsing with the same candidates | Current selection maximizes length minus match cost, and lazy parsing compares that saving with a one-byte penalty | Account for literal-length thresholds and the next repeat-distance state; reuse costs rather than adding repeated calculations |
| An optional higher-ratio encoder        | A bounded four-candidate bucket or limited deeper parse can find matches missed by the two-entry balanced table    | Additional compression time; preserve the fast mode and the decoder format                                                     |
| Larger independent blocks               | Existing 256 KiB results use 4.86% fewer bytes in fast mode and 6.38% fewer in balanced mode than 32 KiB           | Existing decode throughput falls about 8% and 13%; buffering and latency increase                                              |

For parsing, backward extension currently happens only after a candidate and
the lazy decision have been selected. Alternatives can recover different
amounts of pending literals. A selective comparison that accounts for this
and extension-byte thresholds is more informative than raw match savings.
Measure its extra work. The existing `BOLTLZ_EXACT_COST` experiment already
saved only about 0.46% of bytes while reducing balanced throughput to 0.83x
in its earlier screening; simply enabling it is not the speed-first choice.
Earlier larger tables and dual hashes also failed to establish a compelling
default. Use those results as baselines for genuinely different search work.

Format changes remain possible, but the measured token distribution argues
for caution about their payoff:

- **More recent-distance slots:** existing matches reuse the second, third,
  or fourth most recent distinct distance infrequently. Eliminating every
  explicit distance byte for those occurrences would save only 1.34–1.39%
  of fast output and 0.79–0.85% of balanced output on the current parses,
  before paying any tag/index cost. This is a gross saving estimate for a
  fixed parse, not a bound on a redesigned parser's possible improvement.
- **Fixed two-byte distances:** retaining all existing sequences and repeat
  flags but replacing explicit ULEB distances with two bytes would grow
  32 KiB output by 2.90% in fast mode and 1.92% in balanced mode. At 256 KiB,
  a format also needs a way to represent distances above 65,535. A speed
  gain is possible, but it must justify those compression costs.
- **More inline literal bits:** literal runs of 7–14 bytes occur often enough
  to study, but the current fourth high token bit carries repeat-distance
  information. The bit cannot simply serve both purposes. Simulate complete
  token layouts, including escapes, before implementing a new format.
- **Entropy-coded literals or sequences:** literals occupy approximately
  44–51% of encoded bytes, making this a possible larger ratio experiment.
  Byte share alone does not measure their entropy or potential savings.
  Coding tables, buffering, and decode work would change BoltLZ's tradeoff;
  reserve this for an optional mode after the speed work.

LZ4 is a useful implementation reference for pipelined search, narrow hash
positions, and a separate fast decode loop. Its format also gives more inline
literal bits, fixed two-byte offsets, and specific tail restrictions. Those
choices help explain architectural differences, but neither its published
design nor these source observations quantify which choice causes how much
of the measured speed gap. See the pinned
[LZ4 1.10.0 implementation](https://github.com/lz4/lz4/blob/v1.10.0/lib/lz4.c)
and [block format](https://github.com/lz4/lz4/blob/v1.10.0/doc/lz4_Block_format.md).

Freeze the currently selected encoder and decoder before the next timing
round. The existing speed reference predates the last optimization round;
using it alone would make old gains appear alongside new gains. Keep `-O2`
for every candidate and competitor, with identical compiler/target settings.

Screen each idea independently, then useful combinations, at 32/64/256 KiB.
For speed-only changes, require byte-identical encoding, compare decoders
on the same streams, and retain malformed-stream/error and exact-buffer
checks. Use current frozen timings in the same run, alternating order, and
inspect per-file medians and spread. Run sanitizers and differential fuzzing
for any new copy or parsing path before promoting it.

The immediate first batch should be encoder short literal copies, decoder
three-byte distances, and decoder zero-literal/short-match variants. Follow
with explicit balanced candidate evaluation and 16-bit table entries.
Lookahead caching and field writers are small, isolated follow-ups.

Once those gains are established, a reasonable initial ratio experiment
budget is no more than 3% aggregate encode slowdown and 2% aggregate decode
slowdown against the new speed baseline, with per-file regressions reported.
These are proposed selection thresholds, not user requirements. A slower
higher-ratio mode can be evaluated separately. Use an untouched holdout and
another CPU before adopting general-purpose defaults: Silesia has already
been used repeatedly for tuning.
