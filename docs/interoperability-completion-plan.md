# Interoperability completion plan

**Prepared:** 2026-10-27  
**State:** proposed execution plan; implementation goal remains paused.

This is the execution backlog for completing the [archive-format plan](archive-format-plan.md).
The [specification](specification-1.0.md) defines the format, and the
[interoperability matrix](interoperability-matrix.md) records existing evidence.
Use those documents together; this plan does not declare their outstanding work complete.

The intended result is working format-6 interchange across the existing frontend
scope: preserve file bytes, preserve representable names and basic attributes,
refuse unsafe mappings, and report incomplete transfers accurately. Earlier
archive versions remain unsupported. Rich ACL translation, hard-link identity,
device nodes, and sparse-layout preservation remain outside the original basic
profile; Phase 4 richer metadata remains optional.

## Starting point

The full and mini cores already implement format 6, validity flags, canonical
UTF-8 paths, metadata grouping, and Interchange Profile 1. Existing host tests,
Linux full/mini tests, cross-builds, and DOSBox fixtures provide substantial
evidence. Reuse their fixtures and results; repeat them when code changes or
unresolved failures justify it.

Completion is still unproven. The open work includes frontend lifecycle and
metadata coverage, Windows Unicode filesystem handling, target-specific name
mapping, unsafe concurrent path changes, and native execution evidence.
Cross-builds and host API shims do not establish target filesystem behavior.

DOS16 CP437 and CP850 conversion fixtures run as Watcom-built DOS programs in
DOSBox. Accented filename extraction remains unresolved. Exploratory direct
file creation also failed in DOSBox 0.74-3, so the next useful step is an
independent DOS environment and a saved reproducer, rather than another
equivalent host-mount experiment. Full DJGPP execution separately needs a DPMI
host. The 16-bit resource measurements collected before the pause still need
a reproducible report.

On 2026-10-28, the initial core audit found that nonzero reserved archive-header
flags were rejected by full extraction/listing but accepted by mini readers and
some full-core info/edit paths. Those paths now reject the flags with
`BP_ERR_CORRUPT`; regression fixtures use a valid header CRC and cover full and
mini readers plus append, copy, and delete. `make CC=gcc unit-test` and
`make mini-all` passed. The complete I01 requirement ledger remains outstanding.

## Ordered work packages

All packages below are open. A package closes only with the stated evidence.
Environment acquisition can proceed alongside the code work, but unavailable
targets must stay visibly blocked in the backlog.

| ID | Priority and work | Required result and evidence |
| --- | --- | --- |
| I01 | First: turn the existing plan into a requirement ledger | Enumerate every Phase 0–3 and basic Phase 5 acceptance item against every shipped frontend/build, including copied source kits and DLL consumers. For each applicable item record source location, missing behavior, fixture, target environment, and result. Link existing evidence instead of treating already implemented features as new work. |
| I02 | First: finish filename and destination handling | Audit then fix the gaps listed below. Produce exact byte/name results for representable inputs and pre-output refusal for collisions or unmappable names. Include archive path, selected destination root, and source enumeration APIs. |
| I03 | First: finish extraction, metadata, and archive edits | Audit then fix each adapter against the lifecycle and metadata contract below. Verify successful restoration, accurate partial/error results, original-file preservation, and recovery behavior. |
| I04 | Next: make cross-system qualification reproducible | Extend existing fixtures into a shared corpus with independent expected results and target runners. Record real execution separately from builds and host shims; skipped checks stay open. |
| I05 | Next: complete 16-bit resource accounting | Save reproducible binary/static-data measurements and heap/stack bounds for the current DOS16, Win16, and OS/2-16 builds. Exercise index/queue bounds and allocation failures; document allocator overhead and compiler assumptions. |
| I06 | In parallel with I02–I05: obtain and run native environments | Execute the target queue below, fix failures, and retain results tied to the tested commit and filesystem. A missing compiler, SDK, OS image, ROM, or runtime is an explicit dependency. |
| I07 | Last: run the completion audit and release gate | Reconcile the ledger, target results, matrix, specification, bindings, copied sources, build instructions, and release notes. Every required row must have sufficient evidence; no completion claim while required behavior or target evidence remains open. |

### I02: filename and destination code

1. On modern Windows, complete the Phase 2 Unicode API requirement for frontends
   that currently project through the active ANSI codepage. Cover enumeration,
   archive opens, extraction roots, staging, publication, and attributes. Keep
   legacy Windows adapter limits explicit. Verify names outside the active ACP,
   reserved names, case aliases, and file/directory prefix conflicts.
