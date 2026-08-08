#!/usr/bin/env python3
"""Analyze Fluff compression across corpus files.

Compresses files from a corpus, extracts their Fluff bitstreams, and reports
per-file and per-subset ratio summaries. Supports named subsets, arbitrary
directories, and manifest files so experiments can use a stable file list.

Usage examples:
    python3 src/tools/analyze_corpus.py --subset silesia
    python3 src/tools/analyze_corpus.py --manifest fluff-fast
    python3 src/tools/analyze_corpus.py --corpus-path /some/corpus/root
"""

import argparse
import os
import subprocess
import sys
import tempfile
from collections import defaultdict


def _detect_corpus_base():
    candidates = []
    env_path = os.environ.get('BOLTPACK_CORPUS')

    if env_path:
        candidates.append(env_path)

    candidates.extend([
        '/run/media/tony/Volume1/corpus',
        '/var/run/media/tony/Volume1/corpus',
    ])

    for path in candidates:
        if os.path.isdir(path):
            return path

    return candidates[0]


_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(os.path.dirname(_TOOLS_DIR))
_CORPUS_BASE = _detect_corpus_base()
_MANIFEST_DIR = os.path.join(_REPO_ROOT, 'test', 'corpus-manifests')

KNOWN_MANIFESTS = {
    'fluff-fast': os.path.join(_MANIFEST_DIR, 'fluff-fast.txt'),
    'fluff-regression': os.path.join(_MANIFEST_DIR,
                                     'fluff-regression.txt'),
}

DEFAULT_SUBSETS = [
    ('silesia', 'silesia'),
    ('godot_cpp', os.path.join('curated', 'godot_cpp')),
    ('godot_shaders', os.path.join('curated', 'godot_shaders')),
    ('odoo_python', os.path.join('curated', 'odoo_python')),
    ('odoo_xml', os.path.join('curated', 'odoo_xml')),
]


def analyze_bitstream_silent(data):
    """Read the Fluff block header and tree counts without printing."""
    try:
        from analyze_bitstream import BitReader

        br = BitReader(data)

        magic = br.get_bits(16)
        if magic != 0x4C46:
            return None

        version = br.get_bits(8)
        window_bits = br.get_bits(6)
        num_ll = br.get_bits(5) + 257
        num_d = br.get_bits(6) + 1
        hclen = br.get_bits(4) + 4

        return {
            'magic': magic,
            'version': version,
            'window_bits': window_bits,
            'num_ll': num_ll,
            'num_d': num_d,
            'hclen': hclen,
        }
    except Exception:
        return None


def path_under_root(path, root):
    if root is None:
        return False

    try:
        return os.path.commonpath([os.path.abspath(path),
                                   os.path.abspath(root)]) == \
               os.path.abspath(root)
    except ValueError:
        return False


def display_path(path, display_root=None):
    abs_path = os.path.abspath(path)

    if display_root is not None and path_under_root(abs_path, display_root):
        return os.path.relpath(abs_path, display_root)

    return os.path.basename(abs_path)


def find_files(root, max_files=None, display_root=None):
    result = []

    for dirpath, _dirs, files in os.walk(root):
        for fname in sorted(files):
            abs_path = os.path.join(dirpath, fname)
            if os.path.isfile(abs_path):
                result.append((display_path(abs_path,
                                            display_root or root),
                               abs_path))

    result.sort(key=lambda item: item[0])
    if max_files is not None:
        result = result[:max_files]
    return result


def resolve_manifest_path(manifest_arg):
    candidates = []

    if manifest_arg in KNOWN_MANIFESTS:
        candidates.append(KNOWN_MANIFESTS[manifest_arg])

    candidates.append(manifest_arg)
    candidates.append(os.path.join(_MANIFEST_DIR, manifest_arg))
    if not manifest_arg.endswith('.txt'):
        candidates.append(os.path.join(_MANIFEST_DIR, manifest_arg + '.txt'))

    for candidate in candidates:
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)

    raise FileNotFoundError(
        f'manifest not found: {manifest_arg!r} '
        f'(built-ins: {", ".join(sorted(KNOWN_MANIFESTS))})'
    )


def resolve_manifest_entry(entry, manifest_path, corpus_root):
    candidates = []

    if os.path.isabs(entry):
        candidates.append(entry)
    else:
        if corpus_root:
            candidates.append(os.path.join(corpus_root, entry))
        candidates.append(os.path.join(os.path.dirname(manifest_path), entry))

    for candidate in candidates:
        if os.path.exists(candidate):
            return os.path.abspath(candidate)

    return os.path.abspath(candidates[0])


