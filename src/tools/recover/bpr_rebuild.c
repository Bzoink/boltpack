/*
 * bpr_rebuild.c -- write a new, valid archive from the entries that survived.
 *
 * The archive is written by the core's own writer (bp_archive_create_volumes
 * and bp_archive_commit_packed_entry), so the volume layout, end-of-volume
 * blocks, file table, end record and trailers are exactly what every frontend
 * expects, and --volume-size re-splits blocks the way the CLI does.  This
 * module only decides what goes in and feeds each entry to the writer as a
 * stream: header, blocks, trailer.
 *
 * Every entry is verified first, by decoding it and checking the payload
 * against the trailer's (or the trusted table's) checksum, so the writer never
 * meets a structure that fails in the middle of a volume.  A header that was
 * lost is rebuilt from the trusted table's record; a trailer is always written
 * from what was verified (compressed size of the blocks read, payload CRC-32).
 * Entries that are not whole are left out and reported.
 */
#include "bpr.h"

#include "archiver.h"
#include "bp_types.h"
#include "bp_metadata.h"
#include "compress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bp_uint64 u64c(uint64_t v)
{
    bp_uint64 r;
    r.lo = (bp_uint32)(v & 0xFFFFFFFFu);
    r.hi = (bp_uint32)(v >> 32);
    return r;
}

static const char *bp_strerror(int code)
{
    switch (code) {
    case BP_ERR_IO:                 return "I/O error";
    case BP_ERR_NOMEM:              return "out of memory";
    case BP_ERR_CHECKSUM:           return "checksum mismatch";
    case BP_ERR_CORRUPT:            return "a structure is corrupt";
    case BP_ERR_TRUNCATED:          return "unexpected end of data";
    case BP_ERR_OVERFLOW:           return "size exceeds the volume size";
    case BP_ERR_NAME_TOO_LONG:      return "file name too long";
    case BP_ERR_UNSUPPORTED_METHOD: return "unsupported compression method";
    case BP_ERR_COMPRESS:           return "compression failed";
    case BP_ERR_INVALID_ARG:        return "invalid argument";
    case BP_ERR_VOLUME:             return "volume error";
    default:                        return "error";
    }
}

/* ---- output volumes ---------------------------------------------------- */

typedef struct {
    const char *out;                /* name of the last volume */
    bpr_rebuild_info *info;
    char       *tmp;
    bpr_wfile  *cur;
    bp_io_t     io;
} sinkctx;

static long io_read(void *h, void *buf, bp_uint32 n)
{
    return bpr_wfile_read((bpr_wfile *)h, buf, n);
}
static long io_write(void *h, const void *buf, bp_uint32 n)
{
    return bpr_wfile_write((bpr_wfile *)h, buf, n);
}
static int io_seek(void *h, bp_off_t off, int origin)
{
    return bpr_wfile_seek((bpr_wfile *)h, (int64_t)off, origin);
}
static bp_off_t io_tell(void *h)
{
    return (bp_off_t)bpr_wfile_tell((bpr_wfile *)h);
}

static void problem(bpr_rebuild_info *i, const char *fmt, const char *a)
{
    if (i->problem[0] == '\0') snprintf(i->problem, sizeof i->problem, fmt, a);
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p != NULL) memcpy(p, s, n);
    return p;
}

/* "backup.bolt" -> "backup.000": the extension replaced by the volume number
 * (spec 2.5); a name without an extension gets the number as one. */
static char *volume_name(const char *out, unsigned number)
{
    size_t n = strlen(out), stem = n, i;
    char *p = (char *)malloc(n + 8);
    if (p == NULL) return NULL;
    for (i = n; i > 0 && out[i - 1] != '/' && out[i - 1] != '\\'; i--)
        if (out[i - 1] == '.') { stem = i - 1; break; }
    memcpy(p, out, stem);
    sprintf(p + stem, ".%03u", number % 1000u);
    return p;
}

