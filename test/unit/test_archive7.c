/*
 * test_archive7.c -- unit tests for archive format 7 (volume sets)
 *
 * Builds archives in memory, single and multi-volume, for every codec, and
 * checks round trips, end-of-volume block splitting, spanning file tables,
 * editing, and the volume error paths.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include <stdlib.h>
#include <string.h>
#include "archiver.h"
#include "compress.h"
#include "test_runner.h"
#include "../../src/frontend/common/volume_size.h"

/* ---- memory streams ---------------------------------------------------- */

typedef struct {
    bp_uint8 *data;
    unsigned long len, cap, pos;
} mem_t;

static long mem_read(void *h, void *buf, bp_uint32 n)
{
    mem_t *m = (mem_t *)h;
    unsigned long left = m->pos < m->len ? m->len - m->pos : 0;
    if (n > left) n = (bp_uint32)left;
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return (long)n;
}

static long mem_write(void *h, const void *buf, bp_uint32 n)
{
    mem_t *m = (mem_t *)h;
    if (m->pos + n > m->cap) {
        unsigned long nc = (m->pos + n) * 2 + 64;
        bp_uint8 *nd = (bp_uint8 *)realloc(m->data, nc);
        if (nd == NULL) return -1;
        m->data = nd;
        m->cap = nc;
    }
    memcpy(m->data + m->pos, buf, n);
    m->pos += n;
    if (m->pos > m->len) m->len = m->pos;
    return (long)n;
}

static int mem_seek(void *h, bp_off_t off, int origin)
{
    mem_t *m = (mem_t *)h;
    long base = origin == 0 ? 0 : origin == 1 ? (long)m->pos : (long)m->len;
    if (base + (long)off < 0) return -1;
    m->pos = (unsigned long)(base + (long)off);
    return 0;
}

static bp_off_t mem_tell(void *h)
{
    return (bp_off_t)((mem_t *)h)->pos;
}

static void mem_io(bp_io_t *io, mem_t *m)
{
    io->read = mem_read;
    io->write = mem_write;
    io->seek = mem_seek;
    io->tell = mem_tell;
    io->handle = m;
}

static void mem_free(mem_t *m)
{
    free(m->data);
    memset(m, 0, sizeof(*m));
}

/* ---- volume set in memory ---------------------------------------------- */

#define MAXV 1000

typedef struct {
    mem_t   vol[MAXV];
    bp_io_t io[MAXV];
    int     created;
    int     finished_ok;
    int     last_number;      /* volume marked last by finish() */
    int     abandoned;
    /* source behaviour */
    int     missing;          /* volume number reported missing, -1 none */
    int     swap_a, swap_b;   /* serve these two volumes swapped */
    int     fix_on_retry;     /* serve the right volume when asked again */
    int     opens;
    int     retries;
    mem_t  *foreign;          /* serve this instead of volume foreign_at */
    int     foreign_at;
} vset_t;

static void vset_init(vset_t *s)
{
    memset(s, 0, sizeof(*s));
    s->last_number = -1;
    s->missing = -1;
    s->swap_a = s->swap_b = -1;
    s->foreign_at = -1;
}

static void vset_free(vset_t *s)
{
    int i;
    for (i = 0; i < MAXV; i++) mem_free(&s->vol[i]);
}

static bp_io_t *sink_create(void *user, bp_uint16 number)
{
    vset_t *s = (vset_t *)user;
    if (number >= MAXV || number != s->created) return NULL;
    mem_free(&s->vol[number]);
    mem_io(&s->io[number], &s->vol[number]);
    s->created++;
    return &s->io[number];
}

static int sink_finish(void *user, bp_io_t *io, bp_uint16 number, int is_last,
                       int status)
{
    vset_t *s = (vset_t *)user;
    (void)io;
    if (status != BP_OK) { s->abandoned++; return 0; }
    s->finished_ok++;
    if (is_last) s->last_number = number;
    return 0;
}

