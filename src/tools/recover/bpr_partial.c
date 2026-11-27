/*
 * bpr_partial.c -- recover what survives of an entry that lost blocks.
 *
 * The output is NAME.damaged, exactly original_size bytes long: bytes from
 * verified blocks in place, everything else zero.  The report lists every byte
 * range as ok or lost, with the reason.  The one rule that matters: a byte is
 * called ok only if its block's CRC-32 matched, the block decoded, and the
 * block's position in the payload is certain.
 *
 * Position.  A block's output offset is the sum of the uncompressed sizes of
 * the blocks before it (a forward chain), or original_size minus the sizes of
 * the blocks after it (a chain backwards from an anchored end).  Neither size
 * field is covered by a CRC, so a size is trusted only when
 *   - the block's data CRC matched and the block then decoded to exactly that
 *     many bytes (a wrong size makes an exact-length decoder fail; a raw block
 *     must have comp == orig), or
 *   - the block failed its CRC but is a full block (orig == block_size), or
 *   - it is the one block of the entry whose size is unknown, in which case
 *     it is original_size minus every other block's size (deduction).
 *
 * Deduction needs an anchored end: the blocks after the unknown one are walked
 * by their compressed sizes until a valid entry trailer, and the walk must end
 * exactly where the entry ends.  "Exactly" means one of
 *   - the trailer's compressed_size equals the stream distance from the
 *     entry's first block to the trailer (all volumes in between present), or
 *   - the trusted file table says the next entry starts 16 bytes later.
 * A walk that lands anywhere else (a nested archive stored in the entry ends at
 * its own trailer, not this one) is rejected, so those blocks are never placed.
 *
 * Two or more unknown sizes cannot be deduced.  Then the blocks up to the last
 * unknown one are lost and only the blocks after it, placed backwards from the
 * end, are recovered.  When a damaged size field hides where the next block
 * starts, the tool scans forward for a block header followed by data whose CRC
 * matches and whose chain reaches the anchored end.
 *
 * Fluff.  Blocks keep a 256 KiB history window across blocks.  After a lost
 * block, a later compressed Fluff block cannot be decoded reliably and is lost
 * as "fluff-history"; raw blocks (comp == orig) are verbatim and still ok.
 * BoltLite and BoltLZ blocks are independent.
 */
#include "bpr.h"

#include "bp_types.h"
#include "fluff.h"

#include <stdlib.h>
#include <string.h>

#define SCAN_CHUNK (1u << 20)
#define FLUFF_WINDOW (1u << 18)     /* history a Fluff block can reach back to */

enum { SZ_T = 0, SZ_F, SZ_U };      /* how a block's size is known */

typedef struct {
    int         v;
    uint64_t    off;                /* header position, normalized */
    uint32_t    comp, orig, crc;
    int         hdr;                /* the 12 header bytes were readable */
    int         plausible;          /* sizes pass the structural checks */
    int         crc_ok;             /* data read and its CRC-32 matched */
    const char *reason;             /* why the block could not be loaded */
} pblk;

typedef struct {
    int      anchored;              /* the walk ends exactly at the entry's end */
    int      trailer;               /* the walk ended at a CRC-valid entry trailer */
    int      dist_ok;               /* ... and the stream distance to it is known: */
    uint64_t dist, csize;           /*   dist bytes of blocks; the trailer says csize */
    int      ok_seen;               /* a block with a matching CRC was found */
    uint64_t nU;                    /* blocks whose size is unknown */
    uint64_t nblocks;
    uint64_t sum_all;               /* sum of orig fields of every block */
    int      sv;                    /* start of the trusted suffix: the first */
    uint64_t soff;                  /*   block after the last unknown one */
    uint64_t suffix_orig;           /* sum of orig over the trusted suffix */
} pchain;

typedef struct {
    bpr_ctx         *c;
    const bpr_entry *e;
    bpr_xres        *r;
    bpr_out         *out;
    bpr_decoder      d, sd;         /* emit decoder, survey decoder */
    int              have_d, have_sd;
    uint32_t         bs;
    int              dv;            /* start of the entry's first block */
    uint64_t         doff;
    int              have_tend;     /* trusted table: where the blocks end */
    int              tv;
    uint64_t         toff;
    int              v;             /* walk position */
    uint64_t         off, out_pos, remaining, comp_sum;
    int              lost_seen, resumed, verified, no_anchor;
    int              unsure;        /* a block was placed on the full-size rule alone */
    uint32_t         crc;           /* CRC-32 of the bytes written so far */
    int              memo_on;       /* scan: remember block starts of failed walks */
    uint64_t        *memo;          /*   (open-addressing set, 0 = empty) */
    size_t           memo_cap, memo_n;
    uint64_t        *tmp;           /*   block starts visited by the walk in progress */
    size_t           tmp_cap, tmp_n;
    /* Fluff history.  ring[0] holds the last FLUFF_WINDOW bytes of the payload
     * as decoder A sees them.  After the first loss a second decoder B runs
     * alongside, and ring[1] is its history: lost stretches are filled with
     * bytes in A and their complements in B, so a decoded byte that differs
     * between the two was copied (directly or through other copies) from a
     * lost stretch. */
    uint8_t         *ring[2];
    size_t           ring_n, ring_w;
    int              taint;         /* decoder B is running */
    bpr_decoder      d2;
    int              have_d2;
    uint8_t         *fill;          /* 2 * FLUFF_WINDOW filler bytes */
    uint32_t         rng;
    const char      *stop;
} pctx;

static int add_range(bpr_xres *r, uint64_t s, uint64_t e, int ok,
                     const char *reason)
{
    bpr_range *last;
    if (e <= s) return 0;
    if (r->nranges > 0) {
        last = &r->ranges[r->nranges - 1];
        if (last->e == s && last->ok == ok &&
            (ok || last->reason == reason ||
             (last->reason && reason && strcmp(last->reason, reason) == 0))) {
            last->e = e;
            goto count;
        }
    }
    if (r->nranges == r->capranges) {
        size_t cap = r->capranges ? r->capranges * 2 : 8;
        bpr_range *n = (bpr_range *)realloc(r->ranges, cap * sizeof(*n));
        if (n == NULL) return -1;
        r->ranges = n;
        r->capranges = cap;
    }
    last = &r->ranges[r->nranges++];
    last->s = s;
    last->e = e;
    last->ok = ok;
    last->reason = ok ? NULL : reason;
count:
    if (ok) r->bytes_ok += e - s;
    else r->bytes_lost += e - s;
    return 0;
}

