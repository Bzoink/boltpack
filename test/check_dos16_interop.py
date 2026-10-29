#!/usr/bin/env python3
"""Exercise DOS16 archive creation and extraction under DOSBox.

Build with `make dos16` and `make CC=gcc modern` first.
"""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
DOS = Path(os.environ.get("BP_DOS16_EXE", ROOT / "build/dos16/bpmini.exe"))
LINUX = ROOT / "build/linux/boltpack"


def main():
    with tempfile.TemporaryDirectory(prefix="bp_dos16_") as directory:
        root = Path(directory)
        shutil.copy(DOS, root / "BPMINI.EXE")
        (root / "INPUT.TXT").write_bytes(b"DOS format six\n")
        os.utime(root / "INPUT.TXT", (951786000, 951786000))
        (root / "OTHER.TXT").write_bytes(b"replacement archive\n")
        (root / "OUT").mkdir()
        (root / "SRC" / "SUB").mkdir(parents=True)
        (root / "SRC" / "SUB" / "A.TXT").write_bytes(b"nested\n")
        (root / "SRC" / "B.TXT").write_bytes(b"sibling\n")
        os.utime(root / "SRC" / "SUB", (951786000, 951786000))
        os.utime(root / "SRC", (951786000, 951786000))
        # Exercise the recursive DOS16 source walker near the projected-index
        # key budget while keeping every component inside the 8.3 profile.
        deep_parts = ["D"] * 33
        deep_source = root / "DEEP" / Path(*deep_parts) / "LEAF.TXT"
        deep_source.parent.mkdir(parents=True)
        deep_payload = b"DOS16 deep traversal stack fixture\n"
        deep_source.write_bytes(deep_payload)
        (root / "TREEOUT").mkdir()
        (root / "CASE" / "Dir").mkdir(parents=True)
        (root / "CASE" / "dir").mkdir()
        (root / "CASE" / "Dir" / "ONE.TXT").write_bytes(b"upper tree\n")
        (root / "CASE" / "dir" / "TWO.TXT").write_bytes(b"lower tree\n")
        (root / "CASEOUT").mkdir()
        (root / "LONGSRC").mkdir()
        (root / "LONGSRC" / "LongerName.txt").write_bytes(b"long DOS name\n")
        (root / "LONGOUT").mkdir()
        (root / "CHARSRC").mkdir()
        (root / "CHARSRC" / "Bad?.txt").write_bytes(b"invalid DOS character\n")
        (root / "CHAROUT").mkdir()
        (root / "OEM_SRC").mkdir()
        (root / "OEM_SRC" / "café.txt").write_bytes(b"CP437 filename\n")
        subprocess.run([str(LINUX), "c", "CASE.BLT", "CASE"], cwd=root,
                       capture_output=True, check=True)
        subprocess.run([str(LINUX), "c", "LONG.BLT", "LONGSRC"], cwd=root,
                       capture_output=True, check=True)
        subprocess.run([str(LINUX), "c", "BADCHAR.BLT", "CHARSRC"], cwd=root,
                       capture_output=True, check=True)
        subprocess.run([str(LINUX), "c", "--store", "OEM.BLT", "OEM_SRC"], cwd=root,
                       capture_output=True, check=True)
        env = os.environ.copy()
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", TZ="UTC")
        commands = [
            f"mount c {directory}", "c:", "set BP_TZ_OFFSET=60",
            "bpmini c TEST.BLT INPUT.TXT > CREATE.TXT",
            "if errorlevel 1 echo FAIL > CREATE.BAD",
            "bpmini c REPLACE.BLT INPUT.TXT > REPLACE1.TXT",
            "if errorlevel 1 echo FAIL > REPLACE1.BAD",
            "bpmini c REPLACE.BLT OTHER.TXT > REPLACE2.TXT",
            "if errorlevel 1 echo FAIL > REPLACE2.BAD",
            "bpmini x TEST.BLT OUT > EXTRACT.TXT",
            "if errorlevel 1 echo PARTIAL > EXTRACT.BAD",
            "bpmini c TREE.BLT SRC > TREE.TXT",
            "if errorlevel 1 echo FAIL > TREE.BAD",
            "bpmini c DEEP.BLT DEEP > DEEP.TXT",
            "if errorlevel 1 echo FAIL > DEEP.BAD",
            "bpmini c TOO-DEEP.BLT DEEP > TOO-DEEP.TXT",
            "if errorlevel 1 echo FAIL > TOO-DEEP.BAD",
            "bpmini x TREE.BLT TREEOUT > TREEX.TXT",
            "if errorlevel 1 echo PARTIAL > TREEX.BAD",
            "bpmini x LONG.BLT LONGOUT > LONG.TXT",
            "if errorlevel 1 echo PARTIAL > LONG.BAD",
            "bpmini x BADCHAR.BLT CHAROUT > BADCHAR.TXT",
            "if errorlevel 1 echo PARTIAL > BADCHAR.BAD",
            "bpmini t OEM.BLT > OEMTEST.TXT",
            "if errorlevel 1 echo FAIL > OEMTEST.BAD",
            "bpmini x TEST.BLT LONGROOTX > ROOT.TXT",
            "if errorlevel 1 echo PARTIAL > ROOT.BAD",
            "set BP_TZ_OFFSET=",
            "bpmini c UNKNOWN.BLT INPUT.TXT > UNKNOWN.TXT",
            "if errorlevel 1 echo PARTIAL > UNKNOWN.BAD",
            "exit",
        ]
        # DOSBox limits the number of -c commands; run the test as a batch.
        (root / "CHECK.BAT").write_text("\r\n".join(commands[2:]) + "\r\n")
        args = ["dosbox", "-c", commands[0], "-c", commands[1],
                "-c", "CHECK.BAT", "-c", "exit"]
        result = subprocess.run(args, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=30, check=True)
        def marked(name):
            path = root / name
            return path.exists() and bool(path.read_text().strip())

        assert not marked("CREATE.BAD"), (root / "CREATE.TXT").read_text(errors="replace")
        assert not marked("REPLACE1.BAD"), \
            (root / "REPLACE1.TXT").read_text(errors="replace")
        assert not marked("REPLACE2.BAD"), \
            (root / "REPLACE2.TXT").read_text(errors="replace")
        assert marked("UNKNOWN.BAD"), "missing timezone was not reported"
        assert not marked("TREE.BAD"), (root / "TREE.TXT").read_text(errors="replace")
        assert marked("LONG.BAD"), "DOS16 accepted a filename requiring an 8.3 alias"
        assert list((root / "LONGOUT").iterdir()) == [], \
            "long DOS16 name modified the destination before refusal"
        assert marked("BADCHAR.BAD"), "DOS16 accepted a DOS-invalid filename character"
        assert list((root / "CHAROUT").iterdir()) == [], \
            "invalid DOS16 name modified the destination before refusal"
        assert not marked("OEMTEST.BAD"), (root / "OEMTEST.TXT").read_text(errors="replace")
        assert marked("ROOT.BAD"), "DOS16 accepted an output root outside the 8.3 profile"
        assert not (root / "LONGROOTX").exists(), \
            "DOS16 created an output root that needs long filename support"
        assert (root / "TREEOUT/SRC/SUB/A.TXT").read_bytes() == b"nested\n"
        assert (root / "TREEOUT/SRC/B.TXT").read_bytes() == b"sibling\n"
        extracted = root / "OUT" / "INPUT.TXT"
        assert extracted.exists(), ((root / "EXTRACT.TXT").read_text(errors="replace") +
                                    " output names: " + repr(list((root / "OUT").iterdir())))
        assert extracted.read_bytes() == b"DOS format six\n", "DOS bytes differ"
        # Some DOSBox versions acknowledge _dos_setftime without setting it.
        # In that case the frontend must report partial extraction.
        assert (marked("EXTRACT.BAD") or
                int(extracted.stat().st_mtime) == 951786000), \
            "DOS time was lost without a partial-extraction result"
        result = subprocess.run([str(LINUX), "x", "-o", str(root / "LINUX"),
                                 str(root / "TEST.BLT")], capture_output=True)
        assert result.returncode != 0 and b"not restored" in result.stderr, \
            "Linux must report that portable FAT attributes were skipped"
        listing = subprocess.run([str(LINUX), "l", str(root / "TEST.BLT")],
                                 capture_output=True, check=True)
        assert b"META" in listing.stdout, "DOS did not archive portable attributes"
        tree_listing = subprocess.run([str(LINUX), "l", str(root / "TREE.BLT")],
                                      capture_output=True, check=True)
        assert tree_listing.stdout.count(b"META") >= 4, "directory metadata was not archived"
        deep_listing = subprocess.run([str(LINUX), "l", str(root / "DEEP.BLT")],
                                      capture_output=True)
        if deep_listing.returncode != 0:
            raise AssertionError("DOS16 deep archive rejected by Linux: " +
                                 deep_listing.stderr.decode(errors="replace") +
                                 "\nDOS16 output:\n" +
                                 (root / "DEEP.TXT").read_text(errors="replace") +
                                 "\nDOS16 diagnostics:\n" +
                                 ((root / "DEEP.ERR").read_text(errors="replace")
                                  if (root / "DEEP.ERR").exists() else "(no stderr file)") +
                                 "\nDOSBox output:\n" +
                                 result.stdout.decode(errors="replace") +
                                 result.stderr.decode(errors="replace"))
        assert b"LEAF.TXT" in deep_listing.stdout, "DOS16 deep traversal omitted its leaf"
        deep_output = root / "DEEP-LINUX"
        deep_output.mkdir()
        deep_extract = subprocess.run([str(LINUX), "x", "-o", str(deep_output),
                                       str(root / "DEEP.BLT")], capture_output=True)
        if deep_extract.returncode != 1 or b"not restored" not in deep_extract.stderr:
            raise AssertionError("Linux deep extraction did not report DOS metadata partial: " +
                                 deep_extract.stderr.decode(errors="replace"))
        if (deep_output / "DEEP" / Path(*deep_parts) / "LEAF.TXT").read_bytes() != deep_payload:
            raise AssertionError("DOS16 deep traversal changed the leaf bytes")
        output = root / "LINUX" / "INPUT.TXT"
        assert output.read_bytes() == b"DOS format six\n", "Linux bytes differ"
        assert int(output.stat().st_mtime) == 951782400, "UTC offset was not applied"

        replacement_listing = subprocess.run(
            [str(LINUX), "l", str(root / "REPLACE.BLT")],
            capture_output=True, check=True).stdout
        assert b"OTHER.TXT" in replacement_listing and b"INPUT.TXT" not in replacement_listing, \
            "DOS16 did not publish the replacement archive contents"
        (root / "REPLACEOUT").mkdir()
        subprocess.run([str(LINUX), "x", "-o", str(root / "REPLACEOUT"),
                        str(root / "REPLACE.BLT")], capture_output=True, check=False)
        assert (root / "REPLACEOUT" / "OTHER.TXT").read_bytes() == b"replacement archive\n"
        assert not list(root.glob("BPK*.TMP")), "DOS16 left an archive stage after replacement"
        assert not list(root.glob("BK*.BAK")), "DOS16 left a backup after successful replacement"

        # Flip an encoded byte without updating the block CRC. The mini reader
        # must remove the newly created file after the callback closes it.
        broken = bytearray((root / "TEST.BLT").read_bytes())
        entry_header_size = struct.unpack_from("<H", broken, 32)[0]
        name_len = struct.unpack_from("<H", broken, 34)[0]
        data_offset = 32 + entry_header_size
        assert entry_header_size == 42 + name_len
        assert struct.unpack_from("<I", broken, data_offset)[0] > 0
        payload_offset = data_offset + 12
        table_offset = struct.unpack_from("<Q", broken, len(broken) - 20)[0]
        assert payload_offset < table_offset
        broken[payload_offset] ^= 0x80
        (root / "BROKEN.BLT").write_bytes(broken)
        (root / "BADOUT").mkdir()
        broken_commands = [
            f"mount c {directory}", "c:",
            "bpmini x BROKEN.BLT BADOUT > BROKEN.TXT",
            "if errorlevel 1 echo FAIL > BROKEN.BAD",
            "bpmini x CASE.BLT CASEOUT > CASE.TXT",
            "if errorlevel 1 echo FAIL > CASE.BAD", "exit",
        ]
        (root / "BAD.BAT").write_text("\r\n".join(broken_commands[2:]) + "\r\n")
        subprocess.run(["dosbox", "-c", broken_commands[0], "-c", broken_commands[1],
                        "-c", "BAD.BAT", "-c", "exit"], env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=30, check=True)
        assert marked("BROKEN.BAD"), "corrupt DOS extraction was reported as success"
        assert not (root / "BADOUT" / "INPUT.TXT").exists(), \
            "failed DOS16 extraction left a partial destination"
        assert marked("CASE.BAD"), "case-folded DOS destination collision was not reported"
        assert not (root / "CASEOUT" / "CASE").exists(), \
            "case-folded DOS directory collision modified the destination before refusal"
        if os.environ.get("BP_DOS16_STACK_REPORT"):
            import re
            samples = []
            heap_samples = []
            for path in sorted(root.glob("*.TXT")):
                text = path.read_text(errors="replace")
                values = [(int(value), source, int(line))
                          for value, source, line in re.findall(
                    r"BP_STACK_LOW_WATER=(\d+) FILE=([^ ]+) LINE=(\d+)", text)]
                if values:
                    samples.append((path.name, min(values)))
                heap_values = [int(value) for value in re.findall(
                    r"BP_DOS_LARGEST_BLOCK_LOW_BYTES=(\d+)", text)]
                if heap_values:
                    heap_samples.append((path.name, min(heap_values)))
            if not samples:
                raise AssertionError("instrumented DOS16 binary produced no resource samples")
            if not heap_samples:
                raise AssertionError("instrumented DOS16 binary produced no DOS heap samples")
            report = Path(os.environ.get("BP_DOS16_STACK_REPORT_FILE",
                                        root / "STACKLOW.TXT"))
            report.parent.mkdir(parents=True, exist_ok=True)
            report.write_text("DOS16 instrumented stack low-water bytes\n" +
                              "\n".join("%s=%d at %s:%d" %
                                         (name, value, source, line)
                                         for name, (value, source, line) in samples) +
                              "\nminimum=%d\n\n" %
                              min(value for _, (value, _, _) in samples) +
                              "DOS16 DOS-manager largest-block low-water bytes\n" +
                              "\n".join("%s=%d" % (name, value)
                                         for name, value in heap_samples) +
                              "\nminimum=%d\n" % min(value for _, value in heap_samples))
            print("DOS16 stack probe minimum available bytes:",
                  min(value for _, (value, _, _) in samples), "report:", report)
            print("DOS16 DOS-manager largest-block minimum bytes:",
                  min(value for _, value in heap_samples))
        assert not marked("DEEP.BAD"), (root / "DEEP.TXT").read_text(errors="replace")
        print("DOS16/DOSBox to Linux interoperability checks passed")


if __name__ == "__main__":
    main()
