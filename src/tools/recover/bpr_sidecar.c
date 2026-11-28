/* protect, check and repair: the .bpr recovery file.
 * Format and semantics: recovery plan sections 8, 18 and 19. */
#include "bpr_sc.h"

#include <stdlib.h>
#include <string.h>


/* ---- small helpers ------------------------------------------------------- */

static const char *base_name(const char *path)
{
    const char *p = path, *b = path;
    for (; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static char *join2(const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    char *p = (char *)malloc(a + b + 2);
    if (p == NULL) return NULL;
    memcpy(p, dir, a);
    if (a > 0 && dir[a - 1] != '/' && dir[a - 1] != '\\') p[a++] = '/';
    memcpy(p + a, name, b + 1);
    return p;
}

static void jstr(FILE *o, const char *s)
{
    fputc('"', o);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { fputc('\\', o); fputc(ch, o); }
        else if (ch < 0x20) fprintf(o, "\\u%04x", ch);
        else fputc(ch, o);
    }
    fputc('"', o);
}

/* Read the bytes of sector `local` of a volume of recorded size `size`, zero
 * padded to ss.  The file may be shorter or longer than recorded.  0 ok. */
static int read_volume_sector(bpr_file *f, uint64_t size, uint32_t ss, uint32_t local,
                              uint8_t *buf)
{
    uint64_t off = (uint64_t)local * ss, len;
    if (off >= size) return -1;
    len = size - off < ss ? size - off : ss;
    if (bpr_file_read(f, off, buf, (size_t)len) != 0) return -1;
    if (len < ss) memset(buf + len, 0, (size_t)(ss - len));
    return 0;
}

static uint32_t slot_of(const bpr_sc *sc, uint32_t idx)
{
    uint32_t lo = 0, hi = sc->nvol;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (sc->vol[mid].first <= idx) lo = mid; else hi = mid;
    }
    return lo;
}

/* ---- protect --------------------------------------------------------------- */

static int parse_permille(const char *s, unsigned *out)
{
    unsigned whole = 0, frac = 0;
    const char *p = s;
    int digits = 0;
    while (*p >= '0' && *p <= '9') { whole = whole * 10 + (unsigned)(*p - '0'); p++; digits++; if (whole > 1000) return -1; }
    if (digits == 0) return -1;
    if (*p == '.') {
        p++;
        if (*p >= '0' && *p <= '9') { frac = (unsigned)(*p - '0'); p++; }
        else return -1;
        if (*p >= '0' && *p <= '9') return -1;      /* one decimal only */
    }
    if (*p != '\0') return -1;
    *out = whole * 10 + frac;
    return (*out >= 1 && *out <= 1000) ? 0 : -1;
}

static int parse_size(const char *s, uint32_t *out)
{
    unsigned long long v = 0;
    const char *p = s;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned)(*p - '0'); p++; if (v > (1ull << 32)) return -1; }
    if (*p == 'K' || *p == 'k') { v <<= 10; p++; }
    else if (*p == 'M' || *p == 'm') { v <<= 20; p++; }
    if (*p != '\0' || v > BPR_SC_MAX_SS || v < BPR_SC_MIN_SS || v % 8 != 0) return -1;
    *out = (uint32_t)v;
    return 0;
}

static int write_all(bpr_wfile *f, uint64_t off, const void *buf, size_t n)
{
    if (bpr_wfile_seek(f, (int64_t)off, 0) != 0) return -1;
    return bpr_wfile_write(f, buf, n) == (long)n ? 0 : -1;
}

static void protect_usage(FILE *o)
{
    fputs("Usage: boltpack-recover protect [options] -r PERCENT [-o SET.bpr] VOLUME...\n"
          "\n"
          "Writes a recovery file with parity for an intact archive or volume set.\n"
          "Damage that leaves at most r bad sectors in each group, and any burst of\n"
          "damage up to the chosen percentage, can later be repaired byte for byte.\n"
          "\n"
          "  -r PERCENT         redundancy, 0.1 to 100 (for example 10)\n"
          "  -o FILE            the recovery file (default: the first volume's name\n"
          "                     with the extension replaced by .bpr); never replaced\n"
          "  --sector-size N    sector size in bytes, multiple of 8, 64 to 16M\n"
          "                     (default 64K; K and M suffixes)\n"
          "  --format text|json summary format\n", o);
}