static bp_io_t *sink_create(void *user, bp_uint16 number)
{
    sinkctx *s = (sinkctx *)user;
    size_t n = strlen(s->out);
    s->tmp = (char *)malloc(n + 16);
    if (s->tmp == NULL) return NULL;
    sprintf(s->tmp, "%s.tmp%03u", s->out, (unsigned)number % 1000u);
    s->cur = bpr_wfile_create(s->tmp);
    if (s->cur == NULL) {
        problem(s->info, "cannot create %s", s->tmp);
        free(s->tmp);
        s->tmp = NULL;
        return NULL;
    }
    s->io.read = io_read;
    s->io.write = io_write;
    s->io.seek = io_seek;
    s->io.tell = io_tell;
    s->io.handle = s->cur;
    return &s->io;
}

static int sink_finish(void *user, bp_io_t *io, bp_uint16 number, int is_last,
                       int status)
{
    sinkctx *s = (sinkctx *)user;
    char *final = NULL;
    int rc = 0;
    (void)io;
    if (s->cur != NULL && bpr_wfile_close(s->cur) != 0 && status == BP_OK) {
        problem(s->info, "write error on %s", s->tmp);
        status = BP_ERR_IO;
    }
    s->cur = NULL;
    if (status != BP_OK) {
        if (s->tmp != NULL) bpr_path_remove(s->tmp);
        free(s->tmp);
        s->tmp = NULL;
        return 0;
    }
    final = is_last ? dup_str(s->out) : volume_name(s->out, number);
    if (final == NULL || bpr_path_rename_new(s->tmp, final) != 0) {
        problem(s->info, "cannot create %s (does it exist?)", final ? final : s->out);
        bpr_path_remove(s->tmp);
        free(final);
        rc = -1;
    } else {
        char **nf = (char **)realloc(s->info->files,
                                     (s->info->nfiles + 1) * sizeof(char *));
        if (nf == NULL) { rc = -1; free(final); }
        else {
            s->info->files = nf;
            nf[s->info->nfiles++] = final;
        }
    }
    free(s->tmp);
    s->tmp = NULL;
    return rc;
}

/* ---- one entry as a stream --------------------------------------------- */

typedef struct {
    const bpr_ctx   *c;
    const bpr_entry *e;
    uint8_t         *hdr;
    size_t           hdr_n;
    uint8_t          trl[BPR_ENTRY_TRL];
    int              phase;         /* 0 header, 1 blocks, 2 trailer, 3 done */
    size_t           pos;
    int              v;
    uint64_t         off, remaining;
    uint8_t         *blk;
    size_t           blk_n, blk_cap, blk_pos;
} eread;

static void er_reset(eread *r)
{
    r->phase = 0;
    r->pos = 0;
    r->v = r->e->vol;
    r->off = r->e->off + r->e->header_size;
    bpr_norm_pos(r->c, &r->v, &r->off);
    r->remaining = r->e->original_size;
    r->blk_n = r->blk_pos = 0;
}

static int er_load_block(eread *r)
{
    uint8_t h[BPR_BLOCK_HDR];
    bpr_fail f = { NULL, -1 };
    uint32_t comp, orig;
    const bpr_vol *x;
    if (bpr_read_struct(r->c, r->v, r->off, h, sizeof h, &f, "x") != 0) return -1;
    comp = bpr_le32(h);
    orig = bpr_le32(h + 4);
    if (comp == 0 || orig == 0 || comp > orig || orig > r->remaining) return -1;
    x = bpr_vol_at(r->c, r->v);
    if (r->off + BPR_BLOCK_HDR + comp > bpr_read_limit(x)) return -1;
    if (r->blk_cap < BPR_BLOCK_HDR + (size_t)comp) {
        uint8_t *n = (uint8_t *)realloc(r->blk, BPR_BLOCK_HDR + (size_t)comp);
        if (n == NULL) return -1;
        r->blk = n;
        r->blk_cap = BPR_BLOCK_HDR + (size_t)comp;
    }
    memcpy(r->blk, h, BPR_BLOCK_HDR);
    if (bpr_file_read(x->f, r->off + BPR_BLOCK_HDR, r->blk + BPR_BLOCK_HDR, comp) != 0)
        return -1;
    r->blk_n = BPR_BLOCK_HDR + (size_t)comp;
    r->blk_pos = 0;
    r->remaining -= orig;
    r->off += BPR_BLOCK_HDR + comp;
    bpr_norm_pos(r->c, &r->v, &r->off);
    return 0;
}

