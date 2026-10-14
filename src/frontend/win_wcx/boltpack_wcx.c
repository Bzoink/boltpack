/*
 * boltpack_wcx.c -- Total Commander WCX packer plugin for BoltPack
 *
 * Implements the WCX packer plugin API. All WCX types and constants are
 * defined inline so wcxhead.h is not required.
 *
 * Compression Method Selection
 * ============================
 * This plugin reads the compression method from the Windows registry:
 *   HKEY_CURRENT_USER\Software\BoltPack\CompressionMethod (DWORD)
 *
 * Valid values:
 *   0 = Stored (no compression)
 *   1 = BoltLite (small-memory LZ77)
 *   2 = boltlz (default if not set)
 *   3 = Fluff (LZ77+FSE/tANS, best ratio on all targets)
 *   4 = zstd (Zstandard, best ratio on modern 64-bit; BOLTPACK_ZSTD builds only)
 *
 * Use boltpack-config.exe to configure the registry setting interactively.
 *
 * Build as a DLL (MinGW cross-compiler from Linux):
 *   i686-w64-mingw32-gcc   -shared -Wall -O2 -I... ... -Wl,--kill-at -o boltpack.wcx
 *   x86_64-w64-mingw32-gcc -shared -Wall -O2 -I... ... -Wl,--kill-at -o boltpack.wcx64
 *
 * Install: copy boltpack.wcx (or .wcx64) to the Total Commander plugins
 * directory and configure via Configuration -> Packer plugins.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Released under the BSD 2-Clause License
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef WCX_EXPORT
# if defined(_WIN32) || defined(_WIN64)
#  define WCX_EXPORT __declspec(dllexport)
# else
#  define WCX_EXPORT
# endif
#endif

#include "../../core/archiver.h"
#include "../../core/compress.h"
#ifdef BOLTPACK_ZSTD
#include "../../core/zstd_wrap.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* ==========================================================================
 * WCX API constants and types (from Total Commander SDK wcxhead.h)
 * ========================================================================== */
 
 #ifndef WCX_MAX_PATH
 #define WCX_MAX_PATH 260
 #endif
 
 /* ==========================================================================
  * Codepage conversion helpers
  * ========================================================================== */
/* Convert a string from the active ANSI codepage (ACP) to UTF-8.
 * Returns 0 on success, -1 on failure. */
static int acp_to_utf8(const char *acp, char *utf8, int utf8_size)
{
    wchar_t wbuf[WCX_MAX_PATH];
    int wlen, ulen;

    wlen = MultiByteToWideChar(CP_ACP, 0, acp, -1, wbuf, WCX_MAX_PATH);
    if (wlen == 0) return -1;

    ulen = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, utf8, utf8_size,
                               NULL, NULL);
    if (ulen == 0) return -1;

    return 0;
}

/* Convert a string from UTF-8 to the active ANSI codepage (ACP).
 * Returns 0 on success, -1 on failure. */
static int utf8_to_acp(const char *utf8, char *acp, int acp_size)
{
    wchar_t wbuf[WCX_MAX_PATH];
    int wlen, alen;

    wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, WCX_MAX_PATH);
    if (wlen == 0) return -1;

    alen = WideCharToMultiByte(CP_ACP, 0, wbuf, wlen, acp, acp_size,
                               NULL, NULL);
    if (alen == 0) return -1;

    return 0;
}

/* ==========================================================================
 * WCX API constants and types
 * (from Total Commander SDK wcxhead.h -- reproduced here for
 *  self-contained builds)
 * ========================================================================== */

/* OpenArchive modes */
#define PK_OM_LIST     0
#define PK_OM_EXTRACT  1

/* ProcessFile operations */
#define PK_SKIP     0
#define PK_TEST     1
#define PK_EXTRACT  2

/* WCX error codes */
#define E_SUCCESS          0
#define E_END_ARCHIVE     10
#define E_NO_MEMORY       11
#define E_BAD_DATA        12
#define E_BAD_ARCHIVE     13
#define E_UNKNOWN_FORMAT  14
#define E_EOPEN           15
#define E_ECREATE         16
#define E_ECLOSE          17
#define E_EREAD           18
#define E_EWRITE          19
#define E_SMALL_BUF       20
#define E_ECOPY           21
#define E_EABORTED        22

