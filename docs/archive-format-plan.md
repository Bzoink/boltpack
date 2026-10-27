# Archive Format Interoperability Plan

**Reviewed:** 2026-10-27

**Status:** implementation in progress; the core and specification use format 6.  
**Basis:** [archive format audit](archive-format-audit.md).

This replaces the interoperability plan previously embedded in the audit. Its
goal is predictable transfer: preserve file bytes, preserve representable names
and attributes, and report every unsupported or lossy operation. Identical
filesystem results on every OS are not achievable: CP/M has no directories,
legacy filesystems cannot represent every Unicode name, and some systems have
no equivalent for a resource fork or a POSIX permission bit.

The format-6 core, timestamp validity flag, UTF-8 and canonical path validation,
and file-table consistency checks are implemented in both full and mini cores.
Supplementary metadata and resource forks must follow one exact file or
directory subject, and resource forks are unique within that group. Mini local
entry helpers now append the required directory slash and give portable FAT
metadata the same canonical directory path; Linux mini's direct core writer
does the same. Linux mini now validates all projected primary paths before
creating the destination tree, refusing file/directory prefix conflicts and
overlong components. The QEMU interop fixture checks both archive entry orders
and confirms an existing destination tree is unchanged. Format-6 tests cover
the common helper, Linux mini runtime, and a DOS16 nested-directory transfer.
Mini frontends have explicit name
conversion. The Linux CLI, Linux mini, Windows CLI, full Atari CLI, and full
OS/2 CLI stage regular-file output until checksums pass. Linux full and mini
frontends validate the complete path table before creating the destination
tree, refusing overlong components and file/directory prefix conflicts; Linux
uses directory-relative operations. EMX OS/2 checks existing components with
`lstat`, while Watcom OS/2 relies on pathname APIs. The Windows CLI now writes
and restores portable FAT flags, while deferring directory flags until
children finish. The Linux FLTK GUI now uses the same directory-relative
no-follow traversal, stages regular files and symlinks beside their
destinations, commits after checksum verification, and restores file and
deferred directory mode/time metadata. Unsupported metadata and resource forks
produce a partial result. This frontend has build evidence but still needs
interactive runtime tests. This plan remains incomplete: other frontends and wrappers
still need comparable metadata and extraction lifecycle coverage. The full DOS
frontend now uses a bounded CP850 projected-name preflight and status-aware
staged file extraction. It checks close and checksum status before publication,
removes failed stages, uses a recovery name during forced replacement, and
reports skipped resource forks and symlinks as partial. Create, add, and delete
write exclusive sibling stages; add copies the original before appending, and
all three keep an existing archive under a recovery name until publication
succeeds. They check target identity before publication, refuse multiply linked
archive targets, preserve known archive FAT flags on DJGPP, and retain the
original on handled write, close, or publication failures. The host fixture
injects those failures for create/add/delete. File extraction captures and
restores portable FAT flags with DJGPP's DOS attribute API; directory flags
are deferred until children finish. DJGPP's timestamp API cannot set directory
times on plain DOS, so the frontend reports those restores as partial. The
reproducible
`make dos-host-test` target validates frontend logic with a host build,
including CP850 case aliases, hidden/system name flags, read-only file and
directory restoration, and injected write/close failures, but does not
validate DOS APIs. Its archive-edit fixtures also check byte-for-byte
preservation and cleanup after write, close, and publication failures.
`make dos-djgpp` is the actual cross-build target, but it skips here because
the DJGPP compiler is unavailable; no DOS runtime fixture is available either.
The full DOS CLI's path-based publication has no protection against a
concurrent parent-directory change. Generated short-name aliases on systems
without long filename support are not modeled by its preflight. The Windows CLI
refuses case-insensitive destination collisions and common reserved or
trailing-dot/space names; other destination mappings and Unicode normalization
remain open. The full Atari CLI now validates the base GEMDOS 8.3 ASCII path
profile and rejects case-folded path collisions before output changes. Its
index is capped at 1,024 primary entries and 32 KiB of path keys; larger
archives return partial after that bound. This targets the original GEMDOS
profile, whose filename rules are documented in the [Atari ST GEM Programming
manual](https://www.bitsavers.org/pdf/atari/ST/Atari_ST_GEM_Programming_1986/GEM_0042.pdf).
The Windows CLI has cross-build evidence but still needs Windows
runtime tests. The full Atari CLI now writes native and portable FAT metadata,
defers directory time and attribute restoration, and stages extracted files
before checksum-verified commit. It checks existing directory components for symlinks
and uses no-replace GEMDOS renames; forced replacement keeps a recovery copy
until the new file is committed. Atari name projection rejects non-ASCII,
non-8.3, and over-125-byte paths, and refuses case-insensitive collisions.
Atari create/add/delete now use exclusive sibling archive stages. Add streams
the original archive into a stage before in-place append on that stage, keeping
memory bounded by the core's normal append storage; delete no longer uses a
fixed temporary name or removes the original before replacement. Publication
backs up a regular target, checks its identity again after the backup rename,
restores it if publishing fails, and preserves hidden/system/archive flags.
Read-only archive targets are refused. A crash during staging leaves the
original intact; a crash between backup and publication can leave the original
at the recovery name. `--interchange-profile-1` is available for create/add.
The Atari cross-build passes, but runtime and interruption tests still need an
Atari emulator or hardware. Path-based TOS calls cannot prevent a concurrent
rename of a checked parent. Full OS/2 now writes native attributes and the portable
FAT envelope for files and directories, applies native attributes ahead of
portable fallback, and restores directory timestamps and flags deepest first.
Its bounded queue holds 256 directories and 32 KiB of paths. Extraction first
checks projected names against the destination code page and filesystem; FAT
targets require 8.3 components, while other OS/2 filesystems use OS/2 case
mapping to reject collisions. Regular files are written to exclusive sibling
temporary files. After checksum verification OS/2 reserves the final filename
with exclusive-create and copies the verified stage; forced replacement keeps
a recovery copy and rolls it back if publication fails. OS/2 timestamps use
the fixed `BP_TZ_OFFSET` setting. The 32-bit Watcom compiler compiled the OS/2
frontend and core and linked an LX executable with `make os2-watcom`, but no
OS/2 runtime is available here. A process interruption during the final copy
can leave a partial destination. OS/2 pathname operations still cannot prevent
a concurrent parent substitution. HPFS `.TYPE` extended attributes are not
transferred. No Atari or OS/2 runtime round trip has been run.
OS/2 create and delete now write to exclusive sibling staging files and publish
only after successful archive finalization, flush, and close. Replacing an
existing archive first moves it to a recovery name and restores it if stage
publication fails. A process interruption between the two rename operations
can leave the original at that recovery path; interruption while writing a
stage can leave an orphan temporary file while the original remains intact.
These edit paths have Watcom cross-build evidence, but still need OS/2 runtime
qualification.
OS/2 append now copies the original into a unique sibling stage and appends
there, so handled failures leave the original untouched without a bounded tail
backup. It refuses read-only archives, blocks concurrent writers while copying,
checks the opened source and path status before publication, and retains the
archive's OS/2 attribute flags plus creation/access timestamps; last-write time
reflects the append. Publication uses the same backup-and-recovery rename as
create/delete, so interruption between those renames can leave the original at
the recovery path. The pathname checks cannot exclude every concurrent
replacement race. Watcom cross-build evidence passes, but OS/2 runtime and
interruption tests remain unavailable. The 32-bit Watcom build's addressable
archive size remains below 2 GiB.
DOS16, Win16, and OS/2-16 also require an explicit fixed timezone offset for
timestamp conversion; other DOS-derived frontends still need review. Their
mini extraction callbacks now remove output after a data or close failure.
DOS16 and Win16 close the verified output before reopening it to restore a
timestamp; OS/2-16 applies timestamps only after close succeeds. All three
16-bit targets cross-build. Extraction now preflights the complete archive
name table on all three targets, rejecting projected case aliases, parent
directory conflicts, and file/directory conflicts before creating output. It
uses ASCII folds plus CP437/CP850 letter maps, and refuses non-ASCII names on
other configured codepages. The index is bounded to 128 distinct projected
path prefixes and 4 KiB of key storage. DOSBox validates DOS16 nested-directory
and portable-metadata transfer, timezone conversion, cleanup after a corrupt
block, and pre-output refusal of case-only directory aliases; it does not
qualify Win16 or OS/2-16 runtimes. The Linux
CLI and Linux mini, plus FreeBSD, NetBSD, and OpenBSD builds of the
shared POSIX wrapper, resolve archive-controlled directories with directory-
relative `openat` calls and reject symlink components. Staged files and final
commits use the same open parent handle. Deterministic tests cover the full
Linux and Linux mini implementations' parent substitution, concurrent
destination creation, write failure, and close failure; failed stages are
removed without publishing output, and forced replacement preserves the
existing file when close fails. Other frontends still need equivalent
protection. A process
able to rename an already opened directory outside the extraction tree is a
separate concurrent-mutation limitation. The checked-in classic Mac 68k core
copies now match the format-6 core. Some findings below originated in the
format-5 review; the phase descriptions and acceptance criteria track the
current format-6 work. Use the interoperability matrix for the latest
per-frontend evidence and remaining qualification limits.

Full Atari and OS/2 extraction limit native metadata application to the
matching successfully extracted subject and use status-aware cleanup. Both
write and restore the portable FAT envelope, prefer native attributes on the
matching platform, and defer directory timestamps and attributes until after
children. Each has a 256-directory/32-KiB queue bound and reports overflow.
Atari stages files and checks existing parent components but lacks
directory-relative operations. OS/2 stages files and checks path components;
EMX builds can reject existing symbolic-link components, while Watcom builds
cannot. Both adapters have cross-compile evidence but still need runtime
verification. Their path-based APIs cannot prevent concurrent parent
substitution.

The shared Amiga-family CLI now uses status-aware extraction, verifies and
closes regular-file stages before publishing, and keeps an existing file under
a recovery name during forced replacement. It refuses non-regular targets and
existing symlink path components, rejects AmigaDOS volume separators in
projected names, and preflights case-folded path collisions and file/directory
prefix conflicts before output changes. The name index is capped at 1,024
primary entries and 32 KiB of keys; archives exceeding the bound are refused.
Matching Amiga protection/comment metadata applies only after its exact
subject succeeds. Directory times and protection are deferred until children
finish. Negative or host-unrepresentable source times are warned about and
stored as unknown, while epoch zero stays valid. Malformed native Amiga metadata is rejected; foreign native metadata,
portable FAT envelopes, symlinks, and resource forks return partial. The m68k
cross-build passes cleanly, but no Amiga runtime or emulator round trip is
available. AmigaOS 4, MorphOS, and AROS use this source but their cross-toolchains
are unavailable here. Path-based checks retain a parent-substitution race, and
interruption between backup and publication can leave the original at its
recovery name.

The Amiga GadTools GUI now uses status-aware extraction with sibling staging,
bounded case-folded name and file/directory-prefix preflight, checked existing
directory components, and deferred directory time/protection restoration. It
captures and restores native protection/comment metadata for files; GUI archive
creation does not add directory entries, so it has no directory metadata to
capture. Its Compression menu offers Interchange Profile 1 for new archives,
selecting BoltLite with 16 KiB blocks under mini-reader limits.
Latin-1-unrepresentable names, resource forks, symlinks, foreign
metadata, and portable FAT envelopes are skipped with a partial result. Source
times outside the host's nonnegative representable range are recorded as
unknown. The GUI cross-build passes, but its path-based checks retain a
parent-substitution race and no Amiga runtime or emulator round trip is
available.

The Atari GEM GUI now applies the full Atari CLI's 8.3 ASCII projection,
case-folded collision and file/directory-prefix preflight (bounded at 1,024
names and 32 KiB), and 125-byte GEMDOS path limit before extraction. It checks
existing directory components for symlinks, stages regular files beside their
targets, verifies checksums and close before publication, and uses recovery
renames for replacement. It restores representable times, native TOS
attributes, and portable FAT fallback; directory metadata is deferred until
children finish. GUI archive creation now stages the complete archive,
preserves the replaced archive's non-read-only TOS attributes, converts source
names strictly to UTF-8, preflights 8.3 projected collisions within the same
1,024-name/32-KiB bound, records directories including empty ones, and writes
native plus portable FAT metadata for files and directories. Unsupported source links,
unrepresentable names, resource forks, foreign metadata, and POSIX modes are
reported or refused. Its Compression menu includes Interchange Profile 1,
which selects BoltLite with 16 KiB blocks and enforces mini-reader limits.
Both GUI and CLI pass Atari cross-builds with warnings
treated as errors. No Hatari emulator is installed for runtime round trips;
the GUI's pathname checks retain a concurrent parent-substitution race.

The portable metadata envelope and FAT validity/value masks are specified and
both cores validate its bounded payload during extraction and testing. They
reject orphaned or duplicate portable envelopes and verify the subject type.
The full OS/2 CLI, DOS16, Win16, and OS/2-16 capture portable FAT flags for files and directories
and defer directory restoration until children are extracted. DOS16 and Win16
report directory times as unrestored; OS/2-16 attempts to restore them. The
directory queue is limited to 256 entries. DOS16 has a DOSBox nested-directory
round-trip test; Win16 and OS/2-16 still need target-system testing. Other
platform adapters remain outstanding.
Both cores reject duplicate primary names (including trailing-slash aliases) and invalid portable metadata
groups when reading and before finalizing a new archive. Copy, append, and
deletion tests cover a file and its portable metadata group. Linux CLI append
and deletion write to unique same-directory stages and publish by atomic
rename. Both retain owner, group, and permission bits where the caller has
authority to set them, and abort before publication if those cannot be
preserved. Both refuse targets with multiple hard links because replacement
would otherwise leave aliases pointing at different archive contents. Append
syncs the staged archive before publication, and checks the
original file identity, size, and high-resolution mtime/ctime before replacing
it. Delete also checks identity, size, and mtime before publication.
Failure-injection tests prove that failed publication leaves the original
archive byte-for-byte intact and removes the stage. These replacements do not
preserve ACLs or extended attributes. The Windows CLI now stages append and
deletion beside the target and publishes through `ReplaceFile`, with an old
archive recovery file if publication fails. It copies the original archive
for append, refuses reparse-point and multiply linked targets, and checks the
file identity before publication. Native target metadata is delegated to
Windows replacement behavior and still needs runtime verification. A native
Windows smoke script covers append/delete integrity, hidden attributes, and
conditional hard-link/reparse-point refusal fixtures, but has not run here.
The OS/2 CLI now copies the original archive to a unique sibling stage and
appends there; handled failures leave the original untouched. Publication uses
a recovery rename, so interruption between the backup and publish renames can
leave the original at its recovery name. The Watcom cross-build passes, but
runtime and interruption tests remain unavailable. Atari append also uses a
sibling stage.
Total Commander WCX append copies the original to an exclusive sibling stage,
finalizes the updated archive there, and atomically replaces the original only
after successful close. WCX deletion also writes an exclusive sibling stage
and replaces the original atomically, preserving it on copy, close, or
publication failure. Both operations refuse directory and reparse-point
archive targets. Destination-specific collisions, native/portable
precedence, and other archive-edit cases remain outstanding. The mini
extractor checks all table and inline entry headers before opening an output
file.

The macOS CLI now stages create, append, and delete operations in exclusive
sibling files and publishes them by same-filesystem rename. Add/delete reject
non-regular targets and refuse publication if the archive path's identity,
size, or second-resolution timestamps changed during the operation. The staged
replacement retains permission bits, but not archive ownership, ACLs, or
extended attributes. Extraction now uses the status-aware core callback,
stages regular files and symlinks until checksum verification, checks
path-based parent components, and preflights up to 65,535 physical entries for
case/Unicode collisions and file/directory prefix conflicts. It restores
resource forks, Finder info, Unix modes, and valid mtimes where supported;
directory modes and times are deferred. Classic Finder creation/modification
dates and portable FAT flags are reported as partial because this frontend
does not restore them. Negative or host-unrepresentable source mtimes are
encoded as unknown and warned about; epoch zero remains valid. Its path checks retain a concurrent
parent-substitution race, and the macOS target build and runtime round trip
remain unverified in this environment.

The macOS Cocoa GUI extraction path now checks existing path components for
symlinks and non-directories, writes regular files to exclusive sibling stages,
and publishes only after checksum verification and successful flush, mode
setup, and close. A failed extraction leaves an existing destination file
untouched. It restores only format-6 timestamps marked valid, including epoch
zero, preserves the extracted file's access time, and defers explicit directory
times until children finish. Symlinks, resource forks, and native sidecar
metadata are skipped with a partial result. On macOS 10.10 and later, the
Cocoa GUI pins the chosen destination root and traverses archive paths with
directory-relative `openat`/`O_NOFOLLOW` operations; stage creation and
publication stay relative to the held parent directory. This blocks parent
symlink substitution during normal traversal. A concurrent actor can still
rename an already opened directory outside the extraction tree. On macOS 10.9,
staged pathname checks remain as a compatibility fallback and the GUI reports
the parent-substitution limitation as partial. Case and Unicode normalization
collision preflight runs before output changes and refuses file/directory
prefix conflicts; target runtime validation remains open.

BoltPack is not widely used, so earlier archive versions need not be supported.
Use that freedom to define a clear new contract. Basic transfer correctness is
the first deliverable; richer metadata remains a separate, optional phase.

## Review corrections

| Original proposal | Problem | Revised decision |
| --- | --- | --- |
| Add `MTIME_VALID` while keeping format 5 compatible | New readers would treat all existing format-5 timestamps as absent; old readers would still restore an unknown zero timestamp | Introduce format 6 and reject earlier versions explicitly |
| Call `done_fn` even when `out == NULL` | Existing callbacks dereference `out`; NULL also means skip, dry-run, unsupported entry, or open failure | Introduce explicit extraction outcomes and separate cleanup from successful completion |
| Link the existing converter and choose CP1252 for Win16/OS2-16 | These frontends use DOS-style filename APIs; OS name alone does not determine encoding. Existing converters can substitute characters and return success | Select the actual filename API's encoding and expose lossy conversion as a distinct result |
| Warn and continue on malformed UTF-8 | This contradicts the filename contract and permits ambiguous names | Reject malformed archive names; distinguish valid but unrepresentable names |
| Treat unfamiliar metadata and corrupt metadata alike | An unsupported attribute is different from broken framing, a bad CRC, or failed I/O | Skip well-formed unsupported metadata with a reported outcome; fail on corruption |
| Map archive bits to mtime and POSIX write bits directly to FAT read-only | These concepts do not have equivalent semantics | Preserve native values; allow only documented, explicit cross-platform mappings |
| Profile specifies only codecs and UTF-8 | Mini rejects blocks over 32 KiB and has seek, entry-count, path, and memory limits | Specify resource limits and destination capabilities as part of conformance |

Evidence in the current implementation:

- [Core extraction API](../src/core/archiver.h): NULL means skip;
  [Linux completion callback](../src/frontend/linux/main.c) dereferences `out`.
- [Core writer](../src/core/archiver.c): `packed_out->flags` is reset to zero
  even though the inline header contains the supplied flags. File-table
  records currently omit flags.
- [Mini writer and reader](../src/core/mini/archiver.c): the writer API has no
  flags argument; `mini_read_inline_meta` does not return flags to extraction.
- [Codepage API](../src/core/bp_codepage.h): unrepresentable characters may be
  replaced with `_` while returning success; full Shift-JIS mapping is absent.
- [Win16 frontend](../src/frontend/win16/main.c) uses `_dos_findfirst`;
  [OS/2-16 frontend](../src/frontend/os2_16/main.c) also uses DOS-style scanning.
- [Mini limits](../src/core/bp_types.h): 32 KiB blocks and 65,535 physical
  entries; [mini documentation](mini.md) describes the practical 2 GiB I/O limit.

## Phase 0 — Freeze the format and extraction contract

Write the format-6 specification before implementing its writer. Keep the
current format-5 specification clearly identified until the coordinated
implementation change. No legacy decoder, permissive version fallback, or
migration utility is required.

Recommended format-6 changes:

1. Assign entry flag `0x4000` to `BP_ENTRY_FLAG_MTIME_VALID`. A set bit makes
   zero a valid Unix epoch timestamp. A clear bit means time is unknown and
   writers must put zero in the timestamp field; readers must not apply it.
   Keep `MODE_VALID` at `0x8000` and the existing low 12 mode bits.
2. Append the two-byte entry flags to the file-table record tail, increasing
   it from 41 to 43 bytes. Listing, mini extraction, and archive rewriting then
   have the same validity information as sequential extraction. Define the
   duplicated fields and reject conflicting table and inline header values
   before filesystem operations trust them.
3. Retain the current unsigned 64-bit UTC seconds field for the basic profile.
   Pre-1970 dates are outside that field's range and must be reported as
   unrepresentable. Never silently cast negative dates or truncate to `.lo`.
   Check the destination time API's range, including 2038/2106 boundaries.
4. Define conversion of local DOS/FAT times to UTC and back, including the
   two-second resolution and timezone assumption. Query the target environment
   where possible; allow an explicit configured offset where it is not. If a
   source time cannot be interpreted reliably, report it as unknown.
5. Specify length and overflow checks. With the existing 42-byte fixed entry
   header and 16-bit total header length, the maximum encoded name is 65,493
   bytes, not 65,535. Frontends may impose smaller limits. Allocation arithmetic
   must also fit 16-bit `size_t` and conversion-buffer index types.
6. Define a versioned metadata envelope and reserve a metadata-only namespace
   tag before implementing basic FAT attributes. If the existing `platform`
   byte carries this tag, distinguish it from actual creator OS identifiers.
   The first supported record is FAT attribute validity and value masks for
   read-only, hidden, system, and archive (`0x27`); absent differs from false.

The metadata contract must define byte order, lengths, namespace/schema version,
subject entry type, duplicate records, unknown record kinds, and precedence.
Bind supplementary entries to a contiguous subject group with the same exact
archive path. Resource forks remain part of their owning file group. Reject
orphan metadata and conflicting duplicate primary paths; add/replace/delete
operations must act on complete groups. Apply metadata only to a successfully
extracted subject, never to an existing file skipped by overwrite policy.

Use an envelope limit of 256 uncompressed bytes for the initial FAT record.
Unknown well-formed optional records may be streamed past with CRC checking;
bad framing, overflow, truncation, or checksum failure are errors. Finish the
wire field table and byte examples in the specification before encoder work.

**Acceptance:** matching format-6 byte diagrams and fixtures for known epoch
zero, absent time, mode/FAT validity masks, table flags, and metadata grouping;
an explicit reject-earlier-versions policy. A plan alone does not change
`BP_FORMAT_VERSION`.

## Phase 1 — Carry metadata and extraction outcomes through the core

Update full and mini cores together, including direct writes, packed parallel
writes, commit, list/info, append, copy, and delete. Preserve flags in memory
and on disk. Add an explicit metadata/flags argument to the mini writer and
update the DLL exports, binding structures, and all callers that currently
use wrappers supplying zero flags. Do not infer timestamp validity from its
numeric value in a compatibility wrapper.

Replace the overloaded NULL result with an explicit extraction contract:

| Outcome | Required behavior |
| --- | --- |
| Extract regular file or stream | An owned output stream exists; verify bytes and successful close before committing |
| Create directory | Record its creation/result and queue final metadata without inventing a data stream |
| Skip existing/unsupported entry | Record the reason; do not apply its attributes or supplementary records |
| Dry-run | Validate and report planned actions without creating objects or applying metadata |
| Open/write/close/CRC failure or cancellation | Return failure, clean up resources, and do not run successful metadata restoration |

Specify stream ownership and exactly-once cleanup separately from completion.
Completion receives a verified outcome and may return a restore error. Apply
this contract to the mini core too: its current checksum-mismatch path prints
a warning but can finish with `BP_OK`. Skipped or unverified entries must not
be counted as successfully restored. Preserve any existing destination file
when a new extraction fails; stage output where the target supports it.

Directory finalization runs deepest first after child creation. Use a bounded
queue or a second archive/table pass on constrained targets; do not retain an
unbounded whole-tree structure. Finalize only successfully created/selected
directories, retain a failure result after partial extraction, and check every
metadata syscall. Read-only/protection attributes are applied after data,
resource forks, and other metadata have been written.

**Acceptance:** explicit outcomes for empty files, directories, dry-run,
overwrite skips, unsupported entries, open/write/close errors, cancellation,
CRC failures, and metadata failures; no null dereferences, leaked handles,
false success, or mutation of skipped targets.

## Phase 2 — Enforce names and destination mappings

Validate counted archive names before converting or passing them to C string
APIs: reject embedded NULs, malformed/overlong UTF-8, surrogate code points,
values beyond U+10FFFF, and unsafe path components. Use the same validation
for table and inline names. Validate destination paths again after conversion
or mapping; a lexically safe archive name alone does not prevent writing
through an existing filesystem symlink or reparse point outside the root.

The conversion API now distinguishes unrepresentable characters, invalid
input, and insufficient space. Shift-JIS double-byte characters outside the
implemented half-width range return an explicit unsupported result. DOS,
Atari, Amiga, and OS/2 archive creation refuses names that cannot be converted;
legacy extraction callbacks refuse unrepresentable names rather than passing
UTF-8 bytes into a local filesystem API. Atari extraction uses a strict base
GEMDOS 8.3 ASCII projection and tracks case-folded paths before writing; its
collision index is bounded at 1,024 entries and 32 KiB of keys. Windows CLI conversion uses strict ACP
conversion and tracks projected paths during extraction. It refuses
case-insensitive collisions, reserved device names, invalid path characters,
and trailing dots or spaces, even when `--force` is set. Remaining encoding
work includes broader target tests, Unicode normalization and non-Windows
collision checks, and checking every source filename API against its configured
codepage.

The Amiga-family CLI converts to its configured Latin-1 filename API, rejects
volume/device separators after conversion, and preflights case-folded path
collisions plus file/directory prefix conflicts. Its 1,024-entry/32-KiB bound
fails before extraction creates output. The comparison folds ASCII and the
Latin-1 uppercase letter ranges; target filesystem comparison behavior still
needs runtime qualification.

Select the source codepage from the actual filename API and active environment.
DOS-style Win16/OS2-16 paths require the appropriate OEM/local codepage, not an
assumed CP1252. Provide an explicit override and documented build default when
detection is unavailable. On Windows NT targets, use Unicode filesystem APIs
for names beyond the active ANSI codepage; legacy targets declare their limits.

Compile only needed conversion tables into mini builds and measure binary,
heap, and stack costs on 16-bit builds. A single-byte codepage may expand to
three UTF-8 bytes per input byte; size buffers with checked arithmetic and
account for the complete destination path. Do not claim the existing module
is a single-table footprint without checking what the linker retains.

Default to a reported refusal for valid names the destination cannot express.
An explicit lossy-name option may map them, but must report the original and
resulting names and detect collisions before overwriting anything. Include
case folding, Unicode normalization, reserved device names, trailing dots or
spaces, truncation, and CP/M flat 8.3 projection in collision handling.
`--force` does not authorize two distinct archive names to silently merge.

The CP/M-86 writer uses a strict flat 8.3 projection: it rejects truncation,
directories, unsupported characters, case-folded duplicates, and a source that
aliases the archive output before opening the destination. It refuses an
existing archive. Extraction materializes only exact uppercase 8.3 names;
directories and nonmatching names return a partial result. The projection is
covered by a C89 host frontend test that verifies archive creation, listing,
extraction, collision refusal, and preservation of existing archive bytes. No
CP/M-86 compiler or runtime is available here.

**Acceptance:** representative CP437/850 names round-trip exactly through
Linux and the relevant mini API; invalid UTF-8 fails; valid unmappable names
have an explicit result; projected collisions cannot overwrite earlier files.

## Phase 3 — Restore basic native attributes consistently

Apply valid POSIX modes and known times to files and directories. Use APIs
that act on the symlink itself for link timestamps; do not use a call that
follows the target. Report unavailable link-time support. Do not manufacture
an original access time from mtime. Set-ID modes require an explicit restore
policy rather than automatic privilege restoration from an archive.

Use the basic FAT metadata record from Phase 0 across DOS, Win16, Win32,
OS/2, and Atari adapters. The Windows CLI captures and restores the four
portable FAT flags for files and directories, using `GetFileAttributesA` and
`SetFileAttributesA`; it also restores directory times after child extraction.
The full Atari and OS/2 CLIs capture native attributes and the portable FAT envelope,
then restores directory times and flags after child extraction. It stages file
output using same-directory temporary files and verifies checksums before
publishing. Atari uses GEMDOS renames; OS/2 reserves the destination with
exclusive-create and copies the verified stage, preserving the old file under
a recovery name during forced replacement. OS/2 FAT targets require 8.3 names
and all OS/2 targets preflight case-mapped collisions. The final copy can leave
a partial file if the process stops mid-copy, and path APIs leave a race with
concurrent parent substitution. Watcom cross-compilation and linking of the
OS/2 executable succeeds, but neither the Atari nor OS/2 adapter has target
runtime evidence yet. Win16, DOS, and Atari mini frontends use their target
APIs. The native Win32 GUI now uses strict ACP-to-UTF-8 conversion for source
names and UTF-8-to-ACP projection for extraction, refuses reserved or
colliding Windows names, checks archive-controlled directory components for
reparse points, stages regular files until checksums pass, restores
representable times and portable FAT attributes, and defers directory
restoration. Archive creation exposes Interchange Profile 1 and writes to a
same-directory stage before publishing a completed archive; it refuses source
reparse points instead of following them. File-dialog and drag/drop creation
preflight case-insensitive duplicate names and file/directory path conflicts,
bounded at 65,535 primary entries/path nodes and 16 MiB of path keys. It reports
POSIX modes, symlinks, and resource forks as partial. Its path-based checks
have a race with concurrent parent substitution, and preflight does not detect
Unicode normalization collisions or source-tree changes during creation. It
has cross-build evidence but no Windows runtime qualification. The Windows
FLTK GUI now uses strict UTF-8-to-ACP name and destination-root projection
(and opens the chooser-selected archive path through UTF-16), Windows component
checks, case-insensitive projected-path preflight, reparse-point checks for the
chosen root and traversed directories, and sibling staging committed only after
the core reports a verified checksum and successful close. It restores
representable file and deferred directory times plus portable FAT flags, and
reports modes, links, resource forks, unsupported metadata, and lossy restores
as partial. Its collision preflight is bounded at 65,535 selected primary
entries and 65,535 projected path nodes; exceeding either bound refuses before
output changes. Win32 and Win64 cross-builds pass; Windows runtime tests,
parent-swap race coverage, and names outside the active ACP remain unqualified.
The .NET GUI now uses strict UTF-8 archive-name decoding and Unicode file APIs,
collision and reserved-name preflight, path-based reparse checks, exclusive
sibling staging with checksum-verified publication, portable FAT and valid
timestamp restoration, and deferred directory metadata. It stages new archives
before replacement and writes portable FAT metadata when creating them.
Its path preflight is bounded at 65,535 primary entries and 65,535 unique
projected path nodes; either limit refuses extraction before output changes.
Symlinks, resource forks, POSIX modes, unsupported metadata, and
unrepresentable source times are reported as partial. The DLL provides
status-aware extraction, correctly sized archive-context allocation, Unicode
file opening, exclusive temporary creation, and checked close. Win32 and Win64
DLL cross-builds pass, but this environment has no .NET compiler or Windows
runtime; the managed project is not yet compile- or runtime-qualified.
Path-based reparse checks retain a concurrent parent-substitution race. The
Total Commander WCX now rejects invalid, reserved, unrepresentable, and colliding
Windows paths before listing, with a 65,535-primary-entry bound. It verifies
archive data for the WCX test operation, stages selected regular files beside
their destinations, and publishes only after checksum verification and close.
WCX append and deletion now stage a complete replacement archive beside the
target and atomically publish after successful finalization and close.
It captures and restores portable FAT flags, restores representable file times,
and defers directory times and flags until `CloseArchive`. Unrepresentable
source times are encoded as unknown and cause a WCX error after the archive is
finalized. Symlinks, resource forks, POSIX
modes, unsupported metadata, and lossy restores return a WCX creation error
(the WCX API has no partial-preservation result). Win32 and Win64 cross-builds
pass; plugin-host runtime tests and concurrent parent-swap coverage remain
unavailable. The path-based component checks still have a race with concurrent
parent substitution. The legacy WCX `ReadHeader` API refuses entries larger
than `INT_MAX`; `ReadHeaderEx` carries 64-bit sizes.

The Amiga-family CLI stages regular files beside their destinations and
publishes only after checksum verification and successful close. Forced
replacement uses a recovery rename, with rollback on reported publication
failure. It checks existing parents with `lstat`, refuses symlinks and
non-regular output targets, and defers matching Amiga protection/comment
metadata and directory times until the subject succeeds and children finish.
Only the m68k frontend has a cross-build result here; Amiga-family runtime
behavior, crash recovery, and the pathname parent-swap race remain unqualified.

Native attributes take precedence on their matching platform; portable
records are fallback information for other platforms. Specify field-level
precedence so record order cannot change the result. Do not equate the FAT
archive bit with a timestamp comparison, infer executability from a suffix,
or map one POSIX write bit to complete read-only semantics. Amiga permissions
include inverted bits and independent delete/execute permissions. Any lossy
cross-family mapping must be explicit, documented, and reported.

**Acceptance:** regular and empty files, nested directories, read-only files,
and directories containing protected children restore correctly on supported
targets. Unsupported attributes are reported; native and portable records
cannot silently override each other.

## Phase 4 — Optional richer metadata

Implement this after the basic profile passes. Extend the versioned envelope
with nanosecond time fractions, optional access time, owner/group names, and
namespaced extended attributes. Define exact TLV widths, supported versions,
singleton/duplicate rules, and numeric ranges, including nanoseconds below
1,000,000,000. Suggested initial limits are 64 TLVs and 64 KiB uncompressed
metadata per subject, with individual values capped at 32 KiB. Check lengths
before allocation; mini readers may stream and verify records they cannot
apply. These caps do not apply to separate resource-fork data streams.

Owner/group names do not guarantee the same identity on another machine.
Changing ownership or applying security-related xattrs requires explicit
policy. Unknown valid optional metadata produces a reported skip; corruption
always fails. A strict preservation mode fails when requested metadata cannot
be restored; ordinary transfer mode reports a partial-preservation outcome.

Copying an archive intact preserves its foreign metadata bytes. Extracting on
a foreign filesystem and repacking does not automatically preserve skipped
metadata or resource forks. If that workflow becomes required, design an
explicit sidecar/manifest carrier with association and collision rules; it is
outside the first delivery. Warn when skipping a resource fork can make an
otherwise extracted file unusable.

## Phase 5 — Qualify a realistic interchange profile

"BoltPack Interchange Profile 1" uses format 6. Readers support Stored and
BoltLite; writers select either, use 16 KiB blocks by default, and never exceed
32 KiB for any blocked entry, including Stored and supplementary entries.
BoltLZ, Fluff, and zstd remain optional capabilities outside this profile.

For a mini-compatible archive, cap total serialized size at 2,147,483,647
bytes and physical entry count at 65,535, including metadata and resource
forks. Enforce limits during creation, including table/footer overhead, and
before extraction commits output. Check file sizes, native path limits, and
available working memory as separate capabilities.

The common naming subset for the smallest targets is a flat set of unique
uppercase ASCII 8.3 filenames. UTF-8 paths, directories, and symlinks remain
supported in the format, but exact extraction requires a destination capable
of representing them. No profile claim requires CP/M to recreate directories
or an ASCII-only reader to materialize arbitrary Unicode names. Unsupported
cases must have a defined reported outcome instead of a success claim.

Keep a capability matrix by frontend/build: codecs, block/file/archive limits,
filename encodings, path model, timestamp range/resolution/timezone handling,
POSIX modes, FAT flags, links, and platform metadata. Distinguish compiled,
host-tested, emulator-tested, and hardware-tested support.

The mini writer and reader enforce the 2,147,483,647-byte serialized-size
limit, including table and footer bytes. The Linux mini CLI exposes
`--interchange-profile-1` for creation, defaulting to BoltLite with 16 KiB
blocks and permitting Stored or BoltLite. Its QEMU interop fixture verifies
the method and block settings and integrity-checks both methods with the mini
and full readers. The full writer now offers
`BP_PROFILE_INTERCHANGE_1`, which selects BoltLite with 16 KiB blocks and
checks codecs, block and per-entry sizes, physical entry count, and projected
archive size during create, append, packed-entry commit, and archive copy.
Profile append validates existing inline headers against the table before
changing the archive. The Linux, Windows, Atari, OS/2, and macOS CLIs expose
this as `--interchange-profile-1` for create and add. The QL CLI now exposes
the profile for create and add; its host-built fixture checks both codecs and
verifies create/append output with QL, full, and mini readers. The X68000 CLI
now exposes it for create/add; its host-built DOS-call fixture checks both
codecs and verifies create/append output with X68000, full, and mini readers.
The Human68k target compiler and runtime remain unavailable. RISC OS now
exposes create/add profile selection; a host syntax check passes, but its
target compiler and runtime are unavailable. QDOS target build and runtime
qualification also remain unavailable. The Plan 9 CLI now exposes create/add
profile selection; its host-shim fixture verifies Stored and BoltLite output
with Plan 9, full, and mini readers. Native Plan 9 build/runtime qualification
remains unavailable. The Linux and Windows
FLTK GUIs offer the profile for archive creation and network sending. Readers reject oversized
declared archives before extraction. Both cores reject absolute, drive-prefixed,
backslash, empty, `.` and `..` path components, and require primary directory
names to end in `/`. Host tests cover profile output, append, refusal of
extended codecs, entry-size and serialized-size limits, and a mini-reader test
of a full-writer profile archive. Windows has cross-build evidence but still
needs runtime qualification of option behavior and emitted archives. The full
OS/2 CLI also exposes create/add profile selection; its Watcom cross-build
passes, and OS/2 runtime qualification remains unavailable. The macOS CLI now exposes the
same profile option; its strict C89 host build and create/add/test smoke pass,
but a macOS target build and runtime profile round trip remain outstanding.
The Windows FLTK GUI offers the profile for archive creation and network
sending; the Atari GEM, Amiga GadTools, native Win32, and Windows .NET GUIs
offer it for creation. The macOS Cocoa GUI source also offers the profile in
its compression sheet, and the Haiku BeAPI GUI now offers it in the Compression
menu. The DLL profile wrapper cross-builds, but the .NET GUI
has no managed build or Windows runtime qualification; the Cocoa GUI has no
macOS target build or runtime qualification. Haiku's target cross-build and
runtime qualification remain outstanding. Classic Mac frontends still lack a
profile option. GUI profile code and target
qualification remain incomplete.
Per-frontend evidence is tracked in
[interoperability-matrix.md](interoperability-matrix.md).

### Required validation

| Area | Fixtures and expected checks |
| --- | --- |
| Format | Independently specified golden bytes; version rejection; inline/table agreement; boundary name lengths; truncated fields; invalid lengths; header/table/block/payload CRC failures |
| Timestamp/flags | Unknown versus known epoch zero; `.hi` nonzero; negative/out-of-range source time; 2038/2106 limits; DOS timezone/resolution; packed, serial, mini, list, append, copy, and delete paths |
| Lifecycle | Directories finalized after children; empty files; skip/dry-run; close failure; cancellation; bad CRC; attributes never applied to a failed or skipped subject |
| Names | Actual codepage fixtures; complete UTF-8 validity cases; unrepresentable names; 8.3/case/normalization collisions; destination symlink/reparse traversal |
| Attributes | Native/portable precedence; read-only files; mode bits; timestamp precision; safe link times; valid unknown TLVs versus malformed/oversized/CRC-failing metadata |
| Resource limits | 8/16/32 KiB blocks; rejection of oversized blocks; metadata overhead in entry/size limits; constrained heap and allocation failures |
| Cross-target | Full-to-mini and mini-to-full; Linux/Windows; Amiga/Atari; representative Mac resource forks; DOS/Win16/OS2-16/CP-M behavior where tools or hardware exist |

The format validation now includes three fixed archives laid out from the
specification field tables rather than generated by the project writer: a
170-byte blocked Stored entry, a 158-byte unframed Stored entry, and a
159-byte unframed BoltLite entry. Both readers integrity-check the layouts and
extract the single-blob cases. Tests also verify corrupt single-blob payloads
and reject an oversized mini single blob before opening output. This is initial
golden-byte coverage. Full and mini writers accept the maximum 65,493-byte
header name, both readers verify that record, and both writers reject a 65,494-
byte name. Separate fixtures verify full and mini readers reject versions 1–5.
These checks do not replace the remaining corruption, metadata, and
target-round-trip cases in the table.

Golden expectations must not be generated solely by the same writer under
test. On capable destinations require exact bytes/names/attributes. On limited
destinations assert the documented projection or refusal and diagnostic; do
not require identical filesystem trees across incompatible capabilities.
Compare timestamps at declared destination resolution and modes under a
controlled umask. Re-run through archive copy, append, and deletion to verify
metadata grouping and flags survive editing.

## Delivery order and completion

Deliver Phases 0-3 plus the basic-profile checks from Phase 5 as the first
interoperability release. Phase 4 is optional and must not delay basic filename,
timestamp, directory, and FAT-attribute correctness. Update source kits, copied
classic-Mac core sources, DLL bindings, frontends, documentation, and release
notes together at the format-6 cutover; every shipped reader must reject
unsupported versions and report incomplete transfers accurately.

Implementation is complete only when each claimed capability has a recorded
result on its target or an explicitly narrower support claim. Phase 4 remains
optional; Phases 0-3 and the full basic profile are still the completion gate.

Portable ACL mapping, hard-link identity, device nodes, and sparse-file layout
are outside the first delivery. Numeric ownership and full filesystem backup
fidelity are not guarantees of the basic interchange profile.
