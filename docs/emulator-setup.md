# Sinclair QL, Atari, Amiga, Plan 9, Haiku, and Windows emulator setup

This is the working setup observed on Tony's Debian machine on 2026-10-28.
Paths are local to this machine; other installations should provide their own
images and use the corresponding paths. Keep ROMs and system disks outside the
repository and run tests against disposable working copies.

## Sinclair QL: XTC68/C68 and sQLux

An XTC68/C68 SDK was installed under the ignored `build/ql-sdk/` directory on
2026-10-29. The source is [XTC68](https://github.com/stronnag/xtc68); its
`sdk-install.sh` setup requires the C68 runtime package. With the SDK's `bin`
directory on `PATH`, build the target with:

```sh
make ql QL_CC="$PWD/build/ql-sdk/prefix/bin/qcc"
```

The result is `build/ql/boltpack`, a QDOS executable. The make target checks
for linker diagnostics reporting unresolved symbols. The QDOS headers assume
case-insensitive include paths (`SYS/types.h` versus `sys/types.h`); on this
Linux host the local ignored SDK tree needs a `SYS/types.h` symlink to
`../sys/types.h`.

sQLux is built under ignored `build/ql-sdk/` from
[`SinclairQL/sQLux`](https://github.com/SinclairQL/sQLux), using its bundled
`Minerva_1.98a1.bin` ROM. The hostFS directory
`build/ql/shared/` is mounted as `WIN1` with `qdos-like`. Run with at least
`--ramtop 1024`; the default 128 KiB does not leave enough room for Boltpack.
From the repository root, a QDOS archive can be created with:

```sh
xvfb-run -a build/ql-sdk/sqlux-build3/sqlux \
  --romdir "$PWD/build/ql-sdk/source/sqlux/roms" \
  --sysrom Minerva_1.98a1.bin --ramtop 1024 \
  --device "WIN1,$PWD/build/ql/shared,qdos-like" \
  --boot_cmd 'EXEC_W win1_boltpack;"c -0 win1_test win1_input"'
```

On 2026-10-29, the runtime test below passed in sQLux. It runs the QDOS binary,
creates a QDOS archive readable by Linux, extracts Linux-created files back to
QDOS, exercises `-o`, checks nested directories and preserves an occupied
stage name. Linux full and mini readers verified the QDOS-created archive.

```sh
make CC=gcc QL_CC="$PWD/build/ql-sdk/prefix/bin/qcc" \
  QL_SQLUX="$PWD/build/ql-sdk/sqlux-build3/sqlux" \
  QL_ROM="$PWD/build/ql-sdk/source/sqlux/roms/Minerva_1.98a1.bin" \
  ql-sqlux-interoperability-test
```

The runtime check found and fixed the QDOS path bug: archive member `/`
separators must become QDOS `_` separators, and output names inherit the
archive's device prefix when no `-o` is supplied. Previously stage opens used
bare names and retried on `ENOENT`; the stage search is now bounded to 4,096
candidates. The emulator uses hostFS `qdos-like`, so this qualifies that
emulator backend, not physical QL filing systems. `FS.DATE` file and directory
timestamp restoration passes for nested Linux-created archives with independent
expected UTC seconds. The runner sets `TZ=UTC`: QDOS stores seconds since 1961
but has no timezone field. Unsupported date calls or values beyond the signed
QDOS range report partial. Host fault injection verifies ordinary replacement
rollback and preservation of both recovery artifacts if rollback also fails.
Native rollback, crash durability, QDOS protection bits, and custom device
names remain unqualified.

A separate recursive-source probe on this same hostFS backend remains
unqualified: opening a host directory through QDOS and reading the expected
64-byte qdirect record returned a record with `d_szname == 0`, and the archive
writer left an incomplete archive after emitting the directory entry. This
backend therefore cannot currently provide independent target evidence for
recursive QDOS source enumeration or qdirect file timestamp capture. The probe
does not establish whether the limitation is in sQLux hostFS or the frontend;
use a QDOS image/filer with working directory channels before making that
claim.

## Haiku R1/beta6 x86_64 live image under QEMU

The official [Haiku R1/beta6 download directory](https://haiku.osuosl.org/r1beta6/)
provides the x86_64 anyboot ISO and its SHA-256 file. The locally downloaded
image is kept outside the repository at
`~/images/Haiku/haiku-r1beta6-x86_64-anyboot.iso`; its published and computed
SHA-256 matched (`bb7743474fd175aea511be1799764ba054465285bbdf54c4722ca61d1fb4f5c9`).
QEMU reached the live desktop with this command; the FAT directory drive makes
`~/images/Haiku/shared` available as a guest disk for source and result transfer:

```sh
qemu-system-x86_64 -name boltpack-haiku-test -m 4096 -smp 4 \
  -cdrom "$HOME/images/Haiku/haiku-r1beta6-x86_64-anyboot.iso" \
  -drive file=fat:rw:"$HOME/images/Haiku/shared",format=raw,if=ide \
  -boot d -net none -display sdl -no-reboot -no-shutdown
```

This only qualifies that the image boots to the desktop. Terminal invocation
and compiler availability have not yet been confirmed through the headless
Xvfb input setup, so it is not evidence of a native Haiku build or runtime test.
Use a usable graphical session or another repeatable guest command channel to
continue the native CLI and BeAPI GUI checks. Keep the disk image and shared
working files outside the repository.

## Plan 9: 9front amd64 under QEMU

The following setup and run record is historical. The user has clarified that
there is no usable Plan 9 emulation environment available for current work, so
native guest checks are deferred and must not be claimed as freshly verified.
The image path or presence of QEMU alone does not establish a runnable guest.

The native Plan 9 CLI was built and exercised in 9front 12111 amd64 with QEMU
TCG. The qcow2 image is stored outside the repository at
`~/images/Plan9/9front-12111.amd64.qcow2`; the official [9front nightly build
page](https://build.9front.org/) provides the image. `qemu-img check` reported
no errors. KVM was unavailable in this environment, so use TCG:

```sh
qemu-system-x86_64 -accel tcg,thread=multi -cpu max -smp 2 -m 2048 \
  -machine pc \
  -drive file="$HOME/images/Plan9/9front-12111.amd64.qcow2",format=qcow2,if=ide \
  -drive file=/path/to/plan9-transfer.img,format=raw,if=ide \
  -drive file=/path/to/boltpack-plan9-source.iso,media=cdrom,if=ide \
  -nic none -display none -serial mon:stdio -no-reboot
```

Press Enter at the default local-disk boot prompt and again at `user[glenda]:`.
With no framebuffer, rio prints a display error and exits back to the usable
`term%` shell; this does not stop the guest. Mount the Joliet source ISO and
FAT transfer disk with `9660srv`/`dossrv`, then mount `/srv/9660` and `/srv/dos`
on `/n/iso` and `/n/share` respectively. The raw FAT image can be read or
written by Plan 9 and inspected after shutdown with mtools.

For amd64, build every translation unit with `6c -pFVw -D__PLAN9__
-DPLAN9_CPU_X86_64 -I /amd64/include -I /sys/include`, plus the core and Plan 9
frontend include paths; link the resulting `.6` objects with `6l`. Plan 9's
native compiler accepts bare `-DNAME` defines, so `archiver.c` maps
`__PLAN9__` and the architecture marker to the archive creator identity. Use
`-DPLAN9_CPU_X86` for i386 or `-DPLAN9_CPU_ARM32` for ARM. `-p` enables the full
preprocessor and native include paths must precede APE paths. The tested guest
built and ran the CLI, read/extracted
a Linux archive, created Stored and Fluff archives, and transferred a
Plan 9-created Stored archive back to Linux. Linux full and mini readers
accepted it and the extracted payload matched byte-for-byte. Linux reported
Plan 9 permission metadata as unsupported/partial. The guest also created an archive with a 0550 directory and 0640 file; a
rebuilt guest CLI extracted it, restoring both modes and the directory mtime.
Directory mode restoration preserves Plan 9’s `DMDIR` bit. Other CPUs, broader
filesystem/API edge cases, and graphical startup remain unqualified.

R05 subsequently disabled real extraction because the frontend only used
pathname-based output operations and could not safely anchor staging or
publication. The historical extraction and metadata results above do not
describe the current behavior. The host-shim test now verifies that real
extraction refuses before output creation and that `-n` dry-run remains
available. Re-enable guest extraction only after the safe publication path is
implemented and target-qualified.

## Linux mini: native 32-bit Linux (QEMU fallback)

On the current Debian host, the 32-bit Linux mini executable runs directly.
`make CC=gcc linuxmini-interoperability-test` uses native execution and passes.
An explicit probe found `qemu-i386` returns no entries for `readdir` on the
fixture directory, causing recursive-create tests to produce incomplete
archives. On another host where direct 32-bit execution is unavailable, try
`make CC=gcc LINUXMINI_RUNNER=qemu-i386 linuxmini-interoperability-test` and
verify the directory-enumeration fixture before relying on it.

`test/reference/interoperability/linux-mini-qemu.json` independently pins the
metadata and Profile 1 payload hashes. The runner checks Profile 1 archives in
both directions between Linux mini and the full core, in addition to its
metadata, overwrite, corrupt-input, and collision checks.

## DOS: DOSBox-X FAT12

The repeatable FAT12 transfer test uses DOSBox-X 2025.02.01 and mtools; it
creates a disposable 720 KiB floppy image and runs the Watcom DOS16 executable
on it:

```sh
make CC=gcc dosboxx-fat12-interoperability-test
```

The test verifies CP437 filename extraction and timestamp restoration, DOS16
archive creation readable by Linux, and replacement of an existing archive.
Before replacement it sets the FAT HIDDEN, SYSTEM, and ARCHIVE flags, then
checks the resulting archive contents with Linux and the flags with `mattrib`.
The default run deletes its temporary image after completion; pass
`--output DIR` directly to `test/check_dosboxx_cp437.py` to retain an image and
logs for inspection. Linux reports partial where DOS cannot restore foreign
metadata. This emulator run qualifies the emulated FAT12 path and DOSBox-X DOS
API behavior, not every DOS implementation or filesystem.
The independent expected-result manifest is
[`test/reference/interoperability/dos16-fat12.json`](../test/reference/interoperability/dos16-fat12.json).
It pins the CP437 spelling and bytes, source and replacement payload hashes,
and FAT-visible timestamp. The runner checks Linux extraction of both the
DOS-created archive and the archive after DOS16 replacement.

The CP850 name round-trip has its own DOS16 build and runner:

```sh
make CC=gcc dosboxx-fat12-cp850-interoperability-test
```

It uses `Ø.TXT`, a character whose CP850 byte differs from CP437, and checks
DOS batch arguments, DOS-created archive names, Linux extraction, extraction
back to DOS FAT12, and the on-disk 8.3 byte (`0x9D`). Pass `--output DIR` to
`test/check_dosboxx_cp850.py` to retain the image and logs. This checks uppercase
`Ø`. Although the guest reports codepage 850, DOS `DIR ø.TXT` still searches
byte `0x9B` and fails against the FAT uppercase short-name byte `0x9D`. The
DOSBox-X lowercase lookup remains unqualified; the FreeDOS runtime check below
qualifies that path with its country/codepage tables loaded.

## DOS: FreeDOS 1.4 CP850 on QEMU

FreeDOS's floppy edition provides an independent DOS filesystem runtime. The
runner modifies a disposable copy of the boot floppy, loads the FreeDOS
CP850 country and console tables, and checks lowercase-name transfer plus
nested directory transfers in both directions:

```sh
make CC=gcc \
  FREEDOS_BOOT_IMAGE=/path/to/144m/x86BOOT.img \
  FREEDOS_MODE=/path/to/MODE.COM \
  FREEDOS_DISPLAY=/path/to/DISPLAY.EXE \
  FREEDOS_EGA_CPX=/path/to/EGA.CPX \
  FREEDOS_OUTPUT=build/freedos-cp850-run \
  freedos-cp850-interoperability-test
```

The boot image is `144m/x86BOOT.img` from the [FreeDOS 1.4 Floppy Edition](https://download.freedos.org/1.4/FD14-FloppyEdition.zip).
Extract `MODE.COM` and `EGA.CPX` from the `mode.zip` base package, and
`DISPLAY.EXE` from `display.zip` in the [official FreeDOS package repository](https://ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/latest/base/).
The downloaded archive SHA-256 values used here are `45b1fa7c52dd996c3bfa5e352ffcd410781b952a6ad629f15a4c9ec4bbaefc5a`
(Floppy Edition), `c92e07e9bea8f132437c542184c0cadc7fbabe3543174209c2799407ab671bdb`
(`mode.zip`), and `1ae9498e8b4ba12db3fcf0d83fea0cb0fa4c44ee0a02ac5e1ee9a29700cca459`
(`display.zip`). The run record contains SHA-256 values for the extracted boot
image and executables.
The runner uses `DISPLAY`, `MODE CON CP PREPARE/SELECT`, and `CHCP 850`; it
asserts that DOS `DIR` resolves lowercase `ø.TXT`, that DOS16 archives that
name as UTF-8 for Linux, and that extraction of a Linux-created lowercase
archive reproduces the payload on FAT12. FreeDOS reports its active codepage
as 850 (while its system codepage remains 437); this setup still resolves the
CP850 FAT short-name case mapping correctly. The runner also checks nested
Linux-created and DOS-created trees, exact file bytes, empty directories, and
Hidden/System directory attribute restoration, and a low-space archive replacement
that must fail while preserving the previous archive byte-for-byte without a
stage or recovery-backup leak. It tests `bpmini r TARGET.BLT BK####.BAK`, which
integrity-tests a same-directory recovery backup, refuses an existing target,
restores a missing target, refuses to overwrite an existing target, and rejects a corrupt backup without changing it. Replacement reserves a paired `BPK####.TMP`
stage and `BK####.BAK` backup; a test build exits after the target-to-backup
rename, then a second FreeDOS boot runs the production recovery command and
checks the original archive and stage. DOS cannot restore directory mtime
through its file timestamp API, so the expected outcome is partial.

This machine's FreeDOS 1.4 boot image and package files are in
`/home/tony/images/FreeDOS`. The repeatable CP850-only record is retained at
`build/r10-freedos-cp850-20261030-make/`. The expanded interoperability,
low-space publication, and recovery run and disposable image are retained at
`build/r10-freedos-recovery-corrupt-negative-20261030/`; `run.json` records asset, binary,
manifest, payload, recovery, and floppy hashes plus QEMU/mtools versions.
Set `FREEDOS_OUTPUT=build/<run-name>` on the Make target to retain a new run
instead of using its default temporary directory. The 2026-10-30 run passed
under QEMU 10.0.13 with exact bytes in both transfer directions.
The process-exit injection verifies the rename interruption boundary and
explicit recovery on FreeDOS FAT12; it does not establish hardware power-loss
durability, native DOS behavior, or behavior on other filesystems.

The full DJGPP frontend can also run with CWSDPMI from the DJGPP `csdpmi`
package:

```sh
make CC=gcc CWSDPMI=/path/to/CWSDPMI.EXE dos-djgpp-dosboxx-fat12-test
```

This test creates, appends to, deletes from, and tests a full-DOS archive on
the same kind of FAT12 image; it also extracts a Linux-created archive in
DOSBox-X, then Linux verifies and extracts the edited DOS archive with exact
payload bytes. The test sets HIDDEN, SYSTEM, and ARCHIVE on the DOS-created
archive before append/delete and verifies all three flags survive. Pass
`--output DIR` directly to `test/check_djgpp_dosboxx.py` to retain the image
and logs. The runner also writes `manifest.json` with the source revision and
worktree state, tool versions, executable/image hashes, FAT12 geometry, and
independent input hashes. This qualifies DOSBox-X FAT12 behavior with CWSDPMI;
native DOS, other filesystems, and crash recovery still need separate checks.

## Atari: Hatari

- Emulator: Hatari 2.5.0.
- Machine: STE, TOS 1.62.
- ROM: `~/images/Atari/tos162se.img`.
- Other available TOS images: `tos106se.img` and `tos205se.img` in the same
  directory.

On a graphical desktop, start an interactive emulator with:

```sh
hatari --machine ste --tos "$HOME/images/Atari/tos162se.img" --sound off
```

The following bounded, headless startup check was run successfully. It exits
after 250 vertical blanks; it does not run BoltPack or prove file transfer.

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  hatari --machine ste --tos "$HOME/images/Atari/tos162se.img" \
  --sound off --confirm-quit off --run-vbls 250
```

The Atari cross-compiler is available here. `make atari-cli` and
`make atari-gui` build the applications. On 2026-10-28, Hatari 2.5.0 with
TOS 1.62 auto-launched `build/atari/boltpack.prg` into the GEM main window.
The first capture showed the executable name overprinting the neighboring menu
labels. The menu builder now names the first Desk title `BoltPack`, reserves
its full width, aligns each pull-down with its title, and uses the specified
visible `G_BOX` menu bar. The rebuilt application displays `BoltPack`,
`Archive`, `Compression`, and `Help` without overprinting in Hatari. This
qualifies startup and menu rendering. An earlier Hatari keyboard injection
verified Ctrl+N opens the create file selector, but a fresh-build replay failed
to reproduce it with lowercase or uppercase key input. An older mouse
press/drag capture showed Archive → Open opening the archive selector, but
replaying the documented fast-click input failed against both current and
captured older binaries. The repeatable menu-pointer runner now inserts a pause
between clicks and verifies that the selector appears. The keyboard
investigation exposed and fixed an AES binding mismatch: the legacy
`evnt_multi()` wrapper shifted its keyboard outputs for this mintlib, so the
GEM event loop now uses the structure-based `evnt_multi_fast()` API. The
selectors now start at the GEMDOS current drive's root instead of assuming
floppy A:. The helper uses GEMDOS `Dgetdrv()` for the drive letter, as
documented in the [Atari Compendium §2.56](https://info-coach.fr/atari/software/_development/Atari-Compendium.pdf).
Hatari confirmed that the application launched from C: opens the
create selector at `C:\\*.*`, lists host-drive files, and returns a selected
`SMOKE.TXT` to the create flow. Startup, menu, selector opening, and one-file
selection are verified. A temporary test build set the follow-up alert's
default response to Create so Hatari keyboard injection could exercise the
remaining create/save path without changing the shipped UI. It produced an
Atari-origin format-6 archive containing the data and two supplementary
metadata entries. Linux's reader passed `test` and extracted the original
`SMOKE.TXT` bytes exactly; it reported the Atari metadata as unsupported on
Linux. Reopening that archive in the normal Atari GUI now shows only its data
file: the list callback filters supplementary META and RSRC records. The
normal GUI then extracted that archive to a fresh `OUT` directory; output bytes
matched the source and its timestamp restored to whole-second precision. The
host-directory drive changed the POSIX mode from 0664 to 0644, so this does not
qualify native FAT attributes or POSIX mode preservation. Mouse activation of
Create in the shipped dialog, cancellation, and GUI metadata restoration on a
native FAT volume remain unqualified. GEM's leftmost Desk
title is the application name and aligns with its pull-down; see the
[Atari Compendium, AES menu bar and GEM menu guidelines](https://info-coach.fr/atari/software/_development/Atari-Compendium.pdf)
(§6.26 and §11.15); the same reference documents the `evnt_multi` keyboard
outputs (§6.71).

On 2026-10-29, additional `xdotool` probes under `xvfb-run` moved the Hatari
pointer onto the `SOURCE.TXT` row, selector OK button, and Create button, but
the selector did not advance and no archive was produced. Repeating with a
double-click and explicit X window focus did not change that result. These
attempts do not qualify the shipped Create interaction; the current Xvfb input
path is not a usable click driver for this dialog. The earlier successful
source-selection and temporary-default-button runs remain the only positive
GUI evidence. Revisit with a native desktop/session or another input method
that can confirm AES button events from the guest.

A later replay check with a fresh `make CC=gcc atari-gui` build also failed to
reproduce the earlier Ctrl+N result through hconsole. Both lowercase and
uppercase key events left the application at its main window, so the literal
`2` alert-button input was never tested. Captures and scripts are in
`build/r02-retained-20261030/boltpack-atari-create-key2-_ns0az8h` and
`build/r02-retained-20261030/boltpack-atari-create-upperN-af5sgkrg`. Retain the earlier Ctrl+N run as
positive evidence, but treat it as non-reproducible with the current headless
input setup; production Create-button and save-flow interaction remain open.

The Linux/Atari host-directory exchange runner uses an independent expected
result manifest for payload sizes and SHA-256 hashes, plus the expected refusal
and partial outcomes. Run it with:

```sh
python3 test/check_hatari_interop.py --tos "$HOME/images/Atari/tos162se.img" \
  --output build/r02-atari-host-20261031
```

The run retains its exchange drive, command log, source snapshot, independent
manifest hash, and hash-indexed artifacts under the output path. `run.json`
records the Hatari and compiler versions and the GEMDOS host-directory
filesystem limitation. Without `--output`, it creates a unique directory under
`build/`.

A temporary Hatari 2.6.1-devel build (upstream commit `23bc3060`) exposed its
new `mousemove`, `leftdown`, and `leftup` control events. Under Xvfb, these
events did not move the guest pointer from its initial position or activate the
selector buttons, even when the corresponding Atari screen coordinates were
used. Hatari keyboard events can type a filename into the selector and Enter
reaches the “Added to archive list” alert, but keyboard navigation did not
activate its Create button. This confirms that the obstacle is guest input
delivery through the current headless harness, rather than the Create handler;
it adds no production Create-button qualification. The temporary build and
input scripts live under `/tmp` and are not required by the normal test suite.

For a repeatable manual launch, put a copy of the PRG on the GEMDOS folder and
run:

```sh
BP_ATARI_TMP=$(mktemp -d /tmp/boltpack-atari.XXXXXX)
mkdir "$BP_ATARI_TMP/drive"
cp build/atari/boltpack.prg "$BP_ATARI_TMP/drive/BOLTPACK.PRG"
hatari --machine ste --tos "$HOME/images/Atari/tos162se.img" \
  --sound off --harddrive "$BP_ATARI_TMP/drive" --auto C:\\BOLTPACK.PRG
```

Other menu item actions, choosing files in the selector, archive
creation/extraction, cancellation, and GUI metadata behavior still need runtime
validation. A reproducible CLI exchange test uses
Hatari's host-directory drive and retains its commands, exit statuses, logs,
files, source snapshot, tool versions, and artifact hashes in a build evidence
directory:

```sh
python3 test/check_hatari_interop.py --tos "$HOME/images/Atari/tos162se.img"
```

This checks Linux-to-Atari and Atari-to-Linux file bytes for Stored and
BoltLite, Atari append/delete, failed edit preservation, and checksum failure
cleanup over an existing destination. It also checks nested and empty
directories and pre-output refusal of a long name, case-colliding files, and
case-colliding implicit parents. On 2026-10-29, the same fixture passed under
TOS 1.06 and 2.05 as well as the previously tested TOS 1.62. Evidence is in
`build/r02-retained-20261030/boltpack-hatari-pp7plo0t` (1.06) and `build/r02-retained-20261030/boltpack-hatari-he45l44l`
(2.05).
Directory timestamp restoration reports partial on this backend. Linux reports
partial preservation for Atari-specific metadata. It does not validate a native FAT disk, attributes,
timestamps, or GUI interactions; test those separately.

For FAT12 floppy testing, run:

```sh
python3 test/check_hatari_fat.py --tos "$HOME/images/Atari/tos162se.img"
```

This uses a disposable 720 KiB image and `hconsole.py` for a normal Hatari
quit. The `--run-vbls` option terminates before the changed floppy is saved;
the control-console `quit` ejects it. The test therefore reads the final files
back from the image with mtools after Hatari exits. FAT12 file hidden/system
flags and timestamps round-trip. Generated conflict fixtures on TOS 1.06,
1.62, and 2.05 verify native TOS HIDDEN wins over portable ARCHIVE in both
metadata record orders. Evidence is in `build/r02-retained-20261030/boltpack-atari-fat-0_ye2d3a`,
`build/r02-retained-20261030/boltpack-atari-fat-8lbfpw9y`, and
`build/r02-retained-20261030/boltpack-atari-fat-27zmstyp`. TOS 1.62 reports partial for directory times
and nonzero directory attributes. The GUI startup/shortcut check does not
qualify file selection, archive operations, or metadata behavior.

The FAT12 runner was repeated on 2026-10-29 after the current frontend changes;
TOS 1.06, 1.62, and 2.05 all passed guest transfer, archive edits, file FAT
flags/timestamps, and metadata precedence checks. Retained run directories are
`build/r02-retained-20261030/boltpack-atari-fat-eduejk3t` (1.06),
`build/r02-retained-20261030/boltpack-atari-fat-m134gjg8` (1.62), and
`build/r02-retained-20261030/boltpack-atari-fat-y_mzqmnc` (2.05). The host-directory interoperability
runner also passed again on TOS 1.62; its logs and exchange files are in
`build/r02-retained-20261030/boltpack-hatari-qfqjcm8i`.
A direct GEMDOS probe on the same TOS/FAT12 setup returned `-33` (`EFILNF`)
for `Fattrib` directory reads and writes and for opening a directory with
`Fopen`, so handle-based `Fdatime` cannot set a directory timestamp either.
This result is specific to the tested TOS 1.62 environment.

The FAT12 guest transfer/edit test also passed with TOS 1.06 and 2.05 on
2026-10-29, including verification of the saved floppy image after clean
shutdown. Evidence is in `build/r02-retained-20261030/boltpack-atari-fat-a066pl85` (1.06) and
`build/r02-retained-20261030/boltpack-atari-fat-6bdokojg` (2.05). These runs extend CLI and FAT12
coverage across three TOS versions; they do not qualify native hardware or the
GEM GUI.

For a direct Atari-to-Amiga transfer, run the combined producer/consumer test:

```sh
make CC=gcc HATARI_TOS="$HOME/images/Atari/tos162se.img" \
  XDFTOOL=build/venv-amiga-tools/bin/xdftool \
  atari-amiga-interoperability-test
```

The runner creates and edits a Profile 1 archive on a disposable FAT12 floppy
under Hatari/TOS 1.62, then copies that archive to the FS-UAE host drive and
validates/extracts it with the Amiga CLI. It then sends the Amiga-created
Profile 1 archive back to TOS on a fresh FAT12 floppy and checks the extracted
bytes. Both guest runners consume the independent
`test/reference/interoperability/atari-amiga-fsuae.json` manifest. It pins
bytes, the Amiga name projection, nested file, and empty directory. The Amiga
validates all 15 Atari archive entries; both directions extract exact payloads
and report partial for the other platform's native metadata. The full combined
run passed again on 2026-10-29 with evidence in
`build/r02-retained-20261030/boltpack-atari-amiga-twyz0hzg`; this run rebuilt both guest binaries and
repeated the transfer in both directions. An earlier invocation without the
documented `XDFTOOL` override could not start the Amiga phase because `xdftool`
is installed only in the project venv on this machine. The prior passing run is
in `build/r02-retained-20261030/boltpack-atari-amiga-jjfobm6w`; separate consumer runs are retained at
`build/r02-retained-20261030/boltpack-atari-amiga-oxdq54d6` and `build/r02-retained-20261030/boltpack-amiga-atari-reverse`.
Host-directory Amiga storage does not qualify OFS/FFS semantics.

## Amiga: FS-UAE

- Emulator: FS-UAE 3.1.66.
- Machine: A500+ with Kickstart 2.04, revision 37.175.
- ROM: `~/images/Amiga/kickstart-2.04-a500plus.rom`.
- Workbench disk: `~/images/Amiga/Workbench-2.04/1-Workbench.adf`.
  Extras, Fonts, and Install disks are in the same directory.

For an interactive graphical desktop, create a temporary copy of the Workbench
disk and launch:

```sh
BP_AMIGA_TMP=$(mktemp -d /tmp/boltpack-amiga.XXXXXX)
cp "$HOME/images/Amiga/Workbench-2.04/1-Workbench.adf" "$BP_AMIGA_TMP/workbench.adf"
fs-uae --amiga_model='A500+' \
  --kickstart_file="$HOME/images/Amiga/kickstart-2.04-a500plus.rom" \
  --floppy_drive_0="$BP_AMIGA_TMP/workbench.adf" \
  --floppy_drive_speed=800 --warp_mode=1 --fullscreen=0
```

The copy is a working disk: any guest writes affect it, not the original ADF.
Its temporary directory can be removed after the emulator exits and any needed
test output has been collected.

The validated boot used Xvfb with a 1024×768×24 screen and software OpenGL
(`LIBGL_ALWAYS_SOFTWARE=1`). The screen remained white during startup; with
the accelerated settings above, the boot reached a keymap-selection prompt
after roughly 35 seconds. Enter `0` and press Enter to decline copying a keymap;
the Workbench desktop then appeared. A virtual-display run is useful for
automation but is not an interactive desktop for the user.

FS-UAE's Print Screen shortcut saved the actual emulated frame during this
check. A temporary test home needed a `Desktop` directory for the screenshot
files. A guest startup screenshot is not proof of BoltPack interoperability.
For unattended CLI testing, install `amitools` in a Python environment and
provide its `xdftool` executable to the runner:

```sh
python3 test/check_fsuae_interop.py \
  --rom "$HOME/images/Amiga/kickstart-2.04-a500plus.rom" \
  --workbench "$HOME/images/Amiga/Workbench-2.04/1-Workbench.adf" \
  --xdftool /path/to/xdftool
```

The runner modifies only a disposable Workbench copy, boots a host-directory
drive, captures guest exit statuses and verifies Stored and BoltLite profile
file bytes both ways, a successful profile append, and incompatible options.
It also verifies failed creation and append after a valid first input preserve
the existing archive and clean up a failed new archive, a successful and
failed delete, and archive protection/comment preservation across edits.
It checks native protection/comment restoration on an archived file and two
directories (including an empty one), and failed checksum extraction over an
existing file with metadata.
Its independent expected-result manifest is
[`test/reference/interoperability/amiga-host-directory.json`](../test/reference/interoperability/amiga-host-directory.json).
The manifest pins profile, append, nested-tree, and codec payloads; both Amiga
guest extraction and Linux cross-extraction are checked against those hashes.
The expanded 16 KiB host-directory run passed on 2026-10-29; the manifest-backed
rerun is in `build/r02-retained-20261030/boltpack-fsuae-neoktew4`.
It defaults to a 256 KiB shell stack; add `--stack 0` to use the guest default.
The CLI binary carries an AmigaDOS `$STACK:8192` cookie and, on libnix builds,
calls libnix's stack initializer to expand undersized process stacks and restore
them on exit. AmigaDOS documents the cookie for shells and Workbench launchers
([stack-control guide](https://wiki.amigaos.net/wiki/Controlling_Application_Stack));
the explicit libnix initialization also covers our Workbench 2.04 CLI launch.
Without startup expansion, the shallow tree create stalled at the 4 KiB
default, while explicit 8 KiB, 16 KiB, and 256 KiB stacks passed. With it, the
64-level create/extract, profile/edit, and four-codec corpus passes with
`--stack 0`; evidence is `build/r02-retained-20261030/boltpack-fsuae-_0zil2jv`. Repeated one-character
directory names keep that fixture inside the AmigaDOS path limit. The CLI has
also reduced core writer and traversal stack use. The host-directory runner also tests and
extracts Stored, BoltLite, BoltLZ and Fluff archives over a 512 KiB input with
256 KiB blocks using the default shell stack; Linux independently verifies and
extracts each Amiga-created archive byte-identically. Maximum block size and
all-codec runs on native floppy filesystems are qualified below.
After changing archive transaction names to include a target hint and pair the
recovery backup with its stage, `test/check_fsuae_interop.py` passed again on
2026-10-29, including the host-directory edit and four-codec round trips at
256 KiB blocks. Evidence is in `build/r02-retained-20261030/boltpack-fsuae-oo0d3xik`.
The runner accepts `--chip-memory`, `--fast-memory`, and `--codec-block` to
qualify resource-constrained configurations. This command uses the A500+'s
512 KiB chip RAM, no fast RAM, the shell's default stack, and 16 KiB blocks:

```sh
python3 test/check_fsuae_interop.py \
  --rom "$HOME/images/Amiga/kickstart-2.04-a500plus.rom" \
  --workbench "$HOME/images/Amiga/Workbench-2.04/1-Workbench.adf" \
  --xdftool /path/to/xdftool --chip-memory 512 --fast-memory 0 \
  --codec-block 16k --stack 0 --expect-fluff-refusal
```

That low-memory run passed profile transfer/edit checks and Stored, BoltLite,
and BoltLZ round trips in both guest and Linux. Fluff creation returned
nonzero without publishing an archive or leaving a stage; Fluff extraction
was therefore not attempted. The original evidence is in
`build/r02-retained-20261030/boltpack-fsuae-h_m6dzqc`; after adding the libnix stack expansion, the
same workload also passed at `--stack 0` in `build/r02-retained-20261030/boltpack-fsuae-ag4i8b4j`.
An explicit 16 KiB shell stack passed as well in
`build/r02-retained-20261030/boltpack-fsuae-rqay1fec`. This checks one constrained workload, not
general heap-exhaustion handling. Linux reports partial
preservation for Amiga-native metadata. Hardware power-loss durability, the
GUI, and additional archived metadata coverage remain open.

The native floppy runner also accepts `--chip-memory` and `--fast-memory`.
On 2026-10-29, the complete OFS and FFS transfer/edit/metadata/name-refusal
corpora passed with 512 KiB chip RAM, no fast RAM, and the Workbench default
shell stack (`--stack 0`). The corpus sets and compares timestamps on three
nested directories in both filesystems. Results are retained in
`build/r02-retained-20261030/boltpack-amiga-ofs-4vpuos0_` and `build/r02-retained-20261030/boltpack-amiga-ffs-63mltyc8`.
This qualifies those workloads under constrained guest memory; it does not
qualify every codec under constrained memory.

With normal A500+ memory, native OFS and FFS also passed all four codec
round trips on 2026-10-29. The guest created and tested Stored, BoltLite,
BoltLZ, and Fluff archives from a 256 KiB file using 256 KiB blocks. Linux
independently tested and extracted each archive byte-for-byte. Evidence is in
`build/r02-retained-20261030/boltpack-amiga-ofs-causbh48` and `build/r02-retained-20261030/boltpack-amiga-ffs-hu_1pcai`;
both manifests record the filesystem, guest memory, block size, and fixture
size. Run with `--codec-block 256k --timeout 300` to reproduce.

The native transfer/edit/metadata suite was repeated on 2026-10-29 with the
default A500+ memory configuration, `--skip-codecs --depth 4 --stack 0`.
Both filesystems passed; retained evidence is in
`build/r02-retained-20261030/boltpack-amiga-ofs-90l0l1jb` (OFS) and
`build/r02-retained-20261030/boltpack-amiga-ffs-2mp6wwkf` (FFS).

The host-directory backend does not establish symlink-safe creation: in a
probe, FS-UAE exposed a host symlink as a regular file and the CLI archived its
target. A separate FFS `MakeLink ... SOFT` attempt failed, so native soft-link
handling remains unverified.

`test/check_fsuae_interrupt.py` exercises process interruption before
publication on FS-UAE's host-directory drive. On 2026-10-29, the runner killed
the emulator after a sibling stage had received data during forced extraction
and archive append. The existing extraction destination remained byte-identical;
the original archive hash also remained identical. Each interrupted operation
left its incomplete sibling stage because process termination bypassed normal
cleanup. Evidence is in `build/r02-retained-20261030/boltpack-amiga-interrupt-xl1qrycr` (extraction)
and `build/r02-retained-20261030/boltpack-amiga-interrupt-wjouwy_m` (append). Reproduce with
`--size-mib 64 --operation extract` or `--operation append`. On 2026-10-29, I
reran both 64 MiB cases successfully; evidence is in
`build/r02-retained-20261030/boltpack-amiga-interrupt-1_so52kc` (extraction) and
`build/r02-retained-20261030/boltpack-amiga-interrupt-h11wz8jn` (append). The current environment did
not have `xdftool` on PATH, so I installed pinned amitools 0.8.1 into the
ignored `build/venv-amiga-tools` environment. Reproduce both checks with:

```sh
python3 -m venv build/venv-amiga-tools
build/venv-amiga-tools/bin/python -m pip install amitools==0.8.1
make CC=gcc XDFTOOL=build/venv-amiga-tools/bin/xdftool fsuae-interruption-test
```

The Make target accepts `AMIGA_ROM`, `AMIGA_WORKBENCH`, `FSUAE_BIN`,
`XDFTOOL`, and `AMIGA_INTERRUPT_SIZE_MIB` overrides. These cases verify
pre-publication safety on the host-directory backend. Separate tests below
cover recovery in the rename window on the host-directory, OFS, and FFS paths.

The separate `fsuae-publish-rollback-test` target builds a test-only CLI with a
one-shot failure injected at the stage-to-target rename, after the old archive
has moved to its backup name. The runner confirms the original archive hash is
unchanged, Linux can still verify the archive, and the rollback removes the
stage and backup. The test passed again on 2026-10-30. The durable run record, command, emulator
log, source snapshot, original/replacement archives, and state are retained in
`build/r11-amiga-publish-failure-20261030-retained/`; its `run.json` records
FS-UAE 3.1.66, amitools 0.8.1, compiler, ROM, Workbench, binary, and archive
hashes. Reproduce with:

```sh
make CC=gcc XDFTOOL=build/venv-amiga-tools/bin/xdftool fsuae-publish-rollback-test
```

This deterministically exercises the error rollback path; it does not simulate
power loss between the two rename calls.

`fsuae-source-race-test` pauses the Amiga CLI after it opens a regular source
file. The runner replaces that pathname from the host, resumes the guest, and
checks that the source identity recheck rejects the operation, leaves the
existing archive byte-identical, and removes the archive stage. On 2026-10-29
this passed on the FS-UAE host-directory backend; evidence is in
`build/r02-retained-20261030/boltpack-amiga-interrupt-ojan6mms`.
Reproduce with:

```sh
make CC=gcc XDFTOOL=build/venv-amiga-tools/bin/xdftool fsuae-source-race-test
```

The test proves this pathname replacement is detected on the emulator backend;
it does not rule out a transient swap-and-restore race or qualify other Amiga
filesystems.

`fsuae-publish-interruption-test` uses a separate test-only CLI that pauses
after moving the old archive to its recovery name and before publishing the
new archive. The runner terminates FS-UAE during that interval, confirms the
target is absent, verifies the recovery file hash and staged replacement, then
boots again and runs the production recovery command. The fixture creates a
second valid backup/stage pair with the same target hint, verifies that
`boltpack r INTERRUPT.BLT` refuses the ambiguity, then invokes
`boltpack r INTERRUPT.BLT BRINTERRUPTBLTDUP001` and checks the original is
restored while the unselected pair remains untouched. On 2026-10-29 this
passed; evidence is in `build/r02-retained-20261030/boltpack-amiga-interrupt-jy3ea2br`.
Reproduce with:

```sh
make CC=gcc XDFTOOL=build/venv-amiga-tools/bin/xdftool fsuae-publish-interruption-test
```

This demonstrates that both transaction files survive process termination on
the host-directory backend and that the recovery command can verify and
restore the original when the target is absent. Archive stage and backup names
start with `BP` and `BR`, include the first 32 ASCII letters/digits from the
target basename, and share a transaction suffix. The recovery command refuses
an existing target, requires a matching regular backup and its paired stage,
verifies both archives, then restores the backup and leaves the replacement
stage for manual publication or inspection. With no backup name supplied, it
requires a unique match and refuses ambiguity. For names sharing the first 32
ASCII alphanumerics, specify the exact backup leaf name as the optional second
argument. This host-directory test does not model hardware power loss or
native-floppy filesystem behavior.

`fsuae-native-publish-recovery-test` repeats the interruption on disposable
OFS and FFS ADFs. The test-only CLI pauses after moving the original archive,
signals the host through the Workbench drive, and waits until the floppy has
had time to flush before the runner terminates FS-UAE. A second boot runs the
production recovery command, then `xdftool` reads the restored archive and
retained stage back for independent Linux integrity checks. Both filesystems
passed on 2026-10-29: OFS evidence is in
`build/r02-retained-20261030/boltpack-amiga-native-recovery-uqdx3d0b`; FFS evidence is in
`build/r02-retained-20261030/boltpack-amiga-native-recovery-9o6wgxsi`. This covers process termination
on FS-UAE's writable ADF backend after buffer settling. It does not model
hardware power loss or qualify physical floppy hardware.

Reproduce both filesystem cases with:

```sh
make CC=gcc XDFTOOL=build/venv-amiga-tools/bin/xdftool fsuae-native-publish-recovery-test
```

To run a separate test against a disposable native OFS or FFS floppy image,
use the same arguments with `test/check_fsuae_native.py`; add `--filesystem ffs`
for FFS (the default is OFS). It creates and formats its
own ADF, enables direct writes only on the disposable images, waits for the
guest's delayed floppy writes, and retains the resulting image and logs. It
checks a BoltLite profile transfer, create/append/delete, failed append, native
file and directory protection/comments, file and nested-directory timestamp
restoration, and Linux extraction of the edited archive. It transfers `café.txt` both ways and checks pre-output
refusal for unrepresentable Unicode and case-colliding implicit parents. It
defaults to a 256 KiB guest stack; pass `--stack 0` to retain the Workbench
default and `--depth N` (up to 64) to add a nested directory create/extract fixture.
Short repeated directory names keep the cumulative pathname within AmigaDOS
limits. `--timeout 300` allows the slow native floppy create/extract fixture to
finish; the emulator's default timeout may be too short for deeper trees.
After adding Amiga source identity checks, the native OFS and FFS fixtures were
rerun with `--skip-codecs --depth 4`; both passed with the current 68k build.
Evidence is in `build/r02-retained-20261030/boltpack-amiga-ofs-lvaace5z` (OFS) and
`build/r02-retained-20261030/boltpack-amiga-ffs-ewtnkm4u` (FFS).
FS-UAE passed 49-level trees on both OFS and FFS with the default shell stack.
Both runs used `--skip-codecs --timeout 300` to isolate traversal from the
slower codec workload. The guest extracted each archive back onto the
disposable floppy, and the runner read the saved leaf from the image and checked
its bytes. Evidence is in `build/r02-retained-20261030/boltpack-amiga-ofs-xs7y75m8` and
`build/r02-retained-20261030/boltpack-amiga-ffs-lmg413hf`. Both OFS and FFS also pass 56- and
64-level create/extract/readback with a 16 KiB shell stack, 2 MiB chip RAM,
and 8 MiB fast RAM. More recently, both pass the full 64-level fixture with an
explicit 4 KiB stack at the same memory settings; these runs used
`--skip-codecs --timeout 300` and are retained at
`build/r02-retained-20261030/boltpack-amiga-ofs-czinf8rz` (OFS) and
`build/r02-retained-20261030/boltpack-amiga-ffs-l_z74afe` (FFS). The 16 KiB 56-level runs are
`build/r02-retained-20261030/boltpack-amiga-ofs-op458crf` and `build/r02-retained-20261030/boltpack-amiga-ffs-ah054847`;
64-level 16 KiB runs are `build/r02-retained-20261030/boltpack-amiga-ofs-vxmstmb6` and
`build/r02-retained-20261030/boltpack-amiga-ffs-1z4qonhx`. Earlier runs also passed these depths with
a 256 KiB stack. On 2026-10-29, both OFS and FFS also passed the full 64-level
fixture with `--stack 0`, 2 MiB chip RAM, and 8 MiB fast RAM, using
`--skip-codecs --timeout 300`. Evidence is in
`build/r02-retained-20261030/boltpack-amiga-ofs-2997misu` and `build/r02-retained-20261030/boltpack-amiga-ffs-97_b_6rq`.
Each run's `manifest.txt` and `host/STACK.TXT` record a 3,200-byte probe
process stack and `cli_DefaultStack` of 1,024 longwords (4,096 bytes). The
explicit `--stack 4096` setting produced the same values; AmigaDOS defines the
CLI field as the minimum stack allocated for commands, and its default is
4,000 bytes ([DOS reference manual](https://developer.amigaos.net/sites/default/files/downloads/2024-10/Amiga_ROM_Kernel_Reference_Manual_DOS.pdf)).
So `--stack 4096` is not evidence of a larger-than-default stack. The previous
default-stack timeouts were not reproduced with the 300-second runner timeout.
Low-memory directory-time evidence is in
`build/r02-retained-20261030/boltpack-amiga-ofs-dko1vgfd` and `build/r02-retained-20261030/boltpack-amiga-ffs-954opbc8`.
On 2026-10-29, the native runner also verified conflicting Amiga protection
and portable FAT metadata in both record orders on OFS and FFS. Native
protection remained exactly 64, extracted bytes matched, and the unsupported
portable record was reported partial. The reproducible target passed on
2026-10-29. Evidence: OFS
`build/r02-retained-20261030/boltpack-amiga-ofs-4pfxha_n`; FFS
`build/r02-retained-20261030/boltpack-amiga-ffs-1961fuon`. The runner builds the Linux fixture with
`--host-cc` (defaulting to `$CC` or `cc`) and runs the conflict cases each time.
The full focused OFS/FFS corpus, including these precedence cases, is available
as `make CC=gcc XDFTOOL=build/venv-amiga-tools/bin/xdftool
fsuae-native-interoperability-test`. The target writes timestamped, complete
run directories under `build/amiga-native-ofs-*` and
`build/amiga-native-ffs-*`. On 2026-10-30, this corpus exposed that AmigaDOS
can return `ETXTBSY` when `lstat()` examines a still-open staged file in a
nested directory. The production CLI now snapshots the stage through its open
handle and checks pathname identity after close, before publication. OFS and
FFS pass the nested file and metadata round trip; current records are in
`build/amiga-native-ofs-20261030-224048` and
`build/amiga-native-ffs-20261030-224048`.

## Windows: Wine (partial 64-bit CLI feedback)

Wine 10.0's 64-bit engine can run the Win64 CLI in a 64-bit-only prefix. This
machine lacks `wine32`; on initial prefix setup, `wineboot` fails because
`C:\\windows\\syswow64\\rundll32.exe` is missing. Wine nevertheless creates
the prefix directories. Linking its 64-bit `rundll32.exe` into `syswow64`
allows the 64-bit prefix to finish initialization:

```sh
export WINEPREFIX=/tmp/boltpack-wine64-prefix
export WINEARCH=win64
export WINEDEBUG=-all
mkdir -p "$WINEPREFIX"
/usr/lib/wine/wine64 /usr/lib/x86_64-linux-gnu/wine/x86_64-windows/wineboot.exe -u || true
ln -s ../system32/rundll32.exe "$WINEPREFIX/drive_c/windows/syswow64/rundll32.exe"
/usr/lib/wine/wine64 /usr/lib/x86_64-linux-gnu/wine/x86_64-windows/wineboot.exe -u
```

On 2026-10-29, `test/check_windows_cli.py --wine /usr/lib/wine/wine64` passed
supplementary-Unicode source/archive/output paths, corrupt-block extraction
preservation and temporary-stage cleanup, file and directory hidden/system
flags and timestamps, parallel Fluff metadata creation, hard-link edit
refusal, append, test, and delete. The runner also cross-checked archives both
ways with the Linux CLI: Linux verified Windows output and extracted the data
while reporting Windows FAT metadata as partial; Windows extracted Linux data
and reported unsupported POSIX mode metadata as partial. Windows now describes
that result as partial success instead of an unknown error. Windows extraction
now scans the complete primary-name table before creating the output root.
Linux-created archives containing a reserved Windows device name, a trailing
dot or space, a case-insensitive alias, or conflicting file/directory prefixes
in either table order are refused without creating that root. The fixture skips
reparse-point refusal because Wine does not expose Linux symlinks as Windows
reparse points. It verifies read-only archive append/delete are refused before stage creation,
preserving bytes and READONLY/HIDDEN/SYSTEM flags. It also checks that archive
HIDDEN and SYSTEM attributes survive successful edits: the publisher reapplies
captured supported attribute bits after `ReplaceFile` and rolls back if
application or verification fails.
This confirms the Wine path only; it does not qualify native
Windows/NTFS/FAT behavior or ACL/stream merging.

The runner's independent result manifest is
[`test/reference/interoperability/windows-cli.json`](../test/reference/interoperability/windows-cli.json);
it pins fixture payload hashes, Unicode spelling, portable flag values, and
timestamps. Wine path conversion and CLI calls have finite timeouts. A repeat
run on 2026-10-29 passed the expanded checks, including independently hashed
data after append and delete. Wine confirms HIDDEN/SYSTEM retention after both
edits, but does not qualify Windows reparse-point refusal, native ACL/stream
merging, or recovery behavior.

The WCX Unicode entry points also pass under Wine. To retain the fixture and
transcript instead of letting the runner remove its temporary directory, run:

```sh
make CC=gcc WINE=/usr/lib/wine/wine64 win64-wcx-wine-test
python3 test/check_wcx_unicode_wine.py \
  --wine /usr/lib/wine/wine64 \
  --runner build/win64/wcx_unicode_wine.exe \
  --wcx build/win64/boltpack.wcx64 \
  --linux-boltpack build/linux/boltpack --keep \
  > build/r02-retained-20261030/boltpack-wcx-wine-20261029.log 2>&1
```

The 2026-10-29 run passed `OpenArchiveW`, `ReadHeaderExW`,
`ProcessFileW`, `PackFilesW` create/append, and `DeleteFilesW`, including
supplementary-Unicode archive and entry paths. Linux verified the packed
archive bytes and final entries; directory time was checked after close. The
ANSI header call refused the non-ACP entry. Retained evidence is in
`/home/tony/.wine/dosdevices/c:/boltpack-wcx-wine-u50d4hk_`, with the runner
transcript at `build/r02-retained-20261030/boltpack-wcx-wine-20261029.log`. This remains a Wine API
fixture; it does not verify behavior inside Total Commander or native Windows.

The Win32 and Win64 CLI builds had an undeclared runtime dependency on
`libgomp-1.dll` despite the intended static OpenMP link. The Makefile now leaves
the final GNU linker state static so GCC's trailing implicit OpenMP runtime
link resolves to `libgomp.a`; `objdump -p` confirms neither executable imports
`libgomp-1.dll`. The Wine fixture then ran without copying runtime DLLs beside
the executable.