/* GetPackerCaps flags */
#define PK_CAPS_NEW       1   /* can create new archives      */
#define PK_CAPS_MODIFY    2   /* can modify (delete from) archives */
#define PK_CAPS_MULTIPLE  4   /* multiple files per archive   */
#define PK_CAPS_DELETE    8   /* can delete files from archives */

/* PackFiles flags */
#define PK_PACK_MOVE_FILES  1
#define PK_PACK_SAVE_PATHS  2

typedef struct {
    char *ArcName;
    int   OpenMode;
    int   OpenResult;
    char *CmtBuf;
    int   CmtBufSize;
    int   CmtSize;
    int   CmtState;
} tOpenArchiveData;

typedef struct {
    char ArcName[260];
    char FileName[260];
    int  Flags;
    int  PackSize;
    int  UnpSize;
    int  HostOS;
    int  FileCRC;
    int  FileTime;   /* DOS date/time packed format */
    int  UnpVer;
    int  Method;
    int  FileAttr;
    char *CmtBuf;
    int   CmtBufSize;
    int   CmtSize;
    int   CmtState;
} tHeaderData;

typedef struct {
    char  ArcName[1024];
    char  FileName[1024];
    int   Flags;
    unsigned int  PackSize;
    unsigned int  PackSizeHigh;
    unsigned int  UnpSize;
    unsigned int  UnpSizeHigh;
    int   HostOS;
    int   FileCRC;
    int   FileTime;
    int   UnpVer;
    int   Method;
    int   FileAttr;
    char *CmtBuf;
    int   CmtBufSize;
    int   CmtSize;
    int   CmtState;
    char  Reserved[1024];
} tHeaderDataEx;

typedef int (WINAPI *tChangeVolProc)  (char *ArcName, int Mode);
typedef int (WINAPI *tProcessDataProc)(char *FileName, int Size);

/* ==========================================================================
 * File I/O adapters (same pattern as win/main.c)
 * ========================================================================== */

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

/* ==========================================================================
 * Utility helpers
 * ========================================================================== */

/* Convert a Unix timestamp to DOS/FAT packed date-time integer. */
static int unix_to_dostime(bp_uint32 mtime)
{
    time_t t = (time_t)mtime;
    struct tm *tm_val;
    int dosdate;
    int dostime;

    tm_val = localtime(&t);
    if (tm_val == NULL) return 0;

    dosdate = ((tm_val->tm_year - 80) << 9)
            | ((tm_val->tm_mon  + 1)  << 5)
            |   tm_val->tm_mday;
    dostime = (tm_val->tm_hour << 11)
            | (tm_val->tm_min  <<  5)
            | (tm_val->tm_sec  >>  1);

    return (dosdate << 16) | dostime;
}

/* Convert forward slashes to backslashes in-place. */
static void to_backslash(char *s)
{
    for (; *s; s++) { if (*s == '/') *s = '\\'; }
}

/* Convert backslashes to forward slashes in-place. */
static void to_fwdslash(char *s)
{
    for (; *s; s++) { if (*s == '\\') *s = '/'; }
}

/* Return 1 if path ends with '/' or '\'. */
static int ends_with_sep(const char *s)
{
    size_t len = strlen(s);
    return len > 0 && (s[len - 1] == '/' || s[len - 1] == '\\');
}

