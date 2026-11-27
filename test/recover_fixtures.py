"""Fixtures and damage for the boltpack-recover tests.

Fixtures are generated into a temporary directory at test time (*.bolt is
gitignored, and archive bytes are not reproducible anyway: the CLI takes the
set id from the clock).  What is reproducible is structural: file contents
and mtimes are fixed, the damage RNG is seeded, and damage is placed by
structure ("the middle of entry k's second block", "the table header") instead
of by absolute offset.  A test therefore checks structural expectations, never
byte-identical archives.

Ground truth comes from bp_set_bytes.parse_set() on the intact set, before any
damage, and never from the tool under test.
"""
import os
import random
import shutil
import struct
import subprocess

import bp_archive_bytes as bab
import bp_set_bytes as sb

WORDS = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu "
         "nu xi omicron pi rho sigma tau upsilon phi chi psi omega archive "
         "volume block entry header trailer table record stream window").split()

FIXED_TIME = 1700000000


class CoverageError(Exception):
    pass


# --- content ------------------------------------------------------------

def text_bytes(rng, size):
    out = []
    n = 0
    while n < size:
        line = " ".join(rng.choice(WORDS) for _ in range(rng.randint(6, 14)))
        out.append(line + "\n")
        n += len(line) + 1
    return "".join(out).encode()[:size]


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def fix_times(root, extra=0):
    for dirpath, dirnames, filenames in os.walk(root):
        for name in dirnames + filenames:
            p = os.path.join(dirpath, name)
            os.utime(p, (FIXED_TIME + extra, FIXED_TIME + extra),
                     follow_symlinks=False)
        os.utime(dirpath, (FIXED_TIME, FIXED_TIME), follow_symlinks=False)


def make_tree(root, seed, scale=1.0, many=0):
    """A source tree covering the entry shapes the CLI writes: text, random
    (incompressible), empty, mixed, tiny, nested directories and a symlink.
    Returns the input names in a fixed order (relative to root's parent)."""
    rng = random.Random(seed)
    top = os.path.basename(root)
    write(os.path.join(root, "text.txt"), text_bytes(rng, int(120000 * scale)))
    write(os.path.join(root, "code.c"),
          b"".join(b"int f%d(int x){return x*%d+1;}\n" % (i, i)
                   for i in range(int(900 * scale))))
    write(os.path.join(root, "rnd.bin"), rng.randbytes(int(40000 * scale)))
    write(os.path.join(root, "empty"), b"")
    write(os.path.join(root, "tiny"), b"x")
    write(os.path.join(root, "mixed.dat"),
          text_bytes(rng, int(30000 * scale)) + rng.randbytes(int(30000 * scale)) +
          text_bytes(rng, int(10000 * scale)))
    write(os.path.join(root, "sub", "a.txt"), text_bytes(rng, 3000))
    write(os.path.join(root, "sub", "deep", "b.txt"), text_bytes(rng, 9000))
    write(os.path.join(root, "sub", "deep", "c.bin"), rng.randbytes(5000))
    os.symlink("text.txt", os.path.join(root, "link"))
    names = ["text.txt", "code.c", "rnd.bin", "empty", "tiny", "mixed.dat",
             "sub", "link"]
    for i in range(many):
        write(os.path.join(root, "many", "f%04d.txt" % i),
              text_bytes(rng, rng.randint(80, 400)))
    if many:
        names.append("many")
    fix_times(root)
    return ["%s/%s" % (top, n) for n in names]


# --- fixture creation ----------------------------------------------------

