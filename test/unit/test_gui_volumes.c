/*
 * test_gui_volumes.c -- the disk prompts of the Amiga and Atari ST GUIs
 *
 * Builds src/frontend/common/gui_volumes.h on the POSIX volume hooks with
 * BP_VOL_OWN_PROMPT, as the GUIs do, and plays the user: the "drive" is a
 * directory and the "shelf" holds the other disks.  A scripted prompt swaps
 * disks by moving files between the two, answers with a wrong disk, or
 * presses Cancel.  Covers the paths the GUIs take but that no emulator run
 * reaches: create with a prompt before every volume but the first, extract
 * with disk changes, a wrong disk followed by the right one, Cancel (the GUI
 * stays silent), and opening a damaged single archive without a prompt.
 *
 * POSIX, built with the full core (see the Makefile target gui-volume-test).
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
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

/* ---- FILE streams, as the GUIs make them ------------------------------- */

static long file_read(void *h, void *buf, bp_uint32 n)
{
    return (long)fread(buf, 1, n, (FILE *)h);
}

static long file_write(void *h, const void *buf, bp_uint32 n)
{
    return (long)fwrite(buf, 1, n, (FILE *)h);
}

static int file_seek(void *h, bp_off_t off, int origin)
{
    return fseek((FILE *)h, (long)off, origin);
}

static bp_off_t file_tell(void *h)
{
    return (bp_off_t)ftell((FILE *)h);
}

static void bp_io_from_file(bp_io_t *io, FILE *fp)
{
    io->read = file_read;
    io->write = file_write;
    io->seek = file_seek;
    io->tell = file_tell;
    io->handle = fp;
}

static const char *bp_strerror(int code)
{
    (void)code;
    return "error";
}

/* ---- the prompt: the user, played by a script -------------------------- */

typedef int (*prompt_script_t)(int writing, const char *what,
                               const char *name, const char *why);

static prompt_script_t g_script;
static int g_prompts;
static int g_writing_prompts;
static int g_wrong_disk_prompts;
static char g_last_what[64];
static char g_last_name[256];
static char g_last_why[256];
static char g_drive[200];
static char g_shelf[200];

#define BP_VOL_OWN_PROMPT 1
#define BP_VOL_NO_NOTE 1
#define BP_VOL_VERBOSE_EXPR 0

static const char *gui_vol_prompt_text(int writing, const char *what,
                                       const char *name, const char *problem);
static int g_text_ok = 1;

static int vol_hook_prompt(int writing, const char *what, const char *name,
                           const char *problem)
{
    const char *base = strrchr(name, '/');
    base = base != NULL ? base + 1 : name;
    g_prompts++;
    if (writing) g_writing_prompts++;
    strncpy(g_last_what, what, sizeof(g_last_what) - 1);
    strncpy(g_last_name, name, sizeof(g_last_name) - 1);
    strncpy(g_last_why, problem, sizeof(g_last_why) - 1);
    if (strstr(problem, "wrong volume") != NULL) g_wrong_disk_prompts++;
    /* the text a GUI shows must name the volume and the file wanted */
    {
        const char *text = gui_vol_prompt_text(writing, what, name, problem);
        if (strstr(text, what) == NULL || strstr(text, base) == NULL)
            g_text_ok = 0;
    }
    return g_script != NULL ? g_script(writing, what, name, problem) : 0;
}

#include "../../src/frontend/posix/volume_files.h"
#include "../../src/frontend/common/gui_volumes.h"

/* ---- directories as drive and shelf ------------------------------------ */

static void path_in(char *out, size_t size, const char *dir, const char *base)
{
    snprintf(out, size, "%s/%s", dir, base);
}

static int exists_in(const char *dir, const char *base)
{
    char p[512];
    path_in(p, sizeof(p), dir, base);
    return access(p, F_OK) == 0;
}

static void move_to(const char *from, const char *to, const char *base)
{
    char a[512], b[512];
    path_in(a, sizeof(a), from, base);
    path_in(b, sizeof(b), to, base);
    rename(a, b);
}

/* every file in the drive goes to the shelf: the disk is taken out */
static void eject(void)
{
    DIR *d = opendir(g_drive);
    struct dirent *e;
    char names[64][64];
    int n = 0, i;
    if (d == NULL) return;
    while ((e = readdir(d)) != NULL && n < 64)
        if (e->d_name[0] != '.') strcpy(names[n++], e->d_name);
    closedir(d);
    for (i = 0; i < n; i++) move_to(g_drive, g_shelf, names[i]);
}

