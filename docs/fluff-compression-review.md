# Fluff: improving compression within a small speed budget

Review date: 2026-10-15. Source revision:
`f627bac5e77da844a863a99a41bdfd8eb3c3d0ee`.

This is a **pre-v1.8 snapshot**, not a description of the current codec. At the
recorded revision the encoder wrote `0x08`/`0x88` and the proposed parser work
was still unmeasured. BoltPack 1.8 later shipped the shifted-state `0x09`/`0x89`
layout; see [the measured follow-up](fluff-experiments-2026-10-15.md) and
[the allocation report](fluff-memory-2026-10-15.md) for current behavior and
results. The baseline analysis and proposed parser experiments below remain a
historical record.

**Yes: there are credible opportunities to improve Fluff's compression without
substantially slowing compression. The best first experiments are better match
cost estimates and retaining a cheap alternative to the longest match.** Both
can use the existing format and search budget. Their actual gains remain to be
measured; this review does not establish a percentage improvement for them.

At the recorded revision, this was an analysis and development proposal; its
baseline corpus analysis and one small experiment on a private source copy did
not establish a throughput improvement. The later v1.8 wire-format change is
documented separately and did not adopt the parser proposals here.

As a starting acceptance policy, interpret a small speed cost as at most **3%
more aggregate encoding time**, with no unexplained regression above **5% in a
representative workload category**. These are proposed engineering limits, not
requirements already established by the project. Measure decoding and memory
too: encoder-only changes can still change decoder performance through the
tokens they produce.

## 1. What the current code actually does

The relevant implementation is [fluff.c](../src/core/fluff.c), with parallel
compression in [fluff_fast.c](../src/core/fluff_fast.c) and archive integration
in [archiver.c](../src/core/archiver.c).

| Area | Current behavior | Consequence for optimization |
| --- | --- | --- |
| History | 256 KiB window; ordinary match search caps distance at window size minus one | Bigger blocks or a bigger window are not simple tuning switches |
| Match finding | 17-bit four-byte hash chain, seeded by one candidate from a 16-bit three-byte hash | Much of the infrastructure for a cheap alternative candidate already exists |
| Levels | Chain limits 8, 16, 32, 64, 96, 128, 192, 256, 512 | Levels mostly change search depth, rather than parser quality |
| Early stop | A chain improvement reaching 64 bytes can terminate search; parser skips lazy search for matches of at least 64 bytes | Increasing chain depth alone will not explore beyond all early exits |
| Match representation | Lengths 3–258; explicit distance or one of three repeat distances | The longest match need not have the lowest encoded cost |
| Parsing | Greedy with one-byte lazy lookahead; second lookahead only after the first wins | Selective second lookahead already exists |
| Scoring | Normal match: `18 + length_extra + distance_extra`; repeat: `11 + length_extra`; replaced literals: `length * 8` | Ranking does not reflect actual block symbol probabilities |
| Entropy coding | Per-block FSE tables: 1,024 lit/len slots and 512 distance slots | Costs vary with the parse and normalization |
| Current output | Compact `0x08`, or `0x88` when a reset must be signaled | Parser changes can retain compatibility with current decoders |
| Repeat state | Three distances initialized to zero in each block's encoder and decoder | History persists across blocks, but the repeat ring does not |
| Allocation | Reused match tables, token buffers, stream scratch, and FSE working tables | Reuse is already implemented; `fse_normalize` still allocates temporary remainders |

Normal matches encode a length symbol, length extras, a distance symbol and
distance extras. Repeat matches encode a repeat symbol, a **five-bit raw length
index**, and length extras. There is no separately entropy-coded repeat-length
stream in the current format.

Some comments and documents describe older behavior. In particular:

- The current parallel default is **two blocks per group**, not four.
- `fluff_fast_compress_blocks_with_history` already preloads preceding history
  for contiguous inputs. The archive layer passes that history. Groups do not
  necessarily discard the dictionary; cold groups use the reset form.