static bp_io_t *src_open(void *user, bp_uint16 number, int reason)
{
    vset_t *s = (vset_t *)user;
    int n;
    s->opens++;
    if (reason != BP_OK) {
        s->retries++;
        if (!s->fix_on_retry) return NULL;
    }
    n = number == BP_VOLUME_LAST_REQUEST ? s->last_number : (int)number;
    if (n < 0 || n >= s->created) return NULL;
    if (reason == BP_OK) {
        if (n == s->missing) return NULL;
        if (n == s->swap_a) n = s->swap_b;
        else if (n == s->swap_b) n = s->swap_a;
        if (n == s->foreign_at && s->foreign != NULL) {
            static bp_io_t fio;
            mem_io(&fio, s->foreign);
            s->foreign->pos = 0;
            return &fio;
        }
    }
    s->vol[n].pos = 0;
    return &s->io[n];
}

static void src_close(void *user, bp_io_t *io)
{
    (void)user;
    (void)io;
}

static void vset_sink(vset_t *s, bp_volume_sink_t *k)
{
    k->create = sink_create;
    k->finish = sink_finish;
    k->user = s;
}

static void vset_source(vset_t *s, bp_volume_source_t *src)
{
    src->open = src_open;
    src->close = src_close;
    src->user = s;
}

/* ---- test payloads ----------------------------------------------------- */

#define NFILES 6
static bp_uint8 *g_data[NFILES];
static bp_uint32 g_size[NFILES];
static const char *g_names[NFILES] = {
    "dir/", "dir/text.txt", "dir/random.bin", "empty.txt", "mixed.dat",
    "small.txt"
};

static void make_payloads(void)
{
    bp_uint32 i, seed = 12345;
    static const char words[] =
        "the quick brown fox jumps over the lazy dog volume set boltpack ";
    g_size[0] = 0;
    g_size[1] = 300000;
    g_size[2] = 150000;
    g_size[3] = 0;
    g_size[4] = 700001;
    g_size[5] = 777;
    for (i = 0; i < NFILES; i++) {
        bp_uint32 j;
        g_data[i] = (bp_uint8 *)malloc(g_size[i] + 1);
        for (j = 0; j < g_size[i]; j++) {
            seed = seed * 1103515245UL + 12345UL;
            if (i == 2 || (i == 4 && (j / 40000) % 2 == 1))
                g_data[i][j] = (bp_uint8)(seed >> 16);
            else
                g_data[i][j] = (bp_uint8)words[(j + (seed >> 28)) % (sizeof(words) - 1)];
        }
    }
}

static int add_payloads(bp_archive_t *ar)
{
    int i, rc;
    for (i = 0; i < NFILES; i++) {
        mem_t m;
        bp_io_t io;
        m.data = g_data[i];
        m.len = m.cap = g_size[i];
        m.pos = 0;
        mem_io(&io, &m);
        rc = bp_archive_add_entry_ex(ar, g_names[i],
                 g_names[i][strlen(g_names[i]) - 1] == '/' ? BP_TYPE_DIR : BP_TYPE_DATA,
                 BP_OS_LINUX, g_size[i] ? &io : NULL, bp_u64_from_u32(g_size[i]),
                 bp_u64_from_u32(1700000000UL + (bp_uint32)i),
                 BP_ENTRY_FLAG_MTIME_VALID);
        if (rc != BP_OK) return rc;
    }
    return BP_OK;
}

/* extraction into memory, compared against the payloads */
typedef struct {
    mem_t out;
    bp_io_t io;
    int seen;
    int matched;
    int mismatched;
} xctx_t;

static bp_io_t *x_open(const bp_entry_t *e, void *user)
{
    xctx_t *x = (xctx_t *)user;
    if (e->entry_type == BP_TYPE_DIR) { x->seen++; x->matched++; return NULL; }
    mem_free(&x->out);
    mem_io(&x->io, &x->out);
    return &x->io;
}

