# Recovering Damaged Archives: `boltpack-recover`

`boltpack-recover` is a separate program for archives and volume sets that the
BoltPack CLI will not read. It reads format 7 only (see
[specification-2.0.md](specification-2.0.md)); format 6 archives need an older
release.

The CLI is strict on purpose: if the file table is damaged or the last volume
is missing it refuses the whole set, and `boltpack test` reports little more
than "truncated or damaged". `boltpack-recover` does the opposite. It reports
exactly what survives, writes what it can verify, keeps what it cannot as
clearly marked partial files, and never presents a guess as a fact.

It does four things:

| Command | What it does | Changes the input? |
| --- | --- | --- |
| `analyze` | Reports every volume, entry and byte range it can account for | no |
| `extract` | Writes the entries that can be recovered, partial ones as `NAME.damaged` | no |
| `rebuild` | Writes a new, valid archive (or volume set) from the entries that verified | no |
| `protect`, `check`, `repair` | Adds redundancy to an intact archive beforehand, and repairs damage from it afterwards | no |

None of the commands modifies a volume file. Output goes to a new file or to a
directory that does not exist or is empty.

## Building

```sh
make recover            # build/recover/boltpack-recover
make win-recover        # build/win32/boltpack-recover.exe and build/win64/boltpack-recover.exe (MinGW)
```

The tool is C11 with no dependencies beyond the repository's own core. All
operating system access is in one small platform file per OS
(`src/tools/recover/bpr_platform_posix.c`, `bpr_platform_win.c`).

## Quick start

```sh
# What is left?
boltpack-recover analyze broken.bolt

# Save what can be saved (the directory must not exist or must be empty)
boltpack-recover extract -o rescued broken.bolt

# A volume set: give every file, in any order; files are identified by their headers
boltpack-recover extract -o rescued set.bolt set.b01 set.b02 set.b03

# A new valid archive from the entries that verify
boltpack-recover rebuild -o clean.bolt broken.bolt

# Before anything goes wrong: protect a set with 10% parity
boltpack-recover protect -r 10 archive.bolt           # writes archive.bpr
# After damage: see what is wrong, then repair into a new directory
boltpack-recover check archive.bpr archive.bolt
boltpack-recover repair -o repaired archive.bpr archive.bolt
```

## Reading damaged archives

### How it finds things

A format 7 archive is made of self-describing, checksummed structures: volume
headers and trailers, entry headers, blocks, entry trailers, the file table and
the end record. The tool reads each volume, follows the structures from the
start where they are intact, and scans for the next valid structure where they
are not. It then reconciles what it found with the file table, if the table
verifies.

A file name is never trusted on its own. Volumes are identified by their set ID
and volume number from the header, then the trailer, and only then by file name
(`.bolt`, `.b01`, ...). Volumes of another set, duplicates and unreadable files
are reported and set aside.

### What "verified" means

For every entry the report says how far the tool got:

| Outcome | Meaning |
| --- | --- |
| `verified` | Every block's CRC and decoding passed, and the payload checksum matches the entry trailer. Written under its real name. |
| `verified-by-table` | As above, with the checksum confirmed by the trusted file table because the entry trailer was lost. |
| `complete-unverified` | Every block decoded but no checksum was left to compare with. |
| `created` | A directory or symlink entry. |
| `partial` | Some blocks were lost. Written as `NAME.damaged`, full length, lost bytes zero-filled. The report gives the byte ranges that are `ok` and those that are `lost`, with a reason for each. |
| `skipped-partial` | Nothing useful could be placed (for example a missing volume). Reason given. |
| `skipped-suspect` | Looks like an entry but lies inside another entry's data (a nested archive). Not written, to avoid inventing files. |
| `skipped-supplementary` | Resource-fork and metadata entries: not extracted by name (`rebuild` keeps them). |
| `skipped-duplicate-name`, `no-header`, `failed` | See the reason in the report. |

