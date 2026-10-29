# Interoperability completion plan

**Updated:** 2026-10-29

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

The latest `make CC=gcc test` run passed on 2026-10-29 after adding shared
Amiga metadata-decoder and GUI directory-queue host tests to the aggregate
target. It also passed the archive-format units, 160 Linux CLI integration
checks, source/extraction race and failure fixtures, and the existing DOS,
QL, X68000, Plan 9, RISC OS, OS/2 publication, Haiku, and macOS host-shim suites.
Available FS-UAE OFS/FFS, Amiga 68k CLI/GUI cross-builds, and Linux mini QEMU
checks were run as separate focused targets.

After the Plan 9 extraction fix, the aggregate `make CC=gcc test` passed again.
Its host-shim fixture rejects both file/child conflict orders before output
creation and verifies deferred directory/file metadata restoration. Native
9front amd64 under QEMU TCG also restored archived 0550/0640 modes and directory
mtime; the Plan 9 mode update preserves `DMDIR`. RISC OS target filesystem
behavior remains unqualified despite its host-staging checks.

The 2026-10-29 local regression pass completed `make CC=gcc test` with all
160 Linux CLI integration checks passing, including the legacy profile host
runners and the new QDOS component-projection test. Separate targeted passes
completed `mini-all`, `unit-test-dos16`,
`dos16-stack-interoperability-test`, `linux-gui-interoperability-test`,
`source-race-test`, `x68k-host-test`, `macosx-host-test`,
`posix-dirfd-host-test`, `cpm86-host-test`, `linux-cli-extract-failure-test` (file staging failures and deferred directory metadata failure), `haiku-host-stage-test`,
`riscos-path-test`, `riscos-stage-host-test`, `win32`, `win64`, and the Win64
Wine CLI fixture. These host and emulator results do not close the native-only
qualification gaps listed in the matrix.

The full `make CC=gcc test` gate was rerun after adding the Linux CLI extraction
failure fixture and the OS/2 staged-publication policy test to the aggregate
target. It passed, including the 160 Linux CLI integration checks, source-race
and symlink publication fixtures, POSIX dirfd host builds, RISC OS/QL/X68000/
Plan 9 Profile 1 cross-reader checks, DOS host lifecycle/profile checks, Haiku
host-shim fixtures, the OS/2 publication rollback cases, and file and
deferred-directory extraction failure cases. This remains host/cross-build
evidence; it does not close the target-runtime gaps below.

The subsequent Linux mini audit found that its deferred-directory metadata
queue stopped at 1,024 entries even though whole-archive preflight allows
65,535 names. The queue now uses the same entry bound as preflight, appends
nodes in constant time, and sorts them in place by path depth before applying
metadata. The QEMU-i386 interoperability runner extracts 4,097 child
directories and checks all names plus pinned parent/child mtimes. `make CC=gcc
linuxmini-interoperability-test` passes, including the full/mini extraction
race fixtures.

After adding QL staged extraction, full-table name preflight, and corrupt-
payload preservation cases, `make CC=gcc test` passed again on 2026-10-29.
The run included the QL host fixture, all 160 Linux CLI integration checks,
source-race/publication fixtures, and the remaining host interoperability
suites. An isolated XTC68/C68 `qcc` SDK was installed under ignored `build/`
and built the QL target on 2026-10-29. On the same date, sQLux was built with
the bundled Minerva ROM and used to run the QDOS executable. A Stored archive
created by QL passed Linux full and mini integrity checks and exposed the
expected 31-byte payload. The first native extraction attempts exposed a path
projection bug: QDOS requires `_` separators and a device-qualified path, while
the frontend used POSIX-style slash paths and bare staging names. After fixing
projection and device inheritance, `ql-sqlux-interoperability-test` passed
QDOS create/Linux full+mini read, Linux-to-QDOS extraction, nested directory
extraction, `-o` extraction, and exclusive stage-collision preservation with
exact payload comparison. See [`emulator-setup.md`](emulator-setup.md) for the
setup and repeat command.

The QL build exposed a missing `boltlite.c` translation unit in the Makefile
target (the special-SDK script already included it); the target now links it
and rejects compiler logs that report unresolved symbols. The direct build
recipe in `frontends.md` now includes the same source. sQLux qualifies QDOS
archive creation and extraction across its hostFS backend, including nested
paths, an occupied stage name, forced replacement, and exact file and
directory timestamp round trips with a UTC emulator clock. The frontend sets
dates through QDOS `FS.DATE` and restores directory dates deepest-first after
all children. The host fixture injects both publish and rollback failures and
confirms it retains the original recovery copy and verified stage if both
operations fail. QDOS has no timezone field. `--tz-offset MINUTES` supplies a
fixed minutes-east-of-UTC offset for source and destination timestamps; an
omitted option retains the zero-offset/UTC assumption. Malformed or
out-of-range values make creation times unknown and extraction partial. The
offset has no daylight-saving logic. Host tests cover parsing, range
boundaries and conversion round-trips; sQLux now verifies the +60 minute
extraction path. Timestamps beyond QDOS's signed date range and filing systems
without `FS.DATE` support report partial. Native rollback after publication
failure, crash durability, protection flags, and physical QL filing systems
remain unqualified.
Setup details are in [`emulator-setup.md`](emulator-setup.md).

Haiku R1/beta6 x86_64 was downloaded outside the repository, checked against
the published SHA-256, and booted to its live desktop under QEMU on 2026-10-29.
The headless Xvfb input path has not yet provided a repeatable way to launch
Terminal, so compiler availability and native CLI/BeAPI execution remain
unverified. The exact image and boot command are recorded in
[`emulator-setup.md`](emulator-setup.md); resume with a reliable guest command
channel instead of treating the desktop boot as target qualification.

After preserving Plan 9's `DMDIR` bit during native permission restoration,
`make CC=gcc test` passed again, including the stricter Plan 9 host shim that
rejects mode updates which clear the directory type. The 9front amd64 guest
also extracted a native archive with a 0550 directory, 0640 file, and restored
directory timestamp. The latest aggregate run also includes the RISC OS Profile
1 host SWI transfer fixture; its create/append output passed Linux full and
mini integrity and payload checks.
The RISC OS staged-extraction host test is now part of the aggregate `test`
target. `make CC=gcc test` passed after adding an end-to-end full-core fixture
containing foreign native and portable FAT metadata: extraction returns partial,
publishes the exact primary payload, and leaves no stage. The full run reported
279 archive-format checks and 160 Linux CLI integration checks passing. The SWI
shim still does not qualify native RISC OS behavior.

The 9front amd64 build recipe now uses native bare `-D` architecture markers.
The native compiler rejects `-DNAME=value`, so creator identity is detected in
the core from `__PLAN9__` and `PLAN9_CPU_X86_64` (with i386 and ARM markers also
supported). A fresh guest-built v6 archive reported Plan 9/x86-64 in both the
guest CLI and Linux CLI after transfer through the FAT disk. The focused
`make CC=gcc plan9-host-test` profile and metadata checks passed after the fix.
The subsequent aggregate `make CC=gcc test` also passed after the Classic Mac
68k copied core was synchronized, including its core-sync gate, RISC OS profile
transfer, Plan 9 host-shim checks, and the Linux CLI integration suite.

On 2026-10-29, `make CC=gcc haiku-host-stage-test` passed after Haiku CLI
create, append, and delete were moved to sibling archive stages and extraction
gained bounded whole-archive path preflight. The fixture checks that failed
create/append/delete preserve the old archive and leave no stage, successful
append/delete output remains readable by Linux, and a file/directory path
conflict is refused before the output root is created. It verifies existing
Unix archive mode survives create-overwrite, append, and delete. It also retains
staged extraction and Profile 1 checks. A host `rename` fault injection also
verifies failed append publication preserves the original archive and removes
the stage. This host shim does not verify BFS case mapping, attributes, native
APIs, or native path races.

The same `haiku-host-stage-test` now creates a file and directory tree whose
source timestamps are one second before the Unix epoch. It verifies that all
primary records have zero mtime with `MTIME_VALID` clear, that Haiku's former
32-bit cast is gone, and that Linux reads and extracts the payload bytes
exactly. This exercises the Haiku frontend on a host shim; it does not qualify
BFS timestamp resolution or native runtime behavior.

`make CC=gcc macosx-host-test` now also creates a macOS-CLI archive from a
pre-epoch host file and inspects the format-6 table directly. The mtime is zero
with `MTIME_VALID` clear, and the frontend integrity check passes. This closes
the macOS CLI host regression for signed timestamp wrapping; it does not
qualify a macOS filesystem or Apple runtime.

The full DOS frontend host fixture now does the same for a negative source
mtime. `make CC=gcc dos-host-test` passes with the DOS host shim encoding zero
and clearing `MTIME_VALID`, then validating the archive. DOSBox/DJGPP FAT times
remain constrained to the filesystem's supported range; the host result checks
that a signed POSIX time cannot wrap into a plausible unsigned format-6 value.
The DJGPP build is warning-free, and
`make CC=gcc CWSDPMI=/path/to/CWSDPMI.EXE
dos-djgpp-dosboxx-fat12-test` also passes create/append/delete/test/extract on
DOSBox-X FAT12 after this change.

The RISC OS writer now distinguishes an absent/pre-1970 5-byte datestamp from
valid epoch zero, writes unknown time explicitly, and retains the full
post-epoch seconds value rather than narrowing it to 32 bits. The profile host
fixture uses pre-epoch file and directory mtimes, checks the table flags, and
passes Linux full and mini integrity/extraction checks with
`make CC=gcc riscos-profile-host-test`. This runs the production frontend
through the host SWI shim; native RISC OS filesystem behavior remains open.