/* Safe string copy with guaranteed null termination. */
static void safe_strcpy(char *dst, size_t dstsz, const char *src)
{
    size_t i;

    if (dstsz == 0) return;

    for (i = 0; i + 1 < dstsz && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* Concatenate dir + sep + name into out (size outsz) safely. */
static void path_join(char *out, size_t outsz, const char *dir,
                      const char *name)
{
    if (outsz == 0) return;

    out[0] = '\0';
    safe_strcpy(out, outsz, dir);

    if (!ends_with_sep(dir)) {
        size_t len = strlen(out);
        if (len < outsz - 1) {
            out[len] = '\\';
            out[len + 1] = '\0';
        }
    }

    size_t len = strlen(out);
    if (len < outsz - 1) {
        strncat(out, name, outsz - len - 1);
    }
}

/* Create directory and all parents (Windows-only, uses CreateDirectoryA). */
static void bp_mkdirs(const char *path)
{
    char tmp[WCX_MAX_PATH];
    char *p;

    if (strlen(path) >= sizeof(tmp)) return;
    strcpy(tmp, path);

    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
}

/* Create all parent directories for a file path. */
static void mkdirs_for_file(const char *path)
{
    char tmp[WCX_MAX_PATH];
    char *sep;
    char *bsep;

    if (strlen(path) >= sizeof(tmp)) return;
    strcpy(tmp, path);

    sep  = strrchr(tmp, '/');
    bsep = strrchr(tmp, '\\');
    if (bsep != NULL && (sep == NULL || bsep > sep)) sep = bsep;

    if (sep != NULL && sep != tmp) {
        *sep = '\0';
        bp_mkdirs(tmp);
    }
}

/* ==========================================================================
 * Archive handle -- one per OpenArchive call
 * ========================================================================== */

typedef struct {
    char             arc_name[WCX_MAX_PATH];
    FILE            *fp;
    bp_io_t          io;

    /* Entry table populated at open time via bp_archive_list. */
    struct wcx_entry *entries;
    int               entry_count;
    int               entry_cap;
    int               current;       /* next index for ReadHeader */
    int               selected;      /* index of file returned by last ReadHeader */

    /* Callbacks set by Total Commander. */
    tProcessDataProc  proc_cb;
    tChangeVolProc    vol_cb;
} wcx_arc_t;

typedef struct wcx_entry {
    char      name[WCX_MAX_PATH];   /* archive path, backslash separators */
    char      name_fwd[WCX_MAX_PATH]; /* same path, forward-slash separators */
    bp_uint32 orig_size;
    bp_uint32 comp_size;
    bp_uint32 mtime;
    int       is_dir;
    int       method;               /* 0=stored, 1=BoltLite, 2=boltlz, 3=fluff, 4=zstd */
} wcx_entry_t;

/* bp_archive_list callback: append one entry to arc->entries[]. */
static int list_populate(const bp_entry_t *e, void *user_data)
{
    wcx_arc_t   *arc = (wcx_arc_t *)user_data;
    wcx_entry_t *ent;
    struct wcx_entry *newbuf;
    int newcap;

    /* Grow the entry array if needed. */
    if (arc->entry_count >= arc->entry_cap) {
        newcap = (arc->entry_cap == 0) ? 64 : arc->entry_cap * 2;
        newbuf = (struct wcx_entry *)realloc(
                     arc->entries,
                     (size_t)newcap * sizeof(wcx_entry_t));
        if (newbuf == NULL) return -1;
        arc->entries   = newbuf;
        arc->entry_cap = newcap;
    }

    ent = &arc->entries[arc->entry_count++];
    memset(ent, 0, sizeof(*ent));

    /* Archive stores UTF-8; convert to ACP for Total Commander display
     * and filesystem operations. */
    if (utf8_to_acp(e->name, ent->name_fwd, WCX_MAX_PATH) != 0) {
        strncpy(ent->name_fwd, e->name, WCX_MAX_PATH - 1);
        ent->name_fwd[WCX_MAX_PATH - 1] = '\0';
    }

    strncpy(ent->name, ent->name_fwd, WCX_MAX_PATH - 1);
    ent->name[WCX_MAX_PATH - 1] = '\0';
    to_backslash(ent->name);  /* TC expects Windows path separators */

    ent->orig_size = bp_u64_to_u32(e->original_size);
    ent->comp_size = bp_u64_to_u32(e->compressed_size);
    ent->mtime     = e->mtime.lo;
    ent->is_dir    = (e->entry_type == BP_TYPE_DIR);
    ent->method    = (int)e->method;

    return 0;
}

/* ==========================================================================
 * Extraction callbacks (used by ProcessFile via bp_archive_extract_all)
 * ========================================================================== */

typedef struct {
    const char       *target_fwd;   /* entry name, forward-slash separators */
    const char       *dest_path;    /* full OS path to write extracted file  */
    int               found;        /* set to 1 once the target is matched   */
    tProcessDataProc  proc_cb;
} extract_ctx_t;

static bp_io_t *extract_open(const bp_entry_t *e, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char fwd[WCX_MAX_PATH];
    char fwd_acp[WCX_MAX_PATH];
    FILE *fp;
    bp_io_t *io;

    /* Convert UTF-8 archive name to ACP, then normalize to forward slashes
     * so it can be compared with the ACP target_fwd from list_populate. */
    if (utf8_to_acp(e->name, fwd_acp, sizeof(fwd_acp)) != 0) {
        strncpy(fwd_acp, e->name, WCX_MAX_PATH - 1);
        fwd_acp[WCX_MAX_PATH - 1] = '\0';
    }
    strncpy(fwd, fwd_acp, WCX_MAX_PATH - 1);
    fwd[WCX_MAX_PATH - 1] = '\0';
    to_fwdslash(fwd);

    if (strcmp(fwd, ctx->target_fwd) != 0) return NULL; /* skip this entry */

    ctx->found = 1;

    /* Directories: just create the directory tree, no file to open. */
    if (e->entry_type == BP_TYPE_DIR) {
        bp_mkdirs(ctx->dest_path);
        return NULL;
    }

    /* Skip non-data entry types that are not meaningful on Windows. */
    if (e->entry_type == BP_TYPE_RSRC || e->entry_type == BP_TYPE_META) {
        return NULL;
    }

    mkdirs_for_file(ctx->dest_path);

    fp = fopen(ctx->dest_path, "wb");
    if (fp == NULL) return NULL;

    io = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (io == NULL) { fclose(fp); return NULL; }
    bp_io_from_file(io, fp);
    return io;
}

static void extract_done(const bp_entry_t *e, bp_io_t *out, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    FILE *fp;

    (void)e;

    if (out == NULL) return;
    fp = (FILE *)out->handle;
    fclose(fp);
    free(out);

    if (ctx->proc_cb) {
        /* Notify TC that the file has been fully processed. */
        ctx->proc_cb((char *)ctx->dest_path, -1);
    }
}

/* ==========================================================================
 * Pack helpers (used by PackFiles)
 * ========================================================================== */

static int pack_add_file(bp_archive_t *ar, const char *src_path,
                         const char *arc_name)
{
    struct _stat64 st;
    FILE *fp;
    bp_io_t fio;
    char dir_arc[WCX_MAX_PATH + 1];  /* +1 for trailing '/' appended below */
    int rc;

    if (_stat64(src_path, &st) != 0) return BP_ERR_IO;

    if (S_ISDIR(st.st_mode)) {
        /* Ensure the archive directory name ends with '/'. */
        if (!ends_with_sep(arc_name)) {
            snprintf(dir_arc, sizeof(dir_arc), "%s/", arc_name);
        } else {
            strncpy(dir_arc, arc_name, WCX_MAX_PATH - 1);
            dir_arc[WCX_MAX_PATH - 1] = '\0';
        }

        {
            char dir_arc_utf8[WCX_MAX_PATH * 2];
            if (acp_to_utf8(dir_arc, dir_arc_utf8, sizeof(dir_arc_utf8)) != 0) {
                strncpy(dir_arc_utf8, dir_arc, sizeof(dir_arc_utf8) - 1);
                dir_arc_utf8[sizeof(dir_arc_utf8) - 1] = '\0';
            }
            rc = bp_archive_add_entry(ar, dir_arc_utf8, BP_TYPE_DIR,
                                      BP_OS_WINDOWS, NULL, bp_u64_from_u32(0),
                                      bp_off_to_u64((bp_off_t)st.st_mtime));
        }
        /* Do NOT recurse into subdirectories here. Total Commander's
         * AddList already enumerates every file and directory individually,
         * so recursing would produce duplicate entries in the archive. */
        return rc;
    }

    /* Regular file. */
    fp = fopen(src_path, "rb");
    if (fp == NULL) return BP_ERR_IO;

    bp_io_from_file(&fio, fp);

    {
        char arc_name_utf8[WCX_MAX_PATH * 2];
        if (acp_to_utf8(arc_name, arc_name_utf8, sizeof(arc_name_utf8)) != 0) {
            strncpy(arc_name_utf8, arc_name, sizeof(arc_name_utf8) - 1);
            arc_name_utf8[sizeof(arc_name_utf8) - 1] = '\0';
        }
        rc = bp_archive_add_entry(ar, arc_name_utf8, BP_TYPE_DATA,
                                  BP_OS_WINDOWS, &fio,
                                  bp_off_to_u64((bp_off_t)st.st_size),
                                  bp_off_to_u64((bp_off_t)st.st_mtime));
    }
    fclose(fp);
    return rc;
}

/* ==========================================================================
 * Global pack-time callbacks
 *
 * Total Commander calls SetChangeVolProc / SetProcessDataProc with
 * hArcData == (HANDLE)-1 before PackFiles (no open archive).  Store
 * those callbacks here so PackFiles can use them.
 * ========================================================================== */

static tChangeVolProc   g_vol_cb;
static tProcessDataProc g_proc_cb;

/* ==========================================================================
 * Registry helpers
 *
 * Read/write BoltPack configuration from HKEY_CURRENT_USER\Software\BoltPack
 * ========================================================================== */

/*
 * Get the configured compression method from the registry.
 * Returns: 0 (stored), 1 (BoltLite), 2 (boltlz -- default), 3 (fluff),
 *          or 4 (zstd, modern builds only)
 */
static int get_compression_method(void)
{
    HKEY hKey;
    DWORD method = 2;  /* default: boltlz */
    DWORD size = sizeof(DWORD);
    DWORD regType;
    LONG ret;

    ret = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\BoltPack", 0,
                        KEY_READ, &hKey);
    if (ret != ERROR_SUCCESS) {
        return 2;  /* registry key doesn't exist, use default */
    }

    ret = RegQueryValueExA(hKey, "CompressionMethod", NULL, &regType,
                           (LPBYTE)&method, &size);

    RegCloseKey(hKey);

    if (ret != ERROR_SUCCESS) {
        return 2;  /* value doesn't exist, use default */
    }

    /* Validate: accept 0-4; fall back to boltlz for unknown values */
    if (method > 4) {
        return 2;
    }
    /* method 4 (zstd) requires a BOLTPACK_ZSTD build */
#ifndef BOLTPACK_ZSTD
    if (method == 4) {
        return 2;
    }
#endif

    return (int)method;
}

/*
 * Set the compression method in the registry.
 * Returns 0 on success, -1 on failure.
 */
static int set_compression_method(int method)
{
    HKEY hKey;
    DWORD dwDisposition;
    DWORD dwMethod = (DWORD)method;
    LONG ret;

    ret = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\BoltPack", 0, NULL,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL,
                          &hKey, &dwDisposition);
    if (ret != ERROR_SUCCESS) {
        return -1;
    }

    ret = RegSetValueExA(hKey, "CompressionMethod", 0, REG_DWORD,
                         (const BYTE *)&dwMethod, sizeof(dwMethod));

    RegCloseKey(hKey);

    return (ret == ERROR_SUCCESS) ? 0 : -1;
}