int bpr_cmd_protect(int argc, char **argv)
{
    const char *outpath = NULL, *format = "text";
    char **files = NULL;
    char *defname = NULL;
    int nfiles = 0, i, rc = 3, json;
    unsigned permille = 0;
    uint32_t ss = BPR_SC_DEFAULT_SS, G, k, g, j;
    bpr_ctx *c = NULL;
    bpr_sc sc;
    bpr_wfile *out = NULL;
    uint8_t *sec = NULL, *par = NULL, *buf;
    size_t len;
    char err[200];
    int wrote_file = 0;

    memset(&sc, 0, sizeof sc);
    files = (char **)calloc((size_t)argc, sizeof(char *));
    if (files == NULL) return 3;
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            if (parse_permille(argv[++i], &permille) != 0) {
                fprintf(stderr, "boltpack-recover: -r wants a percentage from 0.1 to 100\n");
                goto done;
            }
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        else if (strcmp(argv[i], "--sector-size") == 0 && i + 1 < argc) {
            if (parse_size(argv[++i], &ss) != 0) {
                fprintf(stderr, "boltpack-recover: bad --sector-size\n");
                goto done;
            }
        } else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) format = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            protect_usage(stdout);
            rc = 0;
            goto done;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "boltpack-recover: unknown option %s\n", argv[i]);
            goto done;
        } else files[nfiles++] = argv[i];
    }
    if (nfiles == 0 || permille == 0) { protect_usage(stderr); goto done; }
    if (strcmp(format, "text") != 0 && strcmp(format, "json") != 0) {
        fprintf(stderr, "boltpack-recover: --format is text or json\n");
        goto done;
    }
    json = strcmp(format, "json") == 0;

    bpr_gf_init();
    c = (bpr_ctx *)calloc(1, sizeof *c);
    if (c == NULL) goto done;
    if (bpr_load_files(c, nfiles, files) != 0 || c->nfiles == 0) {
        fprintf(stderr, "boltpack-recover: cannot read the volume files\n");
        goto done;
    }
    bpr_identify(c);
    if (bpr_analyze(c) != 0) {
        fprintf(stderr, "boltpack-recover: cannot analyse the volume files\n");
        goto done;
    }
    if (!c->complete || c->volume_count < 1) {
        fprintf(stderr, "boltpack-recover: protect needs a complete set to know the volume order; "
                        "this one is not (%d volume(s) missing). Protect the archive while it is intact.\n",
                c->nmissing);
        goto done;
    }
    for (i = 0; i < c->nfiles; i++)
        if (c->files[i].role != BPR_ROLE_USED) {
            fprintf(stderr, "boltpack-recover: %s is not part of the set (stranger or duplicate volume)\n",
                    c->files[i].path);
            goto done;
        }

    {
        size_t e;
        unsigned okc = 0;
        for (e = 0; e < c->nent; e++) if (c->ent[e].status == BPR_ST_OK) okc++;
        if (c->table.status != BPR_TBL_TRUSTED || okc != c->nent)
            fprintf(stderr, "boltpack-recover: warning: this set is already damaged (%u of %u entries verify, "
                            "table %s); the recovery file protects it as it is\n", okc, (unsigned)c->nent,
                    c->table.status == BPR_TBL_TRUSTED ? "trusted" : "not trusted");
    }
    sc.nvol = (uint32_t)c->volume_count;
    sc.ss = ss;
    sc.set_id = c->have_set ? c->set_id : 0;
    sc.vol = (bpr_sc_vol *)calloc(sc.nvol, sizeof *sc.vol);
    if (sc.vol == NULL) goto done;
    {
        uint64_t total = 0;
        for (k = 0; k < sc.nvol; k++) {
            const bpr_vol *v = c->slot[k];
            const char *bn;
            if (v == NULL) { fprintf(stderr, "boltpack-recover: volume %u is missing\n", k); goto done; }
            bn = base_name(v->path);
            if (!bpr_sc_valid_name(bn, strlen(bn))) {
                fprintf(stderr, "boltpack-recover: the file name %s cannot be recorded\n", bn);
                goto done;
            }
            strcpy(sc.vol[k].name, bn);
            sc.vol[k].size = v->size;
            total += (v->size + ss - 1) / ss;
            if (total > BPR_SC_MAX_SECTORS) {
                fprintf(stderr, "boltpack-recover: too many sectors; use a larger --sector-size\n");
                goto done;
            }
        }
        sc.S = (uint32_t)total;
    }
    if (sc.S == 0) { fprintf(stderr, "boltpack-recover: nothing to protect\n"); goto done; }
    bpr_sc_params(sc.S, permille, &sc.ngroups, &G, &sc.r);
    sc.nparity = sc.ngroups * sc.r;
    if (bpr_sc_geometry(&sc, err, sizeof err) != 0 || bpr_sc_layout(&sc) != 0) {
        fprintf(stderr, "boltpack-recover: cannot protect this set with these numbers (%s)\n", err);
        goto done;
    }
    if ((uint64_t)sc.r * ss > BPR_SC_MAX_PARITY_MEM) {
        fprintf(stderr, "boltpack-recover: --sector-size is too large for this redundancy\n");
        goto done;
    }
    if (outpath == NULL) {
        const char *first = c->slot[0]->path, *dot;
        size_t n = strlen(first);
        defname = (char *)malloc(n + 6);
        if (defname == NULL) goto done;
        memcpy(defname, first, n + 1);
        dot = strrchr(base_name(defname), '.');
        if (dot != NULL && dot != base_name(defname)) n = (size_t)(dot - defname);
        memcpy(defname + n, ".bpr", 5);
        outpath = defname;
    }
    sc.tab = (uint8_t *)calloc((size_t)sc.S + sc.nparity, 12);
    sec = (uint8_t *)malloc(ss);
    par = (uint8_t *)malloc((size_t)sc.r * ss);
    if (sc.tab == NULL || sec == NULL || par == NULL) goto done;

    out = bpr_wfile_create(outpath);
    if (out == NULL) {
        fprintf(stderr, "boltpack-recover: cannot create %s (it must not exist)\n", outpath);
        goto done;
    }
    wrote_file = 1;
    for (g = 0; g < sc.ngroups; g++) {
        uint32_t gs = bpr_sc_group_size(sc.S, sc.ngroups, g);
        memset(par, 0, (size_t)sc.r * ss);
        for (k = 0; k < gs; k++) {
            uint32_t idx = g + k * sc.ngroups, s = slot_of(&sc, idx);
            if (read_volume_sector(c->slot[s]->f, sc.vol[s].size, ss, idx - sc.vol[s].first, sec) != 0) {
                fprintf(stderr, "boltpack-recover: cannot read %s\n", c->slot[s]->path);
                goto done;
            }
            bpr_sc_set_entry(&sc, idx, sec);
            for (j = 0; j < sc.r; j++)
                bpr_gf_muladd(par + (size_t)j * ss, sec, ss, bpr_gf_cauchy(sc.r, j, k));
        }
        for (j = 0; j < sc.r; j++) {
            uint32_t p = g * sc.r + j;
            bpr_sc_set_entry(&sc, sc.S + p, par + (size_t)j * ss);
            if (write_all(out, sc.p_off + (uint64_t)p * ss, par + (size_t)j * ss, ss) != 0) {
                fprintf(stderr, "boltpack-recover: write error on %s\n", outpath);
                goto done;
            }
        }
    }
    buf = bpr_sc_header_bytes(&sc, &len);
    if (buf == NULL || len != sc.hdr_len ||
        write_all(out, 0, buf, len) != 0 || write_all(out, sc.h2_off, buf, len) != 0) {
        free(buf);
        fprintf(stderr, "boltpack-recover: write error on %s\n", outpath);
        goto done;
    }
    free(buf);
    buf = bpr_sc_table_bytes(&sc, &len);
    if (buf == NULL || len != sc.table_len ||
        write_all(out, sc.t_off, buf, len) != 0 || write_all(out, sc.t2_off, buf, len) != 0) {
        free(buf);
        fprintf(stderr, "boltpack-recover: write error on %s\n", outpath);
        goto done;
    }
    free(buf);
    {
        uint8_t ft[BPR_SC_FOOTER];
        bpr_sc_footer_bytes(&sc, ft);
        if (write_all(out, sc.t2_off + sc.table_len, ft, sizeof ft) != 0) {
            fprintf(stderr, "boltpack-recover: write error on %s\n", outpath);
            goto done;
        }
    }
    if (bpr_wfile_close(out) != 0) {
        out = NULL;
        fprintf(stderr, "boltpack-recover: write error on %s\n", outpath);
        goto done;
    }
    out = NULL;
    {   /* read it back: structure and size */
        bpr_sc back;
        int ok = bpr_sc_load(&back, outpath, err, sizeof err) == 0 &&
                 back.file_size == sc.t2_off + sc.table_len + BPR_SC_FOOTER &&
                 back.S == sc.S && back.r == sc.r;
        if (ok) {
            uint32_t u;
            for (u = 0; u < back.nchunks; u++) if (back.chunk[u] != 1) ok = 0;
        }
        if (ok) bpr_sc_free(&back);
        if (!ok) {
            fprintf(stderr, "boltpack-recover: the recovery file did not read back correctly\n");
            goto done;
        }
    }
    if (json) {
        printf("{\"protect\":{\"sidecar\":"); jstr(stdout, outpath);
        printf(",\"volumes\":%u,\"sector_size\":%u,\"sectors\":%u,\"groups\":%u,\"parity_per_group\":%u,"
               "\"parity_sectors\":%u,\"max_group\":%u,\"sidecar_bytes\":%llu,\"kernel\":\"%s\"}}\n",
               sc.nvol, sc.ss, sc.S, sc.ngroups, sc.r, sc.nparity, G,
               (unsigned long long)(sc.t2_off + sc.table_len + BPR_SC_FOOTER), bpr_gf_kernel_name());
    } else {
        printf("Wrote %s: %u volume(s), %u sectors of %u bytes in %u group(s) of up to %u,\n"
               "%u parity sectors per group (%u in all), %llu bytes. Kernel: %s.\n",
               outpath, sc.nvol, sc.S, sc.ss, sc.ngroups, G, sc.r, sc.nparity,
               (unsigned long long)(sc.t2_off + sc.table_len + BPR_SC_FOOTER), bpr_gf_kernel_name());
    }
    rc = 0;
    wrote_file = 0;