static int write_zeros(bpr_out *o, uint64_t n)
{
    static const uint8_t z[65536];
    while (n > 0) {
        size_t k = n > sizeof z ? sizeof z : (size_t)n;
        if (bpr_out_write(o, z, k) != 0) return -1;
        n -= k;
    }
    return 0;
}

/* ---- Fluff history --------------------------------------------------- */

static void ring_put(uint8_t *ring, size_t w, const uint8_t *b, size_t n)
{
    if (n > FLUFF_WINDOW) {
        b += n - FLUFF_WINDOW;
        w = (w + (n - FLUFF_WINDOW)) % FLUFF_WINDOW;
        n = FLUFF_WINDOW;
    }
    while (n > 0) {
        size_t take = FLUFF_WINDOW - w;
        if (take > n) take = n;
        memcpy(ring + w, b, take);
        w = (w + take) % FLUFF_WINDOW;
        b += take;
        n -= take;
    }
}

/* Append n output bytes to the history rings: a as decoder A produced them,
 * b as decoder B did (the same unless the stretch was lost). */
static void ring_push(pctx *p, const uint8_t *a, const uint8_t *b, size_t n)
{
    if (p->ring[0] == NULL) return;
    ring_put(p->ring[0], p->ring_w, a, n);
    if (p->taint) ring_put(p->ring[1], p->ring_w, b, n);
    p->ring_w = (p->ring_w + n) % FLUFF_WINDOW;
    p->ring_n += n;
    if (p->ring_n > FLUFF_WINDOW) p->ring_n = FLUFF_WINDOW;
}

/* Make a decoder's history equal to a ring (the plain decode-only reset, not
 * the encoder's: preload would allocate the match finder and hash the window). */
static void ring_prime(fluff_ctx_t *f, const uint8_t *ring, size_t n, size_t w)
{
    size_t start = (w + FLUFF_WINDOW - n) % FLUFF_WINDOW;
    fluff_ctx_reset_decode(f);
    if (n == 0) return;
    if (start + n <= FLUFF_WINDOW) {
        fluff_ctx_feed_raw(f, ring + start, (bp_uint32)n);
    } else {
        size_t first = FLUFF_WINDOW - start;
        fluff_ctx_feed_raw(f, ring + start, (bp_uint32)first);
        fluff_ctx_feed_raw(f, ring, (bp_uint32)(n - first));
    }
}

/* A failed decode may have changed a Fluff history window, and a retry would
 * then decode against the wrong history and still "succeed" with wrong bytes.
 * Rebuild each running decoder's history from its own ring. */
static void fluff_refresh(pctx *p)
{
    if (p->ring[0] == NULL) return;
    ring_prime((fluff_ctx_t *)p->d.fctx, p->ring[0], p->ring_n, p->ring_w);
    if (p->taint)
        ring_prime((fluff_ctx_t *)p->d2.fctx, p->ring[1], p->ring_n, p->ring_w);
}

/* Start the second decoder, with the history the first one has now. */
static int taint_begin(pctx *p)
{
    if (p->taint || p->ring[0] == NULL) return 0;
    p->ring[1] = (uint8_t *)malloc(FLUFF_WINDOW);
    p->fill = (uint8_t *)malloc(2u * FLUFF_WINDOW);
    if (p->ring[1] == NULL || p->fill == NULL) return -1;
    memcpy(p->ring[1], p->ring[0], FLUFF_WINDOW);
    if (bpr_dec_begin(&p->d2, BP_METHOD_FLUFF) != 0) return -1;
    p->have_d2 = 1;
    p->taint = 1;
    p->rng = 0x9E3779B9u;
    ring_prime((fluff_ctx_t *)p->d2.fctx, p->ring[1], p->ring_n, p->ring_w);
    return 0;
}

/* n payload bytes were lost.  Advance both histories over them with filler:
 * the same bytes in decoder A and their complements in decoder B, so a copy
 * out of the hole can never come out equal.  Exactly n bytes, or a whole window
 * if n is more (nothing reaches back further than that).  Returns -1 on error. */
static int hole_fill(pctx *p, uint64_t n)
{
    size_t len, i;
    uint8_t *fa, *fb;
    p->lost_seen = 1;
    if (p->ring[0] == NULL) return 0;
    if (taint_begin(p) != 0) return -1;
    len = n > FLUFF_WINDOW ? FLUFF_WINDOW : (size_t)n;
    fa = p->fill;
    fb = p->fill + FLUFF_WINDOW;
    for (i = 0; i < len; i++) {
        p->rng ^= p->rng << 13;
        p->rng ^= p->rng >> 17;
        p->rng ^= p->rng << 5;
        fa[i] = (uint8_t)(p->rng >> 11);
        fb[i] = (uint8_t)~fa[i];
    }
    fluff_ctx_feed_raw((fluff_ctx_t *)p->d.fctx, fa, (bp_uint32)len);
    fluff_ctx_feed_raw((fluff_ctx_t *)p->d2.fctx, fb, (bp_uint32)len);
    ring_push(p, fa, fb, len);
    return 0;
}

/* Decode the block in p->d.in with every decoder that is running.  NULL if all
 * of them succeed; if they disagree, or any fails, the block is not decoded. */
static const char *dec_pair(pctx *p, uint32_t comp, uint32_t orig)
{
    const char *wa, *wb;
    if (!p->taint) return bpr_dec_block(&p->d, comp, orig);
    if (bpr_grow(&p->d2.in, &p->d2.in_cap, comp) != 0) return "no-memory";
    memcpy(p->d2.in, p->d.in, comp);
    wa = bpr_dec_block(&p->d, comp, orig);
    wb = bpr_dec_block(&p->d2, comp, orig);
    return wa != NULL ? wa : wb;
}