/* ==========================================================================
 * WCX exported functions
 * ========================================================================== */

__declspec(dllexport)
HANDLE WINAPI OpenArchive(tOpenArchiveData *data)
{
    wcx_arc_t *arc;
    int rc;

    arc = (wcx_arc_t *)calloc(1, sizeof(wcx_arc_t));
    if (arc == NULL) { data->OpenResult = E_NO_MEMORY; return (HANDLE)0; }

    arc->current  = 0;
    arc->selected = -1;

    strncpy(arc->arc_name, data->ArcName, WCX_MAX_PATH - 1);

    arc->fp = fopen(data->ArcName, "rb");
    if (arc->fp == NULL) {
        free(arc);
        data->OpenResult = E_EOPEN;
        return (HANDLE)0;
    }

    bp_io_from_file(&arc->io, arc->fp);

    /* Load the full entry table upfront so ReadHeader can work without
     * re-scanning the archive on each call. bp_archive_list handles its
     * own seeking internally, so the file position afterwards does not
     * matter -- bp_archive_extract_all will also seek internally. */
    rc = bp_archive_list(&arc->io, list_populate, arc);
    if (rc != BP_OK) {
        free(arc->entries);
        fclose(arc->fp);
        free(arc);
        data->OpenResult = (rc == BP_ERR_MAGIC) ? E_UNKNOWN_FORMAT
                                                 : E_BAD_ARCHIVE;
        return (HANDLE)0;
    }

    data->OpenResult = E_SUCCESS;
    return (HANDLE)arc;
}

