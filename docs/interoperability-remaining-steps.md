# Remaining steps to complete archive interoperability

**Updated:** 2026-10-31
**Baseline:** `6d396f6` (`test: strengthen interoperability coverage`)
**Goal state:** active; execution has resumed.

This checklist turns the open items in the
[completion plan](interoperability-completion-plan.md) and
[requirement ledger](interoperability-requirement-ledger.md) into work that can
be assigned, implemented, and reviewed. Existing passing work is the baseline.
The full `TMPDIR="$PWD/build/tmp-test" make CC=gcc test` gate passed on
2026-10-30 after the current QNX/AIX fail-closed extraction changes. It includes
format and codec units, the 160-case Linux CLI suite, source and extraction race
fixtures, POSIX dirfd shims, QNX/AIX refusal checks, and frontend host suites.
It was rerun on 2026-10-30 after the Atari parent-redirection harness and the
current shared-source work; all suites passed, including the Linux CLI's 160
cases, legacy frontend host profiles, Plan 9 fail-closed host checks, and the
POSIX race/failure suites. Native Plan 9 was not run.
The gate passed again on 2026-10-30 after the Atari held-directory cleanup
changes: format/codec units, 160 Linux CLI cases, source/path race checks, POSIX
dirfd and fail-closed wrapper suites, Atari path tests, Linux mini, RISC OS,
Haiku host shims, QL/X68000/Plan 9 host profiles, macOS host checks, DOS, and
CP/M-86 all completed successfully. No native Haiku or Plan 9 test was run.
Rerun it after changes that affect shared behavior; do not repeat it merely to
start an unrelated task.

The goal covers format 6 and the active scoped frontends: safe file transfer,
representable names and basic attributes, explicit partial/error outcomes,
archive edits, and validation on the relevant filesystems. Formats 1–5 remain
unsupported. Optional Phase 4 rich metadata translation is outside this work.
**Scope decision (2026-10-30): native Haiku/BeOS qualification is deferred by the
user because its toolchain/runtime cannot be verified here. It is excluded from
the current completion gate; retain existing Haiku host-shim evidence and do not
spend remaining work setting up Haiku. Reopen R12 when a verifiable target
environment is available or the user changes this scope.**

**Environment decision (2026-10-30): native Plan 9 qualification is unavailable
because this machine has no usable Plan 9 emulator/runtime. Exclude the native
rerun and other Plan 9 CPU targets from the current completion gate; retain the
Plan 9 host-shim checks in scope and treat existing guest results as historical.
Reopen native Plan 9 qualification when a working environment is available.**

## Execution order

1. Do R01, then R02–R04 on this machine. Start each with the specific missing
   case listed below; reuse existing fixtures and emulator setup.
2. Use R05 to resolve the known safety contract and code gaps. It may expose
   changes needed in the platform tasks; give each concrete defect its own
   regression and change.
3. Continue R06 only with a new diagnostic or newly available GUI/filesystem
   input. R07 recursive source enumeration now passes on sQLux hostFS using
   `Q_DIR`/`IO_FSTRG`; its remaining cases require a physical QDOS filesystem.
4. Prepare environment prerequisites for active target tasks R08–R11, R13, and
   R15 while local work proceeds. Skip deferred R12 and the unavailable native
   portion of R14; an unavailable environment for another task must not prevent
   progress on independent work.
5. Complete R16 after implementation and platform runs, then R17 as the final
   gate. I01 is already recorded; I02–I07 close through these tasks.

R02's independent Amiga OFS/FFS manifests and sQLux evidence records are now
retained. Native Plan 9 runs are unavailable because this machine has no usable
emulator; keep the existing guest results historical and run only the host-shim
checks here. Continue with locally runnable lifecycle, resource-accounting, and
race-safety work without attempting guest setup.

| Initial readiness | Tasks |
| --- | --- |
| Ready for local work | R01–R05; R14 host-shim checks |
| Local investigation with a bounded new approach | R06 GUI input and parent-path cases |
| Depends on a target environment or additional filesystem | R07 physical QDOS metadata/recovery; R08–R11; R13, R15 |
| Deferred by environment availability | R14 — native Plan 9 rerun and other CPUs; host-shim checks remain active |
| Deferred by user scope decision | R12 — Haiku/BeOS native qualification |
| Depends on preceding results | R16 and R17 |

## Work available on this machine

### R01 — Record the exact remaining acceptance cases (I02/I03/I06)

- For every partial/unqualified ledger area, record the missing subcase,
  production entry point, existing fixture to extend, and required environment.
  Preserve existing passes; do not restart a source-wide audit.
- Use R02–R15 below as the task index. Split a task further when its listed
  subcases can be completed independently, and record an actual result per
  frontend/build rather than one result for an OS family.
- **Done when:** every open F0/F1/F2/F3/F5 area in the ledger has a named task
  and a concrete missing-case checklist. No vague “other cases” closure claims.

### R02 — Complete independent manifests and durable run records (I04/I06)

**Status:** the native Amiga OFS/FFS evidence slice is complete. R02 remains
in progress for adapted runs that still need independent manifests or complete
execution records. Native Plan 9 runs are excluded from this machine's scope
because no usable emulator/runtime is available; keep the host-shim checks in
scope. On 2026-10-30, all 58 artifact paths cited from `/tmp` in the
interoperability and emulator docs were copied to the ignored
`build/r02-retained-20261030/` tree; `index.json` records each copied file size
and SHA-256. The copies are byte-for-byte verified. This preserves historical
artifacts but does not make transcript-only or manifest-less runs reproducible
current validation.

2026-10-30 OFS and FFS runs passed with
[`amiga-native.json`](../test/reference/interoperability/amiga-native.json).
Evidence is retained under `build/r02-amiga-ofs-20261030` and
`build/r02-amiga-ffs-20261030`; each includes the input manifest, command log,
revision, worktree status and tracked diff, executable/image hashes, archives,
and observed results. These runs used a dirty worktree; untracked files are
listed in `git-status.txt` and the expected manifest is copied separately.
The corpus was rerun on 2026-10-30 after correcting the tool-version recorder.
OFS and FFS both passed again, retaining the full worktree and emulator record
under `build/tmp-test/boltpack-amiga-ofs-t3gsq_2k` and
`build/tmp-test/boltpack-amiga-ffs-wqfn1k2g`. The refreshed manifests report
`xdftool_amitools=0.8.1`; each manifest's expected-manifest hash matches the
current independent JSON. These use `--skip-codecs`; the native codec corpus
remains covered by its separate prior runs. After the later shared-core changes,
OFS and FFS both passed again using explicit repository-local output paths:
[`build/amiga-native-ofs-20261030`](../build/amiga-native-ofs-20261030/manifest.txt)
and [`build/amiga-native-ffs-20261030`](../build/amiga-native-ffs-20261030/manifest.txt).
Each record includes the command log, expected payloads, worktree diff/status,
compiler/emulator versions, and image/binary/archive hashes. These also use
`--skip-codecs`; separate full-codec OFS and FFS runs passed against the same
current binaries with a 600-second emulator timeout, retaining artifacts at
[`build/amiga-native-codecs-ofs-20261030-retry`](../build/amiga-native-codecs-ofs-20261030-retry/manifest.txt)
and [`build/amiga-native-codecs-ffs-20261030`](../build/amiga-native-codecs-ffs-20261030/manifest.txt).
The initial OFS attempt hit the old 90-second timeout during the final delete;
`check_fsuae_native.py` now defaults to 600 seconds. The explicit 600-second
OFS and FFS codec runs passed, but a separate OFS invocation using the default
timed out during final delete at the 600-second limit; its diagnostic artifacts
are in [`build/amiga-native-codecs-ofs-default-20261030`](../build/amiga-native-codecs-ofs-default-20261030/commands.log).
The default-path run therefore remains unqualified in this run; the cause was
not isolated. The latest native runner now consumes all independent filename
and refusal/partial expectations from `amiga-native.json`; its Make target
retains complete, timestamped outputs under
[`build/amiga-native-ofs-20261030-224048`](../build/amiga-native-ofs-20261030-224048/manifest.txt)
and [`build/amiga-native-ffs-20261030-224048`](../build/amiga-native-ffs-20261030-224048/manifest.txt).
Both runs passed nested tree transfer, metadata precedence, failed-append
preservation, and expected refusal/partial checks. The native codec corpus is
separate and still uses its earlier records. Other adapted runners still
require durable manifests. The native OFS/FFS interrupted-publication recovery
case now has an independent payload manifest at
[`test/reference/interoperability/amiga-native-recovery.json`](../test/reference/interoperability/amiga-native-recovery.json)
and emits a machine-readable run record with command logs, source snapshot,
environment/input hashes, and hashes for 140 artifacts. Both filesystem runs
passed on 2026-10-30:
[`build/amiga-native-recovery-ofs-20261030-234206/run.json`](../build/amiga-native-recovery-ofs-20261030-234206/run.json)
and
[`build/amiga-native-recovery-ffs-20261030-234206/run.json`](../build/amiga-native-recovery-ffs-20261030-234206/run.json).
Each restored the original archive byte-for-byte, consumed the backup, and
retained the integrity-tested replacement stage. This records deterministic
FS-UAE process termination after a buffer-settle interval; it does not establish
physical power-loss durability.

The stack/depth probe limit was raised to 128 and the native OFS and FFS
interchange corpus passed at 65 nested levels with `--stack 0` (no shell-stack
override). The runner cross-checked the deepest leaf bytes after guest extraction;
the guest reports a 3,200-byte process stack and a 4,096-byte CLI default. Run
records and emulator artifacts are in
[`build/r02-amiga-depth65-ofs-20261030/manifest.txt`](../build/r02-amiga-depth65-ofs-20261030/manifest.txt)
and
[`build/r02-amiga-depth65-ffs-20261030/manifest.txt`](../build/r02-amiga-depth65-ffs-20261030/manifest.txt).
Repeat with `make CC=gcc AMIGA_NATIVE_DEPTH=65
XDFTOOL=build/venv-amiga-tools/bin/xdftool fsuae-native-interoperability-test`.
This raises the qualified depth by one level; maximum supported depth and
additional constrained-memory thresholds are still unmeasured.

Higher-depth diagnostics on 2026-10-31 did not extend that qualification. The
128-level OFS source-creation attempt returned failure and produced no archive;
its deepest member path is 269 characters, so the result cannot isolate a
resource limit from path handling. At 117 levels the OFS source archive was
created, but extraction did not finish before the runner's 600-second FS-UAE
limit; the runner stopped before byte verification, and the sequential target
therefore did not reach FFS. These attempts do not establish a supported
maximum depth. `check_fsuae_native.py` now writes a hash-indexed `failure.json`
with the exception, source/worktree snapshot, environment, inputs, manifest,
and retained artifacts when a run fails after output setup. A controlled
preflight-failure check verified the record and every indexed artifact hash.
The 65-level OFS/FFS pass remains the current deep-traversal qualification.

