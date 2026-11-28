#!/usr/bin/env python3
"""Tests for boltpack-recover (phase 1: `analyze`).

What is checked, and against what:

* Ground truth is parse_set() on the intact fixture, taken before any damage.
  Nothing the tool reports is used to decide what was expected.
* Clean fixtures: every entry is reported where the table says, with the same
  fields, status ok, chained, and the set is complete with nothing unaccounted.
* Damaged fixtures: every entry whose structures (header, blocks, trailer) do
  not intersect a byte that actually changed, and whose volumes still exist,
  MUST be reported with status ok.  An entry that does intersect damage MUST
  NOT be reported ok.  A reported entry that is not in the truth (a header
  found inside another entry's data) MUST be labelled suspect.
* Nested archives: an archive stored inside an archive has CRC-valid headers
  in its data.  Clean, they must be ignored (not reported as entries); with the
  holding entry damaged they may appear, but only as suspect.
* A seeded sweep applies random structural damage to every fixture.

"Deterministic" here means structural: contents and mtimes are fixed and the
RNG is seeded, but the CLI draws the set id from the clock, so archive bytes
differ run to run.  Expectations are therefore structural, never byte-for-byte.

Usage: python3 test/check_recover.py [--recover BIN] [--boltpack BIN]
                                     [--seed N] [--sweep N] [--keep DIR]
"""
import argparse
import collections
import json
import os
import random
import re
import shutil
import struct
import subprocess
import time


# No single process may hang the suite: every child gets a time limit, and one
# that exceeds it is killed and shows up as a failed run (exit status -999).
CALL_TIMEOUT = 600
WINE_CALL_TIMEOUT = 180     # under Wine a launch occasionally stalls once
_real_run = subprocess.run
timeouts = {"retried": 0}


def _run_with_timeout(*args, **kw):
    limit = kw.pop("timeout", None)
    if limit is None:
        limit = WINE_CALL_TIMEOUT if WIN["runner"] else CALL_TIMEOUT
    argv = args[0] if args else kw.get("args", [])
    # Only a read-only command is retried: a timed-out extract or rebuild may
    # have written into its output, and a second try would be refused.
    attempts = 2 if WIN["runner"] and "analyze" in argv else 1
    for attempt in range(attempts):
        try:
            return _real_run(*args, timeout=limit, **kw)
        except subprocess.TimeoutExpired as exc:
            cmd = " ".join(str(x) for x in (args[0] if args else kw.get("args", [])))[:200]
            print("TIMEOUT after %ds (attempt %d of %d): %s" % (
                limit, attempt + 1, attempts, cmd), flush=True)
            timeouts["retried"] += 1
            last = exc
    return subprocess.CompletedProcess(args[0] if args else None, -999,
                                       last.stdout or b"", last.stderr or b"timed out")


subprocess.run = _run_with_timeout


class NamesOnlyDone(Exception):
    pass


def warm_up(binary):
    """The first launch after a Wine server starts also starts its helper
    processes (winedevice.exe), which inherit the launcher's stdout and stderr.
    With pipes they never reach end of file and the caller waits for them
    (this stalled runs for minutes, twice).  So the first launch writes to
    /dev/null; later ones find the helpers already running."""
    if not WIN["runner"]:
        return
    t0 = time.time()
    try:
        _real_run(tool_cmd(binary) + ["--help"], stdout=subprocess.DEVNULL,
                  stderr=subprocess.DEVNULL, timeout=900)
    except subprocess.TimeoutExpired:
        print("warm-up launch did not finish in 900 s", flush=True)
    print("warm-up launch: %.1f s" % (time.time() - t0), flush=True)
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import recover_fixtures as rf  # noqa: E402
import fluff_oracle as fo  # noqa: E402
import bp_set_bytes as sb  # noqa: E402

ROOT = os.path.dirname(HERE)

passed = 0
failed = 0
oracle = None          # FluffOracle, created in main()
failures = []
scenarios = {"clean fixtures": 0, "damage runs": 0, "nested runs": 0,
             "hostile inputs": 0, "known-limitation runs": 0}


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        failures.append(what)
        print("FAIL: " + what)


# --- Windows mode ---------------------------------------------------------
#
# With --windows-runner the tool under test is a Windows executable run through
# Wine.  Every path handed to it is converted to a drive-letter path (Z:\...),
# which is what a Windows user types, and paths in its reports are converted
# back.  The tool's host rules then apply: names Windows cannot hold are
# reported and not written, symlink entries are skipped (as the Windows CLI
# does), and Unix modes are not restored.  Checks that cannot mean the same
# thing there are skipped, counted, and printed at the end.

WIN = {"runner": None}
skipped = collections.Counter()


def host_path(p):
    """A path as the tool under test wants it."""
    if WIN["runner"] and os.path.isabs(p):
        return "Z:" + p.replace("/", "\\")
    return p


def unhost(p):
    """A path from the tool's report, as a local path."""
    if WIN["runner"] and re.match(r"^[A-Za-z]:\\", p):
        return p[2:].replace("\\", "/")
    return p


def tool_cmd(binary):
    return ([WIN["runner"]] if WIN["runner"] else []) + [binary]


def skip(reason):
    skipped[reason] += 1


_DEVICES = ("CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$")


def win_name_reason(name):
    """Why Windows cannot hold this archive name, or None.  An independent model
    of the rules, written from their description (reserved device names with or
    without an extension, forbidden characters, trailing dot or space, a
    component over 255, a path of 260 or more); used to say what the tool must
    refuse on Windows."""
    import unicodedata
    if isinstance(name, bytes):
        name = name.decode("utf-8")
    name = unicodedata.normalize("NFC", name).rstrip("/")
    if len(name) >= 260:
        return "path-too-long"
    for ch in name:
        if ch in ':<>"|?*' or ord(ch) < 32:
            return "forbidden-character"
    for comp in name.split("/"):
        if comp.endswith(".") or comp.endswith(" "):
            return "trailing-dot-or-space"
        if len(comp) > 255:
            return "component-too-long"
        base = comp.split(".")[0].rstrip(" .").upper()
        if base in _DEVICES or (len(base) == 4 and base[:3] in ("COM", "LPT") and
                                (base[3] in "123456789" or base[3] in "\u00b9\u00b2\u00b3")):
            return "reserved-device-name"
    return None


def host_expected(fx, te):
    """What extracting entry te by name must produce on this host: fx.expected(),
    except that on Windows symlinks are not extracted and names Windows cannot
    hold are refused."""
    want = fx.expected(te)
    if want is not None and WIN["runner"]:
        if want[0] == "symlink":
            skip("symlink entries (not extracted on Windows)")
            return None
        if win_name_reason(te.name) is not None:
            skip("entries whose names Windows cannot hold")
            return None
    return want




