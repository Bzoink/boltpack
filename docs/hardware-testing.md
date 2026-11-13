# Hardware testing

**Created:** 2026-11-11

This guide is for testing BoltPack on real machines and reporting what
happened. The automated tests in `make test` run on a Linux host with shims and
emulators; they cannot show how a real filesystem, SDK or operating system
behaves. The [interoperability matrix](interoperability-matrix.md) records which
frontends have that kind of evidence and which do not. A run described here
turns "unverified" into a result, or into a bug report that can be fixed.

Each system has its own section. Add a section for another system with the
template at the end.

## Status

Nothing in this guide has been run on hardware yet. Every expected result below
is what the code was written and host-tested to do, not something observed on
the machine. When a result differs, the difference is the useful finding.

| System | Section | Last result |
| ------ | ------- | ----------- |
| PowerPC Mac OS X 10.5 (G4) | [Mac OS X 10.5](#powerpc-mac-os-x-105) | not run |
| Mac OS 9 (PowerPC) | [Mac OS 9](#mac-os-9-powerpc) | not run |
| QNX, HP-UX, IRIX, NeXTSTEP, SCO, Tru64, AIX < 7.3 | [Unix systems without `openat()`](#unix-systems-without-openat-qnx-hp-ux-irix-nextstep-sco-tru64-aix-before-73) | not run |

## Qualification and evidence

The [interoperability matrix](interoperability-matrix.md) records capabilities
and per-frontend evidence. The
[requirement ledger](interoperability-requirement-ledger.md) maps that evidence
to format, lifecycle, name, metadata, and Profile 1 acceptance. Passing a host
shim, cross-build, or emulator fixture does not qualify an untested native OS,
filesystem, GUI, or CPU variant.

For each target, verify create, list, test, extract, and every supported archive
edit. Transfer archives in both directions with a full reader and a constrained
reader where applicable. Compare payload bytes independently, including empty
files and directories. Check exact native name projection, case and prefix
collisions, unrepresentable names, codec/block limits, unknown versus epoch-zero
times, deferred directory attributes, and native/portable metadata precedence
in both record orders. Unsupported preservation must be reported explicitly.

Failure cases include corrupt data, failed writes/closes, cancellation,
publication failure, existing read-only targets, and interrupted replacement.
Existing files and archives must survive handled failures. Recovery tests must
check both restored bytes and leftover stages, backups, or journals. A process
kill in an emulator does not establish power-loss durability on hardware.

Important remaining target checks include:

| Target | Qualification still needed |
| --- | --- |
| Windows CLI, GUIs, DLL and WCX | Native Windows filesystem and reparse-point behavior, sharing and publication semantics, GUI cancellation and metadata restoration. Wine evidence is limited to Wine. |
| Win16 and OS/2 | Native filesystem operations, attribute restoration, recovery, and runtime heap/stack exhaustion. DOS16 measurements do not qualify these runtimes; see [resource accounting](16-bit-resource-accounting.md). |
| Atari GEM GUI | Safe cleanup of incomplete stages after restart or parent substitution, the TOS 1.62 device-error prompt, and a working FreeMiNT GUI launch. EmuTOS FAT12 device-error retry and TOS interrupted-backup recovery have separate emulator evidence. |
| Amiga GUI and OS4/MorphOS/AROS variants | GUI behavior on native OFS/FFS, timestamp zone/DST policy, replacement failures, cancellation, and separate build/runtime checks for each shipped variant. CLI results do not qualify GUI callbacks. |
| Classic Mac OS | Complete nested 68k extraction/publication and executable/resource-fork round trips; runnable PowerPC packaging and target execution. A PowerPC XCOFF link alone is insufficient. |
| QDOS, RISC OS, Human68k and CP/M-86 | Real filesystem name/metadata handling, edits, recovery and constrained-resource failures. sQLux hostFS header/enumeration checks and host shims do not establish all native filesystem behavior. |
| POSIX wrappers | Native compiler/API availability, encoding, timestamps, private-root or directory-relative extraction, and archive edits on each supported OS version. |
| Plan 9 and Haiku/BeOS | Native qualification remains open. Preserve host-shim evidence and label older guest runs by their tested revision; deferred native work is not a passing result. |

Reusable input manifests are in
[`test/reference/interoperability/`](../test/reference/interoperability/).
For example, `atari-amiga-fsuae.json` defines bidirectional interchange, and
`amiga-native-recovery.json` defines interrupted-publication expectations.
Use independent expected payloads and metadata rather than having a reader
validate only its own writer's output. The
[usable round-trip gate](usable-roundtrip.md) additionally runs restored
applications to check that essential metadata makes them usable.

Retain the exact command, source revision and working-tree diff, compiler and
runtime versions, expected input manifest, executable/ROM/image hashes, and
observed output hashes. Record passes, expected partial/refused operations,
defects, and unavailable environments separately. Links into ignored `build/`
directories refer to local evidence; they are not artifacts shipped in a fresh
checkout. Versioned evidence summaries live in [`docs/evidence/`](evidence/).
Rerun affected checks when behavior changes; documentation cleanup alone does
not create new runtime evidence.

## How to report

A report is most useful when it lets the problem be reproduced without a second
round of questions. Send, for every test that did not behave as expected:

1. **The test number and the exact command.** Copy it from your terminal.
2. **The complete output, both streams.** Run commands as
   `command 2>&1 | tee test-NAME.txt` so that nothing is lost and the output
   can be attached as a file.
3. **The exit status.** Run `echo $?` immediately after the command.
4. **The machine.** Run these once and send them with your first report:

   ```sh
   sw_vers; uname -a; gcc --version | head -1
   ```

5. **The archive.** If a test used an archive that behaved strangely, send the
   `.bp` file, or the output of `build/macosx-ppc/boltpack l archive.bp` if it
   is large. Archives contain your file data, so send only test files.
6. **What you expected and what you saw,** in a sentence each, even if it
   seems obvious.

For a problem inside an archive operation, run the command again with
`BOLTPACK_DEBUG=1` in front of it and send that output too. It prints one line
for every extended attribute that is stored, skipped or restored:

```sh
BOLTPACK_DEBUG=1 build/macosx-ppc/boltpack c test.bp folder 2>&1 | tee test-debug.txt
```

A pass is also worth reporting. A one-line "T3 passed on 10.5.8" is evidence
that goes into the matrix.

### Exit status and messages

| Result | Meaning |
| ------ | ------- |
| exit 0, no `boltpack:` messages | Everything requested was done. |
| exit 0 with `warning:` lines | The archive was written, but something could not be stored. The text names the item and the reason. |
| exit 1, `extraction completed with partial preservation (N items affected)` | The file data was extracted, but N items lost metadata that could not be applied. The lines above it say which and why. |
| exit 1, any other `boltpack:` message | The operation failed. The message is the reason. |

Messages about an attribute, resource fork or Finder info name the item, the
attribute, the reason, and where it came from the system error number
(`errno`). The end of a run prints a summary such as
`native attributes: 4 stored on 3 items` or `native attributes: 3 restored`.
`boltpack l archive.bp` lists every entry; its Type column is `DATA`, `DIR`,
`RSRC` (resource fork), `META` (Unix mode, Finder info or other platform
metadata), `LINK` or `ATTR` (extended attributes).

## PowerPC Mac OS X 10.5

**Target:** a G4 (or G5) running Mac OS X 10.5 Leopard. Extended attributes
need 10.4 or later. Mac OS X 10.3 and earlier cannot be tested with this build.

**What this tests:** that the command-line tool builds and runs on a system
that lacks the `openat()` family, and that file data, resource forks, Finder
info and extended attributes survive a create and extract round trip.

### Build

You need Xcode 3.x (the Developer Tools) for `gcc`. Copy
`dist/boltpack-1.11-source-macosx-ppc.tar.gz` to the Mac, then:

```sh
tar xzf boltpack-1.11-source-macosx-ppc.tar.gz
cd boltpack-1.11-source-macosx-ppc
sh scripts/macos/build-native.sh ppc 2>&1 | tee build.txt
```

The tool is built as `build/macosx-ppc/boltpack`. The build enables the
emulation of the `openat()` family (`-DBP_FORCE_LEGACY_AT`) itself. If you set
`CFLAGS` yourself, include the flags the script's default uses
(`-ansi -pedantic -Wall -Wextra -O2`).

**T0 – build.** Expected: the last line is `Built: build/macosx-ppc/boltpack`
and `build/macosx-ppc/boltpack` exists. Warnings are fine. Report the whole
`build.txt` for any error, especially one mentioning `lutimes`, `futimes`,
`fchdir`, `listxattr`, or an undefined symbol or implicit declaration, since
those are the system calls this port assumes the system provides.

Check that it runs: `build/macosx-ppc/boltpack help`.

Make a short name for the commands below:

```sh
B="$PWD/build/macosx-ppc/boltpack"
```

### Prepare a test folder

Developer Tools provide `SetFile` and `GetFileInfo` in `/Developer/Tools`.

```sh
mkdir -p ~/bptest/src/sub && cd ~/bptest
echo "plain data" > src/plain.txt
echo "document data" > src/doc.txt
echo "nested data" > src/sub/nested.txt

# Resource fork (HFS+ only; use a volume that is HFS+, not FAT or NFS)
echo "pretend resource fork" > src/doc.txt/..namedfork/rsrc   # 22 bytes

# Finder type and creator
/Developer/Tools/SetFile -t TEXT -c ttxt src/doc.txt

# Extended attributes
xattr -w com.example.test "hello" src/plain.txt
xattr -w com.example.other "second value" src/plain.txt
xattr -w com.example.dir "directory attribute" src/sub

# A symbolic link
ln -s plain.txt src/link
```

Record what is there, so it can be compared later:

```sh
/Developer/Tools/GetFileInfo src/doc.txt
ls -l src/doc.txt/..namedfork/rsrc
xattr -l src/plain.txt src/sub
```

### Tests

**T1 – create and list.**

```sh
$B c test.bp src 2>&1 | tee test-t1.txt
$B l test.bp
$B t test.bp
```

Expected: exit 0 and no warnings. The create output ends with a line like
`native attributes: 3 stored on 2 items` (the two attributes on `plain.txt`
and the one on `sub`). The list shows, for `src/doc.txt`, a `DATA`, an `RSRC`
and a `META` line of size 10 (the Finder record) as well as a 2-byte `META`
(Unix mode), and `ATTR` lines for `src/plain.txt` and `src/sub/`. `t` reports
every entry OK. If the attribute counts differ, the files carry attributes the
test did not set (for example `com.apple.TextEncoding`); check them with
`xattr -l` and report the counts you see.

**T2 – extract into a clean place.**

```sh
mkdir out && $B x -o out test.bp 2>&1 | tee test-t2.txt
echo $?
```

Expected: exit 0; a `native attributes: 3 restored` line; no warnings.

**T3 – compare.**

```sh
cmp src/plain.txt out/src/plain.txt && cmp src/doc.txt out/src/doc.txt && cmp src/sub/nested.txt out/src/sub/nested.txt && echo data-ok
/Developer/Tools/GetFileInfo out/src/doc.txt
ls -l out/src/doc.txt/..namedfork/rsrc
cat out/src/doc.txt/..namedfork/rsrc
xattr -l out/src/plain.txt out/src/sub
ls -l out/src/link
```

Expected: `data-ok`; the same type (`TEXT`) and creator (`ttxt`) as before;
the resource fork has the same size and content; the three attributes are
present with the same values; `link` points to `plain.txt`.

**T4 – round trip after deleting the original.** Delete the `src` folder, extract
the archive again, and repeat T3's comparisons. This guards against a result
that only looks right because the original was still there.

**T5 – error reporting.** Check that a failure is reported rather than hidden.

First, extract somewhere that cannot be written:

```sh
$B x -o /System/nope test.bp 2>&1 | tee test-t5a.txt; echo $?
```

Expected: exit 1 and a message saying why (the output directory cannot be
created or written). It must not crash and must not leave files behind.

Second, an observation rather than a prediction: extract onto a FAT32 USB stick
or a disk image made with `hdiutil create -size 10m -fs MS-DOS fat.dmg`
(mount it, then extract into it):

```sh
$B x -o /Volumes/NAME test.bp 2>&1 | tee test-t5b.txt; echo $?
```

Mac OS X may keep extended attributes on such a volume in hidden `._` files, or
refuse them. Both are acceptable; what matters is that nothing is dropped
silently. If attributes or the resource fork cannot be stored, expect a message
for each item naming the reason, a summary of how many could not be restored,
and exit status 1. If they are stored, check them with `xattr -l`. Report which
happened, with the output.

**T6 – edit an archive.**

```sh
echo more > extra.txt
$B a test.bp extra.txt
$B l test.bp
$B d test.bp src/plain.txt
$B l test.bp
```

Expected: the `ATTR` entry for `src/plain.txt` disappears together with that
file, and the other `ATTR`, `RSRC` and `META` entries are still there.

### Moving archives to and from Mac OS 9

See the [Mac OS 9 section](#mac-os-9-powerpc). The `.bolt` extension is not
enough there: the Mac OS 9 program identifies an archive by its Finder type
`BOLT`. Set it on the Mac OS X side before copying:

```sh
/Developer/Tools/SetFile -t BOLT -c BoPk test.bp
```

(`BoPk` is the creator code the classic Mac programs use.) If the transfer method
drops type codes (FAT disks, most network shares, email), set the type again on
the Mac OS 9 machine with a file-type utility, or use a transfer method that
keeps HFS metadata (an HFS or HFS+ disk, disk image, or Disk Copy image).

## Mac OS 9 (PowerPC)

**Target:** a PowerPC Mac running Mac OS 9 with the BoltPack Mac OS 9 program.
This program was not changed or rebuilt for this round of testing; the Mac OS X
tool was changed to match it.

**What this tests:** that archives made on Mac OS X unpack on Mac OS 9 with
resource forks and Finder type and creator intact, and that archives made on
Mac OS 9 unpack on Mac OS X the same way.

The Mac OS 9 program is a drag-and-drop application: drop a `.bolt` archive on
it (or its window) to extract it, drop files or a folder to create one. By
default it extracts into the folder holding the archive. Keep file names to
plain ASCII letters and digits, at most 31 characters, while testing: the Mac
OS X filesystem stores accented names in a decomposed form that the Mac OS 9
program may not be able to represent in MacRoman, and it replaces or refuses
characters it cannot map.

**M1 – Mac OS X archive, Mac OS 9 extraction.** Use `test.bp` from the Mac OS X
tests above, with type `BOLT` set, and copy it to the Mac OS 9 machine. Drop it
on the program.

Expected: the files are extracted. The program may report that some items were
not fully restored. For an archive from Mac OS X that is expected, because the
archive carries a Unix permission record and extended attributes that Mac OS 9
cannot apply; it should not report the Finder type, creator or the resource
fork as lost. Report the exact wording of any alert.

Then check:

- `doc.txt` has type `TEXT` and creator `ttxt` (a type and creator editor, or
  ResEdit's Get File/Folder Info). The creator being right is what proves the
  Finder record was understood.
- `doc.txt` has a resource fork of 22 bytes (ResEdit, or Get Info shows the
  size of each fork on some versions).
- `sub` and `sub/nested.txt` exist; `plain.txt` has the right data.
- The symbolic link `link` is expected to be missing or reported; Mac OS 9 has
  no symbolic links.

**M2 – Mac OS 9 archive, Mac OS X extraction.** On the Mac OS 9 machine, make a
small folder: one text file with a resource fork and a non-default type and
creator, one plain file. Drop the folder on the program to create `Name.bolt`.
Copy it to the Mac OS X machine (any transfer method works here, the archive
data is a plain file) and extract it:

```sh
$B l name.bolt
$B x -o out2 name.bolt 2>&1 | tee test-m2.txt; echo $?
/Developer/Tools/GetFileInfo out2/FolderName/file
ls -l out2/FolderName/file/..namedfork/rsrc
```

Expected: exit 0. The type and creator and the size and content of the
resource fork match what the Mac OS 9 machine had. Before this round of
changes this direction would also have lost the Finder record, so a failure
here is informative either way.

**M3 – names.** Repeat M1 with one file named with a space and a long name of
exactly 31 characters, and one of 40 characters. The 40-character name cannot
exist on Mac OS 9; report what the program does with it.

## Unix systems without `openat()` (QNX, HP-UX, IRIX, NeXTSTEP, SCO, Tru64, AIX before 7.3)

**Target:** the real system, built natively. These wrappers extract by pathname,
so `extract` only writes into a new directory it creates itself, private to the
user (mode 0700), and opens it to its normal permissions afterwards. Whether the
shared source compiles at all with each system's own compiler and headers has
not been tried: that is the first thing to find out.

**What this tests:** that the tool builds, that archives round-trip, and that the
private-directory rule behaves.

### Build

```sh
make FRONTEND=qnx    # or hpux, irix, nextstep, sco, tru64, aix
```

Use the wrapper's native compiler (`make CC=cc ...` if the default is wrong) and
send the whole output of a failing build. Likely problem spots: a missing header
(`<sys/statvfs.h>` is used for a free-space check; the NeXTSTEP wrapper skips it
with `BP_NO_STATVFS`, and any other system lacking it can add `-DBP_NO_STATVFS`),
a missing function the compiler reports as undeclared, or an old compiler
rejecting something. The tool is built
as `build/<frontend>/boltpack`.

**T0 – build and run.** Expected: it builds, and `boltpack help` prints usage.

### Tests

Make a small tree: a few files (one empty, one over 64 KiB), a nested directory,
an empty directory, a file with the execute bit, and one relative symlink.

**T1 – create, list, test.** `boltpack c t.bp tree`, `boltpack l t.bp`,
`boltpack t t.bp`. Expected: exit 0, every entry listed, every entry OK.

**T2 – the refusals.** Each must fail with a message and write nothing:

```sh
boltpack x t.bp                    # no -o: asks for -o DIR
mkdir existing; boltpack x -o existing t.bp   # existing directory: refused
boltpack x -o nope/deeper t.bp     # parent missing: refused, nothing created
```

**T3 – extract into a new directory.** `boltpack x -o out t.bp`, then
`ls -ld out`. Expected: the data matches the original (`cmp` each file), the
execute bit is kept, file and directory times match, `out` has the normal
permissions for your umask (for example `drwxr-xr-x` with umask 022), and the
symlink exists. An archive with a symlink is reported as partial because a
symlink's own time cannot be set; that is expected.

**T4 – a failed extraction stays private.** Corrupt one byte in the middle of an
archive made with `boltpack c -0`, extract it, and check that it fails with a
checksum message, that the message says the directory is left private, and that
`ls -ld` shows `drwx------`.

**T5 – edits.** `boltpack a t.bp newfile`, `boltpack d t.bp name`, `boltpack l t.bp`.

### Known assumptions

| Assumption | Where | How a failure would look |
| ---------- | ----- | ------------------------ |
| The source compiles with the native compiler and headers | `src/frontend/linux/main.c` | Build error |
| `mkdir`, `chmod`, `lstat`, `umask`, `geteuid` behave as in POSIX.1 | `private_root_create` | T2 or T3 messages saying the directory cannot be made private |
| The filesystem honors mode 0700 for other users | `private_root_create` | Not detectable from a single-user test; relevant on shared machines |
| `fopen`/`fseeko` handle archives over 2 GiB | shared | Large-archive failures only |

## Known assumptions to confirm

These are the places where the code relies on something the host tests cannot
check. A report that confirms or refutes any of them is valuable even if
nothing else fails.

| Assumption | Where | How a failure would look |
| ---------- | ----- | ------------------------ |
| Mac OS X 10.5 provides `fchdir()`, `lutimes()`, `futimes()` | `src/frontend/posix/legacy_at.h` | Build error: undefined symbol or implicit declaration |
| gcc on 10.5 defines the deployment-target macro, or the build script's flag forces the emulation | `src/frontend/macosx/main.c` | Build error about `openat`; the build script should prevent it |
| `listxattr`/`getxattr`/`fsetxattr` on 10.5 behave as on later systems, including listing `com.apple.ResourceFork` and `com.apple.FinderInfo` | `add_native_attrs_entry` | Resource fork or Finder info missing from the list in T1 |
| HFS+ on 10.5 accepts the attribute names and values in the tests | `nattr_apply` | `cannot set attribute ... (errno N)` in T2 |
| The Mac OS 9 program reads the 10-byte Finder record | `src/frontend/macos9/main.c` | M1: type or creator wrong or alert about metadata |
| Mac OS 9 treats the `BP_OS_MACOSX` records as ignorable | `src/frontend/macos9/main.c` | M1: extraction stops or fails instead of reporting partial |

## Adding another system

Copy this section, replace the placeholders, and add a row to the Status table.

```markdown
## <System name and version>

**Target:** <hardware and OS versions; what is not covered>

**What this tests:** <one or two sentences>

### Build / install

<exact commands; the expected last line; what to report on failure>

### Tests

**T1 – <name>.** <commands>. Expected: <exit status, output, files>.

### Known assumptions

<assumptions this system could refute>
```

Keep expected results to what the code and host tests actually produce, and
state outright what has not been run, so that a result from the hardware can be
read as evidence rather than as a surprise.