static long er_read(void *h, void *buf, bp_uint32 count)
{
    eread *r = (eread *)h;
    uint8_t *out = (uint8_t *)buf;
    size_t got = 0;
    while (got < count && r->phase < 3) {
        const uint8_t *src;
        size_t avail, take;
        if (r->phase == 0) {
            src = r->hdr + r->pos;
            avail = r->hdr_n - r->pos;
        } else if (r->phase == 1) {
            if (r->blk_pos == r->blk_n) {
                if (r->remaining == 0) { r->phase = 2; r->pos = 0; continue; }
                if (er_load_block(r) != 0) return -1;
            }
            src = r->blk + r->blk_pos;
            avail = r->blk_n - r->blk_pos;
        } else {
            src = r->trl + r->pos;
            avail = BPR_ENTRY_TRL - r->pos;
        }
        take = count - got < avail ? count - got : avail;
        memcpy(out + got, src, take);
        got += take;
        if (r->phase == 0) {
            r->pos += take;
            if (r->pos == r->hdr_n) { r->phase = 1; r->pos = 0; }
        } else if (r->phase == 1) {
            r->blk_pos += take;
        } else {
            r->pos += take;
            if (r->pos == BPR_ENTRY_TRL) r->phase = 3;
        }
    }
    return (long)got;
}

static long er_write(void *h, const void *buf, bp_uint32 n)
{
    (void)h; (void)buf; (void)n;
    return -1;
}
static int er_seek(void *h, bp_off_t off, int origin)
{
    if (off != 0 || origin != 0) return -1;
    er_reset((eread *)h);
    return 0;
}
static bp_off_t er_tell(void *h)
{
    (void)h;
    return 0;
}

/* The entry's header: verbatim from the volume, or built from the trusted
 * table's record when the original was lost. */
static uint8_t *entry_header(const bpr_ctx *c, const bpr_entry *e, size_t *n)
{
    uint8_t *h = (uint8_t *)malloc(e->header_size);
    if (h == NULL) return NULL;
    if (!e->from_table) {
        bpr_fail f = { NULL, -1 };
        if (bpr_read_struct(c, e->vol, e->off, h, e->header_size, &f, "x") != 0 ||
            bpr_crc32(0, h, e->header_size - 4u) !=
                bpr_le32(h + e->header_size - 4u)) {
            free(h);
            return NULL;
        }
    } else {
        unsigned nl = e->name_len, in = 4 + nl, i;
        uint32_t bs = e->original_size ? bpr_infer_block_size(c, e) : 0;
        uint64_t v;
        if (e->header_size != BPR_ENTRY_FIXED + nl || (e->original_size && bs == 0)) {
            free(h);
            return NULL;
        }
        memset(h, 0, e->header_size);
        h[0] = (uint8_t)e->header_size; h[1] = (uint8_t)(e->header_size >> 8);
        h[2] = (uint8_t)nl; h[3] = (uint8_t)(nl >> 8);
        memcpy(h + 4, e->name, nl);
        for (i = 0, v = e->original_size; i < 8; i++, v >>= 8) h[in + i] = (uint8_t)v;
        h[in + 8] = e->method;
        h[in + 9] = 1;                              /* CRC-32 */
        for (i = 0; i < 4; i++) h[in + 10 + i] = (uint8_t)(bs >> (8 * i));
        h[in + 14] = e->entry_type;
        h[in + 15] = e->platform;
        for (i = 0, v = e->mtime; i < 8; i++, v >>= 8) h[in + 16 + i] = (uint8_t)v;
        h[in + 24] = (uint8_t)e->flags; h[in + 25] = (uint8_t)(e->flags >> 8);
        {
            uint32_t crc = bpr_crc32(0, h, e->header_size - 4u);
            for (i = 0; i < 4; i++) h[e->header_size - 4 + i] = (uint8_t)(crc >> (8 * i));
        }
    }
    *n = e->header_size;
    return h;
}

