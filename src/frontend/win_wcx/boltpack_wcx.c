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
#include "../../core/bp_metadata.h"
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
#include <sys/utime.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>

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
    UINT codepage = GetACP();
    DWORD flags = codepage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    int wlen, ulen;

    wlen = MultiByteToWideChar(codepage, flags, acp, -1, wbuf, WCX_MAX_PATH);
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
    BOOL used_default = FALSE;
    UINT codepage = GetACP();
    DWORD flags = codepage == CP_UTF8 ? 0 : WC_NO_BEST_FIT_CHARS;
    int wlen, alen;

    wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                               wbuf, WCX_MAX_PATH);
    if (wlen == 0) return -1;

    alen = WideCharToMultiByte(codepage, flags, wbuf, wlen, acp, acp_size,
                               NULL, codepage == CP_UTF8 ? NULL : &used_default);
    if (alen == 0 || used_default) return -1;

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
static int unix_to_dostime(bp_uint64 mtime, int mtime_valid)
{
    bp_off_t seconds;
    time_t t;
    struct tm *tm_val;
    int dosdate;
    int dostime;

    if (!mtime_valid) return 0;
    seconds = bp_u64_to_off(mtime);
    if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), mtime) != 0)
        return 0;
    t = (time_t)seconds;
    if ((bp_off_t)t != seconds) return 0;
    tm_val = localtime(&t);
    if (tm_val == NULL) return 0;
    if (tm_val->tm_year + 1900 < 1980 || tm_val->tm_year + 1900 > 2107)
        return 0;

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

/* Create/check each archive-controlled directory component without following
 * existing reparse points. The target root is supplied by Total Commander. */
static int wcx_ensure_directories(const char *path, int include_leaf)
{
    char input[WCX_MAX_PATH], full[WCX_MAX_PATH], current[WCX_MAX_PATH];
    char *file_part = NULL;
    DWORD length, attr;
    size_t root_len, i, n;

    if (path == NULL || strlen(path) >= sizeof(input)) return -1;
    strcpy(input, path);
    n = strlen(input);
    while (include_leaf && n > 0 &&
           (input[n - 1] == '/' || input[n - 1] == '\\')) {
        if (n == 3 && input[1] == ':' && input[2] == input[n - 1]) break;
        input[--n] = '\0';
    }

    length = GetFullPathNameA(input, (DWORD)sizeof(full), full, &file_part);
    if (length == 0 || length >= sizeof(full)) return -1;
    n = strlen(full);
    if (!include_leaf) {
        char *slash = strrchr(full, '\\');
        char *fslash = strrchr(full, '/');
        if (fslash != NULL && (slash == NULL || fslash > slash)) slash = fslash;
        if (slash == NULL) return -1;
        if (slash == full + 2 && full[1] == ':') slash[1] = '\0';
        else if (slash == full) return -1;
        else *slash = '\0';
        n = strlen(full);
    }
    if (n >= 2 && full[0] == '\\' && full[1] == '\\') {
        size_t p = 2;
        while (p < n && full[p] != '\\') p++;
        if (p == n) return -1;
        p++;
        while (p < n && full[p] != '\\') p++;
        root_len = p;
    } else if (n >= 3 && full[1] == ':') {
        root_len = 3;
    } else {
        return -1;
    }
    if (root_len == 0 || root_len >= sizeof(current)) return -1;
    memcpy(current, full, root_len);
    current[root_len] = '\0';
    attr = GetFileAttributesA(current);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
        (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return -1;

    for (i = root_len; i <= n; i++) {
        if (i < n && full[i] != '\\' && full[i] != '/') continue;
        if (i == root_len) { root_len = i + 1; continue; }
        if (i >= sizeof(current)) return -1;
        memcpy(current, full, i);
        current[i] = '\0';
        attr = GetFileAttributesA(current);
        if (attr == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) ||
                (!CreateDirectoryA(current, NULL) && GetLastError() != ERROR_ALREADY_EXISTS))
                return -1;
            attr = GetFileAttributesA(current);
        }
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
            (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return -1;
        root_len = i + 1;
    }
    return 0;
}

static LONG g_wcx_temp_counter = 0;

static int wcx_open_sibling_stage(const char *target, char *temporary,
                                 size_t temporary_size, FILE **file)
{
    char directory[WCX_MAX_PATH], full_target[WCX_MAX_PATH];
    char *slash, *fslash;
    DWORD full_length;
    size_t length;
    int attempt;
    full_length = GetFullPathNameA(target, (DWORD)sizeof(full_target),
                                   full_target, NULL);
    if (full_length == 0 || full_length >= sizeof(full_target)) return -1;
    length = strlen(full_target);
    if (length >= sizeof(directory) || temporary_size < WCX_MAX_PATH) return -1;
    strcpy(directory, full_target);
    slash = strrchr(directory, '\\');
    fslash = strrchr(directory, '/');
    if (fslash != NULL && (slash == NULL || fslash > slash)) slash = fslash;
    if (slash == NULL) return -1;
    slash[1] = '\0';
    for (attempt = 0; attempt < 256; attempt++) {
        LONG serial = InterlockedIncrement(&g_wcx_temp_counter);
        HANDLE handle;
        int fd;
        FILE *fp;
        int count = _snprintf(temporary, temporary_size,
                    "%s.boltpack-%08lx.tmp", directory,
                    (unsigned long)(GetTickCount() ^ GetCurrentProcessId() ^ serial));
        if (count < 0 || (size_t)count >= temporary_size) return -1;
        handle = CreateFileA(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                             NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            return -1;
        }
        fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_WRONLY);
        if (fd < 0) { CloseHandle(handle); DeleteFileA(temporary); return -1; }
        fp = _fdopen(fd, "wb");
        if (fp == NULL) { _close(fd); DeleteFileA(temporary); return -1; }
        *file = fp;
        return 0;
    }
    return -1;
}