def analyze(binary, paths):
    r = subprocess.run(tool_cmd(binary) + ["analyze", "--format", "json"] +
                       [host_path(p) for p in paths],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode not in (0, 1, 2):
        return None, r.returncode, r.stderr.decode(errors="replace")
    try:
        return json.loads(r.stdout), r.returncode, ""
    except ValueError as exc:
        return None, r.returncode, "bad JSON: %s" % exc


def by_location(report):
    out = {}
    for e in report["entries"]:
        if e["status"] != "table-only":
            out[(e["volume"], e["offset"])] = e
    return out


def entry_matches(te, e):
    """Compare one truth entry with one reported entry; return a list of
    mismatching field names."""
    bad = []
    pairs = (
        ("name", te.name.decode("utf-8"), e["name"]),
        ("type", te.entry_type, e["type"]),
        ("method", te.method, e["method"]),
        ("platform", te.platform, e["platform"]),
        ("flags", te.flags, e["flags"]),
        ("mtime", te.mtime, e["mtime"]),
        ("original_size", te.original_size, e["original_size"]),
        ("block_size", te.block_size, e["block_size"]),
    )
    for name, want, got in pairs:
        if want != got:
            bad.append("%s want %r got %r" % (name, want, got))
    return bad


def entry_complete_matches(te, e):
    bad = entry_matches(te, e)
    pairs = (
        ("end_volume", te.end_volume, e.get("end_volume")),
        ("end_offset", te.end_offset, e.get("end_offset")),
        ("blocks", len(te.blocks), e.get("blocks")),
        ("compressed_size", te.compressed_size, e.get("compressed_size")),
        ("checksum", te.checksum, e.get("checksum")),
    )
    for name, want, got in pairs:
        if want != got:
            bad.append("%s want %r got %r" % (name, want, got))
    return bad


def check_clean(binary, fx):
    vs = fx.vs
    tag = "clean %s" % fx.name
    scenarios["clean fixtures"] += 1
    rep, rc, err = analyze(binary, fx.paths)
    check(rep is not None, "%s: analyze ran (%s)" % (tag, err))
    if rep is None:
        return
    check(rc == 0, "%s: exit status 0 (got %d)" % (tag, rc))
    s = rep["set"]
    check(s["complete"] and s["volume_count"] == vs.volume_count and
          s["missing"] == [] and s["count_source"] == "end-record",
          "%s: set complete with %d volumes: %r" % (tag, vs.volume_count, s))
    check(int(s["set_id"], 16) == vs.set_id, "%s: set id" % tag)
    check(rep["table"]["status"] == "trusted" and
          rep["table"]["entry_count"] == len(vs.entries),
          "%s: table trusted with %d entries" % (tag, len(vs.entries)))
    check(rep["end_record"]["status"] == "ok" and
          rep["end_record"]["at_expected_location"],
          "%s: end record at its expected place" % tag)
    check(rep["walk"]["reached_table"], "%s: walk reached the table" % tag)
    check(rep["unaccounted"] == [], "%s: no unaccounted bytes: %r" %
          (tag, rep["unaccounted"][:3]))
    got = by_location(rep)
    check(len(got) == len(vs.entries),
          "%s: %d entries reported, %d expected" %
          (tag, len(got), len(vs.entries)))
    for te in vs.entries:
        e = got.get((te.volume, te.offset))
        if e is None:
            check(False, "%s: entry %d %r not reported" % (tag, te.index, te.name))
            continue
        bad = entry_complete_matches(te, e)
        check(not bad, "%s: entry %d fields: %s" % (tag, te.index, bad))
        check(e["status"] == "ok" and e["confidence"] == "chained" and
              e["in_table"] is True and e["table_index"] == te.index and
              "table_conflict" not in e,
              "%s: entry %d ok, chained, matches the table" % (tag, te.index))
    nested = rf._nested_hits(vs)
    if nested:
        check(0 < rep["ignored_inside_blocks"] <= nested,
              "%s: %d header-shaped patterns inside data ignored (python saw %d)"
              % (tag, rep["ignored_inside_blocks"], nested))
    else:
        check(rep["ignored_inside_blocks"] == 0,
              "%s: nothing ignored inside data" % tag)
    check(rep["summary"]["entries_suspect"] == 0, "%s: no suspect entries" % tag)


def expected_set(kind, vs, damage):
    """Facts about the set that a damage kind implies; list of (what, fn)."""
    last = vs.volume_count - 1
    out = []
    if kind in ("shuffle-names", "duplicate-volume", "volume-header",
                "volume-trailer", "volume-header-and-trailer",
                "truncate-trailer"):
        out.append(("set complete", lambda r: r["set"]["complete"] and
                    r["set"]["missing"] == []))
    if kind == "delete-volume":
        gone = sorted(damage.lost_volumes)
        if gone == [last]:
            out.append(("last volume reported missing",
                        lambda r: not r["set"]["complete"] and
                        not r["set"]["last_volume_found"]))
        else:
            out.append(("missing volumes %r" % gone,
                        lambda r: r["set"]["missing"] == gone and
                        not r["set"]["complete"]))
    if kind == "duplicate-volume":
        out.append(("one duplicate reported",
                    lambda r: sum(1 for v in r["volumes"]
                                  if v["role"] == "duplicate") == 1))
    if kind in ("zero-end-and-trailer", "truncate-before-table"):
        out.append(("end record missing, table not trusted",
                    lambda r: r["end_record"]["status"] == "missing" and
                    r["table"]["status"] != "trusted"))
    if kind in ("table-magic", "table-record"):
        out.append(("table reported damaged",
                    lambda r: r["table"]["status"] == "damaged"))
    if kind in ("block-data-bit", "block-comp-size", "block-orig-size",
                "block-crc", "entry-header-bit", "entry-trailer-bit",
                "zero-entry"):
        out.append(("walk reported as broken",
                    lambda r: not r["walk"]["reached_table"]))
    if kind == "truncate-trailer":
        out.append(("table still trusted",
                    lambda r: r["table"]["status"] == "trusted"))
    if kind == "volume-header-and-trailer" and vs.volume_count > 1:
        out.append(("volume identified by name",
                    lambda r: any(v["identified_by"] == "name"
                                  for v in r["volumes"])))
    return out


def check_damage(binary, fx, kind, rng, workdir, param=None, label=None):
    vs = fx.vs
    damage, ctx = rf.make_damage(fx, kind, rng, param)
    d = os.path.join(workdir, "damaged")
    paths = rf.write_damaged(ctx, damage, d)
    # Real volumes first: on a tie the tool prefers the first file given.
    paths = sorted(paths, key=lambda p: (p.endswith(".dup"), p))
    tag = "%s / %s (%s)" % (label or fx.name, kind, damage.description)
    scenarios["damage runs"] += 1
    rep, rc, err = analyze(binary, paths)
    check(rep is not None, "%s: analyze ran: %s" % (tag, err))
    if rep is None:
        return None
    intact = set(e.index for e in damage.intact_entries(vs))
    got = by_location(rep)
    truth_at = dict(((e.volume, e.offset), e) for e in vs.entries)
    for te in vs.entries:
        e = got.get((te.volume, te.offset))
        if te.index in intact:
            if e is None:
                check(False, "%s: intact entry %d %r not found" %
                      (tag, te.index, te.name))
                continue
            bad = entry_complete_matches(te, e)
            check(e["status"] == "ok" and not bad,
                  "%s: intact entry %d %r reported ok (status %s, %s %s)" %
                  (tag, te.index, te.name, e["status"], e.get("reason"), bad))
        elif e is not None:
            check(e["status"] != "ok",
                  "%s: entry %d %r intersects damage but is reported ok" %
                  (tag, te.index, te.name))
    for loc, e in got.items():
        if loc in truth_at:
            continue
        check(e["confidence"] == "suspect",
              "%s: entry %r at %r is not in the archive and not suspect "
              "(%s)" % (tag, e["name"], loc, e["confidence"]))
    for what, fn in expected_set(kind, vs, damage):
        check(fn(rep), "%s: %s" % (tag, what))
    check(rc in (0, 1, 2), "%s: exit status %d" % (tag, rc))
    return rep, damage, ctx


def check_nested(binary, fx, rng, workdir):
    vs = fx.vs
    inner = [e for e in vs.entries if e.name.endswith(b"inner.bolt")][0]
    after = [e for e in vs.entries if e.index > inner.index]
    check(len(after) >= 1, "%s: entries follow the nested archive" % fx.name)
    for kind, i in (("block-comp-size", 0), ("block-crc", 0),
                    ("block-data-bit", 0), ("block-comp-size", 1)):
        ctx = rf.DamageContext(fx)
        s = [x for x in inner.structures if x.kind == sb.KIND_BLOCK][i]
        if kind == "block-comp-size":
            ctx.flip(s.volume, s.start, 1)
        elif kind == "block-crc":
            ctx.flip(s.volume, s.start + 8, 3)
        else:
            ctx.flip(s.volume, s.start + 12 + 100, 2)
        dmg = rf.Damage(kind, "nested %s of block %d of the holding entry" %
                        (kind, i))
        for v in ctx.data:
            dmg.changed.extend(rf._changed_ranges(v, vs.data[v], ctx.data[v]))
        paths = rf.write_damaged(ctx, dmg, os.path.join(workdir, "nested"))
        tag = "%s / %s" % (fx.name, dmg.description)
        scenarios["nested runs"] += 1
        rep, rc, err = analyze(binary, paths)
        check(rep is not None, "%s: analyze ran: %s" % (tag, err))
        if rep is None:
            continue
        got = by_location(rep)
        truth_at = dict(((e.volume, e.offset), e) for e in vs.entries)
        for te in dmg.intact_entries(vs):
            e = got.get((te.volume, te.offset))
            check(e is not None and e["status"] == "ok",
                  "%s: intact entry %r found ok" % (tag, te.name))
        he = got.get((inner.volume, inner.offset))
        check(he is not None and he["status"] == "damaged",
              "%s: the holding entry is reported damaged" % tag)
        extras = [e for loc, e in got.items() if loc not in truth_at]
        check(len(extras) >= 1,
              "%s: the damage exposes nested entries (found %d)" %
              (tag, len(extras)))
        check(all(e["confidence"] == "suspect" for e in extras),
              "%s: every nested hit is suspect: %r" %
              (tag, [(e["name"], e["confidence"]) for e in extras
                     if e["confidence"] != "suspect"]))
        check(all(e.get("inside") == he_index(rep, inner) for e in extras
                  if e.get("suspect_reason") == "inside-damaged-entry"),
              "%s: nested hits inside the damaged entry name it" % tag)


def check_nested_header_lost(binary, fx, rng, workdir):
    """The header of the entry that holds a nested archive is destroyed, with the
    table intact (and, in the second variant, the tail of the entry before it
    too, so that entry is damaged and the walk restarts at an isolated hit).
    The nested archive's entries are genuine and chain to each other, but the
    table does not list them: every one must be skipped as suspect, and nothing
    from inside the nested archive may be written."""
    vs = fx.vs
    inner = [e for e in vs.entries if e.name.endswith(b"inner.bolt")][0]
    before = [e for e in vs.entries if e.index == inner.index - 1][0]
    names = set(e.name.decode("utf-8").rstrip("/") for e in vs.entries)
    for n in list(names):               # implicit parent directories
        while "/" in n:
            n = n.rsplit("/", 1)[0]
            names.add(n)
    h = inner.structures[0]
    for variant in ("header", "previous entry's tail and header"):
        ctx = rf.DamageContext(fx)
        lo = h.start
        if variant != "header":
            b = [x for x in before.structures if x.kind == sb.KIND_BLOCK][-1]
            lo = b.start + 20
        ctx.zero(h.volume, lo, h.end)
        d = rf.Damage("nested-header", "%s zeroed" % variant)
        for v in ctx.data:
            d.changed.extend(rf._changed_ranges(v, vs.data[v], ctx.data[v]))
        tag = "%s / nested archive, %s" % (fx.name, d.description)
        scenarios["nested header lost"] += 1
        rep, out = run_partial(binary, fx, ctx, d, workdir)
        check(rep is not None, "%s: extract ran" % tag)
        if rep is None:
            continue
        written = set()
        for dp, _dn, fn in os.walk(out):
            for f in fn + _dn:
                written.add(os.path.relpath(os.path.join(dp, f), out))
        stray = [w for w in written if w not in names and
                 not (w.endswith(".damaged") and w[:-8] in names)]
        check(not stray, "%s: nothing from inside the nested archive was written (%r)"
              % (tag, stray[:3]))
        truth = set((e.volume, e.offset) for e in vs.entries)
        nested = [e for e in rep["entries"]
                  if (e["volume"], e["offset"]) not in truth]
        check(all(e["confidence"] == "suspect" for e in nested),
              "%s: every nested hit is suspect (%r)" %
              (tag, [(e["name"], e["confidence"]) for e in nested
                     if e["confidence"] != "suspect"][:3]))


limitations = []


def check_duplicate_order(binary, fx, rng, workdir):
    """Two files claim one volume number and look equally good (valid header,
    valid trailer, right size), one of them damaged.  Whichever order they are
    given in, the tool must end up using the copy whose entries verify, so
    nothing is lost and a whole extraction matches the original."""
    v = fx.vs.volume_count // 2
    damage, ctx = rf.make_damage(fx, "duplicate-volume", rng, v)
    paths = rf.write_damaged(ctx, damage, os.path.join(workdir, "dupe"))
    for order, key in (("duplicate first", lambda p: (not p.endswith(".dup"), p)),
                       ("duplicate last", lambda p: (p.endswith(".dup"), p))):
        scenarios["tied duplicate volumes"] += 1
        ordered = sorted(paths, key=key)
        rep, rc, err = analyze(binary, ordered)
        tag = "%s / duplicate of volume %d, %s" % (fx.name, v, order)
        check(rep is not None, "%s: analyze ran: %s" % (tag, err))
        if rep is None:
            continue
        chosen = [x for x in rep["volumes"] if x["role"] == "used" and x["number"] == v]
        check(len(chosen) == 1 and sum(1 for x in rep["volumes"]
                                       if x["role"] == "duplicate") == 1,
              "%s: exactly one copy used and one reported duplicate" % tag)
        check(len(chosen) == 1 and not chosen[0]["path"].endswith(".dup"),
              "%s: the intact copy is the one used" % tag)
        got = by_location(rep)
        lost = [te for te in fx.vs.entries
                if (te.volume, te.offset) not in got or
                got[(te.volume, te.offset)]["status"] != "ok"]
        check(not lost, "%s: every entry found intact (%d lost)" % (tag, len(lost)))
        base = os.path.join(workdir, "dupe-x")
        shutil.rmtree(base, ignore_errors=True)
        os.makedirs(base)
        out = os.path.join(base, "out")
        xrep, xrc, xerr = run_extract(binary, ordered, out)
        check(xrep is not None, "%s: extract ran" % tag)
        if xrep is not None:
            verify_tree(tag, fx, out, list(fx.vs.entries), siblings=())


def check_robust(binary, fx, rng, workdir):
    """Hostile and degenerate input: the tool must report, never crash."""
    d = os.path.join(workdir, "robust")
    os.makedirs(d, exist_ok=True)
    cases = {
        "empty": b"",
        "one-byte": b"B",
        "short-header": b"BOLT" + bytes(10),
        "zeros": bytes(4096),
        "random": rng.randbytes(100000),
        "text": b"hello world\n" * 1000,
    }
    src = open(fx.paths[-1], "rb").read()
    for i in range(24):
        cases["cut-%d" % i] = src[:rng.randrange(len(src))]
    for i in range(12):
        data = bytearray(src)
        for _ in range(rng.randint(1, 40)):
            data[rng.randrange(len(data))] = rng.randrange(256)
        cases["garbled-%d" % i] = bytes(data)
    for name, data in cases.items():
        scenarios["hostile inputs"] += 1
        p = os.path.join(d, name + ".bolt")
        with open(p, "wb") as f:
            f.write(data)
        rep, rc, err = analyze(binary, [p])
        check(rep is not None, "robust %s: analyze ran (rc %d) %s" %
              (name, rc, err[:200]))
    rep, rc, err = analyze(binary, [os.path.join(d, "does-not-exist.bolt")])
    check(rc == 3, "robust: missing input gives exit status 3 (got %d)" % rc)


def check_known_limitation(binary, fx, workdir):
    """The holding entry's header AND the table are damaged.  Nothing in
    phase 1 says the nested entries are data: no header contains them and no
    table excludes them.  Phase 4 (block-level resync) is expected to fix this
    by finding the orphan blocks that contain them.  Recorded, not failed; if
    the tool starts labelling them suspect this reports it as resolved."""
    vs = fx.vs
    inner = [e for e in vs.entries if e.name.endswith(b"inner.bolt")][0]
    ctx = rf.DamageContext(fx)
    h = inner.structures[0]
    ctx.flip(h.volume, h.start + 5, 1)
    t = vs.table_structures[0]
    ctx.flip(t.volume, t.start, 0)
    dmg = rf.Damage("known-limit", "holding entry header and table damaged")
    for v in ctx.data:
        dmg.changed.extend(rf._changed_ranges(v, vs.data[v], ctx.data[v]))
    paths = rf.write_damaged(ctx, dmg, os.path.join(workdir, "limit"))
    rep, rc, err = analyze(binary, paths)
    tag = "%s / %s" % (fx.name, dmg.description)
    scenarios["known-limitation runs"] += 1
    check(rep is not None, "%s: analyze ran: %s" % (tag, err))
    if rep is None:
        return
    got = by_location(rep)
    truth_at = dict(((e.volume, e.offset), e) for e in vs.entries)
    for te in dmg.intact_entries(vs):
        e = got.get((te.volume, te.offset))
        check(e is not None and e["status"] == "ok",
              "%s: intact entry %r found ok" % (tag, te.name))
    extras = [e for loc, e in got.items() if loc not in truth_at]
    loud = [e for e in extras if e["confidence"] != "suspect"]
    if loud:
        limitations.append("%s: %d nested entries are reported as real "
                           "(no containing header, no table)" %
                           (tag, len(loud)))
    else:
        print("NOTE: %s: limitation resolved, nested entries are suspect" % tag)
    # The same case through extract: reported entries that are not suspect get
    # written.  Record what lands in the output beyond the archive's own names.
    base = os.path.join(workdir, "limit-x")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base)
    out = os.path.join(base, "out")
    xrep, xrc, xerr = run_extract(binary, paths, out)
    check(xrep is not None, "%s: extract ran: %s" % (tag, xerr))
    if xrep is not None:
        by_path = truth_paths(fx)
        stray = [rel for rel in snapshot(out)
                 if rel not in by_path and not rel.endswith(".damaged") and
                 rel.startswith("inner")]
        if stray:
            limitations.append("%s: extract writes %d nested files from inside "
                               "the archive's own data (%s...)" %
                               (tag, len(stray), sorted(stray)[0]))


def he_index(rep, inner):
    for i, e in enumerate(rep["entries"]):
        if (e["volume"], e["offset"]) == (inner.volume, inner.offset):
            return i
    return None


# --- phase 2: extraction ------------------------------------------------

scenarios.update({"extract clean": 0, "extract damaged": 0,
                  "extract hostile": 0})


def run_extract(binary, paths, outdir, fmt="json", allow=(0, 1, 2), extra=()):
    r = subprocess.run(tool_cmd(binary) + ["extract", "--format", fmt, "-o",
                                           host_path(outdir)] +
                       list(extra) + [host_path(p) for p in paths],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # Any status outside the documented ones (a sanitizer exit, a crash) is a
    # failure in every extraction test, not only where the status is asserted.
    check(r.returncode in allow,
          "extract exit status %d outside %r: %s" %
          (r.returncode, allow, r.stderr.decode(errors="replace")[:300]))
    if fmt != "json":
        return None, r.returncode, r.stderr.decode(errors="replace")
    try:
        return json.loads(r.stdout), r.returncode, r.stderr.decode(errors="replace")
    except ValueError:
        return None, r.returncode, r.stderr.decode(errors="replace")


def snapshot(root):
    """relative path -> (kind, data or target, mode, mtime) for a tree."""
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames + filenames:
            p = os.path.join(dirpath, name)
            rel = os.path.relpath(p, root)
            st = os.lstat(p)
            if os.path.islink(p):
                out[rel] = ("symlink", os.readlink(p), None, None)
            elif os.path.isdir(p):
                out[rel] = ("dir", None, st.st_mode & 0o7777, int(st.st_mtime))
            else:
                with open(p, "rb") as f:
                    out[rel] = ("file", f.read(), st.st_mode & 0o7777,
                                int(st.st_mtime))
    return out


def truth_paths(fx):
    """archive path -> truth entry, for the entry types extracted by name."""
    out = {}
    for te in fx.vs.entries:
        if te.entry_type in (0, 3, 4):
            out.setdefault(te.name.decode("utf-8").rstrip("/"), te)
    return out


def verify_tree(tag, fx, outdir, required, siblings=(), neutral=False):
    """Safety and correctness of an extraction.  Everything written under a
    real name must be exactly right; required entries must be present; the
    only other things allowed are NAME.damaged files and parent directories."""
    snap = snapshot(outdir)
    by_path = truth_paths(fx)
    parents = set()
    for pth in by_path:
        parts = pth.split("/")
        for i in range(1, len(parts)):
            parents.add("/".join(parts[:i]))
    for rel, (kind, data, _mode, _mtime) in snap.items():
        if rel.endswith(".damaged"):
            check(kind == "file", "%s: %s is a file" % (tag, rel))
            continue
        te = by_path.get(rel)
        if te is None:
            check(kind == "dir" and rel in parents,
                  "%s: unexpected %s %r in the output" % (tag, kind, rel))
            continue
        want = fx.expected(te) if neutral else host_expected(fx, te)
        if want is None:
            check(False, "%s: %r is not extractable by name here" % (tag, rel))
        elif want[0] == "dir":
            check(kind == "dir", "%s: %r should be a directory" % (tag, rel))
        else:
            check(kind == want[0] and data == want[1],
                  "%s: %r has the wrong content or kind (%s)" % (tag, rel, kind))
    for te in required:
        want = fx.expected(te) if neutral else host_expected(fx, te)
        if want is None:
            continue
        rel = te.name.decode("utf-8").rstrip("/")
        got = snap.get(rel)
        ok = got is not None and got[0] == want[0] and (
            want[0] == "dir" or got[1] == want[1])
        check(ok, "%s: required entry %d %r was not extracted correctly" %
              (tag, te.index, te.name))
    # Nothing may appear beside the output directory.
    parent = os.path.dirname(os.path.abspath(outdir))
    check(sorted(os.listdir(parent)) == sorted(
              [os.path.basename(outdir)] + list(siblings)),
          "%s: files were written outside the output directory: %r" %
          (tag, os.listdir(parent)))
    return snap


def check_extract_clean(binary, boltpack, fx, workdir):
    tag = "extract clean %s" % fx.name
    scenarios["extract clean"] += 1
    base = os.path.join(workdir, "xc-" + fx.name)
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base)
    out = os.path.join(base, "out")
    rep, rc, err = run_extract(binary, fx.paths, out)
    check(rep is not None, "%s: extract ran (%s)" % (tag, err))
    if rep is None:
        return
    supplementary = any(e.entry_type in (1, 2) for e in fx.vs.entries)
    windows_skips = WIN["runner"] and any(
        e.entry_type == 3 or win_name_reason(e.name) is not None
        for e in fx.vs.entries)
    check(rc == (1 if supplementary or windows_skips else 0),
          "%s: exit status %d (supplementary entries: %s)" %
          (tag, rc, supplementary))
    snap = verify_tree(tag, fx, out, fx.vs.entries)
    counts = rep["extraction"]["counts"]
    allowed = ("verified", "created", "skipped-supplementary")
    if WIN["runner"]:
        allowed += ("skipped-symlink", "skipped-host-name")
    check(all(n == 0 for k, n in counts.items() if k not in allowed),
          "%s: only verified/created/supplementary outcomes: %r" % (tag, counts))
    if WIN["runner"]:
        # The model says exactly which entries Windows must refuse and why.
        by_loc = dict(((e.volume, e.offset), e) for e in fx.vs.entries)
        for r_ in rep["extraction"]["results"]:
            te_ = by_loc.get((r_["volume"], r_["offset"]))
            if te_ is None:
                continue
            reason = win_name_reason(te_.name)
            if te_.entry_type == 3:
                check(r_["outcome"] == "skipped-symlink",
                      "%s: symlink %r is skipped (%s)" % (tag, r_["name"], r_["outcome"]))
            elif reason is not None:
                check(r_["outcome"] == "skipped-host-name" and r_["detail"] == reason,
                      "%s: %r refused for %r (got %s %r)" %
                      (tag, r_["name"], reason, r_["outcome"], r_["detail"]))
            else:
                check(r_["outcome"] != "skipped-host-name",
                      "%s: %r wrongly refused (%r)" % (tag, r_["name"], r_["detail"]))
    # Metadata: mode and mtime come from the entry flags.
    for te in fx.vs.entries:
        if te.entry_type not in (0, 4):
            continue
        got = snap.get(te.name.decode("utf-8").rstrip("/"))
        if got is None:
            continue
        if te.flags & 0x4000:
            check(got[3] == te.mtime, "%s: mtime of %r: %r != %r" %
                  (tag, te.name, got[3], te.mtime))
        if te.flags & 0x8000:
            if WIN["runner"]:
                skip("Unix mode bits (not restored on Windows)")
                continue
            want = te.flags & 0o1777
            check(got[2] == want, "%s: mode of %r: %o != %o" %
                  (tag, te.name, got[2], want))
    # Second oracle: the CLI's own extraction of the same archive.
    if fx.payloads is None:
        ref = os.path.join(base, "ref")
        last = fx.paths[-1]
        r = subprocess.run([boltpack, "extract", "-qq", "-o", ref, last],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        check(r.returncode == 0, "%s: reference extraction ran: %s" %
              (tag, r.stdout.decode(errors="replace")[:200]))
        if r.returncode == 0:
            want = snapshot(ref)
            if WIN["runner"]:
                # the CLI on Linux extracts symlinks and every name; the tool on
                # Windows does not
                refused = set(te.name.decode("utf-8").rstrip("/")
                              for te in fx.vs.entries
                              if te.entry_type == 3 or win_name_reason(te.name))
                for k in list(want):
                    if k in refused or any(k.startswith(r_ + "/") for r_ in refused):
                        del want[k]
                        skip("CLI-extracted entries Windows does not extract")
            check(sorted(want) == sorted(snap),
                  "%s: same file set as the CLI (%r)" %
                  (tag, sorted(set(want) ^ set(snap))[:5]))
            by_path = truth_paths(fx)
            for rel in want:
                if rel not in snap:
                    continue
                te = by_path.get(rel)
                # An implicit parent directory has no archive entry, so its
                # mtime is just when each run last wrote into it.
                has_mtime = te is not None and bool(te.flags & 0x4000)
                same_mtime = (not has_mtime) or want[rel][3] == snap[rel][3]
                if WIN["runner"]:
                    skip("Unix mode bits (not restored on Windows)")
                    same_mode = want[rel][:2] == snap[rel][:2]
                else:
                    same_mode = want[rel][:3] == snap[rel][:3]
                check(same_mode and same_mtime,
                      "%s: %r differs from the CLI's extraction "
                      "(mode/mtime %r vs %r)" %
                      (tag, rel, want[rel][2:], snap[rel][2:]))


def check_extract_damage(binary, fx, kind, rng, workdir, param=None, label=None,
                         extra_ctx=None):
    damage, ctx = rf.make_damage(fx, kind, rng, param)
    if extra_ctx is not None:
        extra_ctx(ctx, damage)
    base = os.path.join(workdir, "xd")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base)
    paths = rf.write_damaged(ctx, damage, os.path.join(base, "in"))
    paths = sorted(paths, key=lambda p: (p.endswith(".dup"), p))
    out = os.path.join(base, "out")
    tag = "extract %s / %s (%s)" % (label or fx.name, kind, damage.description)
    scenarios["extract damaged"] += 1
    rep, rc, err = run_extract(binary, paths, out)
    check(rep is not None and rc in (0, 1, 2),
          "%s: extract ran (rc %d) %s" % (tag, rc, err[:200]))
    if rep is None:
        return None
    intact = damage.intact_entries(fx.vs)
    verify_tree(tag, fx, out, intact, siblings=("in",))
    for te in fx.vs.entries:
        res = find_result(rep, te)
        if res is not None and res["outcome"] == "partial":
            verify_partial(tag, fx, te, res, out)
    return rep, damage, out


