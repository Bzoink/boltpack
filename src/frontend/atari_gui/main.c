/*
 * main.c -- Atari GEM GUI frontend for BoltPack
 *
 * Target:   Atari ST/STE/TT/Falcon, TOS 1.04+ with GEM/AES
 * Compiler: m68k-atari-mint-gcc (Vincent Riviere's cross-toolchain)
 *
 * Uses GEM/AES for windowed UI: menus, file selector, alert boxes.
 * File list is drawn via VDI text calls inside a standard GEM window.
 * No resource file required -- menus are built in code.
 *
 * ANSI C (C89/C90) throughout.
 *
 * Cross-compilation from Linux:
 *   m68k-atari-mint-gcc -ansi -Wall -Wextra -O2               \
 *       -I src/core                         \
 *       src/core/compress.c src/core/archiver.c                \
 *       src/core/bp_codepage.c                                 \
 *       src/core/fluff.c src/core/boltlite.c                       \
 *       src/frontend/atari_gui/main.c                          \
 *       src/core/boltlz.c                             \
 *       -lgem -o build/atari/boltpack.app
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "../../core/archiver.h"
#include "../../core/bp_metadata.h"
#include "../../core/bp_time.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/boltlite.h"
#include "../../core/bp_codepage.h"
#include "../atari/atari_path.h"

#include <gem.h>            /* AES + VDI (gemlib) */
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

/* mintlib exposes these POSIX interfaces through the target runtime. */
extern int lstat(const char *path, struct stat *st);
extern int mkstemp(char *template_path);
extern int fileno(FILE *stream);
extern FILE *fdopen(int fd, const char *mode);

#ifndef FA_RDONLY
# define FA_RDONLY 0x01
# define FA_HIDDEN 0x02
# define FA_SYSTEM 0x04
# define FA_LABEL  0x08
# define FA_DIR    0x10
# define FA_ARCH   0x20
#endif
#ifndef FA_ARCH
# define FA_ARCH 0x20
#endif

/* ====================================================================== */
/*  Constants                                                             */
/* ====================================================================== */

#define BP_MAX_PATH   512
#define MAX_ENTRIES   2048
#define LINE_HEIGHT   16
#define PROGRESS_PANEL_H  ((LINE_HEIGHT * 4) + 12)
#define ATARI_FLUFF_MIN_FREE  (2304L * 1024L)
#define COL_NAME      8
#define COL_ORIG      198
#define COL_COMP      270
#define COL_RATIO     334
#define COL_METHOD    386
#define COL_DATE      446

#ifndef OS_CHECKED
#define OS_CHECKED 0x0004
#endif

/* Menu tree object indices (built in code, not from RSC). */
#define MN_DESK       3
#define MN_ARCHIVE    4
#define MN_COMPRESS   5
#define MN_HELP       6

#define MENU_APP_TITLE_CHARS       10
#define MENU_ARCHIVE_TITLE_CHARS    9
#define MENU_COMPRESS_TITLE_CHARS  13
#define MENU_HELP_TITLE_CHARS       6
#define MENU_ARCHIVE_X              MENU_APP_TITLE_CHARS
#define MENU_COMPRESS_X             (MENU_ARCHIVE_X + MENU_ARCHIVE_TITLE_CHARS)
#define MENU_HELP_X                 (MENU_COMPRESS_X + MENU_COMPRESS_TITLE_CHARS)

#define MENU_DA_SLOTS 6
#define MENU_PULLDOWNS 7
#define MENU_DESK_BOX 8
#define MN_ABOUT      9
#define MENU_DESK_SEP 10
#define MENU_DA_FIRST 11
#define MENU_DA_LAST  (MENU_DA_FIRST + MENU_DA_SLOTS - 1)

#define MENU_ARCH_BOX  (MENU_DA_LAST + 1)
#define MN_OPEN        (MENU_ARCH_BOX + 1)
#define MN_EXTRACT     (MENU_ARCH_BOX + 2)
#define MN_CLOSE       (MENU_ARCH_BOX + 3)
#define MN_VALIDATE    (MENU_ARCH_BOX + 4)
#define MN_INFO        (MENU_ARCH_BOX + 5)
#define MENU_ARCH_SEP  (MENU_ARCH_BOX + 6)
#define MN_QUIT        (MENU_ARCH_BOX + 7)

#define MENU_COMP_BOX      (MENU_ARCH_BOX + 8)
#define MN_CREATE          (MENU_COMP_BOX + 1)
#define MENU_COMP_SEP      (MENU_COMP_BOX + 2)
#define MN_METHOD_STORED   (MENU_COMP_BOX + 3)
#define MN_METHOD_BOLTLITE     (MENU_COMP_BOX + 4)
#define MN_METHOD_BOLTLZ    (MENU_COMP_BOX + 5)
#define MN_METHOD_FLUFF    (MENU_COMP_BOX + 6)
#define MN_INTERCHANGE_PROFILE (MENU_COMP_BOX + 7)

#define MENU_HELP_BOX (MENU_COMP_BOX + 8)
#define MN_HELP_ABOUT (MENU_HELP_BOX + 1)
#define MN_DOC        (MENU_HELP_BOX + 2)

#define MENU_OBJS   (MN_DOC + 1)

/* ====================================================================== */
/*  Globals                                                               */
/* ====================================================================== */

static short ap_id;
static short vdi_handle;
static short work_in[11], work_out[57];
static short font_w, font_h;
static short scr_w, scr_h;

static OBJECT *menu_tree;
static OBJECT menu_objs[MENU_OBJS];

static short win_handle = -1;
static short win_x, win_y, win_w, win_h;

/* Archive state */
typedef struct {
    char name[256];
    char orig_str[16];
    char comp_str[16];
    char ratio_str[8];
    char method_str[12];
    char date_str[20];
} gui_entry_t;

static gui_entry_t *entries;
static int entry_count;
static int entries_capacity;
static int scroll_pos;

static FILE *archive_fp;
static char archive_path[BP_MAX_PATH];
static bp_archive_info_t archive_info;
static bp_uint8 current_method = BP_METHOD_BOLTLZ;
static int current_interchange_profile;

typedef struct {
    const char   *verb;
    const char   *codec_name;
    double        total_bytes;
    double        done_bytes;
    double        file_bytes;
    double        file_total;
    unsigned long total_files;
    unsigned long current_file;
    clock_t       start_ticks;
    int           active;
    int           total_pct;
    int           last_pct;
    int           last_file_index;
    char          current_name[BP_MAX_PATH];
} atari_progress_ui_t;

static atari_progress_ui_t *active_progress;

static int gather_count;
static char **gather_files;
static unsigned char *gather_is_directory;
static int gather_capacity;
static int gather_skipped_sources;

static void redraw_window(short rx, short ry, short rw, short rh);

/* ====================================================================== */
/*  File I/O adapter                                                      */
/* ====================================================================== */

static long file_read(void *h, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)h);
}
static long file_write(void *h, const void *buf, bp_uint32 count)
{
    return (long)fwrite(buf, 1, (size_t)count, (FILE *)h);
}
static int file_seek(void *h, bp_off_t offset, int origin)
{
    if (offset < (bp_off_t)LONG_MIN || offset > (bp_off_t)LONG_MAX)
        return -1;
    return fseek((FILE *)h, (long)offset, origin);
}
static bp_off_t file_tell(void *h)
{
    long pos = ftell((FILE *)h);
    if (pos < 0L) return (bp_off_t)-1;
    return (bp_off_t)pos;
}

static bp_io_t make_io(FILE *fp)
{
    bp_io_t io;
    io.read   = file_read;
    io.write  = file_write;
    io.seek   = file_seek;
    io.tell   = file_tell;
    io.handle = fp;
    return io;
}

/* ====================================================================== */
/*  Helpers                                                               */
/* ====================================================================== */

static void format_size(double bytes, char *buf, int buflen)
{
    if (bytes < 1024.0)
        sprintf(buf, "%.0f B", bytes);
    else if (bytes < 1024.0 * 1024.0)
        sprintf(buf, "%.1f KB", bytes / 1024.0);
    else
        sprintf(buf, "%.1f MB", bytes / (1024.0 * 1024.0));
    (void)buflen;
}

static void format_size_compact(double bytes, char *buf, int buflen)
{
    if (bytes < 1024.0)
        sprintf(buf, "%.0fB", bytes);
    else if (bytes < 1024.0 * 1024.0)
        sprintf(buf, "%.1fKB", bytes / 1024.0);
    else
        sprintf(buf, "%.1fMB", bytes / (1024.0 * 1024.0));
    (void)buflen;
}

static double u64_to_dbl(bp_uint64 v)
{
    return (double)v.lo + (double)v.hi * 4294967296.0;
}

static unsigned long long u64_to_ull(bp_uint64 v)
{
    return ((unsigned long long)v.hi << 32) | (unsigned long long)v.lo;
}

static void format_size_compact_u64(bp_uint64 bytes, char *buf, int buflen)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB", "EB" };
    unsigned long long value;
    unsigned long long scale;
    unsigned long whole;
    unsigned long frac;
    int unit;

    value = u64_to_ull(bytes);
    scale = 1ULL;
    unit = 0;

    while (unit < 6 && value >= scale * 1024ULL) {
        scale *= 1024ULL;
        unit++;
    }

    if (unit == 0) {
        sprintf(buf, "%luB", (unsigned long)value);
        (void)buflen;
        return;
    }

    whole = (unsigned long)(value / scale);
    frac = 0UL;
    if (whole < 100UL)
        frac = (unsigned long)(((value % scale) * 10ULL) / scale);

    if (frac != 0UL)
        sprintf(buf, "%lu.%lu%s", whole, frac, units[unit]);
    else
        sprintf(buf, "%lu%s", whole, units[unit]);
    (void)buflen;
}

static void show_alert(const char *msg);

static char *dup_string(const char *src)
{
    char *dst;
    size_t len;

    if (src == NULL)
        return NULL;

    len = strlen(src) + 1;
    dst = (char *)malloc(len);
    if (dst == NULL)
        return NULL;

    memcpy(dst, src, len);
    return dst;
}

static void free_entries(void)
{
    if (entries != NULL) {
        free(entries);
        entries = NULL;
    }
    entry_count = 0;
    entries_capacity = 0;
    scroll_pos = 0;
}

static int alloc_entries(int count)
{
    size_t bytes;

    free_entries();

    if (count <= 0)
        return 1;

    bytes = (size_t)count * sizeof(gui_entry_t);
    entries = (gui_entry_t *)malloc(bytes);
    if (entries == NULL)
        return 0;

    entries_capacity = count;
    return 1;
}

static void free_path_array(char **paths, int count)
{
    int i;

    if (paths == NULL)
        return;

    for (i = 0; i < count; i++) {
        if (paths[i] != NULL) {
            free(paths[i]);
            paths[i] = NULL;
        }
    }
}

static void free_gather_files(void)
{
    if (gather_files != NULL) {
        free_path_array(gather_files, gather_count);
        free(gather_files);
        gather_files = NULL;
    }
    free(gather_is_directory);
    gather_is_directory = NULL;

    gather_count = 0;
    gather_capacity = 0;
    gather_skipped_sources = 0;
}

static int ensure_gather_capacity(int needed)
{
    char **new_files;
    unsigned char *new_types;
    int new_capacity;

    if (needed <= gather_capacity)
        return 1;

    new_capacity = gather_capacity ? gather_capacity * 2 : 64;
    while (new_capacity < needed)
        new_capacity *= 2;

    new_files = (char **)malloc((size_t)new_capacity * sizeof(char *));
    new_types = (unsigned char *)malloc((size_t)new_capacity);
    if (new_files == NULL || new_types == NULL) {
        free(new_files);
        free(new_types);
        return 0;
    }
    if (gather_count != 0) {
        memcpy(new_files, gather_files, (size_t)gather_count * sizeof(char *));
        memcpy(new_types, gather_is_directory, (size_t)gather_count);
    }
    free(gather_files);
    free(gather_is_directory);
    gather_files = new_files;
    gather_is_directory = new_types;
    gather_capacity = new_capacity;
    return 1;
}

static int add_gather_entry(const char *path, int is_directory)
{
    char *copy;
    int i;

    for (i = 0; i < gather_count; i++) {
        if (gather_is_directory[i] == (unsigned char)(is_directory != 0) &&
            strcmp(gather_files[i], path) == 0) return 0;
    }

    if (gather_count >= MAX_ENTRIES)
        return -2;

    if (!ensure_gather_capacity(gather_count + 1))
        return -1;

    copy = dup_string(path);
    if (copy == NULL)
        return -1;

    gather_files[gather_count] = copy;
    gather_is_directory[gather_count] = (unsigned char)(is_directory != 0);
    gather_count++;
    return 0;
}

static int add_gather_file(const char *path)
{
    return add_gather_entry(path, 0);
}

static int add_gather_directory(const char *path)
{
    return add_gather_entry(path, 1);
}

static const char *method_name(int m)
{
    switch (m) {
        case 0: return "Stored";
        case 1: return "BoltLite";
        case 2: return "boltlz";
        case 3: return "fluff";
    }
    return "?";
}

