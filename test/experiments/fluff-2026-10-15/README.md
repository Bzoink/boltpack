# Fluff compression experiments

These patches reproduce the isolated, rejected prototypes and the first ANS
state-layout prototype. They are benchmark inputs, not codec configurations.
Start each patch from `fluff.c` at revision
`f627bac5e77da844a863a99a41bdfd8eb3c3d0ee` (SHA-256
`b0884d8fae7346a556807899152279708e36c78f3e0ad8db319cc24676a0e532`).

```sh
mkdir -p build/fluff-prototype
git show f627bac5:src/core/fluff.c > build/fluff-prototype/fluff.c
patch -d build/fluff-prototype -p1 < test/experiments/fluff-2026-10-15/backward.patch
clang -ansi -pedantic -O3 -march=native -Isrc/core test/bench_fluff.c \
  build/fluff-prototype/fluff.c -o build/fluff-prototype/bench
build/fluff-prototype/bench 8 1 262144 test/corpus/silesia/*
```

Every prototype was tested with its own matching in-memory decoder. Format
prototypes reuse the old version byte in isolation, so they must be encoded
and decoded by the same prototype. They do not represent supported archive
formats. The retained implementation uses distinct `0x09`/`0x89` versions and
preserves legacy decoding.

`screening.json` records exact sizes and source hashes for twelve Silesia files,
256 KiB blocks, persistent per-file contexts, and level 8. Screening timings
were not paired; use the final paired measurements linked in
[the report](../../../docs/fluff-experiments-2026-10-15.md) for speed claims.

The variants change one mechanism at a time:

| Patch | Mechanism |
| --- | --- |
| `lazy0` | Remove the lazy parser's extra eight-bit margin |
| `fixed6`, `fixed7` | Estimate literals at six or seven bits |
| `cost14`, `cost22` | Change normal match overhead from 18 bits |
| `fallback` | Retain the independently found nearby sub-hash match |
| `backward` | Extend a match backwards over preceding literal tokens |
| `adaptive` | Block literal histogram/prefix costs and periodically refreshed symbol costs |
| `long8`, `long10`, `long12` | Last length code starts at 259 with 8/10/12 extra bits |
| `ll9`, `ll11`, `ll12` | Change literal/length table log from 10 |
| `dist10`, `ll11dist10` | Increase distance table log from 9, alone or with LL log 11 |
| `marginal` | Allocate normalized counts by an integer marginal-cost approximation |
| `rep-fse` | Separate 256-state FSE table for repeat-length indices |
| `ans-state` | Shift ANS virtual states to `[L, 2L)`; unoptimized first prototype |

None of these parser, alphabet, or table-size changes is combined with the
retained state-layout change.
