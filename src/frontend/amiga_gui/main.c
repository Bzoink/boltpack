/*
 * main.c -- AmigaOS GadTools GUI frontend for BoltPack
 *
 * Target:   AmigaOS 2.0+ (Kickstart 37+), 68k
 * Compiler: m68k-amigaos-gcc (bebbo's fork) with -noixemul
 *
 * Uses Intuition windows, GadTools menus & listview, ASL file requester,
 * and Workbench AppWindow for drag-and-drop.
 *
 * Cross-compilation from Linux:
 *   m68k-amigaos-gcc -noixemul -ansi -Wall -O2                \
 *       -I src/core                         \
 *       src/core/compress.c src/core/archiver.c                \
 *       src/core/bp_codepage.c                                 \
 *       src/core/fluff.c src/core/boltlite.c                       \
 *       src/frontend/amiga_gui/main.c                          \
 *       src/core/boltlz.c                             \
 *       -lasl -lgadtools -lworkbench                           \
 *       -o build/amiga/boltpack-gui
 *
 * NOTE: -pedantic omitted because AmigaOS NDK headers are not strict C89.
 *
 * ANSI C (C89/C90) in the portable code sections.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/boltlite.h"
#include "../../core/bp_codepage.h"

#ifndef BP_CREATOR_OS
#  define BP_CREATOR_OS BP_OS_AMIGA_68K
#endif
#ifndef BP_CREATOR_CPU
#  define BP_CREATOR_CPU BP_CPU_M68K
#endif

/* AmigaOS system headers */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <workbench/workbench.h>
#include <workbench/startup.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <inline/alib.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/graphics.h>
#include <proto/wb.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>
#include <dirent.h>

/* ====================================================================== */
/*  Constants                                                             */
/* ====================================================================== */

#define BP_MAX_PATH          512
#define MAX_ENTRIES          2048
#define STATUS_LINE_COUNT    3
#define AMIGA_FLUFF_MIN_FREE (2304UL * 1024UL)
#define BOLDPACK_LOG_NAME    "boldpack.log"
#define USTR(x)              ((const UBYTE *)(x))

/* Development-only GUI logging. Enable with -DBP_GUI_DEV_LOGGING=1. */
#ifndef BP_GUI_DEV_LOGGING
#define BP_GUI_DEV_LOGGING 0
#endif

/* Menu item IDs */
#define MN_OPEN        1
#define MN_EXTRACT     2
#define MN_CLOSE       3
#define MN_CREATE      4
#define MN_QUIT        5
#define MN_ABOUT       6
#define MN_DOC         7
#define MN_VALIDATE    8
#define MN_INFO        9
#define MN_METHOD_STORED  10
#define MN_METHOD_BOLTLITE    11
#define MN_METHOD_BOLTLZ   12
#define MN_METHOD_FLUFF   13

/* Gadget IDs */
#define GID_LISTVIEW   1
#define GID_STATUS1    2
#define GID_STATUS2    3
#define GID_STATUS3    4

/* ====================================================================== */
/*  Globals                                                               */
/* ====================================================================== */

struct Library *GadToolsBase;
struct Library *AslBase;
struct Library *WorkbenchBase;

static struct Window *win;
static struct Screen *pub_screen;
static APTR vi;   /* VisualInfo */
static struct Menu *menu_strip;
static struct Gadget *gad_list;
static struct Gadget *listview_gad;
static struct Gadget *status1_gad;
static struct Gadget *status2_gad;
static struct Gadget *status3_gad;
#if BP_GUI_DEV_LOGGING
static FILE *log_fp;
static char log_path[BP_MAX_PATH];
#endif

/* AppWindow for Workbench drag-and-drop */
static struct MsgPort *app_port;
static struct AppWindow *app_win;

/* Archive state */
static FILE *archive_fp;
static char archive_path[BP_MAX_PATH];
static bp_archive_info_t archive_info;

/* List of entries for GadTools listview */
static struct List entry_list;
struct EntryNode {
    struct Node node;
    char display[256];  /* formatted line for listview */
};
static struct EntryNode *entry_nodes;
static int entry_count;
static int entry_capacity;

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
    int           last_log_pct_bucket;
    int           last_log_file_index;
    char          current_name[BP_MAX_PATH];
} amiga_progress_ui_t;

static amiga_progress_ui_t *active_progress;
static bp_uint8 current_method = BP_METHOD_BOLTLZ;
static char status_lines[STATUS_LINE_COUNT][160];
static int gather_count;
static char **gather_files;
static int gather_capacity;

static void show_requester(const char *title, const char *msg);
static void refresh_window_title(void);
static void refresh_idle_status(void);
static void sync_compression_menu_checks(void);
static int build_wbarg_path(BPTR lock, const char *name, char *path);
static void clear_entry_list(void);

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

static const char *bp_strerror(int code)
{
    switch (code) {
    case BP_OK:                    return "success";
    case BP_ERR_IO:                return "I/O error";
    case BP_ERR_NOMEM:             return "out of memory";
    case BP_ERR_MAGIC:             return "not a BoltPack archive (bad magic)";
    case BP_ERR_VERSION:           return "unsupported archive version";
    case BP_ERR_CHECKSUM:          return "checksum mismatch";
    case BP_ERR_CORRUPT:           return "archive is corrupt";
    case BP_ERR_TRUNCATED:         return "unexpected end of file";
    case BP_ERR_OVERFLOW:          return "size exceeds platform limits";
    case BP_ERR_NAME_TOO_LONG:     return "file name too long";
    case BP_ERR_UNSUPPORTED_METHOD:return "unsupported compression method";
    case BP_ERR_COMPRESS:          return "compression failed";
    case BP_ERR_DECOMPRESS:        return "decompression failed";
    case BP_ERR_ABORTED:           return "operation aborted";
    case BP_ERR_NOT_FOUND:         return "entry not found";
    case BP_ERR_INVALID_ARG:       return "invalid argument";
    default:                       return "unknown error";
    }
}

static void close_log(void)
{
#if BP_GUI_DEV_LOGGING
    if (log_fp != NULL) {
        fclose(log_fp);
        log_fp = NULL;
    }
#else
    return;
#endif
}

static void log_message(const char *fmt, ...)
{
#if BP_GUI_DEV_LOGGING
    va_list ap;
    time_t now;
    struct tm *tm_info;
    char ts[32];

    if (log_fp == NULL)
        return;

    ts[0] = '\0';
    now = time(NULL);
    if (now != (time_t)-1) {
        tm_info = localtime(&now);
        if (tm_info != NULL)
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
    }

    if (ts[0] != '\0')
        fprintf(log_fp, "[%s] ", ts);

    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    va_end(ap);

    fputc('\n', log_fp);
    fflush(log_fp);
#else
    (void)fmt;
#endif
}

static void log_memory_snapshot(const char *stage)
{
#if BP_GUI_DEV_LOGGING
    ULONG any_free;
    ULONG any_largest;
    ULONG chip_free;
    ULONG chip_largest;
    ULONG fast_free;
    ULONG fast_largest;

    any_free = AvailMem(MEMF_ANY);
    any_largest = AvailMem(MEMF_ANY | MEMF_LARGEST);
    chip_free = AvailMem(MEMF_CHIP);
    chip_largest = AvailMem(MEMF_CHIP | MEMF_LARGEST);
#ifdef MEMF_FAST
    fast_free = AvailMem(MEMF_FAST);
    fast_largest = AvailMem(MEMF_FAST | MEMF_LARGEST);
#else
    fast_free = 0UL;
    fast_largest = 0UL;
#endif

    log_message("%s memory: any=%lu largest=%lu chip=%lu chip-largest=%lu fast=%lu fast-largest=%lu",
                stage ? stage : "runtime",
                (unsigned long)any_free,
                (unsigned long)any_largest,
                (unsigned long)chip_free,
                (unsigned long)chip_largest,
                (unsigned long)fast_free,
                (unsigned long)fast_largest);
#else
    (void)stage;
#endif
}

