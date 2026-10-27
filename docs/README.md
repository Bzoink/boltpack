# BoltPack Documentation

BoltPack is a lightweight file archiver designed for maximum portability across
vintage and modern systems. This directory holds a small set of canonical
product documents plus a narrow set of support guides.

Start with `architecture.md` for the layer model, or `specification-1.0.md` when
you need exact format rules.

## Quick Start

```sh
make
./build/linux/boltpack c archive.bolt myfiles/
./build/linux/boltpack x archive.bolt
```

## Canonical Docs

| File                   | Purpose                                                           |
| ---------------------- | ----------------------------------------------------------------- |
| `architecture.md`      | Layer boundaries, core interfaces, progress model, and error flow |
| `specification-1.0.md` | Normative archive and codec rules                                 |
| `format.md`            | Concise overview of the `.bolt` / `.blt` file format              |
| `archive-format-audit.md` | Portability and filesystem-attribute preservation audit         |
| `algorithms.md`        | BoltLite, boltlz, and Fluff implementation behavior                   |
| `portability.md`       | Supported environments, C89 constraints, and toolchain reference  |
| `frontends.md`         | Frontend responsibilities and platform-specific integration notes |

## Support Docs

| File                   | Purpose                                                |
| ---------------------- | ------------------------------------------------------ |
| `BUILD.md`             | Native build procedures and platform-specific caveats  |
| `mini.md`              | 16-bit mini variant architecture and capability limits |
| [archive-format-plan.md](archive-format-plan.md) | Format 6 work plan and remaining interoperability tasks |
| [interoperability-matrix.md](interoperability-matrix.md) | Per-frontend capabilities and validation evidence |
| `DEPENDENCY-CHECKS.md` | Build and packaging dependency-check behavior          |
| `RELEASES.md`          | Release workflow (maintainer reference)                |

## Codec Development and Evaluations

| File                                                               | Purpose                                                                             |
| ------------------------------------------------------------------ | ----------------------------------------------------------------------------------- |
| [boltlz-design.md](boltlz-design.md)                               | BoltLZ representation, token layout, and decoder constraints                       |
| [boltlz-development.md](boltlz-development.md)                     | BoltLZ implementation, benchmarks, and reproducible validation commands             |
| [boltlz-validation-2026-10-14.md](boltlz-validation-2026-10-14.md) | Independent corpus evaluation and full-block robustness checks                      |
| [boltlite-evaluation-2026-10-14.md](boltlite-evaluation-2026-10-14.md) | BoltLite format-5 evaluation and measured tradeoffs                                  |
| [fluff-compression-review.md](fluff-compression-review.md)         | Pre-v1.8 Fluff ratio review, historical baseline, speed budget, and experiment plan |
| [fluff-experiments-2026-10-15.md](fluff-experiments-2026-10-15.md) | Measured Fluff ratio experiments and reproduction commands                           |
| [fluff-memory-2026-10-15.md](fluff-memory-2026-10-15.md)           | Fluff allocation accounting, buffer reuse, and memory/speed measurements             |

## Elsewhere in the Repository

| File                  | Purpose                                          |
| --------------------- | ------------------------------------------------ |
| `../README.md`        | Project overview, downloads, and build summary   |
| `../CONTRIBUTING.md`  | Build and test steps, C89 rules, PR expectations |
| `../SECURITY.md`      | How to report a vulnerability                    |
| `../RELEASE-NOTES.md` | What changed in each version                     |
| `../CONTRIBUTORS.md`  | Project author and third-party attributions      |

## Repository Landmarks

| Path               | Purpose                                         |
| ------------------ | ----------------------------------------------- |
| `../src/core/`     | Portable archive core and codec implementations |
| `../src/frontend/` | Platform-specific frontends                     |
| `../test/`         | Unit tests, integration tests, and fixtures     |
| `../scripts/`      | Build, packaging, and release scripts           |

Use `specification-1.0.md` for exact rules, `algorithms.md` for current codec
behavior, and `frontends.md` or `portability.md` when working outward from the
core into platform-specific concerns.
