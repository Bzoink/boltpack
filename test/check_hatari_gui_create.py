#!/usr/bin/env python3
"""Run the shipped Atari GEM create flow and cross-check its v6 archive."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = json.loads((ROOT / "test/reference/interoperability/atari-gui-create.json")
                     .read_text(encoding="utf-8"))


def run(command, cwd, env=None, timeout=180):
    return subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=timeout)


def mdir_self_time(output):
    for line in output.splitlines():
        fields = line.split()
        if fields and fields[0] == "." and len(fields) >= 2 and fields[1] == "<DIR>":
            return " ".join(fields[-2:])
    return None


def build_prefix_archive(output):
    helper = output / "make_mini_path_conflict"
    sources = [
        ROOT / "test/make_mini_path_conflict.c",
        ROOT / "src/core/mini/archiver.c",
        ROOT / "src/core/mini/compress.c",
        ROOT / "src/core/mini/boltlz_decomp.c",
        ROOT / "src/core/boltlite.c",
        ROOT / "src/core/boltlz.c",
        ROOT / "src/core/bp_codepage.c",
        ROOT / "src/frontend/mini_shared/mini_common.c",
    ]
    command = shlex.split(os.environ.get("CC", "cc")) + [
        "-ansi", "-pedantic", "-Wall", "-Wextra", "-Wno-unused-function",
        "-DBP_MINI", "-DBP_CREATOR_OS=BP_OS_LINUX",
        "-I", str(ROOT / "src/core"),
        "-I", str(ROOT / "src/core/mini"),
        "-I", str(ROOT / "src/frontend/mini_shared"),
        *map(str, sources), "-o", str(helper),
    ]
    subprocess.run(command, check=True, cwd=ROOT, capture_output=True)
    return helper


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", type=Path, required=True)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", type=Path,
                        default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    parser.add_argument("--gui-prg", type=Path,
                        default=ROOT / "build/atari/boltpack.prg")
    parser.add_argument("--linux-cli", type=Path,
                        default=ROOT / "build/linux/boltpack")
    parser.add_argument("--mini-cli", type=Path,
                        default=ROOT / "build/linux32mini/bpmini")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build")
    parser.add_argument("--fat12", action="store_true",
                        help="run the guest on a disposable 720 KiB FAT12 floppy")
    args = parser.parse_args()

    tos = args.tos.expanduser().resolve(strict=True)
    gui = args.gui_prg.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    mini = args.mini_cli.resolve(strict=True)
    hconsole = args.hconsole.expanduser().resolve(strict=True)
    output_root = args.output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="atari-gui-create-", dir=output_root))
    host = out / "host"
    host.mkdir()
    expected = MANIFEST["source"]
    collision = MANIFEST["collision_archive"]
    prefix_collision = MANIFEST["prefix_collision_archive"]
    payloads = {}
    for item in expected["files"]:
        data = item["content"].encode("utf-8") * item["repetitions"]
        assert len(data) == item["bytes"]
        assert hashlib.sha256(data).hexdigest() == item["sha256"]
        payloads[item["name"]] = data
    for directory in expected["directories"]:
        (host / directory).mkdir(parents=True, exist_ok=True)
    replacement_seed = {}
    for item in expected["files"]:
        stale = ("stale pre-existing target: " + item["name"] + "\n").encode("ascii")
        replacement_seed[item["name"]] = stale
    sources = {}
    for item in expected["files"]:
        source = host / item["name"]
        source.write_bytes(payloads[item["name"]])
        os.utime(source, (951786000, 951786000))
        sources[item["name"]] = source
    shutil.copy2(gui, host / "BOLTPACK.PRG")
    (host / "OUT").mkdir()
    archive_name = MANIFEST["expected"]["archive"]
    overwrite_seed = b"pre-existing archive sentinel must be replaced atomically\n"
    (host / archive_name).write_bytes(overwrite_seed)
    for directory in expected["directories"]:
        (host / "OUT" / directory).mkdir(parents=True, exist_ok=True)
    for name, stale in replacement_seed.items():
        (host / "OUT" / name).write_bytes(stale)
    (host / "COLLIDE").mkdir()
    (host / "PREFIX").mkdir()
    collision_source = out / "collision-source"
    collision_source.mkdir()
    for item in collision["entries"]:
        (collision_source / item["name"]).write_text(item["content"], encoding="ascii")
    collision_archive = out / collision["name"]
    collision_create = run([str(linux), "c", str(collision_archive)] +
                           [item["name"] for item in collision["entries"]],
                           collision_source)
    (out / "collision-create.log").write_text(collision_create.stdout, encoding="utf-8")
    if collision_create.returncode != 0:
        raise RuntimeError("Linux could not create case-collision fixture; evidence: " + str(out))
    collision_test = run([str(linux), "t", str(collision_archive)], out)
    (out / "collision-integrity.log").write_text(collision_test.stdout, encoding="utf-8")
    if collision_test.returncode != 0:
        raise RuntimeError("Linux rejected case-collision fixture; evidence: " + str(out))
    prefix_archive = out / prefix_collision["name"]
    prefix_helper = build_prefix_archive(out)
    prefix_create = run([str(prefix_helper), str(prefix_archive)], out)
    (out / "prefix-collision-create.log").write_text(prefix_create.stdout, encoding="utf-8")
    if prefix_create.returncode != 0:
        raise RuntimeError("could not create file/directory prefix archive; evidence: " + str(out))
    prefix_test = run([str(linux), "t", str(prefix_archive)], out)
    (out / "prefix-collision-integrity.log").write_text(prefix_test.stdout, encoding="utf-8")
    if prefix_test.returncode != 0:
        raise RuntimeError("Linux rejected file/directory prefix fixture; evidence: " + str(out))
    floppy = out / "floppy.st"
    if args.fat12:
        for utility in ("mformat", "mcopy", "mmd", "mattrib"):
            if shutil.which(utility) is None:
                raise RuntimeError("FAT12 mode requires mtools utility " + utility)
        formatted = run(["mformat", "-i", str(floppy), "-C", "-f", "720", "::"], out)
        (out / "mformat.log").write_text(formatted.stdout, encoding="utf-8")
        if formatted.returncode != 0:
            raise RuntimeError("could not format disposable FAT12 image; evidence: " + str(out))
        for directory in expected["directories"]:
            made_tree_dir = run(["mmd", "-i", str(floppy), "::" + directory], out)
            if made_tree_dir.returncode != 0:
                raise RuntimeError("could not create FAT12 source tree; evidence: " + str(out))
        overwrite_seed_path = out / "existing-archive-seed.blt"
        overwrite_seed_path.write_bytes(overwrite_seed)
        for host_file, guest_name in [(host / "BOLTPACK.PRG", "BOLTPACK.PRG"),
                                      (overwrite_seed_path, archive_name)] + [
                (sources[item["name"]], item["name"]) for item in expected["files"]]:
            copied = run(["mcopy", "-i", str(floppy), str(host_file), "::" + guest_name], out)
            if copied.returncode != 0:
                raise RuntimeError("could not seed FAT12 image; evidence: " + str(out))
        readonly = run(["mattrib", "-i", str(floppy), "+r", "+h", "+s",
                        "::" + expected["files"][0]["name"]], out)
        if readonly.returncode != 0:
            raise RuntimeError("could not mark FAT12 source read-only; evidence: " + str(out))
        directory_attrs = run(["mattrib", "-i", str(floppy), "+h", "+s",
                                "::TREE/empty"], out)
        if directory_attrs.returncode != 0:
            raise RuntimeError("could not mark FAT12 source directory hidden/system; evidence: " + str(out))
        made_dir = run(["mmd", "-i", str(floppy), "::OUT"], out)
        if made_dir.returncode != 0:
            raise RuntimeError("could not create FAT12 extraction directory; evidence: " + str(out))
        for directory in expected["directories"]:
            made_output_tree_dir = run(["mmd", "-i", str(floppy), "::OUT/" + directory], out)
            if made_output_tree_dir.returncode != 0:
                raise RuntimeError("could not pre-create FAT12 replacement tree; evidence: " + str(out))
        stale_sources = {}
        for index, (name, stale) in enumerate(replacement_seed.items()):
            stale_source = out / ("stale-target-" + str(index))
            stale_source.write_bytes(stale)
            stale_sources[name] = stale_source
            copied_stale = run(["mcopy", "-i", str(floppy), str(stale_source), "::OUT/" + name], out)
            if copied_stale.returncode != 0:
                raise RuntimeError("could not seed FAT12 replacement target " + name + "; evidence: " + str(out))
        made_collision_dir = run(["mmd", "-i", str(floppy), "::COLLIDE"], out)
        if made_collision_dir.returncode != 0:
            raise RuntimeError("could not create FAT12 collision directory; evidence: " + str(out))
        made_prefix_dir = run(["mmd", "-i", str(floppy), "::PREFIX"], out)
        if made_prefix_dir.returncode != 0:
            raise RuntimeError("could not create FAT12 prefix directory; evidence: " + str(out))
        copied_collision = run(["mcopy", "-i", str(floppy), str(collision_archive),
                                "::" + collision["name"]], out)
        if copied_collision.returncode != 0:
            raise RuntimeError("could not seed FAT12 collision archive; evidence: " + str(out))
        copied_prefix = run(["mcopy", "-i", str(floppy), str(prefix_archive),
                             "::" + prefix_collision["name"]], out)
        if copied_prefix.returncode != 0:
            raise RuntimeError("could not seed FAT12 prefix archive; evidence: " + str(out))
    else:
        shutil.copy2(collision_archive, host / collision["name"])
        shutil.copy2(prefix_archive, host / prefix_collision["name"])

    script = out / "HCONSOLE.TXT"
    script.write_text("\n".join((
        "sleep 20",
        "keydown 29",
        "keypress n",
        "keyup 29",
        "sleep 4",
        "text " + expected["select"],
        "keypress 28",
        "sleep 3",
        "keypress 28",  # Create is the alert's default action.
        "sleep 4",
        "keypress 28",  # Accept the default ARCHIVE.BLT save name.
        "sleep 15",
        "keypress 28",  # Dismiss the expected unsupported directory-attribute alert.
        "sleep 1",
        "keydown 29",
        "keypress e",
        "keyup 29",
        "sleep 3",
        "text OUT",
        "keypress 28",  # Select the precreated OUT destination directory.
        "sleep 20",  # Let slower TOS versions finish disk writes before the next key.
        "screenshot",  # Capture the expected partial directory-metadata alert.
        "keypress 28",  # Dismiss partial Atari metadata alert, if shown.
        "sleep 2",
        "screenshot",
        "sleep 1",
        "keydown 29",
        "keypress o",
        "keyup 29",
        "sleep 3",
        "text " + collision["name"],
        "keypress 28",
        "sleep 4",
        "keydown 29",
        "keypress e",
        "keyup 29",
        "sleep 3",
        "text COLLIDE",
        "keypress 28",
        "sleep 4",
        "screenshot",
        "sleep 1",
        "keypress 28",  # Dismiss the projected-name collision alert.
        "sleep 1",
        "quit",
        "",
    )), encoding="ascii")

    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    hatari_command = [
        sys.executable, str(hconsole), str(script), "--exit", "--",
        "--machine", "ste", "--tos", str(tos), "--sound", "off",
        "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
        "--memsize", "4",
    ]
    if args.fat12:
        hatari_command.extend(("--disk-a", str(floppy), "--protect-floppy", "off",
                               "--auto", r"A:\BOLTPACK.PRG"))
    else:
        hatari_command.extend(("--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG"))
    guest = run(hatari_command, out, env=env, timeout=150)
    (out / "hatari.log").write_text(guest.stdout, encoding="utf-8")
    if guest.returncode != 0:
        raise RuntimeError("Hatari GUI run failed; evidence: " + str(out))

    prefix_evidence = out / "prefix-run"
    prefix_evidence.mkdir()
    prefix_script = prefix_evidence / "HCONSOLE.TXT"
    prefix_script.write_text("\n".join((
        "sleep 20",
        "keydown 29",
        "keypress o",
        "keyup 29",
        "sleep 3",
        "text " + prefix_collision["name"],
        "keypress 28",
        "sleep 4",
        "keydown 29",
        "keypress e",
        "keyup 29",
        "sleep 3",
        "text PREFIX",
        "keypress 28",
        "sleep 4",
        "screenshot",
        "sleep 1",
        "keypress 28",  # Dismiss the file/directory-prefix refusal alert.
        "sleep 1",
        "quit",
        "",
    )), encoding="ascii")
    prefix_command = [
        sys.executable, str(hconsole), str(prefix_script), "--exit", "--",
        "--machine", "ste", "--tos", str(tos), "--sound", "off",
        "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
        "--memsize", "4",
    ]
    if args.fat12:
        prefix_command.extend(("--disk-a", str(floppy), "--protect-floppy", "off",
                               "--auto", r"A:\BOLTPACK.PRG"))
    else:
        prefix_command.extend(("--harddrive", str(host), "--auto", r"C:\BOLTPACK.PRG"))
    prefix_guest = run(prefix_command, prefix_evidence, env=env)
    (prefix_evidence / "hatari.log").write_text(prefix_guest.stdout, encoding="utf-8")
    if prefix_guest.returncode != 0:
        raise RuntimeError("Hatari prefix-collision flow failed; evidence: " + str(out))
    if not (prefix_evidence / "grab0001.png").is_file():
        raise RuntimeError("prefix-collision screenshot is missing; evidence: " + str(out))

    if args.fat12:
        copied = run(["mcopy", "-i", str(floppy), "::" + archive_name,
                      str(out / archive_name)], out)
        (out / "mcopy-archive.log").write_text(copied.stdout, encoding="utf-8")
        if copied.returncode != 0:
            raise RuntimeError("GUI did not publish archive on FAT12 floppy; evidence: " + str(out))
        archive = out / archive_name
    else:
        archive = host / archive_name
    if not archive.is_file():
        raise RuntimeError("production GUI did not create " + archive_name +
                           "; evidence: " + str(out))
    if archive.read_bytes() == overwrite_seed:
        raise RuntimeError("production GUI did not replace the pre-existing archive; evidence: " + str(out))
    if args.fat12:
        root_listing = run(["mdir", "-b", "-i", str(floppy), "::"], out)
        if root_listing.returncode != 0:
            raise RuntimeError("could not inspect FAT12 root after archive replacement; evidence: " + str(out))
        root_names = sorted(line.strip().rsplit("/", 1)[-1].upper()
                            for line in root_listing.stdout.splitlines() if line.strip())
        leftover_archive_stages = [name for name in root_names
                                   if name.startswith(("BP", "BR"))]
    else:
        leftover_archive_stages = sorted(
            path.name for path in host.iterdir()
            if path.name.upper().startswith(("BP", "BR")))
    if leftover_archive_stages:
        raise RuntimeError("archive replacement left stage/backup names: " +
                           repr(leftover_archive_stages) + "; evidence: " + str(out))

    if args.fat12:
        gui_extracted = {}
        fat12_dir_times = {}
        copy_logs = []
        for item in expected["files"]:
            target = out / ("gui-extracted-" + Path(item["name"]).name)
            copied = run(["mcopy", "-i", str(floppy), "::OUT/" + item["name"],
                          str(target)], out)
            copy_logs.append(copied.stdout)
            if copied.returncode != 0:
                raise RuntimeError("GUI extraction did not publish " + item["name"] +
                                   " into FAT12 OUT; evidence: " + str(out))
            gui_extracted[item["name"]] = target
        (out / "mcopy-extracted.log").write_text("\n".join(copy_logs), encoding="utf-8")
        source_meta_name = expected["files"][0]["name"]
        source_attrs = run(["mattrib", "-i", str(floppy), "::" + source_meta_name], out)
        extracted_attrs = run(["mattrib", "-i", str(floppy), "::OUT/" + source_meta_name], out)
        (out / "fat12-attributes.log").write_text(
            "SOURCE\n" + source_attrs.stdout + "EXTRACTED\n" + extracted_attrs.stdout,
            encoding="utf-8")
        if source_attrs.returncode != 0 or extracted_attrs.returncode != 0:
            raise RuntimeError("could not read FAT12 source/extracted attributes; evidence: " + str(out))
        attr_letters_by_name = {"archive": "A", "read-only": "R",
                                "hidden": "H", "system": "S"}
        expected_flags = MANIFEST["expected"]["fat12_source_attributes"]
        expected_restored_flags = MANIFEST["expected"]["fat12_gui_extracted_attributes"]
        for label, result, expected_names in (
                ("source", source_attrs, expected_flags),
                ("GUI-extracted", extracted_attrs, expected_restored_flags)):
            line = result.stdout.splitlines()[0] if result.stdout.splitlines() else ""
            attr_fields = line.split("::", 1)[0].split()
            attr_letters = set("".join(attr_fields))
            wanted = {attr_letters_by_name[name] for name in expected_names}
            if not wanted.issubset(attr_letters):
                raise RuntimeError(label + " FAT12 attributes differ from manifest: " +
                                   result.stdout + "; evidence: " + str(out))
        source_dir_attrs = run(["mattrib", "-i", str(floppy), "::TREE/empty"], out)
        extracted_dir_attrs = run(["mattrib", "-i", str(floppy), "::OUT/TREE/empty"], out)
        expected_dir_flags = MANIFEST["expected"]["fat12_source_directory_attributes"]
        expected_dir_restored_flags = MANIFEST["expected"]["fat12_gui_extracted_directory_attributes"]
        for label, result, expected_names in (
                ("source directory", source_dir_attrs, expected_dir_flags),
                ("GUI-extracted directory", extracted_dir_attrs, expected_dir_restored_flags)):
            fields = result.stdout.splitlines()[0].split("::", 1)[0].split() if result.stdout.splitlines() else []
            attr_letters = set("".join(fields))
            wanted = {attr_letters_by_name[name] for name in expected_names}
            if result.returncode != 0 or not wanted.issubset(attr_letters):
                raise RuntimeError(label + " FAT12 attributes differ from manifest: " +
                                   result.stdout + "; evidence: " + str(out))
            if label == "GUI-extracted directory":
                source_dir_flags = {attr_letters_by_name[name]
                                    for name in expected_dir_flags}
                if attr_letters & source_dir_flags:
                    raise RuntimeError("TOS unexpectedly restored source directory flags; "
                                       "update the manifest and qualification: " + str(out))
        collision_listing = run(["mdir", "-a", "-i", str(floppy), "::COLLIDE"], out)
        (out / "fat12-collision-directory.log").write_text(
            collision_listing.stdout, encoding="utf-8")
        if collision_listing.returncode != 0 or "Alpha" in collision_listing.stdout:
            raise RuntimeError("GUI wrote case-colliding entries to FAT12; evidence: " + str(out))
        prefix_listing = run(["mdir", "-a", "-i", str(floppy), "::PREFIX"], out)
        (out / "fat12-prefix-directory.log").write_text(
            prefix_listing.stdout, encoding="utf-8")
        if prefix_listing.returncode != 0 or "NODE" in prefix_listing.stdout:
            raise RuntimeError("GUI wrote file/directory-prefix entries to FAT12; evidence: " + str(out))
    else:
        gui_extracted = {item["name"]: host / "OUT" / item["name"]
                         for item in expected["files"]}
        if any((host / "COLLIDE").iterdir()):
            raise RuntimeError("GUI wrote case-colliding entries to host drive; evidence: " + str(out))
        if any((host / "PREFIX").iterdir()):
            raise RuntimeError("GUI wrote file/directory-prefix entries to host drive; evidence: " + str(out))
    for item in expected["files"]:
        extracted_file = gui_extracted[item["name"]]
        if not extracted_file.is_file() or extracted_file.read_bytes() != payloads[item["name"]]:
            raise RuntimeError("Atari GUI extraction payload mismatch for " + item["name"] +
                               "; evidence: " + str(out))
    if args.fat12:
        for directory in expected["directories"]:
            listing = run(["mdir", "-a", "-i", str(floppy), "::OUT/" + directory], out)
            if listing.returncode != 0 or any(
                    token in listing.stdout.upper() for token in ("BP", "BR")):
                raise RuntimeError("FAT12 extraction left a staging/recovery name in " +
                                   directory + "; evidence: " + str(out))
    else:
        if any(path.name.startswith(("BP", "BR"))
               for path in (host / "OUT").rglob("*")):
            raise RuntimeError("Atari extraction left a staging/recovery name; evidence: " + str(out))
    for directory in expected["directories"]:
        relative = Path(directory)
        if args.fat12:
            source_listing = run(["mdir", "-a", "-i", str(floppy), "::" + directory], out)
            extracted_listing = run(["mdir", "-a", "-i", str(floppy),
                                     "::OUT/" + directory], out)
            if source_listing.returncode != 0 or extracted_listing.returncode != 0:
                raise RuntimeError("Atari GUI did not extract directory " + directory +
                                   "; evidence: " + str(out))
            source_time = mdir_self_time(source_listing.stdout)
            extracted_time = mdir_self_time(extracted_listing.stdout)
            if source_time is None or extracted_time is None:
                raise RuntimeError("could not read FAT12 directory timestamps for " + directory +
                                   "; evidence: " + str(out))
            fat12_dir_times[directory] = {
                "source": source_time,
                "gui_extracted": extracted_time,
            }
        elif not (host / "OUT" / relative).is_dir():
            raise RuntimeError("Atari GUI did not extract directory " + directory +
                               "; evidence: " + str(out))
    if args.fat12:
        (out / "fat12-directory-times.json").write_text(
            json.dumps(fat12_dir_times, indent=2) + "\n", encoding="utf-8")

    integrity = run([str(linux), "t", str(archive)], out)
    (out / "linux-integrity.log").write_text(integrity.stdout, encoding="utf-8")
    if integrity.returncode != 0:
        raise RuntimeError("Linux rejected GUI-created archive; evidence: " + str(out))

    info = run([str(linux), "i", str(archive)], out)
    (out / "linux-info.log").write_text(info.stdout, encoding="utf-8")
    for required in ("Format version: 6", "Creator OS:     Atari ST",
                     "Creator CPU:    Motorola 68k"):
        if required not in info.stdout:
            raise RuntimeError("unexpected creator/format info; evidence: " + str(out))
    listing = run([str(linux), "l", str(archive)], out)
    (out / "linux-list.log").write_text(listing.stdout, encoding="utf-8")
    if listing.returncode != 0:
        raise RuntimeError("Linux could not list GUI-created archive; evidence: " + str(out))
    for item in expected["files"]:
        if item["name"] not in listing.stdout:
            raise RuntimeError("GUI archive is missing file " + item["name"] +
                               "; evidence: " + str(out))
    for directory in expected["directories"]:
        if directory.rstrip("/") + "/" not in listing.stdout:
            raise RuntimeError("GUI archive is missing directory " + directory +
                               "; evidence: " + str(out))

    extraction = out / "linux-extract"
    extraction.mkdir()
    extracted = run([str(linux), "x", "-o", str(extraction), str(archive)], out)
    (out / "linux-extract.log").write_text(extracted.stdout, encoding="utf-8")
    if extracted.returncode != 1 or "some entries or attributes were not restored" not in extracted.stdout:
        raise RuntimeError("expected metadata partial result was not reported; evidence: " + str(out))
    for item in expected["files"]:
        payload = extraction / item["name"]
        if not payload.is_file() or payload.read_bytes() != payloads[item["name"]]:
            raise RuntimeError("Linux extraction payload mismatch for " + item["name"] +
                               "; evidence: " + str(out))
    for directory in expected["directories"]:
        if not (extraction / directory).is_dir():
            raise RuntimeError("Linux extraction missed directory " + directory +
                               "; evidence: " + str(out))

    mini_test = run([str(mini), "t", str(archive)], out)
    (out / "mini-integrity.log").write_text(mini_test.stdout, encoding="utf-8")
    if mini_test.returncode != 0:
        raise RuntimeError("Linux mini rejected GUI-created archive; evidence: " + str(out))
    mini_extract = out / "mini-extract"
    mini_extract.mkdir()
    mini_result = run([str(mini), "x", str(archive), str(mini_extract)], out)
    (out / "mini-extract.log").write_text(mini_result.stdout, encoding="utf-8")
    if mini_result.returncode != 1 or "metadata not restored" not in mini_result.stdout:
        raise RuntimeError("expected mini metadata partial result was not reported; evidence: " + str(out))
    for item in expected["files"]:
        mini_payload = mini_extract / item["name"]
        if not mini_payload.is_file() or mini_payload.read_bytes() != payloads[item["name"]]:
            raise RuntimeError("Linux mini extraction payload mismatch for " + item["name"] +
                               "; evidence: " + str(out))
    for directory in expected["directories"]:
        if not (mini_extract / directory).is_dir():
            raise RuntimeError("Linux mini extraction missed directory " + directory +
                               "; evidence: " + str(out))

    readonly_replacement = None
    if args.fat12:
        # GEMDOS refuses to rename a read-only destination. Exercise that
        # publication failure after the stage has been completely decoded.
        failure_dir = out / "failed-replacement"
        failure_dir.mkdir()
        failure_case = MANIFEST["expected"]["fat12_readonly_replacement"]
        failure_target = failure_case["target"]
        failure_sentinel = failure_case["sentinel"].encode("ascii")
        failure_source = failure_dir / "sentinel.txt"
        failure_source.write_bytes(failure_sentinel)
        for command in (
                ["mattrib", "-i", str(floppy), "-r", "::" + failure_target],
                ["mdel", "-i", str(floppy), "::" + failure_target],
                ["mcopy", "-i", str(floppy), str(failure_source),
                 "::" + failure_target],
                ["mattrib", "-i", str(floppy), "+r", "::" + failure_target]):
            result = run(command, out)
            if result.returncode != 0:
                raise RuntimeError("could not seed read-only replacement target: " +
                                   result.stdout + "; evidence: " + str(out))
        failure_script = failure_dir / "HCONSOLE.TXT"
        failure_script.write_text("\n".join((
            "sleep 20",
            "keydown 29", "keypress o", "keyup 29",
            "sleep 3", "text " + MANIFEST["expected"]["archive"], "keypress 28",
            "sleep 4",
            "keydown 29", "keypress e", "keyup 29",
            "sleep 3", "text OUT", "keypress 28",
            "sleep 20", "screenshot",
            "keypress 28", "sleep 1", "quit", "",
        )), encoding="ascii")
        failure_command = [
            sys.executable, str(hconsole), str(failure_script), "--exit", "--",
            "--machine", "ste", "--tos", str(tos), "--sound", "off",
            "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
            "--memsize", "4", "--disk-a", str(floppy), "--protect-floppy", "off",
            "--auto", r"A:\BOLTPACK.PRG",
        ]
        failed_guest = run(failure_command, failure_dir, env=env, timeout=180)
        (failure_dir / "hatari.log").write_text(failed_guest.stdout, encoding="utf-8")
        if failed_guest.returncode != 0:
            raise RuntimeError("Hatari failed during read-only replacement case; evidence: " +
                               str(failure_dir))
        recovered_target = failure_dir / "recovered-target.txt"
        recovered = run(["mcopy", "-i", str(floppy), "::" + failure_target,
                         str(recovered_target)], out)
        (failure_dir / "readback.log").write_text(recovered.stdout, encoding="utf-8")
        if recovered.returncode != 0 or recovered_target.read_bytes() != failure_sentinel:
            raise RuntimeError("failed replacement changed the existing destination; evidence: " +
                               str(failure_dir))
        attr_result = run(["mattrib", "-i", str(floppy), "::" + failure_target], out)
        listing_result = run(["mdir", "-a", "-i", str(floppy), "::OUT/TREE"], out)
        (failure_dir / "attributes-and-stages.log").write_text(
            "ATTRIBUTES\n" + attr_result.stdout + "STAGES\n" + listing_result.stdout,
            encoding="utf-8")
        listing_names = [line.split()[0].upper()
                         for line in listing_result.stdout.splitlines()
                         if line.split()]
        if (attr_result.returncode != 0 or "R" not in attr_result.stdout.splitlines()[0] or
                listing_result.returncode != 0 or
                any(name.startswith(("BP", "BR")) for name in listing_names)):
            raise RuntimeError("failed replacement lost READONLY or left a stage/recovery name; "
                               "evidence: " + str(failure_dir))
        readonly_replacement = {
            "target": failure_target,
            "sentinel_sha256": hashlib.sha256(failure_sentinel).hexdigest(),
            "read_only_preserved": True,
            "stage_and_recovery_names_removed": True,
            "screenshot": "failed-replacement/grab0001.png",
            "hatari_command": failure_command,
        }

    if not all((out / name).is_file() for name in
               ("grab0001.png", "grab0002.png", "grab0003.png")):
        raise RuntimeError("Hatari screenshots are missing; evidence: " + str(out))
    try:
        revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                  check=True, capture_output=True, text=True).stdout.strip()
        dirty = bool(subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
                                    check=True, capture_output=True, text=True).stdout.strip())
    except subprocess.CalledProcessError:
        revision, dirty = "unknown", True
    run_record = {
        "case": MANIFEST["case"],
        "expected_manifest": "test/reference/interoperability/atari-gui-create.json",
        "expected_manifest_sha256": hashlib.sha256(
            (ROOT / "test/reference/interoperability/atari-gui-create.json").read_bytes()
        ).hexdigest(),
        "tos_rom": tos.name,
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "tested_revision": revision,
        "worktree_dirty": dirty,
        "hatari_version": next((line for line in guest.stdout.splitlines()
                                 if "Hatari v" in line), "unknown"),
        "tos_image_sha256": hashlib.sha256(tos.read_bytes()).hexdigest(),
        "gui_program_sha256": hashlib.sha256(gui.read_bytes()).hexdigest(),
        "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "collision_archive_sha256": hashlib.sha256(collision_archive.read_bytes()).hexdigest(),
        "prefix_collision_archive_sha256": hashlib.sha256(prefix_archive.read_bytes()).hexdigest(),
        "collision_entries_refused": [item["name"] for item in collision["entries"]],
        "collision_output_empty": True,
        "prefix_collision_entries_expected": prefix_collision["entries"],
        "prefix_collision_output_empty": True,
        "prefix_collision_alert_screenshot": "prefix-run/grab0001.png",
        "payload_sha256": {name: hashlib.sha256((extraction / name).read_bytes()).hexdigest()
                           for name in payloads},
        "preexisting_destination_files_replaced": list(replacement_seed),
        "filesystem": "720 KiB FAT12 floppy image" if args.fat12 else "Hatari GEMDOS host directory",
        "fat12_image_sha256": hashlib.sha256(floppy.read_bytes()).hexdigest()
            if args.fat12 else None,
        "filesystem_setup": [
            "mformat -i floppy.st -C -f 720 ::",
            "mmd source TREE directories, OUT, COLLIDE, and PREFIX; mcopy GUI and pinned files; "
            "set TREE/src.txt read-only/hidden/system and TREE/empty hidden/system; "
            "copy case-fold and file/directory-prefix archives",
        ] if args.fat12 else [],
        "fat12_source_attributes": source_attrs.stdout.strip() if args.fat12 else None,
        "fat12_gui_extracted_attributes": extracted_attrs.stdout.strip() if args.fat12 else None,
        "fat12_source_directory_attributes": source_dir_attrs.stdout.strip() if args.fat12 else None,
        "fat12_gui_extracted_directory_attributes": extracted_dir_attrs.stdout.strip() if args.fat12 else None,
        "fat12_directory_attribute_outcome": MANIFEST["expected"]["fat12_directory_attribute_outcome"] if args.fat12 else None,
        "fat12_directory_timestamps": fat12_dir_times if args.fat12 else None,
        "existing_archive_replaced": archive.read_bytes() != overwrite_seed,
        "existing_archive_seed_sha256": hashlib.sha256(overwrite_seed).hexdigest(),
        "published_archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "archive_replacement_leftover_stage_or_backup_names": leftover_archive_stages,
        "fat12_failed_readonly_replacement": readonly_replacement,
        "hatari_command": hatari_command,
        "prefix_hatari_command": prefix_command,
        "gui_extraction_sha256": {
            name: hashlib.sha256(gui_extracted[name].read_bytes()).hexdigest()
            for name in payloads
        },
        "result": "GUI extraction recovered exact bytes; " +
            ("all four FAT file attributes restored; directory Hidden/System flags "
             "remain unavailable on TOS; directory metadata reports partial; "
             if args.fat12 else "") +
            "case-fold and file/directory-prefix GUI collision alerts were captured with empty output directories; v6 full and mini integrity pass; both extract exact bytes and report metadata partial" +
            ("; read-only FAT12 replacement failure preserved original bytes/READONLY "
             "and removed the stage" if readonly_replacement else ""),
    }
    (out / "run.json").write_text(json.dumps(run_record, indent=2) + "\n",
                                   encoding="utf-8")

    print("Atari GEM production Create flow passed on Hatari with TOS ROM " + tos.name)
    print("Filesystem: " + ("720 KiB FAT12 floppy" if args.fat12 else "GEMDOS host directory"))
    print("Format 6 creator identity and full/mini integrity verified")
    print("Atari GUI, Linux full, and Linux mini recover all pinned files and directories")
    print("Linux full and mini report expected partial Atari metadata status")
    print("Atari prefix-collision alert screenshot: prefix-run/grab0001.png")
    print("Evidence: " + str(out))


if __name__ == "__main__":
    main()