__declspec(dllexport)
int WINAPI ReadHeader(HANDLE hArcData, tHeaderData *hd)
{
    wcx_arc_t   *arc = (wcx_arc_t *)hArcData;
    wcx_entry_t *e;

    if (arc->current >= arc->entry_count) return E_END_ARCHIVE;

    e = &arc->entries[arc->current];
    arc->selected = arc->current;
    arc->current++;

    memset(hd, 0, sizeof(*hd));
    safe_strcpy(hd->ArcName, sizeof(hd->ArcName), arc->arc_name);
    safe_strcpy(hd->FileName, sizeof(hd->FileName), e->name);

    hd->Flags    = 0;
    hd->PackSize = (int)(e->comp_size & 0xFFFFFFFFu);
    hd->UnpSize  = (int)(e->orig_size & 0xFFFFFFFFu);
    hd->HostOS   = 0; /* 0 = DOS/Windows in WCX convention */
    hd->FileCRC  = 0; /* WCX FileCRC field is not exposed via the list API */
    hd->FileTime = unix_to_dostime(e->mtime);
    hd->UnpVer   = 0;
    hd->Method   = e->method;
    hd->FileAttr = e->is_dir ? FILE_ATTRIBUTE_DIRECTORY
                              : FILE_ATTRIBUTE_NORMAL;
    hd->CmtBuf     = NULL;
    hd->CmtBufSize = 0;
    hd->CmtSize    = 0;
    hd->CmtState   = 0;

    return E_SUCCESS;
}