The independent Amiga fixture now includes a Linux-created symbolic-link
entry. FS-UAE OFS and FFS runs both returned the expected partial result, left
the destination link absent, and preserved the input archive byte-for-byte;
the guest alert text and empty destination listing are retained with the
records at
[`build/amiga-native-ofs-20261030-amiga-link/manifest.txt`](../build/amiga-native-ofs-20261030-amiga-link/manifest.txt)
and
[`build/amiga-native-ffs-20261030-amiga-link/manifest.txt`](../build/amiga-native-ffs-20261030-amiga-link/manifest.txt).
AmigaDOS symlink creation is unsupported and is not claimed.

Native Plan 9 recording remains environment-dependent: this machine has no
usable Plan 9 emulator, per the user's clarification. Do not attempt guest runs
here. Existing 9front results elsewhere in the documentation are historical
evidence, not a fresh or reproducible current-machine validation. Defer native
Plan 9 reruns until a working emulator/runtime is available.

The original QL evidence at `build/i02-i03-sqlux-final-evidence-20261029`
has been checked: all seven archive sizes and hashes, the copied QL executable,
and the reference manifest match its recorded values. Its Linux/mini binaries
were not retained and cannot be independently reconstructed from that record.
The 2026-10-30 sQLux rerun passed and retained eight guest commands, host command
output, the runner and expected-manifest snapshots, worktree status/diff, and
artifact hashes in `build/r02-ql-sqlux-20261030`. The runner and archive hashes
in its manifest were verified. These runs exercise the QDOS-like host filesystem;
they do not qualify a physical QDOS filing system. Plan 9 host-shim checks remain
in scope; native run recording is deferred until a usable emulator/runtime is
available.

The X68000 host-shim Profile 1 runner now retains its generated inputs, archives,
outputs, command log, compiler/environment data, source diff, and hashes. The
2026-10-30 rerun passed 53 recorded subprocess cases against the independent
[`x68k-host-shim.json`](../test/reference/interoperability/x68k-host-shim.json);
all 30 recorded artifact hashes and the manifest hash were verified. Evidence
is in [`build/x68k-host/runs/x68k-profile-r0e813j9/run.json`](../build/x68k-host/runs/x68k-profile-r0e813j9/run.json). This closes the X68000 adapted-run evidence gap only; native Human68k behavior remains unqualified.

The macOS POSIX host runner now reads profile, pre-epoch, epoch-zero, and mode
expectations from an independent JSON manifest and preserves its fixture tree,
command results, compiler/environment, source diff, and hashes. The 2026-10-30
run passed 31 subprocess cases; all 26 artifact hashes were verified. Evidence
is in [`build/macosx-host/runs/macosx-host-x7y85oi_/run.json`](../build/macosx-host/runs/macosx-host-x7y85oi_/run.json). This covers the host POSIX branch only; native macOS APIs remain open.

The Atari FAT12 → Amiga FS-UAE → Atari FAT12 aggregate runner now retains a
top-level run record in addition to the per-system child manifests. The
2026-10-31 rerun passed and indexed 456 artifacts; the record pins both
independent manifests, all three child runs, ROMs, executables, source hashes,
the worktree diff, commands, tool versions, locale/timezone, and artifact
hashes:
[`build/atari-amiga-interoperability-20261031-000405/run.json`](../build/atari-amiga-interoperability-20261031-000405/run.json).
The Atari side uses TOS 1.62 FAT12 under Hatari; the Amiga side uses FS-UAE's
host-directory backend. This record does not qualify native Amiga OFS/FFS; those
are tracked separately under R02/R11.

The standalone Atari GEMDOS host-directory runner also now writes a retained
`run.json`. Its 2026-10-31 TOS 1.62 run passed 55 artifact hash checks and the
independent expected-result manifest; it records guest commands/results,
compiler and Hatari versions, TOS and executable hashes, and the source diff:
[`build/r02-atari-host-20261031/run.json`](../build/r02-atari-host-20261031/run.json).
The run uses Hatari's GEMDOS host-directory backend; it is separate from the
Atari FAT12 evidence above.

The standalone Atari FAT12 runner also retains a hash-indexed `run.json` and
uses a dated output directory under `build/` by default (or `--output DIR` for
an explicit path). Its 2026-10-31 TOS 1.62 run passed the guest transfer, archive
edit, metadata, refusal, and failure-preservation cases; all 53 artifact hashes
and its independent manifest hash were verified. It records the TOS and CLI
input hashes, Hatari/compiler/environment details, commands, source revision
and diff, and the TOS directory-metadata limitation:
[`build/r02-atari-fat12-20261031/run.json`](../build/r02-atari-fat12-20261031/run.json).

The Atari CLI FAT12 fixture now also creates and integrity-tests a Linux v6
archive containing a representable sibling and `payload-🧪.txt`. TOS 1.62's
whole-archive preflight reports `unsafe or unrepresentable Atari name`, returns
failure, and leaves the existing empty output directory untouched. The
independent expectations are in
[`atari-cli-unicode-refusal.json`](../test/reference/interoperability/atari-cli-unicode-refusal.json);
the complete rerun, including all regular FAT12 cases, is
[`build/atari-cli-unicode-20261031/run.json`](../build/atari-cli-unicode-20261031/run.json).

The DOSBox-X CP850 transfer runner now writes to a retained `build/r02-*`
directory by default and emits a machine-readable run record. Its 2026-10-31
rerun passed `Ø.TXT` archive creation and bidirectional extraction on FAT12;
the record pins the independent `dos-cp850-fat12.json` manifest, source and
binary hashes, DOSBox-X/mtools versions, commands, environment, worktree
snapshot, and 16 artifact hashes:
[`build/r02-dosboxx-cp850-wsh5pi75/run.json`](../build/r02-dosboxx-cp850-wsh5pi75/run.json).

The DOSBox-X CP437 FAT12 runner now uses the same retained-record format. Its
2026-10-31 run passed café filename extraction, DOS16 archive creation and
replacement, Linux cross-extraction, and FAT attribute preservation; its
metadata-only loss remains an explicit partial result. The run record pins the
independent `dos16-fat12.json` manifest, environment, commands, worktree
snapshot, and 25 artifact hashes:
[`build/r02-dosboxx-cp437-jgv9qfjp/run.json`](../build/r02-dosboxx-cp437-jgv9qfjp/run.json).

The Linux FLTK GUI integration runner now copies its complete generated fixture
tree from a disk-backed temporary directory into a retained `build/r02-*`
directory and emits a machine-readable record. Its 2026-10-31 full run passed
the production Profile 1 create flow, mini cross-reader, nested and flattened
extraction, cancellation, corruption, injected I/O/metadata failures, and
unsafe-name/path-conflict preflight. The record pins the independent
`linux-fltk-gui.json` manifest, frontend binaries, source state and 39 artifact
hashes; retained artifacts total about 537 MB:
[`build/r02-linux-gui-49ru776e/run.json`](../build/r02-linux-gui-49ru776e/run.json).
The 256 MiB cancellation fixture now uses the configured build filesystem
instead of `/tmp`, which was nearly full tmpfs on this machine.

The Amiga/Linux FS-UAE host-directory runner now emits a machine-readable run
record and defaults to retained output under `build/`. The 2026-10-31 run
passed Stored and BoltLite profile exchange, staged edits, archive protection
and comment preservation, corruption refusal, nested metadata, and all four
codec round trips at 256 KiB blocks. Its run record pins both independent
manifests, ROM/Workbench/toolchain inputs, the worktree snapshot, guest command
status files, and 331 artifact hashes:
[`build/r02-fsuae-host-20261031-1032/run.json`](../build/r02-fsuae-host-20261031-1032/run.json).

- Apply the same independent-manifest and durable-run-record standard to the
  remaining adapted runners whose evidence is temporary, transcript-only, or
  missing expected values. Include source revision and dirty diff,
  compiler/flags, emulator, filesystem, locale/timezone, commands, results, and
  archive hashes. Keep licensed images out of version control and host-filesystem
  evidence separate from guest-native filesystems. Plan 9 native runs remain
  deferred until a usable emulator/runtime exists.
- Check existing QL evidence links and retain a verified copy where referenced;
  a path alone is insufficient if the artifact has disappeared.
- **Validate:** run only the adapted runners. For Amiga use
  `make CC=gcc XDFTOOL=build/venv-amiga-tools/bin/xdftool fsuae-native-interoperability-test`.
- **Done when:** expected values are independent of the archive writer and each
  adapted run produces a complete, retained evidence record for both success
  and expected refusal/partial cases.

### R03 — Close missing mini and Linux GUI lifecycle cases (I03)

**Closed for Linux mini and Linux FLTK on 2026-10-30.** The comparison found
zero-byte file extraction missing from both production fixtures. Independent
empty-file cases now pin bytes, mode, and epoch-zero mtime in the mini/full
interchange runner and the FLTK extraction fixture. Both focused gates passed
with native 32-bit mini execution; the production FLTK gate also passed its
Profile 1, cancellation, checksum, write, flush, close, metadata, and path
preflight cases.

Prior findings: A format-6 blocked payload with an extra byte inside its
declared compressed region was accepted by both full and mini archive tests.
Both readers now enforce that block headers and data consume exactly the
declared compressed size and reject empty subjects with nonempty payloads.
`test_block_payload_boundaries` constructs checksum-correct archives with one
byte added or removed and checks both archive-test outcomes, full extraction
preflight, and mini extraction cleanup. Classic Mac's copied full core is
synchronized. Remaining lifecycle
cases still need the comparison below.

The full core now also checks block framing before archive edits and before
opening extraction output. A Linux append reproducer confirmed the previous
behavior published an archive that failed its own test; the corrected command
refuses it, preserves the original bytes, and cleans its stage. Unit fixtures
cover append, copy, and delete refusal. Framing preflight does not verify the
compressed bytes' checksums; archive test retains that responsibility.
The aggregate `make CC=gcc test` passed on 2026-10-30 with disk-backed `TMPDIR`,
including 160 Linux CLI checks and the frontend host suites. After the empty-file
fixtures were added, `make CC=gcc linuxmini-interoperability-test` and
`make CC=gcc linux-gui-interoperability-test` both passed on 2026-10-30 with
direct native 32-bit mini execution. Core unit tests separately cover empty
archives, dry-run streams, skipped subjects, unsupported supplementary records,
metadata ordering, and hard/partial completion errors. Remaining frontend-specific
lifecycle gaps are assigned to their target rows; R03 is closed for the Linux
mini and FLTK scope. The same mini fixture failed under `qemu-i386` because that
runner's `readdir` returned no directory entries on this host; a minimal probe
confirmed direct 32-bit execution sees the entries. The Makefile now defaults
to direct execution and supports `LINUXMINI_RUNNER=qemu-i386` as an explicit
fallback on hosts where direct 32-bit execution is unavailable.

