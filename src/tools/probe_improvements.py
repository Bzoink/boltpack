#!/usr/bin/env python3
"""
probe_improvements.py -- Probe compression ratio improvement opportunities.

Runs three analyses:
  1. Entropy ceiling  -- Shannon entropy lower bound per file
  2. Chain depth scan -- Recompile fluff with different MAX_CHAIN values
  3. Match quality    -- Simulate cost-based match filtering on fluff tokens

Usage:
    python3 src/tools/probe_improvements.py [--corpus-path PATH] [--subset SUBSET]
    python3 src/tools/probe_improvements.py --quick   (3 files, fast)
"""

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile

BOLTPACK  = os.path.join(os.path.dirname(__file__), '..', '..', 'build', 'linux', 'boltpack')
BOLTPACK  = os.path.normpath(BOLTPACK)
SRCDIR    = os.path.join(os.path.dirname(__file__), '..', '..', 'src', 'core')
SRCDIR    = os.path.normpath(SRCDIR)
LIBDIR    = os.path.join(os.path.dirname(__file__), '..', '..', 'lib')
LIBDIR    = os.path.normpath(LIBDIR)

DEFAULT_CORPUS = '/var/run/media/tony/Volume1/corpus/curated'
DEFAULT_SUBSET = 'silesia'

# ─── Entropy analysis ────────────────────────────────────────────────────────

def byte_entropy(data: bytes) -> float:
    """Shannon entropy in bits/byte for a byte stream."""
    if not data:
        return 0.0
    freq = [0] * 256
    for b in data:
        freq[b] += 1
    n = len(data)
    h = 0.0
    for f in freq:
        if f > 0:
            p = f / n
            h -= p * math.log2(p)
    return h


def block_entropy(data: bytes, block_size: int = 32768) -> float:
    """Average per-block entropy (captures local structure better)."""
    if len(data) <= block_size:
        return byte_entropy(data)
    total = 0.0
    count = 0
    for off in range(0, len(data), block_size):
        chunk = data[off : off + block_size]
        total += byte_entropy(chunk) * len(chunk)
        count += len(chunk)
    return total / count if count else 0.0


def entropy_ceiling(filepath: str) -> dict:
    """Compute entropy-based theoretical minimum compressed size."""
    with open(filepath, 'rb') as f:
        data = f.read()
    n = len(data)
    global_h = byte_entropy(data)
    block_h  = block_entropy(data, 32768)   # same as gzip block
    block_h2 = block_entropy(data, 262144)  # same as fluff block
    return {
        'size':      n,
        'global_h':  global_h,
        'block_h':   block_h,
        'block_h2':  block_h2,
        # Minimum compressed bytes estimated from entropy
        'min_bytes_global':  int(global_h  * n / 8),
        'min_bytes_block32': int(block_h   * n / 8),
        'min_bytes_block256':int(block_h2  * n / 8),
    }


# ─── Chain depth scan ─────────────────────────────────────────────────────────

def build_fluff_chain(chain_depth: int, build_dir: str) -> str | None:
    """Compile a boltpack binary with a given MAX_CHAIN override.
    Returns path to binary or None on failure.
    """
    binary = os.path.join(build_dir, f'boltpack_chain{chain_depth}')
    if os.path.exists(binary):
        return binary  # reuse cached build

    fluff_src  = os.path.join(SRCDIR, 'fluff.c')
    archiver_src = os.path.join(SRCDIR, 'archiver.c')
    compress_src = os.path.join(SRCDIR, 'compress.c')
    codepage_src = os.path.join(SRCDIR, 'bp_codepage.c')
    linux_main = os.path.join(os.path.dirname(__file__), '..', '..', 'src',
                               'frontend', 'linux', 'main.c')
    linux_main = os.path.normpath(linux_main)
    fastlz_src = os.path.join(LIBDIR, 'fastlz', 'fastlz.c')

    cmd = [
        'gcc', '-ansi', '-pedantic', '-Wall',
        '-O2',
        f'-DFLUFF_MAX_CHAIN={chain_depth}',
        '-I', SRCDIR,
        '-I', os.path.join(LIBDIR, 'fastlz'),
        '-I', os.path.join(LIBDIR, 'miniz'),
        fluff_src, archiver_src, compress_src, codepage_src,
        fastlz_src, linux_main,
        '-o', binary,
    ]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f'  [build failed chain={chain_depth}]', file=sys.stderr)
        print(result.stderr[:400], file=sys.stderr)
        return None
    return binary