__declspec(dllexport)
int WINAPI ReadHeaderEx(HANDLE hArcData, tHeaderDataEx *hd)
{
    wcx_arc_t   *arc = (wcx_arc_t *)hArcData;
    wcx_entry_t *e;

    if (arc->current >= arc->entry_count) return E_END_ARCHIVE;

    e = &arc->entries[arc->current];
    arc->selected = arc->current;
    arc->current++;

    memset(hd, 0, sizeof(*hd));
    safe_strcpy(hd->ArcName, sizeof(hd->ArcName), arc->arc_name);
    safe_strcpy(hd->FileName, sizeof(hd->FileName), e->name);

    hd->Flags        = 0;
    hd->PackSize     = e->comp_size;
    hd->PackSizeHigh = 0;
    hd->UnpSize      = e->orig_size;
    hd->UnpSizeHigh  = 0;
    hd->HostOS       = 0;
    hd->FileCRC      = 0;
    hd->FileTime     = unix_to_dostime(e->mtime);
    hd->UnpVer       = 0;
    hd->Method       = e->method;
    hd->FileAttr     = e->is_dir ? FILE_ATTRIBUTE_DIRECTORY
                                  : FILE_ATTRIBUTE_NORMAL;
    hd->CmtBuf      = NULL;
    hd->CmtBufSize  = 0;
    hd->CmtSize     = 0;
    hd->CmtState    = 0;

    return E_SUCCESS;
}