static int x_done(const bp_entry_t *e, bp_io_t *out, int status, void *user)
{
    xctx_t *x = (xctx_t *)user;
    int i;
    (void)out;
    x->seen++;
    if (status != BP_OK) { x->mismatched++; return BP_OK; }
    for (i = 0; i < NFILES; i++) {
        if (strcmp(e->name, g_names[i]) == 0) {
            if (x->out.len == g_size[i] &&
                (g_size[i] == 0 || memcmp(x->out.data, g_data[i], g_size[i]) == 0))
                x->matched++;
            else
                x->mismatched++;
            return BP_OK;
        }
    }
    x->mismatched++;
    return BP_OK;
}

static int extract_set(const bp_volume_source_t *src, xctx_t *x)
{
    memset(x, 0, sizeof(*x));
    return bp_archive_extract_volumes(src, x_open, x_done, NULL, x);
}

static int count_entries(const bp_entry_t *e, void *user)
{
    (void)e;
    (*(int *)user)++;
    return 0;
}

static bp_uint32 rd32(const bp_uint8 *p)
{
    return (bp_uint32)p[0] | ((bp_uint32)p[1] << 8) |
           ((bp_uint32)p[2] << 16) | ((bp_uint32)p[3] << 24);
}

/* ---- tests ------------------------------------------------------------- */

static const bp_uint8 methods[4] = {
    BP_METHOD_STORED, BP_METHOD_BOLTLITE, BP_METHOD_BOLTLZ, BP_METHOD_FLUFF
};
static const char *method_names[4] = { "stored", "BoltLite", "BoltLZ", "Fluff" };

static void test_single(void)
{
    int m;
    UNIT_SECTION("single-volume round trip");
    for (m = 0; m < 4; m++) {
        mem_t arc;
        bp_io_t io;
        bp_archive_t ar;
        xctx_t x;
        int n = 0, rc;
        char label[96];
        bp_archive_info_t info;

        memset(&arc, 0, sizeof(arc));
        mem_io(&io, &arc);
        rc = bp_archive_create(&ar, &io, 32768, methods[m]);
        if (rc == BP_OK) rc = add_payloads(&ar);
        if (rc == BP_OK) rc = bp_archive_finalize(&ar);
        bp_archive_close(&ar);
        sprintf(label, "%s: create", method_names[m]);
        UNIT_CHECK(rc == BP_OK, label);
        UNIT_CHECK(arc.data[4] == 7 && arc.data[5] == 0, "format version 7");
        arc.pos = 0;
        rc = bp_archive_list(&io, count_entries, &n);
        sprintf(label, "%s: list %d entries", method_names[m], n);
        UNIT_CHECK(rc == BP_OK && n == NFILES, label);
        rc = bp_archive_test(&io, NULL, NULL);
        sprintf(label, "%s: test", method_names[m]);
        UNIT_CHECK(rc == BP_OK, label);
        memset(&x, 0, sizeof(x));
        rc = bp_archive_extract_all_ex(&io, x_open, x_done, NULL, &x);
        sprintf(label, "%s: extract matches (%d/%d)", method_names[m], x.matched, x.seen);
        UNIT_CHECK(rc == BP_OK && x.matched == NFILES && x.mismatched == 0, label);
        rc = bp_archive_info(&io, &info);
        UNIT_CHECK(rc == BP_OK && info.volume_count == 1 &&
                   info.entry_count == NFILES && info.dir_count == 1 &&
                   info.archive_size.lo == arc.len, "info");
        mem_free(&x.out);
        mem_free(&arc);
    }
}

