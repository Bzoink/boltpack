# Interoperability completion plan

**Updated:** 2026-10-28

**State:** execution plan; the tracked implementation goal is active.

**Current requested focus:** close the remaining interoperability gaps that can
be developed and verified on this machine, including available emulator checks.
Keep native-only qualification gaps visible; completing local work does not
prove the wider original goal.

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
DOSBox. DOSBox 0.74-3 still fails the accented-name host-mount and direct-create
probes. On 2026-10-28, `test/check_dosboxx_cp437.py` verified a Linux-created
`café.txt` archive extracted to a DOSBox-X-mounted FAT12 image: mtools read the
exact CP437 filename and bytes back, and the FAT directory timestamp matched
the expected value. DOS16 reported partial because some Linux metadata has no
DOS representation. The same run created a DOS archive on FAT12 and Linux
verified its contents and extracted bytes. Both readers report partial for
metadata their platform cannot represent. Host-mounted CP437 extraction still
reports partial and does not create the expected host file. DOS16 archive edit
commands are not provided. Full DJGPP execution separately needs a working
DPMI host. The 16-bit resource measurements still need a reproducible report.

Hatari 2.5.0 ran for 250 frames with TOS 1.62. FS-UAE 3.1.66 booted Kickstart
2.04 and a copy of the Workbench 2.04 disk to the desktop under Xvfb with
software rendering; accelerated floppy access and warp mode got past a long
white startup, and answering `0` then Enter dismissed the keymap prompt. These
are startup checks only: BoltPack has not yet run in either emulator. Wine is
installed, but a fresh-prefix Windows CLI probe failed with a missing component;
32-bit Wine support is also absent. QEMU x86 is installed; guest OS images
must be supplied before it can run Windows or OS/2.

On 2026-10-28, the initial core audit found that nonzero reserved archive-header
flags were rejected by full extraction/listing but accepted by mini readers and
some full-core info/edit paths. Those paths now reject the flags with
`BP_ERR_CORRUPT`; regression fixtures use a valid header CRC and cover full and
mini readers plus append, copy, and delete. `make CC=gcc unit-test` and
`make mini-all` passed. The complete I01 requirement ledger remains outstanding.

Hatari 2.5.0 with TOS 1.62 has run the full Atari CLI using a disposable
GEMDOS host-directory drive. `test/check_hatari_interop.py` records guest exit
statuses and verifies Stored and BoltLite file bytes in both directions with
Linux. It also verifies append and delete, failed edit preservation, and
checksum failure cleanup over an existing file. The test exposed TOS libc's
unstable synthesized inode values; archive edit identity checks now use file
type, size, and timestamps. Same-size substitutions with matching timestamps
cannot be excluded. Linux reports partial preservation of Atari-specific
metadata. This host-directory result does not qualify target attributes or
timestamps, or GEM GUI use.

On 2026-10-28, `test/check_hatari_fat.py` also ran the full CLI against a
disposable 720 KiB FAT12 floppy image under Hatari. Linux-created BoltLite
archives extracted on the floppy; Atari create, append, extract, delete, and
test succeeded; the guest copied restored file bytes and the edited archive
back to Hatari's GEMDOS host drive, where Linux verified them. TOS reports
different `st_dev` and synthesized `st_ino` values for the same FAT file via
pathname and handle queries, so Atari edit identity checks now compare regular
file type, size, and timestamps only. The disk image itself did not retain the
guest's directory changes after Hatari exited, despite sector writes and
successful in-guest reads. The test therefore qualifies the FAT12 guest
filesystem operations and byte round trip during the run, but not durable
floppy-image writeback. Native attribute and timestamp restoration and the
GEM GUI remain open.

The same host-directory test now transfers a nested tree with an empty
directory both ways and verifies pre-output refusal for a long 8.3 name and a
case-colliding pair. Atari source enumeration required conversion of `/` to
GEMDOS separators for `Fattrib`; extraction now scans archive names before
opening output files. The host-directory backend still refuses restoration of
directory timestamps, which returns partial. The current collision index tracks
implicit parent directories as well as complete projected paths; host tests
cover aliases and file/directory prefix conflicts in both entry orders. Hatari
also verifies CLI refusal of an implicit-parent case collision before output.
The GEM GUI now uses the same index for extraction and source-name checks and
its staged replacement check no longer relies on TOS's unstable synthesized
inodes. The GUI cross-build passes; its actual UI and replacement behavior
still need emulator runtime validation.

The Amiga GadTools GUI now includes directories, including empty directories,
and captures native protection and comments for files and directories. It
converts Latin-1 source names to UTF-8, checks projected names and implicit
parent collisions, and stages archive creation beside its destination. Traversal
includes dot-prefixed names, rejects links and non-regular sources, and reports
enumeration and I/O failures. Retained host tests cover traversal, source names,
collision preflight and staged publication; `make amiga-gui` cross-builds the
GUI. These checks are not an Amiga runtime test: emulator round trips and
pathname-based source and parent substitution remain open. Replacement reports
partial preservation when the old archive's native metadata cannot be retained.