static void init_log(void)
{
#if BP_GUI_DEV_LOGGING
    char chosen_path[BP_MAX_PATH];
    char prog_path[BP_MAX_PATH];
    char *part;
    FILE *fp;
    extern struct WBStartup *_WBenchMsg;

    chosen_path[0] = '\0';
    prog_path[0] = '\0';
    strcpy(chosen_path, BOLDPACK_LOG_NAME);

    if (_WBenchMsg != NULL && _WBenchMsg->sm_NumArgs > 0) {
        if (build_wbarg_path(_WBenchMsg->sm_ArgList[0].wa_Lock,
                             (const char *)_WBenchMsg->sm_ArgList[0].wa_Name,
                             prog_path)) {
            part = (char *)PathPart((STRPTR)prog_path);
            if (part != NULL) {
                *part = '\0';
                if (AddPart((STRPTR)prog_path, (STRPTR)BOLDPACK_LOG_NAME,
                            BP_MAX_PATH)) {
                    strncpy(chosen_path, prog_path, sizeof(chosen_path) - 1);
                    chosen_path[sizeof(chosen_path) - 1] = '\0';
                }
            }
        }
    }

    fp = fopen(chosen_path, "w");
    if (fp == NULL && strcmp(chosen_path, BOLDPACK_LOG_NAME) != 0) {
        fp = fopen(BOLDPACK_LOG_NAME, "w");
        if (fp != NULL)
            strcpy(chosen_path, BOLDPACK_LOG_NAME);
    }
    if (fp == NULL) {
        fp = fopen("T:" BOLDPACK_LOG_NAME, "w");
        if (fp != NULL)
            strcpy(chosen_path, "T:" BOLDPACK_LOG_NAME);
    }
    if (fp == NULL)
        return;

    log_fp = fp;
    strncpy(log_path, chosen_path, sizeof(log_path) - 1);
    log_path[sizeof(log_path) - 1] = '\0';
    setvbuf(log_fp, NULL, _IONBF, 0);

    log_message("BoltPack Amiga GUI log started: %s", log_path);
    log_message("startup mode: %s", _WBenchMsg ? "Workbench" : "CLI");
    log_memory_snapshot("startup");
#else
    return;
#endif
}

/* ====================================================================== */
/*  Helpers                                                               */
/* ====================================================================== */

static double u64_to_dbl(bp_uint64 v)
{
    return (double)v.lo + (double)v.hi * 4294967296.0;
}

static unsigned long long u64_to_ull(bp_uint64 v)
{
    return ((unsigned long long)v.hi << 32) | (unsigned long long)v.lo;
}

static void format_size_compact_u64(bp_uint64 bytes, char *buf)
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

static void format_size(double bytes, char *buf)
{
    if (bytes < 1024.0)
        sprintf(buf, "%.0f B", bytes);
    else if (bytes < 1024.0 * 1024.0)
        sprintf(buf, "%.1f KB", bytes / 1024.0);
    else
        sprintf(buf, "%.1f MB", bytes / (1024.0 * 1024.0));
}

static void format_size_compact(double bytes, char *buf)
{
    if (bytes < 1024.0)
        sprintf(buf, "%.0fB", bytes);
    else if (bytes < 1024.0 * 1024.0)
        sprintf(buf, "%.1fKB", bytes / 1024.0);
    else
        sprintf(buf, "%.1fMB", bytes / (1024.0 * 1024.0));
}

static void format_rate(double bytes_per_sec, char *buf)
{
    char size_buf[24];

    if (bytes_per_sec <= 0.0) {
        strcpy(buf, "-");
        return;
    }

    format_size(bytes_per_sec, size_buf);
    sprintf(buf, "%s/s", size_buf);
}

static void log_progress_update(amiga_progress_ui_t *ctx,
                                const bp_progress_t *p,
                                int pct, int file_index)
{
    int pct_bucket;
    unsigned long file_count;
    unsigned long file_number;
    char total_done_buf[24];
    char total_buf[24];
    char file_done_buf[24];
    char file_buf[24];
    const char *name;

    if (ctx == NULL || p == NULL)
        return;

    pct_bucket = pct / 5;
    if (pct_bucket == ctx->last_log_pct_bucket &&
        file_index == ctx->last_log_file_index)
        return;

    file_count = p->file_count ? (unsigned long)p->file_count : ctx->total_files;
    file_number = (file_index >= 0) ? (unsigned long)(file_index + 1) : 0UL;
    name = p->file_name ? p->file_name : "(none)";

    format_size(u64_to_dbl(p->archive_orig_done), total_done_buf);
    format_size(ctx->total_bytes > 0.0 ? ctx->total_bytes
                                       : u64_to_dbl(p->archive_orig_total),
                total_buf);
    format_size(u64_to_dbl(p->file_bytes_done), file_done_buf);
    format_size(u64_to_dbl(p->file_orig_size), file_buf);

    log_message("%s progress: file %lu/%lu total=%d%% current='%s' file=%s/%s total=%s/%s",
                ctx->verb ? ctx->verb : "Operation",
                file_number,
                file_count,
                pct,
                name,
                file_done_buf,
                file_buf,
                total_done_buf,
                total_buf);

    ctx->last_log_pct_bucket = pct_bucket;
    ctx->last_log_file_index = file_index;
}

static ULONG amiga_largest_free_block(void)
{
    return AvailMem(MEMF_ANY | MEMF_LARGEST);
}

static int fluff_memory_ok(void)
{
    return amiga_largest_free_block() >= AMIGA_FLUFF_MIN_FREE;
}

static void show_fluff_memory_alert(ULONG avail)
{
    char need_buf[32];
    char avail_buf[32];
    char msg[192];

    format_size((double)AMIGA_FLUFF_MIN_FREE, need_buf);
    format_size((double)avail, avail_buf);
    sprintf(msg, "Fluff needs about %s\nLargest free block: %s",
            need_buf, avail_buf);
    log_message("Fluff unavailable: largest free block is %lu bytes",
                (unsigned long)avail);
    show_requester("Compression", msg);
}

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

static void free_entry_storage(void)
{
    if (entry_nodes != NULL) {
        free(entry_nodes);
        entry_nodes = NULL;
    }
    entry_capacity = 0;
}

static int ensure_entry_capacity(int needed)
{
    struct EntryNode *new_nodes;
    size_t bytes;

    if (needed <= entry_capacity)
        return 1;

    bytes = (size_t)needed * sizeof(struct EntryNode);
    new_nodes = (struct EntryNode *)malloc(bytes);
    if (new_nodes == NULL)
        return 0;

    free_entry_storage();
    entry_nodes = new_nodes;
    entry_capacity = needed;
    return 1;
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

    if (!ensure_gather_capacity(gather_count + 1))
        return -1;

    copy = dup_string(path);
    if (copy == NULL)
        return -1;

    gather_files[gather_count++] = copy;
    return 0;
}

static int build_wbarg_path(BPTR lock, const char *name, char *path)
{
    if (path == NULL)
        return 0;

    path[0] = '\0';

    if (lock != 0) {
        if (!NameFromLock(lock, (STRPTR)path, BP_MAX_PATH))
            path[0] = '\0';
    }

    if (name != NULL && name[0] != '\0') {
        if (path[0] != '\0') {
            if (!AddPart((STRPTR)path, (STRPTR)name, BP_MAX_PATH))
                return 0;
        } else {
            strncpy(path, name, BP_MAX_PATH - 1);
            path[BP_MAX_PATH - 1] = '\0';
        }
    }

    return path[0] != '\0';
}

static int copy_appmessage_paths(const struct AppMessage *amsg,
                                 char **paths, int max_paths)
{
    int i;
    int cnt;
    char path[BP_MAX_PATH];

    if (amsg == NULL || paths == NULL || amsg->am_ArgList == NULL)
        return 0;

    for (i = 0; i < max_paths; i++)
        paths[i] = NULL;

    cnt = 0;
    for (i = 0; i < (int)amsg->am_NumArgs && cnt < max_paths; i++) {
        if (!build_wbarg_path(amsg->am_ArgList[i].wa_Lock,
                              (const char *)amsg->am_ArgList[i].wa_Name,
                              path))
            continue;

        paths[cnt] = dup_string(path);
        if (paths[cnt] == NULL) {
            free_path_array(paths, cnt);
            return -1;
        }

        cnt++;
    }

    return cnt;
}