class Fixture(object):
    def __init__(self, name, directory, paths, vs):
        self.name = name
        self.directory = directory
        self.paths = sorted(paths)
        self.vs = vs
        self.srcdir = None      # CLI fixtures: the tree the archive was made from
        self.payloads = None    # byte-built fixtures: entry index -> payload

    def expected(self, te):
        """What extracting entry te must produce: ("file", bytes),
        ("symlink", target) or ("dir", None); None for entries that are not
        extracted by name (resource forks, metadata).  Taken from the source
        the archive was made from, never from any archive reader."""
        if te.entry_type not in (0, 3, 4):
            return None
        if self.payloads is not None:
            if te.entry_type == 4:
                return ("dir", None)
            data = self.payloads[te.index]
            if te.entry_type == 3:
                return ("symlink", data.decode("utf-8"))
            return ("file", data)
        p = os.path.join(self.srcdir, te.name.decode("utf-8").rstrip("/"))
        if os.path.islink(p):
            return ("symlink", os.readlink(p))
        if os.path.isdir(p):
            return ("dir", None)
        with open(p, "rb") as f:
            return ("file", f.read())

    def volume_files(self):
        """volume number -> path"""
        return dict(self.vs.paths)


def run_cli(boltpack, cwd, args):
    r = subprocess.run([boltpack] + args, cwd=cwd, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    if r.returncode != 0:
        raise RuntimeError("boltpack %s failed: %s" %
                           (" ".join(args), r.stdout.decode(errors="replace")))
    return r.stdout.decode(errors="replace")


def cli_fixture(boltpack, outdir, name, srcdir, inputs, args, required=()):
    os.makedirs(outdir, exist_ok=True)
    archive = os.path.join(outdir, name + ".bolt")
    run_cli(boltpack, srcdir, ["create", "-qq"] + list(args) +
            [archive] + list(inputs))
    files = sorted(os.path.join(outdir, f) for f in os.listdir(outdir)
                   if f.startswith(name + ".") and f != name + ".dup")
    vs = sb.parse_set(files)
    sb.check_chain(vs)
    fx = Fixture(name, outdir, files, vs)
    fx.srcdir = srcdir
    check_coverage(fx, required)
    return fx


def synthetic_fixture(outdir, name="synthetic"):
    """One archive built with the byte-level writer so it contains entry kinds
    the CLI does not emit on this host: resource fork, metadata, unicode and
    long names, a multi-block entry."""
    os.makedirs(outdir, exist_ok=True)
    rng = random.Random(77)
    e = bab.entry
    entries = [
        e("docs/", entry_type=4),
        e("docs/readme.txt", text_bytes(rng, 20000), block_size=4096),
        e("empty", b""),
        e("link", b"docs/readme.txt", entry_type=3),
        e("forked", b"data fork " * 50),
        e("forked", rng.randbytes(700), entry_type=1, platform=0x02),
        e("forked", b"\x01\x02\x03meta", entry_type=2, platform=0xFE),
        e("ångström/日本語.txt", b"unicode " * 40),
        e("long/" + "d" * 200 + "/" + "n" * 200, b"long name"),
        e("big.bin", rng.randbytes(33000), block_size=8192),
        e("last", b"last entry"),
    ]
    data = bab.build7({"entries": entries})
    path = os.path.join(outdir, name + ".bolt")
    with open(path, "wb") as f:
        f.write(data)
    vs = sb.parse_set([path])
    sb.check_chain(vs)
    fx = Fixture(name, outdir, [path], vs)
    fx.payloads = dict((i, b"".join(d for _o, d in ent["blocks"]))
                       for i, ent in enumerate(entries))
    check_coverage(fx, ("types01234", "empty-entry", "multi-block"))
    return fx


def check_coverage(fx, required):
    """Fail loudly when a fixture does not contain what it is meant to."""
    vs = fx.vs
    have = {}
    have["types01234"] = {0, 1, 2, 3, 4} <= set(e.entry_type for e in vs.entries)
    have["symlink"] = any(e.entry_type == 3 for e in vs.entries)
    have["directory"] = any(e.entry_type == 4 for e in vs.entries)
    have["empty-entry"] = any(e.original_size == 0 and e.entry_type == 0
                              for e in vs.entries)
    have["multi-block"] = any(len(e.blocks) > 1 for e in vs.entries)
    have["multi-volume"] = vs.volume_count >= 3
    have["eov-block"] = vs.has_short_end_of_volume_block()
    have["crossing-entry"] = bool(vs.entries_crossing_volumes())
    have["table-spans"] = vs.table_spans_volumes()
    have["raw-block"] = any(comp == orig for e in vs.entries
                            for (orig, comp) in e.blocks)
    have["compressed-block"] = any(comp < orig for e in vs.entries
                                   for (orig, comp) in e.blocks)
    have["nested-hits"] = _nested_hits(vs) > 0
    for r in required:
        if not have.get(r):
            raise CoverageError("fixture %s lacks %s (have %s)" % (
                fx.name, r, sorted(k for k, v in have.items() if v)))
    return have


def _nested_hits(vs):
    """Header-shaped CRC-valid bytes that are not real entries."""
    real = set((e.volume, e.offset) for e in vs.entries)
    n = 0
    for v, raw in vs.data.items():
        for off in sb.raw_header_hits(raw, sb.VOLUME_HEADER,
                                      len(raw) - sb.VOLUME_TRAILER):
            if (v, off) not in real:
                n += 1
    return n


def build_all(boltpack, root):
    """Build every fixture under root; returns a dict name -> Fixture."""
    out = {}
    src = os.path.join(root, "src")

    tree = os.path.join(src, "t1")
    names = make_tree(tree, seed=1)
    for tag, flags in (("stored", ["-0"]), ("lite", ["-1", "-b", "8k"]),
                       ("lz", ["-2", "-b", "16k"]),
                       ("fluff", ["-F", "-b", "16k"])):
        req = ["symlink", "directory", "empty-entry", "multi-block"]
        if tag != "stored":
            req.append("compressed-block")
        out["tree-" + tag] = cli_fixture(
            boltpack, os.path.join(root, "tree-" + tag), "tree-" + tag,
            src, names, flags, req)

    big = os.path.join(src, "t2")
    names2 = make_tree(big, seed=2, scale=2.2)
    out["vol-fluff"] = cli_fixture(
        boltpack, os.path.join(root, "vol-fluff"), "vol-fluff", src, names2,
        ["-F", "-b", "8k", "-v", "16K"],
        ["multi-volume", "eov-block", "crossing-entry", "symlink",
         "directory", "empty-entry", "compressed-block", "raw-block"])
    out["vol-stored"] = cli_fixture(
        boltpack, os.path.join(root, "vol-stored"), "vol-stored", src, names2,
        ["-0", "-b", "8k", "-v", "16K"],
        ["multi-volume", "eov-block", "crossing-entry"])

    out["vol-lz"] = cli_fixture(
        boltpack, os.path.join(root, "vol-lz"), "vol-lz", src, names2,
        ["-2", "-b", "8k", "-v", "16K"],
        ["multi-volume", "eov-block", "crossing-entry", "compressed-block"])
    out["vol-lite"] = cli_fixture(
        boltpack, os.path.join(root, "vol-lite"), "vol-lite", src, names2,
        ["-1", "-b", "8k", "-v", "16K"],
        ["multi-volume", "eov-block", "crossing-entry", "compressed-block"])

    # One entry several times the Fluff window (256 KiB) with small blocks, so
    # the history ring wraps many times inside a run of blocks.
    wide = os.path.join(src, "t4")
    wrng = random.Random(8)
    write(os.path.join(wide, "wide.txt"), text_bytes(wrng, 700000) +
          wrng.randbytes(3000) + text_bytes(wrng, 40000))
    fix_times(wide)
    out["wide-fluff"] = cli_fixture(
        boltpack, os.path.join(root, "wide-fluff"), "wide-fluff", src,
        ["t4/wide.txt"], ["-F", "-b", "8k"], ["multi-block", "compressed-block"])

    many = os.path.join(src, "t3")
    names3 = make_tree(many, seed=3, scale=0.2, many=900)
    out["vol-many"] = cli_fixture(
        boltpack, os.path.join(root, "vol-many"), "vol-many", src, names3,
        ["-F", "-b", "8k", "-v", "16K"],
        ["multi-volume", "table-spans", "symlink", "empty-entry"])

    # Nested: a .bolt held inside a .bolt.  Order the inputs so entries follow
    # the inner archive, and keep the inner archive incompressible so its bytes
    # sit in raw blocks (Fluff stores a block raw when it saves under 1%).
    inner_src = os.path.join(src, "inner")
    irng = random.Random(4)
    for i, size in enumerate((9000, 12000, 20000, 700)):
        write(os.path.join(inner_src, "r%d.bin" % i), irng.randbytes(size))
    write(os.path.join(inner_src, "dir", "empty"), b"")
    fix_times(inner_src)
    inner_names = ["inner/r0.bin", "inner/r1.bin", "inner/dir", "inner/r2.bin",
                   "inner/r3.bin"]
    inner_dir = os.path.join(root, "inner")
    os.makedirs(inner_dir, exist_ok=True)
    inner_archive = os.path.join(inner_dir, "inner.bolt")
    run_cli(boltpack, src, ["create", "-qq", "-0", "-b", "8k", inner_archive] +
            inner_names)
    outer = os.path.join(src, "outer")
    write(os.path.join(outer, "a_first.txt"),
          text_bytes(random.Random(5), 6000))
    shutil.copy(inner_archive, os.path.join(outer, "inner.bolt"))
    write(os.path.join(outer, "z_after1.txt"),
          text_bytes(random.Random(6), 7000))
    write(os.path.join(outer, "z_after2.txt"),
          text_bytes(random.Random(7), 2000))
    fix_times(outer)
    onames = ["outer/a_first.txt", "outer/inner.bolt", "outer/z_after1.txt",
              "outer/z_after2.txt"]
    out["nested-stored"] = cli_fixture(
        boltpack, os.path.join(root, "nested-stored"), "nested-stored", src,
        onames, ["-0", "-b", "8k"], ["nested-hits", "multi-block"])
    out["nested-fluff"] = cli_fixture(
        boltpack, os.path.join(root, "nested-fluff"), "nested-fluff", src,
        onames, ["-F", "-b", "8k"], ["nested-hits", "raw-block"])

    out["synthetic"] = synthetic_fixture(os.path.join(root, "synthetic"))
    return out


# --- damage -------------------------------------------------------------

class Damage(object):
    """What was done to a copy of a fixture, and what is still intact."""

    def __init__(self, kind, description):
        self.kind = kind
        self.description = description
        self.changed = []       # (volume, start, end), bytes that really changed
        self.lost_volumes = set()
        self.set_notes = {}     # expectations about the set, see check_recover
        self.dir = None
        self.paths = []

    def intact_entries(self, vs):
        out = []
        for e in vs.entries:
            ok = True
            for s in e.structures:
                if s.volume in self.lost_volumes:
                    ok = False
                    break
                for (v, a, b) in self.changed:
                    if v == s.volume and a < s.end and s.start < b:
                        ok = False
                        break
                if not ok:
                    break
            if ok:
                out.append(e)
        return out


def _changed_ranges(vol, before, after):
    """Ranges where after differs from before (including truncated tails)."""
    out = []
    n = min(len(before), len(after))
    i = 0
    while i < n:
        if before[i] != after[i]:
            j = i
            while j < n and before[j] != after[j]:
                j += 1
            out.append((vol, i, j))
            i = j
        else:
            i += 1
    if len(before) > len(after):
        out.append((vol, n, len(before)))
    return out


def _mid(a, b):
    return (a + b) // 2


def _pick_entry(rng, vs, pred=lambda e: True):
    cands = [e for e in vs.entries if pred(e)]
    return rng.choice(cands) if cands else None


class DamageContext(object):
    """Mutable copies of a fixture's volumes, plus the structure map."""

    def __init__(self, fx):
        self.fx = fx
        self.vs = fx.vs
        self.data = dict((v, bytearray(raw)) for v, raw in fx.vs.data.items())
        self.names = dict((v, os.path.basename(p))
                          for v, p in fx.vs.paths.items())
        self.extra = {}           # file name -> bytes (stranger/duplicate)
        self.deleted = set()

    def flip(self, vol, off, bit=0):
        self.data[vol][off] ^= 1 << bit

    def zero(self, vol, start, end):
        self.data[vol][start:end] = bytes(end - start)

    def truncate(self, vol, length):
        del self.data[vol][length:]


def _block(e, i=0):
    return [s for s in e.structures if s.kind == sb.KIND_BLOCK][i]


def make_damage(fx, kind, rng, param=None):
    """Return (Damage, DamageContext) for one damage kind.  kind is one of
    DAMAGE_KINDS.  Selection of the target uses rng; param narrows it."""
    vs = fx.vs
    ctx = DamageContext(fx)
    d = Damage(kind, kind)
    last = vs.volume_count - 1

    def block_target(min_blocks=1):
        e = _pick_entry(rng, vs, lambda e: len(e.blocks) >= min_blocks)
        return e

    if kind == "block-data-bit":
        e = block_target()
        i = rng.randrange(len(e.blocks))
        s = _block(e, i)
        off = s.start + 12 + rng.randrange(s.length - 12)
        ctx.flip(s.volume, off, rng.randrange(8))
        d.description = "bit in data of entry %d block %d" % (e.index, i)
    elif kind in ("block-comp-size", "block-orig-size", "block-crc"):
        e = block_target()
        i = rng.randrange(len(e.blocks))
        s = _block(e, i)
        base = {"block-comp-size": 0, "block-orig-size": 4,
                "block-crc": 8}[kind]
        ctx.flip(s.volume, s.start + base + rng.randrange(4), rng.randrange(8))
        d.description = "%s of entry %d block %d" % (kind, e.index, i)
    elif kind == "entry-header-bit":
        e = _pick_entry(rng, vs)
        s = e.structures[0]
        ctx.flip(s.volume, s.start + rng.randrange(s.length), rng.randrange(8))
        d.description = "bit in header of entry %d" % e.index
    elif kind == "entry-trailer-bit":
        e = _pick_entry(rng, vs)
        s = e.structures[-1]
        ctx.flip(s.volume, s.start + rng.randrange(s.length), rng.randrange(8))
        d.description = "bit in trailer of entry %d" % e.index
    elif kind == "zero-entry":
        e = _pick_entry(rng, vs, lambda e: len(e.blocks) >= 1)
        for s in e.structures:
            ctx.zero(s.volume, s.start, s.end)
        d.description = "entry %d zeroed" % e.index
    elif kind == "zero-span":
        n = param or rng.choice((512, 4096, 20000))
        v = rng.randrange(vs.volume_count)
        body = ctx.data[v]
        start = rng.randrange(sb.VOLUME_HEADER,
                              max(sb.VOLUME_HEADER + 1,
                                  len(body) - sb.VOLUME_TRAILER - n))
        end = min(start + n, len(body) - sb.VOLUME_TRAILER)
        ctx.zero(v, start, end)
        d.description = "%d bytes zeroed at %d in volume %d" % (n, start, v)
    elif kind == "truncate-before-table":
        v = vs.table_volume
        ctx.truncate(last, vs.table_offset if v == last else
                     len(ctx.data[last]) - sb.VOLUME_TRAILER - sb.END_RECORD)
        d.description = "last volume cut at the table"
    elif kind == "truncate-trailer":
        ctx.truncate(last, len(ctx.data[last]) - sb.VOLUME_TRAILER)
        d.description = "last volume trailer cut off"
    elif kind == "zero-end-and-trailer":
        n = len(ctx.data[last])
        ctx.zero(last, n - sb.VOLUME_TRAILER - sb.END_RECORD, n)
        d.description = "end record and trailer zeroed"
    elif kind == "table-magic":
        t = vs.table_structures[0]
        ctx.flip(t.volume, t.start, 0)
        d.description = "table header magic flipped"
    elif kind == "table-record":
        t = vs.table_structures[-1]
        ctx.flip(t.volume, t.start + t.length - 3, 0)
        d.description = "bit in the last table structure"
    elif kind == "volume-header":
        v = param if param is not None else rng.randrange(vs.volume_count)
        ctx.zero(v, 0, sb.VOLUME_HEADER)
        d.description = "volume %d header zeroed" % v
    elif kind == "volume-trailer":
        v = param if param is not None else rng.randrange(vs.volume_count)
        n = len(ctx.data[v])
        ctx.zero(v, n - sb.VOLUME_TRAILER, n)
        d.description = "volume %d trailer zeroed" % v
    elif kind == "volume-header-and-trailer":
        v = param if param is not None else rng.randrange(vs.volume_count)
        n = len(ctx.data[v])
        ctx.zero(v, 0, sb.VOLUME_HEADER)
        ctx.zero(v, n - sb.VOLUME_TRAILER, n)
        d.description = "volume %d header and trailer zeroed" % v
    elif kind == "delete-volume":
        v = param if param is not None else rng.randrange(vs.volume_count)
        ctx.deleted.add(v)
        d.lost_volumes.add(v)
        d.description = "volume %d deleted" % v
    elif kind == "shuffle-names":
        names = ["zz%d.dat" % i for i in range(vs.volume_count)]
        rng.shuffle(names)
        ctx.names = dict((v, names[v]) for v in range(vs.volume_count))
        d.description = "volume files renamed at random"
    elif kind == "duplicate-volume":
        v = param if param is not None else rng.randrange(vs.volume_count)
        copy = bytearray(vs.data[v])
        copy[sb.VOLUME_HEADER + rng.randrange(100)] ^= 0x40
        ctx.extra["copy-of-%d.dup" % v] = bytes(copy)
        d.description = "a damaged duplicate of volume %d added" % v
    elif kind == "random-flips":
        n = param or rng.randint(1, 6)
        for _ in range(n):
            v = rng.randrange(vs.volume_count)
            ctx.flip(v, rng.randrange(len(ctx.data[v])), rng.randrange(8))
        d.description = "%d random bit flips" % n
    else:
        raise ValueError(kind)

    for v in range(vs.volume_count):
        if v in ctx.deleted:
            continue
        d.changed.extend(_changed_ranges(v, vs.data[v], ctx.data[v]))
    return d, ctx


DAMAGE_KINDS = (
    "block-data-bit", "block-comp-size", "block-orig-size", "block-crc",
    "entry-header-bit", "entry-trailer-bit", "zero-entry", "zero-span",
    "truncate-before-table", "truncate-trailer", "zero-end-and-trailer",
    "table-magic", "table-record", "volume-header", "volume-trailer",
    "volume-header-and-trailer", "delete-volume", "shuffle-names",
    "duplicate-volume", "random-flips")


def write_damaged(ctx, damage, directory):
    """Write the damaged copy of the set into directory (created fresh)."""
    if os.path.isdir(directory):
        shutil.rmtree(directory)
    os.makedirs(directory)
    paths = []
    for v in sorted(ctx.data):
        if v in ctx.deleted:
            continue
        p = os.path.join(directory, ctx.names[v])
        with open(p, "wb") as f:
            f.write(bytes(ctx.data[v]))
        paths.append(p)
    for name, data in ctx.extra.items():
        p = os.path.join(directory, name)
        with open(p, "wb") as f:
            f.write(data)
        paths.append(p)
    damage.dir = directory
    damage.paths = paths
    return paths