__declspec(dllexport)
int WINAPI ProcessFile(HANDLE hArcData, int Operation,
                       char *DestPath, char *DestName)
{
    wcx_arc_t   *arc = (wcx_arc_t *)hArcData;
    wcx_entry_t *e;
    extract_ctx_t ctx;
    char dest[WCX_MAX_PATH];
    int rc;

    if (arc->selected < 0 || arc->selected >= arc->entry_count) {
        return E_EABORTED;
    }

    e = &arc->entries[arc->selected];
    arc->selected = -1;

    if (Operation == PK_SKIP) return E_SUCCESS;

    if (Operation == PK_TEST) {
        /* bp_archive_test() could be called here for a full integrity
         * check, but TC drives testing through its own CRC verification
         * after extraction. Return success and let TC handle it. */
        return E_SUCCESS;
    }

    /* PK_EXTRACT */

    /* Determine the destination filesystem path. */
    if (DestName != NULL && DestName[0] != '\0') {
        /* TC provides the full target path. */
        strncpy(dest, DestName, WCX_MAX_PATH - 1);
        dest[WCX_MAX_PATH - 1] = '\0';
    } else if (DestPath != NULL && DestPath[0] != '\0') {
        /* TC provides the target directory; append the archive entry name. */
        path_join(dest, sizeof(dest), DestPath, e->name);
    } else {
        strncpy(dest, e->name, WCX_MAX_PATH - 1);
        dest[WCX_MAX_PATH - 1] = '\0';
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.target_fwd = e->name_fwd;
    ctx.dest_path  = dest;
    ctx.proc_cb    = arc->proc_cb;

    /* bp_archive_extract_all manages its own seeking (reads footer to
     * locate the file table, then reads entries sequentially from there).
     * Calling it once per requested file is safe with the kept file handle
     * and will naturally skip non-matching entries via the open callback. */
    rc = bp_archive_extract_all(&arc->io,
                                extract_open, extract_done,
                                NULL, &ctx);

    if (rc == BP_ERR_ABORTED) return E_EABORTED;
    if (rc != BP_OK)          return E_BAD_DATA;
    if (!ctx.found)           return E_BAD_DATA;

    if (arc->proc_cb && !e->is_dir) {
        arc->proc_cb(dest, (int)e->orig_size);
    }

    return E_SUCCESS;
}

__declspec(dllexport)
int WINAPI CloseArchive(HANDLE hArcData)
{
    wcx_arc_t *arc = (wcx_arc_t *)hArcData;
    if (arc == NULL) return E_SUCCESS;
    if (arc->fp) fclose(arc->fp);
    free(arc->entries);
    free(arc);
    return E_SUCCESS;
}

__declspec(dllexport)
int WINAPI GetPackerCaps(void)
{
    /* PK_CAPS_NEW: can create archives.
     * PK_CAPS_MODIFY: can modify (delete from) archives.
     * PK_CAPS_MULTIPLE: archives hold multiple files.
     * PK_CAPS_DELETE: can delete files from archives. */
    return PK_CAPS_NEW | PK_CAPS_MODIFY | PK_CAPS_MULTIPLE | PK_CAPS_DELETE;
}

__declspec(dllexport)
void WINAPI SetChangeVolProc(HANDLE hArcData, tChangeVolProc pProc)
{
    if (hArcData == (HANDLE)-1 || hArcData == NULL) {
        g_vol_cb = pProc;
    } else {
        wcx_arc_t *arc = (wcx_arc_t *)hArcData;
        arc->vol_cb = pProc;
    }
}

__declspec(dllexport)
void WINAPI SetProcessDataProc(HANDLE hArcData, tProcessDataProc pProc)
{
    if (hArcData == (HANDLE)-1 || hArcData == NULL) {
        g_proc_cb = pProc;
    } else {
        wcx_arc_t *arc = (wcx_arc_t *)hArcData;
        arc->proc_cb = pProc;
    }
}

__declspec(dllexport)
int WINAPI PackFiles(char *PackedFile, char *SubPath,
                     char *SrcPath, char *AddList, int Flags)
{
    FILE *fp;
    bp_io_t aio;
    bp_archive_t ar;
    char *name;
    char src[WCX_MAX_PATH];
    char arc_name[WCX_MAX_PATH];
    const char *base;
    char *sep;
    char *fsep;
    int rc;
    int appending = 0;
    int method;

    rc = bp_compress_init();
    if (rc != BP_OK) return E_BAD_DATA;

    /* Get compression method from registry (defaults to boltlz) */
    method = get_compression_method();

    /* If the archive already exists, append in-place. */
    {
        DWORD attr = GetFileAttributesA(PackedFile);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            appending = 1;
        }
    }

    if (appending) {
        fp = fopen(PackedFile, "r+b");
        if (fp == NULL) return E_EOPEN;

        bp_io_from_file(&aio, fp);
        rc = bp_archive_open_append(&ar, &aio, BP_BLOCK_DEFAULT,
                                    method);
        if (rc != BP_OK) {
            fclose(fp);
            return E_BAD_DATA;
        }
    } else {
        fp = fopen(PackedFile, "w+b");
        if (fp == NULL) return E_ECREATE;

        bp_io_from_file(&aio, fp);
        rc = bp_archive_create(&ar, &aio, BP_BLOCK_DEFAULT, method);
        if (rc != BP_OK) {
            fclose(fp);
            return E_BAD_DATA;
        }
    }

    /* AddList is a double-null-terminated list of filenames. */
    for (name = AddList; name != NULL && *name != '\0';
         name += strlen(name) + 1) {

        /* Build the full source path. */
        if (SrcPath != NULL && SrcPath[0] != '\0') {
            path_join(src, sizeof(src), SrcPath, name);
        } else {
            strncpy(src, name, WCX_MAX_PATH - 1);
            src[WCX_MAX_PATH - 1] = '\0';
        }

        /* Build the archive entry name. */
        if (SubPath != NULL && SubPath[0] != '\0') {
            safe_strcpy(arc_name, sizeof(arc_name), SubPath);

            if (!ends_with_sep(SubPath)) {
                size_t len = strlen(arc_name);
                if (len < sizeof(arc_name) - 1) {
                    arc_name[len] = '/';
                    arc_name[len + 1] = '\0';
                }
            }

            size_t len = strlen(arc_name);
            if (len < sizeof(arc_name) - 1) {
                strncat(arc_name, name, sizeof(arc_name) - len - 1);
            }
        } else if (Flags & PK_PACK_SAVE_PATHS) {
            /* Preserve relative paths as-is. */
            safe_strcpy(arc_name, sizeof(arc_name), name);
        } else {
            /* Strip any directory prefix; store bare filename only. */
            sep  = strrchr(name, '\\');
            fsep = strrchr(name, '/');
            if (fsep != NULL && (sep == NULL || fsep > sep)) sep = fsep;
            base = (sep != NULL) ? sep + 1 : name;
            safe_strcpy(arc_name, sizeof(arc_name), base);
        }

        /* Archive paths always use forward slashes. */
        to_fwdslash(arc_name);

        rc = pack_add_file(&ar, src, arc_name);
        if (rc != BP_OK) {
            bp_archive_close(&ar);
            fclose(fp);
            return E_ECOPY;
        }
    }

    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    fclose(fp);

    if (rc != BP_OK) {
        return E_BAD_DATA;
    }

    return E_SUCCESS;
}

