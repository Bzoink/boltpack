/*
 * bpr_scan.c -- find BoltPack structures by content, ignoring position.
 *
 * Entry headers carry no magic, so a candidate is any offset where
 * header_size == 34 + name_len, the trailing CRC-32 matches, and the fields
 * obey the format's rules (spec 2.3.1).  The first test rejects nearly every
 * offset after two compares, so the pass runs at read speed.  A hit proves
 * the bytes form a valid header, not that they are an entry of this archive:
 * a .bolt stored inside a .bolt contains genuine headers.  The analysis, not
 * the scanner, decides which hits count.
 */
#include "bpr.h"

#include "archive_name.h"

#include <stdlib.h>
#include <string.h>

#define CHUNK (8u << 20)
#define OVER  (65536u + 64u)

int bpr_parse_entry_header(const uint8_t *p, size_t avail, bpr_cand *out)
{
    unsigned hs, nl, in;
    uint8_t etype;
    uint64_t orig;
    uint32_t bs;

    if (avail < BPR_ENTRY_FIXED + 1) return 0;
    hs = bpr_le16(p);
    nl = bpr_le16(p + 2);
    if (hs != BPR_ENTRY_FIXED + nl || nl < 1 || nl > BPR_MAX_NAME) return 0;
    if (avail < hs) return 0;
    in = 4 + nl;
    if (p[in + 9] != 1) return 0;               /* checksum_type: CRC-32 */
    if (p[in + 8] > 3) return 0;                /* method 0-3 (4 retired) */
    etype = p[in + 14];
    if (etype > 4) return 0;
    orig = bpr_le64(p + in);
    bs = bpr_le32(p + in + 10);
    if (etype == 4 && orig != 0) return 0;
    if (orig > 0 && (bs < 1 || bs > BPR_MAX_BLOCK)) return 0;
    if (bpr_crc32(0, p, hs - 4) != bpr_le32(p + hs - 4)) return 0;
    if (!bp_archive_name_valid((const char *)p + 4, nl)) return 0;
    if (!bp_archive_name_type_valid((const char *)p + 4, nl, etype)) return 0;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->header_size = (uint16_t)hs;
        out->name_len = (uint16_t)nl;
        out->original_size = orig;
        out->method = p[in + 8];
        out->checksum_type = p[in + 9];
        out->entry_type = etype;
        out->platform = p[in + 15];
        out->mtime = bpr_le64(p + in + 16);
        out->flags = bpr_le16(p + in + 24);
        out->block_size = bs;
        out->name = (uint8_t *)malloc(nl);
        if (out->name == NULL) return 0;
        memcpy(out->name, p + 4, nl);
    }
    return 1;
}

static int add_cand(bpr_ctx *c, const bpr_cand *k)
{
    if (c->ncand == c->capcand) {
        size_t cap = c->capcand ? c->capcand * 2 : 256;
        bpr_cand *n = (bpr_cand *)realloc(c->cand, cap * sizeof(*n));
        if (n == NULL) return -1;
        c->cand = n;
        c->capcand = cap;
    }
    c->cand[c->ncand++] = *k;
    return 0;
}

static int add_tblt(bpr_ctx *c, int vol, uint64_t off)
{
    if (c->ntblt == c->captblt) {
        size_t cap = c->captblt ? c->captblt * 2 : 16;
        uint64_t *n = (uint64_t *)realloc(c->tblt_hits, cap * sizeof(*n));
        if (n == NULL) return -1;
        c->tblt_hits = n;
        c->captblt = cap;
    }
    c->tblt_hits[c->ntblt++] = ((uint64_t)vol << 48) | off;
    return 0;
}

/* The end record ends with its magic; verify one whose magic sits at abs. */
static void check_end_record(bpr_ctx *c, const bpr_vol *v, int vol,
                             uint64_t magic_at)
{
    uint8_t r[BPR_END_REC];
    bpr_endrec e;
    uint64_t start;
    if (magic_at < BPR_END_REC - 4 + BPR_VOL_HDR) return;
    start = magic_at - (BPR_END_REC - 4);
    if (bpr_file_read(v->f, start, r, sizeof r) != 0) return;
    if (bpr_le32(r + 44) != BPR_MAGIC_END) return;
    if (bpr_crc32(0, r, 40) != bpr_le32(r + 40)) return;
    if (bpr_le32(r + 36) != 0) return;
    memset(&e, 0, sizeof e);
    e.vol = vol;
    e.off = start;
    e.set_id = bpr_le64(r);
    e.volume_count = bpr_le16(r + 8);
    e.table_volume = bpr_le16(r + 10);
    e.table_crc = bpr_le32(r + 12);
    e.table_offset = bpr_le64(r + 16);
    e.table_size = bpr_le64(r + 24);
    e.entry_count = bpr_le32(r + 32);
    if (e.volume_count < 1 || e.volume_count > BPR_MAX_VOLUMES) return;
    if (c->nendc == c->capendc) {
        size_t cap = c->capendc ? c->capendc * 2 : 4;
        bpr_endrec *n = (bpr_endrec *)realloc(c->endc, cap * sizeof(*n));
        if (n == NULL) return;
        c->endc = n;
        c->capendc = cap;
    }
    c->endc[c->nendc++] = e;
}

int bpr_scan_all(bpr_ctx *c)
{
    uint8_t *buf = (uint8_t *)malloc((size_t)CHUNK + OVER);
    int vol;
    if (buf == NULL) return -1;
    for (vol = 0; vol <= c->max_number; vol++) {
        const bpr_vol *v = c->slot[vol];
        uint64_t lo = BPR_VOL_HDR, hi, pos;
        if (v == NULL) continue;
        hi = v->trl_ok ? v->size - BPR_VOL_TRL : v->size;
        for (pos = lo; pos < hi; pos += CHUNK) {
            uint64_t want = v->size - pos;
            uint64_t span = hi - pos;
            size_t i, limit;
            if (want > (uint64_t)CHUNK + OVER) want = (uint64_t)CHUNK + OVER;
            if (bpr_file_read(v->f, pos, buf, (size_t)want) != 0) break;
            limit = (size_t)(span < CHUNK ? span : CHUNK);
            for (i = 0; i < limit; i++) {
                const uint8_t *p = buf + i;
                size_t avail = (size_t)want - i;
                unsigned hs, nl;
                if (avail < 4) break;
                hs = bpr_le16(p);
                nl = bpr_le16(p + 2);
                if (hs == BPR_ENTRY_FIXED + nl && nl >= 1 && avail >= hs) {
                    bpr_cand k;
                    if (bpr_parse_entry_header(p, avail, &k)) {
                        k.vol = vol;
                        k.off = pos + i;
                        if (add_cand(c, &k) != 0) {
                            free(k.name);
                            free(buf);
                            return -1;
                        }
                    }
                }
                {
                    uint32_t m = bpr_le32(p);
                    if (m == BPR_MAGIC_TBL) add_tblt(c, vol, pos + i);
                    else if (m == BPR_MAGIC_END)
                        check_end_record(c, v, vol, pos + i);
                }
            }
        }
    }
    free(buf);
    return 0;
}
