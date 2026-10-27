# Archive Format Interoperability Plan

**Reviewed:** 2026-10-26  
**Status:** implementation in progress; the core and specification use format 6.  
**Basis:** [archive format audit](archive-format-audit.md).

This replaces the interoperability plan previously embedded in the audit. Its
goal is predictable transfer: preserve file bytes, preserve representable names
and attributes, and report every unsupported or lossy operation. Identical
filesystem results on every OS are not achievable: CP/M has no directories,
legacy filesystems cannot represent every Unicode name, and some systems have
no equivalent for a resource fork or a POSIX permission bit.

The format-6 core, timestamp validity flag, UTF-8 validation, and file-table
consistency checks are implemented. Mini frontends have explicit name
conversion, and the Linux CLI uses verified temporary files for extraction.
This plan is not complete: portable FAT attribute support across platforms, consistent loss reporting,
full platform name handling, and safe extraction lifecycle handling across all
frontends remain outstanding. DOS16, Win16, and OS/2-16 now require an explicit fixed
timezone offset for timestamp conversion; other DOS-derived frontends still
need review. The Linux CLI and Linux mini resolve archive-controlled directories
with directory-relative `openat` calls and reject symlink components. Staged
files and final commits use the same open parent handle. Deterministic tests
cover parent substitution, concurrent destination creation, and write failure.
Other frontends still need equivalent protection. A process able to rename an
already opened directory outside the extraction tree is a separate
concurrent-mutation limitation. The checked-in
classic Mac 68k core copies now match the format-6 core. The evidence and phased tasks below describe
the original format-5 review, not a claim that each issue is still unchanged.

Atari and full OS/2 extraction now limit native metadata application to the
matching successfully extracted subject and use status-aware cleanup. This
does not yet implement the portable metadata envelope or deferred directory
attributes. The OS/2 changes require verification with a working OS/2 compiler
and runtime.

The portable metadata envelope and FAT validity/value masks are specified and
both cores validate its bounded payload during extraction and testing. They
reject orphaned or duplicate portable envelopes and verify the subject type.
DOS16, Win16, and OS/2-16 capture portable FAT flags for files and directories
and defer directory restoration until children are extracted. DOS16 and Win16
report directory times as unrestored; OS/2-16 attempts to restore them. The
directory queue is limited to 256 entries. DOS16 has a DOSBox nested-directory
round-trip test; Win16 and OS/2-16 still need target-system testing. Other
platform adapters remain outstanding.
Both cores reject duplicate primary names (including trailing-slash aliases) and invalid portable metadata
groups when reading and before finalizing a new archive. Copy, append, and
deletion tests cover a file and its portable metadata group. Linux and Windows
in-place append now require a bounded table backup and restore it on
finalization failure; the Linux regression checks byte-for-byte restoration.
Destination-specific collisions, native/portable precedence, and other
archive-edit cases remain outstanding. The mini extractor checks all table and
inline entry headers before opening an output file.

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
UTF-8 bytes into a local filesystem API. Windows CLI conversion already uses
strict ACP conversion. Remaining encoding work includes broader target tests,
normalization and case-fold collision detection, and checking every source
filename API against its configured codepage.

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
OS/2, and Atari adapters. Use each target's native attribute API; Win16/DOS
and Atari cannot simply reuse a Win32 `SetFileAttributes` call. Cover CLI,
GUI, DLL/.NET, and Total Commander paths, with shared helpers where practical.

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

No runtime changes or new conformance claims are made by this plan review.
Implementation is complete only when each claimed capability has a recorded
result on its target or an explicitly narrower support claim.

Portable ACL mapping, hard-link identity, device nodes, and sparse-file layout
are outside the first delivery. Numeric ownership and full filesystem backup
fidelity are not guarantees of the basic interchange profile.
