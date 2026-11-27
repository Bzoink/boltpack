/*
 * bpr_analyze.c -- decide what the scan found.
 *
 * Every CRC-valid entry-header hit is checked by walking its structures
 * (header, blocks, trailer) with the sizes it declares.  Hits are taken in
 * archive order.  A hit inside a block that already verified is not an entry:
 * a .bolt stored in a .bolt contains real headers, and the verified outer
 * block says those bytes are data.  A hit in a span the walk could not
 * account for is kept but labelled by how it was reached:
 *
 *   chained   starts where a verified entry ended (the section 5.4 walk)
 *   isolated  found by scanning only
 *   suspect   found where an earlier damaged entry's data could contain it
 *
 * Phase 1 decodes nothing.  "ok" means every structure of the entry parses
 * and every CRC-32 matches; the payload checksum is not checked until blocks
 * are decoded in a later phase.
 */
#include "bpr.h"

#include "archive_name.h"

#include <stdlib.h>
#include <string.h>

static uint8_t *g_buf;
static size_t g_cap;

static int need_buf(size_t n)
{
    if (n <= g_cap) return 0;
    {
        uint8_t *b = (uint8_t *)realloc(g_buf, n);
        if (b == NULL) return -1;
        g_buf = b;
        g_cap = n;
    }
    return 0;
}

const bpr_vol *bpr_vol_at(const bpr_ctx *c, int v)
{
    if (v < 0 || v >= BPR_MAX_VOLUMES) return NULL;
    return c->slot[v];
}

uint64_t bpr_read_limit(const bpr_vol *x)
{
    return x->trl_ok ? x->size - BPR_VOL_TRL : x->size;
}

/* After a structure ends exactly at the end of a body, the next one starts at
 * offset 32 of the next volume (spec 2.1). */
void bpr_norm_pos(const bpr_ctx *c, int *v, uint64_t *off)
{
    const bpr_vol *x = bpr_vol_at(c, *v);
    if (x == NULL) return;
    if (*off == x->size - BPR_VOL_TRL || (!x->trl_ok && *off == x->size)) {
        (*v)++;
        *off = BPR_VOL_HDR;
    }
}

/* Read n bytes of one structure at (v, off); the structure must lie inside
 * the volume body. */
int bpr_read_struct(const bpr_ctx *c, int v, uint64_t off, void *buf,
                       size_t n, bpr_fail *f, const char *short_reason)
{
    const bpr_vol *x = bpr_vol_at(c, v);
    if (x == NULL) {
        f->reason = "missing-volume";
        f->need_volume = v;
        return -1;
    }
    if (off > bpr_read_limit(x) || n > bpr_read_limit(x) - off) {
        f->reason = short_reason;
        return -1;
    }
    if (bpr_file_read(x->f, off, buf, n) != 0) {
        f->reason = "read-error";
        return -1;
    }
    return 0;
}

/* True if a valid entry trailer for the damaged entry owner ends exactly at
 * cand: its compressed_size equals the bytes between owner's header and the
 * trailer, so the owner's data cannot contain cand. */
static int trailer_closes(const bpr_ctx *c, const bpr_entry *owner,
                          const bpr_cand *cand)
{
    uint8_t t[BPR_ENTRY_TRL];
    bpr_fail f = { NULL, -1 };
    uint64_t data_start = owner->off + owner->header_size;
    if (cand->vol != owner->vol || cand->off < data_start + BPR_ENTRY_TRL)
        return 0;
    if (bpr_read_struct(c, cand->vol, cand->off - BPR_ENTRY_TRL, t, sizeof t,
                        &f, "x") != 0) return 0;
    if (bpr_crc32(0, t, 12) != bpr_le32(t + 12)) return 0;
    return bpr_le64(t) == cand->off - BPR_ENTRY_TRL - data_start;
}

static int trailer_at(const bpr_ctx *c, int v, uint64_t off)
{
    uint8_t t[BPR_ENTRY_TRL];
    bpr_fail f = { NULL, -1 };
    return bpr_read_struct(c, v, off, t, sizeof t, &f, "x") == 0 &&
           bpr_crc32(0, t, 12) == bpr_le32(t + 12);
}

static uint64_t ext_bound(const bpr_ctx *c, const bpr_cand *k)
{
    uint64_t blocks = 0, slack = c->nvolumes_present > 1 ? 1 : 0;
    if (k->original_size > 0)
        blocks = (k->original_size + k->block_size - 1) / k->block_size;
    return k->off + k->header_size + k->original_size +
           (uint64_t)BPR_BLOCK_HDR * (blocks + slack) + BPR_ENTRY_TRL;
}