static int wcx_copy_stream(FILE *source, FILE *destination)
{
    unsigned char buffer[64 * 1024];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), source)) != 0) {
        if (fwrite(buffer, 1, count, destination) != count) return -1;
    }
    return ferror(source) ? -1 : 0;
}

static int wcx_flush_stage(FILE *file)
{
    return fflush(file) == 0 && _commit(_fileno(file)) == 0 ? 0 : -1;
}

static int wcx_apply_fat(const char *path, bp_uint8 valid, bp_uint8 value)
{
    DWORD old_attributes = GetFileAttributesA(path);
    DWORD desired, actual;
    if (old_attributes == INVALID_FILE_ATTRIBUTES) return -1;
    desired = (old_attributes & ~(DWORD)valid) | (DWORD)value;
    if (!SetFileAttributesA(path, desired)) return -1;
    actual = GetFileAttributesA(path);
    return actual != INVALID_FILE_ATTRIBUTES && (actual & valid) == value ? 0 : -1;
}

static int wcx_set_time(const char *path, bp_uint64 mtime)
{
    bp_off_t seconds = bp_u64_to_off(mtime);
    time_t t;
    struct _stat64 st;
    struct _utimbuf ut;
    if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), mtime) != 0) return -1;
    t = (time_t)seconds;
    if ((bp_off_t)t != seconds || _stat64(path, &st) != 0) return -1;
    ut.actime = (time_t)st.st_atime;
    ut.modtime = t;
    return _utime(path, &ut);
}

static int wcx_set_open_file_time(FILE *fp, bp_uint64 mtime)
{
    bp_off_t seconds = bp_u64_to_off(mtime);
    ULONGLONG epoch = 11644473600ULL;
    ULONGLONG ticks;
    FILETIME file_time;
    intptr_t raw_handle;
    if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), mtime) != 0 ||
        (ULONGLONG)seconds > (ULLONG_MAX / 10000000ULL) - epoch) return -1;
    ticks = ((ULONGLONG)seconds + epoch) * 10000000ULL;
    file_time.dwLowDateTime = (DWORD)ticks;
    file_time.dwHighDateTime = (DWORD)(ticks >> 32);
    raw_handle = _get_osfhandle(_fileno(fp));
    if (raw_handle == -1) return -1;
    return SetFileTime((HANDLE)raw_handle, NULL, NULL, &file_time) ? 0 : -1;
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
    int               list_error;
    int               current;       /* next index for ReadHeader */
    int               selected;      /* index of file returned by last ReadHeader */

    /* Callbacks set by Total Commander. */
    tProcessDataProc  proc_cb;
    tChangeVolProc    vol_cb;
} wcx_arc_t;

typedef struct wcx_entry {
    char      name[WCX_MAX_PATH];   /* archive path, backslash separators */
    char      name_fwd[WCX_MAX_PATH]; /* same path, forward-slash separators */
    bp_uint64 orig_size;
    bp_uint64 comp_size;
    bp_uint64 mtime;
    bp_uint16 flags;
    bp_uint8  entry_type;
    int       is_dir;
    int       method;               /* 0=stored, 1=BoltLite, 2=boltlz, 3=fluff, 4=zstd */
    int       extracted;
    int       directory_finalized;
    int       portable_meta_present;
    int       foreign_meta_present;
    int       resource_present;
    bp_uint8  fat_valid;
    bp_uint8  fat_value;
    char      output_path[WCX_MAX_PATH];
} wcx_entry_t;

static int wcx_component_safe(const WCHAR *name, size_t length)
{
    size_t i, base_length = length;
    WCHAR base[16];
    if (length == 0 || name[length - 1] == L'.' || name[length - 1] == L' ')
        return 0;
    for (i = 0; i < length; i++) {
        WCHAR c = name[i];
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'|' || c == L'?' || c == L'*' || c == L'\\') return 0;
        if (c == L'.' && base_length == length) base_length = i;
    }
    if (base_length == 0 || base_length >= sizeof(base) / sizeof(base[0])) return 1;
    memcpy(base, name, base_length * sizeof(WCHAR));
    base[base_length] = L'\0';
    if (_wcsicmp(base, L"CON") == 0 || _wcsicmp(base, L"PRN") == 0 ||
        _wcsicmp(base, L"AUX") == 0 || _wcsicmp(base, L"NUL") == 0 ||
        _wcsicmp(base, L"CONIN$") == 0 || _wcsicmp(base, L"CONOUT$") == 0)
        return 0;
    if (base_length == 4 &&
        (_wcsnicmp(base, L"COM", 3) == 0 || _wcsnicmp(base, L"LPT", 3) == 0) &&
        base[3] >= L'1' && base[3] <= L'9') return 0;
    return 1;
}