def compress_ratio(binary: str, filepath: str) -> float | None:
    """Compress a file to a temp archive, return ratio (0..1). None on error."""
    with tempfile.NamedTemporaryFile(suffix='.bolt', delete=False) as tf:
        tmp_archive = tf.name
    try:
        r = subprocess.run(
            [binary, 'c', '-F', '-q', tmp_archive, filepath],
            capture_output=True, timeout=120)
        if r.returncode != 0:
            return None
        orig = os.path.getsize(filepath)
        comp = os.path.getsize(tmp_archive)
        return comp / orig if orig > 0 else 0.0
    except Exception:
        return None
    finally:
        try:
            os.unlink(tmp_archive)
        except OSError:
            pass


def chain_depth_scan(test_files: list[str], build_dir: str) -> list[dict]:
    """Test multiple chain depths on a set of files."""
    depths = [32, 64, 128, 256, 512, 1024, 2048]
    results = []

    for depth in depths:
        binary = build_fluff_chain(depth, build_dir)
        if binary is None:
            continue
        ratios = []
        total_orig = 0
        total_comp = 0
        for fp in test_files:
            orig = os.path.getsize(fp)
            with tempfile.NamedTemporaryFile(suffix='.bolt', delete=False) as tf:
                tmp_archive = tf.name
            try:
                r = subprocess.run(
                    [binary, 'c', '-F', '-q', tmp_archive, fp],
                    capture_output=True, timeout=120)
                if r.returncode == 0:
                    comp = os.path.getsize(tmp_archive)
                    ratios.append(comp / orig)
                    total_orig += orig
                    total_comp += comp
            except Exception:
                pass
            finally:
                try:
                    os.unlink(tmp_archive)
                except OSError:
                    pass

        if ratios:
            results.append({
                'depth':    depth,
                'files':    len(ratios),
                'avg_ratio': total_comp / total_orig if total_orig else 0,
            })
    return results


# ─── Match quality / short-match filter simulation ───────────────────────────

# Distance cost model: dist_symbol extra bits for DEFLATE/fluff distances
DIST_EXTRA = [
    0,0,0,0, 1,1,2,2, 3,3,4,4, 5,5,6,6,
    7,7,8,8, 9,9,10,10, 11,11,12,12, 13,13,
    14, 14, 15, 15, 16, 16    # fluff extended dist symbols 30-35
]

LEN_EXTRA = [
    0,0,0,0,0,0,0,0,   # 257-264
    1,1,1,1,           # 265-268
    2,2,2,2,           # 269-272
    3,3,3,3,           # 273-276
    4,4,4,4,           # 277-280
    5,5,5,5,           # 281-284
    0                  # 285
]

LEN_BASE = [
    3,4,5,6,7,8,9,10,
    11,13,15,17,
    19,23,27,31,
    35,43,51,59,
    67,83,99,115,
    131,163,195,227,
    258
]

DIST_BASE = [
    1,2,3,4, 5,7,9,13,
    17,25,33,49, 65,97,129,193,
    257,385,513,769, 1025,1537,2049,3073,
    4097,6145,8193,12289, 16385,24577,
    32769,49153,65537,98305,131073,196609
]


def dist_to_sym(d: int) -> int:
    for i in range(len(DIST_BASE) - 1, -1, -1):
        if d >= DIST_BASE[i]:
            return i
    return 0


def len_to_sym(length: int) -> int:
    for i in range(len(LEN_BASE) - 1, -1, -1):
        if length >= LEN_BASE[i]:
            return i
    return 0


