#!/usr/bin/env python3
"""Exercise DOS16 archive creation and extraction under DOSBox.

Build with `make dos16` and `make CC=gcc modern` first.
"""
import os
from pathlib import Path
import hashlib
import json
import re
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
        writer_probe_path = os.environ.get("BP_DOS16_WRITER_PROBE")
        if writer_probe_path:
            shutil.copy(Path(writer_probe_path), root / "WRITMAX.EXE")
        limit_fixture = ROOT / "build/unit-test-dos16/LIMIT.BLT"
        if not limit_fixture.is_file():
            raise RuntimeError("DOS16 resource test requires unit-test-dos16 LIMIT.BLT")
        shutil.copy2(limit_fixture, root / "LIMIT.BLT")
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
        too_deep_parts = ["D"] * 34
        too_deep_source = root / "DEEPTOO" / Path(*too_deep_parts) / "LEAF.TXT"
        too_deep_source.parent.mkdir(parents=True)
        too_deep_source.write_bytes(b"must be rejected before stack exhaustion\n")
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
        # Exercise deferred directory metadata allocation near the DOS16
        # projected-name index limit. The source root, 126 empty child
        # directories, and one leaf consume 128 projected names.
        queue_source = root / "QUEUESRC"
        queue_source.mkdir()
        queue_names = ["D%03d" % index for index in range(126)]
        for name in queue_names:
            (queue_source / name).mkdir()
        queue_payload = b"DOS16 deferred directory queue fixture\n"
        (queue_source / queue_names[0] / "LEAF.TXT").write_bytes(queue_payload)
        (root / "QUEUEOUT").mkdir()
        subprocess.run([str(LINUX), "c", "CASE.BLT", "CASE"], cwd=root,
                       capture_output=True, check=True)
        subprocess.run([str(LINUX), "c", "LONG.BLT", "LONGSRC"], cwd=root,
                       capture_output=True, check=True)
        subprocess.run([str(LINUX), "c", "BADCHAR.BLT", "CHARSRC"], cwd=root,
                       capture_output=True, check=True)
        subprocess.run([str(LINUX), "c", "--store", "OEM.BLT", "OEM_SRC"], cwd=root,
                       capture_output=True, check=True)
        subprocess.run([str(LINUX), "c", "QUEUE.BLT", "QUEUESRC"], cwd=root,
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
            "bpmini c DEEPBAD.BLT DEEPTOO > DEEPBAD.TXT",
            "if errorlevel 1 echo FAIL > DEEPBAD.BAD",
            "bpmini t LIMIT.BLT > LIMTEST.TXT",
            "if errorlevel 1 echo FAIL > LIMTEST.BAD",
            "if exist WRITMAX.EXE WRITMAX.EXE > WRITMAX.TXT",
            "if errorlevel 1 echo FAIL > WRITMAX.BAD",
            "bpmini c TOO-DEEP.BLT DEEP > TOO-DEEP.TXT",
            "if errorlevel 1 echo FAIL > TOO-DEEP.BAD",
            "bpmini x TREE.BLT TREEOUT > TREEX.TXT",
            "if errorlevel 1 echo PARTIAL > TREEX.BAD",
            "bpmini x QUEUE.BLT QUEUEOUT > QUEUEOUT.TXT",
            "if errorlevel 1 echo PARTIAL > QUEUEOUT.BAD",
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
                                stderr=subprocess.PIPE,
                                timeout=120 if writer_probe_path else 30,
                                check=True)
        def marked(name):
            path = root / name
            return path.exists() and bool(path.read_text().strip())

        assert not marked("CREATE.BAD"), (root / "CREATE.TXT").read_text(errors="replace")
        assert not marked("REPLACE1.BAD"), \
            (root / "REPLACE1.TXT").read_text(errors="replace")
        assert not marked("REPLACE2.BAD"), \
            (root / "REPLACE2.TXT").read_text(errors="replace")
        assert marked("UNKNOWN.BAD"), "missing timezone was not reported"
        assert not marked("LIMTEST.BAD"), \
            (root / "LIMTEST.TXT").read_text(errors="replace")
        writer_probe_values = None
        writer_record = None
        if writer_probe_path:
            assert not marked("WRITMAX.BAD"), \
                (root / "WRITMAX.TXT").read_text(errors="replace")
            writer_text = (root / "WRITMAX.TXT").read_text(errors="replace")
            if "PASS: writer_entries=1213 data_bytes=32768 method=boltlite" not in writer_text:
                raise AssertionError("DOS16 maximum-writer fixture did not complete: " +
                                     writer_text)
            writer_match = re.search(
                r"BP_ALLOC_SUCCESSFUL_CALLS=(\d+) REQUESTED_BYTES=(\d+) "
                r"USABLE_BYTES=(\d+) MAX_REQUEST=(\d+) MAX_USABLE=(\d+) "
                r"MAX_BLOCK_SLACK=(\d+) PEAK_LIVE_USABLE=(\d+) "
                r"LIVE_USABLE_AT_EXIT=(\d+) INVALID_SIZES=(\d+)", writer_text)
            if not writer_match:
                raise AssertionError("DOS16 maximum-writer probe produced no allocation record: " +
                                     writer_text)
            writer_probe_values = tuple(int(value) for value in writer_match.groups())
            if writer_probe_values[8] != 0:
                raise AssertionError("DOS16 writer _msize reported an invalid usable size")
            if writer_probe_values[3] < 65502 or writer_probe_values[6] < writer_probe_values[3]:
                raise AssertionError("DOS16 writer probe did not account for the maximum entry table")
            writer_archive = root / "WRITMAX.BLT"
            writer_test = subprocess.run([str(LINUX), "t", str(writer_archive)],
                                         capture_output=True)
            if writer_test.returncode != 0:
                raise AssertionError("Linux rejected DOS16 maximum-writer archive: " +
                                     writer_test.stderr.decode(errors="replace"))
            retained_archive = Path(os.environ.get(
                "BP_DOS16_WRITER_ARCHIVE", ROOT / "build/dos16-stack/WRITMAX.BLT"))
            retained_archive.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(writer_archive, retained_archive)
            sha256 = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
            revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                      capture_output=True, text=True).stdout.strip()
            worktree = subprocess.run(["git", "status", "--short"], cwd=ROOT,
                                      capture_output=True, text=True).stdout.splitlines()
            writer_record = {
                "purpose": "DOS16 maximum writer-table and BoltLite allocation measurement",
                "command": "make CC=gcc dos16-stack-interoperability-test",
                "dosbox": (result.stdout.decode(errors="replace").splitlines()[0]
                           if result.stdout else "unknown"),
                "compiler": "Open Watcom 2.0 beta (2026-10-14)",
                "revision": revision,
                "worktree_status": worktree,
                "writer_entries": 1213,
                "directory_entries": 1212,
                "data_bytes": 32768,
                "method": "BoltLite",
                "allocation_fields": ["successful_calls", "requested_bytes", "usable_bytes",
                                      "max_request", "max_usable", "max_block_slack",
                                      "peak_live_usable", "live_usable_at_exit", "invalid_sizes"],
                "allocation_values": list(writer_probe_values),
                "archive_linux_integrity": "pass",
                "archive_path": str(retained_archive),
                "archive_sha256": sha256(retained_archive),
                "writer_executable_sha256": sha256(writer_probe_path),
                "runner_sha256": sha256(__file__),
                "fixture_source_sha256": sha256(ROOT / "test/dos16_writer_resource16.c"),
            }
            record_path = retained_archive.with_name("writer-resource-run.json")
            record_path.write_text(json.dumps(writer_record, indent=2) + "\n",
                                   encoding="utf-8")
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
        # Directory times can be unrepresentable on the DOSBox host mount, so
        # accept the documented partial status while requiring all output.
        if marked("QUEUEOUT.BAD"):
            assert (root / "QUEUEOUT.BAD").read_text().strip() == "PARTIAL", \
                (root / "QUEUEOUT.TXT").read_text(errors="replace")
        observed_queue_dirs = sorted(path.name for path in
                                     (root / "QUEUEOUT" / "QUEUESRC").iterdir())
        assert observed_queue_dirs == queue_names, \
            "DOS16 extraction missed or renamed deferred-metadata directories"
        assert ((root / "QUEUEOUT" / "QUEUESRC" / queue_names[0] /
                 "LEAF.TXT").read_bytes() == queue_payload), \
            "DOS16 queue fixture payload differs"
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
            samples = []
            heap_samples = []
            allocation_samples = []
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
                allocation_values = re.findall(
                    r"BP_ALLOC_SUCCESSFUL_CALLS=(\d+) REQUESTED_BYTES=(\d+) "
                    r"USABLE_BYTES=(\d+) MAX_REQUEST=(\d+) MAX_USABLE=(\d+) "
                    r"MAX_BLOCK_SLACK=(\d+) PEAK_LIVE_USABLE=(\d+) "
                    r"LIVE_USABLE_AT_EXIT=(\d+) INVALID_SIZES=(\d+)", text)
                if allocation_values:
                    allocation_samples.append((path.name,
                        tuple(int(value) for value in allocation_values[-1])))
            if not samples:
                raise AssertionError("instrumented DOS16 binary produced no resource samples")
            if not heap_samples:
                raise AssertionError("instrumented DOS16 binary produced no DOS heap samples")
            if not allocation_samples:
                raise AssertionError("instrumented DOS16 binary produced no allocator samples")
            if any(values[8] for _, values in allocation_samples):
                raise AssertionError("Watcom _msize returned a block smaller than its request")
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
                              "\nminimum=%d\n\n" % min(value for _, value in heap_samples) +
                              "DOS16 Watcom allocation usable block observations\n" +
                              "command successful_calls requested_bytes usable_bytes "
                              "max_request max_usable max_slack peak_live_usable "
                              "live_at_exit invalid_sizes\n" +
                              "\n".join("%s %s" % (name, " ".join(map(str, values)))
                                         for name, values in allocation_samples) +
                              "\nmax_single_request=%d\nmax_single_usable=%d\n"
                              "max_single_block_slack=%d\npeak_live_usable=%d\n" % (
                                  max(values[3] for _, values in allocation_samples),
                                  max(values[4] for _, values in allocation_samples),
                                  max(values[5] for _, values in allocation_samples),
                                  max(values[6] for _, values in allocation_samples)))
            print("DOS16 stack probe minimum available bytes:",
                  min(value for _, (value, _, _) in samples), "report:", report)
            print("DOS16 DOS-manager largest-block minimum bytes:",
                  min(value for _, value in heap_samples))
            print("DOS16 Watcom max requested/usable/slack bytes:",
                  max(values[3] for _, values in allocation_samples),
                  max(values[4] for _, values in allocation_samples),
                  max(values[5] for _, values in allocation_samples))
            print("DOS16 Watcom peak tracked live usable bytes:",
                  max(values[6] for _, values in allocation_samples))
            if writer_probe_values is not None:
                print("DOS16 maximum writer table + BoltLite peak live usable bytes:",
                      writer_probe_values[6], "largest request:", writer_probe_values[3])
                print("DOS16 writer evidence:",
                      ROOT / "build/dos16-stack/writer-resource-run.json")
        assert not marked("DEEP.BAD"), (root / "DEEP.TXT").read_text(errors="replace")
        assert marked("DEEPBAD.BAD"), "DOS16 accepted a tree beyond the stack-depth bound"
        assert not (root / "DEEPBAD.BLT").exists(), \
            "DOS16 published an archive for a rejected over-depth source"
        assert not list(root.glob("BPK*.TMP")), \
            "DOS16 left an archive stage after rejecting an over-depth source"
        print("DOS16/DOSBox to Linux interoperability checks passed")


if __name__ == "__main__":
    main()