static void refresh_status_gadgets(void)
{
    if (!win)
        return;

    if (status1_gad) {
        GT_SetGadgetAttrs(status1_gad, win, NULL,
            GTTX_Text, (ULONG)status_lines[0], TAG_DONE);
    }
    if (status2_gad) {
        GT_SetGadgetAttrs(status2_gad, win, NULL,
            GTTX_Text, (ULONG)status_lines[1], TAG_DONE);
    }
    if (status3_gad) {
        GT_SetGadgetAttrs(status3_gad, win, NULL,
            GTTX_Text, (ULONG)status_lines[2], TAG_DONE);
    }

    if (status1_gad)
        RefreshGList(status1_gad, win, NULL, 3);
}

static void set_status_lines(const char *line1, const char *line2,
                             const char *line3)
{
    strncpy(status_lines[0], line1 ? line1 : "", sizeof(status_lines[0]) - 1);
    status_lines[0][sizeof(status_lines[0]) - 1] = '\0';
    strncpy(status_lines[1], line2 ? line2 : "", sizeof(status_lines[1]) - 1);
    status_lines[1][sizeof(status_lines[1]) - 1] = '\0';
    strncpy(status_lines[2], line3 ? line3 : "", sizeof(status_lines[2]) - 1);
    status_lines[2][sizeof(status_lines[2]) - 1] = '\0';
    refresh_status_gadgets();
}

