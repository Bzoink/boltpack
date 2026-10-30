#!/usr/bin/env python3
"""Kill Amiga archive publication on a disposable OFS/FFS floppy, then recover."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def artifact_index(root):
    return {
        path.relative_to(root).as_posix(): {
            "size": path.stat().st_size,
            "sha256": sha256(path),
        }
        for path in sorted(root.rglob("*"))
        if path.is_file() and path.name != "run.json"
    }


def run(command, log, timeout=30, cwd=None):
    log.write((repr(command) + "\n").encode())
    log.flush()
    return subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT,
                          check=True, timeout=timeout)


def boot(args, env, system_disk, native_disk, drive, log_path):
    command = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24", args.fs_uae,
               "--amiga_model=A500+", "--kickstart_file=" + str(args.rom),
               "--floppy_drive_0=" + str(system_disk),
               "--floppy_drive_1=" + str(native_disk),
               "--hard_drive_0=" + str(drive), "--floppy_drive_speed=800",
               "--warp_mode=1", "--fullscreen=0", "--chip_memory=2048",
               "--fast_memory=8192", "--writable_floppy_images=1"]
    with log_path.open("wb") as log:
        log.write((repr(command) + "\n").encode())
        log.flush()
        guest = subprocess.Popen(command, env=env, stdout=log,
                                 stderr=subprocess.STDOUT,
                                 start_new_session=True)
        return guest


def stop(guest):
    if guest.poll() is None:
        os.killpg(guest.pid, signal.SIGTERM)
    try:
        guest.wait(timeout=10)
    except subprocess.TimeoutExpired:
        os.killpg(guest.pid, signal.SIGKILL)
        guest.wait()


def wait_for(path, guest, timeout, out, description):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if guest.poll() is not None:
            raise RuntimeError(description + ": emulator exited early; evidence: " + str(out))
        time.sleep(0.01)
    raise RuntimeError(description + ": timed out; evidence: " + str(out))


def list_root(args, disk, log):
    result = subprocess.run([args.xdftool, "-r", str(disk), "list"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            check=True)
    log.write(result.stdout)
    return result.stdout.decode("latin-1", "replace")


def read_file(args, disk, name, destination, log):
    run([args.xdftool, "-r", str(disk), "read", name, str(destination)], log)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--rom", type=Path, required=True)
    p.add_argument("--workbench", type=Path, required=True)
    p.add_argument("--xdftool", default="xdftool")
    p.add_argument("--fs-uae", default="fs-uae")
    p.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    p.add_argument("--pause-cli", type=Path, required=True)
    p.add_argument("--recovery-cli", type=Path, default=ROOT / "build/amiga/boltpack")
    p.add_argument("--filesystem", choices=("ofs", "ffs"), default="ofs")
    p.add_argument("--manifest", type=Path,
                   default=ROOT / "test/reference/interoperability/amiga-native-recovery.json")
    p.add_argument("--timeout", type=int, default=90)
    p.add_argument("--settle-seconds", type=int, default=35,
                   help="allow native floppy buffers to flush while the CLI is paused")
    p.add_argument("--output", type=Path)
    args = p.parse_args()

    out = (args.output.resolve() if args.output else
           Path(tempfile.mkdtemp(prefix="boltpack-amiga-native-recovery-")))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    drive = out / "host"
    drive.mkdir()
    home = out / "home"
    (home / "Desktop").mkdir(parents=True)
    rom = args.rom.expanduser().resolve(strict=True)
    workbench = args.workbench.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    pause_cli = args.pause_cli.resolve(strict=True)
    recovery_cli = args.recovery_cli.resolve(strict=True)
    manifest_path = args.manifest.resolve(strict=True)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    (out / "amiga-native-recovery.json").write_bytes(manifest_path.read_bytes())
    system_disk = out / "workbench.adf"
    native_disk = out / "data.adf"
    shutil.copyfile(workbench, system_disk)
    shutil.copyfile(pause_cli, drive / "boltpack-pause")
    shutil.copyfile(recovery_cli, drive / "boltpack")

    original_spec = manifest["original"]
    original_data = bytes((i * 29 + i // 11) & 0xff
                          for i in range(original_spec["size"]))
    if hashlib.sha256(original_data).hexdigest() != original_spec["sha256"]:
        raise RuntimeError("independent original payload manifest is inconsistent")
    replacement_spec = manifest["replacement"]
    replacement_data = (replacement_spec["content"].encode("utf-8") *
                        replacement_spec["repetitions"])
    if (len(replacement_data) != replacement_spec["size"] or
            hashlib.sha256(replacement_data).hexdigest() !=
            replacement_spec["sha256"]):
        raise RuntimeError("independent replacement payload manifest is inconsistent")
    (drive / "ORIGINAL.BIN").write_bytes(original_data)
    (drive / "REPLACEMENT.BIN").write_bytes(replacement_data)
    original_archive = drive / "INTERRUPT.BLT"
    with (out / "setup.log").open("wb") as log:
        run([str(linux), "c", "-0", original_archive.name, "ORIGINAL.BIN"], log,
            timeout=120, cwd=drive)
        run([args.xdftool, str(native_disk), "create"], log)
        fmt = [args.xdftool, str(native_disk), "format", "BPNATIVE"]
        if args.filesystem == "ffs":
            fmt.append("ffs")
        run(fmt, log)
        run([args.xdftool, str(native_disk), "write", str(original_archive),
             "INTERRUPT.BLT"], log)

        first_startup = out / "publish-startup"
        first_startup.write_text("\n".join([
            "C:SetPatch >NIL: QUIET",
            "C:Assign T: RAM:",
            "C:Stack 262144",
            "Run >NIL: DH0:boltpack-pause c DF1:INTERRUPT.BLT DH0:REPLACEMENT.BIN",
            "C:Wait 1",
            "C:Echo STARTED >DH0:STARTED.TXT",
            ""]), encoding="ascii")
        run([args.xdftool, str(system_disk), "delete", "S/Startup-Sequence"], log)
        run([args.xdftool, str(system_disk), "write", str(first_startup),
             "S/Startup-Sequence"], log)

    env = os.environ.copy()
    env.update(HOME=str(home), LIBGL_ALWAYS_SOFTWARE="1", SDL_AUDIODRIVER="dummy")
    guest = boot(args, env, system_disk, native_disk, drive, out / "publish-fs-uae.log")
    try:
        wait_for(drive / "RECOVERY-WINDOW.TXT", guest, args.timeout, out,
                 "publish did not reach the backup/publication window")
        time.sleep(args.settle_seconds)
        # Snapshot the disposable floppy while the CLI is paused, then stop the
        # entire emulator to learn whether those sectors survive the restart.
        shutil.copyfile(native_disk, out / "at-window.adf")
    finally:
        stop(guest)

    # The ADF is disposable and writable; inspect only after FS-UAE releases it.
    with (out / "inspect.log").open("wb") as log:
        listing = list_root(args, native_disk, log)
        (out / "disk-after-stop.list").write_text(listing, encoding="latin-1")
        with (out / "inspect-window.log").open("wb") as window_log:
            window_listing = list_root(args, out / "at-window.adf", window_log)
            (out / "disk-at-window.list").write_text(window_listing, encoding="latin-1")
        backup_names = [line.split()[0] for line in listing.splitlines()
                        if line.startswith("  BR") and ".uaem" not in line.lower()]
        stage_names = [line.split()[0] for line in listing.splitlines()
                       if line.startswith("  BP") and ".uaem" not in line.lower()]
        if ("INTERRUPT.BLT" in listing or len(backup_names) != 1 or
                len(stage_names) != 1):
            raise RuntimeError("OFS/FFS interruption did not persist one paired backup/stage; evidence: " + str(out))
        backup_file, stage_file = out / "original-backup.blt", out / "replacement-stage.blt"
        read_file(args, native_disk, backup_names[0], backup_file, log)
        read_file(args, native_disk, stage_names[0], stage_file, log)
    expected_hash = hashlib.sha256(original_archive.read_bytes()).hexdigest()
    if hashlib.sha256(backup_file.read_bytes()).hexdigest() != expected_hash:
        raise RuntimeError("native recovery backup differs from the original; evidence: " + str(out))
    expected_stage_hash = hashlib.sha256(stage_file.read_bytes()).hexdigest()
    subprocess.run([str(linux), "t", str(stage_file)], check=True, timeout=120,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    recovery_startup = out / "recovery-startup"
    recovery_startup.write_text("\n".join([
        "C:SetPatch >NIL: QUIET",
        "C:Assign T: RAM:",
        "C:Stack 262144",
        "DH0:boltpack r DF1:INTERRUPT.BLT >DH0:RECOVERY.LOG",
        "C:Echo $RC >DH0:RECOVERY.RC",
        "C:Wait 1",
        "C:Echo FINISHED >DH0:DONE.TXT",
        ""]), encoding="ascii")
    with (out / "recovery-setup.log").open("wb") as log:
        run([args.xdftool, str(system_disk), "delete", "S/Startup-Sequence"], log)
        run([args.xdftool, str(system_disk), "write", str(recovery_startup),
             "S/Startup-Sequence"], log)
    (drive / "STARTED.TXT").unlink(missing_ok=True)
    guest = boot(args, env, system_disk, native_disk, drive, out / "recovery-fs-uae.log")
    try:
        wait_for(drive / "DONE.TXT", guest, args.timeout, out,
                 "native recovery command did not finish")
    finally:
        stop(guest)
    if (drive / "RECOVERY.RC").read_text(encoding="ascii").strip() != "0":
        raise RuntimeError("native recovery command returned failure; evidence: " + str(out))

    with (out / "final-inspect.log").open("wb") as log:
        listing = list_root(args, native_disk, log)
        if "INTERRUPT.BLT" not in listing or "BR" in listing:
            raise RuntimeError("native recovery did not restore target/consume backup; evidence: " + str(out))
        final_stage_names = [line.split()[0] for line in listing.splitlines()
                             if line.startswith("  BP") and ".uaem" not in line.lower()]
        if final_stage_names != stage_names:
            raise RuntimeError("native recovery changed or duplicated the replacement stage; evidence: " + str(out))
        final_archive = out / "restored-original.blt"
        remaining_stage = out / "retained-replacement.blt"
        read_file(args, native_disk, "INTERRUPT.BLT", final_archive, log)
        if len(stage_names) == 1:
            read_file(args, native_disk, stage_names[0], remaining_stage, log)
    if (hashlib.sha256(final_archive.read_bytes()).hexdigest() != expected_hash or
            not remaining_stage.exists() or
            hashlib.sha256(remaining_stage.read_bytes()).hexdigest() != expected_stage_hash):
        raise RuntimeError("native recovery changed original or removed replacement stage; evidence: " + str(out))
    subprocess.run([str(linux), "t", str(final_archive)], check=True, timeout=120,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    subprocess.run([str(linux), "t", str(remaining_stage)], check=True, timeout=120,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                     text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"],
                                         cwd=ROOT, text=True).strip())
    state = [
        "result=PASS",
        "filesystem=" + args.filesystem.upper(),
        "interruption=FS-UAE process termination after native floppy buffer settle",
        "recovery=production boltpack r verified backup and stage, restored original",
        "replacement_stage=retained and Linux integrity-tested",
        "original_archive_sha256=" + expected_hash,
        "backup_name=" + backup_names[0],
        "stage_name=" + stage_names[0],
        "fsuae=" + subprocess.check_output([args.fs_uae, "--version"], text=True).splitlines()[0],
        "xdftool_path=" + str(args.xdftool),
        "commit=" + commit,
        "worktree_dirty=" + str(dirty).lower(),
    ]
    (out / "result.txt").write_text("\n".join(state) + "\n", encoding="ascii")
    status = subprocess.check_output(["git", "status", "--short"], cwd=ROOT,
                                     text=True).splitlines()
    diff = subprocess.check_output(["git", "diff", "--binary", "HEAD"],
                                   cwd=ROOT)
    (out / "tracked-diff.patch").write_bytes(diff)
    source_paths = [
        ROOT / "Makefile",
        ROOT / "test/check_fsuae_native_recovery.py",
        ROOT / "test/reference/interoperability/amiga-native-recovery.json",
        ROOT / "src/frontend/amiga/main.c",
        ROOT / "src/core/archiver.c",
        ROOT / "src/core/compress.c",
        ROOT / "src/core/boltlite.c",
        ROOT / "src/core/boltlz.c",
        ROOT / "src/core/fluff.c",
        ROOT / "src/core/bp_codepage.c",
    ]
    fsuae_version = subprocess.check_output([args.fs_uae, "--version"],
                                            text=True).splitlines()[0]
    xdftool_version = "unknown"
    xdftool_venv_python = Path(args.xdftool).resolve().parent / "python"
    if xdftool_venv_python.is_file():
        version_result = subprocess.run(
            [str(xdftool_venv_python), "-c",
             "import importlib.metadata; print(importlib.metadata.version('amitools'))"],
            capture_output=True, text=True, check=False)
        if version_result.returncode == 0:
            xdftool_version = version_result.stdout.strip()
    record = {
        "purpose": "Native Amiga %s interrupted-publication recovery" % args.filesystem.upper(),
        "date": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "result": "PASS: original archive restored byte-for-byte; backup consumed; valid replacement stage retained",
        "revision": commit,
        "worktree_status": status,
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "environment": {
            "platform": platform.platform(),
            "python": sys.version.split()[0],
            "fsuae": fsuae_version,
            "xdftool": str(Path(args.xdftool).resolve()),
            "amitools": xdftool_version,
            "filesystem": args.filesystem.upper(),
            "rom_sha256": sha256(rom),
            "workbench_adf_sha256": sha256(workbench),
        },
        "inputs": {
            "linux_cli": {"path": str(linux), "sha256": sha256(linux)},
            "pause_cli": {"path": str(pause_cli), "sha256": sha256(pause_cli)},
            "recovery_cli": {"path": str(recovery_cli), "sha256": sha256(recovery_cli)},
            "independent_manifest": {
                "path": "test/reference/interoperability/amiga-native-recovery.json",
                "sha256": sha256(manifest_path),
            },
        },
        "payloads": {
            "original": {"size": len(original_data),
                          "sha256": hashlib.sha256(original_data).hexdigest()},
            "replacement": {"size": len(replacement_data),
                             "sha256": hashlib.sha256(replacement_data).hexdigest()},
            "original_archive": {"sha256": expected_hash},
            "backup_archive": {"sha256": hashlib.sha256(backup_file.read_bytes()).hexdigest()},
            "replacement_stage": {"sha256": expected_stage_hash},
            "restored_archive": {"sha256": hashlib.sha256(final_archive.read_bytes()).hexdigest()},
        },
        "procedure": [
            "Native CLI creates the original archive on the selected Amiga floppy filesystem.",
            "Native CLI pauses after backup publication; FS-UAE is terminated after the buffer-settle interval.",
            "Backup and replacement stage are extracted from the floppy and integrity-tested by Linux.",
            "Production recovery command runs natively and restores the original; backup is consumed and verified stage retained.",
        ],
        "command_logs": [
            "setup.log", "publish-fs-uae.log", "inspect.log", "inspect-window.log",
            "recovery-setup.log", "recovery-fs-uae.log", "final-inspect.log",
        ],
        "source_sha256": {path.relative_to(ROOT).as_posix(): sha256(path)
                          for path in source_paths if path.is_file()},
        "artifacts": artifact_index(out),
        "limitations": [
            "This is deterministic process termination under FS-UAE, not host power loss or physical Amiga hardware durability.",
            "The test uses a host-mounted directory for Workbench tools and a native OFS/FFS floppy for the archive transaction.",
        ],
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print("PASS: Amiga %s recovery restored the original and retained the valid replacement stage; evidence: %s" %
          (args.filesystem.upper(), out / "run.json"))


if __name__ == "__main__":
    main()