/* Write verified payload bytes, keeping a running CRC-32 of the payload. */
static int emit_ok(pctx *p, const void *buf, size_t n)
{
    p->crc = bpr_crc32(p->crc, buf, n);
    ring_push(p, (const uint8_t *)buf, (const uint8_t *)buf, n);
    return bpr_out_write(p->out, buf, n);
}

/* The block just decoded (orig bytes in p->d.out, and p->d2.out if decoder B is
 * running) goes to the output at p->out_pos.  Bytes the two decoders agree on do
 * not depend on anything lost and are ok; the rest are tainted: lost, zeroed. */
static int emit_decoded(pctx *p, uint32_t orig)
{
    const uint8_t *a = p->d.out, *b = p->d2.out;
    size_t i = 0;
    if (!p->taint || memcmp(a, b, orig) == 0) {
        if (emit_ok(p, a, orig) != 0) return -1;
        return add_range(p->r, p->out_pos, p->out_pos + orig, 1, NULL) != 0 ? -1 : 0;
    }
    while (i < orig) {
        size_t j = i;
        int same = a[i] == b[i];
        while (j < orig && (a[j] == b[j]) == same) j++;
        if (same && (j - i >= p->c->taint_min_ok || i == 0 || j == orig)) {
            if (emit_ok(p, a + i, j - i) != 0) return -1;
            if (add_range(p->r, p->out_pos + i, p->out_pos + j, 1, NULL) != 0)
                return -1;
            i = j;
            continue;
        }
        /* A tainted stretch.  A short run of agreeing bytes between two tainted
         * runs of the same block is a few literals among copies: reporting it as
         * ok would only multiply ranges, so it is reported tainted too. */
        for (;;) {
            size_t k = j;
            int s2;
            if (k >= orig) break;
            s2 = a[k] == b[k];
            while (k < orig && (a[k] == b[k]) == s2) k++;
            if (!s2 || (k - j < p->c->taint_min_ok && k < orig)) j = k;
            else break;
        }
        if (write_zeros(p->out, j - i) != 0) return -1;
        ring_push(p, a + i, b + i, j - i);
        if (add_range(p->r, p->out_pos + i, p->out_pos + j, 0, "fluff-taint") != 0)
            return -1;
        i = j;
    }
    return 0;
}

/* ---- geometry -------------------------------------------------------- */

/* Bytes of the archive stream from (v1, o1) to (v2, o2), when every volume
 * between them is present.  0 on success. */
static int stream_dist(const bpr_ctx *c, int v1, uint64_t o1, int v2,
                       uint64_t o2, uint64_t *out)
{
    uint64_t sum = 0;
    int w;
    const bpr_vol *x;
    if (v2 < v1) return -1;
    if (v1 == v2) {
        if (o2 < o1) return -1;
        *out = o2 - o1;
        return 0;
    }
    x = bpr_vol_at(c, v1);
    if (x == NULL || bpr_read_limit(x) < o1) return -1;
    sum += bpr_read_limit(x) - o1;
    for (w = v1 + 1; w < v2; w++) {
        x = bpr_vol_at(c, w);
        if (x == NULL || bpr_read_limit(x) < BPR_VOL_HDR) return -1;
        sum += bpr_read_limit(x) - BPR_VOL_HDR;
    }
    if (o2 < BPR_VOL_HDR) return -1;
    *out = sum + (o2 - BPR_VOL_HDR);
    return 0;
}

/* Where the trusted table says the entry's blocks end (the first byte of its
 * trailer): 16 bytes before the next record, or before the file table. */
static void init_geometry(pctx *p)
{
    const bpr_ctx *c = p->c;
    const bpr_entry *e = p->e;
    int nv;
    uint64_t no;

    p->dv = e->vol;
    p->doff = e->off + e->header_size;
    bpr_norm_pos(c, &p->dv, &p->doff);
    p->have_tend = 0;
    if (c->table.status != BPR_TBL_TRUSTED || !c->have_end ||
        e->table_index < 0 || (uint32_t)e->table_index >= c->table.nrec)
        return;
    {
        const bpr_entry *rec = &c->table.rec[e->table_index];
        if (rec->vol != e->vol || rec->off != e->off) return;
        if ((uint32_t)e->table_index + 1 < c->table.nrec) {
            nv = c->table.rec[e->table_index + 1].vol;
            no = c->table.rec[e->table_index + 1].off;
        } else {
            nv = c->end.table_volume;
            no = c->end.table_offset;
        }
    }
    if (no == BPR_VOL_HDR) {
        const bpr_vol *x;
        if (nv <= 0) return;
        x = bpr_vol_at(c, nv - 1);
        if (x == NULL || bpr_read_limit(x) < BPR_VOL_HDR + BPR_ENTRY_TRL) return;
        nv--;
        no = bpr_read_limit(x);
    }
    if (no < BPR_VOL_HDR + BPR_ENTRY_TRL) return;
    p->tv = nv;
    p->toff = no - BPR_ENTRY_TRL;
    bpr_norm_pos(c, &p->tv, &p->toff);
    p->have_tend = 1;
}

/* ---- block loading --------------------------------------------------- */

/* Read the block header at (v, off) and its data into dec->in.  Returns 0 if
 * the structure is readable and plausible (crc_ok says whether the CRC matched),
 * -1 otherwise with b->reason; b->hdr says whether the header bytes were read. */
static int load_block(pctx *p, bpr_decoder *dec, int v, uint64_t off,
                      uint64_t max_orig, pblk *b)
{
    uint8_t h[BPR_BLOCK_HDR];
    bpr_fail f = { NULL, -1 };
    const bpr_vol *x;

    memset(b, 0, sizeof *b);
    bpr_norm_pos(p->c, &v, &off);
    b->v = v;
    b->off = off;
    if (bpr_read_struct(p->c, v, off, h, sizeof h, &f, "block-truncated") != 0) {
        b->reason = f.reason;
        return -1;
    }
    b->hdr = 1;
    b->comp = bpr_le32(h);
    b->orig = bpr_le32(h + 4);
    b->crc = bpr_le32(h + 8);
    if (b->comp == 0 || b->orig == 0 || b->comp > b->orig ||
        b->orig > BPR_MAX_BLOCK || b->orig > max_orig ||
        (p->bs != 0 && b->orig > p->bs)) {
        b->reason = "block-header";
        return -1;
    }
    x = bpr_vol_at(p->c, v);
    if (off + BPR_BLOCK_HDR + b->comp > bpr_read_limit(x)) {
        b->reason = "block-truncated";
        return -1;
    }
    b->plausible = 1;
    if (bpr_grow(&dec->in, &dec->in_cap, b->comp) != 0 ||
        bpr_file_read(x->f, off + BPR_BLOCK_HDR, dec->in, b->comp) != 0) {
        b->reason = "read-error";
        return -1;
    }
    b->crc_ok = bpr_crc32(0, dec->in, b->comp) == b->crc;
    return 0;
}