static const char *format_methods(const bp_uint32 *counts)
{
    static char buf[128];
    static const char * const names[4] = {"Stored", "BoltLite", "boltlz", "Fluff"};
    int pos = 0;
    int first = 1;
    int i;
    buf[0] = '\0';
    for (i = 0; i < 4; i++) {
        if (counts[i] == 0) continue;
        if (!first) { buf[pos++] = ','; buf[pos++] = ' '; }
        pos += sprintf(buf + pos, "%s (%lu)", names[i], (unsigned long)counts[i]);
        first = 0;
    }
    if (first) strcpy(buf, "none");
    return buf;
}

static void format_rate(double bytes_per_sec, char *buf, int buflen)
{
    char size_buf[32];

    if (bytes_per_sec <= 0.0) {
        strcpy(buf, "-");
        return;
    }

    format_size(bytes_per_sec, size_buf, sizeof(size_buf));
    sprintf(buf, "%s/s", size_buf);
    (void)buflen;
}

static long atari_largest_free_block(void)
{
    long avail;

    avail = (long)Malloc(-1L);
    if (avail < 0L)
        return 0L;
    return avail;
}

static int fluff_memory_ok(void)
{
    return atari_largest_free_block() >= ATARI_FLUFF_MIN_FREE;
}

static void show_fluff_memory_alert(long avail)
{
    char need_buf[32];
    char avail_buf[32];
    char msg[192];

    format_size((double)ATARI_FLUFF_MIN_FREE, need_buf, sizeof(need_buf));
    format_size((double)avail, avail_buf, sizeof(avail_buf));
    sprintf(msg, "Fluff needs about %s|Largest free block: %s",
            need_buf, avail_buf);
    show_alert(msg);
}

static void show_alert(const char *msg)
{
    char buf[256];
    sprintf(buf, "[1][%s][ OK ]", msg);
    form_alert(1, buf);
}

static void show_about_dialog(void)
{
    char free_buf[32];
    char msg[256];

    format_size((double)atari_largest_free_block(),
                free_buf, sizeof(free_buf));
    sprintf(msg,
            "BoltPack 1.1|Portable archive format|Stored, BoltLite, boltlz, fluff|Largest free block: %s",
            free_buf);
    show_alert(msg);
}

static void set_main_window_chrome(void)
{
    char title[128];
    const char *base;

    if (win_handle < 0)
        return;

    if (archive_path[0] != '\0' && archive_path[0] != '\x01') {
        base = strrchr(archive_path, '\\');
        if (!base)
            base = strrchr(archive_path, '/');
        base = base ? base + 1 : archive_path;
        sprintf(title, " BoltPack - %s ", base);
    } else {
        strcpy(title, " BoltPack ");
    }

    wind_set_str(win_handle, WF_NAME, title);
}

static void compact_progress_name(const char *src, char *dst, int dst_size)
{
    int len;
    int keep;

    if (dst_size <= 0)
        return;

    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = (int)strlen(src);
    if (len < dst_size) {
        strcpy(dst, src);
        return;
    }

    if (dst_size < 5) {
        strncpy(dst, src, (size_t)(dst_size - 1));
        dst[dst_size - 1] = '\0';
        return;
    }

    keep = dst_size - 4;
    strcpy(dst, "...");
    strncpy(dst + 3, src + len - keep, (size_t)keep);
    dst[keep + 3] = '\0';
}

static void sync_compression_menu_checks(void)
{
    menu_objs[MN_METHOD_STORED].ob_state &= (short)~OS_CHECKED;
    menu_objs[MN_METHOD_BOLTLITE].ob_state &= (short)~OS_CHECKED;
    menu_objs[MN_METHOD_BOLTLZ].ob_state &= (short)~OS_CHECKED;
    menu_objs[MN_METHOD_FLUFF].ob_state &= (short)~OS_CHECKED;
    menu_objs[MN_INTERCHANGE_PROFILE].ob_state &= (short)~OS_CHECKED;
    if (current_interchange_profile)
        menu_objs[MN_INTERCHANGE_PROFILE].ob_state |= OS_CHECKED;

    switch (current_method) {
        case BP_METHOD_STORED:
            menu_objs[MN_METHOD_STORED].ob_state |= OS_CHECKED;
            break;
        case BP_METHOD_BOLTLITE:
            menu_objs[MN_METHOD_BOLTLITE].ob_state |= OS_CHECKED;
            break;
        case BP_METHOD_FLUFF:
            menu_objs[MN_METHOD_FLUFF].ob_state |= OS_CHECKED;
            break;
        default:
            current_method = BP_METHOD_BOLTLZ;
            menu_objs[MN_METHOD_BOLTLZ].ob_state |= OS_CHECKED;
            break;
    }
}

static int select_compression_method(bp_uint8 method)
{
    if (method == BP_METHOD_FLUFF) {
        long avail;

        avail = atari_largest_free_block();
        if (avail < ATARI_FLUFF_MIN_FREE) {
            show_fluff_memory_alert(avail);
            return 0;
        }
    }

    current_method = method;
    current_interchange_profile = 0;
    sync_compression_menu_checks();
    return 1;
}

static void toggle_interchange_profile(void)
{
    current_interchange_profile = !current_interchange_profile;
    if (current_interchange_profile)
        current_method = BP_METHOD_BOLTLITE;
    sync_compression_menu_checks();
}

static int ensure_selected_method_available(void)
{
    long avail;

    if (current_method != BP_METHOD_FLUFF)
        return 1;

    avail = atari_largest_free_block();
    if (avail >= ATARI_FLUFF_MIN_FREE)
        return 1;

    show_fluff_memory_alert(avail);
    return 0;
}

static void restore_main_window_after_dialog(void)
{
    if (win_handle < 0)
        return;

    wind_set(win_handle, WF_TOP, 0, 0, 0, 0);
    set_main_window_chrome();
    redraw_window(win_x, win_y, win_w, win_h);
}

static void start_progress_ui(atari_progress_ui_t *ctx, const char *verb,
                              const char *codec_name,
                              double total_bytes, unsigned long total_files)
{
    ctx->verb = verb;
    ctx->codec_name = codec_name;
    ctx->total_bytes = total_bytes;
    ctx->done_bytes = 0.0;
    ctx->file_bytes = 0.0;
    ctx->file_total = 0.0;
    ctx->total_files = total_files;
    ctx->current_file = 0UL;
    ctx->start_ticks = clock();
    ctx->active = 1;
    ctx->total_pct = 0;
    ctx->last_pct = -1;
    ctx->last_file_index = -1;
    ctx->current_name[0] = '\0';
    active_progress = ctx;

    redraw_window(win_x, win_y, win_w, win_h);
}

static int update_progress_ui(const bp_progress_t *p, void *user_data)
{
    atari_progress_ui_t *ctx = (atari_progress_ui_t *)user_data;
    double done;
    double total;
    unsigned long file_count;
    int pct;
    int file_index;

    done = u64_to_dbl(p->archive_orig_done);
    total = ctx->total_bytes > 0.0 ? ctx->total_bytes
                                   : u64_to_dbl(p->archive_orig_total);
    pct = total > 0.0 ? (int)((done * 100.0) / total) : 0;
    if (pct > 100)
        pct = 100;

    file_index = (int)p->file_index;
    if (pct == ctx->last_pct && file_index == ctx->last_file_index)
        return 0;

    file_count = p->file_count ? (unsigned long)p->file_count : ctx->total_files;

    ctx->done_bytes = done;
    ctx->total_bytes = total;
    ctx->file_bytes = u64_to_dbl(p->file_bytes_done);
    ctx->file_total = u64_to_dbl(p->file_orig_size);
    ctx->current_file = (unsigned long)(file_index + 1);
    ctx->total_pct = pct;
    if (file_count > 0)
        ctx->total_files = file_count;
    if (p->file_name) {
        strncpy(ctx->current_name, p->file_name,
                sizeof(ctx->current_name) - 1);
        ctx->current_name[sizeof(ctx->current_name) - 1] = '\0';
    } else {
        ctx->current_name[0] = '\0';
    }

    redraw_window(win_x, win_y, win_w, win_h);

    ctx->last_pct = pct;
    ctx->last_file_index = file_index;
    return 0;
}

static void stop_progress_ui(atari_progress_ui_t *ctx)
{
    if (ctx) {
        ctx->active = 0;
        ctx->current_name[0] = '\0';
    }

    if (active_progress == ctx)
        active_progress = NULL;

    set_main_window_chrome();
    redraw_window(win_x, win_y, win_w, win_h);
}

static short progress_panel_height(void)
{
    if (active_progress && active_progress->active)
        return PROGRESS_PANEL_H;
    return 0;
}

static void draw_progress_panel(short clip_x, short clip_y, short clip_w)
{
    atari_progress_ui_t *ctx;
    short pxy[4];
    short y;
    char line[160];
    char done_buf[32];
    char total_buf[32];
    char rate_buf[32];
    char name_buf[96];
    double elapsed;
    double rate;

    ctx = active_progress;
    if (!ctx || !ctx->active)
        return;

    y = clip_y + 2;
    if (ctx->codec_name && ctx->codec_name[0] != '\0')
        sprintf(line, "%s with %s", ctx->verb, ctx->codec_name);
    else
        strcpy(line, ctx->verb);
    v_gtext(vdi_handle, clip_x + COL_NAME, y, line);

    y += LINE_HEIGHT;
    if (ctx->total_files > 0)
        sprintf(line, "Files: %lu/%lu   Total: %d%%",
                ctx->current_file, ctx->total_files, ctx->total_pct);
    else
        sprintf(line, "Total: %d%%", ctx->total_pct);
    v_gtext(vdi_handle, clip_x + COL_NAME, y, line);

    y += LINE_HEIGHT;
    format_size(ctx->done_bytes, done_buf, sizeof(done_buf));
    format_size(ctx->total_bytes, total_buf, sizeof(total_buf));
    elapsed = (double)(clock() - ctx->start_ticks) / (double)CLOCKS_PER_SEC;
    if (elapsed > 0.1 && ctx->done_bytes > 0.0) {
        rate = ctx->done_bytes / elapsed;
        format_rate(rate, rate_buf, sizeof(rate_buf));
        sprintf(line, "Data: %s / %s   Speed: %s",
                done_buf, total_buf, rate_buf);
    } else {
        sprintf(line, "Data: %s / %s", done_buf, total_buf);
    }
    v_gtext(vdi_handle, clip_x + COL_NAME, y, line);

    y += LINE_HEIGHT;
    compact_progress_name(ctx->current_name, name_buf, sizeof(name_buf));
    if (name_buf[0] != '\0')
        sprintf(line, "Current: %s", name_buf);
    else
        strcpy(line, "Current: Preparing...");
    v_gtext(vdi_handle, clip_x + COL_NAME, y, line);

    pxy[0] = clip_x;
    pxy[1] = clip_y + PROGRESS_PANEL_H - 1;
    pxy[2] = clip_x + clip_w - 1;
    pxy[3] = pxy[1];
    v_pline(vdi_handle, 2, pxy);
}

/* ====================================================================== */
/*  Menu bar (built programmatically)                                     */
/* ====================================================================== */

/*
 * GEM menus are OBJECT trees.  EmuTOS/TOS expects this fixed shape:
 *   0 = screen root
 *   1 = menu bar box
 *   2 = active title strip
 *   3+ = actual menu titles
 *   root.ob_tail = pull-down container
 * The first pull-down must be the Desk menu, and menu_bar() fills the
 * contiguous objects after About+separator with desk accessory titles.
 * Atari TOS supports up to 6 desk accessories, so reserve exactly 6 slots.
 */