def load_manifest_files(manifest_path, corpus_root, max_files=None):
    result = []
    seen = set()

    with open(manifest_path, 'r', encoding='utf-8') as handle:
        for lineno, raw_line in enumerate(handle, 1):
            entry = raw_line.strip()

            if not entry or entry.startswith('#'):
                continue

            resolved = resolve_manifest_entry(entry, manifest_path, corpus_root)

            if os.path.isdir(resolved):
                entry_files = find_files(resolved, display_root=corpus_root)
                if not entry_files:
                    raise ValueError(
                        f'manifest entry {lineno} has no files: {entry}'
                    )
                for item in entry_files:
                    if item[1] not in seen:
                        seen.add(item[1])
                        result.append(item)
                continue

            if os.path.isfile(resolved):
                if resolved not in seen:
                    seen.add(resolved)
                    result.append((display_path(resolved, corpus_root),
                                   resolved))
                continue

            raise FileNotFoundError(
                f'manifest entry {lineno} not found: {entry}'
            )

    if max_files is not None:
        result = result[:max_files]

    return result


class CorpusAnalyzer:
    def __init__(self, corpus_root, boltpack_cmd):
        self.corpus_root = corpus_root
        self.boltpack_cmd = boltpack_cmd
        self.results = defaultdict(list)
        self.errors = []

    def analyze_file(self, filepath):
        archive_path = None
        block_path = None

        try:
            with tempfile.NamedTemporaryFile(suffix='.bolt', delete=False) as tmp:
                archive_path = tmp.name

            result = subprocess.run(
                [self.boltpack_cmd, 'c', '-q', '-F', archive_path, filepath],
                capture_output=True,
                text=True,
                timeout=120,
            )
            if result.returncode != 0:
                msg = result.stderr.strip() or result.stdout.strip()
                self.errors.append(
                    f'{filepath}: compression failed: {msg or "unknown error"}'
                )
                return None

            with tempfile.NamedTemporaryFile(suffix='.raw', delete=False) as tmp:
                block_path = tmp.name

            extract_script = os.path.join(_TOOLS_DIR, 'extract_fluff_block.py')
            result = subprocess.run(
                ['python3', extract_script, archive_path, block_path],
                capture_output=True,
                text=True,
                timeout=30,
            )
            if result.returncode != 0:
                msg = result.stderr.strip() or result.stdout.strip()
                self.errors.append(
                    f'{filepath}: extraction failed: {msg or "unknown error"}'
                )
                return None

            with open(block_path, 'rb') as handle:
                bitstream = handle.read()

            bs_info = analyze_bitstream_silent(bitstream)
            if not bs_info:
                self.errors.append(f'{filepath}: bitstream analysis failed')
                return None

            input_size = os.path.getsize(filepath)
            compressed_size = os.path.getsize(archive_path)

            return {
                'file': os.path.basename(filepath),
                'input_size': input_size,
                'compressed_size': compressed_size,
                'ratio': (100.0 * compressed_size / input_size
                          if input_size > 0 else 0.0),
                'bitstream_info': bs_info,
            }
        except subprocess.TimeoutExpired:
            self.errors.append(f'{filepath}: timeout')
            return None
        except Exception as exc:
            self.errors.append(f'{filepath}: {exc}')
            return None
        finally:
            if block_path and os.path.exists(block_path):
                os.unlink(block_path)
            if archive_path and os.path.exists(archive_path):
                os.unlink(archive_path)

    def analyze_paths(self, label, files):
        print('\n' + '=' * 70)
        print(f'ANALYZING: {label}')
        print('=' * 70)
        print(f'Found {len(files)} files\n')

        results = []
        total_files = len(files)

        for index, (display_name, filepath) in enumerate(files, 1):
            size_mb = os.path.getsize(filepath) / (1024 * 1024.0)
            print(f'  [{index:2d}/{total_files}] {display_name:40s} '
                  f'({size_mb:6.1f} MB)...', end='', flush=True)

            result = self.analyze_file(filepath)
            if result:
                results.append(result)
                print(f' {result["ratio"]:6.2f}%')
            else:
                print(' FAIL')

        self.print_subset_summary(label, results)
        self.results[label] = results

    def analyze_subset(self, label, subset_path, max_files=None):
        if not os.path.isdir(subset_path):
            print(f"Error: subset '{label}' not found at {subset_path}")
            return

        files = find_files(subset_path, max_files=max_files,
                           display_root=self.corpus_root)
        self.analyze_paths(label, files)

    def print_subset_summary(self, label, results):
        if not results:
            print(f'  No successful analyses for {label}')
            return

        total_input = sum(item['input_size'] for item in results)
        total_output = sum(item['compressed_size'] for item in results)
        total_ratio = (100.0 * total_output / total_input
                       if total_input > 0 else 0.0)
        avg_ratio = sum(item['ratio'] for item in results) / len(results)
        min_ratio = min(item['ratio'] for item in results)
        max_ratio = max(item['ratio'] for item in results)

        print('\n  Summary:')
        print(f'    Files analyzed:         {len(results)}')
        print(f'    Total input:            {total_input / (1024 * 1024.0):.1f} MB')
        print(f'    Total output:           {total_output / (1024 * 1024.0):.1f} MB')
        print(f'    Overall ratio:          {total_ratio:.2f}%')
        print(f'    Avg file ratio:         {avg_ratio:.2f}% '
              f'(min {min_ratio:.2f}%, max {max_ratio:.2f}%)')

    def print_global_summary(self):
        print('\n' + '=' * 70)
        print('GLOBAL SUMMARY')
        print('=' * 70)

        all_results = []
        for subset_results in self.results.values():
            all_results.extend(subset_results)

        if not all_results:
            print('No results to summarize')
            return

        total_input = sum(item['input_size'] for item in all_results)
        total_output = sum(item['compressed_size'] for item in all_results)
        total_ratio = (100.0 * total_output / total_input
                       if total_input > 0 else 0.0)
        avg_ratio = sum(item['ratio'] for item in all_results) / len(all_results)
        best = min(all_results, key=lambda item: item['ratio'])
        worst = max(all_results, key=lambda item: item['ratio'])

        print(f'\n  Total files analyzed:   {len(all_results)}')
        print(f'  Total input size:       {total_input / (1024 * 1024.0):.1f} MB')
        print(f'  Total output size:      {total_output / (1024 * 1024.0):.1f} MB')
        print(f'\n  Overall ratio:          {total_ratio:.2f}%')
        print(f'  Average ratio:          {avg_ratio:.2f}%')
        print(f'\n  Best compression:       {best["file"]:30s} '
              f'({best["ratio"]:6.2f}%)')
        print(f'  Worst compression:      {worst["file"]:30s} '
              f'({worst["ratio"]:6.2f}%)')

        if self.errors:
            print(f'\n  Errors encountered: {len(self.errors)}')
            for err in self.errors[:5]:
                print(f'    - {err}')
            if len(self.errors) > 5:
                print(f'    ... and {len(self.errors) - 5} more')