done:
    if (out != NULL) bpr_wfile_close(out);
    if (wrote_file && outpath != NULL) bpr_path_remove(outpath);
    free(sec);
    free(par);
    free(defname);
    free(files);
    if (c != NULL) { bpr_ctx_free(c); free(c); }
    bpr_sc_free(&sc);
    return rc;
}

/* ---- scanning a set against a sidecar --------------------------------------- */

typedef struct {
    const char *path;
    bpr_file   *f;
    uint64_t    size;
    int         slot;           /* matched slot, -1 stranger */
    const char *why;            /* for a stranger */
    int         claimed;        /* lost the contest for its slot */
} infile;

typedef struct {
    bpr_sc      sc;
    infile     *in;
    int         nin;
    int        *slot_file;      /* per slot: index into in[], -1 none */
    uint8_t    *bad;            /* per data sector: 0 good, 1 bad, 2 unknown */
    uint8_t    *pbad;           /* per parity sector: 0 good, 1 bad, 2 unknown */
    uint32_t   *gbad, *gpbad;   /* per group: bad data, unusable parity */
    uint32_t    nbad, nunknown, npbad, ngroup_dmg, ngroup_fail, ntrail;
    int         worst_margin;
    uint64_t   *trailing;       /* per slot: bytes beyond the recorded size */
    int         ambiguous;
    int         structure_only;     /* check without volume files */
    char        amb_msg[300];
} scan_t;

static int sample_agree(const scan_t *s, infile *f, uint32_t slot, uint8_t *buf)
{
    const bpr_sc *sc = &s->sc;
    uint32_t n = sc->vol[slot].nsec, take = n < 8 ? n : 8, k, agree = 0;
    for (k = 0; k < take; k++) {
        uint32_t local = take > 1 ? (uint32_t)((uint64_t)k * (n - 1) / (take - 1)) : 0;
        if (read_volume_sector(f->f, sc->vol[slot].size, sc->ss, local, buf) == 0 &&
            bpr_sc_check(sc, sc->vol[slot].first + local, buf) == 1)
            agree++;
    }
    return (int)agree;
}

static uint32_t full_agree(const scan_t *s, infile *f, uint32_t slot, uint8_t *buf)
{
    const bpr_sc *sc = &s->sc;
    uint32_t i, agree = 0;
    for (i = 0; i < sc->vol[slot].nsec; i++)
        if (read_volume_sector(f->f, sc->vol[slot].size, sc->ss, i, buf) == 0 &&
            bpr_sc_check(sc, sc->vol[slot].first + i, buf) == 1)
            agree++;
    return agree;
}

