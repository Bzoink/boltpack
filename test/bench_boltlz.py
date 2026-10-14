#!/usr/bin/env python3
"""Build, verify, and benchmark BoltLZ against LZ4 and frozen BoltLZ references.

Uses the Python standard library. Native code times codec loops; this driver
creates a reproducible development corpus and records raw measurements.
Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import platform
import random
import statistics
import subprocess
import sys
import time
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "boltlz"
CODECS = ["boltlz-fast", "boltlz-balanced",
          "boltlz-before-fast", "boltlz-before-balanced", "lz4", "lz4-hc-9",
          "boltlz-speed-before-fast", "boltlz-speed-before-balanced",
          "boltlz-review-before-fast", "boltlz-review-before-balanced",
          "boltlz-loop-before-fast", "boltlz-loop-before-balanced"]
DEFAULT_FLAGS = "-ansi -pedantic -Wall -Wextra -Wno-unused-function -O2"
SOURCES = (
    "Makefile",
    "src/core/boltlz.c", "src/core/boltlz.h", "src/core/boltlz_config.h", "src/core/bp_types.h",
    "test/bench_boltlz.c", "test/bench_boltlz.py",
    "test/reference/boltlz_v2.c", "test/reference/boltlz_v2.h",
    "test/reference/boltlz_speed_base.c", "test/reference/boltlz_speed_base.h",
    "test/reference/boltlz_speed_base_config.h", "test/reference/boltlz_speed_base.json",
    "test/reference/boltlz_review_base.c", "test/reference/boltlz_review_base.h",
    "test/reference/boltlz_review_base_config.h", "test/reference/boltlz_review_base.json",
    "test/reference/boltlz_loop_base.c", "test/reference/boltlz_loop_base.h",
    "test/reference/boltlz_loop_base_config.h", "test/reference/boltlz_loop_base.json",
    "test/reference/lz4/lz4.c", "test/reference/lz4/lz4.h",
    "test/reference/lz4/lz4hc.c", "test/reference/lz4/lz4hc.h",
    "test/reference/lz4/upstream.json",
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def lz4_flags(flags: str) -> str:
    # Match GNU make's filter-out, which operates on whitespace-separated
    # words without stripping the shell quoting recorded in the flag string.
    return " ".join(flag for flag in flags.split()
                    if flag not in ("-ansi", "-std=c89", "-std=c90")) + " -std=c99"


def development_corpus(destination: Path) -> list[dict]:
    """Development data, not a general-purpose release/holdout corpus."""
    destination.mkdir(parents=True, exist_ok=True)
    cases = []

    def add(name: str, category: str, data: bytes, origins: list[str], control=False):
        path = destination / name
        path.write_bytes(data)
        cases.append({"path": str(path), "name": name, "category": category,
                      "size": len(data), "sha256": sha256(data),
                      "origins": origins, "control": control, "split": "development"})

    sources = sorted((ROOT / "src" / "core").glob("*.c"))
    sources = [p for p in sources if p.name != "boltlz.c"]
    sources += [ROOT / "src/frontend/linux/main.c"]
    prose = [ROOT / "docs" / name for name in (
        "specification-1.0.md", "algorithms.md", "architecture.md", "portability.md")]
    source_data = b"\n".join(p.read_bytes() for p in sources)[:1024 * 1024]
    prose_data = b"\n".join(p.read_bytes() for p in prose)[:1024 * 1024]
    add("source.bin", "source", source_data, [str(p.relative_to(ROOT)) for p in sources])
    add("prose.bin", "prose", prose_data, [str(p.relative_to(ROOT)) for p in prose])
    executable = Path(sys.executable).resolve()
    add("executable.bin", "executable", executable.read_bytes()[:1024 * 1024],
        [str(executable) + " (first 1 MiB)"])
    rng = random.Random(20261013)
    rows = []
    logs = []
    records = bytearray()
    for i in range(6000):
        rows.append(json.dumps({"id": i, "user": f"user-{rng.randrange(400)}",
                                "type": ["read", "write", "list"][i % 3],
                                "path": f"/objects/{rng.randrange(120)}/{i % 17}",
                                "size": rng.randrange(100000), "ok": i % 97 != 0},
                               separators=(",", ":")))
        logs.append(f"2026-10-13T10:{i % 60:02d}:{i % 59:02d} host-{i % 7} "
                    f"GET /api/items/{rng.randrange(1000)} status={200 if i % 19 else 404} "
                    f"bytes={rng.randrange(90000)} duration={rng.randrange(500)}ms\n")
        records.extend((i % 400).to_bytes(4, "little"))
        records.extend((i * 137).to_bytes(4, "little"))
        records.extend(bytes(24) if i % 5 else rng.randbytes(24))
    add("records.jsonl", "structured", "\n".join(rows).encode(), ["generated structured records; seed 20261013"])
    add("requests.log", "logs", "".join(logs).encode(), ["generated logs; seed 20261013"])
    add("records.bin", "binary-records", bytes(records), ["generated binary records; seed 20261013"])
    random_data = rng.randbytes(1024 * 1024)
    add("random.bin", "incompressible", random_data, ["Python Random.randbytes; seed 20261013"])
    add("mixed.bin", "mixed", random_data[:262144] + source_data + random_data[262144:524288],
        ["random prefix and suffix around real source"])
    add("compressed.bin", "already-compressed", zlib.compress(source_data, 6),
        ["zlib level 6 of source.bin"])
    add("zeros.bin", "control", bytes(65536), ["zero run"], control=True)
    add("periodic.bin", "control", (b"abc" * 21846)[:65536], ["period-three run"], control=True)
    return cases


def external_corpus(directory: Path, split: str = "validation") -> list[dict]:
    if not directory.is_dir():
        raise ValueError(f"Corpus directory does not exist: {directory}")
    cases = []
    for path in sorted(directory.rglob("*")):
        if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
            continue
        if path.stat().st_size > 512 * 1024 * 1024:
            raise ValueError(f"File exceeds harness limit of 512 MiB: {path}")
        data = path.read_bytes()
        relative = str(path.relative_to(directory))
        cases.append({"path": str(path), "name": relative,
                      "category": relative.split("/")[0] if "/" in relative else relative,
                      "size": len(data), "sha256": sha256(data),
                      "origins": [str(path)], "control": False, "split": split})
    if not cases:
        raise ValueError("Corpus has no nonempty regular files")
    return cases


def summarize(samples: list[dict], cases: list[dict], block_size: int,
              codecs: list[str] | None = None) -> dict:
    selected = {case["name"] for case in cases if not case["control"]}
    result = {}
    for codec in codecs or CODECS:
        totals = {"input_bytes": 0, "encoded_bytes": 0, "effective_bytes": 0,
                  "encode_seconds": 0.0, "decode_seconds": 0.0}
        files = []
        for case in cases:
            rows = [s for s in samples if s["codec"] == codec and
                    s["file"] == case["name"] and s["block_size"] == block_size]
            if not rows:
                continue
            assert len({s["encoded_bytes"] for s in rows}) == 1, "unstable encoded size"
            encode_rates = [s["encode_bytes"] / s["encode_seconds"] for s in rows]
            decode_rates = [s["decode_bytes"] / s["decode_seconds"] for s in rows]
            encode_rate = statistics.median(encode_rates)
            decode_rate = statistics.median(decode_rates)
            file_result = {"file": case["name"], "category": case["category"],
                           "control": case["control"], "input_bytes": case["size"],
                           "encoded_bytes": rows[0]["encoded_bytes"],
                           "effective_bytes": rows[0]["effective_bytes"],
                           "encode_mib_s": encode_rate / 1048576,
                           "decode_mib_s": decode_rate / 1048576,
                           "encode_spread_pct": (max(encode_rates) - min(encode_rates)) / encode_rate * 100,
                           "decode_spread_pct": (max(decode_rates) - min(decode_rates)) / decode_rate * 100}
            if all(row.get("frozen_decode_seconds", 0) > 0 for row in rows):
                assert len({row.get("frozen_decoder", "speed") for row in rows}) == 1, "mixed decoder baselines"
                rates = [row["frozen_decode_bytes"] / row["frozen_decode_seconds"] for row in rows]
                frozen_rate = statistics.median(rates)
                file_result.update({"frozen_decode_mib_s": frozen_rate / 1048576,
                                    "frozen_decoder": rows[0].get("frozen_decoder", "speed"),
                                    "decoder_speedup": decode_rate / frozen_rate,
                                    "frozen_decode_spread_pct": (max(rates) - min(rates)) / frozen_rate * 100,
                                    "encoder_identical": all(row.get("encoder_identical", False) for row in rows)})
            files.append(file_result)
            if case["name"] in selected:
                for key in ("input_bytes", "encoded_bytes", "effective_bytes"):
                    totals[key] += file_result[key]
                totals["encode_seconds"] += case["size"] / encode_rate
                totals["decode_seconds"] += case["size"] / decode_rate
                if "frozen_decode_mib_s" in file_result:
                    totals["frozen_decode_seconds"] = totals.get("frozen_decode_seconds", 0) + case["size"] / frozen_rate
        totals["encode_mib_s"] = totals["input_bytes"] / totals["encode_seconds"] / 1048576
        totals["decode_mib_s"] = totals["input_bytes"] / totals["decode_seconds"] / 1048576
        if "frozen_decode_seconds" in totals:
            assert len({f["frozen_decoder"] for f in files}) == 1, "mixed decoder baselines"
            totals["frozen_decoder"] = files[0]["frozen_decoder"]
            totals["frozen_decode_mib_s"] = totals["input_bytes"] / totals["frozen_decode_seconds"] / 1048576
            totals["decoder_speedup"] = totals["decode_mib_s"] / totals["frozen_decode_mib_s"]
            totals["encoder_identical"] = all(f.get("encoder_identical", False) for f in files)
        result[codec] = {"aggregate": totals, "files": files}
    return result


def markdown_report(report: dict) -> str:
    lines = ["# Experimental BoltLZ benchmark", "",
             "Development/validation measurements, not a release performance claim.", "",
             f"CPU: {report['environment']['cpu']}; compiler: {report['environment']['compiler']}",
             f"Flags: `{report['environment']['flags']}`", "",
             f"LZ4 flags: `{report['environment'].get('lz4_flags', 'not recorded')}`", "",
             "Raw decoding is timed even when the archive would store a block. "
             "Synthetic controls are excluded from aggregate results. "
             "Rates summarize per-file medians by total bytes / total inferred time.", ""]
    for block_size, summary in report["summary"].items():
        lines.extend([f"## {int(block_size) // 1024} KiB blocks", "",
                      "| Codec | Raw fraction | Stored-fallback fraction | Encode MiB/s | Decode MiB/s | Size vs LZ4 | Encode vs LZ4 | Decode vs LZ4 |",
                      "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"])
        base = summary["lz4"]["aggregate"]
        for codec in summary:
            row = summary[codec]["aggregate"]
            lines.append(f"| {codec} | {row['encoded_bytes'] / row['input_bytes']:.2%} | "
                         f"{row['effective_bytes'] / row['input_bytes']:.2%} | "
                         f"{row['encode_mib_s']:.1f} | {row['decode_mib_s']:.1f} | "
                         f"{row['encoded_bytes'] / base['encoded_bytes'] - 1:+.2%} | "
                         f"{row['encode_mib_s'] / base['encode_mib_s']:.2f}x | "
                         f"{row['decode_mib_s'] / base['decode_mib_s']:.2f}x |")
        if any(c in summary and b in summary for c, b in zip(CODECS[0:2], CODECS[2:4])):
            lines.extend(["", "### Candidate versus frozen BoltLZ v2", "",
                          "| Mode | Size delta | Encoding speed | Decoding speed |",
                          "| --- | ---: | ---: | ---: |"])
            for codec, before in zip(CODECS[0:2], CODECS[2:4]):
                if codec not in summary or before not in summary:
                    continue
                row = summary[codec]["aggregate"]
                old = summary[before]["aggregate"]
                lines.append(f"| {codec} | {row['encoded_bytes'] / old['encoded_bytes'] - 1:+.2%} | "
                             f"{row['encode_mib_s'] / old['encode_mib_s']:.2f}x | "
                             f"{row['decode_mib_s'] / old['decode_mib_s']:.2f}x |")
        baseline = ("loop" if any(c.startswith("boltlz-loop-before-") for c in summary) else
                    "review" if any(c.startswith("boltlz-review-before-") for c in summary) else "speed")
        lines.extend(["", f"### Speed experiments versus frozen {baseline} baseline", "",
                      "Decoder speed compares identical compressed blocks; encoder identity is checked outside timing.", "",
                      "| Mode | Identical output | Encoding speed | Decoding speed |",
                      "| --- | --- | ---: | ---: |"])
        for mode in ("fast", "balanced"):
            current, before = f"boltlz-{mode}", f"boltlz-{baseline}-before-{mode}"
            if current not in summary or before not in summary:
                continue
            row, old = summary[current]["aggregate"], summary[before]["aggregate"]
            lines.append(f"| {mode} | {row.get('encoder_identical', False)} | "
                         f"{row['encode_mib_s'] / old['encode_mib_s']:.2f}x | "
                         f"{row.get('decoder_speedup', 1):.2f}x |")
        lines.extend(["", "### Candidate versus LZ4 default", "",
                      "| Mode | Size delta | Encoding speed | Decoding speed |",
                      "| --- | ---: | ---: | ---: |"])
        lz4 = summary["lz4"]["aggregate"]
        for codec in CODECS[0:2]:
            if codec not in summary:
                continue
            row = summary[codec]["aggregate"]
            lines.append(f"| {codec} | {row['encoded_bytes'] / lz4['encoded_bytes'] - 1:+.2%} | "
                         f"{row['encode_mib_s'] / lz4['encode_mib_s']:.2f}x | "
                         f"{row['decode_mib_s'] / lz4['decode_mib_s']:.2f}x |")
        for codec in CODECS[0:2]:
            if codec not in summary:
                continue
            lines.extend(["", f"### {codec}: per-file deltas against LZ4 default", "",
                          "| File | Control | Size delta | Encode speedup | Decode speedup | Encode spread | Decode spread |",
                          "| --- | --- | ---: | ---: | ---: | ---: | ---: |"])
            baseline_files = {f["file"]: f for f in summary["lz4"]["files"]}
            for row in summary[codec]["files"]:
                base_file = baseline_files[row["file"]]
                label = row["file"].replace("|", "\\|")
                lines.append(f"| {label} | {'yes' if row['control'] else 'no'} | "
                             f"{row['encoded_bytes'] / base_file['encoded_bytes'] - 1:+.2%} | "
                             f"{row['encode_mib_s'] / base_file['encode_mib_s']:.2f}x | "
                             f"{row['decode_mib_s'] / base_file['decode_mib_s']:.2f}x | "
                             f"{row['encode_spread_pct']:.1f}% | {row['decode_spread_pct']:.1f}% |")
        lines.append("")
    lines.extend(["## Limits", "",
                  "Single-threaded repeated in-memory passes with production bounds checks. "
                  "These are warm-cache measurements for inputs that fit in cache; "
                  "larger corpora rotate over their full input. Allocation, filesystem I/O, "
                  "checksums, archive headers, and process startup are excluded. "
                  "The automatic development corpus contains generated records and concatenated "
                  "repository text, so it is not a substitute for an untouched real-data holdout.", ""])
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, help="external corpus; otherwise create development data")
    parser.add_argument("--split", choices=("development", "validation", "holdout"),
                        help="record corpus purpose (does not guarantee an untouched holdout)")
    parser.add_argument("--block-sizes", default="32768", help="comma-separated sizes in bytes")
    parser.add_argument("--seconds", type=float, default=0.05, help="minimum seconds per timed batch")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--output", type=Path, default=BUILD / "reports" / "latest")
    parser.add_argument("--cc", default="cc", help="compiler executable")
    parser.add_argument("--flags", default=DEFAULT_FLAGS)
    parser.add_argument("--codecs", default=",".join(CODECS), help="comma-separated codec subset; include lz4")
    parser.add_argument("--require-identical", action="store_true", help="reject candidate output differing from the frozen speed baseline")
    parser.add_argument("--no-build", action="store_true", help="reuse a matching build; still run correctness checks")
    args = parser.parse_args()
    codecs = args.codecs.split(",")
    if len(set(codecs)) != len(codecs) or any(c not in CODECS for c in codecs) or not {"lz4"} <= set(codecs):
        parser.error("codecs must be unique known names and include lz4")
    sizes = [int(s) for s in args.block_sizes.split(",")]
    if not sizes or any(s not in (8192, 16384, 32768, 65536, 262144) for s in sizes):
        parser.error("block sizes must be 8192,16384,32768,65536,262144")
    if not 0 < args.seconds <= 60 or not 1 <= args.rounds <= 100:
        parser.error("seconds must be in (0,60]; rounds must be in [1,100]")
    BUILD.mkdir(parents=True, exist_ok=True)
    # A second benchmark would contaminate timings and mutate corpus files.
    lock = (BUILD / ".benchmark.lock").open("a+")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        parser.error("another BoltLZ benchmark is running; wait for it to finish")
    if not args.no_build:
        subprocess.run(["make", "-s", "boltlz-bench", "boltlz-test",
                        f"CC={args.cc}", f"BOLTLZ_CFLAGS={args.flags}"], cwd=ROOT, check=True)
    else:
        stamp_path = BUILD / "bench_boltlz.config"
        if not stamp_path.exists():
            parser.error("--no-build has no link-time build metadata; rebuild first")
        stamp = stamp_path.read_text()
        if stamp != (f"CC={args.cc}\nBOLTLZ_CFLAGS={args.flags}\n"
                     f"BOLTLZ_LZ4_CFLAGS={lz4_flags(args.flags)}\n"):
            parser.error("--no-build compiler/flags do not match the build stamp")
        if any((ROOT / p).stat().st_mtime_ns > (BUILD / "bench_boltlz").stat().st_mtime_ns
               for p in SOURCES if p.endswith((".c", ".h")) or p == "Makefile"):
            parser.error("--no-build source is newer than the benchmark; rebuild first")
        subprocess.run([str(BUILD / "test_boltlz")], check=True)
    source_hashes = {p: sha256((ROOT / p).read_bytes()) for p in SOURCES}
    # Never execute a mutable build path across per-file subprocesses.
    binary_bytes = (BUILD / "bench_boltlz").read_bytes()
    binary_hash = sha256(binary_bytes)
    immutable_binary = BUILD / "binaries" / binary_hash
    immutable_binary.parent.mkdir(parents=True, exist_ok=True)
    if not immutable_binary.exists():
        immutable_binary.write_bytes(binary_bytes)
        immutable_binary.chmod(0o755)
    cases = external_corpus(args.corpus.resolve(), args.split or "validation") if args.corpus else development_corpus(BUILD / "corpus")
    cpu = platform.processor() or platform.machine()
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        cpu = next((line.split(":", 1)[1].strip() for line in cpuinfo.read_text().splitlines()
                    if line.startswith("model name")), cpu)
    samples = []
    started = time.monotonic()
    for block_size in sizes:
        for index, case in enumerate(cases):
            print(f"{block_size // 1024} KiB: {case['name']} ({case['size'] / 1048576:.2f} MiB)", flush=True)
            process = subprocess.run([str(immutable_binary), case["path"], str(block_size),
                                      str(args.seconds), str(args.rounds), str(index % 2),
                                      str(int(args.require_identical)), ",".join(codecs)],
                                     check=True, capture_output=True, text=True)
            for line in process.stdout.splitlines():
                sample = json.loads(line)
                sample.update({"file": case["name"], "block_size": block_size})
                samples.append(sample)
    report = {"schema": 1, "environment": {
        "cpu": cpu, "platform": platform.platform(), "machine": platform.machine(),
        "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
        "flags": args.flags, "threads": 1, "affinity": "inherited", "frequency_policy": "uncontrolled",
        "lz4_flags": lz4_flags(args.flags),
        "cache_policy": "repeated full-input passes", "python": platform.python_version(),
        "zlib": zlib.ZLIB_RUNTIME_VERSION,
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()},
        "settings": {"block_sizes": sizes, "seconds": args.seconds, "rounds": args.rounds,
                     "codecs": codecs, "require_identical": args.require_identical},
        "references": {"lz4": json.loads((ROOT / "test/reference/lz4/upstream.json").read_text()),
                       "speed_baseline": json.loads((ROOT / "test/reference/boltlz_speed_base.json").read_text()),
                       "review_baseline": json.loads((ROOT / "test/reference/boltlz_review_base.json").read_text()),
                       "loop_baseline": json.loads((ROOT / "test/reference/boltlz_loop_base.json").read_text()),
                       "paired_decoder": ("loop" if any(c.startswith("boltlz-loop-before-") for c in codecs) else
                                          "review" if any(c.startswith("boltlz-review-before-") for c in codecs) else "speed"),
                       "lz4_hc_level": 9, "lz4_decoder": "LZ4_decompress_safe"},
        "source_hashes": source_hashes, "binary_hash": binary_hash,
        "cases": cases, "corpus_hash": sha256(json.dumps(
            [(c["name"], c["sha256"]) for c in cases], separators=(",", ":")).encode()),
        "samples": samples, "elapsed_seconds": time.monotonic() - started,
        "summary": {str(s): summarize(samples, cases, s, codecs) for s in sizes}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    json_path = args.output.with_suffix(".json")
    md_path = args.output.with_suffix(".md")
    json_path.write_text(json.dumps(report, indent=2) + "\n")
    markdown = markdown_report(report)
    md_path.write_text(markdown)
    print(markdown.split("###", 1)[0])
    print(f"Reports: {json_path}\n         {md_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Benchmark failed: {error}", file=sys.stderr)
        raise SystemExit(2)