A follow-up probe found a nonempty, unframed entry with zero compressed bytes
also passed Linux append while failing archive test. Full and mini preflight now
reject that inconsistent size pair before opening output. Unit tests cover
full/mini extraction plus full-core append, copy and delete; a Linux/mini CLI
probe confirms refusal and unchanged archive bytes. This does not validate the
contents or checksum of a nonempty unframed payload during edit preflight.

Haiku host-shim fixes are retained as implementation evidence only; native
Haiku qualification is deferred. The CLI host-shim fixture injects
directory-selection failure over an existing directory with a distinct mode
and MIME attribute. Before the fix, the
following native metadata record changed those attributes despite the failure;
the frontend now delivers metadata only for a selected directory or published
stream. The regression checks the original attributes and error outcome. Haiku CLI
queues directory metadata and finalizes deepest-first through identity-checked
handles. Host-shim fixtures verify nested read-only native modes/MIME/timestamps,
Linux portable mode and epoch zero, dry-run, injected mode and timestamp failures,
and parent symlink substitution without modifying the outside directory. Extraction
now holds the selected output root and uses directory-relative traversal, staging,
symlink creation, cleanup, and publication. Before publishing, it reopens the member's
parent from the root and compares its identity with the held parent descriptor. The
optional parent-swap interposer verifies the extraction reports failure, leaves the
outside sentinel unchanged, and removes the stage from the parked directory. Systems
without safe fd timestamp support report partial; native Haiku/BeOS filesystem
qualification remains deferred R12.
The host-shim regression and retained files are in
`build/haiku-dir-order-final-20261030`. Mode and timestamp restoration failures
both report partial while retaining verified children. The finalizer checks
device/inode on an open directory descriptor and uses that descriptor for
mode, MIME and timestamps, so the tested parent replacement does not redirect
those operations. The host shim does not qualify native BFS semantics. Network-enabled frontend
syntax was checked with the same host shim.

Haiku CLI now restores portable regular-file modes on verified stages, strips
set-ID bits with a partial result, and retains the published inode descriptor
for subsequent native metadata. The host fixture verifies epoch-zero timestamps,
failure reporting, native mode/MIME overriding a portable mode of 0000, and a
published-path substitutions both immediately after publication and during
metadata restoration. Both now report partial without changing the outside
target, including for archives without native metadata. A failed descriptor
duplication also reports partial while retaining verified bytes. The parent-directory substitution gap was subsequently closed in the host-shim
implementation using root- and parent-descriptor-relative traversal; native
Haiku qualification is deferred by user decision.
The host-shim evidence is in `build/haiku-file-identity-final-20261030`.

The Haiku CLI's symlink stage previously could publish a temporary regular file
after a short target read, replacing an existing destination despite reporting
partial. It now requires the exact archived length, checks EOF/read errors and
successful close, and refuses oversized targets before publication. The
host-shim fixture injects short reads, trailing bytes, EOF read errors and close
failures for both existing and absent destinations, plus a checksum-correct
4,096-byte target; evidence is retained in
`build/haiku-link-failure-final-20261030`. Native BFS qualification is deferred by user decision. Stage and parent operations
now use held directory descriptors in the host-shim implementation.

Haiku CLI symlink creation already stored a valid no-follow source mtime, but
extraction silently discarded it. The CLI now sets the staged symlink's time
with `utimensat` and `AT_SYMLINK_NOFOLLOW` before publication. Host fixtures
verify target contents and timestamps stay unchanged; injected timestamp errors
leave existing and absent destinations unchanged and clean the stage. Unknown
times are not applied. Unrepresentable times or unavailable no-follow timestamp
APIs publish the verified link with a partial result. The fallback build and
network-enabled syntax path were checked on the host; native Haiku/BeOS support
is deferred and remains unqualified. Retained fixture evidence is in
`build/haiku-link-time-complete-20261030`.

The Haiku CLI parent-directory race is now closed in the host-shim implementation.
Member paths are resolved under an open output-root descriptor; stages and symlinks
are created relative to a held parent descriptor, and publication revalidates that
the selected path still resolves to that same parent. The default Haiku host-shim
suite passed, as did the opt-in adversarial run:
`BP_TEST_HAIKU_PARENT_RACE=1 python3 test/check_haiku_host.py --cc gcc
--linux-boltpack build/linux/boltpack --output build/haiku-parent-race-final`.
The injected parent swap is rejected before publication; the outside sentinel is
unchanged and no stage remains in the moved directory. Existing host race fixtures
were adjusted to intercept `openat`/`renameat`/`utimensat`, matching the descriptor-
relative implementation.

If R12 is reopened after a Haiku toolchain/runtime and BFS volume become
available: build CLI, network-enabled CLI, and BeAPI GUI; run the host fixture's
parent swap, replacement, symlink timestamp, native MIME, mode, epoch-zero,
format-6/Profile 1, and Unicode/case-collision cases; then add an independent
manifest. Validate BeOS separately only if it returns to scope.

- Compare existing mini-core and production Linux FLTK fixture cases with F1
  and F3. List only uncovered outcomes: empty subjects, dry-run, skip,
  unsupported entry, metadata precedence, cancellation, and metadata errors.
  Existing CRC/close failures, epoch-zero, and directory ordering passes stand.
- Add a regression for each genuinely missing outcome; fix production behavior
  when it fails. Assert existing destination bytes survive failure, skipped or
  failed subjects receive no supplementary metadata, and errors/partial status
  match the contract. Verify deferred directory restoration after children.
- **Validate:** relevant core units, `linuxmini-interoperability-test`, or
  `linux-gui-interoperability-test` according to the path changed.
- **Done when:** each applicable lifecycle outcome has explicit evidence, or
  a named target dependency. Update individual ledger areas only to that scope.

### R04 — Finish 16-bit heap and stack accounting (I05)

**Updated 2026-10-30:** `make CC=gcc dos16-interoperability-test` passed,
including 20 DOS16 core cases, all 26 allocation-failure injections, the table,
projected-index, and queue bounds, and DOS16-to-Linux transfer. The instrumented
`make CC=gcc dos16-stack-interoperability-test` passed after adding a source
recursion cap and deeper stack probes in BoltLite search/compression, BoltLZ
decode, and DOS16 extraction callbacks: 34 directory frames succeed, the first
deeper tree is refused, and no final archive or stage remains. The accepted
deep-tree run observes 3,600 bytes available; the refused workload records
3,596 bytes, and the lowest sample across the corpus is 3,594 bytes. The DOS
manager's largest-block low-water is 358,784 bytes. The
instrumented Watcom `_msize()` observations report a largest requested block
of 65,502 bytes and a 163,286-byte peak of tracked live-usable allocations in
the maximum writer workload. The tracker follows alloc/free/realloc without a heap table, but
excludes allocator headers and runtime allocations. Cumulative per-command
totals count API calls, including every reallocation, and are not simultaneous
usage. These are workload measurements, not worst-case proofs or guaranteed
Watcom allocator capacity. The retained run data is
`build/dos16-stack/resource-low-water.txt`.

The DOS16 runner now also extracts 126 queued empty directories and a leaf
file, exercising the 128-name projected-index bound. The full tree and exact
payload pass under DOSBox. The recorded extraction sample has 73,264 bytes peak
tracked live usable allocations, 6,759 bytes sampled stack low-water, and a
429,808-byte DOS-manager largest-block low-water. Directory-time loss is
accepted only as the documented partial result after verifying every output.
This measures a realistic metadata-queue extraction workload; it does not close
the allocator-header, full worst-case call-chain, or Win16/OS/2-16 runtime gaps.
The same instrumented run integrity-tests the 1,213-entry `LIMIT.BLT` resource
fixture. It passes with a 24,520-byte tracked live-usable high-water and 7,462
bytes sampled stack low-water. The reader streams those entries. The new
`WRITMAX.BLT` probe holds the maximum 65,502-byte writer table and retained
names while compressing a 32 KiB file with BoltLite's 36,880-byte output buffer
and 18,432-byte cached workspace. It records a 163,286-byte tracked live-usable
peak, emits 1,213 entries, and Linux validates the archive. This is a workload
measurement rather than a general minimum-memory guarantee. The archive and
hash/worktree run record are retained at
`build/dos16-stack/WRITMAX.BLT` and
`build/dos16-stack/writer-resource-run.json`. The target depends on
`unit-test-dos16` for the entry-bound fixture.

On 2026-10-31, the instrumented run added maximum-entry `list`, `info`, and
verified-backup recovery workloads. Listing/info sampled 7,510/7,394 bytes of
stack availability; recovery sampled 6,911 bytes, integrity-tested and
restored the 1,213-entry archive byte-for-byte, and consumed the backup. The
overall sampled minimum remains 3,594 bytes. Results are in
[`build/dos16-stack/resource-low-water.txt`](../build/dos16-stack/resource-low-water.txt)
and the reproduction is `make CC=gcc dos16-stack-interoperability-test`.

- Maximum writer-table and codec-buffer peak measurement is now covered by
  `WRITMAX.TXT`. Keep measurements workload-specific; do not treat them as a
  worst-case heap or call-chain proof. `_msize()` reports usable user blocks,
  not allocator headers. Allocator metadata and DOS runtime allocations remain
  outside the measurement. The 26 allocation-failure cases and 8 KiB stack
  baseline still pass.
- Derive checked limits from the memory model and simultaneous live allocations.
  The DOS16 source walker now caps at the measured 34-frame boundary; its 35th
  frame is refused before recursion. Maximum-entry create, extract, test, list,
  info, and recovery paths now have workload samples; stack use between probes
  and other compiler/runtime configurations remain unqualified. Label
  measurements by workload rather than treating them as worst-case proofs.
- Update [resource accounting](16-bit-resource-accounting.md) with the derivation,
  measured headroom, enforced limits, and failure outcomes. Carry Win16/OS2-16
  runtime measurements into R10; DOS results do not qualify their runtimes.
- **Validate:** focused DOS16 units, allocation failures, and
  `dos16-stack-interoperability-test`; cross-build affected mini targets.
- **Done when:** DOS16's enforced bounds, measured writer/reader workloads, and
  clean exhaustion behavior are recorded, with Win16/OS/2-16 runtime work
  explicitly assigned to R10. Watcom allocator-header/runtime overhead is an
  explicit measurement limit, not an inferred free-memory guarantee. I05 stays
  open until R10 supplies the other 16-bit runtime measurements.

### R05 — Resolve remaining source/destination race exposure (I02/I03)

- **Concurrent-change contract:** extraction must keep every created or
  published object beneath the selected output root even if a parent pathname
  is replaced during extraction; it must preserve an existing destination if
  verification or publication fails. Where source enumeration/read checks can
  detect substitution, archive creation must fail without publishing the new
  archive. These checks do not promise an atomic filesystem snapshot, defeat
  CRC collisions, or detect writes after final validation.
- Map the documented transient source substitution and destination-parent risks
  to each affected adapter, including pathname-based POSIX wrappers, Windows,
  Atari, Amiga, and Human68k. State the required concurrent-change behavior
  before selecting an implementation.
