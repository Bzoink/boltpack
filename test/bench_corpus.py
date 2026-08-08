#!/usr/bin/env python3
"""
bench_corpus.py -- compare BoltPack legacy fluff, fast fluff, and zip.

Builds separate legacy and modern BoltPack binaries, benchmarks archive
creation and integrity-test throughput on a corpus directory, and reports
compressed size plus relative speedups.

Uses only the Python standard library.

Run:
    python3 test/bench_corpus.py
    python3 test/bench_corpus.py --corpus test/corpus/curated --runs 5
    python3 test/bench_corpus.py --threads 16 --verbose
"""

from __future__ import annotations

import argparse
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass


REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CORPUS = os.path.join(REPO_ROOT, "test", "corpus")
BUILD_BOLT = os.path.join(REPO_ROOT, "build", "linux", "boltpack")


@dataclass
class CaseResult:
    name: str
    archive_size: int
    create_times: list[float]
    test_times: list[float]


def run_cmd(cmd: list[str], cwd: str | None = None,
            verbose: bool = False) -> subprocess.CompletedProcess:
    if verbose:
        print("$", " ".join(cmd))
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)


def run_checked(cmd: list[str], cwd: str | None = None,
                verbose: bool = False) -> subprocess.CompletedProcess:
    result = run_cmd(cmd, cwd=cwd, verbose=verbose)
    if result.returncode != 0:
        if result.stdout:
            sys.stderr.write(result.stdout)
        if result.stderr:
            sys.stderr.write(result.stderr)
        raise RuntimeError("command failed: %s" % " ".join(cmd))
    return result


def human_mib(num_bytes: int) -> float:
    return float(num_bytes) / (1024.0 * 1024.0)


def throughput_mib_per_s(num_bytes: int, seconds: float) -> float:
    if seconds <= 0.0:
        return 0.0
    return human_mib(num_bytes) / seconds


def dir_size_bytes(path: str) -> int:
    total = 0
    for dirpath, _dirnames, filenames in os.walk(path):
        for filename in filenames:
            total += os.path.getsize(os.path.join(dirpath, filename))
    return total


def ensure_tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise RuntimeError("required tool not found in PATH: %s" % name)
    return path


def build_variant(target: str, out_path: str, verbose: bool) -> None:
    build_dir = os.path.join(REPO_ROOT, "build", "linux")

    if os.path.isdir(build_dir):
        shutil.rmtree(build_dir)

    if target == "legacy":
        run_checked(["make", "-s"], cwd=REPO_ROOT, verbose=verbose)
    elif target == "modern":
        run_checked(["make", "-s", "modern"], cwd=REPO_ROOT, verbose=verbose)
    else:
        raise RuntimeError("unknown build target: %s" % target)

    if not os.path.isfile(BUILD_BOLT):
        raise RuntimeError("expected binary missing after build: %s" % BUILD_BOLT)

    shutil.copy2(BUILD_BOLT, out_path)
    os.chmod(out_path, 0o755)


def benchmark_boltpack(name: str, binary: str, corpus_rel: str,
                       archive_path: str, input_bytes: int,
                       runs: int, warmups: int, threads: int,
                       group_blocks: int,
                       verbose: bool) -> CaseResult:
    create_times: list[float]
    test_times: list[float]
    archive_size = 0
    create_times = []
    test_times = []

    def create_cmd() -> list[str]:
        cmd = [binary, "c", "-F"]
        if threads > 1:
            cmd.extend(["-j", str(threads)])
        if group_blocks > 0:
            cmd.extend(["-g", str(group_blocks)])
        cmd.extend([archive_path, corpus_rel])
        return cmd

    def test_cmd() -> list[str]:
        return [binary, "t", archive_path]

    for _ in range(warmups):
        if os.path.exists(archive_path):
            os.remove(archive_path)
        run_checked(create_cmd(), cwd=REPO_ROOT, verbose=verbose)
        run_checked(test_cmd(), cwd=REPO_ROOT, verbose=verbose)

    for _ in range(runs):
        if os.path.exists(archive_path):
            os.remove(archive_path)

        start = time.perf_counter()
        run_checked(create_cmd(), cwd=REPO_ROOT, verbose=verbose)
        mid = time.perf_counter()
        run_checked(test_cmd(), cwd=REPO_ROOT, verbose=verbose)
        end = time.perf_counter()

        archive_size = os.path.getsize(archive_path)
        create_times.append(mid - start)
        test_times.append(end - mid)

    return CaseResult(name, archive_size, create_times, test_times)