def match_min_useful_len(dist: int, lit_bits: float = 8.0,
                          match_overhead: float = 18.0) -> int:
    """
    Minimum match length where encoding a match is cheaper than literals.

    match_overhead = average bits for (len_sym + dist_sym + huffman overhead).
    A match of length L saves L*lit_bits bits but costs:
        (dist_extra_bits + len_extra_bits + match_overhead) bits.

    Minimum profitable L = ceil(overhead / lit_bits) ... but overhead includes
    extra bits which depend on length too.  We solve numerically.

    dist_extra:      extra bits for the distance code alone
    match_overhead:  rough fixed overhead for the two symbol codes (~16 bits
                     for 8-bit symbols + a few bits for extras)
    """
    d_sym = dist_to_sym(dist)
    d_extra = DIST_EXTRA[d_sym]

    for L in range(3, 259):
        l_sym = len_to_sym(L)
        l_extra = LEN_EXTRA[l_sym]
        # Savings from replacing L literals with a match token
        savings = L * lit_bits
        # Cost of the match token
        cost = match_overhead + d_extra + l_extra
        if savings > cost:
            return L
    return 258  # never profitable


def simulate_short_match_filter(filepath: str,
                                  lit_bits: float = 8.0,
                                  overhead: float = 18.0) -> dict:
    """
    Read the raw file and simulate a greedy LZ77 parser with distance-aware
    minimum match filtering.

    This is a SIMULATION only (Python, not the real encoder) but uses the same
    hash-chain logic to estimate how many matches the real encoder would discard
    and what ratio improvement that implies.

    Returns a summary dict.
    """
    import hashlib

    with open(filepath, 'rb') as f:
        data = f.read()

    n = len(data)
    WINDOW = 1 << 18   # 256KB
    MIN_MATCH = 3
    MAX_MATCH = 258
    MAX_CHAIN = 256

    # Simple 3-byte hash table
    hash_table = {}   # 3-byte hash → last position
    hash_chain  = [0] * n

    i = 0
    accepted_matches = 0
    rejected_matches = 0
    accepted_bytes   = 0
    rejected_bytes   = 0  # bytes from rejected matches that become literals
    literals         = 0

    while i < n - MIN_MATCH + 1:
        # hash
        key = data[i:i+3]
        head = hash_table.get(key, -1)

        # insert
        hash_table[key] = i
        if head >= 0:
            hash_chain[i] = head

        # find best match
        best_len = 0
        best_dist = 0
        probe = head
        chain = MAX_CHAIN

        while probe >= 0 and chain > 0:
            dist = i - probe
            if dist > WINDOW or dist <= 0:
                break
            # extend match
            max_len = min(MAX_MATCH, n - i)
            L = 0
            while L < max_len and data[i + L] == data[probe + L]:
                L += 1
            if L > best_len:
                best_len = L
                best_dist = dist
                if L >= 64:  # GOOD_MATCH
                    break
            probe = hash_chain[probe] if probe > 0 else -1
            chain -= 1

        if best_len >= MIN_MATCH:
            min_useful = match_min_useful_len(best_dist, lit_bits, overhead)
            if best_len >= min_useful:
                accepted_matches += 1
                accepted_bytes   += best_len
                i += best_len
            else:
                rejected_matches += 1
                rejected_bytes   += best_len
                literals += 1
                i += 1
        else:
            literals += 1
            i += 1

    return {
        'input_size':       n,
        'accepted_matches': accepted_matches,
        'rejected_matches': rejected_matches,
        'accepted_bytes':   accepted_bytes,
        'rejected_bytes':   rejected_bytes,
        'literals':         literals,
        'match_frac':       accepted_matches / (i + 1) if i else 0,
        'reject_frac':      rejected_matches / (accepted_matches + rejected_matches + 1),
    }


# ─── Main ─────────────────────────────────────────────────────────────────────

def find_test_files(corpus_path: str, subset: str | None,
                    max_files: int = 999) -> list[str]:
    if subset:
        base = os.path.join(corpus_path, subset)
    else:
        base = corpus_path

    files = []
    for entry in sorted(os.listdir(base)):
        path = os.path.join(base, entry)
        if os.path.isfile(path):
            files.append(path)
        if len(files) >= max_files:
            break
    return files