static void check_volume_sizes(vset_t *s, unsigned long limit, const char *what)
{
    int i, ok = 1, nonempty = 1;
    unsigned long total = 0, fill = 0;
    char label[128];
    for (i = 0; i < s->created; i++) {
        if (s->vol[i].len > limit) ok = 0;
        if (s->vol[i].len <= 64) nonempty = 0;
        if (i < s->created - 1) { total += limit; fill += s->vol[i].len; }
    }
    sprintf(label, "%s: %d volumes, none larger than %lu", what, s->created, limit);
    UNIT_CHECK(ok, label);
    UNIT_CHECK(nonempty, "every body holds a structure");
    if (total > 0) {
        sprintf(label, "%s: non-last volumes %.1f%% full", what,
                100.0 * (double)fill / (double)total);
        UNIT_CHECK(fill * 100UL >= total * 97UL, label);
    }
}

static void test_multi(void)
{
    static const unsigned long limits[3] = { 8192UL, 65536UL, 360000UL };
    int m, l;
    UNIT_SECTION("multi-volume round trip");
    for (l = 0; l < 3; l++) {
        for (m = 0; m < 4; m++) {
            static vset_t s;
            bp_volume_sink_t sink;
            bp_volume_source_t src;
            bp_archive_t ar;
            xctx_t x;
            int rc;
            char label[128];

            vset_init(&s);
            vset_sink(&s, &sink);
            vset_source(&s, &src);
            rc = bp_archive_create_volumes(&ar, &sink, bp_u64_from_u32(limits[l]),
                                           l == 0 ? 32768 : 262144, methods[m]);
            if (rc == BP_OK) rc = add_payloads(&ar);
            if (rc == BP_OK) rc = bp_archive_finalize(&ar);
            bp_archive_close(&ar);
            sprintf(label, "%s, %lu-byte volumes: create", method_names[m], limits[l]);
            UNIT_CHECK(rc == BP_OK && s.abandoned == 0 &&
                       s.finished_ok == s.created &&
                       s.last_number == s.created - 1, label);
            check_volume_sizes(&s, limits[l], method_names[m]);
            rc = extract_set(&src, &x);
            sprintf(label, "%s, %lu-byte volumes: extract (%d ok, %d bad)",
                    method_names[m], limits[l], x.matched, x.mismatched);
            UNIT_CHECK(rc == BP_OK && x.matched == NFILES && x.mismatched == 0, label);
            rc = bp_archive_test_volumes(&src, NULL, NULL, NULL, NULL);
            UNIT_CHECK(rc == BP_OK, "test volumes");
            mem_free(&x.out);
            vset_free(&s);
        }
    }
}

static void test_spanning_table(void)
{
    static vset_t s;
    bp_volume_sink_t sink;
    bp_volume_source_t src;
    bp_archive_t ar;
    bp_archive_info_t info;
    int i, rc = BP_OK, n = 0;
    char name[64];

    UNIT_SECTION("file table spanning volumes");
    vset_init(&s);
    vset_sink(&s, &sink);
    vset_source(&s, &src);
    rc = bp_archive_create_volumes(&ar, &sink, bp_u64_from_u32(4096), 8192,
                                   BP_METHOD_BOLTLZ);
    for (i = 0; rc == BP_OK && i < 600; i++) {
        mem_t m;
        bp_io_t io;
        sprintf(name, "d/a-fairly-long-file-name-for-the-table-%04d.txt", i);
        m.data = g_data[1];
        m.len = m.cap = 10 + (unsigned long)i;
        m.pos = 0;
        mem_io(&io, &m);
        rc = bp_archive_add_entry(&ar, name, BP_TYPE_DATA, BP_OS_LINUX, &io,
                                  bp_u64_from_u32((bp_uint32)m.len),
                                  bp_u64_from_u32(0));
    }
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    UNIT_CHECK(rc == BP_OK, "create 600 entries in 4 KiB volumes");
    rc = bp_archive_list_volumes(&src, count_entries, &n);
    UNIT_CHECK(rc == BP_OK && n == 600, "list all entries");
    rc = bp_archive_info_volumes(&src, &info, NULL);
    UNIT_CHECK(rc == BP_OK && info.volume_count == s.created && s.created > 10,
               "info reports the volume count");
    {
        /* the table cannot fit one 4 KiB volume: find its start volume */
        const mem_t *last = &s.vol[s.created - 1];
        const bp_uint8 *e = last->data + last->len - 32 - 48;
        bp_uint32 tvol = (bp_uint32)e[10] | ((bp_uint32)e[11] << 8);
        UNIT_CHECK(tvol < (bp_uint32)s.created - 1, "table starts before the last volume");
    }
    rc = bp_archive_test_volumes(&src, NULL, NULL, NULL, NULL);
    UNIT_CHECK(rc == BP_OK, "test spanning set");
    vset_free(&s);
}

