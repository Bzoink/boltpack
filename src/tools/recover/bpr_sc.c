/* The .bpr recovery file: format, validation, and reading a damaged copy.
 * See the recovery plan, section 19, and test/sidecar_model.py. */
#include "bpr_sc.h"

#include <stdlib.h>
#include <string.h>

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* ---- parameters and checksums ------------------------------------------ */

void bpr_sc_params(uint32_t S, unsigned permille, uint32_t *ngroups, uint32_t *G,
                   uint32_t *r)
{
    unsigned g, gmax = 1;
    uint32_t ng, gg, rr;
    for (g = 1; g < 256; g++) {
        unsigned x = (permille * g + 999) / 1000;      /* r rounds up */
        if (x < 1) x = 1;
        if (g + x <= 255) gmax = g;
    }
    ng = (S + gmax - 1) / gmax;
    if (ng < 1) ng = 1;
    gg = (S + ng - 1) / ng;
    rr = (permille * gg + 999) / 1000;
    if (rr < 1) rr = 1;
    *ngroups = ng;
    *G = gg;
    *r = rr;
}

uint32_t bpr_sc_group_size(uint32_t S, uint32_t ngroups, uint32_t g)
{
    if (g >= S) return 0;
    return (S - 1 - g) / ngroups + 1;
}

uint64_t bpr_sc_hash(uint32_t idx, const uint8_t *data, uint32_t ss)
{
    uint64_t h = 0x243F6A8885A308D3ull ^ ((uint64_t)idx * 0x9E3779B97F4A7C15ull);
    uint32_t i;
    for (i = 0; i + 8 <= ss; i += 8) {
        h = (h ^ bpr_le64(data + i)) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    h ^= ss;
    h ^= h >> 29;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 32;
    return h;
}

int bpr_sc_valid_name(const char *name, size_t len)
{
    size_t i;
    if (len == 0 || len > BPR_SC_MAX_NAME) return 0;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch < 0x20 || ch == 0x7f || ch == '/' || ch == '\\') return 0;
    }
    return 1;
}

/* ---- layout --------------------------------------------------------------- */

static int add64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a > UINT64_MAX - b) return -1;
    *out = a + b;
    return 0;
}

/* Fill vol[].first/nsec and the canonical offsets from nvol, sizes, ss, S,
 * r, ngroups.  -1 if the numbers do not add up or overflow. */
int bpr_sc_layout(bpr_sc *sc)
{
    uint64_t total_sec = 0, vl = 0, hdr, entries, tab, par, v;
    uint32_t i;
    for (i = 0; i < sc->nvol; i++) {
        uint64_t n;
        if (sc->vol[i].size > UINT64_MAX - sc->ss) return -1;
        n = (sc->vol[i].size + sc->ss - 1) / sc->ss;
        sc->vol[i].first = (uint32_t)total_sec;
        sc->vol[i].nsec = (uint32_t)n;
        if (n > BPR_SC_MAX_SECTORS) return -1;
        total_sec += n;
        if (total_sec > BPR_SC_MAX_SECTORS) return -1;
        vl += 10 + strlen(sc->vol[i].name);
    }
    if (total_sec != sc->S) return -1;
    hdr = BPR_SC_FIXED + vl + 4;
    entries = (uint64_t)sc->S + sc->nparity;
    sc->nchunks = (uint32_t)((entries + BPR_SC_CHUNK - 1) / BPR_SC_CHUNK);
    tab = entries * 12 + (uint64_t)sc->nchunks * 4;
    par = (uint64_t)sc->nparity * sc->ss;
    sc->hdr_len = hdr;
    sc->table_len = tab;
    sc->t_off = hdr;
    if (add64(sc->t_off, tab, &sc->p_off) || add64(sc->p_off, par, &sc->h2_off) ||
        add64(sc->h2_off, hdr, &sc->t2_off) || add64(sc->t2_off, tab, &v) ||
        add64(v, BPR_SC_FOOTER, &v))
        return -1;
    return 0;
}