static int wcx_windows_name_safe(const char *name)
{
    WCHAR wide[WCX_MAX_PATH];
    int n = MultiByteToWideChar(CP_ACP, 0, name, -1, wide, WCX_MAX_PATH);
    size_t length, start, i;
    if (n <= 0) return 0;
    length = (size_t)n - 1;
    while (length > 0 && (wide[length - 1] == L'/' || wide[length - 1] == L'\\'))
        length--;
    start = 0;
    for (i = 0; i <= length; i++) {
        size_t component_length;
        if (i < length && wide[i] != L'/' && wide[i] != L'\\') continue;
        component_length = i - start;
        if (!wcx_component_safe(wide + start, component_length)) return 0;
        start = i + 1;
    }
    return length != 0;
}

static void wcx_trim_separator(char *path)
{
    size_t length = strlen(path);
    while (length > 0 && (path[length - 1] == '/' || path[length - 1] == '\\'))
        path[--length] = '\0';
}

static int wcx_windows_case_key(const char *path, WCHAR *key, size_t capacity)
{
    WCHAR wide[WCX_MAX_PATH], folded[WCX_MAX_PATH];
    int wide_length, folded_length;
    size_t length;
    wide_length = MultiByteToWideChar(CP_ACP, 0, path, -1, wide, WCX_MAX_PATH);
    if (wide_length <= 0) return -1;
    length = (size_t)wide_length - 1;
    while (length > 0 && (wide[length - 1] == L'/' || wide[length - 1] == L'\\'))
        length--;
    folded_length = LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, wide,
                                 (int)length, folded, WCX_MAX_PATH - 1);
    if (folded_length < 0 || (size_t)folded_length + 1 > capacity) return -1;
    if (folded_length > 0) memcpy(key, folded, (size_t)folded_length * sizeof(WCHAR));
    key[folded_length] = L'\0';
    return folded_length;
}

static int wcx_projected_collision(const wcx_arc_t *arc, const char *name,
                                   int is_dir)
{
    char candidate[WCX_MAX_PATH];
    WCHAR candidate_key[WCX_MAX_PATH];
    int i;
    safe_strcpy(candidate, sizeof(candidate), name);
    wcx_trim_separator(candidate);
    if (wcx_windows_case_key(candidate, candidate_key,
                             sizeof(candidate_key) / sizeof(candidate_key[0])) < 0)
        return 1;
    for (i = 0; i + 1 < arc->entry_count; i++) {
        char existing[WCX_MAX_PATH];
        WCHAR existing_key[WCX_MAX_PATH];
        size_t candidate_len, existing_len;
        safe_strcpy(existing, sizeof(existing), arc->entries[i].name);
        wcx_trim_separator(existing);
        if (wcx_windows_case_key(existing, existing_key,
                                 sizeof(existing_key) / sizeof(existing_key[0])) < 0)
            return 1;
        candidate_len = wcslen(candidate_key);
        existing_len = wcslen(existing_key);
        if (wcscmp(candidate_key, existing_key) == 0) return 1;
        if (!arc->entries[i].is_dir && existing_len < candidate_len &&
            candidate_key[existing_len] == L'\\' &&
            wcsncmp(candidate_key, existing_key, existing_len) == 0) return 1;
        if (!is_dir && candidate_len < existing_len &&
            existing_key[candidate_len] == L'\\' &&
            wcsncmp(existing_key, candidate_key, candidate_len) == 0) return 1;
    }
    return 0;
}

static int wcx_record_supplementary(const bp_entry_t *e, wcx_arc_t *arc)
{
    char acp[WCX_MAX_PATH];
    char name[WCX_MAX_PATH];
    int i;
    if (!bp_name_is_safe(e->name) || utf8_to_acp(e->name, acp, sizeof(acp)) != 0) {
        arc->list_error = BP_ERR_CORRUPT;
        return -1;
    }
    safe_strcpy(name, sizeof(name), acp);
    to_backslash(name);
    for (i = 0; i < arc->entry_count; i++) {
        wcx_entry_t *subject = &arc->entries[i];
        if (strcmp(subject->name, name) != 0) continue;
        if (e->entry_type == BP_TYPE_RSRC) {
            if (subject->entry_type != BP_TYPE_DATA || subject->resource_present) {
                arc->list_error = BP_ERR_CORRUPT;
                return -1;
            }
            subject->resource_present = 1;
        } else if (e->entry_type == BP_TYPE_META) {
            if (subject->entry_type != BP_TYPE_DATA &&
                subject->entry_type != BP_TYPE_DIR) {
                arc->list_error = BP_ERR_CORRUPT;
                return -1;
            }
            if (e->platform == BP_META_PORTABLE) {
                if (subject->portable_meta_present) {
                    arc->list_error = BP_ERR_CORRUPT;
                    return -1;
                }
                subject->portable_meta_present = 1;
            } else subject->foreign_meta_present = 1;
        }
        return 0;
    }
    arc->list_error = BP_ERR_CORRUPT;
    return -1;
}