static void build_menu(void)
{
    int i;

    for (i = 0; i < MENU_OBJS; i++) {
        memset(&menu_objs[i], 0, sizeof(OBJECT));
        menu_objs[i].ob_next  = -1;
        menu_objs[i].ob_head  = -1;
        menu_objs[i].ob_tail  = -1;
        menu_objs[i].ob_flags = OF_NONE;
        menu_objs[i].ob_state = OS_NORMAL;
    }

    menu_tree = menu_objs;

    /* 0: screen root */
    menu_objs[0].ob_type   = G_IBOX;
    menu_objs[0].ob_head   = 1;
    menu_objs[0].ob_tail   = MENU_PULLDOWNS;
    menu_objs[0].ob_x      = 0;
    menu_objs[0].ob_y      = 0;
    menu_objs[0].ob_width  = scr_w;
    menu_objs[0].ob_height = scr_h;

    /* 1: menu bar */
    menu_objs[1].ob_type   = G_BOX;
    menu_objs[1].ob_next   = MENU_PULLDOWNS;
    menu_objs[1].ob_head   = 2;
    menu_objs[1].ob_tail   = 2;
    menu_objs[1].ob_x      = 0;
    menu_objs[1].ob_y      = 0;
    menu_objs[1].ob_width  = scr_w;
    menu_objs[1].ob_height = font_h + 2;
    menu_objs[1].ob_spec.index = 0x00FF1100L;

    /* 2: active title strip */
    menu_objs[2].ob_type   = G_IBOX;
    menu_objs[2].ob_next   = 1;
    menu_objs[2].ob_head   = MN_DESK;
    menu_objs[2].ob_tail   = MN_HELP;
    menu_objs[2].ob_x      = 0;
    menu_objs[2].ob_y      = 0;
    menu_objs[2].ob_width  = scr_w;
    menu_objs[2].ob_height = font_h;

    /* 3-6: actual titles. The Desk title identifies this application. */
    menu_objs[MN_DESK].ob_type   = G_TITLE;
    menu_objs[MN_DESK].ob_next   = MN_ARCHIVE;
    menu_objs[MN_DESK].ob_spec.free_string = " BoltPack ";
    menu_objs[MN_DESK].ob_x      = 0;
    menu_objs[MN_DESK].ob_y      = 0;
    menu_objs[MN_DESK].ob_width  = (short)(MENU_APP_TITLE_CHARS * font_w);
    menu_objs[MN_DESK].ob_height = font_h + 2;

    menu_objs[MN_ARCHIVE].ob_type   = G_TITLE;
    menu_objs[MN_ARCHIVE].ob_next   = MN_COMPRESS;
    menu_objs[MN_ARCHIVE].ob_spec.free_string = " Archive ";
    menu_objs[MN_ARCHIVE].ob_x      = (short)(MENU_ARCHIVE_X * font_w);
    menu_objs[MN_ARCHIVE].ob_y      = 0;
    menu_objs[MN_ARCHIVE].ob_width  = (short)(9 * font_w);
    menu_objs[MN_ARCHIVE].ob_height = font_h + 2;

    menu_objs[MN_COMPRESS].ob_type   = G_TITLE;
    menu_objs[MN_COMPRESS].ob_next   = MN_HELP;
    menu_objs[MN_COMPRESS].ob_spec.free_string = " Compression ";
    menu_objs[MN_COMPRESS].ob_x      = (short)(MENU_COMPRESS_X * font_w);
    menu_objs[MN_COMPRESS].ob_y      = 0;
    menu_objs[MN_COMPRESS].ob_width  = (short)(13 * font_w);
    menu_objs[MN_COMPRESS].ob_height = font_h + 2;

    menu_objs[MN_HELP].ob_type   = G_TITLE;
    menu_objs[MN_HELP].ob_next   = 2;
    menu_objs[MN_HELP].ob_spec.free_string = " Help ";
    menu_objs[MN_HELP].ob_x      = (short)(MENU_HELP_X * font_w);
    menu_objs[MN_HELP].ob_y      = 0;
    menu_objs[MN_HELP].ob_width  = (short)(6 * font_w);
    menu_objs[MN_HELP].ob_height = font_h + 2;

    /* 7: pull-down container */
    menu_objs[MENU_PULLDOWNS].ob_type   = G_IBOX;
    menu_objs[MENU_PULLDOWNS].ob_next   = 0;
    menu_objs[MENU_PULLDOWNS].ob_head   = MENU_DESK_BOX;
    menu_objs[MENU_PULLDOWNS].ob_tail   = MENU_HELP_BOX;
    menu_objs[MENU_PULLDOWNS].ob_x      = 0;
    menu_objs[MENU_PULLDOWNS].ob_y      = font_h + 2;
    menu_objs[MENU_PULLDOWNS].ob_width  = scr_w;
    menu_objs[MENU_PULLDOWNS].ob_height = scr_h;

    /* Desk menu.  menu_bar() rewrites its child list during install. */
    menu_objs[MENU_DESK_BOX].ob_type   = G_BOX;
    menu_objs[MENU_DESK_BOX].ob_next   = MENU_ARCH_BOX;
    menu_objs[MENU_DESK_BOX].ob_head   = MN_ABOUT;
    menu_objs[MENU_DESK_BOX].ob_tail   = MENU_DA_LAST;
    menu_objs[MENU_DESK_BOX].ob_x      = 0;
    menu_objs[MENU_DESK_BOX].ob_y      = 0;
    menu_objs[MENU_DESK_BOX].ob_width  = (short)(22 * font_w);
    menu_objs[MENU_DESK_BOX].ob_height = (short)((MENU_DA_SLOTS + 2) * (font_h + 2));
    menu_objs[MENU_DESK_BOX].ob_spec.index = 0x00FF1100L;

    menu_objs[MN_ABOUT].ob_type   = G_STRING;
    menu_objs[MN_ABOUT].ob_next   = MENU_DESK_SEP;
    menu_objs[MN_ABOUT].ob_spec.free_string = "  About BoltPack     ";
    menu_objs[MN_ABOUT].ob_x      = 0;
    menu_objs[MN_ABOUT].ob_y      = 0;
    menu_objs[MN_ABOUT].ob_width  = (short)(22 * font_w);
    menu_objs[MN_ABOUT].ob_height = font_h + 2;

    menu_objs[MENU_DESK_SEP].ob_type   = G_STRING;
    menu_objs[MENU_DESK_SEP].ob_next   = MENU_DA_FIRST;
    menu_objs[MENU_DESK_SEP].ob_spec.free_string = "---------------------";
    menu_objs[MENU_DESK_SEP].ob_x      = 0;
    menu_objs[MENU_DESK_SEP].ob_y      = font_h + 2;
    menu_objs[MENU_DESK_SEP].ob_width  = (short)(22 * font_w);
    menu_objs[MENU_DESK_SEP].ob_height = font_h + 2;
    menu_objs[MENU_DESK_SEP].ob_state  = OS_DISABLED;

    {
        static char da_blank[] = "                     ";
        int d;

        for (d = 0; d < MENU_DA_SLOTS; d++) {
            int obj = MENU_DA_FIRST + d;
            menu_objs[obj].ob_type = G_STRING;
            menu_objs[obj].ob_next = (obj < MENU_DA_LAST) ? obj + 1 : MENU_DESK_BOX;
            menu_objs[obj].ob_spec.free_string = da_blank;
            menu_objs[obj].ob_x      = 0;
            menu_objs[obj].ob_y      = (short)((2 + d) * (font_h + 2));
            menu_objs[obj].ob_width  = (short)(22 * font_w);
            menu_objs[obj].ob_height = font_h + 2;
        }
    }

    /* Archive menu. */
    menu_objs[MENU_ARCH_BOX].ob_type   = G_BOX;
    menu_objs[MENU_ARCH_BOX].ob_next   = MENU_COMP_BOX;
    menu_objs[MENU_ARCH_BOX].ob_head   = MN_OPEN;
    menu_objs[MENU_ARCH_BOX].ob_tail   = MN_QUIT;
    menu_objs[MENU_ARCH_BOX].ob_x      = (short)(MENU_ARCHIVE_X * font_w);
    menu_objs[MENU_ARCH_BOX].ob_y      = 0;
    menu_objs[MENU_ARCH_BOX].ob_width  = (short)(22 * font_w);
    menu_objs[MENU_ARCH_BOX].ob_height = (short)(7 * (font_h + 2));
    menu_objs[MENU_ARCH_BOX].ob_spec.index = 0x00FF1100L;

    {
        static char *items[] = {
            " Open...        ^O",
            " Extract...     ^E",
            " Close          ^W",
            " Validate          ",
            " Information       ",
            " ------------------",
            " Quit           ^Q"
        };
        int idx;
        for (idx = 0; idx < 7; idx++) {
            int obj = MN_OPEN + idx;
            menu_objs[obj].ob_type = G_STRING;
            menu_objs[obj].ob_next = (obj < MN_QUIT) ? obj + 1 : MENU_ARCH_BOX;
            menu_objs[obj].ob_spec.free_string = items[idx];
            menu_objs[obj].ob_x      = 0;
            menu_objs[obj].ob_y      = (short)(idx * (font_h + 2));
            menu_objs[obj].ob_width  = (short)(22 * font_w);
            menu_objs[obj].ob_height = font_h + 2;
            if (obj == MENU_ARCH_SEP)
                menu_objs[obj].ob_state = OS_DISABLED;
        }
    }

    /* Compression menu. */
    menu_objs[MENU_COMP_BOX].ob_type   = G_BOX;
    menu_objs[MENU_COMP_BOX].ob_next   = MENU_HELP_BOX;
    menu_objs[MENU_COMP_BOX].ob_head   = MN_CREATE;
    menu_objs[MENU_COMP_BOX].ob_tail   = MN_INTERCHANGE_PROFILE;
    menu_objs[MENU_COMP_BOX].ob_x      = (short)(MENU_COMPRESS_X * font_w);
    menu_objs[MENU_COMP_BOX].ob_y      = 0;
    menu_objs[MENU_COMP_BOX].ob_width  = (short)(26 * font_w);
    menu_objs[MENU_COMP_BOX].ob_height = (short)(7 * (font_h + 2));
    menu_objs[MENU_COMP_BOX].ob_spec.index = 0x00FF1100L;

    {
        static char *items[] = {
            " Create New...  ^N",
            " ------------------",
            "  Stored           ",
            "  BoltLite             ",
            "  boltlz          ",
            "  fluff            ",
            "  Interchange Profile 1"
        };
        int idx;
        for (idx = 0; idx < 7; idx++) {
            int obj = MN_CREATE + idx;
            menu_objs[obj].ob_type = G_STRING;
            menu_objs[obj].ob_next = (obj < MN_INTERCHANGE_PROFILE) ? obj + 1 : MENU_COMP_BOX;
            menu_objs[obj].ob_spec.free_string = items[idx];
            menu_objs[obj].ob_x      = 0;
            menu_objs[obj].ob_y      = (short)(idx * (font_h + 2));
            menu_objs[obj].ob_width  = (short)(26 * font_w);
            menu_objs[obj].ob_height = font_h + 2;
            if (obj == MENU_COMP_SEP)
                menu_objs[obj].ob_state = OS_DISABLED;
        }
    }

    /* Help menu. */
    menu_objs[MENU_HELP_BOX].ob_type   = G_BOX;
    menu_objs[MENU_HELP_BOX].ob_next   = MENU_PULLDOWNS;
    menu_objs[MENU_HELP_BOX].ob_head   = MN_HELP_ABOUT;
    menu_objs[MENU_HELP_BOX].ob_tail   = MN_DOC;
    menu_objs[MENU_HELP_BOX].ob_x      = (short)(MENU_HELP_X * font_w);
    menu_objs[MENU_HELP_BOX].ob_y      = 0;
    menu_objs[MENU_HELP_BOX].ob_width  = (short)(20 * font_w);
    menu_objs[MENU_HELP_BOX].ob_height = (short)(2 * (font_h + 2));
    menu_objs[MENU_HELP_BOX].ob_spec.index = 0x00FF1100L;

    menu_objs[MN_HELP_ABOUT].ob_type   = G_STRING;
    menu_objs[MN_HELP_ABOUT].ob_next   = MN_DOC;
    menu_objs[MN_HELP_ABOUT].ob_spec.free_string = " About BoltPack     ";
    menu_objs[MN_HELP_ABOUT].ob_x      = 0;
    menu_objs[MN_HELP_ABOUT].ob_y      = 0;
    menu_objs[MN_HELP_ABOUT].ob_width  = (short)(20 * font_w);
    menu_objs[MN_HELP_ABOUT].ob_height = font_h + 2;

    menu_objs[MN_DOC].ob_type   = G_STRING;
    menu_objs[MN_DOC].ob_next   = MENU_HELP_BOX;
    menu_objs[MN_DOC].ob_spec.free_string = " Documentation     ";
    menu_objs[MN_DOC].ob_x      = 0;
    menu_objs[MN_DOC].ob_y      = font_h + 2;
    menu_objs[MN_DOC].ob_width  = (short)(20 * font_w);
    menu_objs[MN_DOC].ob_height = font_h + 2;
    menu_objs[MN_DOC].ob_flags  = OF_LASTOB;

    sync_compression_menu_checks();
}

/* ====================================================================== */
/*  Window drawing                                                        */
/* ====================================================================== */

