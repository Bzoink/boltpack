# BoltPack Contributors

## Project Author

**Tony Mattsson** — archive format design, archiver core, compression layer,
and all platform frontends.

## Third-Party Contributions

The following people's prior work is incorporated into BoltPack:

### Ariya Hidayat

**fastlz 0.5.0** — byte-aligned LZ77 compression library.
Vendored in `lib/fastlz/` (C89-ported for BoltPack's strict core rules).
Licensed under the MIT license.

- Home page: <https://github.com/ariya/FastLZ>
- Contact: <ariya.hidayat@gmail.com>

### Haruhiko Okumura

**LZSS algorithm** — the sliding-window LZ77 variant with binary search tree
described in Okumura (1989). BoltPack's `src/core/lzss.c` (method 1) is an
adaptation of that algorithm for the BoltPack archive format and token
encoding.

- Reference: H. Okumura, "Data Compression Program Source Code," 1989.
  Widely distributed as `lzhuf.c` and in the comp.compression FAQ.

### Jarek Duda

**Asymmetric Numeral Systems (ANS / tANS)** — the entropy coding theory
underlying the Finite State Entropy (FSE) component of the Fluff codec in
`src/core/fluff.c`.

- Reference: J. Duda, "Asymmetric numeral systems," arXiv:0902.0271, 2009.
  <https://arxiv.org/abs/0902.0271>

### Meta Platforms, Inc. / Yann Collet

**zstd 1.5.7** — Zstandard real-time data compression algorithm.
Vendored as a single-file amalgamation in `lib/zstd/`. Licensed under the
BSD 2-Clause "Simplified" License.

- Home page: <https://github.com/facebook/zstd>
- Reference: Y. Collet, "Zstandard Compression Algorithm," RFC 8878, 2021.
