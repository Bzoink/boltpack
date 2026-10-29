#!/usr/bin/env python3
"""Exercise Amiga/Linux exchange and metadata on a disposable native floppy."""
import argparse
import hashlib
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
    p.add_argument("--host-cc", default=os.environ.get("CC", "cc"))
    p.add_argument("--amiga-cc", default="/opt/amiga/bin/m68k-amigaos-gcc")
    p.add_argument("--amiga-cli", type=Path, default=ROOT / "build/amiga/boltpack")
    p.add_argument("--linux-cli", type=Path, default=ROOT / "build/linux/boltpack")
    p.add_argument("--output", type=Path)
    p.add_argument("--timeout", type=int, default=90)
    p.add_argument("--stack", type=int, default=262144,
                   help="guest shell stack in bytes; 0 keeps its default")
    p.add_argument("--chip-memory", type=int, choices=(512, 1024, 2048),
                   default=2048, help="FS-UAE chip memory in KiB")
    p.add_argument("--fast-memory", type=int, choices=(0, 2048, 4096, 8192),
                   default=8192, help="FS-UAE fast memory in KiB")
    p.add_argument("--codec-block", choices=("16k", "32k", "256k"),
                   default="256k", help="block size for native floppy codec round trips")
    p.add_argument("--skip-codecs", action="store_true",
                   help="skip the optional large native-floppy codec corpus")
    p.add_argument("--depth", type=int, default=0,
                   help="add a nested source tree at this depth (0 disables it)")
    p.add_argument("--filesystem", choices=("ofs", "ffs"), default="ofs")
    args = p.parse_args()
    if args.depth < 0 or args.depth > 64:
        p.error("--depth must be between 0 and 64")
    out = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="boltpack-amiga-" + args.filesystem + "-"))
    if args.output:
        out.mkdir(parents=True, exist_ok=False)
    drive = out / "host"
    drive.mkdir()
    home = out / "home"
    (home / "Desktop").mkdir(parents=True)
    rom = args.rom.expanduser().resolve(strict=True)
    wb = args.workbench.expanduser().resolve(strict=True)
    linux = args.linux_cli.resolve(strict=True)
    system_disk, native = out / "workbench.adf", out / "data.adf"
    shutil.copyfile(wb, system_disk)
    shutil.copyfile(args.amiga_cli.resolve(strict=True), drive / "boltpack")
    payload = b"Amiga native filesystem interchange\n" * 100
    codec_data = (bytes((i * 73 + i // 17) & 0xff for i in range(256 * 1024))
                  if not args.skip_codecs else b"")
    extra = b"append on native Amiga disk\n" * 70
    (drive / "SRC.TXT").write_bytes(payload)
    if not args.skip_codecs:
        (drive / "CODEC.DAT").write_bytes(codec_data)
    (drive / "NEW.TXT").write_bytes(extra)
    accent_name = "caf\u00e9.txt"
    (drive / accent_name).write_bytes(extra)
    emoji_name = "face\U0001f600.txt"
    (drive / emoji_name).write_bytes(extra)
    (drive / "TREE" / "NEST" / "EMPTY").mkdir(parents=True)
    (drive / "TREE" / "NEST" / "FILE.TXT").write_bytes(payload)
    deep_payload = b"native Amiga deep tree fixture\n" * 32
    # Keep the cumulative pathname below AmigaDOS's legacy path limit so this
    # exercises traversal depth rather than failing on path length first.
    deep_parts = ["D" for _ in range(args.depth)]
    if args.depth:
        deep_leaf = drive / "DEEP" / Path(*deep_parts) / "LEAF.TXT"
        deep_leaf.parent.mkdir(parents=True)
        deep_leaf.write_bytes(deep_payload)
    (drive / "Foo").mkdir()
    (drive / "foo").mkdir()
    (drive / "Foo" / "A.TXT").write_bytes(payload)
    (drive / "foo" / "B.TXT").write_bytes(extra)
    with (out / "commands.log").open("wb") as log:
        def run(command, cwd=None):
            log.write((repr(command) + "\n").encode()); log.flush()
            subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT, check=True)
        run([args.host_cc, "-std=c89", "-Wall", "-Wextra", "-Wno-unused-function",
             "-I" + str(ROOT / "src/core"),
             str(ROOT / "test/amiga/conflicting_metadata_fixture.c"),
             str(ROOT / "src/core/archiver.c"),
             str(ROOT / "src/core/compress.c"),
             str(ROOT / "src/core/boltlite.c"),
             str(ROOT / "src/core/fluff.c"),
             str(ROOT / "src/core/bp_codepage.c"),
             str(ROOT / "src/core/boltlz.c"), "-lm", "-o",
             str(out / "make-conflicting-metadata")])
        for archive_name, order in (("NATFST.BLT", "native-first"),
                                    ("PORTFST.BLT", "portable-first")):
            archive = drive / archive_name
            run([str(out / "make-conflicting-metadata"), str(archive), order])
            run([str(linux), "t", str(archive)])
        bad_meta_archive = drive / "BADMETA.BLT"
        run([str(out / "make-conflicting-metadata"), str(bad_meta_archive),
             "embedded-nul"])
        run([str(linux), "t", str(bad_meta_archive)])
        run([args.amiga_cc, "-noixemul", "-O2", str(ROOT / "test/amiga/metadata_probe.c"),
             "-o", str(drive / "METAPROBE")])
        run([str(linux), "c", "--interchange-profile-1", "-1", "LINUX.BLT", "SRC.TXT"], drive)
        run([str(linux), "c", "--interchange-profile-1", "-1", "UNICODE.BLT", accent_name], drive)
        run([str(linux), "c", "--interchange-profile-1", "UNREP.BLT", "SRC.TXT", emoji_name], drive)
        run([str(linux), "c", "--interchange-profile-1", "CASE.BLT", "Foo/A.TXT", "foo/B.TXT"], drive)
        run([args.xdftool, str(native), "create"])
        fmt = [args.xdftool, str(native), "format", "BPNATIVE"]
        if args.filesystem == "ffs": fmt.append("ffs")
        run(fmt)
        names_to_copy = ["SRC.TXT", "NEW.TXT", "LINUX.BLT",
                         "NATFST.BLT", "PORTFST.BLT", "BADMETA.BLT"]
        if not args.skip_codecs:
            names_to_copy.append("CODEC.DAT")
        for name in names_to_copy:
            run([args.xdftool, str(native), "write", str(drive / name), name])
        run([args.xdftool, str(native), "write", str(drive / accent_name), accent_name])
        run([args.xdftool, str(native), "write", str(drive / "UNICODE.BLT"), "UNICODE.BLT"])
        for name in ("UNREP.BLT", "CASE.BLT"):
            run([args.xdftool, str(native), "write", str(drive / name), name])
        run([args.xdftool, str(native), "makedir", "OUT"])
        run([args.xdftool, str(native), "makedir", "RESTORE"])
        run([args.xdftool, str(native), "makedir", "UOUT"])
        run([args.xdftool, str(native), "makedir", "BADNAME"])
        run([args.xdftool, str(native), "makedir", "BADCASE"])
        run([args.xdftool, str(native), "makedir", "BADMETA"])
        run([args.xdftool, str(native), "makedir", "TREE"])
        run([args.xdftool, str(native), "makedir", "TREE/NEST"])
        run([args.xdftool, str(native), "makedir", "TREE/NEST/EMPTY"])
        run([args.xdftool, str(native), "write",
             str(drive / "TREE" / "NEST" / "FILE.TXT"), "TREE/NEST/FILE.TXT"])
        if args.depth:
            run([args.xdftool, str(native), "makedir", "DEEP"])
            for level in range(args.depth):
                directory = "/".join(["DEEP"] + deep_parts[:level + 1])
                run([args.xdftool, str(native), "makedir", directory])
            run([args.xdftool, str(native), "write", str(deep_leaf),
                 "/".join(["DEEP"] + deep_parts + ["LEAF.TXT"])])
        run([args.xdftool, str(native), "makedir", "DIRRESTORE"])
        run([args.xdftool, str(native), "makedir", "NATIVEFIRST"])
        run([args.xdftool, str(native), "makedir", "PORTABLEFIRST"])
        if args.depth:
            run([args.xdftool, str(native), "makedir", "DEEPOUT"])
        startup = out / "startup"
        startup.write_text("\n".join([
            "C:SetPatch >NIL: QUIET", "C:Assign T: RAM:"] +
            (["C:Stack %d" % args.stack] if args.stack else []) + ["C:FailAt 100",
            "DH0:METAPROBE stack >DH0:STACK.TXT",
            "DH0:METAPROBE set DF1:SRC.TXT", "C:Echo $RC >DH0:SET.RC",
            "DH0:METAPROBE date DF1:SRC.TXT", "C:Echo $RC >DH0:DATE.RC",
            "DH0:METAPROBE time DF1:SRC.TXT >DH0:TIMEBEF.TXT",
            "DH0:METAPROBE get DF1:SRC.TXT >DH0:BEFORE.TXT",
            "DH0:METAPROBE set DF1:TREE", "C:Echo $RC >DH0:TREESET.RC",
            "DH0:METAPROBE set DF1:TREE/NEST", "C:Echo $RC >DH0:NESTSET.RC",
            "DH0:METAPROBE set DF1:TREE/NEST/EMPTY", "C:Echo $RC >DH0:EMPTYSET.RC",
            "DH0:METAPROBE set DF1:TREE/NEST/FILE.TXT", "C:Echo $RC >DH0:TREEFILESET.RC",
            "DH0:METAPROBE get DF1:TREE >DH0:TREESRC.TXT",
            "DH0:METAPROBE get DF1:TREE/NEST >DH0:NESTSRC.TXT",
            "DH0:METAPROBE get DF1:TREE/NEST/EMPTY >DH0:EMPTYSRC.TXT",
            "DH0:METAPROBE get DF1:TREE/NEST/FILE.TXT >DH0:TREEFILESRC.TXT",
            "DH0:METAPROBE date DF1:TREE", "C:Echo $RC >DH0:TREEDATE.RC",
            "DH0:METAPROBE date DF1:TREE/NEST", "C:Echo $RC >DH0:NESTDATE.RC",
            "DH0:METAPROBE date DF1:TREE/NEST/EMPTY", "C:Echo $RC >DH0:EMPTYDATE.RC",
            "DH0:METAPROBE time DF1:TREE >DH0:TREETIMEBEF.TXT",
            "DH0:METAPROBE time DF1:TREE/NEST >DH0:NESTTIMEBEF.TXT",
            "DH0:METAPROBE time DF1:TREE/NEST/EMPTY >DH0:EMPTYTIMEBEF.TXT",
            *( (["DH0:boltpack c --interchange-profile-1 DF1:DEEP.BLT DF1:DEEP >DH0:DEEPCRT.LOG",
                 "C:Echo $RC >DH0:DEEPCRT.RC",
                 "DH0:boltpack x -o DF1:DEEPOUT DF1:DEEP.BLT >DH0:DEEPEXTRACT.LOG",
                 "C:Echo $RC >DH0:DEEPEXTRACT.RC"] if args.depth else []) ),
            "DH0:boltpack x -o DF1:OUT DF1:LINUX.BLT >DH0:EXTRACT.LOG", "C:Echo $RC >DH0:EXTRACT.RC",
            "DH0:boltpack x -o DF1:NATIVEFIRST DF1:NATFST.BLT >DH0:NATFST.LOG", "C:Echo $RC >DH0:NATFST.RC",
            "DH0:boltpack x -o DF1:PORTABLEFIRST DF1:PORTFST.BLT >DH0:PORTFST.LOG", "C:Echo $RC >DH0:PORTFST.RC",
            "DH0:METAPROBE get DF1:NATIVEFIRST/CONFLICT.TXT >DH0:NATFST.META.TXT",
            "DH0:METAPROBE get DF1:PORTABLEFIRST/CONFLICT.TXT >DH0:PORTFST.META.TXT",
            "DH0:boltpack x -o DF1:BADMETA DF1:BADMETA.BLT >DH0:BADMETA.LOG",
            "C:Echo $RC >DH0:BADMETA.RC",
            "DH0:METAPROBE get DF1:BADMETA/CONFLICT.TXT >DH0:BADMETA.META.TXT",
            *( [] if args.skip_codecs else [
            "DH0:boltpack c -b %s DF1:AMICODEC_LZ.BLT DF1:CODEC.DAT >DH0:CODECLZCRT.LOG" % args.codec_block,
            "C:Echo $RC >DH0:CODECLZCRT.RC",
            "DH0:boltpack c -b %s -F DF1:AMICODEC_FL.BLT DF1:CODEC.DAT >DH0:CODEFLCRT.LOG" % args.codec_block,
            "C:Echo $RC >DH0:CODEFLCRT.RC",
            "DH0:boltpack c -b %s -0 DF1:AMICODEC_ST.BLT DF1:CODEC.DAT >DH0:CODESTCRT.LOG" % args.codec_block,
            "C:Echo $RC >DH0:CODESTCRT.RC",
            "DH0:boltpack c -b %s -1 DF1:AMICODEC_LI.BLT DF1:CODEC.DAT >DH0:CODELICRT.LOG" % args.codec_block,
            "C:Echo $RC >DH0:CODELICRT.RC",
            "DH0:boltpack t DF1:AMICODEC_LZ.BLT >DH0:CODECLZT.LOG", "C:Echo $RC >DH0:CODECLZT.RC",
            "DH0:boltpack t DF1:AMICODEC_FL.BLT >DH0:CODEFLT.LOG", "C:Echo $RC >DH0:CODEFLT.RC",
            "DH0:boltpack t DF1:AMICODEC_ST.BLT >DH0:CODESTT.LOG", "C:Echo $RC >DH0:CODESTT.RC",
            "DH0:boltpack t DF1:AMICODEC_LI.BLT >DH0:CODELIT.LOG", "C:Echo $RC >DH0:CODELIT.RC"] ),
            "DH0:boltpack x -o DF1:UOUT DF1:UNICODE.BLT >DH0:UNICODE.LOG", "C:Echo $RC >DH0:UNICODE.RC",
            "DH0:boltpack c --interchange-profile-1 DF1:ACCENT.BLT DF1:caf\u00e9.txt >DH0:ACCENT.LOG", "C:Echo $RC >DH0:ACCENT.RC",
            "DH0:boltpack x -o DF1:BADNAME DF1:UNREP.BLT >DH0:UNREP.LOG", "C:Echo $RC >DH0:UNREP.RC",
            "DH0:boltpack x -o DF1:BADCASE DF1:CASE.BLT >DH0:CASE.LOG", "C:Echo $RC >DH0:CASE.RC",
            "DH0:boltpack c --interchange-profile-1 DF1:AMIGA.BLT DF1:SRC.TXT >DH0:CREATE.LOG", "C:Echo $RC >DH0:CREATE.RC",
            "DH0:boltpack a --interchange-profile-1 DF1:AMIGA.BLT DF1:NEW.TXT >DH0:APPEND.LOG", "C:Echo $RC >DH0:APPEND.RC",
            "C:Copy DF1:AMIGA.BLT DF1:SNAP.BLT QUIET",
            "DH0:boltpack a --interchange-profile-1 DF1:AMIGA.BLT DF1:SRC.TXT DF1:MISSING.TXT >DH0:FAILADD.LOG",
            "C:Echo $RC >DH0:FAILADD.RC",
            "C:Copy DF1:AMIGA.BLT DF1:FAILADD.BLT QUIET",
            "DH0:boltpack x -o DF1:RESTORE DF1:AMIGA.BLT >DH0:RESTORE.LOG", "C:Echo $RC >DH0:RESTORE.RC",
            "DH0:METAPROBE get DF1:RESTORE/SRC.TXT >DH0:AFTER.TXT",
            "DH0:METAPROBE time DF1:RESTORE/SRC.TXT >DH0:TIMEAFT.TXT",
            "DH0:boltpack c --interchange-profile-1 DF1:TREE.BLT DF1:TREE >DH0:TREECREATE.LOG",
            "C:Echo $RC >DH0:TREECREATE.RC",
            "DH0:boltpack x -o DF1:DIRRESTORE DF1:TREE.BLT >DH0:TREERESTORE.LOG",
            "C:Echo $RC >DH0:TREERESTORE.RC",
            "DH0:METAPROBE get DF1:DIRRESTORE/TREE >DH0:TREEDST.TXT",
            "DH0:METAPROBE get DF1:DIRRESTORE/TREE/NEST >DH0:NESTDST.TXT",
            "DH0:METAPROBE get DF1:DIRRESTORE/TREE/NEST/EMPTY >DH0:EMPTYDST.TXT",
            "DH0:METAPROBE get DF1:DIRRESTORE/TREE/NEST/FILE.TXT >DH0:TREEFILEDST.TXT",
            "DH0:METAPROBE time DF1:DIRRESTORE/TREE >DH0:TREETIMEAFT.TXT",
            "DH0:METAPROBE time DF1:DIRRESTORE/TREE/NEST >DH0:NESTTIMEAFT.TXT",
            "DH0:METAPROBE time DF1:DIRRESTORE/TREE/NEST/EMPTY >DH0:EMPTYTIMEAFT.TXT",
            "DH0:boltpack d DF1:AMIGA.BLT NEW.TXT >DH0:DELETE.LOG", "C:Echo $RC >DH0:DELETE.RC",
            # Amiga floppy handlers defer writes; let them flush before stopping FS-UAE.
            "C:Wait 5", "C:Echo FINISHED >DH0:DONE.TXT", ""]), encoding="latin-1")
        run([args.xdftool, str(system_disk), "delete", "S/Startup-Sequence"])
        run([args.xdftool, str(system_disk), "write", str(startup), "S/Startup-Sequence"])
        env = os.environ.copy()
        env.update(HOME=str(home), LIBGL_ALWAYS_SOFTWARE="1", SDL_AUDIODRIVER="dummy")
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24", args.fs_uae,
               "--amiga_model=A500+", "--kickstart_file=" + str(rom),
               "--floppy_drive_0=" + str(system_disk), "--floppy_drive_1=" + str(native),
               "--hard_drive_0=" + str(drive), "--floppy_drive_speed=800", "--warp_mode=1",
               "--fullscreen=0", "--chip_memory=" + str(args.chip_memory),
               "--fast_memory=" + str(args.fast_memory),
               "--writable_floppy_images=1"]
        log.write((repr(cmd) + "\n").encode()); log.flush()
        guest = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline = time.monotonic() + args.timeout
            while not (drive / "DONE.TXT").is_file():
                if guest.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("FS-UAE stopped or timed out; evidence: " + str(out))
                time.sleep(0.25)
        finally:
            if guest.poll() is None: os.killpg(guest.pid, signal.SIGTERM)
            try: guest.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(guest.pid, signal.SIGKILL); guest.wait()
        for name in ("SET", "DATE", "TREESET", "NESTSET", "EMPTYSET", "TREEFILESET",
                     "TREECREATE", "TREERESTORE", "EXTRACT", "UNICODE", "ACCENT",
                     "CREATE", "APPEND", "RESTORE", "DELETE") + (
                     () if args.skip_codecs else ("CODECLZCRT", "CODEFLCRT", "CODESTCRT",
                         "CODELICRT", "CODECLZT", "CODEFLT", "CODESTT", "CODELIT")):
            if (drive / (name + ".RC")).read_text().strip() != "0":
                raise RuntimeError("guest failed " + name + "; evidence: " + str(out))
        for name in ("TREEDATE", "NESTDATE", "EMPTYDATE"):
            if (drive / (name + ".RC")).read_text().strip() != "0":
                raise RuntimeError("cannot set native directory timestamp: " + name + "; evidence: " + str(out))
        if args.depth:
            for name in ("DEEPCRT", "DEEPEXTRACT"):
                if (drive / (name + ".RC")).read_text().strip() != "0":
                    raise RuntimeError("deep-tree guest failure: " + name + "; evidence: " + str(out))
            deep_output = out / "deep-output.txt"
            run([args.xdftool, "-r", str(native), "read",
                 "/".join(["DEEPOUT", "DEEP"] + deep_parts + ["LEAF.TXT"]),
                 str(deep_output)])
            if deep_output.read_bytes() != deep_payload:
                raise RuntimeError("deep-tree content mismatch; evidence: " + str(out))
        stack_report = (drive / "STACK.TXT").read_text().strip()
        stack_fields = {}
        for field in stack_report.split():
            key, value = field.split("=", 1)
            stack_fields[key] = int(value)
        if set(stack_fields) != {"process_stack_bytes", "cli_default_stack_longwords"}:
            raise RuntimeError("invalid guest stack report %r; evidence: %s" %
                               (stack_report, out))
        if (stack_fields["process_stack_bytes"] <= 0 or
                stack_fields["cli_default_stack_longwords"] <= 0):
            raise RuntimeError("guest stack probe returned an invalid size %r; evidence: %s" %
                               (stack_report, out))
        for name in ("FAILADD", "UNREP", "CASE"):
            if (drive / (name + ".RC")).read_text().strip() == "0":
                raise RuntimeError("native refusal returned success: " + name + "; evidence: " + str(out))
        if (drive / "BADMETA.RC").read_text().strip() == "0":
            raise RuntimeError("embedded-NUL Amiga comment was accepted; evidence: " + str(out))
        malformed_comment = (drive / "BADMETA.META.TXT").read_bytes()
        if not malformed_comment.endswith(b":\n"):
            raise RuntimeError("malformed Amiga comment was partially applied: " +
                               repr(malformed_comment) + "; evidence: " + str(out))
        for name in ("NATFST", "PORTFST"):
            if ((drive / (name + ".RC")).read_text().strip() != "1" or
                    b"[foreign metadata]" not in
                    (drive / (name + ".LOG")).read_bytes()):
                raise RuntimeError("conflicting metadata did not report partial: " + name +
                                   "; evidence: " + str(out))
            metadata = (drive / (name + ".META.TXT")).read_text().strip()
            if not metadata.startswith("64:"):
                raise RuntimeError("portable metadata changed native Amiga protection " +
                                   name + ": " + metadata + "; evidence: " + str(out))
        before = (drive / "BEFORE.TXT").read_bytes()
        if (b"BoltPack archive comment" not in before or not (int(before.split(b":", 1)[0]) & 64) or
                before != (drive / "AFTER.TXT").read_bytes()):
            raise RuntimeError("native file metadata mismatch; evidence: " + str(out))
        if ((drive / "TIMEBEF.TXT").read_text().strip() != "10000:600:0" or
                (drive / "TIMEBEF.TXT").read_bytes() != (drive / "TIMEAFT.TXT").read_bytes()):
            raise RuntimeError("native file timestamp mismatch; evidence: " + str(out))
        for name in ("TREE", "NEST", "EMPTY", "TREEFILE"):
            before_meta = (drive / (name + "SRC.TXT")).read_bytes()
            after_meta = (drive / (name + "DST.TXT")).read_bytes()
            if (b"BoltPack archive comment" not in before_meta or
                    not (int(before_meta.split(b":", 1)[0]) & 64) or
                    before_meta != after_meta):
                raise RuntimeError("native tree metadata mismatch for %s; evidence: %s" %
                                   (name, out))
        for name in ("TREE", "NEST", "EMPTY"):
            before_time = (drive / (name + "TIMEBEF.TXT")).read_text().strip()
            after_time = (drive / (name + "TIMEAFT.TXT")).read_text().strip()
            if before_time != "10000:600:0" or after_time != before_time:
                raise RuntimeError("native directory timestamp mismatch for %s: %s -> %s; evidence: %s" %
                                   (name, before_time, after_time, out))
        for source, dest in (("OUT/SRC.TXT", "linux-to-amiga"),
                             ("RESTORE/SRC.TXT", "amiga-restored"),
                             ("RESTORE/NEW.TXT", "amiga-added"), ("AMIGA.BLT", "amiga.blt"),
                             ("SNAP.BLT", "before-failure.blt"), ("FAILADD.BLT", "after-failure.blt"),
                             ("UOUT/" + accent_name, "unicode-to-amiga"), ("ACCENT.BLT", "accent.blt"),
                             ("TREE.BLT", "tree.blt"),
                             ("BADMETA/CONFLICT.TXT", "badmeta-payload"),
                             ("NATIVEFIRST/CONFLICT.TXT", "native-first-conflict"),
                             ("PORTABLEFIRST/CONFLICT.TXT", "portable-first-conflict")):
            run([args.xdftool, "-r", str(native), "read", source, str(out / dest)])
        codec_archives = {
            "BOLTLZ": "AMICODEC_LZ.BLT",
            "FLUFF": "AMICODEC_FL.BLT",
            "STORED": "AMICODEC_ST.BLT",
            "LITE": "AMICODEC_LI.BLT",
        }
        if not args.skip_codecs:
            for codec, guest_name in codec_archives.items():
                archive = out / ("native-codec-" + codec.lower() + ".blt")
                run([args.xdftool, "-r", str(native), "read", guest_name, str(archive)])
        for folder in ("BADNAME", "BADCASE"):
            listing = subprocess.run([args.xdftool, "-r", str(native), "list", folder],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True).stdout
            (out / (folder + ".list")).write_bytes(listing)
            if len(listing.splitlines()) != 1:
                raise RuntimeError("native refusal modified output directory: " + folder)
    if ((out / "linux-to-amiga").read_bytes() != payload or
        (out / "amiga-restored").read_bytes() != payload or (out / "amiga-added").read_bytes() != extra or
        (out / "unicode-to-amiga").read_bytes() != extra or
        (out / "native-first-conflict").read_bytes() !=
        b"Amiga metadata precedence fixture\n" or
        (out / "portable-first-conflict").read_bytes() !=
        b"Amiga metadata precedence fixture\n"):
        raise RuntimeError("native file bytes differ; evidence: " + str(out))
    if (out / "badmeta-payload").read_bytes() != b"Amiga metadata precedence fixture\n":
        raise RuntimeError("malformed metadata damaged verified payload; evidence: " + str(out))
    if (out / "before-failure.blt").read_bytes() != (out / "after-failure.blt").read_bytes():
        raise RuntimeError("failed native append changed archive; evidence: " + str(out))
    if not args.skip_codecs:
        for codec in ("boltlz", "fluff", "stored", "lite"):
            archive = out / ("native-codec-" + codec + ".blt")
            checked = subprocess.run([str(linux), "t", str(archive)],
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (out / ("linux-native-codec-" + codec + "-test.log")).write_bytes(checked.stdout)
            if checked.returncode:
                raise RuntimeError("Linux rejected native %s archive; evidence: %s" % (codec, out))
            target = out / ("linux-native-codec-" + codec)
            target.mkdir()
            extracted = subprocess.run([str(linux), "x", "-o", str(target), str(archive)],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (out / ("linux-native-codec-" + codec + "-extract.log")).write_bytes(extracted.stdout)
            if extracted.returncode != 1 or (target / "CODEC.DAT").read_bytes() != codec_data:
                raise RuntimeError("Linux extraction of native %s archive differed; evidence: %s" % (codec, out))
    tree_target = out / "tree-output"
    tree_target.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(tree_target), str(out / "tree.blt")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-tree-extract.log").write_bytes(result.stdout)
    if (result.returncode != 1 or
            b"some entries or attributes were not restored" not in result.stdout or
            not (tree_target / "TREE" / "NEST" / "EMPTY").is_dir() or
            (tree_target / "TREE" / "NEST" / "FILE.TXT").read_bytes() != payload):
        raise RuntimeError("Linux extraction of native directory archive failed; evidence: " + str(out))
    accent_target = out / "linux-accent"
    accent_target.mkdir()
    accent_result = subprocess.run([str(linux), "x", "-o", str(accent_target), str(out / "accent.blt")],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-accent.log").write_bytes(accent_result.stdout)
    if (accent_result.returncode != 1 or b"some entries or attributes were not restored" not in accent_result.stdout or
            (accent_target / accent_name).read_bytes() != extra):
        raise RuntimeError("Amiga Latin-1 filename transfer failed; evidence: " + str(out))
    result = subprocess.run([str(linux), "l", str(out / "amiga.blt")], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=True)
    (out / "linux-list.log").write_bytes(result.stdout)
    if b"SRC.TXT" not in result.stdout or b"NEW.TXT" in result.stdout:
        raise RuntimeError("native delete result incorrect; evidence: " + str(out))
    target = out / "linux-output"
    target.mkdir()
    result = subprocess.run([str(linux), "x", "-o", str(target), str(out / "amiga.blt")],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / "linux-extract.log").write_bytes(result.stdout)
    if (result.returncode != 1 or b"some entries or attributes were not restored" not in result.stdout or
            (target / "SRC.TXT").read_bytes() != payload or (target / "NEW.TXT").exists()):
        raise RuntimeError("Linux extraction of native-created archive failed; evidence: " + str(out))
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
        "backend=FS-UAE Amiga " + args.filesystem.upper() + " floppy",
        "depth=" + str(args.depth),
        "guest_stack_bytes=" + str(args.stack),
        "guest_process_stack_bytes=" + str(stack_fields["process_stack_bytes"]),
        "guest_cli_default_stack_longwords=" + str(
            stack_fields["cli_default_stack_longwords"]),
        "guest_cli_default_stack_bytes=" + str(
            stack_fields["cli_default_stack_longwords"] * 4),
        "chip_memory_kib=" + str(args.chip_memory),
        "fast_memory_kib=" + str(args.fast_memory),
        "codec_block=" + ("skipped" if args.skip_codecs else args.codec_block),
        "codec_fixture_bytes=" + str(len(codec_data)),
        "directory_mtime_roundtrip=three nested directory timestamps verified",
        "native_portable_metadata_precedence=" + args.filesystem.upper() +
        ": native protection preserved in both record orders; unsupported portable FAT metadata reported partial",
        "commit=" + commit,
        "worktree_dirty=" + str(dirty).lower(),
        "host=" + platform.platform(),
        "locale=" + current_locale,
        "timezone=" + ",".join(time.tzname),
        "fs_uae=" + command_version([args.fs_uae, "--version"]),
        "amiga_compiler=" + command_version([args.amiga_cc, "--version"]),
        "metadata_probe_flags=-noixemul -O2",
        "amiga_cli=" + str(args.amiga_cli.resolve()),
        "amiga_cli_sha256=" + hashlib.sha256(args.amiga_cli.read_bytes()).hexdigest(),
        "linux_cli=" + str(linux),
        "linux_cli_sha256=" + hashlib.sha256(linux.read_bytes()).hexdigest(),
        "rom_sha256=" + hashlib.sha256(rom.read_bytes()).hexdigest(),
        "workbench_sha256=" + hashlib.sha256(wb.read_bytes()).hexdigest(),
    ]
    (out / "manifest.txt").write_text("\n".join(manifest) + "\n")
    print("PASS: native Amiga " + args.filesystem.upper() + " interchange, edits and file metadata; evidence:", out)


if __name__ == "__main__":
    main()