static void next_pos(const pctx *p, const pblk *b, int *v, uint64_t *off)
{
    *v = b->v;
    *off = b->off + BPR_BLOCK_HDR + b->comp;
    bpr_norm_pos(p->c, v, off);
}

/* ---- failed-walk memory ------------------------------------------------
 * When the scan tries many candidate starts inside one long run of genuine
 * blocks, every candidate walks the same blocks to the same failure: quadratic.
 * A walk that fails for a reason that does not depend on where it started
 * (an unreadable or implausible block, or an end that does not anchor) makes
 * every block start it visited a dead end; later candidates stop there. */

static uint64_t mkey(int v, uint64_t off)
{
    return ((uint64_t)(v + 1) << 48) | off;
}

static size_t mslot(uint64_t k, size_t cap)
{
    return (size_t)((k * 0x9E3779B97F4A7C15ull) >> 24) & (cap - 1);
}

static int memo_has(const pctx *p, uint64_t k)
{
    size_t i;
    if (p->memo_cap == 0) return 0;
    for (i = mslot(k, p->memo_cap); p->memo[i] != 0; i = (i + 1) & (p->memo_cap - 1))
        if (p->memo[i] == k) return 1;
    return 0;
}

static void memo_add(pctx *p, uint64_t k)
{
    size_t i;
    if (memo_has(p, k)) return;
    if ((p->memo_n + 1) * 2 > p->memo_cap) {
        size_t ncap = p->memo_cap ? p->memo_cap * 2 : 4096, j;
        uint64_t *n;
        if (ncap > ((size_t)1 << 23)) return;       /* stop remembering */
        n = (uint64_t *)calloc(ncap, sizeof *n);
        if (n == NULL) return;
        for (j = 0; j < p->memo_cap; j++)
            if (p->memo[j] != 0) {
                size_t q = mslot(p->memo[j], ncap);
                while (n[q] != 0) q = (q + 1) & (ncap - 1);
                n[q] = p->memo[j];
            }
        free(p->memo);
        p->memo = n;
        p->memo_cap = ncap;
    }
    for (i = mslot(k, p->memo_cap); p->memo[i] != 0; i = (i + 1) & (p->memo_cap - 1)) {}
    p->memo[i] = k;
    p->memo_n++;
}

static void memo_commit(pctx *p)
{
    size_t i;
    for (i = 0; i < p->tmp_n; i++) memo_add(p, p->tmp[i]);
    p->tmp_n = 0;
}

static void memo_visit(pctx *p, int v, uint64_t off)
{
    if (p->tmp_n == p->tmp_cap) {
        size_t cap = p->tmp_cap ? p->tmp_cap * 2 : 256;
        uint64_t *n;
        if (cap > ((size_t)1 << 22)) return;
        n = (uint64_t *)realloc(p->tmp, cap * sizeof *n);
        if (n == NULL) return;
        p->tmp = n;
        p->tmp_cap = cap;
    }
    p->tmp[p->tmp_n++] = mkey(v, off);
}

/* ---- survey ---------------------------------------------------------- */

/* Walk blocks from (v, off) by their compressed sizes until a valid entry
 * trailer (or the table's end of blocks), without writing anything, and say
 * how well the walk is anchored.  max_sum bounds the uncompressed bytes the
 * walk may describe.  quick: stop as soon as one block matches its CRC.
 * first_ok: the first block must match its CRC. */
static void survey(pctx *p, int v, uint64_t off, uint64_t max_sum, int first_ok,
                   int quick, pchain *ch)
{
    uint64_t csize = 0;
    int by_trailer = 0, first = 1;

    memset(ch, 0, sizeof *ch);
    p->tmp_n = 0;
    if (p->e->method == BP_METHOD_FLUFF) {
        /* A full window of anything: only whether a block decodes matters, and
         * that does not depend on the history's content. */
        static const uint8_t zeros[FLUFF_WINDOW];
        fluff_ctx_reset_decode((fluff_ctx_t *)p->sd.fctx);
        fluff_ctx_feed_raw((fluff_ctx_t *)p->sd.fctx, zeros, FLUFF_WINDOW);
    }
    bpr_norm_pos(p->c, &v, &off);
    ch->sv = v;
    ch->soff = off;
    for (;;) {
        uint8_t t[BPR_ENTRY_TRL];
        bpr_fail f = { NULL, -1 };
        pblk b;
        int cls, nv;
        uint64_t noff;

        bpr_norm_pos(p->c, &v, &off);
        if (bpr_read_struct(p->c, v, off, t, sizeof t, &f, "x") == 0 &&
            bpr_crc32(0, t, 12) == bpr_le32(t + 12)) {
            by_trailer = 1;
            csize = bpr_le64(t);
            break;
        }
        if (p->have_tend && v == p->tv && off == p->toff) break;
        if (ch->sum_all >= max_sum) return;
        if (p->memo_on) {
            if (memo_has(p, mkey(v, off))) { memo_commit(p); return; }
            memo_visit(p, v, off);
        }
        if (load_block(p, &p->sd, v, off, BPR_MAX_BLOCK, &b) != 0) {
            if (p->memo_on && !first) memo_commit(p);
            return;
        }
        if (ch->sum_all + b.orig > max_sum) return;
        if (first && first_ok && !b.crc_ok) return;
        first = 0;
        if (b.crc_ok) {
            ch->ok_seen = 1;
            if (quick) return;
        }
        if (!b.crc_ok) {
            cls = b.orig == p->bs ? SZ_F : SZ_U;
        } else if (b.comp == b.orig) {
            cls = SZ_T;
        } else {
            cls = bpr_dec_block(&p->sd, b.comp, b.orig) == NULL ? SZ_T : SZ_U;
        }
        ch->sum_all += b.orig;
        ch->nblocks++;
        nv = b.v;
        noff = b.off + BPR_BLOCK_HDR + b.comp;
        if (cls == SZ_U) {
            ch->nU++;
            ch->suffix_orig = 0;
            bpr_norm_pos(p->c, &nv, &noff);
            ch->sv = nv;
            ch->soff = noff;
        } else {
            ch->suffix_orig += b.orig;
        }
        v = nv;
        off = noff;
    }
    ch->trailer = by_trailer;
    if (by_trailer) {
        ch->csize = csize;
        ch->dist_ok = stream_dist(p->c, p->dv, p->doff, v, off, &ch->dist) == 0;
    }
    /* The walk stopped at a trailer (or the table's end of blocks) at (v, off). */
    if (p->have_tend) {
        ch->anchored = v == p->tv && off == p->toff;
    } else if (by_trailer) {
        uint64_t dist;
        ch->anchored = stream_dist(p->c, p->dv, p->doff, v, off, &dist) == 0 &&
                       dist == csize;
    }
    if (p->memo_on && !ch->anchored) memo_commit(p);
}

