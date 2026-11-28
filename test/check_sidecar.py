#!/usr/bin/env python3
"""Tests for boltpack-recover protect / check / repair (the .bpr recovery file).

    python3 test/check_sidecar.py --recover build/recover/boltpack-recover \
        --boltpack build/linux/boltpack [--windows-runner wine] [--seed N]

Ground truth never comes from the tool: sidecar_model.py writes the expected
sidecar from the format text, the damage is planned here (so the expected bad
sectors, groups and budgets are known), and every repair is compared byte for
byte with the volume files as they were before the damage.
"""
import argparse
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_recover as cr          # noqa: E402  (helpers, counters, timeouts)
import recover_fixtures as rf       # noqa: E402
import sidecar_model as m           # noqa: E402

check = cr.check
scen = Counter()
KERNELS_SEEN = Counter()
BIN = None
BOLTPACK = None
ENV = None


def hp(p):
    return cr.host_path(p)


def run(args, env_extra=None):
    env = dict(os.environ)
    for k, v in (env_extra or {}).items():
        if v is None:
            env.pop(k, None)            # None removes the variable
        else:
            env[k] = v
    r = subprocess.run(cr.tool_cmd(BIN) + args, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, env=env)
    return r.returncode, r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")


def jrun(args, env_extra=None):
    rc, out, err = run(args + ["--format", "json"], env_extra)
    try:
        return rc, json.loads(out), err
    except ValueError:
        return rc, None, err + out


