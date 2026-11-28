/*
 * bpr_extract.c -- write out the entries that can be recovered whole.
 *
 * Reconciliation.  The analysis gives entries found by content and, when
 * trusted, the file table.  An entry is extracted when:
 *
 *   - it verified (header, every block, trailer): payload checked against the
 *     trailer's CRC-32; or
 *   - its header and blocks verified but the trailer did not: the payload is
 *     checked against the trusted table's checksum if there is one, else it is
 *     written and reported complete-unverified; or
 *   - its header is gone but the trusted table lists it and the data at the
 *     listed location verifies against the record: the header is rebuilt from
 *     the record.
 *
 * Everything else is reported and left alone: suspect hits, supplementary
 * (resource fork / metadata) entries, and entries with lost blocks (partial
 * recovery is a later phase).  A file is written under its real name only if
 * every block decoded and the payload checksum matched; otherwise it is kept
 * as NAME.damaged.  Directories and parents are created first, symlinks last,
 * so a symlink in the archive can never redirect a later write.
 */
#include "bpr.h"

#include "boltlite.h"
#include "compress.h"
#include "fluff.h"

#include <stdlib.h>
#include <string.h>

const char *bpr_outcome_name(int o)
{
    switch (o) {
    case BPR_X_VERIFIED: return "verified";
    case BPR_X_VERIFIED_TABLE: return "verified-by-table";
    case BPR_X_UNVERIFIED: return "complete-unverified";
    case BPR_X_CREATED: return "created";
    case BPR_X_PARTIAL: return "partial";
    case BPR_X_SKIP_SUSPECT: return "skipped-suspect";
    case BPR_X_SKIP_SUPPLEMENTARY: return "skipped-supplementary";
    case BPR_X_SKIP_PARTIAL: return "skipped-partial";
    case BPR_X_SKIP_DUPLICATE: return "skipped-duplicate-name";
    case BPR_X_SKIP_HOST_NAME: return "skipped-host-name";
    case BPR_X_SKIP_SYMLINK: return "skipped-symlink";
    case BPR_X_NO_HEADER: return "no-header";
    default: return "failed";
    }
}

/* Walk an entry's blocks from its header, decoding each, calling sink for the
 * decoded bytes.  Returns NULL on success or a static reason. */
typedef bpr_sink_fn sink_fn;

const char *bpr_decode_entry(const bpr_ctx *c, const bpr_entry *e,
                             bpr_sink_fn sink, void *user, uint32_t *crc_out);
#define decode_entry bpr_decode_entry

const char *bpr_decode_entry(const bpr_ctx *c, const bpr_entry *e,
                                sink_fn sink, void *user, uint32_t *crc_out)
{
    bpr_decoder d;
    int v = e->vol;
    uint64_t off = e->off + e->header_size, remaining = e->original_size;
    uint32_t crc = 0;
    const char *why = NULL;
    bpr_fail f = { NULL, -1 };

    if (bpr_dec_begin(&d, e->method) != 0) {
        bpr_dec_end(&d);
        return "unsupported-method";
    }
    bpr_norm_pos(c, &v, &off);
    while (remaining > 0) {
        uint8_t h[BPR_BLOCK_HDR];
        uint32_t comp, orig;
        const bpr_vol *x;
        if (bpr_read_struct(c, v, off, h, sizeof h, &f, "block-truncated") != 0) {
            why = f.reason;
            goto done;
        }
        comp = bpr_le32(h);
        orig = bpr_le32(h + 4);
        if (comp == 0 || orig == 0 || comp > orig || orig > BPR_MAX_BLOCK ||
            orig > remaining) {
            why = "block-header";
            goto done;
        }
        x = bpr_vol_at(c, v);
        if (off + BPR_BLOCK_HDR + comp > bpr_read_limit(x)) {
            why = "block-truncated";
            goto done;
        }
        if (bpr_grow(&d.in, &d.in_cap, comp) != 0) { why = "no-memory"; goto done; }
        if (bpr_file_read(x->f, off + BPR_BLOCK_HDR, d.in, comp) != 0) {
            why = "read-error";
            goto done;
        }
        if (bpr_crc32(0, d.in, comp) != bpr_le32(h + 8)) {
            why = "block-crc";
            goto done;
        }
        why = bpr_dec_block(&d, comp, orig);
        if (why != NULL) goto done;
        crc = bpr_crc32(crc, d.out, orig);
        if (sink != NULL && sink(user, d.out, orig) != 0) {
            why = "write-error";
            goto done;
        }
        remaining -= orig;
        off += BPR_BLOCK_HDR + comp;
        bpr_norm_pos(c, &v, &off);
    }
    *crc_out = crc;
done:
    bpr_dec_end(&d);
    return why;
}