/* ---- resynchronising ------------------------------------------------- */

/* Scan forward from (v, off) for a place the walk can resume: the start of a
 * chain of blocks (a plausible header whose data matches its CRC) or the
 * entry's trailer, such that the walk ends anchored.  Returns 1 and sets
 * (*rv, *roff, ch). */
static int scan_chain(pctx *p, int v, uint64_t off, uint64_t rem, int *rv,
                      uint64_t *roff, pchain *ch)
{
    uint8_t *buf = NULL;
    uint64_t limit_bytes, scanned = 0, crc_budget;
    int found = 0, w = v;
    uint64_t o = off;

    limit_bytes = rem + (uint64_t)BPR_BLOCK_HDR * (rem / (p->bs ? p->bs : 1) + 8) +
                  BPR_ENTRY_TRL + 4096;
    crc_budget = limit_bytes * 4 + (64u << 20);
    buf = (uint8_t *)malloc(SCAN_CHUNK + BPR_ENTRY_TRL);
    if (buf == NULL) return 0;
    p->memo_on = 1;

    while (!found && scanned < limit_bytes) {
        const bpr_vol *x = bpr_vol_at(p->c, w);
        uint64_t lim, pos;
        if (x == NULL) break;
        lim = bpr_read_limit(x);
        pos = o;
        while (!found && pos + BPR_ENTRY_TRL <= lim && scanned < limit_bytes) {
            uint64_t n = lim - pos, i;
            if (n > SCAN_CHUNK) n = SCAN_CHUNK;
            if (bpr_file_read(x->f, pos, buf, (size_t)n) != 0) goto done;
            for (i = 0; i + BPR_ENTRY_TRL <= n && !found; i++) {
                uint32_t comp = bpr_le32(buf + i), orig = bpr_le32(buf + i + 4);
                int cand = 0;
                if (buf[i + 6] == 0 && buf[i + 7] == 0 &&   /* csize < 2^48 */
                    bpr_le32(buf + i + 12) == bpr_crc32(0, buf + i, 12)) {
                    cand = 1;               /* trailer-shaped */
                } else if (p->have_tend && w == p->tv && pos + i == p->toff) {
                    cand = 1;               /* where the trusted table ends the blocks */
                } else if (comp != 0 && orig != 0 && comp <= orig && orig <= rem &&
                           orig <= BPR_MAX_BLOCK && (p->bs == 0 || orig <= p->bs) &&
                           pos + i + BPR_BLOCK_HDR + comp <= lim) {
                    if (crc_budget < comp) goto done;
                    crc_budget -= comp;
                    cand = 1;
                }
                if (!cand || memo_has(p, mkey(w, pos + i))) continue;
                survey(p, w, pos + i, rem, 1, 0, ch);
                if (ch->anchored) {
                    *rv = w;
                    *roff = pos + i;
                    found = 1;
                }
            }
            if (found) break;
            scanned += i;
            pos += i;
            if (n < SCAN_CHUNK) break;
        }
        w++;
        o = BPR_VOL_HDR;
    }
done:
    p->memo_on = 0;
    free(p->memo);
    p->memo = NULL;
    p->memo_cap = p->memo_n = 0;
    free(buf);
    return found;
}

/* Place block a (comp_use bytes of data) as exactly d output bytes at out_pos.
 * require_ok: place it only if it verifies and decodes (the size is then
 * confirmed by the exact-length decode), otherwise leave everything alone.
 * Returns 1 if placed, 0 if not placed, -1 on a write error. */
static int place_single(pctx *p, const pblk *a, uint32_t comp_use, uint64_t d,
                        int require_ok)
{
    const bpr_vol *x = bpr_vol_at(p->c, a->v);
    const char *why = NULL;
    int ok = 0;
    if (bpr_grow(&p->d.in, &p->d.in_cap, comp_use) != 0 ||
        bpr_file_read(x->f, a->off + BPR_BLOCK_HDR, p->d.in, comp_use) != 0) {
        why = "read-error";
    } else if (bpr_crc32(0, p->d.in, comp_use) != a->crc) {
        why = "block-crc";
    } else if ((why = dec_pair(p, comp_use, (uint32_t)d)) == NULL) {
        ok = 1;
    } else {
        fluff_refresh(p);
    }
    if (!ok && require_ok) return 0;
    if (ok) {
        if (emit_decoded(p, (uint32_t)d) != 0) return -1;
    } else {
        if (write_zeros(p->out, d) != 0) return -1;
        if (add_range(p->r, p->out_pos, p->out_pos + d, 0, why) != 0) return -1;
        if (hole_fill(p, d) != 0) return -1;
    }
    p->out_pos += d;
    p->remaining -= d;
    return 1;
}

