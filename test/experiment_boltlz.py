#!/usr/bin/env python3
"""Sequential -O2 encoder experiments on aligned Silesia samples.

Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
Full-corpus validation is separate; these samples are tuning data.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import subprocess
import sys
from pathlib import Path

from bench_boltlz import BUILD, DEFAULT_FLAGS, ROOT


BASE = {"BOLTLZ_HASH_BITS": 13, "BOLTLZ_LAZY_LIMIT": 32,
        "BOLTLZ_SECOND_LIMIT": 262144, "BOLTLZ_DUAL_HASH": 0,
        "BOLTLZ_WIDE_MATCH": 0, "BOLTLZ_EXACT_COST": 0, "BOLTLZ_PRUNE_MATCH": 0,
        "BOLTLZ_REUSE_MATCH": 0, "BOLTLZ_BACKWARD_MATCH": 0,
        "BOLTLZ_HASH_CACHE": 0, "BOLTLZ_WORD_MATCH": 0}
SPEED_SWITCHES = {"BOLTLZ_SHORT_COPY": 0, "BOLTLZ_SMALL_DISTANCE": 0,
                  "BOLTLZ_FAST_DECODE": 0, "BOLTLZ_CHEAP_COST": 0,
                  "BOLTLZ_SPECIALIZE": 0, "BOLTLZ_INLINE_FIND": 0}
BASE.update(SPEED_SWITCHES)
REVIEW_SWITCHES = {"BOLTLZ_ENCODE_SHORT_COPY": 0, "BOLTLZ_DECODE_DISTANCE3": 0,
                   "BOLTLZ_DECODE_ZERO_LITERAL": 0}
BASE.update(REVIEW_SWITCHES)
LOOP_SWITCHES = {"BOLTLZ_NARROW_TABLE": 0, "BOLTLZ_PIPELINE": 0,
                 "BOLTLZ_FULL_CAPACITY": 0, "BOLTLZ_EXTENDED_DECODE": 0}
BASE.update(LOOP_SWITCHES)
VARIANTS = {
    "baseline": {},
    "hash12": {"BOLTLZ_HASH_BITS": 12},
    "hash14": {"BOLTLZ_HASH_BITS": 14},
    "lazy0": {"BOLTLZ_LAZY_LIMIT": 0},
    "lazy8": {"BOLTLZ_LAZY_LIMIT": 8},
    "lazy16": {"BOLTLZ_LAZY_LIMIT": 16},
    "second8": {"BOLTLZ_SECOND_LIMIT": 8},
    "second16": {"BOLTLZ_SECOND_LIMIT": 16},
    "search8": {"BOLTLZ_LAZY_LIMIT": 8, "BOLTLZ_SECOND_LIMIT": 8},
    "dualhash": {"BOLTLZ_DUAL_HASH": 1},
    "wide8": {"BOLTLZ_WIDE_MATCH": 1},
    "exactcost": {"BOLTLZ_EXACT_COST": 1},
    "prune": {"BOLTLZ_PRUNE_MATCH": 1},
    "greedy8": {"BOLTLZ_LAZY_LIMIT": 0, "BOLTLZ_SECOND_LIMIT": 8},
    "hash14search8": {"BOLTLZ_HASH_BITS": 14, "BOLTLZ_LAZY_LIMIT": 8,
                      "BOLTLZ_SECOND_LIMIT": 8},
    "hash14greedy8": {"BOLTLZ_HASH_BITS": 14, "BOLTLZ_LAZY_LIMIT": 0,
                      "BOLTLZ_SECOND_LIMIT": 8},
    "dualgreedy": {"BOLTLZ_LAZY_LIMIT": 0, "BOLTLZ_DUAL_HASH": 1},
    "reuse": {"BOLTLZ_REUSE_MATCH": 1},
    "backward": {"BOLTLZ_BACKWARD_MATCH": 1},
    "hashcache": {"BOLTLZ_HASH_CACHE": 1},
    "wordmatch": {"BOLTLZ_WORD_MATCH": 1},
    "backwardcache": {"BOLTLZ_BACKWARD_MATCH": 1, "BOLTLZ_HASH_CACHE": 1},
    "backwardword": {"BOLTLZ_BACKWARD_MATCH": 1, "BOLTLZ_WORD_MATCH": 1},
    "lz4ideas": {"BOLTLZ_BACKWARD_MATCH": 1, "BOLTLZ_HASH_CACHE": 1,
                 "BOLTLZ_WORD_MATCH": 1},
}
SPEED_BASE = {**BASE, "BOLTLZ_BACKWARD_MATCH": 1, "BOLTLZ_WORD_MATCH": 1}
SPEED_VARIANTS = {
    "speed-baseline": {},
    "speed-short": {"BOLTLZ_SHORT_COPY": 1},
    "speed-overlap": {"BOLTLZ_SMALL_DISTANCE": 1},
    "speed-decode": {"BOLTLZ_FAST_DECODE": 1},
    "speed-cost": {"BOLTLZ_CHEAP_COST": 1},
    "speed-specialize": {"BOLTLZ_SPECIALIZE": 1},
    "speed-inline": {"BOLTLZ_INLINE_FIND": 1},
    "speed-decoder-all": {"BOLTLZ_SHORT_COPY": 1, "BOLTLZ_SMALL_DISTANCE": 1,
                          "BOLTLZ_FAST_DECODE": 1},
    "speed-encoder-all": {"BOLTLZ_CHEAP_COST": 1, "BOLTLZ_SPECIALIZE": 1,
                          "BOLTLZ_INLINE_FIND": 1},
    "speed-all": {key: 1 for key in SPEED_SWITCHES},
    "speed-headtail": {"BOLTLZ_SHORT_COPY": 2},
    "speed-headtail-overlap": {"BOLTLZ_SHORT_COPY": 2, "BOLTLZ_SMALL_DISTANCE": 1},
    "speed-headtail-decode": {"BOLTLZ_SHORT_COPY": 2, "BOLTLZ_FAST_DECODE": 1},
    "speed-headtail-decoder-all": {"BOLTLZ_SHORT_COPY": 2, "BOLTLZ_SMALL_DISTANCE": 1,
                                   "BOLTLZ_FAST_DECODE": 1},
    "speed-cost-specialize": {"BOLTLZ_CHEAP_COST": 1, "BOLTLZ_SPECIALIZE": 1},
    "speed-headtail-all": {**{key: 1 for key in SPEED_SWITCHES}, "BOLTLZ_SHORT_COPY": 2},
    "speed-widefast": {"BOLTLZ_FAST_DECODE": 2},
    "speed-widefast-headtail": {"BOLTLZ_SHORT_COPY": 2, "BOLTLZ_FAST_DECODE": 2},
    "speed-widefast-overlap": {"BOLTLZ_SHORT_COPY": 2, "BOLTLZ_FAST_DECODE": 2,
                              "BOLTLZ_SMALL_DISTANCE": 1},
    "speed-widefast-all": {**{key: 1 for key in SPEED_SWITCHES}, "BOLTLZ_SHORT_COPY": 2,
                          "BOLTLZ_FAST_DECODE": 2},
}
VARIANTS.update({name: {**SPEED_BASE, **settings} for name, settings in SPEED_VARIANTS.items()})
REVIEW_BASE = {**SPEED_BASE, **SPEED_VARIANTS["speed-widefast-all"]}
REVIEW_VARIANTS = {
    "review-baseline": {},
    "review-encode": {"BOLTLZ_ENCODE_SHORT_COPY": 1},
    "review-distance3": {"BOLTLZ_DECODE_DISTANCE3": 1},
    "review-zero": {"BOLTLZ_DECODE_ZERO_LITERAL": 1},
    "review-encode-distance3": {"BOLTLZ_ENCODE_SHORT_COPY": 1, "BOLTLZ_DECODE_DISTANCE3": 1},
    "review-encode-zero": {"BOLTLZ_ENCODE_SHORT_COPY": 1, "BOLTLZ_DECODE_ZERO_LITERAL": 1},
    "review-decode": {"BOLTLZ_DECODE_DISTANCE3": 1, "BOLTLZ_DECODE_ZERO_LITERAL": 1},
    "review-all": {key: 1 for key in REVIEW_SWITCHES},
    "review-wide-encode": {"BOLTLZ_ENCODE_SHORT_COPY": 2},
    "review-wide-distance3": {"BOLTLZ_ENCODE_SHORT_COPY": 2, "BOLTLZ_DECODE_DISTANCE3": 1},
}
VARIANTS.update({name: {**REVIEW_BASE, **settings} for name, settings in REVIEW_VARIANTS.items()})
LOOP_BASE = {**REVIEW_BASE, **REVIEW_VARIANTS["review-wide-distance3"]}
LOOP_VARIANTS = {
    "loop-baseline": {},
    "loop-narrow": {"BOLTLZ_NARROW_TABLE": 1},
    "loop-pipeline": {"BOLTLZ_PIPELINE": 1},
    "loop-capacity": {"BOLTLZ_FULL_CAPACITY": 1},
    "loop-extended": {"BOLTLZ_EXTENDED_DECODE": 1},
    "loop-wide16": {"BOLTLZ_EXTENDED_DECODE": 2},
    "loop-wide32": {"BOLTLZ_EXTENDED_DECODE": 3},
    "loop-encoder": {"BOLTLZ_NARROW_TABLE": 1, "BOLTLZ_PIPELINE": 1, "BOLTLZ_FULL_CAPACITY": 1},
    "loop-narrow-capacity": {"BOLTLZ_NARROW_TABLE": 1, "BOLTLZ_FULL_CAPACITY": 1},
    "loop-pipeline-capacity": {"BOLTLZ_PIPELINE": 1, "BOLTLZ_FULL_CAPACITY": 1},
    "loop-narrow-pipeline": {"BOLTLZ_NARROW_TABLE": 1, "BOLTLZ_PIPELINE": 1},
    "loop-narrow-wide32": {"BOLTLZ_NARROW_TABLE": 1, "BOLTLZ_EXTENDED_DECODE": 3},
    "loop-narrow-capacity-wide32": {"BOLTLZ_NARROW_TABLE": 1, "BOLTLZ_FULL_CAPACITY": 1,
                                    "BOLTLZ_EXTENDED_DECODE": 3},
    "loop-all16": {"BOLTLZ_NARROW_TABLE": 1, "BOLTLZ_PIPELINE": 1, "BOLTLZ_FULL_CAPACITY": 1,
                   "BOLTLZ_EXTENDED_DECODE": 2},
    "loop-all32": {"BOLTLZ_NARROW_TABLE": 1, "BOLTLZ_PIPELINE": 1, "BOLTLZ_FULL_CAPACITY": 1,
                   "BOLTLZ_EXTENDED_DECODE": 3},
    "loop-shared": {"BOLTLZ_EXTENDED_DECODE": 4},
    "loop-packed": {"BOLTLZ_NARROW_TABLE": 2},
    "loop-packed-capacity": {"BOLTLZ_NARROW_TABLE": 2, "BOLTLZ_FULL_CAPACITY": 1},
    "loop-narrow-capacity-shared": {"BOLTLZ_NARROW_TABLE": 1, "BOLTLZ_FULL_CAPACITY": 1,
                                    "BOLTLZ_EXTENDED_DECODE": 4},
    "loop-packed-capacity-shared": {"BOLTLZ_NARROW_TABLE": 2, "BOLTLZ_FULL_CAPACITY": 1,
                                    "BOLTLZ_EXTENDED_DECODE": 4},
}
VARIANTS.update({name: {**LOOP_BASE, **settings} for name, settings in LOOP_VARIANTS.items()})
SPEED_CODECS = ["boltlz-fast", "boltlz-balanced", "lz4",
                "boltlz-speed-before-fast", "boltlz-speed-before-balanced"]
REVIEW_CODECS = [c.replace("speed-before", "review-before") for c in SPEED_CODECS]
LOOP_CODECS = [c.replace("speed-before", "loop-before") for c in SPEED_CODECS]


def sampled_corpus(source: Path, destination: Path) -> dict:
    """Retain complete original block boundaries through 256 KiB.

    Files larger than three windows contribute first, middle, and final
    windows. Every starting offset and every nonfinal span is 256 KiB aligned.
    No repetition or padding is introduced between selected spans.
    """
    if source.resolve() == destination.resolve():
        raise ValueError("Sample output must differ from the source corpus")
    destination.mkdir(parents=True, exist_ok=True)
    manifest = {}
    window = 262144
    for path in sorted(source.iterdir()):
        if path.is_symlink() or not path.is_file():
            continue
        data = path.read_bytes()
        if not data:
            continue
        if len(data) <= 3 * window:
            spans = [(0, len(data))]
        else:
            starts = [0, (len(data) // 2 // window) * window,
                      ((len(data) - 1) // window) * window]
            spans = [(start, min(window, len(data) - start)) for start in starts]
        sample = b"".join(data[start:start + length] for start, length in spans)
        target = destination / path.name
        if target.is_symlink() or (target.exists() and target.samefile(path)):
            raise ValueError(f"Sample output aliases a source file: {target}")
        target.write_bytes(sample)
        manifest[path.name] = {"original_size": len(data), "sample_size": len(sample),
                               "original_sha256": hashlib.sha256(data).hexdigest(),
                               "sample_sha256": hashlib.sha256(sample).hexdigest(),
                               "spans": spans}
    if not manifest:
        raise ValueError("No corpus files found")
    # Keep metadata outside the input directory so it is not benchmark data.
    destination.with_suffix(".manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=ROOT / "test/corpus/silesia")
    parser.add_argument("--output", type=Path, default=BUILD / "experiments" / "o2")
    parser.add_argument("--variants", default=",".join(VARIANTS))
    parser.add_argument("--seconds", type=float, default=0.015)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--block-sizes", default="32768")
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--append", action="store_true", help="extend an existing summary")
    args = parser.parse_args()
    names = args.variants.split(",")
    if any(name not in VARIANTS for name in names):
        parser.error("unknown variant; available: " + ",".join(VARIANTS))
    if len(set(names)) != len(names):
        parser.error("variant names must be unique")
    args.output.mkdir(parents=True, exist_ok=True)
    BUILD.mkdir(parents=True, exist_ok=True)
    lock = (BUILD / ".experiment.lock").open("a+")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        parser.error("another experiment sweep is running")
    samples = args.output / "samples"
    manifest = sampled_corpus(args.corpus.resolve(), samples)
    summary_path = args.output / "summary.json"
    previous = json.loads(summary_path.read_text()) if args.append and summary_path.exists() else None
    if previous is not None and json.dumps(previous["manifest"], sort_keys=True) != json.dumps(manifest, sort_keys=True):
        parser.error("--append corpus differs from the existing experiments")
    rows = previous["results"] if previous is not None else []
    rows = [row for row in rows if row["variant"] not in names]
    for name in names:
        configuration = {**BASE, **VARIANTS[name]}
        flags = DEFAULT_FLAGS + " " + " ".join(f"-D{k}={v}" for k, v in configuration.items())
        prefix = args.output / name
        log = prefix.with_suffix(".log")
        print(f"Experiment {name}: {configuration}", flush=True)
        with log.open("w") as handle:
            subprocess.run([sys.executable, str(ROOT / "test/bench_boltlz.py"),
                            "--corpus", str(samples), "--flags", flags, "--cc", args.cc,
                            "--split", "development",
                            "--seconds", str(args.seconds), "--rounds", str(args.rounds),
                            "--block-sizes", args.block_sizes, "--output", str(prefix),
                            *(["--codecs", ",".join(LOOP_CODECS if name in LOOP_VARIANTS else
                                                    REVIEW_CODECS if name in REVIEW_VARIANTS else SPEED_CODECS),
                               "--require-identical"] if name in SPEED_VARIANTS or name in REVIEW_VARIANTS or name in LOOP_VARIANTS else [])],
                           cwd=ROOT, check=True, stdout=handle, stderr=subprocess.STDOUT)
        report = json.loads(prefix.with_suffix(".json").read_text())
        for block_size, summary in report["summary"].items():
            for mode in ("fast", "balanced"):
                current = summary[f"boltlz-{mode}"]["aggregate"]
                before_name = (f"boltlz-loop-before-{mode}" if name in LOOP_VARIANTS else
                               f"boltlz-review-before-{mode}" if name in REVIEW_VARIANTS else
                               f"boltlz-speed-before-{mode}" if name in SPEED_VARIANTS else f"boltlz-before-{mode}")
                before = summary[before_name]["aggregate"]
                lz4 = summary["lz4"]["aggregate"]
                rows.append({"variant": name, "mode": mode, "block_size": int(block_size),
                             "configuration": configuration,
                             "size_delta_before": current["encoded_bytes"] / before["encoded_bytes"] - 1,
                             "encode_vs_before": current["encode_mib_s"] / before["encode_mib_s"],
                             "decode_vs_before": (current["decoder_speedup"] if name in SPEED_VARIANTS or name in REVIEW_VARIANTS or name in LOOP_VARIANTS else
                                                  current["decode_mib_s"] / before["decode_mib_s"]),
                             "baseline": before_name,
                             "encoder_identical": current.get("encoder_identical"),
                             "size_delta_lz4": current["encoded_bytes"] / lz4["encoded_bytes"] - 1,
                             "encode_vs_lz4": current["encode_mib_s"] / lz4["encode_mib_s"],
                             "report": str(prefix.with_suffix(".json"))})
        lines = ["# BoltLZ -O2 tuning experiments", "",
                 "Aligned Silesia samples; screening results, not full-corpus claims.", "",
                 "| Variant | Mode | Block KiB | Size vs before | Encode vs before | Decode vs before | Size vs LZ4 | Encode vs LZ4 |",
                 "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
        for row in rows:
            lines.append(f"| {row['variant']} | {row['mode']} | {row['block_size'] // 1024} | "
                         f"{row['size_delta_before']:+.2%} | {row['encode_vs_before']:.2f}x | "
                         f"{row['decode_vs_before']:.2f}x | {row['size_delta_lz4']:+.2%} | "
                         f"{row['encode_vs_lz4']:.2f}x |")
        (args.output / "summary.json").write_text(json.dumps(
            {"manifest": manifest, "results": rows}, indent=2) + "\n")
        (args.output / "summary.md").write_text("\n".join(lines) + "\n")
        for row in rows[-2 * len(report["summary"]):]:
            print(f"  {row['mode']}: size {row['size_delta_before']:+.2%}, "
                  f"encode {row['encode_vs_before']:.2f}x, decode {row['decode_vs_before']:.2f}x before", flush=True)
    print(f"Summary: {args.output / 'summary.md'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Experiment failed: {error}", file=sys.stderr)
        raise SystemExit(2)