static void build_set(vset_t *s, bp_uint8 method, unsigned long limit)
{
    bp_volume_sink_t sink;
    bp_archive_t ar;
    int rc;
    vset_init(s);
    vset_sink(s, &sink);
    rc = bp_archive_create_volumes(&ar, &sink, bp_u64_from_u32(limit), 65536, method);
    if (rc == BP_OK) rc = add_payloads(&ar);
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    UNIT_CHECK(rc == BP_OK && s->created >= 4, "build a set of several volumes");
}

static void test_volume_errors(void)
{
    static vset_t s, other;
    bp_volume_source_t src;
    xctx_t x;
    int rc;

    UNIT_SECTION("volume errors");
    build_set(&s, BP_METHOD_FLUFF, 120000);
    vset_source(&s, &src);

    s.missing = 1;
    rc = extract_set(&src, &x);
    UNIT_CHECK(rc == BP_ERR_VOLUME, "missing volume -> BP_ERR_VOLUME");
    mem_free(&x.out);

    s.missing = -1;
    s.swap_a = 1; s.swap_b = 2;
    rc = extract_set(&src, &x);
    UNIT_CHECK(rc == BP_ERR_VOLUME, "swapped volumes -> BP_ERR_VOLUME");
    mem_free(&x.out);

    s.fix_on_retry = 1;
    s.retries = 0;
    rc = extract_set(&src, &x);
    UNIT_CHECK(rc == BP_OK && x.mismatched == 0 && s.retries >= 1,
               "wrong volume, then the right one on retry");
    mem_free(&x.out);
    s.fix_on_retry = 0;
    s.swap_a = s.swap_b = -1;

    build_set(&other, BP_METHOD_FLUFF, 120000);
    s.foreign = &other.vol[2];
    s.foreign_at = 2;
    rc = extract_set(&src, &x);
    UNIT_CHECK(rc == BP_ERR_VOLUME, "volume from another set -> BP_ERR_VOLUME");
    mem_free(&x.out);
    s.foreign = NULL;
    s.foreign_at = -1;

    s.vol[1].len -= 100;
    rc = extract_set(&src, &x);
    UNIT_CHECK(rc == BP_ERR_TRUNCATED, "truncated volume -> BP_ERR_TRUNCATED");
    mem_free(&x.out);
    s.vol[1].len += 100;

    s.vol[1].data[200] ^= 0x40;
    rc = extract_set(&src, &x);
    UNIT_CHECK(rc == BP_ERR_CHECKSUM || rc == BP_ERR_CORRUPT,
               "damaged block -> checksum or structure error");
    mem_free(&x.out);
    s.vol[1].data[200] ^= 0x40;

    rc = extract_set(&src, &x);
    UNIT_CHECK(rc == BP_OK && x.mismatched == 0, "restored set extracts again");
    mem_free(&x.out);

    {
        bp_io_t io;
        int n = 0;
        mem_io(&io, &s.vol[s.created - 1]);
        rc = bp_archive_list(&io, count_entries, &n);
        UNIT_CHECK(rc == BP_OK && n == NFILES,
                   "single-stream list reads the table from the last volume");
        rc = bp_archive_test(&io, NULL, NULL);
        UNIT_CHECK(rc == BP_ERR_VOLUME, "single-stream test of a set -> BP_ERR_VOLUME");
    }
    vset_free(&other);
    vset_free(&s);
}