/* The block a (if its header was readable) has an unknown size, or its header
 * is unusable.  The walk after it ends anchored (ch).  If a's data verifies and
 * decodes at the size left over, that exact-length decode confirms the size,
 * whatever else is unknown.  If every later size is trusted, the size is
 * deduced and a is placed (lost if its data is bad).  Otherwise everything up
 * to the last unknown block is lost and only the blocks after it, placed
 * backwards from the end, are recovered.  Returns 1 if the walk resumes. */
static int finish_unknown(pctx *p, const pblk *a, uint32_t comp_use, int rv,
                          uint64_t roff, const pchain *ch, const char *why)
{
    uint64_t rem = p->remaining;
    if (!ch->anchored || ch->sum_all >= rem) return 0;
    if (a != NULL && a->hdr && comp_use > 0) {
        uint64_t d = rem - ch->sum_all;
        if (d >= comp_use && d >= 1 && p->bs != 0 && d <= p->bs) {
            int rc = place_single(p, a, comp_use, d, ch->nU != 0);
            if (rc < 0) return rc;
            if (rc == 1) {
                p->v = rv;
                p->off = roff;
                p->verified = 1;
                return 1;
            }
        }
    }
    {
        uint64_t keep = ch->suffix_orig, hole = rem - keep;
        if (hole == 0) return 0;
        if (write_zeros(p->out, hole) != 0) return -1;
        if (add_range(p->r, p->out_pos, p->out_pos + hole, 0, why) != 0) return -1;
        if (hole_fill(p, hole) != 0) return -1;
        p->out_pos += hole;
        p->remaining = keep;
        p->v = ch->sv;
        p->off = ch->soff;
        p->resumed = 1;
        p->verified = 1;
        return 1;
    }
}

/* Where the data of block a ends if the walk resumes at (rv, roff) and a lies
 * in the same stream: used when a's own compressed-size field is damaged. */
static uint32_t geom_comp(const pctx *p, const pblk *a, int rv, uint64_t roff)
{
    const bpr_vol *x = bpr_vol_at(p->c, a->v);
    uint64_t data = a->off + BPR_BLOCK_HDR, end;
    if (x == NULL) return 0;
    if (rv == a->v) end = roff;
    else if (rv == a->v + 1 && roff == BPR_VOL_HDR) end = bpr_read_limit(x);
    else return 0;
    if (end <= data || end - data > BPR_MAX_BLOCK) return 0;
    return (uint32_t)(end - data);
}

/* The walk cannot continue at the current block.  a describes it when its
 * header was readable.  Returns 1 if it resumed, 0 if not, -1 on a write
 * error. */
static int resolve_unknown(pctx *p, const pblk *a, const char *why)
{
    pchain ch;
    int rv;
    uint64_t roff;
    if (p->resumed || p->bs == 0) return 0;
    if (a != NULL && a->hdr && a->comp > 0) {
        /* The compressed size may still be right even if the block's other
         * fields are not: try the block that would follow it. */
        const bpr_vol *xa = bpr_vol_at(p->c, a->v);
        if (xa != NULL && a->off + BPR_BLOCK_HDR + a->comp <= bpr_read_limit(xa)) {
            int nv;
            uint64_t noff;
            next_pos(p, a, &nv, &noff);
            survey(p, nv, noff, p->remaining, 0, 0, &ch);
            if (ch.anchored)
                return finish_unknown(p, a, a->comp, nv, noff, &ch, why);
        }
    }
    if (a != NULL && a->hdr) {
        rv = a->v;
        roff = a->off + 1;
    } else {
        /* The volume is missing or the position unreadable: the stream can
         * only resume at the start of the next volume present. */
        int w = p->v + 1;
        while (w < BPR_MAX_VOLUMES && w <= p->c->max_number + 1 &&
               bpr_vol_at(p->c, w) == NULL) w++;
        if (bpr_vol_at(p->c, p->v) != NULL || bpr_vol_at(p->c, w) == NULL)
            return 0;
        survey(p, w, BPR_VOL_HDR, p->remaining, 1, 0, &ch);
        if (!ch.anchored || ch.nblocks == 0) return 0;
        return finish_unknown(p, NULL, 0, w, BPR_VOL_HDR, &ch, why);
    }
    if (!scan_chain(p, rv, roff, p->remaining, &rv, &roff, &ch)) return 0;
    {
        uint32_t cu = geom_comp(p, a, rv, roff);
        return finish_unknown(p, a, cu, rv, roff, &ch, why);
    }
}

/* ---- the entry's blocks ---------------------------------------------- */

/* Is the entry's end confirmed (valid trailer, or the trusted table's record)
 * after blocks totalling comp_sum bytes, ending at (v, off)? */
static int end_anchored(const bpr_ctx *c, const bpr_entry *e, int v,
                        uint64_t off, uint64_t comp_sum)
{
    uint8_t t[BPR_ENTRY_TRL];
    bpr_fail f = { NULL, -1 };
    bpr_norm_pos(c, &v, &off);
    if (bpr_read_struct(c, v, off, t, sizeof t, &f, "x") == 0 &&
        bpr_crc32(0, t, 12) == bpr_le32(t + 12) && bpr_le64(t) == comp_sum)
        return 1;
    if (c->table.status == BPR_TBL_TRUSTED && e->table_index >= 0 &&
        (uint32_t)e->table_index < c->table.nrec) {
        const bpr_entry *rec = &c->table.rec[e->table_index];
        return rec->vol == e->vol && rec->off == e->off &&
               rec->comp_size == comp_sum;
    }
    return 0;
}

/* An entry whose header was rebuilt from the table has no block size.  Every
 * block but the last (and the odd end-of-volume piece) is full, so the largest
 * plausible block size seen is the block size.  A damaged size field can only
 * make the estimate too large, which makes the later "is this block full?"
 * test fail and the walk stop: the safe direction. */
uint32_t bpr_infer_block_size(const bpr_ctx *c, const bpr_entry *e)
{
    int v = e->vol;
    uint64_t off = e->off + e->header_size, remaining = e->original_size;
    uint32_t best = 0;
    bpr_norm_pos(c, &v, &off);
    while (remaining > 0) {
        uint8_t h[BPR_BLOCK_HDR];
        bpr_fail f = { NULL, -1 };
        uint32_t comp, orig;
        const bpr_vol *xv;
        if (bpr_read_struct(c, v, off, h, sizeof h, &f, "x") != 0) break;
        comp = bpr_le32(h);
        orig = bpr_le32(h + 4);
        xv = bpr_vol_at(c, v);
        if (comp == 0 || orig == 0 || comp > orig || orig > remaining ||
            orig > BPR_MAX_BLOCK ||
            off + BPR_BLOCK_HDR + comp > bpr_read_limit(xv)) break;
        if (orig > best) best = orig;
        remaining -= orig;
        off += BPR_BLOCK_HDR + comp;
        bpr_norm_pos(c, &v, &off);
    }
    return best;
}

