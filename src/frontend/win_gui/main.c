/*
 * main.c -- Win32 GUI frontend for BoltPack (Windows 95+)
 *
 * Pure Win32 API, no MFC/ATL. Links the archiver core statically.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define _WIN32_WINNT 0x0400
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <fcntl.h>
#include <io.h>

#include "resource.h"

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/boltlite.h"
#include "../../core/bp_metadata.h"
#include "../gui_common/help_text.h"

/* ---- constants --------------------------------------------------------- */

#define APP_CLASS_NAME  "BoltPackGuiClass"
#define MAX_ENTRIES     65536

/* ---- bp_io_t adapters for FILE * --------------------------------------- */

static long file_read(void *handle, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)handle);
}

static long file_write(void *handle, const void *buf, bp_uint32 count)
{
    return (long)fwrite(buf, 1, (size_t)count, (FILE *)handle);
}

static int file_seek(void *handle, bp_off_t offset, int origin)
{
    return _fseeki64((FILE *)handle, (__int64)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)_ftelli64((FILE *)handle);
}

static void bp_io_from_file(bp_io_t *io, FILE *fp)
{
    io->read   = file_read;
    io->write  = file_write;
    io->seek   = file_seek;
    io->tell   = file_tell;
    io->handle = fp;
}

/* ---- error string ------------------------------------------------------ */

static const char *bp_strerror(int code)
{
    switch (code) {
    case BP_OK:                     return "success";
    case BP_ERR_IO:                 return "I/O error";
    case BP_ERR_NOMEM:              return "out of memory";
    case BP_ERR_MAGIC:              return "not a BoltPack archive";
    case BP_ERR_VERSION:            return "unsupported archive version";
    case BP_ERR_CHECKSUM:           return "checksum mismatch";
    case BP_ERR_CORRUPT:            return "archive is corrupt";
    case BP_ERR_TRUNCATED:          return "unexpected end of file";
    case BP_ERR_OVERFLOW:           return "size overflow";
    case BP_ERR_NAME_TOO_LONG:      return "file name too long";
    case BP_ERR_UNSUPPORTED_METHOD: return "unsupported compression method";
    case BP_ERR_COMPRESS:           return "compression failed";
    case BP_ERR_DECOMPRESS:         return "decompression failed";
    case BP_ERR_ABORTED:            return "operation aborted";
    case BP_ERR_NOT_FOUND:          return "entry not found";
    case BP_ERR_INVALID_ARG:        return "invalid argument";
    case BP_ERR_PARTIAL:            return "some names or attributes were not restored";
    default:                        return "unknown error";
    }
}

/* ---- formatting helpers ------------------------------------------------ */

static const char *method_name(int m)
{
    switch (m) {
    case BP_METHOD_STORED: return "Stored";
    case BP_METHOD_BOLTLITE:   return "BoltLite";
    case BP_METHOD_BOLTLZ:  return "boltlz";
    case BP_METHOD_FLUFF:  return "fluff";
    default:               return "?";
    }
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

static double u64_to_double(bp_uint64 v)
{
    return (double)v.lo + (double)v.hi * 4294967296.0;
}

static const char *os_name(int os)
{
    switch (os) {
    case BP_OS_UNKNOWN:   return "Unknown";
    case BP_OS_LINUX:     return "Linux";
    case BP_OS_MACOS9:    return "Mac OS 9";
    case BP_OS_MACOSX:    return "Mac OS X";
    case BP_OS_AMIGA_68K: return "AmigaOS (68k)";
    case BP_OS_AMIGA_PPC: return "AmigaOS (PPC)";
    case BP_OS_MORPHOS:   return "MorphOS";
    case BP_OS_AROS:      return "AROS";
    case BP_OS_DOS:       return "DOS";
    case BP_OS_WINDOWS:   return "Windows";
    case BP_OS_FREEBSD:   return "FreeBSD";
    case BP_OS_NETBSD:    return "NetBSD";
    case BP_OS_OPENBSD:   return "OpenBSD";
    case BP_OS_SOLARIS:   return "Solaris";
    case BP_OS_IRIX:      return "IRIX";
    case BP_OS_HPUX:      return "HP-UX";
    case BP_OS_AIX:       return "AIX";
    case BP_OS_ATARI:     return "Atari ST";
    case BP_OS_RISCOS:    return "RISC OS";
    case BP_OS_OS2:       return "OS/2";
    case BP_OS_BEOS:      return "BeOS / Haiku";
    case BP_OS_OPENVMS:   return "OpenVMS";
    case BP_OS_ZOS:       return "z/OS";
    case BP_OS_WASM:      return "WebAssembly";
    case BP_OS_NEXTSTEP:  return "NeXTSTEP";
    case BP_OS_QNX:       return "QNX";
    case BP_OS_PLAN9:     return "Plan 9";
    case BP_OS_TRU64:     return "Tru64 UNIX";
    case BP_OS_SCO:       return "SCO UNIX";
    case BP_OS_HURD:      return "GNU/Hurd";
    default:              return "Unknown";
    }
}

static const char *cpu_name(int cpu)
{
    switch (cpu) {
    case BP_CPU_UNKNOWN: return "Unknown";
    case BP_CPU_X86:     return "x86";
    case BP_CPU_X86_64:  return "x86-64";
    case BP_CPU_ARM32:   return "ARM32";
    case BP_CPU_ARM64:   return "ARM64";
    case BP_CPU_M68K:    return "68k";
    case BP_CPU_PPC32:   return "PowerPC 32";
    case BP_CPU_PPC64:   return "PowerPC 64";
    case BP_CPU_MIPS32:  return "MIPS32";
    case BP_CPU_MIPS64:  return "MIPS64";
    case BP_CPU_SPARC:   return "SPARC";
    case BP_CPU_SPARC64: return "SPARC64";
    case BP_CPU_RISCV32: return "RISC-V 32";
    case BP_CPU_RISCV64: return "RISC-V 64";
    case BP_CPU_ALPHA:   return "Alpha";
    case BP_CPU_PARISC:  return "PA-RISC";
    case BP_CPU_SH:      return "SuperH";
    case BP_CPU_6502:    return "6502";
    case BP_CPU_Z80:     return "Z80";
    case BP_CPU_8086:    return "8086";
    case BP_CPU_E2K:     return "Elbrus";
    case BP_CPU_ZARCH:   return "z/Architecture";
    case BP_CPU_IA64:    return "Itanium";
    default:             return "Unknown";
    }
}

static void format_size(double bytes, char *buf, size_t bufsize)
{
    if (bytes < 1024.0)
        _snprintf(buf, bufsize, "%.0f B", bytes);
    else if (bytes < 1048576.0)
        _snprintf(buf, bufsize, "%.1f KB", bytes / 1024.0);
    else if (bytes < 1073741824.0)
        _snprintf(buf, bufsize, "%.1f MB", bytes / 1048576.0);
    else
        _snprintf(buf, bufsize, "%.2f GB", bytes / 1073741824.0);
    buf[bufsize - 1] = '\0';
}

static void format_time(bp_uint64 mtime, char *buf, size_t bufsize)
{
    time_t t = (time_t)bp_u64_to_ulong(mtime);
    struct tm *tm = localtime(&t);
    if (tm)
        strftime(buf, bufsize, "%Y-%m-%d %H:%M", tm);
    else
        _snprintf(buf, bufsize, "-");
    buf[bufsize - 1] = '\0';
}

/* ---- Windows codepage helpers ------------------------------------------ */
/* Convert UTF-8 path to ANSI (ACP) for file operations on Win9x */

static int utf8_to_acp(const char *utf8, char *acp, int acp_size)
{
    WCHAR wbuf[1024];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, 1024);
    if (wlen <= 0) {
        strncpy(acp, utf8, acp_size);
        acp[acp_size - 1] = '\0';
        return 0;
    }
    WideCharToMultiByte(CP_ACP, 0, wbuf, -1, acp, acp_size, NULL, NULL);
    acp[acp_size - 1] = '\0';
    return 1;
}

/* Extraction must not silently replace an unrepresentable character. */
static int utf8_to_acp_strict(const char *utf8, char *acp, int acp_size)
{
    WCHAR wide[MAX_PATH];
    UINT codepage = GetACP();
    BOOL used_default = FALSE;
    DWORD flags = codepage == CP_UTF8 ? 0 : WC_NO_BEST_FIT_CHARS;
    int wide_len, bytes;
    if (utf8 == NULL || acp == NULL || acp_size <= 0) return -1;
    wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                                   wide, MAX_PATH);
    if (wide_len <= 0) return -1;
    bytes = WideCharToMultiByte(codepage, flags, wide, wide_len, acp, acp_size,
                                NULL, codepage == CP_UTF8 ? NULL : &used_default);
    if (bytes <= 0 || used_default) return -1;
    return 0;
}

/* ---- entry storage ----------------------------------------------------- */

typedef struct {
    char    name[512];
    double  orig_size;
    double  comp_size;
    int     ratio;
    int     method;
    char    date[32];
    int     entry_type;
} EntryInfo;

static EntryInfo *g_entries = NULL;
static int         g_entry_count = 0;
static char        g_archive_path[MAX_PATH] = {0};
static bp_archive_info_t g_info;
static int         g_archive_open = 0;

/* ---- globals ----------------------------------------------------------- */

static HINSTANCE g_hInst;
static HWND      g_hWnd;
static HWND      g_hListView;
static HMENU     g_hMenu;
static HACCEL    g_hAccel;

/* ---- progress context -------------------------------------------------- */

typedef struct {
    HWND     hDlg;
    int      cancelled;
    int      bar_shown;
    DWORD    start_tick;
} ProgressCtx;

static ProgressCtx g_progress;

/* ---- forward declarations ---------------------------------------------- */

typedef struct { const char *title; const char *body; } TextDlgData;

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK AboutDlgProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK ProgressDlgProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK DocDlgProc(HWND, UINT, WPARAM, LPARAM);
static INT_PTR CALLBACK GenericTextDlgProc(HWND, UINT, WPARAM, LPARAM);

static void DoOpen(const char *path);
static void DoClose(void);
static void DoExtract(void);
static void DoValidate(void);
static void DoInfo(void);
static void DoCreate(void);
static void UpdateMenuState(void);
static void PopulateListView(void);
static void ShowLogo(HWND hWnd, HDC hdc);

/* classify-and-act for drag-and-drop */
static int HasBoltExtension(const char *path);
static void ClassifyAndAct(char paths[][MAX_PATH], int count);

/* ---- mkdir recursive (Windows) ----------------------------------------- */

static void MakeDirs(const char *path)
{
    char tmp[MAX_PATH];
    char *p;
    strncpy(tmp, path, MAX_PATH);
    tmp[MAX_PATH - 1] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char old = *p;
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = old;
        }
    }
    CreateDirectoryA(tmp, NULL);
}

static void MakeDirsForFile(const char *path)
{
    char tmp[MAX_PATH];
    char *slash;
    strncpy(tmp, path, MAX_PATH);
    tmp[MAX_PATH - 1] = '\0';
    slash = strrchr(tmp, '\\');
    if (!slash) slash = strrchr(tmp, '/');
    if (slash) {
        *slash = '\0';
        MakeDirs(tmp);
    }
}

/* ---- list callback ----------------------------------------------------- */