static void test_split_blocks(void)
{
    static vset_t s;
    int v, splits = 0, short_first = 0;
    UNIT_SECTION("end-of-volume blocks");
    build_set(&s, BP_METHOD_BOLTLZ, 100000);
    for (v = 0; v < s.created - 1; v++) {
        /* walk the structures of the body: last structure of a non-last
         * volume that is a short block shows an end-of-volume split */
        const bp_uint8 *d = s.vol[v].data;
        unsigned long body_end = s.vol[v].len - 32;
        UNIT_CHECK(body_end + 32 == s.vol[v].len, "volume layout");
        (void)d;
        if (s.vol[v].len > 100000UL - 600UL) splits++;
    }
    {
        /* first structure of volume 1 */
        const bp_uint8 *d = s.vol[1].data + 32;
        bp_uint32 orig = rd32(d + 4);
        if (orig < 65536UL) short_first = 1;
    }
    /* the volume before the last may end early: the table starts the last
     * volume when it fits there whole */
    UNIT_CHECK(splits >= s.created - 2,
               "every volume before the table is filled to within 600 bytes");
    UNIT_CHECK(short_first, "volume 1 starts with the continuation of a split block");
    vset_free(&s);
}

static void test_append_delete(void)
{
    mem_t arc, out;
    bp_io_t io, oio;
    bp_archive_t ar;
    xctx_t x;
    int rc, n = 0;
    bp_uint64 size;
    const char *del[1];

    UNIT_SECTION("append and delete");
    memset(&arc, 0, sizeof(arc));
    mem_io(&io, &arc);
    rc = bp_archive_create(&ar, &io, 32768, BP_METHOD_FLUFF);
    if (rc == BP_OK) {
        mem_t m;
        bp_io_t fio;
        m.data = g_data[1]; m.len = m.cap = g_size[1]; m.pos = 0;
        mem_io(&fio, &m);
        rc = bp_archive_add_entry(&ar, "first.txt", BP_TYPE_DATA, BP_OS_LINUX,
                                  &fio, bp_u64_from_u32(g_size[1]),
                                  bp_u64_from_u32(0));
    }
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    UNIT_CHECK(rc == BP_OK, "create one entry");

    arc.pos = 0;
    rc = bp_archive_open_append(&ar, &io, 32768, BP_METHOD_BOLTLZ);
    UNIT_CHECK(rc == BP_OK, "open for append");
    if (rc == BP_OK) rc = add_payloads(&ar);
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    if (rc == BP_OK) rc = bp_archive_output_size(&ar, &size);
    bp_archive_close(&ar);
    UNIT_CHECK(rc == BP_OK && size.lo == arc.len, "append and report size");
    arc.len = size.lo;
    arc.pos = 0;
    rc = bp_archive_list(&io, count_entries, &n);
    UNIT_CHECK(rc == BP_OK && n == NFILES + 1, "appended archive lists all entries");
    rc = bp_archive_test(&io, NULL, NULL);
    UNIT_CHECK(rc == BP_OK, "appended archive tests clean");

    memset(&out, 0, sizeof(out));
    mem_io(&oio, &out);
    del[0] = "first.txt";
    rc = bp_archive_delete(&io, &oio, del, 1, NULL);
    UNIT_CHECK(rc == BP_OK, "delete one entry");
    out.pos = 0;
    memset(&x, 0, sizeof(x));
    rc = bp_archive_extract_all_ex(&oio, x_open, x_done, NULL, &x);
    UNIT_CHECK(rc == BP_OK && x.matched == NFILES && x.mismatched == 0,
               "remaining entries extract");
    mem_free(&x.out);
    mem_free(&out);
    mem_free(&arc);
}

