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
 *       -I src/core -I lib/fastlz                        \
 *       src/core/compress.c src/core/archiver.c                \
 *       src/core/bp_codepage.c                                 \
 *       src/core/fluff.c src/core/lzss.c                       \
 *       src/frontend/atari_gui/main.c                          \
 *       lib/fastlz/fastlz.c                             \
 *       -lgem -o build/atari/boltpack.app
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/lzss.h"
#include "../../core/bp_codepage.h"

#include <gem.h>            /* AES + VDI (gemlib) */
#include <mint/osbind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

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
#define MN_METHOD_LZSS     (MENU_COMP_BOX + 4)
#define MN_METHOD_FASTLZ    (MENU_COMP_BOX + 5)
#define MN_METHOD_FLUFF    (MENU_COMP_BOX + 6)

#define MENU_HELP_BOX (MENU_COMP_BOX + 7)
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
static bp_uint8 current_method = BP_METHOD_FASTLZ;

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
static int gather_capacity;

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

    gather_count = 0;
    gather_capacity = 0;
}

static int ensure_gather_capacity(int needed)
{
    char **new_files;
    int new_capacity;

    if (needed <= gather_capacity)
        return 1;

    new_capacity = gather_capacity ? gather_capacity * 2 : 64;
    while (new_capacity < needed)
        new_capacity *= 2;

    new_files = (char **)realloc(gather_files,
                                 (size_t)new_capacity * sizeof(char *));
    if (new_files == NULL)
        return 0;

    gather_files = new_files;
    gather_capacity = new_capacity;
    return 1;
}

static int add_gather_file(const char *path)
{
    char *copy;

    if (gather_count >= MAX_ENTRIES)
        return -2;

    if (!ensure_gather_capacity(gather_count + 1))
        return -1;

    copy = dup_string(path);
    if (copy == NULL)
        return -1;

    gather_files[gather_count++] = copy;
    return 0;
}

static const char *method_name(int m)
{
    switch (m) {
        case 0: return "Stored";
        case 1: return "LZSS";
        case 2: return "fastlz";
        case 3: return "fluff";
    }
    return "?";
}