static int list_cb(const bp_entry_t *entry, void *user_data)
{
    (void)user_data;

    if (entry->entry_type == BP_TYPE_META || entry->entry_type == BP_TYPE_RSRC)
        return 0;

    if (g_entry_count >= MAX_ENTRIES)
        return 0;

    {
        EntryInfo *e = &g_entries[g_entry_count];
        if (entry->name) {
            utf8_to_acp(entry->name, e->name, sizeof(e->name));
        } else {
            e->name[0] = '\0';
        }
        e->orig_size  = u64_to_double(entry->original_size);
        e->comp_size  = u64_to_double(entry->compressed_size);
        e->ratio      = (e->orig_size > 0)
                        ? (int)(100.0 - e->comp_size / e->orig_size * 100.0)
                        : 0;
        e->method     = entry->method;
        format_time(entry->mtime, e->date, sizeof(e->date));
        e->entry_type = entry->entry_type;
        g_entry_count++;
    }
    return 0;
}

/* ---- open archive ------------------------------------------------------ */

static void DoOpen(const char *path)
{
    FILE *fp;
    bp_io_t io;
    int rc;

    fp = fopen(path, "rb");
    if (!fp) {
        MessageBoxA(g_hWnd, "Cannot open file.", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    bp_io_from_file(&io, fp);

    memset(&g_info, 0, sizeof(g_info));
    rc = bp_archive_info(&io, &g_info);
    if (rc != BP_OK) {
        fclose(fp);
        MessageBoxA(g_hWnd, bp_strerror(rc), "Error", MB_OK | MB_ICONERROR);
        return;
    }

    /* list entries */
    if (!g_entries) {
        g_entries = (EntryInfo *)malloc(sizeof(EntryInfo) * MAX_ENTRIES);
        if (!g_entries) { fclose(fp); return; }
    }
    g_entry_count = 0;

    _fseeki64(fp, 0, SEEK_SET);
    rc = bp_archive_list(&io, list_cb, NULL);
    fclose(fp);

    if (rc != BP_OK) {
        g_entry_count = 0;
        MessageBoxA(g_hWnd, bp_strerror(rc), "Error", MB_OK | MB_ICONERROR);
        return;
    }

    strncpy(g_archive_path, path, MAX_PATH);
    g_archive_path[MAX_PATH - 1] = '\0';
    g_archive_open = 1;

    PopulateListView();
    UpdateMenuState();
    InvalidateRect(g_hWnd, NULL, TRUE);
}

/* ---- close archive ----------------------------------------------------- */

static void DoClose(void)
{
    g_entry_count = 0;
    g_archive_path[0] = '\0';
    memset(&g_info, 0, sizeof(g_info));
    g_archive_open = 0;

    ListView_DeleteAllItems(g_hListView);
    ShowWindow(g_hListView, SW_HIDE);
    UpdateMenuState();
    InvalidateRect(g_hWnd, NULL, TRUE);
}

/* ---- validate ---------------------------------------------------------- */

typedef struct {
    int ok_count;
    int fail_count;
    char *log;
    int log_size;
    int log_used;
} ValidateCtx;

static void validate_cb(const bp_entry_t *entry, int result, void *user_data)
{
    ValidateCtx *ctx = (ValidateCtx *)user_data;
    char line[600];
    int len;
    if (result == BP_OK) {
        _snprintf(line, sizeof(line), "  OK      %s\r\n",
                  entry->name ? entry->name : "?");
        ctx->ok_count++;
    } else {
        _snprintf(line, sizeof(line), "  FAILED  %s  (%s)\r\n",
                  entry->name ? entry->name : "?",
                  bp_strerror(result));
        ctx->fail_count++;
    }
    line[sizeof(line) - 1] = '\0';
    len = (int)strlen(line);
    if (ctx->log_used + len + 1 < ctx->log_size) {
        memcpy(ctx->log + ctx->log_used, line, (size_t)len + 1);
        ctx->log_used += len;
    }
}

static int validate_progress_cb(const bp_progress_t *p, void *user_data)
{
    ProgressCtx *ctx = (ProgressCtx *)user_data;
    char buf[512];
    char acp_name[512];
    double total;
    double done;
    unsigned long file_total;
    unsigned long dir_total;
    unsigned long entry_total;
    MSG msg;

    if (p->file_name) {
        utf8_to_acp(p->file_name, acp_name, sizeof(acp_name));
        SetDlgItemTextA(ctx->hDlg, IDC_PROG_FILE, acp_name);
    }

    file_total = (unsigned long)(g_info.entry_count - g_info.dir_count);
    dir_total = (unsigned long)g_info.dir_count;
    entry_total = (unsigned long)g_info.entry_count;
    _snprintf(buf, sizeof(buf),
              "%lu files, %lu directories; validating entry %lu of %lu",
              file_total,
              dir_total,
              (unsigned long)(p->file_index + 1),
              entry_total ? entry_total : (unsigned long)p->file_count);
    buf[sizeof(buf) - 1] = '\0';
    SetDlgItemTextA(ctx->hDlg, IDC_PROG_COUNT, buf);

    if (!ctx->bar_shown) {
        DWORD now = GetTickCount();
        if (now - ctx->start_tick >= 500) {
            ShowWindow(GetDlgItem(ctx->hDlg, IDC_PROG_BAR), SW_SHOW);
            ctx->bar_shown = 1;
        }
    }

    if (ctx->bar_shown) {
        total = u64_to_double(g_info.total_original);
        if (total <= 0.0)
            total = u64_to_double(p->archive_orig_total);
        done = u64_to_double(p->archive_orig_done);

        {
            int pct = (total > 0.0) ? (int)(done / total * 100.0) : 0;
            if (pct > 100) pct = 100;
            SendDlgItemMessageA(ctx->hDlg, IDC_PROG_BAR, PBM_SETPOS, pct, 0);
        }
    }

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return ctx->cancelled ? 1 : 0;
}

static void DoValidate(void)
{
    FILE *fp;
    bp_io_t io;
    ValidateCtx ctx;
    char log_buf[65536];
    char summary[128];
    TextDlgData dlg_data;
    char full_buf[66048];
    int rc;

    if (!g_archive_open) return;

    fp = fopen(g_archive_path, "rb");
    if (!fp) {
        MessageBoxA(g_hWnd, "Cannot open archive for testing.",
                    "Validate", MB_OK | MB_ICONERROR);
        return;
    }

    bp_io_from_file(&io, fp);
    ctx.ok_count   = 0;
    ctx.fail_count = 0;
    ctx.log        = log_buf;
    ctx.log_size   = (int)sizeof(log_buf);
    ctx.log_used   = 0;
    log_buf[0]     = '\0';

    g_progress.cancelled  = 0;
    g_progress.bar_shown  = 0;
    g_progress.start_tick = GetTickCount();
    g_progress.hDlg = CreateDialogA(g_hInst, MAKEINTRESOURCEA(IDD_PROGRESS),
                                    g_hWnd, ProgressDlgProc);
    ShowWindow(GetDlgItem(g_progress.hDlg, IDC_PROG_BAR), SW_HIDE);
    ShowWindow(g_progress.hDlg, SW_SHOW);
    SetWindowTextA(g_progress.hDlg, "Validating archive...");
    SetDlgItemTextA(g_progress.hDlg, IDC_PROG_FILE, "Preparing validation...");
    _snprintf(summary, sizeof(summary),
              "%lu files, %lu directories; preparing validation...",
              (unsigned long)(g_info.entry_count - g_info.dir_count),
              (unsigned long)g_info.dir_count);
    summary[sizeof(summary) - 1] = '\0';
    SetDlgItemTextA(g_progress.hDlg, IDC_PROG_COUNT, summary);

    rc = bp_archive_test_progress(&io, validate_cb, &ctx,
                                  validate_progress_cb, &g_progress);

    DestroyWindow(g_progress.hDlg);
    fclose(fp);

    if (rc == BP_ERR_ABORTED) {
        MessageBoxA(g_hWnd, "Validation cancelled.",
                    "Validate", MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (rc != BP_OK && rc != BP_ERR_CHECKSUM) {
        MessageBoxA(g_hWnd, bp_strerror(rc), "Validate", MB_OK | MB_ICONERROR);
        return;
    }

    if (ctx.fail_count == 0)
        _snprintf(summary, sizeof(summary),
                  "%d file%s tested -- all OK",
                  ctx.ok_count, ctx.ok_count == 1 ? "" : "s");
    else
        _snprintf(summary, sizeof(summary),
                  "%d OK, %d FAILED", ctx.ok_count, ctx.fail_count);
    summary[sizeof(summary) - 1] = '\0';

    _snprintf(full_buf, sizeof(full_buf), "%s\r\n\r\n%s",
              summary, log_buf);
    full_buf[sizeof(full_buf) - 1] = '\0';

    dlg_data.title = "Validate Archive";
    dlg_data.body  = full_buf;
    DialogBoxParamA(g_hInst, MAKEINTRESOURCEA(IDD_DOC),
                    g_hWnd, GenericTextDlgProc, (LPARAM)&dlg_data);
}

/* ---- info -------------------------------------------------------------- */

static void DoInfo(void)
{
    char text[2048];
    bp_uint32 file_count;
    double orig_d, comp_d, arc_d;
    int pct;
    TextDlgData dlg_data;

    if (!g_archive_open) return;

    file_count = g_info.entry_count - g_info.dir_count;
    orig_d = u64_to_double(g_info.total_original);
    comp_d = u64_to_double(g_info.total_compressed);
    arc_d  = u64_to_double(g_info.archive_size);
    pct    = (orig_d > 0.0) ? (int)(comp_d / orig_d * 100.0) : 0;

    _snprintf(text, sizeof(text),
        "Archive:         %s\r\n"
        "Format version:  %u\r\n"
        "Created by:      BoltPack %u.%u\r\n"
        "Creator OS:      %s\r\n"
        "Creator CPU:     %s\r\n"
        "Method:          %s\r\n"
        "\r\n"
        "Archive size:    %s\r\n"
        "Entries:         %lu  (%lu file%s, %lu director%s)\r\n"
        "Original size:   %s\r\n"
        "Compressed:      %s  (%d%% of original)",
        g_archive_path,
        (unsigned)g_info.version,
        (unsigned)g_info.creator_ver_major,
        (unsigned)g_info.creator_ver_minor,
        os_name(g_info.creator_os),
        cpu_name(g_info.creator_cpu),
        format_methods(g_info.method_counts),
        "?",  /* placeholder; filled below */
        (unsigned long)g_info.entry_count,
        (unsigned long)file_count,  file_count == 1 ? "" : "s",
        (unsigned long)g_info.dir_count, g_info.dir_count == 1 ? "y" : "ies",
        "?",  /* placeholder; filled below */
        "?",  /* placeholder; filled below */
        pct);

    /* Build properly since format_size writes to a char* */
    {
        char arc_s[64], orig_s[64], comp_s[64];
        format_size(arc_d,  arc_s,  sizeof(arc_s));
        format_size(orig_d, orig_s, sizeof(orig_s));
        format_size(comp_d, comp_s, sizeof(comp_s));
        _snprintf(text, sizeof(text),
            "Archive:         %s\r\n"
            "Format version:  %u\r\n"
            "Created by:      BoltPack %u.%u\r\n"
            "Creator OS:      %s\r\n"
            "Creator CPU:     %s\r\n"
            "Method:          %s\r\n"
            "\r\n"
            "Archive size:    %s\r\n"
            "Entries:         %lu  (%lu file%s, %lu director%s)\r\n"
            "Original size:   %s\r\n"
            "Compressed:      %s  (%d%% of original)",
            g_archive_path,
            (unsigned)g_info.version,
            (unsigned)g_info.creator_ver_major,
            (unsigned)g_info.creator_ver_minor,
            os_name(g_info.creator_os),
            cpu_name(g_info.creator_cpu),
            format_methods(g_info.method_counts),
            arc_s,
            (unsigned long)g_info.entry_count,
            (unsigned long)file_count,  file_count == 1 ? "" : "s",
            (unsigned long)g_info.dir_count, g_info.dir_count == 1 ? "y" : "ies",
            orig_s, comp_s, pct);
        text[sizeof(text) - 1] = '\0';
    }

    dlg_data.title = "Archive Information";
    dlg_data.body  = text;
    DialogBoxParamA(g_hInst, MAKEINTRESOURCEA(IDD_DOC),
                    g_hWnd, GenericTextDlgProc, (LPARAM)&dlg_data);
}

/* ---- progress callback ------------------------------------------------- */

static int progress_cb(const bp_progress_t *p, void *user_data)
{
    ProgressCtx *ctx = (ProgressCtx *)user_data;
    char buf[512];
    MSG msg;

    /* update file label */
    if (p->file_name) {
        char acp_name[512];
        utf8_to_acp(p->file_name, acp_name, sizeof(acp_name));
        SetDlgItemTextA(ctx->hDlg, IDC_PROG_FILE, acp_name);
    }

    /* update count */
    _snprintf(buf, sizeof(buf), "File %lu of %lu",
              (unsigned long)(p->file_index + 1),
              (unsigned long)p->file_count);
    buf[sizeof(buf) - 1] = '\0';
    SetDlgItemTextA(ctx->hDlg, IDC_PROG_COUNT, buf);

    /* show progress bar after 0.5 seconds */
    if (!ctx->bar_shown) {
        DWORD now = GetTickCount();
        if (now - ctx->start_tick >= 500) {
            ShowWindow(GetDlgItem(ctx->hDlg, IDC_PROG_BAR), SW_SHOW);
            ctx->bar_shown = 1;
        }
    }

    /* update bar */
    if (ctx->bar_shown) {
        double total = u64_to_double(p->archive_orig_total);
        double done  = u64_to_double(p->archive_orig_done);
        if (!bp_u64_is_zero(p->file_orig_size)) {
            double ff = u64_to_double(p->file_bytes_done) /
                        u64_to_double(p->file_orig_size);
            done += ff * u64_to_double(p->file_orig_size);
        }
        {
            int pct = (total > 0) ? (int)(done / total * 100.0) : 0;
            SendDlgItemMessageA(ctx->hDlg, IDC_PROG_BAR, PBM_SETPOS, pct, 0);
        }
    }

    /* pump messages to stay responsive */
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return ctx->cancelled ? 1 : 0;
}

/* ---- extract ----------------------------------------------------------- */

typedef struct win_gui_name_node_s {
    struct win_gui_name_node_s *next;
    WCHAR name[MAX_PATH];
} WinGuiNameNode;

typedef struct win_gui_dir_restore_s {
    struct win_gui_dir_restore_s *next;
    char path[MAX_PATH];
    bp_uint64 mtime;
    int mtime_valid;
    bp_uint8 fat_valid;
    bp_uint8 fat_value;
} WinGuiDirectoryRestore;

typedef struct {
    bp_io_t io;
    char target[MAX_PATH];
    char temporary[MAX_PATH];
} WinGuiStagedFile;

typedef struct {
    bp_io_t io;
    bp_uint8 data[BP_META_MAX_SIZE];
    unsigned length;
} WinGuiMetadataSink;

typedef struct {
    char dest_dir[MAX_PATH];
    char last_subject_name[MAX_PATH];
    char last_subject_path[MAX_PATH];
    bp_uint8 last_subject_type;
    int last_subject_valid;
    int last_subject_ok;
    ProgressCtx *progress;
    WinGuiNameNode *names;
    WinGuiDirectoryRestore *directories;
    WinGuiMetadataSink metadata;
    int open_error;
    unsigned int partial_count;
    unsigned int directory_count;
} ExtractCtx;

static LONG g_gui_temp_counter = 0;
static int g_create_reparse_refused = 0;
static int g_interchange_profile = 0;

static int win_gui_validate_component(const WCHAR *component, size_t length)
{
    size_t i, base_length = length;
    WCHAR base[16];
    if (length == 0 || component[length - 1] == L'.' || component[length - 1] == L' ')
        return 0;
    for (i = 0; i < length; i++) {
        WCHAR c = component[i];
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'|' || c == L'?' || c == L'*') return 0;
    }
    for (i = 0; i < length; i++) {
        if (component[i] == L'.') { base_length = i; break; }
    }
    if (base_length == 0 || base_length >= sizeof(base) / sizeof(base[0])) return 1;
    memcpy(base, component, base_length);
    base[base_length] = L'\0';
    if (_wcsicmp(base, L"CON") == 0 || _wcsicmp(base, L"PRN") == 0 ||
        _wcsicmp(base, L"AUX") == 0 || _wcsicmp(base, L"NUL") == 0) return 0;
    if (base_length == 4 &&
        (_wcsnicmp(base, L"COM", 3) == 0 || _wcsnicmp(base, L"LPT", 3) == 0) &&
        base[3] >= L'1' && base[3] <= L'9') return 0;
    return 1;
}

static int win_gui_project_name(const char *utf8, char *local, size_t capacity)
{
    WCHAR wide[MAX_PATH];
    size_t length, start, i;
    int wide_length;
    if (utf8 == NULL || !bp_name_is_safe(utf8) ||
        utf8_to_acp_strict(utf8, local, (int)capacity) != 0) return BP_ERR_PARTIAL;
    wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                                      wide, MAX_PATH);
    if (wide_length <= 0) return BP_ERR_PARTIAL;
    length = (size_t)wide_length - 1;
    if (length == 0 || length >= MAX_PATH) return BP_ERR_NAME_TOO_LONG;
    start = 0;
    for (i = 0; i <= length; i++) {
        size_t component_length;
        if (i < length && wide[i] != L'/') continue;
        component_length = i - start;
        if (component_length == 0) {
            if (i == length && length > 0 && wide[length - 1] == L'/') break;
            return BP_ERR_INVALID_ARG;
        }
        if (!win_gui_validate_component(wide + start, component_length))
            return BP_ERR_INVALID_ARG;
        start = i + 1;
    }
    length = strlen(local);
    if (length == 0 || length >= capacity || length >= MAX_PATH)
        return BP_ERR_NAME_TOO_LONG;
    {
        char *p = local;
        while (*p) {
            if (*p == '/') *p = '\\';
            p = CharNextA(p);
        }
    }
    return BP_OK;
}

static int win_gui_register_name(ExtractCtx *ctx, const char *name)
{
    WCHAR normalized[MAX_PATH];
    int length;
    WinGuiNameNode *node, **at;
    unsigned int count = 0;
    length = MultiByteToWideChar(CP_ACP, 0, name, -1, normalized, MAX_PATH);
    if (length <= 0) return BP_ERR_PARTIAL;
    if (length > 1 && normalized[length - 2] == L'\\') normalized[length - 2] = L'\0';
    for (node = ctx->names; node != NULL; node = node->next) {
        if (_wcsicmp(node->name, normalized) == 0) return BP_ERR_PARTIAL;
        count++;
    }
    if (count >= 65535U) return BP_ERR_OVERFLOW;
    node = (WinGuiNameNode *)calloc(1, sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    wcscpy(node->name, normalized);
    at = &ctx->names;
    while (*at != NULL) at = &(*at)->next;
    *at = node;
    return BP_OK;
}

static int win_gui_make_full_path(const ExtractCtx *ctx, const char *relative,
                                  char *full, size_t full_size)
{
    size_t root_length = strlen(ctx->dest_dir);
    size_t relative_length = strlen(relative);
    const char *last_root_char = root_length == 0 ? ctx->dest_dir :
                                 CharPrevA(ctx->dest_dir, ctx->dest_dir + root_length);
    int separator = root_length != 0 && *last_root_char != '\\' &&
                    *last_root_char != '/';
    if (root_length + (size_t)separator + relative_length + 1 > full_size ||
        root_length + (size_t)separator + relative_length >= MAX_PATH)
        return BP_ERR_NAME_TOO_LONG;
    memcpy(full, ctx->dest_dir, root_length);
    if (separator) full[root_length++] = '\\';
    memcpy(full + root_length, relative, relative_length + 1);
    return BP_OK;
}

static int win_gui_ensure_directories(ExtractCtx *ctx, const char *relative,
                                      int include_leaf)
{
    char local[MAX_PATH], current[MAX_PATH];
    char *part, *next, *scan;
    DWORD attributes;
    size_t local_length;
    if (strlen(relative) >= sizeof(local)) return BP_ERR_NAME_TOO_LONG;
    strcpy(local, relative);
    local_length = strlen(local);
    if (local_length > 0) {
        char *last = CharPrevA(local, local + local_length);
        if (*last == '\\' && CharNextA(last) == local + local_length) *last = '\0';
    }
    if (strlen(ctx->dest_dir) >= sizeof(current)) return BP_ERR_NAME_TOO_LONG;
    strcpy(current, ctx->dest_dir);
    attributes = GetFileAttributesA(current);
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return BP_ERR_IO;
    part = local;
    while (*part != '\0') {
        next = NULL;
        for (scan = part; *scan; scan = CharNextA(scan)) {
            if (*scan == '\\') { next = scan; break; }
        }
        if (next != NULL) *next = '\0';
        if (next == NULL && !include_leaf) break;
        if (strlen(current) + strlen(part) + 2 >= sizeof(current))
            return BP_ERR_NAME_TOO_LONG;
        {
            size_t current_length = strlen(current);
            char *last = CharPrevA(current, current + current_length);
            if (*last != '\\' && *last != '/') strcat(current, "\\");
        }
        strcat(current, part);
        attributes = GetFileAttributesA(current);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) ||
                (!CreateDirectoryA(current, NULL) && GetLastError() != ERROR_ALREADY_EXISTS))
                return BP_ERR_IO;
            attributes = GetFileAttributesA(current);
        }
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return BP_ERR_INVALID_ARG;
        if (next == NULL) break;
        *next = '\\';
        part = next + 1;
    }
    return BP_OK;
}