static char *damaged_name(const bpr_entry *e)
{
    size_t n = e->name_len;
    char *p;
    while (n > 0 && e->name[n - 1] == '/') n--;
    p = (char *)malloc(n + 9);
    if (p == NULL) return NULL;
    memcpy(p, e->name, n);
    memcpy(p + n, ".damaged", 9);
    return p;
}

/* Walk the blocks, writing the output.  Returns 0 normally, -1 on a write
 * error. */
static int walk_blocks(pctx *p)
{
    const bpr_entry *e = p->e;
    bpr_ctx *c = p->c;

    while (p->remaining > 0) {
        pblk b;
        int nv, res;
        uint64_t noff;
        int full;

        if (load_block(p, &p->d, p->v, p->off, p->remaining, &b) != 0) {
            p->stop = b.reason;
            res = resolve_unknown(p, &b, b.reason);
            if (res < 0) return -1;
            if (res == 1) { p->stop = NULL; continue; }
            break;
        }
        p->v = b.v;
        p->off = b.off;
        if (p->bs == 0) { p->stop = "block-header"; break; }
        next_pos(p, &b, &nv, &noff);

        if (b.crc_ok) {
            if (b.orig == p->remaining && p->unsure && !p->verified &&
                !end_anchored(c, e, nv, noff, p->comp_sum + BPR_BLOCK_HDR + b.comp)) {
                /* Nothing confirms where the entry ends, so this block's size
                 * cannot be trusted either. */
                p->stop = "size-uncertain";
                break;
            }
            {
                const char *why = dec_pair(p, b.comp, b.orig);
                if (why != NULL) {
                    fluff_refresh(p);
                    /* The data is intact but does not decode to this size, so
                     * the size field is what is wrong. */
                    p->stop = "decode-error";
                    res = resolve_unknown(p, &b, p->stop);
                    if (res < 0) return -1;
                    if (res == 1) { p->stop = NULL; continue; }
                    break;
                }
                if (emit_decoded(p, b.orig) != 0) return -1;
            }
        } else {
            /* The data failed its CRC.  Where the next block starts depends on
             * this block's compressed size, which only the blocks after it can
             * confirm. */
            pchain ch;
            int surveyed = 0;
            full = b.orig == p->bs ||
                   (b.orig == p->remaining &&
                    end_anchored(c, e, nv, noff, p->comp_sum + BPR_BLOCK_HDR + b.comp));
            if (!p->verified || !full) {
                /* Only an exact walk to the entry's end settles a size that
                 * is not a full block's; a quick look for one good block is
                 * enough to believe the position of the next block. */
                survey(p, nv, noff, p->remaining, 0, p->no_anchor && full, &ch);
                surveyed = 1;
                if (!ch.anchored) p->no_anchor = 1;
            }
            if (surveyed && !ch.anchored && !(full && ch.ok_seen)) {
                p->stop = full ? "block-header" : "size-uncertain";
                res = resolve_unknown(p, &b, p->stop);
                if (res < 0) return -1;
                if (res == 1) { p->stop = NULL; continue; }
                break;
            }
            if (surveyed && ch.anchored) {
                if (!full || (ch.nU == 0 && b.orig + ch.sum_all != p->remaining)) {
                    p->stop = "size-uncertain";
                    res = finish_unknown(p, &b, b.comp, nv, noff, &ch, "block-crc");
                    if (res < 0) return -1;
                    if (res == 1) { p->stop = NULL; continue; }
                    break;
                }
                p->verified = 1;
            }
            if (write_zeros(p->out, b.orig) != 0) return -1;
            if (add_range(p->r, p->out_pos, p->out_pos + b.orig, 0, "block-crc") != 0)
                return -1;
            if (hole_fill(p, b.orig) != 0) return -1;
            p->unsure = 1;
        }
        p->out_pos += b.orig;
        p->remaining -= b.orig;
        p->comp_sum += BPR_BLOCK_HDR + b.comp;
        p->v = nv;
        p->off = noff;
    }
    return 0;
}

/* Every byte of the entry was recovered and verified block by block (this
 * happens when a damaged size field was the only damage).  Confirm the payload
 * with the trailer's or the trusted table's checksum and, if it matches, keep
 * the file under its real name instead of NAME.damaged.  Sets r->outcome to
 * BPR_X_PARTIAL if it stays as it is. */
static void promote_whole(pctx *p, bpr_xlist *x, bpr_xres *r, char *name)
{
    const bpr_entry *e = p->e;
    const bpr_ctx *c = p->c;
    uint32_t want = 0;
    int have_want = 0, by_table = 0;
    size_t n = strlen(name) - 8;
    char *real;
    {
        uint8_t t[BPR_ENTRY_TRL];
        bpr_fail f = { NULL, -1 };
        int v = p->v;
        uint64_t off = p->off;
        bpr_norm_pos(c, &v, &off);
        if (bpr_read_struct(c, v, off, t, sizeof t, &f, "x") == 0 &&
            bpr_crc32(0, t, 12) == bpr_le32(t + 12)) {
            want = bpr_le32(t + 8);
            have_want = 1;
        } else if (e->from_table) {
            want = e->checksum;
            have_want = by_table = 1;
        } else if (c->table.status == BPR_TBL_TRUSTED && e->table_index >= 0 &&
                   (uint32_t)e->table_index < c->table.nrec) {
            const bpr_entry *rec = &c->table.rec[e->table_index];
            if (rec->vol == e->vol && rec->off == e->off &&
                rec->original_size == e->original_size) {
                want = rec->checksum;
                have_want = by_table = 1;
            }
        }
    }
    r->outcome = BPR_X_PARTIAL;
    if (have_want && p->crc != want) {
        r->outcome = BPR_X_FAILED;
        r->detail = "payload-checksum-mismatch";
        free(r->ranges);
        r->ranges = NULL;
        r->nranges = r->capranges = 0;
        r->bytes_ok = r->bytes_lost = 0;
        return;
    }
    real = (char *)malloc(n + 1);
    if (real == NULL) return;
    memcpy(real, name, n);
    real[n] = '\0';
    if (bpr_fs_exists(x->root, real) || bpr_fs_rename(x->root, name, real) != 0) {
        free(real);
        return;
    }
    bpr_fs_restore_meta(x->root, real, e->flags, e->mtime, 0);
    r->written = real;
    r->detail = NULL;
    r->outcome = !have_want ? BPR_X_UNVERIFIED
               : by_table ? BPR_X_VERIFIED_TABLE : BPR_X_VERIFIED;
    free(r->ranges);
    r->ranges = NULL;
    r->nranges = r->capranges = 0;
    r->bytes_ok = r->bytes_lost = 0;
}