/* ---- sinks ------------------------------------------------------------ */

typedef struct { bpr_out *out; } file_sink;

static int sink_file(void *user, const uint8_t *data, uint32_t len)
{
    return bpr_out_write(((file_sink *)user)->out, data, len);
}

typedef struct { uint8_t *buf; size_t n, cap; } mem_sink;

static int sink_mem(void *user, const uint8_t *data, uint32_t len)
{
    mem_sink *m = (mem_sink *)user;
    if (m->n + len > m->cap) return -1;
    memcpy(m->buf + m->n, data, len);
    m->n += len;
    return 0;
}

/* ---- plan ------------------------------------------------------------- */

static char *dup_name(const bpr_entry *e, const char *suffix)
{
    size_t sl = suffix ? strlen(suffix) : 0;
    size_t n = e->name_len;
    char *p;
    while (n > 0 && e->name[n - 1] == '/') n--;
    p = (char *)malloc(n + sl + 2);
    if (p == NULL) return NULL;
    memcpy(p, e->name, n);
    if (suffix) memcpy(p + n, suffix, sl);
    p[n + sl] = '\0';
    return p;
}

static bpr_xres *add_res(bpr_xlist *x, const bpr_entry *e, int outcome,
                         const char *detail)
{
    bpr_xres *r;
    if (x->n == x->cap) {
        size_t cap = x->cap ? x->cap * 2 : 256;
        bpr_xres *n = (bpr_xres *)realloc(x->r, cap * sizeof(*n));
        if (n == NULL) return NULL;
        x->r = n;
        x->cap = cap;
    }
    r = &x->r[x->n++];
    memset(r, 0, sizeof(*r));
    r->e = e;
    r->outcome = outcome;
    r->detail = detail;
    return r;
}

/* Which checksum, if any, can confirm this entry's payload? */
int bpr_expected_checksum(const bpr_ctx *c, const bpr_entry *e,
                          uint32_t *want, int *by_table)
{
    if (e->status == BPR_ST_OK) {
        *want = e->checksum;
        *by_table = 0;
        return 1;
    }
    if (c->table.status == BPR_TBL_TRUSTED && e->table_index >= 0 &&
        (uint32_t)e->table_index < c->table.nrec) {
        const bpr_entry *r = &c->table.rec[e->table_index];
        if (r->vol == e->vol && r->off == e->off &&
            r->original_size == e->original_size &&
            r->comp_size == e->blocks_comp) {
            *want = r->checksum;
            *by_table = 1;
            return 1;
        }
    }
    return 0;
}

int bpr_entry_whole(const bpr_entry *e)
{
    if (e->status == BPR_ST_OK) return 1;
    if (e->covered_orig != e->original_size) return 0;
    return e->from_table ||
           (e->reason != NULL && strncmp(e->reason, "trailer-", 8) == 0);
}

/* Same rule as bp_symlink_target_is_safe in src/core/archiver.c: relative,
 * no drive prefix, no backslash, and never climbing above the extraction root. */
static int symlink_target_safe(const char *name, const char *target)
{
    int depth = 0;
    const char *p;
    if (target[0] == '\0' || target[0] == '/' || target[1] == ':' ||
        strchr(target, '\\') != NULL) return 0;
    for (p = name; *p; p++)
        if (*p == '/' && p[1] != '\0') depth++;
    p = target;
    while (*p) {
        const char *comp = p;
        unsigned len = 0;
        while (p[len] != '/' && p[len] != '\0') len++;
        if (len == 2 && comp[0] == '.' && comp[1] == '.') {
            if (--depth < 0) return 0;
        } else if (len > 0 && !(len == 1 && comp[0] == '.')) {
            depth++;
        }
        p += len;
        if (*p == '/') p++;
    }
    return 1;
}