int bpr_verify_entry(bpr_ctx *c, const bpr_cand *k, int idx, bpr_entry *e,
                     int account)
{
    bpr_fail f = { NULL, -1 };
    int v = k->vol;
    uint64_t off = k->off + k->header_size;
    uint64_t remaining = k->original_size, comp_sum = 0;
    uint8_t h[BPR_BLOCK_HDR], t[BPR_ENTRY_TRL];

    memset(e, 0, sizeof(*e));
    e->vol = k->vol;
    e->off = k->off;
    e->header_size = k->header_size;
    e->name_len = k->name_len;
    e->name = k->name;                  /* ownership moves to the entry */
    e->original_size = k->original_size;
    e->method = k->method;
    e->entry_type = k->entry_type;
    e->platform = k->platform;
    e->flags = k->flags;
    e->mtime = k->mtime;
    e->block_size = k->block_size;
    e->inside = -1;
    e->in_table = -1;
    e->table_index = -1;
    e->need_volume = -1;
    e->status = BPR_ST_DAMAGED;

    if (account) bpr_ivs_add(&c->iv[k->vol], k->off, off, BPR_K_ENTHDR, idx);
    bpr_norm_pos(c, &v, &off);
    e->end_vol = v;
    e->end_off = off;

    while (remaining > 0) {
        uint32_t comp, orig, bcrc;
        const bpr_vol *x;
        if (bpr_read_struct(c, v, off, h, sizeof h, &f, "block-truncated") != 0)
            goto fail;
        comp = bpr_le32(h);
        orig = bpr_le32(h + 4);
        bcrc = bpr_le32(h + 8);
        if (comp == 0 || orig == 0 || comp > orig || orig > k->block_size ||
            orig > remaining) {
            f.reason = "block-header";
            goto fail;
        }
        x = bpr_vol_at(c, v);
        if (off + BPR_BLOCK_HDR + comp > bpr_read_limit(x)) {
            f.reason = "block-truncated";
            goto fail;
        }
        if (need_buf(comp) != 0) { f.reason = "no-memory"; goto fail; }
        if (bpr_file_read(x->f, off + BPR_BLOCK_HDR, g_buf, comp) != 0) {
            f.reason = "read-error";
            goto fail;
        }
        if (bpr_crc32(0, g_buf, comp) != bcrc) {
            f.reason = "block-crc";
            goto fail;
        }
        if (account) {
            bpr_ivs_add(&c->iv[v], off, off + BPR_BLOCK_HDR + comp,
                        BPR_K_BLOCK, idx);
            bpr_ivs_add(&c->blk[v], off, off + BPR_BLOCK_HDR + comp,
                        BPR_K_BLOCK, idx);
        }
        e->covered_orig += orig;
        e->blocks_comp = comp_sum + BPR_BLOCK_HDR + comp;
        remaining -= orig;
        comp_sum += BPR_BLOCK_HDR + comp;
        e->blocks++;
        off += BPR_BLOCK_HDR + comp;
        bpr_norm_pos(c, &v, &off);
        e->end_vol = v;
        e->end_off = off;
    }
    if (bpr_read_struct(c, v, off, t, sizeof t, &f, "trailer-truncated") != 0)
        goto fail;
    if (bpr_crc32(0, t, 12) != bpr_le32(t + 12)) {
        f.reason = "trailer-crc";
        goto fail;
    }
    if (bpr_le64(t) != comp_sum) {
        f.reason = "trailer-size";
        goto fail;
    }
    if (account) bpr_ivs_add(&c->iv[v], off, off + BPR_ENTRY_TRL, BPR_K_ENTTRL, idx);
    e->comp_size = comp_sum;
    e->checksum = bpr_le32(t + 8);
    e->end_vol = v;
    e->end_off = off + BPR_ENTRY_TRL;
    e->status = BPR_ST_OK;
    return 0;
fail:
    e->reason = f.reason;
    e->need_volume = f.need_volume;
    return -1;
}

static int cmp_iv(const void *a, const void *b)
{
    const bpr_iv *x = (const bpr_iv *)a, *y = (const bpr_iv *)b;
    if (x->s != y->s) return x->s < y->s ? -1 : 1;
    return x->e < y->e ? -1 : (x->e > y->e);
}

static void sort_ivs(bpr_ivs *v)
{
    if (v->dirty && v->n > 1) qsort(v->a, v->n, sizeof(bpr_iv), cmp_iv);
    v->dirty = 0;
}

