# LZ4 benchmark reference

These files are unmodified upstream LZ4 1.10.0 sources from the
[v1.10.0 tag](https://github.com/lz4/lz4/tree/v1.10.0/lib).
`upstream.json` records their SHA-256 hashes and `LICENSE` preserves the
upstream BSD 2-Clause license. They are linked only into the experimental
benchmark, not the BoltPack archive implementation.

The benchmark measures `LZ4_compress_default`, and
`LZ4_compress_HC_extStateHC` at level 9 with preallocated state.
Both use `LZ4_decompress_safe` and independent blocks with the same boundaries
as BoltLZ. Each timed compressor call includes state initialization;
allocation, I/O, and round-trip verification are outside the timed loops.

LZ4 is compiled as C99 to enable its upstream inline paths. BoltLZ
retain the C89 build. The compiler and optimization flags are shared, including
the default `-O2`; reports record both sets of language flags.
