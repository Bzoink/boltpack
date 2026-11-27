"""Ground truth for Fluff history damage, independent of boltpack-recover.

A Fluff block can copy bytes from the 256 KiB of output before it, across block
boundaries.  When some blocks of an entry are lost, every output byte that was
copied (directly or through other copies) from a lost block is unknowable; the
rest is fine.  This module computes that set for an *intact* fixture:

  * the core Fluff decoder is loaded through ctypes (fluff.c built as a shared
    object), so it shares no code with the tool;
  * the entry is decoded block by block from the true prefix;
  * every lost block is replaced by random filler (different for every seed;
    the tool uses complements, this does not);
  * an output byte that differs from the true payload was influenced by the
    hole.  The union over several seeds is the tainted set (a tainted byte
    escapes one seed with probability 1/256, all three with about 2**-24).
"""
import ctypes
import os
import random
import subprocess

WINDOW = 1 << 18
MIN_OK = 64        # the tool reports an ok run shorter than this, between two
                   # tainted runs of one block, as tainted too
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def smooth(bad, lo, hi, min_ok=MIN_OK):
    """Within [lo, hi) (one block), mark as tainted every run of untainted bytes
    shorter than min_ok that has tainted bytes on both sides."""
    i = lo
    while i < hi:
        j = i
        while j < hi and bool(bad[j]) == bool(bad[i]):
            j += 1
        if not bad[i] and j - i < min_ok and i > lo and j < hi:
            for k in range(i, j):
                bad[k] = 1
        i = j


class FluffOracle(object):
    def __init__(self, workdir):
        so = os.path.join(workdir, "libfluff-oracle.so")
        if not os.path.exists(so):
            subprocess.check_call(
                ["cc", "-shared", "-fPIC", "-O2", "-I" + os.path.join(ROOT, "src/core"),
                 "-o", so, os.path.join(ROOT, "src/core/fluff.c")])
        lib = ctypes.CDLL(so)
        lib.fluff_ctx_alloc.restype = ctypes.c_void_p
        lib.fluff_ctx_free.argtypes = [ctypes.c_void_p]
        lib.fluff_ctx_reset_decode.argtypes = [ctypes.c_void_p]
        lib.fluff_ctx_feed_raw.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                           ctypes.c_uint32]
        lib.fluff_decompress_block_ctx.argtypes = [
            ctypes.c_char_p, ctypes.c_uint32, ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
        lib.fluff_decompress_block_ctx.restype = ctypes.c_int
        self.lib = lib

    def tainted(self, te, vs, payload, lost, seeds=(11, 22, 33), min_ok=MIN_OK):
        """Byte positions of entry te that cannot be recovered when the blocks
        whose indexes are in `lost` are gone, not counting those blocks'
        own bytes.  Returns a sorted list of (start, end) ranges."""
        blocks = [s for s in te.structures if s.kind == "block"]
        bad = bytearray(len(payload))
        lib = self.lib
        for seed in seeds:
            rnd = random.Random(seed)
            ctx = lib.fluff_ctx_alloc()
            lib.fluff_ctx_reset_decode(ctx)
            pos = 0
            for i, ((orig, comp), s) in enumerate(zip(te.blocks, blocks)):
                if i in lost:
                    n = min(orig, WINDOW)
                    lib.fluff_ctx_feed_raw(ctx, rnd.randbytes(n), n)
                else:
                    data = bytes(vs.data[s.volume][s.start + 12:s.start + 12 + comp])
                    if comp == orig:
                        lib.fluff_ctx_feed_raw(ctx, data, orig)
                    else:
                        out = ctypes.create_string_buffer(orig)
                        dl = ctypes.c_uint32(orig)
                        rc = lib.fluff_decompress_block_ctx(
                            data, comp, out, ctypes.byref(dl), ctx)
                        if rc != 0 or dl.value != orig:
                            raise ValueError("oracle: block %d does not decode" % i)
                        got = out.raw
                        want = payload[pos:pos + orig]
                        if got != want:
                            for k in range(orig):
                                if got[k] != want[k]:
                                    bad[pos + k] = 1
                            smooth(bad, pos, pos + orig, min_ok)
                pos += orig
            lib.fluff_ctx_free(ctx)
        out = []
        i = 0
        while i < len(bad):
            if bad[i]:
                j = i
                while j < len(bad) and bad[j]:
                    j += 1
                out.append((i, j))
                i = j
            else:
                i += 1
        return out
