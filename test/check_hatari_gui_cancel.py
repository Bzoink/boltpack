#!/usr/bin/env python3
"""Exercise production Atari GEM selector, active-cancel, and interruption paths."""
import hashlib
import json
import os
from pathlib import Path
import signal
import shutil
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
    parser.add_argument("--gui-prg", type=Path,
                        default=ROOT / "build/atari/boltpack.prg")
    parser.add_argument("--linux-cli", type=Path,
                        default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build")
    parser.add_argument("--active-extraction", action="store_true",
                        help="cancel during staged block extraction")
    parser.add_argument("--interrupt-extraction", action="store_true",
                        help="terminate Hatari during staged extraction")
    parser.add_argument("--substitute-stage", action="store_true",
                        help="replace the live stage with a same-size, same-mtime file")
    parser.add_argument("--substitute-parent", action="store_true",
                        help="move the output directory and replace its path with a symlink")
    args = parser.parse_args()
    if sum((args.active_extraction, args.interrupt_extraction,
            args.substitute_stage, args.substitute_parent)) > 1:
        parser.error("choose only one extraction action")
    extraction_in_progress = (args.active_extraction or args.interrupt_extraction or
                              args.substitute_stage or args.substitute_parent)

    if not os.environ.get("DISPLAY"):
        raise RuntimeError("run this diagnostic under Xvfb or an X11 display")
    if shutil.which("xdotool") is None:
        raise RuntimeError("xdotool is required for GEM pointer input")
    hatari_binary = shutil.which(args.hatari)
    if hatari_binary is None:
        raise RuntimeError("Hatari executable not found: " + args.hatari)
    tos = args.tos.expanduser().resolve(strict=True)
    gui = args.gui_prg.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    hconsole = args.hconsole.expanduser().resolve(strict=True)
    out_root = args.output_root.resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="atari-gui-cancel-", dir=out_root))
    host = out / "host"
    host.mkdir()
    output_dir = host / "OUT"
    output_dir.mkdir()
    if extraction_in_progress:
        payload_name = "PAYLOAD.BIN"
        payload_size = (16 if args.interrupt_extraction else
                        1 if (args.substitute_stage or args.substitute_parent) else
                        64) * 1024 * 1024
        chunk = bytes(range(256)) * 256
        with (host / payload_name).open("wb") as payload:
            for _ in range(payload_size // len(chunk)):
                payload.write(chunk)
        sentinel = b"pre-existing destination must survive cancellation\n"
        (host / "OUT" / payload_name).write_bytes(sentinel)
        create_args = [str(linux), "c", "--store", "ARCHIVE.BLT", payload_name]
    else:
        payload_name = "PAYLOAD.TXT"
        sentinel = None
        (host / payload_name).write_bytes(b"cancel selector must preserve this destination\n")
        create_args = [str(linux), "c", "ARCHIVE.BLT", payload_name]
    shutil.copy2(gui, host / "BOLTPACK.PRG")

    created = run(create_args, host, timeout=180)
    (out / "create.log").write_text(created.stdout, encoding="utf-8")
    if created.returncode != 0:
        raise RuntimeError("could not build the Atari cancellation archive: " + str(out))
    if extraction_in_progress:
        verified = run([str(linux), "t", str(host / "ARCHIVE.BLT")], out, timeout=90)
        (out / "archive-test.log").write_text(verified.stdout, encoding="utf-8")
        if verified.returncode != 0:
            raise RuntimeError("Linux rejected the active-cancellation archive: " + str(out))
    substituted_stage = None
    parked_stage = None
    replacement_digest = None
    moved_output_dir = None
    redirect_output_dir = None

    # Open a valid archive and request extraction. The host pointer clicks
    # either the selector Abort button or the in-window progress Cancel button.
    script = out / "HCONSOLE.TXT"
    commands = [
        "sleep 20",
        "keydown 29", "keypress o", "keyup 29",
        "sleep 3", "text ARCHIVE.BLT", "keypress 28",
        "sleep 5",
    ]
    if extraction_in_progress:
        # The larger archive can take several seconds to index on emulated
        # 68k hardware. Leave time for the open and destination dialogs to
        # finish before capturing their states or injecting the next key.
        commands.extend(("sleep 30", "screenshot", "keydown 29", "keypress e", "keyup 29",
                         "sleep 10", "screenshot", "text OUT", "keypress 28",
                         "sleep 5", "screenshot"))
        if args.interrupt_extraction:
            commands.extend(("sleep 300",))
        elif args.substitute_stage or args.substitute_parent:
            commands.extend(("sleep 60", "screenshot"))
        else:
            commands.extend(("sleep 20", "screenshot"))
    else:
        commands.extend(("keydown 29", "keypress e", "keyup 29", "sleep 4",
                         "screenshot", "sleep 30", "screenshot"))
    commands.extend(("sleep 1", "quit", ""))
    script.write_text("\n".join(commands), encoding="ascii")
    env = os.environ.copy()
    env.pop("SDL_VIDEODRIVER", None)
    env.pop("SDL_AUDIODRIVER", None)
    env["PATH"] = str(Path(hatari_binary).resolve().parent) + os.pathsep + env.get("PATH", "")
    command = [
        sys.executable, str(hconsole), str(script), "--exit", "--",
        "--machine", "ste", "--tos", str(tos), "--sound", "off",
        "--confirm-quit", "off", "--fast-forward",
        "off" if extraction_in_progress else "on", "--fast-boot", "on",
        "--memsize", "4", "--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG",
    ]
    hatari_log = (out / "hatari.log").open("w", encoding="utf-8")
    process = subprocess.Popen(command, cwd=out, env=env, stdout=hatari_log,
                               stderr=subprocess.STDOUT, text=True,
                               start_new_session=True)
    staged_bytes_before_cancel = None
    try:
        first_screen = out / ("grab0002.png" if extraction_in_progress else "grab0001.png")
        deadline = time.monotonic() + (180 if extraction_in_progress else 90)
        while time.monotonic() < deadline and process.poll() is None:
            if first_screen.exists():
                break
            time.sleep(0.25)
        if not first_screen.exists():
            raise RuntimeError("destination-selector screenshot was not captured: " + str(out))
        if not extraction_in_progress:
            time.sleep(1)
        windows = run(["xdotool", "search", "--onlyvisible", "--name", "Hatari"], out)
        if windows.returncode != 0 or not windows.stdout.strip():
            raise RuntimeError("Hatari window was not visible: " + str(out))
        window = windows.stdout.splitlines()[0].strip()
        click = ["xdotool", "windowfocus", "--sync", window]
        run(click, out, timeout=10)
        mouse_x, mouse_y = ("690", "148") if extraction_in_progress else ("600", "430")
        if args.interrupt_extraction:
            # Killing the emulator after the sibling stage has received bytes
            # exercises the crash window without relying on scripted delays.
            stage = None
            stage_deadline = time.monotonic() + 180
            while time.monotonic() < stage_deadline and process.poll() is None:
                candidates = [path for path in (host / "OUT").iterdir()
                              if path.name.startswith("BP") and
                              path.name != "BPRECOVR.JRN"]
                if candidates and candidates[0].is_file() and candidates[0].stat().st_size > 0:
                    stage = candidates[0]
                    staged_bytes_before_cancel = stage.stat().st_size
                    break
                time.sleep(0.02)
            if stage is None:
                raise RuntimeError("extraction never produced a non-empty stage file: " + str(out))
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=30)
        elif (args.active_extraction or args.substitute_stage or
              args.substitute_parent):
            # Observe a non-empty staged file to prove extraction has entered
            # its streaming writer before sending Cancel. This avoids relying
            # on emulator wall-clock delays or input racing the selector.
            stage = None
            stage_deadline = time.monotonic() + 180
            while time.monotonic() < stage_deadline and process.poll() is None:
                candidates = [path for path in (host / "OUT").iterdir()
                              if path.name.startswith("BP") and
                              path.name != "BPRECOVR.JRN"]
                if candidates and candidates[0].is_file() and candidates[0].stat().st_size > 0:
                    stage = candidates[0]
                    staged_bytes_before_cancel = stage.stat().st_size
                    break
                time.sleep(0.05)
            if stage is None:
                raise RuntimeError("extraction never produced a non-empty stage file: " + str(out))
            if args.substitute_stage:
                parked_stage = out / "parked-original-stage.bin"
                os.link(stage, parked_stage)
                replacement = out / "replacement-stage.bin"
                replacement_data = b"X" * payload_size
                replacement.write_bytes(replacement_data)
                replacement_digest = hashlib.sha256(replacement_data).hexdigest()
                os.replace(replacement, stage)
                substituted_stage = stage
            elif args.substitute_parent:
                moved_output_dir = out / "OUT-MOVED"
                redirect_output_dir = out / "OUT-REDIRECT"
                redirect_output_dir.mkdir()
                (redirect_output_dir / payload_name).write_bytes(sentinel)
                os.rename(output_dir, moved_output_dir)
                os.symlink(redirect_output_dir, output_dir)
                substituted_stage = stage
            if args.substitute_stage or args.substitute_parent:
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline and process.poll() is None:
                    if args.substitute_stage:
                        original_stat = parked_stage.stat()
                        if original_stat.st_size == payload_size:
                            os.utime(stage, ns=(original_stat.st_atime_ns,
                                               original_stat.st_mtime_ns))
                    elif stage.exists() and stage.stat().st_size == payload_size:
                        pass
                    time.sleep(0.01)
                if process.poll() is None:
                    raise RuntimeError("Hatari did not finish after stage substitution: " + str(out))
                process.wait(timeout=30)
        if not args.interrupt_extraction:
            if not (args.substitute_stage or args.substitute_parent):
                click = ["xdotool", "mousemove", "--window", window, mouse_x, mouse_y]
                moved = run(click, out, timeout=10)
                if moved.returncode != 0:
                    raise RuntimeError("could not move pointer to cancellation control: " + str(out))
        if args.interrupt_extraction:
            pass
        elif args.active_extraction:
            pressed = run(["xdotool", "mousedown", "1"], out, timeout=10)
            if pressed.returncode != 0:
                raise RuntimeError("could not hold the progress Cancel button: " + str(out))
            try:
                # Keep the button held until a codec-block callback observes
                # it. Some TOS/emulator combinations process blocks slowly.
                process.wait(timeout=180)
            finally:
                run(["xdotool", "mouseup", "1"], out, timeout=10)
        elif not (args.substitute_stage or args.substitute_parent):
            click_log = run(["xdotool", "click", "1"], out, timeout=10)
            if click_log.returncode != 0:
                raise RuntimeError("could not click GUI cancellation control: " + str(out))
            process.wait(timeout=90)
    except Exception:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=5)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        hatari_log.close()
        raise
    hatari_log.close()
    before_screenshot = "grab0003.png" if extraction_in_progress else "grab0001.png"
    after_screenshot = "grab0004.png" if (args.active_extraction or args.substitute_stage or
                                             args.substitute_parent) else (
        "grab0003.png" if args.interrupt_extraction else "grab0002.png")
    input_record = out / ("INTERRUPTION.txt" if args.interrupt_extraction else
                          "STAGE-SUBSTITUTION.txt" if args.substitute_stage else
                          "PARENT-SUBSTITUTION.txt" if args.substitute_parent else
                          "CANCEL-INPUT.txt")
    input_record.write_text(
        ("The output directory was moved and its selected pathname replaced by a symlink "
         "to a separate directory while extraction was writing\n" if args.substitute_parent else
         "A same-size replacement was atomically moved over the open stage; its timestamp "
         "was synchronized with the still-open original before extraction completed\n" if args.substitute_stage else
         "Hatari process group force-terminated after a non-empty staged file appeared; "
         "the pre-existing target is checked after process exit\n" if args.interrupt_extraction else
         "Xvfb window-relative Cancel press held until Hatari returns, after a non-empty "
         "staged file appeared, at 690,148; "
         "grab0001.png is the loaded archive and grab0002.png is the destination selector\n" if args.active_extraction else
         "Xvfb window-relative click at 600,430; selector Abort button visible "
         "in grab0001.png\n"),
        encoding="utf-8")
    if process.returncode != 0 and not args.interrupt_extraction:
        raise RuntimeError("Hatari GUI cancellation run failed: " + str(out))
    if args.active_extraction and not (out / after_screenshot).is_file():
        raise RuntimeError("post-cancellation screenshot is missing: " + str(out))
    if args.active_extraction and not (out / before_screenshot).is_file():
        raise RuntimeError("active-extraction screenshot is missing: " + str(out))
    if (args.substitute_stage or args.substitute_parent) and not (out / after_screenshot).is_file():
        raise RuntimeError("post-substitution screenshot is missing: " + str(out))
    output_dir = host / "OUT"
    if extraction_in_progress:
        expected = output_dir / payload_name
        if not expected.is_file() or expected.read_bytes() != sentinel:
            raise RuntimeError("incomplete extraction replaced the existing destination: " + str(out))
        leftovers = [path for path in output_dir.iterdir() if path != expected]
        interrupted_journal = output_dir / "BPRECOVR.JRN"
        interrupted_journal_sha256 = None
        if args.active_extraction:
            if leftovers:
                raise RuntimeError("cancelled extraction left staged output: " + str(out))
        elif args.substitute_stage:
            original_stat = parked_stage.stat()
            replacement_stat = substituted_stage.stat()
            if (original_stat.st_size != payload_size or
                    replacement_stat.st_size != payload_size or
                    original_stat.st_mtime_ns != replacement_stat.st_mtime_ns):
                raise RuntimeError("substitution fixture did not match completed stage size/time: " + str(out))
            if (len(leftovers) != 1 or leftovers[0] != substituted_stage or
                    hashlib.sha256(leftovers[0].read_bytes()).hexdigest() != replacement_digest):
                raise RuntimeError("substituted stage was published or removed: " + str(out))
        elif args.substitute_parent:
            moved_target = moved_output_dir / payload_name
            moved_entries = list(moved_output_dir.iterdir())
            moved_stages = [path for path in moved_entries
                            if path != moved_target and path.name.startswith("BP") and
                            path.name != "BPRECOVR.JRN"]
            moved_journal = moved_output_dir / "BPRECOVR.JRN"
            if (not output_dir.is_symlink() or not moved_target.is_file() or
                    moved_target.read_bytes() != sentinel or
                    expected.read_bytes() != sentinel or leftovers or
                    not moved_journal.is_file() or
                    len(moved_entries) != 2 + len(moved_stages) or
                    any(path.name != substituted_stage.name for path in moved_stages)):
                raise RuntimeError(
                    "parent substitution escaped or changed a destination: " + str(out) +
                    "; symlink=" + str(output_dir.is_symlink()) +
                    "; moved-target=" + str(moved_target.is_file()) +
                    "; target-preserved=" + str(moved_target.is_file() and moved_target.read_bytes() == sentinel) +
                    "; redirected-target-preserved=" + str(expected.is_file() and expected.read_bytes() == sentinel) +
                    "; leftovers=" + repr([path.name for path in leftovers]) +
                    "; moved-entries=" + repr([path.name for path in moved_entries]) +
                    "; stage-names=" + repr([path.name for path in moved_stages]) +
                    "; selected-stage=" + str(substituted_stage.name))
            if moved_stages and (len(moved_stages) != 1 or
                                 moved_stages[0].stat().st_size != payload_size or
                                 hashlib.sha256(moved_stages[0].read_bytes()).hexdigest() !=
                                 hashlib.sha256((host / payload_name).read_bytes()).hexdigest()):
                raise RuntimeError("parent substitution left an unexpected staged file: " + str(out))
        else:
            stages = [path for path in leftovers if path != interrupted_journal]
            if (not interrupted_journal.is_file() or len(stages) != 1 or
                    not stages[0].is_file() or not stages[0].name.startswith("BP") or
                    stages[0].stat().st_size != staged_bytes_before_cancel or
                    stages[0].stat().st_size >= payload_size):
                raise RuntimeError("interrupted extraction did not retain exactly one partial "
                                   "stage and recovery journal alongside the preserved target: " + str(out))
            interrupted_stage = stages[0]
            interrupted_journal_sha256 = hashlib.sha256(
                interrupted_journal.read_bytes()).hexdigest()
    elif list(output_dir.iterdir()):
        raise RuntimeError("cancelled extraction modified the destination: " + str(out))
    retry_result = None
    if args.interrupt_extraction:
        retry_dir = out / "retry"
        retry_dir.mkdir()
        retry_script = retry_dir / "HCONSOLE.TXT"
        retry_script.write_text("\n".join((
            "sleep 20",
            "keydown 29", "keypress o", "keyup 29",
            "sleep 3", "text ARCHIVE.BLT", "keypress 28",
            "sleep 30",
            "keydown 29", "keypress e", "keyup 29",
            "sleep 10", "text OUT", "keypress 28",
            "sleep 5", "screenshot", "sleep 180", "screenshot", "quit", "",
        )), encoding="ascii")
        retry_command = [
            sys.executable, str(hconsole), str(retry_script), "--exit", "--",
            "--machine", "ste", "--tos", str(tos), "--sound", "off",
            "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
            "--memsize", "4", "--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG",
        ]
        recovered = expected.read_bytes()
        source_payload = (host / payload_name).read_bytes()
        retried = run(retry_command, retry_dir, env=env, timeout=300)
        (retry_dir / "hatari.log").write_text(retried.stdout, encoding="utf-8")
        recovered = expected.read_bytes()
        retry_leftovers = [path for path in output_dir.iterdir() if path != expected]
        if (retried.returncode != 0 or recovered != source_payload or
                interrupted_journal.exists() or not interrupted_stage.is_file() or
                retry_leftovers != [interrupted_stage]):
            raise RuntimeError("retry after interruption did not restore the payload while "
                               "clearing the journal and retaining only the old partial stage: " + str(out))
        if not (retry_dir / "grab0001.png").is_file():
            raise RuntimeError("retry-after-interruption screenshot is missing: " + str(retry_dir))
        retry_result = {
            "result": "retry cleared the journal and restored exact archive bytes; the unverified prior partial stage remains",
            "payload_sha256": hashlib.sha256(recovered).hexdigest(),
            "partial_stage_name": interrupted_stage.name,
            "partial_stage_bytes": interrupted_stage.stat().st_size,
            "interrupted_journal_sha256": interrupted_journal_sha256,
            "screenshot": "retry/grab0001.png",
            "hatari_command": retry_command,
        }
    result = {
        "purpose": ("Atari GEM refuses parent-path symlink substitution during staged extraction" if args.substitute_parent else
                    "Atari GEM refuses same-size, same-mtime staged-file substitution" if args.substitute_stage else
                    "Atari GEM process interruption preserves the destination during staged extraction" if args.interrupt_extraction else
                    "Atari GEM progress Cancel preserves an existing destination" if args.active_extraction
                    else "Atari GEM destination-selector Abort preserves an existing empty destination"),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "hatari": run([hatari_binary, "--version"], out).stdout.strip(),
        "tos_rom": tos.name,
        "tos_sha256": hashlib.sha256(tos.read_bytes()).hexdigest(),
        "gui_sha256": hashlib.sha256(gui.read_bytes()).hexdigest(),
        "archive_sha256": hashlib.sha256((host / "ARCHIVE.BLT").read_bytes()).hexdigest(),
        "payload_bytes": payload_size if extraction_in_progress else None,
        "expected_target_sha256": hashlib.sha256(sentinel).hexdigest() if extraction_in_progress else None,
        "observed_target_sha256": hashlib.sha256(expected.read_bytes()).hexdigest() if extraction_in_progress else None,
        "input": ("move OUT to OUT-MOVED and replace OUT with a symlink to separate OUT-REDIRECT while stage is open" if args.substitute_parent else
                  "atomically replace the open stage with same-size data, then match its mtime to the parked original" if args.substitute_stage else
                  "Hatari/hconsole process group killed after observing a non-empty stage; no GUI Cancel input" if args.interrupt_extraction else
                  "xdotool held mouse button at Hatari window-relative 690,148 until Hatari returned" if args.active_extraction
                  else "xdotool mouse click at Hatari window-relative 600,430 (Abort button)"),
        "before_cancel_screenshot": before_screenshot if not (args.interrupt_extraction or args.substitute_stage or args.substitute_parent) else None,
        "after_click_screenshot": after_screenshot if not (args.interrupt_extraction or args.substitute_stage or args.substitute_parent) else None,
        "after_action_screenshot": after_screenshot if (args.active_extraction or args.substitute_stage or args.substitute_parent) else None,
        "pre_interrupt_screenshot": before_screenshot if args.interrupt_extraction else None,
        "archive_window_baseline": "grab0001.png" if extraction_in_progress else None,
        "destination_selector_screenshot": "grab0002.png" if extraction_in_progress else None,
        "staged_bytes_before_cancel": staged_bytes_before_cancel,
        "substituted_stage_name": substituted_stage.name if args.substitute_stage else None,
        "substituted_stage_bytes": substituted_stage.stat().st_size if args.substitute_stage else None,
        "substituted_stage_sha256": hashlib.sha256(substituted_stage.read_bytes()).hexdigest() if args.substitute_stage else None,
        "parked_original_stage_size": parked_stage.stat().st_size if args.substitute_stage else None,
        "substitution_size_mtime_match": (substituted_stage.stat().st_size == parked_stage.stat().st_size and
                                            substituted_stage.stat().st_mtime_ns == parked_stage.stat().st_mtime_ns)
                                           if args.substitute_stage else None,
        "moved_output_directory": str(moved_output_dir) if args.substitute_parent else None,
        "redirect_output_directory": str(redirect_output_dir) if args.substitute_parent else None,
        "selected_output_is_symlink": output_dir.is_symlink() if args.substitute_parent else None,
        "moved_target_sha256": hashlib.sha256((moved_output_dir / payload_name).read_bytes()).hexdigest() if args.substitute_parent else None,
        "redirect_target_sha256": hashlib.sha256((redirect_output_dir / payload_name).read_bytes()).hexdigest() if args.substitute_parent else None,
        "orphaned_stage_name": moved_stages[0].name if args.substitute_parent and moved_stages else None,
        "orphaned_stage_bytes": moved_stages[0].stat().st_size if args.substitute_parent and moved_stages else None,
        "orphaned_stage_sha256": hashlib.sha256(moved_stages[0].read_bytes()).hexdigest() if args.substitute_parent and moved_stages else None,
        "orphaned_journal_sha256": hashlib.sha256(moved_journal.read_bytes()).hexdigest() if args.substitute_parent else None,
        "interrupted_stage_name": interrupted_stage.name if args.interrupt_extraction else None,
        "interrupted_stage_bytes": interrupted_stage.stat().st_size if args.interrupt_extraction else None,
        "interrupted_journal_sha256": interrupted_journal_sha256 if args.interrupt_extraction else None,
        "process_exit_signal": "SIGKILL" if args.interrupt_extraction else None,
        "retry_after_interruption": retry_result,
        "result": ("selected output path was redirected to another directory; both target sentinels were preserved and the staged file and recovery journal remain in the moved directory" if args.substitute_parent and moved_stages else
                   "selected output path was redirected to another directory; both target sentinels were preserved and no stage remains" if args.substitute_parent else
                  "same-size, same-mtime stage substitution was refused; existing target preserved and substituted stage left untouched" if args.substitute_stage else
                   "Hatari was force-terminated mid-extraction; existing target bytes preserved and a partial stage remains for manual inspection" if args.interrupt_extraction else
                   "Cancel clicked; existing target bytes preserved and stage removed" if args.active_extraction
                    else "Abort clicked; pre-existing OUT directory remains empty"),
    }
    (out / "run.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if args.active_extraction:
        (host / payload_name).unlink()
        (host / "ARCHIVE.BLT").unlink()
    action = ("parent-path substitution" if args.substitute_parent else
              "staged-file substitution" if args.substitute_stage else
              "interruption and retry recovery" if args.interrupt_extraction else
              "extraction cancellation")
    print("Atari GEM " + action + " check passed on Hatari with TOS", tos.name)
    print("Evidence:", out)


if __name__ == "__main__":
    main()
