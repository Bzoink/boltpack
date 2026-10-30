#!/usr/bin/env python3
"""Exercise Atari/Linux archive transfers on a disposable FAT12 floppy."""
import argparse
import datetime
import hashlib
import json
import locale
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
EXPECTED_PATH = ROOT / "test/reference/interoperability/atari-amiga-fsuae.json"
ATARI_AMIGA = json.loads(EXPECTED_PATH.read_text(encoding="utf-8"))


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command_version(command):
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True,
                                timeout=10, check=False)
    except (OSError, subprocess.SubprocessError) as exc:
        return "unavailable: " + str(exc)
    return result.stdout.strip().splitlines()[0] if result.stdout.strip() else \
        "unknown (exit %d)" % result.returncode


def artifact_index(root):
    return {
        path.relative_to(root).as_posix(): {
            "size": path.stat().st_size,
            "sha256": sha256(path),
        }
        for path in sorted(root.rglob("*"))
        if path.is_file() and path != root / "run.json"
    }


def manifest_bytes(archive_name):
    record = ATARI_AMIGA["files"][archive_name]
    data = record["content"].encode("utf-8") * record["repetitions"]
    if len(data) != record["bytes"] or hashlib.sha256(data).hexdigest() != record["sha256"]:
        raise RuntimeError("independent Atari-Amiga manifest is inconsistent for " + archive_name)
    return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--tos", type=Path, required=True)
    p.add_argument("--hatari", default="hatari")
    p.add_argument("--hconsole", type=Path,
                    default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    p.add_argument("--cc", default="m68k-atari-mint-gcc")
    p.add_argument("--host-cc", default="cc",
                    help="host compiler for the malformed-metadata archive fixture")
    p.add_argument("--atari-cli", type=Path, default=ROOT / "build/atari/boltpack.tos")
    p.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    p.add_argument("--amiga-archive", type=Path,
                   help="also extract an Amiga-created archive on the FAT12 guest")
    p.add_argument("--output", type=Path)
    args = p.parse_args()
    if args.output:
        out = args.output.resolve()
        out.mkdir(parents=True, exist_ok=False)
    else:
        build = ROOT / "build"
        build.mkdir(exist_ok=True)
        out = Path(tempfile.mkdtemp(prefix="atari-fat12-", dir=build))
    host = out / "host"
    host.mkdir()
    image = out / "floppy.st"
    tos = args.tos.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    shutil.copyfile(args.atari_cli.resolve(strict=True), host / "BOLTPACK.TOS")
    data = manifest_bytes("SRC.TXT")
    nested_data = manifest_bytes("TREE/NESTED/FILE.TXT")
    extra = b"Atari FAT append\n" * 90
    (host / "SRC.TXT").write_bytes(data)
    os.utime(host / "SRC.TXT", (951786000, 951786000))
    tree = host / "TREE"
    (tree / "NESTED").mkdir(parents=True)
    (tree / "EMPTY").mkdir()
    (tree / "NESTED" / "FILE.TXT").write_bytes(nested_data)
    for path in (tree / "NESTED" / "FILE.TXT", tree / "NESTED", tree / "EMPTY", tree):
        os.utime(path, (951786000, 951786000))
    (host / "NEW.TXT").write_bytes(extra)
    wrong_type_archive = out / "BADMETA.BLT"
    with (out / "commands.log").open("wb") as log:
        def run(command, cwd=None, env=None, expected=(0,), timeout=80):
            log.write((repr(command) + "\n").encode()); log.flush()
            result = subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=timeout)
            log.write(result.stdout + ("\nexit=%d\n" % result.returncode).encode()); log.flush()
            if result.returncode not in expected:
                raise RuntimeError("command failed; evidence: " + str(out))
            return result.stdout
        run([args.host_cc, "-std=c89", "-Wall", "-Wextra", "-Wno-unused-function",
             "-Isrc/core",
             str(ROOT / "test/atari/wrong_type_metadata_fixture.c"),
             str(ROOT / "src/core/archiver.c"),
             str(ROOT / "src/core/compress.c"),
             str(ROOT / "src/core/boltlite.c"),
             str(ROOT / "src/core/fluff.c"),
             str(ROOT / "src/core/bp_codepage.c"),
             str(ROOT / "src/core/boltlz.c"), "-lm", "-o",
             str(out / "make-wrong-type-metadata")])
        run([str(out / "make-wrong-type-metadata"), str(wrong_type_archive)])
        run([str(linux), "t", str(wrong_type_archive)], expected=(1,))
        run([args.host_cc, "-std=c89", "-Wall", "-Wextra", "-Wno-unused-function",
             "-Isrc/core",
             str(ROOT / "test/atari/conflicting_metadata_fixture.c"),
             str(ROOT / "src/core/archiver.c"),
             str(ROOT / "src/core/compress.c"),
             str(ROOT / "src/core/boltlite.c"),
             str(ROOT / "src/core/fluff.c"),
             str(ROOT / "src/core/bp_codepage.c"),
             str(ROOT / "src/core/boltlz.c"), "-lm", "-o",
             str(out / "make-conflicting-metadata")])
        native_first_archive = out / "NATFST.BLT"
        portable_first_archive = out / "PORTFST.BLT"
        run([str(out / "make-conflicting-metadata"),
             str(native_first_archive), "native-first"])
        run([str(out / "make-conflicting-metadata"),
             str(portable_first_archive), "portable-first"])
        run([str(linux), "t", str(native_first_archive)])
        run([str(linux), "t", str(portable_first_archive)])
        run([args.cc, "-O2", str(ROOT / "test/atari/guest_runner.c"), "-o", str(host / "RUN.TOS")])
        run([str(linux), "c", "--interchange-profile-1", "-1", "LINUX.BLT", "SRC.TXT"], host)
        run(["mformat", "-i", str(image), "-C", "-f", "720", "::"])
        for name in ("SRC.TXT", "NEW.TXT", "LINUX.BLT"):
            run(["mcopy", "-m", "-i", str(image), str(host / name), "::/" + name])
        run(["mcopy", "-i", str(image), str(host / "LINUX.BLT"), "::/EDIT.BLT"])
        run(["mcopy", "-i", str(image), str(host / "LINUX.BLT"), "::/READONLY.BLT"])
        run(["mattrib", "-i", str(image), "+h", "+s", "::/EDIT.BLT"])
        run(["mattrib", "-i", str(image), "+r", "::/READONLY.BLT"])
        if args.amiga_archive:
            run(["mcopy", "-i", str(image),
                 str(args.amiga_archive.expanduser().resolve(strict=True)),
                 "::/AMIGA.BLT"])
        run(["mcopy", "-i", str(image), str(wrong_type_archive), "::/BADMETA.BLT"])
        run(["mcopy", "-i", str(image), str(native_first_archive), "::/NATFST.BLT"])
        run(["mcopy", "-i", str(image), str(portable_first_archive), "::/PORTFST.BLT"])
        run(["mcopy", "-s", "-m", "-i", str(image), str(tree), "::/TREE"])
        run(["mattrib", "-i", str(image), "+h", "+s", "::/SRC.TXT"])
        run(["mattrib", "-i", str(image), "+h", "+s", "::/TREE"])
        run(["mattrib", "-i", str(image), "+h", "+s", "::/TREE/EMPTY"])
        for name in (("OUT", "RESTORE", "METAOUT", "NATOUT", "PORTOUT") +
                     (("AMIOUT",) if args.amiga_archive else ())):
            run(["mmd", "-i", str(image), "::/" + name])
        before = out / "floppy-before.st"
        shutil.copyfile(image, before)
        commands = [
            r"x -o A:\OUT A:\LINUX.BLT",
            r"c --interchange-profile-1 A:\ATARI.BLT A:\SRC.TXT A:\TREE",
            r"a --interchange-profile-1 A:\ATARI.BLT A:\NEW.TXT",
            r"x -o A:\RESTORE A:\ATARI.BLT",
            r"d A:\ATARI.BLT NEW.TXT",
            r"t A:\ATARI.BLT",
            r"x -o A:\METAOUT A:\BADMETA.BLT",
            r"x -o A:\NATOUT A:\NATFST.BLT",
            r"x -o A:\PORTOUT A:\PORTFST.BLT",
            r"a A:\EDIT.BLT A:\NEW.TXT",
            r"d A:\EDIT.BLT SRC.TXT",
            r"a A:\READONLY.BLT A:\NEW.TXT",
            r"d A:\READONLY.BLT SRC.TXT",
            r"c A:\READONLY.BLT A:\NEW.TXT",
        ]
        if args.amiga_archive:
            commands.append(r"x -o A:\AMIOUT A:\AMIGA.BLT")
        (host / "COMMANDS.TXT").write_text("\n".join(commands) + "\n", encoding="ascii")
        env = os.environ.copy()
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
        hconsole_script = out / "HCONSOLE.TXT"
        hconsole_script.write_text("sleep 25\nquit\n", encoding="ascii")
        run([sys.executable, str(args.hconsole), str(hconsole_script), "--exit", "--",
             "--machine", "ste", "--tos", str(tos), "--sound", "off",
             "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on", "--memsize", "4",
             "--harddrive", str(host), "--disk-a", str(image), "--protect-floppy", "off",
             "--auto", r"C:\RUN.TOS"], ROOT, env, timeout=80)
        statuses = (host / "RESULT.TXT").read_text().splitlines()
        expected_statuses = ["0", "0", "0", "1", "0", "0", "1", "0", "0",
                             "0", "0", "1", "1", "1"]
        if args.amiga_archive:
            expected_statuses.append("1")
        if statuses != expected_statuses:
            raise RuntimeError("guest command statuses %r; evidence: %s" % (statuses, out))
        extract_log = (host / "LOG03.TXT").read_text(errors="replace")
        if "some attributes were not restored" not in extract_log:
            raise RuntimeError("Atari directory metadata limitation was not reported; evidence: " + str(out))
        listing = run(["mdir", "-i", str(image), "::/"])
        (out / "floppy-after-exit.txt").write_bytes(listing)
        old_bytes = before.read_bytes()
        new_bytes = image.read_bytes()
        changed = []
        for offset in range(0, min(len(old_bytes), len(new_bytes)), 512):
            old_sector = old_bytes[offset:offset + 512]
            new_sector = new_bytes[offset:offset + 512]
            if old_sector != new_sector:
                changed.append((offset // 512, sum(a != b for a, b in zip(old_sector, new_sector))))
        (out / "floppy-sector-diff.txt").write_text(
            "changed_sectors=%d\n" % len(changed) +
            "\n".join("sector=%d changed_bytes=%d" % row for row in changed) + "\n")
        if not changed:
            raise RuntimeError("Hatari did not save any floppy sectors; evidence: " + str(out))
        edited_attributes = run(["mattrib", "-X", "-i", str(image), "::/EDIT.BLT"]).decode("ascii").strip()
        readonly_attributes = run(["mattrib", "-X", "-i", str(image), "::/READONLY.BLT"]).decode("ascii").strip()
        if not {"H", "S"}.issubset(set(edited_attributes)) or "R" in edited_attributes:
            raise RuntimeError("Atari append/delete did not preserve archive HIDDEN/SYSTEM flags: %r; evidence: %s" %
                               (edited_attributes, out))
        if "R" not in readonly_attributes:
            raise RuntimeError("Atari read-only archive lost its flag; evidence: " + str(out))
        if not all("archive is read-only" in (host / ("LOG%02d.TXT" % index)).read_text(errors="replace")
                   for index in (11, 12, 13)):
            raise RuntimeError("Atari did not report expected read-only archive refusal; evidence: " + str(out))
        for src, dst in (("::/OUT/SRC.TXT", "linux-to-atari"),
                         ("::/RESTORE/SRC.TXT", "restored"),
                         ("::/RESTORE/NEW.TXT", "added"),
                         ("::/RESTORE/TREE/NESTED/FILE.TXT", "nested"),
                         ("::/METAOUT/CONFLICT.TXT", "wrong-type-restored"),
                         ("::/NATOUT/CONFLICT.TXT", "native-first-restored"),
                         ("::/PORTOUT/CONFLICT.TXT", "portable-first-restored"),
                         ("::/ATARI.BLT", "atari.blt")):
            run(["mcopy", "-i", str(image), src, str(out / dst)])
        run(["mcopy", "-i", str(image), "::/EDIT.BLT", str(out / "edited-atari.blt")])
        run(["mcopy", "-i", str(image), "::/READONLY.BLT", str(out / "readonly-atari.blt")])
        if (out / "readonly-atari.blt").read_bytes() != (host / "LINUX.BLT").read_bytes():
            raise RuntimeError("Atari refused read-only archive edits but changed its bytes; evidence: " + str(out))
        if args.amiga_archive:
            amiga_member = ATARI_AMIGA["amiga_archive_member_names"][0]
            amiga_record = ATARI_AMIGA["files"]["AMIGA/" + amiga_member]
            run(["mcopy", "-i", str(image),
                 "::/AMIOUT/" + amiga_record["atari_name"],
                 str(out / "amiga-to-atari")])
        wrong_type_attr = run(["mattrib", "-p", "-i", str(image),
                               "::/METAOUT/CONFLICT.TXT"]).decode("ascii")
        if "+h" in wrong_type_attr:
            raise RuntimeError("Atari applied directory FAT metadata to a file; evidence: " + str(out))
        precedence = {}
        for label, path in (("native-first", "::/NATOUT/CONFLICT.TXT"),
                            ("portable-first", "::/PORTOUT/CONFLICT.TXT")):
            attr_text = run(["mattrib", "-p", "-i", str(image), path]).decode("ascii")
            attributes = set(re.findall(r"\+(?:r|h|s|a)", attr_text))
            if "+h" not in attributes or "+a" in attributes or "+s" in attributes:
                raise RuntimeError("native metadata did not win %s conflict: %r; evidence: %s" %
                                   (label, attributes, out))
            precedence[label] = sorted(attributes)
        (out / "metadata-precedence.txt").write_text(
            "native TOS metadata wins over conflicting portable FAT metadata\n"
            "native-first=%r\nportable-first=%r\n" %
            (precedence["native-first"], precedence["portable-first"]),
            encoding="ascii")
        def fat_metadata(path, parent, short_name, is_dir=False):
            attr_text = run(["mattrib", "-p", "-i", str(image), path]).decode("ascii")
            attributes = set(re.findall(r"\+(?:r|h|s|a)", attr_text))
            listing_text = run(["mdir", "-a", "-i", str(image), parent]).decode("ascii")
            entry_line = next((line for line in listing_text.splitlines()
                               if line.split()[:1] == [short_name]), "")
            fields = entry_line.split()
            if (len(fields) < 2 or
                    (is_dir and fields[1] != "<DIR>") or
                    (not is_dir and fields[1] != "TXT")):
                raise RuntimeError("FAT entry missing for %s; evidence: %s" % (path, out))
            return attributes, tuple(fields[-2:])
        file_metadata = [
            fat_metadata("::/SRC.TXT", "::/", "SRC"),
            fat_metadata("::/RESTORE/SRC.TXT", "::/RESTORE", "SRC"),
        ]
        if not file_metadata[0][0].issubset(file_metadata[1][0]) or \
                not {"+h", "+s"}.issubset(file_metadata[1][0]):
            raise RuntimeError("Atari did not restore file hidden/system flags: %r; evidence: %s" % (file_metadata, out))
        if file_metadata[0][1] != file_metadata[1][1] or \
                not file_metadata[0][1][0].startswith("2000-"):
            raise RuntimeError("Atari FAT file timestamp changed: %r; evidence: %s" % (file_metadata, out))
        dir_source_attr = run(["mattrib", "-p", "-i", str(image), "::/TREE"]).decode("ascii")
        dir_restored_attr = run(["mattrib", "-p", "-i", str(image), "::/RESTORE/TREE"]).decode("ascii")
        (out / "restored-metadata.txt").write_text(
            "source/restored FAT file metadata=%r\n" % (file_metadata,) +
            "source TREE attributes=%r\nrestored TREE attributes=%r\n" %
            (dir_source_attr.strip(), dir_restored_attr.strip()) +
            "directory timestamp/attribute restore reports partial on TOS 1.62\n")
    if ((out / "linux-to-atari").read_bytes() != data or
            (out / "restored").read_bytes() != data or (out / "added").read_bytes() != extra):
        raise RuntimeError("FAT floppy bytes differ; evidence: " + str(out))
    if (out / "nested").read_bytes() != nested_data:
        raise RuntimeError("FAT directory transfer bytes differ; evidence: " + str(out))
    if args.amiga_archive:
        amiga_member = ATARI_AMIGA["amiga_archive_member_names"][0]
        expected = manifest_bytes("AMIGA/" + amiga_member)
        if (out / "amiga-to-atari").read_bytes() != expected:
            raise RuntimeError("Amiga-to-Atari FAT12 payload mismatch; evidence: " + str(out))
    if (out / "wrong-type-restored").read_bytes() != b"Wrong type metadata test\n":
        raise RuntimeError("Atari metadata-subject refusal changed payload bytes; evidence: " + str(out))
    for label, path in (("native-first", "native-first-restored"),
                        ("portable-first", "portable-first-restored")):
        if (out / path).read_bytes() != b"native metadata wins\n":
            raise RuntimeError("metadata precedence changed %s payload; evidence: %s" %
                               (label, out))
    edited_listing = subprocess.run([str(linux), "l", str(out / "edited-atari.blt")],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    check=True).stdout
    if b"NEW.TXT" not in edited_listing or b"SRC.TXT" in edited_listing:
        raise RuntimeError("Atari archive attribute edit result has incorrect contents; evidence: " + str(out))
    edited_extract = out / "edited-atari-extract"
    edited_extract.mkdir()
    edited_result = subprocess.run(
        [str(linux), "x", "-o", str(edited_extract), str(out / "edited-atari.blt")],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (edited_result.returncode != 1 or
            b"some entries or attributes were not restored" not in edited_result.stdout or
            (edited_extract / "NEW.TXT").read_bytes() != extra):
        raise RuntimeError("Atari edited archive payload mismatch; evidence: " + str(out))
    listing = subprocess.run([str(linux), "l", str(out / "atari.blt")], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, check=True).stdout
    (out / "linux-list.log").write_bytes(listing)
    if b"SRC.TXT" not in listing or b"TREE/" not in listing or b"NEW.TXT" in listing:
        raise RuntimeError("FAT archive delete result incorrect; evidence: " + str(out))
    extracted = out / "linux-extract"
    extracted.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(extracted), str(out / "atari.blt")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(result.stdout)
    if (result.returncode != 1 or b"some entries or attributes were not restored" not in result.stdout or
            (extracted / "SRC.TXT").read_bytes() != data or
            (extracted / "TREE/nested/file.txt").read_bytes() != nested_data or
            not (extracted / "TREE/empty").is_dir() or (extracted / "NEW.TXT").exists()):
        raise RuntimeError("Linux extraction of Atari archive failed; evidence: " + str(out))
    (out / "manifest.txt").write_text(
        "backend=Atari FAT12 floppy under Hatari\n" +
        "image_writeback=verified after Hatari control-console quit/eject\n" +
        "file_metadata=hidden/system flags and FAT timestamp verified\n" +
        "directory_metadata=partial on TOS 1.62; timestamp and nonzero attributes unavailable\n" +
        "rom_sha256=" + hashlib.sha256(tos.read_bytes()).hexdigest() + "\n")
    status = subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT)
    diff = subprocess.check_output(["git", "diff", "--binary", "HEAD"], cwd=ROOT)
    (out / "git-status.txt").write_bytes(status)
    (out / "git-diff.patch").write_bytes(diff)
    try:
        locale_name = locale.setlocale(locale.LC_ALL, None)
    except locale.Error:
        locale_name = "unknown"
    source_paths = (
        ROOT / "Makefile", Path(__file__).resolve(), EXPECTED_PATH,
        ROOT / "src/frontend/atari/main.c", ROOT / "src/core/archiver.c",
        ROOT / "src/core/compress.c", ROOT / "src/core/bp_codepage.c",
        ROOT / "test/atari/guest_runner.c",
    )
    record = {
        "purpose": "Linux/Atari FAT12 CLI exchange under Hatari",
        "result": "PASS: transfer, edits, metadata, refusal, and failure-preservation cases",
        "date_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "worktree_status": status.decode("utf-8", "replace").splitlines(),
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "environment": {
            "platform": platform.platform(),
            "python": sys.version.split()[0],
            "locale": locale_name,
            "timezone": list(time.tzname),
            "hatari": command_version([args.hatari, "--version"]),
            "atari_compiler": command_version([args.cc, "--version"]),
            "host_compiler": command_version([args.host_cc, "--version"]),
        },
        "filesystem": "720 KiB FAT12 floppy image under Hatari/TOS",
        "commands": "commands.log",
        "expected_manifest": {
            "path": EXPECTED_PATH.relative_to(ROOT).as_posix(),
            "size": EXPECTED_PATH.stat().st_size,
            "sha256": sha256(EXPECTED_PATH),
        },
        "inputs": {
            "tos_sha256": sha256(tos),
            "linux_cli_sha256": sha256(linux),
            "atari_cli_sha256": sha256(args.atari_cli.resolve(strict=True)),
            "hconsole_sha256": sha256(args.hconsole.resolve(strict=True)),
            "amiga_archive_sha256": sha256(args.amiga_archive.resolve(strict=True))
            if args.amiga_archive else None,
        },
        "source_sha256": {
            path.relative_to(ROOT).as_posix(): sha256(path)
            for path in source_paths if path.is_file()
        },
        "artifacts": artifact_index(out),
        "limitations": [
            "This run uses a Hatari-emulated FAT12 image, not physical Atari hardware.",
            "TOS 1.62 reports unsupported directory metadata as partial.",
        ],
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n",
                                   encoding="utf-8")
    print("PASS: Atari FAT12 guest transfer and archive edits; evidence:",
          out / "run.json")


if __name__ == "__main__":
    main()