def main():
    parser = argparse.ArgumentParser(
        description='Analyze Fluff compression across corpus files'
    )
    parser.add_argument(
        '--subset',
        help=('Analyze a single subset directory. Relative paths are resolved '
              'from --corpus-path.'),
    )
    parser.add_argument(
        '--manifest',
        help=(
            'Manifest file or built-in manifest name. '
            f'Built-ins: {", ".join(sorted(KNOWN_MANIFESTS))}. '
            'Manifest lines may name files or directories; relative entries '
            'are resolved from --corpus-path.'
        ),
    )
    parser.add_argument('--max-files', type=int,
                        help='Limit to N files per subset or manifest')
    parser.add_argument('--corpus-path', default=_CORPUS_BASE,
                        help=f'Path to corpus root (default: {_CORPUS_BASE})')
    parser.add_argument('--boltpack',
                        default=os.path.join(_REPO_ROOT, 'build', 'linux',
                                             'boltpack'),
                        help='Path to boltpack binary')

    args = parser.parse_args()

    if not os.path.isfile(args.boltpack):
        print(f'Error: boltpack binary not found at {args.boltpack}',
              file=sys.stderr)
        sys.exit(2)

    if not os.path.isdir(args.corpus_path):
        print(f'Error: corpus root not found at {args.corpus_path}',
              file=sys.stderr)
        sys.exit(2)

    analyzer = CorpusAnalyzer(args.corpus_path, args.boltpack)

    if args.manifest:
        try:
            manifest_path = resolve_manifest_path(args.manifest)
            files = load_manifest_files(manifest_path, args.corpus_path,
                                        max_files=args.max_files)
        except (FileNotFoundError, ValueError) as exc:
            print(f'Error: {exc}', file=sys.stderr)
            sys.exit(2)

        label = os.path.splitext(os.path.basename(manifest_path))[0]
        print(f'Corpus root: {args.corpus_path}')
        print(f'Manifest:    {manifest_path}')
        analyzer.analyze_paths(label, files)
    elif args.subset:
        if os.path.isabs(args.subset):
            subset_path = args.subset
            label = os.path.basename(args.subset.rstrip(os.sep))
        else:
            subset_path = os.path.join(args.corpus_path, args.subset)
            label = args.subset

        print(f'Corpus root: {args.corpus_path}')
        analyzer.analyze_subset(label, subset_path, args.max_files)
    else:
        print(f'Corpus root: {args.corpus_path}')
        for label, relative_path in DEFAULT_SUBSETS:
            subset_path = os.path.join(args.corpus_path, relative_path)
            if os.path.isdir(subset_path):
                analyzer.analyze_subset(label, subset_path, args.max_files)

    analyzer.print_global_summary()


if __name__ == '__main__':
    main()