def main():
    parser = argparse.ArgumentParser(description='Probe fluff compression improvement opportunities')
    parser.add_argument('--corpus-path', default=DEFAULT_CORPUS)
    parser.add_argument('--subset', default=DEFAULT_SUBSET)
    parser.add_argument('--max-files', type=int, default=12)
    parser.add_argument('--quick', action='store_true',
                        help='Use 3 representative files only')
    parser.add_argument('--skip-build', action='store_true',
                        help='Skip chain depth recompile scan')
    parser.add_argument('--skip-simulate', action='store_true',
                        help='Skip Python LZ77 simulation (slow for large files)')
    args = parser.parse_args()

    max_files = 3 if args.quick else args.max_files
    files = find_test_files(args.corpus_path, args.subset, max_files)

    if not files:
        print(f'No files found in {args.corpus_path}/{args.subset}')
        sys.exit(1)

    print(f'=== Fluff Improvement Probe ===')
    print(f'Corpus: {args.corpus_path}/{args.subset}')
    print(f'Files:  {len(files)}')
    print()

    # ── 1. Entropy ceiling ────────────────────────────────────────────────
    print('─' * 70)
    print('1. ENTROPY CEILING (Shannon lower bound on compression)')
    print('─' * 70)
    print(f'  {"File":18s}  {"Size":>9s}  {"Global H":>8s}  {"Floor32k%":>9s}  {"Floor256k%":>10s}')
    print(f'  {"-"*18}  {"-"*9}  {"-"*8}  {"-"*9}  {"-"*10}')

    total_orig = 0
    total_min_global = 0
    total_min_256 = 0

    for fp in files:
        name = os.path.basename(fp)
        try:
            e = entropy_ceiling(fp)
        except Exception as ex:
            print(f'  {name:18s}  ERROR: {ex}')
            continue

        orig = e['size']
        fl32  = e['min_bytes_block32']  / orig * 100
        fl256 = e['min_bytes_block256'] / orig * 100
        total_orig       += orig
        total_min_global += e['min_bytes_global']
        total_min_256    += e['min_bytes_block256']

        print(f'  {name:18s}  {orig/1e6:>8.1f}M'
              f'  {e["global_h"]:>7.4f}b'
              f'  {fl32:>8.2f}%'
              f'  {fl256:>9.2f}%')

    if total_orig > 0:
        print(f'  {"TOTAL":18s}  {total_orig/1e6:>8.1f}M'
              f'  {"":>8s}'
              f'  {"":>9s}'
              f'  {total_min_256/total_orig*100:>9.2f}%')

    print()
    print('  Note: "Floor256k%" is the absolute minimum achievable by any')
    print('  block coder with 256 KB blocks (Huffman or arithmetic).  LZ77')
    print('  goes further by exploiting repetition across the window.')
    print()

    # ── 2. Chain depth scan ───────────────────────────────────────────────
    if not args.skip_build:
        print('─' * 70)
        print('2. CHAIN DEPTH SCAN (recompile with different MAX_CHAIN)')
        print('   Shows how much ratio improves vs. compression time cost.')
        print('─' * 70)

        with tempfile.TemporaryDirectory() as build_dir:
            depths = [32, 64, 128, 256, 512, 1024, 2048]
            # Use only the first few files for speed
            scan_files = files[:min(4, len(files))]
            scan_names = [os.path.basename(f) for f in scan_files]
            print(f'  Files: {", ".join(scan_names)}')
            print()

            baseline_ratio = None
            results = []
            for depth in depths:
                print(f'  Building chain={depth:4d}...', end='', flush=True)
                binary = build_fluff_chain(depth, build_dir)
                if binary is None:
                    print(' SKIP (build failed)')
                    continue

                total_orig_c = 0
                total_comp_c = 0
                ok = True
                for fp in scan_files:
                    orig = os.path.getsize(fp)
                    with tempfile.NamedTemporaryFile(suffix='.bolt', delete=False) as tf:
                        tmp = tf.name
                    try:
                        r = subprocess.run(
                            [binary, 'c', '-F', '-q', tmp, fp],
                            capture_output=True, timeout=180)
                        if r.returncode == 0:
                            comp = os.path.getsize(tmp)
                            total_orig_c += orig
                            total_comp_c += comp
                        else:
                            ok = False
                    except Exception:
                        ok = False
                    finally:
                        try:
                            os.unlink(tmp)
                        except OSError:
                            pass

                if ok and total_orig_c > 0:
                    ratio = total_comp_c / total_orig_c * 100
                    if baseline_ratio is None:
                        baseline_ratio = ratio
                    delta = ratio - baseline_ratio
                    marker = ' ← baseline' if depth == 256 else ''
                    print(f' {ratio:6.2f}%  ({delta:+.2f}% vs 256){marker}')
                    results.append((depth, ratio))
                else:
                    print(' SKIP (compress error)')

        print()

    # ── 3. Minimum match length simulation ────────────────────────────────
    if not args.skip_simulate:
        print('─' * 70)
        print('3. SHORT-MATCH FILTER SIMULATION')
        print('   Estimates how many sub-optimal short/distant matches are accepted.')
        print('   "Rejected" = match found but distance makes it unprofitable.')
        print('─' * 70)

        # Only simulate on small files (simulation is O(n*chain) in Python)
        MAX_SIM_SIZE = 2 * 1024 * 1024  # 2 MB limit for Python simulation
        sim_files = [f for f in files if os.path.getsize(f) <= MAX_SIM_SIZE]
        if not sim_files and files:
            # Use first file regardless, with a warning
            sim_files = files[:1]
            print(f'  Warning: all files > 2 MB; simulating first file only (slow)')

        print(f'  {"File":18s}  {"Size":>8s}  {"Rejected%":>9s}  {"WastedBytes":>11s}  {"Note":s}')
        print(f'  {"-"*18}  {"-"*8}  {"-"*9}  {"-"*11}  {"-"*20}')

        for fp in sim_files[:5]:  # cap at 5 files
            name = os.path.basename(fp)
            size = os.path.getsize(fp)
            if size > MAX_SIM_SIZE:
                print(f'  {name:18s}  {size/1e6:>7.1f}M  SKIP (too large for sim)')
                continue
            print(f'  {name:18s}  {size/1e6:>7.1f}M  ...', end='', flush=True)
            try:
                r = simulate_short_match_filter(fp)
                reject_pct = r["reject_frac"] * 100
                wasted = r["rejected_bytes"]
                note = ''
                if reject_pct > 5:
                    note = '← filter opportunity!'
                print(f'  {reject_pct:8.2f}%  {wasted:>11,d}  {note}')
            except Exception as ex:
                print(f'  ERROR: {ex}')

        print()

    # ── 4. Summary and recommendations ───────────────────────────────────
    print('─' * 70)
    print('4. RECOMMENDATIONS')
    print('─' * 70)
    print("""
  Based on the analysis above:

  A. Increase MAX_CHAIN (easy, ratio gain)
     Current: 256.  Try 512 or 1024 for better matches.
     Cost: 2x-4x compression time; no format change needed.

  B. Distance-aware minimum match length (quick win, ratio gain)
     Currently any length >= 3 is accepted regardless of match distance.
     A match of len=3 at dist=100KB costs ~35 bits vs ~24 bits for 3 literals.
     Filter: require len >= 5 for dist > 8KB, len >= 7 for dist > 64KB.
     Implementation: ~5 lines in fl_find_match_hyb() reject path.
     No format change needed.

  C. Multi-step lazy parsing (medium effort, ~1-3% ratio gain)
     Current: check i+1.  Extend to check i, i+1, i+2 before committing.
     zlib default uses 2-step; zlib level 9 uses longer chains + 3-step.
     No format change needed.

  D. Optimal parser (significant effort, ~3-8% ratio gain vs current)
     Replace greedy+lazy with full DP cost model (like zopfli or zstd).
     Each position's choice considers the Huffman bit cost of each option.
     Requires a two-pass encode (first pass builds frequency table for costs,
     second pass finds optimal parse given those costs).
     No format change needed.

  E. Window extension beyond 256 KB (format change required)
     Most long-distance matches are captured at 256KB already.
     Diminishing returns; only worthwhile for very large files.

  F. Context-adaptive literal coding (encoding research, ~2-5% gain)
     Second-order context model for literals after match vs after literal.
     Would require a format version bump.
""")


if __name__ == '__main__':
    main()