def check_rebuilt_header(binary, fx, rng, workdir):
    """Header destroyed, data and table intact: the entry is rebuilt from the
    table's record and checked against the record's checksum."""
    n = 0
    for _ in range(10):
        te = rng.choice([e for e in fx.vs.entries
                         if e.entry_type == 0 and e.original_size > 0])
        ctx = rf.DamageContext(fx)
        h = te.structures[0]
        ctx.flip(h.volume, h.start + rng.randrange(h.length), rng.randrange(8))
        damage = rf.Damage("header", "header of entry %d" % te.index)
        for v in ctx.data:
            damage.changed.extend(rf._changed_ranges(v, fx.vs.data[v], ctx.data[v]))
        base = os.path.join(workdir, "xr")
        shutil.rmtree(base, ignore_errors=True)
        os.makedirs(base)
        paths = rf.write_damaged(ctx, damage, os.path.join(base, "in"))
        out = os.path.join(base, "out")
        tag = "extract %s / %s" % (fx.name, damage.description)
        scenarios["extract damaged"] += 1
        rep, rc, err = run_extract(binary, paths, out)
        check(rep is not None, "%s: extract ran: %s" % (tag, err))
        if rep is None:
            continue
        rel = te.name.decode("utf-8").rstrip("/")
        res = [r for r in rep["extraction"]["results"]
               if r["name"] == te.name.decode("utf-8") and r["volume"] == te.volume]
        check(len(res) == 1 and res[0]["outcome"] == "verified-by-table" and
              res[0]["header_rebuilt"],
              "%s: rebuilt from the table and verified by it: %r" % (tag, res))
        snap = verify_tree(tag, fx, out, [te], siblings=("in",))
        check(rel in snap, "%s: file written" % tag)
        n += 1


def check_trailer_cases(binary, fx, rng, workdir):
    """Trailer destroyed: with the table, verified by the table; with the table
    also destroyed, written but only complete-unverified."""
    te = rng.choice([e for e in fx.vs.entries
                     if e.entry_type == 0 and e.original_size > 0])
    t = te.structures[-1]
    for with_table_damage in (False, True):
        ctx = rf.DamageContext(fx)
        ctx.flip(t.volume, t.start + 2, 1)
        if with_table_damage:
            ts = fx.vs.table_structures[0]
            ctx.flip(ts.volume, ts.start, 0)
        damage = rf.Damage("trailer", "trailer of entry %d%s" % (
            te.index, " and the table" if with_table_damage else ""))
        for v in ctx.data:
            damage.changed.extend(rf._changed_ranges(v, fx.vs.data[v], ctx.data[v]))
        base = os.path.join(workdir, "xt")
        shutil.rmtree(base, ignore_errors=True)
        os.makedirs(base)
        paths = rf.write_damaged(ctx, damage, os.path.join(base, "in"))
        out = os.path.join(base, "out")
        tag = "extract %s / %s" % (fx.name, damage.description)
        scenarios["extract damaged"] += 1
        rep, rc, err = run_extract(binary, paths, out)
        check(rep is not None, "%s: extract ran: %s" % (tag, err))
        if rep is None:
            continue
        res = [r for r in rep["extraction"]["results"]
               if r["name"] == te.name.decode("utf-8") and r["volume"] == te.volume]
        want = "complete-unverified" if with_table_damage else "verified-by-table"
        check(len(res) == 1 and res[0]["outcome"] == want,
              "%s: outcome %s: %r" % (tag, want, res))
        verify_tree(tag, fx, out, [te], siblings=("in",))


def build_hostile(workdir):
    """Byte-built archives whose entries try to escape, collide, or lie."""
    bab_entry = rf.bab.entry
    rng = random.Random(3)
    d = os.path.join(workdir, "hostile")
    os.makedirs(d, exist_ok=True)
    cases = {}

    def put(name, entries):
        p = os.path.join(d, name + ".bolt")
        with open(p, "wb") as f:
            f.write(rf.bab.build7({"entries": entries}))
        cases[name] = p

    e = bab_entry
    put("symlinks", [
        e("target.txt", b"target data"),
        e("good", b"target.txt", entry_type=3),
        e("escape", b"../outside", entry_type=3),
        e("deep/escape", b"../../outside", entry_type=3),
        e("absolute", b"/etc/passwd", entry_type=3),
        e("drive", b"C:/x", entry_type=3),
        e("backslash", b"..\\\\outside", entry_type=3),
        e("evil", b"sub", entry_type=3),
        e("evil/pwn.txt", b"must stay inside"),
    ])
    put("duplicates", [e("same.txt", b"first"), e("same.txt", b"second")])
    # A damaged "foo" would be written as foo.damaged; a real entry of that name
    # must win it.
    foo = e("foo", rng.randbytes(3000), block_size=1000)
    put("collision", [foo, e("foo.damaged", b"a real file with a partial's name")])
    raw = bytearray(open(os.path.join(d, "collision.bolt"), "rb").read())
    path = os.path.join(d, "collision.bolt")
    vs = rf.sb.parse_set([path])
    blk = [x for x in vs.entries[0].structures if x.kind == rf.sb.KIND_BLOCK][1]
    raw[blk.start + 12 + 10] ^= 0x20
    with open(path, "wb") as f:
        f.write(bytes(raw))
    bad_sum = e("badsum.txt", b"payload with a wrong checksum")
    bad_sum["checksum"] ^= 1
    put("badsum", [e("fine.txt", b"fine"), bad_sum])
    # A block whose CRC is right but whose compressed bytes are not valid.
    garbage = e("garbage.txt", b"x" * 100)
    garbage.update(method=2, blocks=[(100, rng.randbytes(40))])
    put("undecodable", [e("fine.txt", b"fine"), garbage])
    return cases


def check_hostile(binary, workdir):
    cases = build_hostile(workdir)

    def fresh(name):
        base = os.path.join(workdir, "xh-" + name)
        shutil.rmtree(base, ignore_errors=True)
        os.makedirs(base)
        return base

    def results(rep):
        return dict((r["name"], r) for r in rep["extraction"]["results"])

    # symlinks
    scenarios["extract hostile"] += 1
    base = fresh("symlinks")
    out = os.path.join(base, "out")
    rep, rc, err = run_extract(binary, [cases["symlinks"]], out)
    check(rep is not None, "hostile symlinks: extract ran: %s" % err)
    if rep:
        res = results(rep)
        snap = snapshot(out)
        check(sorted(os.listdir(base)) == ["out"], "hostile symlinks: nothing outside the output")
        if WIN["runner"]:
            # Windows does not extract symlinks at all (as the CLI): every link
            # entry is skipped, none exists, and the file "through" the link is
            # an ordinary file in an ordinary directory.
            for lk in ("good", "escape", "deep/escape", "absolute", "drive",
                       "backslash", "evil"):
                check(lk not in snap or snap[lk][0] == "dir"
                      if lk == "evil" else lk not in snap,
                      "hostile symlinks: %s not created on Windows" % lk)
                check(res[lk]["outcome"] == "skipped-symlink",
                      "hostile symlinks: %s skipped as a symlink (%s)" %
                      (lk, res[lk]["outcome"]))
            check(snap.get("evil/pwn.txt", (None,))[0] == "file" and
                  snap["evil"][0] == "dir",
                  "hostile symlinks: the file is an ordinary file in a real directory")
            skip("symlink target safety (symlinks are not extracted on Windows)")
        else:
            check(snap.get("good") == ("symlink", "target.txt", None, None),
                  "hostile symlinks: safe link created")
            for bad in ("escape", "deep/escape", "absolute", "drive", "backslash"):
                check(bad not in snap and res[bad]["outcome"] == "failed",
                      "hostile symlinks: %s refused (%s)" % (bad, res[bad]["outcome"]))
            check(snap.get("evil/pwn.txt", (None,))[0] == "file" and
                  snap["evil"][0] == "dir",
                  "hostile symlinks: a file named through a link stays a real directory")
            check(res["evil"]["outcome"] == "skipped-duplicate-name",
                  "hostile symlinks: the colliding link was skipped (%s)" %
                  res["evil"]["outcome"])
    # an existing, non-empty output directory is refused before writing
    scenarios["extract hostile"] += 1
    base = fresh("exists")
    out = os.path.join(base, "out")
    os.makedirs(out)
    with open(os.path.join(out, "precious"), "wb") as f:
        f.write(b"keep")
    rep, rc, err = run_extract(binary, [cases["duplicates"]], out, allow=(3,))
    check(rc == 3 and os.listdir(out) == ["precious"],
          "hostile: non-empty output directory refused (rc %d)" % rc)
    # an output directory that is a symlink is refused
    if WIN["runner"]:
        # Wine follows a Unix symlink as if it were an ordinary directory and
        # does not show it as a reparse point, so this cannot be tested there.
        skip("output directory that is a symlink (Wine hides reparse points)")
    else:
        scenarios["extract hostile"] += 1
        base = fresh("linkroot")
        os.makedirs(os.path.join(base, "real"))
        os.symlink("real", os.path.join(base, "out"))
        rep, rc, err = run_extract(binary, [cases["duplicates"]],
                                   os.path.join(base, "out"), allow=(3,))
        check(rc == 3 and os.listdir(os.path.join(base, "real")) == [],
              "hostile: output directory that is a symlink refused (rc %d)" % rc)
    # duplicate names
    scenarios["extract hostile"] += 1
    base = fresh("dups")
    out = os.path.join(base, "out")
    rep, rc, err = run_extract(binary, [cases["duplicates"]], out)
    check(rep is not None, "hostile duplicates: extract ran: %s" % err)
    if rep:
        snap = snapshot(out)
        # (on Windows the second one is refused by the host name rules, as a
        # name that collides, before the duplicate check is reached)
        dup_kind = "skipped-host-name" if WIN["runner"] else "skipped-duplicate-name"
        check(snap.get("same.txt", (0, 0))[1] == b"first" and
              rep["extraction"]["counts"][dup_kind] == 1,
              "hostile duplicates: first kept, second skipped, nothing overwritten")
    # a verified file keeps its name against a partial recovery's .damaged
    scenarios["extract hostile"] += 1
    base = fresh("collision")
    out = os.path.join(base, "out")
    rep, rc, err = run_extract(binary, [cases["collision"]], out)
    check(rep is not None, "hostile collision: extract ran: %s" % err)
    if rep:
        snap = snapshot(out)
        check(snap.get("foo.damaged", (0, 0))[1] == b"a real file with a partial's name",
              "hostile collision: the real foo.damaged kept its name (%r)" % sorted(snap))
        check(results(rep)["foo"]["outcome"] in ("skipped-duplicate-name", "skipped-partial") or
              (WIN["runner"] and results(rep)["foo"]["outcome"] == "skipped-host-name"),
              "hostile collision: the partial of foo was not written over it (%s)" %
              results(rep)["foo"]["outcome"])
    # wrong payload checksum behind a valid trailer
    scenarios["extract hostile"] += 1
    base = fresh("badsum")
    out = os.path.join(base, "out")
    rep, rc, err = run_extract(binary, [cases["badsum"]], out)
    check(rep is not None, "hostile badsum: extract ran: %s" % err)
    if rep:
        snap = snapshot(out)
        check("badsum.txt" not in snap and "badsum.txt.damaged" in snap,
              "hostile badsum: kept only as .damaged (%r)" % sorted(snap))
        check(results(rep)["badsum.txt"]["detail"] == "payload-checksum-mismatch",
              "hostile badsum: reported as a checksum mismatch")
        check(snap.get("fine.txt", (0, 0))[1] == b"fine",
              "hostile badsum: the good entry is still extracted")
    # block CRC fine, codec data invalid
    scenarios["extract hostile"] += 1
    base = fresh("garbage")
    out = os.path.join(base, "out")
    rep, rc, err = run_extract(binary, [cases["undecodable"]], out)
    check(rep is not None, "hostile undecodable: extract ran: %s" % err)
    if rep:
        snap = snapshot(out)
        check("garbage.txt" not in snap and
              results(rep)["garbage.txt"]["outcome"] in ("failed", "skipped-partial"),
              "hostile undecodable: no file under the real name (%r)" % sorted(snap))


