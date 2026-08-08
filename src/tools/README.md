# BoltPack Development Tools

This directory contains utility scripts for analyzing, debugging, and working with BoltPack archives and compressed formats.

These are **development/analysis tools**, not part of the core archiver. They help with:

- Bitstream analysis and visualization
- Format debugging and inspection
- Compression profiling
- Resource fork inspection

## Tools

### `analyze_corpus.py`

Comprehensive corpus analysis tool for measuring compression across diverse file types.

**Usage:**

```bash
python3 analyze_corpus.py [--subset <subset>] [--max-files N] [--corpus-path /path]
```

**Features:**

- Compresses files from corpus with fluff codec
- Extracts and analyzes bitstreams in bulk
- Generates per-file and per-subset statistics
- Measures compression ratio across different data types (code, text, binaries, XML, DNA)

**Examples:**

```bash
# Analyze all corpus subsets (quick: 3 files each)
python3 analyze_corpus.py --max-files 3

# Full Silesia suite
python3 analyze_corpus.py --subset silesia

# Godot C++ code
python3 analyze_corpus.py --subset godot_cpp

# Custom corpus path
python3 analyze_corpus.py --corpus-path /path/corpus --subset silesia
```

**Output Includes:**

- Per-file compression ratio
- Per-subset summary (total size, overall ratio, min/max)
- Global summary across all analyzed files
- Error tracking and reporting

**Example Results:**

```text
Silesia (4 files, 100.1 MB):
  Overall ratio: 27.21%
  Best: nci (DNA) at 8.18%
  Worst: mozilla at 36.55%
  Average: 28.75%
```

**Use Cases:**

- Validate compressor performance on real-world corpora
- Compare compression ratios by file type and data pattern
- Identify optimization opportunities for specific data classes
- Benchmark against gzip and other standards

See [CORPUS-ANALYSIS.md](../../test/CORPUS-ANALYSIS.md) for detailed analysis results.

---

### `analyze_bitstream.py`

Analyze the structure of fluff-compressed bitstreams with bit-level breakdown.

**Usage:**

```bash
python3 analyze_bitstream.py <fluff_block_file>
```

**Features:**

- Parse fluff bitstream header (magic, version, window size)
- Decode Huffman tree structure with code-length RLE support
- Analyze token stream (literals, matches, distances)
- Display bit allocation summary and compression statistics
- Show literal/match distribution and match length statistics

**Workflow Example:**

```bash
# Extract block from archive
python3 extract_fluff_block.py /path/to/archive.bolt

# Analyze the bitstream
python3 analyze_bitstream.py archive_extracted.raw
```

**Output Includes:**

```text
[HEADER]
  Version, window bits

[HUFFMAN TREES]
  Code-length RLE analysis
  Literal/Length distribution
  Distance code distribution
  Tree structure overhead

[TOKEN STREAM]
  Literal count and distribution
  Match count and length statistics
  Distance statistics
  Token stream size analysis

[SUMMARY]
  Total archive size breakdown
  Compression efficiency metrics
```

### `extract_fluff_block.py`

Extract raw compressed blocks from `.bolt` archives for analysis.

**Usage:**

```bash
python3 extract_fluff_block.py <archive.bolt> [output.raw]
```

**Behavior:**

- If output path not specified:
  - Looks for `./raw/` directory; uses `raw/extracted_block.raw` if it exists
  - Otherwise creates `<archive>_extracted.raw` in the same directory as the input
- Handles both single-block and multi-block archive formats
- Dumps the first file entry's compressed data

**Example:**

```bash
# Extract from test directory (creates raw/)
cd test/
python3 extract_fluff_block.py test_large_rep.bolt
# Output: raw/extracted_block.raw

# Extract to specific location
python3 extract_fluff_block.py archive.bolt analysis/myfile.raw
```

### `inspect_rsrc.py`

Inspect Classic Mac resource fork (`.rsrc`) files.

**Usage:**

```bash
python3 inspect_rsrc.py <path/to/file.rsrc>
```

**Features:**

- Parse resource fork file header and map
- List all resource types, IDs, names
- Show resource data sizes
- Validate resource fork structure

**Example:**

```bash
python3 inspect_rsrc.py ../resources/macos9/RESOURCE.FRK/Window.rsrc
```

## Integration with BoltPack CLI

These tools are **companions** to the main `boltpack` command-line tool:

```bash
# Main archiving operations
boltpack c archive.bolt files...          # Create archive
boltpack x archive.bolt                   # Extract archive
boltpack l archive.bolt                   # List contents

# Compression profiling (Linux only)
boltpack T file.txt                       # Test fluff compression, outputs fluff-stats.txt

# Analysis tools (development utilities)
cd src/tools/
python3 extract_fluff_block.py ../archive.bolt
python3 analyze_bitstream.py archive_extracted.raw
```

## Use Cases

### Understanding Compression Efficiency

Analyze where bits are being used:

```bash
python3 extract_fluff_block.py large_file.bolt
python3 analyze_bitstream.py large_file_extracted.raw
# See: header %, tree overhead %, token stream %
```

### Debugging Compression Issues

Verify Huffman tree structure and RLE encoding:

```bash
python3 analyze_bitstream.py suspect_block.raw
# Check if code-length RLE symbols (16, 17, 18) are in use
# Verify tree matches expected format version
```

### Analyzing Compression Patterns

Understand match/literal mix for different input types:

```bash
# Repetitive text
python3 extract_fluff_block.py repetitive.bolt
python3 analyze_bitstream.py repetitive_extracted.raw
# Expected: high match %, long average match length

# Mixed text
python3 extract_fluff_block.py mixed.bolt
python3 analyze_bitstream.py mixed_extracted.raw
# Expected: balanced match/literal ratio

# Low-pattern text
python3 extract_fluff_block.py syslog.bolt
python3 analyze_bitstream.py syslog_extracted.raw
# Expected: high literal %, short average match length, diverse Huffman tree
```

## Finding Tools from Project Root

When working from the BoltPack project root:

```bash
# From project root, run tools directly
python3 src/tools/analyze_bitstream.py test/raw/block.raw

# Or from test directory
cd test/
python3 ../src/tools/extract_fluff_block.py large_file.bolt
python3 ../src/tools/analyze_bitstream.py raw/extracted_block.raw
```

## Development Notes

- **Pure Python3:** No external dependencies required
- **Cross-platform:** Run on Linux, macOS, Windows
- **C89 compatibility analysis:** Tools help verify fluff format compliance
- **Not tested on malformed archives:** Add error handling for fuzzing workflows

## Related Documentation

- [BITSTREAM-ANALYSIS.md](../../test/BITSTREAM-ANALYSIS.md) — Analysis report from Phase D
- [docs/format.md](../../docs/format.md) — `.bolt` archive format specification
- [docs/architecture.md](../../docs/architecture.md) — BoltPack architecture and layers
- [test/README.md](../../test/README.md) — Test suite documentation
