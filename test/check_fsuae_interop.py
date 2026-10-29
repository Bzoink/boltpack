#!/usr/bin/env python3
"""Run a basic Amiga/Linux Stored exchange on a disposable FS-UAE host mount."""
import argparse
import hashlib
import json
import locale
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
EXPECTED = json.loads((ROOT / "test/reference/interoperability/amiga-host-directory.json").read_text(encoding="utf-8"))
ATARI_AMIGA = json.loads((ROOT / "test/reference/interoperability/atari-amiga-fsuae.json").read_text(encoding="utf-8"))


def check_manifest_file(path, key):
    expected = EXPECTED["files"][key]
    if expected.get("type") == "directory":
        if not path.is_dir():
            raise RuntimeError("independent expected-result mismatch: missing directory %s" % key)
        return
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != expected["bytes"] or digest != expected["sha256"]:
        raise RuntimeError("independent expected-result mismatch for %s: %d bytes, sha256=%s" %
                           (key, len(data), digest))


def command_version(command):
    try:
        output = subprocess.check_output(command, stderr=subprocess.STDOUT,
                                         text=True, timeout=10)
    except (OSError, subprocess.SubprocessError) as exc:
        return "unavailable: " + str(exc)
    return output.strip().splitlines()[0] if output.strip() else "unknown"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--rom", type=Path, required=True)
    p.add_argument("--workbench", type=Path, required=True)
    p.add_argument("--xdftool", default="xdftool")
    p.add_argument("--fs-uae", default="fs-uae")
    p.add_argument("--amiga-cli", type=Path, default=ROOT / "build/amiga/boltpack")
    p.add_argument("--amiga-cc", default="/opt/amiga/bin/m68k-amigaos-gcc")
    p.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    p.add_argument("--atari-archive", type=Path,
                   help="also extract an Atari-created archive inside the Amiga guest")
    p.add_argument("--export-amiga-archive", action="store_true",
                   help="copy the guest-created Profile 1 archive to the evidence directory")
    p.add_argument("--output", type=Path)
    p.add_argument("--stack", type=int, default=262144,
                   help="guest shell stack in bytes; 0 keeps its default")
    p.add_argument("--depth", type=int, default=0,
                   help="add a nested source tree at this depth (0 disables it)")
    p.add_argument("--chip-memory", type=int, choices=(512, 1024, 2048),
                   default=2048, help="guest chip RAM in KiB")
    p.add_argument("--fast-memory", type=int,
                   choices=(0, 1024, 2048, 4096, 8192), default=8192,
                   help="guest fast RAM in KiB")
    p.add_argument("--codec-block", choices=("16k", "32k", "256k"),
                   default="256k", help="block size for the all-codec corpus")
    p.add_argument("--expect-fluff-refusal", action="store_true",
                   help="expect a nonzero Fluff create with no published archive")
    p.add_argument("--timeout", type=int, default=60)
    args = p.parse_args()
    if args.depth < 0 or args.depth > 64:
        p.error("--depth must be between 0 and 64")
    if args.expect_fluff_refusal and (args.chip_memory != 512 or
                                      args.fast_memory != 0 or
                                      args.codec_block != "16k"):
        p.error("--expect-fluff-refusal requires 512 KiB chip, 0 KiB fast, and 16k blocks")
    out = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="boltpack-fsuae-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    drive = out / "drive"
    drive.mkdir()
    (drive / "OUT").mkdir()
    (drive / "OUTLITE").mkdir()
    for name in ("OUT_STORED", "OUT_LITE", "OUT_BOLTLZ", "OUT_FLUFF"):
        (drive / name).mkdir()
    (drive / "TREEOUT").mkdir()
    (drive / "BADOUT").mkdir()
    if args.atari_archive:
        (drive / "ATARI_OUT").mkdir()
    (drive / "TREE" / "NEST" / "EMPTY").mkdir(parents=True)
    home = out / "home"
    (home / "Desktop").mkdir(parents=True)
    rom = args.rom.expanduser().resolve(strict=True)
    wb = args.workbench.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    disk = out / "workbench.adf"
    shutil.copyfile(wb, disk)
    shutil.copyfile(args.amiga_cli.resolve(strict=True), drive / "boltpack")
    if args.atari_archive:
        shutil.copyfile(args.atari_archive.expanduser().resolve(strict=True),
                        drive / "ATARI.BLT")
    cross_source = ATARI_AMIGA["files"]["AMIGA/SRC.TXT"]
    data = cross_source["content"].encode("utf-8") * cross_source["repetitions"]
    if len(data) != cross_source["bytes"] or \
            hashlib.sha256(data).hexdigest() != cross_source["sha256"]:
        raise RuntimeError("independent Atari-Amiga source manifest is inconsistent")
    codec_data = bytes((i * 73 + i // 17) & 0xff for i in range(512 * 1024))
    (drive / "SRC.TXT").write_bytes(data)
    (drive / "CODEC.DAT").write_bytes(codec_data)
    sentinel = b"existing destination survives failed extraction\n"
    (drive / "BADOUT" / "SRC.TXT").write_bytes(sentinel)
    added = b"Amiga append data\n" * 128
    (drive / "NEW.TXT").write_bytes(added)
    (drive / "TREE" / "NEST" / "FILE.TXT").write_bytes(added)
    check_manifest_file(drive / "SRC.TXT", "src")
    check_manifest_file(drive / "NEW.TXT", "added")
    check_manifest_file(drive / "TREE" / "NEST" / "FILE.TXT", "added")
    check_manifest_file(drive / "TREE" / "NEST" / "EMPTY", "empty")
    check_manifest_file(drive / "CODEC.DAT", "codec")
    check_manifest_file(drive / "BADOUT" / "SRC.TXT", "failed_extraction_sentinel")
    deep_payload = b"default-stack deep tree fixture\n" * 32
    # Keep the cumulative AmigaDOS path below its legacy component/path ceiling
    # so depth tests isolate traversal and stack behavior.
    deep_parts = ["D"] * args.depth
    if args.depth:
        deep_leaf = drive / "DEEP" / Path(*deep_parts) / "LEAF.TXT"
        deep_leaf.parent.mkdir(parents=True)
        deep_leaf.write_bytes(deep_payload)
        (drive / "DEEPOUT").mkdir()
    with (out / "commands.log").open("wb") as log:
        subprocess.run([args.amiga_cc, "-noixemul", "-O2", str(ROOT / "test/amiga/metadata_probe.c"),
                        "-o", str(drive / "METAPROBE")], stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([str(linux), "c", "--interchange-profile-1", "-0", "LINUX.BLT", "SRC.TXT"],
                       cwd=drive, stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([str(linux), "c", "--interchange-profile-1", "-1", "LNX1.BLT", "SRC.TXT"],
                       cwd=drive, stdout=log, stderr=subprocess.STDOUT, check=True)
        corrupt = bytearray((drive / "LINUX.BLT").read_bytes())
        payload = corrupt.find(data)
        if payload < 0 or corrupt.find(data, payload + 1) >= 0:
            raise RuntimeError("stored archive payload not found uniquely")
        corrupt[payload + len(data) // 2] ^= 1
        (drive / "CORRUPT.BLT").write_bytes(corrupt)
        corruption_check = subprocess.run([str(linux), "t", str(drive / "CORRUPT.BLT")],
                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log.write(corruption_check.stdout)
        if corruption_check.returncode == 0 or b"checksum mismatch" not in corruption_check.stdout:
            raise RuntimeError("corrupt fixture did not trigger checksum verification")
        startup = out / "startup"
        initial = ["C:SetPatch >NIL: QUIET", "C:Assign T: RAM:"]
        if args.stack:
            initial.append("C:Stack %d" % args.stack)
        startup.write_text("\n".join(initial + [
            "C:FailAt 100",
            "DH0:boltpack x -o DH0:OUT DH0:LINUX.BLT >DH0:EXTRACT.LOG",
            "C:Echo $RC >DH0:EXTRACT.RC",
            "DH0:boltpack x -o DH0:OUTLITE DH0:LNX1.BLT >DH0:EXLITE.LOG",
            "C:Echo $RC >DH0:EXLITE.RC",
            *(["DH0:boltpack x -o DH0:ATARI_OUT DH0:ATARI.BLT >DH0:ATARI.LOG",
               "C:Echo $RC >DH0:ATARI.RC",
               "DH0:boltpack t DH0:ATARI.BLT >DH0:ATARITEST.LOG",
               "C:Echo $RC >DH0:ATARITEST.RC"] if args.atari_archive else []),
            "DH0:boltpack c -b %s DH0:AMI_BOLTLZ.BLT DH0:CODEC.DAT >DH0:BOLTLZCRT.LOG" % args.codec_block,
            "C:Echo $RC >DH0:BOLTLZCRT.RC",
            "DH0:boltpack c -b %s -F DH0:AMI_FLUFF.BLT DH0:CODEC.DAT >DH0:FLUFFCRT.LOG" % args.codec_block,
            "C:Echo $RC >DH0:FLUFFCRT.RC",
            "DH0:boltpack c -b %s -0 DH0:AMI_STORED.BLT DH0:CODEC.DAT >DH0:STOREDCRT.LOG" % args.codec_block,
            "C:Echo $RC >DH0:STOREDCRT.RC",
            "DH0:boltpack c -b %s -1 DH0:AMI_LITE.BLT DH0:CODEC.DAT >DH0:LITECRT.LOG" % args.codec_block,
            "C:Echo $RC >DH0:LITECRT.RC",
            "DH0:boltpack t DH0:AMI_BOLTLZ.BLT >DH0:BOLTLZT.LOG",
            "C:Echo $RC >DH0:BOLTLZT.RC",
            *([] if args.expect_fluff_refusal else [
                "DH0:boltpack t DH0:AMI_FLUFF.BLT >DH0:FLUFFT.LOG",
                "C:Echo $RC >DH0:FLUFFT.RC"]),
            "DH0:boltpack t DH0:AMI_STORED.BLT >DH0:STOREDT.LOG",
            "C:Echo $RC >DH0:STOREDT.RC",
            "DH0:boltpack t DH0:AMI_LITE.BLT >DH0:LITET.LOG",
            "C:Echo $RC >DH0:LITET.RC",
            "DH0:boltpack x -o DH0:OUT_STORED DH0:AMI_STORED.BLT >DH0:STOREDX.LOG",
            "C:Echo $RC >DH0:STOREDX.RC",
            "DH0:boltpack x -o DH0:OUT_LITE DH0:AMI_LITE.BLT >DH0:LITEX.LOG",
            "C:Echo $RC >DH0:LITEX.RC",
            "DH0:boltpack x -o DH0:OUT_BOLTLZ DH0:AMI_BOLTLZ.BLT >DH0:BOLTLZX.LOG",
            "C:Echo $RC >DH0:BOLTLZX.RC",
            *([] if args.expect_fluff_refusal else [
                "DH0:boltpack x -o DH0:OUT_FLUFF DH0:AMI_FLUFF.BLT >DH0:FLUFFX.LOG",
                "C:Echo $RC >DH0:FLUFFX.RC"]),
            "DH0:boltpack c --interchange-profile-1 -0 DH0:AMIGA.BLT DH0:SRC.TXT >DH0:CREATE.LOG",
            "C:Echo $RC >DH0:CREATE.RC",
            "DH0:boltpack c --interchange-profile-1 DH0:LITE.BLT DH0:SRC.TXT >DH0:LITE.LOG",
            "C:Echo $RC >DH0:LITE.RC",
            "DH0:METAPROBE set DH0:LITE.BLT",
            "C:Echo $RC >DH0:META.RC",
            "DH0:METAPROBE get DH0:LITE.BLT >DH0:METABEF.TXT",
            "DH0:boltpack a --interchange-profile-1 DH0:LITE.BLT DH0:NEW.TXT >DH0:ADD.LOG",
            "C:Echo $RC >DH0:ADD.RC",
            "DH0:METAPROBE get DH0:LITE.BLT >DH0:METAAFT.TXT",
            "C:Copy DH0:LITE.BLT DH0:SNAP.BLT QUIET",
            "DH0:boltpack a --interchange-profile-1 DH0:LITE.BLT DH0:SRC.TXT DH0:MISSING.TXT >DH0:FAILADD.LOG",
            "C:Echo $RC >DH0:FAILADD.RC",
            "DH0:boltpack c --interchange-profile-1 DH0:LITE.BLT DH0:SRC.TXT DH0:MISSING.TXT >DH0:FAILCRT.LOG",
            "C:Echo $RC >DH0:FAILCRT.RC",
            "DH0:boltpack c --interchange-profile-1 DH0:NEWBAD.BLT DH0:SRC.TXT DH0:MISSING.TXT >DH0:FAILNEW.LOG",
            "C:Echo $RC >DH0:FAILNEW.RC",
            "C:Copy DH0:SNAP.BLT DH0:DELETE.BLT QUIET",
            "DH0:METAPROBE set DH0:DELETE.BLT",
            "C:Echo $RC >DH0:DELMETA.RC",
            "DH0:METAPROBE get DH0:DELETE.BLT >DH0:DELBEF.TXT",
            "DH0:boltpack d DH0:DELETE.BLT MISSING.TXT >DH0:FAILDEL.LOG",
            "C:Echo $RC >DH0:FAILDEL.RC",
            "C:Copy DH0:DELETE.BLT DH0:FAILDEL.BLT QUIET",
            "DH0:boltpack d DH0:DELETE.BLT NEW.TXT >DH0:DELETE.LOG",
            "C:Echo $RC >DH0:DELETE.RC",
            "DH0:METAPROBE get DH0:DELETE.BLT >DH0:DELAFT.TXT",
            "DH0:METAPROBE set DH0:TREE/NEST",
            "C:Echo $RC >DH0:DIRSET.RC",
            "DH0:METAPROBE set DH0:TREE/NEST/EMPTY",
            "C:Echo $RC >DH0:EMPSET.RC",
            "DH0:METAPROBE set DH0:TREE/NEST/FILE.TXT",
            "C:Echo $RC >DH0:FILESET.RC",
            "DH0:METAPROBE get DH0:TREE/NEST >DH0:DIRSRC.TXT",
            "DH0:METAPROBE get DH0:TREE/NEST/EMPTY >DH0:EMPSRC.TXT",
            "DH0:METAPROBE get DH0:TREE/NEST/FILE.TXT >DH0:FILESRC.TXT",
            "DH0:boltpack c --interchange-profile-1 DH0:TREE.BLT DH0:TREE >DH0:TREECRT.LOG",
            "C:Echo $RC >DH0:TREECRT.RC",
            "DH0:boltpack x -o DH0:TREEOUT DH0:TREE.BLT >DH0:TREEX.LOG",
            "C:Echo $RC >DH0:TREEX.RC",
            "DH0:METAPROBE get DH0:TREEOUT/TREE/NEST >DH0:DIRDST.TXT",
            "DH0:METAPROBE get DH0:TREEOUT/TREE/NEST/EMPTY >DH0:EMPDST.TXT",
            "DH0:METAPROBE get DH0:TREEOUT/TREE/NEST/FILE.TXT >DH0:FILEDST.TXT",
            *(["DH0:boltpack c --interchange-profile-1 DH0:DEEP.BLT DH0:DEEP >DH0:DEEPCRT.LOG",
               "C:Echo $RC >DH0:DEEPCRT.RC",
               "DH0:boltpack x -o DH0:DEEPOUT DH0:DEEP.BLT >DH0:DEEPEXTRACT.LOG",
               "C:Echo $RC >DH0:DEEPEXTRACT.RC"] if args.depth else []),
            "DH0:METAPROBE set DH0:BADOUT/SRC.TXT",
            "C:Echo $RC >DH0:BADSET.RC",
            "DH0:METAPROBE get DH0:BADOUT/SRC.TXT >DH0:BADBEF.TXT",
            "DH0:boltpack x -f -o DH0:BADOUT DH0:CORRUPT.BLT >DH0:BADX.LOG",
            "C:Echo $RC >DH0:BADX.RC",
            "DH0:METAPROBE get DH0:BADOUT/SRC.TXT >DH0:BADAFT.TXT",
            "DH0:boltpack c --interchange-profile-1 -F DH0:BAD.BLT DH0:SRC.TXT >DH0:BAD.LOG",
            "C:Echo $RC >DH0:BAD.RC", "C:Echo FINISHED >DH0:DONE.TXT", ""]), encoding="ascii")
        for command in ([args.xdftool, str(disk), "delete", "S/Startup-Sequence"],
                        [args.xdftool, str(disk), "write", str(startup), "S/Startup-Sequence"]):
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        env = os.environ.copy()
        env.update(HOME=str(home), LIBGL_ALWAYS_SOFTWARE="1", SDL_AUDIODRIVER="dummy")
        command = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24", args.fs_uae,
                   "--amiga_model=A500+", "--kickstart_file=" + str(rom),
                   "--floppy_drive_0=" + str(disk), "--hard_drive_0=" + str(drive),
                   "--floppy_drive_speed=800", "--warp_mode=1", "--fullscreen=0",
                   "--chip_memory=" + str(args.chip_memory),
                   "--fast_memory=" + str(args.fast_memory)]
        log.write((repr(command) + "\n").encode()); log.flush()
        guest = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        try:
            deadline = time.monotonic() + args.timeout
            while not (drive / "DONE.TXT").is_file():
                if guest.poll() is not None:
                    raise RuntimeError("FS-UAE exited before completion; see " + str(out))
                if time.monotonic() >= deadline:
                    raise RuntimeError("guest timed out; evidence: " + str(out))
                time.sleep(0.25)
        finally:
            if guest.poll() is None:
                os.killpg(guest.pid, signal.SIGTERM)
            try:
                guest.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(guest.pid, signal.SIGKILL)
                guest.wait()
    required_status = ["CREATE.RC", "EXTRACT.RC", "EXLITE.RC", "LITE.RC", "META.RC", "ADD.RC", "DELMETA.RC", "DELETE.RC", "DIRSET.RC", "EMPSET.RC", "FILESET.RC", "TREECRT.RC", "TREEX.RC", "BADSET.RC", "BOLTLZCRT.RC", "STOREDCRT.RC", "LITECRT.RC", "BOLTLZT.RC", "STOREDT.RC", "LITET.RC", "STOREDX.RC", "LITEX.RC", "BOLTLZX.RC"]
    if not args.expect_fluff_refusal:
        required_status.extend(("FLUFFT.RC", "FLUFFX.RC"))
    required_status.append("FLUFFCRT.RC")
    for name in required_status:
        if (drive / name).read_text().strip() != "0":
            if name == "FLUFFCRT.RC" and args.expect_fluff_refusal:
                continue
            raise RuntimeError("guest failure: " + name + "; evidence: " + str(out))
    if args.expect_fluff_refusal:
        fluff_status = (drive / "FLUFFCRT.RC").read_text().strip()
        if fluff_status == "0" or (drive / "AMI_FLUFF.BLT").exists() or \
                any(drive.glob("BP??????")) or list((drive / "OUT_FLUFF").iterdir()):
            raise RuntimeError("constrained Fluff create did not fail cleanly; evidence: " + str(out))
    if args.atari_archive:
        if (drive / "ATARITEST.RC").read_text().strip() != "0":
            raise RuntimeError("Amiga CLI failed to validate Atari-created archive; evidence: " + str(out))
        atari_status = (drive / "ATARI.RC").read_text().strip()
        if atari_status != "1":
            raise RuntimeError("Amiga extraction of Atari metadata should report partial; evidence: " + str(out))
        for archive_name in ATARI_AMIGA["archive_member_names"]:
            expected = ATARI_AMIGA["files"][archive_name]
            restored = drive / "ATARI_OUT" / Path(*expected["amiga_name"].split("/"))
            if expected.get("type") == "directory":
                if not restored.is_dir():
                    raise RuntimeError("Amiga guest missed Atari directory %s; evidence: %s" % (archive_name, out))
            else:
                data = restored.read_bytes()
                if len(data) != expected["bytes"] or \
                        hashlib.sha256(data).hexdigest() != expected["sha256"]:
                    raise RuntimeError("Atari-to-Amiga payload mismatch for %s; evidence: %s" %
                                       (archive_name, out))
    codec_names = ("STORED", "LITE", "BOLTLZ")
    if not args.expect_fluff_refusal:
        codec_names += ("FLUFF",)
    for name in codec_names:
        restored = drive / ("OUT_" + name) / "CODEC.DAT"
        check_manifest_file(restored, "codec")
        archive = drive / ("AMI_" + name + ".BLT")
        linux_test = subprocess.run([str(linux), "t", str(archive)],
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT)
        (out / ("linux-test-" + name.lower() + ".log")).write_bytes(linux_test.stdout)
        if linux_test.returncode != 0:
            raise RuntimeError("Linux rejected Amiga %s archive; evidence: %s" % (name, out))
        linux_out = out / ("linux-codec-" + name.lower())
        linux_out.mkdir()
        result = subprocess.run([str(linux), "x", "-o", str(linux_out), str(archive)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / ("linux-codec-" + name.lower() + ".log")).write_bytes(result.stdout)
        if result.returncode != 1 or not (linux_out / "CODEC.DAT").is_file():
            raise RuntimeError("Linux failed to verify/extract Amiga %s archive; evidence: %s" %
                               (name, out))
        check_manifest_file(linux_out / "CODEC.DAT", "codec")
    if args.depth:
        for name in ("DEEPCRT.RC", "DEEPEXTRACT.RC"):
            if (drive / name).read_text().strip() != "0":
                raise RuntimeError("deep-tree guest failure: " + name + "; evidence: " + str(out))
        deep_restored = drive / "DEEPOUT" / "DEEP" / Path(*deep_parts) / "LEAF.TXT"
        if deep_restored.read_bytes() != deep_payload:
            raise RuntimeError("deep-tree content mismatch; evidence: " + str(out))
    check_manifest_file(drive / "OUT" / "SRC.TXT", "src")
    check_manifest_file(drive / "OUTLITE" / "SRC.TXT", "src")
    if (drive / "BAD.RC").read_text().strip() == "0" or (drive / "BAD.BLT").exists():
        raise RuntimeError("incompatible profile options were not refused; evidence: " + str(out))
    if ((drive / "BADX.RC").read_text().strip() == "0" or
            (drive / "BADOUT" / "SRC.TXT").read_bytes() != sentinel or
            (drive / "BADBEF.TXT").read_bytes() != (drive / "BADAFT.TXT").read_bytes() or
            any(p.name not in ("SRC.TXT", "SRC.TXT.uaem") for p in (drive / "BADOUT").iterdir())):
        raise RuntimeError("corrupt extraction changed existing file, metadata, or left stage; evidence: " + str(out))
    for name in ("FAILADD.RC", "FAILCRT.RC", "FAILNEW.RC", "FAILDEL.RC"):
        if (drive / name).read_text().strip() == "0":
            raise RuntimeError("failed operation returned success: " + name)
    if (drive / "LITE.BLT").read_bytes() != (drive / "SNAP.BLT").read_bytes():
        raise RuntimeError("failed create/append modified existing archive; evidence: " + str(out))
    if (drive / "FAILDEL.BLT").read_bytes() != (drive / "SNAP.BLT").read_bytes():
        raise RuntimeError("failed delete modified existing archive; evidence: " + str(out))
    before = (drive / "METABEF.TXT").read_bytes()
    delete_before = (drive / "DELBEF.TXT").read_bytes()
    if (b"BoltPack archive comment" not in before or not (int(before.split(b":", 1)[0]) & 64) or
            b"BoltPack archive comment" not in delete_before or
            not (int(delete_before.split(b":", 1)[0]) & 64) or
            before != (drive / "METAAFT.TXT").read_bytes() or
            delete_before != (drive / "DELAFT.TXT").read_bytes()):
        raise RuntimeError("archive native protection/comment changed during replacement; evidence: " + str(out))
    if (drive / "NEWBAD.BLT").exists() or list(drive.glob("BP??????")):
        raise RuntimeError("failed create/append left output or stage; evidence: " + str(out))
    for prefix in ("DIR", "EMP", "FILE"):
        original_meta = (drive / (prefix + "SRC.TXT")).read_bytes()
        restored_meta = (drive / (prefix + "DST.TXT")).read_bytes()
        if (b"BoltPack archive comment" not in original_meta or
                not (int(original_meta.split(b":", 1)[0]) & 64) or original_meta != restored_meta):
            raise RuntimeError("archived %s metadata mismatch; evidence: %s" % (prefix, out))
    restored = drive / "TREEOUT" / "TREE" / "NEST"
    if not (restored / "FILE.TXT").is_file():
        raise RuntimeError("archived tree content mismatch; evidence: " + str(out))
    check_manifest_file(restored / "EMPTY", "empty")
    check_manifest_file(restored / "FILE.TXT", "added")
    target = out / "linux-output"
    target.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(target), str(drive / "AMIGA.BLT")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(result.stdout)
    if result.returncode != 1 or b"some entries or attributes were not restored" not in result.stdout:
        raise RuntimeError("unexpected Linux metadata result; evidence: " + str(out))
    check_manifest_file(target / "SRC.TXT", "src")
    lite_target = out / "linux-lite"
    lite_target.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(lite_target), str(drive / "LITE.BLT")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-lite.log").write_bytes(result.stdout)
    if result.returncode != 1 or b"some entries or attributes were not restored" not in result.stdout:
        raise RuntimeError("unexpected Linux BoltLite metadata result; evidence: " + str(out))
    if not (lite_target / "SRC.TXT").is_file() or not (lite_target / "NEW.TXT").is_file():
        raise RuntimeError("Linux BoltLite extraction/append mismatch; evidence: " + str(out))
    check_manifest_file(lite_target / "SRC.TXT", "src")
    check_manifest_file(lite_target / "NEW.TXT", "added")
    deleted_target = out / "linux-delete"
    deleted_target.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(deleted_target), str(drive / "DELETE.BLT")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-delete.log").write_bytes(result.stdout)
    if (result.returncode != 1 or b"some entries or attributes were not restored" not in result.stdout or
            not (deleted_target / "SRC.TXT").is_file() or
            (deleted_target / "NEW.TXT").exists()):
        raise RuntimeError("delete result was incorrect; evidence: " + str(out))
    check_manifest_file(deleted_target / "SRC.TXT", "src")
    if args.export_amiga_archive:
        shutil.copyfile(drive / "AMIGA.BLT", out / "amiga-created.blt")
    info = subprocess.run([str(linux), "i", str(drive / "LITE.BLT")],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True).stdout
    if b"BoltLite" not in info or (drive / "LITE.BLT").stat().st_size >= len(data) + len(added):
        raise RuntimeError("profile did not exercise BoltLite; evidence: " + str(out))
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                     text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"],
                                         cwd=ROOT, text=True).strip())
    try:
        current_locale = locale.setlocale(locale.LC_ALL, None)
    except locale.Error:
        current_locale = "unknown"
    manifest = [
        "result=PASS",
        "backend=FS-UAE host directory",
        "depth=" + str(args.depth),
        "guest_stack_bytes=" + str(args.stack),
        "guest_chip_memory_kib=" + str(args.chip_memory),
        "guest_fast_memory_kib=" + str(args.fast_memory),
        "codec_block=" + args.codec_block,
        "expect_fluff_refusal=" + str(args.expect_fluff_refusal).lower(),
        "commit=" + commit,
        "worktree_dirty=" + str(dirty).lower(),
        "host=" + platform.platform(),
        "locale=" + current_locale,
        "timezone=" + ",".join(time.tzname),
        "fs_uae=" + command_version([args.fs_uae, "--version"]),
        "amiga_compiler=" + command_version([args.amiga_cc, "--version"]),
        "amiga_cli_build_flags=-noixemul -Wall -Wextra -Wno-unused-function -O2",
        "metadata_probe_flags=-noixemul -O2",
        "amiga_cli=" + str(args.amiga_cli.resolve()),
        "amiga_cli_sha256=" + hashlib.sha256(args.amiga_cli.read_bytes()).hexdigest(),
        "linux_cli=" + str(linux),
        "linux_cli_sha256=" + hashlib.sha256(linux.read_bytes()).hexdigest(),
        "rom_sha256=" + hashlib.sha256(rom.read_bytes()).hexdigest(),
        "workbench_sha256=" + hashlib.sha256(wb.read_bytes()).hexdigest(),
        "payload_sha256=" + hashlib.sha256(data).hexdigest(),
    ]
    (out / "manifest.txt").write_text("\n".join(manifest) + "\n")
    if args.expect_fluff_refusal:
        summary = "profile exchange, staged edits, three codec round trips, and clean Fluff refusal"
    else:
        summary = "profile exchange, staged edits, and four codec round trips"
    print("PASS: Amiga/Linux %s (%s blocks, %d KiB chip, %d KiB fast); evidence: %s" %
          (summary, args.codec_block, args.chip_memory, args.fast_memory, out))


if __name__ == "__main__":
    main()