The QL CLI no longer labels direct file arguments as Unix epoch zero when no
parent qdirect timestamp is available. Recursive source traversal carries
regular-file and directory dates from qdirect records; pre-epoch or missing
dates are unknown, while QDOS's exact Unix epoch remains valid zero. The
sQLux hostFS directory probe returned an empty-name qdirect record, so
recursive source enumeration is not target-qualified here. `make CC=gcc
ql-host-test` checks unknown direct-file times and Linux full/mini archive
verification. Extraction now restores representable file and directory dates
through `FS.DATE`; sQLux hostFS verifies exact nested timestamp restoration
with a UTC emulator clock. The QDOS frontend accepts a fixed
`--tz-offset MINUTES` in minutes east of UTC, defaulting to zero and without
daylight-saving logic. The +60 minute setting is now verified in sQLux; other
offsets and physical filing systems remain unqualified.

QL extraction now writes each regular file to a short same-directory staging
name created with `O_CREAT|O_EXCL`; the leaf shrinks to fit the remaining
space in QDOS's 36-byte relative path bound. The status completion callback
closes the stage and publishes it only after the core has verified the entry
checksum; checksum and close failures remove the stage and preserve any
existing destination. Forced replacement
renames the old destination to a sibling backup, publishes the verified stage,
and rolls the backup back if publication fails. Parent-directory creation
errors are reported. `make CC=gcc ql-path-test` covers the 1-byte stage-name
boundary calculation for a nested 36-byte member path. `make CC=gcc
ql-host-test` exercises valid extraction, restoration of representable file
timestamps, preservation of a pre-existing
exclusive-stage-name collision, no-force refusal, corrupt-payload rejection
with and without force,
preservation of an existing target, and stage cleanup. It also injects a
verified-stage publication failure after the old target has moved to its
backup and confirms the old bytes are restored with no leftover stage or
backup. This host fault injection qualifies the transaction logic, not the
native QDOS rename implementation. The host run does not qualify concurrent
writers or target filesystem crash durability. The
`ql-sqlux-interoperability-test` additionally exercises native exclusive stage
creation, close, rename publication, file and directory timestamp restoration,
and extraction under sQLux hostFS. The test runner sets `TZ=UTC` because QDOS
does not store a timezone.
If both stage publication and rollback fail, the host fixture confirms the
frontend reports the retained backup and keeps both the original bytes and
verified replacement stage for manual recovery. Native forced-replacement
rollback after a publication failure, crash durability, metadata, and behavior
on physical QL filing systems remain unqualified.

The Windows native and FLTK GUIs had the same pre-epoch timestamp conversion
defect as the Windows CLI: their `time_t` values were cast to the unsigned
archive timestamp without checking representability. Both Win32/Win64 native
GUI builds and both FLTK GUI builds pass after archive creation was changed to
mark negative or unrepresentable times unknown. The Cocoa and Haiku BeAPI GUI
create paths received the same source fix, but their SDK/toolchain is
unavailable here, so neither path is build-qualified. No GUI runtime currently
qualifies these frontend changes.

On 2026-10-29, `make CC=gcc amiga68k` passed after Amiga CLI source identity
checks were added. Native OFS and FFS exchange fixtures (`--skip-codecs
--depth 4`) passed, and `make CC=gcc
XDFTOOL=build/venv-amiga-tools/bin/xdftool fsuae-source-race-test` replaced a
regular source pathname after open. The CLI rejected the change, retained the
existing archive byte-for-byte, and removed its stage. This does not close
path-based transient swap-and-restore races or the host-mount symlink behavior.

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
commands are not provided. `make CC=gcc
dosboxx-fat12-cp850-interoperability-test` also passes on the
same DOSBox-X FAT12 setup. It compiles the mini with CP850, passes `Ø.TXT`
through DOS batch arguments, creates an archive Linux lists and extracts with
the UTF-8 name intact, and extracts a Linux-created `Ø.TXT` archive for byte
comparison. mtools confirms the FAT short name uses the CP850 uppercase byte
`0x9D`. This qualifies the DOS16 CP850 path for this FAT12/DOSBox-X workflow;
Win16 and OS/2-16 remain open. A lower-case `ø.TXT` probe did not open the
mtools-created FAT12 entry in DOSBox-X. Even though the guest reports codepage
850, its `DIR ø.TXT` cannot match short-name byte `0x9D`; lowercase lookup stays
unqualified on DOSBox-X. On 2026-10-29, `make CC=gcc
freedos-cp850-interoperability-test` passed under QEMU 10.0.13 with FreeDOS
1.4. The runner loads `EGA.CPX`, prepares/selects codepage 850 using FreeDOS
DISPLAY/MODE, then changes the active codepage with CHCP. DOS `DIR ø.TXT`
finds the CP850 FAT entry; DOS16 creates an archive Linux reads as UTF-8
`ø.TXT`, and DOS16 extracts a Linux-created `ø.TXT` archive with matching
bytes. The FAT short name uses uppercase byte `0x9D`. This qualifies lowercase
CP850 round trips on this FreeDOS/FAT12 setup; Win16 and OS/2-16 remain open. Full DJGPP execution
now passes with CWSDPMI under DOSBox-X: the 32-bit CLI creates and tests an
archive on FAT12, extracts a
Linux-created archive, and Linux independently tests and extracts the
DJGPP-created archive byte-identically. Run
`make CC=gcc CWSDPMI=/path/to/CWSDPMI.EXE dos-djgpp-dosboxx-fat12-test` with
`CWSDPMI.EXE` from DJGPP's `csdpmi` package. This qualifies the DOSBox-X FAT12
path, not native DOS crash recovery or all FAT/API variants.
The runtime test also appends a member and deletes the original member, while
HIDDEN, SYSTEM, and ARCHIVE flags are set on the archive; Linux verifies the
edited archive and exact payload afterward, and mtools confirms the flags
survive. The runner retains a machine-readable manifest with revision/worktree
state, compiler and emulator versions, image and binary hashes, FAT12 geometry,
and independent input hashes. It exposed two
DOSBox-X FAT identity differences: `lstat` and `fstat` report different mode
bits for one file, and rename changes its inode number. The DOS frontend now
compares file type and stable object fields for the opened target and verifies
the recovery rename without relying on the path-derived inode. Create/append/
delete and attribute-preserving publication now pass the full FAT12 round trip.
[16-bit resource accounting](16-bit-resource-accounting.md) now
records Watcom map sizes, requested buffer bounds, and DOSBox entry/index/queue
limit checks and 26 core/shared-mini allocation-failure injections, including
writer, reader, extraction, index, queue, path, name conversion, and one for
the projected-index scratch buffer. Three additional DOS16 CLI-level failures
cover the shared DOS16/Win16/OS2-16 directory-walker buffer helper. They report
`BP_ERR_NOMEM`, leave no tracked leaks, and preserve the destination. The DOS16 writer now sizes each recursive source-walker path buffer to its current
path and the DOS `find_t.name` capacity, and reserves an 8 KiB stack. The DOSBox
fixture creates a 33-level directory tree; Linux lists and extracts the archive,
then the runner verifies the exact leaf bytes. The instrumented run observes
3,770 bytes of stack remaining and a 361,184-byte low-water largest block from
the DOS memory manager. Earlier 2 KiB-stack measurements were 172 bytes before,
and 627 bytes after, moving projected-name scratch to the heap. These are
workload observations, not worst-case bounds; the DOS block is not a guaranteed
Watcom C-heap capacity. The heap allocation has an injected failure regression.
Allocator overhead, worst-case stack use, and Win16/OS2-16 runtime behavior
remain open.

DOS16 archive creation now writes an exclusive sibling stage, finalizes and
closes it before publication, and uses a backup rename when replacing an
existing archive. The DOSBox/Linux interoperability fixture creates an archive,
replaces it with different contents, and verifies the replacement through the
Linux reader; it also checks that successful publication leaves no stage or
backup. Allocation-failure cases verify an existing destination's bytes survive
failed creates. Replacement has a short interval where the target is held under
its recovery name, and DOS path operations cannot prevent concurrent parent or
target substitution. Read-only and directory targets are refused. DOSBox
host-mounted testing does not qualify DOS filesystem crash behavior. The
expanded `test/check_dosboxx_cp437.py` run under DOSBox-X 2025.02.01 on a FAT12
image replaces an existing archive after setting HIDDEN, SYSTEM, and ARCHIVE
bits; Linux verifies the replacement contents, and `mattrib` confirms HIDDEN
and SYSTEM remain set. ARCHIVE is also set on this regular-file fixture. This
qualifies these bits on this emulated FAT12 path, not on other DOS runtimes or
filesystems.

Hatari 2.5.0 ran for 250 frames with TOS 1.62. FS-UAE 3.1.66 booted Kickstart
2.04 and a copy of the Workbench 2.04 disk to the desktop under Xvfb with
software rendering; accelerated floppy access and warp mode got past a long
white startup, and answering `0` then Enter dismissed the keymap prompt. These
were startup checks only; the Atari and Amiga emulator work has since advanced
to guest filesystem transfer tests described below and in the matrix. Wine is
installed, but a fresh-prefix Windows CLI probe failed with a missing component;
32-bit Wine support is also absent. QEMU x86 is installed; guest OS images
must be supplied before it can run Windows or OS/2.