static void pfree(pctx *p)
{
    if (p->have_d2) bpr_dec_end(&p->d2);
    free(p->ring[0]);
    free(p->ring[1]);
    free(p->fill);
    free(p->tmp);
}

void bpr_extract_partial(bpr_ctx *c, bpr_xlist *x, bpr_xres *r,
                         const bpr_entry *e)
{
    char *name = damaged_name(e);
    pctx p;
    int blocks_missing = 0;

    memset(&p, 0, sizeof p);
    p.c = c;
    p.e = e;
    p.r = r;
    if (name == NULL) { r->outcome = BPR_X_FAILED; r->detail = "no-memory"; return; }
    if (bpr_fs_exists(x->root, name)) {
        r->outcome = BPR_X_SKIP_DUPLICATE;
        r->detail = "a file with this name was already written";
        goto out;
    }
    if (bpr_fs_make_parents(x->root, name) != 0 ||
        (p.out = bpr_out_create(x->root, name)) == NULL) {
        r->outcome = BPR_X_FAILED;
        r->detail = "cannot create the output file";
        goto out;
    }
    if (bpr_dec_begin(&p.d, e->method) != 0) {
        bpr_dec_end(&p.d);
        bpr_out_abort(p.out);
        bpr_fs_unlink(x->root, name);
        r->outcome = BPR_X_SKIP_PARTIAL;
        r->detail = "unsupported-method";
        goto out;
    }
    p.have_d = 1;
    /* The survey never needs Fluff history: it decodes only independent
     * blocks, so a Fluff entry gets a plain buffer-only decoder. */
    if (bpr_dec_begin(&p.sd, e->method) != 0) {
        bpr_dec_end(&p.sd);
        r->outcome = BPR_X_FAILED;
        r->detail = "no-memory";
        bpr_dec_end(&p.d);
        bpr_out_abort(p.out);
        bpr_fs_unlink(x->root, name);
        goto out;
    }
    p.have_sd = 1;
    if (e->method == BP_METHOD_FLUFF) {
        p.ring[0] = (uint8_t *)calloc(1, FLUFF_WINDOW);
        if (p.ring[0] == NULL) goto write_error;
    }

    p.bs = e->block_size == BPR_MAX_BLOCK ? 0 : e->block_size;
    if (p.bs == 0) p.bs = bpr_infer_block_size(c, e);
    init_geometry(&p);
    p.v = e->vol;
    p.off = e->off + e->header_size;
    p.remaining = e->original_size;
    bpr_norm_pos(c, &p.v, &p.off);

    /* Before decoding anything: parse the whole chain of blocks.  If it runs
     * cleanly to the entry's trailer, the blocks must fill exactly the space the
     * trailer says they do (its compressed_size).  Bytes that were damaged in
     * place never change that; whole blocks removed from the stream make it too
     * small.  Blocks that now sit
     * where an earlier block should be pass their CRC and decode (a Fluff block
     * even decodes against the wrong history without error), so everything
     * placed from here would be placed one block early.  Nothing says where the
     * missing blocks were: report nothing rather than shifted bytes. */
    {
        pchain ch0;
        survey(&p, p.v, p.off, e->original_size, 0, 0, &ch0);
        blocks_missing = ch0.trailer && ch0.dist_ok && ch0.dist < ch0.csize;
    }
    if (!blocks_missing && walk_blocks(&p) != 0) goto write_error;
    if (!blocks_missing && p.out_pos < e->original_size) {
        uint64_t rest = e->original_size - p.out_pos;
        if (write_zeros(p.out, rest) != 0) goto write_error;
        add_range(r, p.out_pos, e->original_size, 0,
                  p.stop ? p.stop : "block-header");
    }
    bpr_dec_end(&p.sd);
    bpr_dec_end(&p.d);
    p.have_sd = p.have_d = 0;
    if (blocks_missing || r->bytes_ok == 0) {
        bpr_out_abort(p.out);
        bpr_fs_unlink(x->root, name);
        r->outcome = BPR_X_SKIP_PARTIAL;
        r->detail = blocks_missing ? "blocks-missing"
                  : p.stop ? p.stop : "no block could be recovered";
        free(r->ranges);
        r->ranges = NULL;
        r->nranges = r->capranges = 0;
        r->bytes_lost = 0;
        goto out;
    }
    if (bpr_out_close(p.out) != 0) {
        bpr_fs_unlink(x->root, name);
        r->outcome = BPR_X_FAILED;
        r->detail = "write-error";
        goto out;
    }
    if (r->bytes_lost == 0 && r->bytes_ok == e->original_size) {
        promote_whole(&p, x, r, name);
        if (r->outcome != BPR_X_PARTIAL) goto out;
    }
    r->outcome = BPR_X_PARTIAL;
    r->detail = NULL;
    r->written = name;
    pfree(&p);
    return;

write_error:
    if (p.have_sd) bpr_dec_end(&p.sd);
    if (p.have_d) bpr_dec_end(&p.d);
    bpr_out_abort(p.out);
    bpr_fs_unlink(x->root, name);
    r->outcome = BPR_X_FAILED;
    r->detail = "write-error";
out:
    pfree(&p);
    free(name);
}