/* bp_archive_list callback: append one entry to arc->entries[]. */
static int list_populate(const bp_entry_t *e, void *user_data)
{
    wcx_arc_t   *arc = (wcx_arc_t *)user_data;
    wcx_entry_t *ent;
    struct wcx_entry *newbuf;
    int newcap;

    /* Supplementary records belong to their primary entry and are never
     * independent files in Total Commander. */
    if (e->entry_type == BP_TYPE_META || e->entry_type == BP_TYPE_RSRC)
        return wcx_record_supplementary(e, arc);
    if (e->entry_type != BP_TYPE_DATA && e->entry_type != BP_TYPE_DIR &&
        e->entry_type != BP_TYPE_SYMLINK) {
        arc->list_error = BP_ERR_CORRUPT;
        return -1;
    }
    if (!bp_name_is_safe(e->name)) { arc->list_error = BP_ERR_CORRUPT; return -1; }
    if (arc->entry_count >= 65535) { arc->list_error = BP_ERR_OVERFLOW; return -1; }

    /* Grow the entry array if needed. */
    if (arc->entry_count >= arc->entry_cap) {
        newcap = (arc->entry_cap == 0) ? 64 : arc->entry_cap * 2;
        newbuf = (struct wcx_entry *)realloc(
                     arc->entries,
                     (size_t)newcap * sizeof(wcx_entry_t));
        if (newbuf == NULL) { arc->list_error = BP_ERR_NOMEM; return -1; }
        arc->entries   = newbuf;
        arc->entry_cap = newcap;
    }

    ent = &arc->entries[arc->entry_count++];
    memset(ent, 0, sizeof(*ent));

    /* Archive stores UTF-8; refuse names that cannot be represented exactly
     * by Total Commander's ANSI filename API. */
    if (utf8_to_acp(e->name, ent->name_fwd, WCX_MAX_PATH) != 0 ||
        !wcx_windows_name_safe(ent->name_fwd)) {
        arc->list_error = BP_ERR_PARTIAL;
        return -1;
    }

    strncpy(ent->name, ent->name_fwd, WCX_MAX_PATH - 1);
    ent->name[WCX_MAX_PATH - 1] = '\0';
    to_backslash(ent->name);  /* TC expects Windows path separators */

    if (wcx_projected_collision(arc, ent->name, e->entry_type == BP_TYPE_DIR)) {
        arc->list_error = BP_ERR_PARTIAL;
        return -1;
    }

    ent->orig_size = e->original_size;
    ent->comp_size = e->compressed_size;
    ent->mtime     = e->mtime;
    ent->flags     = e->flags;
    ent->entry_type = e->entry_type;
    ent->is_dir    = (e->entry_type == BP_TYPE_DIR);
    ent->method    = (int)e->method;

    return 0;
}

/* ==========================================================================
 * Extraction callbacks (used by ProcessFile via bp_archive_extract_all)
 * ========================================================================== */

typedef struct {
    bp_io_t io;
    char target[WCX_MAX_PATH];
    char temporary[WCX_MAX_PATH];
} wcx_staged_file_t;

typedef struct {
    const char       *target_fwd;   /* entry name, forward-slash separators */
    const char       *dest_path;    /* full OS path to write extracted file  */
    wcx_entry_t      *selected;
    int               found;
    int               target_ok;
    int               partial;
    int               target_metadata_seen;
    int               open_error;
    bp_io_t           discard_io;
    bp_metadata_sink_t metadata;
    tProcessDataProc  proc_cb;
} extract_ctx_t;

static long discard_write(void *handle, const void *buf, bp_uint32 count)
{
    (void)handle;
    (void)buf;
    return count <= (bp_uint32)LONG_MAX ? (long)count : -1L;
}

static int extract_name_matches(const bp_entry_t *e, extract_ctx_t *ctx)
{
    char acp[WCX_MAX_PATH];
    if (!bp_name_is_safe(e->name) || utf8_to_acp(e->name, acp, sizeof(acp)) != 0) {
        ctx->open_error = BP_ERR_PARTIAL;
        return 0;
    }
    to_fwdslash(acp);
    return strcmp(acp, ctx->target_fwd) == 0;
}

