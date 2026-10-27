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
DOS = ROOT / "build/dos16/bpmini.exe"
LINUX = ROOT / "build/linux/boltpack"


def main():
    with tempfile.TemporaryDirectory(prefix="bp_dos16_") as directory:
        root = Path(directory)
        shutil.copy(DOS, root / "BPMINI.EXE")
        (root / "INPUT.TXT").write_bytes(b"DOS format six\n")
        os.utime(root / "INPUT.TXT", (951786000, 951786000))
        (root / "OUT").mkdir()
        (root / "SRC" / "SUB").mkdir(parents=True)
        (root / "SRC" / "SUB" / "A.TXT").write_bytes(b"nested\n")
        (root / "SRC" / "B.TXT").write_bytes(b"sibling\n")
        os.utime(root / "SRC" / "SUB", (951786000, 951786000))
        os.utime(root / "SRC", (951786000, 951786000))
        (root / "TREEOUT").mkdir()
        env = os.environ.copy()
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", TZ="UTC")
        commands = [
            f"mount c {directory}", "c:", "set BP_TZ_OFFSET=60",
            "bpmini c TEST.BLT INPUT.TXT > CREATE.TXT",
            "if errorlevel 1 echo FAIL > CREATE.BAD",
            "bpmini x TEST.BLT OUT > EXTRACT.TXT",
            "if errorlevel 1 echo PARTIAL > EXTRACT.BAD",
            "bpmini c TREE.BLT SRC > TREE.TXT",
            "if errorlevel 1 echo FAIL > TREE.BAD",
            "bpmini x TREE.BLT TREEOUT > TREEX.TXT",
            "if errorlevel 1 echo PARTIAL > TREEX.BAD",
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
        assert marked("UNKNOWN.BAD"), "missing timezone was not reported"
        assert not marked("TREE.BAD"), (root / "TREE.TXT").read_text(errors="replace")
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
        output = root / "LINUX" / "INPUT.TXT"
        assert output.read_bytes() == b"DOS format six\n", "Linux bytes differ"
        assert int(output.stat().st_mtime) == 951782400, "UTC offset was not applied"

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
            "if errorlevel 1 echo FAIL > BROKEN.BAD", "exit",
        ]
        subprocess.run(["dosbox", "-c", broken_commands[0], "-c", broken_commands[1],
                        "-c", broken_commands[2], "-c", broken_commands[3],
                        "-c", broken_commands[4]], env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=30, check=True)
        assert marked("BROKEN.BAD"), "corrupt DOS extraction was reported as success"
        assert not (root / "BADOUT" / "INPUT.TXT").exists(), \
            "failed DOS16 extraction left a partial destination"
        print("DOS16/DOSBox to Linux interoperability checks passed")


if __name__ == "__main__":
    main()
