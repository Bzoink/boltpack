#!/usr/bin/env python3
"""Build two Fluff sources and run alternating, verified codec benchmarks.

Example:
  python3 test/bench_fluff.py --baseline-source /tmp/fluff-before.c \
      --output build/fluff-ab test/corpus/silesia/*

The modern option enables SIMD, with sequential encoding in both cases.
Use separate invocations for portable and modern results. No archive metadata
or stored fallback is included in the reported encoded sizes.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--baseline-source", required=True, type=Path)
    parser.add_argument("--candidate-source", type=Path, default=ROOT / "src/core/fluff.c")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cflags", default="-O3 -march=native")
    parser.add_argument("--modern", action="store_true")
    parser.add_argument("--levels", default="1,5,8")
    parser.add_argument("--pairs", type=int, default=5)
    parser.add_argument("--block-size", type=int, default=262144)
    parser.add_argument("--cpu", type=int, help="defaults to the first allowed CPU on Linux")
    args = parser.parse_args()
    levels = [int(level) for level in args.levels.split(",")]
    if args.pairs < 1 or not all(1 <= level <= 9 for level in levels):
        parser.error("pairs must be positive and levels must be 1..9")
    if not 1 <= args.block_size <= 262144:
        parser.error("block size must be 1..262144")
    files = [path.resolve() for path in args.files]
    if not all(path.is_file() for path in files):
        parser.error("all inputs must be regular files")
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    cpu = args.cpu
    if cpu is None and hasattr(os, "sched_getaffinity"):
        cpu = min(os.sched_getaffinity(0))
    flags = shlex.split(args.cflags)
    sources = {"baseline": args.baseline_source.resolve(), "candidate": args.candidate_source.resolve()}
    builds = {}
    objects = []
    exports = sorted(set(re.findall(r"\b(fluff_\w+)\s*\(", (ROOT / "src/core/fluff.h").read_text())))
    common = [args.cc, "-std=c11" if args.modern else "-ansi", "-pedantic", *flags, "-Isrc/core"]
    if args.modern:
        common += ["-DBOLTPACK_FAST_FLUFF", "-fopenmp"]
    for name, source in sources.items():
        frozen = output / f"{name}.c"
        if source != frozen:
            frozen.write_bytes(source.read_bytes())
        obj = output / f"{name}.o"
        command = [*common]
        if name == "baseline":
            command += [f"-D{symbol}=baseline_{symbol}" for symbol in exports]
        command += ["-c", str(frozen), "-o", str(obj)]
        subprocess.run(command, cwd=ROOT, check=True)
        objects.append(str(obj))
        builds[name] = {"command": command, "source_sha256": digest(frozen)}
    command = [*common, "-DFLUFF_BENCH_PAIRED", "test/bench_fluff.c", *objects]
    if args.modern:
        command += ["src/core/fluff_fast.c"]
    command += ["-o", str(output / "bench")]
    subprocess.run(command, cwd=ROOT, check=True)
    manifest = {
        "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
        "platform": platform.platform(), "cpu": cpu,
        "cpuinfo": Path("/proc/cpuinfo").read_text() if Path("/proc/cpuinfo").exists() else None,
        "governor": None, "mode": "modern-sequential" if args.modern else "portable-sequential",
        "builds": builds, "link_command": command, "binary_sha256": digest(output / "bench"), "harness_sha256": digest(ROOT / "test/bench_fluff.c"),
        "files": [{"path": str(path), "size": path.stat().st_size, "sha256": digest(path)} for path in files],
        "levels": levels, "pairs": args.pairs, "block_size": args.block_size,
        "timing": "one warmup per file; allocation and reset excluded; verification outside timing",
    }
    governor = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor")
    if governor.exists():
        manifest["governor"] = governor.read_text().strip()
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    rows = []
    with (output / "samples.csv").open("w", newline="") as handle:
        writer = None
        for level in levels:
            print(f"level {level}: {args.pairs} alternating pairs per file", flush=True)
            command = [str(output / "bench"), str(level), str(args.pairs), str(args.block_size), *map(str, files)]
            if cpu is not None:
                command = ["taskset", "-c", str(cpu), *command]
            with subprocess.Popen(command, text=True, stdout=subprocess.PIPE) as process:
                for row in csv.DictReader(process.stdout):
                    row.update(pair=int(row["iteration"]))
                    if writer is None:
                        writer = csv.DictWriter(handle, fieldnames=list(row))
                        writer.writeheader()
                    writer.writerow(row)
                    rows.append(row)
                    handle.flush()
                if process.wait():
                    raise RuntimeError("codec benchmark failed")
    summaries = []
    for level in levels:
        totals = {}
        for name in sources:
            selected = [r for r in rows if int(r["level"]) == level and r["variant"] == name]
            totals[name] = {
                "encoded_bytes": sum(int(r["encoded_bytes"]) for r in selected if r["pair"] == 0),
                "input_bytes": sum(int(r["input_bytes"]) for r in selected if r["pair"] == 0),
            }
            for metric in ("encode_seconds", "decode_seconds", "reset_seconds"):
                times = [sum(float(r[metric]) for r in selected if r["pair"] == pair) for pair in range(args.pairs)]
                totals[name][metric] = {"median": statistics.median(times), "min": min(times),
                                        "max": max(times), "samples": times}
        summary = {"level": level, "totals": totals}
        for metric in ("encode_seconds", "decode_seconds"):
            summary[metric + "_change_pct"] = 100 * (totals["candidate"][metric]["median"] /
                                                       totals["baseline"][metric]["median"] - 1)
            changes = [100 * (c / b - 1) for b, c in zip(
                totals["baseline"][metric]["samples"], totals["candidate"][metric]["samples"])]
            summary[metric + "_paired_change_pct"] = {
                "median": statistics.median(changes), "min": min(changes), "max": max(changes)}
        summary["encoded_change_pct"] = 100 * (totals["candidate"]["encoded_bytes"] /
                                               totals["baseline"]["encoded_bytes"] - 1)
        summaries.append(summary)
    (output / "summary.json").write_text(json.dumps(summaries, indent=2) + "\n")
    print(json.dumps(summaries, indent=2))


if __name__ == "__main__":
    main()
