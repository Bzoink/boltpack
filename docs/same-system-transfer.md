# Same-system transfer: what survives an archive round trip

**Created:** 2026-11-11
**Archive format:** 6

This document answers one question for every supported system: if an archive is
created on one machine and extracted on another machine **of the same type**
(same operating system and filesystem family), which data and filesystem
resources arrive, which arrive in reduced form, and which do not.

It is not about moving archives between different systems. That case needs name
and attribute mapping, and is covered by the
[interoperability matrix](interoperability-matrix.md), the
[target compatibility profiles](target-compatibility-profiles.md) and the
[specification](specification-1.0.md).

It describes what the code is written to do, taken from the frontend sources and
the matrix, together with how well each system has been verified. Where a
statement rests only on a host test or a cross-build, it says so. Hardware
results are collected in [hardware-testing.md](hardware-testing.md).

## Contents

- [Reading this document](#reading-this-document)
- [What every system carries](#what-every-system-carries)
- [Summary table](#summary-table)
- [Unix-like systems](#unix-like-systems)
- [macOS](#macos)
- [Windows](#windows)
- [DOS, OS/2 and CP/M-86](#dos-os2-and-cpm-86)
- [Classic Mac OS](#classic-mac-os)
- [Amiga](#amiga)
- [Atari](#atari)
- [Sinclair QL, X68000, RISC OS, Plan 9, Haiku/BeOS](#other-systems)
- [Things that are easy to miss](#things-that-are-easy-to-miss)
- [Gaps worth fixing, in priority order](#gaps-worth-fixing-in-priority-order)

## Reading this document

**CPU architecture does not matter.** The archive is byte-order neutral (all
integers are little-endian) and a creating machine's CPU is recorded only for
information. Linux on x86-64, ARM and 32-bit x86, or macOS on PowerPC, Intel and
Apple Silicon, behave the same way for the same operating system. Where a CPU
matters at all it is for limits, such as 16-bit memory ceilings, and those are
noted.

**Names need no mapping within one system.** A name that exists on the source
system is by construction legal on the destination of the same type, so same-type
transfers do not trigger the name projection that cross-system transfers need. Two
rules still apply: a name must be valid UTF-8 to be archived (creation stops with
an error otherwise), and extraction refuses names that would escape the
destination folder.

**Result words.** In the tables below:

| Word | Meaning |
| ---- | ------- |
| yes | Stored on create and restored on extract. |
| partly | Stored or restored with a stated limit. |
| no | Not stored. The data or property is lost in the round trip. |
| n/a | The system has no such thing. |

**Verification words** describe the strongest evidence recorded in the
[requirement ledger](interoperability-requirement-ledger.md) and the matrix:

| Word | Meaning |
| ---- | ------- |
| native | Run on the real system. Nothing has reached this level for any system in the matrix as of this writing, except where a row says otherwise. |
| emulator | Run in an emulator with a real guest filesystem (FS-UAE, Hatari, DOSBox-X, sQLux, Wine). |
| host | Run on Linux against shims or stubs of the target's system calls. |
| build | Cross-built or syntax-checked only. |

**Reporting contract.** Everywhere, a property that cannot be restored is
reported, not dropped silently. The tool finishes extracting the data, says which
items were affected, and exits non-zero (`partial`). The one exception worth
knowing about is in [Things that are easy to miss](#things-that-are-easy-to-miss).

## What every system carries

These hold on every frontend, because they are in the shared core:

- **File contents.** Exact bytes, checked with a CRC-32 on extraction. A bad
  checksum fails the entry and leaves no output behind. Files are written to a
  temporary name next to the destination and renamed into place only after the
  checksum and close succeed.
- **Directories**, including empty ones, as their own entries.
- **Names** as canonical UTF-8 relative paths, in the order encountered.
- **Modification time** as UTC seconds since 1970, with a flag saying whether the
  time is known. A time before 1970 or one the source cannot represent is
  written as unknown, and a time of exactly zero is a real time, not an absence.
- **A permission-bit field** (a 12-bit Unix mode) where the writer has one.
- **Symbolic links** as an entry type, on the systems that have them.
- **Platform records**: extra metadata entries tagged with the creating system,
  such as Amiga protection bits or Mac Finder info. A reader of the same system
  applies them; any other reader skips them and reports them as unsupported.

These are deliberately outside the format and are never carried on any system:
**owner and group**, **ACLs**, **hard-link identity**, **device nodes**,
**sparse-file layout**, and **timestamps other than modification time**.

## Summary table

| System | File data | Empty dirs | Modification time | Permissions / attributes | Symlinks | Forks / type codes | Other native metadata | Verified |
| ------ | --------- | ---------- | ----------------- | ------------------------ | -------- | ------------------ | --------------------- | -------- |
| Linux CLI | yes | yes | yes (seconds) | mode (set-ID bits dropped) | yes | n/a | extended attributes, POSIX ACLs, hard links | host + QEMU |
| Linux FLTK GUI | yes | yes | yes (seconds) | mode (set-ID bits dropped) | yes | n/a | no (reports partial when reading them) | host |
| Linux mini (32-bit) | yes | yes | yes | mode | not checked | n/a | no | host + QEMU/native 32-bit |
| BSDs, Solaris, illumos, Hurd, AIX 7.3, z/OS | yes | yes | yes | mode | yes | n/a | hard links (extended attributes are Linux-only) | host shim only |
| QNX, HP-UX, IRIX, NeXTSTEP, SCO, Tru64, older AIX | yes, into a new private directory | yes | yes (symlink times reported partial) | mode | yes | n/a | no | host shim only |
| macOS CLI | yes | yes | yes | rwx mode | yes | resource fork, Finder info | extended attributes | host shim only |
| macOS Cocoa GUI | yes | yes | yes | mode (set-ID bits dropped) | no | no | no | build only |
| Windows (CLI, FLTK, Win32, .NET, WCX) | yes | yes | yes | FAT attributes | no | n/a | no | emulator (Wine) |
| DOS (full, DJGPP) | yes | yes | yes (2 s) | FAT attributes | n/a | n/a | no | emulator |
| DOS16, Win16, OS/2-16 mini | yes | yes | yes (2 s) | FAT attributes | n/a | n/a | no | emulator (DOS16) |
| OS/2 | yes | yes | yes (2 s) | FAT/HPFS attributes | n/a | no (HPFS EAs lost) | no | build only |
| CP/M-86 mini | yes | no | no | no | n/a | n/a | no | host shim only |
| Mac OS 9 (PowerPC, 68k) | yes | yes | yes (1904-2040) | Finder flags | n/a | resource fork, type/creator | no | build only |
| Amiga (OS 2.x/3.x CLI, GUI) | yes | yes | yes | protection bits | no | n/a | file comment | emulator (FS-UAE) |
| Atari (TOS CLI, GEM GUI) | yes | yes | yes (2 s) | TOS attributes | n/a | n/a | no | emulator (Hatari) |
| Sinclair QL | yes | yes | yes | no | n/a | no | no | emulator (sQLux host FS) |
| X68000 | yes | yes | files only | FAT attributes | n/a | n/a | no | host shim only |
| RISC OS | yes | yes | yes | RISC OS attributes | n/a | filetype, load/exec | no | host shim only |
| Plan 9 | yes | yes | yes | rwx mode | n/a | n/a | no | host shim (native run historical) |
| Haiku / BeOS | yes | yes | yes | mode | yes | n/a | MIME type only | host shim only |

The QNX row extracts differently from the others: see "Which systems can
extract" below.

## Unix-like systems

**Frontend:** `src/frontend/linux/main.c`, which the BSD, Solaris, illumos, Hurd,
AIX, z/OS, QNX, HP-UX, IRIX, NeXTSTEP, SCO and Tru64 frontends compile as thin
wrappers. Linux itself has the CLI, a 32-bit mini CLI and an FLTK GUI.

**Stored**

- File data, directories, and symbolic links (the link's target text).
- Modification time of files, directories and symlinks in whole seconds.
- The permission bits of each item.
- On Linux, extended attributes and POSIX ACLs: `user.*` and
  `system.posix_acl_access`/`system.posix_acl_default` always; `security.*` and
  `trusted.*` (file capabilities, SELinux labels) only with `--security-attrs`.
  They travel in a native attribute container (`ATTR` in `boltpack l`), at most
  64 per item and 32 KiB per value.
- A record for each file that is another name for an earlier file (a hard link).
  The data is still archived for every name, so a reader that does not know the
  record gets an independent copy.

**Restored**

- File data, directories and symlinks. A symlink whose target is absolute, or
  climbs out of the extraction folder, is refused with a message and counted as
  partial. Links that stay inside the extracted tree are recreated.
- Permission bits. Set-user-ID, set-group-ID and sticky bits are not restored;
  the item gets its ordinary permissions and is reported partial.
- Modification times, including a symlink's own time (set without following the
  link). Directory modes and times are applied after the directory's children,
  so extracting files into a read-only directory works.
- On Linux, extended attributes and ACLs, applied to the file just extracted
  (a read-only file is made writable for an instant to take its attribute, then
  put back). `security.*` and `trusted.*` are refused unless `--security-attrs` is
  given, and even then need the privilege the kernel demands; a refusal or failure
  names the item and attribute and ends with a count.
- Hard links, recreated by replacing the extracted copy with a link to the first
  name. The link is made only to a regular file this run extracted with the same
  size and checksum; anything else (a missing, pre-existing, differing or
  out-of-tree target) is refused with a message and the copy stays.

**Not carried**

- Owner and group. A file extracted by a user belongs to that user.
- On systems other than Linux, extended attributes and ACLs (the wrappers record
  hard links but not attributes).
- The space saving of hard links: every name still carries its data in the
  archive; only the sharing is restored.
- Device nodes, FIFOs and sockets: they are not archived. Each one is named in a
  warning and counted in a summary, and creation still succeeds (like `tar`).
- Sparse layout: holes are archived and restored as ordinary zero bytes.
- Access, change and birth times, and the sub-second part of modification times.

**Names.** Linux names are case-sensitive bytes, so no two source names collide.
A name that is not valid UTF-8 cannot be archived and creation stops with an
error.

**Editing archives.** `add` and `delete` write a new archive beside the old one
and rename it into place, keeping the archive file's owner, group and mode where
the user is permitted to.

**Which systems can extract**

| Wrapper | Real extraction | Notes |
| ------- | --------------- | ----- |
| Linux, FreeBSD, NetBSD, OpenBSD, GNU Hurd, Solaris 11, illumos | yes | Directory-relative, no-follow extraction. |
| AIX | directory-relative when built for AIX 7.3 (`_AIX73`) | Older or unidentified AIX builds use the private-directory rule below. |
| z/OS | z/OS 3.1 (June 2024 refresh) or later | Filename encoding has not been qualified; do not rely on non-ASCII names. |
| QNX, HP-UX, IRIX, NeXTSTEP, SCO, Tru64, older AIX | yes, with a restriction | They have no `openat()` family, so extraction is by pathname and cannot stop another process from swapping a directory mid-extraction. They therefore extract only into a new directory they create themselves (`-o DIR`, which must not exist) private to the user (mode 0700), and open it to its normal permissions afterwards. A failed extraction leaves it private. Symlinks are recreated, but their own modification times cannot be set, so an archive containing symlinks reports partial. |

**Verification.** Linux is tested on the host, in user-mode QEMU for i386 and ARM,
and as a native 32-bit process for the mini build. The other wrappers have only
been built and exercised on a Linux host against shims of their directory-relative
calls; no native run has been recorded. The seven pathname-extraction wrappers are
built on the host in the same pathname mode those systems use, and checked for
create, edit, the refusals and private-directory extraction. Whether the shared
source compiles with those systems' own compilers and headers has not been tried.
64-bit builds default to Fluff compression; the extraction rules above are the same
for every codec.

## macOS

**Frontend:** `src/frontend/macosx/main.c` (CLI) and `src/frontend/macosx_gui`
(Cocoa GUI).

**CLI stores and restores**

| Item | Behavior |
| ---- | -------- |
| File data, directories | as on other Unix-like systems |
| Permissions | the nine rwx bits; set-ID bits are not stored |
| Modification time | whole seconds |
| Symbolic links | stored; absolute or escaping targets are refused on extract |
| **Resource fork** | stored as its own entry, up to 16 MiB per file; a larger fork is skipped with a warning and the file's data is still archived |
| **Finder info** | type, creator and Finder flags (10 bytes), restored as the `com.apple.FinderInfo` attribute |
| **Other extended attributes** | stored in a native attribute container, one per file or folder, up to 64 attributes, 32 KiB per value and 64 KiB per item; restored on macOS |

The container includes quarantine flags, "where from" URLs, tags and comments.
It leaves out attributes that other entries carry (resource fork, Finder info)
and system-managed ones (`com.apple.system.*`, `com.apple.rootless`,
`com.apple.macl`, `com.apple.provenance`); the reader refuses to apply those even
if an archive contains them. Oversized or surplus attributes are reported when
the archive is created, with a count at the end.

**Not carried**

- ACLs, owner and group, hard links, and the creation date (it is not stored and
  is not restored; the new file's creation date is the time of extraction).
- Directory Finder info: only files have a Finder record archived.
- Extended-attribute values above 32 KiB.

**Names.** Names are archived exactly as the filesystem reports them. HFS+
reports decomposed Unicode (an accented letter as a base letter plus a combining
mark), and the same bytes come back on an HFS+ volume, so a same-system round trip
is exact.

**Error reporting.** A store or restore problem names the item, the attribute, the
reason and the system error number, and ends with a summary count. The partial
exit status is used when anything could not be restored. `BOLTPACK_DEBUG=1` traces
every attribute.

**Cocoa GUI.** It preserves permissions and modification times and nothing else:
it does not capture resource forks, Finder info or extended attributes, reports
them as partial when reading an archive that contains them, and does not store
symlinks. Use the CLI when those matter. Extraction in the GUI is refused on
macOS older than 10.10.

**Older systems.** The CLI builds for Mac OS X 10.5 PowerPC using an emulation of
the directory-relative calls (see `src/frontend/posix/legacy_at.h`); Mac OS X
10.3 and earlier have no extended attribute calls and are not supported.

**Verification.** Host shim only: the macOS code has been compiled and run on
Linux with the Apple attribute calls replaced by Linux ones. No run on a Mac has
been recorded. See [hardware-testing.md](hardware-testing.md).

## Windows

**Frontends:** the CLI (`src/frontend/win`), the native Win32 GUI, the FLTK GUI,
the .NET GUI with its DLL, and the Total Commander plug-in. They share the same
rules.

**Stored and restored**

- File data and directories (empty ones included), with full Unicode names handled
  through the wide Windows APIs.
- Modification time as UTC seconds.
- The four DOS-style attributes: read-only, hidden, system and archive, for files
  and directories. Directory attributes are applied after the directory's contents
  so a read-only folder can still be filled.

**Not carried**

- NTFS ACLs and ownership, alternate data streams, compression and encryption
  flags, and short (8.3) names.
- Symbolic links and junctions to directories: they are no longer followed.
  Earlier versions followed them, so a link that pointed back up the tree
  recursed until the path limit and the whole create failed, and a link to a
  folder outside the tree pulled that folder's files into the archive. Each one is
  now named in a warning and counted, and the rest is archived. Other reparse
  points (cloud placeholders, deduplicated files) are ordinary files and are
  archived. A symlink to a file is archived as the file it points to, and an
  archive's symlink entries are skipped on extraction with a message.
- Hard links, and creation and access times.
- Unix permission bits and resource forks in an archive from another system: they
  are reported as partial.

**Names.** Reserved device names (`CON`, `NUL`, ...), names ending in a dot or a
space, and names that differ only in case are refused before extraction starts.
Names created on Windows already obey these rules.

**Editing archives.** `add` and `delete` refuse an archive that is read-only, a
reparse point, or has more than one link, and publish the new archive with
`ReplaceFileW`, which keeps the old archive's ACLs and attributes.

**Verification.** The CLI and the FLTK and Total Commander programs have been run
under Wine, for example extracting a Linux-created archive with a supplementary
Unicode name byte-for-byte. The native Win32 and .NET GUI rows are build or
host-preflight evidence only. Native Windows has not been used. Windows 9x is
unsupported. The 32-bit Windows runtime profile tests have not been run (Wine here
lacks its 32-bit runtime).

## DOS, OS/2 and CP/M-86

### Full DOS (DJGPP)

- **Stored and restored:** file data, directories, modification time at the
  two-second resolution of FAT, and the read-only, hidden, system and archive
  attributes (on DJGPP builds). Directory attributes are applied after the
  directory's contents.
- **Time zone:** DOS stores local time. The 16-bit mini builds and OS/2 take the
  zone from `BP_TZ_OFFSET` (minutes east of UTC) and report times as unknown
  when it is missing. How the full DOS build chooses its zone was not checked
  for this document.
- **Directory times** are not supported on plain DOS (reported partial).
- **Names:** names go through code page 850. Without long file name support the
  names must fit 8.3; a name that would need a generated short alias is refused
  before extraction. With long file names (DJGPP checks the target volume)
  longer names work.
- **Not carried:** anything beyond the above (no ownership, links or forks on DOS).
- **Verification:** DOSBox-X with FAT12 (CP437 and CP850 names, long names,
  attributes). A real DOS machine has not been used.

### DOS16, Win16 and OS/2-16 mini

- **Stored and restored:** file data, directories, two-second modification times
  (a configured time zone offset, `BP_TZ_OFFSET`, is required; a missing one is
  reported) and the FAT attribute envelope.
- **Codecs:** Stored and BoltLite only; 32 KiB blocks.
- **Limits:** at most 128 distinct directory prefixes and 4 KiB of name-index
  storage per extraction; DOS16 needs 8.3 names. Mini builds have a 65,535-entry
  and about 2 GiB archive ceiling.
- **Verification:** DOS16 under DOSBox with FAT12 (creation, extraction, CP437 names,
  attributes, a 33-level tree). Win16 and OS/2-16 are build-only.

### OS/2

- **Stored and restored:** data, directories, FAT/HPFS attributes (and, on append,
  the OS/2 attribute flags and creation and access times of the archive), and
  two-second modification times with a fixed time zone offset.
- **Not carried:** HPFS extended attributes such as `.TYPE` and `.LONGNAME`.
- **Limit:** 32-bit offsets, so archives must stay under 2 GiB.
- **Verification:** build only (Watcom cross-build plus a host model of the
  publication logic). No OS/2 run.

### CP/M-86 mini

- **Stored and restored:** file data only. Names must already be flat, uppercase
  8.3 names; extraction refuses directories, non-canonical names and existing
  files.
- **Not carried:** times, attributes, directories. Portable metadata in an archive
  is reported as partial.
- **Verification:** host shim only; no CP/M-86 runtime.

## Classic Mac OS

**Frontends:** the PowerPC GUI (`src/frontend/macos9/main.c`, Mac OS 9) and the
68k GUI source kit.

- **Stored and restored:** the data fork, directories, the **resource fork** (as its
  own entry when it is not empty), and the **Finder type, creator and flags** as a
  10-byte record. The flags are the Finder's own flag word (for example the
  invisible and label bits).
- **Modification times** are converted between the archive's Unix epoch and the
  Classic Mac 32-bit date range (1904 to 2040); a time outside that range is
  reported partial. Directory times are applied after the directory's contents.
- **Names** are converted between MacRoman and UTF-8 with a strict mapping and
  must be at most 31 characters. A name that cannot be mapped is refused or
  reported.
- **Not carried:** Finder comments (kept in the Desktop Database), icon positions,
  folder window and view settings (no code archives Finder info for directories),
  creation dates (only the modification time is archived), and Unix permissions
  and symbolic links (Mac OS 9 has neither; such entries in an archive are
  reported partial).
- **Interoperability note:** archives from the macOS CLI now use the same 10-byte
  Finder record, so type and creator codes round-trip between the two. Archives
  from macOS carry a Unix permission record for every item that Mac OS 9 cannot
  apply, so Mac OS 9 reports them as partial even when nothing visible is lost.
- **Verification:** build only. The PowerPC frontend is compiled and linked with
  Retro68 to XCOFF, which is not a runnable application; the 68k kit builds
  MacBinary applications and disk images. No runtime round trip on Mac OS 9 has
  been recorded.

## Amiga

**Frontends:** the CLI (`src/frontend/amiga`, AmigaOS 2.x/3.x 68k) and the GadTools
GUI. AmigaOS 4, MorphOS and AROS share the CLI source but have not been built here.

- **Stored and restored:** file data; directories, including empty ones; the
  AmigaDOS **protection bits** (the raw `fib_Protection` word) and the **file
  comment** (up to 79 Latin-1 characters) for files and directories; modification
  time. Directory time and protection are applied after the directory's children.
- **Names:** converted through Latin-1; names containing an AmigaDOS volume
  separator (a colon) are refused.
- **Time zone:** AmigaDOS stores local time. Set the `BP_TZ_OFFSET` variable to
  your offset in minutes east of UTC before creating or extracting.
- **Not carried:** soft and hard links (reported partial) and metadata from
  other systems. An icon (`.info`) file is an ordinary file, so it travels when it
  is included in the archive.
- **Verification:** FS-UAE with Workbench 2.04, on host-directory, OFS and FFS
  volumes, including depth tests, protection and comment restoration, and
  process-kill recovery. The GUI is build-only except for the host and emulator
  checks recorded in the ledger. Real hardware: not tested.

## Atari

**Frontends:** the TOS CLI (`src/frontend/atari`) and the GEM GUI.

- **Stored and restored:** file data, directories, TOS attributes (read-only,
  hidden, system and archive; the volume-label and directory bits are never
  stored), and two-second modification times with a time zone from `BP_TZ_OFFSET`.
- **Names:** 8.3 components. Atari ST accented letters are accepted and stored as
  exact bytes (checked on TOS 1.06, 1.62 and 2.05 in Hatari, including a round
  trip made on TOS); spaces, control characters, `: / \ ? *` and a leading byte
  0xE5 are refused, and long names (FreeMiNT) do not fit 8.3. EmuTOS and FreeMiNT
  have not been checked and may fold accented letters.
- **Limits:** GEMDOS paths are limited to 125 bytes.
- **TOS version differences:** the hidden and system flags on **directories** are
  not applied by TOS 1.06, 1.62 or 2.05 (reported partial); directory times are
  restored at FAT12 resolution.
- **Verification:** Hatari with TOS 1.06, 1.62 and 2.05 on host-drive and FAT12
  volumes, including crash recovery of the extraction journal. No real Atari.

## Other systems

### Sinclair QL

- **Stored and restored:** file data, directories, modification time (QDOS seconds
  since 1961, local time; set the offset with `--tz-offset MINUTES`; no daylight
  saving logic). Directory times are restored deepest-first where the system
  allows.
- **Not carried:** the QDOS protection bits, and the file type and data-space
  header of executable files (the frontend reads the type only to recognise
  directories). An executable that needs its data-space header should be checked
  after a round trip.
- **Verification:** sQLux with a host-directory volume, including recursive
  enumeration and exact times. Physical QDOS filing systems have not been tested.

### X68000 (Human68k)

- **Stored and restored:** file data, directories, FAT attributes for files and
  directories, and file modification time (DOS packed local time, two-second
  resolution; `BP_TZ_OFFSET` supplies the zone). Names are Shift-JIS.
- **Not carried:** directory dates, and anything beyond the FAT attributes.
- **Verification:** host shim only, including a Shift-JIS name with a `0x5C` trail
  byte. No X68000 or emulator run.

### RISC OS

- **Stored and restored:** file data, directories, and a 12-byte native record with
  the **load address, execution address and attributes**, which holds the RISC OS
  **filetype** and 5-byte centisecond datestamp. Names use Latin-1 with `.` as the
  directory separator. A time before the Unix epoch is stored as unknown.
- **Not carried:** nothing from other systems; symbolic-link entries in an archive
  are skipped with a message (RISC OS has no links). Resource-fork entries are
  refused.
- **Verification:** 32-bit host shims of the system calls. No RISC OS run.

### Plan 9

- **Stored and restored:** file data, directories, permission modes (rwx) and
  representable modification times. Extraction goes into a new private root.
- **Not carried:** owner and group names, and anything native other than the mode.
- **Verification:** host shims, plus a native 9front run recorded earlier that
  cannot be reproduced now because no emulator is available.

### Haiku / BeOS

- **Stored and restored:** file data, directories, symbolic links, the Unix mode,
  and the BFS **MIME type** (`BEOS:TYPE`). Modification times are whole seconds.
- **Not carried:** BFS attributes other than the MIME type (for example icons and
  custom attributes).
- **Verification:** host shim, with the Haiku attribute calls backed by Linux
  extended attributes. No Haiku run is recorded; native Haiku qualification is
  deferred.

## Things that are easy to miss

1. **Special files are not archived, but are now reported.** FIFOs, sockets and
   device nodes (and on Windows links, junctions and special files) get a warning
   per item and a summary count; creation still exits 0, as `tar` does, so a
   script that must not lose them has to read stderr.
2. **Hard links keep their data twice in the archive.** The link itself is
   recreated on a reader that knows the record, but the archive is as large as
   if each name were a separate file.
3. **Ownership is never restored.** Files extracted as root are still owned by
   whoever extracts them; there is no mode that records owners.
4. **Fork and attribute loss happens on the way out of a system.** If an archive
   passes through a machine that cannot hold a Mac resource fork or an extended
   attribute and is extracted and re-archived there, that data is gone. Copying the
   archive file itself, or editing it with `add` and `delete`, keeps every record.
5. **Time zones matter on systems that store local time** (DOS, OS/2, Amiga,
   Atari, QL, X68000). Set the offset on both machines or times shift.
6. **Partial exit status is routine on some pairs.** Mac OS 9 reports partial for
   archives from macOS because of the Unix permission record it cannot apply; DOS
   reports partial for directory times. Read the messages before treating it as a
   failure.
7. **Verification is thin.** No system has a recorded native round trip, so every
   statement above is what the code and the host or emulator tests establish. The
   table in [hardware-testing.md](hardware-testing.md) tracks the first real
   machines.

## Gaps worth fixing, in priority order

Chosen by three tests: data is really lost, the loss is silent, and a fix can be
verified without the target system. Status as of 2026-11-12.

| # | Gap | Status |
| - | --- | ------ |
| 1 | Skipped items not reported (Unix special files; Windows links, junctions, special files) | **Done.** Linux names each skipped FIFO, socket or device and counts them. The Windows CLI turned out to *follow* directory links, which made a looping link fail the whole create and a link to an outside folder pull its files in; it now reports and skips them. Verified on a Linux host and, for Windows, under Wine (`make linux-native-attrs-test`, `make win-links-test`). |
| 2 | Linux extended attributes, POSIX ACLs, file capabilities | **Done** for `user.*` and ACLs (verified on tmpfs, including a read-only file and a directory's default ACL). `security.*`/`trusted.*` are carried and applied only with `--security-attrs`; that gate is verified, but actually restoring a capability needs root and was not run. |
| 3 | Hard links duplicated | **Done**, with the limit that the archive still holds every name's data. Recreation is refused unless the target is a file this run extracted with the same size and checksum, so a hostile record cannot join unrelated or pre-existing files. Verified including readers that do not know the record. |
| 4 | QDOS file type and data-space header not stored (QL) | **Not done.** It needs the QL toolchain and sQLux, which are not in this environment, and whether sQLux's host-file mode exposes the header was never established. No code was written rather than code that could not be run. |
| 5 | Native attributes on Haiku, OS/2 and Windows | **Not done.** None of the three runs natively here, so the code could only be checked against stand-ins that cannot show how the real system behaves. The container and the Linux code are the template when one of them is available. |

Also worth doing when cheap:

- **Cocoa GUI parity with the macOS CLI.** The GUI stores no resource forks,
  Finder info or extended attributes and does not tell you. Using the CLI's code
  would fix that; it cannot be built or run here.
- **Full DOS time zone.** How the full DOS build picks its zone was not checked
  for this document; confirm it before relying on DOS times.
- **FreeMiNT and EmuTOS name behavior** on Atari: the accented-letter rule is only
  verified on TOS 1.06, 1.62 and 2.05.

Left alone on purpose:

- **CP/M-86** carries file data only because the system offers nothing else.
- **Mac OS 9 reports partial for archives from macOS** because the Unix permission
  record cannot be applied there; that is accurate.
- **Atari long names** (FreeMiNT) until someone needs them; the base rule is 8.3.

To see exactly what an archive carries, list it:
`boltpack l archive` shows one line per entry, with a Type column of `DATA`,
`DIR`, `LINK`, `RSRC` (resource fork), `META` (permissions, Finder info or other
platform metadata) and `ATTR` (extended attributes and hard-link records, from
macOS or Linux).