static void draw_list(short clip_x, short clip_y, short clip_w, short clip_h)
{
    short pxy[4];
    short progress_h;
    short y;
    int i;
    int visible_lines;

    /* Set clipping */
    pxy[0] = clip_x;
    pxy[1] = clip_y;
    pxy[2] = clip_x + clip_w - 1;
    pxy[3] = clip_y + clip_h - 1;
    vs_clip(vdi_handle, 1, pxy);

    /* Clear background */
    vsf_color(vdi_handle, 0); /* white */
    vsf_interior(vdi_handle, 1);
    v_bar(vdi_handle, pxy);

    /* Set text attributes */
    vst_color(vdi_handle, 1); /* black */
    vst_alignment(vdi_handle, 0, 5, &pxy[0], &pxy[1]);

    progress_h = progress_panel_height();
    y = clip_y + 2;
    if (progress_h > 0) {
        draw_progress_panel(clip_x, clip_y, clip_w);
        y = clip_y + progress_h + 2;
    }

    if (entry_count > 0) {
        v_gtext(vdi_handle, clip_x + COL_NAME,   y, "Name");
        v_gtext(vdi_handle, clip_x + COL_ORIG,   y, "Original");
        v_gtext(vdi_handle, clip_x + COL_COMP,   y, "Packed");
        v_gtext(vdi_handle, clip_x + COL_RATIO,  y, "Size%");
        v_gtext(vdi_handle, clip_x + COL_METHOD, y, "Method");
        v_gtext(vdi_handle, clip_x + COL_DATE,   y, "Date");

        y += LINE_HEIGHT;
        pxy[0] = clip_x;
        pxy[1] = y;
        pxy[2] = clip_x + clip_w - 1;
        pxy[3] = y;
        v_pline(vdi_handle, 2, pxy);
        y += 4;

        visible_lines = (clip_h - (y - clip_y)) / LINE_HEIGHT;
        for (i = 0; i < visible_lines && (scroll_pos + i) < entry_count; i++) {
            gui_entry_t *e = &entries[scroll_pos + i];
            short row_y = y + (short)(i * LINE_HEIGHT);

            v_gtext(vdi_handle, clip_x + COL_NAME,   row_y, e->name);
            v_gtext(vdi_handle, clip_x + COL_ORIG,   row_y, e->orig_str);
            v_gtext(vdi_handle, clip_x + COL_COMP,   row_y, e->comp_str);
            v_gtext(vdi_handle, clip_x + COL_RATIO,  row_y, e->ratio_str);
            v_gtext(vdi_handle, clip_x + COL_METHOD, row_y, e->method_str);
            v_gtext(vdi_handle, clip_x + COL_DATE,   row_y, e->date_str);
        }
    } else if (progress_h == 0) {
        vst_color(vdi_handle, 2); /* red/dark */
        v_gtext(vdi_handle, clip_x + 60, clip_y + clip_h / 2,
                "Drop a .bolt file to open, or use Archive > Open");
        vst_color(vdi_handle, 1);
    }

    vs_clip(vdi_handle, 0, pxy);
}

static void redraw_window(short rx, short ry, short rw, short rh)
{
    short wx, wy, ww, wh;
    wind_get(win_handle, WF_WORKXYWH, &wx, &wy, &ww, &wh);

    wind_update(BEG_UPDATE);
    graf_mouse(M_OFF, NULL);

    /* Walk the rectangle list */
    {
        short cx, cy, cw, ch;
        wind_get(win_handle, WF_FIRSTXYWH, &cx, &cy, &cw, &ch);
        while (cw > 0 && ch > 0) {
            /* Clip to redraw rect */
            short x1 = cx > rx ? cx : rx;
            short y1 = cy > ry ? cy : ry;
            short x2 = (cx + cw < rx + rw) ? cx + cw : rx + rw;
            short y2 = (cy + ch < ry + rh) ? cy + ch : ry + rh;
            if (x2 > x1 && y2 > y1)
                draw_list(wx, wy, ww, wh);
            wind_get(win_handle, WF_NEXTXYWH, &cx, &cy, &cw, &ch);
        }
    }

    graf_mouse(M_ON, NULL);
    wind_update(END_UPDATE);
}

/* ====================================================================== */
/*  Archive operations                                                    */
/* ====================================================================== */

static void close_archive(void)
{
    if (archive_fp) {
        fclose(archive_fp);
        archive_fp = NULL;
    }
    free_entries();
    archive_path[0] = '\0';
    memset(&archive_info, 0, sizeof(archive_info));
}

static int list_cb(const bp_entry_t *entry, void *user_data)
{
    gui_entry_t *e;
    double orig, comp;
    (void)user_data;

    /* Supplementary records describe a file; they are not separate items. */
    if (entry->entry_type == BP_TYPE_META ||
        entry->entry_type == BP_TYPE_RSRC)
        return 0;

    if (entry_count >= entries_capacity) return 1;
    e = &entries[entry_count++];

    if (entry->name) {
        strncpy(e->name, entry->name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
    } else {
        strcpy(e->name, "(unknown)");
    }

    orig = u64_to_dbl(entry->original_size);
    comp = u64_to_dbl(entry->compressed_size);
    format_size_compact_u64(entry->original_size,
                            e->orig_str, sizeof(e->orig_str));
    format_size_compact_u64(entry->compressed_size,
                            e->comp_str, sizeof(e->comp_str));

    if (orig > 0)
        sprintf(e->ratio_str, "%.0f%%", (comp / orig) * 100.0);
    else
        strcpy(e->ratio_str, "-");

    strncpy(e->method_str, method_name(entry->method),
            sizeof(e->method_str) - 1);
    e->method_str[sizeof(e->method_str) - 1] = '\0';

    {
        double secs = u64_to_dbl(entry->mtime);
        if (secs > 0) {
            time_t t = (time_t)secs;
            struct tm *tm = localtime(&t);
            if (tm)
                strftime(e->date_str, sizeof(e->date_str),
                         "%Y-%m-%d %H:%M", tm);
            else
                strcpy(e->date_str, "-");
        } else {
            strcpy(e->date_str, "-");
        }
    }
    return 0;
}

static void do_open(const char *path)
{
    bp_io_t io;
    bp_archive_info_t info;
    int rc;

    close_archive();

    archive_fp = fopen(path, "rb");
    if (!archive_fp) {
        show_alert("Cannot open file");
        return;
    }
    strncpy(archive_path, path, sizeof(archive_path) - 1);
    archive_path[sizeof(archive_path) - 1] = '\0';

    io = make_io(archive_fp);
    memset(&info, 0, sizeof(info));
    rc = bp_archive_info(&io, &info);
    if (rc != BP_OK) {
        show_alert("Not a valid BoltPack|archive");
        close_archive();
        return;
    }
    archive_info = info;

    if (!alloc_entries((int)info.entry_count)) {
        show_alert("Out of memory");
        close_archive();
        return;
    }

    fseek(archive_fp, 0L, SEEK_SET);
    io = make_io(archive_fp);
    rc = bp_archive_list(&io, list_cb, NULL);
    if (rc != BP_OK) {
        show_alert("Error reading archive");
        close_archive();
        return;
    }

    set_main_window_chrome();
    redraw_window(win_x, win_y, win_w, win_h);
}

static void selector_set_default_pattern(char *path, const char *pattern);
static int selector_build_path(const char *pattern, const char *fname,
                               char *full, size_t capacity);

static void do_open_dialog(void)
{
    char path[BP_MAX_PATH];
    char fname[128];

    selector_set_default_pattern(path, "*.BLT");
    fname[0] = '\0';

    {
        short button = 0;
        fsel_exinput(path, fname, &button, "Open BoltPack Archive");
        restore_main_window_after_dialog();
        if (button == 0) return;
    }

    if (selector_build_path(path, fname, path, sizeof(path)) != BP_OK) {
        show_alert("Selected path exceeds GEMDOS limit");
        return;
    }
    do_open(path);
}

static int selector_dir_from_pattern(const char *pattern, char *dir,
                                     size_t capacity)
{
    return bp_atari_selector_dir(pattern, BP_MAX_PATH, dir, capacity);
}

static void selector_set_default_pattern(char *path, const char *pattern)
{
    short drive = Dgetdrv();

    if (drive < 0 || drive >= 26)
        drive = 0;
    path[0] = (char)('A' + drive);
    path[1] = ':';
    path[2] = '\\';
    strcpy(path + 3, pattern);
}

static int selector_build_path(const char *pattern, const char *fname,
                               char *full, size_t capacity)
{
    return bp_atari_selector_join(pattern, BP_MAX_PATH, fname, full,
                                  capacity);
}

/* ---- Extract ---- */

#define GUI_ATARI_DIR_QUEUE_MAX 256U
#define GUI_ATARI_DIR_QUEUE_BYTES_MAX 32768UL

typedef struct {
    char dest_dir[BP_MAX_PATH];
    int cancelled;
    int partial;
    int invalid_entry;
    int subject_ok;
    int subject_type;
    int subject_native_seen;
    char subject[BP_MAX_PATH];
    struct atari_gui_dir_restore *directories;
    struct atari_gui_dir_restore *subject_directory;
    unsigned int directory_count;
    unsigned long directory_path_bytes;
    atari_progress_ui_t progress;
} extract_ctx_t;

typedef struct atari_gui_dir_restore {
    struct atari_gui_dir_restore *next;
    char *path;
    bp_uint64 mtime;
    bp_uint16 flags;
    unsigned char native_attr;
    unsigned char fat_valid;
    unsigned char fat_value;
    int native_seen;
    int fat_seen;
} atari_gui_dir_restore_t;

typedef struct {
    bp_io_t io;
    int fd;
    char target[BP_MAX_PATH];
    char temporary[BP_MAX_PATH];
    int target_existed;
    int committed;
    int preserve_target_attributes;
    unsigned char target_attributes;
    struct stat target_snapshot;
} atari_gui_staged_output_t;

typedef struct {
    bp_io_t io;
    bp_uint32 length;
    bp_uint32 position;
    unsigned char data[BP_META_MAX_SIZE];
} atari_gui_meta_output_t;

typedef struct {
    const char *dest_dir;
    bp_atari_tree_t tree;
    const char *error;
} atari_gui_name_index_t;

static int atari_gui_index_projected_name(atari_gui_name_index_t *index,
                                          const char *local, int is_dir)
{
    char key[BP_MAX_PATH];
    unsigned int key_len;
    int rc;
    rc = bp_atari_project_name(local, is_dir, key, sizeof(key), &key_len);
    if (rc != BP_OK) {
        index->error = "Name is outside Atari's 8.3 filename profile";
        return rc;
    }
    rc = bp_atari_tree_add(&index->tree, local, key, key_len, is_dir);
    if (rc == BP_ERR_CORRUPT)
        index->error = "Names collide on the Atari destination";
    else if (rc == BP_ERR_OVERFLOW)
        index->error = "Name index exceeds Atari's bounded limit";
    else if (rc == BP_ERR_NOMEM)
        index->error = "Not enough memory for Atari name checks";
    return rc;
}

static int atari_gui_build_path(const char *dir, const char *name,
                                char *path, size_t capacity)
{
    size_t dlen = strlen(dir), nlen = strlen(name), pos = dlen, len;
    int add_sep = dlen != 0 && dir[dlen - 1] != '/' && dir[dlen - 1] != '\\';
    char *p;
    if (dlen + (size_t)add_sep + nlen + 1 > capacity) return -1;
    memcpy(path, dir, dlen);
    if (add_sep) path[pos++] = '\\';
    memcpy(path + pos, name, nlen + 1);
    for (p = path; *p; p++) if (*p == '/') *p = '\\';
    len = strlen(path);
    while (len > 0 && (path[len - 1] == '/' || path[len - 1] == '\\'))
        path[--len] = '\0';
    return 0;
}

static int atari_gui_preflight_name(const bp_entry_t *entry, void *user_data)
{
    atari_gui_name_index_t *index = (atari_gui_name_index_t *)user_data;
    char local[BP_MAX_PATH], full[BP_MAX_PATH];
    int is_dir, rc;
    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR &&
        entry->entry_type != BP_TYPE_SYMLINK) return 0;
    is_dir = entry->entry_type == BP_TYPE_DIR;
    if (!bp_name_is_safe(entry->name) ||
        bp_utf8_to_local(entry->name, local, sizeof(local), BP_CP_ATARIST) != 0 ||
        atari_gui_build_path(index->dest_dir, local, full, sizeof(full)) != 0 ||
        strlen(full) > BP_ATARI_GEMDOS_PATH_MAX) {
        index->error = "Archive name cannot be represented in an Atari GEMDOS path";
        return 1;
    }
    rc = atari_gui_index_projected_name(index, local, is_dir);
    if (rc != BP_OK) {
        if (rc == BP_ERR_INVALID_ARG || rc == BP_ERR_OVERFLOW)
            index->error = "Archive name is outside Atari's 8.3 filename profile";
        else if (rc == BP_ERR_CORRUPT)
            index->error = "Archive names collide on the Atari destination";
        return 1;
    }
    return 0;
}

static void atari_gui_name_index_clear(atari_gui_name_index_t *index)
{
    bp_atari_tree_free(&index->tree);
}

static int atari_gui_check_dir(const char *path, int create)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno != ENOENT || !create) return -1;
        if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
        if (lstat(path, &st) != 0) return -1;
    }
    return S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) ? 0 : -1;
}

static int makedirs_checked(const char *path)
{
    char tmp[BP_MAX_PATH];
    char *p;
    size_t len;
    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    len = strlen(tmp);
    while (len > 3 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
        tmp[--len] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = '\0';
            if (*tmp && !(p > tmp && p[-1] == ':') &&
                atari_gui_check_dir(tmp, 1) != 0) {
                *p = sep;
                return -1;
            }
            *p = sep;
        }
    }
    if (len >= 2 && tmp[len - 1] == ':' ) return 0;
    return atari_gui_check_dir(tmp, 1);
}