__declspec(dllexport)
int WINAPI DeleteFiles(char *PackedFile, char *DeleteList)
{
    FILE *src_fp, *dst_fp;
    bp_io_t src_io, dst_io;
    char tmp_name[WCX_MAX_PATH];
    int rc;
    bp_uint32 deleted = 0;

    /* Count the entries and build an array of name pointers.
     * DeleteList is double-null terminated, names use backslashes.
     * Archive entries use forward slashes, so convert each name. */
    {
        const char **names;
        bp_uint32 count = 0;
        char *p;
        bp_uint32 i;

        /* first pass: count entries */
        for (p = DeleteList; p && *p; p += strlen(p) + 1)
            count++;

        if (count == 0) return E_SUCCESS;

        names = (const char **)malloc(count * sizeof(char *));
        if (names == NULL) return E_NO_MEMORY;

        /* second pass: build array with forward slashes */
        for (i = 0, p = DeleteList; *p; p += strlen(p) + 1, i++) {
            char *dup = _strdup(p);
            if (dup == NULL) {
                while (i > 0) free((void *)names[--i]);
                free(names);
                return E_NO_MEMORY;
            }
            to_fwdslash(dup);
            names[i] = dup;
        }

        src_fp = fopen(PackedFile, "rb");
        if (src_fp == NULL) {
            for (i = 0; i < count; i++) free((void *)names[i]);
            free(names);
            return E_EOPEN;
        }

        snprintf(tmp_name, sizeof(tmp_name), "%s.bp_tmp", PackedFile);
        dst_fp = fopen(tmp_name, "w+b");
        if (dst_fp == NULL) {
            fclose(src_fp);
            for (i = 0; i < count; i++) free((void *)names[i]);
            free(names);
            return E_ECREATE;
        }

        bp_io_from_file(&src_io, src_fp);
        bp_io_from_file(&dst_io, dst_fp);

        rc = bp_archive_delete(&src_io, &dst_io, names, count, &deleted);

        fclose(src_fp);
        fclose(dst_fp);

        for (i = 0; i < count; i++) free((void *)names[i]);
        free(names);
    }

    if (rc != BP_OK || deleted == 0) {
        remove(tmp_name);
        return (rc != BP_OK) ? E_BAD_DATA : E_SUCCESS;
    }

    if (remove(PackedFile) != 0 || rename(tmp_name, PackedFile) != 0) {
        return E_ECOPY;
    }

    return E_SUCCESS;
}

/* ==========================================================================
 * DllMain
 * ========================================================================== */

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)hInst;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        bp_compress_init();
    }
    return TRUE;
}