# --- host name rules ------------------------------------------------------

scenarios.update({"host names": 0, "path styles": 0})

# (name, entry type, reason Windows must give for refusing it, or None).  The
# table is the oracle: written from the rules, not from the tool.  Order matters
# where names collide (the first claimant keeps the name).
HOST_NAMES = [
    ("ok.txt", 0, None), ("good/ok2.txt", 0, None),
    ("aux.txt", 0, "reserved-device-name"), ("Nul", 0, "reserved-device-name"),
    ("COM1", 0, "reserved-device-name"), ("LPT9", 0, "reserved-device-name"),
    ("COM\u00b2", 0, "reserved-device-name"),
    ("CON.damaged", 0, "reserved-device-name"),
    ("dir/PRN/inner.txt", 0, "reserved-device-name"),
    ("trail.", 0, "trailing-dot-or-space"), ("space ", 0, "trailing-dot-or-space"),
    ("dotdir./f", 0, "trailing-dot-or-space"),
    ("ab:c", 0, "forbidden-character"), ("a<b", 0, "forbidden-character"),
    ("a>b", 0, "forbidden-character"), ("a\"b", 0, "forbidden-character"),
    ("a|b", 0, "forbidden-character"), ("a?b", 0, "forbidden-character"),
    ("a*b", 0, "forbidden-character"), ("ctl\x01x", 0, "forbidden-character"),
    ("x" * 256, 0, "component-too-long"),
    ("/".join(["p" * 60] * 5), 0, "path-too-long"),
    ("README", 0, None), ("readme", 0, "name-collision"),
    ("caf\u00e9", 0, None), ("cafe\u0301", 0, "name-collision"),
    ("x", 0, None), ("X/y", 0, "file-and-directory-conflict"),
    ("Dd/", 4, None), ("dd/", 4, "name-collision"),
    ("a.damaged", 0, None),
]


# A 256-byte component is over Linux's own limit (255 bytes), so it cannot be
# created there either; the tool reports a failure rather than a host-name skip.
LINUX_CANNOT_CREATE = {"x" * 256}


def check_host_names(binary, workdir):
    """Names the host cannot hold or that collide.  On Linux every name is
    written.  On Windows each refused name is reported with its reason and
    nothing is written for it; the first claimant keeps a name; a partial
    recovery A.damaged never takes the name of a real a.damaged, whatever the
    case.  The reported outcome and reason are asserted, not just the file's
    absence (Wine's own filesystem would refuse some of these anyway)."""
    e = rf.bab.entry
    rng = random.Random(11)
    entries = []
    payloads = {}
    for name, etype, _why in HOST_NAMES:
        data = b"" if etype == 4 else b"payload of " + name.encode("utf-8")
        entries.append(e(name, data, entry_type=etype))
        payloads[name] = data
    a_payload = rng.randbytes(3000)
    entries.insert(5, e("A", a_payload, block_size=1000))
    base = os.path.join(workdir, "hostnames")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base)
    src = os.path.join(base, "names.bolt")
    with open(src, "wb") as f:
        f.write(rf.bab.build7({"entries": entries}))
    vs = rf.sb.parse_set([src])
    raw = bytearray(open(src, "rb").read())
    blk = [x for x in vs.entries[5].structures if x.kind == rf.sb.KIND_BLOCK][1]
    raw[blk.start + 12 + 10] ^= 0x20            # A's middle block fails its CRC
    with open(src, "wb") as f:
        f.write(bytes(raw))
    out = os.path.join(base, "out")
    tag = "host names (%s)" % ("Windows" if WIN["runner"] else "Linux")
    scenarios["host names"] += 1
    rep, rc, err = run_extract(binary, [src], out)
    check(rep is not None and rc == 1, "%s: extract ran, exit 1 (rc %s, %s)" %
          (tag, rc, err[:120]))
    if rep is None:
        return
    res = dict((r["name"], r) for r in rep["extraction"]["results"])
    snap = snapshot(out)
    written = set()
    for name, etype, why in HOST_NAMES:
        r = res.get(name)
        check(r is not None, "%s: %r is in the report" % (tag, name))
        if r is None:
            continue
        refused = WIN["runner"] and why is not None
        if not WIN["runner"] and name in LINUX_CANNOT_CREATE:
            check(r["outcome"] == "failed" and
                  r["detail"] == "cannot create the output file",
                  "%s: a 256-byte name is over the filesystem's limit (%s %r)" %
                  (tag, r["outcome"], r["detail"]))
            continue
        if refused:
            check(r["outcome"] == "skipped-host-name" and r["detail"] == why,
                  "%s: %r refused for %r (got %s %r)" %
                  (tag, name, why, r["outcome"], r["detail"]))
        else:
            want = "created" if etype == 4 else "verified"
            check(r["outcome"] == want,
                  "%s: %r written (got %s %r)" % (tag, name, r["outcome"], r["detail"]))
            rel = name.rstrip("/")
            written.add(rel)
            parts = rel.split("/")
            for i in range(1, len(parts)):
                written.add("/".join(parts[:i]))
            if etype == 0:
                got = snap.get(rel)
                check(got is not None and got[0] == "file" and
                      got[1] == payloads[name],
                      "%s: %r has the right content" % (tag, name))
    # The partial: on Linux A.damaged is written beside the real a.damaged; on
    # Windows A.damaged would be the same file name, so it is refused.
    ra = res.get("A")
    if WIN["runner"]:
        check(ra is not None and ra["outcome"] == "skipped-host-name" and
              ra["detail"] == "name-collision",
              "%s: the partial A.damaged yields to the real a.damaged (%r)" %
              (tag, ra and (ra["outcome"], ra["detail"])))
    else:
        check(ra is not None and ra["outcome"] == "partial",
              "%s: A is recovered as A.damaged (%r)" % (tag, ra and ra["outcome"]))
        written.add("A.damaged")
        got = snap.get("A.damaged")
        check(got is not None and len(got[1]) == 3000 and
              got[1][:1000] == a_payload[:1000] and got[1][2000:] == a_payload[2000:],
              "%s: A.damaged holds the intact blocks" % tag)
    # Nothing else is on disk: no refused name, in any spelling, and no parent
    # directory that only a refused name would have needed.
    check(set(snap) == written,
          "%s: exactly the expected files exist (extra %r, missing %r)" %
          (tag, sorted(set(snap) - written)[:5], sorted(written - set(snap))[:5]))
    real = snap.get("a.damaged")
    check(real is not None and real[1] == payloads["a.damaged"],
          "%s: the real a.damaged is intact" % tag)