static int win_gui_open_staged_file(const char *target, char *temporary,
                                    size_t temporary_size, FILE **file)
{
    char directory[MAX_PATH];
    char *slash, *scan;
    HANDLE handle;
    int fd, attempt;
    FILE *fp;
    size_t length = strlen(target);
    if (length >= sizeof(directory) || temporary_size < MAX_PATH)
        return BP_ERR_NAME_TOO_LONG;
    strcpy(directory, target);
    slash = NULL;
    for (scan = directory; *scan; scan = CharNextA(scan))
        if (*scan == '\\' || *scan == '/') slash = scan;
    if (slash == NULL) return BP_ERR_INVALID_ARG;
    slash[1] = '\0';
    for (attempt = 0; attempt < 256; attempt++) {
        LONG serial = InterlockedIncrement(&g_gui_temp_counter);
        int n = _snprintf(temporary, temporary_size,
                          "%s.boltpack-%08lx.tmp", directory,
                          (unsigned long)(GetTickCount() ^ GetCurrentProcessId() ^ serial));
        if (n < 0 || (size_t)n >= temporary_size) return BP_ERR_NAME_TOO_LONG;
        handle = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT,
                             NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            return BP_ERR_IO;
        }
        fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_WRONLY);
        if (fd < 0) { CloseHandle(handle); DeleteFileA(temporary); return BP_ERR_IO; }
        fp = _fdopen(fd, "wb");
        if (fp == NULL) { _close(fd); DeleteFileA(temporary); return BP_ERR_IO; }
        *file = fp;
        return BP_OK;
    }
    return BP_ERR_IO;
}

typedef struct {
    int exists;
    BY_HANDLE_FILE_INFORMATION info;
} ArchiveTargetSnapshot;