static int match_files(scan_t *s, uint8_t *buf)
{
    const bpr_sc *sc = &s->sc;
    int i;
    uint32_t v;
    for (i = 0; i < s->nin; i++) {
        infile *f = &s->in[i];
        int best = -1, bestscore = 0, tie = 0, any_size = 0;
        const char *bn = base_name(f->path);
        if (f->f == NULL) continue;
        f->slot = -1;
        f->why = "does not match any volume of the set";
        for (v = 0; v < sc->nvol; v++) if (sc->vol[v].size == f->size) any_size = 1;
        for (v = 0; v < sc->nvol; v++) {
            int agree, score;
            if (any_size && sc->vol[v].size != f->size) continue;
            agree = sample_agree(s, f, v, buf);
            if (agree == 0) continue;              /* a name alone is not agreement */
            score = agree * 4 + (strcmp(sc->vol[v].name, bn) == 0 ? 1 : 0);
            if (score > bestscore) { best = (int)v; bestscore = score; tie = 0; }
            else if (score == bestscore) tie = 1;
        }
        if (best >= 0 && tie) {
            s->ambiguous = 1;
            snprintf(s->amb_msg, sizeof s->amb_msg,
                     "%s matches more than one volume of the set equally well", f->path);
            return -1;
        }
        if (best < 0) {
            /* no checksum agreement at all: a free slot of the same name */
            for (v = 0; v < sc->nvol; v++)
                if (strcmp(sc->vol[v].name, bn) == 0 && s->slot_file[v] < 0) { best = (int)v; break; }
        }
        if (best < 0) continue;
        if (s->slot_file[best] >= 0) {
            infile *o = &s->in[s->slot_file[best]];
            uint32_t a = full_agree(s, o, (uint32_t)best, buf), b = full_agree(s, f, (uint32_t)best, buf);
            if (a == b) {
                s->ambiguous = 1;
                snprintf(s->amb_msg, sizeof s->amb_msg,
                         "%s and %s both look like volume %d and agree equally", o->path, f->path, best);
                return -1;
            }
            if (b > a) {
                o->slot = -1;
                o->why = "a better copy of the same volume was given";
                s->slot_file[best] = i;
                f->slot = best;
            } else {
                f->why = "a better copy of the same volume was given";
            }
        } else {
            s->slot_file[best] = i;
            f->slot = best;
        }
    }
    return 0;
}

static void free_scan(scan_t *s)
{
    int i;
    for (i = 0; i < s->nin; i++) if (s->in[i].f != NULL) bpr_file_close(s->in[i].f);
    free(s->in); free(s->slot_file); free(s->bad); free(s->pbad);
    free(s->gbad); free(s->gpbad); free(s->trailing);
    bpr_sc_free(&s->sc);
    memset(s, 0, sizeof *s);
}

/* Load the sidecar, open and match the files, and classify every sector. */
static int do_scan(scan_t *s, const char *scpath, int nfiles, char **files, char *err, size_t errn)
{
    const bpr_sc *sc;
    uint8_t *buf = NULL;
    uint32_t v, g, i, p;
    int k;
    memset(s, 0, sizeof *s);
    if (bpr_sc_load(&s->sc, scpath, err, errn) != 0) return -1;
    sc = &s->sc;
    s->in = (infile *)calloc((size_t)nfiles + 1, sizeof *s->in);
    s->slot_file = (int *)malloc(sc->nvol * sizeof(int));
    s->bad = (uint8_t *)calloc(sc->S, 1);
    s->pbad = (uint8_t *)calloc(sc->nparity, 1);
    s->gbad = (uint32_t *)calloc(sc->ngroups, sizeof(uint32_t));
    s->gpbad = (uint32_t *)calloc(sc->ngroups, sizeof(uint32_t));
    s->trailing = (uint64_t *)calloc(sc->nvol, sizeof(uint64_t));
    buf = (uint8_t *)malloc(sc->ss);
    if (!s->in || !s->slot_file || !s->bad || !s->pbad || !s->gbad || !s->gpbad || !s->trailing || !buf) {
        snprintf(err, errn, "out of memory");
        free(buf);
        return -1;
    }
    for (v = 0; v < sc->nvol; v++) s->slot_file[v] = -1;
    for (k = 0; k < nfiles; k++) {
        infile *f = &s->in[s->nin];
        f->path = files[k];
        f->slot = -1;
        f->f = bpr_file_open(files[k]);
        if (f->f == NULL) { f->why = "cannot be read"; s->nin++; continue; }
        f->size = bpr_file_size(f->f);
        s->nin++;
    }
    if (match_files(s, buf) != 0) {
        snprintf(err, errn, "%s", s->amb_msg);
        free(buf);
        return -2;
    }
    /* data sectors */
    for (v = 0; v < sc->nvol; v++) {
        int fi = s->slot_file[v];
        infile *f = fi >= 0 ? &s->in[fi] : NULL;
        if (f != NULL && f->size > sc->vol[v].size) { s->trailing[v] = f->size - sc->vol[v].size; s->ntrail++; }
        for (i = 0; i < sc->vol[v].nsec; i++) {
            uint32_t idx = sc->vol[v].first + i;
            int res = 0;
            if (f != NULL && read_volume_sector(f->f, sc->vol[v].size, sc->ss, i, buf) == 0)
                res = bpr_sc_check(sc, idx, buf);
            if (res == 1) continue;
            s->bad[idx] = res == -1 && f != NULL ? 2 : 1;
            if (s->bad[idx] == 2) s->nunknown++;
            s->nbad++;
            s->gbad[idx % sc->ngroups]++;
        }
    }
    /* parity sectors */
    for (p = 0; p < sc->nparity; p++) {
        int res = -1;
        if (bpr_sc_read_parity(sc, p, buf) == 0) res = bpr_sc_check(sc, sc->S + p, buf);
        else if (bpr_sc_known(sc, sc->S + p)) res = 0;
        if (res == 1) continue;
        s->pbad[p] = res == -1 ? 2 : 1;
        s->npbad++;
        s->gpbad[p / sc->r]++;
    }
    s->worst_margin = 1 << 30;
    for (g = 0; g < sc->ngroups; g++) {
        int margin = (int)sc->r - (int)s->gpbad[g] - (int)s->gbad[g];
        if (s->gbad[g] > 0) {
            s->ngroup_dmg++;
            if (margin < 0) s->ngroup_fail++;
            if (margin < s->worst_margin) s->worst_margin = margin;
        }
    }
    free(buf);
    return 0;
}

/* ---- reporting ---------------------------------------------------------------- */

