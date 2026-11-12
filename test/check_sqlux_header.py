#!/usr/bin/env python3
"""QDOS file headers (type and dataspace) through the QL frontend in sQLux.

An executable's type and dataspace live in its QDOS file header, not in its data.
sQLux keeps them as a 30-byte "]!QDOS File Header" prefix on the host file.  Before,
the QL tool could not archive such a file at all (it sized the file with fseek/ftell,
which counts the host prefix the stream never returns), and would have lost the
header if it had.  Now the header travels in a small BP_OS_QL metadata entry and is
put back on extraction.  Run from the repository root with sQLux and XTC68 built.
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "test"))
import check_sqlux_interop as harness  # noqa: E402

passes = 0
failures = []


def check(ok, name, detail=""):
    global passes
    if ok:
        passes += 1
        print("  PASS  " + name)
    else:
        failures.append(name)
        print("  FAIL  " + name + ("  [" + detail + "]" if detail else ""))


def host_header(path):
    """(type, dataspace, header_length) of a sQLux host file, or None."""
    data = path.read_bytes()
    if not data.startswith(b"]!QDOS File Header"):
        return None
    words = int.from_bytes(data[18:20], "big")
    return data[21], int.from_bytes(data[22:26], "big"), words * 2


def wait_for(path, text):
    return lambda: path.exists() and text in path.read_text(errors="replace")


def run_basic(sqlux, rom, shared, basic, done):
    """Run a SuperBASIC line in sQLux until done() is true."""
    emulator = ["xvfb-run", "-a", str(sqlux), "--romdir", str(rom.parent),
                "--sysrom", rom.name, "--ramtop", "1024",
                "--device", "WIN1,%s,qdos-like" % shared, "--boot_cmd", basic]
    env = dict(os.environ, TZ="UTC")
    process = subprocess.Popen(emulator, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True,
                               start_new_session=True, env=env)
    import signal
    import time
    deadline = time.monotonic() + 40
    stable = None
    while time.monotonic() < deadline:
        if done():
            stable = stable or time.monotonic()
            if time.monotonic() - stable > 0.6:
                break
        time.sleep(0.1)
    os.killpg(process.pid, signal.SIGTERM)
    process.communicate(timeout=5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sqlux", required=True, type=pathlib.Path)
    ap.add_argument("--rom", required=True, type=pathlib.Path)
    ap.add_argument("--ql-binary", required=True, type=pathlib.Path)
    ap.add_argument("--linux-binary", required=True, type=pathlib.Path)
    ap.add_argument("--cc", default="cc")
    args = ap.parse_args()
    sqlux, rom = args.sqlux.resolve(), args.rom.resolve()
    linux = args.linux_binary.resolve()
    work = pathlib.Path(tempfile.mkdtemp(prefix="bp-qlhdr-"))
    shared = work / "shared"
    shared.mkdir()
    try:
        shutil.copy(args.ql_binary, shared / "BOLTPACK")
        (shared / "plainfile").write_bytes(b"plain content\n")

        print("\n--- executables with QDOS headers ---")
        # SEXEC writes a type-1 (executable) file with the dataspace given.
        run_basic(sqlux, rom, shared,
                  "a=RESPR(64):SEXEC win1_exeone,a,64,5000:"
                  "SEXEC win1_exetwo,a,64,12345",
                  lambda: (shared / "exetwo").exists() and
                  (shared / "exetwo").stat().st_size == 94)
        check(host_header(shared / "exeone") == (1, 5000, 30) and
              host_header(shared / "exetwo") == (1, 12345, 30),
              "the emulator made two files with different headers")
        original = {n: (shared / n).read_bytes() for n in ("exeone", "exetwo")}

        log = shared / "log"
        run_basic(sqlux, rom, shared,
                  'EXEC_W win1_BOLTPACK;"c -0 win1_arc win1_exeone win1_exetwo '
                  'win1_plainfile >win1_log 2>win1_err"',
                  wait_for(log, "items archived"))
        err = (shared / "err").read_text(errors="replace") if (shared / "err").exists() else ""
        check("3 items archived" in log.read_text() and err == "",
              "the QL tool archives files that have headers", err + log.read_text())
        listing = subprocess.run([linux, "l", shared / "arc"], text=True,
                                 capture_output=True).stdout
        rows = [l.split() for l in listing.splitlines() if len(l.split()) > 3]
        data = {r[3]: r[0] for r in rows if r[2] == "DATA"}
        metas = [r for r in rows if r[2] == "META"]
        check(data.get("exeone") == "64" and data.get("exetwo") == "64" and
              len(metas) == 2 and all(r[0] == "5" for r in metas),
              "each executable has 64 data bytes and a 5-byte header record", listing)
        arc = (shared / "arc").read_bytes()
        check((bytes([1]) + (5000).to_bytes(4, "little")) in arc and
              (bytes([1]) + (12345).to_bytes(4, "little")) in arc,
              "the records hold the type and the dataspace (little-endian)")
        check(listing.count("META") == 2, "the plain file has no header record", listing)

        print("\n--- extraction puts the headers back ---")
        for n in ("log", "err"):
            if (shared / n).exists():
                (shared / n).unlink()
        run_basic(sqlux, rom, shared,
                  'EXEC_W win1_BOLTPACK;"x -o win1_out win1_arc >win1_log 2>win1_err"',
                  wait_for(log, "files, 0 dirs"))
        err = (shared / "err").read_text(errors="replace") if (shared / "err").exists() else ""
        check(err == "", "extraction reports nothing wrong", err)
        for n in ("exeone", "exetwo"):
            got = shared / ("out_" + n)
            check(got.exists() and got.read_bytes() == original[n],
                  "%s comes back byte-identical, header included" % n,
                  "" if not got.exists() else str(host_header(got)))
        check(host_header(shared / "out_plainfile") is None and
              (shared / "out_plainfile").read_bytes() == b"plain content\n",
              "a file without a header gets none")

        print("\n--- another reader and another writer ---")
        out = work / "linuxout"
        r = subprocess.run([linux, "x", "-o", out, shared / "arc"], text=True,
                           capture_output=True)
        check((out / "exeone").read_bytes() == original["exeone"][30:] and r.returncode == 1,
              "the Linux CLI extracts the 64 data bytes and reports the header as unsupported",
              r.stdout + r.stderr)

        # an archive made elsewhere carrying a QL header record for a file
        objs = []
        for name in ("archiver", "compress", "boltlite", "fluff", "bp_codepage", "boltlz"):
            o = work / (name + ".o")
            subprocess.run([args.cc, "-O2", "-Isrc/core", "-c", "-o", o,
                            ROOT / "src/core" / (name + ".c")], cwd=ROOT, check=True)
            objs.append(str(o))
        helper = work / "make_attr_archive"
        subprocess.run([args.cc, "-O2", "-Isrc/core", "-o", helper,
                        ROOT / "test/macosx_shim/make_attr_archive.c"] + objs,
                       cwd=ROOT, check=True)
        (work / "payload").write_bytes(bytes([1]) + (7777).to_bytes(4, "little"))
        subprocess.run([helper, shared / "madeelsewhere", "item", work / "payload",
                        "file", "0x1E"], check=True)
        for n in ("log", "err"):
            if (shared / n).exists():
                (shared / n).unlink()
        run_basic(sqlux, rom, shared,
                  'EXEC_W win1_BOLTPACK;"x -o win1_other win1_madeelsewhere '
                  '>win1_log 2>win1_err"', wait_for(log, "files, 0 dirs"))
        got = shared / "other_item"
        check(got.exists() and host_header(got) == (1, 7777, 30) and
              got.read_bytes()[30:] == b"data\n",
              "a header record from another writer is applied", "" if not got.exists()
              else str(host_header(got)))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\nResults: %d passed, %d failed" % (passes, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