static void keep_damaged(const char *root, char *name)
{
    char *dmg = (char *)malloc(strlen(name) + 9);
    if (dmg == NULL) { bpr_fs_unlink(root, name); return; }
    strcpy(dmg, name);
    strcat(dmg, ".damaged");
    if (bpr_fs_exists(root, dmg) || bpr_fs_rename(root, name, dmg) != 0)
        bpr_fs_unlink(root, name);
    free(dmg);
}

static void do_file(bpr_ctx *c, bpr_xlist *x, bpr_xres *r, const bpr_entry *e)
{
    char *name = dup_name(e, NULL);
    bpr_out *out;
    file_sink fs;
    uint32_t crc = 0, want = 0;
    int have_want, by_table = 0;
    const char *why;

    if (name == NULL) { r->outcome = BPR_X_FAILED; r->detail = "no-memory"; return; }
    if ((why = bpr_fs_check_name(name, 0)) != NULL) {
        r->outcome = BPR_X_SKIP_HOST_NAME;
        r->detail = why;
        free(name);
        return;
    }
    if (bpr_fs_exists(x->root, name)) {
        r->outcome = BPR_X_SKIP_DUPLICATE;
        r->detail = "a file with this name was already written";
        free(name);
        return;
    }
    if (bpr_fs_make_parents(x->root, name) != 0 ||
        (out = bpr_out_create(x->root, name)) == NULL) {
        r->outcome = BPR_X_FAILED;
        r->detail = "cannot create the output file";
        free(name);
        return;
    }
    fs.out = out;
    why = decode_entry(c, e, sink_file, &fs, &crc);
    have_want = bpr_expected_checksum(c, e, &want, &by_table);
    if (why == NULL && have_want && crc != want) why = "payload-checksum-mismatch";
    if (why != NULL) {
        bpr_out_abort(out);
        if (strcmp(why, "payload-checksum-mismatch") != 0 &&
            strcmp(why, "write-error") != 0) {
            /* Not a whole entry after all: salvage what decodes, in the last
             * pass, so a verified file never loses its name to a .damaged one. */
            bpr_fs_unlink(x->root, name);
            free(name);
            r->outcome = BPR_X_SKIP_PARTIAL;
            r->detail = why;
            return;
        }
        keep_damaged(x->root, name);
        r->outcome = BPR_X_FAILED;
        r->detail = why;
        free(name);
        return;
    }
    if (bpr_out_close(out) != 0) {
        keep_damaged(x->root, name);
        r->outcome = BPR_X_FAILED;
        r->detail = "write-error";
        free(name);
        return;
    }
    bpr_fs_restore_meta(x->root, name, e->flags, e->mtime, 0);
    bpr_fs_commit_name(name, 0);
    r->written = name;
    r->outcome = !have_want ? BPR_X_UNVERIFIED
               : by_table ? BPR_X_VERIFIED_TABLE : BPR_X_VERIFIED;
}

static void do_symlink(bpr_ctx *c, bpr_xlist *x, bpr_xres *r,
                       const bpr_entry *e)
{
    char *name = dup_name(e, NULL), *target = NULL;
    mem_sink m;
    uint32_t crc = 0, want = 0;
    int have_want, by_table = 0;
    const char *why;

    if (name == NULL) { r->outcome = BPR_X_FAILED; r->detail = "no-memory"; return; }
    if (e->original_size == 0 || e->original_size > 65535) {
        r->outcome = BPR_X_FAILED;
        r->detail = "bad-symlink-target-size";
        goto out;
    }
    m.cap = (size_t)e->original_size;
    m.n = 0;
    m.buf = (uint8_t *)malloc(m.cap + 1);
    if (m.buf == NULL) { r->outcome = BPR_X_FAILED; r->detail = "no-memory"; goto out; }
    why = decode_entry(c, e, sink_mem, &m, &crc);
    have_want = bpr_expected_checksum(c, e, &want, &by_table);
    if (why == NULL && have_want && crc != want) why = "payload-checksum-mismatch";
    if (why != NULL) {
        free(m.buf);
        r->outcome = BPR_X_FAILED;
        r->detail = why;
        goto out;
    }
    m.buf[m.n] = '\0';
    target = (char *)m.buf;
    if (strlen(target) != m.n) {
        r->outcome = BPR_X_FAILED;
        r->detail = "symlink target contains a NUL";
    } else if (!symlink_target_safe(name, target)) {
        r->outcome = BPR_X_FAILED;
        r->detail = "unsafe symlink target refused";
    } else if ((why = bpr_fs_check_name(name, 0)) != NULL) {
        r->outcome = BPR_X_SKIP_HOST_NAME;
        r->detail = why;
    } else if (bpr_fs_exists(x->root, name)) {
        r->outcome = BPR_X_SKIP_DUPLICATE;
        r->detail = "a file with this name was already written";
    } else if (bpr_fs_make_parents(x->root, name) != 0 ||
               bpr_fs_symlink(x->root, name, target) != 0) {
        r->outcome = BPR_X_FAILED;
        r->detail = "cannot create the symlink";
    } else {
        bpr_fs_restore_meta(x->root, name, e->flags, e->mtime, 1);
        bpr_fs_commit_name(name, 0);
        r->written = name;
        name = NULL;
        r->outcome = !have_want ? BPR_X_UNVERIFIED
                   : by_table ? BPR_X_VERIFIED_TABLE : BPR_X_VERIFIED;
    }
    free(target);
out:
    free(name);
}