- The comment above `fluff_compress_block_ctx` describes the old fixed 694-byte
  header. The implementation writes variable-sized compact count tables and a
  ten-byte scalar tail after its four-byte stream prefix.
- Three repeat distances, compact headers, lazy matching, cached scratch and
  SIMD comparison in modern builds are existing features, not new proposals.

Use the implementation and [format specification](specification-1.0.md) when
evaluating compatibility, and reconcile stale descriptive text during follow-up.

## 2. Measurements from this review

A temporary C harness compressed all twelve local Silesia files in 256 KiB
blocks with persistent per-file contexts, explicit **level 8**, and the portable
core path. It decoded every block using a separate decoder context and compared
the output byte for byte. Files start with fresh contexts. There were 815 blocks
and 211,938,580 input bytes.

The compiler was Clang 19.1.7 on an Intel Core i5-10500T. The analysis build used
`-ansi -pedantic -O3 -march=native -DFLUFF_STATS`, without
`BOLTPACK_FAST_FLUFF`. These are operation counts and byte sizes, **not timing
measurements**. They exclude archive metadata and the archive's stored fallback.

A second build without `FLUFF_STATS` also round-tripped every block and matched
all per-file encoded fingerprints, token counts and byte totals. The separate
histogram experiment below passed the same roundtrip checks.

| Measurement | Result |
| --- | ---: |
| Encoded bytes | 64,342,839 |
| Encoded / input | 30.3592% |
| Headers, including tables and states/lengths | 212,562 bytes; 0.3304% of encoded size |
| Lit/len FSE stream | 34,902,371 bytes |
| Other stream, including distance FSE and raw extras | 29,227,906 bytes |
| Emitted literals | 25,436,017 |
| Emitted matches | 15,073,167 |
| Repeat matches | 2,043,483; 13.56% of matches |
| Length-three / length-four matches | 6.79% / 10.37% of matches |
| Matches at the 258-byte limit | 44,689; 0.30% of matches, covering 5.44% of input |
| Match-finder searches | 55,079,630 |
| Main hash-chain probes | 2,163,955,092 |
| Chain probes attributed to discarded lookahead | 53.90% |
| Chain probes attributed to accepted normal matches | 36.32% |

Header plus the two stream totals reconcile exactly to the encoded total.
Discarded-lookahead probes are searches whose candidate the parser did not
commit. They are not automatically removable: testing the alternative can be
necessary to choose the current match. These counters also do not measure CPU
time, and main-chain probes exclude separate sub-hash and repeat comparisons.

The corpus suggests three priorities: improve decisions before adding search;
investigate short matches, which are common; and avoid leading with another
header redesign. Small-file workloads may have very different header overhead.

### A small tested experiment: remove unnecessary distance pseudocounts

In `fluff_compress_block_ctx`, the encoder sets `d_freq[0] = 1` whenever distance
symbol zero is unused, even if other distance symbols are present. Only a wholly
empty distance histogram needs a dummy symbol to build a valid table.

A private copy changed this to add the dummy only when the sum of all distance
frequencies is zero. The parse was unchanged. Of the 815 blocks, 263 had real
distance symbols but no use of symbol zero.

| Variant | Encoded bytes | Change |
| --- | ---: | ---: |
| Current code | 64,342,839 | — |
| Dummy only for an empty histogram | 64,342,089 | −750 bytes; −0.00117% |

All variant blocks decoded correctly: seven files became smaller, two larger,
and three were unchanged. `dickens` grew by 138 bytes and `reymont` by 40. FSE
normalization and state coding mean removing an unnecessary count is not a
guarantee of smaller output on every input.

This is a small cleanup candidate, not a meaningful answer to the ratio goal.
It adds a bounded histogram scan; its speed effect was not benchmarked.

### Reproduction artifacts