int bpr_sc_geometry(const bpr_sc *sc, char *err, size_t errn)
{
    uint32_t maxg;
    if (sc->ss < BPR_SC_MIN_SS || sc->ss > BPR_SC_MAX_SS || sc->ss % 8 != 0) {
        snprintf(err, errn, "implausible sector size %u", sc->ss);
        return -1;
    }
    if (sc->S == 0 || sc->r == 0 || sc->ngroups == 0 || sc->ngroups > sc->S) {
        snprintf(err, errn, "implausible group counts");
        return -1;
    }
    if ((uint64_t)sc->ngroups * sc->r != sc->nparity ||
        (uint64_t)sc->S + sc->nparity > BPR_SC_MAX_SECTORS) {
        snprintf(err, errn, "parity count does not match the groups");
        return -1;
    }
    if ((uint64_t)sc->r * sc->ss > BPR_SC_MAX_PARITY_MEM) {
        snprintf(err, errn, "parity of %u sectors of %u bytes is too large to hold", sc->r, sc->ss);
        return -1;
    }
    maxg = (sc->S + sc->ngroups - 1) / sc->ngroups;
    if ((uint64_t)maxg + sc->r > 256) {
        snprintf(err, errn, "group of %u sectors with %u parity exceeds the code", maxg, sc->r);
        return -1;
    }
    return 0;
}

/* ---- serialisation ---------------------------------------------------------- */

uint8_t *bpr_sc_header_bytes(const bpr_sc *sc, size_t *len)
{
    size_t vl = 0, n, p;
    uint8_t *b;
    uint32_t i;
    for (i = 0; i < sc->nvol; i++) vl += 10 + strlen(sc->vol[i].name);
    n = BPR_SC_FIXED + vl + 4;
    b = (uint8_t *)calloc(1, n);
    if (b == NULL) return NULL;
    memcpy(b, "BPRC", 4);
    put16(b + 4, 1);
    put16(b + 6, 0);
    put64(b + 8, sc->set_id);
    put32(b + 16, sc->nvol);
    put32(b + 20, sc->ss);
    put32(b + 24, sc->S);
    put32(b + 28, sc->r);
    put32(b + 32, sc->ngroups);
    put32(b + 36, sc->nparity);
    put64(b + 40, sc->t_off);
    put64(b + 48, sc->p_off);
    put64(b + 56, sc->h2_off);
    put64(b + 64, sc->t2_off);
    put32(b + 72, (uint32_t)vl);
    put32(b + 76, 0);
    p = BPR_SC_FIXED;
    for (i = 0; i < sc->nvol; i++) {
        size_t nl = strlen(sc->vol[i].name);
        put64(b + p, sc->vol[i].size);
        put16(b + p + 8, (uint32_t)nl);
        memcpy(b + p + 10, sc->vol[i].name, nl);
        p += 10 + nl;
    }
    put32(b + p, bpr_crc32(0, b, p));
    *len = n;
    return b;
}

uint8_t *bpr_sc_table_bytes(const bpr_sc *sc, size_t *len)
{
    uint64_t entries = (uint64_t)sc->S + sc->nparity, c, pos = 0;
    uint8_t *b = (uint8_t *)malloc((size_t)sc->table_len);
    if (b == NULL) return NULL;
    for (c = 0; c < entries; c += BPR_SC_CHUNK) {
        uint64_t cnt = entries - c < BPR_SC_CHUNK ? entries - c : BPR_SC_CHUNK;
        memcpy(b + pos, sc->tab + c * 12, (size_t)(cnt * 12));
        put32(b + pos + cnt * 12, bpr_crc32(0, b + pos, (size_t)(cnt * 12)));
        pos += cnt * 12 + 4;
    }
    *len = (size_t)pos;
    return b;
}

void bpr_sc_footer_bytes(const bpr_sc *sc, uint8_t out[BPR_SC_FOOTER])
{
    memset(out, 0, BPR_SC_FOOTER);
    memcpy(out, "CRPB", 4);
    put32(out + 4, (uint32_t)sc->hdr_len);
    put64(out + 8, sc->h2_off);
    put64(out + 16, sc->t2_off);
    put64(out + 24, sc->table_len);
    put64(out + 32, sc->t2_off + sc->table_len + BPR_SC_FOOTER);
    put32(out + 40, bpr_crc32(0, out, 40));
}

void bpr_sc_set_entry(bpr_sc *sc, uint32_t idx, const uint8_t *data)
{
    uint8_t *e = sc->tab + (size_t)idx * 12;
    put32(e, bpr_crc32(0, data, sc->ss));
    put64(e + 4, bpr_sc_hash(idx, data, sc->ss));
}

/* ---- reading ------------------------------------------------------------------ */