2. Qualify CP437/CP850 names through actual DOS, Win16, and OS/2 filename APIs.
   Save the failing DOSBox probe and compare against an independent DOS runtime.
   Verify local-byte → UTF-8 archive → local-byte round trips, including aliases
   formed by case folding, and explicit refusal of unmappable text.
3. Audit QDOS device/underscore mapping, RISC OS dot-separated paths, Human68k
   Shift-JIS limits, Plan 9 paths, and Haiku CLI/GUI destination handling. Implement
   missing bounded collision and prefix checks before output changes. Unsupported
   Shift-JIS double-byte mappings must remain an explicit refusal until implemented.
4. Verify actual filename encodings for POSIX wrappers, especially z/OS USS and
   its compiler/runtime mode. Add conversion or detection where required; do not
   infer UTF-8 support from a successful POSIX compile. Qualify normalization and
   case comparison on Windows and Mac filesystems.
5. Inventory symlink/reparse traversal and concurrent parent/destination changes
   for each adapter. Use held directory/subject handles where the OS supports
   them. Resolve the required concurrent-mutation contract before closing safety
   rows; a pathname preflight alone is insufficient evidence. Where an API cannot
   provide a required operation safely, implement a deliberate refusal and record
   its effect on the requirement. Documentation alone does not repair a code gap.

### I03: lifecycle, metadata, and edits

Apply the existing core extraction contract to every applicable CLI, GUI, and
plugin: distinguish directory creation, owned output, dry-run, skipped subject,
unsupported entry, cancellation, and failure. Verify checksum and successful
close before publication; finalize directory metadata deepest first. A skipped
or failed subject must never transfer supplementary metadata to an existing file.

Complete basic timestamps, modes where applicable, portable FAT flags, and the
native metadata already claimed by each frontend. Test epoch zero, absent time,
negative/unrepresentable source time, destination time boundaries and resolution,
timezone assumptions, protected children, and restoration failures. Verify native
versus portable field precedence in both record orders, including conflicting
values; preserve unrelated attribute bits. Audit Cocoa's mode restoration,
Amiga GUI directory metadata capture, and Haiku/legacy frontend coverage against
the original basic-profile requirements rather than assuming source presence
means completion.

Exercise create, append, copy, and delete where offered. Preserve complete
primary/resource-fork/native/portable groups and flags. Check stage creation,
write, flush, close, publication, target replacement, read-only targets, aliases,
and interruption at each recovery rename. Test OS/2's final verified-file copy
explicitly for interruption damage. Fix publication behavior where required and
document usable recovery paths for targets without an atomic replacement API.
Qualify archive-file attribute retention separately from entry metadata.

## I04: common qualification corpus

Reuse the current independently laid-out format-6 fixtures. Add an expected-result
manifest for each destination, generated independently of the writer under test.
It must specify file bytes, local filename bytes or Unicode spelling, timestamps
at target resolution, applicable attributes, and expected refusal/partial status.

| Corpus area | Required cases |
| --- | --- |
| Basic profile | Stored and BoltLite, 16 KiB default and 32 KiB boundary, empty/nonempty files, nested/empty directories, flat uppercase 8.3 subset for limited targets |
| Names | CP437/850, Unicode outside ACP, MacRoman/Latin-1, malformed UTF-8, unmappable names, case/normalization aliases, reserved names, truncation, prefix conflicts, output-root limits |
| Metadata | Epoch zero/unknown time, target date boundaries, timezone and rounding, modes, all four portable FAT validity/value masks, matching native metadata, conflicting native/portable records, resource forks where supported |
| Failure and safety | Existing destination, corrupt/truncated data, invalid groups, allocation/index/queue limits, write/close errors, cancellation, parent substitution, concurrent destination creation, and interrupted edits |
| Format and limits | Rejection of versions 1–5 and future versions, invalid framing/CRC, profile codec/count/size limits including supplementary entries and table/footer overhead |

For each target, transfer Linux → target → Linux and target → Linux → target.
Run the target's own create/test/extract and available edit operations, then
verify outputs independently. Also transfer directly between representative
legacy destinations with a shared encoding/profile; Linux-only readers do not
prove that relationship. Do not require unsupported filesystems to preserve
features they cannot represent, but require the specified reported outcome.

## I06: target environment queue

This groups execution work without removing any frontend from scope. I01 must
include every shipped adapter, including wrappers omitted from this short table.
Start Windows and macOS environment acquisition first because their APIs also
serve several GUI/plugin consumers.