On 2026-10-28, the initial core audit found that nonzero reserved archive-header
flags were rejected by full extraction/listing but accepted by mini readers and
some full-core info/edit paths. Those paths now reject the flags with
`BP_ERR_CORRUPT`; regression fixtures use a valid header CRC and cover full and
mini readers plus append, copy, and delete. `make CC=gcc unit-test` and
`make mini-all` passed. The I01 ledger is now recorded in
[interoperability-requirement-ledger.md](interoperability-requirement-ledger.md);
it inventories every matrix frontend/build, including the copied Classic Mac
source kit and DLL consumer, and links fixture environments and open gaps.

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

On 2026-10-29, `test/check_hatari_interop.py` also passed with TOS 1.06 and
TOS 2.05. These runs cover the same Linux/Atari Stored and BoltLite exchange,
archive edits, failure preservation, checksum cleanup, nested/empty
directories, and name-collision refusal as the TOS 1.62 run. Their evidence
directories are `/tmp/boltpack-hatari-pp7plo0t` (1.06) and
`/tmp/boltpack-hatari-he45l44l` (2.05).

On 2026-10-28, `test/check_hatari_fat.py` also ran the full CLI against a
disposable 720 KiB FAT12 floppy image under Hatari. Linux-created BoltLite
archives extracted on the floppy; Atari create, append, extract, delete, and
test succeeded; mtools verified restored file bytes and the edited archive
directly from the saved image after Hatari's normal quit/eject. TOS reports
different `st_dev` and synthesized `st_ino` values for the same FAT file via
pathname and handle queries, so Atari edit identity checks now compare regular
file type, size, and timestamps only. `--run-vbls` exits before saving the
floppy, so the test uses control-console quit/eject. This qualifies durable
FAT12 writeback for that workflow. FAT12 file hidden/system flags and a fixed
2000 timestamp round-trip successfully. Nested and empty directories and their
file bytes transfer, but TOS 1.62 `utime` fails on directories and `Fattrib`
cannot restore nonzero attributes to newly created directories; the CLI
reports this as partial. For source directory enumeration, a failed `Fattrib`
on a path that `stat` confirms is a directory is treated as the default zero
optional flags rather than aborting archive creation. The GEM GUI remains open.

On 2026-10-29, the TOS 1.62 FAT12 fixture was extended with a portable FAT
record whose name matches the extracted file but whose declared subject type
is a directory. The Atari CLI reports the archive failure, still restores the
payload, and does not apply the directory HIDDEN bit to the file. Core
extraction rejects the subject-type mismatch before frontend callbacks, and
frontend callbacks also use the shared `bp_metadata_matches_subject` check as
a defensive guard. This is now wired into the DOS, Windows, OS/2, Atari, and
X68000 portable-metadata paths. `test/check_hatari_fat.py` builds and tests
this malformed archive on every run, then verifies payload and FAT attributes
from the saved floppy image.
The repeatable entry point is
`make CC=gcc atari-hatari-fat-test HATARI_TOS="$HOME/images/Atari/tos162se.img"`;
it rebuilds both CLIs before launching Hatari.

The envelope-subject predicate is shared by the DOS, Windows, OS/2, Atari, and
X68000 extractors, so unknown schema versions and declared file/directory type
mismatches cannot reach their attribute-application branches. The current
validation passes `make CC=gcc unit-test`, DOS host extraction/profile tests,
X68000 host interoperability tests, the Atari FAT12 Hatari fixture, Mac OS 9
copied-core synchronization, OS/2 Watcom and Win64 CLI builds, and the full
Windows CLI fixture through `/usr/lib/wine/wine64`. The latter verifies serial
and parallel source mutations, file and parent-path substitutions, metadata,
profile transfer, archive edits, and read-only refusal. Re-running through the
generic `/usr/bin/wine` wrapper had failed during temporary-directory cleanup;
the documented `wine64` runner completed successfully.

The saved-image FAT12 fixture also passed with TOS 1.06 and 2.05 on
2026-10-29. Evidence is in `/tmp/boltpack-atari-fat-a066pl85` and
`/tmp/boltpack-atari-fat-6bdokojg`, respectively. This adds Atari OS-version
coverage while retaining the observed directory-metadata partial result.

On 2026-10-29, the TOS 1.06, 1.62, and 2.05 FAT12 runners verified
native-versus-portable attribute precedence using conflicting metadata records
in both archive record orders. In every case, native TOS HIDDEN won and
portable ARCHIVE was not applied. The fixture is generated by
`test/atari/conflicting_metadata_fixture.c`; Linux integrity checks and Hatari
extraction checks pass. Evidence: TOS 1.06
`/tmp/boltpack-atari-fat-0_ye2d3a`, TOS 1.62
`/tmp/boltpack-atari-fat-8lbfpw9y`, and TOS 2.05
`/tmp/boltpack-atari-fat-27zmstyp`. This closes the Atari CLI case for F3/I03
precedence, while equivalent conflict-order checks for other frontends remain
open.

The same host-directory test now transfers a nested tree with an empty
directory both ways and verifies pre-output refusal for a long 8.3 name and a
case-colliding pair. Atari source enumeration required conversion of `/` to
GEMDOS separators for `Fattrib`; extraction now scans archive names before
opening output files. The host-directory backend still refuses restoration of
directory timestamps, which returns partial. The current collision index tracks
implicit parent directories as well as complete projected paths; host tests
cover aliases and file/directory prefix conflicts in both entry orders. Hatari
also verifies CLI refusal of an implicit-parent case collision before output.
The GEM GUI uses the same index for extraction and source-name checks, and
its staged replacement check no longer relies on TOS's unstable synthesized
inodes. Hatari now qualifies startup/menu rendering, selector opening, one-file
source selection, v6 archive listing, and production GUI extraction on a
host-directory drive. A temporary default-button build exercised create/save;
production Create-button activation, native FAT metadata, cancellation, and
replacement behavior remain open.

The Amiga GadTools GUI now includes directories, including empty directories,
and captures native protection and comments for files and directories. It
converts Latin-1 source names to UTF-8, checks projected names and implicit
parent collisions, and stages archive creation beside its destination. Traversal
includes dot-prefixed names, rejects links and non-regular sources, and reports
enumeration and I/O failures. Retained host tests cover traversal, source names,
collision preflight, staged publication, the shared CLI/GUI Amiga metadata
decoder, and the production GUI directory queue/finalizer helper. The helper
test verifies deepest-first time/protection application, continues after a
restoration failure while reporting partial, and checks entry/path-byte limits. `make amiga-meta-host-test` verifies valid Latin-1 and empty comments,
rejects embedded NUL and malformed lengths without modifying outputs, and both
`make amiga68k` and `make amiga-gui` cross-build the consumers. The OFS/FFS
`fsuae-native-interoperability-test` also extracts a CRC-correct archive with
an embedded-NUL Amiga comment: extraction reports failure, preserves the
verified file bytes, and does not apply a truncated comment. This qualifies the
shared CLI decoder and native filesystems; GUI runtime and pathname-based source
and parent substitution remain open. Replacement reports
partial preservation when the old archive's native metadata cannot be retained.
An FS-UAE launch probe against the available Kickstart 2.04 and Workbench 2.04
images did not reach the GUI: Workbench reports “Local libraries failed to
load,” and the command exits with status 20. The supplied Workbench disks do
not contain `gadtools.library`, which the GUI opens at version 37. I also
checked the local Workbench 2.04 Extras ADF and Workbench 3.1 Workbench and
Extras ADFs; none contains that library. This environment therefore provides
cross-build evidence only; GUI startup and interaction remain unqualified until
a compatible GadTools runtime is supplied.