static int extract_done(const bp_entry_t *e, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    if (out == &ctx->discard_io) return status;
    if (out == &ctx->metadata.io) {
        bp_portable_metadata_t parsed;
        int rc;
        if (status != BP_OK) return status;
        rc = bp_metadata_parse(ctx->metadata.data, ctx->metadata.length, &parsed);
        if (rc == BP_ERR_CORRUPT) return rc;
        if (parsed.supported_version &&
            parsed.subject_type != ctx->selected->entry_type) return BP_ERR_CORRUPT;
        if (extract_name_matches(e, ctx) && ctx->target_ok) {
            ctx->target_metadata_seen = 1;
            if (parsed.has_fat) {
                if (ctx->selected->is_dir) {
                    ctx->selected->fat_valid = parsed.fat_valid;
                    ctx->selected->fat_value = parsed.fat_value;
                } else if (wcx_apply_fat(ctx->dest_path, parsed.fat_valid,
                                          parsed.fat_value) != 0) {
                    ctx->partial = 1;
                }
            }
            if (rc == BP_ERR_PARTIAL || parsed.has_unknown) ctx->partial = 1;
        }
        return (rc == BP_ERR_PARTIAL || parsed.has_unknown) ? BP_ERR_PARTIAL : BP_OK;
    }
    if (e->entry_type == BP_TYPE_DATA) {
        wcx_staged_file_t *stage = (wcx_staged_file_t *)out;
        FILE *fp = (FILE *)out->handle;
        int result = status;
        if (status != BP_OK) {
            fclose(fp);
            DeleteFileA(stage->temporary);
            free(stage);
            ctx->target_ok = 0;
            return status;
        }
        if (result == BP_OK && (e->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
            wcx_set_open_file_time(fp, e->mtime) != 0)
            result = BP_ERR_PARTIAL;
        if (fclose(fp) != 0) result = BP_ERR_IO;
        if (e->flags & BP_ENTRY_FLAG_MODE_VALID) result = BP_ERR_PARTIAL;
        if (result == BP_OK || result == BP_ERR_PARTIAL) {
            if (!MoveFileExA(stage->temporary, stage->target,
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileA(stage->temporary);
                free(stage);
                ctx->target_ok = 0;
                return BP_ERR_IO;
            }
            ctx->target_ok = 1;
            if (result == BP_ERR_PARTIAL) ctx->partial = 1;
            if (ctx->proc_cb) ctx->proc_cb((char *)ctx->dest_path, -1);
            free(stage);
            return result;
        }
        DeleteFileA(stage->temporary);
        free(stage);
        ctx->target_ok = 0;
        return result;
    }
    return status;
}

static bp_io_t *extract_open(const bp_entry_t *e, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    int matches = extract_name_matches(e, ctx);
    FILE *fp;
    if (e->entry_type == BP_TYPE_META) {
        if (matches && ctx->target_ok) {
            if (e->platform == BP_META_PORTABLE) {
                bp_metadata_sink_init(&ctx->metadata);
                return &ctx->metadata.io;
            }
            ctx->partial = 1;
        }
        return &ctx->discard_io;
    }
    if (e->entry_type == BP_TYPE_RSRC) {
        if (matches && ctx->target_ok) ctx->partial = 1;
        return &ctx->discard_io;
    }
    if (e->entry_type == BP_TYPE_DIR) {
        if (!matches) return NULL;
        ctx->found = 1;
        ctx->target_ok = 0;
        if (wcx_ensure_directories(ctx->dest_path, 1) != 0) {
            ctx->open_error = BP_ERR_IO;
            return NULL;
        }
        ctx->target_ok = 1;
        if (e->flags & BP_ENTRY_FLAG_MODE_VALID) ctx->partial = 1;
        return NULL;
    }
    if (e->entry_type == BP_TYPE_SYMLINK) {
        if (matches) { ctx->found = 1; ctx->partial = 1; }
        return &ctx->discard_io;
    }
    if (e->entry_type != BP_TYPE_DATA) return &ctx->discard_io;
    if (!matches) return &ctx->discard_io;

    ctx->found = 1;
    ctx->target_ok = 0;
    if (wcx_ensure_directories(ctx->dest_path, 0) != 0) {
        ctx->open_error = BP_ERR_IO;
        return NULL;
    }
    {
        wcx_staged_file_t *stage = (wcx_staged_file_t *)calloc(1, sizeof(*stage));
        if (stage == NULL) { ctx->open_error = BP_ERR_NOMEM; return NULL; }
        safe_strcpy(stage->target, sizeof(stage->target), ctx->dest_path);
        if (wcx_open_sibling_stage(ctx->dest_path, stage->temporary,
                                   sizeof(stage->temporary), &fp) != 0) {
            free(stage);
            ctx->open_error = BP_ERR_IO;
            return NULL;
        }
        bp_io_from_file(&stage->io, fp);
        return &stage->io;
    }
}

/* ==========================================================================
 * Pack helpers (used by PackFiles)
 * ========================================================================== */

typedef struct {
    bp_uint8 data[BP_META_FAT_SIZE];
    unsigned pos;
} wcx_fat_source_t;

static long wcx_fat_read(void *handle, void *buffer, bp_uint32 count)
{
    wcx_fat_source_t *source = (wcx_fat_source_t *)handle;
    unsigned remaining = BP_META_FAT_SIZE - source->pos;
    if (count > remaining) count = remaining;
    memcpy(buffer, source->data + source->pos, count);
    source->pos += count;
    return (long)count;
}

static int wcx_add_fat_metadata(bp_archive_t *ar, const char *name,
                                DWORD attributes, bp_uint8 subject_type)
{
    wcx_fat_source_t source;
    bp_io_t io;
    int rc = bp_metadata_fat_encode(source.data, sizeof(source.data),
                                    subject_type, BP_META_FAT_MASK,
                                    (bp_uint8)(attributes & BP_META_FAT_MASK));
    if (rc != BP_OK) return rc;
    source.pos = 0;
    io.read = wcx_fat_read;
    io.write = NULL;
    io.seek = NULL;
    io.tell = NULL;
    io.handle = &source;
    return bp_archive_add_entry_ex(ar, name, BP_TYPE_META, BP_META_PORTABLE,
                                   &io, bp_u64_from_u32(BP_META_FAT_SIZE),
                                   bp_u64_from_u32(0), 0);
}

static int pack_add_file(bp_archive_t *ar, const char *src_path,
                         const char *arc_name)
{
    struct _stat64 st;
    DWORD attributes;
    bp_uint64 mtime;
    bp_uint16 flags;
    FILE *fp;
    bp_io_t fio;
    char dir_arc[WCX_MAX_PATH + 1];  /* +1 for trailing '/' appended below */
    char arc_name_utf8[WCX_MAX_PATH * 3 + 1];
    int rc;

    if (_stat64(src_path, &st) != 0) return BP_ERR_IO;
    if (st.st_size < 0) return BP_ERR_OVERFLOW;
    attributes = GetFileAttributesA(src_path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return BP_ERR_IO;
    if (acp_to_utf8(arc_name, arc_name_utf8, sizeof(arc_name_utf8)) != 0)
        return BP_ERR_INVALID_ARG;
    if ((bp_off_t)st.st_mtime >= 0 &&
        (time_t)(bp_off_t)st.st_mtime == st.st_mtime) {
        mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
        flags = BP_ENTRY_FLAG_MTIME_VALID;
    } else {
        mtime = bp_u64_from_u32(0);
        flags = 0;
    }

    if (S_ISDIR(st.st_mode)) {
        /* Ensure the archive directory name ends with '/'. */
        if (!ends_with_sep(arc_name)) {
            int n = snprintf(dir_arc, sizeof(dir_arc), "%s/", arc_name);
            if (n < 0 || (size_t)n >= sizeof(dir_arc)) return BP_ERR_NAME_TOO_LONG;
        } else {
            if (strlen(arc_name) >= sizeof(dir_arc)) return BP_ERR_NAME_TOO_LONG;
            strncpy(dir_arc, arc_name, sizeof(dir_arc) - 1);
            dir_arc[sizeof(dir_arc) - 1] = '\0';
        }

        if (acp_to_utf8(dir_arc, arc_name_utf8, sizeof(arc_name_utf8)) != 0)
            return BP_ERR_INVALID_ARG;
        rc = bp_archive_add_entry_ex(ar, arc_name_utf8, BP_TYPE_DIR,
                                     BP_OS_WINDOWS, NULL, bp_u64_from_u32(0),
                                     mtime, flags);
        if (rc == BP_OK)
            rc = wcx_add_fat_metadata(ar, arc_name_utf8, attributes, BP_TYPE_DIR);
        if (rc == BP_OK && flags == 0) rc = BP_ERR_PARTIAL;
        /* Do NOT recurse into subdirectories here. Total Commander's
         * AddList already enumerates every file and directory individually,
         * so recursing would produce duplicate entries in the archive. */
        return rc;
    }

    /* Regular file. */
    fp = fopen(src_path, "rb");
    if (fp == NULL) return BP_ERR_IO;

    bp_io_from_file(&fio, fp);

    rc = bp_archive_add_entry_ex(ar, arc_name_utf8, BP_TYPE_DATA,
                                 BP_OS_WINDOWS, &fio,
                                 bp_off_to_u64((bp_off_t)st.st_size),
                                 mtime, flags);
    if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (rc == BP_OK)
        rc = wcx_add_fat_metadata(ar, arc_name_utf8, attributes, BP_TYPE_DATA);
    if (rc == BP_OK && flags == 0) rc = BP_ERR_PARTIAL;
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
        int list_error = arc->list_error;
        free(arc->entries);
        fclose(arc->fp);
        free(arc);
        if (list_error == BP_ERR_NOMEM) data->OpenResult = E_NO_MEMORY;
        else if (list_error == BP_ERR_PARTIAL || list_error == BP_ERR_OVERFLOW)
            data->OpenResult = E_ECREATE;
        else data->OpenResult = (rc == BP_ERR_MAGIC) ? E_UNKNOWN_FORMAT
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
    if (e->comp_size.hi != 0 || e->orig_size.hi != 0 ||
        e->comp_size.lo > INT_MAX || e->orig_size.lo > INT_MAX)
        return E_BAD_ARCHIVE;
    arc->selected = arc->current;
    arc->current++;

    memset(hd, 0, sizeof(*hd));
    safe_strcpy(hd->ArcName, sizeof(hd->ArcName), arc->arc_name);
    safe_strcpy(hd->FileName, sizeof(hd->FileName), e->name);

    hd->Flags    = 0;
    hd->PackSize = (int)e->comp_size.lo;
    hd->UnpSize  = (int)e->orig_size.lo;
    hd->HostOS   = 0; /* 0 = DOS/Windows in WCX convention */
    hd->FileCRC  = 0; /* WCX FileCRC field is not exposed via the list API */
    hd->FileTime = unix_to_dostime(e->mtime,
                    (e->flags & BP_ENTRY_FLAG_MTIME_VALID) != 0);
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
    hd->PackSize     = e->comp_size.lo;
    hd->PackSizeHigh = e->comp_size.hi;
    hd->UnpSize      = e->orig_size.lo;
    hd->UnpSizeHigh  = e->orig_size.hi;
    hd->HostOS       = 0;
    hd->FileCRC      = 0;
    hd->FileTime     = unix_to_dostime(e->mtime,
                       (e->flags & BP_ENTRY_FLAG_MTIME_VALID) != 0);
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
    char full_dest[WCX_MAX_PATH];
    DWORD full_length;
    int rc;

    if (arc->selected < 0 || arc->selected >= arc->entry_count) {
        return E_EABORTED;
    }

    e = &arc->entries[arc->selected];
    arc->selected = -1;

    if (Operation == PK_SKIP) return E_SUCCESS;

    if (Operation == PK_TEST) {
        /* The core test verifies checksums itself; never report a successful
         * test without reading and validating the archive payloads. */
        rc = bp_archive_test(&arc->io, NULL, NULL);
        return (rc == BP_OK || rc == BP_ERR_PARTIAL) ? E_SUCCESS : E_BAD_DATA;
    }
    if (Operation != PK_EXTRACT) return E_EABORTED;

    /* PK_EXTRACT */

    /* Determine the destination filesystem path. */
    if (DestName != NULL && DestName[0] != '\0') {
        /* TC provides the full target path. */
        if (strlen(DestName) >= sizeof(dest)) return E_ECREATE;
        strncpy(dest, DestName, WCX_MAX_PATH - 1);
        dest[WCX_MAX_PATH - 1] = '\0';
    } else if (DestPath != NULL && DestPath[0] != '\0') {
        /* TC provides the target directory; append the archive entry name. */
        if (strlen(DestPath) + strlen(e->name) + 2 >= sizeof(dest)) return E_ECREATE;
        path_join(dest, sizeof(dest), DestPath, e->name);
    } else {
        if (strlen(e->name) >= sizeof(dest)) return E_ECREATE;
        strncpy(dest, e->name, WCX_MAX_PATH - 1);
        dest[WCX_MAX_PATH - 1] = '\0';
    }
    full_length = GetFullPathNameA(dest, (DWORD)sizeof(full_dest), full_dest, NULL);
    if (full_length == 0 || full_length >= sizeof(full_dest)) return E_ECREATE;
    strcpy(dest, full_dest);

    memset(&ctx, 0, sizeof(ctx));
    ctx.target_fwd = e->name_fwd;
    ctx.dest_path  = dest;
    ctx.selected   = e;
    ctx.proc_cb    = arc->proc_cb;
    ctx.discard_io.read = NULL;
    ctx.discard_io.write = discard_write;
    ctx.discard_io.seek = NULL;
    ctx.discard_io.tell = NULL;
    ctx.discard_io.handle = &ctx;

    /* bp_archive_extract_all manages its own seeking (reads footer to
     * locate the file table, then reads entries sequentially from there).
     * Calling it once per requested file is safe with the kept file handle
     * and will naturally skip non-matching entries via the open callback. */
    rc = bp_archive_extract_all_ex(&arc->io, extract_open, extract_done,
                                   NULL, &ctx);

    if (rc == BP_ERR_ABORTED) return E_EABORTED;
    if (ctx.open_error != BP_OK)
        return ctx.open_error == BP_ERR_NOMEM ? E_NO_MEMORY : E_ECREATE;
    if (rc != BP_OK && rc != BP_ERR_PARTIAL) return E_BAD_DATA;
    if (!ctx.found)           return E_BAD_DATA;
    if (!ctx.target_ok)        return E_ECREATE;

    e->extracted = 1;
    safe_strcpy(e->output_path, sizeof(e->output_path), dest);

    if (arc->proc_cb && !e->is_dir) {
        arc->proc_cb(dest, (int)e->orig_size.lo);
    }

    if (ctx.partial || (e->portable_meta_present && !ctx.target_metadata_seen))
        return E_ECREATE;
    return E_SUCCESS;
}

static int wcx_directory_order(const void *a, const void *b)
{
    const wcx_entry_t *left = (const wcx_entry_t *)a;
    const wcx_entry_t *right = (const wcx_entry_t *)b;
    int left_dir = left->is_dir && left->extracted;
    int right_dir = right->is_dir && right->extracted;
    size_t left_len, right_len;
    if (left_dir != right_dir) return right_dir - left_dir;
    left_len = strlen(left->output_path);
    right_len = strlen(right->output_path);
    if (left_len < right_len) return 1;
    if (left_len > right_len) return -1;
    return 0;
}

__declspec(dllexport)
int WINAPI CloseArchive(HANDLE hArcData)
{
    wcx_arc_t *arc = (wcx_arc_t *)hArcData;
    int result = E_SUCCESS;
    int i;
    if (arc == NULL) return E_SUCCESS;
    if (arc->entries != NULL) {
        qsort(arc->entries, (size_t)arc->entry_count, sizeof(wcx_entry_t),
              wcx_directory_order);
        for (i = 0; i < arc->entry_count; i++) {
            wcx_entry_t *entry = &arc->entries[i];
            if (!entry->is_dir || !entry->extracted) continue;
            if ((entry->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
                wcx_set_time(entry->output_path, entry->mtime) != 0)
                result = E_ECREATE;
            if (entry->fat_valid && wcx_apply_fat(entry->output_path,
                         entry->fat_valid, entry->fat_value) != 0)
                result = E_ECREATE;
        }
    }
    if (arc->fp) fclose(arc->fp);
    free(arc->entries);
    free(arc);
    return result;
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
    FILE *fp = NULL;
    FILE *source_fp = NULL;
    bp_io_t aio;
    bp_archive_t ar;
    char *name;
    char src[WCX_MAX_PATH];
    char arc_name[WCX_MAX_PATH];
    char target[WCX_MAX_PATH];
    char temporary[WCX_MAX_PATH] = "";
    const char *base;
    char *sep;
    char *fsep;
    DWORD target_length;
    DWORD target_attr;
    int rc;
    int appending = 0;
    int method;
    int partial = 0;
    int archive_open = 0;
    int stage_exists = 0;
    int result = E_ECREATE;

    rc = bp_compress_init();
    if (rc != BP_OK) return E_BAD_DATA;

    /* Get compression method from registry (defaults to boltlz) */
    method = get_compression_method();

    target_length = GetFullPathNameA(PackedFile, (DWORD)sizeof(target),
                                     target, NULL);
    if (target_length == 0 || target_length >= sizeof(target))
        return E_ECREATE;
    target_attr = GetFileAttributesA(target);
    if (target_attr != INVALID_FILE_ATTRIBUTES) {
        if (target_attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            return E_ECREATE;
        appending = 1;
    }

    /* Build the complete archive away from the visible path. This leaves an
     * existing archive intact if adding an entry or finalizing the new table
     * fails, and lets publication replace the old archive atomically. */
    if (wcx_open_sibling_stage(target, temporary, sizeof(temporary), &fp) != 0)
        return E_ECREATE;
    stage_exists = 1;

    if (appending) {
        source_fp = fopen(target, "rb");
        if (source_fp == NULL || wcx_copy_stream(source_fp, fp) != 0 ||
            wcx_flush_stage(fp) != 0) {
            result = E_ECOPY;
            goto pack_cleanup;
        }
        if (fclose(source_fp) != 0) {
            source_fp = NULL;
            result = E_ECOPY;
            goto pack_cleanup;
        }
        source_fp = NULL;
        if (fclose(fp) != 0) {
            fp = NULL;
            result = E_ECOPY;
            goto pack_cleanup;
        }
        fp = fopen(temporary, "r+b");
        if (fp == NULL) {
            result = E_EOPEN;
            goto pack_cleanup;
        }
    }

    bp_io_from_file(&aio, fp);
    if (appending)
        rc = bp_archive_open_append(&ar, &aio, BP_BLOCK_DEFAULT, method);
    else
        rc = bp_archive_create(&ar, &aio, BP_BLOCK_DEFAULT, method);
    if (rc != BP_OK) {
        result = E_BAD_DATA;
        goto pack_cleanup;
    }
    archive_open = 1;

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
        if (rc == BP_ERR_PARTIAL) {
            partial = 1;
            continue;
        }
        if (rc != BP_OK) {
            result = E_ECOPY;
            goto pack_cleanup;
        }
    }

    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    archive_open = 0;

    if (rc != BP_OK) {
        result = E_BAD_DATA;
        goto pack_cleanup;
    }
    if (wcx_flush_stage(fp) != 0) {
        result = E_EWRITE;
        goto pack_cleanup;
    }
    if (fclose(fp) != 0) {
        fp = NULL;
        result = E_ECLOSE;
        goto pack_cleanup;
    }
    fp = NULL;

    if (!MoveFileExA(temporary, target,
                     (appending ? MOVEFILE_REPLACE_EXISTING : 0) |
                     MOVEFILE_WRITE_THROUGH)) {
        result = E_ECOPY;
        goto pack_cleanup;
    }
    stage_exists = 0;

    return partial ? E_ECOPY : E_SUCCESS;

pack_cleanup:
    if (source_fp != NULL) fclose(source_fp);
    if (archive_open) bp_archive_close(&ar);
    if (fp != NULL) fclose(fp);
    if (stage_exists) DeleteFileA(temporary);
    return result;
}

__declspec(dllexport)
int WINAPI DeleteFiles(char *PackedFile, char *DeleteList)
{
    FILE *src_fp = NULL, *dst_fp = NULL;
    bp_io_t src_io, dst_io;
    char tmp_name[WCX_MAX_PATH];
    char target[WCX_MAX_PATH];
    DWORD target_length;
    DWORD target_attr;
    int rc;
    bp_uint32 deleted = 0;
    int stage_exists = 0;

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

        target_length = GetFullPathNameA(PackedFile, (DWORD)sizeof(target),
                                         target, NULL);
        if (target_length == 0 || target_length >= sizeof(target)) {
            for (i = 0; i < count; i++) free((void *)names[i]);
            free(names);
            return E_EOPEN;
        }
        target_attr = GetFileAttributesA(target);
        if (target_attr == INVALID_FILE_ATTRIBUTES ||
            (target_attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            for (i = 0; i < count; i++) free((void *)names[i]);
            free(names);
            return E_EOPEN;
        }
        src_fp = fopen(target, "rb");
        if (src_fp == NULL) {
            for (i = 0; i < count; i++) free((void *)names[i]);
            free(names);
            return E_EOPEN;
        }

        if (wcx_open_sibling_stage(target, tmp_name, sizeof(tmp_name), &dst_fp) != 0) {
            fclose(src_fp);
            for (i = 0; i < count; i++) free((void *)names[i]);
            free(names);
            return E_ECREATE;
        }
        stage_exists = 1;

        bp_io_from_file(&src_io, src_fp);
        bp_io_from_file(&dst_io, dst_fp);

        rc = bp_archive_delete(&src_io, &dst_io, names, count, &deleted);

        if (fclose(src_fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
        src_fp = NULL;
        if (rc == BP_OK && deleted != 0 && wcx_flush_stage(dst_fp) != 0)
            rc = BP_ERR_IO;
        if (fclose(dst_fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
        dst_fp = NULL;

        for (i = 0; i < count; i++) free((void *)names[i]);
        free(names);
    }

    if (rc != BP_OK || deleted == 0) {
        if (stage_exists) DeleteFileA(tmp_name);
        return (rc != BP_OK) ? E_BAD_DATA : E_SUCCESS;
    }

    if (!MoveFileExA(tmp_name, target,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp_name);
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