def benchmark_zip(zip_bin: str, unzip_bin: str, corpus_rel: str,
                  archive_path: str, input_bytes: int,
                  runs: int, warmups: int, verbose: bool) -> CaseResult:
    create_times: list[float]
    test_times: list[float]
    archive_size = 0
    create_times = []
    test_times = []

    def create_cmd() -> list[str]:
        return [zip_bin, "-9", "-q", "-r", "-X", archive_path, corpus_rel]

    def test_cmd() -> list[str]:
        return [unzip_bin, "-tqq", archive_path]

    for _ in range(warmups):
        if os.path.exists(archive_path):
            os.remove(archive_path)
        run_checked(create_cmd(), cwd=REPO_ROOT, verbose=verbose)
        run_checked(test_cmd(), cwd=REPO_ROOT, verbose=verbose)

    for _ in range(runs):
        if os.path.exists(archive_path):
            os.remove(archive_path)

        start = time.perf_counter()
        run_checked(create_cmd(), cwd=REPO_ROOT, verbose=verbose)
        mid = time.perf_counter()
        run_checked(test_cmd(), cwd=REPO_ROOT, verbose=verbose)
        end = time.perf_counter()

        archive_size = os.path.getsize(archive_path)
        create_times.append(mid - start)
        test_times.append(end - mid)

    return CaseResult(name="zip-9",
                      archive_size=archive_size,
                      create_times=create_times,
                      test_times=test_times)