static int win_gui_open_archive_stage(const char *target, char *temporary,
                                      size_t temporary_size, FILE **file)
{
    char directory[MAX_PATH];
    char *slash, *scan;
    int attempt;
    size_t length = strlen(target);
    if (length >= sizeof(directory)) return BP_ERR_NAME_TOO_LONG;
    strcpy(directory, target);
    slash = NULL;
    for (scan = directory; *scan; scan = CharNextA(scan))
        if (*scan == '\\' || *scan == '/') slash = scan;
    if (slash == NULL)
        strcpy(directory, ".\\");
    else
        slash[1] = '\0';

    for (attempt = 0; attempt < 256; attempt++) {
        LONG serial = InterlockedIncrement(&g_gui_temp_counter);
        int n = _snprintf(temporary, temporary_size,
                          "%s.boltpack-create-%08lx.tmp", directory,
                          (unsigned long)(GetTickCount() ^ GetCurrentProcessId() ^ serial));
        HANDLE handle;
        int fd;
        FILE *fp;
        if (n < 0 || (size_t)n >= temporary_size) return BP_ERR_NAME_TOO_LONG;
        handle = CreateFileA(temporary, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            return BP_ERR_IO;
        }
        fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR);
        if (fd < 0) {
            CloseHandle(handle);
            DeleteFileA(temporary);
            return BP_ERR_IO;
        }
        fp = _fdopen(fd, "w+b");
        if (fp == NULL) {
            _close(fd);
            DeleteFileA(temporary);
            return BP_ERR_IO;
        }
        *file = fp;
        return BP_OK;
    }
    return BP_ERR_IO;
}

static int win_gui_archive_target_snapshot(const char *path,
                                           ArchiveTargetSnapshot *snapshot)
{
    DWORD attributes = GetFileAttributesA(path);
    HANDLE handle;
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            snapshot->exists = 0;
            memset(&snapshot->info, 0, sizeof(snapshot->info));
            return 1;
        }
        return 0;
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        return 0;
    handle = CreateFileA(path, FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileInformationByHandle(handle, &snapshot->info)) {
        CloseHandle(handle);
        return 0;
    }
    CloseHandle(handle);
    if (snapshot->info.dwFileAttributes &
        (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    snapshot->exists = 1;
    return 1;
}

static int win_gui_archive_target_unchanged(const ArchiveTargetSnapshot *expected,
                                            const ArchiveTargetSnapshot *current)
{
    if (expected->exists != current->exists) return 0;
    if (!expected->exists) return 1;
    return expected->info.dwVolumeSerialNumber == current->info.dwVolumeSerialNumber &&
           expected->info.nFileIndexHigh == current->info.nFileIndexHigh &&
           expected->info.nFileIndexLow == current->info.nFileIndexLow &&
           expected->info.nFileSizeHigh == current->info.nFileSizeHigh &&
           expected->info.nFileSizeLow == current->info.nFileSizeLow &&
           expected->info.ftLastWriteTime.dwHighDateTime ==
               current->info.ftLastWriteTime.dwHighDateTime &&
           expected->info.ftLastWriteTime.dwLowDateTime ==
               current->info.ftLastWriteTime.dwLowDateTime;
}

/* Returns 0 when published, 1 when published with an old-file backup left,
 * -1 when the target remains intact, or -2 when recovery files must be kept. */
static int win_gui_publish_archive_stage(const char *temporary, const char *target,
                                         const ArchiveTargetSnapshot *expected,
                                         char *backup, size_t backup_size)
{
    ArchiveTargetSnapshot current;
    int n;
    if (!win_gui_archive_target_snapshot(target, &current) ||
        !win_gui_archive_target_unchanged(expected, &current)) {
        SetLastError(ERROR_FILE_INVALID);
        return -1;
    }
    if (!expected->exists)
        return MoveFileA(temporary, target) ? 0 : -1;

    n = _snprintf(backup, backup_size, "%s.previous", temporary);
    if (n < 0 || (size_t)n >= backup_size) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return -1;
    }
    if (GetFileAttributesA(backup) != INVALID_FILE_ATTRIBUTES) {
        SetLastError(ERROR_FILE_EXISTS);
        return -1;
    }

    {
        typedef BOOL (WINAPI *replace_file_fn)(LPCSTR, LPCSTR, LPCSTR,
                                               DWORD, LPVOID, LPVOID);
        FARPROC proc;
        HMODULE kernel = GetModuleHandleA("kernel32.dll");
        replace_file_fn replace_file = NULL;
        proc = kernel == NULL ? NULL : GetProcAddress(kernel, "ReplaceFileA");
        if (proc != NULL)
            memcpy(&replace_file, &proc, sizeof(replace_file));
        if (replace_file != NULL) {
            if (replace_file(target, temporary, backup, 0, NULL, NULL))
                return DeleteFileA(backup) ? 0 : 1;
            if (GetFileAttributesA(backup) != INVALID_FILE_ATTRIBUTES)
                return -2;
            return -1;
        }
    }

    /* Older Windows releases lack ReplaceFileA. Keep a recovery rename if the
     * second move fails, and never discard the previous target in that case. */
    if (!MoveFileA(target, backup)) return -1;
    if (!MoveFileA(temporary, target)) {
        if (!MoveFileA(backup, target)) return -2;
        return -1;
    }
    return DeleteFileA(backup) ? 0 : 1;
}

static int win_gui_apply_fat(const char *path, bp_uint8 valid, bp_uint8 value)
{
    DWORD old_attributes = GetFileAttributesA(path);
    DWORD desired, actual;
    if (old_attributes == INVALID_FILE_ATTRIBUTES) return BP_ERR_PARTIAL;
    desired = (old_attributes & ~(DWORD)valid) | (DWORD)value;
    if (!SetFileAttributesA(path, desired)) return BP_ERR_PARTIAL;
    actual = GetFileAttributesA(path);
    return actual != INVALID_FILE_ATTRIBUTES && (actual & valid) == value
         ? BP_OK : BP_ERR_PARTIAL;
}

static int win_gui_queue_directory(ExtractCtx *ctx, const char *path,
                                   bp_uint64 mtime, int mtime_valid)
{
    WinGuiDirectoryRestore *node, **at;
    size_t length = strlen(path);
    if (length >= MAX_PATH) return BP_ERR_NAME_TOO_LONG;
    for (node = ctx->directories; node != NULL; node = node->next) {
        if (strcmp(node->path, path) == 0) {
            if (mtime_valid) {
                node->mtime = mtime;
                node->mtime_valid = 1;
            }
            return BP_OK;
        }
    }
    if (ctx->directory_count >= 65535U) return BP_ERR_OVERFLOW;
    node = (WinGuiDirectoryRestore *)calloc(1, sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    strcpy(node->path, path);
    node->mtime = mtime;
    node->mtime_valid = mtime_valid;
    at = &ctx->directories;
    while (*at != NULL && strlen((*at)->path) >= length) at = &(*at)->next;
    node->next = *at;
    *at = node;
    ctx->directory_count++;
    return BP_OK;
}

static int win_gui_queue_fat(ExtractCtx *ctx, const char *path,
                             bp_uint8 valid, bp_uint8 value)
{
    WinGuiDirectoryRestore *node;
    int rc = win_gui_queue_directory(ctx, path, bp_u64_from_u32(0), 0);
    if (rc != BP_OK) return rc;
    for (node = ctx->directories; node != NULL; node = node->next) {
        if (strcmp(node->path, path) == 0) {
            node->fat_valid = valid;
            node->fat_value = value;
            return BP_OK;
        }
    }
    return BP_ERR_IO;
}

static int win_gui_finish_directories(ExtractCtx *ctx, int apply)
{
    WinGuiDirectoryRestore *node = ctx->directories;
    int result = BP_OK;
    ctx->directories = NULL;
    while (node != NULL) {
        WinGuiDirectoryRestore *next = node->next;
        if (apply) {
            if (node->mtime_valid) {
                bp_off_t seconds = bp_u64_to_off(node->mtime);
                time_t t = (time_t)seconds;
                struct _stat64 st;
                struct utimbuf ut;
                if (seconds < 0 ||
                    bp_u64_cmp(bp_off_to_u64(seconds), node->mtime) != 0 ||
                    (bp_off_t)t != seconds || _stat64(node->path, &st) != 0) {
                    result = BP_ERR_PARTIAL;
                } else {
                    ut.actime = (time_t)st.st_atime;
                    ut.modtime = t;
                    if (utime(node->path, &ut) != 0) result = BP_ERR_PARTIAL;
                }
            }
            if (node->fat_valid != 0 &&
                win_gui_apply_fat(node->path, node->fat_valid,
                                  node->fat_value) != BP_OK)
                result = BP_ERR_PARTIAL;
        }
        free(node);
        node = next;
    }
    ctx->directory_count = 0;
    return result;
}

static long win_gui_metadata_write(void *handle, const void *data, bp_uint32 size)
{
    WinGuiMetadataSink *sink = (WinGuiMetadataSink *)handle;
    if (size > BP_META_MAX_SIZE - sink->length) return -1;
    memcpy(sink->data + sink->length, data, size);
    sink->length += size;
    return (long)size;
}

static void win_gui_free_names(ExtractCtx *ctx)
{
    WinGuiNameNode *node = ctx->names;
    while (node != NULL) {
        WinGuiNameNode *next = node->next;
        free(node);
        node = next;
    }
    ctx->names = NULL;
}

static bp_io_t *extract_open_cb(const bp_entry_t *entry, void *user_data)
{
    ExtractCtx *ctx = (ExtractCtx *)user_data;
    char local_name[MAX_PATH];
    char full_path[MAX_PATH];
    FILE *fp;
    int rc;

    if (!entry->name || !bp_name_is_safe(entry->name)) {
        ctx->partial_count++;
        return NULL;
    }
    if (entry->entry_type == BP_TYPE_META) {
        if (entry->platform != BP_META_PORTABLE || !ctx->last_subject_valid ||
            !ctx->last_subject_ok ||
            strcmp(entry->name, ctx->last_subject_name) != 0) {
            ctx->partial_count++;
            return NULL;
        }
        ctx->metadata.length = 0;
        ctx->metadata.io.read = NULL;
        ctx->metadata.io.write = win_gui_metadata_write;
        ctx->metadata.io.seek = NULL;
        ctx->metadata.io.tell = NULL;
        ctx->metadata.io.handle = &ctx->metadata;
        return &ctx->metadata.io;
    }
    if (entry->entry_type == BP_TYPE_RSRC) {
        ctx->partial_count++;
        return NULL;
    }
    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR &&
        entry->entry_type != BP_TYPE_SYMLINK) {
        ctx->partial_count++;
        return NULL;
    }

    ctx->last_subject_valid = 0;
    ctx->last_subject_ok = 0;
    rc = win_gui_project_name(entry->name, local_name, sizeof(local_name));
    if (rc != BP_OK) {
        ctx->partial_count++;
        return NULL;
    }
    rc = win_gui_register_name(ctx, local_name);
    if (rc != BP_OK) {
        ctx->partial_count++;
        return NULL;
    }
    rc = win_gui_make_full_path(ctx, local_name, full_path, sizeof(full_path));
    if (rc != BP_OK) { ctx->partial_count++; return NULL; }
    if (strlen(entry->name) >= sizeof(ctx->last_subject_name) ||
        strlen(full_path) >= sizeof(ctx->last_subject_path)) {
        ctx->partial_count++;
        return NULL;
    }
    strcpy(ctx->last_subject_name, entry->name);
    strcpy(ctx->last_subject_path, full_path);
    ctx->last_subject_type = entry->entry_type;
    ctx->last_subject_valid = 1;

    if (entry->entry_type == BP_TYPE_DIR) {
        rc = win_gui_ensure_directories(ctx, local_name, 1);
        if (rc != BP_OK) { ctx->open_error = rc; return NULL; }
        rc = win_gui_queue_directory(ctx, full_path, entry->mtime,
                    (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) != 0);
        if (rc != BP_OK) { ctx->open_error = rc; return NULL; }
        if (entry->flags & BP_ENTRY_FLAG_MODE_VALID) ctx->partial_count++;
        ctx->last_subject_ok = 1;
        return NULL;
    }
    if (entry->entry_type == BP_TYPE_SYMLINK) {
        ctx->partial_count++;
        return NULL;
    }

    rc = win_gui_ensure_directories(ctx, local_name, 0);
    if (rc != BP_OK) { ctx->open_error = rc; return NULL; }
    {
        WinGuiStagedFile *staged = (WinGuiStagedFile *)calloc(1, sizeof(*staged));
        if (staged == NULL) { ctx->open_error = BP_ERR_NOMEM; return NULL; }
        rc = win_gui_open_staged_file(full_path, staged->temporary,
                                      sizeof(staged->temporary), &fp);
        if (rc != BP_OK) { free(staged); ctx->open_error = rc; return NULL; }
        strcpy(staged->target, full_path);
        bp_io_from_file(&staged->io, fp);
        return &staged->io;
    }
}

