# Vendored code in src/core/crypto

Nothing here is fetched at build time. Both trees are exact copies of upstream
files; they are not edited. To check: compare with the sources named below.

## monocypher/ (suite S1: XChaCha20-Poly1305, BLAKE2b, Argon2id)

| | |
| --- | --- |
| Project | Monocypher, https://monocypher.org/ (GitHub mirror: LoupVaillant/Monocypher) |
| Version | 4.0.3 (2026-06-15), git tag `4.0.3`, commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f` |
| Taken from | the release tarball `monocypher-4.0.3.tar.gz` (SHA-512 `40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965`, as published at https://monocypher.org/download/monocypher-4.0.3.tar.gz.sha512); `src/monocypher.c` and `.h` are identical to the git tag apart from the version line |
| Files | `monocypher.c`, `monocypher.h`, `LICENCE.md` |
| Licence | BSD-2-Clause or CC0-1.0, at your choice (see `LICENCE.md`) |
| Language | C99 |
| SHA-256 | `monocypher.c` `57eb914fc88136119bd41655cccb8c250048bf54d470540625186f8ab16f64be`, `monocypher.h` `c494da712122da7ff679fdcf318a5317e84972b6c950fe9d896212947797facd` |

Monocypher's own test suite (`tests/test.c`) was run on these files in five builds
and its `ctgrind` check under valgrind (plan section 19.1); that is not repeated
by `make crypto-test`.

## ascon-c/ (suite S2 reference: Ascon-AEAD128, SP 800-232)

| | |
| --- | --- |
| Project | ascon-c, https://github.com/ascon/ascon-c |
| Version | commit `446347f21b209f3921c65ece70027c366cbe1693` (2026-01-28, "update references to NIST SP 800-232 to final version") |
| Files | `ref/` (the AEAD128 reference: `aead.c`, `api.h`, `ascon.h`, `constants.h`, `permutations.h`, `printstate.[ch]`, `round.h`, `word.h`) and `crypto_aead.h` from `tests/`; `LICENSE` |
| Licence | CC0-1.0 |
| Language | C99 |
| Use | `bp_ascon_ref.c` (test reference only) includes the permutation, round, word and constant headers. `ref/aead.c` is not compiled into the library: it is the reference the C89 port is tested against. The NIST known-answer files are in `test/reference/encryption/` |

`ref_cxof/` holds ascon-c's Ascon-CXOF128 reference (`hash.c`, its `api.h`, and `crypto_hash.h` from `tests/`); the differential test compares the port's CXOF with it, and the NIST-format known-answer file is in `test/reference/encryption/`.

Since phase 1b the library backend is `bp_ascon_c89.c` (strict C89, 32-bit or 16-bit limbs);
`bp_ascon_ref.c` and the vendored `ref/aead.c` are compiled only into the differential test.
