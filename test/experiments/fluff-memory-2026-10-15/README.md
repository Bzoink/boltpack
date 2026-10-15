# Fluff allocation experiment, 2026-10-15

The retained buffer changes are in `src/core/fluff.c`. `allocations.c` counts
requested codec bytes, including transient table-building allocations, and
checks that every allocation is freed. It excludes caller buffers, bookkeeping,
stack, allocator overhead and process RSS. The growth case deliberately grows
scratch from a 262,143-byte block to 262,144 bytes, measuring the temporary
overlap while preserving the old allocation until growth succeeds.

`retained.patch` applies to the shifted-ANS codec before the memory work
(SHA-256 `f52cd6e1abe45314d2ddbf455255255fdc1efa6678056c7c22132826233df76717e`).
Reverse the patch to reconstruct that baseline from the retained source:

```sh
mkdir -p build/fluff-memory-repro/baseline
cp src/core/fluff.c build/fluff-memory-repro/baseline/fluff.c
patch -R build/fluff-memory-repro/baseline/fluff.c \
  < test/experiments/fluff-memory-2026-10-15/retained.patch
clang -ansi -pedantic -O2 -DFLUFF_ACCOUNT_LEGACY_CALLOC \
  -Ibuild/fluff-memory-repro/baseline -Isrc/core \
  test/experiments/fluff-memory-2026-10-15/allocations.c \
  -o build/fluff-memory-repro/baseline/allocations
build/fluff-memory-repro/baseline/allocations
clang -ansi -pedantic -O2 -Isrc/core \
  test/experiments/fluff-memory-2026-10-15/allocations.c \
  -o build/fluff-memory-repro/allocations
build/fluff-memory-repro/allocations
python3 test/bench_fluff.py \
  --baseline-source build/fluff-memory-repro/baseline/fluff.c \
  --output build/fluff-memory-repro/paired --cc clang \
  --levels 1,8 --pairs 3 test/corpus/silesia/*
```

The baseline needs `FLUFF_ACCOUNT_LEGACY_CALLOC` because its match-table calloc
bypasses the allocator hook. The retained source fixes that ownership mismatch.
For the modern SIMD benchmark, use a separate output directory with
`--cc gcc --modern --levels 8 --pairs 3`.

`decode.c` repeats decoding within each timed sample to reduce the effect of
short scheduler interruptions. It reuses declarations from `test/bench_fluff.c`,
encodes each file once outside timing, and verifies with both decoders. For
example, after a modern paired build in `build/fluff-memory-repro/modern`:

```sh
gcc -std=c11 -pedantic -O3 -march=native -Isrc/core \
  -DBOLTPACK_FAST_FLUFF -fopenmp -DFLUFF_BENCH_PAIRED \
  test/experiments/fluff-memory-2026-10-15/decode.c \
  build/fluff-memory-repro/modern/baseline.o \
  build/fluff-memory-repro/modern/candidate.o src/core/fluff_fast.c \
  -o build/fluff-memory-repro/decode
taskset -c 8 build/fluff-memory-repro/decode 10 5 test/corpus/silesia/*
```

Additional match-table experiments independently halve `FLUFF_HASH_BITS` from
17 to 16, `FLUFF_H3_BITS` from 16 to 15, or both. They keep the 256 KiB history
and chain allocation. These table changes can alter parsing; the retained
scratch-buffer changes preserve the encoded bytes.

The paired follow-up for both smaller tables can be reproduced with:

```sh
python3 - <<'PY'
from pathlib import Path
s = Path('src/core/fluff.c').read_text()
s = s.replace('#define FLUFF_HASH_BITS      17', '#define FLUFF_HASH_BITS      16')
s = s.replace('#define FLUFF_H3_BITS        16', '#define FLUFF_H3_BITS        15')
Path('build/fluff-memory-repro/half.c').write_text(s)
PY
python3 test/bench_fluff.py --baseline-source src/core/fluff.c \
  --candidate-source build/fluff-memory-repro/half.c \
  --output build/fluff-memory-repro/half --cc clang --cpu 8 \
  --levels 1,8 --pairs 2 test/corpus/silesia/*
```

For the targeted holdout timing recheck, use the reconstructed pre-memory
baseline with the current candidate, `--levels 1,8 --pairs 15 --cpu 8`, and
these three inputs under `test/corpus/boltlz-holdout-2026-10-14/`:
`large-fixed-3/world192.txt`, `calgary/obj2`, and `canterbury/grammar.lsp`.
