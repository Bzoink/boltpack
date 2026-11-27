/*
 * bpr_volumes.c -- find out which files are which volume of which set.
 *
 * A volume is identified by its header (spec 2.2), failing that by its
 * trailer (2.4, which repeats set id and number), failing that by its file
 * name (2.5).  The names are a convention only; headers and trailers win.
 */
#include "bpr.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void bpr_ivs_add(bpr_ivs *v, uint64_t s, uint64_t e, int kind, int entry)
{
    bpr_iv *iv;
    if (e <= s) return;
    if (v->n == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 64;
        bpr_iv *n = (bpr_iv *)realloc(v->a, cap * sizeof(*n));
        if (n == NULL) return;
        v->a = n;
        v->cap = cap;
    }
    if (v->n > 0 && s < v->a[v->n - 1].s) v->dirty = 1;
    iv = &v->a[v->n++];
    iv->s = s;
    iv->e = e;
    iv->kind = kind;
    iv->entry = entry;
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d != NULL) memcpy(d, s, n);
    return d;
}

int bpr_load_files(bpr_ctx *c, int n, char **paths)
{
    int i;
    c->files = (bpr_vol *)calloc((size_t)(n > 0 ? n : 1), sizeof(bpr_vol));
    if (c->files == NULL) return -1;
    c->nfiles = 0;
    for (i = 0; i < n; i++) {
        bpr_vol *v = &c->files[c->nfiles];
        v->path = dup_str(paths[i]);
        v->f = bpr_file_open(paths[i]);
        v->number = -1;
        v->role = BPR_ROLE_UNIDENTIFIED;
        if (v->f == NULL) {
            fprintf(stderr, "boltpack-recover: cannot open %s\n", paths[i]);
            free(v->path);
            memset(v, 0, sizeof(*v));
            continue;
        }
        v->size = bpr_file_size(v->f);
        c->nfiles++;
    }
    return 0;
}

/* Number from a name: ".NNN" -> NNN, ".bolt"/".blt" -> last volume. */
static void name_hint(bpr_vol *v)
{
    const char *base = strrchr(v->path, '/');
    const char *dot;
    base = base ? base + 1 : v->path;
    dot = strrchr(base, '.');
    v->name_last = 0;
    if (dot == NULL) return;
    dot++;
    if (strlen(dot) == 3 && isdigit((unsigned char)dot[0]) &&
        isdigit((unsigned char)dot[1]) && isdigit((unsigned char)dot[2])) {
        v->number = (dot[0] - '0') * 100 + (dot[1] - '0') * 10 + (dot[2] - '0');
        v->by = BPR_BY_NAME;
    } else if (strcmp(dot, "bolt") == 0 || strcmp(dot, "blt") == 0) {
        v->name_last = 1;
    }
}

static void read_identity(bpr_vol *v)
{
    uint8_t h[BPR_VOL_HDR], t[BPR_VOL_TRL];
    uint64_t hs = 0, ts = 0;
    int hn = -1, tn = -1;

    v->number = -1;
    if (v->size >= BPR_VOL_HDR && bpr_file_read(v->f, 0, h, sizeof h) == 0 &&
        bpr_le32(h) == BPR_MAGIC_VOL) {
        v->hdr_version = bpr_le16(h + 4);
        if (bpr_crc32(0, h, 28) == bpr_le32(h + 28) && v->hdr_version == 7) {
            v->hdr_ok = 1;
            hs = bpr_le64(h + 8);
            hn = bpr_le16(h + 16);
            if (hn > 999 || hs == 0) {
                v->hdr_ok = 0;
                hn = -1;
            }
        }
    }
    if (v->size >= BPR_VOL_TRL &&
        bpr_file_read(v->f, v->size - BPR_VOL_TRL, t, sizeof t) == 0 &&
        bpr_le32(t + 28) == BPR_MAGIC_TRL &&
        bpr_crc32(0, t, 24) == bpr_le32(t + 24)) {
        ts = bpr_le64(t);
        tn = bpr_le16(t + 8);
        if (tn > 999 || ts == 0) {
            tn = -1;
        } else {
            v->trl_ok = 1;
            v->trl_last = (bpr_le16(t + 10) & 1) != 0;
            v->size_ok = bpr_le64(t + 16) == v->size;
        }
    }
    if (v->hdr_ok) {
        v->set_id = hs;
        v->number = hn;
        v->by = BPR_BY_HEADER;
        if (v->trl_ok && (ts != hs || tn != hn)) v->conflict = 1;
    } else if (v->trl_ok) {
        v->set_id = ts;
        v->number = tn;
        v->by = BPR_BY_TRAILER;
    } else {
        v->by = BPR_BY_NONE;
        name_hint(v);
    }
}

static int score(const bpr_vol *v)
{
    return (v->hdr_ok ? 4 : 0) + (v->trl_ok ? 2 : 0) + (v->size_ok ? 1 : 0);
}