static int atari_gui_make_parents(const char *path)
{
    char parent[BP_MAX_PATH];
    char *slash, *backslash;
    if (strlen(path) >= sizeof(parent)) return -1;
    strcpy(parent, path);
    slash = strrchr(parent, '/');
    backslash = strrchr(parent, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    if (slash == NULL) return 0;
    *slash = '\0';
    if (slash > parent && slash[-1] == ':') return 0;
    return makedirs_checked(parent);
}

static int atari_gui_same_file(const struct stat *a, const struct stat *b)
{
    return S_ISREG(a->st_mode) && S_ISREG(b->st_mode) &&
           /* Plain TOS synthesizes unstable inode values for pathname stats. */
           a->st_dev == b->st_dev &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime &&
           a->st_ctime == b->st_ctime;
}

static int atari_gui_sibling_name(const char *target, const char *base,
                                  char *out, size_t capacity)
{
    const char *slash = strrchr(target, '/');
    const char *backslash = strrchr(target, '\\');
    size_t prefix, base_len = strlen(base);
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    prefix = slash != NULL ? (size_t)(slash - target + 1) : 0;
    if (prefix >= capacity || base_len >= capacity - prefix) return -1;
    memcpy(out, target, prefix);
    memcpy(out + prefix, base, base_len + 1);
    return 0;
}

static int atari_gui_rename_noreplace(const char *from, const char *to)
{
    char old_path[BP_MAX_PATH], new_path[BP_MAX_PATH], *p;
    if (strlen(from) >= sizeof(old_path) || strlen(to) >= sizeof(new_path)) return -1;
    strcpy(old_path, from);
    strcpy(new_path, to);
    for (p = old_path; *p; p++) if (*p == '/') *p = '\\';
    for (p = new_path; *p; p++) if (*p == '/') *p = '\\';
    return Frename(0, old_path, new_path) == 0 ? 0 : -1;
}

static int atari_gui_backup_existing(const char *target, char *backup,
                                     size_t capacity)
{
    static unsigned long sequence = 0;
    unsigned int attempt;
    char base[9];
    for (attempt = 0; attempt < 64U; attempt++) {
        unsigned long id = (((unsigned long)clock() << 4) + sequence++) &
                           0x0FFFFFFFUL;
        sprintf(base, "B%07lX", id);
        if (atari_gui_sibling_name(target, base, backup, capacity) != 0) return -1;
        if (atari_gui_rename_noreplace(target, backup) == 0) return 0;
    }
    return -1;
}

static long staged_output_write(void *handle, const void *data, bp_uint32 count)
{
    atari_gui_staged_output_t *stage = (atari_gui_staged_output_t *)handle;
    const unsigned char *p = (const unsigned char *)data;
    bp_uint32 done = 0;
    while (done < count) {
        long written = (long)write(stage->fd, p + done, (size_t)(count - done));
        if (written <= 0) return -1;
        done += (bp_uint32)written;
    }
    return (long)done;
}

static long meta_output_write(void *handle, const void *data, bp_uint32 count)
{
    atari_gui_meta_output_t *meta = (atari_gui_meta_output_t *)handle;
    if (meta->position > sizeof(meta->data) ||
        count > sizeof(meta->data) - meta->position) return -1;
    memcpy(meta->data + meta->position, data, (size_t)count);
    meta->position += count;
    if (meta->position > meta->length) meta->length = meta->position;
    return (long)count;
}

static long meta_output_read(void *handle, void *data, bp_uint32 count)
{
    (void)handle; (void)data; (void)count;
    return -1;
}

static int meta_output_seek(void *handle, bp_off_t offset, int origin)
{
    (void)handle; (void)offset; (void)origin;
    return -1;
}

static bp_off_t meta_output_tell(void *handle)
{
    return (bp_off_t)((atari_gui_meta_output_t *)handle)->position;
}

static int atari_gui_get_attr(const char *path, unsigned char *attr)
{
    long value = Fattrib((char *)path, 0, 0);
    if (value < 0 || attr == NULL) return -1;
    *attr = (unsigned char)((int)value & BP_META_FAT_MASK);
    return 0;
}

static int atari_gui_apply_attr(const char *path, unsigned char valid,
                                unsigned char value)
{
    long current = Fattrib((char *)path, 0, 0);
    unsigned char desired;
    long result;
    if (current < 0) return -1;
    valid = (unsigned char)(valid & BP_META_FAT_MASK);
    desired = (unsigned char)(((unsigned char)current & (unsigned char)~valid) |
                              (value & valid));
    result = Fattrib((char *)path, 1, (int)(desired & BP_META_FAT_MASK));
    return result < 0 ? -1 : 0;
}

typedef struct {
    const unsigned char *data;
    bp_uint32 length;
    bp_uint32 position;
} atari_gui_meta_reader_t;

static long atari_gui_meta_read(void *handle, void *buffer, bp_uint32 count)
{
    atari_gui_meta_reader_t *reader = (atari_gui_meta_reader_t *)handle;
    bp_uint32 available = reader->length - reader->position;
    if (count > available) count = available;
    if (count == 0) return 0;
    memcpy(buffer, reader->data + reader->position, (size_t)count);
    reader->position += count;
    return (long)count;
}

static long atari_gui_meta_write_unused(void *handle, const void *buffer,
                                        bp_uint32 count)
{
    (void)handle; (void)buffer; (void)count;
    return -1;
}

static int atari_gui_meta_seek(void *handle, bp_off_t offset, int origin)
{
    atari_gui_meta_reader_t *reader = (atari_gui_meta_reader_t *)handle;
    long position;
    if (origin == SEEK_SET) position = (long)offset;
    else if (origin == SEEK_CUR) position = (long)reader->position + (long)offset;
    else if (origin == SEEK_END) position = (long)reader->length + (long)offset;
    else return -1;
    if (position < 0 || (bp_uint32)position > reader->length) return -1;
    reader->position = (bp_uint32)position;
    return 0;
}

static bp_off_t atari_gui_meta_tell(void *handle)
{
    return (bp_off_t)((atari_gui_meta_reader_t *)handle)->position;
}

static int atari_gui_add_meta(bp_archive_t *archive, const char *name,
                              bp_uint8 platform, const unsigned char *data,
                              bp_uint32 length)
{
    atari_gui_meta_reader_t reader;
    bp_io_t io;
    reader.data = data;
    reader.length = length;
    reader.position = 0;
    io.read = atari_gui_meta_read;
    io.write = atari_gui_meta_write_unused;
    io.seek = atari_gui_meta_seek;
    io.tell = atari_gui_meta_tell;
    io.handle = &reader;
    return bp_archive_add_entry_ex(archive, name, BP_TYPE_META, platform, &io,
                                   bp_u64_from_u32(length),
                                   bp_u64_from_u32(0), 0);
}

static void atari_gui_stat_mtime(time_t source, bp_uint64 *mtime,
                                 bp_uint16 *flags)
{
    unsigned long seconds;
    *mtime = bp_u64_from_u32(0);
    *flags = 0;
    if (source < (time_t)0) return;
    seconds = (unsigned long)source;
    if ((time_t)seconds != source) return;
    *mtime = bp_u64_from_ulong(seconds);
    *flags = BP_ENTRY_FLAG_MTIME_VALID;
}

static int atari_gui_add_entry_metadata(bp_archive_t *archive, const char *name,
                                        const char *path, bp_uint8 subject_type,
                                        int *captured)
{
    unsigned char attr, portable[BP_META_FAT_SIZE];
    int rc;
    *captured = 0;
    if (atari_gui_get_attr(path, &attr) != 0) return BP_OK;
    rc = atari_gui_add_meta(archive, name, BP_OS_ATARI, &attr, 1);
    if (rc != BP_OK) return rc;
    rc = bp_metadata_fat_encode(portable, sizeof(portable), subject_type,
                                BP_META_FAT_MASK,
                                (bp_uint8)(attr & BP_META_FAT_MASK));
    if (rc != BP_OK) return rc;
    rc = atari_gui_add_meta(archive, name, BP_META_PORTABLE, portable,
                            (bp_uint32)sizeof(portable));
    if (rc == BP_OK) *captured = 1;
    return rc;
}

static atari_gui_dir_restore_t *queue_directory_restore(
    extract_ctx_t *ctx, const char *path, const bp_entry_t *entry)
{
    atari_gui_dir_restore_t *node;
    size_t len = strlen(path) + 1;
    if (ctx->directory_count >= GUI_ATARI_DIR_QUEUE_MAX ||
        len > GUI_ATARI_DIR_QUEUE_BYTES_MAX - ctx->directory_path_bytes) return NULL;
    node = (atari_gui_dir_restore_t *)calloc(1, sizeof(*node));
    if (node == NULL) return NULL;
    node->path = (char *)malloc(len);
    if (node->path == NULL) { free(node); return NULL; }
    memcpy(node->path, path, len);
    node->mtime = entry->mtime;
    node->flags = entry->flags;
    node->next = ctx->directories;
    ctx->directories = node;
    ctx->directory_count++;
    ctx->directory_path_bytes += (unsigned long)len;
    return node;
}

static int atari_gui_path_depth(const char *path)
{
    int depth = 0;
    while (*path) {
        if (*path == '\\' || *path == '/') depth++;
        path++;
    }
    return depth;
}

static void finish_directory_restores(extract_ctx_t *ctx)
{
    while (ctx->directories != NULL) {
        atari_gui_dir_restore_t **best = &ctx->directories, **link;
        atari_gui_dir_restore_t *node;
        int best_depth = -1;
        for (link = &ctx->directories; *link; link = &(*link)->next) {
            int depth = atari_gui_path_depth((*link)->path);
            if (depth > best_depth) { best = link; best_depth = depth; }
        }
        node = *best;
        *best = node->next;
        if ((node->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
            bp_restore_mtime(node->path, node->mtime) != BP_OK)
            ctx->partial = 1;
        if (node->native_seen) {
            if (atari_gui_apply_attr(node->path, BP_META_FAT_MASK,
                                     node->native_attr) != 0) ctx->partial = 1;
        } else if (node->fat_seen && node->fat_valid != 0 &&
                   atari_gui_apply_attr(node->path, node->fat_valid,
                                        node->fat_value) != 0) {
            ctx->partial = 1;
        }
        free(node->path);
        free(node);
    }
    ctx->directory_count = 0;
    ctx->directory_path_bytes = 0;
}

static void discard_staged_output(atari_gui_staged_output_t *stage)
{
    if (stage->fd >= 0) { (void)close(stage->fd); stage->fd = -1; }
    if (stage->temporary[0]) {
        (void)unlink(stage->temporary);
        stage->temporary[0] = '\0';
    }
}

static int commit_staged_output(atari_gui_staged_output_t *stage)
{
    struct stat current, backup_stat;
    char backup[BP_MAX_PATH], template[BP_MAX_PATH];
    unsigned char current_attributes;
    int fd, backup_moved = 0;
    if (lstat(stage->target, &current) == 0) {
        if (!stage->target_existed || !atari_gui_same_file(&stage->target_snapshot,
                                                           &current))
            return BP_ERR_PARTIAL;
    } else if (errno != ENOENT || stage->target_existed) return BP_ERR_IO;
    if (stage->preserve_target_attributes &&
        (atari_gui_get_attr(stage->target, &current_attributes) != 0 ||
         current_attributes != stage->target_attributes)) return BP_ERR_PARTIAL;
    if (stage->target_existed) {
        if (atari_gui_sibling_name(stage->target, "BRXXXXXX", template,
                                   sizeof(template)) != 0) return BP_ERR_NAME_TOO_LONG;
        fd = mkstemp(template);
        if (fd < 0) return BP_ERR_IO;
        if (close(fd) != 0 || unlink(template) != 0) {
            (void)unlink(template);
            return BP_ERR_IO;
        }
        strcpy(backup, template);
        if (atari_gui_rename_noreplace(stage->target, backup) != 0) return BP_ERR_IO;
        backup_moved = 1;
        if (lstat(backup, &backup_stat) != 0 ||
            !atari_gui_same_file(&stage->target_snapshot, &backup_stat)) {
            if (atari_gui_rename_noreplace(backup, stage->target) != 0)
                printf("Changed destination remains at recovery path: %s\n", backup);
            return BP_ERR_PARTIAL;
        }
        if (stage->preserve_target_attributes &&
            (atari_gui_get_attr(backup, &current_attributes) != 0 ||
             current_attributes != stage->target_attributes)) {
            if (atari_gui_rename_noreplace(backup, stage->target) != 0)
                printf("Changed archive remains at recovery path: %s\n", backup);
            return BP_ERR_PARTIAL;
        }
    }
    if (atari_gui_rename_noreplace(stage->temporary, stage->target) != 0) {
        if (backup_moved && atari_gui_rename_noreplace(backup, stage->target) != 0)
            printf("Original file remains at recovery path: %s\n", backup);
        return BP_ERR_IO;
    }
    stage->temporary[0] = '\0';
    stage->committed = 1;
    if (backup_moved && unlink(backup) != 0) {
        printf("Cannot remove replaced-file backup: %s\n", backup);
        return BP_ERR_PARTIAL;
    }
    return BP_OK;
}

static int open_archive_stage(const char *target,
                              atari_gui_staged_output_t *stage,
                              FILE **stream)
{
    struct stat st;
    int fd;
    memset(stage, 0, sizeof(*stage));
    stage->fd = -1;
    *stream = NULL;
    if (strlen(target) > BP_ATARI_GEMDOS_PATH_MAX ||
        strlen(target) >= sizeof(stage->target) ||
        atari_gui_sibling_name(target, "BPXXXXXX", stage->temporary,
                               sizeof(stage->temporary)) != 0)
        return BP_ERR_NAME_TOO_LONG;
    strcpy(stage->target, target);
    if (lstat(target, &st) == 0) {
        if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) return BP_ERR_IO;
        if (atari_gui_get_attr(target, &stage->target_attributes) != 0 ||
            (stage->target_attributes & FA_RDONLY) != 0) return BP_ERR_IO;
        stage->target_existed = 1;
        stage->preserve_target_attributes = 1;
        stage->target_snapshot = st;
    } else if (errno != ENOENT) {
        return BP_ERR_IO;
    }
    fd = mkstemp(stage->temporary);
    if (fd < 0) return BP_ERR_IO;
    stage->fd = fd;
    *stream = fdopen(fd, "w+b");
    if (*stream == NULL) {
        discard_staged_output(stage);
        return BP_ERR_IO;
    }
    return BP_OK;
}

static int finish_archive_stage(FILE **stream,
                                atari_gui_staged_output_t *stage)
{
    int rc = BP_OK;
    if (fflush(*stream) != 0) rc = BP_ERR_IO;
    if (fclose(*stream) != 0) rc = BP_ERR_IO;
    *stream = NULL;
    stage->fd = -1;
    if (rc == BP_OK && stage->preserve_target_attributes &&
        atari_gui_apply_attr(stage->temporary,
                             (unsigned char)(BP_META_FAT_MASK & ~FA_RDONLY),
                             stage->target_attributes) != 0)
        rc = BP_ERR_IO;
    if (rc == BP_OK) rc = commit_staged_output(stage);
    if (rc != BP_OK) discard_staged_output(stage);
    return rc;
}

static bp_io_t *extract_open_cb(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char local[BP_MAX_PATH], full[BP_MAX_PATH];
    struct stat st;
    if (entry->entry_type == BP_TYPE_DATA || entry->entry_type == BP_TYPE_DIR ||
        entry->entry_type == BP_TYPE_SYMLINK) {
        ctx->subject_ok = 0;
        ctx->subject_type = entry->entry_type;
        ctx->subject_native_seen = 0;
        ctx->subject_directory = NULL;
        ctx->subject[0] = '\0';
    }
    if (!bp_name_is_safe(entry->name) ||
        bp_utf8_to_local(entry->name, local, sizeof(local), BP_CP_ATARIST) != 0 ||
        atari_gui_build_path(ctx->dest_dir, local, full, sizeof(full)) != 0) {
        ctx->partial = 1;
        return NULL;
    }
    if (entry->entry_type == BP_TYPE_META) {
        atari_gui_meta_output_t *meta;
        if (entry->platform != BP_OS_ATARI && entry->platform != BP_META_PORTABLE) {
            ctx->partial = 1;
            return NULL;
        }
        if (!ctx->subject_ok || strcmp(ctx->subject, entry->name) != 0) {
            ctx->partial = 1;
            return NULL;
        }
        if (entry->original_size.hi != 0 ||
            entry->original_size.lo > BP_META_MAX_SIZE ||
            (entry->platform == BP_OS_ATARI && entry->original_size.lo != 1)) {
            ctx->invalid_entry = 1;
            ctx->subject_ok = 0;
            return NULL;
        }
        meta = (atari_gui_meta_output_t *)calloc(1, sizeof(*meta));
        if (meta == NULL) { ctx->partial = 1; return NULL; }
        meta->io.read = meta_output_read;
        meta->io.write = meta_output_write;
        meta->io.seek = meta_output_seek;
        meta->io.tell = meta_output_tell;
        meta->io.handle = meta;
        return &meta->io;
    }
    if (entry->entry_type == BP_TYPE_RSRC ||
        entry->entry_type == BP_TYPE_SYMLINK) {
        ctx->partial = 1;
        return NULL;
    }
    if (entry->entry_type == BP_TYPE_DIR) {
        atari_gui_dir_restore_t *directory;
        if (!bp_u64_is_zero(entry->original_size) ||
            !bp_u64_is_zero(entry->compressed_size) || entry->checksum != 0) {
            ctx->invalid_entry = 1;
            return NULL;
        }
        if (strlen(entry->name) >= sizeof(ctx->subject) ||
            makedirs_checked(full) != 0 || lstat(full, &st) != 0 ||
            !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            ctx->partial = 1;
            return NULL;
        }
        if (entry->flags & BP_ENTRY_FLAG_MODE_VALID) ctx->partial = 1;
        directory = queue_directory_restore(ctx, full, entry);
        if (directory == NULL) { ctx->partial = 1; return NULL; }
        strcpy(ctx->subject, entry->name);
        ctx->subject_ok = 1;
        ctx->subject_directory = directory;
        return NULL;
    }
    if (entry->entry_type != BP_TYPE_DATA ||
        strlen(entry->name) >= sizeof(ctx->subject) ||
        atari_gui_make_parents(full) != 0) {
        ctx->partial = 1;
        return NULL;
    }
    {
        atari_gui_staged_output_t *stage;
        int fd;
        stage = (atari_gui_staged_output_t *)calloc(1, sizeof(*stage));
        if (stage == NULL) { ctx->partial = 1; return NULL; }
        stage->fd = -1;
        strcpy(stage->target, full);
        if (lstat(full, &st) == 0) {
            if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
                free(stage);
                ctx->partial = 1;
                return NULL;
            }
            stage->target_existed = 1;
            stage->target_snapshot = st;
        } else if (errno != ENOENT) {
            free(stage);
            ctx->partial = 1;
            return NULL;
        }
        if (atari_gui_sibling_name(full, "BPXXXXXX", stage->temporary,
                                   sizeof(stage->temporary)) != 0 ||
            (fd = mkstemp(stage->temporary)) < 0) {
            free(stage);
            ctx->partial = 1;
            return NULL;
        }
        stage->fd = fd;
        strcpy(ctx->subject, entry->name);
        stage->io.read = NULL;
        stage->io.write = staged_output_write;
        stage->io.seek = NULL;
        stage->io.tell = NULL;
        stage->io.handle = stage;
        return &stage->io;
    }
}