def print_summary(results: list[CaseResult], input_bytes: int) -> None:
    print("\nCorpus")
    print("  Input size: %.2f MiB" % human_mib(input_bytes))
    print("\nResults")
    print("  %-18s %11s %8s %12s %12s %12s %12s" % (
        "Case", "Archive MiB", "Ratio", "Create s", "Create MiB/s",
        "Test s", "Test MiB/s"))
    for result in results:
        create_avg = statistics.mean(result.create_times)
        test_avg = statistics.mean(result.test_times)
        ratio = (float(result.archive_size) / float(input_bytes) * 100.0) if input_bytes else 0.0
        print("  %-18s %11.2f %7.2f%% %12.3f %12.2f %12.3f %12.2f" % (
            result.name,
            human_mib(result.archive_size),
            ratio,
            create_avg,
            throughput_mib_per_s(input_bytes, create_avg),
            test_avg,
            throughput_mib_per_s(input_bytes, test_avg),
        ))

    by_name = {result.name: result for result in results}
    if "legacy-fluff" in by_name and "fast-fluff" in by_name:
        legacy = statistics.mean(by_name["legacy-fluff"].create_times)
        fast = statistics.mean(by_name["fast-fluff"].create_times)
        size_delta = by_name["fast-fluff"].archive_size - by_name["legacy-fluff"].archive_size
        size_pct = (float(size_delta) / float(by_name["legacy-fluff"].archive_size) * 100.0) if by_name["legacy-fluff"].archive_size else 0.0
        print("\nFast vs legacy")
        print("  Create speedup: %.2fx" % (legacy / fast if fast > 0.0 else 0.0))
        print("  Archive delta: %s bytes (%+.2f%%)" % (format(size_delta, "+,.0f"), size_pct))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Benchmark BoltPack fluff against zip on a corpus directory")
    parser.add_argument("--corpus", default=DEFAULT_CORPUS,
                        help="corpus directory (default: %(default)s)")
    parser.add_argument("--runs", type=int, default=3,
                        help="timed runs per case (default: %(default)s)")
    parser.add_argument("--warmups", type=int, default=1,
                        help="warmup runs per case (default: %(default)s)")
    parser.add_argument("--threads", type=int, default=os.cpu_count() or 1,
                        help="threads for fast fluff (-j N, default: cpu count)")
    parser.add_argument("--group-blocks", type=int, default=0,
                        help="fast fluff blocks per history group (-g N, default: codec default)")
    parser.add_argument("--no-build", action="store_true",
                        help="reuse existing binaries passed with --legacy-bin/--fast-bin")
    parser.add_argument("--legacy-bin", default="",
                        help="path to a prebuilt legacy boltpack binary")
    parser.add_argument("--fast-bin", default="",
                        help="path to a prebuilt fast boltpack binary")
    parser.add_argument("--keep-workdir", default="",
                        help="keep benchmark artifacts in this directory")
    parser.add_argument("--verbose", action="store_true",
                        help="show build and command lines")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    zip_bin = ensure_tool("zip")
    unzip_bin = ensure_tool("unzip")

    corpus = os.path.abspath(args.corpus)
    if not os.path.isdir(corpus):
        print("Corpus directory not found: %s" % corpus, file=sys.stderr)
        return 2

    corpus_rel = os.path.relpath(corpus, REPO_ROOT)
    if corpus_rel.startswith(".."):
        print("Corpus must be inside the repository for stable relative paths: %s" % corpus, file=sys.stderr)
        return 2

    if args.runs < 1 or args.warmups < 0:
        print("runs must be >= 1 and warmups must be >= 0", file=sys.stderr)
        return 2

    input_bytes = dir_size_bytes(corpus)
    workdir = args.keep_workdir or tempfile.mkdtemp(prefix="bp_bench_")
    os.makedirs(workdir, exist_ok=True)

    legacy_bin = args.legacy_bin or os.path.join(workdir, "boltpack-legacy")
    fast_bin = args.fast_bin or os.path.join(workdir, "boltpack-fast")

    print("Corpus: %s" % corpus_rel)
    print("Files: %d" % sum(len(files) for _root, _dirs, files in os.walk(corpus)))
    print("Input size: %.2f MiB" % human_mib(input_bytes))
    print("Runs: %d timed, %d warmup" % (args.runs, args.warmups))
    print("Fast fluff threads: %d" % args.threads)
    if args.group_blocks > 0:
        print("Fast fluff group blocks: %d" % args.group_blocks)

    if not args.no_build:
        print("\nBuilding binaries")
        build_variant("legacy", legacy_bin, args.verbose)
        build_variant("modern", fast_bin, args.verbose)
    else:
        if not os.path.isfile(legacy_bin) or not os.path.isfile(fast_bin):
            print("--no-build requires valid --legacy-bin and --fast-bin", file=sys.stderr)
            return 2

    print("\nBenchmarking")
    results = []
    results.append(benchmark_boltpack(
        name="legacy-fluff",
        binary=legacy_bin,
        corpus_rel=corpus_rel,
        archive_path=os.path.join(workdir, "legacy.bolt"),
        input_bytes=input_bytes,
        runs=args.runs,
        warmups=args.warmups,
        threads=0,
        group_blocks=0,
        verbose=args.verbose,
    ))
    results.append(benchmark_boltpack(
        name="fast-fluff",
        binary=fast_bin,
        corpus_rel=corpus_rel,
        archive_path=os.path.join(workdir, "fast.bolt"),
        input_bytes=input_bytes,
        runs=args.runs,
        warmups=args.warmups,
        threads=args.threads,
        group_blocks=args.group_blocks,
        verbose=args.verbose,
    ))
    results.append(benchmark_zip(
        zip_bin=zip_bin,
        unzip_bin=unzip_bin,
        corpus_rel=corpus_rel,
        archive_path=os.path.join(workdir, "corpus.zip"),
        input_bytes=input_bytes,
        runs=args.runs,
        warmups=args.warmups,
        verbose=args.verbose,
    ))

    print_summary(results, input_bytes)
    print("\nArtifacts: %s" % workdir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())