/* Rebuild entries whose header is gone from the trusted table's record. */
static void rebuild_from_table(bpr_ctx *c, bpr_xlist *x)
{
    bpr_table *t = &c->table;
    uint32_t i;
    if (t->status != BPR_TBL_TRUSTED) return;
    x->own = (bpr_entry *)calloc(t->nrec ? t->nrec : 1, sizeof(bpr_entry));
    if (x->own == NULL) return;
    for (i = 0; i < t->nrec; i++) {
        bpr_entry *rec = &t->rec[i];
        bpr_cand k;
        bpr_entry *e;
        if (rec->status != BPR_ST_TABLE_ONLY || rec->reason == NULL ||
            strcmp(rec->reason, "no-valid-header-at-location") != 0) continue;
        if (rec->header_size != BPR_ENTRY_FIXED + rec->name_len) continue;
        memset(&k, 0, sizeof k);
        k.vol = rec->vol;
        k.off = rec->off;
        k.header_size = rec->header_size;
        k.name_len = rec->name_len;
        k.name = (uint8_t *)malloc(rec->name_len);
        if (k.name == NULL) continue;
        memcpy(k.name, rec->name, rec->name_len);
        k.original_size = rec->original_size;
        k.method = rec->method;
        k.entry_type = rec->entry_type;
        k.platform = rec->platform;
        k.flags = rec->flags;
        k.mtime = rec->mtime;
        k.block_size = BPR_MAX_BLOCK;   /* the record does not carry it */
        e = &x->own[x->nown];
        bpr_verify_entry(c, &k, -1, e, 0);
        if (bpr_entry_whole(e) && e->covered_orig == rec->original_size &&
            e->blocks_comp == rec->comp_size) {
            /* complete: handled as a whole entry below */
        } else if (rec->entry_type == BP_TYPE_DATA && rec->original_size > 0) {
            e->reason = "header-rebuilt-from-table";   /* partial salvage */
        } else {
            free(e->name);
            memset(e, 0, sizeof *e);
            continue;
        }
        e->from_table = 1;
        e->in_table = 1;
        e->table_index = (int)i;
        e->confidence = BPR_CONF_CHAINED;
        e->inside = -1;
        e->checksum = rec->checksum;    /* trusted record, not the damaged trailer */
        e->comp_size = rec->comp_size;
        e->status = BPR_ST_DAMAGED;     /* header not verified: use the table's checksum */
        if (e->reason == NULL) e->reason = "header-rebuilt-from-table";
        x->nown++;
        rec->status = -1;               /* matched */
        rec->reason = NULL;
    }
}

/* Decide what each entry is.  Whole entries get outcome CREATED here (the caller
 * verifies the payload and refines it); the rest get their skip outcome.  With
 * include_supplementary, resource-fork and metadata entries count as ordinary
 * entries (a rebuilt archive carries them; extraction cannot restore them). */