static int extract_done_cb(const bp_entry_t *entry, bp_io_t *out, int status,
                           void *user_data)
{
    ExtractCtx *ctx = (ExtractCtx *)user_data;
    if (entry->entry_type == BP_TYPE_META) {
        bp_portable_metadata_t parsed;
        int parse_rc;
        if (status != BP_OK) return status;
        parse_rc = bp_metadata_parse(ctx->metadata.data, ctx->metadata.length, &parsed);
        if (parse_rc == BP_ERR_CORRUPT) return parse_rc;
        if (parsed.supported_version &&
            parsed.subject_type != ctx->last_subject_type) return BP_ERR_CORRUPT;
        if (parsed.has_fat) {
            int rc;
            if (ctx->last_subject_type == BP_TYPE_DIR)
                rc = win_gui_queue_fat(ctx, ctx->last_subject_path,
                                       parsed.fat_valid, parsed.fat_value);
            else
                rc = win_gui_apply_fat(ctx->last_subject_path,
                                       parsed.fat_valid, parsed.fat_value);
            if (rc != BP_OK) ctx->partial_count++;
        }
        if (parse_rc == BP_ERR_PARTIAL || parsed.has_unknown) ctx->partial_count++;
        (void)out;
        return BP_OK;
    }
    if (entry->entry_type == BP_TYPE_DIR) return BP_OK;
    {
        WinGuiStagedFile *staged = (WinGuiStagedFile *)out;
        FILE *fp = (FILE *)out->handle;
        int result = status;
        if (status != BP_OK) {
            fclose(fp);
            DeleteFileA(staged->temporary);
            free(staged);
            ctx->last_subject_ok = 0;
            return status;
        }
        if (fclose(fp) != 0) result = BP_ERR_IO;
        if (result == BP_OK && (entry->flags & BP_ENTRY_FLAG_MTIME_VALID)) {
            bp_off_t seconds = bp_u64_to_off(entry->mtime);
            time_t t = (time_t)seconds;
            struct _stat64 st;
            struct utimbuf ut;
            if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), entry->mtime) != 0 ||
                (bp_off_t)t != seconds || _stat64(staged->temporary, &st) != 0) {
                result = BP_ERR_PARTIAL;
            } else {
                ut.actime = (time_t)st.st_atime;
                ut.modtime = t;
                if (utime(staged->temporary, &ut) != 0) result = BP_ERR_PARTIAL;
            }
        }
        if (entry->flags & BP_ENTRY_FLAG_MODE_VALID) result = BP_ERR_PARTIAL;
        if (result != BP_ERR_IO && result != BP_ERR_CORRUPT &&
            result != BP_ERR_CHECKSUM &&
            !MoveFileExA(staged->temporary, staged->target,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileA(staged->temporary);
            free(staged);
            ctx->last_subject_ok = 0;
            return BP_ERR_IO;
        }
        if (result == BP_OK || result == BP_ERR_PARTIAL) {
            ctx->last_subject_ok = 1;
            free(staged);
            return result;
        }
        DeleteFileA(staged->temporary);
        free(staged);
        ctx->last_subject_ok = 0;
        return result;
    }
}

static int CALLBACK BrowseSetDirCallback(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData)
{
    (void)lParam;
    if (uMsg == BFFM_INITIALIZED)
        SendMessageA(hwnd, BFFM_SETSELECTION, TRUE, lpData);
    return 0;
}

static void DoExtract(void)
{
    BROWSEINFOA bi;
    LPITEMIDLIST pidl;
    char dest[MAX_PATH];
    char archive_dir[MAX_PATH];
    FILE *fp;
    bp_io_t io;
    ExtractCtx ectx;
    int rc, dir_rc;
    DWORD dest_attributes;

    if (!g_archive_open) return;

    /* Derive the archive's parent directory for the initial browse location */
    strncpy(archive_dir, g_archive_path, MAX_PATH);
    archive_dir[MAX_PATH - 1] = '\0';
    {
        char *sep = strrchr(archive_dir, '\\');
        if (!sep) sep = strrchr(archive_dir, '/');
        if (sep) *sep = '\0';
    }

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_hWnd;
    bi.lpszTitle = "Select destination folder";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn      = BrowseSetDirCallback;
    bi.lParam    = (LPARAM)archive_dir;

    pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return;
    if (!SHGetPathFromIDListA(pidl, dest)) {
        CoTaskMemFree(pidl);
        return;
    }
    CoTaskMemFree(pidl);

    dest_attributes = GetFileAttributesA(dest);
    if (dest_attributes == INVALID_FILE_ATTRIBUTES ||
        !(dest_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxA(g_hWnd, "Destination is not a readable directory.",
                    "Error", MB_OK | MB_ICONERROR);
        return;
    }

    fp = fopen(g_archive_path, "rb");
    if (!fp) {
        MessageBoxA(g_hWnd, "Cannot open archive.", "Error", MB_OK | MB_ICONERROR);
        return;
    }
    bp_io_from_file(&io, fp);

    memset(&ectx, 0, sizeof(ectx));
    strncpy(ectx.dest_dir, dest, MAX_PATH);
    ectx.dest_dir[MAX_PATH - 1] = '\0';

    /* show progress dialog (modeless) */
    g_progress.cancelled  = 0;
    g_progress.bar_shown  = 0;
    g_progress.start_tick = GetTickCount();
    g_progress.hDlg = CreateDialogA(g_hInst, MAKEINTRESOURCEA(IDD_PROGRESS),
                                    g_hWnd, ProgressDlgProc);
    ShowWindow(GetDlgItem(g_progress.hDlg, IDC_PROG_BAR), SW_HIDE);
    ShowWindow(g_progress.hDlg, SW_SHOW);
    SetWindowTextA(g_progress.hDlg, "Extracting...");
    ectx.progress = &g_progress;

    rc = bp_archive_extract_all_ex(&io, extract_open_cb, extract_done_cb,
                                   progress_cb, &ectx);
    if ((rc == BP_OK || rc == BP_ERR_PARTIAL) && ectx.open_error != BP_OK)
        rc = ectx.open_error;
    dir_rc = win_gui_finish_directories(
        &ectx, (rc == BP_OK || rc == BP_ERR_PARTIAL) && ectx.open_error == BP_OK);
    if ((rc == BP_OK || rc == BP_ERR_PARTIAL) && dir_rc != BP_OK) rc = dir_rc;
    if (rc == BP_OK && ectx.partial_count != 0) rc = BP_ERR_PARTIAL;
    win_gui_free_names(&ectx);

    DestroyWindow(g_progress.hDlg);
    fclose(fp);

    if (rc == BP_ERR_ABORTED) {
        MessageBoxA(g_hWnd, "Extraction cancelled.", "BoltPack", MB_OK | MB_ICONINFORMATION);
    } else if (rc != BP_OK) {
        MessageBoxA(g_hWnd, bp_strerror(rc), "Error", MB_OK | MB_ICONERROR);
    } else {
        char msg[128];
        _snprintf(msg, sizeof(msg), "Extraction complete.\n%lu files extracted.",
                  (unsigned long)g_info.file_count);
        msg[sizeof(msg) - 1] = '\0';
        MessageBoxA(g_hWnd, msg, "BoltPack", MB_OK | MB_ICONINFORMATION);
    }
}

/* ---- create archive ---------------------------------------------------- */

static int add_file(bp_archive_t *ar, const char *filepath, const char *arc_name);
static int add_dir_recursive(bp_archive_t *ar, const char *dirpath, const char *prefix);

static int win_gui_acp_name_to_utf8(const char *local, char *utf8, size_t capacity)
{
    int local_length, wide_length, roundtrip_length, utf8_length;
    UINT codepage = GetACP();
    DWORD flags = codepage == CP_UTF8 ? 0 : WC_NO_BEST_FIT_CHARS;
    BOOL used_default = FALSE;
    BOOL *used_default_ptr = codepage == CP_UTF8 ? NULL : &used_default;
    WCHAR *wide = NULL;
    char *roundtrip = NULL;
    int rc = BP_ERR_PARTIAL;

    local_length = (int)strlen(local) + 1;
    wide_length = MultiByteToWideChar(CP_ACP, 0, local, local_length, NULL, 0);
    if (wide_length <= 0) return BP_ERR_PARTIAL;
    wide = (WCHAR *)malloc((size_t)wide_length * sizeof(WCHAR));
    if (wide == NULL) return BP_ERR_NOMEM;
    if (MultiByteToWideChar(CP_ACP, 0, local, local_length,
                            wide, wide_length) <= 0) goto done;

    roundtrip_length = WideCharToMultiByte(codepage, flags,
                                           wide, wide_length, NULL, 0, NULL,
                                           used_default_ptr);
    if (roundtrip_length != local_length || used_default) goto done;
    roundtrip = (char *)malloc((size_t)roundtrip_length);
    if (roundtrip == NULL) { rc = BP_ERR_NOMEM; goto done; }
    used_default = FALSE;
    if (WideCharToMultiByte(codepage, flags, wide, wide_length,
                            roundtrip, roundtrip_length, NULL,
                            used_default_ptr) != roundtrip_length || used_default ||
        memcmp(roundtrip, local, (size_t)local_length) != 0) goto done;

    utf8_length = WideCharToMultiByte(CP_UTF8, 0,
                                      wide, wide_length, NULL, 0, NULL, NULL);
    if (utf8_length <= 0 || (size_t)utf8_length > capacity) {
        rc = BP_ERR_NAME_TOO_LONG;
        goto done;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, wide_length,
                            utf8, (int)capacity, NULL, NULL) != utf8_length)
        goto done;
    rc = BP_OK;

done:
    free(wide);
    free(roundtrip);
    return rc;
}

#define CREATE_NAME_HASH_CAP 131071U
#define CREATE_NAME_MAX_NODES 65535U
#define CREATE_NAME_MAX_BYTES (16U * 1024U * 1024U)

typedef struct {
    WCHAR *key;
    unsigned long hash;
    int kind;       /* 1 = directory, 2 = file */
    int primary;
} CreateNameSlot;

typedef struct {
    CreateNameSlot *slots;
    unsigned int node_count;
    unsigned int primary_count;
    size_t key_bytes;
} CreateNameIndex;

static int win_gui_create_names_init(CreateNameIndex *index)
{
    memset(index, 0, sizeof(*index));
    index->slots = (CreateNameSlot *)calloc(CREATE_NAME_HASH_CAP,
                                            sizeof(CreateNameSlot));
    return index->slots == NULL ? BP_ERR_NOMEM : BP_OK;
}