- For each reproducible gap, add a deterministic substitution fixture and use
  held directory/subject handles or the platform equivalent. Cocoa now refuses
  extraction before output on macOS 10.9, where descriptor-relative traversal is
  unavailable. QNX, older/unidentified AIX, HP-UX, IRIX, NeXTSTEP, SCO, and
  Tru64 builds refuse non-dry-run extraction before output creation; the host
  regression proves refusal and dry-run behavior. Continue auditing the
  non-POSIX adapters and target-specific feature gates; where an operation
  cannot meet the safety contract, implement an explicit safe refusal rather
  than documenting exposure.
- **Mapped status:** Linux full/mini extraction and the Haiku host shim use
  held directory handles with substitution regressions; Cocoa uses this path
  on macOS 10.10+ and refuses extraction on 10.9. BSD, Hurd, Solaris/illumos,
  AIX 7.3-selected, and z/OS-selected POSIX wrappers compile the dirfd path,
  with target API/runtime qualification still assigned to R15. QNX, older or
  unidentified AIX, HP-UX, IRIX, NeXTSTEP, SCO, and Tru64 refuse real
  extraction; `make posix-path-extract-refusal-host-test` verifies refusal
  before output creation and that dry-run remains available. Atari GEM output
  stages capture device, size, and modification time from the open descriptor
  and require those values to match the pathname before publication and cleanup.
  File timestamps are restored after publication. TOS 1.62 production flow passes
  on GEMDOS host storage (`build/atari-gui-create-1q0nwqmg`) and FAT12
  (`build/atari-gui-create-sqsj8mkr`); the corrected FAT12 runner verifies no
  stage remains after a read-only-target failure. Atari extraction now re-reads
  a closed stage and verifies its archived byte count and CRC before commit.
  Hatari/TOS 1.62 replaced the live stage with different same-size bytes and a
  matching mtime during a 1 MiB extraction; the GUI rejected publication,
  preserved the old destination, and left the substituted stage untouched
  (`build/atari-gui-cancel-9faa1pzs`). A prior attempt ended before extraction
  completed and is explicitly marked invalid at
  `build/atari-gui-cancel-m7b5nnof/run.json`. The GEM GUI now pins stage-parent
  and destination-root directory handles when `dirfd()` returns a descriptor,
  then removes the stage and journal relative to those held handles. When no
  descriptor is available, it uses the existing checked pathname cleanup path.
  A FreeMiNT
  runtime probe confirms `fchdir(dirfd(...))` and relative unlink work, and
  that rename of an open parent is refused; the Hatari/TOS host-drive fixture
  confirms the GUI refuses a substituted parent and warns that cleanup may
  leave the stage/journal because TOS reports `opendir()` as `ENOSYS`. Native
  FreeMiNT GUI qualification remains in R06. CRC collisions and a substitution
  after final content validation remain open. Atari CLI/GEM race cases remain
  in R06/R13. Human68k
  pathname-based races remain in R13.
  The Amiga CLI now snapshots the output root, each staged file's parent, and
  the staged-file inode. It refuses publication and deferred file/directory
  metadata changes when those identities differ. `fsuae-parent-substitution-test`
  pauses after opening a staged extraction file, replaces `OUT`, and confirms
  the replacement sentinel is preserved and no file is published there; the
  open stage remains in the renamed original directory because pathname cleanup
  cannot safely reach it after substitution (`build/r05-amiga-parent-substitution-postfix-20261030`).
  A fresh rerun after the cleanup investigation passed at
  [`build/r05-amiga-parent-substitution-evidence-20261030`](../build/r05-amiga-parent-substitution-evidence-20261030/run.json):
  its retained manifest pins the test binary, source diff, emulator/assets,
  replacement sentinel, and the single 1 MiB orphaned stage by SHA-256.
  Holding an AmigaDOS
  parent lock did not provide a safe deletion route on the FS-UAE host drive,
  so cleanup remains an explicit limitation; the runner requires the stage to
  remain with the moved original rather than risk deleting a replacement file.
  This closes the tested redirection case, not the Amiga path-API check/use window.
  A production FS-UAE probe now swaps the source pathname to a same-size
  substitute and restores the original while the original file is open but
  before it is read. The CLI accepts this transient path change; the retained
  archive passes Linux integrity and byte-exact extraction against the original
  1 MiB source. The open AmigaDOS stream pins the original inode, so this
  after-open pathname swap did not change the archived bytes. Evidence is
  [`build/r05-amiga-source-swap-restore-20261030-234624/run.json`](../build/r05-amiga-source-swap-restore-20261030-234624/run.json).
  This does not observe a path change that is fully restored before a pathname
  check, but the tested after-open case preserves the object already opened.
  The 2026-10-30 nested-tree rerun exposed an AmigaDOS filesystem behavior:
  `lstat()` on a still-open nested stage can fail with `ETXTBSY`, causing the
  stage to be discarded and the file omitted. The CLI now snapshots the inode
  through the `mkstemp()` handle, closes the completed stage, then verifies its
  pathname identity before publication. The full nested OFS and FFS runs pass
  with exact file data and native metadata; retained records are
  [`build/amiga-native-ofs-20261030-224048`](../build/amiga-native-ofs-20261030-224048/manifest.txt)
  and [`build/amiga-native-ffs-20261030-224048`](../build/amiga-native-ffs-20261030-224048/manifest.txt).
  The DJGPP DOS publisher now uses the native DOS LFN rename API, with the
  classic DOS rename fallback, so a concurrent target appearing at publication
  is not overwritten. DOSBox-X FAT12 verifies refusal and preservation of both
  source and destination; the full DJGPP CLI transfer/edit suite passes with
  this helper (`build/r10-djgpp-dosboxx-noreplace-20261031-verified/manifest.json`).
  This closes the tested target-overwrite case only; DOS parent substitution,
  other DOS runtimes/filesystems, and OS/2 races remain in R10. Windows reparse/path races remain
  in R08 and native Amiga GUI
  replacement behavior in R11. Plan 9 real
  extraction now refuses before output creation because its frontend only has
  pathname-based operations; `make CC=gcc plan9-host-test` verifies refusal,
  existing-destination preservation, dry-run, create/add, and reader
  interoperability. Native Plan 9 reruns remain deferred. Haiku native
  validation remains deferred under R12 by user decision.
- Preserve the Linux CRC re-read regression and its stated limits: CRC collisions
  and mutations after final validation are not eliminated. Do not claim an atomic
  filesystem snapshot from these checks.
- **Done when:** every known exposure has a fix/refusal plus matching evidence,
  or a specific target test assigned to R08–R15. Native-only cases remain open.

### R06 — Qualify the shipped Atari GEM GUI (I02/I03/I06)