static int parse_header_at(bpr_file *f, uint64_t file_size, uint64_t off, bpr_sc *sc,
                           char *err, size_t errn)
{
    uint8_t fx[BPR_SC_FIXED], *all = NULL;
    uint32_t vl, i;
    uint64_t pos, hdr, v;
    if (off > file_size || file_size - off < BPR_SC_FIXED + 4) {
        snprintf(err, errn, "header does not fit in the file");
        return -1;
    }
    if (bpr_file_read(f, off, fx, sizeof fx) != 0 || memcmp(fx, "BPRC", 4) != 0) {
        snprintf(err, errn, "no header magic");
        return -1;
    }
    vl = bpr_le32(fx + 72);
    if (vl > 1000u * (10u + BPR_SC_MAX_NAME) || bpr_le32(fx + 76) != 0 ||
        file_size - off < BPR_SC_FIXED + (uint64_t)vl + 4) {
        snprintf(err, errn, "implausible volume list");
        return -1;
    }
    hdr = BPR_SC_FIXED + (uint64_t)vl + 4;
    all = (uint8_t *)malloc((size_t)hdr);
    if (all == NULL) { snprintf(err, errn, "out of memory"); return -1; }
    if (bpr_file_read(f, off, all, (size_t)hdr) != 0) {
        free(all);
        snprintf(err, errn, "cannot read the header");
        return -1;
    }
    if (bpr_crc32(0, all, (size_t)hdr - 4) != bpr_le32(all + hdr - 4)) {
        free(all);
        snprintf(err, errn, "header checksum mismatch");
        return -1;
    }
    if (bpr_le16(all + 4) != 1 || bpr_le16(all + 6) != 0) {
        unsigned ver = bpr_le16(all + 4);
        free(all);
        snprintf(err, errn, "unsupported sidecar version %u", ver);
        return -1;
    }
    sc->set_id = bpr_le64(all + 8);
    sc->nvol = bpr_le32(all + 16);
    sc->ss = bpr_le32(all + 20);
    sc->S = bpr_le32(all + 24);
    sc->r = bpr_le32(all + 28);
    sc->ngroups = bpr_le32(all + 32);
    sc->nparity = bpr_le32(all + 36);
    if (sc->nvol < 1 || sc->nvol > BPR_MAX_VOLUMES ||
        bpr_sc_geometry(sc, err, errn) != 0) {
        free(all);
        if (sc->nvol < 1 || sc->nvol > BPR_MAX_VOLUMES) snprintf(err, errn, "bad volume count");
        return -1;
    }
    sc->vol = (bpr_sc_vol *)calloc(sc->nvol, sizeof *sc->vol);
    if (sc->vol == NULL) { free(all); snprintf(err, errn, "out of memory"); return -1; }
    pos = BPR_SC_FIXED;
    for (i = 0; i < sc->nvol; i++) {
        uint32_t nl;
        if (pos + 10 > hdr - 4) goto bad_list;
        nl = bpr_le16(all + pos + 8);
        if (pos + 10 + nl > hdr - 4 || !bpr_sc_valid_name((const char *)all + pos + 10, nl))
            goto bad_list;
        sc->vol[i].size = bpr_le64(all + pos);
        memcpy(sc->vol[i].name, all + pos + 10, nl);
        sc->vol[i].name[nl] = '\0';
        pos += 10 + nl;
    }
    if (pos != hdr - 4) goto bad_list;
    if (bpr_sc_layout(sc) != 0) {
        free(all);
        snprintf(err, errn, "volume sizes do not match the sector count");
        return -1;
    }
    v = bpr_le64(all + 40);
    if (v != sc->t_off || bpr_le64(all + 48) != sc->p_off ||
        bpr_le64(all + 56) != sc->h2_off || bpr_le64(all + 64) != sc->t2_off) {
        free(all);
        snprintf(err, errn, "offsets do not match the layout");
        return -1;
    }
    free(all);
    return 0;
bad_list:
    free(all);
    snprintf(err, errn, "bad volume list");
    return -1;
}

static void free_vols(bpr_sc *sc)
{
    free(sc->vol);
    sc->vol = NULL;
}