A `.damaged` file is never presented as the real file. A byte reported `ok` in
a partial file is a byte the tool decoded and checked; a byte it could not
establish is `lost`, not guessed.

Fluff compression keeps a history across blocks, so a lost block makes later
bytes that were copied from it unknowable. The tool decodes the rest twice with
different stand-ins for the hole and reports a byte `ok` only if both agree.
On ordinary text most of what follows a lost Fluff block ends up `lost`
(reason `fluff-taint`); the tool does not hide that. `--taint-min-run N`
controls how short an `ok` run between tainted runs can be before it is
reported as tainted too (default 64).

### `rebuild`

`rebuild` copies the verified entries, including resource-fork and metadata
entries that `extract` cannot restore, into a new archive that the CLI accepts
(`boltpack test`, `list`, `extract`). Blocks are copied, not recompressed,
except where a volume boundary splits one. With `--volume-size N` (suffixes
`K`, `M`, `G`) the result is a volume set. Entries that are not whole are left
out and listed. The output file must not exist.

### Options

| Option | Meaning |
| --- | --- |
| `-o DIR` / `-o OUT` | Extraction directory (`extract`) or new archive (`rebuild`) |
| `--volume-size N` | `rebuild`: volumes of at most N bytes |
| `--format text\|json` | Report format (default `text`) |
| `--report FILE` | Write the report to FILE |
| `--set-id HEX` | Use only volumes of this set when files of several sets are given |
| `--taint-min-run N` | See above |

### Reports and exit status

`--format json` is the interface for scripts; the schema (version 1) is
documented at the top of `src/tools/recover/bpr_report.c`. The text report is
for people and may change.

| Status | `analyze`, `extract`, `rebuild` |
| --- | --- |
| 0 | The set is complete and everything verified: nothing needed recovering |
| 1 | Something was found but the set is damaged or incomplete |
| 2 | No entry was found |
| 3 | Usage or I/O error |

### Safety

- The output directory must not exist or must be empty. Files are created
  exclusively and never follow a symlink; symlink targets that escape the
  output directory are refused.
- A name is recognised as an entry only if it satisfies the archive name rules
  of the specification (§2.3.1: no traversal, no absolute paths); collisions
  with a name already written are refused before anything is written.
- Output is written path by path, not through held directory handles as the
  CLI's extractor does. Do not run it against untrusted archives into a
  directory that a hostile local user can rename things in while it runs.
- A set that is not whole is reported as such; the tool does not fill in a
  missing volume or a missing file table.

## Recovery files: `protect`, `check`, `repair`

Recovery needs luck. A recovery file removes the luck for the damage it covers.
`protect` computes Reed-Solomon parity over an intact archive or volume set and
writes it to a separate `.bpr` file. The archive itself is never changed, so a
protected archive is an ordinary archive that every BoltPack program reads.

```sh
boltpack-recover protect -r 10 [-o SET.bpr] [--sector-size N] VOLUME...
boltpack-recover check  SET.bpr [VOLUME...]
boltpack-recover repair -o DIR SET.bpr VOLUME...
```

`protect` needs the whole set (it learns the volume order from the archive) and
warns if the set is already damaged; it then protects it as it is. `-r` is the
percentage of the data covered, 0.1 to 100. The default file name is the first
volume's name with its extension replaced by `.bpr`. An existing file is never
replaced. `--sector-size` (default 64K, a multiple of 8) is the unit that is
checked and repaired.

### What is guaranteed

The data is cut into sectors, and sectors are spread over groups of up to about
230 with `r` parity sectors each (`-r 10` gives 24 per group of 231). The
guarantee is exactly this:

- any damage that leaves **at most `r` bad sectors in every group** is repaired
  byte for byte;
- any **contiguous** damage, such as a bad region of a disk, a truncated or
  lost volume file, a damaged header or trailer, up to the requested percentage
  of the data is repaired;
