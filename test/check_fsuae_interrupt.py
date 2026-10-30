#!/usr/bin/env python3
"""Exercise Amiga staged-output interruption and archive replacement rollback."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import platform

ROOT = Path(__file__).resolve().parent.parent
PATTERN = bytes((i * 73 + i // 17) & 0xff for i in range(64 * 1024))


def source_pattern(size):
    return (PATTERN * ((size + len(PATTERN) - 1) // len(PATTERN)))[:size]


def sibling_files(directory, prefix):
    return [path for path in directory.glob(prefix + "*")
            if path.is_file() and not path.name.lower().endswith(".uaem")]


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tool_version(command):
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    return {"command": command, "returncode": result.returncode,
            "output": result.stdout.strip()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--workbench", type=Path, required=True)
    parser.add_argument("--xdftool", default="xdftool")
    parser.add_argument("--fs-uae", default="fs-uae")
    parser.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    parser.add_argument("--amiga-cli", type=Path, default=ROOT / "build/amiga/boltpack")
    parser.add_argument("--recovery-cli", type=Path, default=ROOT / "build/amiga/boltpack")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--size-mib", type=int, choices=(1, 8, 16, 64, 128, 256), default=128)
    parser.add_argument("--operation",
                        choices=("extract", "append", "publish-failure",
                                 "publish-interrupt", "source-replace",
                                 "source-swap-restore", "parent-substitute"),
                        default="extract")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    out = (args.output.resolve() if args.output else
           Path(tempfile.mkdtemp(prefix="boltpack-amiga-interrupt-")))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    drive = out / "host"
    output = drive / "OUT"
    output.mkdir(parents=True)
    home = out / "home"
    (home / "Desktop").mkdir(parents=True)
    rom = args.rom.expanduser().resolve(strict=True)
    workbench = args.workbench.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    amiga = args.amiga_cli.resolve(strict=True)
    recovery_cli = args.recovery_cli.resolve(strict=True)
    shutil.copyfile(amiga, drive / "boltpack")

    # Publication failure/interruption is triggered by a deterministic test hook
    # after staging, so the large payload used by kill-mid-copy cases is unnecessary.
    source_mib = (min(args.size_mib, 1)
                  if args.operation.startswith("publish-") or
                  args.operation in ("source-replace", "source-swap-restore")
                  else args.size_mib)
    source_size = source_mib * 1024 * 1024
    source = drive / "LARGE.BIN"
    digest = hashlib.sha256()
    with source.open("wb") as stream:
        remaining = source_size
        while remaining:
            block = PATTERN[:min(remaining, len(PATTERN))]
            stream.write(block)
            digest.update(block)
            remaining -= len(block)
    archive = drive / "INTERRUPT.BLT"
    create = subprocess.run([str(linux), "c", archive.name, source.name],
                            cwd=drive, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=True, timeout=120)
    (out / "linux-create.log").write_bytes(create.stdout)
    target = output / "LARGE.BIN"
    sentinel = b"original destination must survive an interrupted extraction\n"
    if args.operation == "extract":
        target.write_bytes(sentinel)
    elif args.operation != "parent-substitute":
        (drive / ("REPLACEMENT.BIN" if args.operation.startswith("publish-")
                  else "ADD.BIN")).write_bytes(
                      b"replacement archive content\n" if args.operation.startswith("publish-")
                      else b"append after the interrupted copy\n")
    original_archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
    stage_expected_size = (source_size if args.operation == "extract" else
                           archive.stat().st_size)

    system_disk = out / "workbench.adf"
    shutil.copyfile(workbench, system_disk)
    startup = out / "startup"
    if args.operation == "extract":
        guest_command = "DH0:boltpack x -f -o DH0:OUT DH0:INTERRUPT.BLT >DH0:EXTRACT.LOG"
    elif args.operation == "parent-substitute":
        guest_command = "DH0:boltpack x -o DH0:OUT DH0:INTERRUPT.BLT >DH0:EXTRACT.LOG"
    elif args.operation == "append":
        guest_command = "DH0:boltpack a DH0:INTERRUPT.BLT DH0:ADD.BIN >DH0:APPEND.LOG"
    elif args.operation.startswith("publish-"):
        guest_command = ("DH0:boltpack c --test-fail-publish DH0:INTERRUPT.BLT "
                         "DH0:REPLACEMENT.BIN >DH0:REPLACE.LOG"
                         if args.operation == "publish-failure" else
                         "DH0:boltpack c DH0:INTERRUPT.BLT DH0:REPLACEMENT.BIN "
                         ">DH0:REPLACE.LOG")
    elif args.operation in ("source-replace", "source-swap-restore"):
        guest_command = "DH0:boltpack c DH0:INTERRUPT.BLT DH0:LARGE.BIN >DH0:REPLACE.LOG"
    startup.write_text("\n".join([
        "C:SetPatch >NIL: QUIET",
        "C:Assign T: RAM:",
        "C:Stack 262144",
        guest_command,
        "C:Echo $RC >DH0:OPERATION.RC",
        "C:Echo FINISHED >DH0:DONE.TXT",
        ""]), encoding="ascii")
    with (out / "fs-uae.log").open("wb") as log:
        subprocess.run([args.xdftool, str(system_disk), "delete", "S/Startup-Sequence"],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([args.xdftool, str(system_disk), "write", str(startup),
                        "S/Startup-Sequence"], stdout=log,
                       stderr=subprocess.STDOUT, check=True)
        env = os.environ.copy()
        env.update(HOME=str(home), LIBGL_ALWAYS_SOFTWARE="1", SDL_AUDIODRIVER="dummy")
        command = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24", args.fs_uae,
                   "--amiga_model=A500+", "--kickstart_file=" + str(rom),
                   "--floppy_drive_0=" + str(system_disk),
                   "--hard_drive_0=" + str(drive), "--warp_mode=1",
                   "--fullscreen=0", "--chip_memory=2048", "--fast_memory=8192"]
        (out / "command.txt").write_text(repr(command) + "\n", encoding="utf-8")
        guest = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        stage = None
        stage_size = 0
        deadline = time.monotonic() + args.timeout
        try:
            if args.operation == "publish-failure":
                while time.monotonic() < deadline:
                    if (drive / "DONE.TXT").exists():
                        break
                    if guest.poll() is not None:
                        raise RuntimeError("guest exited before completion marker; evidence: " + str(out))
                    time.sleep(0.01)
                else:
                    raise RuntimeError("guest did not finish publish-failure case; evidence: " + str(out))
            elif args.operation in ("source-replace", "source-swap-restore"):
                while time.monotonic() < deadline:
                    if (drive / "SOURCE-OPENED.TXT").exists():
                        break
                    if (drive / "DONE.TXT").exists():
                        rc_text = (drive / "OPERATION.RC").read_text(encoding="ascii").strip()
                        raise RuntimeError("source operation finished before open pause (rc=" + rc_text + "); evidence: " + str(out))
                    if guest.poll() is not None:
                        raise RuntimeError("guest exited before opening source; evidence: " + str(out))
                    time.sleep(0.01)
                else:
                    raise RuntimeError("guest did not reach source-open pause; evidence: " + str(out))
                replacement = drive / "LARGE.NEW"
                replacement.write_bytes(b"R" * source_size)
                if args.operation == "source-replace":
                    os.replace(replacement, source)
                else:
                    parked_source = drive / "LARGE.OLD"
                    os.replace(source, parked_source)
                    os.replace(replacement, source)
                    source.unlink()
                    os.replace(parked_source, source)
                (drive / "CONTINUE.TXT").write_text("continue\n", encoding="ascii")
                while time.monotonic() < deadline:
                    if (drive / "DONE.TXT").exists():
                        break
                    if guest.poll() is not None:
                        raise RuntimeError("guest exited before source-change result; evidence: " + str(out))
                    time.sleep(0.01)
                else:
                    raise RuntimeError("guest did not finish source-change case; evidence: " + str(out))
            elif args.operation == "publish-interrupt":
                while time.monotonic() < deadline:
                    backups = sibling_files(drive, "BR")
                    if backups and not archive.exists():
                        stages = sibling_files(drive, "BP")
                        if stages:
                            stage = stages[0]
                            stage_size = stage.stat().st_size
                            os.killpg(guest.pid, signal.SIGSTOP)
                            break
                    if (drive / "DONE.TXT").exists() or guest.poll() is not None:
                        raise RuntimeError("guest completed before backup-window interruption; evidence: " + str(out))
                    time.sleep(0.002)
                else:
                    raise RuntimeError("did not observe original at recovery name; evidence: " + str(out))
            elif args.operation == "parent-substitute":
                while time.monotonic() < deadline:
                    candidates = sibling_files(output, "BP")
                    if candidates:
                        stage = candidates[0]
                        stage_size = stage.stat().st_size
                        os.killpg(guest.pid, signal.SIGSTOP)
                        break
                    if (drive / "DONE.TXT").exists() or guest.poll() is not None:
                        raise RuntimeError("guest completed before parent-substitution pause; evidence: " + str(out))
                    time.sleep(0.002)
                else:
                    raise RuntimeError("no extraction stage appeared before timeout; evidence: " + str(out))
                moved_output = drive / "OUT.MOVED"
                os.replace(output, moved_output)
                output.mkdir()
                parent_sentinel = b"replacement output directory must remain untouched\n"
                (output / "SENTINEL.TXT").write_bytes(parent_sentinel)
                os.killpg(guest.pid, signal.SIGCONT)
                while time.monotonic() < deadline:
                    if (drive / "DONE.TXT").exists():
                        break
                    if guest.poll() is not None:
                        raise RuntimeError("guest exited before parent-substitution result; evidence: " + str(out))
                    time.sleep(0.01)
                else:
                    raise RuntimeError("guest did not finish parent-substitution case; evidence: " + str(out))
            else:
                stage_dir = output if args.operation == "extract" else drive
                while time.monotonic() < deadline:
                    candidates = sibling_files(stage_dir, "BP")
                    if candidates:
                        candidate = candidates[0]
                        try:
                            size = candidate.stat().st_size
                        except FileNotFoundError:
                            size = 0
                        if 65536 <= size < stage_expected_size:
                            stage = candidate
                            stage_size = size
                            os.killpg(guest.pid, signal.SIGSTOP)
                            break
                    if (drive / "DONE.TXT").exists() or guest.poll() is not None:
                        raise RuntimeError("guest completed before interruption; evidence: " + str(out))
                    time.sleep(0.002)
                else:
                    raise RuntimeError("no partial stage appeared before timeout; evidence: " + str(out))
        finally:
            if guest.poll() is None:
                os.killpg(guest.pid, signal.SIGKILL)
            try:
                guest.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(guest.pid, signal.SIGKILL)
                guest.wait()

    operation_rc = None
    parent_substitution_stage = None
    if args.operation == "extract" and target.read_bytes() != sentinel:
        raise RuntimeError("interrupted extraction changed the existing destination; evidence: " + str(out))
    if args.operation == "parent-substitute":
        operation_rc = (drive / "OPERATION.RC").read_text(encoding="ascii").strip()
        moved = drive / "OUT.MOVED"
        stages = sibling_files(moved, "BP")
        if (operation_rc == "0" or (output / "LARGE.BIN").exists() or
                (output / "SENTINEL.TXT").read_bytes() !=
                b"replacement output directory must remain untouched\n" or
                len(stages) != 1):
            raise RuntimeError("parent substitution was not refused safely; evidence: " + str(out))
        parent_substitution_stage = stages[0]
    if args.operation == "append" and hashlib.sha256(archive.read_bytes()).hexdigest() != original_archive_hash:
        raise RuntimeError("interrupted append changed the original archive; evidence: " + str(out))
    if args.operation == "publish-failure":
        if hashlib.sha256(archive.read_bytes()).hexdigest() != original_archive_hash:
            raise RuntimeError("failed archive replacement changed the original; evidence: " + str(out))
        operation_rc = (drive / "OPERATION.RC").read_text(encoding="ascii").strip()
        replace_log = (drive / "REPLACE.LOG").read_bytes()
        if operation_rc == "0" or b"1 file" not in replace_log:
            raise RuntimeError("publish-failure hook did not fail after archive finalization; evidence: " + str(out))
        if sibling_files(drive, "BR") or sibling_files(drive, "BP"):
            raise RuntimeError("failed archive replacement left backup/stage files; evidence: " + str(out))
        subprocess.run([str(linux), "t", str(archive)], check=True, timeout=120,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if args.operation == "source-replace":
        if hashlib.sha256(archive.read_bytes()).hexdigest() != original_archive_hash:
            raise RuntimeError("source replacement changed the existing archive; evidence: " + str(out))
        operation_rc = (drive / "OPERATION.RC").read_text(encoding="ascii").strip()
        if operation_rc == "0":
            raise RuntimeError("source replacement was not rejected; evidence: " + str(out))
        if sibling_files(drive, "BP") or sibling_files(drive, "BR"):
            raise RuntimeError("source replacement left archive stages; evidence: " + str(out))
        subprocess.run([str(linux), "t", str(archive)], check=True, timeout=120,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if args.operation == "source-swap-restore":
        operation_rc = (drive / "OPERATION.RC").read_text(encoding="ascii").strip()
        if operation_rc != "0":
            raise RuntimeError("source swap-and-restore was not accepted; evidence: " + str(out))
        expected_source = source_pattern(source_size)
        if (hashlib.sha256(archive.read_bytes()).hexdigest() == original_archive_hash or
                source.read_bytes() != expected_source):
            raise RuntimeError("source swap-and-restore produced unexpected source/archive state; evidence: " + str(out))
        verify = subprocess.run([str(linux), "t", str(archive)], check=False,
                                timeout=120, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        if verify.returncode != 0:
            raise RuntimeError("Linux rejected archive after source swap-and-restore; evidence: " + str(out))
        extracted = out / "swap-restore-extracted"
        extracted.mkdir()
        extraction = subprocess.run(
            [str(linux), "x", "-f", "-o", str(extracted), str(archive)],
            check=False, timeout=120, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT)
        (out / "source-swap-restore-linux-extract.log").write_bytes(
            extraction.stdout)
        if extraction.returncode not in (0, 1):
            raise RuntimeError("Linux could not extract Amiga archive: " +
                               extraction.stdout.decode("utf-8", "replace"))
        if ((extracted / "LARGE.BIN").read_bytes() != expected_source or
                b"some entries or attributes were not restored" not in
                extraction.stdout):
            raise RuntimeError("source swap-and-restore changed archived bytes; evidence: " + str(out))
    if args.operation == "publish-interrupt":
        backups = sibling_files(drive, "BR")
        stages = sibling_files(drive, "BP")
        if archive.exists() or len(backups) != 1 or len(stages) != 1:
            raise RuntimeError("backup-window interruption left unexpected paths; evidence: " + str(out))
        if (not backups[0].name.startswith("BRINTERRUPTBLT") or
                stages[0].name != "BP" + backups[0].name[2:]):
            raise RuntimeError("recovery file names do not identify and pair this archive; evidence: " + str(out))
        if hashlib.sha256(backups[0].read_bytes()).hexdigest() != original_archive_hash:
            raise RuntimeError("recovery backup differs from original archive; evidence: " + str(out))
        subprocess.run([str(linux), "t", str(stages[0])], check=True, timeout=120,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        duplicate_suffix = "DUP001"
        duplicate_backup = drive / (backups[0].name[:-6] + duplicate_suffix)
        duplicate_stage = drive / (stages[0].name[:-6] + duplicate_suffix)
        shutil.copyfile(backups[0], duplicate_backup)
        shutil.copyfile(stages[0], duplicate_stage)
        # Boot a fresh Workbench copy and invoke the production recovery command.
        # The test-only first CLI was killed in the rename window above.
        recovery_disk = out / "recovery-workbench.adf"
        shutil.copyfile(workbench, recovery_disk)
        shutil.copyfile(recovery_cli, drive / "boltpack")
        for marker in (drive / "DONE.TXT", drive / "OPERATION.RC"):
            marker.unlink(missing_ok=True)
        recovery_startup = out / "recovery-startup"
        recovery_startup.write_text("\n".join([
            "C:SetPatch >NIL: QUIET",
            "C:Assign T: RAM:",
            "C:Stack 262144",
            "DH0:boltpack r DH0:INTERRUPT.BLT >DH0:AMBIGUOUS.LOG",
            "C:Echo $RC >DH0:AMBIGUOUS.RC",
            "DH0:boltpack r DH0:INTERRUPT.BLT %s >DH0:RECOVERY.LOG" % duplicate_backup.name.lower(),
            "C:Echo $RC >DH0:OPERATION.RC",
            "C:Echo FINISHED >DH0:DONE.TXT",
            ""]), encoding="ascii")
        with (out / "fs-uae-recovery.log").open("wb") as recovery_log:
            subprocess.run([args.xdftool, str(recovery_disk), "delete", "S/Startup-Sequence"],
                           stdout=recovery_log, stderr=subprocess.STDOUT, check=True)
            subprocess.run([args.xdftool, str(recovery_disk), "write",
                            str(recovery_startup), "S/Startup-Sequence"],
                           stdout=recovery_log, stderr=subprocess.STDOUT, check=True)
            recovery_command = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24", args.fs_uae,
                                "--amiga_model=A500+", "--kickstart_file=" + str(rom),
                                "--floppy_drive_0=" + str(recovery_disk),
                                "--hard_drive_0=" + str(drive), "--warp_mode=1",
                                "--fullscreen=0", "--chip_memory=2048", "--fast_memory=8192"]
            recovery_guest = subprocess.Popen(recovery_command, env=env,
                                              stdout=recovery_log,
                                              stderr=subprocess.STDOUT,
                                              start_new_session=True)
            recovery_deadline = time.monotonic() + args.timeout
            try:
                while time.monotonic() < recovery_deadline:
                    if (drive / "DONE.TXT").exists():
                        break
                    if recovery_guest.poll() is not None:
                        raise RuntimeError("recovery guest exited before completion; evidence: " + str(out))
                    time.sleep(0.05)
                else:
                    raise RuntimeError("recovery guest did not finish; evidence: " + str(out))
            finally:
                if recovery_guest.poll() is None:
                    os.killpg(recovery_guest.pid, signal.SIGTERM)
                try:
                    recovery_guest.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(recovery_guest.pid, signal.SIGKILL)
                    recovery_guest.wait()
        recovery_rc = (drive / "OPERATION.RC").read_text(encoding="ascii").strip()
        if recovery_rc != "0":
            raise RuntimeError("Amiga recovery command failed (rc=" + recovery_rc + "); evidence: " + str(out))
        ambiguous_rc = (drive / "AMBIGUOUS.RC").read_text(encoding="ascii").strip()
        if ambiguous_rc == "0":
            raise RuntimeError("Amiga recovery accepted ambiguous backup names; evidence: " + str(out))
        if (hashlib.sha256(archive.read_bytes()).hexdigest() != original_archive_hash or
                len(sibling_files(drive, "BR")) != 1 or
                len(sibling_files(drive, "BP")) != 2):
            raise RuntimeError("explicit recovery did not restore original and preserve the unselected pair; evidence: " + str(out))
        subprocess.run([str(linux), "t", str(archive)], check=True, timeout=120,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for remaining_stage in sibling_files(drive, "BP"):
            subprocess.run([str(linux), "t", str(remaining_stage)], check=True, timeout=120,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (args.operation in ("extract", "append") and
            (stage is None or not stage.exists() or stage_size >= stage_expected_size)):
        raise RuntimeError("interrupt did not catch an incomplete sibling stage; evidence: " + str(out))
    if args.operation == "publish-failure":
        state = ("result=PASS\noperation=publish-failure\n"
                 "destination=original archive hash preserved\n"
                 "replacement=failed after archive finalization\n"
                 "recovery_backup=restored\nstage=cleaned\n")
    elif args.operation == "publish-interrupt":
        state = ("result=PASS\noperation=publish-interrupt-and-explicit-recovery\n"
                 "target=absent during rename window; Amiga CLI refused ambiguous automatic recovery\n"
                 "recovery_backup=explicitly selected original hash verified and restored\n"
                 "unselected_pair=retained\nstaged_replacement=Linux integrity checked\n")
    elif args.operation == "source-replace":
        state = ("result=PASS\noperation=source-path-replacement\n"
                 "opened_source=kept open while host pathname was replaced\n"
                 "archive=original hash preserved\nstage=cleaned\n")
    elif args.operation == "source-swap-restore":
        state = ("result=PASS\noperation=source-path-swap-and-restore\n"
                 "opened_source=original inode remained open during transient pathname replacement\n"
                 "source_path=original inode restored before final path validation\n"
                 "archive=accepted; Linux integrity and byte-exact extraction passed\n")
    elif args.operation == "parent-substitute":
        state = ("result=PASS\noperation=output-parent-substitution\n"
                 "replacement_output=sentinel preserved; extracted file not published\n"
                 "original_output=renamed directory retains one unpublished stage\n")
    else:
        state = ("result=PASS\noperation=%s\ndestination=%s\n"
                 "stage_size=%d\nexpected_size=%d\n" % (
                     args.operation,
                     "original sentinel preserved" if args.operation == "extract" else "archive hash preserved",
                     stage_size, stage_expected_size))
    (out / "interrupted-state.txt").write_text(state, encoding="ascii")
    status = subprocess.run(["git", "status", "--short"], cwd=ROOT,
                           stdout=subprocess.PIPE, check=True).stdout
    source_diff = subprocess.run(["git", "diff", "--binary"], cwd=ROOT,
                                 stdout=subprocess.PIPE, check=True).stdout
    (out / "source-status.txt").write_bytes(status)
    (out / "source-diff.patch").write_bytes(source_diff)
    manifest = {
        "purpose": "Amiga CLI %s publication and recovery interoperability" % args.operation,
        "result": "PASS",
        "operation": args.operation,
        "date": time.strftime("%Y-%m-%d", time.gmtime()),
        "source_revision": subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                           stdout=subprocess.PIPE, check=True,
                                           text=True).stdout.strip(),
        "worktree_dirty": bool(status.strip()),
        "source_status_sha256": hashlib.sha256(status).hexdigest(),
        "source_diff_sha256": hashlib.sha256(source_diff).hexdigest(),
        "runner_sha256": sha256(Path(__file__)),
        "host": platform.platform(),
        "locale": {name: os.environ.get(name)
                   for name in ("LANG", "LC_ALL", "LC_CTYPE", "TZ")},
        "filesystem": "Workbench hard drive host directory on FS-UAE",
        "emulator_command": command,
        "guest_command": guest_command,
        "guest_return_code": operation_rc,
        "evidence": state.splitlines(),
        "tools": [tool_version([args.fs_uae, "--version"]),
                  tool_version([str(Path(args.xdftool).resolve().parent / "python"),
                               "-c", "from importlib.metadata import version; print(version('amitools'))"]),
                  tool_version(["/opt/amiga/bin/m68k-amigaos-gcc", "--version"])],
        "assets": {"kickstart_sha256": sha256(rom),
                   "workbench_sha256": sha256(workbench),
                   "xdftool_sha256": sha256(args.xdftool)},
        "executables": {"linux_cli_sha256": sha256(linux),
                        "amiga_cli_sha256": sha256(amiga),
                        "recovery_cli_sha256": sha256(recovery_cli)},
        "artifacts": {"workbench_disk_sha256": sha256(system_disk),
                      "archive_before_sha256": original_archive_hash,
                      "archive_after_sha256": sha256(archive),
                      "source_payload_sha256": digest.hexdigest(),
                      "source_payload_bytes": source_size},
    }
    if parent_substitution_stage is not None:
        replacement_sentinel = output / "SENTINEL.TXT"
        manifest["parent_substitution"] = {
            "replacement_output_sentinel_sha256": sha256(replacement_sentinel),
            "replacement_output_file_published": False,
            "renamed_original_stage_name": parent_substitution_stage.name,
            "renamed_original_stage_size": parent_substitution_stage.stat().st_size,
            "renamed_original_stage_sha256": sha256(parent_substitution_stage),
        }
    (out / "run.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                   encoding="utf-8")
    if args.operation == "publish-failure":
        print("PASS: failed Amiga archive replacement restored and verified the original; evidence:", out)
    elif args.operation == "publish-interrupt":
        print("PASS: Amiga recovery restored the original archive and retained the valid replacement stage; evidence:", out)
    elif args.operation == "source-replace":
        print("PASS: Amiga CLI rejected a replaced source path and preserved the original archive; evidence:", out)
    elif args.operation == "source-swap-restore":
        print("PASS: Amiga CLI source swap-and-restore probe completed; evidence:", out)
    elif args.operation == "parent-substitute":
        print("PASS: Amiga CLI rejected a replaced extraction parent without publishing into its replacement; evidence:", out)
    else:
        print("PASS: interrupted Amiga", args.operation,
              "preserved the original destination; stage remains at", stage,
              "(evidence:", out, ")")


if __name__ == "__main__":
    main()