**Progress:** the production Create dialog defaulted to “More,” so Return reopened
source selection. It now defaults to “Create.” `make
CC=gcc HATARI_TOS=/home/tony/images/Atari/tos162se.img atari-gui-create-test`
passed on Hatari/TOS 1.62: it runs the shipped create/save flow, captures a
screenshot and run record, and verifies a manifest-pinned Atari/68k v6 archive
through Linux full and mini integrity/extraction. Both readers recover exact
bytes and report partial for Atari metadata. The repeatable runner selects
`TREE`, which contains two files, a nested directory, and an empty directory,
then drives production create/save and Ctrl+E extraction into `OUT`.
`atari-gui-create-fat12-test` seeds `TREE/src.txt` with Archive, Read-only,
Hidden, and System attributes; all four survive GUI create/extract. The files
and directories also extract exactly, including the empty directory. That initial
TOS 1.62 FAT12 run left all three extracted directory timestamps at the FAT
default `1990-01-01 0:13` instead of the source `2026-10-30 10:48` and displayed
the partial-extraction alert. Later full FAT12 runs on TOS 1.06, 1.62, and 2.05
restored directory times at FAT12 resolution. The earlier FAT12 run, including that alert and
per-directory timestamp evidence, is retained under
`build/atari-gui-create-7h0qxv97`; the earlier host-directory run is under
`build/atari-gui-create-9zvjpbze`. The later Escape-probe runs are under
`build/atari-gui-create-4l70_lo0` and `build/atari-gui-create-nrbkhzlb`. Escape
did not cancel the selector: the captured screen still showed it open, so those
later runs do not qualify destination-selector cancellation or their following
collision flow. The runner no longer treats Escape as cancellation. The corrected
runner opens a `PREFIX.BLT` fixture containing
`node` and `node/child` in a separate Hatari boot. On both host-directory and
FAT12 drives, the production GUI displays its archive-name collision alert and
the precreated `PREFIX` destination remains empty. Current evidence is under
`build/atari-gui-create-0k0bifb_/prefix-run` (host directory) and
`build/atari-gui-create-qmi4zzf8/prefix-run` (FAT12). Earlier runs at
`7h0qxv97` and `9zvjpbze` show refusal of the `Alpha.txt`/`alpha.TXT` case
collision and empty output. The later Escape runs at `4l70_lo0` and
`nrbkhzlb` are not collision evidence because the selector remained open.
`make unit-test` also verifies the shared Atari name tree rejects file/child
conflicts in either insertion order. The earlier TOS 1.62 timestamp-loss
observation was superseded by the later full FAT12 corpus: TOS 1.06, 1.62, and 2.05 now restore directory timestamps at
FAT12 resolution. Directory Hidden/System loss remains a verified partial
outcome. A separate Xvfb mouse test now clicks the selector's
Abort button after opening a valid archive for extraction. On TOS 1.06, 1.62,
and 2.05 the selector closes to the archive window and the existing `OUT`
directory remains empty; before/after screens and ROM/build hashes are retained
at `build/atari-gui-cancel-nt0i02xe`, `build/atari-gui-cancel-4fh3gd05`, and
`build/atari-gui-cancel-2fbw7t_i`. `make atari-gui-cancel-test` reproduces this
with a window-relative xdotool click. This qualifies selector cancellation by
the GUI Abort button; Escape still does not cancel on the tested selector.
`make atari-gui-active-cancel-test` now drives the production Cancel button on
TOS 1.06, 1.62, and 2.05 during a 64 MiB stored-file extraction. Evidence at
`build/atari-gui-cancel-n5_73lb5`, `build/atari-gui-cancel-l_60i0ez`, and
`build/atari-gui-cancel-dv4mcp6n` shows the active progress panel, 32 KiB in
the staged file before cancellation, the cancellation alert, unchanged
pre-existing destination bytes, and removal of the staged file on all three
ROMs. The first TOS 2.05 attempt (`build/atari-gui-cancel-prkly7w8`) did not
register a brief click before the progress callback; the runner now holds the
mouse button until Hatari exits, and the repeat passed. Active cancellation is
qualified on all three tested TOS versions. A FAT12 fixture now
sets Hidden/System on an empty source directory. The archive contains native
and portable metadata records, but TOS 1.06, 1.62, and 2.05 leave both flags
unset on the extracted directory and report partial. Evidence is under
`build/atari-gui-create-5qmqfsxj`, `build/atari-gui-create-m2sg92dk`, and
`build/atari-gui-create-p8exo01f`. This qualifies directory-attribute loss as a
partial outcome on all three tested TOS versions. The production GUI also replaced two pre-existing files with exact archive
bytes and left no stage/recovery names on TOS 1.62, on both FAT12
(`build/atari-gui-create-lymdqxl8`) and GEMDOS host-directory
(`build/atari-gui-create-uukaajwm`) targets. A TOS 1.62 FAT12 run now also forces a publication failure by making the
existing `OUT/TREE/src.txt` read-only. The production GUI reports incomplete
extraction, preserves the sentinel bytes and READONLY attribute, and leaves
no stage or recovery entry (`build/atari-gui-create-msxh3kx1/failed-replacement`).
This qualifies the failure before the old target is renamed. A dedicated test
build now injects a stage-publication failure only after
the existing target has been moved to and verified at its backup path. On
Hatari 2.5.0/TOS 1.62, `make CC=gcc HATARI_TOS=... atari-gui-publish-rollback-test`
restores the original bytes exactly and removes both the failed stage and
backup (`build/atari-gui-rollback-oat0i71h/run.json`). The failure is injected
before the actual stage rename; this verifies successful rollback, not a real
device error. A separate injected rollback failure creates a competing target;
the original remains byte-for-byte at its retained backup path, the stage is
cleaned, and the GUI alert displays the recovery filename
(`build/atari-gui-recovery-s6dvbir7/run.json`). The new `atari-gui-media-error-test` causes Hatari floppy write protection to reject the real stage-to-target GEMDOS rename after the old target has moved to backup. On Hatari 2.5.0 with EmuTOS 1.4 and FAT12, the injected publication fails with `commit_rc=-1 committed=0`, retaining the exact old target at backup, exact replacement at stage, and journal. The production retry then restores the original target, publishes the exact archived replacement, and clears the journal, backup, and stage. Its refreshed run record, pinned to commit `3769da8`, includes a 20-file size/SHA-256 artifact index and verifies every indexed hash: [`build/atari-gui-media-error-3keon0b7/run.json`](../build/atari-gui-media-error-3keon0b7/run.json). This real device-error path is qualified on EmuTOS; TOS 1.62's Swedish retry/abort modal remains unautomated. On 2026-10-31, repeated X11 clicks did not reach the test error callback: the original remained at backup, the stage and journal remained, and `ERROR.TST` was absent. The initial frame shows the modal, but later frames show only extraction progress. The 54-artifact diagnostic is [`build/atari-gui-media-error-tos162-input-20261031/run.json`](../build/atari-gui-media-error-tos162-input-20261031/run.json); do not count it as a pass. Follow-up probes mapped the old window-relative click to the dialog text, then tried a translated Abort-button click and Swedish A+Return input; neither reached `ERROR.TST`. Their indexed failure records are [`build/atari-gui-media-error-8s8elbg0/failure.json`](../build/atari-gui-media-error-8s8elbg0/failure.json), [`build/atari-gui-media-error-92_6x_0q/failure.json`](../build/atari-gui-media-error-92_6x_0q/failure.json), and [`build/atari-gui-media-error-6r19pga9/failure.json`](../build/atari-gui-media-error-6r19pga9/failure.json); the last sends A without Return. TOS 1.62 prompt input remains unqualified. A follow-up diagnostic calibrated the click from the captured screenshot, enabled Hatari mouse grab, tried relative mouse movement, and sent Escape; none reached the production error callback. The six input attempts and hash-indexed screenshots are retained in [`build/atari-gui-media-error-input-followup-20261031/run.json`](../build/atari-gui-media-error-input-followup-20261031/run.json); do not count them as a pass. The extraction open callback writes the checksummed `BPRECOVR.JRN` before streaming a regular-file stage. This lets retry distinguish an untouched old target from a backup-move transaction; the publisher reuses that journal while moving the existing target. A test-only pause kills Hatari/TOS 1.62 after the old target has moved; the recovery run restores exact original bytes, removes the fully verified stage, clears the journal, and then the normal GUI extracts the replacement. The latest `atari-gui-interrupt-after-backup-test` rerun passed at `build/atari-gui-interrupt-backup-6q_jm0mo`. The host-drive mid-stage kill also passed again: retry clears the journal, restores exact archived bytes, and preserves the unverified partial `BP` stage (`build/atari-gui-cancel-ywuowb13/run.json`). A separate TOS 1.62 FAT12 production recovery fixture seeds the equivalent durable pre-publication state with a 32 KiB payload prefix; retry preserves the old partial stage, clears the journal, and extracts exact bytes (`build/atari-stage-recovery-g3763geu/run.json`). FAT12 stage cleanup is deliberately not claimed because portable path identity is not reliable enough to delete an incomplete file. Automatic target recovery after both interruption windows is now qualified; safe cleanup of incomplete stages remains open. The recovery record's target, stage, and backup paths now receive component-wise validation before use: parent components, root-prefix lookalikes, volume changes, and trailing separators are refused, while GEMDOS case and slash variants are accepted. `make CC=gcc atari-path-test` passes 31 cases, including these containment checks; the command, hashes, worktree record, and source diff are retained at [`build/atari-path-test-recovery-containment-20261030/run.json`](../build/atari-path-test-recovery-containment-20261030/run.json). The production GUI also rejects a correctly checksummed journal whose target, stage, and backup all escape through `..`; Hatari/TOS 1.62 confirms the outside target and backup remain byte-exact and extracts nothing. `make CC=gcc HATARI_TOS=/home/tony/images/Atari/tos162se.img atari-gui-journal-path-test` retains the run and alert screenshot at [`build/atari-journal-path-d0cih7au/run.json`](../build/atari-journal-path-d0cih7au/run.json). Safe cleanup of incomplete pre-publication stages and the Hatari host-drive parent-redirection cleanup limitation remain open. `make CC=gcc HATARI_TOS=/home/tony/images/Atari/tos162se.img
atari-gui-stage-substitution-test` now runs a deterministic same-size/same-mtime
replacement during the production 1 MiB stored extraction on Hatari 2.5.0 / TOS
1.62. The runner holds a hard link to the original open stage, atomically
replaces the stage pathname with different bytes, synchronizes the mtime, and
requires the original stream to finish. Production commit re-reads the staged
path and compares its length and CRC with the archive before publishing. The
GUI reports partial, retains the old destination byte-for-byte, and leaves the
substituted stage in place. Both original and replacement reach exactly 1 MiB
with equal mtimes. Evidence is under `build/atari-gui-cancel-9faa1pzs`; the
first attempt is marked invalid because the original stream had not finished.
After adding staged-content verification, the complete production FAT12 GUI
create/extract, attribute, collision, and read-only-failure flow passed again on
TOS 1.62 at `build/atari-gui-create-5l4vrz2o`.

The full TOS 1.62 FAT12 create/extract, metadata, collision, and
read-only-replacement corpus passed against the current worktree on 2026-10-30.
The runner now also seeds a pre-existing `ARCHIVE.BLT`, verifies production GUI
creation replaces its sentinel, and checks the FAT12 root for leftover `BP`/`BR`
stages or backups. This shared commit-path fallback passed on TOS 1.62 at
[`build/atari-gui-create-38b7cdwb/run.json`](../build/atari-gui-create-38b7cdwb/run.json). The latest run verifies exact payload bytes, all four file
attributes, and all three directory timestamps at FAT12 resolution; directory
Hidden/System restoration remains unsupported and is reported partial. It also
verifies empty output on case-fold and file/directory-prefix collisions,
preservation of a read-only existing target, and Linux full/mini integrity plus
exact extraction. The latest TOS 1.62 run record and artifacts are in
[`build/atari-gui-create-7wyonunv/run.json`](../build/atari-gui-create-7wyonunv/run.json); the same corpus passed on TOS 1.06 at
[`build/atari-gui-create-kk7vsdmo/run.json`](../build/atari-gui-create-kk7vsdmo/run.json) and TOS 2.05 at
[`build/atari-gui-create-jz0atcvj/run.json`](../build/atari-gui-create-jz0atcvj/run.json); all three ROMs restore directory timestamps at FAT12 resolution. The preceding TOS 1.62 rerun is at
[`build/atari-gui-create-ugv8qel7/run.json`](../build/atari-gui-create-ugv8qel7/run.json).

The same production FAT12 GUI corpus initially failed on EmuTOS 1.4: file
bytes published, but file-attribute metadata was skipped and the recovery
journal remained in `OUT`. EmuTOS returns a directory stream from `opendir()`
while `dirfd()` returns `-1`; cleanup had treated that stream as a held
directory handle and stopped after publication. Directory pinning now keeps
only streams with a usable descriptor and otherwise selects the existing
checked pathname cleanup path. The EmuTOS rerun passed the full create/extract,
file-attribute, directory timestamp, collision, read-only replacement, and
Linux reader checks, and verifies no journal or stage names remain in `OUT`.
Evidence is retained at
[`build/atari-gui-create-emutos-fixed/atari-gui-create-17o5weao/run.json`](../build/atari-gui-create-emutos-fixed/atari-gui-create-17o5weao/run.json).
The strengthened output-root journal check also passed on TOS 1.62 at
[`build/atari-gui-create-tos162-fixed/atari-gui-create-5434qcwl/run.json`](../build/atari-gui-create-tos162-fixed/atari-gui-create-5434qcwl/run.json).
The directory-attribute limitation is now isolated with a focused GEMDOS probe
on TOS 1.06, 1.62, 2.05, and EmuTOS 1.4: `Fsfirst` and `Fattrib` read report
directory, Hidden, and System bits (`0x16`), while `Fattrib` set returns `-36`
and leaves the bits unchanged. These targets refuse setting the directory
flags; the GUI therefore reports their loss as partial. Probe ROM/result hashes are retained
in [`build/atari-attr-probe/run.json`](../build/atari-attr-probe/run.json).
The FreeMiNT held-directory cleanup probe passed again after this change at
[`build/freemint-probe/production-cleanup-y6rc5iil/run.json`](../build/freemint-probe/production-cleanup-y6rc5iil/run.json), preserving descriptor-relative cleanup where supported.

Out-of-range timestamp metadata is now qualified on TOS 1.62 for both files
and deferred directories. The repeatable
`make CC=gcc HATARI_TOS=/home/tony/images/Atari/tos162se.img
atari-gui-time-range-test` extracts a Linux-created archive whose file and
directory mtime are valid 2,208,988,800-second values (2040-01-01), beyond
mintlib's signed 32-bit `time_t`. The production GUI displays its
partial-extraction alert, preserves the complete tree and exact 77-byte
payload, and leaves no stage or recovery journal. The latest run verifies both
primary entries carry `MTIME_VALID`; screenshot, archive and source hashes,
command, and result are retained at
[`build/atari-gui-time-range-9_lplrth/run.json`](../build/atari-gui-time-range-9_lplrth/run.json).
This verifies the unsupported-time outcome; incomplete pre-publication stage
cleanup remains unresolved where the filesystem cannot prove stage identity
after restart.