FS-UAE with Workbench 2.04 and Kickstart 2.04 has run the Amiga CLI on a
disposable host-directory drive: `test/check_fsuae_interop.py` checks Stored
and BoltLite profile file bytes in both directions with Linux, profile append,
and rejection of incompatible options, and records guest statuses. An early
default-stack create attempt failed before the stack reductions below; the
31-level default-stack corpus now passes. Because AmigaDOS does not grow
process stacks on demand, the CLI now carries an 8 KiB `$STACK` cookie and
calls libnix's stack initializer on startup; libnix expands a too-small process
stack and restores it on exit. An old binary stalled while creating even the
shallow three-level host-directory fixture at the 4 KiB default, while the same
fixture passed with an explicit 8 KiB or larger shell stack. The core BoltLite
workspace now uses heap storage, Amiga source directory
walking uses an explicit heap-backed traversal stack, and extraction callback
path buffers live in the extraction context. The FS-UAE host-directory test
passes nested tree create/extract with `--stack 0` (the shell's 4 KiB default),
including a 64-level source tree, and compares the leaf bytes. The deep fixture
uses one-character repeated directory names to stay within the legacy AmigaDOS
path limit. The CLI carries an 8 KiB `$STACK` cookie and explicitly expands its
stack through libnix on startup, then restores it at exit. Without the libnix
initializer, default-stack runs stalled inconsistently; explicit 8 KiB and
larger shell stacks passed. Evidence for the default-stack 64-level create,
extract, full profile/edit, and four-codec corpus is
`/tmp/boltpack-fsuae-_0zil2jv`. The same runner also creates Stored, BoltLite, BoltLZ, and
Fluff archives from a 512 KiB fixture using 256 KiB blocks; the Amiga guest
tests and extracts each archive, and Linux independently tests and extracts
the Amiga-created archives with byte-identical results. All four pass with the
default shell stack. This qualifies that block and payload workload; maximum
block sizes remain open. A second host-directory run at 512 KiB chip RAM, no
fast RAM, the default shell stack, and 16 KiB blocks passes profile transfer
and edit checks plus Stored, BoltLite, and BoltLZ round trips in both guest
and Linux. Fluff creation returns nonzero without publishing an archive or
leaving a stage under that memory configuration; extraction is not attempted.
The original run is retained at `/tmp/boltpack-fsuae-h_m6dzqc`; after adding
the 8 KiB libnix stack expansion, the same workload also passes at `--stack 0`
in `/tmp/boltpack-fsuae-ag4i8b4j`. An explicit 16 KiB shell-stack comparison
passes in `/tmp/boltpack-fsuae-rqay1fec`. These qualify constrained workloads,
not general heap-exhaustion behavior. The CLI exposes
Interchange Profile 1 for create and add. Creation, append, and delete now write
to sibling stages; FS-UAE tests
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
extraction of edited archives pass. It now creates and restores a nested tree
with protected/commented parent, nested, and empty directories plus a file;
the metadata probe confirms those native fields match exactly after extraction
on both OFS and FFS. Linux extracts the tree contents and reports its expected
partial metadata result. The tests transfer `café.txt` in both directions, and
verify that an unrepresentable Unicode name and an implicit case-colliding
parent are refused before creating output. The CLI preflight
now tracks exact and folded parent paths and refuses unrepresentable names.
These native-disk tests use a 256 KiB guest stack. Native OFS and FFS runs both
pass directory metadata capture/restoration after the traversal refactor. The
native runner now accepts `--depth` through 64 levels and `--stack`; deep
fixtures use short repeated directory names to keep cumulative paths within
AmigaDOS limits. Both OFS and FFS pass 49 nested levels using the Workbench
default shell stack with `--skip-codecs --timeout 300`. Each run creates an
archive from the native floppy tree, extracts it back to the floppy, then reads
the leaf from the saved image for byte comparison. With a 16 KiB shell stack,
OFS and FFS also pass 56- and 64-level create/extract/readback with 2 MiB chip
RAM and 8 MiB fast RAM. Earlier 64-level runs requested `C:Stack 4096`, which
matches the Workbench CLI default; the new `--stack 0` runs establish
default-stack behavior directly. Evidence for the earlier runs is recorded in
`emulator-setup.md`.
On 2026-10-29, both filesystems also passed 64 levels with `--stack 0` at those
memory settings, using a 300-second timeout; evidence is in
`/tmp/boltpack-amiga-ofs-2997misu` and `/tmp/boltpack-amiga-ffs-97_b_6rq`.
The guest probe records the process stack and CLI minimum: 3,200 bytes and
1,024 longwords (4,096 bytes), respectively. `--stack 4096` reports the same
values, so it does not establish a larger stack than the Workbench default.
The earlier default-stack timeouts were not reproduced with this timeout.
Native OFS and FFS also pass all four codecs at 256 KiB blocks, with
Linux cross-testing and byte-identical extraction. Both filesystems round-trip
three nested directory timestamps at 512 KiB chip RAM with no fast RAM. Retained
evidence is recorded in the emulator setup notes.
The new `test/check_fsuae_interrupt.py` kills guest extraction while its
sibling stage is being written and append while the original archive is being
copied to a stage. On the host-directory backend, the pre-existing destination
and archive remain unchanged; an incomplete stage remains for manual cleanup.
`make CC=gcc XDFTOOL=build/venv-amiga-tools/bin/xdftool fsuae-interruption-test`
now reproduces both 64 MiB cases. The 2026-10-29 run
passed with evidence in `/tmp/boltpack-amiga-interrupt-1_so52kc` (extract) and
`/tmp/boltpack-amiga-interrupt-h11wz8jn` (append).
`make CC=gcc XDFTOOL=build/venv-amiga-tools/bin/xdftool fsuae-publish-rollback-test`
also passes: a test-only build injects failure at
stage publication after the original moves to its recovery name. FS-UAE
verifies the original archive hash, Linux integrity check, and stage/backup
cleanup. Evidence is in `/tmp/boltpack-amiga-interrupt-ethaf3ml`. This covers
handled publication failure; hardware power-loss behavior remains open.
`fsuae-publish-interruption-test` kills the test-only CLI after the original
reaches its recovery name and before stage publication. It confirms the target
is absent, verifies the original backup and staged replacement with Linux,
then boots again and exercises the production `r` command. Recovery verifies
both archives, restores the original target and leaves the replacement stage
intact. The 2026-10-29 run passed with evidence in
`/tmp/boltpack-amiga-interrupt-jy3ea2br`. The regular
`test/check_fsuae_interop.py` corpus passed after the transaction-name change,
including staged edit behavior and all four codecs at 256 KiB blocks; evidence
is in `/tmp/boltpack-fsuae-oo0d3xik`.
Archive transaction paths share a six-character suffix and carry the first 32
ASCII alphanumerics of the target basename. The `r` command refuses an existing
target, requires a matching regular-file backup and paired regular stage, and
checks both archives before restoration. Automatic selection requires a unique
32-character hint; an optional exact backup leaf argument supports safe
selection when hints collide. The host-directory FS-UAE fixture verifies both
ambiguity refusal and explicit pair selection. `fsuae-native-publish-recovery-test`
also passes on disposable OFS and FFS ADFs after allowing floppy buffers to
flush before killing FS-UAE; the second boot restores the original and retains
the verified replacement. Hardware power-loss durability, broader native-disk
behavior, broader metadata validation, and GUI runtime remain open. The
available Workbench image set lacks
the GUI's GadTools library, so resolve the guest runtime before attempting
interactive GUI flows.

Amiga CLI traversal rejects source types reported as links or non-regular
files, reports directory enumeration and close failures, and fails when native
metadata cannot be read rather than inventing attributes. It now compares an
opened regular-file handle with its initial `lstat`, rechecks file identity and
size/timestamps after packing and native metadata capture, and checks directory
identity/timestamps during iterative traversal. `fsuae-source-race-test` pauses
after source open, replaces the source path from the host, and verifies the
archive operation fails while preserving the existing archive and removing its
stage. Directory-relative handles are not used, so transient swap-and-restore
races remain possible. FS-UAE's host-directory mount exposed a host symlink as
a regular file: the CLI followed it and archived its target despite `lstat`.
An FFS `MakeLink ... SOFT` probe returned an error, so native soft-link behavior
is not yet qualified.

## Ordered work packages

All packages below are open. A package closes only with the stated evidence.
Environment acquisition can proceed alongside the code work, but unavailable
targets must stay visibly blocked in the backlog.

| ID | Priority and work | Required result and evidence |
| --- | --- | --- |
| I01 | Complete: requirement ledger recorded | [interoperability-requirement-ledger.md](interoperability-requirement-ledger.md) maps Phases 0–3 and basic Phase 5 to every matrix frontend/build, including the Classic Mac copied-source kit and Windows DLL consumer. It links acceptance fixtures, environments, results and remaining gaps. Reconcile it again at I07 after subsequent fixes and test runs. |
| I02 | First: finish filename and destination handling | Audit then fix the gaps listed below. Produce exact byte/name results for representable inputs and pre-output refusal for collisions or unmappable names. Include archive path, selected destination root, and source enumeration APIs. |
| I03 | First: finish extraction, metadata, and archive edits | Audit then fix each adapter against the lifecycle and metadata contract below. Verify successful restoration, accurate partial/error results, original-file preservation, and recovery behavior. |
| I04 | In progress: make cross-system qualification reproducible | Extend existing fixtures into a shared corpus with independent expected results and target runners. The Atari FAT12 ↔ Amiga FS-UAE transfer now uses `test/reference/interoperability/atari-amiga-fsuae.json` and the repeatable `atari-amiga-interoperability-test` target; both directions and the combined three-guest workflow have passed again on 2026-10-29 after rebuilding both guest binaries (evidence: `/tmp/boltpack-atari-amiga-twyz0hzg`). This machine requires the documented `XDFTOOL=build/venv-amiga-tools/bin/xdftool` override. Record real execution separately from builds and host shims; skipped checks stay open. |
| I05 | In progress: 16-bit resource accounting | [16-bit resource accounting](16-bit-resource-accounting.md) records fresh Watcom binary/map measurements, codec-buffer requests, the DOS16 16-bit `size_t` entry-table fix, DOSBox entry-table/index/queue boundary regressions, 26 core/shared-mini allocation failures including the projected-index scratch block, and three injected DOS16 CLI-level directory-buffer failures with `BP_ERR_NOMEM` and zero tracked leaks. DOS16 now reserves an 8 KiB stack and sizes recursive path buffers to their required lengths. The DOSBox fixture verifies a 33-level DOS16-to-Linux tree transfer; the instrumented run observes 3,770 bytes at its lowest stack probe and a 361,184-byte low-water largest DOS-manager block. These are workload measurements, not worst-case bounds or guaranteed C-heap capacity. Allocator overhead, worst-case stack use, and Win16/OS2-16 runtime qualification remain open. |
| I06 | In parallel with I02–I05: develop and run emulator/target fixtures | Use the installed emulators as described below, fix failures, and retain results tied to the tested commit and filesystem. A missing compiler, SDK, OS image, ROM, or runtime remains an explicit dependency. |
| I07 | Last: run the completion audit and release gate | Reconcile the ledger, target results, matrix, specification, bindings, copied sources, build instructions, and release notes. Every required row must have sufficient evidence; no completion claim while required behavior or target evidence remains open. |

### I02: filename and destination code

Progress: Linux full-core source enumeration now walks directories through
held descriptors, obtains child identities relative to those descriptors, and
checks opened directories and regular files against the recorded device/inode
before serial or parallel packing. It also compares high-resolution mtime and
ctime before and after reading regular files; a same-inode write during packing
causes the staged operation to fail. `test/check_source_race.py` replaces a
file pathname and a parent-directory pathname with same-size substitutes,
mutates a file in place during reading, mutates it again immediately after the
source stream closes, and replaces a symlink after its no-follow stat; serial
and parallel paths reject these changes and preserve the original archive.
After all jobs finish, a final pathname snapshot pass checks every file,
directory, and symlink before archive finalization. Linux holds source symlinks
with `O_PATH|O_NOFOLLOW` while reading their targets; the transient
replace-and-restore fixture is rejected through the changed link metadata.
Non-Linux fallbacks use no-follow stat checks around `readlinkat`. Source
snapshots now include file size and mode as well as identity and timestamps. A
deterministic fixture coarsens all reported mtime/ctime values to one-second
precision, then grows or chmods a source after enumeration; serial and parallel
append refuse both changes without modifying the existing archive. Same-size
content mutations that fall within coarse timestamp resolution, and changes
occurring after the final snapshot check, remain open.

The X68000 Shift-JIS conversion and path-scanning portion is now covered at
unit and host-shim level. `scripts/generate_sjis_table.py` produces the strict
JIS X 0208 map; the codepage unit test verifies all 6,879 pairs in both
directions. The host fixture verifies archive creation and extraction for a
name whose trail byte is `0x5c`, transfer to Linux with exact payload bytes,
and refusal of an undefined pair without modifying an existing archive.
Native Human68k path mapping and collision behavior remain open.

The full Windows CLI converts the wide command line to internal
UTF-8 and routes filesystem paths through UTF-16 Win32 and CRT APIs, including
source directory enumeration, archive I/O, staging, publication, and attribute
calls. Serial and parallel packing now sample NT handle change/write times
before and after each source read and fail the staged operation if the source
changes while being packed. The change-time API requires Windows Vista/NT 6.0
or later, matching the frontend's declared minimum. The Wine fixture waits for
the serial archive stage to grow, changes a byte halfway through a 256 MiB
same-size source, and confirms creation fails with no final archive or leftover
stage. A second case sets the worker temp directory to the fixture, waits for
parallel output from a two-file batch, changes a byte halfway through the
64 MiB source, and confirms the batch fails and cleans its temporary outputs.
Both mutation cases pass under Wine; native Windows execution and transient
path-substitution races still need qualification. The file enumerator now
captures each regular file's volume ID, file ID, size, last-write time, and NT
change time, then compares that snapshot to the opened handle in both serial
and parallel packing. A Wine-only inotify fixture replaces a same-size source
after the enumeration handle closes and confirms the queued job is refused
without publishing the archive. Directory jobs now capture the same identity
and timestamps, check the directory before and after walking it, and use
captured attributes for portable metadata. A second inotify fixture replaces
a populated parent directory after its first child is inspected; enumeration
detects the substitution and publishes no archive. These source and parent
replacement cases pass under Wine. Transient swap-and-restore races and native
Windows execution remain open. Before the caller finalizes the staged archive,
the CLI rechecks all captured file snapshots and directory identities, catching
changes that occur after an individual file's pack step has finished.
`make win32-exe` and `make win64-exe` cross-build successfully. Wine 10.0
now runs the Win64 CLI fixture on this machine. It verifies out-of-ACP paths,
corrupt extraction preservation, portable FAT metadata, Linux ↔ Windows archive
transfer, and archive edits. Extraction now preflights all primary paths before
creating the output root; it rejects Windows-reserved names, case aliases, and
file/directory prefix conflicts before publishing entries. It also creates a
Profile 1 archive verified as BoltLite/16 KiB and accepted by Linux full and
mini readers. Reserved-name, trailing-dot/space, case-alias, and both prefix
orders have explicit Linux-created archive fixtures. Append/delete publication
now reapplies captured supported native file-attribute bits and rolls back if
restoration or verification fails; the Wine fixture verifies archive HIDDEN and
SYSTEM survive both edits. Wine cannot expose host symlinks as Win32 reparse
points, and native ACL/stream merge and recovery semantics still need Windows
qualification. The native Win32 GUI has
also migrated its filesystem, dialog, folder-selection, drag/drop, and listing
paths to UTF-16 APIs; `make win32-gui` and `make win64-gui` cross-build. It now
targets Windows NT, with Windows 9x explicitly unsupported. Neither frontend
has Windows runtime qualification here. The Windows FLTK GUI now keeps paths in
UTF-8 through UTF-16 Win32/CRT APIs for files, chooser roots, staging,
publication, temporary files, and drag/drop; Win32 and Win64 FLTK cross-builds
pass. The native and FLTK GUIs have no Windows runtime qualification here. The
WCX now exports Total Commander's Unicode `W` entry points for open, header
listing, extraction, pack, delete, and progress/volume callbacks. The ANSI
exports remain as a strict ACP compatibility path. Win32 and Win64 WCX builds
pass and the Unicode exports are present in the DLL export table. The .NET
frontend already uses Unicode file APIs but lacks managed/runtime qualification
here. The CLI and WCX have Wine runtime coverage; native Win32/FLTK, .NET, and plugin-host behavior remain unqualified here.

The `win64-wcx-wine-test` runtime fixture loads the built WCX DLL under Wine
and exercises `OpenArchiveW`, `ReadHeaderExW`, and `ProcessFileW` with a
supplementary-Unicode archive path and entry name. It calls `PackFilesW` with
Unicode source and archive paths for create and append, then uses `DeleteFilesW`
on both Linux-created and WCX-created
archives; Linux tests and extracts the WCX output byte-for-byte. The same
fixture calls the ANSI `ReadHeaderEx` API through an ASCII archive alias and
confirms it refuses the non-ACP entry name. Wine's `Z:`
root is treated as a reparse point by the plugin's safe extraction checks, so
the test places its temporary fixture on Wine's normal `C:` drive. The fixture
also extracts a nested directory and child, then verifies the archived parent
timestamp after `CloseArchive`. That case exposed narrow CRT directory-time
calls; directory timestamps now use a UTF-16 path and Win32 directory handle.
The fixture passed again on 2026-10-29; retained inputs and outputs are in
`/home/tony/.wine/dosdevices/c:/boltpack-wcx-wine-u50d4hk_`, and its transcript
is `/tmp/boltpack-wcx-wine-20261029.log`. The runner deletes its temporary
directory by default; pass `--keep` when retaining evidence. The Total
Commander host and native Windows behavior remain open.

1. Native Win32, FLTK, CLI, and WCX Unicode entry points keep paths in UTF-8
   internally and use UTF-16 filesystem APIs. Wine qualifies the CLI paths and
   lifecycle listed above, but does not qualify native Win32 runtime behavior.
   Wine now verifies out-of-ACP archive open, header listing, extraction,
   `PackFilesW` creation and append, `DeleteFilesW` on Linux-created and
   WCX-created archives, exact payload bytes, and strict refusal from the ANSI
   header API. Still exercise the plugin in a Total Commander host; verify the
   GUI frontends, record the host version, and qualify ReplaceFile and
   reparse-point behavior on Windows.
2. Qualify CP437/CP850 names through actual DOS, Win16, and OS/2 filename APIs.
   DOS16 CP437 passes on DOSBox-X FAT12; CP850 uppercase passes on DOSBox-X,
   and lowercase passes on FreeDOS 1.4 FAT12 after loading its codepage tables.
   Remaining work is Win16/OS/2-16 runtime qualification and explicit refusal
   checks for unmappable text on those targets. DOSBox 0.74-3 host-mount failure
   remains specific to that emulator path.
3. QDOS archive projection now strips the trailing underscore type marker from
   directory `qdirect` names, preserves embedded underscores in names, strips
   recognized WIN/MDV/FLP/RAM/NFA/SFA drive prefixes only, rejects
   traversal/device separators, and refuses truncated components. Unknown
   prefixes stay intact instead of losing the first name component. The new
   `make ql-path-test` host regression covers these cases and exact-fit/overflow
   path joins. QL child, extraction, and delete-stage path construction now
   refuses overflow rather than silently truncating. QL extraction now performs
   a whole-archive preflight before creating the output root: it requires names
   to convert to Latin-1, enforces the documented 36-byte QDOS pathname bound,
   folds ASCII and one-byte Latin-1 case, and refuses aliases or file/child
   conflicts. `make CC=gcc ql-host-test` checks ASCII and Latin-1 case aliases,
   file/child conflicts, unmappable names, the exact 36-byte boundary, and
   refusal before output creation. The case and length rules follow the QDOS
   directory behavior described in [Directories](https://sinclairql.net/djw/docs/articles/Directories.pdf).
   QDOS root-path parsing, custom device recognition, and native filesystem
   behavior remain unqualified without a QL runtime. RISC OS converts native Latin-1 names to/from UTF-8 and refuses lossy
   dot-separator aliases, reserved path syntax, and components over 10 bytes.
   Before creating the output root, a bounded whole-archive preflight checks
   projected names, case-fold collisions, and file/directory prefix conflicts.
   `make riscos-path-test`, `make riscos-stage-host-test`, and the SWI-stub
   syntax check pass. A new 32-bit host SWI fixture executes RISC OS Profile 1
   create and append, verifies its RISC OS/ARM creator identity, then tests and
   extracts the pinned tree through Linux full and mini readers. This exposed
   missing RISC OS creator-ID defines in the cross-build flags; `make riscos`
   and its documented direct build now set them explicitly; the special-SDK
   wrapper applies the same defines. This is host-shim writer evidence only;
   native SWIs, metadata restoration, and filing-system
equivalence remain unqualified. The fixture also caught and guards the CLI
create summary so supplementary metadata records are not counted as files.
The Atari GEM selector now checks directory/filename joins against GEMDOS’s
125-byte path limit; the host path suite covers exact-fit and overlong cases.
The Haiku CLI preflight now checks each joined output-root/member path against
its path buffer before creating the output root; the host fixture confirms a
near-`PATH_MAX` output path is refused without creating destination components.
The Haiku GUI now preflights the full primary path table with ASCII case folding
and file/directory-prefix checks, then extracts through a root directory
descriptor and same-directory stages. A host helper test verifies collision
and symlink-parent refusal, failed-output preservation, atomic replacement,
and stage cleanup. The GUI now captures and restores the CLI-compatible
`BP_OS_BEOS` payload containing Unix mode and optional `BEOS:TYPE`; file
attributes are restored after checksum verification, and directory attributes
are deferred until children finish. Corrupt or failed native restoration marks
the result partial. Host helpers do not emulate BFS attributes, and no Haiku
cross-compiler/runtime is installed, so native attribute behavior, Unicode
case mapping, and GUI runtime remain unqualified. Plan 9 extraction
   now preflights safe names, duplicate paths, file/directory prefix conflicts,
   and joined-path length before creating the output root. `make
   plan9-host-test` checks the bounded path index, profile interoperability,
   refusal of both file/child collision orders before output-root creation,
   and nested file/directory mode and timestamp restoration. Directory
   metadata is deferred until children are written; unrepresentable or failed
   restoration reports partial. A native amd64 build and CLI run now pass in
   9front 12111 under QEMU TCG. A Linux-to-Plan 9-to-Linux Stored archive
   transfer passes the full and mini readers with exact payload verification;
   Linux correctly reports the Plan 9 permission metadata as unsupported.
   The native guest now also round-trips a Plan 9 archive with a 0550 directory
   and 0640 file: extraction restored both modes and the directory timestamp. A
   directory mode update must preserve Plan 9’s `DMDIR` bit; the frontend and
   host shim now enforce this. Other Plan 9 CPUs and filesystem edge cases still
   need target qualification.
   Standard JIS X 0208 Shift-JIS double-byte mappings are now implemented in
   both conversion directions; CP932 vendor extensions and undefined byte
   pairs remain rejected. X68000 path scans skip valid lead/trail pairs, so a
   trail byte of `0x5c` is not treated as a separator. Unit tests round-trip all 6,879 mapped pairs and cover kanji, hiragana,
   malformed pairs, and that separator edge case. The checked-in table can be
   regenerated with `scripts/generate_sjis_table.py`. `make CC=gcc
   x68k-host-test` archives and extracts a Shift-JIS name whose trail byte is
   `0x5c`, and a Linux reader extracts its UTF-8 spelling and exact payload.
   The QL and Human68k build recipes now set their target creator
   OS/CPU in every core translation unit; the host profile fixtures assert the
   resulting archive IDs. The X68000 host-stub check confirms that an
   undefined double-byte nested directory name is refused before create
   or add modifies an existing archive. Extraction now preflights the complete
   projected table before output-root creation, rejecting case-folded aliases,
   duplicate directories, both file/directory prefix-conflict orders, and
   joined paths beyond the Human68k buffer. The host fixture checks overlong
   members and multi-component roots are refused without creating output.
   Recursive source preflight rejects directory patterns, joined paths, and
   directory archive names beyond its 256-byte buffers. Native Human68k
   path/collision behavior remains open; pathname-based source checks cannot
   prevent concurrent changes during packing.
   X68000 extraction now writes regular files to exclusive sibling stages and
   publishes only after checksum verification and successful close. Forced
   replacement backs up the original and rolls it back if stage publication
   fails; host-stub cases verify checksum failure preservation, no-force
   refusal, successful replacement, injected publication rollback, and stage
   cleanup. X68000 regular-file timestamps now use the shared explicit-offset
   Create, append, and delete also use unique sibling archive stages. Append
   copies the source archive before editing; replacement preserves the
   portable FAT attribute mask and uses backup/rollback publication. The host
   fixture injects edit failures into all three commands and a publication
   failure after backup rename, verifies byte-for-byte preservation and stage
   cleanup, then confirms successful delete output with full, mini, and X68000
   readers. Human68k exclusive-create and rename behavior still needs target
   qualification.
   X68000 regular-file timestamps now use the shared explicit-offset
   DOS/FAT conversion: `BP_TZ_OFFSET` maps local packed dates to UTC on create
   and back on extraction. With no valid offset, creation marks the time
   unknown and extraction reports partial while retaining verified bytes.
   Host-shim coverage checks a leap-day round trip and missing-offset behavior.
   Portable FAT attribute envelopes are now captured from Human68k directory
   results and restored after verified file extraction; directory attributes
   are queued and applied deepest-first after children. The host shim verifies
   file and read-only directory attribute round trips. Directory dates, native
   Human68k date/attribute call behavior, and target runtime semantics remain
   open.
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
native metadata already claimed by each frontend. Mini-core regression coverage
now confirms that skipped or checksum-failed subjects receive no portable
metadata, a partial native-restore result does not suppress other applicable
portable fields, and a hard completion error aborts before supplementary
metadata. Cocoa GUI now restores
valid archived file modes and applies deferred directory modes after children,
using the shared POSIX mode helper; it drops archived set-ID bits and reports
partial. `make macosx-host-test` exercises ordinary modes, absent mode metadata,
and set-ID stripping through that helper. Cocoa SDK build and target runtime
verification remain unavailable. Test epoch zero, absent time,
negative/unrepresentable source time, destination time boundaries and resolution,
timezone assumptions, protected children, and restoration failures. Verify native
versus portable field precedence in both record orders, including conflicting
values; preserve unrelated attribute bits. Atari CLI precedence is verified on
TOS 1.06/1.62/2.05 FAT12, and Amiga CLI native-protection precedence is verified
on OFS and FFS with native and portable records in both orders. The Amiga
frontend preserves the native protection value and reports the unsupported
portable FAT record as partial. Remaining precedence cases on other frontends
stay open. Audit Cocoa's mode restoration, Amiga GUI directory metadata capture,
and legacy frontend coverage against the original basic-profile requirements
rather than assuming source presence means completion. Haiku GUI file and
deferred-directory mode/time extraction is covered by shared host helpers;
the GUI now implements CLI-compatible BFS MIME metadata capture and deferred
restore, but that native API path and Unicode collision behavior still need
target qualification.

The Linux FLTK Xvfb fixture now pins both a file and its deferred parent
directory to valid Unix epoch-zero timestamps. The production GUI extracts
them and the runner checks the resulting file and directory timestamps after
children finish; this closes the epoch-zero case for that frontend only. The
fixture also mutates a valid archive into a checksum-correct `../x` name, then
confirms the GUI rejects it while opening, does not enable extraction, and
writes neither into the selected destination nor outside it.

The Linux GUI's serial and parallel Zstd archive writers now use the format-6
time-validity flag and keep negative or unrepresentable source times unknown.
The Profile 1 creation flow sets its source to one second before the Unix epoch
and checks the resulting table has a zero time with `MTIME_VALID` clear. The
Xvfb fixture also launches the production GUI with Zstd multithreading enabled,
selects two pre-epoch files, then verifies both unknown-time flags and exact
Linux CLI extraction bytes. This exercises the GUI's actual parallel pack
batch. The Windows CLI regression similarly covers serial and parallel Fluff
creation.

The RISC OS CLI now uses the status-aware extraction completion API. It does
not apply native load/exec/attribute metadata unless the metadata payload
checksum succeeded, and malformed payloads or failed `OS_File` restoration
report partial. Data/resource streams now use sibling stages and publish only
after checksum verification and successful close. Forced replacement keeps a
recovery rename and attempts rollback on publication failure. `make
riscos-path-test` passes, and `make riscos-stage-host-test` uses a 32-bit host
SWI shim to verify checksum/close failure preservation, publication, no-force
refusal, rollback after rename failure, refusal to apply corrupt native
metadata, and preflight rejection of case aliases, path-prefix conflicts,
unmappable names, and resource-fork entries. It also runs a full-core extraction
with unsupported foreign native and portable FAT metadata and verifies a partial
result, exact subject bytes, and no remaining stage. Whole-archive name mapping
and collision checks happen before output-root creation. Directory creation still
occurs during streaming. For non-dry-run extraction, the CLI verifies all
payload checksums before creating the output root, then verifies each stream
again as it is staged. RISC OS resource-fork extraction is refused before writes until a
collision-safe sidecar convention is specified. Target SWI rename, cleanup,
metadata, and close behavior remain unqualified.

Exercise create, append, copy, and delete where offered. Preserve complete
primary/resource-fork/native/portable groups and flags. Check stage creation,
write, flush, close, publication, target replacement, read-only targets, aliases,
and interruption at each recovery rename. The OS/2 final verified-file copy has
been eliminated: the closed verified sibling stage is published with
`DosMove`, while forced replacement retains the original in a recovery backup
until publication succeeds. Qualify no-overwrite behavior, rename failures,
and interruptions before and after both OS/2 renames on an OS/2 runtime; archive
edit recovery paths and archive-file attribute retention still need separate
target evidence.
`make os2-publish-host-test` exercises the extracted transaction policy for
first publication, backup-name collisions, no-force refusal, successful
rollback, retained recovery backups, and backup-cleanup failure. It does not
emulate `DosMove`, OS/2 locking, or filesystem crash durability.

## I04: common qualification corpus

Reuse the current independently laid-out format-6 fixtures. Destination-specific
expected-result manifests now cover these runs:

- Atari GEMDOS host directory: [`atari-host-directory.json`](../test/reference/interoperability/atari-host-directory.json)
- Linux mini under QEMU-i386: [`linux-mini-qemu.json`](../test/reference/interoperability/linux-mini-qemu.json)
- Amiga FS-UAE host directory: [`amiga-host-directory.json`](../test/reference/interoperability/amiga-host-directory.json)
- DOS16 on FAT12: [`dos16-fat12.json`](../test/reference/interoperability/dos16-fat12.json)
- Full DJGPP DOS on DOSBox-X FAT12: per-run `manifest.json` from
  `test/check_djgpp_dosboxx.py`
- Windows CLI under Wine/native Windows: [`windows-cli.json`](../test/reference/interoperability/windows-cli.json)
- Linux FLTK GUI under Xvfb: [`linux-fltk-gui.json`](../test/reference/interoperability/linux-fltk-gui.json)
- Plan 9 host-shim profile, collision, and metadata cases: [`plan9-host.json`](../test/reference/interoperability/plan9-host.json)
- X68000 host-shim profile, timezone, and portable FAT attribute cases: [`x68k-host-shim.json`](../test/reference/interoperability/x68k-host-shim.json)

Their runners check fixture inputs and restored payloads against pinned sizes
and SHA-256 values independently of the archive writer. They also record the
expected refusal and partial outcomes; the Windows manifest pins the Unicode
archive spelling, portable metadata flags, and timestamp values. The DOS16
manifest pins the CP437 filename bytes, FAT-visible spelling and timestamp; its
runner now also extracts the DOS-replaced archive on Linux and checks its
independent payload hash. The Amiga manifest pins profile, appended, nested,
and all-codec payload hashes; the FS-UAE runner checks guest and Linux outputs
against them. The Windows runner bounds Wine path conversion and CLI calls so
stuck processes fail with a diagnostic rather than hanging indefinitely. Other
destinations still need manifests. Each manifest must specify file bytes, local
filename bytes or Unicode spelling, timestamps at target resolution, applicable
attributes, and expected refusal/partial status.

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
   and archive edits on a disposable FAT12 floppy; it uses Hatari's control
   console to quit/eject and verifies the saved image after shutdown. Hatari
   2.5.0 with TOS 1.62 now auto-launches `build/atari/boltpack.prg` to its GEM
   main window. The first screenshot showed the process name overprinting menu
   titles. The menu builder now uses the application name in its Desk title,
   reserves its full width, aligns the pull-down boxes, and uses a visible `G_BOX`
   menu bar; the rebuilt UI displays all four titles without overlap. A Hatari
   keyboard-injection run verified Ctrl+N reaches the create handler and opens
   the input file selector. A fresh-build hconsole replay later failed to
   reproduce Ctrl+N with lowercase or uppercase key input, so treat the earlier
   success as non-reproducible with the current headless input setup. A mouse
   press/drag selected Archive → Open and opened the archive selector in the
   rebuilt application. This found a legacy
   `evnt_multi()` binding mismatch that shifted keyboard outputs; the event loop
   now uses `evnt_multi_fast()`. Startup, menu rendering, and these two
   selector-opening paths are qualified. Selectors now default to the current
   GEMDOS drive root; Hatari confirmed `C:\\*.*` and one selected source file
   returns to the create flow. A temporary test build changed only the alert's
   default button to Create, allowing keyboard injection to exercise create and
   save; it produced an Atari-origin v6 archive. Linux `test` passed and
   extraction reproduced the source bytes exactly, while reporting Atari
   metadata as unsupported on Linux. Reopening that archive in the normal GUI
   shows only its data entry after the list callback filters META/RSRC records.
   The normal GUI extracted that archive to an empty directory with exact file
   bytes and restored the timestamp to whole-second precision on Hatari's
   host-directory drive. That drive changed POSIX mode 0664 to 0644, so this
   does not qualify native FAT metadata. Mouse activation of Create in the
   shipped dialog, cancellation, and GUI metadata restoration on native FAT
   remain unqualified.
   A direct TOS 1.62 probe on the FAT12 guest disk returned GEMDOS `-33`
   (`EFILNF`) for `Fattrib` read/set on a directory and for `Fopen` on that
   directory; this also prevents handle-based `Fdatime`. Directory FAT flags
   and timestamps therefore remain unsupported by the tested GEMDOS calls and
   correctly report partial. Do not infer that this OS-level limitation applies
   to MiNT or later TOS implementations.
   File FAT flags/timestamps round-trip on the tested TOS versions. TOS 1.62
   directory times and nonzero flags correctly report partial because GEMDOS
   rejects the directory API calls above. Keep host-directory and FAT-image
   results distinct; interruption recovery still needs runtime validation.
   On 2026-10-29, the FAT12 guest transfer/edit runner was repeated on TOS
   1.06, 1.62, and 2.05 and passed all three; retained evidence is in
   `/tmp/boltpack-atari-fat-eduejk3t`, `/tmp/boltpack-atari-fat-m134gjg8`,
   and `/tmp/boltpack-atari-fat-y_mzqmnc`. The host-directory interoperability
   runner also passed again on TOS 1.62 with evidence in
   `/tmp/boltpack-hatari-qfqjcm8i`.
   On 2026-10-29, the expanded FAT12 runner passed on TOS 1.06, 1.62, and
   2.05; evidence is at `/tmp/boltpack-atari-fat-j2ximutg`,
   `/tmp/boltpack-atari-fat-kxyhzdbi`, and
   `/tmp/boltpack-atari-fat-qw1zzfp2`. Append/delete preserve archive HIDDEN
   and SYSTEM flags; create-overwrite, append, and delete against a READONLY
   archive report that it is read-only, preserve its bytes and READONLY flag,
   and successful edited output passes Linux integrity and exact-payload
   extraction.
2. **Amiga, FS-UAE:** use Kickstart 2.04 and Workbench 2.04 under `~/images/Amiga`.
   `test/check_fsuae_interop.py` covers the host-directory backend and
   `test/check_fsuae_native.py` covers native OFS/FFS floppy images. Both native
   filesystem runs now verify nested directory and empty-directory protection,
   comments, and timestamps as well as file metadata, transfer, edits, and name
   refusal. The CLI's 8 KiB `$STACK` cookie plus libnix startup expansion let
   the host-directory runner pass 64-level create/extract at `--stack 0`, as
   well as the full profile, edit, and four-codec corpus; evidence is
   `/tmp/boltpack-fsuae-_0zil2jv`. The deep fixture uses short repeated names
   to stay under the legacy AmigaDOS path limit. Native OFS and FFS runs also pass the transfer/edit/
   metadata/name-refusal corpus with 512 KiB chip RAM, no fast RAM, and the
   default shell stack; the native runner records these memory settings. Both
   native filesystems also pass guest create/test and Linux test/extract for
   Stored, BoltLite, BoltLZ, and Fluff with a 256 KiB fixture and 256 KiB blocks.
   The retained OFS and FFS codec runs are `/tmp/boltpack-amiga-ofs-causbh48` and
   `/tmp/boltpack-amiga-ffs-hu_1pcai`; 49-level default-stack runs are recorded separately
   in the emulator setup notes.
   On 2026-10-29, the default-memory native transfer/edit/metadata suite also
   passed with `--skip-codecs --depth 4 --stack 0`; retained OFS and FFS evidence
   is `/tmp/boltpack-amiga-ofs-90l0l1jb` and
   `/tmp/boltpack-amiga-ffs-2mp6wwkf`.
   The host-directory backup/publication rename window now has a tested
   recovery command on the host-directory, OFS, and FFS backends. Remaining
   Amiga runtime work is hardware power-loss durability,
   GUI flows (the available
   Workbench 2.04 set lacks `gadtools.library` version 37, so the GUI exits
   before opening). The GUI source gatherer now uses a heap-backed iterative
   traversal instead of recursion; `make amiga-gui-gather-host-test` passes a
   128-level host tree plus symlink, missing-directory, capacity, path-limit,
   gather-callback-failure, and injected frame/path allocation-failure cases
   with zero owned-allocation leaks. That fixture also covers the stat
   identity/size/time comparisons now used by production GUI creation before
   and after regular-file packing and after native metadata capture. The GUI
   cross-builds with `make amiga-gui`; runtime and round-trip behavior and
   native link behavior remain unqualified.
   Keep mounted-directory and native-disk results distinct; the host mount does
   not model native symlink behavior.
3. **DOS, DOSBox-X:** `test/check_dosboxx_cp437.py` passes CP437 filename and
   byte extraction, timestamp restoration, and DOS → Linux archive creation on
   a FAT12 image. Preserve the separate DOSBox 0.74-3 failure and DOSBox-X
   host-directory result. DOS16 mini does not provide archive edit operations.
   `make CC=gcc CWSDPMI=/path/to/CWSDPMI.EXE dos-djgpp-dosboxx-fat12-test`
   passes full-DOS archive creation, append, delete, verification, and
   extraction both ways on DOSBox-X FAT12, with HIDDEN/SYSTEM/ARCHIVE flags
   preserved across append and delete; the runner retains a `manifest.json`
   with tool versions and artifact hashes. Obtain `CWSDPMI.EXE` from DJGPP's
   `csdpmi` package when the helper is not already installed. Native DOS crash
   recovery and other filesystems still need separate checks.
4. **Windows, Wine then Windows:** Win64 Wine feedback is now available through
   `test/check_windows_cli.py --wine`. On this machine it passes supplementary
   Unicode paths, corrupt-extraction preservation and stage cleanup, file and
   directory hidden/system flags and timestamps, pre-1970 time encoded as
   unknown in both serial and parallel Fluff creation, parallel Fluff metadata,
   hard-link refusal, append, test, and delete. It verifies archive HIDDEN and
   SYSTEM survive append/delete after supported attributes are explicitly
   reapplied. Read-only archive targets are refused before stage creation; append/delete leave
   bytes and attributes unchanged. The publisher
   rolls back if attribute application or verification fails. It skips reparse checks because Wine does not model host symlinks as
   Win32 reparse points. Linux and Win64 Wine
   also exchange archives both directions; unsupported foreign metadata is
   returned as partial, with bytes still verified. The
   Win32/Win64 CLI builds now statically link the trailing OpenMP runtime and
   no longer import `libgomp-1.dll`. Run the fixture natively on Windows next;
   Wine does not qualify NTFS/FAT, native ACL/stream merging or recovery, reparse
   handling, GUI, .NET, or Total Commander behavior.
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
| Linux CLI/mini/FLTK | Native Linux, QEMU-i386, usable GUI display | `make CC=gcc linux-gui-interoperability-test` sets Interchange Profile 1 in isolated GUI preferences, creates BoltLite/16 KiB output and verifies/extracts it with the full CLI and Linux mini under QEMU-i386 against pinned bytes, opens/extracts a nested format-6 fixture, verifies file and deferred directory mode and epoch-zero timestamps, rejects a valid file/child prefix-conflict archive without writing output, selects duplicate leaf names for flattened extraction while proving an existing destination survives, and checks cancellation, checksum-failure, and injected write, flush, close, and `futimens` restoration-failure preservation plus stage cleanup. A one-time delayed output write keeps the cancellation progress dialog observable on fast hosts. |
| Windows CLI/native GUI/FLTK/.NET/WCX | Windows with NTFS and FAT-compatible test volume, .NET build tools, Total Commander plugin host | `test/check_windows_cli.py --wine /usr/lib/wine/wine64` passes Win64 Unicode paths, hard-link refusal, append, test, and delete under Wine 10.0. Native Windows must still run the fixture for reparse refusal, ACL/stream merge, and recovery behavior, then qualify the GUI, .NET, and plugin host. Cross-build both word sizes. |
| macOS CLI/Cocoa/legacy GUI | macOS SDK/runtime; case-sensitive and case-insensitive test volumes; supported legacy environment where shipped | Verify Unicode normalization, Apple xattrs/Finder info/resource forks, time/mode restoration, publication, and actual GUI flows. Host declaration stubs remain supporting evidence only. |
| DOS16/full DOS/Win16/OS2-16/full OS2 | DOSBox-X FAT12 plus CWSDPMI for DJGPP, Windows 3.x, OS/2 with FAT and HPFS | DOS16 verifies CP437 extraction, CP850 uppercase creation/extraction on DOSBox-X, lowercase CP850 round trips on FreeDOS 1.4, DOS16 → Linux creation/replacement, and FAT flags. `dos-djgpp-dosboxx-fat12-test` verifies full-DOS create/append/delete/test/extract, Linux/DOS transfers, and HIDDEN/SYSTEM/ARCHIVE preservation across edits. Native DOS crash recovery, other filesystems, directory metadata, Win16, and OS/2 still need target-specific qualification. |
| Atari CLI/GEM GUI | Hatari and TOS 1.62; disposable FAT12 media | `test/check_hatari_fat.py` verifies guest FAT12 transfers, archive edits, clean control-console shutdown/eject, persisted bytes after exit, and refusal to apply a directory-typed portable FAT envelope to a file. The expanded FAT12 fixtures on TOS 1.06, 1.62, and 2.05 verify append/delete preserve archive HIDDEN/SYSTEM flags, read-only create-overwrite/append/delete report the read-only cause and preserve bytes and READONLY, and Linux validates/extracts the edited archive. `atari-amiga-interoperability-test` transfers Profile 1 archives in both directions between Atari FAT12 and the Amiga guest; independent manifest payloads/names and the empty directory match, while foreign native metadata reports partial. GUI startup/menu rendering, Ctrl+N, mouse Archive → Open selector opening, and one create-source selection are qualified. A temporary default-button harness exercised create/save and Linux verified the resulting v6 archive; the shipped Create-button input, other menu actions, native GUI metadata, and interruption recovery remain unqualified. Additional Xvfb/xdotool clicks moved the pointer over source/OK/Create but did not advance the selector. A fresh hconsole key-injection probe selected `SRC.TXT` and reached the native More/Create alert, but did not make Create advance to the save dialog. Normal GUI extraction passes on the host-directory drive. Native FAT attribute/time qualification is partial. |
| Amiga CLI/GadTools and OS4/MorphOS/AROS variants | FS-UAE and Kickstart/Workbench 2.04 are available for 68k, but the supplied Workbench disks lack the GUI's `gadtools.library` version 37; matching SDKs and runtimes are needed for other variants | CLI qualification is in the Amiga results above. The repeatable two-way corpus confirms Atari-created and Amiga-created archives validate and extract on the other guest with exact independent file/directory results; unsupported foreign metadata reports partial. Obtain a compatible 68k GadTools runtime before GUI startup/flow validation; build and qualify each OS4/MorphOS/AROS target with its own SDK and runtime. Shared source and an emulator do not qualify all variants. |
| Classic Mac 68k/PPC | Mac OS 9 runtime; legacy PowerPC build environment | `make macos9-core-sync-test` checks all 17 copied 68k core files against `src/core`, including the exact file inventory. `make macos9-68k-kit` builds the application and resources using the copied source kit. Test MacRoman/HFS collisions, resource forks, Finder fields, directory dates, bounded preflight, partial failures, and profile transfer in target runtimes. PowerPC build and both target runtimes remain unqualified. |
| Plan 9 amd64 | 9front 12111 QEMU TCG guest | Linux-to-Plan 9-to-Linux Stored transfer and native directory/file mode and timestamp restoration pass. A fresh guest-built v6 archive now reports Plan 9/x86-64 in both the guest and Linux readers. `make CC=gcc plan9-host-test` passes; remaining work is other CPUs and filesystem/API edge cases. |
| RISC OS CLI | GCCSDK cross compiler or native Norcroft system and a RISC OS runtime/filesystem | `make riscos-profile-host-test` now checks Profile 1 create/append through a host SWI shim and Linux full/mini readers. Still cross-build with GCCSDK and exercise native SWIs, metadata restoration, filing-system name rules, and the full transfer corpus on target. |
| QL/X68000/CPM-86 | Matching compiler and native filesystem runtime/emulator for each | Replace syntax/host-shim-only evidence with target builds and the applicable corpus. Exercise native path mappings, limits, metadata, and refusal behavior. |
| Haiku CLI/BeAPI GUI | Haiku compiler/runtime and BFS test volume | `make haiku-host-stage-test` checks staged CLI extraction and archive create/append/delete, including failure preservation, Linux cross-reading, and bounded name preflight with a host `fs_attr` shim. `make haiku-gui-extract-host-test` checks GUI path preflight, ASCII case collisions, symlink-parent refusal, core-reported checksum failure preserving an existing file, successful verified publication, and stage cleanup. CLI and GUI source both capture/restore the CLI-compatible mode and `BEOS:TYPE` metadata payload. The CLI host `fs_attr` shim now maps to Linux extended attributes and verifies file/directory MIME round trips plus the 255-byte payload boundary; this does not establish BFS semantics. Still qualify BFS Unicode case mapping, native MIME metadata, timestamps, and GUI runtime behavior on Haiku. |
| Other POSIX wrappers | Native environments for BSDs, Hurd, Solaris/illumos, AIX, z/OS, QNX, and remaining shipped wrappers | Validate actual APIs and feature gates, filename encoding, modes/times, safe traversal, unsupported metadata reporting, and edits. Record OS/compiler versions, especially AIX and z/OS. |

Each run must record commit, frontend/build flags, compiler, OS/emulator version,
filesystem, locale/codepage, timezone, commands, expected/observed result, and
retained logs. A skip is not a pass. Emulator evidence must name the emulator
and filesystem path used; it is not a hardware-performance measurement.

## Execution and completion rules

I01 is recorded; reconcile it again only at the I07 release audit. Atari,
Amiga, and DOS emulator setup and the available CLI transfer fixtures have been
run, including Atari FAT12 on TOS 1.06, 1.62, and 2.05, Amiga host-directory,
OFS and FFS, and DOSBox-X FAT12. Do not repeat setup-only probes. Continue with
an unresolved I02/I03 implementation item from the ledger that can be reproduced
on this machine, and extend the matching fixture before changing code. Obtain
the Windows/macOS and other legacy environments concurrently with that work.
Keep one concrete defect or missing capability per implementation change, with
its meaningful regression evidence.
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
