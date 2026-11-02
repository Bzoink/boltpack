# Target Compatibility Profiles

**Status:** implementation in progress. The Linux x86-64 CLI implements
writer-side Linux POSIX, generic POSIX, Plan 9, Linux mini, CP/M-86, Atari ST,
DOS-family 8.3 and LFN, conservative AmigaOS 68k, Classic Mac HFS, Sinclair
QL, RISC OS, X68000, and Haiku BFS profiles. The native Windows CLI also
implements its Windows NT LFN, Linux POSIX, generic POSIX, Plan 9, Haiku BFS,
Classic HFS, AmigaOS 68k, CP/M-86, Atari ST, Sinclair QL, RISC OS, X68000, and
DOS-family profiles. The macOS CLI implements modern macOS, Linux POSIX,
generic POSIX, Plan 9, Haiku BFS, Classic HFS, AmigaOS 68k, CP/M-86, DOS-family,
Sinclair QL, RISC OS, X68000, and Atari ST writer preflights.

**Creator parity:** The shared POSIX CLI now exposes all Linux target profiles
on Linux i386, ARM32, ARM64, and the POSIX wrappers, with target help, automatic
renaming, and reports. Linux i386 and ARM user-mode QEMU checks cover the
profile matrix; POSIX wrappers were checked with host shims, not native
systems. Native z/OS remains disabled because its filename encoding is not
qualified. Win32/Win64 and macOS use the shared auto-rename and report logic
for all profile families, including Linux full, Linux mini, generic POSIX,
Plan 9, and Haiku BFS. Win64 was checked under Wine, macOS with a POSIX host
shim, and Win32 was cross-built; these do not qualify native Win32 or macOS
behavior. The Linux FLTK GUI exposes the same profile picker, automatic
aliases, and name report on create. Standalone 32-bit legacy CLIs and Linux
mini have not yet gained creator-side target profiles or automatic renaming.
The Plan 9 client now extracts into a new private `-o` root; native runtime
qualification remains open. Linux-created AmigaOS 68k profile archives now pass extraction
and exact Latin-1 name/payload readback on FS-UAE OFS and FFS. Remaining
runtime qualification and writer-client coverage gaps are identified per
catalog row. The complete host profile gate passed on 2026-11-01 with the
locally bootstrapped Mono wrappers:

```sh
make CC=clang MCS="$PWD/build/mono-bootstrap/mcs-local" \
  MONO="$PWD/build/mono-bootstrap/mono-local" target-profile-host-test
```

That run passed the Linux profile integrations, Windows CLI profile matrix
under Wine, Win32 and .NET GUI preflight suites, and the Amiga, RISC OS,
X68000, Haiku, and CP/M-86 host suites. The aggregate target now also builds
the Win32 CLI; a separate `make win32-exe` succeeded on this checkout. The
Win32 runtime profile corpus remains unrun because Wine lacks its 32-bit
runtime. It does not close the native-runtime
qualifications listed below. Profiles are writer-side checks and are not
recorded in the archive format.

## Purpose

A POSIX, Windows, or macOS creator can run `boltpack --help-targets` (or
`boltpack help targets`) to list available profiles, their filename rules,
compression restrictions, and usage examples. The Linux FLTK GUI offers the
same profiles in its Create Archive dialog together with an automatic-alias
option.

A target compatibility profile lets a capable archive creator prepare an archive
for a named reader and destination filesystem. For example, a Linux x86-64
creator could select an Atari ST profile and have BoltPack check names, paths,
compression methods, sizes, and required metadata before publishing the archive.

The archive continues to store canonical UTF-8 paths. A profile describes the
target constraints that those paths and archive records must satisfy. It does
not transcode file contents. Text and binary payloads remain byte-for-byte data.

The existing Interchange Profile 1 is a general writer-side limit set; it is
not recorded in the archive. Its codec and size bounds are a useful base, but
they do not establish that a target filesystem can represent every path.
Profiles should compose these format limits with the target reader and
filesystem limits.

## Linux FLTK GUI

The Create Archive dialog exposes a Target Profile menu with `None` plus the
shared profile set and an automatic-alias checkbox. Selecting a legacy profile
forces BoltLite with 16 KiB blocks (Interchange Profile 1); CP/M-86 forces
BoltLite with 32 KiB blocks; Linux mini and Plan 9 retain the selected codec
within their own ceilings. Create collects the complete source tree before
publishing, runs the same preflight as the CLI, writes Plan 9 mode records
where required, and adds `BPNAMES.TXT` when automatic aliases are selected.
The selection is persisted in the GUI preferences. `make
linux-gui-target-profile-test` drives the dialog under Xvfb and verifies an
Atari ST alias/report archive and a Plan 9 archive through the Linux CLI.

## Implemented Linux POSIX and mini profiles

The shared POSIX, Windows, and macOS CLIs accept `--target-profile=linux-posix-v1` and
`--target-profile=linux-mini-v1` on `create` and `add`. Both retain canonical
UTF-8 paths and use case-sensitive Linux path collision and file/directory
prefix checks with a 255-byte component ceiling and a bounded 4,095-byte path.
They refuse resource-fork and metadata entries because the Linux extractor
skips those record types. The full POSIX profile accepts Linux symlinks and
the full Linux codec set selected at build time.

The mini profile additionally requires Stored, BoltLite, or BoltLZ; block sizes
no larger than 32 KiB; at most 65,535 physical entries; files no larger than
4,294,967,295 bytes; and a serialized archive no larger than 2,147,483,647
bytes. It refuses symlinks and all non-data/non-directory entry types, checks
existing entries on append, and verifies the final staged archive size before
publishing. The writer-side integration test cross-checks created archives
with the native 32-bit Linux mini reader, including payload extraction for all
three supported codecs.

## Implemented generic POSIX UTF-8 profile

The shared POSIX, Windows, and macOS CLIs accept `--target-profile=posix-utf8-v1` on `create`
and `add`. This profile defines a POSIX filesystem contract: strict UTF-8 byte
names, case-sensitive lookup, no Unicode normalization or projection, 255-byte
components, and 4,095-byte relative paths. It rejects unsafe paths, duplicate
names, and file/directory prefix conflicts. It supports regular files,
directories, and safe relative symlinks; it refuses resource forks and
metadata entries skipped by the Linux POSIX extractor. It retains the full
codec set selected by the build and composes with optional Interchange Profile
1 limits.

Append validates existing paths and decompresses existing symlink entries to
check their targets before publishing the edited archive. The profile targets
only filesystems following this byte-sensitive contract and POSIX clients with
directory-relative no-follow extraction enabled. It does not assert that every
POSIX filesystem has these name semantics. Host integration creates, appends,
tests, and extracts a Fluff archive containing a safe symlink and verifies
the payload and link bytes. Native BSD, Hurd, Solaris/illumos, AIX, and z/OS
filesystems still need runtime qualification; wrappers using path-based
fallback extraction fail closed and are not target readers for this profile.

## Implemented Plan 9 UTF-8 profile

The shared POSIX, Windows, and macOS CLIs accept `--target-profile=plan9-utf8-v1` on `create`
and `add`. It retains canonical UTF-8 bytes and case-sensitive path identity;
rejects C0/C1 controls, DEL, symlinks, resource forks, and foreign metadata;
and checks duplicate and file/directory-prefix conflicts. It caps relative
paths at 511 bytes and components at 255 bytes. To stay within the current Plan
9 frontend's path-index resources, it conservatively bounds aggregate indexed
path nodes and name bytes. It writes a Plan 9 `BP_TYPE_META` record carrying the
POSIX rwx permission bits for every file and directory, and refuses source
setuid, setgid, or sticky bits because Plan 9's extractor does not restore
those POSIX extensions. The Windows writer has no source POSIX mode and
synthesizes conservative 0644 file and 0755 directory records. It refuses append when an existing primary entry lacks
matching two-byte Plan 9 mode metadata. Known timestamps must fit Plan 9's
32-bit seconds field; unknown times remain unknown. It permits Stored, BoltLite, BoltLZ, and Fluff with blocks up to 256
KiB,.
Existing archive entries are checked before append.