static void win_gui_create_names_free(CreateNameIndex *index)
{
    unsigned int i;
    if (index->slots != NULL) {
        for (i = 0; i < CREATE_NAME_HASH_CAP; i++)
            free(index->slots[i].key);
        free(index->slots);
    }
    memset(index, 0, sizeof(*index));
}

static unsigned long win_gui_create_name_hash(const WCHAR *key)
{
    unsigned long hash = 2166136261UL;
    while (*key) {
        hash ^= (unsigned long)(unsigned short)*key++;
        hash *= 16777619UL;
    }
    return hash;
}

static int win_gui_create_name_node(CreateNameIndex *index,
                                    const WCHAR *key, size_t length,
                                    int kind, int primary)
{
    unsigned long hash = win_gui_create_name_hash(key);
    unsigned int slot = (unsigned int)(hash % CREATE_NAME_HASH_CAP);
    size_t bytes = (length + 1) * sizeof(WCHAR);

    for (;;) {
        CreateNameSlot *node = &index->slots[slot];
        if (node->key == NULL) {
            if (index->node_count >= CREATE_NAME_MAX_NODES ||
                index->key_bytes > CREATE_NAME_MAX_BYTES - bytes)
                return BP_ERR_OVERFLOW;
            node->key = (WCHAR *)malloc(bytes);
            if (node->key == NULL) return BP_ERR_NOMEM;
            memcpy(node->key, key, length * sizeof(WCHAR));
            node->key[length] = L'\0';
            node->hash = hash;
            node->kind = kind;
            node->primary = primary;
            index->node_count++;
            index->key_bytes += bytes;
            return BP_OK;
        }
        if (node->hash == hash && wcscmp(node->key, key) == 0) {
            if (node->kind != kind || (primary && node->primary))
                return BP_ERR_PARTIAL;
            if (primary) node->primary = 1;
            return BP_OK;
        }
        slot++;
        if (slot == CREATE_NAME_HASH_CAP) slot = 0;
    }
}

static int win_gui_create_name_add(CreateNameIndex *index,
                                   const char *local_name, int is_directory)
{
    WCHAR wide[MAX_PATH];
    WCHAR prefix[MAX_PATH];
    int wide_length;
    size_t length, start, end, prefix_length;
    int rc;

    wide_length = MultiByteToWideChar(CP_ACP, 0, local_name, -1,
                                     wide, MAX_PATH);
    if (wide_length <= 1 || wide_length >= MAX_PATH) return BP_ERR_NAME_TOO_LONG;
    length = (size_t)wide_length - 1;
    while (length > 0 && (wide[length - 1] == L'/' || wide[length - 1] == L'\\'))
        length--;
    if (length == 0) return BP_ERR_INVALID_ARG;
    for (start = 0; start < length; start++)
        if (wide[start] == L'/') wide[start] = L'\\';
    CharLowerBuffW(wide, (DWORD)length);

    index->primary_count++;
    if (index->primary_count > CREATE_NAME_MAX_NODES)
        return BP_ERR_OVERFLOW;

    start = 0;
    for (;;) {
        int final;
        end = start;
        while (end < length && wide[end] != L'\\') end++;
        if (end == start) return BP_ERR_INVALID_ARG;
        prefix_length = end;
        memcpy(prefix, wide, prefix_length * sizeof(WCHAR));
        prefix[prefix_length] = L'\0';
        final = end == length;
        rc = win_gui_create_name_node(index, prefix, prefix_length,
                                      final ? (is_directory ? 1 : 2) : 1,
                                      final ? 1 : 0);
        if (rc != BP_OK) return rc;
        if (final) return BP_OK;
        start = end + 1;
    }
}

static int win_gui_preflight_create_path(CreateNameIndex *index,
                                         const char *filepath,
                                         const char *archive_name)
{
    DWORD attributes = GetFileAttributesA(filepath);
    int is_directory;
    int rc;
    if (attributes == INVALID_FILE_ATTRIBUTES) return BP_ERR_IO;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        g_create_reparse_refused = 1;
        return BP_ERR_INVALID_ARG;
    }
    is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (is_directory) {
        char directory_name[1024];
        char search[MAX_PATH + 4];
        WIN32_FIND_DATAA entry;
        HANDLE find;
        DWORD find_error;
        int n = _snprintf(directory_name, sizeof(directory_name), "%s%s",
                          archive_name,
                          archive_name[0] != '\0' &&
                          archive_name[strlen(archive_name) - 1] == '/' ? "" : "/");
        if (n < 0 || (size_t)n >= sizeof(directory_name))
            return BP_ERR_NAME_TOO_LONG;
        rc = win_gui_create_name_add(index, directory_name, 1);
        if (rc != BP_OK) return rc;
        n = _snprintf(search, sizeof(search), "%s\\*", filepath);
        if (n < 0 || (size_t)n >= sizeof(search)) return BP_ERR_NAME_TOO_LONG;
        find = FindFirstFileA(search, &entry);
        if (find == INVALID_HANDLE_VALUE) return BP_ERR_IO;
        do {
            char child_path[MAX_PATH + 260];
            char child_name[1024];
            if (strcmp(entry.cFileName, ".") == 0 ||
                strcmp(entry.cFileName, "..") == 0) continue;
            n = _snprintf(child_path, sizeof(child_path), "%s\\%s",
                          filepath, entry.cFileName);
            if (n < 0 || (size_t)n >= sizeof(child_path)) {
                FindClose(find);
                return BP_ERR_NAME_TOO_LONG;
            }
            n = _snprintf(child_name, sizeof(child_name), "%s%s",
                          directory_name, entry.cFileName);
            if (n < 0 || (size_t)n >= sizeof(child_name)) {
                FindClose(find);
                return BP_ERR_NAME_TOO_LONG;
            }
            rc = win_gui_preflight_create_path(index, child_path, child_name);
            if (rc != BP_OK) {
                FindClose(find);
                return rc;
            }
        } while (FindNextFileA(find, &entry));
        find_error = GetLastError();
        FindClose(find);
        return find_error == ERROR_NO_MORE_FILES ? BP_OK : BP_ERR_IO;
    }
    return win_gui_create_name_add(index, archive_name, 0);
}

static const char *win_gui_preflight_error(int rc)
{
    if (g_create_reparse_refused)
        return "A source reparse point was refused instead of following its target.";
    if (rc == BP_ERR_PARTIAL)
        return "Selected source names collide under Windows case-insensitive naming rules.";
    if (rc == BP_ERR_OVERFLOW)
        return "Too many archive names or path components were selected.";
    return bp_strerror(rc);
}

static int add_file(bp_archive_t *ar, const char *filepath, const char *arc_name)
{
    DWORD attrs;
    struct _stat64 st;
    FILE *fp;
    bp_io_t fio;
    bp_uint64 fsize, mtime;
    char archive_name[4096];
    int rc;

    attrs = GetFileAttributesA(filepath);
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return BP_ERR_IO;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        g_create_reparse_refused = 1;
        return BP_ERR_INVALID_ARG;
    }

    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        char dname[1024];
        int n = _snprintf(dname, sizeof(dname), "%s/", arc_name);
        if (n < 0 || (size_t)n >= sizeof(dname)) return BP_ERR_NAME_TOO_LONG;
        rc = win_gui_acp_name_to_utf8(dname, archive_name, sizeof(archive_name));
        if (rc != BP_OK) return rc;

        if (_stat64(filepath, &st) != 0)
            return BP_ERR_IO;
        mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
        rc = bp_archive_add_entry(ar, archive_name, BP_TYPE_DIR, BP_OS_WINDOWS,
                                  NULL, bp_u64_from_u32(0), mtime);
        if (rc != BP_OK) return rc;
        return add_dir_recursive(ar, filepath, dname);
    }

    if (_stat64(filepath, &st) != 0)
        return BP_ERR_IO;

    fp = fopen(filepath, "rb");
    if (!fp) return BP_ERR_IO;

    bp_io_from_file(&fio, fp);
    fsize = bp_off_to_u64((bp_off_t)st.st_size);
    mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
    rc = win_gui_acp_name_to_utf8(arc_name, archive_name, sizeof(archive_name));
    if (rc != BP_OK) { fclose(fp); return rc; }
    rc = bp_archive_add_entry(ar, archive_name, BP_TYPE_DATA, BP_OS_WINDOWS,
                              &fio, fsize, mtime);
    fclose(fp);
    return rc;
}

