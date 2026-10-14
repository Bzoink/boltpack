# BoltLite successor evaluation — 2026-10-14

## Result

The format-5 BoltLite codec is a worthwhile replacement for the format-4 LZSS
encoder. On the 28-file Calgary, Canterbury, and large-file holdout corpus, it
compresses 0.57% smaller overall, encodes about 1.8 times faster, and reduces
encoder workspace by 36.9%. Its decoder is about 12% slower on this host, so
the main confirmed gains are encoding speed, archive size, and scratch memory.

| Measure | Format 4 reference | Format 5 | Change |
| ------- | -----------------: | -------: | -----: |
| Input corpus | 17,111,888 B | 17,111,888 B | — |
| Encoded payload | 7,104,810 B | 7,064,416 B | −0.57% |
| Encoder workspace | 29,200 B | 18,432 B | −36.9% |
| Weighted encode throughput | 6.20 MiB/s | 11.27 MiB/s | 1.82× |
| Weighted decode throughput | 395.10 MiB/s | 349.16 MiB/s | −11.6% |
| Files with smaller payload | — | 13 of 28 | — |
| Largest per-file regression | — | +1.98% | — |

All files still clear the archive's 1% compression threshold, so effective
stored size equals the encoded payload total for this corpus.

## Method

The reference is the frozen predecessor LZSS implementation in
`test/reference/lzss_method1_v4.c`. The benchmark compares it directly with
the production codec through `test/bench_boltlite.c`. Files are split into 32 KiB
blocks, round-tripped after each encode, and timed for ten rounds. The
benchmark used Clang 19.1.7 with `-O2` on x86-64 Linux, on an Intel Core i5-10500T.
Throughput is weighted by total input bytes and excludes file I/O.

The corpus command used for the comparison was:

```sh
clang -O2 -ansi -pedantic -Wall -Wextra -Wno-unused-function \
  -Isrc/core -Itest -Itest/reference \
  test/bench_boltlite.c test/reference/lzss_method1_v4.c src/core/boltlite.c \
  -o build/BoltLite-experiment/bench_boltlite
build/BoltLite-experiment/bench_boltlite --rounds 10 \
  $(find test/corpus/boltlz-holdout-2026-10-14 -type f | sort)
```

## Format and implementation

The new stream preserves one flag byte per eight symbols and introduces three
match forms: one-byte short-distance matches, one-byte repeat-distance
matches, and two-byte explicit-distance matches. It uses a 4 KiB history and
supports matches through 66 bytes. The encoder replaces the old tree and ring
workspace with 1024 hash heads and two 4096-entry link arrays. Decoding refers
to prior output directly and needs no codec workspace.

On 16-bit mini builds, blocks are limited to 32 KiB and matcher indices use
native `unsigned int`, avoiding 32-bit arithmetic in the position and match
search loops. The mini test suite also passed with `SIZEOF_INT=2` selected on
the host compiler. This is a configuration-path check, not a measurement on
real 16-bit hardware or a 16-bit compiler; target throughput still needs
measurement on representative hardware.

The method-1 bitstream change is identified by archive format 5. Current
readers reject format 4, and previous BoltPack binaries remain available for
extracting those archives.