static int read_chunk(bpr_file *f, uint64_t file_size, uint64_t off, uint64_t cnt,
                      uint8_t *buf)
{
    uint64_t n = cnt * 12 + 4;
    if (off > file_size || file_size - off < n) return -1;
    if (bpr_file_read(f, off, buf, (size_t)n) != 0) return -1;
    if (bpr_crc32(0, buf, (size_t)(cnt * 12)) != bpr_le32(buf + cnt * 12)) return -1;
    return 0;
}

int bpr_sc_load(bpr_sc *sc, const char *path, char *err, size_t errn)
{
    bpr_file *f;
    uint64_t fs, entries, c;
    uint8_t *tmp = NULL;
    char e1[160] = "";
    memset(sc, 0, sizeof *sc);
    f = bpr_file_open(path);
    if (f == NULL) { snprintf(err, errn, "cannot open %s", path); return -1; }
    fs = bpr_file_size(f);
    sc->file_size = fs;
    if (parse_header_at(f, fs, 0, sc, e1, sizeof e1) != 0) {
        uint8_t ft[BPR_SC_FOOTER];
        uint64_t h2;
        free_vols(sc);
        if (fs < BPR_SC_FOOTER + BPR_SC_FIXED + 4 ||
            bpr_file_read(f, fs - BPR_SC_FOOTER, ft, sizeof ft) != 0 ||
            memcmp(ft, "CRPB", 4) != 0 ||
            bpr_crc32(0, ft, 40) != bpr_le32(ft + 40) || bpr_le64(ft + 32) != fs) {
            snprintf(err, errn, "the sidecar header is damaged (%s) and no usable footer", e1);
            bpr_file_close(f);
            return -1;
        }
        h2 = bpr_le64(ft + 8);
        if (parse_header_at(f, fs, h2, sc, e1, sizeof e1) != 0 || sc->h2_off != h2 ||
            sc->t2_off != bpr_le64(ft + 16) || sc->table_len != bpr_le64(ft + 24)) {
            snprintf(err, errn, "both header copies are damaged (%s)", e1);
            free_vols(sc);
            bpr_file_close(f);
            return -1;
        }
        sc->hdr_from_copy = 1;
    }
    entries = (uint64_t)sc->S + sc->nparity;
    sc->tab = (uint8_t *)calloc((size_t)entries, 12);
    sc->chunk = (uint8_t *)calloc(sc->nchunks, 1);
    tmp = (uint8_t *)malloc(BPR_SC_CHUNK * 12 + 4);
    if (sc->tab == NULL || sc->chunk == NULL || tmp == NULL) {
        snprintf(err, errn, "out of memory");
        free(tmp);
        sc->f = f;
        bpr_sc_free(sc);
        return -1;
    }
    for (c = 0; c < sc->nchunks; c++) {
        uint64_t first = c * BPR_SC_CHUNK;
        uint64_t cnt = entries - first < BPR_SC_CHUNK ? entries - first : BPR_SC_CHUNK;
        uint64_t rel = c * (BPR_SC_CHUNK * 12ull + 4);
        if (read_chunk(f, fs, sc->t_off + rel, cnt, tmp) == 0) sc->chunk[c] = 1;
        else if (read_chunk(f, fs, sc->t2_off + rel, cnt, tmp) == 0) sc->chunk[c] = 2;
        if (sc->chunk[c] != 0) memcpy(sc->tab + first * 12, tmp, (size_t)(cnt * 12));
    }
    free(tmp);
    sc->f = f;
    return 0;
}

void bpr_sc_free(bpr_sc *sc)
{
    if (sc->f != NULL) bpr_file_close(sc->f);
    free(sc->vol);
    free(sc->tab);
    free(sc->chunk);
    memset(sc, 0, sizeof *sc);
}

int bpr_sc_known(const bpr_sc *sc, uint32_t idx)
{
    return sc->chunk[idx / BPR_SC_CHUNK] != 0;
}

int bpr_sc_check(const bpr_sc *sc, uint32_t idx, const uint8_t *data)
{
    const uint8_t *e = sc->tab + (size_t)idx * 12;
    if (!bpr_sc_known(sc, idx)) return -1;
    return bpr_crc32(0, data, sc->ss) == bpr_le32(e) &&
           bpr_sc_hash(idx, data, sc->ss) == bpr_le64(e + 4);
}

int bpr_sc_read_parity(const bpr_sc *sc, uint32_t p, uint8_t *buf)
{
    uint64_t off = sc->p_off + (uint64_t)p * sc->ss;
    return bpr_file_read(sc->f, off, buf, sc->ss);
}