static void refresh_window_title(void)
{
    char title[128];

    if (!win)
        return;

    if (archive_path[0] != '\0') {
        sprintf(title, "BoltPack - %s", FilePart((STRPTR)archive_path));
        SetWindowTitles(win, (STRPTR)title, (UBYTE *)~0);
    } else {
        SetWindowTitles(win, (STRPTR)"BoltPack", (UBYTE *)~0);
    }
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

static void refresh_progress_status(amiga_progress_ui_t *ctx)
{
    char line1[160];
    char line2[160];
    char line3[160];
    char done_buf[32];
    char total_buf[32];
    char rate_buf[32];
    char name_buf[96];
    double elapsed;
    double rate;

    if (!ctx || !ctx->active)
        return;

    if (ctx->codec_name && ctx->codec_name[0] != '\0')
        sprintf(line1, "%s with %s", ctx->verb, ctx->codec_name);
    else
        strcpy(line1, ctx->verb);

    format_size(ctx->done_bytes, done_buf);
    format_size(ctx->total_bytes, total_buf);
    if (ctx->total_files > 0)
        sprintf(line2, "Files: %lu/%lu   Total: %d%%   Data: %s / %s",
                ctx->current_file, ctx->total_files, ctx->total_pct,
                done_buf, total_buf);
    else
        sprintf(line2, "Total: %d%%   Data: %s / %s",
                ctx->total_pct, done_buf, total_buf);

    elapsed = (double)(clock() - ctx->start_ticks) / (double)CLOCKS_PER_SEC;
    compact_progress_name(ctx->current_name, name_buf, sizeof(name_buf));
    if (elapsed > 0.1 && ctx->done_bytes > 0.0) {
        rate = ctx->done_bytes / elapsed;
        format_rate(rate, rate_buf);
        if (name_buf[0] != '\0')
            sprintf(line3, "Current: %s   Speed: %s", name_buf, rate_buf);
        else
            sprintf(line3, "Speed: %s", rate_buf);
    } else if (name_buf[0] != '\0') {
        sprintf(line3, "Current: %s", name_buf);
    } else {
        strcpy(line3, "Current: Preparing...");
    }

    set_status_lines(line1, line2, line3);
}

static void refresh_listview_gadget(void)
{
    if (listview_gad && win) {
        GT_SetGadgetAttrs(listview_gad, win, NULL,
            GTLV_Labels, (ULONG)&entry_list, TAG_DONE);
        RefreshGList(listview_gad, win, NULL, 1);
    }
}

static void format_create_queue_display(const char *base_dir,
                                        const char *path,
                                        const char *state,
                                        int index,
                                        char *dst,
                                        int dst_size)
{
    const char *name;
    int base_len;
    char name_buf[200];

    if (dst == NULL || dst_size <= 0)
        return;

    name = path ? path : "(unknown)";
    if (base_dir != NULL && base_dir[0] != '\0' && path != NULL) {
        base_len = (int)strlen(base_dir);
        if (strncmp(path, base_dir, (size_t)base_len) == 0 &&
            path[base_len] == '/')
            name = path + base_len + 1;
    }

    compact_progress_name(name, name_buf, sizeof(name_buf));
    sprintf(dst, "%-4s %4d  %s",
            state ? state : "[ ]",
            index + 1,
            name_buf[0] ? name_buf : "(unknown)");
}

static void show_create_queue(const char *base_dir)
{
    struct EntryNode *en;
    int i;

    if (gather_count <= 0)
        return;

    clear_entry_list();
    if (!ensure_entry_capacity(gather_count)) {
        log_message("Create queue display skipped: out of memory");
        return;
    }

    for (i = 0; i < gather_count; i++) {
        en = &entry_nodes[entry_count];
        format_create_queue_display(base_dir, gather_files[i], "[ ]",
                                    i, en->display, sizeof(en->display));
        en->node.ln_Name = en->display;
        en->node.ln_Type = 0;
        en->node.ln_Pri  = 0;
        AddTail(&entry_list, &en->node);
        entry_count++;
    }

    refresh_listview_gadget();
}

static void update_create_queue_state(const char *base_dir, int index,
                                      const char *state)
{
    struct EntryNode *en;

    if (entry_nodes == NULL || gather_files == NULL)
        return;
    if (index < 0 || index >= gather_count || index >= entry_count)
        return;

    en = &entry_nodes[index];
    format_create_queue_display(base_dir, gather_files[index], state,
                                index, en->display, sizeof(en->display));
    refresh_listview_gadget();
}

static void start_progress_ui(amiga_progress_ui_t *ctx, const char *verb,
                              const char *codec_name, double total_bytes,
                              unsigned long total_files)
{
    if (ctx == NULL)
        return;

    memset(ctx, 0, sizeof(*ctx));
    ctx->verb = verb;
    ctx->codec_name = codec_name;
    ctx->total_bytes = total_bytes;
    ctx->total_files = total_files;
    ctx->start_ticks = clock();
    ctx->active = 1;
    ctx->last_pct = -1;
    ctx->last_file_index = -1;
    ctx->last_log_pct_bucket = -1;
    ctx->last_log_file_index = -1;
    active_progress = ctx;
    log_message("%s started: method=%s total-bytes=%.0f files=%lu",
                verb ? verb : "Operation",
                codec_name ? codec_name : "n/a",
                total_bytes,
                total_files);
    refresh_progress_status(ctx);
}

static int update_progress_ui(const bp_progress_t *p, void *user_data)
{
    amiga_progress_ui_t *ctx;
    double done;
    double total;
    int pct;
    int file_index;
    unsigned long file_count;

    if (p == NULL || user_data == NULL)
        return 0;

    ctx = (amiga_progress_ui_t *)user_data;
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

    log_progress_update(ctx, p, pct, file_index);
    refresh_progress_status(ctx);
    ctx->last_pct = pct;
    ctx->last_file_index = file_index;
    return 0;
}

static void stop_progress_ui(amiga_progress_ui_t *ctx)
{
    double elapsed;

    if (ctx) {
        elapsed = (double)(clock() - ctx->start_ticks) / (double)CLOCKS_PER_SEC;
        log_message("%s stopped: done-bytes=%.0f total-bytes=%.0f files=%lu elapsed=%.1fs",
                    ctx->verb ? ctx->verb : "Operation",
                    ctx->done_bytes,
                    ctx->total_bytes,
                    ctx->current_file,
                    elapsed);
        ctx->active = 0;
        ctx->current_name[0] = '\0';
    }

    if (active_progress == ctx)
        active_progress = NULL;

    refresh_idle_status();
}

static struct MenuItem *find_menu_item_by_id(int id)
{
    struct Menu *menu;

    if (menu_strip == NULL)
        return NULL;

    for (menu = menu_strip; menu != NULL; menu = menu->NextMenu) {
        struct MenuItem *item;

        for (item = menu->FirstItem; item != NULL; item = item->NextItem) {
            if ((int)(ULONG)GTMENUITEM_USERDATA(item) == id)
                return item;
        }
    }

    return NULL;
}

static void sync_compression_menu_checks(void)
{
    struct MenuItem *stored;
    struct MenuItem *BoltLite;
    struct MenuItem *boltlz;
    struct MenuItem *fluff;

    stored = find_menu_item_by_id(MN_METHOD_STORED);
    BoltLite   = find_menu_item_by_id(MN_METHOD_BOLTLITE);
    boltlz  = find_menu_item_by_id(MN_METHOD_BOLTLZ);
    fluff  = find_menu_item_by_id(MN_METHOD_FLUFF);

    if (stored) stored->Flags &= (UWORD)~CHECKED;
    if (BoltLite)   BoltLite->Flags   &= (UWORD)~CHECKED;
    if (boltlz)  boltlz->Flags  &= (UWORD)~CHECKED;
    if (fluff)  fluff->Flags  &= (UWORD)~CHECKED;

    switch (current_method) {
        case BP_METHOD_STORED:
            if (stored) stored->Flags |= CHECKED;
            break;
        case BP_METHOD_BOLTLITE:
            if (BoltLite) BoltLite->Flags |= CHECKED;
            break;
        case BP_METHOD_FLUFF:
            if (fluff) fluff->Flags |= CHECKED;
            break;
        case BP_METHOD_BOLTLZ:
        default:
            current_method = BP_METHOD_BOLTLZ;
            if (boltlz) boltlz->Flags |= CHECKED;
            break;
    }

    if (win && menu_strip) {
        ClearMenuStrip(win);
        SetMenuStrip(win, menu_strip);
    }
}

static int ensure_selected_method_available(void)
{
    ULONG avail;

    if (current_method != BP_METHOD_FLUFF)
        return 1;

    avail = amiga_largest_free_block();
    if (avail >= AMIGA_FLUFF_MIN_FREE)
        return 1;

    show_fluff_memory_alert(avail);
    current_method = BP_METHOD_BOLTLZ;
    sync_compression_menu_checks();
    refresh_idle_status();
    return 0;
}

static void select_compression_method(bp_uint8 method)
{
    ULONG avail;

    if (method == BP_METHOD_FLUFF) {
        avail = amiga_largest_free_block();
        if (avail < AMIGA_FLUFF_MIN_FREE) {
            show_fluff_memory_alert(avail);
            sync_compression_menu_checks();
            refresh_idle_status();
            return;
        }
    }

    current_method = method;
    log_message("Compression method selected: %s", method_name(current_method));
    sync_compression_menu_checks();
    refresh_idle_status();
}

static void show_about_dialog(void)
{
    char free_buf[32];
    char msg[256];

    format_size((double)amiga_largest_free_block(), free_buf);
    sprintf(msg,
            "BoltPack 1.1\nPortable archive format\nStored, BoltLite, boltlz, fluff\nLargest free block: %s",
            free_buf);
    show_requester("About BoltPack", msg);
}

static void refresh_idle_status(void)
{
    char line1[160];
    char line2[160];
    char line3[160];
    char free_buf[32];
    char orig_buf[32];
    char comp_buf[32];
    double orig;
    double comp;
    bp_uint32 file_count;

    if (active_progress && active_progress->active) {
        refresh_progress_status(active_progress);
        return;
    }

    format_size_compact((double)amiga_largest_free_block(), free_buf);
    sprintf(line1, "Compression: %s   Largest free: %s",
            method_name(current_method), free_buf);

    if (archive_fp) {
        file_count = archive_info.entry_count - archive_info.dir_count;
        orig = u64_to_dbl(archive_info.total_original);
        comp = u64_to_dbl(archive_info.total_compressed);
        format_size_compact_u64(archive_info.total_original, orig_buf);
        format_size_compact_u64(archive_info.total_compressed, comp_buf);
        sprintf(line2, "Archive: %lu files  %lu dirs  Method: %s",
                (unsigned long)file_count,
                (unsigned long)archive_info.dir_count,
                format_methods(archive_info.method_counts));
        if (orig > 0.0)
            sprintf(line3, "Original: %s   Packed: %s (%d%%)",
                    orig_buf, comp_buf, (int)(comp / orig * 100.0));
        else
            sprintf(line3, "Original: %s   Packed: %s",
                    orig_buf, comp_buf);
    } else {
        strcpy(line2, "Open a .bolt/.blt archive or drop files/folders here");
        strcpy(line3, "Create New... archives the selected files recursively");
    }

    set_status_lines(line1, line2, line3);
}

static void show_requester(const char *title, const char *msg)
{
    struct EasyStruct es;
    es.es_StructSize   = sizeof(es);
    es.es_Flags        = 0;
    es.es_Title        = (UBYTE *)title;
    es.es_TextFormat   = (UBYTE *)msg;
    es.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequestArgs(win, &es, NULL, NULL);
}

/* ====================================================================== */
/*  Menu creation                                                         */
/* ====================================================================== */

static struct NewMenu new_menus[] = {
    { NM_TITLE, USTR("Archive"),         NULL, 0, 0, NULL },
    { NM_ITEM,  USTR("Open..."),         USTR("O"),  0, 0, (APTR)MN_OPEN },
    { NM_ITEM,  USTR("Extract Files..."), USTR("E"),  0, 0, (APTR)MN_EXTRACT },
    { NM_ITEM,  USTR("Close"),           USTR("W"),  0, 0, (APTR)MN_CLOSE },
    { NM_ITEM,  USTR("Validate"),        USTR("T"),  0, 0, (APTR)MN_VALIDATE },
    { NM_ITEM,  USTR("Information"),     USTR("I"),  0, 0, (APTR)MN_INFO },
    { NM_ITEM,  NM_BARLABEL,      NULL, 0, 0, NULL },
    { NM_ITEM,  USTR("Quit"),           USTR("Q"),  0, 0, (APTR)MN_QUIT },

    { NM_TITLE, USTR("Compression"),    NULL, 0, 0, NULL },
    { NM_ITEM,  USTR("Create New..."),   USTR("N"),  0, 0, (APTR)MN_CREATE },
    { NM_ITEM,  NM_BARLABEL,      NULL, 0, 0, NULL },
    { NM_ITEM,  USTR("Stored"),        NULL, CHECKIT, 0, (APTR)MN_METHOD_STORED },
    { NM_ITEM,  USTR("BoltLite"),          NULL, CHECKIT, 0, (APTR)MN_METHOD_BOLTLITE },
    { NM_ITEM,  USTR("boltlz"),       NULL, CHECKIT | CHECKED, 0, (APTR)MN_METHOD_BOLTLZ },
    { NM_ITEM,  USTR("Fluff"),         NULL, CHECKIT, 0, (APTR)MN_METHOD_FLUFF },

    { NM_TITLE, USTR("Help"),           NULL, 0, 0, NULL },
    { NM_ITEM,  USTR("About BoltPack"), NULL, 0, 0, (APTR)MN_ABOUT },
    { NM_ITEM,  USTR("Documentation"),  NULL, 0, 0, (APTR)MN_DOC },

    { NM_END,   NULL,             NULL, 0, 0, NULL }
};

/* ====================================================================== */
/*  Listview setup                                                        */
/* ====================================================================== */

static void init_entry_list(void)
{
    NewList(&entry_list);
    entry_count = 0;
}

static void clear_entry_list(void)
{
    /* Detach list from gadget first */
    if (listview_gad && win) {
        GT_SetGadgetAttrs(listview_gad, win, NULL,
            GTLV_Labels, (ULONG)~0, TAG_DONE);
    }
    init_entry_list();
    refresh_listview_gadget();
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
    archive_path[0] = '\0';
    memset(&archive_info, 0, sizeof(archive_info));
}

static int list_cb(const bp_entry_t *entry, void *user_data)
{
    struct EntryNode *en;
    double orig, comp;
    char orig_s[16], comp_s[16], ratio_s[8], date_s[20];
    (void)user_data;

    if (entry_nodes == NULL || entry_count >= entry_capacity)
        return 1;

    en = &entry_nodes[entry_count];

    orig = u64_to_dbl(entry->original_size);
    comp = u64_to_dbl(entry->compressed_size);
    format_size_compact_u64(entry->original_size, orig_s);
    format_size_compact_u64(entry->compressed_size, comp_s);

    if (orig > 0)
        sprintf(ratio_s, "%.0f%%", (comp / orig) * 100.0);
    else
        strcpy(ratio_s, "-");

    {
        double secs = u64_to_dbl(entry->mtime);
        if (secs > 0) {
            time_t t = (time_t)secs;
            struct tm *tm = localtime(&t);
            if (tm)
                strftime(date_s, sizeof(date_s), "%Y-%m-%d %H:%M", tm);
            else
                strcpy(date_s, "-");
        } else {
            strcpy(date_s, "-");
        }
    }

    sprintf(en->display, "%-28.28s %8s %8s %5s %-8s %s",
            entry->name ? entry->name : "(unknown)",
            orig_s, comp_s, ratio_s,
            method_name(entry->method), date_s);

    en->node.ln_Name = en->display;
    en->node.ln_Type = 0;
    en->node.ln_Pri  = 0;
    AddTail(&entry_list, &en->node);
    entry_count++;
    return 0;
}

static void do_open(const char *path)
{
    bp_io_t io;
    bp_archive_info_t info;
    int rc;

    close_archive();
    log_message("Opening archive: %s", path ? path : "(null)");

    clear_entry_list();
    if (!ensure_entry_capacity(MAX_ENTRIES)) {
        show_requester("Error", "Out of memory");
        refresh_window_title();
        refresh_idle_status();
        return;
    }

    archive_fp = fopen(path, "rb");
    if (!archive_fp) {
        show_requester("Error", "Cannot open file");
        refresh_window_title();
        refresh_idle_status();
        return;
    }
    strncpy(archive_path, path, sizeof(archive_path) - 1);
    archive_path[sizeof(archive_path) - 1] = '\0';

    io = make_io(archive_fp);
    memset(&info, 0, sizeof(info));
    rc = bp_archive_info(&io, &info);
    if (rc != BP_OK) {
        log_message("bp_archive_info failed for '%s': rc=%d (%s)",
                    path ? path : "(null)", rc, bp_strerror(rc));
        show_requester("Error", "Not a valid BoltPack archive");
        close_archive();
        refresh_window_title();
        refresh_idle_status();
        return;
    }
    archive_info = info;

    fseek(archive_fp, 0L, SEEK_SET);
    io = make_io(archive_fp);
    rc = bp_archive_list(&io, list_cb, NULL);
    if (rc != BP_OK) {
        log_message("bp_archive_list failed for '%s': rc=%d (%s)",
                    path ? path : "(null)", rc, bp_strerror(rc));
        show_requester("Error", "Error reading archive");
        close_archive();
        refresh_window_title();
        refresh_idle_status();
        return;
    }

    /* Attach list to gadget */
    refresh_listview_gadget();

    refresh_window_title();
    refresh_idle_status();
    log_message("Archive opened: entries=%lu dirs=%lu methods=%s",
                (unsigned long)archive_info.entry_count,
                (unsigned long)archive_info.dir_count,
                format_methods(archive_info.method_counts));
}

static void do_open_dialog(void)
{
    struct FileRequester *freq;

    freq = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText,    (ULONG)"Open BoltPack Archive",
        ASLFR_DoPatterns,   TRUE,
        ASLFR_InitialPattern, (ULONG)"#?.(bolt|blt)",
        TAG_DONE);
    if (!freq) return;

    if (AslRequest(freq, NULL)) {
        char path[BP_MAX_PATH];
        strcpy(path, (const char *)freq->fr_Drawer);
        if (AddPart((STRPTR)path, freq->fr_File, BP_MAX_PATH))
            do_open(path);
    }
    FreeAslRequest(freq);
}