| Target group | Environment needed | Qualification work |
| --- | --- | --- |
| Linux CLI/mini/FLTK | Native Linux, QEMU-i386, usable GUI display | Reuse existing lifecycle/profile/race fixtures; add actual GUI creation, cancellation, extraction, flattened-name collisions, and metadata checks. |
| Windows CLI/native GUI/FLTK/.NET/WCX | Windows with NTFS and FAT-compatible test volume, .NET build tools, Total Commander plugin host | Run `python test/check_windows_cli.py build/win64/boltpack.exe`; extend it for I02/I03, build the managed GUI, and exercise every UI/plugin path. Record denied hard-link/reparse privileges as missing evidence. Cross-build both word sizes. |
| macOS CLI/Cocoa/legacy GUI | macOS SDK/runtime; case-sensitive and case-insensitive test volumes; supported legacy environment where shipped | Verify Unicode normalization, Apple xattrs/Finder info/resource forks, time/mode restoration, publication, and actual GUI flows. Host declaration stubs remain supporting evidence only. |
| DOS16/full DOS/Win16/OS2-16/full OS2 | Independent DOS runtime with FAT, DPMI host for DJGPP, Windows 3.x, OS/2 with FAT and HPFS | Resolve accented-name failure; qualify codepages, FAT flags, deferred directories, timestamps, profile transfer, and edit/recovery behavior. DOSBox conversion tests do not qualify the other adapters. |
| Atari CLI/GEM GUI | Hatari or hardware with appropriate TOS environment and writable media | Run projection/profile corpus, attributes and dates, GUI creation/extraction, staged edits, and interruption recovery. |
| Amiga CLI/GadTools and OS4/MorphOS/AROS variants | Matching SDKs and emulator/hardware/runtime for each claimed variant | Build each variant, then verify Latin-1 mapping, native protection/comment precedence, directory capture/restoration, staging, and recovery. Shared source does not qualify all variants. |
| Classic Mac 68k/PPC | Complete application/resource builds and Mac OS 9 runtime | Test MacRoman/HFS collisions, resource forks, Finder fields, directory dates, bounded preflight, partial failures, and profile transfer for both builds. |
| QL/X68000/RISC OS/Plan 9/CPM-86 | Matching compiler and native filesystem runtime/emulator for each | Replace syntax/host-shim-only evidence with target builds and the applicable corpus. Exercise native path mappings, limits, metadata, and refusal behavior. |
| Haiku CLI/BeAPI GUI | Haiku compiler/runtime and BFS test volume | Complete and qualify collision/symlink handling, native attributes, timestamps, profile behavior, and GUI outcomes. |
| Other POSIX wrappers | Native environments for BSDs, Hurd, Solaris/illumos, AIX, z/OS, QNX, and remaining shipped wrappers | Validate actual APIs and feature gates, filename encoding, modes/times, safe traversal, unsupported metadata reporting, and edits. Record OS/compiler versions, especially AIX and z/OS. |

Each run must record commit, frontend/build flags, compiler, OS/emulator version,
filesystem, locale/codepage, timezone, commands, expected/observed result, and
retained logs. A skip is not a pass. Emulator evidence must name the emulator
and filesystem path used; it is not a hardware-performance measurement.

## Execution and completion rules

On resumption, do I01 first and select the first unresolved I02/I03 code item
that can be reproduced locally. Begin obtaining the Windows/macOS and legacy
environments concurrently with that work. Keep one concrete defect or missing
capability per implementation change, with its meaningful regression evidence.
Use current checks such as `make CC=gcc test`,
`make CC=gcc linuxmini-interoperability-test`,
`make CC=gcc dos16-interoperability-test`, and `make mini-all` according to the
files changed; their existing passing state is a baseline, not unfinished work.

Stop an investigation when repeated probes establish an environment limitation
without new project evidence. Save the reproducer, name the required external
dependency, and move to the next actionable item. Do not repeatedly rerun broad
suites or polish historical documentation while a higher-priority code gap is
actionable. Report progress as ledger rows closed, failures fixed, and target
runs obtained; estimate remaining effort after I01 and environment availability
are known, rather than inventing a percentage from test counts.

I07 requires all original Phase 0–3 and basic Phase 5 obligations to be accounted
for with evidence matching their scope. Unsupported, unrepresentable operations
must have the specified user-visible outcome. Known safety gaps and required
but unavailable target evidence remain open. Any proposed reduction in platform
or capability scope requires an explicit product decision; relabelling a target
unqualified is not completion of its required work. Only then reconcile the
matrix and release documents and declare the archive-format goal complete.
