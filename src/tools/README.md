# BoltPack Development Tools

These helpers inspect codec decisions and Classic Mac resource forks. Run
commands below from the repository root. They are developer tools; the archive
reader and codec tests remain the authority for validation.

## Current inspection tools

### Fluff parser dump

```sh
make parse-dump
build/linux/fluff_parse_dump input.bin build/fluff-parse.csv
```

The C89 tool emits CSV parser decisions for 256 KiB blocks, including literals,
matches and repeat matches. It describes the LZ77 parse, not the complete FSE
bitstream or encoded-bit cost. Build outputs follow the selected `BUILDDIR`.

### Classic Mac resource forks

```sh
python3 src/tools/inspect_rsrc.py path/to/file.rsrc
```

This standalone inspector lists resource types, IDs, names, attributes and data
sizes from an extracted Classic Mac resource fork. It does not extract `.bolt`
archives or qualify resource restoration on a Mac.

## Benchmarks and codec checks

Use the maintained benchmark drivers under `test/`:

- [`test/bench_fluff.py`](../../test/bench_fluff.py) compares isolated Fluff
  baseline/candidate sources with alternating, verified measurements.
- [`test/bench_boltlz.py`](../../test/bench_boltlz.py) compares BoltLZ modes,
  frozen checkpoints and pinned LZ4.
- [`test/bench_corpus.py`](../../test/bench_corpus.py) measures archive-level
  operations; these include costs excluded from direct codec timing.
- `make unit-test` and `make unit-test-asan` exercise direct codec and format
  checks. See [codec validation](../../docs/algorithms.md#8-validation).

The Fluff benchmark's modern mode enables SIMD with sequential encoding;
threaded archive behavior requires separate CLI integration checks.

## Historical analysis helpers

The following Python helpers remain in this directory, but their output is
not evidence of compatibility with current format-6 archives or Fluff
`0x09`/`0x89` streams:

| Tool | Limitation |
| --- | --- |
| `analyze_bitstream.py` | Parses a historical Huffman layout rather than the current FSE representation. |
| `extract_fluff_block.py` | Uses historical archive-header assumptions; it is not the current archive reader or an integrity checker. |
| `analyze_corpus.py` | Uses the historical bitstream analyzer/extractor for its structural statistics. Use the maintained benchmarks for current codec measurements. |
| `probe_improvements.py` | Exploratory chain-depth and token-cost analyses; not a reproducible current-code performance gate. |
| `fluff_db_analyzer.py` | Queries a prototype SQLite `parse_trace` schema; current codec trace wrappers do not populate that database. |

For exact format rules, use the
[specification](../../docs/specification-1.0.md). Current implementation details
are in [algorithms.md](../../docs/algorithms.md); measured experiments and their
retained artifacts are indexed in [the documentation guide](../../docs/README.md).