The local, Git-ignored directory
[`build/fluff-review-2026-10-15`](../build/fluff-review-2026-10-15/) contains
`analyze.c`, per-file CSVs, the private variant and `manifest.json` with source
and corpus SHA-256 hashes. These artifacts are local to this review and are not
part of a fresh checkout. The production `fluff.c` SHA-256 was:

```text
b0884d8fae7346a556807899152279708e36c78f3e0ad8db319cc24676a0e532
```

The harness examines emitted scratch tokens, parses compact headers, checks byte
accounting, and verifies each block. Re-run it locally from the repository root:

```sh
cc -ansi -pedantic -O3 -march=native -DFLUFF_STATS -Isrc/core \
  build/fluff-review-2026-10-15/analyze.c \
  -o build/fluff-review-2026-10-15/analyze
build/fluff-review-2026-10-15/analyze 8 test/corpus/silesia/* \
  > build/fluff-review-2026-10-15/level8.csv
```

For ongoing development, move a cleaned-up harness and result manifest into
version control; do not make long-term validation depend on ignored artifacts.

## 3. Recommended experiments, in order

### 1. Use a better cost model without another match-finding pass

Target `fl_match_savings_est`, `fl_apply_rep_ring` and `fl_parse_greedy`.
Currently a three-byte match is valued against 24 bits of literals, even if
those literals are common and much cheaper under the block's FSE table.
Conversely, fixed normal-match overhead can reject a profitable token when its
length and distance symbols are common. All three repeat symbols get the same
estimated cost despite potentially different frequencies.

Build small fixed-point cost tables and use approximately:

```text
normal_cost = cost_ll[length_symbol] + length_extra_bits
            + cost_dist[distance_symbol] + distance_extra_bits
repeat_cost = cost_ll[repeat_symbol] + 5 + length_extra_bits
gain        = estimated_cost_of_replaced_literals - match_cost
```

An entropy estimate such as `-log2(probability)` is an approximation to FSE's
state-dependent cost, not an exact byte prediction. Actual output size is the
acceptance criterion.

Start with a cheap calibrated model or a current-block byte histogram for
literal estimates. Compare that with smoothed counts from already emitted
tokens, refreshed infrequently. Previous-block tables are another experiment,
but need a defined initial model at file and parallel-group boundaries. Reset
model state explicitly so worker scheduling cannot leak prior files' statistics.

Use integer table lookups in the hot path; do not call logarithms per candidate.
Begin with short-match decisions, where inaccurate costs matter most. A full
per-byte prefix sum makes literal-span costs cheap but costs memory and a pass;
measure that separately rather than assuming it is free. Preserve finite costs
for unseen symbols and avoid unsigned underflow when a match is unprofitable.

Lazy choices should compare paths covering the same input interval, accounting
for leading literals, each token and unmatched trailing bytes. Audit the current
extra `+8` margin in that model. Simply deleting the margin is a different
heuristic, not a demonstrated correction.

**Expected tradeoff:** limited arithmetic and lookup overhead, unchanged search
depth and format. This has the strongest rationale for the first substantive
ratio experiment. A full two-pass reparse is a later, higher-cost alternative.

### 2. Preserve one cheap alternative to the longest match

Target `fl_find_match_hyb`. The chain updates its result when length increases;
the sub-hash wins only when it exceeds the main result's length. The short-match
distance filter then rejects the selected result after search.

This can discard a nearby three-byte match when a distant four-byte match wins
on length and is subsequently filtered out. It can also discard a slightly
shorter match whose cheaper distance makes it preferable. Repeat candidates are
scored later, but ordinary alternative distances have already been lost.

First preserve the sub-hash result as an independent fallback, apply profitability
rules to candidates, and retain at most two useful choices: longest and cheap
nearby. Rank them with the same cost model as repeats and lazy decisions. Keep
the chain cap unchanged. Test this independently with the old model before
combining it with experiment 1.