The new `atari-gui-unicode-refusal-test` covers the filename limit. Linux
creates and integrity-tests a v6 archive containing an ordinary sibling and
the U+1F9EA name `payload-🧪.txt` from the independent
[`atari-gui-unicode-refusal.json`](../test/reference/interoperability/atari-gui-unicode-refusal.json)
manifest. Hatari 2.5.0/TOS 1.62 opens it, displays the production
“Archive name cannot be represented” alert, and leaves the pre-existing empty
`OUT` directory empty, so the representable sibling is not partially
published. The retained host-directory run records ROM, GUI, Linux CLI,
runner, manifest, archive, screenshot and log hashes at
[`build/atari-gui-unicode-k9_m68vd/run.json`](../build/atari-gui-unicode-k9_m68vd/run.json).
This qualifies Hatari GEMDOS host storage on TOS 1.62; FAT12 and other TOS
versions are not inferred from this run.

The parent-path probe `make CC=gcc
HATARI_TOS=/home/tony/images/Atari/tos162se.img
atari-gui-parent-substitution-test` redirects `OUT` to a separate directory while
the stage is open. On Hatari/TOS 1.62, staged-path validation refuses publication
and preserves both target sentinels. The complete staged file and checksummed
`BPRECOVR.JRN` remain in the moved directory because pathname cleanup cannot
reach them after redirection. The GUI now displays a cleanup warning naming either the leftover stage or recovery journal; the production
screenshot, exact target hashes, stage hash, and journal hash are retained in
[`build/atari-gui-cancel-45ugmhan/run.json`](../build/atari-gui-cancel-45ugmhan/run.json).
The fixture was rerun against the latest GUI changes on 2026-10-30; it passed
again with both target sentinels preserved, a 1 MiB orphaned stage, its journal,
and the user-visible cleanup warning recorded in
[`build/atari-gui-cancel-b79z343g/run.json`](../build/atari-gui-cancel-b79z343g/run.json).
After the held-directory cleanup change, the parent-substitution test passed
again; the screenshot now reads “Temp file or journal may remain” and names the
orphaned stage. Its hashes and output state are in
[`build/atari-gui-cancel-m199fiop/run.json`](../build/atari-gui-cancel-m199fiop/run.json).
A TOS probe confirms `lstat` follows the host symlink and `opendir()` returns
`ENOSYS`, so parent-chain symlink checks do not establish safety under Hatari
host-drive emulation. The FreeMiNT runtime probe below qualifies the required
directory-handle calls, while cleanup on ordinary TOS filesystems remains
unqualified. A FreeMiNT 1.18.0 runtime probe now boots from an ACSI FAT16 image under
Hatari 2.5.0 with EmuTOS 1.4 and 4 MiB RAM. It confirms `fchdir(dirfd(...))`
and relative unlink work for both the held stage-parent and output-root handles;
stage and journal cleanup succeed and a sentinel remains intact. With both
handles open, FreeMiNT refuses to rename the output directory, so this probe
confirms the target prevents the tested parent move while handles are held; it
does not exercise move-and-recreate redirection or run the production GUI on
FreeMiNT. Results, hashes, screenshot and limitations are retained in
[`build/freemint-probe/run-cleanup-20261030.json`](../build/freemint-probe/run-cleanup-20261030.json).
The production GUI's held-directory cleanup passes the Hatari/TOS
parent-substitution test. A test-only entry point built from the production
`src/frontend/atari_gui/main.c` now runs the exact stage/journal cleanup helpers
under FreeMiNT. `make CC=gcc
EMUTOS_ROM=/path/to/emutos.img
FREEMINT_ROOT=/path/to/freemint atari-gui-freemint-cleanup-test` passed on
Hatari 2.5.0, EmuTOS 1.4, FreeMiNT 1.18.0, 4 MiB RAM, and an 8 MiB ACSI FAT16
image. It created a stage, journal, and sentinel; pinned the parent/root
handles; confirmed rename refusal while pinned; removed stage and journal
relative to the held handles; and verified the sentinel and absence of stale
warnings. The complete run record and image are retained at
[`build/freemint-probe/production-cleanup-o2_wi0dj/run.json`](../build/freemint-probe/production-cleanup-o2_wi0dj/run.json).
This qualifies the cleanup helper path on FreeMiNT, not the interactive GEM
application or move-and-recreate redirection. The runner requires an EmuTOS
ROM and FreeMiNT boot tree supplied through those variables.

A fresh production-GUI launch probe booted FreeMiNT 1.18.0 and reached the
XaAES loader with EmuTOS 1.4/STE/4 MiB. With the 68000 `xaaes000.km`
copied to the loader’s expected `xaaes.km` name and the required XaAES
resources present, the loader still reports `Fcntl(KM_RUN) failed` with
error `-25`, then falls back to a missing `sh`; BoltPack never opens. The
cause is not isolated, so this is not GUI qualification. The boot image,
XaAES log screen, exact assets and hashes are retained in
[`build/freemint-prod-gui-xaaes000/run.json`](../build/freemint-prod-gui-xaaes000/run.json).

A separate replay of the retained FreeMiNT/EmuTOS ROM-AES image used the
production PRG, archive, `Ctrl+O`/`Ctrl+E` input sequence, and the same Hatari
configuration. Hatari returned to the EmuTOS desktop; the GUI did not open and
`OUT` remained empty. The hash-indexed failure record and screenshot are in
[`build/freemint-recheck/run.json`](../build/freemint-recheck/run.json). This
does not replace or qualify the FreeMiNT cleanup-helper test. Do not count the
ROM-AES route as an interactive GEM pass until the application visibly opens
and the extracted payload is independently verified. No further retry is
planned without a new boot or application-launch diagnostic.

The same production GUI FAT12 create/extract flow passes on TOS 1.06; the
original run is `build/atari-gui-create-6nw6brzd/run.json`. Fresh complete
reruns on TOS 1.06 and 2.05 now also pass the same corpus and restore directory
timestamps at FAT12 resolution; records are `build/atari-gui-create-kk7vsdmo/run.json`
and `build/atari-gui-create-jz0atcvj/run.json`. All three tested ROMs still
report partial because directory Hidden/System flags are unsupported. The runner and manifest now describe the
supplied ROM instead of labelling every run TOS 1.62. A TOS 2.05 attempt created
a valid v6 archive but the first scripted run did not extract into FAT12 `OUT`;
its screenshots and superseded failure record are under
`build/atari-gui-create-xbxkagkv`. Subsequent full runs passed on TOS 2.05,
including `build/atari-gui-create-6d0om8nu/run.json`. The runner now allows 20
seconds for extraction before the next key and 150 seconds for the main Hatari
session. The initial failure cause was not isolated.

The Create-button pointer path is now captured separately under
`build/atari-mouse-diagnostic`: with Hatari 2.5.0 and TOS 1.62, an Xvfb-hosted
`xdotool` click closed the production “More/Create” prompt and opened the Atari
save selector. The screenshot before the click and the resulting save selector
are retained with the exact input coordinates and run record. Hatari's control
socket has no pointer-position event, so this diagnostic uses an X11 pointer;
it does not cover cancellation. A prior Archive → Open pointer capture exists, but it omitted the pause
between title and item clicks; the repeatable qualification below records and
checks the full sequence. It does not exercise file selection, archive loading,
or an error path through that dialog. The same runner now also activates Help → About
and verifies its modal dialog on TOS 1.62 and EmuTOS 1.4; evidence is retained in
[`build/atari-menu-pointer-xdptlsms/run.json`](../build/atari-menu-pointer-xdptlsms/run.json)
and [`build/atari-menu-pointer-956pg4zc/run.json`](../build/atari-menu-pointer-956pg4zc/run.json).
Help → Documentation also opens its production alert on TOS 1.62; evidence is
[`build/atari-menu-pointer-ovmr2f0_/run.json`](../build/atari-menu-pointer-ovmr2f0_/run.json).
The same target passes Help → Documentation and all Compression selections on
TOS 1.62. Stored, BoltLite, BoltLZ, Fluff, and Interchange Profile 1 each
reopen the menu and verify the selected checkmark; records are
[`build/atari-menu-pointer-c7kia_a6/run.json`](../build/atari-menu-pointer-c7kia_a6/run.json),
[`build/atari-menu-pointer-9z5hz6fq/run.json`](../build/atari-menu-pointer-9z5hz6fq/run.json),
[`build/atari-menu-pointer-8zge1kb0/run.json`](../build/atari-menu-pointer-8zge1kb0/run.json),
[`build/atari-menu-pointer-ubm9y72c/run.json`](../build/atari-menu-pointer-ubm9y72c/run.json), and
[`build/atari-menu-pointer-fdx63g99/run.json`](../build/atari-menu-pointer-fdx63g99/run.json).
The expanded thirteen-action `atari-gui-menu-pointer-test` target passed on
TOS 1.62. A fresh rerun on 2026-10-30 passed all thirteen production menu
actions; its records are retained under `build/atari-menu-pointer-emnowwz9`,
`build/atari-menu-pointer-g_zdkmdv`, `build/atari-menu-pointer-yxovn0y5`,
`build/atari-menu-pointer-7lxn6vaa`, `build/atari-menu-pointer-qp4oe0o8`,
`build/atari-menu-pointer-07n9xw35`, `build/atari-menu-pointer-n5nn42dy`,
`build/atari-menu-pointer-vsdzpx6r`, `build/atari-menu-pointer-ci5oxtzu`,
`build/atari-menu-pointer-0e8316qj`, `build/atari-menu-pointer-d4ord4dj`,
`build/atari-menu-pointer-fgfangcj`, and `build/atari-menu-pointer-6x1ybbfe`.
For Archive → Validate, Information, Close, Extract, and Quit, it
creates and opens a Linux-generated archive, opens the Archive menu, captures
the open menu as a checkpoint, then selects the action and verifies the
resulting screen. Validate reports one file and “all passed”; Information
shows its alert, Close returns to the empty archive window, Extract opens the
destination selector, and Quit exits the GUI. The archive menu checkpoint also
detects a missed title click before selecting an item. Aggregate run records:
[`build/atari-menu-pointer-x97wtgkp/run.json`](../build/atari-menu-pointer-x97wtgkp/run.json)
for Validate,
[`build/atari-menu-pointer-b9h7ggq_/run.json`](../build/atari-menu-pointer-b9h7ggq_/run.json)
for Information,
[`build/atari-menu-pointer-1em6lbzu/run.json`](../build/atari-menu-pointer-1em6lbzu/run.json)
for Close,
[`build/atari-menu-pointer-cdha84fl/run.json`](../build/atari-menu-pointer-cdha84fl/run.json)
for Extract, and
[`build/atari-menu-pointer-jdy7mxak/run.json`](../build/atari-menu-pointer-jdy7mxak/run.json)
for Quit. Selector Abort and active-extraction cancellation are separately
qualified.