static void print_scan(FILE *o, int json, const scan_t *s, const char *scpath, const char *verdict,
                       const char *extra_json)
{
    const bpr_sc *sc = &s->sc;
    uint32_t v, i, g, c1 = 0, c2 = 0, c0 = 0;
    int first;
    for (i = 0; i < sc->nchunks; i++) { if (sc->chunk[i] == 1) c1++; else if (sc->chunk[i] == 2) c2++; else c0++; }
    if (!json) {
        fprintf(o, "Recovery file %s: %u volume(s), %u sectors of %u bytes, %u group(s), %u parity per group\n",
                scpath, sc->nvol, sc->S, sc->ss, sc->ngroups, sc->r);
        fprintf(o, "  header: %s; table chunks: %u primary, %u from the copy, %u unreadable\n",
                sc->hdr_from_copy ? "from the copy" : "primary", c1, c2, c0);
        if (s->structure_only) {
            fprintf(o, "  parity sectors that do not verify: %u\n%s\n", s->npbad, verdict);
            return;
        }
        for (v = 0; v < sc->nvol; v++) {
            int fi = s->slot_file ? s->slot_file[v] : -1;
            uint32_t bad = 0;
            for (i = 0; i < sc->vol[v].nsec; i++) if (s->bad[sc->vol[v].first + i]) bad++;
            if (fi < 0) fprintf(o, "  volume %u %s: not given (lost), %u sectors to rebuild\n", v, sc->vol[v].name, sc->vol[v].nsec);
            else fprintf(o, "  volume %u %s: %s, %u of %u sectors bad%s\n", v, sc->vol[v].name, s->in[fi].path, bad,
                         sc->vol[v].nsec, s->trailing[v] ? " (bytes beyond the recorded size)" : "");
        }
        for (i = 0; i < (uint32_t)s->nin; i++)
            if (s->in[i].slot < 0) fprintf(o, "  ignored %s: %s\n", s->in[i].path, s->in[i].why ? s->in[i].why : "stranger");
        fprintf(o, "  bad data sectors: %u (%u in unreadable table chunks); unusable parity sectors: %u\n",
                s->nbad, s->nunknown, s->npbad);
        fprintf(o, "  damaged groups: %u, unrepairable: %u", s->ngroup_dmg, s->ngroup_fail);
        if (s->ngroup_dmg) fprintf(o, ", worst margin %d", s->worst_margin);
        fprintf(o, "\n");
        if (s->ngroup_fail) {
            uint32_t shown = 0;
            for (g = 0; g < sc->ngroups && shown < 20; g++) {
                int margin = (int)sc->r - (int)s->gpbad[g] - (int)s->gbad[g];
                if (s->gbad[g] > 0 && margin < 0) {
                    fprintf(o, "    group %u: %u bad data + %u bad parity sectors, budget %u\n", g, s->gbad[g],
                            s->gpbad[g], sc->r);
                    shown++;
                }
            }
        }
        fprintf(o, "%s\n", verdict);
        return;
    }
    fprintf(o, "{\"sidecar\":{\"path\":"); jstr(o, scpath);
    fprintf(o, ",\"header\":\"%s\",\"set_id\":\"%016llx\",\"volumes\":%u,\"sector_size\":%u,\"sectors\":%u,"
               "\"groups\":%u,\"parity_per_group\":%u,\"parity_sectors\":%u,\"chunks\":{\"primary\":%u,"
               "\"copy\":%u,\"unreadable\":%u},\"kernel\":\"%s\"},\"volumes\":[",
            sc->hdr_from_copy ? "copy" : "primary", (unsigned long long)sc->set_id, sc->nvol, sc->ss,
            sc->S, sc->ngroups, sc->r, sc->nparity, c1, c2, c0, bpr_gf_kernel_name());
    for (v = 0; v < sc->nvol; v++) {
        int fi = s->slot_file[v];
        fprintf(o, "%s{\"slot\":%u,\"name\":", v ? "," : "", v); jstr(o, sc->vol[v].name);
        fprintf(o, ",\"size\":%llu,\"sectors\":%u,\"file\":", (unsigned long long)sc->vol[v].size, sc->vol[v].nsec);
        if (fi < 0) fprintf(o, "null"); else jstr(o, s->in[fi].path);
        fprintf(o, ",\"trailing_bytes\":%llu,\"bad\":[", (unsigned long long)s->trailing[v]);
        first = 1;
        for (i = 0; i < sc->vol[v].nsec; i++)
            if (s->bad[sc->vol[v].first + i]) { fprintf(o, "%s%u", first ? "" : ",", i); first = 0; }
        fprintf(o, "]}");
    }
    fprintf(o, "],\"ignored\":[");
    first = 1;
    for (i = 0; i < (uint32_t)s->nin; i++)
        if (s->in[i].slot < 0) {
            fprintf(o, "%s{\"file\":", first ? "" : ","); jstr(o, s->in[i].path);
            fprintf(o, ",\"why\":"); jstr(o, s->in[i].why ? s->in[i].why : "stranger"); fprintf(o, "}");
            first = 0;
        }
    fprintf(o, "],\"damage\":{\"bad_data_sectors\":%u,\"unknown_data_sectors\":%u,\"bad_parity_sectors\":%u,"
               "\"damaged_groups\":%u,\"unrepairable_groups\":%u,\"worst_margin\":%d,\"unrepairable\":[",
            s->nbad, s->nunknown, s->npbad, s->ngroup_dmg, s->ngroup_fail,
            s->ngroup_dmg ? s->worst_margin : 0);
    first = 1;
    for (g = 0; g < sc->ngroups; g++) {
        int margin = (int)sc->r - (int)s->gpbad[g] - (int)s->gbad[g];
        if (s->gbad[g] > 0 && margin < 0) {
            fprintf(o, "%s{\"group\":%u,\"bad_data\":%u,\"bad_parity\":%u,\"budget\":%u}", first ? "" : ",",
                    g, s->gbad[g], s->gpbad[g], sc->r);
            first = 0;
        }
    }
    fprintf(o, "]},\"verdict\":"); jstr(o, verdict);
    if (extra_json != NULL) fprintf(o, ",%s", extra_json);
    fprintf(o, "}\n");
}

/* ---- check -------------------------------------------------------------------- */

