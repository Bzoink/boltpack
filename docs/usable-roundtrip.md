# Usable round-trip acceptance

BoltPack's transfer goal includes restoring files that can still be used. This
suite creates an application with its data, archives it, restores it, and runs
the restored application. A zero exit status or matching archive checksum alone
is insufficient to pass.

## Run

The Linux gate builds the current CLI and runs the report-classification tests:

```sh
make usable-roundtrip-test
```

It requires a C compiler and a local filesystem supporting Linux extended
attributes and POSIX ACLs. It retains inputs, archives, restored applications,
command logs and a `run.json` under `build/usable-roundtrip-*`.

Select other environments explicitly; build their production binaries first
where the toolchains are available:

```sh
make dos16 atari-cli amiga68k
make usable-roundtrip-test USABLE_PLATFORMS=linux,dos
make usable-roundtrip-test USABLE_PLATFORMS=all \
  HATARI_TOS=/path/to/tos162.img \
  AMIGA_ROM=/path/to/kickstart-2.04.rom \
  AMIGA_WORKBENCH=/path/to/Workbench-2.04.adf \
  XDFTOOL=/path/to/xdftool
```

For packaged binaries, point the runner at the unpacked executable instead of
rebuilding it. The exact executable is hashed in the report:

```sh
python3 test/usable_roundtrip.py --platforms linux \
  --linux-cli /path/to/unpacked/boltpack --output build/usable-package-run
```

`--dos-cli`, `--atari-cli` and `--amiga-cli` select other actual executables.
`--help` lists emulator/compiler overrides. Output directories must be new;
existing evidence is never replaced. The runner uses disposable FAT12 images
and a copy of the Amiga Workbench disk. It does not modify the supplied ROMs or
system disks. It compiles its own small application probes and does not install
software or download assets. Emulator control sockets and FS-UAE's Xvfb display
must be permitted by the execution environment.

## What is checked

| Environment | Restored usability | Additional preservation/failure checks |
| --- | --- | --- |
| Linux CLI | A script is invoked through its restored symbolic link; a restored native executable reads restored data. Stored, BoltLite, BoltLZ and Fluff are exercised. | Exact binary/UTF-8 payload names, modes, file/directory times, empty directories, hard-link identity, binary extended attributes, POSIX ACLs; corrupt forced extraction preserves existing bytes/attributes and cleans stages; failed archive replacement preserves the old archive; an unsafe link produces a visible partial result while the application remains usable. |
| DOS16 mini / DOSBox-X FAT12 | A restored `.COM` program executes and produces a known marker. | Executable and data bytes, empty directory, read-only/hidden attributes, independent Linux validation of the DOS-created archive, and existing-file preservation after corrupt extraction. Data is checked separately; the COM program itself does not consume it. |
| Atari TOS / Hatari FAT12 | A restored native program consumes restored data and writes a completion marker. | Exact application bytes, empty directory and executable FAT attributes; existing transfer/edit/refusal checks, including preservation of read-only archives. Directory-time partial outcomes are checked explicitly. |
| Amiga / FS-UAE host directory | A restored script with its native script protection bit launches a restored native program against restored data. | Exact program/script/data bytes, empty directory, script protection/comment metadata; existing all-codec transfer checks and corrupt extraction/archive-edit failure preservation. The guest has an `ENV:` assignment and an explicit UTC offset. |

Atari and Amiga extend the existing interoperability runners with
`--usable-roundtrip`; normal runner invocations keep their existing cases.
The Atari guest writes a completion marker and Hatari exits normally so the
floppy is flushed; an expired deadline is a failure, not a shortened pass.
The source of the portable C89 application is `test/usable/probe.c` and the
fixed data/marker expectations are in `test/reference/usable-roundtrip.json`.

## Results and evidence

The combined 2026-11-13 run passed Linux, DOS16/DOSBox-X FAT12, Atari TOS 1.62
FAT12 and Amiga FS-UAE host-directory checks. Its retained
[run record](../build/usable-roundtrip-g9sblzzy/run.json) includes expected
partial outcomes for Linux unsafe links and DOS/Atari directory timestamps.
This is host/emulator evidence, not physical-hardware qualification.

- Exit **0**: all selected environments passed their required checks. Expected
  partial preservation is identified in the checks/limitations.
- Exit **1**: at least one selected environment failed. Inspect its command log
  and retained guest record. Independent environments still run.
- Exit **2**: no failures, but an environment is unavailable because a required
  executable, tool, ROM or system disk is missing. This is incomplete, not pass.

`run.json` records the source revision and dirty state, runner/manifest hashes,
actual executable hashes, command arguments, return codes, per-check outcomes,
limitations and hashes of retained artifacts. Emulator evidence is labeled as
such. Unselected platforms, physical hardware, privileged security-attribute
restoration, other compiler configurations and the full distribution-install
workflow remain unqualified. macOS/Classic Mac, Windows and QL application
usability cases are future additions; their existing test evidence is not
promoted into a pass by this gate.

Use the [interoperability matrix](interoperability-matrix.md) for the broader
capability evidence and [same-system transfer](same-system-transfer.md) for
properties each frontend captures. A passing application corpus does not imply
complete filesystem replication.