The Plan 9 reader extracts only when given `-o` naming a directory that does
not already exist. It creates that root with owner-only permissions, changes
into it before writing, resolves archive paths relative to that working
directory, and uses exclusive file creation. This avoids merging with an
existing tree and prevents the archive from selecting paths outside the new
root. Extraction without `-o` and extraction into an existing output directory
are refused; `-f` does not override this rule. Metadata restores Plan 9 rwx
permissions and representable timestamps. `make plan9-host-test` checks
create/append, accepted methods, controls, long paths,
symlink refusal, exact extraction bytes, nested paths, case-sensitive names,
metadata restoration, dry-run behavior, and existing-output preservation
through host shims. This does not qualify native Plan 9 runtime or successful
transfer on a native 9P filesystem.
The [Plan 9 9P introduction](https://9p.io/sources/plan9/sys/man/5/0intro)
describes UTF-8 names and excludes slash and control characters. The 511-byte
path ceiling is BoltPack's current frontend limit, not a universal 9P limit.

## Implemented CP/M-86 profile

The Linux x86-64, macOS, and Windows CLIs accept
`--target-profile=cpm86-83` on `create` and `add`. They project valid flat
ASCII 8.3 source names to uppercase and refuse source
directories, slashes, drive separators, unsupported characters, unrepresentable
names, and projected collisions. The archived names are the projected uppercase
names; payload bytes are unchanged. The profile permits only regular files and
rejects symlinks, directories, resource forks, and metadata.

It defaults to BoltLite and permits Stored, with blocks no larger than 32 KiB.
It enforces the CP/M-86 frontend's 256-entry extraction table, signed 32-bit
file-size limit, and the mini reader's 2,147,483,647-byte serialized archive
limit. Since the CP/M-86 filesystem has no timestamp or POSIX mode restore,
the writer omits those host attributes; append refuses existing records that
carry them. Existing entries are otherwise checked on append. No automatic
alias or name report is provided; invalid names fail before archive
publication.

`test/check_target_cpm86_profile.py` verifies the Linux writer by extracting
archives with the CP/M-86 host frontend, payload integrity, append, collisions,
name restrictions, codec/block limits, file-size limits, and the 256-entry
boundary. `make macosx-host-test` and `make win64-target-profile-test` verify
uppercase projection, metadata omission, collision-safe append, file-only
policy, codec refusal, and Linux reader acceptance from their respective
writers. These host checks do not qualify execution on CP/M-86 hardware.

## Implemented DOS CP850 LFN profile

The Linux x86-64, macOS, and Windows CLIs accept
`--target-profile=dos-cp850-lfn-v1` on `create` and `add`. They convert names
exactly to CP850, enforce the DOS LFN forbidden
character, trailing dot/space, reserved device, case-fold, and path-conflict
rules, with 255-byte component and 259-byte relative-path ceilings. The macOS
host shim and Windows Wine tests also verify CP850 projection, reserved names,
case aliases, create/append refusal, and Linux-reader acceptance. The profile uses
Interchange Profile 1 limits and accepts regular files and directories plus
portable or DOS FAT-attribute metadata; it rejects links, resource forks,
foreign metadata, and files above the DOS client's signed 32-bit I/O ceiling.

The DOS client detects LFN support on the extraction volume. An archive with
long names therefore requires an LFN-capable destination; use
`--target-profile=dos-cp850-83` when the destination must work without LFN.
A Linux writer cannot inspect that remote volume, so profile selection does
not claim volume qualification. Host integration checks long names with
spaces and CP850 characters, append, collisions, invalid names, and path
limits. `make dos-host-test` also passes a profile-created archive through
the DOS frontend's host LFN simulation, verifies CP850 output bytes, and checks
that its simulated no-LFN mode refuses the archive before output creation.
The DJGPP full-client fixture enables DOSBox-X's LFN API and `KEYB UK 850`,
then integrity-tests and extracts `Long Filename Ø With Spaces.txt` on FAT12.
mtools reads back that exact Unicode name and payload. The baseline create/
append/delete and no-replace rename checks run separately under DOSBox-X
defaults; the LFN-enabled session isolates the profile test. Evidence is
retained in
[`dosboxx-cp850-lfn-nonascii-20261101/manifest.json`](../build/dosboxx-cp850-lfn-nonascii-20261101/manifest.json);
the earlier ASCII-only run remains at
[`dosboxx-lfn-profile-20261101/manifest.json`](../build/dosboxx-lfn-profile-20261101/manifest.json).
CP850 mapping on other DOS implementations and filesystems remains unqualified.

## Implemented Windows NT LFN profile

The native Windows CLI accepts `--target-profile=windows-nt-lfn-v1` on `create`
and `add`. It converts canonical UTF-8 archive paths to UTF-16, applies NFC to
the collision keys, validates Windows forbidden characters and device names
(including the COM/LPT superscript 1, 2, and 3 aliases), rejects trailing
dots/spaces, and enforces 255 UTF-16 code units per component and the
frontend's 259-unit relative-path limit. `CompareStringOrdinal` checks
case-insensitive duplicate paths and file/directory prefix conflicts. Names
remain unchanged in the archive; no aliasing or content transcoding occurs.

The profile uses every codec supported by the selected Windows CLI build. It
accepts portable metadata, including the Windows writer's FAT-attribute
records; restoration is best effort because unknown portable attributes may
still be reported partial. It refuses resource forks, symlinks, and foreign
metadata. Host integration is `make win64-target-profile-test`; it exercises
the native Windows code under Wine and checks Linux-created invalid/collision
fixtures. The Win32 GUI applies matching create-side name/path refusal rules
before staging an archive from either the file picker or drag-and-drop: NFC
collision keys, forbidden characters and device names, trailing-dot/space
refusal, 255-unit components, a 259-unit relative-path limit, and
case-insensitive duplicate/prefix checks. It loads `NormalizeString`
dynamically so it does not add a hard dependency on Vista's `normaliz.dll`; on
older Windows versions where normalization is unavailable, it conservatively
refuses non-ASCII create names. `make win32-gui win64-gui` verifies compilation,
and `make win64-gui-profile-test` runs the production GUI preflight functions
under Wine. It checks ordinary names, 255/256-unit component boundaries,
259/260-unit path boundaries, reserved and forbidden names, case and
file/directory-prefix collisions, NFC collision behavior or conservative
fallback refusal, and recursive source-tree enumeration with case-colliding
children. This confirms the preflight rules through the Wine Windows API layer;
the full GUI dialog flow, native Windows behavior, source-tree mutation, and
reparse-point handling remain unqualified.
The .NET GUI now reuses its Windows path validator for create preflight, adding
the 255-unit component and 259-unit relative-path limits and COM/LPT
superscript-device aliases; its existing preflight also applies NFC collision
keys, case-insensitive duplicate checks, and file/directory-prefix checks
before staging. A Mono 6.12 host build compiled all .NET GUI sources, and
`win-gui-net-profile-host-test` executes the production validator and
create-item preflight through reflection. It covers Windows reserved names,
superscript aliases, 255/256-unit components, 259/260-unit paths, forbidden
names, supplementary Unicode, case/NFC collisions, and file/directory-prefix
conflicts. This is Mono host evidence; the SDK-style .NET 4.8 project build and
Windows WinForms runtime remain unqualified. The FLTK GUI now recursively preflights selected
sources before opening the save dialog, using UTF-16 Windows component rules,
NFC case-collision keys, path limits, entry-type checks, and reparse-point
refusal. `make win64-fltk-wine-create-test` exercises its production GUI with
a supplementary-Unicode filename; Linux full and mini readers validate the
archive and Linux extraction reproduces exact payload bytes. Linux reports
partial because Windows FAT metadata is not restored there; the runner accepts
that result only when every skipped item is such metadata. The production GUI
preflight has two Wine collision cases: `make
win64-fltk-wine-source-collision-test` refuses `Case.txt`/`case.txt`, and
`make win64-fltk-wine-nfc-collision-test` refuses composed `café.txt` and
decomposed `café.txt`. Both show the preflight alert before the save dialog
and publish no archive. Run records retain the alert screenshots: [case
collision](../build/r08-win64-fltk-wine-create-dn0h0ceg/run.json) and [NFC
collision](../build/r08-win64-fltk-wine-create-t8eqezop/run.json). The WCX `PackFiles` and
`PackFilesW` exports now preflight the full incoming list and existing archive
before staging. They enforce UTF-16/NFC Windows names, reserved-name and
length rules, duplicate/prefix checks, regular-file/directory types, and
portable-metadata-only append policy. `make win64-wcx-wine-test` exercises
Unicode create/append/delete, refuses case and NFC collisions before staging,
and confirms a rejected colliding append leaves the prior archive byte-for-
byte unchanged; Linux full-reader checks verify the transfer. Native Windows
and volume-specific runtime qualification remain open. Like the GUI clients,
WCX resolves `NormalizeString` dynamically and refuses non-ASCII names on
Windows versions that lack the normalization API.

## Implemented Classic Mac HFS profile

The Linux x86-64, macOS, and native Windows CLIs accept
`--target-profile=macos-classic-hfs-v1` on `create` and `add`. Both require
exact MacRoman representability, check HFS's 31-byte component and
conservative 255-byte relative-path ceilings, reject colon, backslash, control
characters, and dot components, and check MacRoman case-fold collisions and
file/directory prefix conflicts. Names in
the archive remain canonical UTF-8; the profile does not change file content.

The profile implies Interchange Profile 1 limits and supports regular files
and directories only. Both writers reject symbolic links, resource forks, and
metadata records, and cap individual files at signed 32-bit I/O size. The
macOS writer omits Unix mode records that have no Classic HFS meaning; on macOS
it refuses source resource forks and meaningful Finder information instead of
silently dropping them. The Windows writer omits portable FAT records and
refuses source read-only, hidden, or system flags. Existing append entries with
metadata or resource forks fail preflight. `make target-profile-host-test`
covers the Linux writer; `make macosx-host-test` checks the macOS writer; and
`make win64-target-profile-test` exercises the Windows writer under Wine.
`make macos9-68k` and `make macos9-68k-kit` package the 68k frontend as a
MacBinary APPL and create an HFS disk image. The final Rez step must consume the
`.r` resource source: passing the intermediate raw `.rsrc` fork silently
omitted the GUI resources. The build-time guard verifies the `APPL/BoPk`
identity against `kCreatorCode` and requires the GUI, code, and sizing resource
types in the packaged MacBinary ([resource package audit](../build/macos-classic-qemu/resource-package-20261101/run.json)).
QEMU 10.0.13 boots System 7.5.3 and mounts a hybrid HFS CD
containing the corrected package and a Linux-created profile archive. The
corrected app visibly launches. On a writable HFS test disk it extracted a
stored root-level `plain.txt`; the 28-byte guest file was copied back and
matched the Linux source SHA-256 exactly. A later profile attempt exposed and
fixed a staging-name check that rejected every stage below a directory. After
that fix, the guest created both nested directories and materialized three
root-level data files with expected lengths, but left the nested 21-byte file
as `BP0003.TMP` instead of publishing it as `Grüße.dat`; the app reported
partial restoration. The [run record](../build/macos-classic-qemu/resource-package-20261101/runtime-nested-stage-fix-20261101.json)
and [HFS tree](../build/macos-classic-qemu/resource-package-20261101/hfs-tree-after-stage-fix.txt)
retain this limited evidence. Nested file publication and byte equality are
not qualified. Non-ASCII MacRoman names, HFS collision behavior, and metadata
restoration also remain unqualified. See the
[emulator notes](emulator-setup.md) and retained
[runtime artifacts](../build/macos-classic-qemu/resource-package-20261101/).

## Implemented Haiku BFS profile

The shared POSIX, Windows, and macOS CLIs accept `--target-profile=haiku-bfs-v1` on `create` and
`add`. It preserves canonical UTF-8 path bytes, checks BFS's 255-byte component
and 1,023-byte path bounds, and rejects unsafe paths, duplicates, and
file/directory prefix conflicts. BFS directory lookup is byte-sensitive and
case-sensitive; the profile does not fold case or normalize Unicode. It accepts
regular files, directories, symlinks, and existing `BP_OS_BEOS` metadata, while
refusing resource forks and foreign metadata that the Haiku CLI skips.
Haiku's [BFS lookup implementation](https://github.com/haiku/haiku/blob/master/src/add-ons/kernel/file_systems/bfs/kernel_interface.cpp)
passes the UTF-8 name bytes and byte length directly to its B+tree lookup; the
[public storage limits](https://github.com/haiku/haiku/blob/master/headers/os/storage/StorageDefs.h)
derive name and path buffer sizes from `NAME_MAX` and `MAXPATHLEN`.

The profile selects Interchange Profile 1, so new archives use Stored or
BoltLite, 16 KiB blocks, and its entry, per-file, and serialized-size limits.
The Linux writer test cross-extracts case-distinct names, NFC/NFD-distinct
names, a maximum-length component, and a symlink through the Haiku host shim;
it also checks existing symlink-target validation during append, path-limit
refusals, incompatible codecs, and refusal to append to an existing archive
with a too-long BFS path. `make haiku-host-stage-test` passed with Clang on the
current worktree. The host shim uses Linux filesystem
semantics; native BFS volume behavior, MIME attributes, and Haiku runtime
qualification remain unclaimed, as native Haiku qualification was deferred.

## Implemented Sinclair QL profile

The Linux x86-64 CLI accepts `--target-profile=sinclair-ql-latin1-v1` on
`create` and `add`. It requires exact Latin-1 names, enforces QDOS's 36-byte
relative-path limit, and checks case-fold collisions after projecting archive
slashes to QDOS underscores. This catches aliases such as `foo_bar` and
`foo/bar`. It rejects empty/dot components, control characters, colon, and
backslash. Archive names remain canonical UTF-8 and payload bytes are not
changed.

The profile implies Interchange Profile 1 and accepts regular files and
directories only; symlinks, resource forks, and metadata are refused. It caps
files at signed 32-bit I/O size. Timestamps can be restored only when they are
within QDOS's date range and extraction uses the correct `--tz-offset`; QDOS
protection flags are not preserved. `make target-profile-host-test` checks
name projection, aliases, limits, append, and refusal-before-publication.
`make ql-host-test` cross-extracts a Linux-created profile archive through the
QL frontend host build and checks Latin-1 path bytes and payloads. Physical QL
filing-system runtime qualification remains open. The Minerva 1.98 sQLux
hostFS integration also extracts Linux-created flat and nested profile names
with accented Latin-1 bytes intact; this does not qualify physical QL disks.

The macOS CLI also accepts `--target-profile=sinclair-ql-latin1-v1` for both
create and append. Its writer omits Unix mode metadata, refuses source resource
forks and Finder information, and checks exact Latin-1 conversion, QDOS
slash-to-underscore projection, case aliases, the 36-byte path ceiling,
supported entry types, file size, and the Stored/BoltLite 16 KiB codec policy.
`make CC=clang macosx-host-test` verifies create and append, metadata
omission/refusal, flat/nested projection collisions, limits, and Linux-reader
archive integrity on the POSIX host shim ([run record](../build/macosx-host/runs/macosx-host-01_ynba0/run.json)).
This does not qualify native macOS xattrs or QDOS runtime behavior.

## Implemented RISC OS profile

The Linux x86-64, Windows, and macOS CLIs accept
`--target-profile=riscos-latin1-v1` on `create` and `add`. It requires exact
Latin-1 representability, limits each projected
FileCore leaf to ten bytes and each relative path to 255 bytes, rejects
absolute/empty/dot components, RISC OS reserved syntax, and literal dots that
would alias the native dot directory separator. It applies FileSwitch's
Latin-1 case folding and checks duplicate and file/directory-prefix conflicts.
Archive names stay canonical UTF-8 and file contents are unchanged.

The profile implies Interchange Profile 1 and further limits regular files to
signed 32-bit I/O size. It permits regular data and directory entries only;
links, resource forks, and metadata records are refused. Linux, Windows, and
macOS writers omit host metadata, so the RISC OS frontend applies destination
defaults. Existing RISC OS native metadata is
rejected on append rather than silently copied under a profile that does not
preserve it. `make target-profile-host-test` covers name projection, case and
syntax refusal, append checks, codec/block limits, and RISC OS native-metadata
refusal; `make riscos-profile-host-test` independently verifies archives made
by the RISC OS writer through Linux full and mini readers. The Win64 Wine
profile suite covers Latin-1 creation, ten-byte and dot-syntax refusals,
existing case-alias and append refusal, metadata omission, codec limits, and
Linux-reader acceptance. The macOS POSIX host shim separately checks RISC OS
profile Latin-1 names, FileCore syntax, ten-byte components, 255-byte paths,
case aliases, metadata refusal, append preservation, codec limits, and Linux
reader integrity ([run record](../build/macosx-host/runs/macosx-host-irdb43l9/run.json)).
These are host checks; native macOS and filing-system equivalence, metadata
defaults, and runtime qualification remain open.

## Implemented X68000 profile

The shared POSIX, Windows, and macOS CLIs accept `--target-profile=x68000-sjis-v1` on
`create` and `add`. This profile targets Human68k v3's 18.3 filename form. It requires exact
standard Shift-JIS representability (not CP932 extensions), allows at most 18
bytes before the optional dot and 3 bytes after it, and caps the relative path
at 255 Shift-JIS bytes to match the frontend path buffer. It rejects DOS
forbidden characters, empty components, repeated/trailing dots, ASCII case
aliases, and file/directory-prefix conflicts. Shift-JIS lead/trail pairs are
counted and checked as characters, so a trail byte `0x5c` is not misread as a
path separator. Archive names remain UTF-8 and payload bytes are unchanged.

The profile implies Interchange Profile 1 and caps files at signed 32-bit I/O
size. It allows data and directories only; symlinks, resource forks, and
metadata are refused. Linux and Windows writers omit host metadata, so
destination defaults apply. `make target-profile-host-test` checks exact
Shift-JIS projection, 18.3 and path limits, collisions, invalid mappings,
append, and codec/block refusals. It also extracts a profile-created archive
through the X68000 frontend's host DOS-call shim and verifies raw Shift-JIS
filename bytes and payloads. The Win64 Wine profile suite checks standard
Shift-JIS creation (including a `0x5c` trail byte), 18.3 and character
refusals, case and prefix collisions, metadata omission, codec limits, and
Linux-reader acceptance, including refusal of an existing Interchange Profile 1
archive whose valid components combine to a path over 255 bytes. These checks do
not qualify Human68k v3 runtime, older 8.3 Human68k, or volume-specific behavior.

## Implemented Atari ST writer profile

The Linux x86-64, macOS, and Windows CLIs accept `--target-profile=atari-st`
on `create` and `add`. This implies Interchange Profile 1: 16 KiB blocks, BoltLite by
default (or Stored with `-0`), and its entry-count/archive-size bounds. Other
codecs, block sizes, and codec levels are rejected. The macOS CLI performs
strict preflight and refuses native Mac mode, Finder, and resource-fork data;
the POSIX, Windows, and macOS CLIs implement automatic aliases and the name report.

On Linux `create`, `--target-name-report` adds a mapping report to the archive.
Each row maps the original UTF-8 archive path to its target path; non-ASCII bytes
in the original are written as `\\xHH` escapes. The report is `BPNAMES.TXT`
except for RISC OS, where it is `BPNAMESTXT`. Auto-rename reserves that name
and aliases a conflicting input. The report
option is create-only; append does not rewrite an earlier report. The current
Atari preflight index is bounded to 1,024 projected entries, so that profile
enforces the lower count in addition to Interchange Profile 1's general ceiling.

In the shared POSIX, Windows, and macOS CLIs, `--target-auto-rename` supports the Atari ST profile and the
DOS-family 8.3 profiles (`dos-cp850-83`, `dos-cp437-83`, `dos16-cp850-mini`,
`win16-cp437-mini`, `os2-16-cp850-mini`, and `os2-fat-cp850`), plus
`dos-cp850-lfn-v1`, `amigaos-68k-latin1`, `macos-classic-hfs-v1`,
`sinclair-ql-latin1-v1`, `riscos-latin1-v1`, `x68000-sjis-v1`, `cpm86-83`,
`linux-posix-v1`, `linux-mini-v1`, `posix-utf8-v1`, `plan9-utf8-v1`, and
`haiku-bfs-v1`. It includes a name report
automatically. The target must be selected explicitly:

```text
boltpack create --target-profile=atari-st --target-auto-rename archive.blt source/
boltpack create --target-profile=dos-cp850-83 --target-auto-rename archive.blt source/
```

The Atari implementation uses its uppercase ASCII 8.3 namespace. It preserves
unique Atari-valid components and replaces unsupported, overlong, or colliding components with deterministic
aliases using a short readable prefix and hexadecimal hash suffix. Collisions
are probed until a free target path is found. Directory components and their
descendants receive consistent mappings. Payload bytes are unchanged. The option
is create-only and still refuses symlinks, unsupported metadata, and trees
exceeding target limits. The Atari profile caps mapped paths at 125 bytes. The report records original UTF-8 paths (with non-ASCII bytes escaped)
and final archive paths. The Linux host integration includes a source file named
`BPNAMES.TXT`, verifies that it receives another alias, and resolves each
reported destination to the extracted file with the expected original bytes.
It also reserves the first generated alias of a long filename with a directly
legal source name and verifies that alias allocation probes to a distinct
path. Atari and DOS-family reports use native backslash separators.

For DOS-family 8.3 profiles, the allocator preserves names that fit the selected
CP437 or CP850 8.3 rules and checks collisions using that code page's case
folding. Other names receive unique 8.3 aliases. The target column in the
report uses the selected DOS code page and backslash path separators.
The CP850 long-filename profile preserves valid long names and produces longer,
readable aliases within its component and path limits when a name needs an
alias. Its report also uses CP850 and backslash separators.
The AmigaOS 68k and Classic HFS profiles preserve names representable in
Latin-1 and MacRoman respectively, with component ceilings of 30 and 31 bytes.
Their reports use the corresponding target encoding; Amiga paths use slashes
and Classic HFS paths use colons. Non-Atari alias indexes use the host
preflight's entry and memory bounds; each target's own limits still apply.
The Sinclair QL profile uses Latin-1 and checks collisions after projecting
archive separators to QDOS underscores. Its report uses the same projection.
Auto-rename conservatively caps mapped paths at 35 bytes so directory entries
also fit the profile's 36-byte limit.
RISC OS uses Latin-1, limits components to ten bytes, and treats dots as path
separators. Its report is named `BPNAMESTXT` and uses dots between components.
X68000 uses standard Shift-JIS, 18.3 byte limits, and case-insensitive ASCII
comparison that preserves multibyte characters. CP/M-86 flattens files to
unique uppercase 8.3 names because its profile does not support directories.
The Linux, POSIX, Haiku, and Plan 9 profiles preserve valid case-sensitive
UTF-8 names and apply their individual component and path limits. Their report
uses UTF-8; control bytes in target paths are escaped as `\xHH`.

The strict profile accepts only names that survive Atari ST codepage
conversion and the printable-ASCII GEMDOS 8.3 projection. It checks
the 125-byte full file-specification ceiling, uppercase/case-fold collisions, duplicate
projected names, file/directory prefix conflicts, and the 2,147,483,647-byte
per-file signed-I/O ceiling. It refuses symbolic links, resource forks, and
metadata from platforms other than Atari ST or the portable metadata namespace.
Neither mode transcodes file contents. Create preflights all collected entries
before writing archive contents; append checks existing and new entries
together before adding.

The macOS and Windows CLIs also accept the strict profile. `make macosx-host-test` checks
create/append, GEMDOS case collisions, high-bit filename refusal, codec limits,
and omission of Unix mode metadata. This POSIX-host test does not qualify native
macOS resource-fork or Finder metadata handling. `make win64-target-profile-test`
checks Windows creation, append collision refusal, high-bit filename refusal,
codec limits, and Linux-reader acceptance under Wine; native Windows behavior
remains unqualified.

The [Atari ST GEM Programming reference (1986)](https://www.bitsavers.org/pdf/atari/ST/Atari_ST_GEM_Programming_1986/GEM_0042.pdf)
documents 125 characters as the maximum full file specification. That is also
the BoltPack Atari client's checked path-buffer limit. The archive profile's 2,147,483,647
byte maximum follows Interchange Profile 1 and the Atari client's signed-long
file seek interface; GEMDOS documents `Fseek`'s offset and return as signed
`LONG` values (see the [Atari Compendium Fseek reference](https://info-coach.fr/atari/software/_development/Atari-Compendium.pdf)).
These are conservative profile/client bounds, not claims
that every GEMDOS filesystem has identical limits. The Atari client checks the
joined output path too, so a deep `-o` destination can still be refused even
when each archive member fits.

The profile fixes blocks at 16 KiB and caps its path/collision preflight at
1,024 entries (32 KiB index budget). Hatari/TOS 1.62 passes the profile-backed
FAT12 fixture with 1 MiB RAM. With 512 KiB, Hatari reports that the 335 KiB
full CLI executable is too large to load and all guest commands return -39.
The current Atari CLI therefore needs at least the tested 1 MiB configuration;
the profile does not currently cover 512 KiB machines. The fixture's memory
size is configurable with `ATARI_HATARI_MEMSIZE`.

The 2026-11-01 Hatari/TOS 1.62 FAT12 rerun also creates an archive from the
Linux UTF-8 filename `öong.txt` with `--target-auto-rename`, extracts it on the
Atari guest, and verifies both the `BPNAMES.TXT` mapping and byte-identical
payload under the reported alias ([run record](../build/atari-hatari-alias-profile-20261101-rerun/run.json)).

The filename encoding step uses the Atari ST codepage, but classic GEMDOS
filename syntax separately rejects every byte greater than `0x7f`. Therefore
an accented character such as `ö` is representable in Atari text, yet is not a
legal classic GEMDOS filename character. Strict mode refuses it, while
`--target-auto-rename` assigns a legal ASCII 8.3 alias and records the original
UTF-8 path in `BPNAMES.TXT`. This restriction follows the [1986 Atari GEMDOS
filename reference](https://www.bitsavers.org/pdf/atari/ST/Atari_ST_GEM_Programming_1986/GEM_0042.pdf),
which explicitly excludes characters greater than `0x7f` from filenames.

Strict writer preflight is implemented in the Linux x86-64, macOS, and Windows
CLIs, as are automatic aliases and `BPNAMES.TXT`. This does not establish
that every Atari ST filesystem or TOS version has identical practical limits.

The selected profile is not recorded in the archive. Format version 6 requires
its reserved header flags to remain zero, so the CLI does not store a profile
identifier there. An archive created under this option therefore carries no
machine-readable target-compatibility assertion, and a later append must select
the profile again to receive these checks.

## Implemented DOS-family 8.3 profiles

The Linux x86-64, macOS, and Windows CLIs accept these target identifiers
on `create` and `add`:
`dos-cp850-83`, `dos-cp437-83`, `dos16-cp850-mini`,
`win16-cp437-mini`, `os2-16-cp850-mini`, and `os2-fat-cp850`. They compose
Interchange Profile 1: BoltLite by default or Stored with `-0`, 16 KiB blocks,
and the common count, file-size, and archive-size bounds.

These profiles convert UTF-8 paths to CP850 or CP437, enforce a conservative
8.3 character set, reject reserved DOS device names, case-fold with the
selected codepage, detect projected duplicates and file/directory conflicts,
and cap paths at 259 bytes. They reject absolute paths, symbolic links,
resource forks, and foreign-platform metadata. Portable and matching target
metadata are accepted. Each file is limited to the signed 32-bit I/O range.
Nonconforming names are refused rather than shortened. Host integration
coverage is in `test/check_target_dos_profiles.py`; it does not replace target
runtime qualification. The CP850 8.3 Linux-created archive now also passes the
DOSBox-X FAT12 round trip: DOS extracts `Ø.TXT`, while Linux integrity-tests
and extracts the DOS-created archive. The run record is
[`r02-dosboxx-cp850-s4b_r8eo/run.json`](../build/r02-dosboxx-cp850-s4b_r8eo/run.json).
This qualifies the emulated DOSBox-X/FAT12 path for this CP850 character and
profile, not every DOS implementation or filesystem.

The macOS CLI also accepts all six identifiers. It applies the same exact
projection, 8.3/device-name rules, collision checks, signed-I/O ceiling,
and Interchange Profile 1 codec/size limits. It omits Unix mode records,
rejects metadata outside the selected target namespace, and on macOS refuses source resource
forks and meaningful Finder information. `make macosx-host-test` checks
all six create paths, CP437/CP850 names, CP850 append, invalid names, option
refusal, and Linux-reader acceptance through the POSIX host shim; native
DOS-family filesystem behavior remains unqualified.

The Windows 32/64 CLI also accepts `dos-cp850-83`, `dos-cp437-83`,
`dos16-cp850-mini`, `win16-cp437-mini`, `os2-16-cp850-mini`, and
`os2-fat-cp850`. Its preflight validates exact CP850/CP437 projection, 8.3
character and device-name rules, case aliases, file/directory conflicts, entry
types, and the signed 32-bit file ceiling on both create and append. It retains
portable FAT metadata and composes Interchange Profile 1. The Win64 Wine
integration creates all six profiles and verifies them with the Linux reader.
CP850 and CP437 8.3 cases cover both encodings and incompatible codec refusal;
CP850 additionally checks append and rejection of invalid existing names. Native
DOS/Win16/OS/2 volume behavior remains unqualified.

## Implemented AmigaOS 68k profile

The Linux x86-64, macOS, and native Windows CLIs accept
`--target-profile=amigaos-68k-latin1` on `create` and `add`. All three compose
Interchange Profile 1: BoltLite by default or Stored with `-0`, 16 KiB blocks,
and the common count, file-size, and archive-size bounds. Archive paths remain
canonical UTF-8; preflight requires exact ISO-8859-1 projection to the AmigaOS
68k filename namespace.

The profile currently caps each projected component at 30 bytes and the
relative path at 255 bytes, rejects absolute paths, empty/dot components,
control characters, backslashes, and the Amiga volume separator (`:`), and
checks Latin-1 case-fold collisions and file/directory prefix conflicts. It
allows regular files, directories, and AmigaOS 68k metadata; it rejects
portable or other platform metadata, resource forks, symlinks, and files above
the signed 32-bit I/O limit. This matches the Amiga reader, which applies only
its own metadata namespace. The profile does not rename names or transcode
payloads. The Windows writer omits portable FAT metadata, which has no Amiga
meaning, and refuses source read-only, hidden, or system attributes instead of
silently losing them. The macOS writer omits Unix mode metadata and, on macOS,
refuses resource forks and meaningful Finder information. All three append
paths reject existing foreign metadata.
These numeric filesystem limits are conservative v1 rules and still need
qualification against the specific Amiga filesystems and OS versions.

`test/check_target_amiga_profile.py` covers Latin-1 names, nested paths,
unrepresentable characters, component limits, separators, and archive
integrity for the Linux writer. `make macosx-host-test` covers macOS CLI
creation and append through the POSIX host shim, and
`make win64-target-profile-test` covers Windows creation and append under Wine.
Both check Latin-1 collision refusal, unrepresentable names, codec/block
restrictions, metadata policy, and Linux-reader verification. These host tests
do not establish native macOS qualification. FS-UAE 3.1.66 with Kickstart
2.04 verifies a Linux-created profile archive containing `café.txt` on both
OFS and FFS floppies: the Amiga CLI exits successfully, and `xdftool` reads the
exact path and payload back from each disk. Run records are
[`OFS`](../build/amiga-native-ofs-target-profile-getvar-20261101/manifest.txt)
and [`FFS`](../build/amiga-native-ffs-target-profile-getvar-20261101/manifest.txt).
This qualifies that profile transfer on the tested emulator/filesystem
combination, not every AmigaDOS volume or OS version.

## Proposed behavior

A full-featured creator should accept a target profile on create and append,
for example:

```text
boltpack create --target-profile=atari-st archive.blt source/
```

The initial command-line option is intended for full clients on 32-bit and
wider systems. The selected profile describes the destination, independently
of the creator's operating system. A constrained client may still consume an
archive made under a profile if its reader supports the declared codecs and
limits.

The selected profile should:

1. Preflight the complete source tree before opening the final archive path.
2. Convert each source name exactly from the creator's native encoding to
   UTF-8. Refuse invalid or unmappable names; do not transliterate silently.
3. Project each UTF-8 name to the target's native path representation and
   validate component lengths, full path lengths, separators, reserved names,
   case folding, normalization, and file/directory prefix conflicts.
4. Check every selected codec, block size, entry size, entry count, total
   serialized size, entry type, and required metadata against the target
   reader's declared capabilities.
5. Refuse publication if the archive would violate the profile. Diagnostics
   should identify the path or property that failed.

Regular files and directories are the baseline supported entry types. A
profile must explicitly allow and qualify symlink creation before it can
promise symlink extraction. Profiles for filesystems without symlinks should
refuse archives containing them in strict mode. Resource forks, Finder fields,
Amiga comments/protection bits, RISC OS filetypes, and other platform metadata
must have an explicit required/portable/best-effort policy; they must not be
silently treated as ordinary file contents.

Profiles default to exact names and refusal. The shared POSIX, Windows, and
macOS CLIs support an explicit auto-alias mode backed by `BPNAMES.TXT` for
Atari ST and the DOS-family 8.3 and CP850 long-filename profiles, AmigaOS 68k,
Classic HFS, Sinclair QL, RISC OS, X68000, CP/M-86, and the Linux, POSIX, Haiku
and Plan 9 profiles.

For append, validate both the existing archive and all new entries under the
selected profile. Do not append to an archive that already violates it.

## On-disk declaration

An optional archive-level declaration could record a stable profile identifier
and policy revision, such as `atari-st-gemdos-v1`. This would let `boltpack info`
show the intended compatibility claim and let a reader check it. The
declaration must be treated as a claim, not proof: readers still validate
archive structure and projected paths themselves.

The serialization mechanism remains open. First check whether an optional
archive-level descriptor can be added compatibly to format version 6 and safely
ignored by old readers. If not, define it in a future format version. Do not
reuse creator OS/CPU fields or imply that the creator's system is the target.
An untagged archive remains valid; a missing declaration means “no target
profile asserted.”

Profile identifiers need explicit revisions. Changing the allowed codepage,
name rules, codec set, or numeric limits changes conformance and therefore
requires a new profile revision or identifier.

## Profile catalog

This catalog groups frontends that share a target filesystem contract. A
profile is for a target environment, not for each CLI or GUI binary. Limits
marked **confirm** require target-specific qualification before the profile
can make a strong compatibility promise.

| Profile | Target constraints to enforce | Codec and size policy | Current evidence and qualification |
|---|---|---|---|
| `linux-posix-v1` | Valid canonical UTF-8 paths; 255-byte components and bounded 4,095-byte paths; case-sensitive names; reject duplicate and file/directory-prefix conflicts. Refuse resource and metadata entries skipped by the Linux extractor; allow symlinks. | Full reader codecs compiled into the target. Optional Interchange Profile 1 limits. | The shared POSIX, Windows, and macOS CLI writer option `--target-profile=linux-posix-v1`; host checks cover path encoding, symlinks, case-sensitive names, and Linux-reader acceptance. Win64 Wine and the macOS POSIX host shim also cover automatic aliases and reports. |
| `linux-mini-v1` | Same Linux path mapping; bounded preflight; regular files and directories only; check existing archive when appending. | Stored, BoltLite, and BoltLZ; 32 KiB maximum block; 65,535 physical entries; 4,294,967,295-byte per-file limit; 2,147,483,647-byte serialized archive limit. | The shared POSIX, Windows, and macOS CLI writer option `--target-profile=linux-mini-v1`; integration checks use the native 32-bit mini reader for integrity and extracted payloads. Runtime checks cover this Linux target profile directly. |
| `windows-nt-lfn-v1` | Windows CLI: UTF-8 paths mapped through UTF-16; NFC collision keys; reject reserved device names, trailing dots/spaces, case-insensitive aliases, and file/directory conflicts; 255-unit components and 259-unit relative paths. Win32, .NET, FLTK, and WCX apply create-side preflight. | Methods supported by the selected Windows reader build; optional Interchange Profile 1 limits. Portable Windows FAT metadata is best effort. | Native Windows CLI option `--target-profile=windows-nt-lfn-v1`; `make win64-target-profile-test` runs create/append and Linux-created collision/invalid-name archives under Wine. `make win32-target-profile-test` runs the same corpus against the Win32 binary when 32-bit Wine is installed; this machine has the MinGW Win32 compiler but Wine reports that its 32-bit runtime is missing. Win32 GUI changes compile and `make win64-gui-profile-test` exercises its production preflight under Wine; full dialog/create flow remains unqualified. Mono 6.12 compiles the .NET GUI sources and `make win-gui-net-profile-host-test` exercises its production create validator and collision rules. SDK-style .NET 4.8 project build and Windows WinForms runtime remain unqualified. `make win64-fltk-wine-create-test` verifies GUI creation with a supplementary-Unicode filename, Linux full/mini integrity, and exact payload extraction; case and NFC source-collision suites verify refusal before the save dialog and archive publication ([case](../build/r08-win64-fltk-wine-create-dn0h0ceg/run.json), [NFC](../build/r08-win64-fltk-wine-create-t8eqezop/run.json)). `make win64-wcx-wine-test` verifies Unicode pack/append and refuses case/NFC collision fixtures while preserving the archive on failed append. Native Windows volume behavior remains unqualified. |
| `dos-cp850-83-v1` | CP850 conversion; DOS legal-character/device-name checks; uppercase/case-fold collision checks; flat 8.3 components and path limits. | Conservative Stored/BoltLite and Interchange Profile 1 limits for broad 16-bit compatibility. | Linux x86-64, macOS, and Windows 32/64 CLI writer option `--target-profile=dos-cp850-83`; Linux integration, the macOS POSIX host shim, and Win64 Wine cover encoding, reserved names, collisions, append, and archive integrity. DOSBox-X 2025.02.01/FAT12 verifies the profile-created `Ø.TXT` archive extracts on DOS16 and a DOS16-created archive extracts on Linux; [run record](../build/r02-dosboxx-cp850-s4b_r8eo/run.json). FreeDOS 1.4 on QEMU 10.0.13/FAT12 also verifies lowercase `ø.TXT` lookup and create with CP850 selected, exact archive transfer in both directions, nested directories and FAT flags, low-space replacement preservation, and backup recovery ([run record](../build/freedos-cp850-profile-audit-20261101/run.json)). This does not qualify every DOS filesystem. |
| `dos-cp437-83-v1` | Same DOS 8.3 and path rules, using CP437 conversion and CP437 case folding. | Conservative Stored/BoltLite and Interchange Profile 1 limits. | Linux x86-64, macOS, and Windows 32/64 CLI writer option `--target-profile=dos-cp437-83`; Linux integration, the macOS POSIX host shim, and Win64 Wine verify CP437 projection and Linux-reader acceptance. DOSBox-X 2025.02.01/FAT12 extracts the profile-created `café.txt` archive and verifies the DOS16-created archive on Linux; [run record](../build/r02-dosboxx-cp437-p5c1f2hy/run.json). The fixture reports partial because DOS cannot restore Linux metadata; this does not qualify every DOS filesystem. |
| `dos-cp850-lfn-v1` | Exact CP850 conversion; DOS LFN forbidden-character/device rules; 255-byte components, 259-byte paths, case-fold collision and prefix checks. DOS reader probes destination LFN support when extracting; choose `--target-profile=dos-cp850-83` for guaranteed short-name-only transfer. | Stored/BoltLite and Interchange Profile 1 bounds; signed 32-bit file-size ceiling; regular files/directories and DOS/portable FAT attributes. | Linux x86-64, macOS, and Windows 32/64 CLI writer option `--target-profile=dos-cp850-lfn-v1`; Linux, macOS host-shim, and Win64 Wine checks cover long CP850 names, reserved names, collisions, append, codec refusal, and Linux-reader acceptance. DOS host LFN simulation and DJGPP DOSBox-X 2025.02.01/FAT12 with LFN enabled and `KEYB UK 850` verify extraction of `Long Filename Ø With Spaces.txt`; mtools reads the exact Unicode name and payload; [run record](../build/dosboxx-cp850-lfn-nonascii-20261101/manifest.json). Earlier ASCII-only evidence remains at [this run record](../build/dosboxx-lfn-profile-20261101/manifest.json). Native DOS and other filesystems remain unqualified. |
| `dos16-cp850-mini-v1` | CP850 conversion under the DOS16 mini client's 8.3, device-name, path, collision, and buffer limits. | Mini-compatible Stored/BoltLite limits. | Linux x86-64, macOS, and Windows 32/64 CLI writer option `--target-profile=dos16-cp850-mini`; host integration covers shared constraints and Win64 Wine verifies profile creation and Linux-reader acceptance. DOS16 emulator tests cover reader behavior, not native target-volume behavior. |
| `win16-cp437-mini-v1` | CP437 conversion; 16-bit path and buffer limits; case-fold preflight; Windows 3.x filesystem rules. | Mini-compatible Stored/BoltLite limits. | Linux x86-64, macOS, and Windows 32/64 CLI writer option `--target-profile=win16-cp437-mini`; host integration covers shared constraints and Win64 Wine verifies profile creation and Linux-reader acceptance. Win16 runtime remains unqualified. |
| `os2-16-cp850-mini-v1` | CP850 conversion; 16-bit path and buffer limits; filesystem-specific case and name projection. | Mini-compatible Stored/BoltLite limits. | Linux x86-64, macOS, and Windows 32/64 CLI writer option `--target-profile=os2-16-cp850-mini`; host integration covers shared constraints and Win64 Wine verifies profile creation and Linux-reader acceptance. OS/2-16 runtime remains unqualified. |
| `os2-fat-cp850-v1` | CP850 conversion; FAT 8.3 rules, case-folding, reserved names, and OS/2 path limits. | Stored/BoltLite for conservative transfer; signed-I/O and Interchange Profile 1 limits. | Linux x86-64, macOS, and Windows 32/64 CLI writer option `--target-profile=os2-fat-cp850`; host integration covers shared FAT name constraints and Win64 Wine verifies profile creation and Linux-reader acceptance. OS/2 full CLI cross-build and publication tests exist; native filesystem runtime remains unqualified. HPFS or other IFS targets require separate profiles. |
| `atari-st-gemdos-v1` | Exact Atari ST codepage conversion followed by classic GEMDOS legality checks; GEMDOS excludes bytes above `0x7f`, with 8.3 components, 125-byte full file-specification cap, ASCII case folding, and collision/prefix preflight. Strict mode refuses shortening or high-bit filename bytes; Linux auto-rename creates ASCII aliases with a mapping report. | Stored/BoltLite, 16 KiB blocks, Interchange Profile 1 count/archive-size limits, and a 1,024-entry/32 KiB name-index budget. The signed-long GEMDOS seek path motivates the 2,147,483,647-byte archive/file ceiling. Current full Atari CLI requires at least 1 MiB RAM (Hatari/TOS 1.62); 512 KiB cannot load the executable. | The shared POSIX, macOS, and Windows CLIs accept `--target-profile=atari-st` and `--target-auto-rename`. Linux host integration verifies strict rejection of Atari-codepage-representable `ö` plus alias/report behavior. `make macosx-host-test` verifies strict create/append, GEMDOS case collision and high-bit refusal, codec limits, and mode-metadata omission ([run](../build/macosx-host/runs/macosx-host-hclhqtxq/run.json)); it does not qualify native macOS metadata behavior. `make win64-target-profile-test` under Wine covers projected-name refusal, creation, append collision, codec refusal, and Linux-reader acceptance. Hatari/TOS 1.62 FAT12 verifies profile extraction, and a [guest alias-transfer run](../build/atari-hatari-alias-profile-20261101-rerun/run.json) confirms the report and exact payload for an accented source name. The [4 MiB run](../build/atari-hatari-fat-20261101-135330/run.json) and [1 MiB run](../build/atari-hatari-fat-20261101-135525/run.json) passed; the [512 KiB failure record](../build/atari-hatari-fat-20261101-135705/run.json) retains Hatari’s “Program too large” diagnostic; the 317,438-byte `-Os` build failed the same way in [this run](../build/atari-hatari-fat-512k-os/run.json). Physical Atari and other TOS/filesystem behavior remain open. |
| `amigaos-68k-latin1-v1` | Exact Latin-1 projection; reject colon, absolute paths, empty/dot components, and control characters; 30-byte component and 255-byte relative-path v1 ceilings; Latin-1 case-fold collision and prefix checks. | Stored/BoltLite with Interchange Profile 1 bounds; signed 32-bit per-file I/O ceiling. | Linux x86-64, macOS, and Windows 32/64 CLI options `--target-profile=amigaos-68k-latin1`; Linux integration plus macOS POSIX-host and Win64 Wine tests cover creation, append, encoding, limits, metadata omission/refusal, and Linux-reader acceptance. FS-UAE 3.1.66/Kickstart 2.04 extracts Linux-created profile archives and reads back `café.txt` and exact bytes on OFS and FFS; [OFS](../build/amiga-native-ofs-target-profile-getvar-20261101/manifest.txt), [FFS](../build/amiga-native-ffs-target-profile-getvar-20261101/manifest.txt). Native macOS and other Amiga versions/filesystems remain unqualified. OS4, MorphOS, and AROS need separate profiles. |
| `cpm86-83-v1` | Flat uppercase ASCII 8.3 names; regular files only; reject directories, separators, unsupported characters, and projected collisions. | Stored/BoltLite, 32 KiB max blocks, 256 entries, signed 32-bit files, 2,147,483,647-byte archive ceiling. | Linux x86-64, macOS, and Windows 32/64 CLI writer option `--target-profile=cpm86-83`; Linux integration extracts with the CP/M-86 frontend and checks limits, macOS host-shim and Windows Wine checks cover projection, append, collisions, metadata omission, codec refusal, and Linux-reader acceptance. Native CP/M-86 runtime remains unqualified. |
| `sinclair-ql-latin1-v1` | Exact Latin-1 conversion; project `/` to QDOS `_` before collision checks; reject empty/dot components, controls, colon, and backslash; 36-byte relative-path ceiling; case-fold aliases. QL native source creation strips recognized device prefixes; Linux, macOS, and Windows writers validate archive-member output projection. | Stored/BoltLite and Interchange Profile 1 bounds; signed 32-bit per-file I/O ceiling; data and directories only, no metadata, resource forks, or links. Timestamp restoration depends on QDOS range and timezone. | Linux x86-64, macOS, and Windows CLIs accept `--target-profile=sinclair-ql-latin1-v1`. Linux and macOS host suites check projection, aliases, limits, metadata policy, append, and refusals; macOS evidence is [here](../build/macosx-host/runs/macosx-host-01_ynba0/run.json). `ql-host-test` checks frontend extraction, and Minerva 1.98 sQLux hostFS extracts Linux-created Latin-1 flat/nested names byte-identically. Win64 Wine checks creation, slash collisions, append refusal, unmappable names, codec refusal, and Linux-reader acceptance. Physical QL disks and native macOS behavior remain unqualified. |
| `x68000-sjis-v1` | Human68k v3 18.3 names in strict standard Shift-JIS; 18-byte stem, 3-byte extension, 255-byte relative path; reject CP932 extensions, DOS-forbidden characters, malformed dot/separator syntax, ASCII case aliases, and file/directory conflicts; scanner respects Shift-JIS trail bytes. | Stored/BoltLite with Interchange Profile 1 bounds; signed 32-bit per-file I/O ceiling; data and directories only, no links, resource forks, or metadata. | The shared POSIX, Windows, and macOS CLI writer option `--target-profile=x68000-sjis-v1`; host integration cross-extracts through the X68000 frontend and verifies raw Shift-JIS paths. Win64 Wine covers 18.3, trail-byte handling, a greater-than-255-byte existing path refusal, case/prefix collisions, codec refusal, metadata omission, and Linux-reader acceptance. Native Human68k v3 behavior and older 8.3 systems remain unqualified. |
| `riscos-latin1-v1` | Exact Latin-1 conversion; 10-byte components and conservative 255-byte relative paths; reject absolute paths, empty/dot components, RISC OS syntax characters, and literal dots that alias directory separators; FileSwitch Latin-1 case-fold and file/directory-prefix checks. Data and directories only. Linux, Windows, and macOS writers omit native/portable host metadata; the RISC OS client uses destination defaults. | Stored/BoltLite with Interchange Profile 1 limits (16 KiB blocks, 65,535 physical entries, 4 GiB per file and 2 GiB serialized archive); additionally cap files at signed 32-bit I/O size. | Linux x86-64, Windows, and macOS CLI writer option `--target-profile=riscos-latin1-v1`; Linux and macOS host integration check Latin-1 names, FileCore limits, aliases, append validation, and archive integrity. RISC OS host-shim tests verify native-writer output through Linux full/mini readers. Win64 Wine checks Latin-1 creation, component/dot refusals, existing case-alias refusal, metadata omission, codec limits, and Linux-reader acceptance. macOS host shim also verifies metadata omission/refusal and failed-append preservation ([run record](../build/macosx-host/runs/macosx-host-irdb43l9/run.json)). Native filing-system equivalence and metadata defaults remain unqualified. |
| `macos-classic-hfs-v1` | MacRoman mapping; HFS component limit of 31 bytes; conservative 255-byte path limit; reject colon, controls, backslash, dot components, and unmappable names; MacRoman case-fold and file/directory conflict checks. Refuses links, resource forks, and metadata; the macOS CLI omits Unix mode records and refuses source Finder/resource-fork data, while the Windows CLI omits FAT records and refuses read-only/hidden/system sources. | Stored/BoltLite and Interchange Profile 1 bounds; signed 32-bit per-file I/O ceiling. | Linux x86-64, macOS, and native Windows CLI writer options `--target-profile=macos-classic-hfs-v1`; Linux and macOS host integration plus Win64 under Wine check conversion, collisions, limits, create, append, metadata/type policy, and integrity. QEMU 10.0.13 q800 boots System 7.5.3; the 68k app launches and extracts a stored root-level file with byte-identical payload readback. A later profile run creates both nested directories and three root-level files with expected lengths, but leaves the nested 21-byte payload under staging name `BP0003.TMP` and reports partial; the stage-prefix bug found in that run is fixed. Nested publication and payload hashes remain unqualified. Non-ASCII MacRoman behavior and metadata restoration remain unqualified. See [runtime notes](emulator-setup.md) and [nested run record](../build/macos-classic-qemu/resource-package-20261101/runtime-nested-stage-fix-20261101.json). HFS and HFS+ are separate. |
| `macos-modern-v1` | UTF-8 archive paths; native CoreFoundation decomposition and case-fold collision checks, applied conservatively even on case-sensitive volumes; reject unsafe paths and names exceeding compile-time `NAME_MAX`/`PATH_MAX`; reject file/directory prefix conflicts. | Full codec set compiled into the macOS CLI; optional Interchange Profile 1 can be combined. Actual volume-specific limits and `off_t`/filesystem size remain platform constraints. | macOS CLI option `--target-profile=macos-modern-v1` on create/add performs writer preflight over the complete resulting archive. It uses the same checks as extraction. The Linux host shim can test ASCII case aliases and size/prefix checks but its fallback name key is ASCII-only; native CoreFoundation Unicode normalization and APFS/HFS+ volume behavior require macOS runtime qualification. Native metadata capture remains the regular macOS CLI policy: resource forks, Finder info, mode, and checked times are captured where available. |
| `haiku-bfs-v1` | Canonical UTF-8 byte paths; BFS byte-sensitive, case-sensitive lookup; 255-byte components and 1,023-byte relative paths; duplicate and file/directory-prefix checks. Data, directories, safe symlinks, and Haiku `BP_OS_BEOS` metadata are supported; refuse resource forks and foreign metadata. | Stored/BoltLite with Interchange Profile 1 bounds. | The shared POSIX, Windows, and macOS CLI writer option `--target-profile=haiku-bfs-v1`; `make haiku-host-stage-test` cross-extracts case/normalization-distinct names, a 255-byte component, and a symlink through the Haiku host shim, and checks safe-link append, path, codec, and refusal behavior. Win64 Wine and the macOS POSIX host shim cover automatic aliases, reports, and Linux-reader acceptance. Native BFS, MIME semantics, and runtime remain unqualified; native qualification was deferred. |
| `plan9-utf8-v1` | Canonical UTF-8; case-sensitive paths; reject controls, DEL, unsafe components, symlinks, and foreign metadata; paths up to 511 bytes, components up to 255 bytes; conservatively bound path-index resources. Writes source rwx permissions as two-byte `BP_OS_PLAN9` metadata; refuses setuid, setgid, and sticky bits; known timestamps must fit unsigned 32-bit seconds. | Stored, BoltLite, BoltLZ, or Fluff; blocks up to 256 KiB. Do not impose a stronger file-size ceiling without evidence from the selected file server. | The shared POSIX, Windows, and macOS CLI writer option `--target-profile=plan9-utf8-v1`; the Windows writer synthesizes 0644/0755 mode records because it has no POSIX mode to preserve. The Plan 9 reader uses `-o` to extract into a new private root, restores supported mode/time metadata, and refuses existing roots and implicit current-directory extraction. `make plan9-host-test` verifies writer create/append, exact mode metadata, timestamp range, codec limits, unsafe names, long paths, symlinks, exact extraction bytes, nested/case-sensitive names, metadata, dry-run, and existing-root preservation. Win64 Wine and the macOS POSIX host shim cover automatic aliases and Linux-reader acceptance. Native runtime and successful transfer remain unqualified. |
| `posix-utf8-v1` | Strict UTF-8 byte names; case-sensitive byte-exact lookup without normalization; 255-byte components and 4,095-byte relative paths; duplicate and file/directory-prefix checks. Supports data, directories, and safe relative symlinks; refuses resource forks and metadata skipped by the Linux POSIX extractor. Requires a reader with directory-relative no-follow extraction enabled. | Full reader codecs compiled into the target; optional Interchange Profile 1. | The shared POSIX, Windows, and macOS CLI writer option `--target-profile=posix-utf8-v1`; host integration verifies Fluff, symlink create/append/transfer, path checks, and integrity. Win64 Wine and the macOS POSIX host shim cover automatic aliases and Linux-reader acceptance. Targets POSIX filesystems conforming to the stated byte-sensitive contract; it does not claim universal POSIX filesystem semantics. Wrapper runtime qualification remains open, and path-based fallback wrappers are excluded. |

## How profiles relate to Interchange Profile 1

Interchange Profile 1 remains the common format-level profile for constrained
readers: Stored/BoltLite, bounded blocks, entry count, entry size, and serialized
archive size. A target profile adds filesystem names, path limits, entry-type
support, and metadata requirements. A target profile may be stricter than
Interchange Profile 1, but it must not relax a limit required by the selected
reader.

The writer must choose the intersection of the requested target profile and
the actual writer/reader capabilities. An explicit method option outside that
intersection should fail rather than be silently changed.

## Compatibility levels and reporting

Profiles should define one of these outcomes for metadata:

- **Required:** refuse an archive operation when the requested metadata cannot
  be preserved as specified.
- **Portable:** retain supported portable metadata in the archive; report
  native restoration that the target cannot perform.
- **Best effort:** preserve payload and representable paths, and return a
  partial result for unsupported metadata.

Do not describe an archive as “fully compatible” when only payload and names
are guaranteed. `boltpack info` should distinguish a target profile assertion
from observed codec support and from metadata loss reported during extraction.

## Remaining qualification work

The profile identifiers and their writer-side semantics are specified above.
Profiles are deliberately not stored in the archive: every selected writer
preflights the complete resulting archive, including existing entries during
append, and stores ordinary format records. This keeps legacy readers able to
open the archive without needing to understand a target-profile declaration.

The remaining work is evidence gathering and any fixes it exposes:

1. Run each profile through its native filesystem or a suitable emulator where
   the catalog marks runtime behavior unqualified. Do not infer target
   qualification from host shims or another operating system's reader.
2. Revisit numeric ceilings only when target-specific evidence contradicts the
   documented value. In particular, retain the stated Atari GEMDOS seek/RAM,
   Amiga path, and QL/X68000 runtime limits as conservative until native
   evidence supports changing them.
3. Keep Atari auto-renaming and its create-time mapping report specific to the
   Atari profile. Reports are not restored as archive metadata; users retain
   the report when they need to map aliases back to source names.
4. Keep unsupported clients and runtime environments listed per catalog row;
   host-shim success alone does not close those gaps.

Related records: [Interoperability matrix](interoperability-matrix.md),
[Interoperability requirement ledger](interoperability-requirement-ledger.md),
and [archive format plan](archive-format-plan.md).