def check_path_styles(binary, boltpack, fx, workdir):
    """The ways a user spells paths: relative to the current directory, with a
    trailing separator on the output directory, and (Windows) forward slashes
    after the drive letter.  The result must be the same extraction."""
    base = os.path.join(workdir, "pathstyles")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base)
    vols = []
    for p in fx.paths:
        shutil.copy(p, base)
        vols.append(os.path.basename(p))
    styles = [("relative", "outrel", vols, base, os.path.join(base, "outrel")),
              ("relative, trailing slash", "outrel2/", vols, base,
               os.path.join(base, "outrel2"))]
    if WIN["runner"]:
        absout = host_path(os.path.join(base, "outwin"))
        styles.append(("drive, trailing backslash", absout + "\\",
                       [host_path(os.path.join(base, v)) for v in vols], base,
                       os.path.join(base, "outwin")))
        styles.append(("drive, forward slashes",
                       host_path(os.path.join(base, "outwin2")).replace("\\", "/"),
                       [host_path(os.path.join(base, v)).replace("\\", "/")
                        for v in vols], base, os.path.join(base, "outwin2")))
    for label, outarg, volargs, cwd, outdir in styles:
        scenarios["path styles"] += 1
        tag = "path style %s / %s" % (label, fx.name)
        r = subprocess.run(tool_cmd(binary) + ["extract", "--format", "json",
                                               "-o", outarg] + volargs,
                           cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        check(r.returncode in (0, 1), "%s: exit status %d (%s)" %
              (tag, r.returncode, r.stderr.decode(errors="replace")[:150]))
        if not os.path.isdir(outdir):
            check(False, "%s: output directory %r exists" % (tag, outdir))
            continue
        snap = snapshot(outdir)
        for te in fx.vs.entries:
            want = host_expected(fx, te)
            if want is None or want[0] != "file":
                continue
            got = snap.get(te.name.decode("utf-8"))
            check(got is not None and got[1] == want[1],
                  "%s: %r extracted correctly" % (tag, te.name))
    # rebuild with a relative output name
    scenarios["path styles"] += 1
    tag = "path style relative rebuild / %s" % fx.name
    r = subprocess.run(tool_cmd(binary) + ["rebuild", "--format", "json", "-o",
                                           "rebuilt.bolt"] + vols,
                       cwd=base, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    check(r.returncode == 0, "%s: exit status %d" % (tag, r.returncode))
    t = subprocess.run([boltpack, "test", os.path.join(base, "rebuilt.bolt")],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    check(t.returncode == 0, "%s: the CLI tests the new archive" % tag)


# --- phase 3: partial recovery -------------------------------------------

scenarios.update({"partial: damaged blocks": 0, "partial: damaged size field": 0,
                  "partial: data+size, full block": 0,
                  "partial: data+size, short block": 0,
                  "partial: trailer+table lost": 0, "partial: volume deleted": 0,
                  "partial: wrong size, data intact": 0,
                  "partial safety": 0, "lost middle volume": 0,
                  "partial: suffix after a gap": 0,
                  "partial: unknown size deduced": 0,
                  "partial: nested holding entry": 0,
                  "partial: size says full": 0, "partial: two size faults": 0,
                  "tied duplicate volumes": 0, "partial: fluff taint": 0, "excised blocks": 0, "nested header lost": 0,
                  "partial: fluff size after loss": 0,
                  "partial: fluff size after loss (larger)": 0,
                  "partial: fluff size after loss (smaller)": 0,
                  "partial: fluff size after loss (comp)": 0})


def block_ranges(te):
    """Output byte range of each block of a truth entry."""
    out, pos = [], 0
    for orig, _comp in te.blocks:
        out.append((pos, pos + orig))
        pos += orig
    return out


def merge(ranges):
    """Union of ranges: overlapping and adjacent ones are joined."""
    out = []
    for a, b in sorted(ranges):
        if out and out[-1][1] >= a:
            out[-1] = (out[-1][0], max(out[-1][1], b))
        else:
            out.append((a, b))
    return out


def find_result(rep, te):
    for r in rep["extraction"]["results"]:
        if r["volume"] == te.volume and r["offset"] == te.offset and \
                r["name"] == te.name.decode("utf-8"):
            return r
    return None


def verify_partial(tag, fx, te, res, outdir):
    """A .damaged file must be the right size; ok ranges must hold exactly the
    original bytes; lost ranges must be zero; ranges must tile the payload."""
    scenarios["partial safety"] += 1
    rel = te.name.decode("utf-8").rstrip("/") + ".damaged"
    path = os.path.join(outdir, rel)
    check(os.path.isfile(path), "%s: %s written" % (tag, rel))
    if not os.path.isfile(path):
        return
    with open(path, "rb") as f:
        got = f.read()
    want = fx.expected(te)[1]
    check(len(got) == te.original_size == len(want),
          "%s: %s has the full length (%d of %d)" % (tag, rel, len(got), te.original_size))
    pos = 0
    ok_bytes = 0
    for rg in res["ranges"]:
        if rg["start"] != pos or rg["end"] <= rg["start"]:
            check(False, "%s: ranges tile the payload (%r)" % (tag, res["ranges"][:4]))
            return
        pos = rg["end"]
        seg = got[rg["start"]:rg["end"]]
        if rg["status"] == "ok":
            ok_bytes += rg["end"] - rg["start"]
            check(seg == want[rg["start"]:rg["end"]],
                  "%s: ok range %d-%d of %s holds the original bytes" %
                  (tag, rg["start"], rg["end"], rel))
        else:
            check(seg == bytes(len(seg)) and rg["reason"],
                  "%s: lost range %d-%d of %s is zero-filled with a reason" %
                  (tag, rg["start"], rg["end"], rel))
    check(pos == te.original_size and ok_bytes == res["bytes_ok"] and
          res["bytes_ok"] + res["bytes_lost"] == te.original_size,
          "%s: ranges cover the payload and the totals add up" % tag)


def partial_damage(fx, te, flips):
    """flips: list of (block index, kind) with kind in data|crc.  Returns the
    damaged context and Damage."""
    ctx = rf.DamageContext(fx)
    blocks = [s for s in te.structures if s.kind == sb.KIND_BLOCK]
    for i, kind in flips:
        s = blocks[i]
        if kind == "data":
            ctx.flip(s.volume, s.start + 12 + (s.length - 12) // 2, 3)
        elif kind == "crc":
            ctx.flip(s.volume, s.start + 9, 2)
        elif kind == "orig":
            ctx.flip(s.volume, s.start + 4, 1)
        else:
            ctx.flip(s.volume, s.start, 1)
    d = rf.Damage("partial", "blocks %s of entry %d" % (flips, te.index))
    for v in ctx.data:
        d.changed.extend(rf._changed_ranges(v, fx.vs.data[v], ctx.data[v]))
    return ctx, d


last_rc = [None]


def run_partial(binary, fx, ctx, d, workdir, header_te=None, extra=()):
    base = os.path.join(workdir, "xp")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base)
    paths = rf.write_damaged(ctx, d, os.path.join(base, "in"))
    out = os.path.join(base, "out")
    scenarios["extract damaged"] += 1
    rep, rc, err = run_extract(binary, paths, out, extra=extra)
    last_rc[0] = rc
    return rep, out


def check_partial_exact(binary, fx, rng, workdir, header_destroyed=False):
    """Damage chosen blocks of one entry; the reported lost ranges must be
    exactly those blocks (independent codecs), or those blocks plus the
    later compressed blocks that need their history (Fluff)."""
    method = None
    cands = [e for e in fx.vs.entries if e.entry_type == 0 and len(e.blocks) >= 4]
    if not cands:
        return
    te = rng.choice(cands)
    n = len(te.blocks)
    # Damaged blocks are chosen among full blocks and the entry's last block:
    # a damaged short end-of-volume block has an unverifiable size, and the
    # tool stops there (recorded as a limitation).
    eligible = [i for i, (orig, _c) in enumerate(te.blocks)
                if orig == te.block_size or i == n - 1]
    if len(eligible) < 2:
        return
    for flips_n in (1, 2, 3):
        idx = sorted(rng.sample(eligible, min(flips_n, len(eligible) - 1)))
        flips = [(i, rng.choice(("data", "crc"))) for i in idx]
        ctx, d = partial_damage(fx, te, flips)
        if header_destroyed:
            h = te.structures[0]
            ctx.flip(h.volume, h.start + 6, 0)
            d.changed = []
            for v in ctx.data:
                d.changed.extend(rf._changed_ranges(v, fx.vs.data[v], ctx.data[v]))
        tag = "partial %s / %s%s" % (fx.name, d.description,
                                     " + header" if header_destroyed else "")
        scenarios["partial: damaged blocks"] += 1
        rep, out = run_partial(binary, fx, ctx, d, workdir)
        check(rep is not None, "%s: extract ran" % tag)
        if rep is None:
            continue
        res = find_result(rep, te)
        ranges = block_ranges(te)
        bad = set(i for i, _k in flips)
        lost_expected = expected_lost(fx, te, bad)
        if lost_expected == [(0, te.original_size)]:
            # Nothing can be recovered: no file, reported as skipped.
            check(res is not None and res["outcome"] == "skipped-partial" and
                  not os.path.exists(os.path.join(
                      out, te.name.decode("utf-8") + ".damaged")),
                  "%s: nothing recoverable, nothing written (%r)" %
                  (tag, res and res["outcome"]))
            continue
        check(res is not None and res["outcome"] == "partial",
              "%s: reported partial (%r)" % (tag, res and res["outcome"]))
        if res is None or res["outcome"] != "partial":
            continue
        if header_destroyed:
            check(res["header_rebuilt"], "%s: header rebuilt from the table" % tag)
        verify_partial(tag, fx, te, res, out)
        lost_got = merge([(r["start"], r["end"]) for r in res["ranges"]
                          if r["status"] == "lost"])
        check(lost_got == lost_expected,
              "%s: lost ranges %r, expected %r" %
              (tag, lost_got[:6], lost_expected[:6]))


def verify_whole(tag, fx, te, res, outdir):
    """The entry came back whole: written under its real name, verified, and
    byte for byte what the archive was made from."""
    rel = te.name.decode("utf-8").rstrip("/")
    path = os.path.join(outdir, rel)
    check(res is not None and res["outcome"] in ("verified", "verified-by-table"),
          "%s: recovered whole and verified (%r)" % (tag, res and res["outcome"]))
    check(os.path.isfile(path), "%s: %s written under its real name" % (tag, rel))
    if os.path.isfile(path):
        with open(path, "rb") as f:
            got = f.read()
        check(got == fx.expected(te)[1], "%s: %s equals the original" % (tag, rel))
    check(not os.path.exists(path + ".damaged"),
          "%s: no .damaged copy left beside it" % tag)


def last_block(te):
    return len(te.blocks) - 1


def is_known(te, i):
    """A damaged block whose size the tool may rely on without deduction: a
    full block, or the entry's last block."""
    return te.blocks[i][0] == te.block_size or i == last_block(te)


def lost_block_set(te, damaged):
    """Blocks that cannot be placed.  Each damaged block is lost.  A damaged
    block that is neither full nor last has an unknown size; one such size is
    deduced, but two or more cannot be, so everything from the first to the
    last of them is lost (the blocks between them cannot be placed)."""
    lost = set(damaged)
    unknown = sorted(i for i in damaged if te.blocks[i][0] != te.block_size)
    if len(unknown) >= 2:
        lost.update(range(unknown[0], unknown[-1] + 1))
    return lost


def expected_lost(fx, te, lost_blocks):
    """The byte ranges that must be reported lost when the blocks in
    lost_blocks cannot be used: those blocks, plus (Fluff) every byte of the
    later blocks that was copied from them, taken from the independent oracle."""
    ranges = block_ranges(te)
    out = [ranges[i] for i in lost_blocks]
    if te.method == 3:
        out += oracle.tainted(te, fx.vs, fx.expected(te)[1], set(lost_blocks))
    return merge(out)


def expected_lost_independent(te, damaged):
    ranges = block_ranges(te)
    return merge([ranges[i] for i in lost_block_set(te, damaged)])


def check_partial_size_damage(binary, fx, rng, workdir):
    """A damaged size field with the block's data intact.  The walk resynchronises
    (the next block is found by scan or by the intact comp field), the lost size
    is deduced from original_size, and the block itself still verifies and
    decodes: the whole entry comes back, under its real name."""
    cands = [e for e in fx.vs.entries if e.entry_type == 0 and len(e.blocks) >= 4]
    if not cands:
        return
    te = rng.choice(cands)
    for kind in ("comp", "orig"):
        i = rng.randrange(0, len(te.blocks))
        ctx, d = partial_damage(fx, te, [(i, kind)])
        tag = "partial %s / %s" % (fx.name, d.description.replace("'", ""))
        scenarios["partial: damaged size field"] += 1
        rep, out = run_partial(binary, fx, ctx, d, workdir)
        check(rep is not None, "%s: extract ran" % tag)
        if rep is None:
            continue
        verify_whole(tag, fx, te, find_result(rep, te), out)
        check(last_rc[0] == 1,
              "%s: exit status 1 (recovered, but the archive was damaged), got %r" %
              (tag, last_rc[0]))


def check_partial_double_fault(binary, fx, rng, workdir):
    """A full block's data AND its size field are both damaged.  Its size is
    deduced from original_size, so only that block is lost and every block after
    it is placed (Fluff: plus the later compressed blocks, which need the lost
    block's history, unless a later block of unknown size forces a larger hole)."""
    cands = [e for e in fx.vs.entries if e.entry_type == 0 and len(e.blocks) >= 4]
    if not cands:
        return
    te = rng.choice(cands)
    eligible = [i for i, (o, _c) in enumerate(te.blocks)
                if o == te.block_size and 0 < i < len(te.blocks) - 1]
    if not eligible:
        return
    i = rng.choice(eligible)
    ctx, d = partial_damage(fx, te, [(i, "data"), (i, "orig")])
    tag = "partial %s / data and size of block %d of entry %d" % (fx.name, i, te.index)
    scenarios["partial: data+size, full block"] += 1
    rep, out = run_partial(binary, fx, ctx, d, workdir)
    check(rep is not None, "%s: extract ran" % tag)
    if rep is None:
        return
    res = find_result(rep, te)
    check(res is not None and res["outcome"] == "partial",
          "%s: reported partial (%r)" % (tag, res and res["outcome"]))
    if res is None or res["outcome"] != "partial":
        return
    verify_partial(tag, fx, te, res, out)
    lost = merge([(r["start"], r["end"]) for r in res["ranges"]
                  if r["status"] == "lost"])
    want = expected_lost(fx, te, {i})
    check(lost == want, "%s: lost %r, expected %r" % (tag, lost[:5], want[:5]))


def check_partial_unanchored(binary, fx, rng, workdir):
    """Trailer and table both destroyed, plus a damaged block: nothing confirms
    where the entry ends, so the last block's size is not trusted and it is
    reported lost (size-uncertain) even though its bytes are intact."""
    cands = [e for e in fx.vs.entries if e.entry_type == 0 and len(e.blocks) >= 4]
    if not cands:
        return
    te = rng.choice(cands)
    n = len(te.blocks)
    eligible = [i for i, (o, _c) in enumerate(te.blocks)
                if o == te.block_size and i < n - 1]
    if not eligible:
        return
    i = rng.choice(eligible)
    ctx, d = partial_damage(fx, te, [(i, "data")])
    t = te.structures[-1]
    ctx.flip(t.volume, t.start + 1, 0)
    ts = fx.vs.table_structures[0]
    ctx.flip(ts.volume, ts.start, 0)
    d.changed = []
    for v in ctx.data:
        d.changed.extend(rf._changed_ranges(v, fx.vs.data[v], ctx.data[v]))
    tag = "partial %s / block %d, trailer and table of entry %d" % (fx.name, i, te.index)
    scenarios["partial: trailer+table lost"] += 1
    rep, out = run_partial(binary, fx, ctx, d, workdir)
    check(rep is not None, "%s: extract ran" % tag)
    if rep is None:
        return
    res = find_result(rep, te)
    check(res is not None and res["outcome"] == "partial",
          "%s: reported partial (%r)" % (tag, res and res["outcome"]))
    if res is None or res["outcome"] != "partial":
        return
    verify_partial(tag, fx, te, res, out)
    ranges = block_ranges(te)
    lost = merge([(r["start"], r["end"]) for r in res["ranges"]
                  if r["status"] == "lost"])
    check(lost == merge([ranges[i], ranges[-1]]),
          "%s: the damaged block and the unconfirmed last block are lost (%r)" %
          (tag, lost))
    last = [r for r in res["ranges"] if r["end"] == te.original_size][-1]
    check(last["status"] == "lost" and last["reason"] == "size-uncertain",
          "%s: last block reported size-uncertain (%r)" % (tag, last))


def check_partial_short_double_fault(binary, fx, rng, workdir):
    """A short, compressed, non-final block (an end-of-volume piece) with both
    its data and its size damaged to a still-plausible smaller size.  Only the
    'full or final block' rule stops the walk here; without it every later block
    would be placed at the wrong offset."""
    options = []
    for te in fx.vs.entries:
        if te.entry_type != 0:
            continue
        blocks = [s for s in te.structures if s.kind == sb.KIND_BLOCK]
        for i, (orig, comp) in enumerate(te.blocks):
            if orig < te.block_size and i < len(te.blocks) - 1 and comp < orig:
                for k in range(orig.bit_length()):
                    if (orig >> k) & 1 and orig - (1 << k) >= comp:
                        options.append((te, i, k, blocks[i]))
    if not options:
        return
    te, i, k, s = rng.choice(options)
    ctx = rf.DamageContext(fx)
    ctx.flip(s.volume, s.start + 12 + (s.length - 12) // 2, 3)
    ctx.flip(s.volume, s.start + 4 + k // 8, k % 8)
    d = rf.Damage("short", "data and size bit %d of short block %d" % (k, i))
    for v in ctx.data:
        d.changed.extend(rf._changed_ranges(v, fx.vs.data[v], ctx.data[v]))
    tag = "partial %s / %s of entry %d" % (fx.name, d.description, te.index)
    scenarios["partial: data+size, short block"] += 1
    rep, out = run_partial(binary, fx, ctx, d, workdir)
    check(rep is not None, "%s: extract ran" % tag)
    if rep is None:
        return
    res = find_result(rep, te)
    if res is None or res["outcome"] != "partial":
        check(res is not None and res["outcome"] == "skipped-partial",
              "%s: partial or nothing recoverable (%r)" % (tag, res and res["outcome"]))
        return
    verify_partial(tag, fx, te, res, out)
    lost = merge([(r["start"], r["end"]) for r in res["ranges"]
                  if r["status"] == "lost"])
    want = expected_lost(fx, te, {i})
    check(lost == want, "%s: lost %r, expected %r" % (tag, lost[:5], want[:5]))


def check_partial_wrong_size_intact_data(binary, fx, rng, workdir):
    """The data and CRC of a block are intact but its size field has been
    overwritten with another plausible value (smaller but still >= comp, or
    larger up to the block size).  Decoding is the only thing that can notice,
    and every codec must notice (otherwise the later blocks would be placed at a
    shifted offset, or the wrong bytes called ok).  The true size is then deduced
    from original_size and the whole entry is recovered and verified.  For Fluff
    this also checks that a failed decode attempt does not corrupt the history
    the retry decodes against."""
    blocks_of = {}
    options = []
    for te in fx.vs.entries:
        if te.entry_type != 0 or len(te.blocks) < 3:
            continue
        blocks = [s for s in te.structures if s.kind == sb.KIND_BLOCK]
        blocks_of[te.index] = blocks
        remaining = te.original_size
        for i, (orig, comp) in enumerate(te.blocks):
            remaining_after = remaining - orig
            if i < len(te.blocks) - 1 and i >= 1:
                smaller = orig - max(1, (orig - comp) // 2) if comp < orig else None
                if smaller is not None and smaller >= comp and smaller < orig:
                    options.append((te, i, smaller, "smaller"))
                if orig + 3 <= min(te.block_size, remaining):
                    options.append((te, i, orig + 3, "larger"))
            remaining = remaining_after
    if not options:
        return 0
    n = 0
    for te, i, newsize, how in rng.sample(options, min(6, len(options))):
        s = blocks_of[te.index][i]
        ctx = rf.DamageContext(fx)
        import struct as _st
        ctx.data[s.volume][s.start + 4:s.start + 8] = _st.pack("<I", newsize)
        d = rf.Damage("size", "size of block %d of entry %d set to %d (%s, true %d)" %
                      (i, te.index, newsize, how, te.blocks[i][0]))
        for v in ctx.data:
            d.changed.extend(rf._changed_ranges(v, fx.vs.data[v], ctx.data[v]))
        tag = "partial %s / %s" % (fx.name, d.description)
        scenarios["partial: wrong size, data intact"] += 1
        rep, out = run_partial(binary, fx, ctx, d, workdir)
        check(rep is not None, "%s: extract ran" % tag)
        if rep is None:
            continue
        res = find_result(rep, te)
        verify_whole(tag, fx, te, res, out)
        check(last_rc[0] == 1, "%s: exit status 1 for a repaired archive (%r)" %
              (tag, last_rc[0]))
        n += 1
    return n


def check_partial_missing_volume(binary, fx, rng, workdir, with_table=True, v=None,
                                 more=()):
    """A volume is gone mid-entry.  The blocks before it are recovered.  With
    the table, the blocks after it are recovered too, placed backwards from the
    end of the entry (the table says exactly where the entry ends, so the walk
    from the next volume's first structure is anchored).  Without the table the
    stream cannot be anchored across the gap and only the prefix is kept."""
    last = fx.vs.volume_count - 1
    if v is None:
        v = rng.randrange(1, last)
    lost = [v] + list(more)
    ctx = rf.DamageContext(fx)
    d = rf.Damage("lost-volume", "volume%s %s deleted" %
                  ("s" if more else "", ", ".join(str(x) for x in lost)))
    for x in lost:
        ctx.deleted.add(x)
        d.lost_volumes.add(x)
    if not with_table:
        destroy_table(ctx, d)
    tag = "partial %s / %s%s" % (fx.name, d.description,
                                 "" if with_table else " (no table)")
    scenarios["partial: volume deleted"] += 1
    rep, out = run_partial(binary, fx, ctx, d, workdir)
    check(rep is not None, "%s: extract ran" % tag)
    if rep is None:
        return
    for te in fx.vs.entries:
        if te.entry_type != 0 or not any(s.volume in lost for s in te.structures):
            continue
        if te.structures[0].volume in lost:
            continue                      # header lost with the volume
        res = find_result(rep, te)
        blocks = [s for s in te.structures if s.kind == sb.KIND_BLOCK]
        if not any(s.volume in lost for s in blocks):
            # Only the trailer was in the lost volume: every block is there and
            # the trusted table (if it survived) confirms the payload.
            if with_table:
                verify_whole(tag, fx, te, res, out)
            else:
                check(res is not None and res["outcome"] == "complete-unverified",
                      "%s: %r whole but unverified without the table (%r)" %
                      (tag, te.name, res and res["outcome"]))
            continue
        if res is None or res["outcome"] != "partial":
            check(res is not None and res["outcome"] == "skipped-partial",
                  "%s: %r partial or nothing recoverable (%r)" %
                  (tag, te.name, res and res["outcome"]))
            continue
        verify_partial(tag, fx, te, res, out)
        lo, hi = min(lost), max(lost)
        # With the table the blocks after the gap are placed; without it only
        # the prefix is.  Fluff blocks that copied from a lost block are lost too.
        first = min(i for i, s in enumerate(blocks) if s.volume in lost)
        gone = set(i for i, s in enumerate(blocks)
                   if s.volume in lost or (not with_table and i > first))
        want = expected_lost(fx, te, gone)
        got = merge([(r["start"], r["end"]) for r in res["ranges"]
                     if r["status"] == "lost"])
        check(got == want,
              "%s: %r loses exactly the blocks in the lost volume%s "
              "(got %r, want %r)" % (tag, te.name, " and what copied from them"
                                     if te.method == 3 else "", got[:4], want[:4]))
        if with_table:
            scenarios["partial: suffix after a gap"] += 1


def check_partial_unknown_size(binary, fx, rng, workdir):
    """Short end-of-volume blocks (and a short last block) with damaged data.
    Their sizes are unknown.  One unknown size is deduced from original_size,
    so exactly that block is lost; two cannot be, so everything between the
    first and the last of them is lost."""
    options = []
    for te in fx.vs.entries:
        if te.entry_type != 0:
            continue
        shorts = [i for i, (o, _c) in enumerate(te.blocks)
                  if o < te.block_size and i < len(te.blocks) - 1]
        if shorts:
            options.append((te, shorts))
    if not options:
        return 0
    te, shorts = rng.choice(options)
    n = 0
    for k in (1, 2):
        pool = list(shorts)
        if te.blocks[-1][0] < te.block_size:
            pool.append(len(te.blocks) - 1)
        if len(pool) < k:
            continue
        damaged = sorted(rng.sample(pool, k))
        ctx, d = partial_damage(fx, te, [(i, "data") for i in damaged])
        tag = "partial %s / data of short blocks %s of entry %d" % (
            fx.name, damaged, te.index)
        scenarios["partial: unknown size deduced"] += 1
        rep, out = run_partial(binary, fx, ctx, d, workdir)
        check(rep is not None, "%s: extract ran" % tag)
        if rep is None:
            continue
        res = find_result(rep, te)
        want = expected_lost(fx, te, lost_block_set(te, damaged))
        if want == [(0, te.original_size)]:
            check(res is not None and res["outcome"] == "skipped-partial",
                  "%s: nothing recoverable (%r)" % (tag, res and res["outcome"]))
            continue
        check(res is not None and res["outcome"] == "partial",
              "%s: reported partial (%r)" % (tag, res and res["outcome"]))
        if res is None or res["outcome"] != "partial":
            continue
        verify_partial(tag, fx, te, res, out)
        lost = merge([(r["start"], r["end"]) for r in res["ranges"]
                      if r["status"] == "lost"])
        check(lost == want, "%s: lost %r, expected %r" % (tag, lost[:4], want[:4]))
        n += 1
    return n


def check_partial_fluff_runs(binary, fx, rng, workdir):
    """Fluff after a lost block, with every run reported (--taint-min-run 1) and
    with the default.  The lost ranges must be exactly the lost block plus the
    bytes that were copied from it, as computed by the independent oracle; the
    default additionally absorbs short runs of untainted bytes between tainted
    ones (and the oracle applies the same rule)."""
    cands = [e for e in fx.vs.entries if e.entry_type == 0 and e.method == 3 and
             len(e.blocks) >= 4]
    if not cands:
        return 0
    te = rng.choice(cands)
    i = rng.randrange(0, len(te.blocks) - 1)
    n = 0
    for min_run in (1, 64, 1000):
        ctx, d = partial_damage(fx, te, [(i, "data")])
        tag = "partial %s / block %d of entry %d lost, --taint-min-run %d" % (
            fx.name, i, te.index, min_run)
        scenarios["partial: fluff taint"] += 1
        rep, out = run_partial(binary, fx, ctx, d, workdir,
                               extra=("--taint-min-run", str(min_run)))
        check(rep is not None, "%s: extract ran" % tag)
        if rep is None:
            continue
        res = find_result(rep, te)
        ranges = block_ranges(te)
        payload = fx.expected(te)[1]
        want = merge([ranges[i]] + oracle.tainted(te, fx.vs, payload, {i},
                                                  min_ok=min_run))
        if want == [(0, te.original_size)]:
            check(res is not None and res["outcome"] == "skipped-partial",
                  "%s: nothing recoverable (%r)" % (tag, res and res["outcome"]))
            continue
        check(res is not None and res["outcome"] == "partial",
              "%s: reported partial (%r)" % (tag, res and res["outcome"]))
        if res is None or res["outcome"] != "partial":
            continue
        verify_partial(tag, fx, te, res, out)
        got = merge([(r["start"], r["end"]) for r in res["ranges"]
                     if r["status"] == "lost"])
        check(got == want, "%s: lost %r, expected %r" % (tag, got[:4], want[:4]))
        reasons = set(r["reason"] for r in res["ranges"] if r["status"] == "lost")
        check(reasons <= {"block-crc", "fluff-taint"},
              "%s: reasons %r" % (tag, reasons))
        n += 1
    return n


def check_partial_fluff_size_after_loss(binary, fx, rng, workdir, how="larger"):
    """A Fluff entry with one block lost, and further on a block whose size
    field is wrong while its data is intact.  The decode at the wrong size
    fails; the retry at the deduced size must run against both histories as they
    were before the failed attempt.  Only the lost block and what copied from it
    may be reported lost.  A size larger than the true one lets the decoder run
    to the end of the block (advancing its history) before the length check
    fails, a smaller one stops it early, so only "larger" can corrupt a history:
    every kind is run, "larger" on a short block that has room for it."""
    options = []
    for te in fx.vs.entries:
        if te.entry_type != 0 or te.method != 3 or len(te.blocks) < 5:
            continue
        for i in range(0, len(te.blocks) - 3):
            if te.blocks[i][0] != te.block_size:
                continue                # a short lost block would be a second unknown
            for j in range(i + 2, len(te.blocks) - 1):
                oj, cj = te.blocks[j]
                if how == "larger" and oj + 3 > te.block_size:
                    continue
                if how == "smaller" and not (oj - 1 >= cj and oj > 1 and
                                             oj < te.block_size):
                    continue
                options.append((te, i, j))
    if not options:
        return 0
    te, i, j = rng.choice(options)
    ctx, d = partial_damage(fx, te, [(i, "data")])
    sj = [x for x in te.structures if x.kind == sb.KIND_BLOCK][j]
    oj, cj = te.blocks[j]
    if how == "larger":
        ctx.data[sj.volume][sj.start + 4:sj.start + 8] = struct.pack("<I", oj + 3)
    elif how == "smaller":
        ctx.data[sj.volume][sj.start + 4:sj.start + 8] = struct.pack("<I", oj - 1)
    else:
        ctx.flip(sj.volume, sj.start + 1, 2)
    d.changed = []
    for v in ctx.data:
        d.changed.extend(rf._changed_ranges(v, fx.vs.data[v], ctx.data[v]))
    tag = "partial %s / block %d lost, size field of block %d damaged (%s), entry %d" % (
        fx.name, i, j, how, te.index)
    scenarios["partial: fluff size after loss"] += 1
    scenarios["partial: fluff size after loss (%s)" % how] += 1
    rep, out = run_partial(binary, fx, ctx, d, workdir,
                           extra=("--taint-min-run", "1"))
    check(rep is not None, "%s: extract ran" % tag)
    if rep is None:
        return 0
    res = find_result(rep, te)
    # A full lost block has a known size, so the later block's wrong size is
    # deduced and it is decoded.  A short lost block (an end-of-volume piece)
    # has an unknown size too, and two unknown sizes cannot be deduced: the
    # stretch from the first to the second is lost.
    lost_blocks = {i} if te.blocks[i][0] == te.block_size else set(range(i, j + 1))
    want = merge([block_ranges(te)[b] for b in lost_blocks] + oracle.tainted(
        te, fx.vs, fx.expected(te)[1], lost_blocks, min_ok=1))
    if want == [(0, te.original_size)]:
        check(res is not None and res["outcome"] == "skipped-partial",
              "%s: nothing recoverable (%r)" % (tag, res and res["outcome"]))
        return 1
    check(res is not None and res["outcome"] == "partial",
          "%s: reported partial (%r)" % (tag, res and res["outcome"]))
    if res is None or res["outcome"] != "partial":
        return 0
    verify_partial(tag, fx, te, res, out)
    got = merge([(r["start"], r["end"]) for r in res["ranges"]
                 if r["status"] == "lost"])
    check(got == want, "%s: lost %r, expected %r" % (tag, got[:4], want[:4]))
    return 1


def check_excised_block(binary, fx, rng, workdir):
    """One whole block (header and data) is removed from the stream, so every
    later structure moves up.  The blocks after it still pass their CRC and
    decode (a Fluff block even decodes against the wrong history without error),
    so a reader that only follows the chain would call them ok one block too
    early.  The block sizes no longer add up to original_size: the entry must be
    reported as missing blocks, with no byte ok, and every other data entry must
    still come back verified (the stream shifted under them and under the
    table's offsets)."""
    cands = [e for e in fx.vs.entries if e.entry_type == 0 and len(e.blocks) >= 2]
    if not cands:
        return 0
    n = 0
    for te in rng.sample(cands, min(3, len(cands))):
        i = rng.randrange(0, len(te.blocks))
        blocks = [x for x in te.structures if x.kind == sb.KIND_BLOCK]
        s_ = blocks[i]
        ctx = rf.DamageContext(fx)
        extra = ""
        if rng.random() < 0.5:
            # a second fault in the same entry (damaged data of a short block, so
            # its size is unknown too): the sizes no longer add up either way, and
            # only the length of the stream shows that blocks are missing
            others = [b for b in range(len(te.blocks)) if b != i and
                      te.blocks[b][0] < te.block_size]
            if others:
                j = rng.choice(others)
                sj = blocks[j]
                ctx.flip(sj.volume, sj.start + 12 + (sj.length - 12) // 2, 3)
                extra = " and the data of block %d damaged" % j
        del ctx.data[s_.volume][s_.start:s_.start + s_.length]
        d = rf.Damage("excise", "block %d of entry %d removed%s" % (i, te.index, extra))
        d.changed = [(s_.volume, s_.start, len(ctx.data[s_.volume]))]
        tag = "excise %s / %s" % (fx.name, d.description)
        scenarios["excised blocks"] += 1
        rep, out = run_partial(binary, fx, ctx, d, workdir)
        check(rep is not None, "%s: extract ran" % tag)
        if rep is None:
            continue
        res = find_result(rep, te)
        check(res is not None and res["outcome"] == "skipped-partial" and
              res["detail"] == "blocks-missing",
              "%s: reported as missing blocks, nothing written (%r)" %
              (tag, res and (res["outcome"], res.get("detail"))))
        check(not os.path.exists(os.path.join(out, te.name.decode("utf-8") + ".damaged")),
              "%s: no .damaged file written" % tag)
        # Everything else comes back, found by name (offsets moved).
        got = {}
        for r in rep["extraction"]["results"]:
            got.setdefault(r["name"].rstrip("/"), []).append(r["outcome"])
        for e in fx.vs.entries:
            if e.index == te.index or e.entry_type not in (0, 3, 4):
                continue
            name = e.name.decode("utf-8").rstrip("/")
            outs = got.get(name, [])
            if not extra:
                okset = ("verified", "verified-by-table", "created",
                         "complete-unverified")
                if WIN["runner"] and e.entry_type == 3:
                    okset += ("skipped-symlink",)
                check(any(o in okset for o in outs),
                      "%s: %s still extracted (%r)" % (tag, name, outs))
            exp = fx.expected(e)
            path = os.path.join(out, name)
            if exp and exp[0] == "file" and os.path.isfile(path):
                with open(path, "rb") as f:
                    check(f.read() == exp[1], "%s: %s is the original" % (tag, name))
        n += 1
    return n


# --- phase 6: rebuild -----------------------------------------------------

import glob

scenarios.update({"rebuild clean": 0, "rebuild damaged": 0, "rebuild hostile": 0,
                  "rebuild volume sets": 0})


def run_rebuild(binary, paths, out, vsize=None, extra=()):
    cmd = tool_cmd(binary) + ["rebuild", "--format", "json", "-o", host_path(out)]
    if vsize:
        cmd += ["--volume-size", str(vsize)]
    r = subprocess.run(cmd + list(extra) + [host_path(p) for p in paths],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    check(r.returncode in (0, 1, 2, 3),
          "rebuild exit status %d outside the documented ones: %s" %
          (r.returncode, r.stderr.decode(errors="replace")[:300]))
    try:
        rep = json.loads(r.stdout)
    except ValueError:
        rep = None
    if rep is not None and "rebuild" in rep:
        rep["rebuild"]["files"] = [unhost(f) for f in rep["rebuild"]["files"]]
    return rep, r.returncode, r.stderr.decode(errors="replace")


def entry_key(e):
    return (e.name, e.entry_type, e.original_size, e.mtime, e.flags, e.checksum)


def entry_bytes(vs, e):
    return b"".join(bytes(vs.data[s.volume][s.start:s.end]) for s in e.structures)


def check_rebuild(binary, boltpack, fx, workdir, vsize=None, damage=None,
                  ctx=None, kind="clean", expect_rc=None):
    """Rebuild fx (clean, or damaged by `ctx`/`damage`), then check the new
    archive with the independent oracle, the CLI, and the source."""
    tag = "rebuild %s / %s%s" % (fx.name, kind, " / volume size %d" % vsize if vsize else "")
    base = os.path.join(workdir, "rb")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(os.path.join(base, "rb"))
    if ctx is not None:
        paths = rf.write_damaged(ctx, damage, os.path.join(base, "in"))
        paths = sorted(paths, key=lambda p: (p.endswith(".dup"), p))
        siblings = ("rb", "in")
    else:
        paths = fx.paths
        siblings = ("rb",)
    out = os.path.join(base, "rb", "new.bolt")
    scenarios["rebuild clean" if ctx is None else "rebuild damaged"] += 1
    if vsize:
        scenarios["rebuild volume sets"] += 1
    rep, rc, err = run_rebuild(binary, paths, out, vsize)
    check(rep is not None, "%s: rebuild ran (%s)" % (tag, err[:200]))
    if rep is None:
        return None
    if expect_rc is not None:
        check(rc == expect_rc, "%s: exit status %d, expected %d" % (tag, rc, expect_rc))
    files = rep["rebuild"]["files"]
    leftovers = [f for f in os.listdir(os.path.join(base, "rb"))]
    check(sorted(leftovers) == sorted(os.path.basename(f) for f in files),
          "%s: only the volumes are left (%r vs %r)" % (tag, leftovers, files))
    intact = (damage.intact_entries(fx.vs) if damage is not None else
              list(fx.vs.entries))
    if not files:
        check(rep["rebuild"]["entries"] == 0 and not any(
            te.entry_type in (0, 3, 4) for te in intact),
            "%s: nothing written although entries were intact" % tag)
        return rep
    # 1. The independent parser accepts the new set and the chain is gap-free.
    try:
        ovs = sb.parse_set(files)
        sb.check_chain(ovs)
    except Exception as ex:               # noqa: BLE001
        check(False, "%s: oracle rejects the new archive: %s" % (tag, ex))
        return rep
    check(len(ovs.entries) == rep["rebuild"]["entries"],
          "%s: entry count %d vs reported %d" %
          (tag, len(ovs.entries), rep["rebuild"]["entries"]))
    # 2. Every entry in it is a real entry of the source, unchanged.
    pool = {}
    for e in fx.vs.entries:
        pool.setdefault(entry_key(e), []).append(e)
    order = []
    reencoded = set(r["name"].rstrip("/") for r in rep["rebuild"]["results"]
                    if r.get("reencoded"))
    rebuilt_hdr = set(r["name"].rstrip("/") for r in rep["rebuild"]["results"]
                      if r.get("header_rebuilt"))
    for oe in ovs.entries:
        key = entry_key(oe)
        src = pool.get(key)
        check(src, "%s: %r is not an entry of the source" % (tag, oe.name))
        if not src:
            continue
        se = src.pop(0)
        order.append(se.index)
        name = oe.name.decode("utf-8").rstrip("/")
        same_layout = (vsize is None and fx.vs.volume_count == 1 and
                       name not in reencoded and name not in rebuilt_hdr)
        if same_layout:
            check(entry_bytes(ovs, oe) == entry_bytes(fx.vs, se),
                  "%s: %r is not byte-identical to the original" % (tag, oe.name))
    check(order == sorted(order), "%s: entries are in the source's order (%r)" % (tag, order[:8]))
    if ctx is None:
        # Exactly the entries whose blocks were cut at the source's volume
        # boundaries (a short block before the last one) are cut again; every
        # other entry is copied block for block.
        irregular = set(e.name.decode("utf-8").rstrip("/") for e in fx.vs.entries
                        if any(o < e.block_size for o, _c in e.blocks[:-1]))
        check(reencoded == irregular,
              "%s: re-encoded entries %r, expected %r" %
              (tag, sorted(reencoded)[:4], sorted(irregular)[:4]))
    # 3. Every intact entry made it.
    present = {}
    for oe in ovs.entries:
        present.setdefault(entry_key(oe), 0)
        present[entry_key(oe)] += 1
    intact_idx = set(e.index for e in intact)
    for te in intact:
        if te.entry_type in (1, 2):
            # a resource fork or metadata entry is only valid beside its own
            # data fork: required only if that one is intact too
            head = [e for e in fx.vs.entries if e.index < te.index and
                    e.entry_type not in (1, 2)][-1]
            if head.index not in intact_idx or head.name != te.name:
                continue
        check(present.get(entry_key(te), 0) > 0,
              "%s: intact entry %r is missing from the new archive" % (tag, te.name))
    # 4. Volume sizes.
    if vsize:
        for f in files[:-1] + files[-1:]:
            check(os.path.getsize(f) <= vsize,
                  "%s: %s is %d bytes, over the %d limit" %
                  (tag, os.path.basename(f), os.path.getsize(f), vsize))
    check(os.path.basename(files[-1]) == "new.bolt" and
          all(os.path.basename(f) == "new.%03d" % i for i, f in enumerate(files[:-1])),
          "%s: volume names %r" % (tag, [os.path.basename(f) for f in files]))
    # 5. The CLI reads it and restores exactly the original bytes.
    if fx.payloads is None:
        last = files[-1]
        t = subprocess.run([boltpack, "test", last], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
        check(t.returncode == 0, "%s: boltpack test: %s" %
              (tag, t.stdout.decode(errors="replace")[-200:]))
        xo = os.path.join(base, "x")
        x = subprocess.run([boltpack, "extract", "-qq", "-o", xo, last],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        check(x.returncode == 0, "%s: boltpack extract: %s" %
              (tag, x.stdout.decode(errors="replace")[-200:]))
        if x.returncode == 0:
            verify_tree(tag, fx, xo, [t_ for t_ in intact if t_.entry_type in (0, 3, 4)],
                        siblings=siblings, neutral=True)
    # 5b. The CLI's other commands treat it as an ordinary archive: list, info,
    # and (for one volume) add a file and delete it again.
    if fx.payloads is None:
        last = files[-1]
        li = subprocess.run([boltpack, "list", last], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
        check(li.returncode == 0, "%s: boltpack list" % tag)
        listing = li.stdout.decode("utf-8", "replace")
        for te in intact:
            if te.entry_type in (0, 3):
                check(te.name.decode("utf-8") in listing,
                      "%s: boltpack list shows %r" % (tag, te.name))
        inf = subprocess.run([boltpack, "info", last], stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
        check(inf.returncode == 0, "%s: boltpack info" % tag)
        if len(files) == 1:
            extra_src = os.path.join(base, "addme.txt")
            with open(extra_src, "wb") as f:
                f.write(b"added after the rebuild\n" * 100)
            work = os.path.join(base, "cp.bolt")
            shutil.copy(last, work)
            a = subprocess.run([boltpack, "add", "-qq", work, "addme.txt"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               cwd=base)
            check(a.returncode == 0, "%s: boltpack add: %s" %
                  (tag, a.stdout.decode(errors="replace")[-150:]))
            t3 = subprocess.run([boltpack, "test", work], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
            check(t3.returncode == 0, "%s: boltpack test after add" % tag)
            os.remove(work)
            os.remove(extra_src)
    # 6. The tool's own extraction of the new archive agrees.
    xo2 = os.path.join(base, "x2")
    rep2, rc2, err2 = run_extract(binary, files, xo2)
    # (exit status 1 only because extraction cannot restore resource forks and
    # metadata entries, which a rebuilt archive keeps)
    want_rc = 1 if any(oe.entry_type in (1, 2) for oe in ovs.entries) else 0
    if WIN["runner"] and any(oe.entry_type == 3 for oe in ovs.entries):
        want_rc = 1             # symlinks are skipped on Windows (exit status 1)
    check(rep2 is not None and rc2 == want_rc,
          "%s: boltpack-recover reads its own output cleanly (rc %s, want %d)" %
          (tag, rc2, want_rc))
    return rep


def check_rebuild_hostile(binary, boltpack, fx, workdir):
    base = os.path.join(workdir, "rbh")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(base)
    out = os.path.join(base, "new.bolt")
    tag = "rebuild %s hostile" % fx.name
    scenarios["rebuild hostile"] += 1
    # An existing output is never replaced.
    with open(out, "wb") as f:
        f.write(b"precious")
    rep, rc, err = run_rebuild(binary, fx.paths, out)
    check(rc == 3, "%s: existing output refused (rc %d)" % (tag, rc))
    with open(out, "rb") as f:
        check(f.read() == b"precious", "%s: existing output untouched" % tag)
    os.remove(out)
    # A later volume's name that already exists is not overwritten either, and
    # the volumes finished before it are removed again.
    with open(os.path.join(base, "new.001"), "wb") as f:
        f.write(b"precious")
    rep, rc, err = run_rebuild(binary, fx.paths, out, vsize=20000)
    check(rc == 3, "%s: existing numbered volume refused (rc %d)" % (tag, rc))
    with open(os.path.join(base, "new.001"), "rb") as f:
        check(f.read() == b"precious", "%s: existing volume untouched" % tag)
    check(sorted(os.listdir(base)) == ["new.001"],
          "%s: nothing else left behind (%r)" % (tag, os.listdir(base)))
    os.remove(os.path.join(base, "new.001"))
    # A volume size too small is an error, with nothing written.
    rep, rc, err = run_rebuild(binary, fx.paths, out, vsize=100)
    check(rc == 3 and not os.listdir(base),
          "%s: tiny volume size refused, nothing written (rc %d, %r)" %
          (tag, rc, os.listdir(base)))
    # A directory that cannot be written into.
    rep, rc, err = run_rebuild(binary, fx.paths, os.path.join(base, "nodir", "x.bolt"))
    check(rc == 3, "%s: unwritable destination (rc %d)" % (tag, rc))


def check_rebuild_checksum_lie(binary, boltpack, fx, rng, workdir):
    """An entry's trailer says one checksum but its payload has another, and the
    trailer's own CRC is valid (so every structure verifies).  Only decoding
    the payload shows it.  The entry must be left out, reported, and the rest of
    the archive rebuilt."""
    import zlib
    te = rng.choice([e for e in fx.vs.entries if e.entry_type == 0 and e.original_size > 0])
    t = te.structures[-1]
    ctx = rf.DamageContext(fx)
    raw = ctx.data[t.volume]
    raw[t.start + 8] ^= 0x55
    raw[t.start + 12:t.start + 16] = struct.pack("<I", zlib.crc32(bytes(raw[t.start:t.start + 12])) & 0xFFFFFFFF)
    damage = rf.Damage("lie", "checksum of entry %d replaced, trailer CRC fixed" % te.index)
    damage.changed = [(t.volume, t.start, t.end)]
    tag = "rebuild %s / %s" % (fx.name, damage.description)
    base = os.path.join(workdir, "rbl")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(os.path.join(base, "rb"))
    paths = rf.write_damaged(ctx, damage, os.path.join(base, "in"))
    out = os.path.join(base, "rb", "new.bolt")
    scenarios["rebuild damaged"] += 1
    rep, rc, err = run_rebuild(binary, paths, out)
    check(rep is not None and rc == 1, "%s: rebuild ran, exit status 1 (%r)" % (tag, rc))
    if rep is None:
        return
    res = [r for r in rep["rebuild"]["results"] if r["name"] == te.name.decode("utf-8")]
    check(res and res[0]["outcome"] == "failed" and
          res[0]["detail"] == "payload-checksum-mismatch",
          "%s: the entry is reported as failed (%r)" % (tag, res))
    ovs = sb.parse_set(rep["rebuild"]["files"])
    names = [e.name for e in ovs.entries]
    check(te.name not in names, "%s: the entry is not in the new archive" % tag)
    others = [e for e in fx.vs.entries if e.index != te.index and
              e.entry_type in (0, 3, 4)]
    check(all(e.name in names for e in others),
          "%s: every other entry is in it" % tag)
    t2 = subprocess.run([boltpack, "test", rep["rebuild"]["files"][-1]],
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    check(t2.returncode == 0, "%s: boltpack test passes" % tag)


def check_rebuild_duplicates(binary, boltpack, workdir):
    """Two data entries with one name cannot be in one archive (the writer
    refuses it).  The first stays, the second is reported and left out, and a
    resource fork whose data fork was dropped goes with it."""
    import bp_archive_bytes as bab
    rng = random.Random(3)
    e = bab.entry
    entries = [
        e("a.txt", b"first a " * 50),
        e("a.txt", b"second a " * 50),
        e("a.txt", b"fork of the second", entry_type=1, platform=0x02),
        e("b.txt", b"bee " * 80),
        e("b.txt", rng.randbytes(300), entry_type=1, platform=0x02),
        e("b.txt", rng.randbytes(300), entry_type=1, platform=0x02),
    ]
    base = os.path.join(workdir, "rbd")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(os.path.join(base, "rb"))
    src = os.path.join(base, "dup.bolt")
    with open(src, "wb") as f:
        f.write(bab.build7({"entries": entries}))
    out = os.path.join(base, "rb", "new.bolt")
    tag = "rebuild duplicate names"
    scenarios["rebuild damaged"] += 1
    rep, rc, err = run_rebuild(binary, [src], out)
    check(rep is not None and rc == 1, "%s: rebuild ran (rc %r, %s)" % (tag, rc, err[:150]))
    if rep is None:
        return
    outs = [(r["name"], r["type"], r["outcome"]) for r in rep["rebuild"]["results"]]
    check(outs.count(("a.txt", 0, "skipped-duplicate-name")) == 1,
          "%s: the second a.txt is reported (%r)" % (tag, outs))
    check(outs.count(("a.txt", 1, "skipped-supplementary")) == 1,
          "%s: the fork that followed the dropped one is reported (%r)" % (tag, outs))
    check(outs.count(("b.txt", 1, "skipped-supplementary")) == 1,
          "%s: a second resource fork is dropped (%r)" % (tag, outs))
    ovs = sb.parse_set(rep["rebuild"]["files"])
    check([(e_.name, e_.entry_type) for e_ in ovs.entries] ==
          [(b"a.txt", 0), (b"b.txt", 0), (b"b.txt", 1)],
          "%s: the new archive holds the first of each (%r)" %
          (tag, [(e_.name, e_.entry_type) for e_ in ovs.entries]))


def check_partial_lying_full(binary, fx, rng, workdir):
    """A short block's data is damaged and its size field is overwritten with
    exactly the full block size, so the block looks like an ordinary full one.
    Taken at its word, every later block would be placed too far on.  The
    block sizes no longer add up to original_size, which gives it away: the
    real size is deduced and only that block is lost."""
    options = []
    for te in fx.vs.entries:
        if te.entry_type != 0:
            continue
        for i, (o, _c) in enumerate(te.blocks):
            if o < te.block_size and i < len(te.blocks) - 1 and len(te.blocks) >= 3:
                options.append((te, i))
    if not options:
        return 0
    n = 0
    for te, i in rng.sample(options, min(4, len(options))):
        s = [x for x in te.structures if x.kind == sb.KIND_BLOCK][i]
        ctx = rf.DamageContext(fx)
        ctx.flip(s.volume, s.start + 12 + (s.length - 12) // 2, 3)
        ctx.data[s.volume][s.start + 4:s.start + 8] = struct.pack("<I", te.block_size)
        d = rf.Damage("lie", "short block %d of entry %d: data damaged, size set to %d" %
                      (i, te.index, te.block_size))
        for v in ctx.data:
            d.changed.extend(rf._changed_ranges(v, fx.vs.data[v], ctx.data[v]))
        tag = "partial %s / %s" % (fx.name, d.description)
        scenarios["partial: size says full"] += 1
        rep, out = run_partial(binary, fx, ctx, d, workdir)
        check(rep is not None, "%s: extract ran" % tag)
        if rep is None:
            continue
        res = find_result(rep, te)
        check(res is not None and res["outcome"] == "partial",
              "%s: reported partial (%r)" % (tag, res and res["outcome"]))
        if res is None or res["outcome"] != "partial":
            continue
        verify_partial(tag, fx, te, res, out)
        lost = merge([(r["start"], r["end"]) for r in res["ranges"]
                      if r["status"] == "lost"])
        want = expected_lost(fx, te, {i})
        check(lost == want, "%s: lost %r, expected %r" % (tag, lost[:4], want[:4]))
        n += 1
    return n


def check_partial_two_size_faults(binary, fx, rng, workdir):
    """Size fields of two blocks of one entry are damaged, data intact.  The
    walk after the first one cannot reach the end of the entry (the second
    damaged field stops it), so it is not anchored and must not be used to
    place anything.  The tool resumes after the second block instead; the blocks
    between, and the two, are lost, because two unknown sizes cannot be
    deduced.  Nothing reported ok may differ from the original."""
    cands = [e for e in fx.vs.entries if e.entry_type == 0 and len(e.blocks) >= 6]
    if not cands:
        return 0
    te = rng.choice(cands)
    n = len(te.blocks)
    i = rng.randrange(0, n - 4)
    j = rng.randrange(i + 2, n - 1)
    ctx, d = partial_damage(fx, te, [(i, rng.choice(("comp", "orig"))),
                                     (j, rng.choice(("comp", "orig")))])
    tag = "partial %s / size fields of blocks %d and %d of entry %d" % (
        fx.name, i, j, te.index)
    scenarios["partial: two size faults"] += 1
    rep, out = run_partial(binary, fx, ctx, d, workdir)
    check(rep is not None, "%s: extract ran" % tag)
    if rep is None:
        return 0
    res = find_result(rep, te)
    if res is None or res["outcome"] != "partial":
        # A walk that can still be anchored recovers everything.
        check(res is not None and res["outcome"] in ("verified", "skipped-partial"),
              "%s: partial, whole or nothing (%r)" % (tag, res and res["outcome"]))
        return 1
    verify_partial(tag, fx, te, res, out)
    ranges = block_ranges(te)
    ok = merge([(r["start"], r["end"]) for r in res["ranges"]
                if r["status"] == "ok"])
    check(ok and ok[0][0] == 0 and ok[0][1] >= ranges[i - 1][1] if i > 0 else True,
          "%s: the blocks before the first fault are recovered (%r)" % (tag, ok[:2]))
    return 1


def check_partial_nested_size(binary, fx, rng, workdir):
    """A size field of the entry that holds a nested archive is damaged.  The
    nested archive's blocks are genuine, CRC-valid blocks sitting in the
    holding entry's data; resynchronising must never adopt one of them.  The
    holding entry comes back whole, and nothing from inside it is written."""
    te = [e for e in fx.vs.entries if e.name.endswith(b"inner.bolt")][0]
    names = set(e.name.decode("utf-8").rstrip("/") for e in fx.vs.entries)
    n = 0
    for i in range(len(te.blocks)):
        for kind in ("comp", "orig"):
            ctx, d = partial_damage(fx, te, [(i, kind)])
            tag = "partial %s / %s of block %d of the holding entry" % (fx.name, kind, i)
            scenarios["partial: nested holding entry"] += 1
            rep, out = run_partial(binary, fx, ctx, d, workdir)
            check(rep is not None, "%s: extract ran" % tag)
            if rep is None:
                continue
            res = find_result(rep, te)
            verify_whole(tag, fx, te, res, out)
            written = set()
            for dp, _dn, fn in os.walk(out):
                for f in fn:
                    written.add(os.path.relpath(os.path.join(dp, f), out))
            stray = [w for w in written if w not in names]
            check(not stray, "%s: nothing from inside the nested archive was "
                  "written (%r)" % (tag, stray[:3]))
            n += 1
    # and a data fault together with a size fault in one block: no byte called
    # ok may differ from the original, and no nested file may appear
    for _ in range(6):
        i = rng.randrange(len(te.blocks))
        ctx, d = partial_damage(fx, te, [(i, "data"), (i, rng.choice(("comp", "orig")))])
        tag = "partial %s / data and size of block %d of the holding entry" % (fx.name, i)
        scenarios["partial: nested holding entry"] += 1
        rep, out = run_partial(binary, fx, ctx, d, workdir)
        check(rep is not None, "%s: extract ran" % tag)
        if rep is None:
            continue
        res = find_result(rep, te)
        if res is not None and res["outcome"] == "partial":
            verify_partial(tag, fx, te, res, out)
        written = set()
        for dp, _dn, fn in os.walk(out):
            for f in fn:
                written.add(os.path.relpath(os.path.join(dp, f), out))
        stray = [w for w in written if w not in names and
                 not (w.endswith(".damaged") and w[:-8] in names)]
        check(not stray, "%s: nothing from inside the nested archive was written (%r)" %
              (tag, stray[:3]))
        n += 1
    return n


def destroy_table(ctx, damage):
    """Zero the whole file table and the end record (they are in the last
    volume, or spread over the last volumes)."""
    vs = ctx.vs
    for s in vs.table_structures + [vs.end_record]:
        if s.volume in ctx.deleted:
            continue
        ctx.zero(s.volume, s.start, s.end)
    for v in set(s.volume for s in vs.table_structures + [vs.end_record]):
        if v not in ctx.deleted:
            damage.changed.extend(rf._changed_ranges(v, vs.data[v], ctx.data[v]))
    damage.description += ", table and end record zeroed"


def check_lost_middle_volume(binary, fx, workdir):
    """Phase 4 acceptance: a lost middle volume.  Every entry wholly in other
    volumes must be recovered, with the table and without it."""
    last = fx.vs.volume_count - 1
    n = 0
    for v in range(1, last):
        for with_table in (True, False):
            rng = random.Random(v)
            scenarios["lost middle volume"] += 1
            check_extract_damage(
                binary, fx, "delete-volume", rng, workdir, v,
                label="%s %s" % (fx.name, "table" if with_table else "no table"),
                extra_ctx=None if with_table else destroy_table)
            n += 1
    return n


def main():
    global oracle
    ap = argparse.ArgumentParser()
    ap.add_argument("--recover",
                    default=os.path.join(ROOT, "build/recover/boltpack-recover"))
    ap.add_argument("--boltpack",
                    default=os.path.join(ROOT, "build/linux/boltpack"))
    ap.add_argument("--seed", type=int, default=20261127)
    ap.add_argument("--sweep", type=int, default=12,
                    help="random damage runs per fixture")
    ap.add_argument("--keep", help="keep generated files in this directory")
    ap.add_argument("--names-only", action="store_true",
                    help="run only the clean fixtures, hostile, host-name and "
                         "path-style scenarios")
    ap.add_argument("--windows-runner", metavar="CMD",
                    help="--recover is a Windows executable; run it with CMD "
                         "(for example wine) and check the Windows host rules")
    args = ap.parse_args()
    WIN["runner"] = args.windows_runner
    args.recover = os.path.abspath(args.recover)
    args.boltpack = os.path.abspath(args.boltpack)
    for p in (args.recover, args.boltpack):
        if not os.path.exists(p):
            print("missing %s (build it first)" % p)
            return 2
    warm_up(args.recover)

    root = args.keep or tempfile.mkdtemp(prefix="bpr-test-")
    os.makedirs(root, exist_ok=True)
    try:
        fixtures = rf.build_all(args.boltpack, root)
        print("Fixtures:")
        for n, fx in fixtures.items():
            have = rf.check_coverage(fx, ())
            print("  %-14s %3d volume(s) %4d entries  %s" % (
                n, fx.vs.volume_count, len(fx.vs.entries),
                ", ".join(sorted(k for k, v in have.items() if v))))

        for fx in fixtures.values():
            check_clean(args.recover, fx)

        xwork = os.path.join(root, "xwork")
        os.makedirs(xwork, exist_ok=True)
        for fx in fixtures.values():
            check_extract_clean(args.recover, args.boltpack, fx, xwork)

        if args.names_only:
            # quick mode for the host name rules (a few minutes under Wine)
            check_hostile(args.recover, xwork)
            check_host_names(args.recover, xwork)
            for name in ("tree-lz", "vol-fluff"):
                check_path_styles(args.recover, args.boltpack, fixtures[name], xwork)
            raise NamesOnlyDone()
        oracle = fo.FluffOracle(root)
        work = os.path.join(root, "work")
        os.makedirs(work, exist_ok=True)
        rng = random.Random(args.seed)

        # Every damage kind on a fixture it makes sense for.
        singles = ["tree-fluff", "tree-stored", "tree-lz", "tree-lite",
                   "synthetic", "nested-fluff"]
        sets = ["vol-fluff", "vol-stored", "vol-many", "vol-lz", "vol-lite"]
        for kind in rf.DAMAGE_KINDS:
            names = singles + sets if kind not in (
                "delete-volume", "shuffle-names", "duplicate-volume") \
                else sets
            for name in names:
                fx = fixtures[name]
                for rep_i in range(2):
                    check_damage(args.recover, fx, kind, rng, work)

        # Targeted multi-volume cases.
        fx = fixtures["vol-fluff"]
        last = fx.vs.volume_count - 1
        for v in (0, last, 1, last - 1):
            check_damage(args.recover, fx, "delete-volume", rng, work, v,
                         label="vol-fluff v%d" % v)
        check_damage(args.recover, fx, "volume-header-and-trailer", rng, work,
                     last, label="vol-fluff last")

        # Phase 2: extraction under every damage kind, and the reconciler cases.
        for kind in rf.DAMAGE_KINDS:
            for name in singles + sets:
                if kind in ("delete-volume", "shuffle-names",
                            "duplicate-volume") and name in singles:
                    continue
                check_extract_damage(args.recover, fixtures[name], kind, rng,
                                     xwork)
        fx = fixtures["vol-fluff"]
        for v in (0, last, last - 1):
            check_extract_damage(args.recover, fx, "delete-volume", rng, xwork,
                                 v, label="vol-fluff v%d" % v)
        for name in ("tree-fluff", "tree-lz", "tree-lite", "tree-stored",
                     "vol-fluff", "vol-many"):
            check_rebuilt_header(args.recover, fixtures[name], rng, xwork)
            check_trailer_cases(args.recover, fixtures[name], rng, xwork)
        check_hostile(args.recover, xwork)
        check_host_names(args.recover, xwork)
        for name in ("tree-lz", "vol-fluff"):
            check_path_styles(args.recover, args.boltpack, fixtures[name], xwork)
        for name in ("tree-stored", "tree-lite", "tree-lz", "tree-fluff",
                     "vol-stored", "vol-fluff", "vol-lz", "vol-lite"):
            for _ in range(3):
                check_partial_exact(args.recover, fixtures[name], rng, xwork)
                check_partial_exact(args.recover, fixtures[name], rng, xwork,
                                    header_destroyed=True)
                check_partial_size_damage(args.recover, fixtures[name], rng, xwork)
        for name in ("tree-stored", "tree-lite", "tree-lz", "tree-fluff",
                     "vol-stored", "vol-fluff"):
            for _ in range(3):
                check_partial_double_fault(args.recover, fixtures[name], rng, xwork)
        for name in ("tree-lz", "tree-lite", "tree-fluff", "tree-stored",
                     "vol-fluff", "vol-lz", "vol-lite", "vol-stored"):
            for _ in range(12 if "fluff" in name else 6):
                check_partial_wrong_size_intact_data(args.recover, fixtures[name], rng, xwork)
        for name in ("vol-fluff", "vol-lz", "vol-lite", "vol-many"):
            for _ in range(8):
                check_partial_short_double_fault(args.recover, fixtures[name], rng, xwork)
        for name in ("tree-stored", "tree-lite", "tree-lz"):
            for _ in range(4):
                check_partial_unanchored(args.recover, fixtures[name], rng, xwork)
        for name in ("vol-stored", "vol-fluff", "vol-lz", "vol-lite", "vol-many"):
            check_lost_middle_volume(args.recover, fixtures[name], xwork)
        for name in ("vol-stored", "vol-fluff", "vol-lz", "vol-lite", "vol-many"):
            fx = fixtures[name]
            for v in range(1, fx.vs.volume_count - 1):
                check_partial_missing_volume(args.recover, fx, rng, xwork, v=v)
                check_partial_missing_volume(args.recover, fx, rng, xwork,
                                             with_table=False, v=v)
            # two adjacent volumes lost: the walk must skip both
            for v in range(1, fx.vs.volume_count - 2):
                check_partial_missing_volume(args.recover, fx, rng, xwork, v=v,
                                             more=(v + 1,))
        for name in ("vol-stored", "vol-lz", "vol-lite", "vol-many", "vol-fluff"):
            for _ in range(8):
                check_partial_unknown_size(args.recover, fixtures[name], rng, xwork)
        for name in ("nested-stored", "nested-fluff"):
            check_partial_nested_size(args.recover, fixtures[name], rng, xwork)
            check_nested_header_lost(args.recover, fixtures[name], rng, xwork)
        for name in ("tree-stored", "tree-lite", "tree-lz", "tree-fluff", "vol-stored",
                     "vol-lz", "vol-lite", "vol-fluff", "vol-many"):
            for _ in range(4):
                check_excised_block(args.recover, fixtures[name], rng, xwork)
        for name in ("tree-fluff", "vol-fluff", "vol-many", "wide-fluff", "wide-fluff"):
            for _ in range(6):
                check_partial_fluff_runs(args.recover, fixtures[name], rng, xwork)
        for name in ("tree-fluff", "vol-fluff", "vol-many"):
            for how in ("larger",) * 6 + ("smaller",) * 2 + ("comp",) * 2:
                check_partial_fluff_size_after_loss(args.recover, fixtures[name], rng,
                                                    xwork, how=how)
        for name in ("vol-stored", "vol-lz", "vol-lite", "vol-many", "vol-fluff"):
            for _ in range(3):
                check_partial_lying_full(args.recover, fixtures[name], rng, xwork)
        for name in ("tree-stored", "tree-lz", "tree-lite", "tree-fluff", "vol-stored",
                     "vol-lz", "vol-lite", "vol-fluff"):
            for _ in range(8):
                check_partial_two_size_faults(args.recover, fixtures[name], rng, xwork)
        for name, fx in fixtures.items():
            for _ in range(args.sweep):
                kind = rng.choice(rf.DAMAGE_KINDS)
                if fx.vs.volume_count == 1 and kind in (
                        "delete-volume", "shuffle-names", "duplicate-volume"):
                    continue
                check_extract_damage(args.recover, fx, kind, rng, xwork,
                                     label="sweep " + name)

        # Phase 6: rebuild.
        for name, fx in fixtures.items():
            for vsize in (None, 20000, 65536):
                check_rebuild(args.recover, args.boltpack, fx, xwork, vsize=vsize,
                              expect_rc=0)
        for name in ("tree-lz", "tree-fluff", "vol-lite", "vol-fluff"):
            for _ in range(3):
                check_rebuild_checksum_lie(args.recover, args.boltpack, fixtures[name],
                                           rng, xwork)
        check_rebuild_duplicates(args.recover, args.boltpack, xwork)
        for name in ("tree-lz", "vol-fluff", "synthetic"):
            check_rebuild_hostile(args.recover, args.boltpack, fixtures[name], xwork)
        for kind in rf.DAMAGE_KINDS:
            for name, fx in fixtures.items():
                if fx.vs.volume_count == 1 and kind in (
                        "delete-volume", "shuffle-names", "duplicate-volume"):
                    continue
                damage, ctx = rf.make_damage(fx, kind, rng)
                check_rebuild(args.recover, args.boltpack, fx, xwork,
                              vsize=rng.choice((None, 20000)), damage=damage,
                              ctx=ctx, kind="%s (%s)" % (kind, damage.description))

        check_robust(args.recover, fixtures["tree-fluff"], rng, work)
        for name in ("vol-fluff", "vol-stored", "vol-many"):
            check_duplicate_order(args.recover, fixtures[name], rng, work)
        for name in ("nested-stored", "nested-fluff"):
            check_nested(args.recover, fixtures[name], rng, work)
            check_known_limitation(args.recover, fixtures[name], work)

        # Seeded sweep: random kinds on random fixtures.
        kinds = [k for k in rf.DAMAGE_KINDS]
        for name, fx in fixtures.items():
            for _ in range(args.sweep):
                kind = rng.choice(kinds)
                if fx.vs.volume_count == 1 and kind in (
                        "delete-volume", "shuffle-names", "duplicate-volume"):
                    continue
                check_damage(args.recover, fx, kind, rng, work,
                             label="sweep " + name)
    except NamesOnlyDone:
        pass
    finally:
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)

    if limitations:
        print("\nKnown limitations (not failures):")
        for item in limitations:
            print("  " + item)
    if skipped:
        print("\nChecks that cannot mean the same thing on this host (skipped, counted):")
        for k, n in sorted(skipped.items()):
            print("  %6d  %s" % (n, k))
    if timeouts["retried"]:
        print("\nProcess time limits hit: %d (each retried once under Wine)" %
              timeouts["retried"])
    print("\nScenarios: " + ", ".join("%d %s" % (n, k)
                                       for k, n in scenarios.items()))
    print("%d passed, %d failed (checks include per-entry assertions; "
          "scenarios above are the independent runs)" % (passed, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