static void check_usage(FILE *o)
{
    fputs("Usage: boltpack-recover check [--format text|json] SET.bpr [VOLUME...]\n"
          "\n"
          "Verifies the recovery file and, if volume files are given, compares them\n"
          "with it: which sectors are bad, how many each group lost, and whether the\n"
          "damage can be repaired.  Changes nothing.  Exit status: 0 nothing wrong,\n"
          "1 damage that can be repaired, 2 damage beyond repair, 3 error.\n", o);
}

int bpr_cmd_check(int argc, char **argv)
{
    const char *format = "text", *report = NULL, *scpath = NULL;
    char **files = (char **)calloc((size_t)argc, sizeof(char *));
    int nfiles = 0, i, rc = 3, json;
    scan_t s;
    char err[300];
    FILE *o;
    const char *verdict;

    if (files == NULL) return 3;
    memset(&s, 0, sizeof s);
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) format = argv[++i];
        else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) report = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { check_usage(stdout); free(files); return 0; }
        else if (argv[i][0] == '-' && argv[i][1] != '\0') { fprintf(stderr, "boltpack-recover: unknown option %s\n", argv[i]); free(files); return 3; }
        else if (scpath == NULL) scpath = argv[i];
        else files[nfiles++] = argv[i];
    }
    if (scpath == NULL || (strcmp(format, "text") != 0 && strcmp(format, "json") != 0)) {
        check_usage(stderr);
        free(files);
        return 3;
    }
    json = strcmp(format, "json") == 0;
    bpr_gf_init();
    {
        int r = do_scan(&s, scpath, nfiles, files, err, sizeof err);
        if (r != 0) {
            fprintf(stderr, "boltpack-recover: %s\n", err);
            free_scan(&s);
            free(files);
            return 3;
        }
    }
    if (nfiles == 0) {
        /* the sidecar alone: structure only; every data sector is simply not examined */
        s.structure_only = 1;
        memset(s.bad, 0, s.sc.S);
        s.nbad = 0; s.nunknown = 0; s.ngroup_dmg = 0; s.ngroup_fail = 0;
        memset(s.gbad, 0, s.sc.ngroups * sizeof(uint32_t));
        verdict = s.npbad ? "The recovery file itself is damaged (some parity sectors do not verify)."
                          : "The recovery file is intact. Give the volume files to compare them with it.";
        rc = s.npbad ? 1 : 0;
        {   /* slots are all 'not given' here: show the structure only */
            uint32_t v;
            for (v = 0; v < s.sc.nvol; v++) s.slot_file[v] = -1;
        }
    } else if (s.ngroup_fail) {
        verdict = "Damage is beyond what the recovery file can repair; use extract or rebuild.";
        rc = 2;
    } else if (s.nbad || s.npbad || s.ntrail) {
        verdict = "Damage found and the recovery file can repair it.";
        rc = 1;
    } else {
        verdict = "Every sector verifies. Nothing to repair.";
        rc = 0;
    }
    o = report ? bpr_fopen_write(report) : stdout;
    if (o == NULL) { fprintf(stderr, "boltpack-recover: cannot write %s\n", report); rc = 3; }
    else {
        print_scan(o, json, &s, scpath, verdict, NULL);
        if (o != stdout) fclose(o);
    }
    free_scan(&s);
    free(files);
    return rc;
}

/* ---- repair ---------------------------------------------------------------------- */

static void repair_usage(FILE *o)
{
    fputs("Usage: boltpack-recover repair [options] -o DIR SET.bpr VOLUME...\n"
          "\n"
          "Rebuilds damaged or missing volume files from the recovery file and writes\n"
          "repaired copies into DIR, which must not exist or be empty.  Nothing is\n"
          "written unless every damaged group can be repaired.  The originals are\n"
          "never changed.  Exit status: 0 repaired (or nothing to repair), 1 repaired\n"
          "but the archive is itself damaged, 2 beyond repair, 3 error.\n"
          "\n"
          "  -o DIR             directory for the repaired volumes\n"
          "  --format text|json report format\n"
          "  --report FILE      write the report to FILE\n", o);
}

typedef struct {
    int      written_ok;
    uint32_t repaired, unverified;
    int      archive_checked, archive_complete, archive_table, archive_ok_entries, archive_entries;
} repair_t;

/* Decode one group; sectors go to the spool at ordinal * ss. */
static int decode_group(scan_t *s, uint32_t g, const uint32_t *badlist, uint32_t nbadlist,
                        bpr_wfile *spool, uint8_t *work, char *err, size_t errn,
                        uint32_t *unverified)
{
    const bpr_sc *sc = &s->sc;
    uint32_t gs = bpr_sc_group_size(sc->S, sc->ngroups, g), k, e = 0, a, b;
    uint32_t miss[256], rows[256], nrows = 0, j;
    uint8_t *tmp = work, *sec = work + (size_t)sc->r * sc->ss, *res = sec + sc->ss;
    uint8_t *mat = NULL, *inv = NULL;
    for (k = 0; k < gs; k++) if (s->bad[g + k * sc->ngroups]) miss[e++] = k;
    if (e == 0) return 0;
    for (j = 0; j < sc->r && nrows < e; j++)
        if (s->pbad[g * sc->r + j] == 0) rows[nrows++] = j;
    if (nrows < e) { snprintf(err, errn, "internal: group %u has too few good parity sectors", g); return -1; }
    mat = (uint8_t *)malloc((size_t)e * e);
    inv = (uint8_t *)malloc((size_t)e * e);
    if (mat == NULL || inv == NULL) { free(mat); free(inv); snprintf(err, errn, "out of memory"); return -1; }
    for (a = 0; a < e; a++) {
        if (bpr_sc_read_parity(sc, g * sc->r + rows[a], tmp + (size_t)a * sc->ss) != 0) {
            snprintf(err, errn, "cannot read parity sector");
            free(mat); free(inv);
            return -1;
        }
    }
    for (k = 0; k < gs; k++) {
        uint32_t idx = g + k * sc->ngroups, v;
        if (s->bad[idx]) continue;
        v = slot_of(sc, idx);
        if (read_volume_sector(s->in[s->slot_file[v]].f, sc->vol[v].size, sc->ss, idx - sc->vol[v].first, sec) != 0) {
            snprintf(err, errn, "a sector that verified earlier cannot be read again");
            free(mat); free(inv);
            return -1;
        }
        for (a = 0; a < e; a++)
            bpr_gf_muladd(tmp + (size_t)a * sc->ss, sec, sc->ss, bpr_gf_cauchy(sc->r, rows[a], k));
    }
    for (a = 0; a < e; a++)
        for (b = 0; b < e; b++) mat[a * e + b] = bpr_gf_cauchy(sc->r, rows[a], miss[b]);
    if (bpr_gf_invert(mat, inv, e) != 0) {
        snprintf(err, errn, "internal: singular matrix in group %u", g);
        free(mat); free(inv);
        return -1;
    }
    for (b = 0; b < e; b++) {
        uint32_t idx = g + miss[b] * sc->ngroups, lo = 0, hi = nbadlist;
        int chk;
        memset(res, 0, sc->ss);
        for (a = 0; a < e; a++) bpr_gf_muladd(res, tmp + (size_t)a * sc->ss, sc->ss, inv[b * e + a]);
        chk = bpr_sc_check(sc, idx, res);
        if (chk == 0) {
            snprintf(err, errn, "internal: repaired sector %u does not verify", idx);
            free(mat); free(inv);
            return -1;
        }
        if (chk < 0) (*unverified)++;
        while (lo < hi) {       /* ordinal of idx in the sorted list */
            uint32_t mid = (lo + hi) / 2;
            if (badlist[mid] < idx) lo = mid + 1; else hi = mid;
        }
        if (write_all(spool, (uint64_t)lo * sc->ss, res, sc->ss) != 0) {
            snprintf(err, errn, "cannot write the scratch file");
            free(mat); free(inv);
            return -1;
        }
    }
    free(mat);
    free(inv);
    return 0;
}