The same thirteen production menu actions now pass on Hatari 2.5.0 with the
bundled EmuTOS 1.4 ROM, using the current GUI binary. A first sequence replay
missed Help → About; isolated retries and the full rerun passed. The pointer
runner now waits for an initial GUI screenshot, captures the open menu, and
requires a visible menu-state change before clicking the item. All thirteen
per-action records pin the same ROM, GUI hash, clean revision, and screenshots;
all 88 artifact hashes were verified. Records are
[`build/atari-menu-pointer-ault41ip/run.json`](../build/atari-menu-pointer-ault41ip/run.json),
[`build/atari-menu-pointer-2q_l_ogk/run.json`](../build/atari-menu-pointer-2q_l_ogk/run.json),
[`build/atari-menu-pointer-b2nn78oo/run.json`](../build/atari-menu-pointer-b2nn78oo/run.json),
[`build/atari-menu-pointer-z0yibzza/run.json`](../build/atari-menu-pointer-z0yibzza/run.json),
[`build/atari-menu-pointer-cs1hq1ta/run.json`](../build/atari-menu-pointer-cs1hq1ta/run.json),
[`build/atari-menu-pointer-j88ht3tw/run.json`](../build/atari-menu-pointer-j88ht3tw/run.json),
[`build/atari-menu-pointer-hunslvh5/run.json`](../build/atari-menu-pointer-hunslvh5/run.json),
[`build/atari-menu-pointer-2a3f56fs/run.json`](../build/atari-menu-pointer-2a3f56fs/run.json),
[`build/atari-menu-pointer-i4wv390h/run.json`](../build/atari-menu-pointer-i4wv390h/run.json),
[`build/atari-menu-pointer-yo0z0wmp/run.json`](../build/atari-menu-pointer-yo0z0wmp/run.json),
[`build/atari-menu-pointer-wmhv_u92/run.json`](../build/atari-menu-pointer-wmhv_u92/run.json),
[`build/atari-menu-pointer-1qrj4hbi/run.json`](../build/atari-menu-pointer-1qrj4hbi/run.json), and
[`build/atari-menu-pointer-m14spxai/run.json`](../build/atari-menu-pointer-m14spxai/run.json).

The older Archive → Open pointer capture at
[`build/atari-mouse-menu-probe/run.json`](../build/atari-mouse-menu-probe/run.json)
was initially not reproducible because its record omitted the pause between
clicks. The repeatable `make CC=gcc
HATARI_TOS=/home/tony/images/Atari/tos162se.img atari-gui-menu-pointer-test`
now passes on Hatari 2.5.0/TOS 1.62. It focuses the Hatari window, clicks the
Archive title at window-relative `(330,6)`, waits 1.5 seconds, clicks Open at
`(335,38)`, and verifies the selector in the saved screenshot. The run record,
commands, binary/ROM hashes, and screenshots are retained at
[`build/atari-menu-pointer-g0lz579b/run.json`](../build/atari-menu-pointer-g0lz579b/run.json).
The same runner also passes with EmuTOS 1.4 under Hatari 2.5.0; the color
screen uses a different palette, and the dialog-region screenshot check passes
there as well. Its ROM/build hashes and screenshots are retained at
[`build/atari-menu-pointer-5jo8tryc/run.json`](../build/atari-menu-pointer-5jo8tryc/run.json).
The same target also clicks Help at window-relative `(650,6)`, waits 1.5 seconds,
and clicks About at `(650,38)`. It opens the production About dialog on TOS 1.62;
the TOS 1.62 and EmuTOS 1.4 run records, hashes, and screenshots are retained at
[`build/atari-menu-pointer-xdptlsms/run.json`](../build/atari-menu-pointer-xdptlsms/run.json)
and [`build/atari-menu-pointer-956pg4zc/run.json`](../build/atari-menu-pointer-956pg4zc/run.json).
The earlier fast-click replays remain valid evidence that the missing delay
made the input sequence unreliable; Archive → Open is qualified on both TOS
1.62 and EmuTOS 1.4, while Help → About is qualified on TOS 1.62 and EmuTOS 1.4, and Help → Documentation
is qualified on TOS 1.62.

- Continue auditing the TOS-version-specific directory timestamp and attribute
  outcomes; test rollback with a real device publication error and safe cleanup
  of incomplete pre-publication stages. Out-of-range file timestamps now have
  an explicit partial-result regression. Injected rollback failure with a
  competing destination and crash recovery after moving the old target are
  already covered. Active-extraction cancellation is qualified on TOS 1.06, 1.62, and
  2.05. The
  destination selector's mouse Abort button is qualified on TOS 1.06, 1.62,
  and 2.05; Escape remains ineffective.
- **Done when:** the shipped GUI passes these flows on the named TOS/filesystem.
  If the trace demonstrates an emulator input limitation, retain it and record
  the exact alternative input/runtime dependency; that case remains open.

### R07 — Qualify QDOS source enumeration and recovery (I02/I03/I06)

- **Completed sQLux hostFS slice:** `make CC=gcc ql-sqlux-qdirect-diagnostic`
  now opens directory channels with `Q_DIR == 4` and reads actual qdirect
  records through `IO_FSTRG`. The independent fixture contains two files and
  two directories; the QDOS-created archive retains both nested paths, and
  Linux full and mini readers test and extract the exact payload bytes. The
  hash-indexed evidence is
  [`build/r07-ql-qdirect-final-20261031/run.json`](../build/r07-ql-qdirect-final-20261031/run.json).
  The earlier all-zero record came from opening the directory as a regular
  file, not from a hostFS inability to enumerate it. The frontend accepts both
  zero-name and `ERR_EF` end-of-directory forms, normalizes hostFS parent-prefixed
  names, and recognizes the hostFS directory type marker.
- **Remaining:** run the same independent file/directory manifest on a physical
  QDOS filing system. Verify native timestamp/protection behavior, publication
  failure rollback, and recovery retention there; host fault injection covers
  only the host implementation.
- Preserve the passing sQLux hostFS transfer and -300/+60/+330 offset evidence.
  Physical filing-system/timezone claims require their own runtime evidence.
- **Done when:** physical filesystem recovery and metadata behavior have target
  evidence; the recursive sQLux hostFS transfer slice is complete.

## Target qualification and environment dependencies

Use existing environments where available; obtain only missing compilers,
runtimes, or filesystems and record their versions. Reuse the common corpus and
extend existing runners. A cross-build
or host shim supports development but does not replace target filesystem checks.