/* ---- what the writer will accept ----------------------------------------
 * The core refuses an archive in which two data, directory or symlink entries
 * share a name, or in which a resource-fork or metadata entry does not directly
 * follow the entry it belongs to (src/core/bp_metadata.h).  Damage can produce
 * both: a data fork lost while its resource fork survived, or a name found
 * twice.  Apply the same rules while selecting, so that such entries are left
 * out and reported instead of failing the whole rebuild at the end. */

typedef struct {
    const uint8_t **name;           /* open-addressing set of normalized names */
    uint16_t       *len;
    size_t          cap, n;
} nameset;

static uint32_t name_hash(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* 1 if the name was added, 0 if already present, -1 on no memory. */
static int nameset_add(nameset *s, const uint8_t *p, size_t n)
{
    size_t i;
    if ((s->n + 1) * 2 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 256, j;
        const uint8_t **nn = (const uint8_t **)calloc(ncap, sizeof *nn);
        uint16_t *nl = (uint16_t *)calloc(ncap, sizeof *nl);
        if (nn == NULL || nl == NULL) { free(nn); free(nl); return -1; }
        for (j = 0; j < s->cap; j++)
            if (s->name[j] != NULL) {
                size_t q = name_hash(s->name[j], s->len[j]) & (ncap - 1);
                while (nn[q] != NULL) q = (q + 1) & (ncap - 1);
                nn[q] = s->name[j];
                nl[q] = s->len[j];
            }
        free(s->name);
        free(s->len);
        s->name = nn;
        s->len = nl;
        s->cap = ncap;
    }
    for (i = name_hash(p, n) & (s->cap - 1); s->name[i] != NULL;
         i = (i + 1) & (s->cap - 1))
        if (s->len[i] == n && memcmp(s->name[i], p, n) == 0) return 0;
    s->name[i] = p;
    s->len[i] = (uint16_t)n;
    s->n++;
    return 1;
}

static size_t normalized_len(const bpr_entry *e)
{
    size_t n = e->name_len;
    while (n > 0 && e->name[n - 1] == '/') n--;
    return n;
}

/* Drop from sel (in archive order) every entry the writer would refuse.
 * Returns the new count, or (size_t)-1 on no memory. */
static size_t apply_group_rules(bpr_xres **sel, size_t nsel)
{
    nameset set;
    const bpr_entry *head = NULL;
    int resource_seen = 0, portable_seen = 0;
    size_t k, w = 0;

    memset(&set, 0, sizeof set);
    for (k = 0; k < nsel; k++) {
        bpr_xres *r = sel[k];
        const bpr_entry *e = r->e;
        if (e->entry_type != BP_TYPE_RSRC && e->entry_type != BP_TYPE_META) {
            int a = nameset_add(&set, e->name, normalized_len(e));
            if (a < 0) { free(set.name); free(set.len); return (size_t)-1; }
            if (a == 0) {
                r->outcome = BPR_X_SKIP_DUPLICATE;
                r->detail = "another entry with this name is in the new archive";
                head = NULL;        /* its supplementary entries lose their owner */
                continue;
            }
            head = e;
            resource_seen = portable_seen = 0;
        } else {
            int ok = head != NULL && head->name_len == e->name_len &&
                     memcmp(head->name, e->name, e->name_len) == 0;
            if (ok && e->entry_type == BP_TYPE_RSRC)
                ok = head->entry_type == BP_TYPE_DATA && !resource_seen;
            if (ok && e->entry_type == BP_TYPE_META)
                ok = (head->entry_type == BP_TYPE_DATA ||
                      head->entry_type == BP_TYPE_DIR) &&
                     !(e->platform == BP_META_PORTABLE && portable_seen);
            if (!ok) {
                r->outcome = BPR_X_SKIP_SUPPLEMENTARY;
                r->detail = "its owning entry is not in the new archive";
                continue;
            }
            if (e->entry_type == BP_TYPE_RSRC) resource_seen = 1;
            else if (e->platform == BP_META_PORTABLE) portable_seen = 1;
        }
        sel[w++] = r;
    }
    free(set.name);
    free(set.len);
    return w;
}

/* ---- an entry's payload as a stream ------------------------------------ */

/* Used to re-add an entry whose blocks cannot be copied: the payload is decoded
 * block by block and the core cuts and encodes it again. */
typedef struct {
    const bpr_ctx   *c;
    const bpr_entry *e;
    bpr_decoder      d;
    int              have_d;
    int              v;
    uint64_t         off, remaining;
    size_t           n, pos;
} dread;

static void dr_reset(dread *r)
{
    if (r->have_d) bpr_dec_end(&r->d);
    r->have_d = bpr_dec_begin(&r->d, r->e->method) == 0;
    r->v = r->e->vol;
    r->off = r->e->off + r->e->header_size;
    bpr_norm_pos(r->c, &r->v, &r->off);
    r->remaining = r->e->original_size;
    r->n = r->pos = 0;
}

static int dr_next(dread *r)
{
    uint8_t h[BPR_BLOCK_HDR];
    bpr_fail f = { NULL, -1 };
    uint32_t comp, orig;
    const bpr_vol *x;
    if (!r->have_d) return -1;
    if (bpr_read_struct(r->c, r->v, r->off, h, sizeof h, &f, "x") != 0) return -1;
    comp = bpr_le32(h);
    orig = bpr_le32(h + 4);
    if (comp == 0 || orig == 0 || comp > orig || orig > r->remaining) return -1;
    x = bpr_vol_at(r->c, r->v);
    if (r->off + BPR_BLOCK_HDR + comp > bpr_read_limit(x)) return -1;
    if (bpr_grow(&r->d.in, &r->d.in_cap, comp) != 0 ||
        bpr_file_read(x->f, r->off + BPR_BLOCK_HDR, r->d.in, comp) != 0 ||
        bpr_crc32(0, r->d.in, comp) != bpr_le32(h + 8) ||
        bpr_dec_block(&r->d, comp, orig) != NULL) return -1;
    r->n = orig;
    r->pos = 0;
    r->remaining -= orig;
    r->off += BPR_BLOCK_HDR + comp;
    bpr_norm_pos(r->c, &r->v, &r->off);
    return 0;
}

static long dr_read(void *h, void *buf, bp_uint32 count)
{
    dread *r = (dread *)h;
    size_t got = 0;
    while (got < count) {
        size_t take;
        if (r->pos == r->n) {
            if (r->remaining == 0) break;
            if (dr_next(r) != 0) return -1;
        }
        take = r->n - r->pos;
        if (take > count - got) take = count - got;
        memcpy((uint8_t *)buf + got, r->d.out + r->pos, take);
        r->pos += take;
        got += take;
    }
    return (long)got;
}

static int dr_seek(void *h, bp_off_t off, int origin)
{
    if (off != 0 || origin != 0) return -1;
    dr_reset((dread *)h);
    return 0;
}

/* Every block but the last of an entry must be a full block (spec 2.3.2),
 * except where a volume boundary falls.  Blocks that were short because of a
 * boundary in the source volumes are in the wrong place in any other layout,
 * and every reader rejects them: such an entry has to be cut again. */
static int entry_regular(const bpr_ctx *c, const bpr_entry *e, uint32_t bs)
{
    int v = e->vol;
    uint64_t off = e->off + e->header_size, remaining = e->original_size;
    bpr_norm_pos(c, &v, &off);
    while (remaining > 0) {
        uint8_t h[BPR_BLOCK_HDR];
        bpr_fail f = { NULL, -1 };
        uint32_t comp, orig;
        if (bpr_read_struct(c, v, off, h, sizeof h, &f, "x") != 0) return 0;
        comp = bpr_le32(h);
        orig = bpr_le32(h + 4);
        if (orig == 0 || orig > remaining) return 0;
        if (orig < bs && orig != remaining) return 0;
        remaining -= orig;
        off += BPR_BLOCK_HDR + comp;
        bpr_norm_pos(c, &v, &off);
    }
    return 1;
}

static int cmp_entries(const void *a, const void *b)
{
    const bpr_xres *x = *(const bpr_xres *const *)a, *y = *(const bpr_xres *const *)b;
    if (x->e->vol != y->e->vol) return x->e->vol < y->e->vol ? -1 : 1;
    if (x->e->off != y->e->off) return x->e->off < y->e->off ? -1 : 1;
    return 0;
}

void bpr_rebuild_info_free(bpr_rebuild_info *i)
{
    size_t k;
    for (k = 0; k < i->nfiles; k++) free(i->files[k]);
    free(i->files);
    memset(i, 0, sizeof *i);
}

int bpr_rebuild(bpr_ctx *c, bpr_xlist *x, const char *out, uint64_t volume_size,
                bpr_rebuild_info *info)
{
    bpr_xres **sel = NULL;
    size_t nsel = 0, k;
    sinkctx sk;
    bp_volume_sink_t vs;
    bp_archive_t ar;
    int rc, open = 0, method = BP_METHOD_BOLTLZ, have_method = 0;
    uint32_t block_size = BP_BLOCK_DEFAULT;
    bp_uint8 bpm;

    memset(info, 0, sizeof *info);
    memset(&sk, 0, sizeof sk);
    sk.out = out;
    sk.info = info;
    if (bpr_path_exists(out)) {
        problem(info, "%s already exists", out);
        return -1;
    }
    sel = (bpr_xres **)malloc((x->n ? x->n : 1) * sizeof *sel);
    if (sel == NULL) { problem(info, "%s", "out of memory"); return -1; }
    for (k = 0; k < x->n; k++) {
        bpr_xres *r = &x->r[k];
        const bpr_entry *e = r->e;
        uint32_t crc = 0, want = 0;
        int have_want = 0, by_table = 0;
        const char *why;
        if (r->outcome != BPR_X_CREATED) continue;
        if (e->entry_type != BP_TYPE_DIR) {
            why = bpr_decode_entry(c, e, NULL, NULL, &crc);
            have_want = bpr_expected_checksum(c, e, &want, &by_table);
            if (why == NULL && have_want && crc != want) why = "payload-checksum-mismatch";
            if (why != NULL) {
                r->outcome = BPR_X_FAILED;
                r->detail = why;
                continue;
            }
            r->outcome = !have_want ? BPR_X_UNVERIFIED
                       : by_table ? BPR_X_VERIFIED_TABLE : BPR_X_VERIFIED;
            r->crc = crc;
        }
        if (e->original_size > 0 && e->entry_type != BP_TYPE_DIR) {
            uint32_t bs = e->from_table ? bpr_infer_block_size(c, e) : 0;
            if (!e->from_table) {
                uint8_t b4[4];
                bpr_fail f = { NULL, -1 };
                if (bpr_read_struct(c, e->vol, e->off + 4 + e->name_len + 10, b4,
                                    4, &f, "x") == 0) bs = bpr_le32(b4);
            }
            r->reencoded = !entry_regular(c, e, bs);
            if (r->reencoded && !have_method) {
                /* The archive's codec and block size are what re-encoded
                 * entries get: take the first such entry's. */
                method = e->method;
                block_size = bs;
                have_method = 1;
            }
        }
        sel[nsel++] = r;
    }
    qsort(sel, nsel, sizeof *sel, cmp_entries);
    nsel = apply_group_rules(sel, nsel);
    if (nsel == (size_t)-1) { problem(info, "%s", "out of memory"); free(sel); return -1; }

    if (nsel == 0) {                /* nothing to write: leave no file behind */
        free(sel);
        return 0;
    }
    if (bp_compress_init() != BP_OK) {
        problem(info, "%s", "compression initialisation failed");
        free(sel);
        return -1;
    }
    vs.create = sink_create;
    vs.finish = sink_finish;
    vs.user = &sk;
    bpm = (bp_uint8)method;
    rc = bp_archive_create_volumes(&ar, &vs, u64c(volume_size), block_size, bpm);
    if (rc != BP_OK) {
        if (rc == BP_ERR_INVALID_ARG && volume_size != 0)
            problem(info, "%s", "--volume-size is too small (at least 4096 bytes)");
        else
            problem(info, "cannot start the archive: %s", bp_strerror(rc));
        goto fail;
    }
    open = 1;

    for (k = 0; k < nsel; k++) {
        bpr_xres *r = sel[k];
        const bpr_entry *e = r->e;
        eread rd;
        bp_io_t io;
        bp_packed_entry_t p;
        size_t hn = 0;
        uint8_t *hdr;
        uint32_t crc = e->entry_type == BP_TYPE_DIR ? 0 : r->crc;
        if (r->reencoded) {
            dread dr;
            bp_io_t dio;
            char *nm = (char *)malloc((size_t)e->name_len + 1);
            if (nm == NULL) { problem(info, "%s", "out of memory"); goto fail; }
            memcpy(nm, e->name, e->name_len);
            nm[e->name_len] = '\0';
            memset(&dr, 0, sizeof dr);
            dr.c = c;
            dr.e = e;
            dr_reset(&dr);
            dio.read = dr_read; dio.write = er_write; dio.seek = dr_seek;
            dio.tell = er_tell; dio.handle = &dr;
            rc = bp_archive_add_entry_ex(&ar, nm, e->entry_type, e->platform, &dio,
                                         u64c(e->original_size), u64c(e->mtime),
                                         e->flags);
            if (dr.have_d) bpr_dec_end(&dr.d);
            if (rc != BP_OK && info->problem[0] == '\0')
                snprintf(info->problem, sizeof info->problem,
                         "re-encoding %.100s failed: %s", nm, bp_strerror(rc));
            free(nm);
            if (rc != BP_OK) goto fail;
            info->entries++;
            continue;
        }
        hdr = entry_header(c, e, &hn);
        uint64_t comp = e->blocks_comp;
        int i;

        if (hdr == NULL) {
            r->outcome = BPR_X_FAILED;
            r->detail = "header-unreadable";
            continue;
        }
        memset(&rd, 0, sizeof rd);
        rd.c = c;
        rd.e = e;
        rd.hdr = hdr;
        rd.hdr_n = hn;
        for (i = 0; i < 8; i++) rd.trl[i] = (uint8_t)(comp >> (8 * i));
        for (i = 0; i < 4; i++) rd.trl[8 + i] = (uint8_t)(crc >> (8 * i));
        {
            uint32_t tc = bpr_crc32(0, rd.trl, 12);
            for (i = 0; i < 4; i++) rd.trl[12 + i] = (uint8_t)(tc >> (8 * i));
        }
        er_reset(&rd);
        io.read = er_read; io.write = er_write; io.seek = er_seek; io.tell = er_tell;
        io.handle = &rd;

        memset(&p, 0, sizeof p);
        p.name = (char *)malloc((size_t)e->name_len + 1);
        if (p.name == NULL) { free(hdr); problem(info, "%s", "out of memory"); goto fail; }
        memcpy(p.name, e->name, e->name_len);
        p.name[e->name_len] = '\0';
        p.name_len = e->name_len;
        p.header_size = e->header_size;
        p.original_size = u64c(e->original_size);
        p.compressed_size = u64c(comp);
        p.checksum = crc;
        p.method = e->method;
        p.checksum_type = BP_CKSUM_CRC32;
        p.block_size = bpr_le32(hdr + 4 + e->name_len + 10);
        p.entry_type = e->entry_type;
        p.platform = e->platform;
        p.mtime = u64c(e->mtime);
        p.flags = e->flags;
        p.serialized_size = u64c((uint64_t)hn + comp + BPR_ENTRY_TRL);
        rc = bp_archive_commit_packed_entry(&ar, &io, &p);
        bp_packed_entry_dispose(&p);
        free(rd.blk);
        free(hdr);
        if (rc != BP_OK) {
            if (info->problem[0] == '\0')
                snprintf(info->problem, sizeof info->problem,
                         "writing %.100s failed: %s", (const char *)e->name,
                         bp_strerror(rc));
            goto fail;
        }
        info->entries++;
    }
    rc = bp_archive_finalize(&ar);
    if (rc != BP_OK) {
        if (info->problem[0] == '\0')
            snprintf(info->problem, sizeof info->problem,
                     "finishing the archive failed: %s", bp_strerror(rc));
        goto fail;
    }
    bp_archive_close(&ar);
    free(sel);
    return 0;

fail:
    if (open) bp_archive_close(&ar);
    for (k = 0; k < info->nfiles; k++) bpr_path_remove(info->files[k]);
    {
        char keep[sizeof info->problem];
        memcpy(keep, info->problem, sizeof keep);
        bpr_rebuild_info_free(info);
        memcpy(info->problem, keep, sizeof keep);
    }
    free(sel);
    return -1;
}