int bpr_cmd_repair(int argc, char **argv)
{
    const char *format = "text", *report = NULL, *scpath = NULL, *outdir = NULL;
    char **files = (char **)calloc((size_t)argc, sizeof(char *));
    int nfiles = 0, i, rc = 3, json;
    scan_t s;
    char err[300];
    const bpr_sc *sc;
    repair_t rp;
    char *spool_path = NULL;
    bpr_wfile *spool = NULL;
    uint32_t *badlist = NULL, nbadlist = 0, v, k, g;
    uint8_t *work = NULL, *buf = NULL;
    char **created = NULL;
    uint32_t ncreated = 0;
    const char *verdict = "";
    FILE *o;

    if (files == NULL) return 3;
    memset(&s, 0, sizeof s);
    memset(&rp, 0, sizeof rp);
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) format = argv[++i];
        else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) report = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outdir = argv[++i];
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) { repair_usage(stdout); free(files); return 0; }
        else if (argv[i][0] == '-' && argv[i][1] != '\0') { fprintf(stderr, "boltpack-recover: unknown option %s\n", argv[i]); free(files); return 3; }
        else if (scpath == NULL) scpath = argv[i];
        else files[nfiles++] = argv[i];
    }
    if (scpath == NULL || outdir == NULL || (strcmp(format, "text") != 0 && strcmp(format, "json") != 0)) {
        repair_usage(stderr);
        free(files);
        return 3;
    }
    json = strcmp(format, "json") == 0;
    bpr_gf_init();
    {
        int r = do_scan(&s, scpath, nfiles, files, err, sizeof err);
        if (r != 0) {
            fprintf(stderr, "boltpack-recover: %s\n", err);
            free_scan(&s);
            free(files);
            return 3;
        }
    }
    sc = &s.sc;
    if (s.ngroup_fail) {
        verdict = "Damage is beyond what the recovery file can repair; nothing was written. Use extract or rebuild.";
        rc = 2;
        goto report;
    }
    if (s.nbad == 0 && s.ntrail == 0) {
        verdict = "Every sector verifies. Nothing to repair, nothing written.";
        rc = 0;
        goto report;
    }
    /* host rules for the names the repaired files will get */
    for (v = 0; v < sc->nvol; v++) {
        const char *why = bpr_fs_check_name(sc->vol[v].name, 0);
        if (why != NULL) {
            fprintf(stderr, "boltpack-recover: the volume name %s cannot be written here (%s)\n", sc->vol[v].name, why);
            goto fail;
        }
        bpr_fs_commit_name(sc->vol[v].name, 0);
    }
    if (bpr_fs_prepare_root(outdir) != 0) {
        fprintf(stderr, "boltpack-recover: %s must not exist or must be an empty directory\n", outdir);
        goto fail;
    }
    /* sorted list of bad data sectors, scratch file */
    badlist = (uint32_t *)malloc((size_t)(s.nbad + 1) * sizeof(uint32_t));
    work = (uint8_t *)malloc((size_t)(sc->r + 2) * sc->ss);
    buf = (uint8_t *)malloc(sc->ss);
    created = (char **)calloc(sc->nvol + 1, sizeof(char *));
    if (badlist == NULL || work == NULL || buf == NULL || created == NULL) goto fail;
    for (k = 0; k < sc->S; k++) if (s.bad[k]) badlist[nbadlist++] = k;
    spool_path = join2(outdir, "repair.scratch");
    if (spool_path == NULL || (spool = bpr_wfile_create(spool_path)) == NULL) {
        fprintf(stderr, "boltpack-recover: cannot create a scratch file in %s\n", outdir);
        goto fail;
    }
    for (g = 0; g < sc->ngroups; g++)
        if (s.gbad[g] > 0 &&
            decode_group(&s, g, badlist, nbadlist, spool, work, err, sizeof err, &rp.unverified) != 0) {
            fprintf(stderr, "boltpack-recover: %s\n", err);
            goto fail;
        }
    rp.repaired = nbadlist;
    /* write the volumes */
    for (v = 0; v < sc->nvol; v++) {
        char *path = join2(outdir, sc->vol[v].name);
        bpr_wfile *w;
        uint32_t i2;
        int fi = s.slot_file[v];
        if (path == NULL || (w = bpr_wfile_create(path)) == NULL) {
            fprintf(stderr, "boltpack-recover: cannot create %s\n", path ? path : sc->vol[v].name);
            free(path);
            goto fail;
        }
        created[ncreated++] = path;
        for (i2 = 0; i2 < sc->vol[v].nsec; i2++) {
            uint32_t idx = sc->vol[v].first + i2, lo = 0, hi = nbadlist;
            uint64_t off = (uint64_t)i2 * sc->ss, len = sc->vol[v].size - off;
            if (len > sc->ss) len = sc->ss;
            if (s.bad[idx]) {
                while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (badlist[mid] < idx) lo = mid + 1; else hi = mid; }
                if (bpr_wfile_seek(spool, (int64_t)((uint64_t)lo * sc->ss), 0) != 0 ||
                    bpr_wfile_read(spool, buf, sc->ss) != (long)sc->ss) {
                    fprintf(stderr, "boltpack-recover: cannot read the scratch file\n");
                    bpr_wfile_close(w);
                    goto fail;
                }
            } else if (read_volume_sector(s.in[fi].f, sc->vol[v].size, sc->ss, i2, buf) != 0) {
                fprintf(stderr, "boltpack-recover: a sector that verified earlier cannot be read again\n");
                bpr_wfile_close(w);
                goto fail;
            }
            if (bpr_wfile_write(w, buf, (size_t)len) != (long)len) {
                fprintf(stderr, "boltpack-recover: write error on %s\n", path);
                bpr_wfile_close(w);
                goto fail;
            }
        }
        if (bpr_wfile_close(w) != 0) {
            fprintf(stderr, "boltpack-recover: write error on %s\n", path);
            goto fail;
        }
    }
    bpr_wfile_close(spool);
    spool = NULL;
    bpr_path_remove(spool_path);
    /* read the result back and verify every sector against the table */
    for (v = 0; v < sc->nvol; v++) {
        bpr_file *f = bpr_file_open(created[v]);
        uint32_t i2;
        if (f == NULL || bpr_file_size(f) != sc->vol[v].size) {
            fprintf(stderr, "boltpack-recover: %s did not read back at the recorded size\n", created[v]);
            if (f) bpr_file_close(f);
            goto fail;
        }
        for (i2 = 0; i2 < sc->vol[v].nsec; i2++) {
            if (read_volume_sector(f, sc->vol[v].size, sc->ss, i2, buf) != 0 ||
                bpr_sc_check(sc, sc->vol[v].first + i2, buf) == 0) {
                fprintf(stderr, "boltpack-recover: %s sector %u does not verify after repair\n", created[v], i2);
                bpr_file_close(f);
                goto fail;
            }
        }
        bpr_file_close(f);
    }
    /* the tool's own analysis of the repaired set */
    {
        bpr_ctx *c = (bpr_ctx *)calloc(1, sizeof *c);
        if (c != NULL && bpr_load_files(c, (int)sc->nvol, created) == 0 && c->nfiles > 0) {
            size_t e;
            bpr_identify(c);
            if (bpr_analyze(c) == 0) {
                rp.archive_checked = 1;
                rp.archive_complete = c->complete;
                rp.archive_table = c->table.status == BPR_TBL_TRUSTED;
                rp.archive_entries = (int)c->nent;
                for (e = 0; e < c->nent; e++) if (c->ent[e].status == BPR_ST_OK) rp.archive_ok_entries++;
            }
        }
        if (c != NULL) { bpr_ctx_free(c); free(c); }
    }
    rp.written_ok = 1;
    if (rp.archive_checked && rp.archive_complete && rp.archive_table &&
        rp.archive_ok_entries == rp.archive_entries) {
        verdict = "Repaired. Every sector verifies and the archive checks out.";
        rc = 0;
    } else if (rp.archive_checked) {
        verdict = "Restored to the protected state, which was itself damaged: every sector matches the "
                  "recovery file, but the archive does not verify. Use extract or rebuild on the repaired copy.";
        rc = 1;
    } else {
        verdict = "Repaired. Every sector verifies; the archive could not be analysed.";
        rc = 1;
    }
    goto report;