| Task / packages | Dependency | Concrete actions and completion evidence |
| --- | --- | --- |
| R08 — Windows (I02/I03/I06) | Native Windows; NTFS and FAT-compatible test volume; .NET tools; Total Commander | The full Win64 CLI corpus passed on Wine 10.0 on 2026-10-30 (`TMPDIR="$PWD/build/tmp-test" python3 test/check_windows_cli.py --wine /usr/lib/wine/wine64`); a fresh Win64 rebuild and run record are in [`build/win64/cli-wine-20261030`](../build/win64/cli-wine-20261030/run.json): Unicode paths, serial/parallel source-write and path-substitution refusal, corrupt extraction preservation, metadata/timestamps, Linux ↔ Windows transfers, Profile 1 compatibility, unsafe-name/collision preflight, hard-link/read-only edit refusal, append/test/delete. After commit `cffaf57`, a fresh `make win64-exe` and full rerun passed on Wine 10.0 with the documented 64-bit runner; the 19-case output, binary/tool hashes, worktree snapshot, and source diff are retained in [`build/r08-wine-cli-refresh-20261030`](../build/r08-wine-cli-refresh-20261030/run.json). The parent-directory race now accepts either direct identity-change reporting or a failed child inspection, while requiring refusal and no archive/stage publication. Run it on native Windows and cover reparse/parent substitution, ACL/stream retention during replacement, and rename/recovery failures. The rebuilt WCX passed `make CC=gcc WINE=/usr/lib/wine/wine64 win64-wcx-wine-test` on 2026-10-30; the binary, runner, verifier, and worktree hashes are retained in [`build/win64/wcx-wine-20261030`](../build/win64/wcx-wine-20261030/run.json). The test covers Unicode Open/List/Extract/Pack/Append/Delete, ANSI non-ACP refusal, Linux verification, and nested directory time finalization. Build and exercise native GUI, FLTK, managed GUI/DLL consumer, and WCX inside Total Commander. Verify cancellation, metadata ordering, and existing-file preservation for each consumer and shipped word size. Wine results remain supporting evidence only. |
| R09 — macOS and Classic Mac (I02/I03/I06) | macOS SDK/runtime with case-sensitive and insensitive volumes; Classic Mac 68k/PPC runtime and PPC build tools | `make macos9-ppc-xcoff` now compiles and links the PPC frontend with the installed Retro68 compiler and import libraries. The result is XCOFF, not a runnable Mac OS 9 application; native PPC app conversion/resource packaging and runtime behavior remain unqualified; the compiler, inputs, revision, linked artifact format, and hashes are recorded in [`build/macos9-ppc/run.json`](../build/macos9-ppc/run.json). `make CC=gcc macosx-host-test` passed on 2026-10-30, covering the POSIX CLI lifecycle and syntax-checking Apple API branches with host stubs; the independent fixture manifest, 31 command results, source diff, and 26 verified artifacts are retained in [`build/macosx-host/runs/macosx-host-x7y85oi_/run.json`](../build/macosx-host/runs/macosx-host-x7y85oi_/run.json). It does not exercise xattrs or native macOS filesystem APIs. Still run the macOS CLI/Cocoa cases on Apple systems for normalization/case collisions, MacRoman/HFS mapping, file/directory metadata, resources/Finder fields, staged failures, and profile transfer. Exercise both Classic Mac apps in their target runtimes and retain the 68k copied-core synchronization check. |
| R10 — DOS and Windows/OS2 legacy (I02/I03/I05/I06) | FreeDOS 1.4 boot image and CP850 files; Windows 3.x; OS/2 FAT/HPFS; suitable DOS filesystem/runtime | Full DJGPP DOS passed the DOSBox-X FAT12 create/append/delete/test/extract suite again on 2026-10-30; durable manifest and artifacts: [`build/r10-djgpp-dosboxx-fat12-20261030`](../build/r10-djgpp-dosboxx-fat12-20261030/manifest.json). On 2026-10-31 the POSIX `rename()` and DJGPP `_rename()` probes both replaced an existing FAT12 destination; diagnostic and hashes: [`build/r10-dos-rename-overwrite-diagnostic-20261031/run.json`](../build/r10-dos-rename-overwrite-diagnostic-20261031/run.json). The DJGPP publisher now uses INT 21h LFN rename with classic DOS rename fallback, both refusing an existing destination. Full DOSBox-X FAT12 create/append/delete/test/extract and bidirectional byte checks passed; an explicit nested-path probe confirms the helper preserves both the source and existing destination. Its retained manifest includes source/tool/environment details and 15 artifact hashes: [`build/r10-djgpp-dosboxx-noreplace-20261031-verified/manifest.json`](../build/r10-djgpp-dosboxx-noreplace-20261031-verified/manifest.json). This qualifies target-overwrite refusal for the tested DJGPP/DOSBox-X FAT12 path only; parent substitution, native DOS behavior, other FAT variants, and OS/2 races remain open. FreeDOS 1.4/QEMU 10.0.13 passes lowercase CP850; nested Linux→DOS, DOS→DOS, and DOS→Linux tree transfers; Hidden/System directory restoration; and an insufficient-space staged replacement that leaves the original archive bytes intact with no stage/backup leak. Unsupported DOS directory timestamps report partial. The same run verifies explicit `bpmini r` recovery from an integrity-tested `BK####.BAK`, refuses to overwrite an existing target, rejects a corrupt backup without modifying it, and recovers the original archive after a test process exits between publication renames. Evidence: [`build/r10-freedos-recovery-corrupt-negative-20261030/run.json`](../build/r10-freedos-recovery-corrupt-negative-20261030/run.json). A fresh rerun of the complete FreeDOS 1.4 CP850/tree/metadata/rollback suite passed against the pushed source on 2026-10-30; its retained image, logs, manifests and source record are in [`build/r10-freedos-cp850-refresh-20261030`](../build/r10-freedos-cp850-refresh-20261030/run.json). These cases qualify directory metadata, handled low-space replacement, and that deterministic process-exit recovery on this FreeDOS FAT12 image; they do not establish hardware power-loss durability. Other FAT variants remain open. `make win16 os2-16` cross-builds both 16-bit frontends with Open Watcom; output hashes and tool versions are retained in [`build/r10-win16-os2-20261030/run.json`](../build/r10-win16-os2-20261030/run.json). A fresh rebuild on 2026-10-30 again completed both targets without Watcom warnings; the command log, worktree diff/status, compiler identity, and EXE/map hashes are retained in [`build/r10-win16-os2-refresh-20261030/run.json`](../build/r10-win16-os2-refresh-20261030/run.json). The build follows the mini entry-capacity specialization for 16-bit `size_t`. An isolated Debian trixie Wine 10.0/i386 diagnostic on 2026-10-31 ran a trivial Win16 file-writing probe, but BoltPack `c` timed out with a 32-byte incomplete archive that Linux rejected. This is inconclusive, not a Win16 pass or a confirmed product defect; retained diagnostics are in [`build/r10-win16-wine-diagnostic-20261031/run.json`](../build/r10-win16-wine-diagnostic-20261031/run.json). Run Win16/OS2-16 lifecycle, names, profile, and resource-bound cases in real runtimes. On full OS/2 verify `DosMove` no-overwrite, rollback/interruption, archive attribute retention, and parent substitution. Record heap/stack results for each 16-bit runtime. |
| R11 — Amiga GUI and variants (I02/I03/I06) | Compatible 68k `gadtools.library` v37 or newer; separate OS4/MorphOS/AROS SDKs and runtimes | The installed compiler is 68k-only: `command -v` found no `ppc-amigaos-gcc`, MorphOS compiler, or `aros-x86_64-gcc`, and `/opt/amiga` contains only the m68k-AmigaOS sysroot. Native OS4/MorphOS/AROS cross-builds are unavailable on this machine. The installed NDK has GadTools headers and link descriptors but no runtime library. A scan of all 221 ADF images in the user-provided Workbench archive found no `gadtools.library`; evidence is [`build/amiga-gadtools-probe/run.json`](../build/amiga-gadtools-probe/run.json). The 68k CLI's deterministic publish-failure hook passes under FS-UAE: it restores the original archive byte-for-byte, verifies it with Linux, and removes stage and backup files; a complete run record is [`build/r11-amiga-publish-failure-20261030-retained/run.json`](../build/r11-amiga-publish-failure-20261030-retained/run.json). Native OFS and FFS interrupted-publication recovery also pass with independent payload expectations and complete hash-indexed run records: [`build/amiga-native-recovery-ofs-20261030-234206/run.json`](../build/amiga-native-recovery-ofs-20261030-234206/run.json) and [`build/amiga-native-recovery-ffs-20261030-234206/run.json`](../build/amiga-native-recovery-ffs-20261030-234206/run.json). Both restore the original byte-for-byte after deterministic FS-UAE process termination and retain the verified replacement stage; see R02 for evidence details. This does not establish physical power-loss durability. Native OFS and FFS also pass 65-level traversal and extraction at the default shell stack; deeper limits and additional constrained-memory thresholds remain open (R02). Foreign symbolic links are explicitly skipped with a partial result on both OFS and FFS; AmigaDOS symlink creation is not supported (R02). Obtain a runtime library before the GUI create/extract/edit, metadata, collision, cancellation, and replacement-failure corpus can run on OFS/FFS. Build/run each shipped OS4/MorphOS/AROS variant separately and record remaining resource and durability results. |
| R12 — Haiku (I02/I03/I06) — NATIVE DEFERRED | Haiku compiler/runtime and BFS volume | User decision: skip native Haiku/BeOS qualification for now because this environment cannot verify it. `make CC=gcc haiku-host-stage-test` passed on 2026-10-30; this checks host-shim staged extraction, edits, metadata behavior, path races, and Profile 1 only. The command log, test outputs, hashes, source revision, and dirty diff are retained in [`build/r12-haiku-host-shim-20261030`](../build/r12-haiku-host-shim-20261030/run.json). Keep host-shim evidence in scope; native qualification does not block the current completion gate. Reopen native work only when the target toolchain/runtime and BFS test volume are available or scope changes. On reopening, build CLI/BeAPI GUI, run the listed name, metadata, edit and profile cases, and add independent manifests under R02. |
| R13 — RISC OS, Human68k, CP/M-86 (I02/I03/I05/I06) | Each target's compiler and runtime/native filesystem | On 2026-10-30, `make CC=gcc riscos-path-test riscos-stage-host-test riscos-profile-host-test` passed: path projection, staged extraction refusal/cleanup, and a 32-bit SWI host-shim Profile 1 writer whose archive was verified/extracted by Linux full and mini. `make CC=gcc cpm86-host-test` also passed its 8.3/codepage/path projection checks. Commands, host/compiler identity, revision, worktree state/diff, source hashes, and artifact hashes are retained in [`build/r13-r15-host-shims-20261030/run.json`](../build/r13-r15-host-shims-20261030/run.json). These are host-shim checks, not native target qualification. Still build/run each frontend on its target. RISC OS: verify SWI rename/close/metadata behavior, name mappings, unsupported resource refusal, and rollback. Human68k: verify native Shift-JIS including trail byte `0x5c`, case collisions, date/attribute calls, exclusive stages, and edits. CP/M-86: verify 8.3/codepage mapping, representable metadata, clean refusals, profile limits, and 16-bit resource failures. Cross-read target output with full/mini readers. |
| R14 — Plan 9 (I02/I03/I06) — NATIVE RUN DEFERRED | Working 9front emulator/runtime; other shipped CPU builds/runtimes | No usable Plan 9 emulation is available for this work, per the user's clarification. `make CC=gcc plan9-host-test` passed on 2026-10-30: profile create/add, Linux full/mini cross-reader checks, bounded path-index and collision preflight, and fail-closed extraction (including existing-target preservation and dry-run). The command log, expected manifest, source revision, dirty diff, and artifact hashes are retained in [`build/r14-plan9-host-20261030`](../build/r14-plan9-host-20261030/run.json). This host-shim result does not qualify a native runtime. Retain existing documented amd64 extraction/metadata results as historical evidence only; do not claim a fresh guest run. When a working emulator/runtime is available, rerun the amd64 corpus and test missing API/filesystem cases, then qualify other shipped CPU targets separately. |
| R15 — Other POSIX wrappers (I02/I03/I06) | Actual BSD, Hurd, Solaris/illumos, AIX, z/OS, QNX, and other shipped wrapper environments | `make CC=gcc posix-path-extract-refusal-host-test` passed on 2026-10-30 for QNX, legacy/unidentified AIX, HP-UX, IRIX, NeXTSTEP, SCO, and Tru64: each refuses real extraction before output creation, while dry-run remains available. Commands and host executables are retained in [`build/r13-r15-host-shims-20261030/run.json`](../build/r13-r15-host-shims-20261030/run.json). This does not qualify their native APIs or filesystems. Enumerate every wrapper in the ledger source inventory; create a result per build. Verify native feature gates, filename encoding/conversion (especially z/OS runtime modes), safe traversal, file/directory metadata, staged edits, and visible partial outcomes. Resolve the known pathname-based race paths through R05. Host libc feature-branch builds do not close these rows. |

## Final convergence

### R16 — Reconcile evidence and required behavior (I01/I04/I06)

- For every ledger row, check F0/F1/F2/F3/F5 against the exact missing-case list
  from R01. Record executed command, source revision, environment, result and
  retained artifacts. Separate pass, expected partial/refusal, defect, and
  unavailable-environment outcomes.
- Verify native-versus-portable metadata precedence in both record orders for
  each applicable adapter; verify unknown/epoch-zero times, date range and
  rounding, directory ordering, and unrelated attribute preservation.
- Check every offered create/append/copy/delete operation preserves complete
  primary/resource/native/portable groups, supported archive attributes, and
  the previous target on handled failure. Qualify required interruption cases.
- Repair each uncovered behavior and extend its fixture before closing its row.
  Fix stale evidence links and remove contradictory historical status claims.
- **Done when:** every active scoped obligation has adequate evidence and no
  required behavior or environment remains unresolved. Explicitly deferred R12
  and unavailable native Plan 9 qualification are excluded from this gate and
  remain visible as deferred; their applicable host-shim checks still must pass.
  Until then R17 is blocked.

### R17 — Run the release gate and close the goal (I07)

- Reconcile specification, archive-format plan, matrix, ledger, bindings/DLL
  consumers, copied Classic Mac sources, build instructions, and release notes
  with the final behavior. Document real limits and expected partial outcomes.
- Run the final aggregate host gate, affected cross-builds/mini builds, copied
  source sync, and target fixtures justified by changes since their last pass.
  Use a disk-backed `TMPDIR` if `/tmp` lacks space for the sparse 4 GiB fixture.
  Do not repeatedly rerun unchanged expensive targets without a new concern.
- Review the final diff and retained results; commit the completed work. Publish
  or push only when requested. Update the tracked goal to complete only after
  every mandatory item is satisfied; report external dependencies if any remain.
- **Done when:** required checks pass on the final source, documentation agrees,
  all mandatory active ledger cases are closed, deferred R12 and native Plan 9
  qualification are clearly recorded, and the completion report links evidence.

## Progress and investigation rules

Track each task as ready, in progress, dependent on a named environment, or done;
include the fixing commit and evidence when closing it. Report concrete cases
closed and remaining prerequisites, rather than estimating a completion percentage
from test counts. Tasks above describe planned work and are not new execution
results.

Keep one concrete defect per implementation change. Stop an investigation after
repeated probes show the same environment limitation without new project
evidence: retain the reproducer, name the missing capability, and take the next
independent task. Reopen that investigation when a new diagnostic hypothesis or
dependency becomes available.