/* the disk with this file is put in the drive */
static void insert(const char *base)
{
    eject();
    move_to(g_shelf, g_drive, base);
}

static void copy_as(const char *from_dir, const char *from, const char *to_dir,
                    const char *to)
{
    char a[512], b[512], buf[4096];
    FILE *in, *out;
    size_t n;
    path_in(a, sizeof(a), from_dir, from);
    path_in(b, sizeof(b), to_dir, to);
    in = fopen(a, "rb");
    out = fopen(b, "wb");
    if (in == NULL || out == NULL) return;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
}

static void clear_dirs(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -f '%s'/* '%s'/*", g_drive, g_shelf);
    (void)system(cmd);
}

static void reset_counters(void)
{
    g_script = NULL;
    g_prompts = g_writing_prompts = g_wrong_disk_prompts = 0;
    g_text_ok = 1;
    g_last_what[0] = g_last_name[0] = g_last_why[0] = '\0';
}

/* ---- the archive ------------------------------------------------------- */

#define DATA_SIZE 300000UL
#define VOL_SIZE  100000UL

static bp_uint8 g_data[DATA_SIZE];

static void fill_data(void)
{
    unsigned long i;
    bp_uint32 x = 12345UL;
    for (i = 0; i < DATA_SIZE; i++) {
        x = x * 1103515245UL + 12345UL;    /* incompressible, so 4 volumes */
        g_data[i] = (bp_uint8)(x >> 16);
    }
}

/* write drive/set.blt as a set of VOL_SIZE volumes through the GUI plumbing */
static int create_set(const char *name)
{
    char path[512];
    bp_volume_sink_t sink;
    bp_archive_t ar;
    mem_t m;
    bp_io_t io;
    int rc;

    path_in(path, sizeof(path), g_drive, name);
    if (gui_vol_begin_write(path) != 0) return -100;
    vol_sink_init(&gui_vol, &sink);
    memset(&ar, 0, sizeof(ar));
    rc = bp_archive_create_volumes(&ar, &sink, bp_u64_from_u32(VOL_SIZE), 0,
                                   BP_METHOD_BOLTLZ);
    if (rc != BP_OK) return rc;
    memset(&m, 0, sizeof(m));
    m.data = g_data;
    m.len = m.cap = DATA_SIZE;
    mem_io(&io, &m);
    rc = bp_archive_add_entry_ex(&ar, "data.bin", BP_TYPE_DATA, BP_OS_LINUX,
                                 &io, bp_u64_from_u32(DATA_SIZE),
                                 bp_u64_from_u32(1700000000UL),
                                 BP_ENTRY_FLAG_MTIME_VALID);
    if (rc == BP_OK) rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    return rc;
}

typedef struct {
    mem_t out;
    bp_io_t io;
    int matched;
    int mismatched;
} xctx_t;

static bp_io_t *x_open(const bp_entry_t *e, void *user)
{
    xctx_t *x = (xctx_t *)user;
    (void)e;
    free(x->out.data);
    memset(&x->out, 0, sizeof(x->out));
    mem_io(&x->io, &x->out);
    return &x->io;
}

static int x_done(const bp_entry_t *e, bp_io_t *out, int status, void *user)
{
    xctx_t *x = (xctx_t *)user;
    (void)e;
    (void)out;
    if (status == BP_OK && x->out.len == DATA_SIZE &&
        memcmp(x->out.data, g_data, DATA_SIZE) == 0)
        x->matched++;
    else
        x->mismatched++;
    return BP_OK;
}

static int extract_set(const char *name, xctx_t *x)
{
    char path[512];
    int rc;
    memset(x, 0, sizeof(*x));
    path_in(path, sizeof(path), g_drive, name);
    if (gui_vol_begin_read(path) != 0) return -100;
    rc = bp_archive_extract_volumes(&gui_vol_src, x_open, x_done, NULL, x);
    vol_source_done(&gui_vol);
    free(x->out.data);
    x->out.data = NULL;
    return rc;
}

/* ---- scripts ----------------------------------------------------------- */

/* writing: take the disk out, as the user would, and offer an empty one */
static int script_swap_out(int writing, const char *what, const char *name,
                           const char *why)
{
    (void)writing; (void)what; (void)name; (void)why;
    eject();
    return 1;
}

/* reading: put the disk that holds the wanted file in the drive */
static int script_insert_wanted(int writing, const char *what,
                                const char *name, const char *why)
{
    const char *base = strrchr(name, '/');
    (void)writing; (void)what; (void)why;
    base = base != NULL ? base + 1 : name;
    insert(base);
    return 1;
}

/* reading: the first time, the wrong disk (volume 1 under the wanted name) */
static int g_wrong_given;
static int script_wrong_first(int writing, const char *what, const char *name,
                              const char *why)
{
    const char *base = strrchr(name, '/');
    base = base != NULL ? base + 1 : name;
    if (!g_wrong_given && strcmp(base, "set.000") != 0) {
        g_wrong_given = 1;
        eject();
        copy_as(g_shelf, "set.000", g_drive, base);
        return 1;
    }
    /* a wrong file sits in the drive: take it out, then the right disk */
    {
        char p[512];
        path_in(p, sizeof(p), g_drive, base);
        remove(p);
    }
    return script_insert_wanted(writing, what, name, why);
}

static int script_cancel(int writing, const char *what, const char *name,
                         const char *why)
{
    (void)writing; (void)what; (void)name; (void)why;
    return 0;
}

/* ---- tests ------------------------------------------------------------- */

static void test_create(void)
{
    int rc;
    UNIT_SECTION("create: prompt before every volume but the first");
    clear_dirs();
    reset_counters();
    g_script = script_swap_out;
    rc = create_set("set.blt");
    UNIT_CHECK(rc == BP_OK, "set written");
    UNIT_CHECK(g_prompts == 3, "3 prompts for a 4 volume set (none for volume 1)");
    UNIT_CHECK(g_writing_prompts == 3, "all of them ask for an empty disk");
    UNIT_CHECK(strcmp(g_last_what, "volume 4") == 0, "the last prompt is for volume 4");
    UNIT_CHECK(g_text_ok, "prompt text names the volume and the file");
    UNIT_CHECK(exists_in(g_drive, "set.blt") && !exists_in(g_drive, "set.000"),
               "only the last volume is on the last disk");
    UNIT_CHECK(exists_in(g_shelf, "set.000") && exists_in(g_shelf, "set.001") &&
               exists_in(g_shelf, "set.002"), "the earlier volumes went to earlier disks");
    UNIT_CHECK(gui_vol.written == 4, "four volumes written");
}

static void test_create_cancel(void)
{
    int rc;
    UNIT_SECTION("create: Cancel at a prompt");
    clear_dirs();
    reset_counters();
    g_script = script_cancel;
    rc = create_set("set.blt");
    UNIT_CHECK(rc != BP_OK, "create fails");
    UNIT_CHECK(gui_vol.aborted, "the volume code records the user's Cancel");
    UNIT_CHECK(gui_vol_failure(rc) == NULL, "the GUI shows no error");
    UNIT_CHECK(!exists_in(g_drive, "set.blt"), "no archive is left behind");
}

static void test_extract_swaps(void)
{
    xctx_t x;
    int rc;
    UNIT_SECTION("extract: disk changes, starting from the last disk");
    clear_dirs();
    reset_counters();
    g_script = script_swap_out;
    UNIT_CHECK(create_set("set.blt") == BP_OK, "set written");
    reset_counters();
    g_script = script_insert_wanted;
    rc = extract_set("set.blt", &x);
    UNIT_CHECK(rc == BP_OK, "extract succeeds");
    UNIT_CHECK(x.matched == 1 && x.mismatched == 0, "data is intact");
    UNIT_CHECK(g_prompts >= 3, "disks were asked for");
    UNIT_CHECK(g_writing_prompts == 0, "as media to read, not to write");
    UNIT_CHECK(g_text_ok, "prompt text names the volume and the file");
}

static void test_extract_wrong_disk(void)
{
    xctx_t x;
    int rc;
    UNIT_SECTION("extract: a wrong disk, then the right one");
    clear_dirs();
    reset_counters();
    g_script = script_swap_out;
    UNIT_CHECK(create_set("set.blt") == BP_OK, "set written");
    reset_counters();
    g_wrong_given = 0;
    g_script = script_wrong_first;
    rc = extract_set("set.blt", &x);
    UNIT_CHECK(g_wrong_given, "the wrong disk was offered");
    UNIT_CHECK(g_wrong_disk_prompts >= 1, "it was refused as the wrong volume");
    UNIT_CHECK(rc == BP_OK && x.matched == 1 && x.mismatched == 0,
               "the right disk then completes the extraction");
}

static void test_extract_cancel(void)
{
    xctx_t x;
    int rc;
    UNIT_SECTION("extract: Cancel at a prompt");
    clear_dirs();
    reset_counters();
    g_script = script_swap_out;
    UNIT_CHECK(create_set("set.blt") == BP_OK, "set written");
    reset_counters();
    g_script = script_cancel;
    rc = extract_set("set.blt", &x);
    UNIT_CHECK(rc != BP_OK, "extraction stops");
    UNIT_CHECK(g_prompts == 1, "after one prompt");
    UNIT_CHECK(gui_vol.aborted && gui_vol_failure(rc) == NULL,
               "and the GUI shows no error");
}

static void test_open_reads_last_volume_only(void)
{
    char path[512];
    bp_archive_info_t info;
    bp_uint32 sidecars;
    int rc;
    UNIT_SECTION("open: the last disk alone is enough");
    clear_dirs();
    reset_counters();
    g_script = script_swap_out;
    UNIT_CHECK(create_set("set.blt") == BP_OK, "set written");
    reset_counters();
    path_in(path, sizeof(path), g_drive, "set.blt");
    UNIT_CHECK(gui_vol_begin_read(path) == 0, "source prepared");
    gui_vol.interactive = 0;      /* as do_open() sets it */
    memset(&info, 0, sizeof(info));
    rc = bp_archive_info_volumes(&gui_vol_src, &info, &sidecars);
    vol_source_done(&gui_vol);
    UNIT_CHECK(rc == BP_OK, "info works with only the last volume present");
    UNIT_CHECK(info.volume_count == 4, "and reports 4 volumes");
    UNIT_CHECK(g_prompts == 0, "without a prompt");
}

static void test_open_damaged_single(void)
{
    char path[512];
    bp_archive_info_t info;
    bp_uint32 sidecars;
    int rc;
    FILE *fp;
    UNIT_SECTION("open: a damaged single archive is not a disk prompt");
    clear_dirs();
    reset_counters();
    path_in(path, sizeof(path), g_drive, "bad.blt");
    fp = fopen(path, "wb");
    fwrite("BOLT\7\0garbage", 1, 13, fp);
    fclose(fp);
    UNIT_CHECK(gui_vol_begin_read(path) == 0, "source prepared");
    gui_vol.interactive = 0;
    memset(&info, 0, sizeof(info));
    rc = bp_archive_info_volumes(&gui_vol_src, &info, &sidecars);
    vol_source_done(&gui_vol);
    UNIT_CHECK(rc != BP_OK, "open fails");
    UNIT_CHECK(g_prompts == 0, "with no prompt");
    UNIT_CHECK(gui_vol_failure(rc) != NULL && gui_vol.problem[0] != '\0',
               "and with a reason to show");
    /* the same file with prompts on (what the other operations do) asks */
    g_script = script_cancel;
    UNIT_CHECK(gui_vol_begin_read(path) == 0, "source prepared again");
    rc = bp_archive_info_volumes(&gui_vol_src, &info, &sidecars);
    vol_source_done(&gui_vol);
    UNIT_CHECK(rc != BP_OK, "still fails");
}

int main(int argc, char **argv)
{
    char root[] = "/tmp/bp_gui_volumes_XXXXXX";
    char cmd[300];

    (void)argc;
    (void)argv;
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(g_drive, sizeof(g_drive), "%s/drive", root);
    snprintf(g_shelf, sizeof(g_shelf), "%s/shelf", root);
    mkdir(g_drive, 0700);
    mkdir(g_shelf, 0700);
    fill_data();

    test_create();
    test_create_cancel();
    test_extract_swaps();
    test_extract_wrong_disk();
    test_extract_cancel();
    test_open_reads_last_volume_only();
    test_open_damaged_single();

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    (void)system(cmd);
    return UNIT_REPORT();
}