static int extract_done_cb(const bp_entry_t *entry, bp_io_t *out, int status,
                           void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    if (entry->entry_type == BP_TYPE_META) {
        atari_gui_meta_output_t *meta = (atari_gui_meta_output_t *)out->handle;
        char local[BP_MAX_PATH], full[BP_MAX_PATH];
        if (status != BP_OK) { free(meta); return BP_OK; }
        if (entry->platform == BP_OS_ATARI) {
            unsigned char attr;
            if (meta->length != 1) { free(meta); return BP_ERR_CORRUPT; }
            if ((meta->data[0] & (unsigned char)~BP_META_FAT_MASK) != 0) {
                free(meta);
                return BP_ERR_CORRUPT;
            }
            attr = (unsigned char)(meta->data[0] & BP_META_FAT_MASK);
            ctx->subject_native_seen = 1;
            if (ctx->subject_directory != NULL) {
                ctx->subject_directory->native_seen = 1;
                ctx->subject_directory->native_attr = attr;
            } else if (bp_utf8_to_local(entry->name, local, sizeof(local),
                                        BP_CP_ATARIST) != 0 ||
                       atari_gui_build_path(ctx->dest_dir, local, full,
                                            sizeof(full)) != 0 ||
                       atari_gui_apply_attr(full, BP_META_FAT_MASK, attr) != 0) {
                ctx->partial = 1;
            }
        } else if (entry->platform == BP_META_PORTABLE) {
            bp_portable_metadata_t parsed;
            int parse_rc = bp_metadata_parse(meta->data, meta->length, &parsed);
            if (parse_rc == BP_ERR_CORRUPT) { free(meta); return parse_rc; }
            if (parse_rc == BP_ERR_PARTIAL) ctx->partial = 1;
            if (parsed.has_fat && !ctx->subject_native_seen) {
                if (ctx->subject_directory != NULL) {
                    ctx->subject_directory->fat_seen = 1;
                    ctx->subject_directory->fat_valid = parsed.fat_valid;
                    ctx->subject_directory->fat_value = parsed.fat_value;
                } else if (bp_utf8_to_local(entry->name, local, sizeof(local),
                                            BP_CP_ATARIST) != 0 ||
                           atari_gui_build_path(ctx->dest_dir, local, full,
                                                sizeof(full)) != 0 ||
                           (parsed.fat_valid != 0 &&
                            atari_gui_apply_attr(full, parsed.fat_valid,
                                                 parsed.fat_value) != 0)) {
                    ctx->partial = 1;
                }
            }
        }
        free(meta);
        return ctx->partial ? BP_ERR_PARTIAL : BP_OK;
    }
    {
        atari_gui_staged_output_t *stage =
            (atari_gui_staged_output_t *)out->handle;
        int close_rc = close(stage->fd);
        int rc;
        stage->fd = -1;
        if (close_rc != 0) {
            discard_staged_output(stage);
            free(stage);
            ctx->subject_ok = 0;
            ctx->partial = 1;
            return status == BP_OK ? BP_ERR_IO : BP_OK;
        }
        if (status != BP_OK) {
            discard_staged_output(stage);
            free(stage);
            ctx->subject_ok = 0;
            return BP_OK;
        }
        if (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) {
            if (bp_restore_mtime(stage->temporary, entry->mtime) != BP_OK)
                ctx->partial = 1;
        }
        if (entry->flags & BP_ENTRY_FLAG_MODE_VALID) ctx->partial = 1;
        rc = commit_staged_output(stage);
        if (rc != BP_OK) {
            discard_staged_output(stage);
            free(stage);
            ctx->subject_ok = 0;
            ctx->partial = 1;
            return rc;
        }
        ctx->subject_ok = 1;
        ctx->subject_type = BP_TYPE_DATA;
        ctx->subject_native_seen = 0;
        free(stage);
        return BP_OK;
    }
}

static int extract_progress_cb(const bp_progress_t *p, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    return update_progress_ui(p, &ctx->progress);
}

