# Security Policy

## Reporting a Vulnerability

**Please do not open a public GitHub issue for a security vulnerability.**

Report it privately using either:

1. **GitHub private vulnerability reporting** — go to the
   [Security tab](https://github.com/Bzoink/boltpack/security/advisories/new)
   and open a draft advisory. This is preferred.
2. **Email** — <tony.mattsson@protonmail.com>, with `BoltPack security` in the
   subject line.

BoltPack is maintained by one person, so please allow a few days for an initial
reply. You will get an acknowledgement within **7 days**. If you have not heard
anything by then, feel free to send a follow-up.

Please include:

- A description of the issue and its impact
- The platform, build (CLI, GUI, mini, plugin), and BoltPack version
- A proof-of-concept archive or input file, if you have one
- Whether the issue is already public anywhere

## Scope

BoltPack parses untrusted input — archives you did not create — so the highest
priority issues are in the read path:

### In scope

- Memory-safety bugs in the archive parser or any decompressor (`archiver.c`,
  `compress.c`, `fluff.c`, `lzss.c`, `zstd_wrap.c`, and the `mini/` variants):
  buffer overflows, out-of-bounds reads, integer overflow leading to bad
  allocation or bad bounds, use-after-free
- Path traversal on extraction — an archive entry that writes outside the
  destination directory
- Denial of service from a small crafted archive: unbounded memory growth,
  non-terminating decompression loops
- Anything that lets a crafted archive cause code execution or unintended file
  writes

### Out of scope

- Bugs that require the attacker to already control the machine running BoltPack
- Crashes only reachable with a debug/sanitizer build and deliberately invalid
  API arguments from a caller you control
- Missing hardening flags in a build you configured yourself
- Vulnerabilities in vendored third-party code that are already public upstream
  — report those to miniLZO or zstd; do tell us so we can update the vendored
  copy
- Archive corruption that is detected and reported correctly (`BP_ERR_CHECKSUM`,
  `BP_ERR_CORRUPT`, `BP_ERR_TRUNCATED`) — that is the format working as designed

## Supported Versions

BoltPack is a small project with a single maintainer. Security fixes land in the
current release only; there are no backport branches.

| Version | Supported |
| ------- | --------- |
| 1.4     | ✅        |
| ≤ 1.3   | ❌        |

If you are running an older build, please update before reporting.

## Disclosure

Fixes are released as a new version with the issue described in
[RELEASE-NOTES.md](RELEASE-NOTES.md) and a GitHub Security Advisory. Reporters
are credited unless they ask not to be. Please give us a chance to ship a fix
before publishing details.

## A Note on Vintage Targets

BoltPack runs on platforms with no memory protection, no ASLR, and no modern
mitigations — MS-DOS, AmigaOS, Classic Mac OS, and the 16-bit `mini` builds.
Memory-safety bugs there are exploitable in ways they would not be on Linux, and
the constrained builds have smaller buffers and tighter limits. Reports that
only reproduce on a vintage or `mini` target are still in scope and still
valuable.