FS-UAE with Workbench 2.04 and Kickstart 2.04 has run the Amiga CLI on a
disposable host-directory drive: `test/check_fsuae_interop.py` checks Stored
and BoltLite profile file bytes in both directions with Linux, profile append,
and rejection of incompatible options, and records guest statuses. A default
shell-stack create attempt failed to complete and left an invalid archive.
The core BoltLite workspace now uses heap storage, and recursive Amiga source
path buffers no longer occupy large stack frames. The current FS-UAE corpus,
including nested directories and edits, passes with the default shell stack;
the runner defaults to 256 KiB and accepts `--stack 0` for this check. Deep
recursion and other codecs still require stack qualification. The CLI exposes Interchange Profile 1 for
create and add. Creation, append, and delete now write to sibling stages; FS-UAE tests
verify that a missing second input leaves existing archives unchanged and a
failed new archive unpublished. Replacement preserves captured Amiga
protection and comments or fails before publication; protected targets are
refused. The host-directory emulator test confirms a nondefault protection bit
and comment survive append and delete, and unchanged archive bytes after a
failed delete. It also checks a file, a directory and an empty directory
retain native protection and comments through an Amiga archive, and that a
corrupted stored payload leaves an existing file and its metadata unchanged.
`test/check_fsuae_native.py` separately exercises disposable native OFS and
FFS floppies: BoltLite profile transfer, create/append/delete, failed-append
preservation, file protection, comment and timestamp restoration, and Linux
extraction of the edited archive pass. The tests transfer `café.txt` in both
directions, and verify that an unrepresentable Unicode name and an implicit
case-colliding parent are refused before creating output. The CLI preflight
now tracks exact and folded parent paths and refuses unrepresentable names.
These native-disk tests use a 256 KiB
guest stack.
Interruption recovery, broader native-disk behavior, deep-recursion stack sizing,
broader metadata validation, and GUI runtime remain open.

Amiga CLI traversal rejects source types reported as links or non-regular
files, reports directory enumeration and close failures, and fails when native
metadata cannot be read rather than inventing attributes. FS-UAE's
host-directory mount exposed a host symlink as a regular file: the CLI followed
it and archived its target despite `lstat`. An FFS `MakeLink ... SOFT` probe
returned an error, so native soft-link behavior is not yet qualified. This
backend limitation and path-based source substitution remain open.

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
| I06 | In parallel with I02–I05: develop and run emulator/target fixtures | Use the installed emulators as described below, fix failures, and retain results tied to the tested commit and filesystem. A missing compiler, SDK, OS image, ROM, or runtime remains an explicit dependency. |
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

## I06: emulator development and test sequence

The verified local startup commands and image locations are recorded in
[emulator setup](emulator-setup.md).

Start with the installed emulators and the user's images under `~/images`.
Keep ROMs and OS disks out of the repository. Make disposable disk copies and
exchange directories for each run; never alter the user's originals. Expose
image and emulator paths as runner arguments or environment variables rather
than requiring this particular home directory on another machine. Save the
commands, guest output, exit/result markers, manifest, and emulator/OS versions
with the test result. Use explicit timeouts and terminate only processes started
by that runner.

1. **Atari, Hatari:** `make atari-cli`, `make atari-gui`, and TOS 1.62 startup
   are verified. `test/check_hatari_interop.py` qualifies Linux exchange, nested
   and empty directories, collision refusal, corruption cleanup, and archive
   edits on Hatari's GEMDOS host-directory drive. `test/check_hatari_fat.py`
   additionally qualifies in-guest Linux ↔ Atari Stored/BoltLite byte transfer
   and archive edits on a disposable FAT12 floppy; its guest runner captures
   output before emulator shutdown because Hatari does not retain the changed
   directory entries in the image. Next, verify native attributes/timestamps,
   durable image writeback with a suitable emulator/eject workflow, interruption
   recovery, and the GEM GUI. Keep host-directory and FAT-image results distinct.
2. **Amiga, FS-UAE:** use Kickstart 2.04 and Workbench 2.04 under `~/images/Amiga`.
   Boot a disposable copy of `Workbench-2.04/1-Workbench.adf`; Xvfb, software
   OpenGL, `--floppy_drive_speed=800`, and `--warp_mode=1` worked in the startup
   check. The initial keymap prompt must be handled reproducibly. Confirm the
   `make amiga68k` toolchain is available and build the CLI and GadTools GUI;
   if it is missing, provision it before claiming a target build. Arrange a
   writable exchange disk or directory and a guest command/status mechanism.
   Run Linux ↔ Amiga profile transfers, then check Latin-1 names, case collisions,
   protection bits/comments, times and directories, failed extraction, and
   archive-edit recovery. Test the GUI independently. Capture the emulator's
   own frame when needed; generic X11 captures can be misleading. Validate
   mounted-directory and native disk filesystem behavior separately.
3. **DOS, DOSBox-X:** `test/check_dosboxx_cp437.py` passes CP437 filename and
   byte extraction, timestamp restoration, and DOS → Linux archive creation on
   a FAT12 image. Preserve the separate DOSBox 0.74-3 failure and DOSBox-X
   host-directory result. DOS16 mini does not provide archive edit operations.
   Confirm whether a DPMI host is present before attempting the full DJGPP
   binary; qualify full-DOS archive edits separately if a usable DPMI runtime is
   available.