The quick rejection at `cur_p[best_len]` only helps find matches that beat the
current length threshold. A general cost-based search cannot recover shorter
candidates behind that rejection. Start with already compared candidates and the
sub-hash; weakening rejection throughout the chain could be expensive.

**Expected tradeoff:** a small amount of candidate bookkeeping for the bounded
version. Ratio benefit and the cost of more full comparisons require measurement.

### 3. Spend lookahead work selectively

Target `fl_parse_greedy` and `FLUFF_GOOD_MATCH`. Given the measured discarded
probe share, do not add unconditional third-position lookahead or deeper chains.
Instead, evaluate a shallow first probe, then allow deeper work only when its
estimated gain makes a different decision plausible. Experiment with separate
limits for the main search and speculative search.

Any search saving can fund better scoring or a small bounded search at ambiguous
positions. Keep the 64-byte early exit as a baseline; test changes to it separately
from chain-depth changes. Evaluate the complete ratio/time tradeoff: reducing
search alone may make compression faster and worse.

**Expected tradeoff:** potentially enough saved time to pay for experiments 1–2,
but heuristic gating can miss useful alternatives. Probe counts guide the
experiment; release-build timings decide it.

### 4. Extend matches backwards through pending literals

The current parser emits literal tokens immediately and does not perform the
backward extension used in BoltLZ. A discovered match can sometimes cover one or
more preceding literals at the same distance, reducing token overhead.

Prototype a small bounded extension through the immediately preceding literal
run. Remove the replaced literal tokens and histogram counts consistently;
respect block start, source history availability and the 258-byte length limit.
Do not cross an already committed match or change repeat-ring semantics. Hash
insertion must remain monotonic rather than reinserting positions and creating
bad chain links. Validate that the extended token's length-code cost still wins.

**Expected tradeoff:** no additional chain search, but more parser bookkeeping.
This is format-compatible and worth testing after the simpler candidate work.

### 5. Improve normalization only if measurements justify it

`fse_normalize` assigns at least one slot per active symbol, distributes slack by
largest remainder, and removes excess from the largest allocations. An
alternative allocation based on marginal entropy cost could improve coding for
skewed histograms without changing table sizes or the format.

Keep this lower priority. Account for serialized count-table bytes as well as
payload size; a theoretically better histogram can still yield a larger block.
Moving the temporary remainder array into reusable context scratch is a separate
speed experiment, especially for small blocks. The measured pseudocount change
shows how small entropy-table cleanups can be in practice.

## 4. Changes to defer

| Proposal | Why it is not the first step |
| --- | --- |
| Raise every level's chain depth | Extra pointer chasing and comparisons; does not fix bad scoring or all early exits |
| Full optimal parsing or repeated reparsing | More candidate storage, CPU time and repeat-state complexity; consider an opt-in high level only after establishing the speed budget |
| Larger history, longer matches, new literal contexts or entropy-table reuse | Needs a separately specified compatible encoding or a new stream version and decoder work; changes memory and validation requirements |
| Preserve repeat distances across blocks | Encoder and decoder currently reset them per block; carrying them silently would break decoding |
| Entropy-code repeat lengths in the existing lit/len table | Source comments report a previous net-negative experiment; retrieve its evidence before repeating it. A separate alphabet is a distinct format experiment |
| Enlarge every parallel group | Warm history already exists; larger groups reduce available parallelism and may only amortize preload overhead |
| Another compact-header redesign | Only 0.33% header share on this large-file baseline; measure many-small-file data first |

Minor encoder opportunities include the unsearchable absolute position zero
(zero is also the empty hash sentinel), and ordinary three-byte tail matches
skipped by the `src_rem < 4` return. Their likely scope is small. Any position
representation change also needs a careful 32-bit-wrap audit; avoid mixing it
into a cost-model experiment.

## 5. Make the development loop reliable first

Existing tools provide a start, but their limits matter:

