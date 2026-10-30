#!/usr/bin/env python3
"""Replay a production Atari GEM menu pointer path under Hatari."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parent.parent


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def png_rgb_rows(path):
    """Decode the non-interlaced 8-bit RGB Hatari PNG dump."""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG screenshot")
    width = height = bit_depth = color_type = interlace = None
    image_data = bytearray()
    offset = 8
    while offset + 12 <= len(data):
        length = int.from_bytes(data[offset:offset + 4], "big")
        kind = data[offset + 4:offset + 8]
        body = data[offset + 8:offset + 8 + length]
        offset += length + 12
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", body)
        elif kind == b"IDAT":
            image_data.extend(body)
        elif kind == b"IEND":
            break
    if (bit_depth, color_type, interlace) != (8, 2, 0):
        raise ValueError("expected non-interlaced 8-bit RGB Hatari screenshot")
    raw = zlib.decompress(image_data)
    stride = width * 3
    rows = []
    cursor = 0
    for row_index in range(height):
        filter_type = raw[cursor]
        scan = bytearray(raw[cursor + 1:cursor + 1 + stride])
        cursor += stride + 1
        prior = rows[row_index - 1] if row_index else bytearray(stride)
        for i in range(stride):
            left = scan[i - 3] if i >= 3 else 0
            up = prior[i]
            upper_left = prior[i - 3] if i >= 3 else 0
            if filter_type == 1:
                scan[i] = (scan[i] + left) & 255
            elif filter_type == 2:
                scan[i] = (scan[i] + up) & 255
            elif filter_type == 3:
                scan[i] = (scan[i] + ((left + up) // 2)) & 255
            elif filter_type == 4:
                estimate = left + up - upper_left
                pa, pb, pc = abs(estimate - left), abs(estimate - up), abs(estimate - upper_left)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else upper_left
                scan[i] = (scan[i] + predictor) & 255
            elif filter_type != 0:
                raise ValueError("unsupported PNG row filter")
        rows.append(scan)
    return width, height, rows


def png_nonwhite_count(path, region):
    """Count non-white pixels in a GEM dialog region of the screenshot."""
    width, height, rows = png_rgb_rows(path)
    left, top, right, bottom = region
    if left < 0 or top < 0 or right > width or bottom > height:
        raise ValueError("region outside screenshot")
    return sum(1 for y in range(top, bottom) for x in range(left, right)
               if tuple(rows[y][x * 3:x * 3 + 3]) != (255, 255, 255))


def png_black_count(path, region):
    """Count black pixels, used to verify a checked GEM menu item."""
    width, height, rows = png_rgb_rows(path)
    left, top, right, bottom = region
    if left < 0 or top < 0 or right > width or bottom > height:
        raise ValueError("region outside screenshot")
    return sum(1 for y in range(top, bottom) for x in range(left, right)
               if tuple(rows[y][x * 3:x * 3 + 3]) == (0, 0, 0))


def screenshot_difference(left, right):
    width1, height1, rows1 = png_rgb_rows(left)
    width2, height2, rows2 = png_rgb_rows(right)
    if (width1, height1) != (width2, height2):
        raise ValueError("screenshots have different dimensions")
    return sum(1 for y in range(height1) for x in range(width1)
               if rows1[y][x * 3:x * 3 + 3] != rows2[y][x * 3:x * 3 + 3])


def run(command, cwd, timeout=120):
    return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=timeout,
                          check=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", required=True, type=Path)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", type=Path,
                        default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    parser.add_argument("--gui-prg", type=Path,
                        default=ROOT / "build/atari/boltpack.prg")
    parser.add_argument("--linux-cli", type=Path,
                        default=ROOT / "build/linux/boltpack")
    parser.add_argument("--action", choices=("archive-open", "help-about",
                                              "help-documentation",
                                              "archive-validate",
                                              "archive-information",
                                              "archive-close",
                                              "archive-extract",
                                              "archive-quit",
                                              "compression-stored",
                                              "compression-boltlite",
                                              "compression-boltlz",
                                              "compression-fluff",
                                              "compression-profile"),
                        default="archive-open",
                        help="production menu action to exercise")
    parser.add_argument("--output-root", type=Path,
                        default=ROOT / "build")
    args = parser.parse_args()

    if not os.environ.get("DISPLAY"):
        parser.error("run under xvfb-run or a desktop session so xdotool can inject clicks")
    hatari_binary = shutil.which(args.hatari)
    if shutil.which("xdotool") is None:
        parser.error("required command not found: xdotool")
    if hatari_binary is None:
        parser.error("required command not found: " + str(args.hatari))
    tos = args.tos.expanduser().resolve(strict=True)
    gui = args.gui_prg.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True) if args.action.startswith("archive-") else None
    hconsole = args.hconsole.expanduser().resolve(strict=True)
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="atari-menu-pointer-", dir=output_root))
    host = out / "host"
    host.mkdir()
    shutil.copy2(gui, host / "BOLTPACK.PRG")
    payload = b"Atari archive menu validation fixture\r\n" * 5
    archive_hash = None
    archive_actions = ("archive-validate", "archive-information", "archive-close",
                       "archive-extract", "archive-quit")
    if args.action in archive_actions:
        (host / "PAYLOAD.TXT").write_bytes(payload)
        created = run([str(linux), "c", "ARCHIVE.BLT", "PAYLOAD.TXT"], host)
        (out / "archive-create.log").write_text(created.stdout, encoding="utf-8")
        if created.returncode != 0:
            raise RuntimeError("Linux could not create archive menu fixture: " + str(out))
        archive_hash = sha256(host / "ARCHIVE.BLT")
    script = out / "hconsole.txt"
    if args.action in archive_actions:
        hconsole_commands = (
            "sleep 12\nkeydown 29\nkeypress o\nkeyup 29\n"
            "sleep 3\ntext ARCHIVE.BLT\nkeypress 28\n"
            "sleep 8\nscreenshot\nsleep 8\nscreenshot\n"
            "sleep 8\nscreenshot\nsleep 2\nquit\n")
    else:
        hconsole_commands = "sleep 8\nscreenshot\nsleep 8\nscreenshot\nsleep 2\nquit\n"
    script.write_text(hconsole_commands, encoding="ascii")
    env = os.environ.copy()
    env.pop("SDL_VIDEODRIVER", None)
    env.pop("SDL_AUDIODRIVER", None)
    env["PATH"] = str(Path(hatari_binary).resolve().parent) + os.pathsep + env.get("PATH", "")
    command = [sys.executable, str(hconsole), str(script), "--exit", "--",
               "--machine", "ste", "--tos", str(tos), "--sound", "off",
               "--confirm-quit", "off", "--fast-forward", "on",
               "--fast-boot", "on", "--memsize", "4", "--harddrive",
               str(host), "--auto", r"C:\BOLTPACK.PRG"]
    log_path = out / "hatari.log"
    with log_path.open("w", encoding="utf-8") as log:
        proc = subprocess.Popen(command, cwd=out, env=env, stdout=log,
                                stderr=subprocess.STDOUT)
        try:
            window = None
            deadline = time.monotonic() + 40
            while time.monotonic() < deadline and proc.poll() is None:
                found = run(["xdotool", "search", "--onlyvisible", "--name", "Hatari"],
                            out, timeout=3)
                if found.returncode == 0 and found.stdout.strip():
                    window = found.stdout.splitlines()[0].strip()
                    break
                time.sleep(0.25)
            if window is None:
                raise RuntimeError("visible Hatari window did not appear; evidence: " + str(out))
            if args.action in archive_actions:
                first_capture = out / "grab0001.png"
                capture_deadline = time.monotonic() + 50
                while not first_capture.exists() and time.monotonic() < capture_deadline:
                    if proc.poll() is not None:
                        break
                    time.sleep(0.1)
                if not first_capture.exists():
                    raise RuntimeError("archive did not open before menu input; evidence: " + str(out))
                # Let GEM finish its archive-window redraw/event processing
                # before sending a pointer event into the menu bar.
                time.sleep(1)
            else:
                time.sleep(5)
            geometry = run(["xdotool", "getwindowgeometry", "--shell", window], out)
            (out / "geometry.txt").write_text(geometry.stdout, encoding="ascii")
            title_x, item_x, item_y, action_name, expected_dialog = {
                "archive-open": (330, 335, 38, "Archive > Open",
                                 "Open BoltPack Archive selector"),
                "help-about": (650, 650, 38, "Help > About",
                               "About BoltPack dialog"),
                "help-documentation": (650, 650, 56, "Help > Documentation",
                                       "documentation alert"),
                "archive-validate": (330, 335, 92, "Archive > Validate",
                                     "Validate result alert"),
                "archive-information": (330, 335, 110, "Archive > Information",
                                        "Archive information alert"),
                "archive-extract": (330, 335, 56, "Archive > Extract",
                                    "file selector"),
                "archive-close": (330, 335, 74, "Archive > Close",
                                  "archive window closed"),
                "archive-quit": (330, 335, 146, "Archive > Quit",
                                 "application exited"),
                "compression-stored": (500, 500, 74, "Compression > Stored",
                                       "Stored method check"),
                "compression-boltlite": (500, 500, 92, "Compression > BoltLite",
                                          "BoltLite method check"),
                "compression-boltlz": (500, 500, 110, "Compression > BoltLZ",
                                        "BoltLZ method check"),
                "compression-fluff": (500, 500, 128, "Compression > Fluff",
                                       "Fluff method check"),
                "compression-profile": (500, 500, 146,
                                        "Compression > Interchange Profile 1",
                                        "Interchange Profile 1 check"),
            }[args.action]
            menu_delay = (3 if args.action in archive_actions else 1.5)
            title_click = [
                ["xdotool", "windowfocus", "--sync", window],
                ["xdotool", "mousemove", "--window", window,
                 str(title_x), "6"],
                ["xdotool", "click", "--delay", "250", "1"],
            ]
            for item in title_click:
                result = run(item, out, timeout=10)
                if result.returncode != 0:
                    raise RuntimeError("mouse title click failed: " + result.stdout)
            time.sleep(menu_delay)
            if args.action in archive_actions:
                menu_capture = out / "grab0002.png"
                capture_deadline = time.monotonic() + 20
                while not menu_capture.exists() and time.monotonic() < capture_deadline:
                    if proc.poll() is not None:
                        break
                    time.sleep(0.1)
                if not menu_capture.exists():
                    raise RuntimeError("Archive menu did not remain visible; evidence: " + str(out))
                menu_change = screenshot_difference(first_capture, menu_capture)
                if menu_change < 1000:
                    raise RuntimeError("Archive title click did not open its menu; evidence: " + str(out))
            item_click = [
                ["xdotool", "mousemove", "--window", window,
                 str(item_x), str(item_y)],
                ["xdotool", "click", "--delay", "250", "1"],
            ]
            for item in item_click:
                result = run(item, out, timeout=10)
                if result.returncode != 0:
                    raise RuntimeError("mouse menu-item click failed: " + result.stdout)
            inputs = title_click + item_click
            if args.action.startswith("compression-"):
                time.sleep(1)
                reopen_steps = [["xdotool", "mousemove", "--window", window,
                                 str(title_x), "6"],
                                ["xdotool", "click", "--delay", "250", "1"]]
                for item in reopen_steps:
                    result = run(item, out, timeout=10)
                    if result.returncode != 0:
                        raise RuntimeError("reopen compression menu failed: " + result.stdout)
                inputs.extend(reopen_steps)
            if args.action in archive_actions:
                result_capture = out / "grab0003.png"
                capture_deadline = time.monotonic() + 20
                while not result_capture.exists() and time.monotonic() < capture_deadline:
                    if proc.poll() is not None:
                        break
                    time.sleep(0.1)
                if not result_capture.exists():
                    raise RuntimeError("Archive action result was not captured; evidence: " + str(out))
            try:
                proc.wait(timeout=40)
            except subprocess.TimeoutExpired:
                raise RuntimeError("Hatari did not finish; evidence: " + str(out))
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()

    screenshots = sorted(out.glob("grab*.png"))
    if len(screenshots) < 2:
        raise RuntimeError("Hatari did not retain both screenshots; evidence: " + str(out))
    region = (96, 90, 735, 450)
    nonwhite = [png_nonwhite_count(path, region) for path in screenshots]
    threshold = {"help-documentation": 15000,
                 "archive-validate": 9000,
                 "archive-information": 9000,
                 "archive-extract": 5000,
                 "compression-stored": 9000,
                 "compression-boltlite": 9000,
                 "compression-boltlz": 9000,
                 "compression-fluff": 9000,
                 "compression-profile": 9000}.get(args.action, 20000)
    if args.action not in ("archive-close", "archive-quit") and nonwhite[-1] < threshold:
        raise RuntimeError(action_name + " dialog marker absent from screenshots; evidence: " + str(out))
    pixel_diff = None
    if args.action in archive_actions:
        pixel_diff = screenshot_difference(screenshots[-2], screenshots[-1])
        if pixel_diff < 2000:
            raise RuntimeError(action_name + " produced no visible state change; evidence: " + str(out))
    check_region = None
    check_pixels = None
    if args.action.startswith("compression-"):
        check_top = item_y + (52 if args.action == "compression-profile" else 49)
        check_bottom = item_y + (64 if args.action == "compression-profile" else 59)
        check_region = (403, check_top, 415, check_bottom)
        check_pixels = png_black_count(screenshots[-1], check_region)
        if check_pixels < 20:
            raise RuntimeError(action_name + " checkmark missing from the reopened menu; evidence: " + str(out))
    status = subprocess.check_output(["git", "status", "--short"], cwd=ROOT,
                                     text=True).splitlines()
    diff = subprocess.check_output(["git", "diff", "--binary"], cwd=ROOT)
    record = {
        "purpose": "Replay Atari GEM " + action_name + " pointer activation",
        "result": "PASS: production " + expected_dialog + " followed pointer activation.",
        "environment": {
            "hatari": next((line.strip() for line in (out / "hatari.log").read_text(
                encoding="utf-8").splitlines() if line.startswith("INFO : Hatari v")), "version in hatari.log"),
            "tos": tos.name,
            "tos_sha256": sha256(tos),
            "gui_sha256": sha256(gui),
            "resolution": "832x588 Hatari window; STE, 4 MiB",
        },
        "action": args.action,
        "archive_sha256": archive_hash,
        "input": (["create ARCHIVE.BLT with Linux CLI",
                   "open ARCHIVE.BLT in Atari GUI and wait for archive window"]
                  if args.action in ("archive-validate", "archive-information") else []) +
                 ["focus Hatari", "click menu title window-relative (%d,6)" % title_x,
                  "wait %s seconds" % menu_delay,
                  "click menu item window-relative (%d,%d)" % (item_x, item_y)] +
                 (["reopen Compression to verify selected-item checkmark"]
                  if args.action.startswith("compression-") else []),
        "verification": {
            "dialog_screenshot": screenshots[-1].name,
            "dialog_region_nonwhite_pixels": {
                "region": list(region), "by_screenshot": nonwhite,
                "threshold": threshold,
            },
        "pixels_changed_from_pre_action_screenshot": pixel_diff,
            "checked_item": ({"region": list(check_region),
                              "black_pixels": check_pixels,
                              "minimum": 20}
                             if check_region is not None else None),
        },
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"],
                                            cwd=ROOT, text=True).strip(),
        "worktree_status": status,
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "artifacts": {path.name: {"sha256": sha256(path), "size": path.stat().st_size}
                      for path in [script, log_path, *screenshots, out / "geometry.txt"]},
    }
    if archive_hash is not None:
        record["fixture"] = {
            "archive": "host/ARCHIVE.BLT",
            "payload_sha256": hashlib.sha256(payload).hexdigest(),
            "linux_cli_sha256": sha256(linux),
        }
        for artifact in (out / "archive-create.log", host / "ARCHIVE.BLT"):
            record["artifacts"][str(artifact.relative_to(out))] = {
                "sha256": sha256(artifact), "size": artifact.stat().st_size,
            }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print("Atari " + action_name + " pointer replay passed")
    print("Evidence: " + str(out / "run.json"))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.TimeoutExpired, subprocess.CalledProcessError,
            RuntimeError, ValueError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        sys.exit(1)