/* ---- Validate ---- */

struct amiga_test_ctx {
    int ok_count;
    int fail_count;
};

static void amiga_validate_cb(const bp_entry_t *entry, int result, void *ud)
{
    struct amiga_test_ctx *ctx = (struct amiga_test_ctx *)ud;
    (void)entry;
    if (result == BP_OK) ctx->ok_count++;
    else ctx->fail_count++;
}

static void do_validate(void)
{
    bp_io_t io;
    struct amiga_test_ctx ctx;
    char msg[128];
    int rc;

    if (!archive_fp) {
        show_requester("BoltPack", "No archive is open");
        return;
    }

    log_message("Validate archive requested: %s",
                archive_path[0] ? archive_path : "(none)");

    fseek(archive_fp, 0L, SEEK_SET);
    io = make_io(archive_fp);
    ctx.ok_count   = 0;
    ctx.fail_count = 0;

    set_status_lines("Validating archive", "Checking data and checksums", "");
    rc = bp_archive_test(&io, amiga_validate_cb, &ctx);
    (void)rc;
    log_message("Validate complete: ok=%d fail=%d rc=%d",
                ctx.ok_count, ctx.fail_count, rc);

    if (ctx.fail_count == 0)
        sprintf(msg, "All %d file%s OK",
                ctx.ok_count, ctx.ok_count == 1 ? "" : "s");
    else
        sprintf(msg, "OK: %d  FAILED: %d", ctx.ok_count, ctx.fail_count);

    show_requester("Validate Archive", msg);
    refresh_idle_status();
}

static void do_info(void)
{
    char msg[512];
    double orig, comp;
    int pct;
    bp_uint32 file_count;
    char orig_buf[32];
    char comp_buf[32];

    if (!archive_fp) {
        show_requester("BoltPack", "No archive is open");
        return;
    }

    file_count = archive_info.entry_count - archive_info.dir_count;
    orig = u64_to_dbl(archive_info.total_original);
    comp = u64_to_dbl(archive_info.total_compressed);
    pct  = (orig > 0.0) ? (int)(comp / orig * 100.0) : 0;
    format_size_compact_u64(archive_info.total_original, orig_buf);
    format_size_compact_u64(archive_info.total_compressed, comp_buf);

    sprintf(msg,
        "Entries: %lu files, %lu dirs\n"
        "Method: %s\n"
        "Original: %s\n"
        "Packed: %s (%d%%)",
        (unsigned long)file_count,
        (unsigned long)archive_info.dir_count,
        format_methods(archive_info.method_counts),
        orig_buf, comp_buf, pct);

    show_requester("Archive Information", msg);
}

/* ---- Extract ---- */

