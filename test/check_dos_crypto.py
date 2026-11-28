#!/usr/bin/env python3
"""Run the strict C89 Ascon (src/core/crypto/bp_ascon_c89.c) as a 16-bit DOS program.

Builds test/dos/ascon_check.c with Open Watcom (compact model, 8086 code, the flags of the
repo's dos16 build) in the 32-bit-limb and the 16-bit-limb variant, and runs each in DOSBox-X
against the NIST SP 800-232 known-answer files (every AEAD record, 129 XOF records).  With
--bench SECONDS it then times Ascon-AEAD128 sealing of 4 KiB with the DOSBox-X CPU set to an
8086 at a fixed cycle count; those numbers are emulator figures, not hardware.

A second program (test/dos/crypto_check.c) runs the S2 side of the crypto layer proper: key
derivation, sealed frames of every kind, commitments, the entropy pool against its Python model,
the frame limits and the budget, and with --bench times bp_frame_seal (hedged nonce included).

usage: python3 test/check_dos_crypto.py [--dosbox-x PATH] [--watcom DIR] [--bench SECONDS] [--cycles N] [--frame BYTES]
"""
import argparse, os, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REF = ROOT / "test" / "reference" / "encryption"
C = ROOT / "src" / "core" / "crypto"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dosbox-x", default="dosbox-x")
    ap.add_argument("--watcom", default="/opt/watcom")
    ap.add_argument("--bench", type=int, default=0, help="seconds of emulated time to time sealing (0 = correctness only)")
    ap.add_argument("--cycles", default="4770", help="DOSBox-X fixed cycles per ms for the bench run (4770 ~ a 4.77 MHz 8088)")
    ap.add_argument("--opt", default="-os", help="Watcom optimisation flag (-os size, -ox speed)")
    ap.add_argument("--frame", type=int, default=4096, help="frame size of the bp_frame_seal timing (bytes, at most 32768)")
    args = ap.parse_args()
    wbin = Path(args.watcom) / "binl64" / "wcl"
    if not wbin.exists() or shutil.which(args.dosbox_x) is None:
        print("SKIPPED: needs Open Watcom (%s) and DOSBox-X (%s)" % (wbin, args.dosbox_x)); return 0
    env = os.environ.copy()
    env.update(WATCOM=args.watcom, INCLUDE=args.watcom + "/h", PATH=str(wbin.parent) + os.pathsep + env["PATH"],
               SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    fails = 0
    with tempfile.TemporaryDirectory(prefix="bpdoscrypto") as tmp:
        d = Path(tmp)
        shutil.copy(REF / "LWC_AEAD_KAT_128_128.txt", d / "KAT.TXT")
        shutil.copy(REF / "LWC_XOF_KAT_128_512_first129.txt", d / "XOF.TXT")
        for name, flag in (("W32", ""), ("W16", "-DBP_ASCON_WORD16")):
            exe = "ASCHK%s.EXE" % name
            cmd = ["wcl", "-q", "-bt=dos", "-mc", "-0", args.opt, "-zm", "-w4", "-we", "-wcd=202", "-DSIZEOF_INT=2", "-DSIZEOF_LONG=4"]
            if flag: cmd.append(flag)
            cmd += ["-I" + str(C), str(ROOT / "test/dos/ascon_check.c"), str(C / "bp_ascon_c89.c"), str(C / "bp_cutil.c"), "-fe=" + str(d / exe)]
            r = subprocess.run(cmd, env=env, cwd=d, capture_output=True, text=True, timeout=300)
            if r.returncode != 0:
                print("BUILD FAILED", name, r.stdout[-500:], r.stderr[-500:]); fails += 1; continue
            for label, cycles, secs in (("correctness", "max", 0), ("bench", "fixed " + args.cycles, args.bench)):
                if label == "bench" and not args.bench: continue
                for f in ("DONE.TXT", "RESULT.TXT"):
                    if (d / f).exists(): (d / f).unlink()
                limit = 900 if secs == 0 else 120 + 4 * secs
                cmdline = [args.dosbox_x, "-nopromptfolder", "-nogui", "-fastlaunch", "-time-limit", str(limit),
                           "-set", "dos lfn=false", "-set", "cpu cycles=" + cycles, "-set", "cpu cputype=8086",
                           "-c", "mount c " + str(d), "-c", "c:",
                           "-c", ("%s KAT.TXT XOF.TXT 0" % exe) if secs == 0 else ("%s - - %d" % (exe, secs)), "-c", "echo done > DONE.TXT", "-c", "exit"]
                p = subprocess.run(cmdline, env=env, timeout=limit + 60, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                cpu_lines = [l for l in p.stdout.decode(errors="replace").splitlines() if "cputype" in l.lower() or "cpu type" in l.lower()]
                if cpu_lines and label == "correctness" and name == "W32": print("   DOSBox-X reports:", cpu_lines[:2])
                res = (d / "RESULT.TXT").read_text(errors="replace") if (d / "RESULT.TXT").exists() else ""
                ok = (d / "DONE.TXT").exists() and "done failures 0" in res and (secs > 0 or ("aead 1089 records, 0 failures" in res and "xof 129 records, 0 failures" in res)) and (secs == 0 or "bench" in res)
                print("%s %s (%s, cycles=%s): %s" % (exe, label, "8086", cycles, "ok" if ok else "FAILED"))
                print("   " + res.strip().replace("\n", "\n   "))
                if not ok:
                    fails += 1
                    print(p.stdout.decode(errors="replace")[-600:])
        # ---- the crypto layer proper (suite S2): vectors from the repository's reference files
        import check_hatari_crypto
        check_hatari_crypto.make_vectors(d / "crypto_vectors.h")
        for name, flag in (("W32", ""), ("W16", "-DBP_ASCON_WORD16")):
            exe = "CRYP%s.EXE" % name
            cmd = ["wcl", "-q", "-bt=dos", "-mc", "-0", args.opt, "-zm", "-w4", "-we", "-wcd=202", "-DSIZEOF_INT=2", "-DSIZEOF_LONG=4", "-DBP_CRYPTO_TESTING"]
            if flag: cmd.append(flag)
            cmd += ["-I" + str(C), "-I" + str(d), str(ROOT / "test/dos/crypto_check.c")] + [str(C / f) for f in ("bp_cutil.c", "bp_crypto.c", "bp_entropy.c", "bp_entropy_os_none.c", "bp_ascon_c89.c")] + ["-fe=" + str(d / exe)]
            r = subprocess.run(cmd, env=env, cwd=d, capture_output=True, text=True, timeout=300)
            if r.returncode != 0:
                print("BUILD FAILED", exe, r.stdout[-800:], r.stderr[-500:]); fails += 1; continue
            for label, cycles, secs in (("correctness", "max", 0), ("bench", "fixed " + args.cycles, args.bench)):
                if label == "bench" and not args.bench: continue
                for f in ("DONE.TXT", "RESULT.TXT"):
                    if (d / f).exists(): (d / f).unlink()
                limit = 900 if secs == 0 else 120 + 8 * secs
                cmdline = [args.dosbox_x, "-nopromptfolder", "-nogui", "-fastlaunch", "-time-limit", str(limit),
                           "-set", "dos lfn=false", "-set", "cpu cycles=" + cycles, "-set", "cpu cputype=8086",
                           "-c", "mount c " + str(d), "-c", "c:", "-c", "%s %d %d" % (exe, secs, args.frame), "-c", "echo done > DONE.TXT", "-c", "exit"]
                p = subprocess.run(cmdline, env=env, timeout=limit + 60, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                res = (d / "RESULT.TXT").read_text(errors="replace") if (d / "RESULT.TXT").exists() else ""
                ok = (d / "DONE.TXT").exists() and "done failures 0" in res and (secs > 0 or ("wrapper S2: 1 derivations, 5 frames, 1 commitments: 0 failures" in res and "pool, limits, budget: 0 failures" in res)) and (secs == 0 or "bench" in res)
                print("%s %s (%s, cycles=%s): %s" % (exe, label, "8086", cycles, "ok" if ok else "FAILED"))
                print("   " + res.strip().replace("\n", "\n   "))
                if not ok:
                    fails += 1
                    print(p.stdout.decode(errors="replace")[-600:])
    print("check_dos_crypto: %s" % ("FAILED" if fails else "passed"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