/* Is off inside a verified block of volume vol? */
static int inside_verified_block(bpr_ctx *c, int vol, uint64_t off)
{
    bpr_ivs *b = &c->blk[vol];
    size_t lo = 0, hi = b->n;
    sort_ivs(b);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (b->a[mid].e <= off) lo = mid + 1;
        else hi = mid;
    }
    return lo < b->n && b->a[lo].s <= off && off < b->a[lo].e;
}

static int add_entry(bpr_ctx *c, const bpr_entry *e)
{
    if (c->nent == c->capent) {
        size_t cap = c->capent ? c->capent * 2 : 256;
        bpr_entry *n = (bpr_entry *)realloc(c->ent, cap * sizeof(*n));
        if (n == NULL) return -1;
        c->ent = n;
        c->capent = cap;
    }
    c->ent[c->nent++] = *e;
    return 0;
}

static void choose_end_record(bpr_ctx *c)
{
    size_t i;
    int best = -1;
    for (i = 0; i < c->nendc; i++) {
        const bpr_endrec *e = &c->endc[i];
        if (c->have_set && e->set_id != c->set_id) continue;
        if (best < 0 || e->vol > c->endc[best].vol ||
            (e->vol == c->endc[best].vol && e->off > c->endc[best].off))
            best = (int)i;
    }
    if (best < 0) return;
    c->have_end = 1;
    c->end = c->endc[best];
    {
        const bpr_vol *x = bpr_vol_at(c, c->end.vol);
        c->end_at_expected = x != NULL &&
            c->end.off == x->size - BPR_VOL_TRL - BPR_END_REC;
    }
}

/* Read len bytes of the archive stream starting at (v, off): the
 * concatenation of volume bodies, as the table is stored. */
static int stream_read(const bpr_ctx *c, int v, uint64_t off, uint8_t *dst,
                       uint64_t len, int *need)
{
    while (len > 0) {
        const bpr_vol *x = bpr_vol_at(c, v);
        uint64_t limit, take;
        if (x == NULL) {
            *need = v;
            return -1;
        }
        limit = x->size - BPR_VOL_TRL;
        if (off >= limit) {
            v++;
            off = BPR_VOL_HDR;
            continue;
        }
        take = limit - off;
        if (take > len) take = len;
        if (bpr_file_read(x->f, off, dst, (size_t)take) != 0) return -2;
        dst += take;
        len -= take;
        off += take;
        if (off == limit) {
            v++;
            off = BPR_VOL_HDR;
        }
    }
    return 0;
}

static void add_table_intervals(bpr_ctx *c, int v, uint64_t off, uint64_t len)
{
    while (len > 0) {
        const bpr_vol *x = bpr_vol_at(c, v);
        uint64_t limit, take;
        if (x == NULL) return;
        limit = x->size - BPR_VOL_TRL;
        if (off >= limit) {
            v++;
            off = BPR_VOL_HDR;
            continue;
        }
        take = limit - off;
        if (take > len) take = len;
        bpr_ivs_add(&c->iv[v], off, off + take, BPR_K_TABLE, -1);
        len -= take;
        v++;
        off = BPR_VOL_HDR;
    }
}

static int entry_cmp_loc(const bpr_entry *e, int vol, uint64_t off)
{
    if (e->vol != vol) return e->vol < vol ? -1 : 1;
    if (e->off != off) return e->off < off ? -1 : 1;
    return 0;
}