- `make parse-dump` builds a working CSV token trace. It uses a persistent context
  and 256 KiB blocks, but does not expose a level option, rejected candidates,
  individual repeat slots or complete encoded bit accounting.
- Detailed search getters require `FLUFF_STATS`; normal builds leave those
  detailed counters zero. Literal and match totals are maintained separately.
  Repeat probe/win statistics include speculative evaluation, so do not equate
  them with emitted repeat-token counts. Count emitted tokens directly.
- The `*_ctx_trace` wrappers currently ignore their trace argument. The presence
  of JSON infrastructure does not mean codec events are actually wired up.
- `analyze_bitstream.py` describes a historical Huffman layout and old alphabets.
  It is not a current `0x08` FSE analyzer.
- Linux command `T` / `debug-fluff` concatenates compressed blocks without retaining
  their sizes, reuses the encoding context to verify, and breaks after one decode
  call. Repair it before trusting it for multi-block roundtrips or profiling.
- `test_compress.c` directly tests BoltLZ and BoltLite. Fluff has archive-level
  tests and CLI integration checks, including a two-thread grouped roundtrip,
  but no equivalent dedicated direct-codec test file in this snapshot.
- `make fuzz` references the absent `test/test_fuzz.py`. The existing BoltLZ
  fuzzer does not exercise Fluff.
- `bench_corpus.py` measures whole archive creation and integrity testing against
  ZIP and between portable/modern builds. Its default build path removes
  `build/linux`; use prebuilt isolated binaries. It lacks the explicit level,
  per-file paired A/B analysis and codec-only timing needed for parser tuning.
  Also, `--threads 1` does not pass a disabling switch to the modern CLI, so it
  is not a reliable way to force sequential Fluff with that script.

### Build a small Fluff-specific benchmark and test harness

Adapt the structure of [bench_boltlz.c](../test/bench_boltlz.c) and
[bench_boltlz.py](../test/bench_boltlz.py), retaining Fluff's stateful behavior.
Use explicit encoder/decoder contexts and retain every block's compressed and
uncompressed size. Decode with a fresh decoder context for every file, feed raw
blocks when stored fallback is used, and compare lengths and bytes.

Support portable sequential, modern sequential, and modern grouped compression
as distinct cases. Modern SIMD performance must not be confused with threading
or parser quality. Include group sizes 1, 2, 4 and 8 and several thread counts;
verify real parallel execution rather than accepting an ignored CLI option.

Emit machine-readable per-file results with source/binary hashes, compiler and
flags, CPU, corpus hashes, level, block size, group size, threads, raw encoded
bytes, actual archive bytes, encode/decode times, peak memory and verification
status. Keep instrumented and release builds separate. Count candidate outcomes,
emitted token types, header bytes, distance bits and length bits in diagnostic
builds; measure time in builds without tracing or detailed statistics.

### Run controlled, paired experiments

1. Freeze baseline binaries and a manifest. Build each candidate in its own
   directory; keep production defaults unchanged until an experiment passes.
2. Start at levels 1, 5 and 8, then check level 9 and all remaining levels for
   correctness. Use 256 KiB blocks first, then smaller supported archive sizes.
3. Change one mechanism at a time. Run cheap direct correctness checks before
   the corpus sweep. Keep rejected experiments and their results reproducible.
4. Preload inputs outside timed codec loops. Report context allocation/reset
   separately and include them in small-file and end-to-end measurements.
5. Warm up, then alternate baseline/candidate order for at least five paired
   repetitions, with each timed sample long enough to avoid timer noise. Do not
   run competing benchmark jobs. Pin sequential work where supported and record
   frequency policy. Report medians and variation; repeat only when noise
   prevents a decision.
6. Compare `sum(encoded_bytes) / sum(input_bytes)` and throughput based on total
   bytes / total elapsed time. Report each file and workload category as well;
   an average of ratios can hide large-file regressions.