static void makedirs(const char *path)
{
    char tmp[BP_MAX_PATH];
    char *p;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

struct amiga_extract_ctx {
    char dest[BP_MAX_PATH];
    amiga_progress_ui_t progress;
};

static bp_io_t *extract_open_cb(const bp_entry_t *entry, void *ud)
{
    struct amiga_extract_ctx *ctx = (struct amiga_extract_ctx *)ud;
    char full[BP_MAX_PATH];
    char *sl;
    FILE *fp;
    bp_io_t *out;

    if (!entry->name) return NULL;
    if (entry->entry_type == BP_TYPE_RSRC ||
        entry->entry_type == BP_TYPE_META)
        return NULL;

    if (strlen(ctx->dest) + 1 + strlen(entry->name) >= sizeof(full))
        return NULL;

    {
        size_t dest_len;
        size_t name_len;

        dest_len = strlen(ctx->dest);
        name_len = strlen(entry->name);
        memcpy(full, ctx->dest, dest_len);
        full[dest_len] = '/';
        memcpy(full + dest_len + 1, entry->name, name_len + 1);
    }

    /* Path traversal check */
    if (strstr(full, "/../") != NULL) return NULL;

    if (entry->entry_type == BP_TYPE_DIR) {
        makedirs(full);
        return NULL;
    }

    sl = strrchr(full, '/');
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
    struct amiga_extract_ctx *ctx = (struct amiga_extract_ctx *)ud;
    return update_progress_ui(p, &ctx->progress);
}

static void do_extract(void)
{
    struct FileRequester *freq;
    struct amiga_extract_ctx ctx;
    bp_io_t io;
    int rc;
    unsigned long total_files;
    char init_dir[BP_MAX_PATH];

    if (!archive_fp) {
        show_requester("BoltPack", "No archive is open");
        return;
    }

    log_message("Extract requested: %s",
                archive_path[0] ? archive_path : "(none)");

    /* Derive archive directory for the initial drawer */
    strncpy(init_dir, archive_path, sizeof(init_dir) - 1);
    init_dir[sizeof(init_dir) - 1] = '\0';
    {
        char *sep = (char *)PathPart((STRPTR)init_dir);
        *sep = '\0';
    }

    freq = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"Select Destination",
        ASLFR_DrawersOnly, TRUE,
        ASLFR_InitialDrawer, (ULONG)init_dir,
        TAG_DONE);    if (!freq) return;

    if (!AslRequest(freq, NULL)) {
        log_message("Extract cancelled by user");
        FreeAslRequest(freq);
        return;
    }

    {
        strncpy(ctx.dest, (const char *)freq->fr_Drawer,
            sizeof(ctx.dest) - 1);
        ctx.dest[sizeof(ctx.dest) - 1] = '\0';
        FreeAslRequest(freq);

        total_files = (unsigned long)(archive_info.entry_count - archive_info.dir_count);
        start_progress_ui(&ctx.progress, "Extracting archive", NULL,
                          u64_to_dbl(archive_info.total_original),
                          total_files);
        log_message("Extract destination: %s", ctx.dest);
        log_memory_snapshot("before extract");
        SetPointer(win, NULL, 0, 0, 0, 0); /* busy pointer */

        fseek(archive_fp, 0L, SEEK_SET);
        io = make_io(archive_fp);

        rc = bp_archive_extract_all(&io,
            extract_open_cb, extract_done_cb,
            extract_progress_cb, (void *)&ctx);

        ClearPointer(win);
        stop_progress_ui(&ctx.progress);
        log_message("Extract complete: rc=%d (%s)", rc, bp_strerror(rc));
        log_memory_snapshot("after extract");

        if (rc != BP_OK && rc != BP_ERR_ABORTED)
            show_requester("Error", "Extraction error");
        refresh_window_title();
    }
}

/* ---- Create ---- */

static int gather_dir(const char *dir)
{
    DIR *dp;
    struct dirent *de;
    int rc;

    dp = opendir(dir);
    if (!dp) {
        log_message("Gather directory skipped (cannot open): %s",
                    dir ? dir : "(null)");
        return 0;
    }

    while ((de = readdir(dp)) != NULL) {
        char full[BP_MAX_PATH];
        struct stat st;

        if (de->d_name[0] == '.') continue;
        sprintf(full, "%s/%s", dir, de->d_name);

        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            rc = gather_dir(full);
            if (rc != 0) {
                closedir(dp);
                return rc;
            }
        } else if (gather_count < MAX_ENTRIES) {
            rc = add_gather_file(full);
            if (rc != 0) {
                closedir(dp);
                return rc;
            }
            if (gather_count == 1 || (gather_count % 64) == 0)
                log_message("Gathered %d file(s) so far, latest='%s'",
                            gather_count, full);
        } else {
            closedir(dp);
            return -2;
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

static void do_create_from_paths(const char **paths, int path_count)
{
    struct FileRequester *freq;
    char out_path[BP_MAX_PATH];
    char base_dir[BP_MAX_PATH];
    amiga_progress_ui_t progress;
    double total_bytes;
    FILE *outfp;
    bp_io_t outio;
    bp_archive_t ar;
    struct stat out_st;
    int gather_rc;
    int rc, i;

    log_message("Create requested for %d selected path(s)", path_count);
    for (i = 0; i < path_count; i++)
        log_message("Create source %d: %s", i + 1, paths[i]);
    log_memory_snapshot("before gather");

    /* Gather files */
    free_gather_files();
    gather_count = 0;
    base_dir[0] = '\0';
    gather_rc = 0;

    for (i = 0; i < path_count; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0) continue;

        if (base_dir[0] == '\0') {
            const char *sl;
            strncpy(base_dir, paths[i], sizeof(base_dir) - 1);
            base_dir[sizeof(base_dir) - 1] = '\0';
            sl = strrchr(base_dir, '/');
            if (sl)
                base_dir[sl - base_dir] = '\0';
            else
                base_dir[0] = '\0';
        }

        if (S_ISDIR(st.st_mode))
            gather_rc = gather_dir(paths[i]);
        else if (gather_count < MAX_ENTRIES)
            gather_rc = add_gather_file(paths[i]);
        else
            gather_rc = -2;

        if (S_ISDIR(st.st_mode))
            log_message("Gathering directory tree: %s", paths[i]);
        else if (gather_rc == 0)
            log_message("Queued file: %s", paths[i]);

        if (gather_rc != 0)
            break;
    }

    if (gather_rc == -2) {
        log_message("Create aborted: too many files selected (limit %d)",
                    MAX_ENTRIES);
        show_requester("BoltPack", "Too many files to archive");
        free_gather_files();
        return;
    }

    if (gather_rc != 0) {
        log_message("Create aborted during gather: rc=%d", gather_rc);
        show_requester("Error", "Out of memory");
        free_gather_files();
        return;
    }

    if (gather_count == 0) {
        log_message("Create aborted: no files gathered");
        show_requester("BoltPack", "No files to archive");
        free_gather_files();
        return;
    }

    if (!ensure_selected_method_available()) {
        free_gather_files();
        return;
    }

    total_bytes = gathered_total_bytes();
    log_message("Gather complete: files=%d base='%s' total-bytes=%.0f",
                gather_count,
                base_dir[0] ? base_dir : "(none)",
                total_bytes);
    log_memory_snapshot("after gather");

    /* Save dialog */
    freq = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"Save Archive As",
        ASLFR_DoSaveMode, TRUE,
        ASLFR_InitialFile, (ULONG)"archive.bolt",
        TAG_DONE);
    if (!freq) {
        log_message("Create save dialog allocation failed");
        free_gather_files();
        return;
    }

    if (!AslRequest(freq, NULL)) {
        log_message("Create cancelled at save dialog");
        FreeAslRequest(freq);
        free_gather_files();
        return;
    }

    strcpy(out_path, (const char *)freq->fr_Drawer);
    AddPart((STRPTR)out_path, freq->fr_File, BP_MAX_PATH);
    FreeAslRequest(freq);
    log_message("Create output path selected: %s", out_path);

    outfp = fopen(out_path, "wb");
    if (!outfp) {
        log_message("Create failed: cannot open output file '%s'", out_path);
        show_requester("Error", "Cannot create file");
        free_gather_files();
        return;
    }

    show_create_queue(base_dir);
    start_progress_ui(&progress, "Creating archive",
                      method_name(current_method), total_bytes,
                      (unsigned long)gather_count);
    log_memory_snapshot("before create");
    SetPointer(win, NULL, 0, 0, 0, 0);

    outio = make_io(outfp);
    memset(&ar, 0, sizeof(ar));
    rc = bp_archive_create(&ar, &outio, 0, current_method);
    log_message("bp_archive_create('%s') rc=%d (%s) method=%s",
                out_path, rc, bp_strerror(rc), method_name(current_method));
    if (rc != BP_OK) {
        fclose(outfp);
        show_requester("Error", "Create failed");
        ClearPointer(win);
        stop_progress_ui(&progress);
        free_gather_files();
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

        update_create_queue_state(base_dir, i, "[>]");
        rel = gather_files[i];
        if (base_dir[0] && strncmp(rel, base_dir, strlen(base_dir)) == 0)
            rel += strlen(base_dir) + 1;

        if (stat(gather_files[i], &st) != 0) {
            log_message("Create skip %d/%d: stat failed for '%s'",
                        i + 1, gather_count, gather_files[i]);
            update_create_queue_state(base_dir, i, "[!]");
            continue;
        }
        fsize = bp_off_to_u64((bp_off_t)st.st_size);
        mtime = bp_off_to_u64((bp_off_t)st.st_mtime);

        log_message("Create add %d/%d: source='%s' rel='%s' size=%llu",
                    i + 1,
                    gather_count,
                    gather_files[i],
                    rel,
                    u64_to_ull(fsize));

        fp = fopen(gather_files[i], "rb");
        if (!fp) {
            log_message("Create skip %d/%d: cannot open '%s'",
                        i + 1, gather_count, gather_files[i]);
            update_create_queue_state(base_dir, i, "[!]");
            continue;
        }
        fio = make_io(fp);

        rc = bp_archive_add_entry(&ar, rel,
            BP_TYPE_DATA, BP_CREATOR_OS,
            &fio, fsize, mtime);
        fclose(fp);

        log_message("Create result %d/%d: rc=%d (%s)",
                    i + 1, gather_count, rc, bp_strerror(rc));

        if (rc == BP_OK)
            update_create_queue_state(base_dir, i, "[OK]");
        else
            update_create_queue_state(base_dir, i, "[!]");

        if (rc != BP_OK) break;
    }

    if (rc == BP_OK) {
        rc = bp_archive_finalize(&ar);
        log_message("bp_archive_finalize('%s') rc=%d (%s)",
                    out_path, rc, bp_strerror(rc));
    }
    bp_archive_close(&ar);
    fclose(outfp);
    ClearPointer(win);
    stop_progress_ui(&progress);
    free_gather_files();
    log_memory_snapshot("after create");

    if (rc != BP_OK) {
        log_message("Create failed for '%s': rc=%d (%s)",
                    out_path, rc, bp_strerror(rc));
        show_requester("Error", "Create failed");
        refresh_window_title();
        return;
    }

    if (stat(out_path, &out_st) == 0) {
        log_message("Create complete: '%s' size=%lu bytes",
                    out_path, (unsigned long)out_st.st_size);
    } else {
        log_message("Create complete: '%s' size unknown", out_path);
    }

    /* Open the newly created archive */
    do_open(out_path);
}

