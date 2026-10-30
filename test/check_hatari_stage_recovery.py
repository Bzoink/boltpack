#!/usr/bin/env python3
"""Exercise production recovery from an interrupted Atari FAT12 extraction."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command, cwd, env=None, timeout=240):
    return subprocess.run(command, cwd=cwd, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, timeout=timeout)


def mtools(tool, image, args, cwd, timeout=60):
    return run([tool, "-i", str(image), *args], cwd, timeout=timeout)


def path_field(value):
    raw = value.encode("ascii")
    if len(raw) > 125:
        raise ValueError("GEMDOS recovery path exceeds 125 bytes")
    return raw + bytes(126 - len(raw))


def make_journal(target, temporary, backup, old, new):
    import struct
    import zlib
    record = bytearray(414)
    record[:8] = b"BPTXN6\0\0"
    struct.pack_into(">I", record, 8, 1)
    struct.pack_into(">I", record, 16, 1)
    struct.pack_into(">I", record, 20, len(old))
    struct.pack_into(">I", record, 24, zlib.crc32(old))
    struct.pack_into(">I", record, 28, len(new))
    struct.pack_into(">I", record, 32, zlib.crc32(new))
    record[36:162] = path_field(target)
    record[162:288] = path_field(temporary)
    record[288:414] = path_field(backup)
    struct.pack_into(">I", record, 12, zlib.crc32(record))
    return bytes(record)


def guest_command(tos, hconsole, image, script):
    return [sys.executable, str(hconsole), str(script), "--exit", "--",
            "--machine", "ste", "--tos", str(tos), "--sound", "off",
            "--confirm-quit", "off", "--fast-forward", "on", "--fast-boot", "on",
            "--memsize", "4", "--disk-a", str(image), "--protect-floppy", "off",
            "--auto", r"A:\BOLTPACK.PRG"]


def fat_names(output):
    return sorted(line.strip().rsplit("/", 1)[-1].upper()
                  for line in output.splitlines() if line.strip())


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tos", type=Path, required=True)
    parser.add_argument("--hatari", default="hatari")
    parser.add_argument("--hconsole", type=Path, default=Path("/usr/share/hatari/hconsole/hconsole.py"))
    parser.add_argument("--final-gui", type=Path, required=True)
    parser.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    parser.add_argument("--output-root", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    tos = args.tos.expanduser().resolve(strict=True)
    gui = args.final_gui.resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    hconsole = args.hconsole.expanduser().resolve(strict=True)
    hatari = shutil.which(args.hatari)
    if hatari is None:
        raise RuntimeError("Hatari executable not found: " + args.hatari)
    output = args.output_root.resolve()
    output.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="atari-stage-recovery-", dir=output))
    host = out / "host"
    host.mkdir()
    image = out / "floppy.st"
    payload = bytes(range(256)) * 512  # 128 KiB stored member
    partial_stage = payload[:32768]
    old_target = b"existing target must survive staged-extraction interruption\n"
    (host / "PAYLOAD.BIN").write_bytes(payload)
    created = run([str(linux), "c", "--store", "ARCHIVE.BLT", "PAYLOAD.BIN"], host)
    (out / "archive-create.log").write_text(created.stdout, encoding="utf-8")
    if created.returncode != 0:
        raise RuntimeError("could not create the recovery archive: " + str(out))
    archive = host / "ARCHIVE.BLT"
    verified = run([str(linux), "t", str(archive)], out)
    (out / "archive-test.log").write_text(verified.stdout, encoding="utf-8")
    if verified.returncode != 0:
        raise RuntimeError("Linux rejected the recovery archive: " + str(out))

    setup = run(["mformat", "-i", str(image), "-C", "-f", "1440", "::"], out)
    if setup.returncode != 0:
        raise RuntimeError("could not create FAT12 media: " + setup.stdout)
    setup = mtools("mmd", image, ["::/OUT"], out)
    if setup.returncode != 0:
        raise RuntimeError("could not create OUT directory: " + setup.stdout)
    for source, dest in ((gui, "::/BOLTPACK.PRG"), (archive, "::/ARCHIVE.BLT")):
        copied = mtools("mcopy", image, ["-o", str(source), dest], out)
        if copied.returncode != 0:
            raise RuntimeError("could not seed FAT12 image: " + copied.stdout)
    for source, dest, data in (
            (out / "old-target.seed", "::/OUT/PAYLOAD.BIN", old_target),
            (out / "partial-stage.seed", "::/OUT/BPSTG01", partial_stage)):
        source.write_bytes(data)
        copied = mtools("mcopy", image, ["-o", str(source), dest], out)
        if copied.returncode != 0:
            raise RuntimeError("could not seed interrupted files: " + copied.stdout)

    # This is the durable pre-publication state after process termination:
    # old target intact, stage contains a strict prefix, and v1 journal exists.
    journal = make_journal(r"A:\OUT\PAYLOAD.BIN", r"A:\OUT\BPSTG01",
                           r"A:\OUT\BRSTG01", old_target, payload)
    journal_path = out / "BPRECOVR.JRN"
    journal_path.write_bytes(journal)
    copied = mtools("mcopy", image, ["-o", str(journal_path), "::/OUT/BPRECOVR.JRN"], out)
    if copied.returncode != 0:
        raise RuntimeError("could not seed interrupted transaction journal: " + copied.stdout)
    listing = mtools("mdir", image, ["-b", "::/OUT"], out)
    names_before = fat_names(listing.stdout)
    if names_before != ["BPRECOVR.JRN", "BPSTG01", "PAYLOAD.BIN"]:
        raise RuntimeError("seeded crash state differs from expectation: " + repr(names_before))
    old_copy = out / "old-before-recovery.bin"
    copied = mtools("mcopy", image, ["::/OUT/PAYLOAD.BIN", str(old_copy)], out)
    if copied.returncode != 0 or old_copy.read_bytes() != old_target:
        raise RuntimeError("fixture did not preserve the existing target before recovery")

    script = out / "extract.HCONSOLE.TXT"
    script.write_text("\n".join((
        "sleep 20", "screenshot", "keydown 29", "keypress o", "keyup 29",
        "sleep 3", "text ARCHIVE.BLT", "keypress 28", "sleep 4", "screenshot",
        "keydown 29", "keypress e", "keyup 29", "sleep 3",
        "text OUT", "keypress 28", "sleep 5", "screenshot", "sleep 30",
        "screenshot", "quit", "",
    )), encoding="ascii")
    env = os.environ.copy()
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    extract_command = guest_command(tos, hconsole, image, script)
    extracted = run(extract_command, out, env=env, timeout=180)
    (out / "production-recovery.log").write_text(extracted.stdout, encoding="utf-8")
    if extracted.returncode != 0:
        raise RuntimeError("production GUI recovery command failed: " + str(out))
    listing_after = mtools("mdir", image, ["-b", "::/OUT"], out)
    names_after = fat_names(listing_after.stdout)
    if names_after != ["BPSTG01", "PAYLOAD.BIN"]:
        raise RuntimeError("recovery did not preserve only the unverified old stage: " + repr(names_after))
    recovered = out / "recovered.bin"
    copied = mtools("mcopy", image, ["::/OUT/PAYLOAD.BIN", str(recovered)], out)
    if copied.returncode != 0 or recovered.read_bytes() != payload:
        raise RuntimeError("production retry did not restore exact archived bytes: " + str(out))
    stage_copy = out / "preserved-partial-stage.bin"
    copied = mtools("mcopy", image, ["::/OUT/BPSTG01", str(stage_copy)], out)
    if copied.returncode != 0 or stage_copy.read_bytes() != partial_stage:
        raise RuntimeError("recovery changed the unverified partial stage: " + str(out))

    status = run(["git", "status", "--short"], ROOT)
    diff = run(["git", "diff", "--binary", "--", "Makefile",
                "src/frontend/atari_gui/main.c", "test/check_hatari_journal_path.py",
                "test/check_hatari_stage_recovery.py"], ROOT)
    (out / "source-status.txt").write_text(status.stdout, encoding="utf-8")
    (out / "source-diff.patch").write_text(diff.stdout, encoding="utf-8")
    record = {
        "purpose": "recover the destination after pre-publication interruption on Atari FAT12",
        "result": "PASS: original target survived; production retry restored exact archive bytes; unverifiable partial stage was retained and journal cleared",
        "hatari": run([hatari, "--version"], out).stdout.strip(),
        "tos_rom": tos.name,
        "tos_sha256": hashlib.sha256(tos.read_bytes()).hexdigest(),
        "gui_sha256": hashlib.sha256(gui.read_bytes()).hexdigest(),
        "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "source_revision": run(["git", "rev-parse", "HEAD"], ROOT).stdout.strip(),
        "source_status_sha256": hashlib.sha256(status.stdout.encode()).hexdigest(),
        "source_diff_sha256": hashlib.sha256(diff.stdout.encode()).hexdigest(),
        "filesystem": "1.44 MiB FAT12 floppy image",
        "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "old_target_sha256": hashlib.sha256(old_target).hexdigest(),
        "journal_sha256": hashlib.sha256(journal).hexdigest(),
        "partial_stage_bytes": len(partial_stage),
        "partial_stage_sha256": hashlib.sha256(partial_stage).hexdigest(),
        "pre_recovery_names": names_before,
        "post_recovery_names": names_after,
        "recovered_sha256": hashlib.sha256(recovered.read_bytes()).hexdigest(),
        "old_target_preserved_before_retry": old_copy.read_bytes() == old_target,
        "partial_stage_preserved_unmodified": stage_copy.read_bytes() == partial_stage,
        "screenshots": ["grab0001.png", "grab0002.png", "grab0003.png", "grab0004.png"],
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print("PASS: FAT12 recovery restored exact target bytes and preserved the unverified stage; evidence:", out)


if __name__ == "__main__":
    main()