int bpr_plan(bpr_ctx *c, bpr_xlist *x, int include_supplementary)
{
    size_t k;

    memset(x, 0, sizeof *x);
    fluff_init();
    rebuild_from_table(c, x);
    for (k = 0; k < c->nent + x->nown; k++) {
        const bpr_entry *e = k < c->nent ? &c->ent[k] : &x->own[k - c->nent];
        bpr_xres *r;
        int outcome = BPR_X_CREATED;
        const char *detail = NULL;
        if (e->confidence == BPR_CONF_SUSPECT) {
            outcome = BPR_X_SKIP_SUSPECT;
            detail = e->suspect_reason;
        } else if (!include_supplementary &&
                   (e->entry_type == BP_TYPE_RSRC || e->entry_type == BP_TYPE_META)) {
            outcome = BPR_X_SKIP_SUPPLEMENTARY;
            detail = "resource fork or metadata entry: restore it from a rebuilt archive";
        } else if (!bpr_entry_whole(e)) {
            outcome = BPR_X_SKIP_PARTIAL;
            detail = e->reason;
        }
        r = add_res(x, e, outcome, detail);
        if (r == NULL) return -1;
        if (e->from_table) r->rebuilt = 1;
    }
    for (k = 0; k < c->table.nrec; k++) {
        const bpr_entry *rec = &c->table.rec[k];
        if (rec->status != BPR_ST_TABLE_ONLY) continue;
        if (add_res(x, rec, BPR_X_NO_HEADER, rec->reason) == NULL) return -1;
    }
    return 0;
}

int bpr_extract(bpr_ctx *c, const char *root, bpr_xlist *x)
{
    size_t k;
    int pass;
    size_t first_res;

    if (bpr_plan(c, x, 0) != 0) return -1;
    x->root = root;
    first_res = 0;
    for (pass = 0; pass < 4; pass++) {
        size_t i;
        for (i = first_res; i < x->n; i++) {
            bpr_xres *r = &x->r[i];
            const bpr_entry *e = r->e;
            if (r->outcome != BPR_X_CREATED &&
                !(pass == 3 && r->outcome == BPR_X_SKIP_PARTIAL &&
                  e->entry_type == BP_TYPE_DATA)) continue;
            if (pass == 0 && e->entry_type == BP_TYPE_DIR) {
                char *name = dup_name(e, "/");
                const char *why = name ? bpr_fs_check_name(name, 1) : NULL;
                if (why != NULL) {
                    r->outcome = BPR_X_SKIP_HOST_NAME;
                    r->detail = why;
                } else if (name == NULL || bpr_fs_mkdir(root, name) != 0) {
                    r->outcome = BPR_X_FAILED;
                    r->detail = "cannot create the directory";
                } else {
                    bpr_fs_commit_name(name, 1);
                    r->written = name;
                    name = NULL;
                }
                free(name);
            } else if (pass == 3 && r->outcome == BPR_X_SKIP_PARTIAL) {
                bpr_extract_partial(c, x, r, e);
            } else if (pass == 1 && e->entry_type == BP_TYPE_DATA) {
                do_file(c, x, r, e);
            } else if (pass == 2 && e->entry_type == BP_TYPE_SYMLINK) {
                if (!bpr_fs_symlinks_supported()) {
                    r->outcome = BPR_X_SKIP_SYMLINK;
                    r->detail = "symlinks are not extracted on this host";
                } else {
                    do_symlink(c, x, r, e);
                }
            }
        }
    }
    /* Directory permissions and times last, deepest first. */
    {
        size_t i = x->n;
        while (i-- > first_res) {
            bpr_xres *r = &x->r[i];
            if (r->outcome == BPR_X_CREATED && r->e->entry_type == BP_TYPE_DIR)
                bpr_fs_restore_meta(root, r->written, r->e->flags, r->e->mtime, 0);
        }
    }
    for (k = 0; k < x->n; k++) x->counts[x->r[k].outcome]++;
    return 0;
}

void bpr_xlist_free(bpr_xlist *x)
{
    size_t i;
    for (i = 0; i < x->n; i++) {
        free(x->r[i].written);
        free(x->r[i].ranges);
    }
    free(x->r);
    for (i = 0; i < x->nown; i++) free(x->own[i].name);
    free(x->own);
    memset(x, 0, sizeof *x);
}