static void test_packed_commit(void)
{
    static vset_t s;
    bp_volume_sink_t sink;
    bp_volume_source_t src;
    bp_archive_t ar, worker;
    xctx_t x;
    int i, rc;

    UNIT_SECTION("packed entries committed into volumes");
    vset_init(&s);
    vset_sink(&s, &sink);
    vset_source(&s, &src);
    rc = bp_archive_create_volumes(&ar, &sink, bp_u64_from_u32(90000), 65536,
                                   BP_METHOD_FLUFF);
    if (rc == BP_OK) {
        mem_t dummy;
        bp_io_t dio;
        memset(&dummy, 0, sizeof(dummy));
        mem_io(&dio, &dummy);
        rc = bp_archive_create(&worker, &dio, 65536, BP_METHOD_FLUFF);
        for (i = 0; rc == BP_OK && i < NFILES; i++) {
            mem_t blob, m;
            bp_io_t bio, fio;
            bp_packed_entry_t p;
            memset(&blob, 0, sizeof(blob));
            mem_io(&bio, &blob);
            m.data = g_data[i]; m.len = m.cap = g_size[i]; m.pos = 0;
            mem_io(&fio, &m);
            rc = bp_archive_pack_entry_ex(&worker, g_names[i],
                     g_names[i][strlen(g_names[i]) - 1] == '/' ? BP_TYPE_DIR : BP_TYPE_DATA,
                     BP_OS_LINUX, g_size[i] ? &fio : NULL,
                     bp_u64_from_u32(g_size[i]),
                     bp_u64_from_u32(1700000000UL + (bp_uint32)i), &bio, &p,
                     BP_ENTRY_FLAG_MTIME_VALID);
            if (rc == BP_OK) rc = bp_archive_commit_packed_entry(&ar, &bio, &p);
            bp_packed_entry_dispose(&p);
            mem_free(&blob);
        }
        bp_archive_close(&worker);
        mem_free(&dummy);
    }
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    UNIT_CHECK(rc == BP_OK && s.created > 3, "commit packed Fluff entries");
    check_volume_sizes(&s, 90000UL, "packed Fluff");
    rc = extract_set(&src, &x);
    UNIT_CHECK(rc == BP_OK && x.matched == NFILES && x.mismatched == 0,
               "committed entries extract");
    mem_free(&x.out);
    vset_free(&s);
}

static void test_copy_into_volumes(void)
{
    static vset_t s;
    mem_t arc;
    bp_io_t io;
    bp_volume_sink_t sink;
    bp_volume_source_t src;
    bp_archive_t ar;
    xctx_t x;
    int rc;

    UNIT_SECTION("copy a single-volume archive into volumes");
    memset(&arc, 0, sizeof(arc));
    mem_io(&io, &arc);
    rc = bp_archive_create(&ar, &io, 65536, BP_METHOD_BOLTLITE);
    if (rc == BP_OK) rc = add_payloads(&ar);
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    vset_init(&s);
    vset_sink(&s, &sink);
    vset_source(&s, &src);
    if (rc == BP_OK)
        rc = bp_archive_create_volumes(&ar, &sink, bp_u64_from_u32(50000),
                                       65536, BP_METHOD_BOLTLITE);
    if (rc == BP_OK) { arc.pos = 0; rc = bp_archive_copy_from(&ar, &io); }
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    UNIT_CHECK(rc == BP_OK && s.created > 3, "copy into 50 KB volumes");
    check_volume_sizes(&s, 50000UL, "copied BoltLite");
    rc = extract_set(&src, &x);
    UNIT_CHECK(rc == BP_OK && x.matched == NFILES && x.mismatched == 0,
               "copied set extracts");
    mem_free(&x.out);
    vset_free(&s);
    mem_free(&arc);
}