static void do_extract(void)
{
    char path[BP_MAX_PATH];
    char fname[128];
    char selected[BP_MAX_PATH];
    short button;
    bp_io_t io;
    extract_ctx_t ctx;
    atari_gui_name_index_t *index;
    struct stat dest_stat;
    const char *preflight_error;
    int rc;

    if (!archive_fp) {
        show_alert("No archive is open");
        return;
    }

    {
        const char *sep;
        sep = strrchr(archive_path, '\\');
        if (!sep) sep = strrchr(archive_path, '/');
        if (sep && (sep - archive_path + 1) < (int)sizeof(path)) {
            int dlen = (int)(sep - archive_path + 1);
            strncpy(path, archive_path, (size_t)dlen);
            path[dlen] = '\0';
            strncat(path, "*.*", sizeof(path) - (size_t)dlen - 1);
        } else {
            selector_set_default_pattern(path, "*.*");
        }
    }
    fname[0] = '\0';
    fsel_exinput(path, fname, &button, "Select destination folder");
    restore_main_window_after_dialog();
    if (button == 0) return;

    if (selector_build_path(path, fname, selected, sizeof(selected)) != BP_OK) {
        show_alert("Selected path exceeds GEMDOS limit");
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    {
        struct stat st;
        if (stat(selected, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(ctx.dest_dir, selected, sizeof(ctx.dest_dir) - 1);
            ctx.dest_dir[sizeof(ctx.dest_dir) - 1] = '\0';
        } else {
            if (selector_dir_from_pattern(path, ctx.dest_dir,
                                          sizeof(ctx.dest_dir)) != BP_OK) {
                show_alert("Selected path exceeds GEMDOS limit");
                return;
            }
        }
    }
    if (lstat(ctx.dest_dir, &dest_stat) != 0 || !S_ISDIR(dest_stat.st_mode) ||
        S_ISLNK(dest_stat.st_mode)) {
        show_alert("Destination must be a real directory");
        return;
    }

    index = (atari_gui_name_index_t *)calloc(1, sizeof(*index));
    if (index == NULL) {
        show_alert("Not enough memory to check archive names");
        return;
    }
    index->dest_dir = ctx.dest_dir;
    if (fseek(archive_fp, 0L, SEEK_SET) != 0) {
        free(index);
        show_alert("Cannot seek in archive");
        return;
    }
    io = make_io(archive_fp);
    rc = bp_archive_list(&io, atari_gui_preflight_name, index);
    preflight_error = index->error;
    atari_gui_name_index_clear(index);
    free(index);
    if (rc != BP_OK) {
        show_alert(preflight_error != NULL ? preflight_error :
                   "Cannot validate archive names");
        return;
    }
    if (fseek(archive_fp, 0L, SEEK_SET) != 0) {
        show_alert("Cannot rewind archive after name checks");
        return;
    }

    start_progress_ui(&ctx.progress, "Extracting archive", NULL,
                      u64_to_dbl(archive_info.total_original),
                      (unsigned long)(archive_info.entry_count - archive_info.dir_count));
    graf_mouse(BUSYBEE, NULL);
    io = make_io(archive_fp);
    rc = bp_archive_extract_all_ex(&io, extract_open_cb, extract_done_cb,
                                   extract_progress_cb, &ctx);
    finish_directory_restores(&ctx);
    if (ctx.invalid_entry && (rc == BP_OK || rc == BP_ERR_PARTIAL))
        rc = BP_ERR_CORRUPT;
    else if (rc == BP_OK && ctx.partial)
        rc = BP_ERR_PARTIAL;
    graf_mouse(ARROW, NULL);
    stop_progress_ui(&ctx.progress);

    if (rc == BP_ERR_PARTIAL)
        show_alert("Partial extraction: some data or attributes were not restored");
    else if (rc != BP_OK && rc != BP_ERR_ABORTED)
        show_alert("Extraction failed; incomplete files were not published");
}

/* ---- Validate and archive information ---- */
struct atari_test_ctx {
    int ok_count;
    int fail_count;
};

static void atari_validate_cb(const bp_entry_t *entry, int result, void *ud)
{
    struct atari_test_ctx *ctx = (struct atari_test_ctx *)ud;
    (void)entry;
    if (result == BP_OK) ctx->ok_count++;
    else ctx->fail_count++;
}

static void do_validate(void)
{
    bp_io_t io;
    struct atari_test_ctx ctx;
    char msg[256];
    int rc;

    if (!archive_fp) {
        show_alert("No archive is open");
        return;
    }

    fseek(archive_fp, 0L, SEEK_SET);
    io = make_io(archive_fp);
    ctx.ok_count   = 0;
    ctx.fail_count = 0;

    graf_mouse(BUSYBEE, NULL);
    rc = bp_archive_test(&io, atari_validate_cb, &ctx);
    graf_mouse(ARROW, NULL);

    (void)rc;
    if (ctx.fail_count == 0)
        sprintf(msg, "[1][Validate OK|%d file%s|all passed][OK]",
                ctx.ok_count, ctx.ok_count == 1 ? "" : "s");
    else
        sprintf(msg, "[1][Validate result|OK: %d|FAILED: %d][ OK ]",
                ctx.ok_count, ctx.fail_count);
    form_alert(1, msg);
}

static void do_info(void)
{
    char orig_str[32];
    char comp_str[32];
    char msg[256];
    double orig, comp;
    int pct;
    bp_uint32 file_count;

    if (!archive_fp) {
        show_alert("No archive is open");
        return;
    }

    file_count = archive_info.entry_count - archive_info.dir_count;
    orig = u64_to_dbl(archive_info.total_original);
    comp = u64_to_dbl(archive_info.total_compressed);
    pct  = (orig > 0.0) ? (int)(comp / orig * 100.0) : 0;
    format_size_compact_u64(archive_info.total_original,
                            orig_str, sizeof(orig_str));
    format_size_compact_u64(archive_info.total_compressed,
                            comp_str, sizeof(comp_str));

    sprintf(msg, "[1][Archive info|Entries: %lu files  %lu dirs|"
                 "Method: %s|Original: %s|Packed: %s (%d%%)][OK]",
            (unsigned long)file_count,
            (unsigned long)archive_info.dir_count,
            format_methods(archive_info.method_counts),
            orig_str, comp_str, pct);
    form_alert(1, msg);
}

/* ---- Create ---- */
static int gather_dir(const char *dir)
{
    DIR *dp;
    struct dirent *de;
    int add_rc;

    dp = opendir(dir);
    if (!dp) {
        gather_skipped_sources++;
        return 0;
    }
    add_rc = add_gather_directory(dir);
    if (add_rc != 0) {
        closedir(dp);
        return add_rc;
    }

    while ((de = readdir(dp)) != NULL) {
        char full[BP_MAX_PATH];
        struct stat st;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (strlen(dir) + strlen(de->d_name) + 2 >= sizeof(full)) {
            gather_skipped_sources++;
            continue;
        }
        sprintf(full, "%s\\%s", dir, de->d_name);

        if (lstat(full, &st) != 0) {
            gather_skipped_sources++;
            continue;
        }

        if (S_ISLNK(st.st_mode)) {
            gather_skipped_sources++;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            add_rc = gather_dir(full);
            if (add_rc != 0) {
                closedir(dp);
                return add_rc;
            }
        } else if (S_ISREG(st.st_mode)) {
            add_rc = add_gather_file(full);
            if (add_rc != 0) {
                closedir(dp);
                return add_rc;
            }
        } else gather_skipped_sources++;
    }
    closedir(dp);
    return 0;
}

static double gathered_total_bytes(void)
{
    double total;
    int i;

    total = 0.0;
    for (i = 0; i < gather_count; i++) {
        struct stat st;

        if (gather_is_directory[i]) continue;
        if (lstat(gather_files[i], &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        total += (double)st.st_size;
    }

    return total;
}

static int atari_gui_is_separator(char c)
{
    return c == '/' || c == '\\';
}

static int atari_gui_path_char_equal(char a, char b)
{
    if (atari_gui_is_separator(a) && atari_gui_is_separator(b)) return 1;
    if (a >= 'a' && a <= 'z') a = (char)(a - ('a' - 'A'));
    if (b >= 'a' && b <= 'z') b = (char)(b - ('a' - 'A'));
    return a == b;
}

static int atari_gui_parent_path(const char *path, char *parent,
                                 size_t capacity)
{
    char *slash, *backslash;
    size_t len;
    if (strlen(path) >= capacity) return -1;
    strcpy(parent, path);
    slash = strrchr(parent, '/');
    backslash = strrchr(parent, '\\');
    if (backslash != NULL && (slash == NULL || backslash > slash)) slash = backslash;
    if (slash == NULL) {
        parent[0] = '\0';
        return 0;
    }
    if (slash == parent) {
        parent[1] = '\0';
        return 0;
    }
    if (slash > parent && slash[-1] == ':') {
        slash[1] = '\0';
        return 0;
    }
    *slash = '\0';
    len = strlen(parent);
    while (len > 1 && atari_gui_is_separator(parent[len - 1]) &&
           !(len == 3 && parent[1] == ':' &&
             atari_gui_is_separator(parent[2])))
        parent[--len] = '\0';
    return 0;
}

static void atari_gui_merge_parent(char *base, const char *parent)
{
    size_t i = 0, base_len = strlen(base), parent_len = strlen(parent), end;
    while (i < base_len && i < parent_len &&
           atari_gui_path_char_equal(base[i], parent[i])) i++;
    if (i == base_len && (i == parent_len ||
        (i > 0 && atari_gui_is_separator(base[i - 1])) ||
        atari_gui_is_separator(parent[i]))) return;
    if (i == parent_len && atari_gui_is_separator(base[i])) {
        strcpy(base, parent);
        return;
    }
    end = i;
    while (end > 0 && !atari_gui_is_separator(base[end - 1])) end--;
    if (end == 0) {
        base[0] = '\0';
    } else if (end == 1 && atari_gui_is_separator(base[0])) {
        base[1] = '\0';
    } else if (end == 3 && base[1] == ':' &&
               atari_gui_is_separator(base[2])) {
        base[3] = '\0';
    } else {
        base[end - 1] = '\0';
    }
}

static const char *atari_gui_relative_path(const char *path,
                                           const char *base)
{
    size_t i, len = strlen(base);
    if (len == 0) return path;
    for (i = 0; i < len; i++) {
        if (path[i] == '\0' || !atari_gui_path_char_equal(path[i], base[i]))
            return path;
    }
    if (atari_gui_is_separator(base[len - 1])) return path + len;
    if (atari_gui_is_separator(path[len])) return path + len + 1;
    if (path[len] == '\0') return path + len;
    return path;
}

static void do_create_from_paths(char **paths, int path_count)
{
    char out_path[BP_MAX_PATH];
    char out_fname[128];
    char base_dir[BP_MAX_PATH];
    double total_bytes;
    atari_progress_ui_t progress;
    short button;
    FILE *outfp;
    bp_io_t outio;
    bp_archive_t ar;
    atari_gui_staged_output_t archive_stage;
    atari_gui_name_index_t *source_names;
    int gather_rc;
    int preservation_partial;
    int archive_published;
    int base_initialized;
    const char *create_error;
    int rc, i;

    free_gather_files();
    base_dir[0] = '\0';
    base_initialized = 0;
    gather_rc = 0;

    for (i = 0; i < path_count; i++) {
        struct stat st;
        char parent[BP_MAX_PATH];
        if (lstat(paths[i], &st) != 0 || S_ISLNK(st.st_mode) ||
            atari_gui_parent_path(paths[i], parent, sizeof(parent)) != 0)
            continue;
        if (!base_initialized) {
            strcpy(base_dir, parent);
            base_initialized = 1;
        }
        else atari_gui_merge_parent(base_dir, parent);
    }

    for (i = 0; i < path_count; i++) {
        struct stat st;

        if (lstat(paths[i], &st) != 0) {
            gather_skipped_sources++;
            continue;
        }

        if (S_ISLNK(st.st_mode)) {
            gather_skipped_sources++;
            continue;
        }

        if (S_ISDIR(st.st_mode))
            gather_rc = gather_dir(paths[i]);
        else if (S_ISREG(st.st_mode))
            gather_rc = add_gather_file(paths[i]);
        else {
            gather_skipped_sources++;
            continue;
        }

        if (gather_rc != 0)
            break;
    }

    if (gather_rc == -2) {
        show_alert("Too many files to archive");
        free_gather_files();
        return;
    }

    if (gather_rc != 0) {
        show_alert("Out of memory");
        free_gather_files();
        return;
    }

    if (gather_count == 0) {
        show_alert(gather_skipped_sources != 0
                   ? "No supported regular files were selected"
                   : "No files to archive");
        free_gather_files();
        return;
    }

    if (!ensure_selected_method_available()) {
        free_gather_files();
        return;
    }

    total_bytes = gathered_total_bytes();

    selector_set_default_pattern(out_path, "*.BLT");
    strcpy(out_fname, "ARCHIVE.BLT");
    fsel_exinput(out_path, out_fname, &button, "Save archive as");
    restore_main_window_after_dialog();
    if (button == 0) {
        free_gather_files();
        return;
    }

    {
        char *sl = strrchr(out_path, '\\');
        char *slash = strrchr(out_path, '/');
        size_t base_len;
        if (slash != NULL && (sl == NULL || slash > sl)) sl = slash;
        if (sl == NULL) {
            show_alert("Cannot determine archive output folder");
            free_gather_files();
            return;
        }
        sl[1] = '\0';
        base_len = strlen(out_path);
        if (base_len + strlen(out_fname) + 1 > sizeof(out_path)) {
            show_alert("Archive path is too long");
            free_gather_files();
            return;
        }
        strcpy(out_path + base_len, out_fname);
    }

    rc = open_archive_stage(out_path, &archive_stage, &outfp);
    if (rc != BP_OK) {
        show_alert("Cannot create file");
        free_gather_files();
        return;
    }

    start_progress_ui(&progress, "Creating archive",
                      current_interchange_profile ? "Interchange 1" : method_name(current_method),
                      total_bytes, (unsigned long)gather_count);

    graf_mouse(BUSYBEE, NULL);

    outio = make_io(outfp);
    memset(&ar, 0, sizeof(ar));
    if (current_interchange_profile)
        rc = bp_archive_create_profile(&ar, &outio, BP_PROFILE_INTERCHANGE_1);
    else
        rc = bp_archive_create(&ar, &outio, 0, current_method);
    if (rc != BP_OK) {
        bp_archive_close(&ar);
        (void)fclose(outfp);
        outfp = NULL;
        archive_stage.fd = -1;
        discard_staged_output(&archive_stage);
        graf_mouse(ARROW, NULL);
        stop_progress_ui(&progress);
        free_gather_files();
        show_alert("Create failed");
        return;
    }
    ar.progress_fn = update_progress_ui;
    ar.progress_data = &progress;
    preservation_partial = gather_skipped_sources != 0;
    archive_published = 0;
    create_error = NULL;
    source_names = (atari_gui_name_index_t *)calloc(1, sizeof(*source_names));
    if (source_names == NULL) {
        create_error = "Not enough memory for Atari name checks";
        rc = BP_ERR_NOMEM;
    }

    for (i = 0; rc == BP_OK && i < gather_count; i++) {
        char *rel;
        char archive_name[BP_MAX_PATH];
        struct stat st;
        bp_uint64 fsize, mtime;
        bp_uint16 mtime_flags;
        bp_uint8 subject_type;
        int metadata_captured;
        int name_rc;
        FILE *fp;
        bp_io_t fio;
        int is_directory;

        rel = (char *)atari_gui_relative_path(gather_files[i], base_dir);
        is_directory = gather_is_directory[i] != 0;
        subject_type = is_directory ? BP_TYPE_DIR : BP_TYPE_DATA;

        /* Convert backslash to forward slash */
        {
            char *c;
            char relcopy[BP_MAX_PATH];
            size_t rel_len;
            int index_rc;
            strncpy(relcopy, rel, sizeof(relcopy) - 1);
            relcopy[sizeof(relcopy) - 1] = '\0';
            for (c = relcopy; *c; c++)
                if (*c == '\\') *c = '/';
            rel_len = strlen(relcopy);
            if (is_directory && (rel_len == 0 || relcopy[rel_len - 1] != '/')) {
                if (rel_len + 1 >= sizeof(relcopy)) {
                    create_error = "A directory name is too long";
                    rc = BP_ERR_NAME_TOO_LONG;
                    break;
                }
                relcopy[rel_len++] = '/';
                relcopy[rel_len] = '\0';
            }

            if (bp_local_to_utf8(relcopy, archive_name, sizeof(archive_name),
                                 BP_CP_ATARIST) != 0) {
                create_error = "A selected filename cannot be converted to UTF-8";
                rc = BP_ERR_INVALID_ARG;
                break;
            }
            source_names->error = NULL;
            index_rc = atari_gui_index_projected_name(source_names,
                                                      relcopy, is_directory);
            name_rc = index_rc;
            if (name_rc != BP_OK) {
                create_error = source_names->error != NULL
                             ? source_names->error
                             : "A name is outside Atari's 8.3 profile";
                rc = name_rc;
                break;
            }

            if (lstat(gather_files[i], &st) != 0 ||
                (is_directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) {
                rc = BP_ERR_IO;
                break;
            }
            if (!is_directory &&
                (st.st_size < 0 || (off_t)(bp_off_t)st.st_size != st.st_size)) {
                create_error = "A source file exceeds Atari's archive size range";
                rc = BP_ERR_OVERFLOW;
                break;
            }
            fsize = is_directory ? bp_u64_from_u32(0)
                                 : bp_off_to_u64((bp_off_t)st.st_size);
            atari_gui_stat_mtime(st.st_mtime, &mtime, &mtime_flags);
            if (mtime_flags == 0 && st.st_mtime != (time_t)0) {
                printf("Source time not representable; storing unknown: %s\n",
                       gather_files[i]);
                preservation_partial = 1;
            }

            if (is_directory) {
                rc = bp_archive_add_entry_ex(&ar, archive_name, BP_TYPE_DIR,
                                             BP_OS_ATARI, NULL,
                                             bp_u64_from_u32(0), mtime,
                                             mtime_flags);
            } else {
                fp = fopen(gather_files[i], "rb");
                if (!fp) {
                    rc = BP_ERR_IO;
                    break;
                }
                fio = make_io(fp);
                rc = bp_archive_add_entry_ex(&ar, archive_name,
                    BP_TYPE_DATA, BP_OS_ATARI, &fio, fsize, mtime, mtime_flags);
                if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
            }
            if (rc == BP_OK) {
                rc = atari_gui_add_entry_metadata(&ar, archive_name,
                                                  gather_files[i], subject_type,
                                                  &metadata_captured);
                if (rc == BP_OK && !metadata_captured) {
                    printf("Could not read Atari attributes: %s\n",
                           gather_files[i]);
                    preservation_partial = 1;
                }
            }
        }

        if (rc != BP_OK) break;
    }

    if (source_names != NULL) {
        atari_gui_name_index_clear(source_names);
        free(source_names);
    }

    if (rc == BP_ERR_OVERFLOW && current_interchange_profile &&
        create_error == NULL)
        create_error = "Interchange Profile 1 size or entry limit exceeded";

    if (rc == BP_OK)
        rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    if (rc == BP_OK) {
        rc = finish_archive_stage(&outfp, &archive_stage);
        archive_published = archive_stage.committed;
    } else {
        if (outfp != NULL) {
            (void)fclose(outfp);
            outfp = NULL;
            archive_stage.fd = -1;
        }
        discard_staged_output(&archive_stage);
    }

    graf_mouse(ARROW, NULL);
    stop_progress_ui(&progress);
    free_gather_files();

    if (rc != BP_OK) {
        if (rc == BP_ERR_PARTIAL && archive_published) {
            show_alert("Archive created, but its replacement backup needs attention");
            do_open(out_path);
        } else {
            show_alert(create_error != NULL ? create_error :
                       "Create failed; the existing archive was preserved");
        }
        return;
    }

    if (preservation_partial)
        show_alert("Archive created, but some source attributes or times were not saved");

    do_open(out_path);
}

static void do_create(void)
{
    char in_path[BP_MAX_PATH];
    char in_fname[128];
    char selected[BP_MAX_PATH];
    char *paths[64];
    short button;
    int i;
    int path_count;

    selector_set_default_pattern(in_path, "*.*");
    path_count = 0;

    for (i = 0; i < 64; i++)
        paths[i] = NULL;

    for (;;) {
        int again;
        struct stat st;

        in_fname[0] = '\0';
        fsel_exinput(in_path, in_fname, &button, "Select file or folder to archive");
        restore_main_window_after_dialog();

        if (button == 0) {
            if (path_count == 0)
                return;
            break;
        }

        if (selector_build_path(in_path, in_fname, selected,
                                sizeof(selected)) != BP_OK) {
            show_alert("Selected path exceeds GEMDOS limit");
            continue;
        }
        if (stat(selected, &st) != 0) {
            show_alert("Select a file or folder");
            continue;
        }

        if (path_count >= 64) {
            show_alert("Too many selections");
            break;
        }

        paths[path_count] = dup_string(selected);
        if (paths[path_count] == NULL) {
            show_alert("Out of memory");
            free_path_array(paths, path_count);
            return;
        }
        path_count++;

        again = form_alert(1,
            "[1][Added to archive list.|Select another file or folder?][More|Create]");
        if (again != 1)
            break;
    }

    do_create_from_paths(paths, path_count);
    free_path_array(paths, path_count);
}

/* ====================================================================== */
/*  Event loop                                                            */
/* ====================================================================== */

static void handle_menu(short title, short item)
{
    menu_tnormal(menu_tree, title, 1);

    if (title == MN_DESK) {
        switch (item) {
            case MN_ABOUT:
                show_about_dialog();
                break;
        }
    } else if (title == MN_ARCHIVE) {
        switch (item) {
            case MN_OPEN:      do_open_dialog(); break;
            case MN_EXTRACT:   do_extract();     break;
            case MN_CLOSE:
                close_archive();
                set_main_window_chrome();
                redraw_window(win_x, win_y, win_w, win_h);
                break;
            case MN_VALIDATE:  do_validate();    break;
            case MN_INFO:      do_info();        break;
            case MN_QUIT:
                close_archive();
                archive_path[0] = '\x01';
                break;
        }
    } else if (title == MN_HELP) {
        switch (item) {
            case MN_HELP_ABOUT:
                show_about_dialog();
                break;
            case MN_DOC:
                show_alert("See README for|full documentation");
                break;
        }
    } else if (title == MN_COMPRESS) {
        switch (item) {
            case MN_CREATE:
                do_create();
                break;
            case MN_METHOD_STORED:
                select_compression_method(BP_METHOD_STORED);
                break;
            case MN_METHOD_BOLTLITE:
                select_compression_method(BP_METHOD_BOLTLITE);
                break;
            case MN_METHOD_BOLTLZ:
                select_compression_method(BP_METHOD_BOLTLZ);
                break;
            case MN_METHOD_FLUFF:
                select_compression_method(BP_METHOD_FLUFF);
                break;
            case MN_INTERCHANGE_PROFILE:
                toggle_interchange_profile();
                break;
        }
    }
}

static void event_loop(void)
{
    short msg[8];
    short mk, kr;
    short events;
    EVMULT_IN ev_in;
    EVMULT_OUT ev_out;

    /* The legacy evnt_multi wrapper does not match this mintlib's MT AES
       prototype (16-bit timer halves vs. one long interval plus click count),
       which shifts the keyboard outputs. The structure API keeps them aligned. */
    memset(&ev_in, 0, sizeof(ev_in));
    ev_in.emi_flags = MU_MESAG | MU_KEYBD;
    ev_in.emi_bclicks = 1;
    ev_in.emi_bmask = 1;
    ev_in.emi_bstate = 1;

    for (;;) {
        memset(&ev_out, 0, sizeof(ev_out));
        events = evnt_multi_fast(&ev_in, msg, &ev_out);
        mk = ev_out.emo_kmeta;
        kr = ev_out.emo_kreturn;

        if (events & MU_MESAG) {
            switch (msg[0]) {
                case MN_SELECTED:
                    handle_menu(msg[3], msg[4]);
                    if (archive_path[0] == '\x01') return;
                    break;

                case WM_REDRAW:
                    if (msg[3] == win_handle)
                        redraw_window(msg[4], msg[5],
                                      msg[6], msg[7]);
                    break;

                case WM_CLOSED:
                    if (msg[3] == win_handle) return;
                    break;

                case WM_MOVED:
                    if (msg[3] == win_handle) {
                        wind_set(win_handle, WF_CURRXYWH,
                                 msg[4], msg[5], msg[6], msg[7]);
                        win_x = msg[4]; win_y = msg[5];
                        win_w = msg[6]; win_h = msg[7];
                    }
                    break;

                case WM_SIZED:
                    if (msg[3] == win_handle) {
                        wind_set(win_handle, WF_CURRXYWH,
                                 msg[4], msg[5], msg[6], msg[7]);
                        win_x = msg[4]; win_y = msg[5];
                        win_w = msg[6]; win_h = msg[7];
                    }
                    break;

                case WM_TOPPED:
                    if (msg[3] == win_handle)
                        wind_set(win_handle, WF_TOP, 0, 0, 0, 0);
                    break;

                case WM_ARROWED:
                    if (msg[3] == win_handle) {
                        if (msg[4] == WA_UPLINE && scroll_pos > 0)
                            scroll_pos--;
                        else if (msg[4] == WA_DNLINE)
                            scroll_pos++;
                        else if (msg[4] == WA_UPPAGE && scroll_pos > 10)
                            scroll_pos -= 10;
                        else if (msg[4] == WA_DNPAGE)
                            scroll_pos += 10;
                        redraw_window(win_x, win_y, win_w, win_h);
                    }
                    break;
            }
        }

        if (events & MU_KEYBD) {
            /* Check for keyboard shortcuts (Ctrl+key) */
            short scan = (kr >> 8) & 0xFF;
            if (mk & K_CTRL) {
                switch (scan) {
                    case 0x18: do_open_dialog(); break; /* Ctrl+O */
                    case 0x12: do_extract();     break; /* Ctrl+E */
                    case 0x11:                          /* Ctrl+W */
                        close_archive();
                        set_main_window_chrome();
                        redraw_window(win_x, win_y, win_w, win_h);
                        break;
                    case 0x31: do_create();      break; /* Ctrl+N */
                    case 0x10: return;                  /* Ctrl+Q */
                }
            }
        }
    }
}

/* ====================================================================== */
/*  Main                                                                  */
/* ====================================================================== */

int main(void)
{
    int i;
    short desk_x, desk_y, desk_w, desk_h;
    short open_x, open_y, open_w, open_h;

    ap_id = appl_init();
    if (ap_id < 0) return 1;

    if (bp_compress_init() != BP_OK) {
        form_alert(1, "[3][Cannot init compression][ Quit ]");
        appl_exit();
        return 1;
    }

    /* Open VDI workstation */
    for (i = 0; i < 10; i++) work_in[i] = 1;
    work_in[10] = 2; /* raster coordinates */
    { short sh1, sh2; vdi_handle = graf_handle(&font_w, &font_h, &sh1, &sh2); }
    v_opnvwk(work_in, &vdi_handle, work_out);
    scr_w = work_out[0] + 1;
    scr_h = work_out[1] + 1;

    /* Build and install menu */
    build_menu();
    menu_bar(menu_tree, 1);

    /* Create main window */
    win_handle = wind_create(
        NAME | CLOSER | MOVER | SIZER | UPARROW | DNARROW | VSLIDE,
        0, font_h + 3, scr_w, scr_h - font_h - 3);

    if (win_handle < 0) {
        menu_bar(menu_tree, 0);
        v_clsvwk(vdi_handle);
        appl_exit();
        return 1;
    }

    set_main_window_chrome();
    wind_get(0, WF_WORKXYWH, &desk_x, &desk_y, &desk_w, &desk_h);
    open_w = 600;
    open_h = 360;
    if (open_w > desk_w) open_w = desk_w;
    if (open_h > desk_h) open_h = desk_h;
    open_x = desk_x;
    open_y = desk_y;
    if (desk_w > open_w)
        open_x = desk_x + (desk_w - open_w) / 2;
    if (desk_h > open_h)
        open_y = desk_y + (desk_h - open_h) / 2;
    wind_open(win_handle, open_x, open_y, open_w, open_h);
    wind_get(win_handle, WF_CURRXYWH, &win_x, &win_y, &win_w, &win_h);

    graf_mouse(ARROW, NULL);

    event_loop();

    /* Cleanup */
    close_archive();
    wind_close(win_handle);
    wind_delete(win_handle);
    menu_bar(menu_tree, 0);
    v_clsvwk(vdi_handle);
    appl_exit();

    return 0;
}
