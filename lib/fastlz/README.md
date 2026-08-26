# fastlz

Byte-aligned LZ77 compression library.

- Upstream: https://github.com/ariya/FastLZ
- Version: 0.5.0 (FASTLZ_VERSION 0x000500)
- License: MIT (see `LICENSE`)

BoltPack vendors a C89 port of upstream `fastlz.c` so it compiles under the
project's strict ANSI C89 core rules (`-ansi -pedantic`). The port changes
(`<stdint.h>` replaced with local typedefs, declarations hoisted to block
tops) are documented in the header of `fastlz.c` itself so the diff against
upstream is reviewable.