static void do_create_dialog(void)
{
    struct FileRequester *freq;
    char path[BP_MAX_PATH];
    char *selected_paths[256];
    const char *ptrs[256];
    const char *p;
    int num_args;
    int cnt;
    int i;

    for (i = 0; i < 256; i++)
        selected_paths[i] = NULL;

    freq = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"Select Files to Archive",
        ASLFR_DoMultiSelect, TRUE,
        TAG_DONE);
    if (!freq) return;

    log_message("Create dialog opened");

    if (!AslRequest(freq, NULL)) {
        log_message("Create dialog cancelled by user");
        FreeAslRequest(freq);
        return;
    }

    num_args = (int)freq->fr_NumArgs;
    log_message("Create dialog selection count: %d", num_args);

    if (num_args > 0) {
        /* Multi-select mode */
        cnt = 0;
        for (i = 0; i < num_args && cnt < 256; i++) {
            strcpy(path, (const char *)freq->fr_Drawer);
            if (AddPart((STRPTR)path,
                (STRPTR)freq->fr_ArgList[i].wa_Name, BP_MAX_PATH)) {
                selected_paths[cnt] = dup_string(path);
                if (selected_paths[cnt] == NULL)
                    break;
                ptrs[cnt] = selected_paths[cnt];
                cnt++;
            }
        }
        FreeAslRequest(freq);
        if (i < num_args && cnt < 256 && selected_paths[cnt] == NULL) {
            free_path_array(selected_paths, cnt);
            show_requester("Error", "Out of memory");
            return;
        }
        if (cnt > 0)
            do_create_from_paths(ptrs, cnt);
        free_path_array(selected_paths, cnt);
    } else {
        /* Single file */
        strcpy(path, (const char *)freq->fr_Drawer);
        AddPart((STRPTR)path, freq->fr_File, BP_MAX_PATH);
        FreeAslRequest(freq);
        selected_paths[0] = dup_string(path);
        if (selected_paths[0] == NULL) {
            show_requester("Error", "Out of memory");
            return;
        }
        p = selected_paths[0];
        do_create_from_paths(&p, 1);
        free(selected_paths[0]);
    }
}

/* ====================================================================== */
/*  Drag-and-drop (AppWindow)                                             */
/* ====================================================================== */

static int is_bolt_file(const char *path)
{
    size_t len = strlen(path);
    if (len > 5 && strcmp(path + len - 5, ".bolt") == 0) return 1;
    if (len > 4 && strcmp(path + len - 4, ".blt") == 0) return 1;
    return 0;
}

static void handle_dropped_paths(char **paths, int path_count)
{
    int i;
    const char *ptrs[256];

    if (path_count <= 0)
        return;

    log_message("Received %d dropped path(s)", path_count);

    /* If exactly one .bolt/.blt file, open it */
    if (path_count == 1) {
        if (is_bolt_file(paths[0])) {
            do_open(paths[0]);
            return;
        }
    }

    /* Otherwise create archive from dropped items */
    for (i = 0; i < path_count && i < 256; i++)
        ptrs[i] = paths[i];

    do_create_from_paths(ptrs, path_count);
}

/* ====================================================================== */
/*  Event loop                                                            */
/* ====================================================================== */

static void event_loop(void)
{
    ULONG win_mask, app_mask, mask;
    struct IntuiMessage *imsg;
    struct AppMessage *amsg;
    BOOL running = TRUE;

    win_mask = 1UL << win->UserPort->mp_SigBit;
    app_mask = app_port ? (1UL << app_port->mp_SigBit) : 0;

    while (running) {
        mask = Wait(win_mask | app_mask);

        /* Process window messages */
        if (mask & win_mask) {
            while ((imsg = GT_GetIMsg(win->UserPort)) != NULL) {
                ULONG cl  = imsg->Class;
                UWORD code = imsg->Code;
                struct Gadget *gad = (struct Gadget *)imsg->IAddress;
                GT_ReplyIMsg(imsg);

                switch (cl) {
                    case IDCMP_CLOSEWINDOW:
                        running = FALSE;
                        break;

                    case IDCMP_MENUPICK:
                        while (code != MENUNULL) {
                            struct MenuItem *mi = ItemAddress(menu_strip, code);
                            if (mi) {
                                APTR ud = GTMENUITEM_USERDATA(mi);
                                switch ((int)(ULONG)ud) {
                                    case MN_OPEN:    do_open_dialog(); break;
                                    case MN_EXTRACT: do_extract();     break;
                                    case MN_CLOSE:
                                        close_archive();
                                        clear_entry_list();
                                        free_entry_storage();
                                        refresh_window_title();
                                        refresh_idle_status();
                                        break;
                                    case MN_VALIDATE: do_validate();    break;
                                    case MN_INFO:     do_info();        break;
                                    case MN_CREATE:   do_create_dialog(); break;
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
                                    case MN_QUIT:    running = FALSE;    break;
                                    case MN_ABOUT:   show_about_dialog(); break;
                                    case MN_DOC:
                                        show_requester("Documentation",
                                            "See README for full documentation.\n\n"
                                            "Use Compression to choose Stored, BoltLite,\n"
                                            "boltlz or Fluff for new archives.\n"
                                            "Drop .bolt/.blt files to open them.\n"
                                            "Drop other files or drawers to archive them.");
                                        break;
                                }
                                code = mi->NextSelect;
                            } else {
                                break;
                            }
                        }
                        break;

                    case IDCMP_REFRESHWINDOW:
                        GT_BeginRefresh(win);
                        GT_EndRefresh(win, TRUE);
                        break;
                }
                (void)gad;
            }
        }

        /* Process AppWindow messages (DnD) */
        if (app_port && (mask & app_mask)) {
            while ((amsg = (struct AppMessage *)
                    GetMsg(app_port)) != NULL) {
                char *paths[256];
                int path_count;

                if (amsg->am_Type == AMTYPE_APPWINDOW) {
                    path_count = copy_appmessage_paths(amsg, paths, 256);
                    ReplyMsg((struct Message *)amsg);

                    if (path_count < 0) {
                        show_requester("Error", "Out of memory");
                    } else {
                        handle_dropped_paths(paths, path_count);
                        free_path_array(paths, path_count);
                    }
                } else {
                    ReplyMsg((struct Message *)amsg);
                }
            }
        }
    }
}