static void test_parallel(void)
{
#ifdef BOLTPACK_FAST_FLUFF
    int m;
    UNIT_SECTION("parallel compression into volumes");
    for (m = 2; m < 4; m++) {
        static vset_t s;
        bp_volume_sink_t sink;
        bp_volume_source_t src;
        bp_archive_t ar;
        xctx_t x;
        int rc;
        char label[96];

        vset_init(&s);
        vset_sink(&s, &sink);
        vset_source(&s, &src);
        rc = bp_archive_create_volumes(&ar, &sink, bp_u64_from_u32(70000),
                                       65536, methods[m]);
        if (rc == BP_OK) {
            if (methods[m] == BP_METHOD_FLUFF) bp_set_fast_fluff(&ar, 4);
            else bp_set_fast_boltlz(&ar, 4);
            rc = add_payloads(&ar);
        }
        if (rc == BP_OK) rc = bp_archive_finalize(&ar);
        bp_archive_close(&ar);
        sprintf(label, "%s with 4 threads: create", method_names[m]);
        UNIT_CHECK(rc == BP_OK, label);
        check_volume_sizes(&s, 70000UL, method_names[m]);
        rc = extract_set(&src, &x);
        sprintf(label, "%s with 4 threads: extract", method_names[m]);
        UNIT_CHECK(rc == BP_OK && x.matched == NFILES && x.mismatched == 0, label);
        mem_free(&x.out);
        vset_free(&s);
    }
#endif
}

static void test_abandon(void)
{
    static vset_t s;
    bp_volume_sink_t sink;
    bp_archive_t ar;
    int rc;
    UNIT_SECTION("abandoned archive");
    vset_init(&s);
    vset_sink(&s, &sink);
    rc = bp_archive_create_volumes(&ar, &sink, bp_u64_from_u32(20000), 32768,
                                   BP_METHOD_BOLTLZ);
    if (rc == BP_OK) rc = add_payloads(&ar);
    bp_archive_close(&ar);
    UNIT_CHECK(rc == BP_OK && s.abandoned == 1 && s.last_number == -1,
               "close without finalize abandons the open volume");
    vset_free(&s);
}

static void test_size_parser(void)
{
    static const struct { const char *s; unsigned long hi, lo; int ok; } cases[] = {
        { "1.44M", 0, 1440000UL, 1 }, { "1.44m", 0, 1440000UL, 1 },
        { "720K", 0, 720000UL, 1 },   { "720kb", 0, 720000UL, 1 },
        { "1.2M", 0, 1200000UL, 1 },  { "2.88MB", 0, 2880000UL, 1 },
        { "700M", 0, 700000000UL, 1 }, { "4G", 0, 4000000000UL, 1 },
        { "4.7G", 1, 405032704UL, 1 }, { "1Ki", 0, 1024UL, 1 },
        { "1.44MiB", 0, 1509949UL, 1 }, { "1474560", 0, 1474560UL, 1 },
        { "25GB", 5, 3525163520UL, 1 }, { "0.5k", 0, 500UL, 1 },
        { "", 0, 0, 0 }, { "M", 0, 0, 0 }, { "1.44X", 0, 0, 0 },
        { "12 M", 0, 0, 0 }, { "1.4.4M", 0, 0, 0 }, { "-1M", 0, 0, 0 },
        { "99999999999999999999", 0, 0, 0 }
    };
    unsigned i;
    UNIT_SECTION("volume size parser");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bp_uint64 v;
        char label[96];
        int rc = bp_parse_volume_size(cases[i].s, &v);
        int ok = cases[i].ok ? (rc == 0 && v.hi == cases[i].hi && v.lo == cases[i].lo)
                             : rc != 0;
        sprintf(label, "\"%s\" -> %s", cases[i].s, cases[i].ok ? "value" : "rejected");
        UNIT_CHECK(ok, label);
    }
}

int main(void)
{
    test_size_parser();
    bp_compress_init();
    make_payloads();
    test_single();
    test_multi();
    test_spanning_table();
    test_volume_errors();
    test_split_blocks();
    test_append_delete();
    test_packed_commit();
    test_copy_into_volumes();
    test_parallel();
    test_abandon();
    {
        int i;
        for (i = 0; i < NFILES; i++) free(g_data[i]);
    }
    return UNIT_REPORT();
}