- **scattered** damage is repaired only up to roughly a quarter to a half of the
  percentage, because a group fails when more than `r` of its own sectors are
  bad. For a 1 GiB archive with `-r 10`, random damage to about 2.6% of the
  sectors is repaired with a failure risk below one in a million, 4.5% with a
  risk of one in a hundred.
- Damage to the recovery file's own parity sectors counts against the budget of
  its group. The recovery file keeps its header and sector table twice, so
  damage to one copy does not matter.

`check` prints the margin of the worst group, so you can see how close a set is
to the limit.

### What `repair` does

`repair` writes repaired copies of the damaged or missing volume files into a
new directory, under the names recorded in the recovery file. Given files are
matched to volumes by their checksums, not by name, so renamed or shuffled
files work; a file that matches nothing is ignored and reported. It writes
nothing unless every damaged group can be repaired (then it exits 2 and you
fall back to `extract` and `rebuild`). Every repaired sector is verified
against the recovery file after writing, and the tool then analyses the result
itself.

| Status | `check`, `repair` |
| --- | --- |
| 0 | Nothing wrong (`check`) or repaired and the archive verifies (`repair`) |
| 1 | Repairable damage found (`check`), or restored to the protected state, which was itself damaged (`repair`) |
| 2 | Beyond repair |
| 3 | Usage error, unusable recovery file, I/O error, or an ambiguous file |

### Speed and memory

The parity arithmetic is GF(2^8). A portable table kernel is always available;
SSSE3 and AVX2 kernels are used when the CPU has them. On one core, a 256 MiB
archive takes about 1.3 s to protect and 1.7 s to repair a 9% burst with AVX2,
4.3 s and 4.5 s without. Memory is a few sectors per group (12 MiB at the
default sector size). `BPR_GF_KERNEL=scalar|ssse3|avx2` forces a kernel.

## Windows

`boltpack-recover.exe` (32 and 64 bit, built with MinGW) behaves like the Linux
build with the Windows rules of the CLI:

- names Windows cannot hold (reserved device names, forbidden characters,
  trailing dot or space, names that collide ignoring case or Unicode form, paths
  of 260 characters or more) are reported as `skipped-host-name` with the reason
  and are not written; the first entry actually written keeps a contested name;
- symlink entries are not extracted (as in the CLI): `skipped-symlink`;
- Unix mode bits are not restored; modification times are.

## Limits

- **Format 7 only.** Archives with a wrong `format_version` are not repaired.
- **Headerless entries.** An entry whose header is in a lost volume is not
  recovered by name.
- **Nested archives.** If an archive stored inside another loses both its
  holding entry's header and the file table, the inner archive's entries can be
  reported (and extracted) as real files of the outer one.
- **Fluff.** A lost block taints most of what follows in the same entry (see
  above); a Fluff entry whose first block is lost yields little.
- **Two faults in one entry.** A block-size field overwritten with exactly the
  block size, together with a second unknown size in the same entry, can make
  wrong bytes be reported `ok`. Random damage does not produce this.
- **Recovery files** protect against the damage they were sized for; they are no
  substitute for a second copy.
- **Evidence.** The Windows builds are tested under Wine, not on real Windows.
  Archives over 4 GiB are untested on the 32-bit build. Sets of close to 1,000
  volumes are untested.

## Tests

```sh
make recover-test            # ~1.5 min: the recovery tool against generated, damaged archives
make recover-sidecar-test    # seconds: protect, check, repair
make win-recover-test        # ~11 min: both Windows builds under Wine, the same suites
```

The tests generate their own archives, damage them in planned ways and compare
the result with ground truth that comes from independent code
(`test/bp_set_bytes.py`, `test/sidecar_model.py`, `test/fluff_oracle.py`), never
from the tool under test. Sanitizer builds (AddressSanitizer, UBSan) were run
on the same suites. See `test/check_recover.py` and `test/check_sidecar.py`.