4. **Windows, Wine then Windows:** repair the isolated Wine-prefix setup and
   provision 32-bit Wine where needed. Cross-build Win32/Win64 binaries, run
   the existing Windows CLI fixture where Wine supports its APIs, and add the
   Unicode and edit cases as each code slice lands. Record Wine version and
   filesystem. Wine is an early feedback environment, not evidence of native
   Windows NTFS/FAT, reparse, GUI, .NET, or Total Commander behavior; retain
   those target runs in the queue below.
5. **QEMU:** continue the existing QEMU-i386 Linux-mini checks. For full DOS,
   Windows, OS/2, or other guests, use a suitable guest OS image and a prepared
   writable test disk only after confirming availability. Record guest and disk
   setup; an installed emulator alone is not a running target OS.

For every emulator slice, first reproduce a specific missing behavior, add a
fixture with an independent expected result, implement the smallest code change,
and rerun the focused guest and host checks. Expand the corpus only after that
slice passes. An emulator startup, a successful build, and a desktop screenshot
are not archive-transfer or attribute-preservation results.

## Remaining target environment queue

This groups execution work without removing any frontend from scope. I01 must
include every shipped adapter, including wrappers omitted from this short table.
Use Hatari, FS-UAE and DOSBox-X now while obtaining Windows/macOS and other
missing native environments in parallel.

| Target group | Environment needed | Qualification work |
| --- | --- | --- |
| Linux CLI/mini/FLTK | Native Linux, QEMU-i386, usable GUI display | Reuse existing lifecycle/profile/race fixtures; add actual GUI creation, cancellation, extraction, flattened-name collisions, and metadata checks. |
| Windows CLI/native GUI/FLTK/.NET/WCX | Windows with NTFS and FAT-compatible test volume, .NET build tools, Total Commander plugin host | Run `python test/check_windows_cli.py build/win64/boltpack.exe`; extend it for I02/I03, build the managed GUI, and exercise every UI/plugin path. Record denied hard-link/reparse privileges as missing evidence. Cross-build both word sizes. |
| macOS CLI/Cocoa/legacy GUI | macOS SDK/runtime; case-sensitive and case-insensitive test volumes; supported legacy environment where shipped | Verify Unicode normalization, Apple xattrs/Finder info/resource forks, time/mode restoration, publication, and actual GUI flows. Host declaration stubs remain supporting evidence only. |
| DOS16/full DOS/Win16/OS2-16/full OS2 | Independent DOS runtime with FAT, DPMI host for DJGPP, Windows 3.x, OS/2 with FAT and HPFS | Resolve accented-name failure; qualify codepages, FAT flags, deferred directories, timestamps, profile transfer, and edit/recovery behavior. DOSBox conversion tests do not qualify the other adapters. |
| Atari CLI/GEM GUI | Hatari and TOS 1.62 are installed; prepare writable guest media | Run projection/profile corpus, attributes and dates, GUI creation/extraction, staged edits, and interruption recovery. |
| Amiga CLI/GadTools and OS4/MorphOS/AROS variants | FS-UAE and Workbench 2.04 are available for 68k; matching SDKs and runtimes are needed for each other variant | Build each variant, then verify Latin-1 mapping, native protection/comment precedence, directory capture/restoration, staging, and recovery. Shared source and a 68k emulator do not qualify all variants. |
| Classic Mac 68k/PPC | Complete application/resource builds and Mac OS 9 runtime | Test MacRoman/HFS collisions, resource forks, Finder fields, directory dates, bounded preflight, partial failures, and profile transfer for both builds. |
| QL/X68000/RISC OS/Plan 9/CPM-86 | Matching compiler and native filesystem runtime/emulator for each | Replace syntax/host-shim-only evidence with target builds and the applicable corpus. Exercise native path mappings, limits, metadata, and refusal behavior. |
| Haiku CLI/BeAPI GUI | Haiku compiler/runtime and BFS test volume | Complete and qualify collision/symlink handling, native attributes, timestamps, profile behavior, and GUI outcomes. |
| Other POSIX wrappers | Native environments for BSDs, Hurd, Solaris/illumos, AIX, z/OS, QNX, and remaining shipped wrappers | Validate actual APIs and feature gates, filename encoding, modes/times, safe traversal, unsupported metadata reporting, and edits. Record OS/compiler versions, especially AIX and z/OS. |

Each run must record commit, frontend/build flags, compiler, OS/emulator version,
filesystem, locale/codepage, timezone, commands, expected/observed result, and
retained logs. A skip is not a pass. Emulator evidence must name the emulator
and filesystem path used; it is not a hardware-performance measurement.

## Execution and completion rules

On resumption, begin I01 and the Atari, Amiga, and DOS emulator setup above;
select the first unresolved I02/I03 code item that those fixtures reproduce.
Obtain the Windows/macOS and other legacy environments concurrently with that
work. Keep one concrete defect or missing
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
