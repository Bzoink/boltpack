#!/usr/bin/env python3
"""Cause and recover a real FAT12 write-protection error after backup rename."""
import argparse
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


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command, cwd, timeout=30):
    return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=timeout,
                          check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", required=True, type=Path)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", type=Path,
                        default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    parser.add_argument("--gui-prg", required=True, type=Path,
                        help="test build that pauses after moving the old target")
    parser.add_argument("--linux-cli", type=Path,
                        default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    if not os.environ.get("DISPLAY"):
        parser.error("run under Xvfb or a desktop display")
    for utility in ("mformat", "mcopy", "mdir"):
        if shutil.which(utility) is None:
            parser.error("FAT12 media-error test requires " + utility)
    tos = args.tos.expanduser().resolve(strict=True)
    gui = args.gui_prg.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    hconsole = args.hconsole.expanduser().resolve(strict=True)
    hatari = shutil.which(args.hatari)
    if hatari is None:
        parser.error("Hatari executable not found: " + args.hatari)
    out_root = args.output_root.resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="atari-gui-media-error-", dir=out_root))
    host = out / "host"
    host.mkdir()
    source = out / "source"
    source.mkdir()
    payload = b"new verified payload after real FAT12 write-protect failure\n" * 19
    original = b"old destination must survive the write-protect failure\n" * 11
    (source / "PAYLOAD.TXT").write_bytes(payload)
    archive = out / "ARCHIVE.BLT"
    created = run([str(linux), "c", "--store", str(archive), "PAYLOAD.TXT"], source)
    (out / "archive-create.log").write_text(created.stdout, encoding="utf-8")
    if created.returncode != 0:
        raise RuntimeError("could not create archive fixture: " + str(out))
    if run([str(linux), "t", str(archive)], out).returncode != 0:
        raise RuntimeError("Linux rejected test archive: " + str(out))

    image = out / "floppy.st"
    for command in (
            ["mformat", "-i", str(image), "-C", "-f", "720", "::"],
            ["mmd", "-i", str(image), "::OUT"]):
        result = run(command, out)
        if result.returncode != 0:
            raise RuntimeError("could not prepare FAT12 fixture: " + result.stdout + str(out))
    program_copy = run(["mcopy", "-i", str(image), str(gui), "::BOLTPACK.PRG"], out)
    archive_copy = run(["mcopy", "-i", str(image), str(archive), "::ARCHIVE.BLT"], out)
    old_file = out / "original-target.txt"
    old_file.write_bytes(original)
    target_copy = run(["mcopy", "-i", str(image), str(old_file), "::OUT/PAYLOAD.TXT"], out)
    if any(result.returncode != 0 for result in (program_copy, archive_copy, target_copy)):
        raise RuntimeError("could not seed FAT12 fixture: " + str(out))

    script = out / "HCONSOLE.TXT"
    script.write_text("\n".join((
        "sleep 20",
        "keydown 29", "keypress o", "keyup 29",
        "sleep 3", "text ARCHIVE.BLT", "keypress 28",
        "sleep 4",
        "keydown 29", "keypress e", "keyup 29",
        "sleep 3", "text OUT", "keypress 28",
        "",
    )), encoding="ascii")
    env = os.environ.copy()
    env.pop("SDL_VIDEODRIVER", None)
    env.pop("SDL_AUDIODRIVER", None)
    env["PYTHONUNBUFFERED"] = "1"
    env["PATH"] = str(Path(hatari).resolve().parent) + os.pathsep + env.get("PATH", "")
    command = [sys.executable, str(hconsole), str(script), "--", "--machine", "ste",
               "--tos", str(tos), "--sound", "off", "--confirm-quit", "off",
               "--fast-forward", "on", "--fast-boot", "on", "--memsize", "4",
               "--disk-a", str(image), "--protect-floppy", "off", "--harddrive",
               str(host), "--auto", r"A:\BOLTPACK.PRG"]
    log_path = out / "hatari.log"
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(command, cwd=out, env=env, stdin=subprocess.PIPE,
                                   stdout=log, stderr=subprocess.STDOUT, text=True,
                                   start_new_session=True)
        def send(line):
            process.stdin.write(line + "\n")
            process.stdin.flush()

        def wait_option(value, deadline):
            while time.monotonic() < deadline and process.poll() is None:
                log_text = log_path.read_text(encoding="utf-8", errors="replace")
                if "Command line with '2' arguments:" in log_text:
                    response = log_text.rsplit("Command line with '2' arguments:", 1)[-1]
                    if "'--protect-floppy'" in response and "'" + value + "'" in response:
                        return True
                time.sleep(0.05)
            return False

        try:
            ready = host / "READY.TST"
            deadline = time.monotonic() + 120
            while time.monotonic() < deadline and process.poll() is None and not ready.exists():
                time.sleep(0.05)
            if process.poll() is not None or not ready.is_file():
                raise RuntimeError("GUI did not reach the post-backup pause: " + str(out))
            send("--protect-floppy on")
            if not wait_option("on", time.monotonic() + 10):
                raise RuntimeError("Hatari did not accept runtime floppy protection: " + str(out))
            time.sleep(0.5)
            (host / "RELEASE.TST").write_text("continue into protected rename\n")
            # GEMDOS displays a write-protect retry/abort prompt. EmuTOS accepts
            # its Abort mnemonic followed by Return.
            time.sleep(0.5)
            send("screenshot")
            time.sleep(0.5)
            send("keypress a")
            send("keypress 28")
            error_marker = host / "ERROR.TST"
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline and process.poll() is None and not error_marker.exists():
                time.sleep(0.05)
            if process.poll() is not None or not error_marker.is_file():
                raise RuntimeError("write-protected publication did not report an error: " + str(out))
            error_text = error_marker.read_text(encoding="ascii", errors="replace")
            if "commit_rc=" not in error_text or "committed=0" not in error_text:
                raise RuntimeError("write-protected failure did not return an unpublished commit error: " + str(out))
            send("quit")
            process.stdin.close()
            process.wait(timeout=30)
            if process.returncode != 0:
                raise RuntimeError("Hatari exited unsuccessfully: " + str(out))
        except BaseException:
            if process.poll() is None:
                try:
                    send("quit")
                    process.stdin.close()
                    process.wait(timeout=5)
                except Exception:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=30)
            raise
        finally:
            if process.stdin and not process.stdin.closed:
                process.stdin.close()

    listing = run(["mdir", "-a", "-b", "-i", str(image), "::OUT"], out)
    names = [Path(line.strip()).name.upper()
             for line in listing.stdout.splitlines() if line.strip()]
    backups = [name for name in names if name.startswith("BR")]
    stages = [name for name in names
              if name.startswith("BP") and name != "BPRECOVR.JRN"]
    if (listing.returncode != 0 or len(backups) != 1 or len(stages) != 1 or
            any(name.startswith("PAYLOAD") for name in names)):
        raise RuntimeError("write-protected failure state lacks target/backup/stage: " + str(out))
    backup_copy = out / "observed-backup.bin"
    stage_copy = out / "observed-stage.bin"
    journal_copy = out / "observed-journal.bin"
    backup_result = run(["mcopy", "-o", "-i", str(image),
                         "::OUT/" + backups[0], str(backup_copy)], out)
    stage_result = run(["mcopy", "-o", "-i", str(image),
                        "::OUT/" + stages[0], str(stage_copy)], out)
    journal_result = run(["mcopy", "-o", "-i", str(image),
                          "::OUT/BPRECOVR.JRN", str(journal_copy)], out)
    if (backup_result.returncode != 0 or stage_result.returncode != 0 or
            journal_result.returncode != 0 or backup_copy.read_bytes() != original or
            stage_copy.read_bytes() != payload):
        raise RuntimeError("protected failure did not preserve exact transaction bytes: " + str(out))
    failure_state = {
        "backup_name": backups[0], "stage_name": stages[0],
        "backup_sha256": sha256(backup_copy), "stage_sha256": sha256(stage_copy),
        "journal_sha256": sha256(journal_copy),
    }
    shutil.copyfile(image, out / "write-protected-failure-state.st")

    retry_script = out / "RETRY.TXT"
    retry_script.write_text("\n".join((
        "sleep 20",
        "keydown 29", "keypress o", "keyup 29",
        "sleep 3", "text ARCHIVE.BLT", "keypress 28",
        "sleep 4",
        "keydown 29", "keypress e", "keyup 29",
        "sleep 3", "text OUT", "keypress 28",
        "sleep 20", "keypress 28", "sleep 1", "quit", "",
    )), encoding="ascii")
    retry_command = [sys.executable, str(hconsole), str(retry_script), "--exit", "--",
                     "--machine", "ste", "--tos", str(tos), "--sound", "off",
                     "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
                     "--memsize", "4", "--disk-a", str(image), "--protect-floppy", "off",
                     "--harddrive", str(host), "--auto", r"A:\BOLTPACK.PRG"]
    retry = run(retry_command, out, timeout=150)
    (out / "retry-hatari.log").write_text(retry.stdout, encoding="utf-8")
    if retry.returncode != 0:
        raise RuntimeError("production retry emulator failed: " + str(out))
    recovered = out / "recovered-target.txt"
    result = run(["mcopy", "-o", "-i", str(image), "::OUT/PAYLOAD.TXT",
                  str(recovered)], out)
    if result.returncode != 0 or recovered.read_bytes() != payload:
        raise RuntimeError("retry did not restore and publish exact replacement bytes: " + str(out))
    final_listing = run(["mdir", "-a", "-b", "-i", str(image), "::OUT"], out)
    final_names = [Path(line.strip()).name.upper()
                   for line in final_listing.stdout.splitlines() if line.strip()]
    remaining = [name for name in final_names
                 if name.startswith(("BP", "BR")) and name != "BPRECOVR.JRN"]
    journal = run(["mcopy", "-i", str(image), "::OUT/BPRECOVR.JRN",
                   str(out / "remaining-journal.bin")], out)
    if final_listing.returncode != 0 or remaining or journal.returncode == 0:
        raise RuntimeError("recovered retry left transaction artifacts: " + str(out))

    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                       text=True).strip()
    status = subprocess.check_output(["git", "status", "--short"], cwd=ROOT,
                                     text=True).splitlines()
    diff = subprocess.check_output(["git", "diff", "--binary"], cwd=ROOT)
    (out / "source-status.txt").write_text("\n".join(status) + "\n", encoding="utf-8")
    (out / "source-diff.patch").write_bytes(diff)
    record = {
        "purpose": "Exercise production Atari GEM rollback after an actual FAT12 write-protection error",
        "result": "PASS: Hatari write protection caused the real stage-to-target GEMDOS rename to fail after the old target was moved; exact old backup, verified replacement stage, and journal remained, then production retry restored and published exact bytes with no transaction artifacts.",
        "hatari": run([hatari, "--version"], out).stdout.strip(),
        "rom": tos.name,
        "rom_sha256": sha256(tos),
        "gui_sha256": sha256(gui),
        "linux_cli_sha256": sha256(linux),
        "runner_sha256": sha256(Path(__file__)),
        "source_sha256": sha256(ROOT / "src/frontend/atari_gui/main.c"),
        "source_revision": revision,
        "source_status": status,
        "source_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "archive_sha256": sha256(archive),
        "floppy_final_sha256": sha256(image),
        "failure_state_image_sha256": sha256(out / "write-protected-failure-state.st"),
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "original_sha256": hashlib.sha256(original).hexdigest(),
        "failure_state": failure_state,
        "commit_error": error_text.strip(),
        "recovered_target_sha256": sha256(recovered),
    }
    record["artifacts"] = {
        str(path.relative_to(out)): {"size": path.stat().st_size,
                                     "sha256": sha256(path)}
        for path in sorted(out.rglob("*")) if path.is_file()
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print("Atari GEM real FAT12 write-protection rollback and retry passed")
    print("Evidence: " + str(out / "run.json"))


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.TimeoutExpired, subprocess.CalledProcessError,
            RuntimeError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        sys.exit(1)
