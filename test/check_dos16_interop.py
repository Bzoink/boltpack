#!/usr/bin/env python3
"""Exercise DOS16 archive creation and extraction under DOSBox.

Build with `make dos16` and `make CC=gcc modern` first.
"""
import os
from pathlib import Path
import shutil
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
        env = os.environ.copy()
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", TZ="UTC")
        commands = [
            f"mount c {directory}", "c:", "set BP_TZ_OFFSET=60",
            "bpmini c TEST.BLT INPUT.TXT > CREATE.TXT",
            "if errorlevel 1 echo FAIL > CREATE.BAD",
            "bpmini x TEST.BLT OUT > EXTRACT.TXT",
            "if errorlevel 1 echo PARTIAL > EXTRACT.BAD",
            "set BP_TZ_OFFSET=",
            "bpmini c UNKNOWN.BLT INPUT.TXT > UNKNOWN.TXT",
            "if errorlevel 1 echo PARTIAL > UNKNOWN.BAD",
            "exit",
        ]
        args = ["dosbox"]
        for command in commands:
            args.extend(("-c", command))
        result = subprocess.run(args, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=30, check=True)
        def marked(name):
            path = root / name
            return path.exists() and bool(path.read_text().strip())

        assert not marked("CREATE.BAD"), (root / "CREATE.TXT").read_text(errors="replace")
        assert marked("UNKNOWN.BAD"), "missing timezone was not reported"
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
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        output = root / "LINUX" / "INPUT.TXT"
        assert output.read_bytes() == b"DOS format six\n", "Linux bytes differ"
        assert int(output.stat().st_mtime) == 951782400, "UTC offset was not applied"
        print("DOS16/DOSBox to Linux interoperability checks passed")


if __name__ == "__main__":
    main()
