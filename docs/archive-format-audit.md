# Archive Format Portability and Attribute Preservation Audit

**Audit date:** 2026-10-26  
**Format reviewed:** archive format 5  
**Scope:** on-disk format, core extraction behavior, and representative platform frontends.

This is a historical audit of format 5. Format 6 implementation has begun;
see the [plan's status](archive-format-plan.md) for its current progress.

## Summary

BoltPack is suitable for transferring ordinary file contents between supported
systems when the destination supports the archive's compression method. The
format uses fixed little-endian fields, relative slash-separated paths, and
CRC-32 checks for the archive header, file table, compressed blocks, and
uncompressed entry data.

BoltPack does not currently provide full-fidelity filesystem replication.
Filename conversion varies by frontend, common metadata is limited, and
platform-specific metadata is opaque to other platforms. Treat it as a
cross-platform file-transfer archive with partial metadata preservation. The
[interoperability plan](archive-format-plan.md) defines the proposed steps to
make file and attribute transfer between systems predictable.

This is a static code and documentation audit. No cross-platform round-trip
matrix or target-system extraction tests were run for this report.

## Findings

### 1. Some writers do not meet the UTF-8 filename contract

The format specifies UTF-8 entry names and says frontends convert from native
encodings before writing. The 16-bit mini frontends do not link the codepage
conversion module. The mini archive writer writes the supplied name bytes
verbatim, and its display helper replaces non-ASCII bytes with `?`. CP/M-86
also flattens names to uppercase 8.3 form.

As a result, ASCII names are reliable across these systems; non-ASCII names
from DOS/Win16/OS/2-16 are not guaranteed to be valid UTF-8, and CP/M names are
necessarily restricted. The current mini design-goal table says archive paths
are UTF-8, so the implementation and documentation disagree.

References: [format encoding rule](specification-1.0.md#73-character-encoding),
[mini build goals](mini.md#design-goals),
[mini name serialization](../src/core/mini/archiver.c),
[CP/M name normalization](../src/frontend/cpm86/main.c).

### 2. Directory modes and modification times are recorded but not restored on Linux

The Linux writer records mode flags and modification time for directories;
the mode flags are the 12 permission and set-ID bits (`st_mode & 0x0FFF`),
not the full file-type-inclusive `st_mode`. During extraction, the Linux
frontend creates the directory with a fixed `0755` mode and returns `NULL`
instead of an output stream. The archive core invokes the completion callback
only when both a callback is registered and an output stream exists. The
code that applies mode and time is in that callback, so directory mode and
modification time are never restored. Creating child files also changes the
directory's modification time.

Regular Linux files do restore the archived permission bits and modification
time. Symlink targets are stored and relative safe links can be recreated, but
the symlink completion path returns before applying its archived mode or time.
The regular-file restore uses `chmod()` and `utime()`, which follow symlinks,
so those calls could not restore link attributes as they stand.

References: [Linux writer captures mode/time](../src/frontend/linux/main.c),
[directory extraction](../src/frontend/linux/main.c),
[completion callback condition](../src/core/archiver.c).

### 3. Generic attribute preservation is limited; platform metadata is opaque

The common entry header stores a modification time in whole seconds and an
optional 12-bit Unix mode value. There is no common portable representation for
access time, owner/group IDs, ACLs, or arbitrary extended attributes. POSIX
extraction sets both access time and modification time to the archived
modification time, so the original access time is not retained.

`BP_TYPE_META` carries platform-tagged payloads. Examples include Amiga
protection bits and comments, Atari attributes, RISC OS load/exec data, Haiku
MIME types, and OS/2 attributes. These payloads are meaningful to their
corresponding frontend; most other extractors skip them. Mac resource forks
and Finder information have similar platform limits. This is useful for
same-platform restoration of selected attributes, but it is not a cross-OS
attribute mapping layer.

References: [entry fields and types](specification-1.0.md#23-file-entries),
[platform metadata examples](frontends.md).

### 4. Windows attribute support is overstated in the frontend summary

The frontend guide lists FAT attributes for the Windows 32/64 CLI and the
Windows 3.1 CLI. In the current Windows 32/64 CLI path, archive creation
writes entry names, contents, and modification times; it does not emit
Windows attribute metadata. Extraction skips `BP_TYPE_META` and restores
modification time only. The other Windows frontends behave the same way:
the Windows 3.1 CLI, the Win32 GUI, the FLTK GUI, the .NET GUI, and the
Total Commander plugin read file attributes only to detect directories and
neither serialize nor restore attribute metadata. Read-only, hidden,
system, and archive flags therefore do not round-trip through any current
Windows path.

References: [Windows frontend summary](frontends.md),
[Windows writer](../src/frontend/win/main.c),
[Windows metadata skip and timestamp restore](../src/frontend/win/main.c).

### 5. A zero timestamp is ambiguous

CP/M-86 has no timestamp API and writes zero for `mtime`. Linux and Windows
extractors pass the stored value to `utime()` without treating zero as
“unknown”; an entry created on CP/M can therefore receive the Unix epoch as
its extracted timestamp. The format has no `mtime_valid` flag to distinguish
an absent time from the valid instant 1970-01-01 00:00:00 UTC. Timestamp
precision is also limited to whole seconds by the format.

References: [CP/M timestamp handling](../src/frontend/cpm86/main.c),
[Linux timestamp restore](../src/frontend/linux/main.c),
[Windows timestamp restore](../src/frontend/win/main.c).

### 6. The normative specification had a version contradiction

The specification defines format version 5 and later requires readers and
writers to use version 5. Its encoder conformance section said writers should
set version `0x0001`. An independent implementation following that instruction
would produce an archive current readers reject. This audit corrected that
instruction to `0x0005` and updated the specification's software-version
header to 1.9. The specification remains marked as Development. At the time
of writing this correction and the present audit exist only as working-tree
changes; the checked-in history still carries the contradictory instruction,
so the specification update and this report should be committed together.

Reference: [conflicting encoder instruction](specification-1.0.md#92-encoder-conformance),
[version 5 policy](specification-1.0.md#102-archive-version-policy).

## Compatibility policy

Readers currently accept format version 5 only. Because BoltPack is not widely
used, support for earlier archive versions is not required at this stage. The
strict policy can remain. If the on-disk layout changes, update the normative
specification and use an appropriate new format version; a backward reader or
migration tool is optional unless product needs change.

Compression-method support still varies by build. For exchange with vintage
mini targets, Stored is the broadest baseline. BoltLite is supported by the
mini writer and reader. Mini builds do not support Fluff or zstd, and 16-bit
mini builds decode BoltLZ but cannot encode it.

## Assessment

| Transfer requirement | Assessment |
| --- | --- |
| File contents and accidental-corruption detection | Good |
| ASCII relative filenames | Good, subject to destination filesystem limits |
| Unicode filenames across all frontends | Incomplete; do not rely on non-ASCII names from 16-bit mini systems |
| Regular-file modification time | Partial; whole-second precision and source/destination limits apply |
| Regular-file permissions | Partial; primarily POSIX-to-POSIX |
| Directory permissions and times | Not currently restored by the Linux extractor |
| ACLs, ownership, access times, and arbitrary xattrs | Not generally preserved |
| Platform-specific metadata | Selected same-platform restoration; commonly skipped elsewhere |
| Symlinks | Supported by selected POSIX frontends; unsupported or skipped by others |
| Older archive versions | Intentionally unsupported under the current policy |

## Interoperability plan

The revised implementation plan is maintained in
[archive-format-plan.md](archive-format-plan.md). It proposes format 6, explicit
extraction outcomes, strict filename conversion, and a profile bounded by mini
resource limits. Earlier archive versions need not be supported. These are
planned changes; the findings above describe the current format-5 implementation.