class Prot(object):
    """One protected set: pristine volume bytes, the sidecar, helpers."""

    def __init__(self, fx, work, ss, percent, tag, volumes=None):
        self.tag = tag
        self.ss = ss
        self.percent = percent
        self.vols = volumes if volumes is not None else [
            (os.path.basename(fx.vs.paths[v]), fx.vs.data[v]) for v in range(fx.vs.volume_count)]
        self.set_id = fx.vs.set_id if volumes is None else 0
        self.dir = os.path.join(work, tag)
        shutil.rmtree(self.dir, ignore_errors=True)
        os.makedirs(self.dir)
        self.orig = os.path.join(self.dir, "orig")
        os.makedirs(self.orig)
        self.paths = []
        for name, data in self.vols:
            p = os.path.join(self.orig, name)
            with open(p, "wb") as f:
                f.write(data)
            self.paths.append(p)
        self.sidecar = os.path.join(self.dir, "set.bpr")
        self.cli_test = fx.payloads is None and volumes is None
        self._clean = None
        self.blob, self.info = m.build(self.vols, ss, percent, self.set_id)
        self.S = self.info["S"]
        self.ng, self.r = self.info["ngroups"], self.info["r"]
        self.starts = []
        n = 0
        for c in self.info["vol_sectors"]:
            self.starts.append(n)
            n += c

    def baseline_clean(self):
        """Does the tool's own analysis find the original set fully intact?
        (Some fixtures are not: nested archives, the synthetic set.)"""
        if self._clean is None:
            rep, rc, err = cr.analyze(BIN, [hp(x) for x in self.paths])
            self._clean = bool(rep and rep["set"]["complete"] and rep["table"]["status"] == "trusted" and
                               all(e["status"] == "ok" for e in rep["entries"]))
        return self._clean

    def protect(self, extra=(), env=None):
        for _ in range(2):
            if os.path.exists(self.sidecar):
                os.remove(self.sidecar)
            res = run(["protect", "-r", str(self.percent), "--sector-size", str(self.ss),
                       "-o", hp(self.sidecar)] + list(extra) + [hp(p) for p in self.paths], env)
            if res[0] != -999 or not cr.WIN["runner"]:
                break
        return res

    def loc(self, i):
        """(volume, local sector) of global data sector i."""
        for v in range(len(self.vols) - 1, -1, -1):
            if self.starts[v] <= i and self.info["vol_sectors"][v] > 0:
                return v, i - self.starts[v]
        raise ValueError(i)

    def group(self, g):
        return m.group_members(g, self.S, self.ng)

    # --- damaged copies -------------------------------------------------
    def copy_set(self, name):
        d = os.path.join(self.dir, name)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        out = []
        for vname, data in self.vols:
            p = os.path.join(d, vname)
            with open(p, "wb") as f:
                f.write(data)
            out.append(p)
        return d, out

    def damage(self, paths, sectors, rng, kind=None):
        """Damage each global data sector in `sectors` inside the files."""
        bufs = {}
        for i in sectors:
            v, local = self.loc(i)
            if v not in bufs:
                with open(paths[v], "rb") as f:
                    bufs[v] = bytearray(f.read())
            b = bufs[v]
            off = local * self.ss
            end = min(off + self.ss, len(b))
            k = kind or rng.choice(("flip", "zero", "garbage"))
            if k == "flip":
                p = rng.randrange(off, end)
                b[p] ^= 1 << rng.randrange(8)
            elif k == "zero":
                if any(b[off:end]):
                    b[off:end] = b"\0" * (end - off)
                else:
                    b[off] ^= 0xFF
            else:
                for p in range(off, end):
                    b[p] = rng.randrange(256)
                if bytes(b[off:end]) == self.vols[v][1][off:end]:
                    b[off] ^= 0xFF
        for v, b in bufs.items():
            with open(paths[v], "wb") as f:
                f.write(b)

    def expected_bad(self, sectors):
        out = {}
        for i in sectors:
            v, local = self.loc(i)
            out.setdefault(v, []).append(local)
        return {v: sorted(set(l)) for v, l in out.items()}

    def group_counts(self, sectors, parity_bad=()):
        d = Counter(i % self.ng for i in sectors)
        p = Counter(q // self.r for q in parity_bad)
        return d, p

    def verify_repaired(self, outdir, tag):
        ok = True
        for name, data in self.vols:
            p = os.path.join(outdir, name)
            if not os.path.exists(p):
                ok = False
                check(False, "%s: %s was written" % (tag, name))
                continue
            with open(p, "rb") as f:
                same = f.read() == data
            if not same:
                ok = False
            check(same, "%s: %s is byte for byte the original" % (tag, name))
        return ok


def fresh_out(prot, name):
    d = os.path.join(prot.dir, name)
    shutil.rmtree(d, ignore_errors=True)
    return d


def boltpack_test(prot, outdir, tag):
    """The CLI's own integrity test on the repaired set (CLI-built fixtures)."""
    if not prot.cli_test:
        return
    last = os.path.join(outdir, prot.vols[-1][0])
    r = subprocess.run([BOLTPACK, "test", last], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    check(r.returncode == 0, "%s: boltpack test on the repaired set (rc %d)" % (tag, r.returncode))


# ---------------------------------------------------------------- scenarios

def t_format(fx, work, ss, percent, tag):
    scen["format"] += 1
    p = Prot(fx, work, ss, percent, tag)
    rc, out, err = p.protect()
    check(rc == 0, "%s: protect ran (rc %d) %s" % (tag, rc, err[-120:]))
    if rc != 0:
        return None
    with open(p.sidecar, "rb") as f:
        got = f.read()
    need = -(-m.permille_of(percent) * p.S // 1000)
    check(p.r * p.ng >= need, "%s: r*groups = %d covers %s%% of %d sectors (%d)" % (tag, p.r * p.ng, percent, p.S, need))
    check(got == p.blob, "%s: protect output equals the independent model byte for byte "
          "(%d vs %d bytes)" % (tag, len(got), len(p.blob)))
    if got != p.blob:
        for i, (a, b) in enumerate(zip(got, p.blob)):
            if a != b:
                print("   first difference at byte %d" % i)
                break
    rc, j, err = jrun(["check", hp(p.sidecar)])
    check(rc == 0 and j is not None, "%s: check of the sidecar alone (rc %d)" % (tag, rc))
    forced = os.environ.get("BPR_GF_KERNEL")
    if j and forced:
        ok_names = {"scalar": ("scalar",), "ssse3": ("ssse3", "scalar"), "avx2": ("avx2", "scalar")}[forced]
        check(j["sidecar"]["kernel"] in ok_names and (forced != "scalar" or j["sidecar"]["kernel"] == "scalar"),
              "%s: BPR_GF_KERNEL=%s is honoured (the tool reports %s)" % (tag, forced, j["sidecar"]["kernel"]))
    if j:
        KERNELS_SEEN[j["sidecar"]["kernel"]] += 1
    # exists: never replaced
    rc2, _, _ = run(["protect", "-r", str(percent), "--sector-size", str(ss), "-o",
                     hp(p.sidecar)] + [hp(x) for x in p.paths])
    check(rc2 == 3, "%s: an existing recovery file is never replaced (rc %d)" % (tag, rc2))
    check(open(p.sidecar, "rb").read() == p.blob, "%s: and is left unchanged" % tag)
    return p


def REPRC(p):
    """Exit status of a successful repair: 0, or 1 when the archive that was
    protected is itself not intact (some fixtures are not)."""
    return 0 if p.baseline_clean() else 1


def digest(paths):
    out = []
    for f in paths:
        try:
            with open(f, "rb") as fh:
                out.append(zlib.crc32(fh.read()))
        except OSError:
            out.append(None)
    return out


def do_repair(p, files, outname, extra=()):
    out = fresh_out(p, outname)
    before = digest(files + [p.sidecar])
    rc, j, err = jrun(["repair", "-o", hp(out), hp(p.sidecar)] + [hp(x) for x in files] + list(extra))
    check(digest(files + [p.sidecar]) == before,
          "%s %s: repair left the volume files and the recovery file unchanged" % (p.tag, outname))
    return rc, j, err, out


def t_clean(p, work):
    scen["clean"] += 1
    rc, j, err = jrun(["check", hp(p.sidecar)] + [hp(x) for x in p.paths])
    check(rc == 0 and j and j["damage"]["bad_data_sectors"] == 0,
          "%s: pristine set checks clean (rc %d)" % (p.tag, rc))
    rc, j, err, out = do_repair(p, p.paths, "out-clean")
    check(rc == 0 and not os.path.exists(out), "%s: nothing to repair writes nothing (rc %d)" % (p.tag, rc))


def t_burst(p, rng, label, start, length):
    scen["burst"] += 1
    tag = "%s %s" % (p.tag, label)
    d, files = p.copy_set("dmg-burst")
    sectors = [i for i in range(start, start + length) if i < p.S]
    p.damage(files, sectors, rng)
    rc, j, err = jrun(["check", hp(p.sidecar)] + [hp(x) for x in files])
    want = p.expected_bad(sectors)
    ok = rc == 1 and j is not None
    check(ok, "%s: check finds repairable damage (rc %d)" % (tag, rc))
    if j:
        got = {v["slot"]: v["bad"] for v in j["volumes"] if v["bad"]}
        check(got == want, "%s: check names exactly the damaged sectors" % tag)
        check(j["damage"]["unrepairable_groups"] == 0, "%s: no group over budget" % tag)
    rc, j, err, out = do_repair(p, files, "out-burst")
    check(rc in (0, 1) and j is not None, "%s: repair succeeded (rc %d) %s" % (tag, rc, err[-150:]))
    if rc in (0, 1):
        if p.verify_repaired(out, tag):
            boltpack_test(p, out, tag)
        a = j["repair"]["archive"]
        clean = a["checked"] and a["complete"] and a["table_trusted"] and a["entries"] == a["entries_ok"]
        check(clean == p.baseline_clean(),
              "%s: the tool's analysis of the result agrees with its analysis of the original (%s)" % (tag, clean))
        check(rc == (0 if p.baseline_clean() else 1), "%s: exit status says whether the archive itself is intact" % tag)


def t_scattered(p, rng, tag_extra="", per_group=None):
    scen["scattered"] += 1
    tag = "%s scattered%s" % (p.tag, tag_extra)
    d, files = p.copy_set("dmg-scat")
    sectors = []
    for g in range(p.ng):
        mem = p.group(g)
        n = per_group if per_group is not None else p.r
        n = min(n, len(mem), p.r)
        sectors += rng.sample(mem, n)
    p.damage(files, sectors, rng)
    rc, j, err = jrun(["check", hp(p.sidecar)] + [hp(x) for x in files])
    check(rc == 1 and j and j["damage"]["bad_data_sectors"] == len(sectors),
          "%s: check counts %d bad sectors, all groups at budget (rc %d)" % (tag, len(sectors), rc))
    if j:
        check(j["damage"]["worst_margin"] == 0 or len(p.group(0)) < p.r or per_group is not None,
              "%s: worst margin is zero when every group is at budget" % tag)
    rc, j, err, out = do_repair(p, files, "out-scat")
    want = 0 if p.baseline_clean() else 1
    check(rc == want, "%s: repair succeeded (rc %d, want %d)" % (tag, rc, want))
    if rc in (0, 1) and p.verify_repaired(out, tag):
        boltpack_test(p, out, tag)


def t_over(p, rng):
    scen["over limit"] += 1
    tag = p.tag + " over limit"
    g = rng.randrange(p.ng)
    mem = p.group(g)
    if len(mem) <= p.r:
        return
    d, files = p.copy_set("dmg-over")
    sectors = rng.sample(mem, p.r + 1)
    p.damage(files, sectors, rng)
    rc, j, err = jrun(["check", hp(p.sidecar)] + [hp(x) for x in files])
    check(rc == 2 and j and j["damage"]["unrepairable_groups"] == 1 and
          j["damage"]["unrepairable"][0]["group"] == g and
          j["damage"]["unrepairable"][0]["bad_data"] == p.r + 1,
          "%s: check reports group %d as unrepairable (rc %d)" % (tag, g, rc))
    rc, j, err, out = do_repair(p, files, "out-over")
    check(rc == 2, "%s: repair refuses (rc %d)" % (tag, rc))
    check(not os.path.exists(out), "%s: nothing is written" % tag)
    # one sector fewer: repairs
    p.damage(files, [], rng)
    d, files = p.copy_set("dmg-over2")
    p.damage(files, sectors[:-1], rng)
    rc, j, err, out = do_repair(p, files, "out-over2")
    check(rc == (0 if p.baseline_clean() else 1), "%s: one sector fewer is repaired (rc %d)" % (tag, rc))
    if rc in (0, 1):
        p.verify_repaired(out, tag + " (at the limit)")


def t_lost_volume(p, rng, v):
    scen["lost volume"] += 1
    tag = "%s lost volume %d" % (p.tag, v)
    d, files = p.copy_set("dmg-lost")
    os.remove(files[v])
    given = [f for i, f in enumerate(files) if i != v]
    nsec = p.info["vol_sectors"][v]
    rc, j, err, out = do_repair(p, given, "out-lost")
    fits = nsec <= p.r * p.ng and all(
        sum(1 for i in range(p.starts[v], p.starts[v] + nsec) if i % p.ng == g) <= p.r
        for g in range(p.ng))
    if fits:
        check(rc == (0 if p.baseline_clean() else 1), "%s: repair rebuilt the missing volume (rc %d) %s" % (tag, rc, err[-120:]))
        if rc in (0, 1):
            p.verify_repaired(out, tag)
            boltpack_test(p, out, tag)
    else:
        check(rc == 2 and not os.path.exists(out), "%s: too much for the parity, refused (rc %d)" % (tag, rc))


def t_parity_damage(p, rng):
    """Damage in the sidecar's parity region reduces the budget."""
    scen["parity damage"] += 1
    tag = p.tag + " parity damage"
    g = rng.randrange(p.ng)
    mem = p.group(g)
    if p.r < 2 or len(mem) < p.r:
        return
    for nbadp, ok_expect in ((1, True), (p.r, False)):
        nd = p.r - nbadp + (0 if ok_expect else 1)      # data+parity == r, or r+1
        nd = max(nd, 0)
        if nd == 0 and ok_expect:
            nd = 0
        d, files = p.copy_set("dmg-par")
        sectors = rng.sample(mem, min(nd, len(mem)))
        p.damage(files, sectors, rng)
        sc = os.path.join(p.dir, "dmg-par", "set.bpr")
        blob = bytearray(p.blob)
        pars = rng.sample(range(g * p.r, g * p.r + p.r), nbadp)
        for q in pars:
            off = p.info["p_off"] + q * p.ss + rng.randrange(p.ss)
            blob[off] ^= 0x5A
        with open(sc, "wb") as f:
            f.write(blob)
        out = fresh_out(p, "out-par")
        rc, j, err = jrun(["repair", "-o", hp(out), hp(sc)] + [hp(x) for x in files])
        total = len(sectors) + nbadp
        expect_ok = total <= p.r or len(sectors) == 0
        if len(sectors) == 0:
            # parity-only damage: nothing to repair in the volumes
            check(rc == 0 and not os.path.exists(out), "%s: parity-only damage, volumes fine (rc %d)" % (tag, rc))
            continue
        if expect_ok:
            check(rc == REPRC(p), "%s: %d data + %d parity bad within budget %d repairs (rc %d)" % (tag, len(sectors), nbadp, p.r, rc))
            if rc == REPRC(p):
                p.verify_repaired(out, tag)
        else:
            check(rc == 2 and not os.path.exists(out),
                  "%s: %d data + %d parity bad exceeds budget %d (rc %d)" % (tag, len(sectors), nbadp, p.r, rc))


def t_sidecar_damage(p, rng):
    scen["sidecar damage"] += 1
    tag = p.tag + " damaged sidecar"
    info = p.info
    d, files = p.copy_set("dmg-sc")
    sectors = rng.sample(range(p.S), min(p.r, p.S))
    # keep each group within budget
    sectors = [i for i in sectors]
    cnt = Counter(i % p.ng for i in sectors)
    sectors = [i for i in sectors if cnt[i % p.ng] <= p.r]
    p.damage(files, sectors, rng)

    def with_sidecar(blob, name):
        sc = os.path.join(p.dir, "dmg-sc", name)
        with open(sc, "wb") as f:
            f.write(blob)
        out = fresh_out(p, "out-sc")
        rc, j, err = jrun(["repair", "-o", hp(out), hp(sc)] + [hp(x) for x in files])
        return rc, j, err, out

    # primary header destroyed
    b = bytearray(p.blob)
    b[0:16] = b"\xff" * 16
    rc, j, err, out = with_sidecar(b, "h.bpr")
    check(rc == REPRC(p) and j and j["sidecar"]["header"] == "copy",
          "%s: primary header destroyed, the copy is used (rc %d)" % (tag, rc))
    if rc == REPRC(p):
        p.verify_repaired(out, tag + " (header copy)")
    # footer destroyed: primary header still fine
    b = bytearray(p.blob)
    b[-48:] = b"\0" * 48
    rc, j, err, out = with_sidecar(b, "f.bpr")
    check(rc == REPRC(p), "%s: footer destroyed, primary header used (rc %d)" % (tag, rc))
    # both headers destroyed
    b = bytearray(p.blob)
    b[0:8] = b"\xff" * 8
    b[info["h2_off"]:info["h2_off"] + 8] = b"\xff" * 8
    rc, j, err, out = with_sidecar(b, "hh.bpr")
    check(rc == 3 and not os.path.exists(out), "%s: both headers destroyed is refused (rc %d)" % (tag, rc))
    # primary table chunk 0 damaged: copy chunk used
    b = bytearray(p.blob)
    b[info["t_off"] + 5] ^= 0xFF
    rc, j, err, out = with_sidecar(b, "t.bpr")
    check(rc == REPRC(p) and j and j["sidecar"]["chunks"]["copy"] >= 1,
          "%s: damaged table chunk read from the copy (rc %d)" % (tag, rc))
    if rc == REPRC(p):
        p.verify_repaired(out, tag + " (table copy)")
    # same chunk damaged in both copies: unknown chunk.  Its data sectors cannot be
    # told from bad ones and are rebuilt from parity, so with a single chunk (up to
    # 1,024 entries) every sector counts as bad: repairable only if S <= r * groups.
    b = bytearray(p.blob)
    b[info["t_off"] + 5] ^= 0xFF
    b[info["t2_off"] + 9] ^= 0xFF
    entries = p.S + p.info["nparity"]
    first_chunk = min(m.CHUNK, entries)
    data_in_chunk = min(first_chunk, p.S)
    per_group = Counter(i % p.ng for i in range(data_in_chunk))
    fits = max(per_group.values()) <= p.r
    rc, j, err, out = with_sidecar(b, "tt.bpr")
    check(j is not None and j["sidecar"]["chunks"]["unreadable"] >= 1,
          "%s: a chunk damaged in both copies is reported unreadable (rc %d)" % (tag, rc))
    if fits:
        check(rc == REPRC(p), "%s: its sectors are rebuilt from parity (rc %d)" % (tag, rc))
        if rc == REPRC(p):
            p.verify_repaired(out, tag + " (unknown chunk)")
            check(j["repair"]["unverified_sectors"] >= 1, "%s: and reported as not checkable" % tag)
    else:
        check(rc == 2 and not os.path.exists(out),
              "%s: %d sectors with no checksum exceed the budget, refused (rc %d)" % (tag, data_in_chunk, rc))
    # truncated sidecar: header and table survive, parity gone
    b = bytes(p.blob[:info["p_off"]])
    rc, j, err, out = with_sidecar(b, "trunc.bpr")
    check(rc == 2 and not os.path.exists(out),
          "%s: a sidecar without its parity cannot repair, nothing is written (rc %d)" % (tag, rc))
    # garbage and empty files
    for nm, blob in (("empty.bpr", b""), ("junk.bpr", os.urandom(300)), ("tiny.bpr", b"BPRC")):
        rc, j, err, out = with_sidecar(blob, nm)
        check(rc == 3 and not os.path.exists(out), "%s: %s is refused (rc %d)" % (tag, nm, rc))


def patch_version(p, version, flags=0):
    b = bytearray(p.blob)
    h = m.parse_header(bytes(b))
    struct.pack_into("<HH", b, 4, version, flags)
    end = h["hdr_len"]
    struct.pack_into("<I", b, end - 4, zlib.crc32(bytes(b[:end - 4])) & 0xFFFFFFFF)
    return b


def patch_header(p, **changes):
    """A sidecar blob with header fields changed and the header CRC fixed."""
    b = bytearray(p.blob)
    h = m.parse_header(bytes(b))
    hdr_end = h["hdr_len"]
    fields = dict(nvol=16, ss=20, S=24, r=28, ngroups=32, nparity=36)
    for k, v in changes.items():
        struct.pack_into("<I", b, fields[k], v)
    crc = zlib.crc32(bytes(b[:hdr_end - 4])) & 0xFFFFFFFF
    struct.pack_into("<I", b, hdr_end - 4, crc)
    return bytes(b)


def t_hostile_sidecar(p, rng):
    scen["hostile sidecar"] += 1
    tag = p.tag + " hostile sidecar"
    d, files = p.copy_set("dmg-host")
    cases = [
        ("one group too big", dict(ngroups=1, nparity=p.r)),
        ("r too big for the group", dict(r=250, nparity=250 * p.ng)),
        ("parity count mismatch", dict(nparity=p.ng * p.r + 1)),
        ("sector size zero", dict(ss=0)),
        ("sector size unaligned", dict(ss=p.ss + 1)),
        ("sector size huge", dict(ss=1 << 30)),
        ("r zero", dict(r=0, nparity=0)),
        ("groups zero", dict(ngroups=0, nparity=0)),
        ("more groups than sectors", dict(ngroups=p.S + 1, nparity=(p.S + 1) * p.r)),
        ("volume count zero", dict(nvol=0)),
        ("volume count huge", dict(nvol=70000)),
        ("sector count wrong", dict(S=p.S + 1)),
        ("parity too large to hold", dict(ss=1 << 24, r=255, ngroups=1, nparity=255)),
    ]
    for name, ch in cases:
        blob = patch_header(p, **ch)
        # a copy of the header too, so the fallback cannot hide the bad one
        b = bytearray(blob)
        h2 = p.info["h2_off"]
        b[h2:h2 + p.info["hdr_len"]] = blob[:p.info["hdr_len"]]
        sc = os.path.join(p.dir, "dmg-host", "h.bpr")
        with open(sc, "wb") as f:
            f.write(b)
        rc, out, err = run(["check", hp(sc)] + [hp(x) for x in files])
        check(rc == 3 and err.strip() != "", "%s: %s is refused with a reason (rc %d)" % (tag, name, rc))
    for ver, fl in ((2, 0), (0, 0), (1, 1)):
        b = patch_version(p, ver, fl)
        b[p.info["h2_off"]:p.info["h2_off"] + p.info["hdr_len"]] = b[:p.info["hdr_len"]]
        sc = os.path.join(p.dir, "dmg-host", "v.bpr")
        with open(sc, "wb") as f:
            f.write(b)
        rc, out, err = run(["check", hp(sc)] + [hp(x) for x in files])
        check(rc == 3 and ("version" in err or "damaged" in err),
              "%s: version %d flags %d is refused (rc %d: %s)" % (tag, ver, fl, rc, err.strip()[:70]))
    # numbers the format forbids, in an otherwise consistent sidecar
    for name, force in (("one group of every sector", (1, 3)), ("r too large for the group", (1, 255))):
        blob, _ = m.build(p.vols, p.ss, p.percent, p.set_id, force=force)
        sc = os.path.join(p.dir, "dmg-host", "g.bpr")
        with open(sc, "wb") as f:
            f.write(blob)
        out = fresh_out(p, "out-host")
        rc, j, err = jrun(["repair", "-o", hp(out), hp(sc)] + [hp(x) for x in files])
        g_size = -(-p.S // force[0])
        if g_size + force[1] > 256:
            check(rc == 3 and "exceeds the code" in err, "%s: %s (group of %d + %d parity) is refused (rc %d: %s)" % (
                tag, name, g_size, force[1], rc, err.strip()[:80]))
    # names in the volume list: traversal and separators are refused
    for bad in ("../evil", "a/b", "..", ".", "a\\b", "x\x01y"):
        vols = [(bad if i == 0 else n, dd) for i, (n, dd) in enumerate(p.vols)]
        try:
            blob, _ = m.build(vols, p.ss, p.percent, p.set_id)
        except Exception:
            continue
        sc = os.path.join(p.dir, "dmg-host", "n.bpr")
        with open(sc, "wb") as f:
            f.write(blob)
        out = fresh_out(p, "out-host")
        rc, j, err = jrun(["repair", "-o", hp(out), hp(sc)] + [hp(x) for x in files])
        check(rc == 3 and not os.path.exists(os.path.join(os.path.dirname(out), "evil")) and
              not os.path.exists(os.path.join(out, "evil")),
              "%s: volume name %r is refused (rc %d)" % (tag, bad, rc))
    # random bit flips in the header and footer: never a crash
    for _ in range(40):
        b = bytearray(p.blob)
        region = rng.choice(("hdr", "foot"))
        if region == "hdr":
            pos = rng.randrange(p.info["hdr_len"])
        else:
            pos = len(b) - 48 + rng.randrange(48)
        b[pos] ^= 1 << rng.randrange(8)
        sc = os.path.join(p.dir, "dmg-host", "z.bpr")
        with open(sc, "wb") as f:
            f.write(b)
        rc, j, err = jrun(["check", hp(sc)] + [hp(x) for x in files])
        check(rc in (0, 1, 2, 3) and (rc == 3 or j is not None),
              "%s: bit flip in %s at %d handled (rc %d)" % (tag, region, pos, rc))


def t_lying_parity(p, rng):
    """Parity that does not match the data but whose table entry does (a buggy
    writer, or damage that kept the checksums consistent): the repair must
    fail loudly, never write bytes that are not the originals."""
    scen["lying parity"] += 1
    tag = p.tag + " lying parity"
    d, files = p.copy_set("dmg-lie")
    g = rng.randrange(p.ng)
    sectors = [rng.choice(p.group(g))]
    p.damage(files, sectors, rng)
    h = m.parse_header(p.blob)
    blob = bytearray(p.blob)
    for j in range(p.r):
        q = g * p.r + j
        off = h["p_off"] + q * p.ss
        blob[off + 7] ^= 0xA5
        ent = m.sector_entry(p.S + q, bytes(blob[off:off + p.ss]), p.ss)
        blob = bytearray(m.patch_entry(bytes(blob), h, p.S + q, ent))
    sc = os.path.join(p.dir, "dmg-lie", "set.bpr")
    with open(sc, "wb") as f:
        f.write(blob)
    out = fresh_out(p, "out-lie")
    rc, j, err = jrun(["repair", "-o", hp(out), hp(sc)] + [hp(x) for x in files])
    check(rc == 3, "%s: a sidecar that lies about its parity is caught (rc %d)" % (tag, rc))
    written = [n for n in os.listdir(out)] if os.path.isdir(out) else []
    check(written == [], "%s: and no volume file is left behind (%r)" % (tag, written))


def t_tie(p, rng):
    scen["tie"] += 1
    tag = p.tag + " tie"
    if len(p.vols) < 2 or len(p.vols[0][1]) != len(p.vols[1][1]):
        check(False, "%s: needs two volumes of equal size" % tag)
        return
    d = os.path.join(p.dir, "tie")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    half = len(p.vols[0][1]) // 2
    half -= half % p.ss
    spliced = p.vols[0][1][:half] + p.vols[1][1][half:]
    sp = os.path.join(d, "spliced.dat")
    with open(sp, "wb") as f:
        f.write(spliced)
    rest = [x for i, x in enumerate(p.paths) if i > 1]
    rc, j, err, out = do_repair(p, [sp] + rest, "out-tie")
    check(rc == 3 and "equally well" in err and not os.path.exists(out),
          "%s: a file that matches two volumes equally is refused (rc %d: %s)" % (tag, rc, err.strip()[:90]))


def t_mapping(p, rng):
    scen["file mapping"] += 1
    tag = p.tag + " file mapping"
    d = os.path.join(p.dir, "map")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    # renamed volumes, shuffled order, a damaged one, a stranger and a duplicate
    order = list(range(len(p.vols)))
    rng.shuffle(order)
    files = []
    for k, v in enumerate(order):
        path = os.path.join(d, "renamed-%d.dat" % k)
        with open(path, "wb") as f:
            f.write(p.vols[v][1])
        files.append(path)
    stranger = os.path.join(d, "stranger.bin")
    with open(stranger, "wb") as f:
        f.write(os.urandom(777))
    sect = rng.sample(range(p.S), min(p.r, p.S))
    cnt = Counter(i % p.ng for i in sect)
    sect = [i for i in sect if cnt[i % p.ng] <= p.r]
    # damage through the shuffled paths: map volume -> file
    vpath = [None] * len(p.vols)
    for k, v in enumerate(order):
        vpath[v] = files[k]
    p.damage(vpath, sect, rng)
    rc, j, err, out = do_repair(p, files + [stranger], "out-map")
    check(rc == REPRC(p), "%s: renamed and shuffled files are matched by content (rc %d) %s" % (tag, rc, err[-120:]))
    if j:
        ign = [x["file"] for x in j["ignored"]]
        check(any(x.endswith("stranger.bin") for x in ign), "%s: the stranger file is reported ignored" % tag)
    if rc == REPRC(p):
        p.verify_repaired(out, tag)
    # a second, worse copy of a volume is ignored in favour of the better one
    if len(p.vols) > 1:
        dup = os.path.join(d, "worse-copy.dat")
        with open(dup, "wb") as f:
            f.write(b"\0" * len(p.vols[0][1]))
        rc, j, err, out = do_repair(p, files + [dup], "out-map2")
        check(rc == REPRC(p), "%s: a worse copy of a volume is set aside (rc %d)" % (tag, rc))


def t_size(p, rng):
    scen["size changes"] += 1
    tag = p.tag + " size changes"
    # truncated last volume (a few sectors cut) and one extended with junk
    for kind in ("truncate", "extend"):
        d, files = p.copy_set("dmg-size")
        v = len(files) - 1
        data = bytearray(p.vols[v][1])
        if kind == "truncate":
            cut = min(len(data) - 1, p.ss * 2 + 3)
            data = data[:len(data) - cut]
        else:
            data += os.urandom(100)
        with open(files[v], "wb") as f:
            f.write(data)
        rc, j, err, out = do_repair(p, files, "out-size")
        check(rc == REPRC(p), "%s: %s volume repaired to the recorded size (rc %d)" % (tag, kind, rc))
        if rc == REPRC(p):
            p.verify_repaired(out, "%s %s" % (tag, kind))
        if kind == "extend" and j:
            check(j["volumes"][v]["trailing_bytes"] == 100, "%s: the extra bytes are reported" % tag)


def t_already_damaged(fx, work, rng):
    """protect on an archive that is already damaged: warned, and repair
    restores it to that state, reported as such."""
    scen["already damaged"] += 1
    tag = "tree-lz already damaged"
    vols = [(os.path.basename(fx.vs.paths[v]), bytearray(fx.vs.data[v])) for v in range(fx.vs.volume_count)]
    # damage a payload byte deep in the first volume (a block CRC will fail)
    name, data = vols[0]
    data[len(data) // 2] ^= 0xFF
    vols = [(n, bytes(d)) for n, d in vols]
    p = Prot(fx, work, 512, 10, "dmgproto", volumes=vols)
    rc, out, err = p.protect()
    check(rc == 0 and "already damaged" in err, "%s: protect warns but writes (rc %d) %s" % (tag, rc, err[-100:]))
    if rc != 0:
        return
    d, files = p.copy_set("dmg-ad")
    sect = rng.sample(range(p.S), min(p.r, p.S))
    cnt = Counter(i % p.ng for i in sect)
    sect = [i for i in sect if cnt[i % p.ng] <= p.r]
    p.damage(files, sect, rng)
    rc, j, err, out = do_repair(p, files, "out-ad")
    check(rc == 1 and j and "itself damaged" in j["verdict"],
          "%s: restored to the protected state, reported as still damaged (rc %d)" % (tag, rc))
    if rc == 1:
        p.verify_repaired(out, tag)


def t_kernels(p, rng):
    scen["kernels"] += 1
    tag = p.tag + " kernels"
    rc, out, err = p.protect(extra=["--format", "json"], env={"BPR_GF_KERNEL": None})
    try:
        best = json.loads(out)["protect"]["kernel"]
    except ValueError:
        best = "?"
    base = open(p.sidecar, "rb").read()
    for k in ("scalar", "ssse3", "avx2"):
        want = {"scalar": "scalar", "ssse3": "ssse3" if best in ("ssse3", "avx2") else "scalar",
                "avx2": "avx2" if best == "avx2" else "scalar"}[k]
        rc, out, err = p.protect(extra=["--format", "json"], env={"BPR_GF_KERNEL": k})
        try:
            got = json.loads(out)["protect"]["kernel"]
        except ValueError:
            got = "?"
        check(rc == 0 and open(p.sidecar, "rb").read() == base,
              "%s: BPR_GF_KERNEL=%s writes the same sidecar (rc %d)" % (tag, k, rc))
        check(got == want, "%s: forcing %s runs the %s kernel (protect reports %s)" % (tag, k, want, got))
        d, files = p.copy_set("dmg-k")
        sect = []
        for g in range(p.ng):
            sect += rng.sample(p.group(g), min(p.r, len(p.group(g))))
        p.damage(files, sect, rng)
        out = fresh_out(p, "out-k")
        rc, j, err = jrun(["repair", "-o", hp(out), hp(p.sidecar)] + [hp(x) for x in files],
                          {"BPR_GF_KERNEL": k})
        check(rc == REPRC(p), "%s: repair with kernel %s (rc %d)" % (tag, k, rc))
        check(j is not None and j["sidecar"]["kernel"] == want,
              "%s: and repair ran the %s kernel (%s)" % (tag, want, j and j["sidecar"]["kernel"]))
        if rc == REPRC(p):
            p.verify_repaired(out, "%s %s" % (tag, k))
    p.protect()                     # leave the default sidecar in place


def t_unknown_chunk(p, rng):
    """A table chunk lost in both copies, in a set big enough to afford it:
    the sectors it covers are rebuilt from parity although they were fine, and
    reported as not checkable; real damage elsewhere is still repaired."""
    scen["unknown chunk"] += 1
    tag = p.tag + " unknown chunk"
    entries = p.S + p.info["nparity"]
    check(entries > 2 * m.CHUNK, "%s: the set has several table chunks" % tag)
    d, files = p.copy_set("dmg-uc")
    per_group = Counter(i % p.ng for i in range(m.CHUNK))
    spare = p.r - max(per_group.values())
    check(spare >= 1, "%s: the chunk fits in the budget with %d to spare" % (tag, spare))
    g = max(per_group, key=lambda x: -per_group[x])
    far = [i for i in p.group(g) if i >= m.CHUNK][:max(spare, 0)]
    p.damage(files, far, rng)
    b = bytearray(p.blob)
    b[p.info["t_off"] + 5] ^= 0xFF
    b[p.info["t2_off"] + 9] ^= 0xFF
    sc = os.path.join(p.dir, "dmg-uc", "set.bpr")
    with open(sc, "wb") as f:
        f.write(b)
    out = fresh_out(p, "out-uc")
    rc, j, err = jrun(["repair", "-o", hp(out), hp(sc)] + [hp(x) for x in files])
    check(rc == REPRC(p), "%s: repaired although one chunk of checksums is gone (rc %d)" % (tag, rc))
    if rc == REPRC(p):
        p.verify_repaired(out, tag)
        check(j["repair"]["unverified_sectors"] == m.CHUNK,
              "%s: exactly the %d sectors of that chunk are reported as not checkable (%s)" % (
                  tag, m.CHUNK, j["repair"]["unverified_sectors"]))
        check(j["repair"]["repaired_sectors"] == m.CHUNK + len(far), "%s: and %d real ones repaired" % (tag, len(far)))


def t_default_name(fx, work):
    scen["default name"] += 1
    tag = "default name"
    d = os.path.join(work, "defname")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    paths = []
    for v in range(fx.vs.volume_count):
        pth = os.path.join(d, os.path.basename(fx.vs.paths[v]))
        with open(pth, "wb") as f:
            f.write(fx.vs.data[v])
        paths.append(pth)
    rc, out, err = run(["protect", "-r", "10", "--sector-size", "512"] + [hp(x) for x in paths])
    stem = os.path.splitext(os.path.basename(fx.vs.paths[0]))[0]
    check(rc == 0 and os.path.exists(os.path.join(d, stem + ".bpr")),
          "%s: without -o the recovery file is <first volume>.bpr (rc %d, %s)" % (tag, rc, sorted(os.listdir(d))[-1]))


def t_params(fx, work):
    """Group arithmetic at the edges, against the model."""
    shapes = [(512, 0.1), (512, 1), (512, 50), (512, 100), (64, 10), (4096, 5), (512, 12.5)]
    for ss, pct in shapes:
        scen["params"] += 1
        p = Prot(fx, work, ss, pct, "params-%d-%s" % (ss, pct))
        rc, out, err = p.protect()
        if rc != 0:
            check(False, "%s: protect (rc %d) %s" % (p.tag, rc, err[-100:]))
            continue
        check(open(p.sidecar, "rb").read() == p.blob,
              "%s: sector %d, %s%% (S=%d, %d groups, r=%d) equals the model" % (p.tag, ss, pct, p.S, p.ng, p.r))
        # damage exactly r in group 0 (or all of it if smaller) and repair
        rng = random.Random(1)
        d, files = p.copy_set("dmg-params")
        mem = p.group(0)
        sect = rng.sample(mem, min(p.r, len(mem)))
        p.damage(files, sect, rng)
        rc, j, err, out = do_repair(p, files, "out-params")
        check(rc == REPRC(p), "%s: repair at the budget (rc %d)" % (p.tag, rc))
        if rc == REPRC(p):
            p.verify_repaired(out, p.tag)


def main():
    global BIN, BOLTPACK
    ap = argparse.ArgumentParser()
    ap.add_argument("--recover", required=True)
    ap.add_argument("--boltpack", required=True)
    ap.add_argument("--seed", type=int, default=20261128)
    ap.add_argument("--windows-runner", metavar="CMD")
    ap.add_argument("--keep")
    args = ap.parse_args()
    BIN = os.path.abspath(args.recover)
    BOLTPACK = os.path.abspath(args.boltpack)
    if args.windows_runner:
        cr.WIN["runner"] = args.windows_runner
    cr.warm_up(BIN)
    rng = random.Random(args.seed)
    root = args.keep or tempfile.mkdtemp(prefix="bpr-sc-")
    os.makedirs(root, exist_ok=True)
    try:
        fixtures = rf.build_all(BOLTPACK, root)
        work = os.path.join(root, "work")
        os.makedirs(work, exist_ok=True)

        # (fixture, sector size, percent): every fixture, sizes chosen to give real groups
        plan = [("tree-stored", 512, 10), ("tree-lite", 512, 10), ("tree-lz", 512, 12.5),
                ("tree-fluff", 1024, 10), ("vol-fluff", 256, 20), ("vol-stored", 512, 15),
                ("vol-lz", 512, 10), ("vol-lite", 512, 10), ("wide-fluff", 4096, 10),
                ("vol-many", 512, 10), ("vol-many", 64, 25), ("nested-stored", 512, 10),
                ("nested-fluff", 512, 10), ("synthetic", 256, 10)]
        for name, ss, pct in plan:
            fx = fixtures[name]
            p = t_format(fx, work, ss, pct, "%s-%d-%s" % (name, ss, pct))
            if p is None:
                continue
            t_clean(p, work)
            budget = -(-m.permille_of(pct) * p.S // 1000)       # the requested percentage of the sectors
            # bursts: the head of the set (volume header), the tail (trailer), random, the maximum
            t_burst(p, rng, "head", 0, min(budget, p.S))
            t_burst(p, rng, "tail", max(0, p.S - budget), budget)
            t_burst(p, rng, "one sector", rng.randrange(p.S), 1)
            t_burst(p, rng, "middle max", rng.randrange(max(1, p.S - budget)), budget)
            t_burst(p, rng, "random", rng.randrange(p.S), rng.randint(1, budget))
            t_scattered(p, rng)
            t_scattered(p, rng, " half", per_group=max(1, p.r // 2))
            t_over(p, rng)
            if len(p.vols) > 1:
                for v in (0, len(p.vols) - 1, len(p.vols) // 2):
                    t_lost_volume(p, rng, v)
            t_parity_damage(p, rng)
            t_lying_parity(p, rng)
            if name in ("tree-stored", "vol-fluff"):
                t_sidecar_damage(p, rng)
                t_hostile_sidecar(p, rng)
                t_mapping(p, rng)
                t_size(p, rng)
            if name in ("tree-fluff", "vol-stored"):
                t_kernels(p, rng)
            if name == "vol-stored":
                t_tie(p, rng)
            if name == "vol-many" and ss == 64:
                t_unknown_chunk(p, rng)
        t_default_name(fixtures["tree-stored"], work)
        t_default_name(fixtures["vol-stored"], work)
        t_params(fixtures["tree-lz"], work)
        t_already_damaged(fixtures["tree-lz"], work, rng)
    finally:
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)
    print("\nKernels used: " + ", ".join("%s x%d" % kv for kv in sorted(KERNELS_SEEN.items())))
    print("Scenarios: " + ", ".join("%d %s" % (n, k) for k, n in scen.items()))
    print("%d passed, %d failed" % (cr.passed, cr.failed))
    return 1 if cr.failed else 0


if __name__ == "__main__":
    sys.exit(main())