fail:
    rc = 3;
    if (spool != NULL) { bpr_wfile_close(spool); spool = NULL; }
    if (spool_path != NULL) bpr_path_remove(spool_path);
    for (v = 0; v < ncreated; v++) bpr_path_remove(created[v]);
    verdict = "Repair failed; the files it had written were removed.";
report:
    o = report ? bpr_fopen_write(report) : stdout;
    if (o == NULL) {
        fprintf(stderr, "boltpack-recover: cannot write %s\n", report);
        rc = 3;
    } else {
        if (rc == 3) fprintf(stderr, "%s\n", verdict);
        else if (!json) {
            print_scan(o, 0, &s, scpath, verdict, NULL);
            if (rp.written_ok)
                fprintf(o, "  repaired %u sector(s) into %s%s\n", rp.repaired, outdir,
                        rp.unverified ? " (some could not be checked against the table)" : "");
            if (rp.archive_checked)
                fprintf(o, "  repaired archive: %s, table %s, %d of %d entries verify\n",
                        rp.archive_complete ? "complete" : "incomplete",
                        rp.archive_table ? "trusted" : "not trusted", rp.archive_ok_entries, rp.archive_entries);
        } else {
            char ex[600];
            if (rp.written_ok) {
                snprintf(ex, sizeof ex,
                         "\"repair\":{\"repaired_sectors\":%u,\"unverified_sectors\":%u,"
                         "\"archive\":{\"checked\":%d,\"complete\":%d,\"table_trusted\":%d,\"entries\":%d,\"entries_ok\":%d}}",
                         rp.repaired, rp.unverified, rp.archive_checked, rp.archive_complete,
                         rp.archive_table, rp.archive_entries, rp.archive_ok_entries);
                print_scan(o, 1, &s, scpath, verdict, ex);
            } else print_scan(o, 1, &s, scpath, verdict, NULL);
        }
        if (o != stdout) fclose(o);
    }
    free(badlist);
    free(work);
    free(buf);
    free(spool_path);
    for (v = 0; v < ncreated; v++) free(created[v]);
    free(created);
    free_scan(&s);
    free(files);
    return rc;
}