static int add_dir_recursive(bp_archive_t *ar, const char *dirpath, const char *prefix)
{
    char search[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE hFind;

    _snprintf(search, sizeof(search), "%s\\*", dirpath);
    search[sizeof(search) - 1] = '\0';

    hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return BP_ERR_IO;

    do {
        char full[MAX_PATH + 260];
        char arc[1024];
        int rc;

        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;

        _snprintf(full, sizeof(full), "%s\\%s", dirpath, fd.cFileName);
        full[sizeof(full) - 1] = '\0';
        _snprintf(arc, sizeof(arc), "%s%s", prefix, fd.cFileName);
        arc[sizeof(arc) - 1] = '\0';

        rc = add_file(ar, full, arc);
        if (rc != BP_OK) { FindClose(hFind); return rc; }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return BP_OK;
}

static void DoCreate(void)
{
    OPENFILENAMEA ofn;
    char input_buf[32768]; /* OFN_ALLOWMULTISELECT buffer */
    char save_buf[MAX_PATH];
    char stage_buf[MAX_PATH + 64];
    char backup_buf[MAX_PATH + 128];
    char recovery_msg[MAX_PATH * 2 + 192];
    char *p;
    char dir[MAX_PATH];
    FILE *fp;
    bp_io_t io;
    bp_archive_t ar;
    ArchiveTargetSnapshot target_snapshot;
    CreateNameIndex create_names;
    int preflight_rc;
    int publish_state;
    int rc;

    g_create_reparse_refused = 0;

    /* pick input files */
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = g_hWnd;
    ofn.lpstrFilter     = "All Files\0*.*\0";
    ofn.lpstrFile       = input_buf;
    ofn.nMaxFile        = sizeof(input_buf);
    ofn.Flags           = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST;
    ofn.lpstrTitle      = "Select files to archive";
    input_buf[0] = '\0';

    if (!GetOpenFileNameA(&ofn)) return;

    /* pick output path */
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = g_hWnd;
    ofn.lpstrFilter     = "BoltPack Archives (*.bolt)\0*.bolt\0All Files\0*.*\0";
    ofn.lpstrFile       = save_buf;
    ofn.nMaxFile        = MAX_PATH;
    ofn.Flags           = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt     = "bolt";
    ofn.lpstrTitle      = "Save archive as";
    save_buf[0] = '\0';

    if (!GetSaveFileNameA(&ofn)) return;

    if (!win_gui_archive_target_snapshot(save_buf, &target_snapshot)) {
        MessageBoxA(g_hWnd, "The selected destination must be a regular file.",
                    "Error", MB_OK | MB_ICONERROR);
        return;
    }
    preflight_rc = win_gui_create_names_init(&create_names);
    if (preflight_rc != BP_OK) {
        MessageBoxA(g_hWnd, bp_strerror(preflight_rc), "Error", MB_OK | MB_ICONERROR);
        return;
    }
    p = input_buf + strlen(input_buf) + 1;
    preflight_rc = BP_OK;
    if (*p == '\0') {
        const char *base = strrchr(input_buf, '\\');
        base = base ? base + 1 : input_buf;
        preflight_rc = win_gui_preflight_create_path(&create_names, input_buf, base);
    } else {
        int n = _snprintf(dir, sizeof(dir), "%s", input_buf);
        if (n < 0 || (size_t)n >= sizeof(dir))
            preflight_rc = BP_ERR_NAME_TOO_LONG;
        while (preflight_rc == BP_OK && *p) {
            char full[MAX_PATH + 260];
            n = _snprintf(full, sizeof(full), "%s\\%s", dir, p);
            if (n < 0 || (size_t)n >= sizeof(full))
                preflight_rc = BP_ERR_NAME_TOO_LONG;
            else
                preflight_rc = win_gui_preflight_create_path(&create_names, full, p);
            p += strlen(p) + 1;
        }
    }
    win_gui_create_names_free(&create_names);
    if (preflight_rc != BP_OK) {
        MessageBoxA(g_hWnd, win_gui_preflight_error(preflight_rc),
                    "Cannot create archive", MB_OK | MB_ICONERROR);
        return;
    }
    if (win_gui_open_archive_stage(save_buf, stage_buf, sizeof(stage_buf), &fp) != BP_OK) {
        MessageBoxA(g_hWnd, "Cannot create a staging file beside the destination.",
                    "Error", MB_OK | MB_ICONERROR);
        return;
    }
    bp_io_from_file(&io, fp);

    memset(&ar, 0, sizeof(ar));

    g_progress.cancelled  = 0;
    g_progress.bar_shown  = 0;
    g_progress.start_tick = GetTickCount();
    g_progress.hDlg = CreateDialogA(g_hInst, MAKEINTRESOURCEA(IDD_PROGRESS),
                                    g_hWnd, ProgressDlgProc);
    ShowWindow(GetDlgItem(g_progress.hDlg, IDC_PROG_BAR), SW_HIDE);
    ShowWindow(g_progress.hDlg, SW_SHOW);
    SetWindowTextA(g_progress.hDlg, "Creating archive...");

    if (g_interchange_profile)
        rc = bp_archive_create_profile(&ar, &io, BP_PROFILE_INTERCHANGE_1);
    else
        rc = bp_archive_create(&ar, &io, BP_BLOCK_DEFAULT, BP_METHOD_BOLTLZ);
    if (rc != BP_OK) {
        DestroyWindow(g_progress.hDlg);
        fclose(fp);
        DeleteFileA(stage_buf);
        MessageBoxA(g_hWnd, bp_strerror(rc), "Error", MB_OK | MB_ICONERROR);
        return;
    }
    ar.progress_fn   = progress_cb;
    ar.progress_data = &g_progress;

    /* Parse OFN_ALLOWMULTISELECT result:
     * Single file: "C:\path\file" (one null-terminated string)
     * Multi file:  "C:\dir\0file1\0file2\0\0" */
    p = input_buf + strlen(input_buf) + 1;
    if (*p == '\0') {
        /* single file selected */
        const char *base = strrchr(input_buf, '\\');
        base = base ? base + 1 : input_buf;
        rc = add_file(&ar, input_buf, base);
    } else {
        /* multiple files: input_buf is directory, p iterates filenames */
        strncpy(dir, input_buf, MAX_PATH);
        dir[MAX_PATH - 1] = '\0';
        while (*p && !g_progress.cancelled) {
            char full[MAX_PATH + 260];
            _snprintf(full, sizeof(full), "%s\\%s", dir, p);
            full[sizeof(full) - 1] = '\0';
            rc = add_file(&ar, full, p);
            if (rc != BP_OK) break;
            p += strlen(p) + 1;
        }
    }

    if (rc == BP_OK && !g_progress.cancelled)
        rc = bp_archive_finalize(&ar);

    bp_archive_close(&ar);
    DestroyWindow(g_progress.hDlg);
    if (rc == BP_OK && fflush(fp) != 0) rc = BP_ERR_IO;
    if (rc == BP_OK && _commit(_fileno(fp)) != 0) rc = BP_ERR_IO;
    if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;

    if (g_progress.cancelled || rc == BP_ERR_ABORTED) {
        DeleteFileA(stage_buf);
        MessageBoxA(g_hWnd, "Archive creation cancelled.", "BoltPack",
                    MB_OK | MB_ICONINFORMATION);
    } else if (rc != BP_OK) {
        DeleteFileA(stage_buf);
        MessageBoxA(g_hWnd,
            g_create_reparse_refused
                ? "A source reparse point was refused instead of following its target."
                : bp_strerror(rc),
            "Error", MB_OK | MB_ICONERROR);
    } else {
        publish_state = win_gui_publish_archive_stage(stage_buf, save_buf,
            &target_snapshot, backup_buf, sizeof(backup_buf));
        if (publish_state >= 0) {
            DoOpen(save_buf);
            if (publish_state == 1) {
                _snprintf(recovery_msg, sizeof(recovery_msg),
                          "Archive saved. The previous archive remains at:\n%s",
                          backup_buf);
                recovery_msg[sizeof(recovery_msg) - 1] = '\0';
                MessageBoxA(g_hWnd, recovery_msg, "BoltPack", MB_OK | MB_ICONINFORMATION);
            }
        } else if (publish_state == -2) {
            _snprintf(recovery_msg, sizeof(recovery_msg),
                      "Replacement did not finish cleanly. Keep both recovery files:\n%s\n%s",
                      stage_buf, backup_buf);
            recovery_msg[sizeof(recovery_msg) - 1] = '\0';
            MessageBoxA(g_hWnd, recovery_msg, "Error", MB_OK | MB_ICONERROR);
        } else {
            DeleteFileA(stage_buf);
            MessageBoxA(g_hWnd, "The destination changed or could not be replaced.",
                        "Error", MB_OK | MB_ICONERROR);
        }
    }
}

/* ---- menu state -------------------------------------------------------- */

static void UpdateMenuState(void)
{
    HMENU hMenu = GetMenu(g_hWnd);
    EnableMenuItem(hMenu, IDM_EXTRACT,  g_archive_open ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(hMenu, IDM_CLOSE,    g_archive_open ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(hMenu, IDM_VALIDATE, g_archive_open ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(hMenu, IDM_INFO,     g_archive_open ? MF_ENABLED : MF_GRAYED);
    CheckMenuItem(hMenu, IDM_INTERCHANGE_PROFILE,
                  MF_BYCOMMAND | (g_interchange_profile ? MF_CHECKED : MF_UNCHECKED));
    DrawMenuBar(g_hWnd);
}

/* ---- list view --------------------------------------------------------- */

static void CreateListView(HWND hParent)
{
    RECT rc;
    LVCOLUMNA lvc;
    static const char *col_names[] = {
        "Name", "Original", "Compressed", "Ratio", "Method", "Date"
    };
    static const int col_widths[] = { 240, 90, 90, 60, 70, 130 };
    int i;

    GetClientRect(hParent, &rc);
    g_hListView = CreateWindowExA(0, WC_LISTVIEWA, "",
        WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, rc.right, rc.bottom,
        hParent, NULL, g_hInst, NULL);

    ListView_SetExtendedListViewStyle(g_hListView,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    memset(&lvc, 0, sizeof(lvc));
    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    for (i = 0; i < 6; i++) {
        lvc.iSubItem = i;
        lvc.pszText  = (LPSTR)col_names[i];
        lvc.cx       = col_widths[i];
        ListView_InsertColumn(g_hListView, i, &lvc);
    }
}

static void PopulateListView(void)
{
    LVITEMA lvi;
    int i;

    ListView_DeleteAllItems(g_hListView);

    for (i = 0; i < g_entry_count; i++) {
        EntryInfo *e = &g_entries[i];
        char buf[128];

        memset(&lvi, 0, sizeof(lvi));
        lvi.mask     = LVIF_TEXT;
        lvi.iItem    = i;
        lvi.iSubItem = 0;
        lvi.pszText  = e->name;
        ListView_InsertItem(g_hListView, &lvi);

        /* Original size */
        if (e->entry_type == BP_TYPE_DATA)
            format_size(e->orig_size, buf, sizeof(buf));
        else
            buf[0] = '\0';
        ListView_SetItemText(g_hListView, i, 1, buf);

        /* Compressed size */
        if (e->entry_type == BP_TYPE_DATA)
            format_size(e->comp_size, buf, sizeof(buf));
        else
            buf[0] = '\0';
        ListView_SetItemText(g_hListView, i, 2, buf);

        /* Ratio */
        if (e->entry_type == BP_TYPE_DATA && e->orig_size > 0)
            _snprintf(buf, sizeof(buf), "%d%%", e->ratio);
        else
            buf[0] = '\0';
        buf[sizeof(buf) - 1] = '\0';
        ListView_SetItemText(g_hListView, i, 3, buf);

        /* Method */
        if (e->entry_type == BP_TYPE_DATA)
            strncpy(buf, method_name(e->method), sizeof(buf));
        else
            buf[0] = '\0';
        buf[sizeof(buf) - 1] = '\0';
        ListView_SetItemText(g_hListView, i, 4, buf);

        /* Date */
        ListView_SetItemText(g_hListView, i, 5, e->date);
    }

    ShowWindow(g_hListView, SW_SHOW);
}

/* ---- logo drawing ------------------------------------------------------ */

static void ShowLogo(HWND hWnd, HDC hdc)
{
    RECT rc;
    const char *line1 = BP_APP_NAME " " BP_APP_VERSION;
    const char *line2 = "A lightweight file archiver";
    const char *line3 = "for vintage and modern systems";
    const char *line4 = "Drop files here to open or create archives";
    HFONT hFontBig, hFontSmall, hOld;
    int cy;

    GetClientRect(hWnd, &rc);

    FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));

    hFontBig   = CreateFontA(-24, 0, 0, 0, FW_BOLD,   0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    hFontSmall = CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    SetBkMode(hdc, TRANSPARENT);

    cy = rc.bottom / 2 - 60;

    hOld = (HFONT)SelectObject(hdc, hFontBig);
    {
        RECT tr = { rc.left, cy, rc.right, cy + 30 };
        DrawTextA(hdc, line1, -1, &tr, DT_CENTER | DT_SINGLELINE);
    }
    cy += 40;

    SelectObject(hdc, hFontSmall);
    {
        RECT tr = { rc.left, cy, rc.right, cy + 20 };
        DrawTextA(hdc, line2, -1, &tr, DT_CENTER | DT_SINGLELINE);
    }
    cy += 20;
    {
        RECT tr = { rc.left, cy, rc.right, cy + 20 };
        DrawTextA(hdc, line3, -1, &tr, DT_CENTER | DT_SINGLELINE);
    }
    cy += 40;

    SetTextColor(hdc, RGB(128, 128, 128));
    {
        RECT tr = { rc.left, cy, rc.right, cy + 20 };
        DrawTextA(hdc, line4, -1, &tr, DT_CENTER | DT_SINGLELINE);
    }

    SelectObject(hdc, hOld);
    DeleteObject(hFontBig);
    DeleteObject(hFontSmall);
}

/* ---- drag-and-drop helpers --------------------------------------------- */

static int HasBoltExtension(const char *path)
{
    size_t len = strlen(path);
    if (len >= 5 && _stricmp(path + len - 5, ".bolt") == 0) return 1;
    if (len >= 4 && _stricmp(path + len - 4, ".blt") == 0) return 1;
    return 0;
}

static void ClassifyAndAct(char paths[][MAX_PATH], int count)
{
    char (*archives)[MAX_PATH] = NULL;
    char (*others)[MAX_PATH]   = NULL;
    int narc = 0, nother = 0;
    int i;

    archives = (char (*)[MAX_PATH])malloc(count * MAX_PATH);
    others   = (char (*)[MAX_PATH])malloc(count * MAX_PATH);
    if (!archives || !others) { free(archives); free(others); return; }

    for (i = 0; i < count; i++) {
        if (HasBoltExtension(paths[i]))
            strncpy(archives[narc++], paths[i], MAX_PATH);
        else
            strncpy(others[nother++], paths[i], MAX_PATH);
    }

    if (narc == 1 && nother == 0) {
        DoOpen(archives[0]);
    } else if (narc > 0 && nother == 0) {
        DoOpen(archives[0]);
    } else if (narc == 0 && nother > 0) {
        /* create archive from dropped files */
        OPENFILENAMEA ofn;
        char save_buf[MAX_PATH];
        char stage_buf[MAX_PATH + 64];
        char backup_buf[MAX_PATH + 128];
        char recovery_msg[MAX_PATH * 2 + 192];
        FILE *fp;
        bp_io_t io;
        bp_archive_t ar;
        ArchiveTargetSnapshot target_snapshot;
        CreateNameIndex create_names;
        int preflight_rc;
        int publish_state;
        int rc;

        memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner   = g_hWnd;
        ofn.lpstrFilter = "BoltPack Archives (*.bolt)\0*.bolt\0All Files\0*.*\0";
        ofn.lpstrFile   = save_buf;
        ofn.nMaxFile    = MAX_PATH;
        ofn.Flags       = OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = "bolt";
        ofn.lpstrTitle  = "Save archive as";
        save_buf[0]     = '\0';

        if (!GetSaveFileNameA(&ofn)) goto cleanup;

        if (!win_gui_archive_target_snapshot(save_buf, &target_snapshot)) {
            MessageBoxA(g_hWnd, "The selected destination must be a regular file.",
                        "Error", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
        g_create_reparse_refused = 0;
        preflight_rc = win_gui_create_names_init(&create_names);
        if (preflight_rc != BP_OK) {
            MessageBoxA(g_hWnd, bp_strerror(preflight_rc), "Error", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
        for (i = 0; i < nother; i++) {
            const char *base = strrchr(others[i], '\\');
            if (!base) base = strrchr(others[i], '/');
            base = base ? base + 1 : others[i];
            preflight_rc = win_gui_preflight_create_path(&create_names,
                                                         others[i], base);
            if (preflight_rc != BP_OK) break;
        }
        win_gui_create_names_free(&create_names);
        if (preflight_rc != BP_OK) {
            MessageBoxA(g_hWnd, win_gui_preflight_error(preflight_rc),
                        "Cannot create archive", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
        if (win_gui_open_archive_stage(save_buf, stage_buf, sizeof(stage_buf), &fp) != BP_OK) {
            MessageBoxA(g_hWnd, "Cannot create a staging file beside the destination.",
                        "Error", MB_OK | MB_ICONERROR);
            goto cleanup;
        }
        bp_io_from_file(&io, fp);

        memset(&ar, 0, sizeof(ar));
        g_progress.cancelled  = 0;
        g_progress.bar_shown  = 0;
        g_progress.start_tick = GetTickCount();
        g_progress.hDlg = CreateDialogA(g_hInst, MAKEINTRESOURCEA(IDD_PROGRESS),
                                        g_hWnd, ProgressDlgProc);
        ShowWindow(GetDlgItem(g_progress.hDlg, IDC_PROG_BAR), SW_HIDE);
        ShowWindow(g_progress.hDlg, SW_SHOW);
        SetWindowTextA(g_progress.hDlg, "Creating archive...");
        if (g_interchange_profile)
            rc = bp_archive_create_profile(&ar, &io, BP_PROFILE_INTERCHANGE_1);
        else
            rc = bp_archive_create(&ar, &io, BP_BLOCK_DEFAULT, BP_METHOD_BOLTLZ);
        if (rc == BP_OK) {
            ar.progress_fn   = progress_cb;
            ar.progress_data = &g_progress;
            for (i = 0; i < nother && !g_progress.cancelled; i++) {
                const char *base = strrchr(others[i], '\\');
                if (!base) base = strrchr(others[i], '/');
                base = base ? base + 1 : others[i];
                rc = add_file(&ar, others[i], base);
                if (rc != BP_OK) break;
            }
            if (rc == BP_OK && !g_progress.cancelled)
                rc = bp_archive_finalize(&ar);
        }
        bp_archive_close(&ar);
        DestroyWindow(g_progress.hDlg);
        if (rc == BP_OK && fflush(fp) != 0) rc = BP_ERR_IO;
        if (rc == BP_OK && _commit(_fileno(fp)) != 0) rc = BP_ERR_IO;
        if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;

        if (g_progress.cancelled || rc == BP_ERR_ABORTED) {
            DeleteFileA(stage_buf);
            MessageBoxA(g_hWnd, "Archive creation cancelled.", "BoltPack",
                        MB_OK | MB_ICONINFORMATION);
        } else if (rc != BP_OK) {
            DeleteFileA(stage_buf);
            MessageBoxA(g_hWnd,
                g_create_reparse_refused
                    ? "A source reparse point was refused instead of following its target."
                    : bp_strerror(rc),
                "Error", MB_OK | MB_ICONERROR);
        } else {
            publish_state = win_gui_publish_archive_stage(stage_buf, save_buf,
                &target_snapshot, backup_buf, sizeof(backup_buf));
            if (publish_state >= 0) {
                DoOpen(save_buf);
                if (publish_state == 1) {
                    _snprintf(recovery_msg, sizeof(recovery_msg),
                              "Archive saved. The previous archive remains at:\n%s",
                              backup_buf);
                    recovery_msg[sizeof(recovery_msg) - 1] = '\0';
                    MessageBoxA(g_hWnd, recovery_msg, "BoltPack",
                                MB_OK | MB_ICONINFORMATION);
                }
            } else if (publish_state == -2) {
                _snprintf(recovery_msg, sizeof(recovery_msg),
                          "Replacement did not finish cleanly. Keep both recovery files:\n%s\n%s",
                          stage_buf, backup_buf);
                recovery_msg[sizeof(recovery_msg) - 1] = '\0';
                MessageBoxA(g_hWnd, recovery_msg, "Error", MB_OK | MB_ICONERROR);
            } else {
                DeleteFileA(stage_buf);
                MessageBoxA(g_hWnd,
                    "The destination changed or could not be replaced.",
                    "Error", MB_OK | MB_ICONERROR);
            }
        }
    } else {
        /* mixed */
        int choice = MessageBoxA(g_hWnd,
            "You dropped both archives and other files.\n"
            "Click Yes to open the archive, No to create a new archive.",
            "BoltPack", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDYES && narc > 0)
            DoOpen(archives[0]);
        else if (choice == IDNO && nother > 0)
            ClassifyAndAct(others, nother);
    }

cleanup:
    free(archives);
    free(others);
}

/* ---- dialog procs ------------------------------------------------------ */

static INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK ProgressDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        SendDlgItemMessageA(hDlg, IDC_PROG_BAR, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_PROG_CANCEL) {
            g_progress.cancelled = 1;
            return TRUE;
        }
        break;
    case WM_CLOSE:
        g_progress.cancelled = 1;
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK DocDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextA(hDlg, IDC_DOC_TEXT, bp_help_text);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* GenericTextDlgProc: lParam = pointer to TextDlgData */
static INT_PTR CALLBACK GenericTextDlgProc(HWND hDlg, UINT msg,
                                            WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG: {
        TextDlgData *d = (TextDlgData *)lParam;
        if (d) {
            SetWindowTextA(hDlg, d->title);
            SetDlgItemTextA(hDlg, IDC_DOC_TEXT, d->body);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ---- main window procedure --------------------------------------------- */

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        CreateListView(hWnd);
        ShowWindow(g_hListView, SW_HIDE);
        UpdateMenuState();
        return 0;

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        if (g_hListView)
            MoveWindow(g_hListView, 0, 0, w, h, TRUE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        if (!g_archive_open)
            ShowLogo(hWnd, hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_OPEN: {
            OPENFILENAMEA ofn;
            char buf[MAX_PATH];
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = hWnd;
            ofn.lpstrFilter = "BoltPack Archives (*.bolt;*.blt)\0*.bolt;*.blt\0All Files\0*.*\0";
            ofn.lpstrFile   = buf;
            ofn.nMaxFile    = MAX_PATH;
            ofn.Flags       = OFN_FILEMUSTEXIST;
            ofn.lpstrTitle  = "Open BoltPack Archive";
            buf[0] = '\0';
            if (GetOpenFileNameA(&ofn))
                DoOpen(buf);
            return 0;
        }
        case IDM_EXTRACT: DoExtract(); return 0;
        case IDM_CLOSE:   DoClose();   return 0;
        case IDM_VALIDATE: DoValidate(); return 0;
        case IDM_INFO:     DoInfo();     return 0;
        case IDM_CREATE:  DoCreate();  return 0;
        case IDM_INTERCHANGE_PROFILE:
            g_interchange_profile = !g_interchange_profile;
            UpdateMenuState();
            return 0;
        case IDM_EXIT:    PostQuitMessage(0); return 0;
        case IDM_DOC:
            DialogBoxA(g_hInst, MAKEINTRESOURCEA(IDD_DOC), hWnd, DocDlgProc);
            return 0;
        case IDM_ABOUT:
            DialogBoxA(g_hInst, MAKEINTRESOURCEA(IDD_ABOUT), hWnd, AboutDlgProc);
            return 0;
        }
        break;

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        int count = (int)DragQueryFileA(hDrop, 0xFFFFFFFF, NULL, 0);
        if (count > 0) {
            char (*paths)[MAX_PATH];
            int i;
            paths = (char (*)[MAX_PATH])malloc(count * MAX_PATH);
            if (paths) {
                for (i = 0; i < count; i++)
                    DragQueryFileA(hDrop, i, paths[i], MAX_PATH);
                ClassifyAndAct(paths, count);
                free(paths);
            }
        }
        DragFinish(hDrop);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

/* ---- WinMain ----------------------------------------------------------- */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSEXA wc;
    MSG msg;
    INITCOMMONCONTROLSEX icex;

    (void)hPrevInstance;

    g_hInst = hInstance;

    bp_compress_init();

    /* init common controls (for ListView, ProgressBar) */
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    /* register window class */
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_BOLTPACK));
    wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName  = MAKEINTRESOURCEA(IDR_MAINMENU);
    wc.lpszClassName = APP_CLASS_NAME;
    wc.hIconSm      = LoadIconA(hInstance, MAKEINTRESOURCEA(IDI_BOLTPACK));
    RegisterClassExA(&wc);

    /* create main window with WS_EX_ACCEPTFILES for drag-and-drop */
    g_hWnd = CreateWindowExA(
        WS_EX_ACCEPTFILES,
        APP_CLASS_NAME,
        "BoltPack",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 520,
        NULL, NULL, hInstance, NULL);

    g_hMenu  = GetMenu(g_hWnd);
    g_hAccel = LoadAcceleratorsA(hInstance, MAKEINTRESOURCEA(IDR_ACCEL));

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    /* if launched with a file argument, open it */
    if (lpCmdLine && *lpCmdLine) {
        char path[MAX_PATH];
        /* strip quotes */
        if (lpCmdLine[0] == '"') {
            char *end;
            strncpy(path, lpCmdLine + 1, MAX_PATH);
            path[MAX_PATH - 1] = '\0';
            end = strchr(path, '"');
            if (end) *end = '\0';
        } else {
            strncpy(path, lpCmdLine, MAX_PATH);
            path[MAX_PATH - 1] = '\0';
        }
        DoOpen(path);
    }

    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (!TranslateAcceleratorA(g_hWnd, g_hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    if (g_entries) free(g_entries);
    return (int)msg.wParam;
}
