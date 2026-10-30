#!/usr/bin/env python3
"""Kill Atari GEM after old-target backup and record transaction artifacts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def run(command, cwd, env=None, timeout=30):
    return subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=timeout)


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", type=Path, required=True)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", type=Path,
                        default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    parser.add_argument("--gui-prg", type=Path, required=True,
                        help="test build that pauses after moving the old target")
    parser.add_argument("--recovery-gui", type=Path, required=True,
                        help="test build that records verified restoration after journal recovery")
    parser.add_argument("--final-gui", type=Path, required=True,
                        help="normal production GUI used to retry after restoration is observed")
    parser.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    if not os.environ.get("DISPLAY"):
        raise RuntimeError("run under Xvfb or an X11 display")
    hatari = shutil.which(args.hatari)
    if hatari is None:
        raise RuntimeError("Hatari executable not found: " + args.hatari)
    tos = args.tos.expanduser().resolve(strict=True)
    gui = args.gui_prg.resolve(strict=True)
    recovery_gui = args.recovery_gui.resolve(strict=True)
    final_gui = args.final_gui.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    hconsole = args.hconsole.expanduser().resolve(strict=True)
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="atari-gui-interrupt-backup-", dir=output_root))
    host = out / "host"
    host.mkdir()
    payload = b"verified replacement payload for interruption window\n" * 37
    original = b"original target before interruption\n" * 13
    (host / "PAYLOAD.TXT").write_bytes(payload)
    (host / "OUT").mkdir()
    target = host / "OUT" / "PAYLOAD.TXT"
    target.write_bytes(original)
    shutil.copy2(gui, host / "BOLTPACK.PRG")
    created = run([str(linux), "c", "ARCHIVE.BLT", "PAYLOAD.TXT"], host)
    (out / "archive-create.log").write_text(created.stdout, encoding="utf-8")
    if created.returncode != 0:
        raise RuntimeError("could not create interruption fixture: " + str(out))
    script = out / "HCONSOLE.TXT"
    script.write_text("\n".join((
        "sleep 20",
        "keydown 29", "keypress o", "keyup 29",
        "sleep 3", "text ARCHIVE.BLT", "keypress 28",
        "sleep 4",
        "keydown 29", "keypress e", "keyup 29",
        "sleep 3", "text OUT", "keypress 28",
        "sleep 180", "quit", "",
    )), encoding="ascii")
    env = os.environ.copy()
    env.pop("SDL_VIDEODRIVER", None)
    env.pop("SDL_AUDIODRIVER", None)
    env["PATH"] = str(Path(hatari).resolve().parent) + os.pathsep + env.get("PATH", "")
    command = [sys.executable, str(hconsole), str(script), "--exit", "--",
               "--machine", "ste", "--tos", str(tos), "--sound", "off",
               "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
               "--memsize", "4", "--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG"]
    log_path = out / "hatari.log"
    log = log_path.open("w", encoding="utf-8")
    process = subprocess.Popen(command, cwd=out, env=env, stdout=log,
                               stderr=subprocess.STDOUT, text=True,
                               start_new_session=True)
    marker = host / "OUT" / "HOLD.TST"
    deadline = time.monotonic() + 180
    while time.monotonic() < deadline and process.poll() is None:
        if marker.is_file() and not target.exists():
            break
        time.sleep(0.02)
    if not marker.is_file() or target.exists():
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=30)
        log.close()
        raise RuntimeError("did not observe the post-backup/pre-publish state: " + str(out))
    backups = sorted(path for path in (host / "OUT").iterdir()
                     if path.name.startswith("BR"))
    stages = sorted(path for path in (host / "OUT").iterdir()
                    if path.name.startswith("BP") and len(path.name) == 8 and
                    "." not in path.name)
    journal = host / "OUT" / "BPRECOVR.JRN"
    if (len(backups) != 1 or len(stages) != 1 or
            backups[0].read_bytes() != original or stages[0].read_bytes() != payload or
            not journal.is_file()):
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=30)
        log.close()
        raise RuntimeError("interruption artifacts do not contain exact original and replacement bytes: " + str(out))
    os.killpg(process.pid, signal.SIGKILL)
    process.wait(timeout=30)
    log.close()
    crash_target_missing = not target.exists()
    crash_backup_name = backups[0].name
    crash_stage_name = stages[0].name
    crash_journal_sha256 = hashlib.sha256(journal.read_bytes()).hexdigest()
    (host / "OUT" / "HOLD.TST").unlink()
    shutil.copy2(recovery_gui, host / "BOLTPACK.PRG")
    recovery_dir = out / "automatic-recovery"
    recovery_dir.mkdir()
    recovery_script = recovery_dir / "HCONSOLE.TXT"
    recovery_script.write_text("\n".join((
        "sleep 20",
        "keydown 29", "keypress o", "keyup 29",
        "sleep 3", "text ARCHIVE.BLT", "keypress 28",
        "sleep 4",
        "keydown 29", "keypress e", "keyup 29",
        "sleep 3", "text OUT", "keypress 28",
        "sleep 20", "screenshot", "sleep 2", "quit", "",
    )), encoding="ascii")
    recovery_command = [sys.executable, str(hconsole), str(recovery_script), "--exit", "--",
                        "--machine", "ste", "--tos", str(tos), "--sound", "off",
                        "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
                        "--memsize", "4", "--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG"]
    recovery_log = (out / "automatic-recovery.log").open("w", encoding="utf-8")
    recovery_process = subprocess.Popen(recovery_command, cwd=recovery_dir, env=env,
                                        stdout=recovery_log, stderr=subprocess.STDOUT,
                                        text=True, start_new_session=True)
    restored_marker = host / "OUT" / "RECOVER.TST"
    recovery_deadline = time.monotonic() + 180
    while time.monotonic() < recovery_deadline and recovery_process.poll() is None:
        if restored_marker.is_file() and target.read_bytes() == original:
            break
        time.sleep(0.02)
    if (not restored_marker.is_file() or target.read_bytes() != original or
            recovery_process.poll() is not None):
        if recovery_process.poll() is None:
            os.killpg(recovery_process.pid, signal.SIGKILL)
        recovery_process.wait(timeout=30)
        recovery_log.close()
        raise RuntimeError("journal recovery did not restore the exact original before retry: " + str(out))
    os.killpg(recovery_process.pid, signal.SIGKILL)
    recovery_process.wait(timeout=30)
    recovery_log.close()
    if (restored_marker.read_bytes() != b"" or
            any(name.startswith(("BP", "BR")) for name in
                (path.name for path in (host / "OUT").iterdir())) or journal.exists()):
        raise RuntimeError("journal recovery did not restore the exact original before retry: " + str(out))
    recovery_marker_sha256 = hashlib.sha256(restored_marker.read_bytes()).hexdigest()
    restored_marker.unlink()
    shutil.copy2(final_gui, host / "BOLTPACK.PRG")
    final_dir = out / "production-retry"
    final_dir.mkdir()
    final_script = final_dir / "HCONSOLE.TXT"
    final_script.write_text(recovery_script.read_text(encoding="ascii"), encoding="ascii")
    final_command = [sys.executable, str(hconsole), str(final_script), "--exit", "--",
                     "--machine", "ste", "--tos", str(tos), "--sound", "off",
                     "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
                     "--memsize", "4", "--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG"]
    final_log = (out / "production-retry.log").open("w", encoding="utf-8")
    final_process = subprocess.Popen(final_command, cwd=final_dir, env=env,
                                     stdout=final_log, stderr=subprocess.STDOUT,
                                     text=True, start_new_session=True)
    try:
        final_process.wait(timeout=180)
    except subprocess.TimeoutExpired:
        os.killpg(final_process.pid, signal.SIGKILL)
        final_process.wait(timeout=30)
        final_log.close()
        raise RuntimeError("normal production retry did not finish: " + str(out))
    final_log.close()
    final_files = sorted(path.name for path in (host / "OUT").iterdir())
    if (final_process.returncode != 0 or target.read_bytes() != payload or
            any(name.startswith(("BP", "BR")) for name in final_files) or
            journal.exists() or not (final_dir / "grab0001.png").is_file()):
        raise RuntimeError("production retry did not recover and complete extraction: " + str(out))
    revision = run(["git", "rev-parse", "HEAD"], ROOT)
    status = run(["git", "status", "--short"], ROOT)
    diff = run(["git", "diff", "--binary", "--", "Makefile",
                "src/frontend/atari_gui/main.c",
                "test/check_hatari_gui_interruption.py"], ROOT)
    (out / "source-status.txt").write_text(status.stdout, encoding="utf-8")
    (out / "source-diff.patch").write_text(diff.stdout, encoding="utf-8")
    result = {
        "purpose": "verify automatic recovery after process interruption between backup and stage publication",
        "injection": "test-only pause marker after the existing target was moved and verified; Hatari killed, then production GUI retried",
        "hatari": run([hatari, "--version"], out).stdout.strip(),
        "tos_rom": tos.name,
        "tos_sha256": hashlib.sha256(tos.read_bytes()).hexdigest(),
        "gui_sha256": hashlib.sha256(gui.read_bytes()).hexdigest(),
        "recovery_gui_sha256": hashlib.sha256(recovery_gui.read_bytes()).hexdigest(),
        "final_gui_sha256": hashlib.sha256(final_gui.read_bytes()).hexdigest(),
        "frontend_source_sha256": hashlib.sha256((ROOT / "src/frontend/atari_gui/main.c").read_bytes()).hexdigest(),
        "source_revision": revision.stdout.strip(),
        "source_status_sha256": hashlib.sha256(status.stdout.encode("utf-8")).hexdigest(),
        "source_diff_sha256": hashlib.sha256(diff.stdout.encode("utf-8")).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "original_sha256": hashlib.sha256(original).hexdigest(),
        "replacement_sha256": hashlib.sha256(payload).hexdigest(),
        "target_missing_after_kill": crash_target_missing,
        "crash_backup_name": crash_backup_name,
        "crash_backup_sha256": hashlib.sha256(original).hexdigest(),
        "crash_stage_name": crash_stage_name,
        "crash_stage_sha256": hashlib.sha256(payload).hexdigest(),
        "crash_journal_sha256": crash_journal_sha256,
        "pause_marker": marker.name,
        "post_recovery_target_sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
        "restored_original_marker_sha256": recovery_marker_sha256,
        "post_recovery_leftover_names": final_files,
        "automatic_recovery_screenshot": None,
        "production_retry_screenshot": "production-retry/grab0001.png",
        "result": "PASS: journal recovery restored the original byte-for-byte and removed the incomplete transaction artifacts; normal production GUI then extracted the exact archive payload",
    }
    (out / "run.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("PASS: production GUI recovered the interrupted backup/stage transaction and extracted exact bytes; evidence: " + str(out))


if __name__ == "__main__":
    main()