/* ====================================================================== */
/*  Startup from Workbench                                                */
/* ====================================================================== */

static void handle_wb_startup(struct WBStartup *wbs)
{
    if (wbs->sm_NumArgs >= 2) {
        /* File was double-clicked or "Open With" */
        char path[BP_MAX_PATH];

        if (build_wbarg_path(wbs->sm_ArgList[1].wa_Lock,
                             (const char *)wbs->sm_ArgList[1].wa_Name,
                             path))
            do_open(path);
    }
}

/* ====================================================================== */
/*  Main                                                                  */
/* ====================================================================== */

int main(void)
{
    struct Gadget *gad;
    struct NewGadget ng;
    UWORD list_top;
    UWORD list_height;
    UWORD status_height;
    UWORD status_top;

    init_log();
    log_message("BoltPack Amiga GUI startup begin");

    /* Open libraries */
    GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37);
    AslBase      = OpenLibrary((STRPTR)"asl.library", 37);
    WorkbenchBase = OpenLibrary((STRPTR)"workbench.library", 37);

    log_message("Library open results: gadtools=%s asl=%s workbench=%s",
                GadToolsBase ? "ok" : "failed",
                AslBase ? "ok" : "failed",
                WorkbenchBase ? "ok" : "failed");

    if (!GadToolsBase || !AslBase) {
        log_message("Startup failed: required libraries missing");
        close_log();
        if (GadToolsBase) CloseLibrary(GadToolsBase);
        if (AslBase)      CloseLibrary(AslBase);
        if (WorkbenchBase) CloseLibrary(WorkbenchBase);
        return 20;
    }

    if (bp_compress_init() != BP_OK) {
        log_message("Startup failed: bp_compress_init returned error");
        close_log();
        CloseLibrary(GadToolsBase);
        CloseLibrary(AslBase);
        if (WorkbenchBase) CloseLibrary(WorkbenchBase);
        return 20;
    }
    log_message("Compression backend initialized successfully");
    log_memory_snapshot("after compression init");
    refresh_idle_status();

    pub_screen = LockPubScreen(NULL);
    if (!pub_screen) goto cleanup_libs;
    log_message("Public screen locked");

    vi = GetVisualInfo(pub_screen, TAG_DONE);
    if (!vi) goto cleanup_screen;
    log_message("Visual info acquired");

    /* Create gadgets */
    gad = CreateContext(&gad_list);
    if (!gad) goto cleanup_vi;

    memset(&ng, 0, sizeof(ng));
    list_top          = pub_screen->BarHeight + 4;
    list_height       = 296;
    status_height     = (UWORD)(pub_screen->Font->ta_YSize + 6);
    status_top        = list_top + list_height + 6;

    ng.ng_LeftEdge   = 4;
    ng.ng_TopEdge    = list_top;
    ng.ng_Width      = 632;
    ng.ng_Height     = list_height;
    ng.ng_GadgetID   = GID_LISTVIEW;
    ng.ng_Flags      = 0;
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr   = pub_screen->Font;

    init_entry_list();

    listview_gad = CreateGadget(LISTVIEW_KIND, gad, &ng,
        GTLV_Labels, (ULONG)&entry_list,
        GTLV_ReadOnly, TRUE,
        GTLV_ShowSelected, 0L,
        TAG_DONE);
    if (!listview_gad) goto cleanup_gad;

    ng.ng_TopEdge  = status_top;
    ng.ng_Height   = status_height;
    ng.ng_GadgetID = GID_STATUS1;
    status1_gad = CreateGadget(TEXT_KIND, listview_gad, &ng,
        GTTX_Text, (ULONG)status_lines[0],
        GTTX_Border, TRUE,
        TAG_DONE);
    if (!status1_gad) goto cleanup_gad;

    ng.ng_TopEdge  = (UWORD)(status_top + status_height + 2);
    ng.ng_GadgetID = GID_STATUS2;
    status2_gad = CreateGadget(TEXT_KIND, status1_gad, &ng,
        GTTX_Text, (ULONG)status_lines[1],
        GTTX_Border, TRUE,
        TAG_DONE);
    if (!status2_gad) goto cleanup_gad;

    ng.ng_TopEdge  = (UWORD)(status_top + (status_height + 2) * 2);
    ng.ng_GadgetID = GID_STATUS3;
    status3_gad = CreateGadget(TEXT_KIND, status2_gad, &ng,
        GTTX_Text, (ULONG)status_lines[2],
        GTTX_Border, TRUE,
        TAG_DONE);
    if (!status3_gad) goto cleanup_gad;

    /* Create menu */
    menu_strip = CreateMenus(new_menus, TAG_DONE);
    if (!menu_strip) goto cleanup_gad;
    LayoutMenus(menu_strip, vi, TAG_DONE);

    /* Open window */
    win = OpenWindowTags(NULL,
        WA_Left,        40,
        WA_Top,         20,
        WA_Width,       640,
        WA_Height,      400,
        WA_Title,       (ULONG)"BoltPack",
        WA_Flags,       WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET |
                        WFLG_SIZEGADGET | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        WA_IDCMP,       IDCMP_CLOSEWINDOW | IDCMP_MENUPICK |
                        IDCMP_REFRESHWINDOW | IDCMP_GADGETUP,
        WA_Gadgets,     (ULONG)gad_list,
        WA_PubScreen,   (ULONG)pub_screen,
        WA_MinWidth,    320,
        WA_MinHeight,   200,
        WA_MaxWidth,    -1,
        WA_MaxHeight,   -1,
        TAG_DONE);

    if (!win) goto cleanup_menu;
    log_message("Main window opened");
    log_memory_snapshot("after window open");

    SetMenuStrip(win, menu_strip);
    sync_compression_menu_checks();
    GT_RefreshWindow(win, NULL);
    refresh_window_title();
    refresh_idle_status();

    /* Set up AppWindow for DnD */
    app_port = CreateMsgPort();
    if (app_port && WorkbenchBase)
        app_win = AddAppWindowA(0, 0, win, app_port, NULL);
    log_message("AppWindow setup: msgport=%s appwindow=%s",
                app_port ? "ok" : "failed",
                app_win ? "ok" : "disabled");

    /* Handle Workbench startup arguments */
    {
        extern struct WBStartup *_WBenchMsg;
        if (_WBenchMsg)
            handle_wb_startup(_WBenchMsg);
    }

    /* Run */
    log_message("Entering main event loop");
    event_loop();
    log_message("Leaving main event loop");

    /* Cleanup */
    close_archive();
    free_entry_storage();
    free_gather_files();

    if (app_win) RemoveAppWindow(app_win);
    if (app_port) {
        struct Message *msg;
        while ((msg = GetMsg(app_port)) != NULL)
            ReplyMsg(msg);
        DeleteMsgPort(app_port);
    }

    ClearMenuStrip(win);
    CloseWindow(win);

cleanup_menu:
    if (menu_strip) FreeMenus(menu_strip);
cleanup_gad:
    if (gad_list) FreeGadgets(gad_list);
cleanup_vi:
    FreeVisualInfo(vi);
cleanup_screen:
    UnlockPubScreen(NULL, pub_screen);
cleanup_libs:
    log_memory_snapshot("shutdown");
    log_message("BoltPack Amiga GUI shutdown complete");
    close_log();
    CloseLibrary(GadToolsBase);
    CloseLibrary(AslBase);
    if (WorkbenchBase) CloseLibrary(WorkbenchBase);

    return 0;
}