void bpr_identify(bpr_ctx *c)
{
    int i, best = 0;
    uint64_t chosen = 0;
    int nvotes = 0;

    c->max_number = -1;
    c->volume_count = -1;
    c->count_source = "inferred";
    for (i = 0; i < c->nfiles; i++) read_identity(&c->files[i]);

    /* Choose the set: the one given, else the one most files vouch for. */
    if (c->fixed_set_id_given) {
        chosen = c->fixed_set_id;
        c->have_set = 1;
    } else {
        for (i = 0; i < c->nfiles; i++) {
            int j, votes = 0;
            if (c->files[i].by != BPR_BY_HEADER &&
                c->files[i].by != BPR_BY_TRAILER) continue;
            for (j = 0; j < c->nfiles; j++)
                if ((c->files[j].by == BPR_BY_HEADER ||
                     c->files[j].by == BPR_BY_TRAILER) &&
                    c->files[j].set_id == c->files[i].set_id) votes++;
            if (votes > nvotes) {
                nvotes = votes;
                chosen = c->files[i].set_id;
                c->have_set = 1;
            }
        }
    }
    c->set_id = chosen;

    for (i = 0; i < c->nfiles; i++) {
        bpr_vol *v = &c->files[i];
        v->role = BPR_ROLE_UNIDENTIFIED;
        if (v->by == BPR_BY_HEADER || v->by == BPR_BY_TRAILER) {
            v->role = (c->have_set && v->set_id == c->set_id)
                          ? BPR_ROLE_USED : BPR_ROLE_STRANGER;
        } else if (v->by == BPR_BY_NAME) {
            /* Only the name identifies it: it joins the chosen set, or, when
             * nothing else identifies a set, stands alone with an unknown id. */
            v->set_id = c->set_id;
            v->role = BPR_ROLE_USED;
        }
    }

    /* One file per volume number; among claimants the best-formed one wins. */
    for (i = 0; i < BPR_MAX_VOLUMES; i++) c->slot[i] = NULL;
    for (i = 0; i < BPR_MAX_VOLUMES; i++) c->ntied[i] = 0;
    for (i = 0; i < c->nfiles; i++) {
        bpr_vol *v = &c->files[i];
        int n = v->number;
        if (v->role != BPR_ROLE_USED || n < 0) continue;
        if (c->slot[n] == NULL) {
            c->slot[n] = v;
            c->ntied[n] = 1;
        } else if (score(v) > score(c->slot[n])) {
            c->slot[n]->role = BPR_ROLE_DUPLICATE;
            c->slot[n] = v;
            c->ntied[n] = 1;
        } else if (score(v) == score(c->slot[n])) {
            /* Equally good claimants: the prefer[n]'th one given wins. */
            if (c->ntied[n]++ == c->prefer[n]) {
                c->slot[n]->role = BPR_ROLE_DUPLICATE;
                c->slot[n] = v;
            } else {
                v->role = BPR_ROLE_DUPLICATE;
            }
        } else {
            v->role = BPR_ROLE_DUPLICATE;
        }
    }
    for (i = 0; i < BPR_MAX_VOLUMES; i++)
        if (c->slot[i] != NULL) best = i + 1;
    c->max_number = best - 1;

    /* A file known only as "the last volume" takes the next free number. */
    for (i = 0; i < c->nfiles; i++) {
        bpr_vol *v = &c->files[i];
        if (v->by != BPR_BY_NONE || !v->name_last) continue;
        if (c->max_number + 1 < BPR_MAX_VOLUMES &&
            c->slot[c->max_number + 1] == NULL) {
            v->number = c->max_number + 1;
            v->by = BPR_BY_NAME;
            v->set_id = c->set_id;
            v->role = BPR_ROLE_USED;
            c->slot[v->number] = v;
            c->max_number = v->number;
            break;
        }
    }
    best = 0;
    for (i = 0; i < BPR_MAX_VOLUMES; i++)
        if (c->slot[i] != NULL) {
            best = i + 1;
            if (c->slot[i]->trl_ok && c->slot[i]->trl_last) c->last_found = 1;
        }
    c->max_number = best - 1;
}

void bpr_ctx_free(bpr_ctx *c)
{
    int i;
    size_t k;
    for (i = 0; i < c->nfiles; i++) {
        bpr_file_close(c->files[i].f);
        free(c->files[i].path);
    }
    free(c->files);
    for (k = 0; k < c->ncand; k++) free(c->cand[k].name);
    free(c->cand);
    free(c->tblt_hits);
    for (k = 0; k < c->nent; k++) free(c->ent[k].name);
    free(c->ent);
    free(c->span);
    free(c->endc);
    for (k = 0; k < c->table.nrec; k++) free(c->table.rec[k].name);
    free(c->table.rec);
    for (i = 0; i < BPR_MAX_VOLUMES; i++) {
        free(c->iv[i].a);
        free(c->blk[i].a);
    }
}