7. Retain candidates on the measured size/time frontier. For a moderately
   complex default change, a provisional target of at least 0.5% fewer encoded
   bytes within the 3% time budget is reasonable. Smaller near-free fixes may
   still be worthwhile. Check decoding, memory and small-file latency before
   accepting either class.
8. Combine individually successful changes, then rerun the combination: model,
   candidate selection and lazy decisions interact. Finally evaluate an untouched
   holdout corpus; do not keep tuning against the holdout.

Use Silesia as one development set, plus source, logs, JSON/XML, executables,
database-like records, many small files, random/already-compressed data and
abruptly changing data distributions. Keep file boundaries intact. Synthetic
long runs, repeated records and near-matches explain behavior but must not
dominate the headline score. The existing Canterbury/Calgary holdout material is
useful, but shared files or distributions already used for tuning should not be
presented as independent evidence.

## 6. Correctness and compatibility gates

Add direct tests alongside each algorithm experiment, targeting its failure
modes rather than asserting the implementation's internal steps.

| Test family | Important cases |
| --- | --- |
| Length and buffer boundaries | Empty-input API behavior; 1–4 bytes; 63/64; 257/258/259; supported block sizes and their tails; exact documented output allocation and canaries |
| Match selection | Near short versus distant longer match; filtered winner with viable fallback; equal-cost ties; each length/distance code boundary; all repeat slots and rotations |
| Lazy and backward decisions | Literal then better match; second-lookahead decision; discarded probes; extension through literals at block/history boundaries; no extension through a committed match |
| History | One file over many blocks; references crossing history/current input; wrap of the window; compression followed by stored fallback and compression again |
| Parallel history | Warm/cold groups, contiguous/noncontiguous sources, short last group, batch boundaries, reset marker followed by ordinary blocks, cold group whose first block is incompressible |
| Long positions | Streaming beyond 4 GiB and near-wrap cases through an internal test hook, auditing absolute comparisons, sentinel handling and counters |
| Decoder robustness | Truncated tables and streams, bad count sums, impossible symbols/distances, invalid repeat use, wrong lengths, reset variants and legacy versions |
| Portability | GCC and Clang; C89 core checks; modern C11/OpenMP build; scalar/SIMD paths; 32-bit and big-endian targets where available |

For encoder-only changes, a frozen **baseline decoder must read candidate
output**, and the candidate decoder must still read baseline/legacy fixtures.
Use both directions even when decoding code was intended to stay unchanged.
Bit-identical compressed output is not required for a better parser. For changes
that claim only speed improvements, require bit-identical output where that is
part of their design. Repeat runs at fixed settings must not depend on worker
scheduling.

Add a Fluff decoder fuzz target with ASan/UBSan, seeded with valid `0x06`,
`0x07`, `0x87`, `0x08` and `0x88` streams. Also fuzz sequences of compressed,
stored and reset blocks with bounded output. Maintain a separate valid-input
encode/decode property test. Malformed input must fail safely; a mutated valid
stream need not always fail because checksums are provided by the archive layer.

Existing checks remain useful after direct codec tests. From the repository root:

```sh
make modern
python3 test/check_boltpack.py --boltpack build/linux/boltpack
make unit-test
make unit-test-asan
make parse-dump
```

These commands are a follow-up validation recipe, not a claim that this review
ran the entire suite. For the review itself, corpus blocks were decoded and
byte-checked in the dedicated local harness. Release timing, parallel stress,
cross-platform checks and a Fluff fuzz campaign remain future work.

## 7. Suggested implementation sequence

First make direct roundtrip tests and paired benchmarking dependable, repairing
the multi-block debug verifier and adding current-format accounting. Next test
the cost model and bounded alternative candidate independently, then together.
Only add selective search or backward extension when measurements show remaining
opportunity. Complete holdout and compatibility validation before changing a
default. Keep larger format changes as a separate project with an explicit
ratio, decode-speed and memory justification.