static const char *format_methods(const bp_uint32 *counts)
{
    static char buf[128];
    static const char * const names[4] = {"Stored", "LZSS", "fastlz", "Fluff"};
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
            "BoltPack 1.1|Portable archive format|Stored, LZSS, fastlz, fluff|Largest free block: %s",
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
    menu_objs[MN_METHOD_LZSS].ob_state &= (short)~OS_CHECKED;
    menu_objs[MN_METHOD_FASTLZ].ob_state &= (short)~OS_CHECKED;
    menu_objs[MN_METHOD_FLUFF].ob_state &= (short)~OS_CHECKED;

    switch (current_method) {
        case BP_METHOD_STORED:
            menu_objs[MN_METHOD_STORED].ob_state |= OS_CHECKED;
            break;
        case BP_METHOD_LZSS:
            menu_objs[MN_METHOD_LZSS].ob_state |= OS_CHECKED;
            break;
        case BP_METHOD_FLUFF:
            menu_objs[MN_METHOD_FLUFF].ob_state |= OS_CHECKED;
            break;
        default:
            current_method = BP_METHOD_FASTLZ;
            menu_objs[MN_METHOD_FASTLZ].ob_state |= OS_CHECKED;
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
    sync_compression_menu_checks();
    return 1;
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
    menu_objs[1].ob_type   = G_IBOX;
    menu_objs[1].ob_next   = MENU_PULLDOWNS;
    menu_objs[1].ob_head   = 2;
    menu_objs[1].ob_tail   = 2;
    menu_objs[1].ob_x      = 0;
    menu_objs[1].ob_y      = 0;
    menu_objs[1].ob_width  = scr_w;
    menu_objs[1].ob_height = font_h + 2;

    /* 2: active title strip */
    menu_objs[2].ob_type   = G_IBOX;
    menu_objs[2].ob_next   = 1;
    menu_objs[2].ob_head   = MN_DESK;
    menu_objs[2].ob_tail   = MN_HELP;
    menu_objs[2].ob_x      = 0;
    menu_objs[2].ob_y      = 0;
    menu_objs[2].ob_width  = (short)(34 * font_w);
    menu_objs[2].ob_height = font_h + 2;

    /* 3-6: actual titles. Keep Desk as a disabled blank placeholder for AES. */
    menu_objs[MN_DESK].ob_type   = G_TITLE;
    menu_objs[MN_DESK].ob_next   = MN_ARCHIVE;
    menu_objs[MN_DESK].ob_spec.free_string = "      ";
    menu_objs[MN_DESK].ob_x      = 0;
    menu_objs[MN_DESK].ob_y      = 0;
    menu_objs[MN_DESK].ob_width  = (short)(6 * font_w);
    menu_objs[MN_DESK].ob_height = font_h + 2;
    menu_objs[MN_DESK].ob_state  = OS_DISABLED;

    menu_objs[MN_ARCHIVE].ob_type   = G_TITLE;
    menu_objs[MN_ARCHIVE].ob_next   = MN_COMPRESS;
    menu_objs[MN_ARCHIVE].ob_spec.free_string = " Archive ";
    menu_objs[MN_ARCHIVE].ob_x      = (short)(6 * font_w);
    menu_objs[MN_ARCHIVE].ob_y      = 0;
    menu_objs[MN_ARCHIVE].ob_width  = (short)(9 * font_w);
    menu_objs[MN_ARCHIVE].ob_height = font_h + 2;

    menu_objs[MN_COMPRESS].ob_type   = G_TITLE;
    menu_objs[MN_COMPRESS].ob_next   = MN_HELP;
    menu_objs[MN_COMPRESS].ob_spec.free_string = " Compression ";
    menu_objs[MN_COMPRESS].ob_x      = (short)(15 * font_w);
    menu_objs[MN_COMPRESS].ob_y      = 0;
    menu_objs[MN_COMPRESS].ob_width  = (short)(13 * font_w);
    menu_objs[MN_COMPRESS].ob_height = font_h + 2;

    menu_objs[MN_HELP].ob_type   = G_TITLE;
    menu_objs[MN_HELP].ob_next   = 2;
    menu_objs[MN_HELP].ob_spec.free_string = " Help ";
    menu_objs[MN_HELP].ob_x      = (short)(28 * font_w);
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
    menu_objs[MENU_ARCH_BOX].ob_x      = (short)(6 * font_w);
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
    menu_objs[MENU_COMP_BOX].ob_tail   = MN_METHOD_FLUFF;
    menu_objs[MENU_COMP_BOX].ob_x      = (short)(15 * font_w);
    menu_objs[MENU_COMP_BOX].ob_y      = 0;
    menu_objs[MENU_COMP_BOX].ob_width  = (short)(22 * font_w);
    menu_objs[MENU_COMP_BOX].ob_height = (short)(6 * (font_h + 2));
    menu_objs[MENU_COMP_BOX].ob_spec.index = 0x00FF1100L;

    {
        static char *items[] = {
            " Create New...  ^N",
            " ------------------",
            "  Stored           ",
            "  LZSS             ",
            "  fastlz          ",
            "  fluff            "
        };
        int idx;
        for (idx = 0; idx < 6; idx++) {
            int obj = MN_CREATE + idx;
            menu_objs[obj].ob_type = G_STRING;
            menu_objs[obj].ob_next = (obj < MN_METHOD_FLUFF) ? obj + 1 : MENU_COMP_BOX;
            menu_objs[obj].ob_spec.free_string = items[idx];
            menu_objs[obj].ob_x      = 0;
            menu_objs[obj].ob_y      = (short)(idx * (font_h + 2));
            menu_objs[obj].ob_width  = (short)(22 * font_w);
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
    menu_objs[MENU_HELP_BOX].ob_x      = (short)(28 * font_w);
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

static void do_open_dialog(void)
{
    char path[BP_MAX_PATH];
    char fname[128];

    strcpy(path, "A:\\*.BLT");
    fname[0] = '\0';

    {
        short button = 0;
        fsel_exinput(path, fname, &button, "Open BoltPack Archive");
        restore_main_window_after_dialog();
        if (button == 0) return;
    }

    /* Build full path from directory + filename */
    {
        char *sl = strrchr(path, '\\');
        if (sl) *(sl + 1) = '\0';
        strcat(path, fname);
    }
    do_open(path);
}

static void selector_dir_from_pattern(const char *pattern, char *dir)
{
    char *sl;

    strncpy(dir, pattern, BP_MAX_PATH - 1);
    dir[BP_MAX_PATH - 1] = '\0';

    sl = strrchr(dir, '\\');
    if (sl)
        *sl = '\0';
}

static void selector_build_path(const char *pattern, const char *fname,
                                char *full)
{
    char dir[BP_MAX_PATH];

    selector_dir_from_pattern(pattern, dir);
    if (fname && fname[0])
        sprintf(full, "%s\\%s", dir, fname);
    else {
        strncpy(full, dir, BP_MAX_PATH - 1);
        full[BP_MAX_PATH - 1] = '\0';
    }
}

/* ---- Extract ---- */

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

/* ---- Extract ---- */

typedef struct {
    char dest_dir[BP_MAX_PATH];
    int cancelled;
    atari_progress_ui_t progress;
} extract_ctx_t;

static void makedirs(const char *path)
{
    char tmp[BP_MAX_PATH];
    char *p;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '\\';
        }
    }
    mkdir(tmp, 0755);
}

static bp_io_t *extract_open_cb(const bp_entry_t *entry, void *ud)
{
    extract_ctx_t *ctx = (extract_ctx_t *)ud;
    char full[BP_MAX_PATH];
    char *sl;
    FILE *fp;
    bp_io_t *out;

    if (!entry->name) return NULL;
    if (entry->entry_type == BP_TYPE_RSRC ||
        entry->entry_type == BP_TYPE_META)
        return NULL;

    sprintf(full, "%s\\%s", ctx->dest_dir, entry->name);
    /* Convert forward slashes to backslash for TOS */
    {
        char *c;
        for (c = full; *c; c++)
            if (*c == '/') *c = '\\';
    }

    /* Path traversal check */
    if (strstr(full, "\\..\\") != NULL) return NULL;

    if (entry->entry_type == BP_TYPE_DIR) {
        makedirs(full);
        return NULL;
    }

    sl = strrchr(full, '\\');
    if (sl) {
        char dir[BP_MAX_PATH];
        memcpy(dir, full, (size_t)(sl - full));
        dir[sl - full] = '\0';
        makedirs(dir);
    }

    fp = fopen(full, "wb");
    if (!fp) return NULL;

    out = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (!out) { fclose(fp); return NULL; }
    *out = make_io(fp);
    return out;
}

static void extract_done_cb(const bp_entry_t *entry, bp_io_t *out,
                             void *ud)
{
    (void)entry; (void)ud;
    if (out) {
        if (out->handle) fclose((FILE *)out->handle);
        free(out);
    }
}

static int extract_progress_cb(const bp_progress_t *p, void *ud)
{
    extract_ctx_t *ctx = (extract_ctx_t *)ud;
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
    int rc;

    if (!archive_fp) {
        show_alert("No archive is open");
        return;
    }

    /* Start the folder selector in the archive's own directory */
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
            strcpy(path, "A:\\*.*");
        }
    }
    fname[0] = '\0';
    fsel_exinput(path, fname, &button, "Select destination folder");
    restore_main_window_after_dialog();
    if (button == 0) return;

    selector_build_path(path, fname, selected);

    /* Use selected directory if one was picked, otherwise the current folder. */
    {
        struct stat st;

        if (stat(selected, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(ctx.dest_dir, selected, sizeof(ctx.dest_dir) - 1);
            ctx.dest_dir[sizeof(ctx.dest_dir) - 1] = '\0';
        } else {
            selector_dir_from_pattern(path, ctx.dest_dir);
        }
    }
    ctx.cancelled = 0;

    start_progress_ui(&ctx.progress, "Extracting archive", NULL,
                      u64_to_dbl(archive_info.total_original),
                      (unsigned long)(archive_info.entry_count - archive_info.dir_count));

    graf_mouse(BUSYBEE, NULL);

    fseek(archive_fp, 0L, SEEK_SET);
    io = make_io(archive_fp);

    rc = bp_archive_extract_all(&io,
        extract_open_cb, extract_done_cb,
        extract_progress_cb, &ctx);

    graf_mouse(ARROW, NULL);
    stop_progress_ui(&ctx.progress);

    if (rc != BP_OK && rc != BP_ERR_ABORTED)
        show_alert("Extraction error");
}

/* ---- Create ---- */

static int gather_dir(const char *dir)
{
    DIR *dp;
    struct dirent *de;
    int add_rc;

    dp = opendir(dir);
    if (!dp) return 0;

    while ((de = readdir(dp)) != NULL) {
        char full[BP_MAX_PATH];
        struct stat st;

        if (de->d_name[0] == '.') continue;
        sprintf(full, "%s\\%s", dir, de->d_name);

        if (stat(full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            add_rc = gather_dir(full);
            if (add_rc != 0) {
                closedir(dp);
                return add_rc;
            }
        } else {
            add_rc = add_gather_file(full);
            if (add_rc != 0) {
                closedir(dp);
                return add_rc;
            }
        }
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

        if (stat(gather_files[i], &st) != 0)
            continue;
        total += (double)st.st_size;
    }

    return total;
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
    int gather_rc;
    int rc, i;

    free_gather_files();
    base_dir[0] = '\0';
    gather_rc = 0;

    for (i = 0; i < path_count; i++) {
        struct stat st;

        if (stat(paths[i], &st) != 0)
            continue;

        if (base_dir[0] == '\0') {
            char *sl;

            strncpy(base_dir, paths[i], sizeof(base_dir) - 1);
            base_dir[sizeof(base_dir) - 1] = '\0';
            sl = strrchr(base_dir, '\\');
            if (sl)
                *sl = '\0';
        }

        if (S_ISDIR(st.st_mode))
            gather_rc = gather_dir(paths[i]);
        else
            gather_rc = add_gather_file(paths[i]);

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
        show_alert("No files to archive");
        free_gather_files();
        return;
    }

    if (!ensure_selected_method_available()) {
        free_gather_files();
        return;
    }

    total_bytes = gathered_total_bytes();

    strcpy(out_path, "A:\\*.BLT");
    strcpy(out_fname, "ARCHIVE.BLT");
    fsel_exinput(out_path, out_fname, &button, "Save archive as");
    restore_main_window_after_dialog();
    if (button == 0) {
        free_gather_files();
        return;
    }

    {
        char *sl = strrchr(out_path, '\\');
        if (sl) *(sl + 1) = '\0';
        strcat(out_path, out_fname);
    }

    outfp = fopen(out_path, "wb");
    if (!outfp) {
        show_alert("Cannot create file");
        free_gather_files();
        return;
    }

    start_progress_ui(&progress, "Creating archive", method_name(current_method),
                      total_bytes, (unsigned long)gather_count);

    graf_mouse(BUSYBEE, NULL);

    outio = make_io(outfp);
    memset(&ar, 0, sizeof(ar));
    rc = bp_archive_create(&ar, &outio, 0, current_method);
    if (rc != BP_OK) {
        fclose(outfp);
        graf_mouse(ARROW, NULL);
        stop_progress_ui(&progress);
        free_gather_files();
        show_alert("Create failed");
        return;
    }
    ar.progress_fn = update_progress_ui;
    ar.progress_data = &progress;

    for (i = 0; i < gather_count; i++) {
        char *rel;
        struct stat st;
        bp_uint64 fsize, mtime;
        FILE *fp;
        bp_io_t fio;

        rel = gather_files[i];
        if (base_dir[0] != '\0' &&
            strncmp(rel, base_dir, strlen(base_dir)) == 0)
            rel += strlen(base_dir) + 1;

        /* Convert backslash to forward slash */
        {
            char *c;
            char relcopy[BP_MAX_PATH];
            strncpy(relcopy, rel, sizeof(relcopy) - 1);
            relcopy[sizeof(relcopy) - 1] = '\0';
            for (c = relcopy; *c; c++)
                if (*c == '\\') *c = '/';

            if (stat(gather_files[i], &st) != 0) continue;
            fsize = bp_off_to_u64((bp_off_t)st.st_size);
            mtime = bp_off_to_u64((bp_off_t)st.st_mtime);

            fp = fopen(gather_files[i], "rb");
            if (!fp) continue;
            fio = make_io(fp);

            rc = bp_archive_add_entry(&ar, relcopy,
                BP_TYPE_DATA, BP_OS_ATARI,
                &fio, fsize, mtime);
            fclose(fp);
        }

        if (rc != BP_OK) break;
    }

    if (rc == BP_OK)
        rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    fclose(outfp);

    graf_mouse(ARROW, NULL);
    stop_progress_ui(&progress);
    free_gather_files();

    if (rc != BP_OK) {
        show_alert("Create failed");
        return;
    }

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

    strcpy(in_path, "A:\\*.*");
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

        selector_build_path(in_path, in_fname, selected);
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
            case MN_METHOD_LZSS:
                select_compression_method(BP_METHOD_LZSS);
                break;
            case MN_METHOD_FASTLZ:
                select_compression_method(BP_METHOD_FASTLZ);
                break;
            case MN_METHOD_FLUFF:
                select_compression_method(BP_METHOD_FLUFF);
                break;
        }
    }
}

static void event_loop(void)
{
    short msg[8];
    short mx, my, mb, mk, kr;
    short events;

    for (;;) {
        events = evnt_multi(
            MU_MESAG | MU_KEYBD,
            1, 1, 1,             /* mouse clicks */
            0, 0, 0, 0, 0,      /* rect 1 */
            0, 0, 0, 0, 0,      /* rect 2 */
            msg,
            0, 0,                /* timer */
            &mx, &my, &mb, &mk, &kr);

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