static long find_entry(const bpr_ctx *c, int vol, uint64_t off)
{
    size_t lo = 0, hi = c->nent;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int r = entry_cmp_loc(&c->ent[mid], vol, off);
        if (r == 0) return (long)mid;
        if (r < 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

static void read_table(bpr_ctx *c)
{
    bpr_table *t = &c->table;
    uint8_t *buf;
    int need = -1, rc;
    uint32_t i, count;
    uint64_t pos;

    t->status = BPR_TBL_ABSENT;
    t->why = "no end record";
    if (!c->have_end) return;
    if (c->end.table_size < BPR_TABLE_HDR || c->end.table_size > (1u << 30)) {
        t->status = BPR_TBL_DAMAGED;
        t->why = "implausible table size";
        return;
    }
    buf = (uint8_t *)malloc((size_t)c->end.table_size);
    if (buf == NULL) { t->status = BPR_TBL_DAMAGED; t->why = "no-memory"; return; }
    rc = stream_read(c, c->end.table_volume, c->end.table_offset, buf,
                     c->end.table_size, &need);
    if (rc == -1) {
        t->status = BPR_TBL_UNREADABLE;
        t->why = "missing-volume";
        free(buf);
        return;
    }
    if (rc != 0) {
        t->status = BPR_TBL_UNREADABLE;
        t->why = "read-error";
        free(buf);
        return;
    }
    if (bpr_crc32(0, buf, (size_t)c->end.table_size) != c->end.table_crc) {
        t->status = BPR_TBL_DAMAGED;
        t->why = "table CRC mismatch";
        free(buf);
        return;
    }
    if (bpr_le32(buf) != BPR_MAGIC_TBL) {
        t->status = BPR_TBL_DAMAGED;
        t->why = "bad table magic";
        free(buf);
        return;
    }
    count = bpr_le32(buf + 4);
    t->total_orig = bpr_le64(buf + 8);
    if (count != c->end.entry_count) {
        t->status = BPR_TBL_DAMAGED;
        t->why = "entry count disagrees with the end record";
        free(buf);
        return;
    }
    t->rec = (bpr_entry *)calloc(count ? count : 1, sizeof(bpr_entry));
    if (t->rec == NULL) { free(buf); t->status = BPR_TBL_DAMAGED; t->why = "no-memory"; return; }
    pos = BPR_TABLE_HDR;
    for (i = 0; i < count; i++) {
        bpr_entry *r = &t->rec[i];
        uint32_t nl;
        uint64_t f;
        if (pos + 2 > c->end.table_size) goto bad;
        nl = bpr_le16(buf + pos);
        if (nl < 1 || nl > BPR_MAX_NAME) goto bad;
        f = pos + 2 + nl;
        if (f + 45 > c->end.table_size) goto bad;
        if (!bp_archive_name_valid((const char *)buf + pos + 2, nl)) goto bad;
        r->name_len = (uint16_t)nl;
        r->name = (uint8_t *)malloc(nl);
        if (r->name == NULL) goto bad;
        memcpy(r->name, buf + pos + 2, nl);
        r->original_size = bpr_le64(buf + f);
        r->comp_size = bpr_le64(buf + f + 8);
        r->entry_type = buf[f + 16];
        r->platform = buf[f + 17];
        r->mtime = bpr_le64(buf + f + 18);
        r->vol = bpr_le16(buf + f + 26);
        r->off = bpr_le64(buf + f + 28);
        r->method = buf[f + 36];
        r->header_size = bpr_le16(buf + f + 37);
        r->checksum = bpr_le32(buf + f + 39);
        r->flags = bpr_le16(buf + f + 43);
        r->status = BPR_ST_TABLE_ONLY;
        r->inside = -1;
        r->in_table = 1;
        r->table_index = (int)i;
        r->need_volume = -1;
        pos = f + 45;
        t->nrec = i + 1;
    }
    if (pos != c->end.table_size) goto bad;
    t->count = count;
    t->status = BPR_TBL_TRUSTED;
    t->why = NULL;
    free(buf);
    return;
bad:
    t->status = BPR_TBL_DAMAGED;
    t->why = "malformed table record";
    free(buf);
}

static const char *compare_record(const bpr_entry *e, const bpr_entry *r)
{
    if (e->name_len != r->name_len ||
        memcmp(e->name, r->name, e->name_len) != 0) return "name";
    if (e->original_size != r->original_size) return "original_size";
    if (e->entry_type != r->entry_type) return "entry_type";
    if (e->platform != r->platform) return "platform";
    if (e->mtime != r->mtime) return "mtime";
    if (e->method != r->method) return "method";
    if (e->header_size != r->header_size) return "header_size";
    if (e->flags != r->flags) return "flags";
    if (e->status == BPR_ST_OK) {
        if (e->comp_size != r->comp_size) return "compressed_size";
        if (e->checksum != r->checksum) return "checksum";
    }
    return NULL;
}

static void match_table(bpr_ctx *c)
{
    bpr_table *t = &c->table;
    uint32_t i;
    size_t k;
    if (t->status != BPR_TBL_TRUSTED) return;
    for (k = 0; k < c->nent; k++) c->ent[k].in_table = 0;
    for (i = 0; i < t->nrec; i++) {
        bpr_entry *r = &t->rec[i];
        long m = find_entry(c, r->vol, r->off);
        if (m >= 0) {
            bpr_entry *e = &c->ent[m];
            e->in_table = 1;
            e->table_index = (int)i;
            e->table_conflict = compare_record(e, r);
            r->reason = NULL;
            r->status = -1;             /* matched: not reported separately */
        } else if (bpr_vol_at(c, r->vol) == NULL) {
            r->reason = "volume-missing";
        } else {
            r->reason = "no-valid-header-at-location";
        }
    }
    /* A trusted table is authority for entries it lists: one that only looked
     * suspect because it lies where a damaged entry's data could extend is
     * real. */
    for (k = 0; k < c->nent; k++) {
        bpr_entry *e = &c->ent[k];
        if (e->confidence == BPR_CONF_SUSPECT && e->in_table == 1 &&
            e->table_conflict == NULL) {
            e->confidence = BPR_CONF_ISOLATED;
            e->suspect_reason = NULL;
            e->inside = -1;
        }
    }
    /* A trusted table lists every real entry.  A header-shaped hit it does
     * not list is not one of them, however well it parses (typically a
     * .bolt held inside a damaged entry). */
    for (k = 0; k < c->nent; k++) {
        bpr_entry *e = &c->ent[k];
        if (e->in_table == 0 && e->confidence != BPR_CONF_SUSPECT && !e->pure) {
            e->confidence = BPR_CONF_SUSPECT;
            e->suspect_reason = "not-in-table";
        }
    }
}

static int add_span(bpr_ctx *c, int vol, uint64_t s, uint64_t e)
{
    size_t n = c->nspan;
    void *p = realloc(c->span, (n + 1) * sizeof(c->span[0]));
    if (p == NULL) return -1;
    c->span = (bpr_span *)p;
    c->span[n].vol = vol;
    c->span[n].s = s;
    c->span[n].e = e;
    c->nspan = n + 1;
    return 0;
}

/* Bytes of an entry labelled suspect are not trusted structure: leave them
 * out of the accounted set so they are reported as unaccounted and later
 * phases may examine them (for example for the blocks that contain them). */
static void strip_suspect(bpr_ctx *c)
{
    int v;
    for (v = 0; v <= c->max_number; v++) {
        bpr_ivs *iv = &c->iv[v];
        size_t i, w = 0;
        for (i = 0; i < iv->n; i++) {
            const bpr_iv *x = &iv->a[i];
            int drop = x->entry >= 0 && (size_t)x->entry < c->nent &&
                       c->ent[x->entry].confidence == BPR_CONF_SUSPECT &&
                       (x->kind == BPR_K_ENTHDR || x->kind == BPR_K_BLOCK ||
                        x->kind == BPR_K_ENTTRL);
            if (!drop) iv->a[w++] = *x;
        }
        iv->n = w;
    }
}

static void compute_spans(bpr_ctx *c)
{
    int v;
    strip_suspect(c);
    for (v = 0; v <= c->max_number; v++) {
        const bpr_vol *x = c->slot[v];
        bpr_ivs *iv = &c->iv[v];
        uint64_t cur;
        size_t i;
        if (x == NULL) continue;
        if (x->hdr_ok) bpr_ivs_add(iv, 0, BPR_VOL_HDR, BPR_K_VOLHDR, -1);
        if (x->trl_ok)
            bpr_ivs_add(iv, x->size - BPR_VOL_TRL, x->size, BPR_K_VOLTRL, -1);
        iv->dirty = 1;
        sort_ivs(iv);
        cur = 0;
        for (i = 0; i < iv->n; i++) {
            if (iv->a[i].s > cur) add_span(c, v, cur, iv->a[i].s);
            if (iv->a[i].e > cur) cur = iv->a[i].e;
        }
        if (cur < x->size) add_span(c, v, cur, x->size);
    }
}

int bpr_analyze(bpr_ctx *c)
{
    static uint64_t sus_from[BPR_MAX_VOLUMES], sus_end[BPR_MAX_VOLUMES];
    static int sus_owner[BPR_MAX_VOLUMES];
    int chain_v = -1, i, chain_broke = 0, chain_pure = 1;
    uint64_t chain_off = 0;
    size_t k;

    c->nvolumes_present = 0;
    for (i = 0; i <= c->max_number; i++)
        if (c->slot[i] != NULL) c->nvolumes_present++;

    if (bpr_scan_all(c) != 0) return -1;

    if (bpr_vol_at(c, 0) != NULL) {
        chain_v = 0;
        chain_off = BPR_VOL_HDR;
    } else {
        chain_broke = 1;
    }
    for (k = 0; k < c->ncand; k++) {
        bpr_cand *cand = &c->cand[k];
        bpr_entry e;
        int chained, suspect = 0, owner = -1, idx = (int)c->nent;

        if (inside_verified_block(c, cand->vol, cand->off)) {
            c->ignored_inside_blocks++;
            free(cand->name);
            cand->name = NULL;
            continue;
        }
        chained = cand->vol == chain_v && cand->off == chain_off;
        if (!chained && cand->off >= sus_from[cand->vol] &&
            cand->off < sus_end[cand->vol]) {
            suspect = 1;
            owner = sus_owner[cand->vol];
            if (trailer_closes(c, &c->ent[owner], cand)) {
                suspect = 0;
                owner = -1;
            }
        }
        bpr_verify_entry(c, cand, idx, &e, 1);
        cand->name = NULL;
        e.pure = chained && chain_pure;
        e.confidence = chained ? BPR_CONF_CHAINED
                     : suspect ? BPR_CONF_SUSPECT : BPR_CONF_ISOLATED;
        e.inside = owner;
        e.suspect_reason = suspect ? "inside-damaged-entry" : NULL;
        if (add_entry(c, &e) != 0) return -1;

        if (!chained && !suspect) { chain_broke = 1; chain_pure = 0; }
        if (e.status == BPR_ST_OK) {
            if (!suspect) {
                chain_v = e.end_vol;
                chain_off = e.end_off;
                bpr_norm_pos(c, &chain_v, &chain_off);
            }
        } else {
            if (chained) {
                chain_v = -1;
                /* The blocks it verified run back to back to a valid trailer
                 * (whole blocks are missing, or the trailer's size field is
                 * wrong): the next structure starts right after, as in a
                 * clean archive. */
                if (e.blocks > 0 && trailer_at(c, e.end_vol, e.end_off)) {
                    chain_v = e.end_vol;
                    chain_off = e.end_off + BPR_ENTRY_TRL;
                    bpr_norm_pos(c, &chain_v, &chain_off);
                }
            }
            if (chained || !suspect) chain_broke = 1;
            {
                uint64_t from = (e.end_vol == e.vol) ? e.end_off
                                                     : e.off + e.header_size;
                uint64_t end = ext_bound(c, &(bpr_cand){
                    .off = e.off, .header_size = e.header_size,
                    .original_size = e.original_size,
                    .block_size = e.block_size });
                /* All blocks verified: the entry ends exactly 16 bytes (its
                 * trailer) later, so nothing can hide inside its data. */
                if (e.covered_orig == e.original_size) end = from;
                if (end > from && (end > sus_end[e.vol] || from >= sus_end[e.vol])) {
                    sus_from[e.vol] = from;
                    sus_end[e.vol] = end;
                    sus_owner[e.vol] = idx;
                }
            }
        }
    }
    if (!chain_broke && chain_v >= 0 && bpr_vol_at(c, chain_v) != NULL) {
        uint8_t m[4];
        bpr_fail f = { NULL, -1 };
        if (bpr_read_struct(c, chain_v, chain_off, m, 4, &f, "x") == 0 &&
            bpr_le32(m) == BPR_MAGIC_TBL)
            c->chain_reached_table = 1;
    }

    choose_end_record(c);
    if (c->have_end) {
        c->volume_count = c->end.volume_count;
        c->count_source = "end-record";
    } else {
        for (i = 0; i <= c->max_number; i++)
            if (c->slot[i] != NULL && c->slot[i]->trl_ok &&
                c->slot[i]->trl_last) {
                c->volume_count = i + 1;
                c->count_source = "trailer";
            }
    }
    c->last_found = c->have_end || c->volume_count >= 0;
    c->nmissing = 0;
    {
        int top = c->volume_count >= 0 ? c->volume_count - 1 : c->max_number;
        for (i = 0; i <= top && i < BPR_MAX_VOLUMES; i++)
            if (c->slot[i] == NULL) c->missing[c->nmissing++] = i;
        c->complete = c->volume_count >= 0 && c->nmissing == 0;
    }

    read_table(c);
    if (c->table.status == BPR_TBL_TRUSTED)
        add_table_intervals(c, c->end.table_volume, c->end.table_offset,
                            c->end.table_size);
    if (c->have_end)
        bpr_ivs_add(&c->iv[c->end.vol], c->end.off, c->end.off + BPR_END_REC,
                    BPR_K_ENDREC, -1);
    match_table(c);
    compute_spans(c);
    free(g_buf);
    g_buf = NULL;
    g_cap = 0;
    return 0;
}
