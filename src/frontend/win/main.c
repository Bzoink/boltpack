/*
 * main.c -- Windows CLI frontend for BoltPack
 *
 * ANSI C (C89/C90) with MinGW POSIX extensions.
 * Primary target: MinGW cross-compiler (i686-w64-mingw32-gcc, x86_64-w64-mingw32-gcc).
 * For other DOS compilers (Turbo C, Watcom), minor adjustments
 * may be needed: mkdir() signature, header differences, etc.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_metadata.h"
#include "../../core/bp_codepage.h"
#include "../cpm86/cpm86_path.h"
#include "../atari/atari_path.h"
#include "../x68k/x68k_path_index.h"
#ifdef BOLTPACK_ZSTD
#include "../../core/zstd_wrap.h"
#endif

#ifdef BOLTPACK_FAST_FLUFF
#include "../../core/fluff_fast.h"
#include <omp.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <wchar.h>
#include <utime.h>

#ifdef BOLTPACK_NET
#include "../common/bp_net_cli.h"
#endif

/* ---- Windows path limit ---------------------------------------------------- */

#define BP_MAX_PATH 1024
#define BP_MAX_PATH_WIDE 260

/* ---- codepage conversion helpers --------------------------------------- */

/* Filesystem paths are UTF-8 inside this frontend and UTF-16 at Win32 API
 * boundaries. Keep the legacy ACP out of path conversion on NT systems. */
static int win_utf8_to_wide(const char *utf8, wchar_t *wide, int capacity)
{
    int length, bytes;
    char *roundtrip;
    if (utf8 == NULL || wide == NULL || capacity <= 0) return -1;
    length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, capacity);
    if (length <= 0) return -1;
    bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (bytes <= 0) return -1;
    roundtrip = (char *)malloc((size_t)bytes);
    if (roundtrip == NULL) return -1;
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, roundtrip, bytes,
                            NULL, NULL) != bytes || strcmp(roundtrip, utf8) != 0) {
        free(roundtrip);
        return -1;
    }
    free(roundtrip);
    return length;
}

static int win_wide_is_valid_utf16(const wchar_t *wide)
{
    const wchar_t *p;
    if (wide == NULL) return 0;
    for (p = wide; *p != L'\0'; p++) {
        if (*p >= 0xD800 && *p <= 0xDBFF) {
            if (p[1] < 0xDC00 || p[1] > 0xDFFF) return 0;
            p++;
        } else if (*p >= 0xDC00 && *p <= 0xDFFF) {
            return 0;
        }
    }
    return 1;
}

static int win_wide_to_utf8(const wchar_t *wide, char *utf8, int capacity)
{
    int length;
    if (wide == NULL || utf8 == NULL || capacity <= 0 ||
        !win_wide_is_valid_utf16(wide)) return -1;
    length = WideCharToMultiByte(CP_UTF8, 0, wide, -1,
                                 utf8, capacity, NULL, NULL);
    return length > 0 ? length : -1;
}

static int win_utf8_copy(const char *source, char *destination, int capacity)
{
    wchar_t check[BP_MAX_PATH_WIDE];
    size_t length;
    if (win_utf8_to_wide(source, check, BP_MAX_PATH_WIDE) < 0) return -1;
    length = strlen(source) + 1;
    if (length > (size_t)capacity) return -1;
    memcpy(destination, source, length);
    return 0;
}

static FILE *win_fopen(const char *path, const char *mode)
{
    wchar_t wide_path[BP_MAX_PATH_WIDE], wide_mode[8];
    int i;
    if (win_utf8_to_wide(path, wide_path, BP_MAX_PATH_WIDE) < 0) return NULL;
    for (i = 0; mode[i] != '\0' && i < 7; i++) wide_mode[i] = (wchar_t)(unsigned char)mode[i];
    if (mode[i] != '\0') return NULL;
    wide_mode[i] = L'\0';
    return _wfopen(wide_path, wide_mode);
}

static int win_stat64(const char *path, struct _stat64 *st)
{
    wchar_t wide[BP_MAX_PATH_WIDE];
    if (win_utf8_to_wide(path, wide, BP_MAX_PATH_WIDE) < 0) return -1;
    return _wstat64(wide, st);
}

static int win_mkdir(const char *path)
{
    wchar_t wide[BP_MAX_PATH_WIDE];
    if (win_utf8_to_wide(path, wide, BP_MAX_PATH_WIDE) < 0) return -1;
    return _wmkdir(wide);
}

static int win_utime(const char *path, struct _utimbuf *times)
{
    wchar_t wide[BP_MAX_PATH_WIDE];
    if (win_utf8_to_wide(path, wide, BP_MAX_PATH_WIDE) < 0) return -1;
    return _wutime(wide, times);
}

typedef struct {
    char *d_name;
} win_dirent;

typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAW data;
    int first;
    int error;
    win_dirent entry;
    char name[BP_MAX_PATH * 3];
} win_DIR;

static win_DIR *win_opendir(const char *path)
{
    win_DIR *directory;
    wchar_t wide[BP_MAX_PATH_WIDE], pattern[BP_MAX_PATH_WIDE];
    size_t length;
    if (win_utf8_to_wide(path, wide, BP_MAX_PATH_WIDE) < 0) return NULL;
    length = wcslen(wide);
    if (length + 3 > BP_MAX_PATH_WIDE) return NULL;
    wcscpy(pattern, wide);
    if (length > 0 && pattern[length - 1] != L'/' && pattern[length - 1] != L'\\')
        pattern[length++] = L'\\';
    pattern[length++] = L'*';
    pattern[length] = L'\0';
    directory = (win_DIR *)calloc(1, sizeof(*directory));
    if (directory == NULL) return NULL;
    directory->handle = FindFirstFileW(pattern, &directory->data);
    if (directory->handle == INVALID_HANDLE_VALUE) {
        free(directory);
        return NULL;
    }
    directory->first = 1;
    return directory;
}

static win_dirent *win_readdir(win_DIR *directory)
{
    if (directory == NULL) return NULL;
    if (!directory->first && !FindNextFileW(directory->handle, &directory->data)) {
        if (GetLastError() != ERROR_NO_MORE_FILES) directory->error = 1;
        return NULL;
    }
    directory->first = 0;
    if (win_wide_to_utf8(directory->data.cFileName, directory->name,
                         sizeof(directory->name)) < 0) {
        directory->error = 1;
        return NULL;
    }
    directory->entry.d_name = directory->name;
    return &directory->entry;
}

static int win_closedir(win_DIR *directory)
{
    int result;
    if (directory == NULL) return -1;
    result = FindClose(directory->handle) ? 0 : -1;
    free(directory);
    return result;
}

static DWORD win_GetFileAttributesA(const char *path)
{
    wchar_t wide[BP_MAX_PATH_WIDE];
    if (win_utf8_to_wide(path, wide, BP_MAX_PATH_WIDE) < 0) return INVALID_FILE_ATTRIBUTES;
    return GetFileAttributesW(wide);
}

static BOOL win_SetFileAttributesA(const char *path, DWORD attributes)
{
    wchar_t wide[BP_MAX_PATH_WIDE];
    if (win_utf8_to_wide(path, wide, BP_MAX_PATH_WIDE) < 0) return FALSE;
    return SetFileAttributesW(wide, attributes);
}

static HANDLE win_CreateFileA(const char *path, DWORD access, DWORD share,
                              LPSECURITY_ATTRIBUTES security, DWORD creation,
                              DWORD flags, HANDLE template_file)
{
    wchar_t wide[BP_MAX_PATH_WIDE];
    if (win_utf8_to_wide(path, wide, BP_MAX_PATH_WIDE) < 0) return INVALID_HANDLE_VALUE;
    return CreateFileW(wide, access, share, security, creation, flags,
                       template_file);
}

static BOOL win_DeleteFileA(const char *path)
{
    wchar_t wide[BP_MAX_PATH_WIDE];
    if (win_utf8_to_wide(path, wide, BP_MAX_PATH_WIDE) < 0) return FALSE;
    return DeleteFileW(wide);
}

static BOOL win_MoveFileExA(const char *source, const char *destination, DWORD flags)
{
    wchar_t wide_source[BP_MAX_PATH_WIDE], wide_destination[BP_MAX_PATH_WIDE];
    if (win_utf8_to_wide(source, wide_source, BP_MAX_PATH_WIDE) < 0 ||
        win_utf8_to_wide(destination, wide_destination, BP_MAX_PATH_WIDE) < 0)
        return FALSE;
    return MoveFileExW(wide_source, wide_destination, flags);
}

static BOOL win_ReplaceFileA(const char *target, const char *replacement,
                             const char *backup, DWORD flags,
                             LPVOID exclude, LPVOID reserved)
{
    wchar_t wide_target[BP_MAX_PATH_WIDE], wide_replacement[BP_MAX_PATH_WIDE];
    wchar_t wide_backup[BP_MAX_PATH_WIDE];
    if (win_utf8_to_wide(target, wide_target, BP_MAX_PATH_WIDE) < 0 ||
        win_utf8_to_wide(replacement, wide_replacement, BP_MAX_PATH_WIDE) < 0 ||
        win_utf8_to_wide(backup, wide_backup, BP_MAX_PATH_WIDE) < 0) return FALSE;
    return ReplaceFileW(wide_target, wide_replacement, wide_backup, flags,
                        exclude, reserved);
}

static DWORD win_GetTempPathA(DWORD capacity, char *path)
{
    wchar_t wide[BP_MAX_PATH_WIDE];
    DWORD length = GetTempPathW(BP_MAX_PATH_WIDE, wide);
    if (length == 0 || length >= BP_MAX_PATH_WIDE ||
        win_wide_to_utf8(wide, path, (int)capacity) < 0) return 0;
    return (DWORD)strlen(path);
}

static UINT win_GetTempFileNameA(const char *directory, const char *prefix,
                                 UINT unique, char *path)
{
    wchar_t wide_directory[BP_MAX_PATH_WIDE], wide_prefix[4], wide_path[BP_MAX_PATH_WIDE];
    UINT result;
    int i;
    if (win_utf8_to_wide(directory, wide_directory, BP_MAX_PATH_WIDE) < 0) return 0;
    for (i = 0; prefix[i] != '\0' && i < 3; i++) wide_prefix[i] = (wchar_t)(unsigned char)prefix[i];
    if (prefix[i] != '\0') return 0;
    wide_prefix[i] = L'\0';
    result = GetTempFileNameW(wide_directory, wide_prefix, unique, wide_path);
    if (result == 0 || win_wide_to_utf8(wide_path, path, BP_MAX_PATH) < 0) return 0;
    return result;
}

static BOOL win_GetDiskFreeSpaceExA(const char *path, PULARGE_INTEGER available,
                                    PULARGE_INTEGER total,
                                    PULARGE_INTEGER free_bytes)
{
    wchar_t wide[BP_MAX_PATH_WIDE];
    if (win_utf8_to_wide(path, wide, BP_MAX_PATH_WIDE) < 0) return FALSE;
    return GetDiskFreeSpaceExW(wide, available, total, free_bytes);
}

static int win_make_utf8_argv(int *argc, char ***argv)
{
    int wide_argc, i;
    LPWSTR *wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    char **utf8_argv;
    if (wide_argv == NULL || wide_argc < 0) return -1;
    utf8_argv = (char **)calloc((size_t)wide_argc + 1U, sizeof(*utf8_argv));
    if (utf8_argv == NULL) { LocalFree(wide_argv); return -1; }
    for (i = 0; i < wide_argc; i++) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                        wide_argv[i], -1, NULL, 0, NULL, NULL);
        if (bytes <= 0) break;
        utf8_argv[i] = (char *)malloc((size_t)bytes);
        if (utf8_argv[i] == NULL ||
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1,
                                utf8_argv[i], bytes, NULL, NULL) != bytes) break;
    }
    LocalFree(wide_argv);
    if (i != wide_argc) {
        while (i >= 0) free(utf8_argv[i--]);
        free(utf8_argv);
        return -1;
    }
    *argc = wide_argc;
    *argv = utf8_argv;
    return 0;
}

#define fopen win_fopen
#define _stat64(path, st) win_stat64((path), (st))
#define _mkdir win_mkdir
#define utime win_utime
#define opendir win_opendir
#define readdir win_readdir
#define closedir win_closedir
#define GetFileAttributesA win_GetFileAttributesA
#define SetFileAttributesA win_SetFileAttributesA
#define CreateFileA win_CreateFileA
#define DeleteFileA win_DeleteFileA
#define MoveFileExA win_MoveFileExA
#define ReplaceFileA win_ReplaceFileA
#define GetTempPathA win_GetTempPathA
#define GetTempFileNameA win_GetTempFileNameA
#define GetDiskFreeSpaceExA win_GetDiskFreeSpaceExA

typedef struct {
    bp_uint8 data[BP_META_FAT_SIZE];
    unsigned pos;
} win_fat_source_t;

static int g_target_classic_hfs;
static int g_target_amiga_68k;
static int g_target_cpm86;
static int g_target_atari_st;
static int g_target_ql;
static int g_target_riscos;
static int g_target_x68000;

static long win_fat_read(void *handle, void *buffer, bp_uint32 count)
{
    win_fat_source_t *source = (win_fat_source_t *)handle;
    unsigned remaining = BP_META_FAT_SIZE - source->pos;
    if (count > remaining) count = remaining;
    memcpy(buffer, source->data + source->pos, count);
    source->pos += count;
    return (long)count;
}

static int add_win_fat_metadata_value(bp_archive_t *ar, const char *name,
                                      bp_uint8 subject_type,
                                      DWORD attributes)
{
    win_fat_source_t source;
    bp_io_t io;
    bp_uint8 value;
    int rc;
    if (g_target_classic_hfs || g_target_amiga_68k || g_target_cpm86 ||
        g_target_ql || g_target_riscos || g_target_x68000)
        return BP_OK;
    if (attributes == INVALID_FILE_ATTRIBUTES) return BP_ERR_IO;
    value = (bp_uint8)(attributes & BP_META_FAT_MASK);
    rc = bp_metadata_fat_encode(source.data, sizeof(source.data),
                                subject_type, BP_META_FAT_MASK, value);
    if (rc != BP_OK) return rc;
    source.pos = 0;
    io.read = win_fat_read; io.write = NULL; io.seek = NULL; io.tell = NULL;
    io.handle = &source;
    return bp_archive_add_entry_ex(ar, name, BP_TYPE_META, BP_META_PORTABLE,
                                   &io, bp_u64_from_u32(BP_META_FAT_SIZE),
                                   bp_u64_from_u32(0), 0);
}

static int add_win_fat_metadata(bp_archive_t *ar, const char *name,
                                const char *path, bp_uint8 subject_type)
{
    DWORD attributes = GetFileAttributesA(path);
    return add_win_fat_metadata_value(ar, name, subject_type, attributes);
}

/* ---- verbosity --------------------------------------------------------- */

#define VERB_DEFAULT 0
#define VERB_QUIET   1
#define VERB_SILENT  2

typedef enum {
    FAST_THREAD_AUTO = 0,
    FAST_THREAD_SINGLE = 1,
    FAST_THREAD_EXPLICIT = 2
} fast_thread_policy_t;

#define FAST_FLUFF_AUTO_MIN_BYTES (4UL * 1024UL * 1024UL)
#define FAST_BOLTLZ_AUTO_MIN_BYTES (1UL * 1024UL * 1024UL)
#define FAST_COMPRESSION_MAX_THREADS 64

static int g_verbosity = VERB_DEFAULT;
static fast_thread_policy_t g_fast_thread_policy = FAST_THREAD_AUTO;
static int g_fast_threads = 0; /* explicit thread count for -j/--threads */
static int g_fast_group_blocks = 0; /* 0 = codec default; >0 = grouped reset span */

/*
 * Path of the in-progress temp archive written by cmd_create.
 * Cleared on success (after MoveFileEx).  The console ctrl handler
 * deletes it so Ctrl+C does not leave a partial .blt file on disk.
 */
static char g_tmp_archive[BP_MAX_PATH * 2] = "";

static void cleanup_tmp_archive_win(void)
{
    if (g_tmp_archive[0] != '\0') {
        DeleteFileA(g_tmp_archive);
        g_tmp_archive[0] = '\0';
    }
}

static BOOL WINAPI ctrl_cleanup_handler(DWORD ctrl_type)
{
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_CLOSE_EVENT ||
        ctrl_type == CTRL_BREAK_EVENT) {
        cleanup_tmp_archive_win();
    }
    return FALSE; /* let default handler terminate the process */
}

/*
 * Directory of the output archive, used as the fallback location for
 * temp files when %TEMP% does not have enough space.  Set by
 * set_tmp_dir_from_archive() early in cmd_create / cmd_add.
 */
static char g_tmp_dir[BP_MAX_PATH] = "";

/*
 * Derive the archive directory from archive_name and store it in
 * g_tmp_dir.  On Windows paths may use / or \ as separators.
 */
static void set_tmp_dir_from_archive(const char *archive_name)
{
    const char *slash;
    const char *bslash;
    const char *last;
    size_t dlen;

    g_tmp_dir[0] = '\0';
    if (archive_name == NULL) return;

    slash  = strrchr(archive_name, '/');
    bslash = strrchr(archive_name, '\\');
    last = slash;
    if (bslash != NULL && (last == NULL || bslash > last))
        last = bslash;

    if (last == archive_name) {
        g_tmp_dir[0] = *last;
        g_tmp_dir[1] = '\0';
    } else if (last != NULL) {
        dlen = (size_t)(last - archive_name);
        if (dlen == 2 && archive_name[1] == ':' &&
            (archive_name[2] == '/' || archive_name[2] == '\\'))
            dlen = 3;
        if (dlen < sizeof(g_tmp_dir)) {
            memcpy(g_tmp_dir, archive_name, dlen);
            g_tmp_dir[dlen] = '\0';
        }
    } else if (archive_name[0] != '\0' && archive_name[1] == ':') {
        g_tmp_dir[0] = archive_name[0];
        g_tmp_dir[1] = ':';
        g_tmp_dir[2] = '\0';
    } else {
        g_tmp_dir[0] = '.';
        g_tmp_dir[1] = '\0';
    }
}

static int win_archive_object_id_equal(
    const BY_HANDLE_FILE_INFORMATION *left,
    const BY_HANDLE_FILE_INFORMATION *right)
{
    return left->dwVolumeSerialNumber == right->dwVolumeSerialNumber &&
           left->nFileIndexHigh == right->nFileIndexHigh &&
           left->nFileIndexLow == right->nFileIndexLow;
}

static int win_archive_info_equal(const BY_HANDLE_FILE_INFORMATION *left,
                                  const BY_HANDLE_FILE_INFORMATION *right)
{
    return win_archive_object_id_equal(left, right) &&
           left->nFileSizeHigh == right->nFileSizeHigh &&
           left->nFileSizeLow == right->nFileSizeLow &&
           left->ftLastWriteTime.dwHighDateTime ==
               right->ftLastWriteTime.dwHighDateTime &&
           left->ftLastWriteTime.dwLowDateTime ==
               right->ftLastWriteTime.dwLowDateTime;
}

static int win_open_source_info(const char *path,
                                BY_HANDLE_FILE_INFORMATION *info_out,
                                FILE_BASIC_INFO *basic_out,
                                int expect_directory)
{
    HANDLE handle;
    int ok;

    handle = CreateFileA(path, FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
                         NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    ok = GetFileInformationByHandle(handle, info_out) != 0 &&
         GetFileInformationByHandleEx(handle, FileBasicInfo, basic_out,
                                       sizeof(*basic_out)) != 0 &&
         (info_out->dwFileAttributes & FILE_ATTRIBUTE_DEVICE) == 0 &&
         (((info_out->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) ==
          (expect_directory != 0));
    CloseHandle(handle);
    return ok;
}

static int win_source_snapshot_equal(const FILE_BASIC_INFO *before,
                                     const FILE_BASIC_INFO *after);

static int win_path_matches_snapshot(
    const char *path, const BY_HANDLE_FILE_INFORMATION *expected_info,
    const FILE_BASIC_INFO *expected_basic, int expect_directory)
{
    BY_HANDLE_FILE_INFORMATION current_info;
    FILE_BASIC_INFO current_basic;

    return win_open_source_info(path, &current_info, &current_basic,
                                expect_directory) &&
           win_archive_info_equal(expected_info, &current_info) &&
           win_source_snapshot_equal(expected_basic, &current_basic);
}

static int win_filetime_to_archive_time(FILETIME source, bp_uint64 *mtime)
{
    ULARGE_INTEGER ticks;
    ULONGLONG unix_epoch_ticks;
    ULONGLONG seconds;

    ticks.LowPart = source.dwLowDateTime;
    ticks.HighPart = source.dwHighDateTime;
    unix_epoch_ticks = (ULONGLONG)11644473600ULL * 10000000ULL;
    *mtime = bp_u64_from_u32(0);
    if (ticks.QuadPart < unix_epoch_ticks) return 0;
    seconds = (ticks.QuadPart - unix_epoch_ticks) / 10000000ULL;
    mtime->lo = (bp_uint32)seconds;
    mtime->hi = (bp_uint32)(seconds >> 32);
    return 1;
}

static int win_open_archive_snapshot(const char *path, HANDLE *handle_out,
                                     BY_HANDLE_FILE_INFORMATION *info_out)
{
    HANDLE handle;

    handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT |
                         FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileInformationByHandle(handle, info_out) ||
        (info_out->dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT |
          FILE_ATTRIBUTE_READONLY)) != 0 ||
        info_out->nNumberOfLinks != 1) {
        CloseHandle(handle);
        return 0;
    }
    *handle_out = handle;
    return 1;
}

static int win_create_archive_stage(HANDLE source_handle, char *temporary,
                                    size_t temporary_size)
{
    HANDLE destination_handle;
    unsigned char buffer[65536];
    DWORD bytes_read;
    DWORD bytes_written;
    DWORD offset;
    LARGE_INTEGER zero;
    int n;

    if (!GetTempFileNameA(g_tmp_dir[0] ? g_tmp_dir : ".", "bpa", 0,
                          temporary))
        return 0;
    if (strlen(temporary) >= temporary_size) {
        DeleteFileA(temporary);
        return 0;
    }
    memcpy(g_tmp_archive, temporary, strlen(temporary) + 1);
    if (!SetConsoleCtrlHandler(ctrl_cleanup_handler, TRUE)) return 0;

    if (source_handle != INVALID_HANDLE_VALUE) {
        zero.QuadPart = 0;
        if (!SetFilePointerEx(source_handle, zero, NULL, FILE_BEGIN)) return 0;
        destination_handle = CreateFileA(temporary, GENERIC_WRITE,
            FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (destination_handle == INVALID_HANDLE_VALUE) return 0;
        for (;;) {
            if (!ReadFile(source_handle, buffer, sizeof(buffer), &bytes_read,
                          NULL)) {
                CloseHandle(destination_handle);
                return 0;
            }
            if (bytes_read == 0) break;
            offset = 0;
            while (offset < bytes_read) {
                if (!WriteFile(destination_handle, buffer + offset,
                               bytes_read - offset, &bytes_written, NULL) ||
                    bytes_written == 0) {
                    CloseHandle(destination_handle);
                    return 0;
                }
                offset += bytes_written;
            }
        }
        if (!FlushFileBuffers(destination_handle)) {
            CloseHandle(destination_handle);
            return 0;
        }
        if (!CloseHandle(destination_handle))
            return 0;
    }

    n = (int)strlen(temporary);
    return n > 0 && (size_t)n < temporary_size;
}

static int win_publish_archive_stage(const char *target, const char *temporary,
                                     char *backup, size_t backup_size,
                                     DWORD original_attributes)
{
    size_t temporary_len;
    DWORD attribute_mask;
    DWORD expected_attributes;
    DWORD actual_attributes;
    DWORD publish_error;

    temporary_len = strlen(temporary);
    if (temporary_len < 4 || temporary_len + 1 > backup_size ||
        strcmp(temporary + temporary_len - 4, ".tmp") != 0) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    memcpy(backup, temporary, temporary_len + 1);
    memcpy(backup + temporary_len - 4, ".bak", 5);
    if (GetFileAttributesA(backup) != INVALID_FILE_ATTRIBUTES) {
        SetLastError(ERROR_FILE_EXISTS);
        return 0;
    }
    if (!ReplaceFileA(target, temporary, backup, 0, NULL, NULL)) {
        DWORD error = GetLastError();
        if (GetFileAttributesA(backup) != INVALID_FILE_ATTRIBUTES) {
            if (GetFileAttributesA(target) == INVALID_FILE_ATTRIBUTES &&
                MoveFileExA(backup, target, MOVEFILE_WRITE_THROUGH)) {
                /* The original is back at its path; stage cleanup is safe. */
            } else {
                fprintf(stderr,
                        "boltpack: replacement recovery file kept at: %s\n",
                        backup);
            }
        }
        SetLastError(error);
        return 0;
    }
    attribute_mask = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                     FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE |
                     FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE |
                     FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    expected_attributes = original_attributes & attribute_mask;
    if (expected_attributes == 0) expected_attributes = FILE_ATTRIBUTE_NORMAL;
    if (!SetFileAttributesA(target, expected_attributes)) {
        publish_error = GetLastError();
        if (!MoveFileExA(backup, target,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            fprintf(stderr,
                    "boltpack: archive attribute restore failed; original kept at: %s\n",
                    backup);
        } else {
            g_tmp_archive[0] = '\0';
        }
        SetLastError(publish_error);
        return 0;
    }
    actual_attributes = GetFileAttributesA(target);
    if (actual_attributes == INVALID_FILE_ATTRIBUTES ||
        (actual_attributes & attribute_mask) !=
            (original_attributes & attribute_mask)) {
        publish_error = actual_attributes == INVALID_FILE_ATTRIBUTES ?
                        GetLastError() : ERROR_INVALID_DATA;
        if (!MoveFileExA(backup, target,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            fprintf(stderr,
                    "boltpack: archive attribute verification failed; original kept at: %s\n",
                    backup);
        } else {
            g_tmp_archive[0] = '\0';
        }
        SetLastError(publish_error);
        return 0;
    }
    g_tmp_archive[0] = '\0';
    if (!DeleteFileA(backup)) {
        fprintf(stderr, "boltpack: old archive backup kept at: %s\n", backup);
    }
    return 1;
}

/* ---- bp_io_t adapters for FILE * --------------------------------------- */

static long file_read(void *handle, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)handle);
}

static long file_write(void *handle, const void *buf, bp_uint32 count)
{
    return (long)fwrite(buf, 1, (size_t)count, (FILE *)handle);
}

/* Core diagnostic callback: print archiver log messages to stderr. */
static void cli_log_stderr(const char *msg, void *user_data)
{
    (void)user_data;
    fprintf(stderr, "%s\n", msg);
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
    io->read  = file_read;
    io->write = file_write;
    io->seek  = file_seek;
    io->tell  = file_tell;
    io->handle = fp;
}

/* ---- error messages ---------------------------------------------------- */

static const char *bp_strerror(int code)
{
    switch (code) {
    case BP_OK:                    return "success";
    case BP_ERR_PARTIAL:           return "partial success (some entries or attributes were not restored)";
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

/* ---- speed formatting -------------------------------------------------- */

static void format_speed(double bytes_per_sec, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes_per_sec < 1024.0) {
        sprintf(buf, "%.0f bytes/s", bytes_per_sec);
    } else if (bytes_per_sec < 1048576.0) {
        sprintf(buf, "%.2f KB/s", bytes_per_sec / 1024.0);
    } else {
        sprintf(buf, "%.2f MB/s", bytes_per_sec / 1048576.0);
    }
}

static double wall_time_seconds(void)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER counter;

    if (QueryPerformanceFrequency(&freq) &&
        QueryPerformanceCounter(&counter) &&
        freq.QuadPart != 0) {
        return (double)counter.QuadPart / (double)freq.QuadPart;
    }
    return (double)GetTickCount() / 1000.0;
}

/* ---- size formatting --------------------------------------------------- */

static void format_size(bp_uint32 bytes, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes < 1024UL) {
        sprintf(buf, "%lu bytes", (unsigned long)bytes);
    } else if (bytes < 1048576UL) {
        sprintf(buf, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1073741824UL) {
        sprintf(buf, "%.1f MB", (double)bytes / 1048576.0);
    } else {
        sprintf(buf, "%.2f GB", (double)bytes / 1073741824.0);
    }
}

static void format_size64(bp_uint64 bytes, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes.hi == 0) {
        format_size(bytes.lo, buf, bufsize);
    } else {
        double d = (double)bytes.lo + (double)bytes.hi * 4294967296.0;
        if (d < 1048576.0) {
            sprintf(buf, "%.1f KB", d / 1024.0);
        } else if (d < 1073741824.0) {
            sprintf(buf, "%.1f MB", d / 1048576.0);
        } else {
            sprintf(buf, "%.2f GB", d / 1073741824.0);
        }
    }
}

/* ---- number formatting ------------------------------------------------- */

static void format_number(bp_uint32 n, char *buf, size_t bufsize)
{
    char tmp[32];
    int len, i, j, commas;

    sprintf(tmp, "%lu", (unsigned long)n);
    len = (int)strlen(tmp);
    commas = (len - 1) / 3;
    if ((size_t)(len + commas + 1) > bufsize) {
        strcpy(buf, tmp);
        return;
    }
    j = len + commas;
    buf[j] = '\0';
    for (i = len - 1; i >= 0; i--) {
        buf[--j] = tmp[i];
        if (i > 0 && (len - i) % 3 == 0) {
            buf[--j] = ',';
        }
    }
}

/* ---- progress callback ------------------------------------------------- */

static int progress_cb(const bp_progress_t *p, void *user_data)
{
    (void)p;
    (void)user_data;
    return 0;
}

/* ---- path utilities ---------------------------------------------------- */

/* return 1 if path ends with / or \ */
static int ends_with_sep(const char *s)
{
    size_t len = strlen(s);
    return len > 0 && (s[len - 1] == '/' || s[len - 1] == '\\');
}

/* concatenate dir + "/" + name into buf */
static void path_join(char *buf, size_t bufsize, const char *dir,
                      const char *name)
{
    (void)bufsize;
    if (ends_with_sep(dir)) {
        sprintf(buf, "%s%s", dir, name);
    } else {
        sprintf(buf, "%s/%s", dir, name);
    }
}

/* strip leading "./" or ".\" */
static const char *clean_path(const char *path)
{
    while (path[0] == '.' && (path[1] == '/' || path[1] == '\\')) {
        path += 2;
    }
    return path;
}

/* convert backslashes to forward slashes in-place (for archive names) */
static void normalize_separators(char *path)
{
    char *p;
    for (p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

/* ---- mkdir -p ---------------------------------------------------------- */

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#define bp_mkdir(path) _mkdir(path)
#else
#define bp_mkdir(path) mkdir(path, 0755)
#endif

static int mkdirs(const char *path)
{
    char tmp[BP_MAX_PATH];
    char *p;

    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);

    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            if (win_mkdir(tmp) != 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    if (win_mkdir(tmp) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* ensure parent directory of path exists */
static int mkdirs_for_file(const char *path)
{
    char tmp[BP_MAX_PATH];
    char *slash;
    char *bslash;

    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);

    /* find last separator (either / or \) */
    slash = strrchr(tmp, '/');
    bslash = strrchr(tmp, '\\');
    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }

    if (slash != NULL && slash != tmp) {
        *slash = '\0';
        if (mkdirs(tmp) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ---- parse block size -------------------------------------------------- */

static bp_uint32 parse_block_size(const char *s)
{
    if (strcmp(s, "8k") == 0 || strcmp(s, "8K") == 0)
        return BP_BLOCK_8K;
    if (strcmp(s, "16k") == 0 || strcmp(s, "16K") == 0)
        return BP_BLOCK_16K;
    if (strcmp(s, "32k") == 0 || strcmp(s, "32K") == 0)
        return BP_BLOCK_32K;
    if (strcmp(s, "64k") == 0 || strcmp(s, "64K") == 0)
        return BP_BLOCK_64K;
    if (strcmp(s, "256k") == 0 || strcmp(s, "256K") == 0)
        return BP_BLOCK_256K;
    return 0;
}

static char parse_command_name(const char *s)
{
    if (s == NULL || s[0] == '\0')
        return '\0';
    if (s[1] == '\0')
        return s[0];
    if (strcmp(s, "create") == 0)
        return 'c';
    if (strcmp(s, "add") == 0)
        return 'a';
    if (strcmp(s, "extract") == 0)
        return 'x';
    if (strcmp(s, "list") == 0)
        return 'l';
    if (strcmp(s, "test") == 0)
        return 't';
    if (strcmp(s, "info") == 0)
        return 'i';
    if (strcmp(s, "delete") == 0)
        return 'd';
    return '\0';
}

static int parse_thread_count_arg(const char *s)
{
    int n;

    n = atoi(s);
    if (n < 0) {
        fprintf(stderr, "boltpack: thread count must be >= 0\n");
        return 0;
    }
    if (n <= 1) {
        g_fast_thread_policy = FAST_THREAD_SINGLE;
        g_fast_threads = 0;
    } else {
        g_fast_thread_policy = FAST_THREAD_EXPLICIT;
        g_fast_threads = n;
    }
    return 1;
}

static char *dup_string(const char *s)
{
    char *copy;
    size_t len;

    len = strlen(s);
    copy = (char *)malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

/* ---- queued add helpers ------------------------------------------------ */

typedef struct {
    char     *path;
    char     *arcname;
    bp_uint8  entry_type;
    bp_uint64 file_size;
    bp_uint64 mtime;
    bp_uint16 mtime_flags;
    BY_HANDLE_FILE_INFORMATION source_info;
    FILE_BASIC_INFO source_basic_info;
    int      source_info_valid;
} add_job_t;

typedef struct {
    add_job_t *items;
    int        count;
    int        cap;
} add_job_list_t;

#ifdef BOLTPACK_FAST_FLUFF
typedef struct {
    FILE              *tmp_fp;
    bp_io_t            tmp_io;
    bp_packed_entry_t  packed;
    int                rc;
} packed_job_t;
#endif

static int collect_path_jobs(add_job_list_t *list, const char *path,
                             const char *arcname);

/* Format 6 stores nonnegative Unix seconds only. Keep negative or
 * unrepresentable source times explicitly unknown instead of wrapping them
 * through the legacy add-entry API's always-valid timestamp flag. */
static int win_archive_mtime(time_t source, bp_uint64 *mtime)
{
    bp_off_t seconds;
    *mtime = bp_u64_from_u32(0);
    if (source < (time_t)0) return 0;
    seconds = (bp_off_t)source;
    if (seconds < (bp_off_t)0 || (time_t)seconds != source) return 0;
    *mtime = bp_off_to_u64(seconds);
    return 1;
}

static void free_add_job(add_job_t *job)
{
    if (job == NULL) return;
    free(job->path);
    free(job->arcname);
    memset(job, 0, sizeof(*job));
}

static void free_job_list(add_job_list_t *list)
{
    int i;

    if (list == NULL) return;
    for (i = 0; i < list->count; i++) {
        free_add_job(&list->items[i]);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int append_job(add_job_list_t *list, const char *path,
                      const char *arcname, bp_uint8 entry_type,
                      bp_uint64 file_size, bp_uint64 mtime,
                      bp_uint16 mtime_flags,
                      const BY_HANDLE_FILE_INFORMATION *source_info,
                      const FILE_BASIC_INFO *source_basic_info)
{
    add_job_t *new_items;
    char *path_copy;
    char *arc_copy;
    int new_cap;

    if (list->count == list->cap) {
        new_cap = list->cap ? list->cap * 2 : 64;
        new_items = (add_job_t *)realloc(list->items,
                                         (size_t)new_cap * sizeof(add_job_t));
        if (new_items == NULL) {
            return BP_ERR_NOMEM;
        }
        list->items = new_items;
        list->cap = new_cap;
    }

    path_copy = dup_string(path);
    arc_copy = dup_string(arcname);
    if (path_copy == NULL || arc_copy == NULL) {
        free(path_copy);
        free(arc_copy);
        return BP_ERR_NOMEM;
    }

    list->items[list->count].path = path_copy;
    list->items[list->count].arcname = arc_copy;
    list->items[list->count].entry_type = entry_type;
    list->items[list->count].file_size = file_size;
    list->items[list->count].mtime = mtime;
    list->items[list->count].mtime_flags = mtime_flags;
    list->items[list->count].source_info_valid =
        source_info != NULL && source_basic_info != NULL;
    if (list->items[list->count].source_info_valid) {
        list->items[list->count].source_info = *source_info;
        list->items[list->count].source_basic_info = *source_basic_info;
    } else {
        memset(&list->items[list->count].source_info, 0,
               sizeof(list->items[list->count].source_info));
        memset(&list->items[list->count].source_basic_info, 0,
               sizeof(list->items[list->count].source_basic_info));
    }
    list->count++;
    return BP_OK;
}

static void print_added_data_result(bp_archive_t *ar, const char *arcname,
                                    bp_uint64 file_size)
{
    bp_entry_t *e;
    bp_uint64 comp;

    if (g_verbosity != VERB_DEFAULT || ar == NULL || ar->entries_written == 0) {
        return;
    }

    printf("compressing: %s", arcname);
    e = &ar->entry_list[ar->entries_written - 1];
    comp = e->compressed_size;
    if (bp_u64_is_zero(comp) || e->method == BP_METHOD_STORED
        || bp_u64_gte(comp, file_size)) {
        char sizebuf[32];
        format_size64(file_size, sizebuf, sizeof(sizebuf));
        printf("  %s (stored)\n", sizebuf);
    } else {
        char sizebuf[32];
        double orig;
        double cmp;
        double pct;

        orig = (double)file_size.lo + (double)file_size.hi * 4294967296.0;
        cmp = (double)comp.lo + (double)comp.hi * 4294967296.0;
        pct = (orig > 0.0) ? (cmp / orig * 100.0) : 0.0;
        format_size64(comp, sizebuf, sizeof(sizebuf));
        printf("  %s (%.0f%%)\n", sizebuf, pct);
    }
}

/* Detect in-place writes while a source file is being packed. Use the
 * handle's NT change time rather than CRT st_mtime, whose one-second
 * resolution can miss same-size writes on a busy source tree. */
static int win_source_snapshot(FILE *fp, FILE_BASIC_INFO *info)
{
    intptr_t os_handle;

    if (fp == NULL || info == NULL) return 0;
    os_handle = _get_osfhandle(_fileno(fp));
    if (os_handle == -1) return 0;
    return GetFileInformationByHandleEx((HANDLE)os_handle, FileBasicInfo,
                                        info, sizeof(*info)) != 0;
}

static int win_source_handle_info(FILE *fp,
                                 BY_HANDLE_FILE_INFORMATION *info)
{
    intptr_t os_handle;

    if (fp == NULL || info == NULL) return 0;
    os_handle = _get_osfhandle(_fileno(fp));
    if (os_handle == -1) return 0;
    return GetFileInformationByHandle((HANDLE)os_handle, info) != 0;
}

static int win_source_snapshot_equal(const FILE_BASIC_INFO *before,
                                     const FILE_BASIC_INFO *after)
{
    return before->ChangeTime.QuadPart == after->ChangeTime.QuadPart &&
           before->LastWriteTime.QuadPart == after->LastWriteTime.QuadPart &&
           before->FileAttributes == after->FileAttributes;
}

static int add_job(bp_archive_t *ar, const add_job_t *job)
{
    char arcname_utf8[BP_MAX_PATH * 2];

    if (job == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    if (job->entry_type == BP_TYPE_DIR) {
        if (job->source_info_valid &&
            !win_path_matches_snapshot(job->path, &job->source_info,
                                       &job->source_basic_info, 1)) {
            fprintf(stderr,
                    "boltpack: source directory changed after enumeration: %s\n",
                    job->path);
            return BP_ERR_IO;
        }
        if (g_verbosity == VERB_DEFAULT) {
            printf("   creating: %s\n", job->arcname);
        }

        if (win_utf8_copy(job->arcname, arcname_utf8, sizeof(arcname_utf8)) != 0) {
            fprintf(stderr, "boltpack: cannot encode filename: %s\n", job->arcname);
            return BP_ERR_INVALID_ARG;
        }

        {
            int rc = bp_archive_add_entry_ex(ar, arcname_utf8, BP_TYPE_DIR,
                                             BP_OS_WINDOWS, NULL,
                                             bp_u64_from_u32(0), job->mtime,
                                             job->mtime_flags);
            if (rc != BP_OK) return rc;
        }
        return job->source_info_valid
            ? add_win_fat_metadata_value(ar, arcname_utf8, BP_TYPE_DIR,
                                         job->source_info.dwFileAttributes)
            : add_win_fat_metadata(ar, arcname_utf8, job->path, BP_TYPE_DIR);
    }

    if (job->entry_type == BP_TYPE_DATA) {
        FILE *fp;
        bp_io_t fio;
        int rc;
        bp_uint64 actual_size;
        struct _stat64 st;
        BY_HANDLE_FILE_INFORMATION opened_info;
        FILE_BASIC_INFO opened_basic_info;
        FILE_BASIC_INFO source_before;
        FILE_BASIC_INFO source_after;

        fp = fopen(job->path, "rb");
        if (fp == NULL) {
            fprintf(stderr, "boltpack: cannot open: %s\n", job->path);
            return BP_ERR_IO;
        }

        if (job->source_info_valid &&
            (!win_source_handle_info(fp, &opened_info) ||
             !win_archive_info_equal(&job->source_info, &opened_info) ||
             !win_source_snapshot(fp, &opened_basic_info) ||
             !win_source_snapshot_equal(&job->source_basic_info,
                                        &opened_basic_info))) {
            fprintf(stderr,
                    "boltpack: source path changed after enumeration: %s\n",
                    job->path);
            fclose(fp);
            return BP_ERR_IO;
        }

        /* Re-stat through the open fd to close the TOCTOU window. */
        actual_size = job->file_size;
        if (_fstat64(_fileno(fp), &st) == 0) {
            bp_uint64 real_size = bp_off_to_u64((bp_off_t)st.st_size);
            if (real_size.lo != job->file_size.lo ||
                real_size.hi != job->file_size.hi) {
                char was_buf[32];
                char now_buf[32];
                format_size64(job->file_size, was_buf, sizeof(was_buf));
                format_size64(real_size, now_buf, sizeof(now_buf));
                fprintf(stderr,
                        "boltpack: warning: %s changed size during archive "
                        "(was %s, now %s) -- archiving current size\n",
                        job->path, was_buf, now_buf);
                actual_size = real_size;
            }
        }

        if (!win_source_snapshot(fp, &source_before)) {
            fprintf(stderr, "boltpack: cannot inspect source file: %s\n",
                    job->path);
            fclose(fp);
            return BP_ERR_IO;
        }

        bp_io_from_file(&fio, fp);

        if (win_utf8_copy(job->arcname, arcname_utf8, sizeof(arcname_utf8)) != 0) {
            fprintf(stderr, "boltpack: cannot encode filename: %s\n", job->arcname);
            fclose(fp);
            return BP_ERR_INVALID_ARG;
        }

        rc = bp_archive_add_entry_ex(ar, arcname_utf8, BP_TYPE_DATA,
                                     BP_OS_WINDOWS, &fio,
                                     actual_size, job->mtime,
                                     job->mtime_flags);
        if (rc == BP_OK &&
            (!win_source_snapshot(fp, &source_after) ||
             !win_source_snapshot_equal(&source_before, &source_after))) {
            fprintf(stderr,
                    "boltpack: source changed while archiving: %s\n",
                    job->path);
            rc = BP_ERR_IO;
        }
        fclose(fp);

        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n",
                    job->path, bp_strerror(rc));
            return rc;
        }

        print_added_data_result(ar, job->arcname, actual_size);
        return job->source_info_valid
            ? add_win_fat_metadata_value(ar, arcname_utf8, BP_TYPE_DATA,
                                         job->source_info.dwFileAttributes)
            : add_win_fat_metadata(ar, arcname_utf8, job->path, BP_TYPE_DATA);
    }

    return BP_OK;
}

static int sum_path_data_size(const char *path, bp_uint64 *total_size,
                              int *data_count)
{
    struct _stat64 st;

    if (_stat64(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (S_ISDIR(st.st_mode)) {
        win_DIR *d;
        win_dirent *ent;
        char child_path[BP_MAX_PATH];
        int rc;

        d = opendir(path);
        if (d == NULL) {
            fprintf(stderr, "boltpack: cannot open directory: %s\n", path);
            return BP_ERR_IO;
        }

        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0)
                continue;

            path_join(child_path, sizeof(child_path), path, ent->d_name);
            rc = sum_path_data_size(child_path, total_size, data_count);
            if (rc != BP_OK) {
                closedir(d);
                return rc;
            }
        }

        if (d->error) {
            closedir(d);
            fprintf(stderr, "boltpack: directory contains an invalid or unreadable name: %s\n", path);
            return BP_ERR_INVALID_ARG;
        }

        closedir(d);
        return BP_OK;
    }

    if (S_ISREG(st.st_mode)) {
        *total_size = bp_u64_add(*total_size, bp_off_to_u64((bp_off_t)st.st_size));
        (*data_count)++;
    }

    return BP_OK;
}

static int summarize_input_args(int argc, char **argv, bp_uint64 *total_size,
                                int *data_count)
{
    int i;
    int rc;

    *total_size = bp_u64_from_u32(0);
    *data_count = 0;
    for (i = 0; i < argc; i++) {
        rc = sum_path_data_size(argv[i], total_size, data_count);
        if (rc != BP_OK)
            return rc;
    }
    return BP_OK;
}

static int choose_fast_fluff_threads(bp_uint64 total_size, int data_count,
                                     bp_uint8 method)
{
#ifdef BOLTPACK_FAST_FLUFF
    bp_uint64 min_size;
    int threads;

    if (method != BP_METHOD_BOLTLZ && method != BP_METHOD_FLUFF
#ifdef BOLTPACK_ZSTD
        && method != BP_METHOD_ZSTD
#endif
        )
        return 0;
    if (g_fast_thread_policy == FAST_THREAD_SINGLE)
        return 0;

    threads = (g_fast_thread_policy == FAST_THREAD_EXPLICIT)
        ? g_fast_threads : fluff_fast_get_threads();
    if (threads <= 1 || data_count <= 0)
        return 0;
    if (threads > FAST_COMPRESSION_MAX_THREADS)
        threads = FAST_COMPRESSION_MAX_THREADS;

    if (g_fast_thread_policy == FAST_THREAD_EXPLICIT) {
        if (g_fast_threads > 0 && g_fast_threads < threads)
            threads = g_fast_threads;
    } else {
        min_size = bp_u64_from_ulong(method == BP_METHOD_BOLTLZ
            ? FAST_BOLTLZ_AUTO_MIN_BYTES : FAST_FLUFF_AUTO_MIN_BYTES);
        if (!bp_u64_gte(total_size, min_size))
            return 0;
    }

    if (data_count > 1 && threads > data_count)
        threads = data_count;

    return threads > 1 ? threads : 0;
#else
    (void)total_size;
    (void)data_count;
    (void)method;
    return 0;
#endif
}

static int collect_directory_jobs(add_job_list_t *list, const char *dir_path,
                                  const char *arc_prefix,
                                  const BY_HANDLE_FILE_INFORMATION *dir_info,
                                  const FILE_BASIC_INFO *dir_basic_info)
{
    win_DIR *d;
    win_dirent *ent;
    int rc;
    char child_path[BP_MAX_PATH];
    char child_arc[BP_MAX_PATH];

    if (!win_path_matches_snapshot(dir_path, dir_info, dir_basic_info, 1)) {
        fprintf(stderr,
                "boltpack: source directory changed during enumeration: %s\n",
                dir_path);
        return BP_ERR_IO;
    }

    d = opendir(dir_path);
    if (d == NULL) {
        fprintf(stderr, "boltpack: cannot open directory: %s\n", dir_path);
        return BP_ERR_IO;
    }

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0)
            continue;

        path_join(child_path, sizeof(child_path), dir_path, ent->d_name);
        path_join(child_arc, sizeof(child_arc), arc_prefix, ent->d_name);

        rc = collect_path_jobs(list, child_path, child_arc);
        if (rc != BP_OK) {
            closedir(d);
            return rc;
        }
    }

    if (d->error) {
        closedir(d);
        fprintf(stderr, "boltpack: directory contains an invalid or unreadable name: %s\n", dir_path);
        return BP_ERR_INVALID_ARG;
    }

    closedir(d);
    if (!win_path_matches_snapshot(dir_path, dir_info, dir_basic_info, 1)) {
        fprintf(stderr,
                "boltpack: source directory changed during enumeration: %s\n",
                dir_path);
        return BP_ERR_IO;
    }
    return BP_OK;
}

static int collect_path_jobs(add_job_list_t *list, const char *path,
                             const char *arcname)
{
    struct _stat64 st;
    char dir_arc[BP_MAX_PATH];
    bp_uint64 mtime;
    bp_uint64 source_size;
    bp_uint16 mtime_flags;
    int rc;
    BY_HANDLE_FILE_INFORMATION source_info;
    FILE_BASIC_INFO source_basic_info;

    if (_stat64(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!win_open_source_info(path, &source_info, &source_basic_info, 1)) {
            fprintf(stderr, "boltpack: cannot inspect source directory: %s\n",
                    path);
            return BP_ERR_IO;
        }
        mtime_flags = win_filetime_to_archive_time(
            source_info.ftLastWriteTime, &mtime)
                    ? BP_ENTRY_FLAG_MTIME_VALID : 0;
        if (mtime_flags == 0) {
            fprintf(stderr,
                    "boltpack: source time not representable; storing unknown: %s\n",
                    path);
        }
        if (!ends_with_sep(arcname)) {
            sprintf(dir_arc, "%s/", arcname);
        } else {
            strcpy(dir_arc, arcname);
        }

        rc = append_job(list, path, dir_arc, BP_TYPE_DIR,
                        bp_u64_from_u32(0),
                        mtime, mtime_flags, &source_info,
                        &source_basic_info);
        if (rc != BP_OK) return rc;
        return collect_directory_jobs(list, path, dir_arc, &source_info,
                                      &source_basic_info);
    }

    if (S_ISREG(st.st_mode)) {
        if (!win_open_source_info(path, &source_info, &source_basic_info, 0)) {
            fprintf(stderr, "boltpack: cannot inspect source file: %s\n",
                    path);
            return BP_ERR_IO;
        }
        mtime_flags = win_filetime_to_archive_time(
            source_info.ftLastWriteTime, &mtime)
                    ? BP_ENTRY_FLAG_MTIME_VALID : 0;
        if (mtime_flags == 0) {
            fprintf(stderr,
                    "boltpack: source time not representable; storing unknown: %s\n",
                    path);
        }
        source_size.lo = source_info.nFileSizeLow;
        source_size.hi = source_info.nFileSizeHigh;
        return append_job(list, path, arcname, BP_TYPE_DATA,
                          source_size, mtime, mtime_flags, &source_info,
                          &source_basic_info);
    }

    return BP_OK;
}

static int collect_arg_jobs(add_job_list_t *list, int argc, char **argv)
{
    int i;
    int rc;

    for (i = 0; i < argc; i++) {
        const char *path;
        const char *arcname;
        char arcbuf[BP_MAX_PATH];

        path = argv[i];
        arcname = clean_path(path);
        strcpy(arcbuf, arcname);
        normalize_separators(arcbuf);

        rc = collect_path_jobs(list, path, arcbuf);
        if (rc != BP_OK) {
            return rc;
        }
    }

    return BP_OK;
}

static void summarize_data_jobs(const add_job_list_t *list,
                                bp_uint64 *total_size,
                                int *data_count)
{
    int i;

    *total_size = bp_u64_from_u32(0);
    *data_count = 0;
    for (i = 0; i < list->count; i++) {
        if (list->items[i].entry_type != BP_TYPE_DATA)
            continue;
        *total_size = bp_u64_add(*total_size, list->items[i].file_size);
        (*data_count)++;
    }
}

static int add_job_list_serial(bp_archive_t *ar, const add_job_list_t *list)
{
    int i;
    int rc;

    for (i = 0; i < list->count; i++) {
        rc = add_job(ar, &list->items[i]);
        if (rc != BP_OK) {
            return rc;
        }
    }

    return BP_OK;
}

#ifdef BOLTPACK_FAST_FLUFF
/*
 * Returns non-zero if total_bytes can comfortably fit in %TEMP%,
 * leaving at least 256 MB headroom.  Checks cumulative size, not per-file,
 * so callers in batch mode must pass the sum of all files in the batch.
 */
static int tmp_fits_on_tmp_drive(double total_bytes)
{
    char tmp_dir[BP_MAX_PATH];
    ULARGE_INTEGER avail;

    if (total_bytes <= 0.0) return 1;
    if (!GetTempPathA(BP_MAX_PATH, tmp_dir)) return 1; /* can't tell -- assume yes */
    if (!GetDiskFreeSpaceExA(tmp_dir, &avail, NULL, NULL)) return 1;
    return total_bytes + 256.0 * 1024.0 * 1024.0 <= (double)avail.QuadPart;
}

/*
 * Returns non-zero if total_bytes can comfortably fit in the archive
 * destination directory (g_tmp_dir), leaving at least 256 MB headroom.
 */
static int tmp_fits_on_archive_drive(double total_bytes)
{
    ULARGE_INTEGER avail;

    if (total_bytes <= 0.0) return 1;
    if (g_tmp_dir[0] == '\0') return 0;
    if (!GetDiskFreeSpaceExA(g_tmp_dir, &avail, NULL, NULL)) return 1;
    return total_bytes + 256.0 * 1024.0 * 1024.0 <= (double)avail.QuadPart;
}

/*
 * Open a temp file at a pre-decided location.
 * use_sys_tmp != 0 -> %TEMP% (tmpfile); 0 -> archive dir (GetTempFileName +
 * FILE_FLAG_DELETE_ON_CLOSE).  Does NOT call GetDiskFreeSpaceEx;
 * the caller must have already decided.  Safe inside an OMP parallel region.
 */
static FILE *open_tmp_file_win_at(int use_sys_tmp)
{
    FILE *fp;
    char name[BP_MAX_PATH];
    HANDLE h;
    int fd;

    if (use_sys_tmp) {
        fp = tmpfile();
        if (fp != NULL) return fp;
        /* tmpfile() failed; fall through to archive directory. */
    }

    if (g_tmp_dir[0] != '\0') {
        if (GetTempFileNameA(g_tmp_dir, "bpt", 0, name)) {
            h = CreateFileA(name,
                            GENERIC_READ | GENERIC_WRITE,
                            0, NULL,
                            CREATE_ALWAYS,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                            NULL);
            if (h != INVALID_HANDLE_VALUE) {
                fd = _open_osfhandle((intptr_t)h, 0);
                if (fd >= 0) {
                    fp = _fdopen(fd, "w+b");
                    if (fp != NULL) return fp;
                    _close(fd); /* also closes h */
                } else {
                    CloseHandle(h);
                }
            }
            /* Clean up orphaned named file before falling through. */
            DeleteFileA(name);
        }
    }

    return tmpfile(); /* last resort */
}

/*
 * Single-file wrapper kept for callers outside batch context.
 * Not safe inside an OMP parallel region -- use open_tmp_file_win_at()
 * with a pre-computed flag instead.
 */
static FILE *open_tmp_file_win(double hint_bytes)
{
    return open_tmp_file_win_at(tmp_fits_on_tmp_drive(hint_bytes));
}

static void cleanup_packed_job(packed_job_t *job)
{
    if (job == NULL) return;
    if (job->tmp_fp != NULL) {
        fclose(job->tmp_fp);
        job->tmp_fp = NULL;
    }
    bp_packed_entry_dispose(&job->packed);
    memset(&job->tmp_io, 0, sizeof(job->tmp_io));
    job->rc = BP_OK;
}

static int add_data_job_batch(bp_archive_t *ar, const add_job_t *jobs,
                              int batch_n, int total_threads)
{
    packed_job_t *packed_jobs;
    int i;
    int rc;
    int batch_use_sys_tmp;
    double total_batch;

    packed_jobs = (packed_job_t *)calloc((size_t)batch_n, sizeof(*packed_jobs));
    if (packed_jobs == NULL) {
        return BP_ERR_NOMEM;
    }

    /*
     * Decide once, before any thread starts, whether the whole batch fits
     * in %TEMP%.  Summing all files prevents the race where N threads each
     * see "enough space" before any has written.  If %TEMP% can't hold the
     * batch but the archive drive can, route all threads to the archive dir.
     */
    total_batch = 0.0;
    for (i = 0; i < batch_n; i++) {
        total_batch += (double)jobs[i].file_size.lo +
                       (double)jobs[i].file_size.hi * 4294967296.0;
    }
    if (tmp_fits_on_tmp_drive(total_batch)) {
        batch_use_sys_tmp = 1;
    } else if (tmp_fits_on_archive_drive(total_batch)) {
        batch_use_sys_tmp = 0; /* route to archive dir */
    } else {
        /* Neither drive has room; let open_tmp_file_win_at try anyway and
         * surface the error naturally when the write fails. */
        batch_use_sys_tmp = 0;
    }

#pragma omp parallel for schedule(dynamic) num_threads(batch_n)
    for (i = 0; i < batch_n; i++) {
        bp_archive_t worker_ar;
        FILE *src_fp;
        FILE *tmp_fp;
        bp_io_t src_io;
        int local_rc;
        char arcname_utf8[BP_MAX_PATH * 2];
        struct _stat64 st_chk;
        BY_HANDLE_FILE_INFORMATION opened_info;
        FILE_BASIC_INFO opened_basic_info;
        bp_uint64 actual_sz;
        FILE_BASIC_INFO source_before;
        FILE_BASIC_INFO source_after;

        memset(&worker_ar, 0, sizeof(worker_ar));
        worker_ar.block_size = ar->block_size;
        worker_ar.method = ar->method;
        worker_ar.method_level = ar->method_level;
        worker_ar.log_fn = ar->log_fn;
        worker_ar.log_data = ar->log_data;
        worker_ar.creator_os = ar->creator_os;
        worker_ar.creator_cpu = ar->creator_cpu;
        worker_ar.fast_fluff_threads = (batch_n == 1) ? total_threads : 0;
        local_rc = BP_OK;
        src_fp = NULL;
        tmp_fp = NULL;
        packed_jobs[i].tmp_fp = NULL;
        memset(&packed_jobs[i].packed, 0, sizeof(packed_jobs[i].packed));
        actual_sz = jobs[i].file_size;

        src_fp = fopen(jobs[i].path, "rb");
        if (src_fp == NULL) {
            local_rc = BP_ERR_IO;
        }

        if (local_rc == BP_OK && jobs[i].source_info_valid &&
            (!win_source_handle_info(src_fp, &opened_info) ||
             !win_archive_info_equal(&jobs[i].source_info, &opened_info) ||
             !win_source_snapshot(src_fp, &opened_basic_info) ||
             !win_source_snapshot_equal(&jobs[i].source_basic_info,
                                        &opened_basic_info))) {
            fprintf(stderr,
                    "boltpack: source path changed after enumeration: %s\n",
                    jobs[i].path);
            local_rc = BP_ERR_IO;
        }

        /* Recheck size through the open fd to close the TOCTOU window. */
        if (local_rc == BP_OK) {
            if (_fstat64(_fileno(src_fp), &st_chk) == 0) {
                bp_uint64 real_sz = bp_off_to_u64((bp_off_t)st_chk.st_size);
                if (real_sz.lo != jobs[i].file_size.lo ||
                    real_sz.hi != jobs[i].file_size.hi) {
                    char was_buf[32];
                    char now_buf[32];
                    format_size64(jobs[i].file_size, was_buf, sizeof(was_buf));
                    format_size64(real_sz, now_buf, sizeof(now_buf));
                    fprintf(stderr,
                            "boltpack: warning: %s changed size during archive "
                            "(was %s, now %s) -- archiving current size\n",
                            jobs[i].path, was_buf, now_buf);
                    actual_sz = real_sz;
                }
            }
            if (!win_source_snapshot(src_fp, &source_before)) {
                fprintf(stderr,
                        "boltpack: cannot inspect source file: %s\n",
                        jobs[i].path);
                local_rc = BP_ERR_IO;
            }
        }
        if (local_rc == BP_OK) {
            tmp_fp = open_tmp_file_win_at(batch_use_sys_tmp);
            if (tmp_fp == NULL) {
                local_rc = BP_ERR_IO;
            }
        }
        if (local_rc == BP_OK) {
            bp_io_from_file(&src_io, src_fp);
            bp_io_from_file(&packed_jobs[i].tmp_io, tmp_fp);
            if (win_utf8_copy(jobs[i].arcname, arcname_utf8,
                            sizeof(arcname_utf8)) != 0) {
                local_rc = BP_ERR_INVALID_ARG;
            } else {
                local_rc = bp_archive_pack_entry_ex(&worker_ar,
                                                    arcname_utf8,
                                                    BP_TYPE_DATA,
                                                    BP_OS_WINDOWS,
                                                    &src_io,
                                                    actual_sz,
                                                    jobs[i].mtime,
                                                    &packed_jobs[i].tmp_io,
                                                    &packed_jobs[i].packed,
                                                    jobs[i].mtime_flags);
            }
        }
        if (local_rc == BP_OK &&
            (!win_source_snapshot(src_fp, &source_after) ||
             !win_source_snapshot_equal(&source_before, &source_after))) {
            fprintf(stderr,
                    "boltpack: source changed while archiving: %s\n",
                    jobs[i].path);
            local_rc = BP_ERR_IO;
        }

        if (src_fp != NULL) {
            fclose(src_fp);
        }
        bp_archive_close(&worker_ar);

        if (local_rc != BP_OK) {
            if (tmp_fp != NULL) {
                fclose(tmp_fp);
            }
            packed_jobs[i].tmp_fp = NULL;
        } else {
            rewind(tmp_fp);
            packed_jobs[i].tmp_fp = tmp_fp;
        }
        packed_jobs[i].rc = local_rc;
    }

    for (i = 0; i < batch_n; i++) {
        if (packed_jobs[i].rc != BP_OK) {
            rc = packed_jobs[i].rc;
            for (i = 0; i < batch_n; i++) {
                cleanup_packed_job(&packed_jobs[i]);
            }
            free(packed_jobs);
            return rc;
        }
    }

    for (i = 0; i < batch_n; i++) {
        rc = bp_archive_commit_packed_entry(ar, &packed_jobs[i].tmp_io,
                                            &packed_jobs[i].packed);
        if (rc != BP_OK) {
            int j;
            for (j = i; j < batch_n; j++) {
                cleanup_packed_job(&packed_jobs[j]);
            }
            free(packed_jobs);
            return rc;
        }
        /* Use packed.original_size (reflects actual size used during pack). */
        print_added_data_result(ar, jobs[i].arcname,
                                packed_jobs[i].packed.original_size);
        {
            char arcname_utf8[BP_MAX_PATH * 2];
            if (win_utf8_copy(jobs[i].arcname, arcname_utf8,
                              sizeof(arcname_utf8)) != 0) {
                int j;
                for (j = i; j < batch_n; j++)
                    cleanup_packed_job(&packed_jobs[j]);
                free(packed_jobs);
                return BP_ERR_INVALID_ARG;
            }
            rc = jobs[i].source_info_valid
                ? add_win_fat_metadata_value(
                      ar, arcname_utf8, BP_TYPE_DATA,
                      jobs[i].source_info.dwFileAttributes)
                : add_win_fat_metadata(ar, arcname_utf8, jobs[i].path,
                                       BP_TYPE_DATA);
            if (rc != BP_OK) {
                int j;
                cleanup_packed_job(&packed_jobs[i]);
                for (j = i + 1; j < batch_n; j++)
                    cleanup_packed_job(&packed_jobs[j]);
                free(packed_jobs);
                return rc;
            }
        }
        cleanup_packed_job(&packed_jobs[i]);
    }

    free(packed_jobs);
    return BP_OK;
}

static int add_job_list_parallel(bp_archive_t *ar, const add_job_list_t *list,
                                 int total_threads)
{
    int i;
    int batch_n;
    int rc;

    i = 0;
    while (i < list->count) {
        if (list->items[i].entry_type == BP_TYPE_DATA) {
            batch_n = 0;
            while (i + batch_n < list->count &&
                   list->items[i + batch_n].entry_type == BP_TYPE_DATA &&
                   batch_n < total_threads) {
                batch_n++;
            }
            /*
             * When batch_n == 1 the single file gets all inner threads.
             * If the temp file for it won't fit on %TEMP%, avoid the
             * parallel path entirely: use the serial add_job path which
             * writes directly to the archive (no temp file, no extra I/O).
             * Inner fast-fluff threading is still applied via
             * ar->fast_fluff_threads which bp_archive_add_entry honours.
             */
            if (batch_n == 1) {
                double hint = (double)list->items[i].file_size.lo +
                              (double)list->items[i].file_size.hi * 4294967296.0;
                if (ar->method == BP_METHOD_BOLTLZ &&
                    hint < 4.0 * (double)ar->block_size) {
                    rc = add_job(ar, &list->items[i]);
                    if (rc != BP_OK) return rc;
                    i++;
                    continue;
                }
                if (!tmp_fits_on_tmp_drive(hint) && !tmp_fits_on_archive_drive(hint)) {
                    rc = add_job(ar, &list->items[i]);
                    if (rc != BP_OK) return rc;
                    i++;
                    continue;
                }
            }
            rc = add_data_job_batch(ar, &list->items[i], batch_n, total_threads);
            if (rc != BP_OK) {
                return rc;
            }
            i += batch_n;
        } else {
            rc = add_job(ar, &list->items[i]);
            if (rc != BP_OK) {
                return rc;
            }
            i++;
        }
    }

    return BP_OK;
}
#endif

static int add_job_list_with_policy(bp_archive_t *ar,
                                    const add_job_list_t *list,
                                    bp_uint8 method)
{
    bp_uint64 total_size;
    int data_count;
    int threads;
    int rc;
    int i;

    summarize_data_jobs(list, &total_size, &data_count);
    threads = choose_fast_fluff_threads(total_size, data_count, method);
    if (threads > 1) {
        if (method == BP_METHOD_BOLTLZ)
            bp_set_fast_boltlz(ar, threads);
        else if (method == BP_METHOD_FLUFF)
            bp_set_fast_fluff(ar, threads);
#ifdef BOLTPACK_FAST_FLUFF
        rc = add_job_list_parallel(ar, list, threads);
        if (rc != BP_OK) return rc;
#else
        rc = add_job_list_serial(ar, list);
        if (rc != BP_OK) return rc;
#endif
    } else {
        rc = add_job_list_serial(ar, list);
        if (rc != BP_OK) return rc;
    }

    /* Recheck collected file state and directory identity immediately before
     * the caller finalizes and publishes the staged archive. Directory
     * metadata can change when our own archive and worker stages live in the
     * source tree, so only identity is meaningful in this final pass. */
    for (i = 0; i < list->count; i++) {
        const add_job_t *job = &list->items[i];
        int expect_directory = job->entry_type == BP_TYPE_DIR;
        int matches;

        if (!job->source_info_valid) continue;
        if (expect_directory) {
            BY_HANDLE_FILE_INFORMATION current_info;
            FILE_BASIC_INFO current_basic;
            matches = win_open_source_info(job->path, &current_info,
                                           &current_basic, 1) &&
                win_archive_object_id_equal(&job->source_info, &current_info);
        } else {
            matches = win_path_matches_snapshot(
                job->path, &job->source_info, &job->source_basic_info, 0);
        }
        if (!matches) {
            fprintf(stderr, "boltpack: source changed before archive "
                    "publication: %s\n", job->path);
            return BP_ERR_IO;
        }
    }
    return BP_OK;
}

/* ---- recursive add ----------------------------------------------------- */

static int add_path(bp_archive_t *ar, const char *path, const char *arcname);

static int add_directory(bp_archive_t *ar, const char *dir_path,
                         const char *arc_prefix)
{
    win_DIR *d;
    win_dirent *ent;
    int rc;
    char child_path[BP_MAX_PATH];
    char child_arc[BP_MAX_PATH];

    d = opendir(dir_path);
    if (d == NULL) {
        fprintf(stderr, "boltpack: cannot open directory: %s\n", dir_path);
        return BP_ERR_IO;
    }

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        path_join(child_path, sizeof(child_path), dir_path, ent->d_name);
        path_join(child_arc, sizeof(child_arc), arc_prefix, ent->d_name);

        rc = add_path(ar, child_path, child_arc);
        if (rc != BP_OK) {
            closedir(d);
            return rc;
        }
    }

    if (d->error) {
        closedir(d);
        fprintf(stderr, "boltpack: directory contains an invalid or unreadable name: %s\n", dir_path);
        return BP_ERR_INVALID_ARG;
    }

    closedir(d);
    return BP_OK;
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    struct _stat64 st;
    bp_uint64 mtime;
    bp_uint16 mtime_flags;
    int rc;

    if (_stat64(path, &st) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }
    mtime_flags = win_archive_mtime(st.st_mtime, &mtime)
                ? BP_ENTRY_FLAG_MTIME_VALID : 0;
    if (mtime_flags == 0) {
        fprintf(stderr,
                "boltpack: source time not representable; storing unknown: %s\n",
                path);
    }

    if (S_ISDIR(st.st_mode)) {
        char dir_arc[BP_MAX_PATH];
        char dir_arc_utf8[BP_MAX_PATH * 2];

        /* ensure arc name ends with '/' */
        if (!ends_with_sep(arcname)) {
            sprintf(dir_arc, "%s/", arcname);
        } else {
            strcpy(dir_arc, arcname);
        }

        if (g_verbosity == VERB_DEFAULT) {
            printf("   creating: %s\n", dir_arc);
        }

        /* Store as UTF-8 in the archive */
        if (win_utf8_copy(dir_arc, dir_arc_utf8, sizeof(dir_arc_utf8)) != 0) {
            fprintf(stderr, "boltpack: cannot encode filename: %s\n", dir_arc);
            return BP_ERR_INVALID_ARG;
        }

        rc = bp_archive_add_entry_ex(ar, dir_arc_utf8, BP_TYPE_DIR,
                                     BP_OS_WINDOWS, NULL,
                                     bp_u64_from_u32(0), mtime, mtime_flags);
        if (rc != BP_OK) return rc;

        rc = add_win_fat_metadata(ar, dir_arc_utf8, path, BP_TYPE_DIR);
        if (rc != BP_OK) return rc;

        return add_directory(ar, path, dir_arc);

    } else if (S_ISREG(st.st_mode)) {
        FILE *fp;
        bp_io_t fio;
        bp_uint64 file_size = bp_off_to_u64((bp_off_t)st.st_size);
        char arcname_utf8[BP_MAX_PATH * 2];

        fp = fopen(path, "rb");
        if (fp == NULL) {
            fprintf(stderr, "boltpack: cannot open: %s\n", path);
            return BP_ERR_IO;
        }
        bp_io_from_file(&fio, fp);

        if (g_verbosity == VERB_DEFAULT) {
            printf("compressing: %s", arcname);
            fflush(stdout);
        }

        /* Store as UTF-8 in the archive */
        if (win_utf8_copy(arcname, arcname_utf8, sizeof(arcname_utf8)) != 0) {
            fprintf(stderr, "boltpack: cannot encode filename: %s\n", arcname);
            fclose(fp);
            return BP_ERR_INVALID_ARG;
        }

        rc = bp_archive_add_entry_ex(ar, arcname_utf8, BP_TYPE_DATA,
                                     BP_OS_WINDOWS, &fio, file_size,
                                     mtime, mtime_flags);
        fclose(fp);

        if (rc != BP_OK) return rc;

        rc = add_win_fat_metadata(ar, arcname_utf8, path, BP_TYPE_DATA);
        if (rc != BP_OK) return rc;

        if (g_verbosity == VERB_DEFAULT) {
            bp_entry_t *e = &ar->entry_list[ar->entries_written - 1];
            bp_uint64 comp = e->compressed_size;
            if (bp_u64_is_zero(comp) || e->method == BP_METHOD_STORED
                || bp_u64_gte(comp, file_size)) {
                char sizebuf[32];
                format_size64(file_size, sizebuf, sizeof(sizebuf));
                printf("  %s (stored)\n", sizebuf);
            } else {
                char sizebuf[32];
                double pct = ((double)comp.lo + (double)comp.hi * 4294967296.0) /
                             ((double)file_size.lo + (double)file_size.hi * 4294967296.0) * 100.0;
                format_size64(comp, sizebuf, sizeof(sizebuf));
                printf("  %s (%.0f%%)\n", sizebuf, pct);
            }
        }

        return BP_OK;
    }

    /* skip other file types (no symlinks, devices, etc. on DOS) */
    return BP_OK;
}

/* ---- list callback ----------------------------------------------------- */

typedef struct {
    bp_uint32 file_count;
    bp_uint32 dir_count;
    bp_uint64 total_orig;
    bp_uint64 total_comp;
} list_ctx_t;

static int list_cb(const bp_entry_t *entry, void *user_data)
{
    list_ctx_t *ctx = (list_ctx_t *)user_data;
    const char *type_str;
    char orig_buf[32], comp_buf[32];

    switch (entry->entry_type) {
    case BP_TYPE_DATA:    type_str = "DATA"; break;
    case BP_TYPE_RSRC:    type_str = "RSRC"; break;
    case BP_TYPE_META:    type_str = "META"; break;
    case BP_TYPE_SYMLINK: type_str = "LINK"; break;
    case BP_TYPE_DIR:     type_str = "DIR "; break;
    default:              type_str = "??? "; break;
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
    } else {
        ctx->file_count++;
    }
    ctx->total_orig = bp_u64_add(ctx->total_orig, entry->original_size);
    ctx->total_comp = bp_u64_add(ctx->total_comp, entry->compressed_size);

    format_number(bp_u64_to_u32(entry->original_size), orig_buf, sizeof(orig_buf));
    format_number(bp_u64_to_u32(entry->compressed_size), comp_buf, sizeof(comp_buf));

    {
        char disp_name[BP_MAX_PATH];
        if (win_utf8_copy(entry->name, disp_name, sizeof(disp_name)) != 0) {
            strncpy(disp_name, entry->name, BP_MAX_PATH - 1);
            disp_name[BP_MAX_PATH - 1] = '\0';
        }
        printf("%10s %10s  %s  %s\n", orig_buf, comp_buf, type_str, disp_name);
    }
    return 0;
}

/* ---- extract callbacks ------------------------------------------------- */

typedef struct win_path_node_s {
    struct win_path_node_s *left;
    struct win_path_node_s *right;
    int height;
    bp_uint8 entry_type;
    wchar_t name[BP_MAX_PATH_WIDE];
} win_path_node_t;

static int win_path_height(const win_path_node_t *node)
{
    return node == NULL ? 0 : node->height;
}

static void win_path_update_height(win_path_node_t *node)
{
    int left = win_path_height(node->left);
    int right = win_path_height(node->right);
    node->height = (left > right ? left : right) + 1;
}

static win_path_node_t *win_path_rotate_right(win_path_node_t *root)
{
    win_path_node_t *top = root->left;
    root->left = top->right;
    top->right = root;
    win_path_update_height(root);
    win_path_update_height(top);
    return top;
}

static win_path_node_t *win_path_rotate_left(win_path_node_t *root)
{
    win_path_node_t *top = root->right;
    root->right = top->left;
    top->left = root;
    win_path_update_height(root);
    win_path_update_height(top);
    return top;
}

static int win_path_compare(const wchar_t *left, const wchar_t *right)
{
    int rc = CompareStringOrdinal(left, -1, right, -1, TRUE);
    if (rc == CSTR_LESS_THAN) return -1;
    if (rc == CSTR_GREATER_THAN) return 1;
    return rc == CSTR_EQUAL ? 0 : 2;
}

static win_path_node_t *win_path_find_exact(win_path_node_t *root,
                                            const wchar_t *name)
{
    while (root != NULL) {
        int cmp = win_path_compare(name, root->name);
        if (cmp == 2) return NULL;
        if (cmp == 0) return root;
        root = cmp < 0 ? root->left : root->right;
    }
    return NULL;
}

static win_path_node_t *win_path_lower_bound(win_path_node_t *root,
                                             const wchar_t *name)
{
    win_path_node_t *best = NULL;
    while (root != NULL) {
        int cmp = win_path_compare(root->name, name);
        if (cmp == 2) return NULL;
        if (cmp >= 0) {
            best = root;
            root = root->left;
        } else {
            root = root->right;
        }
    }
    return best;
}

static int win_path_has_prefix(const wchar_t *name, const wchar_t *prefix,
                               size_t prefix_length)
{
    int rc = CompareStringOrdinal(name, (int)prefix_length, prefix,
                                   (int)prefix_length, TRUE);
    return rc == CSTR_EQUAL;
}

static int win_path_compare_n(const wchar_t *left, size_t left_length,
                              const char *right, size_t right_length)
{
    wchar_t wide[16];
    size_t i;
    int rc;
    if (left_length != right_length || right_length >= 16U) return 1;
    for (i = 0; i < right_length; i++) wide[i] = (unsigned char)right[i];
    rc = CompareStringOrdinal(left, (int)left_length, wide,
                              (int)right_length, TRUE);
    if (rc == CSTR_EQUAL) return 0;
    return rc == CSTR_LESS_THAN ? -1 : 1;
}

static win_path_node_t *win_path_insert(win_path_node_t *root,
                                        const wchar_t *name,
                                        bp_uint8 entry_type, int *result)
{
    int cmp, balance;
    win_path_node_t *child;
    if (root == NULL) {
        root = (win_path_node_t *)calloc(1, sizeof(*root));
        if (root == NULL) { *result = BP_ERR_NOMEM; return NULL; }
        wcscpy(root->name, name);
        root->height = 1;
        root->entry_type = entry_type;
        *result = BP_OK;
        return root;
    }
    cmp = win_path_compare(name, root->name);
    if (cmp == 2) { *result = BP_ERR_INVALID_ARG; return root; }
    if (cmp == 0) { *result = BP_ERR_PARTIAL; return root; }
    if (cmp < 0) {
        child = win_path_insert(root->left, name, entry_type, result);
        if (*result != BP_OK) return root;
        root->left = child;
    } else {
        child = win_path_insert(root->right, name, entry_type, result);
        if (*result != BP_OK) return root;
        root->right = child;
    }
    win_path_update_height(root);
    balance = win_path_height(root->left) - win_path_height(root->right);
    if (balance > 1) {
        if (win_path_compare(name, root->left->name) > 0)
            root->left = win_path_rotate_left(root->left);
        return win_path_rotate_right(root);
    }
    if (balance < -1) {
        if (win_path_compare(name, root->right->name) < 0)
            root->right = win_path_rotate_right(root->right);
        return win_path_rotate_left(root);
    }
    return root;
}

static void win_path_free(win_path_node_t *root)
{
    if (root == NULL) return;
    win_path_free(root->left);
    win_path_free(root->right);
    free(root);
}

static int win_component_is_device(const wchar_t *component, size_t length)
{
    size_t base = 0;
    size_t i;
    static const char *const devices[] = {
        "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"
    };
    while (base < length && component[base] != L'.') base++;
    while (base > 0 && (component[base - 1] == L' ' ||
                        component[base - 1] == L'.')) base--;
    for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        size_t n = strlen(devices[i]);
        if (base == n && win_path_compare_n(component, base, devices[i], n) == 0)
            return 1;
    }
    if (base == 4 &&
        (win_path_compare_n(component, 3, "COM", 3) == 0 ||
         win_path_compare_n(component, 3, "LPT", 3) == 0) &&
        ((component[3] >= L'1' && component[3] <= L'9') ||
         component[3] == 0x00B9 || component[3] == 0x00B2 ||
         component[3] == 0x00B3)) return 1;
    return 0;
}

static int win_project_windows_path(const char *local_name, wchar_t *key,
                                    int key_capacity)
{
    wchar_t normalized[BP_MAX_PATH_WIDE];
    int length, normalized_length, i, start;
    length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, local_name, -1,
                                 key, key_capacity);
    if (length <= 0) return BP_ERR_INVALID_ARG;
    length--;
    if (!win_wide_is_valid_utf16(key)) return BP_ERR_INVALID_ARG;
    for (i = 0; i < length; i++)
        if (key[i] == L'/') key[i] = L'\\';
    normalized_length = NormalizeString(NormalizationC, key, length,
                                        normalized, BP_MAX_PATH_WIDE);
    if (normalized_length <= 0 || normalized_length >= key_capacity)
        return BP_ERR_INVALID_ARG;
    memcpy(key, normalized,
           (size_t)normalized_length * sizeof(wchar_t));
    length = normalized_length;
    key[length] = L'\0';
    if (length == 0 || key[0] == L'\\' || length >= BP_MAX_PATH_WIDE)
        return BP_ERR_INVALID_ARG;
    for (i = 0; i < length; i++) {
        if (key[i] == L':' || key[i] == L'<' || key[i] == L'>' ||
            key[i] == L'"' || key[i] == L'|' || key[i] == L'?' ||
            key[i] == L'*' || key[i] < 32) return BP_ERR_INVALID_ARG;
    }
    while (length > 0 && key[length - 1] == L'\\') length--;
    if (length == 0) return BP_ERR_INVALID_ARG;
    key[length] = L'\0';
    start = 0;
    for (i = 0; i <= length; i++) {
        if (key[i] == L'\\' || key[i] == L'\0') {
            int component_length = i - start;
            if (component_length <= 0 || key[i - 1] == L'.' ||
                component_length > 255 ||
                key[i - 1] == L' ' ||
                (component_length == 1 && key[start] == L'.') ||
                win_component_is_device(key + start,
                                        (size_t)component_length))
                return BP_ERR_INVALID_ARG;
            start = i + 1;
        }
    }
    return BP_OK;
}

typedef struct {
    const char *out_dir;    /* -o PATH or NULL */
    int         force;      /* -f flag */
    int         dry_run;    /* -n flag */
    bp_uint32   file_count;
    bp_uint32   dir_count;
    bp_uint32   unsafe_count;  /* entries skipped for unsafe names */
    int         slash_warned;  /* printed the leading-'/' warning once */
    struct win_dir_attr_s *directories;
    win_path_node_t *projected_names;
    char        last_subject_path[BP_MAX_PATH];
    bp_uint8    last_subject_type;
    int         last_subject_valid;
    int         last_subject_ok;
} extract_ctx_t;

static const char *extract_entry_name(const bp_entry_t *entry,
                                      extract_ctx_t *ctx);

static int win_register_output_path(extract_ctx_t *ctx, const char *local_name,
                                    bp_uint8 entry_type)
{
    wchar_t key[BP_MAX_PATH_WIDE];
    size_t i, length;
    int rc = win_project_windows_path(local_name, key, BP_MAX_PATH_WIDE);
    if (rc != BP_OK) return rc;
    length = wcslen(key);
    for (i = 0; i < length; i++) {
        if (key[i] == L'\\') {
            wchar_t saved = key[i];
            key[i] = L'\0';
            {
                win_path_node_t *ancestor =
                    win_path_find_exact(ctx->projected_names, key);
                if (ancestor != NULL && ancestor->entry_type == BP_TYPE_DATA) {
                    key[i] = saved;
                    return BP_ERR_PARTIAL;
                }
            }
            key[i] = saved;
        }
    }
    {
        wchar_t descendant_prefix[BP_MAX_PATH_WIDE];
        win_path_node_t *next;
        if (entry_type == BP_TYPE_DATA && length + 1 < BP_MAX_PATH_WIDE) {
            memcpy(descendant_prefix, key, length * sizeof(wchar_t));
            descendant_prefix[length] = L'\\';
            descendant_prefix[length + 1] = L'\0';
            next = win_path_lower_bound(ctx->projected_names,
                                        descendant_prefix);
            if (next != NULL &&
                win_path_has_prefix(next->name, descendant_prefix,
                                    length + 1)) return BP_ERR_PARTIAL;
        }
    }
    ctx->projected_names = win_path_insert(ctx->projected_names, key,
                                           entry_type, &rc);
    return rc;
}

typedef struct {
    char *key;
    bp_uint8 type;
} win_hfs_path_t;

static int win_legacy_paths_check(win_hfs_path_t *items, bp_uint32 count,
                                  const char *profile);

typedef struct {
    char name[BP_ATARI_GEMDOS_PATH_MAX + 1U];
    bp_uint8 type;
} win_atari_path_t;

static int win_atari_path_prefix_conflict(const win_atari_path_t *paths,
                                          unsigned count)
{
    unsigned i, j;
    for (i = 0; i < count; i++) {
        size_t length = strlen(paths[i].name);
        for (j = 0; j < count; j++) {
            if (paths[i].type != BP_TYPE_DATA || i == j) continue;
            if (strncmp(paths[j].name, paths[i].name, length) == 0 &&
                paths[j].name[length] == '\\') return 1;
        }
    }
    return 0;
}

static int win_atari_profile_add_name(bp_atari_name_set_t *names,
                                      win_atari_path_t *paths,
                                      unsigned *count, const char *name,
                                      bp_uint8 type)
{
    char key[BP_ATARI_GEMDOS_PATH_MAX + 1U];
    unsigned int key_length;
    int rc;
    if (*count >= BP_ATARI_PATH_MAX_ENTRIES) return BP_ERR_OVERFLOW;
    rc = bp_atari_project_name(name, type == BP_TYPE_DIR, key, sizeof(key),
                               &key_length);
    if (rc != BP_OK) return rc;
    rc = bp_atari_name_set_add(names, key, key_length);
    if (rc != BP_OK) return rc;
    memcpy(paths[*count].name, key, (size_t)key_length + 1U);
    paths[*count].type = type;
    (*count)++;
    return BP_OK;
}

/* Atari ST GEMDOS writer profile: classic printable-ASCII 8.3 names,
 * bounded path/index, and only data/directories plus portable/Atari metadata. */
static int win_atari_profile_preflight(const add_job_list_t *jobs,
                                       const bp_archive_t *existing)
{
    bp_atari_name_set_t names;
    win_atari_path_t *paths;
    unsigned count = 0;
    int rc = BP_OK;
    int i;

    bp_atari_name_set_init(&names);
    paths = (win_atari_path_t *)calloc(BP_ATARI_PATH_MAX_ENTRIES,
                                      sizeof(*paths));
    if (paths == NULL) return BP_ERR_NOMEM;
    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            if (entry->entry_type == BP_TYPE_META) {
                if (entry->platform == BP_META_PORTABLE ||
                    entry->platform == BP_OS_ATARI) continue;
                fprintf(stderr,
                        "boltpack: Atari ST profile refuses foreign metadata: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto win_atari_profile_done;
            }
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: Atari ST profile does not support this entry type: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto win_atari_profile_done;
            }
            if (entry->entry_type == BP_TYPE_DATA &&
                (entry->original_size.hi != 0 ||
                 entry->original_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: Atari ST profile file exceeds signed 32-bit I/O limit: %s\n",
                        entry->name);
                rc = BP_ERR_OVERFLOW;
                goto win_atari_profile_done;
            }
            rc = win_atari_profile_add_name(&names, paths, &count,
                                            entry->name, entry->entry_type);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: Atari ST name is invalid, colliding, or over the profile index limit: %s\n",
                        entry->name);
                goto win_atari_profile_done;
            }
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            if (job->entry_type == BP_TYPE_SYMLINK) {
                fprintf(stderr,
                        "boltpack: Atari ST profile does not support symbolic links: %s\n",
                        job->path);
                rc = BP_ERR_PARTIAL;
                goto win_atari_profile_done;
            }
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) continue;
            if (job->entry_type == BP_TYPE_DATA &&
                (job->file_size.hi != 0 ||
                 job->file_size.lo > 0x7FFFFFFFUL)) {
                fprintf(stderr,
                        "boltpack: Atari ST profile file exceeds signed 32-bit I/O limit: %s\n",
                        job->path);
                rc = BP_ERR_OVERFLOW;
                goto win_atari_profile_done;
            }
            rc = win_atari_profile_add_name(&names, paths, &count,
                                            job->arcname, job->entry_type);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: Atari ST name is invalid, colliding, or over the profile index limit: %s\n",
                        job->path);
                goto win_atari_profile_done;
            }
        }
    }
    if (win_atari_path_prefix_conflict(paths, count)) {
        fprintf(stderr,
                "boltpack: Atari ST profile has a file/directory prefix conflict\n");
        rc = BP_ERR_PARTIAL;
    }

win_atari_profile_done:
    bp_atari_name_set_free(&names);
    free(paths);
    return rc;
}

/* Sinclair QL names are Latin-1; QDOS uses underscore in place of archive
 * separators, so project before case-fold collision checking. */
static int win_ql_add_name(win_hfs_path_t *items, bp_uint32 *count,
                           const char *utf8_name, bp_uint8 type,
                           bp_uint64 file_size)
{
    char local[256], folded[256];
    size_t length, scan_length, start, i;
    if (type != BP_TYPE_DATA && type != BP_TYPE_DIR) return BP_ERR_PARTIAL;
    if (type == BP_TYPE_DATA &&
        (file_size.hi != 0 || file_size.lo > 0x7FFFFFFFUL))
        return BP_ERR_OVERFLOW;
    if (bp_utf8_to_local(utf8_name, local, (int)sizeof(local),
                         BP_CP_8859_1) != 0)
        return BP_ERR_INVALID_ARG;
    length = strlen(local);
    if (length == 0 || length > 36U || local[0] == '/')
        return BP_ERR_NAME_TOO_LONG;
    scan_length = length;
    if (type == BP_TYPE_DIR && local[length - 1U] == '/') scan_length--;
    for (start = 0, i = 0; i <= scan_length; i++) {
        if (i == scan_length || local[i] == '/') {
            size_t n = i - start, j;
            if (n == 0 || (n == 1U && local[start] == '.') ||
                (n == 2U && local[start] == '.' && local[start + 1U] == '.'))
                return BP_ERR_INVALID_ARG;
            for (j = start; j < i; j++) {
                unsigned char c = (unsigned char)local[j];
                if (c < 0x20U || c == 0x7FU || c == ':' || c == '\\')
                    return BP_ERR_INVALID_ARG;
            }
            start = i + 1U;
        }
    }
    for (i = 0; i < length; i++)
        if (local[i] == '/') local[i] = '_';
    if (bp_local_casefold(local, folded, (int)sizeof(folded),
                          BP_CP_8859_1) != 0)
        return BP_ERR_INVALID_ARG;
    if (*count >= BP_PROFILE1_MAX_ENTRIES) return BP_ERR_OVERFLOW;
    items[*count].key = dup_string(folded);
    if (items[*count].key == NULL) return BP_ERR_NOMEM;
    items[*count].type = type;
    (*count)++;
    return BP_OK;
}

static int win_ql_profile_preflight(const add_job_list_t *jobs,
                                    const bp_archive_t *existing)
{
    win_hfs_path_t *items;
    bp_uint32 count = 0, i;
    int rc = BP_OK;
    items = (win_hfs_path_t *)calloc(BP_PROFILE1_MAX_ENTRIES,
                                     sizeof(*items));
    if (items == NULL) return BP_ERR_NOMEM;
    if (existing != NULL) {
        for (i = 0; i < existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: Sinclair QL profile refuses metadata, links, or special entries: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto win_ql_done;
            }
            rc = win_ql_add_name(items, &count, entry->name,
                                 entry->entry_type, entry->original_size);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: existing entry violates Sinclair QL profile: %s\n",
                        entry->name);
                goto win_ql_done;
            }
        }
    }
    if (jobs != NULL) {
        int j;
        for (j = 0; j < jobs->count; j++) {
            const add_job_t *job = &jobs->items[j];
            rc = win_ql_add_name(items, &count, job->arcname,
                                 job->entry_type, job->file_size);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: source entry violates Sinclair QL profile: %s\n",
                        job->path);
                goto win_ql_done;
            }
        }
    }
    rc = win_legacy_paths_check(items, count, "Sinclair QL");
win_ql_done:
    for (i = 0; i < count; i++) free(items[i].key);
    free(items);
    return rc;
}

/* RISC OS FileCore uses Latin-1 names and treats a period as path syntax.
 * Keep archive slashes as path boundaries after validating each leaf. */
static int win_riscos_add_name(win_hfs_path_t *items, bp_uint32 *count,
                               const char *utf8_name, bp_uint8 type,
                               bp_uint64 file_size)
{
    char local[256], folded[256];
    size_t length, scan_length, start, i;
    if (type != BP_TYPE_DATA && type != BP_TYPE_DIR) return BP_ERR_PARTIAL;
    if (type == BP_TYPE_DATA &&
        (file_size.hi != 0 || file_size.lo > 0x7FFFFFFFUL))
        return BP_ERR_OVERFLOW;
    if (bp_utf8_to_local(utf8_name, local, (int)sizeof(local),
                         BP_CP_8859_1) != 0)
        return BP_ERR_INVALID_ARG;
    length = strlen(local);
    if (length == 0 || length > 255U || local[0] == '/')
        return BP_ERR_NAME_TOO_LONG;
    scan_length = length;
    if (type == BP_TYPE_DIR && local[length - 1U] == '/') scan_length--;
    for (start = 0, i = 0; i <= scan_length; i++) {
        if (i == scan_length || local[i] == '/') {
            size_t n = i - start, j;
            if (n == 0 || n > 10U || (n == 1U && local[start] == '.') ||
                (n == 2U && local[start] == '.' && local[start + 1U] == '.'))
                return BP_ERR_NAME_TOO_LONG;
            for (j = start; j < i; j++) {
                unsigned char c = (unsigned char)local[j];
                if (c < 0x20U || c == 0x7FU || c == '.' || c == ':' ||
                    c == '*' || c == '#' || c == '$' || c == '%' ||
                    c == '&' || c == '@' || c == '"' || c == '|' ||
                    c == '^' || c == '\\') return BP_ERR_INVALID_ARG;
            }
            start = i + 1U;
        }
    }
    if (type == BP_TYPE_DIR && local[length - 1U] == '/')
        local[--length] = '\0';
    if (bp_local_casefold(local, folded, (int)sizeof(folded),
                          BP_CP_8859_1) != 0)
        return BP_ERR_INVALID_ARG;
    if (*count >= BP_PROFILE1_MAX_ENTRIES) return BP_ERR_OVERFLOW;
    items[*count].key = dup_string(folded);
    if (items[*count].key == NULL) return BP_ERR_NOMEM;
    items[*count].type = type;
    (*count)++;
    return BP_OK;
}

static int win_riscos_profile_preflight(const add_job_list_t *jobs,
                                        const bp_archive_t *existing)
{
    win_hfs_path_t *items;
    bp_uint32 count = 0, i;
    int rc = BP_OK;
    items = (win_hfs_path_t *)calloc(BP_PROFILE1_MAX_ENTRIES,
                                     sizeof(*items));
    if (items == NULL) return BP_ERR_NOMEM;
    if (existing != NULL) {
        for (i = 0; i < existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: RISC OS profile refuses metadata, links, or special entries: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto win_riscos_done;
            }
            rc = win_riscos_add_name(items, &count, entry->name,
                                     entry->entry_type, entry->original_size);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: existing entry violates RISC OS profile: %s\n",
                        entry->name);
                goto win_riscos_done;
            }
        }
    }
    if (jobs != NULL) {
        int j;
        for (j = 0; j < jobs->count; j++) {
            const add_job_t *job = &jobs->items[j];
            rc = win_riscos_add_name(items, &count, job->arcname,
                                     job->entry_type, job->file_size);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: source entry violates RISC OS profile: %s\n",
                        job->path);
                goto win_riscos_done;
            }
        }
    }
    rc = win_legacy_paths_check(items, count, "RISC OS");
win_riscos_done:
    for (i = 0; i < count; i++) free(items[i].key);
    free(items);
    return rc;
}

/* Human68k v3 uses standard Shift-JIS 18.3 names. Count multibyte
 * characters before inspecting punctuation so a 0x5c trail byte stays data. */
static int win_x68000_add_name(x68k_path_index_t *paths,
                               const char *utf8_name, int is_directory)
{
    char local[X68K_INDEX_MAX_PATH * 3U];
    size_t length, scan_length, start, i;
    int rc;
    if (bp_utf8_to_local(utf8_name, local, (int)sizeof(local),
                         BP_CP_SJIS) != 0) {
        fprintf(stderr,
                "boltpack: X68000 profile cannot encode standard Shift-JIS name: %s\n",
                utf8_name);
        return BP_ERR_PARTIAL;
    }
    length = strlen(local);
    if (length == 0 || length > 255U || local[0] == '/')
        return BP_ERR_NAME_TOO_LONG;
    scan_length = length;
    if (is_directory && local[length - 1U] == '/') scan_length--;
    for (start = 0, i = 0; i <= scan_length; i++) {
        if (i == scan_length || local[i] == '/') {
            size_t component_length = i - start;
            size_t stem_bytes = 0, extension_bytes = 0, j;
            int in_extension = 0, has_dot = 0;
            if (component_length == 0) return BP_ERR_PARTIAL;
            for (j = start; j < i;) {
                unsigned char c = (unsigned char)local[j];
                size_t char_bytes = 1U;
                if (x68k_index_sjis_pair_at(local + j)) {
                    char_bytes = 2U;
                } else if ((c >= 0x81U && c <= 0x9FU) ||
                           (c >= 0xE0U && c <= 0xEFU)) {
                    return BP_ERR_PARTIAL;
                }
                if (char_bytes == 1U) {
                    if (c == '.') {
                        if (has_dot || j == start || j + 1U == i)
                            return BP_ERR_PARTIAL;
                        has_dot = 1;
                        in_extension = 1;
                        j++;
                        continue;
                    }
                    if (c < 0x21U || c == 0x7FU || c == '"' || c == '*' ||
                        c == '+' || c == ',' || c == ':' || c == ';' ||
                        c == '<' || c == '=' || c == '>' || c == '?' ||
                        c == '[' || c == '\\' || c == ']' || c == '|')
                        return BP_ERR_PARTIAL;
                }
                if (in_extension) extension_bytes += char_bytes;
                else stem_bytes += char_bytes;
                j += char_bytes;
            }
            if (stem_bytes == 0 || stem_bytes > 18U || extension_bytes > 3U)
                return BP_ERR_NAME_TOO_LONG;
            start = i + 1U;
        }
    }
    rc = x68k_path_index_add(paths, local, is_directory);
    if (rc != BP_OK)
        fprintf(stderr,
                "boltpack: X68000 projected path collision or path limit: %s\n",
                utf8_name);
    return rc;
}

static int win_x68000_profile_preflight(const add_job_list_t *jobs,
                                        const bp_archive_t *existing)
{
    x68k_path_index_t paths;
    int rc = x68k_path_index_init(&paths);
    int i;
    if (rc != BP_OK) return rc;
    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: X68000 profile refuses metadata, links, or special entries: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto win_x68000_done;
            }
            if (entry->entry_type == BP_TYPE_DATA &&
                (entry->original_size.hi != 0 ||
                 entry->original_size.lo > 0x7FFFFFFFUL)) {
                rc = BP_ERR_OVERFLOW;
                goto win_x68000_done;
            }
            rc = win_x68000_add_name(&paths, entry->name,
                                     entry->entry_type == BP_TYPE_DIR);
            if (rc != BP_OK) goto win_x68000_done;
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: X68000 profile does not support links or special entries: %s\n",
                        job->path);
                rc = BP_ERR_PARTIAL;
                goto win_x68000_done;
            }
            if (job->entry_type == BP_TYPE_DATA &&
                (job->file_size.hi != 0 ||
                 job->file_size.lo > 0x7FFFFFFFUL)) {
                rc = BP_ERR_OVERFLOW;
                goto win_x68000_done;
            }
            rc = win_x68000_add_name(&paths, job->arcname,
                                     job->entry_type == BP_TYPE_DIR);
            if (rc != BP_OK) goto win_x68000_done;
        }
    }
win_x68000_done:
    x68k_path_index_free(&paths);
    return rc;
}

/* Windows NT path profile used by the native Windows CLI. Path keys are
 * validated as UTF-16, normalized to NFC for conservative collision checks,
 * and compared with Windows ordinal case-insensitive semantics. */
static int win_target_profile_preflight(const add_job_list_t *jobs,
                                        const bp_archive_t *existing)
{
    extract_ctx_t ctx;
    int rc = BP_OK;
    int i;

    memset(&ctx, 0, sizeof(ctx));
    if (existing != NULL) {
        for (i = 0; i < (int)existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            if (entry->entry_type == BP_TYPE_META) {
                if (entry->platform == BP_META_PORTABLE) continue;
                fprintf(stderr,
                        "boltpack: Windows NT profile refuses non-portable metadata: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto win_target_profile_done;
            }
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: Windows NT profile does not support this entry type: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto win_target_profile_done;
            }
            rc = win_register_output_path(&ctx, entry->name,
                                          entry->entry_type);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: Windows NT path is invalid, too long, or collides: %s\n",
                        entry->name);
                goto win_target_profile_done;
            }
        }
    }
    if (jobs != NULL) {
        for (i = 0; i < jobs->count; i++) {
            const add_job_t *job = &jobs->items[i];
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: Windows NT profile does not support this source entry type: %s\n",
                        job->path);
                rc = BP_ERR_PARTIAL;
                goto win_target_profile_done;
            }
            rc = win_register_output_path(&ctx, job->arcname,
                                          job->entry_type);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: Windows NT path is invalid, too long, or collides: %s\n",
                        job->path);
                goto win_target_profile_done;
            }
        }
    }

win_target_profile_done:
    win_path_free(ctx.projected_names);
    return rc;
}

#define WIN_DOS_PROFILE_NONE          0
#define WIN_DOS_PROFILE_CP850_83      1
#define WIN_DOS_PROFILE_CP437_83      2
#define WIN_DOS_PROFILE_DOS16_CP850   3
#define WIN_DOS_PROFILE_WIN16_CP437   4
#define WIN_DOS_PROFILE_OS2_16_CP850  5
#define WIN_DOS_PROFILE_OS2_FAT_CP850 6
#define WIN_DOS_PROFILE_CP850_LFN      7

static int win_dos_profile_codepage(int profile)
{
    if (profile == WIN_DOS_PROFILE_CP437_83 ||
        profile == WIN_DOS_PROFILE_WIN16_CP437)
        return BP_CP_437;
    return profile == WIN_DOS_PROFILE_NONE ? BP_CP_INVALID : BP_CP_850;
}

static bp_uint8 win_dos_profile_metadata(int profile)
{
    if (profile == WIN_DOS_PROFILE_WIN16_CP437) return BP_OS_WIN16;
    if (profile == WIN_DOS_PROFILE_OS2_16_CP850 ||
        profile == WIN_DOS_PROFILE_OS2_FAT_CP850) return BP_OS_OS2;
    return BP_OS_DOS;
}

static const char *win_dos_profile_label(int profile)
{
    switch (profile) {
    case WIN_DOS_PROFILE_CP850_83: return "DOS CP850 8.3";
    case WIN_DOS_PROFILE_CP437_83: return "DOS CP437 8.3";
    case WIN_DOS_PROFILE_DOS16_CP850: return "DOS16 CP850 mini";
    case WIN_DOS_PROFILE_WIN16_CP437: return "Win16 CP437 mini";
    case WIN_DOS_PROFILE_OS2_16_CP850: return "OS/2-16 CP850 mini";
    case WIN_DOS_PROFILE_OS2_FAT_CP850: return "OS/2 FAT CP850";
    case WIN_DOS_PROFILE_CP850_LFN: return "DOS CP850 LFN";
    default: return "DOS-family 8.3";
    }
}

static int win_hfs_path_compare(const void *left, const void *right)
{
    const win_hfs_path_t *a = (const win_hfs_path_t *)left;
    const win_hfs_path_t *b = (const win_hfs_path_t *)right;
    return strcmp(a->key, b->key);
}

static int win_legacy_register_name(win_hfs_path_t *items, bp_uint32 *count,
                                    const char *utf8_name, bp_uint8 type,
                                    bp_uint64 file_size, int codepage,
                                    size_t max_component, size_t max_path)
{
    char native[4096];
    char folded[4096];
    size_t n, component = 0, i;
    int rc;
    if (type != BP_TYPE_DATA && type != BP_TYPE_DIR) return BP_ERR_PARTIAL;
    if (file_size.hi != 0 || file_size.lo > 0x7FFFFFFFUL)
        return BP_ERR_OVERFLOW;
    if (utf8_name[0] == '/' || !bp_name_is_safe(utf8_name))
        return BP_ERR_INVALID_ARG;
    rc = bp_utf8_to_local(utf8_name, native, (int)sizeof(native),
                          codepage);
    if (rc != 0) return rc == BP_CP_NO_SPACE ? BP_ERR_NAME_TOO_LONG :
                                               BP_ERR_INVALID_ARG;
    n = strlen(native);
    if (type == BP_TYPE_DIR && n > 0 && native[n - 1] == '/')
        native[--n] = '\0';
    if (n == 0 || n > max_path) return BP_ERR_NAME_TOO_LONG;
    for (i = 0; i <= n; i++) {
        unsigned char ch = (unsigned char)native[i];
        if (ch == '/' || ch == '\0') {
            if (component == 0 || component > max_component)
                return BP_ERR_NAME_TOO_LONG;
            if ((component == 1 && native[i - 1] == '.') ||
                (component == 2 && native[i - 2] == '.' &&
                 native[i - 1] == '.')) return BP_ERR_INVALID_ARG;
            component = 0;
        } else {
            if (ch < 0x20 || ch == 0x7F || ch == ':' || ch == '\\')
                return BP_ERR_INVALID_ARG;
            component++;
        }
    }
    if (bp_local_casefold(native, folded, (int)sizeof(folded),
                          codepage) != 0) return BP_ERR_INVALID_ARG;
    if (*count >= BP_PROFILE1_MAX_ENTRIES) return BP_ERR_OVERFLOW;
    items[*count].key = dup_string(folded);
    if (items[*count].key == NULL) return BP_ERR_NOMEM;
    items[*count].type = type;
    (*count)++;
    return BP_OK;
}

static int win_legacy_paths_check(win_hfs_path_t *items, bp_uint32 count,
                                  const char *profile)
{
    bp_uint32 i;
    int rc = BP_OK;
    qsort(items, count, sizeof(win_hfs_path_t), win_hfs_path_compare);
    for (i = 1; i < count; i++) {
        if (strcmp(items[i - 1].key, items[i].key) == 0) {
            fprintf(stderr,
                    "boltpack: %s profile found a projected case-fold collision\n",
                    profile);
            return BP_ERR_INVALID_ARG;
        }
    }
    for (i = 0; i < count; i++) {
        char prefix[4096];
        char *slash;
        size_t n = strlen(items[i].key);
        if (n >= sizeof(prefix)) { rc = BP_ERR_NAME_TOO_LONG; break; }
        strcpy(prefix, items[i].key);
        for (slash = strchr(prefix, '/'); slash != NULL;
             slash = strchr(slash + 1, '/')) {
            bp_uint32 lo = 0, hi = count;
            int parent = -1;
            *slash = '\0';
            while (lo < hi) {
                bp_uint32 mid = lo + (hi - lo) / 2;
                int cmp = strcmp(items[mid].key, prefix);
                if (cmp == 0) { parent = (int)mid; break; }
                if (cmp < 0) lo = mid + 1;
                else hi = mid;
            }
            *slash = '/';
            if (parent >= 0 && items[parent].type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: %s profile found a file/directory path conflict\n",
                        profile);
                rc = BP_ERR_INVALID_ARG;
                break;
            }
        }
        if (rc != BP_OK) break;
    }
    return rc;
}

static int win_dos_component_83(const char *component, size_t length)
{
    size_t stem = 0, extension = 0, i;
    char reserved[9];
    if (length == 0 || component[length - 1U] == ' ' ||
        component[length - 1U] == '.') return 0;
    while (stem < length && component[stem] != '.') stem++;
    if (stem == 0 || stem > 8U) return 0;
    if (stem < length) {
        if (stem + 1U == length) return 0;
        extension = length - stem - 1U;
        if (extension > 3U ||
            memchr(component + stem + 1U, '.', extension) != NULL) return 0;
    }
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)component[i];
        if (c == '.') continue;
        if (c < 0x20U || c == 0x7FU) return 0;
        if (c >= 0x80U) continue;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || strchr("$%'-_@~`!(){}^#&", c)))
            return 0;
    }
    if (stem <= 4U) {
        for (i = 0; i < stem; i++) {
            unsigned char c = (unsigned char)component[i];
            if (c >= 0x80U) return 1;
            reserved[i] = (char)(c >= 'a' && c <= 'z'
                               ? c - 'a' + 'A' : c);
        }
        reserved[stem] = '\0';
        if (strcmp(reserved, "CON") == 0 || strcmp(reserved, "PRN") == 0 ||
            strcmp(reserved, "AUX") == 0 || strcmp(reserved, "NUL") == 0 ||
            (stem == 4U && reserved[3] >= '1' && reserved[3] <= '9' &&
             (memcmp(reserved, "COM", 3) == 0 ||
              memcmp(reserved, "LPT", 3) == 0))) return 0;
    }
    return 1;
}

static int win_dos_83_add_name(win_hfs_path_t *items, bp_uint32 *count,
                               const char *utf8_name, bp_uint8 type,
                               bp_uint64 file_size, int codepage)
{
    char local[260];
    size_t length, i, start = 0;
    int rc;
    if (bp_utf8_to_local(utf8_name, local, (int)sizeof(local), codepage) != 0)
        return BP_ERR_INVALID_ARG;
    length = strlen(local);
    if (type == BP_TYPE_DIR && length != 0 && local[length - 1U] == '/')
        local[--length] = '\0';
    if (length == 0 || length >= 260U || local[0] == '/')
        return BP_ERR_NAME_TOO_LONG;
    for (i = 0; i <= length; i++) {
        size_t component_length;
        if (i != length && local[i] != '/') {
            if (local[i] == '\\' || local[i] == ':') return BP_ERR_INVALID_ARG;
            continue;
        }
        component_length = i - start;
        if (!win_dos_component_83(local + start, component_length))
            return BP_ERR_INVALID_ARG;
        start = i + 1U;
    }
    rc = win_legacy_register_name(items, count, utf8_name, type, file_size,
                                  codepage, 12U, 259U);
    return rc;
}

static int win_dos_lfn_component_valid(const char *component, size_t length)
{
    size_t stem = 0, i;
    char reserved[9];
    if (length == 0 || length > 255U || component[length - 1U] == '.' ||
        component[length - 1U] == ' ') return 0;
    for (i = 0; i < length; i++) {
        unsigned char c = (unsigned char)component[i];
        if (c < 0x20U || c == 0x7FU || c == '\\' ||
            strchr("\"*:<>?|", c) != NULL ||
            (c == ' ' && (i + 1U == length || component[i + 1U] == '.')))
            return 0;
    }
    while (stem < length && component[stem] != '.') stem++;
    if (stem <= 8U) {
        for (i = 0; i < stem; i++) {
            unsigned char c = (unsigned char)component[i];
            reserved[i] = (char)(c >= 'a' && c <= 'z'
                               ? c - 'a' + 'A' : c);
        }
        reserved[stem] = '\0';
        if (strcmp(reserved, "CON") == 0 || strcmp(reserved, "PRN") == 0 ||
            strcmp(reserved, "AUX") == 0 || strcmp(reserved, "NUL") == 0 ||
            (stem == 4U && reserved[3] >= '1' && reserved[3] <= '9' &&
             (memcmp(reserved, "COM", 3) == 0 ||
              memcmp(reserved, "LPT", 3) == 0))) return 0;
    }
    return 1;
}

static int win_dos_lfn_add_name(win_hfs_path_t *items, bp_uint32 *count,
                                const char *utf8_name, bp_uint8 type,
                                bp_uint64 file_size, int codepage)
{
    char local[4096];
    size_t length, start, i;
    if (bp_utf8_to_local(utf8_name, local, (int)sizeof(local), codepage) != 0)
        return BP_ERR_INVALID_ARG;
    length = strlen(local);
    if (type == BP_TYPE_DIR && length != 0 && local[length - 1U] == '/')
        local[--length] = '\0';
    if (length == 0 || length > 259U || local[0] == '/')
        return BP_ERR_NAME_TOO_LONG;
    for (start = 0, i = 0; i <= length; i++) {
        if (i == length || local[i] == '/') {
            if (!win_dos_lfn_component_valid(local + start, i - start))
                return BP_ERR_INVALID_ARG;
            start = i + 1U;
        }
    }
    return win_legacy_register_name(items, count, utf8_name, type, file_size,
                                    codepage, 255U, 259U);
}

static int win_dos_83_profile_preflight(const add_job_list_t *jobs,
                                        const bp_archive_t *existing,
                                        int codepage,
                                        bp_uint8 metadata_platform,
                                        const char *profile,
                                        int long_names)
{
    win_hfs_path_t *items;
    bp_uint32 count = 0, i;
    int rc = BP_OK;
    items = (win_hfs_path_t *)calloc(BP_PROFILE1_MAX_ENTRIES,
                                     sizeof(win_hfs_path_t));
    if (items == NULL) return BP_ERR_NOMEM;
    if (existing != NULL) {
        for (i = 0; i < existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            if (entry->entry_type == BP_TYPE_META) {
                if (entry->platform == BP_META_PORTABLE ||
                    entry->platform == metadata_platform) continue;
                fprintf(stderr,
                        "boltpack: existing archive has metadata unsupported by %s: %s\n",
                        profile, entry->name);
                rc = BP_ERR_PARTIAL;
                goto win_dos_done;
            }
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: %s does not support this existing entry type: %s\n",
                        profile, entry->name);
                rc = BP_ERR_PARTIAL;
                goto win_dos_done;
            }
            rc = long_names
               ? win_dos_lfn_add_name(items, &count, entry->name,
                                      entry->entry_type,
                                      entry->original_size, codepage)
               : win_dos_83_add_name(items, &count, entry->name,
                                     entry->entry_type,
                                     entry->original_size, codepage);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: existing entry violates %s: %s\n",
                        profile, entry->name);
                goto win_dos_done;
            }
        }
    }
    if (jobs != NULL) {
        int j;
        for (j = 0; j < jobs->count; j++) {
            const add_job_t *job = &jobs->items[j];
            if (job->entry_type != BP_TYPE_DATA &&
                job->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: %s supports only regular files and directories: %s\n",
                        profile, job->path);
                rc = BP_ERR_PARTIAL;
                goto win_dos_done;
            }
            rc = long_names
               ? win_dos_lfn_add_name(items, &count, job->arcname,
                                      job->entry_type, job->file_size,
                                      codepage)
               : win_dos_83_add_name(items, &count, job->arcname,
                                     job->entry_type, job->file_size,
                                     codepage);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: source entry violates %s: %s\n",
                        profile, job->path);
                goto win_dos_done;
            }
        }
    }
    rc = win_legacy_paths_check(items, count, profile);

win_dos_done:
    for (i = 0; i < count; i++) free(items[i].key);
    free(items);
    return rc;
}

static int win_cpm86_profile_preflight(add_job_list_t *jobs,
                                       const bp_archive_t *existing)
{
    char names[256][BP_CPM86_NAME_SIZE];
    bp_uint32 count = 0, i, j;
    int k;
    if (existing != NULL) {
        if (existing->entries_written > 256U ||
            (existing->method != BP_METHOD_STORED &&
             existing->method != BP_METHOD_BOLTLITE) ||
            existing->block_size > BP_BLOCK_32K)
            return BP_ERR_OVERFLOW;
        for (i = 0; i < existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            if (entry->entry_type != BP_TYPE_DATA || entry->flags != 0 ||
                !bp_u64_is_zero(entry->mtime) ||
                (entry->method != BP_METHOD_STORED &&
                 entry->method != BP_METHOD_BOLTLITE) ||
                entry->block_size > BP_BLOCK_32K ||
                (entry->block_size == 0 &&
                 entry->original_size.lo > BP_BLOCK_32K) ||
                entry->original_size.hi != 0 ||
                entry->original_size.lo > 0x7FFFFFFFUL ||
                strchr(entry->name, '/') != NULL ||
                strchr(entry->name, '\\') != NULL ||
                strchr(entry->name, ':') != NULL ||
                !bp_cpm86_project_name(entry->name, names[count]) ||
                strcmp(entry->name, names[count]) != 0) {
                fprintf(stderr, "boltpack: existing entry violates CP/M-86 profile: %s\n",
                        entry->name);
                return BP_ERR_PARTIAL;
            }
            count++;
        }
    }
    if (jobs == NULL) return BP_OK;
    if ((bp_uint32)jobs->count > 256U - count) return BP_ERR_OVERFLOW;
    for (k = 0; k < jobs->count; k++) {
        add_job_t *job = &jobs->items[k];
        char projected[BP_CPM86_NAME_SIZE];
        char *copy;
        if (job->entry_type != BP_TYPE_DATA ||
            job->file_size.hi != 0 || job->file_size.lo > 0x7FFFFFFFUL ||
            strchr(job->arcname, '/') != NULL ||
            strchr(job->arcname, '\\') != NULL ||
            strchr(job->arcname, ':') != NULL ||
            !bp_cpm86_project_name(job->arcname, projected)) {
            fprintf(stderr, "boltpack: source is not a regular flat CP/M-86 8.3 file: %s\n",
                    job->path);
            return BP_ERR_PARTIAL;
        }
        for (j = 0; j < count; j++) {
            if (strcmp(projected, names[j]) == 0) {
                fprintf(stderr, "boltpack: CP/M-86 projected name collision: %s\n",
                        job->path);
                return BP_ERR_INVALID_ARG;
            }
        }
        strcpy(names[count++], projected);
        copy = dup_string(projected);
        if (copy == NULL) return BP_ERR_NOMEM;
        free(job->arcname);
        job->arcname = copy;
        job->mtime = bp_u64_from_u32(0);
        job->mtime_flags = 0;
    }
    return BP_OK;
}

static int win_cpm86_archive_size_check(FILE *fp)
{
    __int64 end, current;
    if (fflush(fp) != 0) return BP_ERR_IO;
    current = _ftelli64(fp);
    if (current < 0 || _fseeki64(fp, 0, SEEK_END) != 0)
        return BP_ERR_IO;
    end = _ftelli64(fp);
    if (end < 0) return BP_ERR_IO;
    if (_fseeki64(fp, current, SEEK_SET) != 0) return BP_ERR_IO;
    return (unsigned __int64)end <= 0x7FFFFFFFULL
         ? BP_OK : BP_ERR_OVERFLOW;
}

/* Windows is a full creator too, so its CLI can prepare Classic HFS archives
 * without requiring a Linux round trip. Keys use exact MacRoman projection
 * and case folding; the archive retains canonical UTF-8 names. */
static int win_classic_hfs_profile_preflight(const add_job_list_t *jobs,
                                             const bp_archive_t *existing)
{
    win_hfs_path_t *items;
    bp_uint32 count = 0, i;
    int rc = BP_OK;
    items = (win_hfs_path_t *)calloc(BP_PROFILE1_MAX_ENTRIES,
                                     sizeof(win_hfs_path_t));
    if (items == NULL) return BP_ERR_NOMEM;
    if (existing != NULL) {
        for (i = 0; i < existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: Classic HFS profile refuses links, resource forks, and metadata entries: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto win_hfs_done;
            }
            rc = win_legacy_register_name(items, &count, entry->name,
                                          entry->entry_type,
                                          entry->original_size,
                                          BP_CP_MACROMAN, 31U, 255U);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: existing entry violates Classic HFS profile: %s\n",
                        entry->name);
                goto win_hfs_done;
            }
        }
    }
    if (jobs != NULL) {
        int j;
        for (j = 0; j < jobs->count; j++) {
            const add_job_t *job = &jobs->items[j];
            DWORD attributes = job->source_info_valid
                             ? job->source_info.dwFileAttributes
                             : GetFileAttributesA(job->path);
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                rc = BP_ERR_IO;
                goto win_hfs_done;
            }
            if ((attributes & (FILE_ATTRIBUTE_READONLY |
                               FILE_ATTRIBUTE_HIDDEN |
                               FILE_ATTRIBUTE_SYSTEM)) != 0) {
                fprintf(stderr,
                        "boltpack: Classic HFS profile cannot preserve source FAT attributes: %s\n",
                        job->path);
                rc = BP_ERR_PARTIAL;
                goto win_hfs_done;
            }
            rc = win_legacy_register_name(items, &count, job->arcname,
                                          job->entry_type, job->file_size,
                                          BP_CP_MACROMAN, 31U, 255U);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: source entry violates Classic HFS profile: %s\n",
                        job->path);
                goto win_hfs_done;
            }
        }
    }
    rc = win_legacy_paths_check(items, count, "Classic HFS");

win_hfs_done:
    for (i = 0; i < count; i++) free(items[i].key);
    free(items);
    return rc;
}

/* AmigaOS 68k has a Latin-1 name space and its own native metadata namespace.
 * Windows source FAT metadata is not meaningful there, so the profile omits
 * it and refuses source attributes it cannot represent. */
static int win_amiga_profile_preflight(const add_job_list_t *jobs,
                                       const bp_archive_t *existing)
{
    win_hfs_path_t *items;
    bp_uint32 count = 0, i;
    int rc = BP_OK;
    items = (win_hfs_path_t *)calloc(BP_PROFILE1_MAX_ENTRIES,
                                     sizeof(win_hfs_path_t));
    if (items == NULL) return BP_ERR_NOMEM;
    if (existing != NULL) {
        for (i = 0; i < existing->entries_written; i++) {
            const bp_entry_t *entry = &existing->entry_list[i];
            if (entry->entry_type == BP_TYPE_META &&
                entry->platform == BP_OS_AMIGA_68K) continue;
            if (entry->entry_type != BP_TYPE_DATA &&
                entry->entry_type != BP_TYPE_DIR) {
                fprintf(stderr,
                        "boltpack: AmigaOS 68k profile refuses foreign metadata, resource forks, and links: %s\n",
                        entry->name);
                rc = BP_ERR_PARTIAL;
                goto win_amiga_done;
            }
            rc = win_legacy_register_name(items, &count, entry->name,
                                          entry->entry_type,
                                          entry->original_size,
                                          BP_CP_8859_1, 30U, 255U);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: existing entry violates AmigaOS 68k profile: %s\n",
                        entry->name);
                goto win_amiga_done;
            }
        }
    }
    if (jobs != NULL) {
        int j;
        for (j = 0; j < jobs->count; j++) {
            const add_job_t *job = &jobs->items[j];
            DWORD attributes = job->source_info_valid
                             ? job->source_info.dwFileAttributes
                             : GetFileAttributesA(job->path);
            if (attributes == INVALID_FILE_ATTRIBUTES) {
                rc = BP_ERR_IO;
                goto win_amiga_done;
            }
            if ((attributes & (FILE_ATTRIBUTE_READONLY |
                               FILE_ATTRIBUTE_HIDDEN |
                               FILE_ATTRIBUTE_SYSTEM)) != 0) {
                fprintf(stderr,
                        "boltpack: AmigaOS 68k profile cannot preserve source FAT attributes: %s\n",
                        job->path);
                rc = BP_ERR_PARTIAL;
                goto win_amiga_done;
            }
            rc = win_legacy_register_name(items, &count, job->arcname,
                                          job->entry_type, job->file_size,
                                          BP_CP_8859_1, 30U, 255U);
            if (rc != BP_OK) {
                fprintf(stderr,
                        "boltpack: source entry violates AmigaOS 68k profile: %s\n",
                        job->path);
                goto win_amiga_done;
            }
        }
    }
    rc = win_legacy_paths_check(items, count, "AmigaOS 68k");

win_amiga_done:
    for (i = 0; i < count; i++) free(items[i].key);
    free(items);
    return rc;
}

/* Validate every primary path before extraction creates or replaces anything.
 * The streaming extractor also registers each path as a defense in depth, but
 * that check alone can discover a later case alias after an earlier entry has
 * already been published. */
static int win_preflight_path(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    const char *name;
    int rc;

    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR)
        return 0;
    name = extract_entry_name(entry, ctx);
    if (!bp_name_is_safe(name)) {
        fprintf(stderr, "boltpack: unsafe Windows destination name: %s\n",
                entry->name);
        return 1;
    }
    rc = win_register_output_path(ctx, name, entry->entry_type);
    if (rc == BP_ERR_PARTIAL) {
        fprintf(stderr, "boltpack: destination name collision: %s\n", name);
        return 1;
    }
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: unsupported Windows filename: %s\n", name);
        return 1;
    }
    return 0;
}

static int win_preflight_archive_paths(bp_io_t *aio, extract_ctx_t *ctx)
{
    int rc = bp_archive_list(aio, win_preflight_path, ctx);
    if (rc == BP_OK && aio->seek(aio->handle, 0, SEEK_SET) != 0)
        rc = BP_ERR_IO;
    return rc;
}

typedef struct {
    bp_io_t io;
    bp_uint8 data[BP_META_MAX_SIZE];
    unsigned length;
} win_metadata_sink_t;

typedef struct win_dir_attr_s {
    struct win_dir_attr_s *next;
    bp_uint8 valid;
    bp_uint8 value;
    bp_uint64 mtime;
    int mtime_valid;
    char path[BP_MAX_PATH];
} win_dir_attr_t;

typedef struct {
    bp_io_t io;
    char target[BP_MAX_PATH];
    char temporary[BP_MAX_PATH];
    int force;
} win_staged_file_t;

static long win_metadata_write(void *handle, const void *data, bp_uint32 size)
{
    win_metadata_sink_t *sink = (win_metadata_sink_t *)handle;
    if (size > BP_META_MAX_SIZE - sink->length) return -1;
    memcpy(sink->data + sink->length, data, size);
    sink->length += (unsigned)size;
    return (long)size;
}

static int win_apply_fat_attributes(const char *path, bp_uint8 valid,
                                    bp_uint8 value);

static LONG g_extract_temp_counter = 0;

static int win_open_staged_file(const char *target, char *temporary,
                                size_t temporary_size, FILE **file)
{
    char directory[BP_MAX_PATH];
    char *slash1, *slash2, *slash;
    size_t length;
    unsigned attempt;
    HANDLE handle;
    int fd;
    FILE *fp;
    if (strlen(target) >= sizeof(directory) || temporary_size < BP_MAX_PATH)
        return BP_ERR_NAME_TOO_LONG;
    strcpy(directory, target);
    slash1 = strrchr(directory, '\\');
    slash2 = strrchr(directory, '/');
    slash = slash1;
    if (slash2 != NULL && (slash == NULL || slash2 > slash)) slash = slash2;
    if (slash == NULL) strcpy(directory, ".");
    else {
        length = (size_t)(slash - directory);
        if (length == 0) length = 1;
        else if (length == 2 && directory[1] == ':') length = 3;
        directory[length] = '\0';
    }
    for (attempt = 0; attempt < 256U; attempt++) {
        unsigned long serial = (unsigned long)GetTickCount() ^
            ((unsigned long)GetCurrentProcessId() << 16) ^
            (unsigned long)InterlockedIncrement(&g_extract_temp_counter) ^ attempt;
        int written = _snprintf(temporary, temporary_size,
                                "%s\\bp%08lx.tmp", directory, serial);
        if (written < 0 || (size_t)written >= temporary_size) return BP_ERR_NAME_TOO_LONG;
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

static int win_copy_directory_path(char *out, size_t out_size,
                                   const char *path)
{
    size_t length = strlen(path);
    if (length > 0 &&
        (path[length - 1] == '/' || path[length - 1] == '\\') &&
        length != 1 && !(length == 3 && path[1] == ':'))
        length--;
    if (length >= out_size) return BP_ERR_NAME_TOO_LONG;
    memcpy(out, path, length);
    out[length] = '\0';
    return BP_OK;
}

static int win_queue_directory_attributes(extract_ctx_t *ctx,
                                          const char *path,
                                          bp_uint8 valid, bp_uint8 value)
{
    char normalized[BP_MAX_PATH];
    win_dir_attr_t *node, **at;
    size_t length;
    int rc = win_copy_directory_path(normalized, sizeof(normalized), path);
    if (rc != BP_OK) return rc;
    length = strlen(normalized);
    for (node = ctx->directories; node != NULL; node = node->next) {
        if (strcmp(node->path, normalized) == 0) {
            node->valid = valid;
            node->value = value;
            return BP_OK;
        }
    }
    node = (win_dir_attr_t *)calloc(1, sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    node->valid = valid; node->value = value;
    memcpy(node->path, normalized, length + 1);
    at = &ctx->directories;
    while (*at != NULL && strlen((*at)->path) >= length) at = &(*at)->next;
    node->next = *at; *at = node;
    return BP_OK;
}

static int win_queue_directory_time(extract_ctx_t *ctx, const char *path,
                                    bp_uint64 mtime, int valid)
{
    char normalized[BP_MAX_PATH];
    win_dir_attr_t *node, **at;
    size_t length;
    int rc = win_copy_directory_path(normalized, sizeof(normalized), path);
    if (rc != BP_OK) return rc;
    length = strlen(normalized);
    for (node = ctx->directories; node != NULL; node = node->next) {
        if (strcmp(node->path, normalized) == 0) {
            node->mtime = mtime;
            node->mtime_valid = valid;
            return BP_OK;
        }
    }
    node = (win_dir_attr_t *)calloc(1, sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    memcpy(node->path, normalized, length + 1);
    node->mtime = mtime;
    node->mtime_valid = valid;
    at = &ctx->directories;
    while (*at != NULL && strlen((*at)->path) >= length) at = &(*at)->next;
    node->next = *at; *at = node;
    return BP_OK;
}

static int win_set_path_mtime(const char *path, bp_uint64 mtime)
{
    const ULONGLONG ticks_per_second = 10000000ULL;
    const ULONGLONG unix_epoch_filetime = 11644473600ULL * 10000000ULL;
    bp_off_t seconds = bp_u64_to_off(mtime);
    time_t converted = (time_t)seconds;
    ULONGLONG ticks;
    FILETIME file_time;
    HANDLE handle;
    int result = BP_ERR_PARTIAL;

    if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), mtime) != 0 ||
        (bp_off_t)converted != seconds ||
        (ULONGLONG)seconds >
            ((ULONGLONG)~(ULONGLONG)0 - unix_epoch_filetime) /
                ticks_per_second)
        return BP_ERR_PARTIAL;
    ticks = unix_epoch_filetime + (ULONGLONG)seconds * ticks_per_second;
    file_time.dwLowDateTime = (DWORD)ticks;
    file_time.dwHighDateTime = (DWORD)(ticks >> 32);
    handle = CreateFileA(path, FILE_WRITE_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) return BP_ERR_PARTIAL;
    if (SetFileTime(handle, NULL, NULL, &file_time)) result = BP_OK;
    if (!CloseHandle(handle)) result = BP_ERR_PARTIAL;
    return result;
}

static int win_finish_directory_attributes(extract_ctx_t *ctx, int apply)
{
    win_dir_attr_t *node = ctx->directories;
    int result = BP_OK;
    ctx->directories = NULL;
    while (node != NULL) {
        win_dir_attr_t *next = node->next;
        if (apply) {
            if (node->mtime_valid) {
                if (win_set_path_mtime(node->path, node->mtime) != BP_OK)
                    result = BP_ERR_PARTIAL;
                if (result != BP_OK)
                    fprintf(stderr, "boltpack: directory timestamp not restored: %s\n",
                            node->path);
            }
            if (node->valid != 0 &&
                win_apply_fat_attributes(node->path, node->valid,
                                         node->value) != BP_OK) {
                fprintf(stderr, "boltpack: directory FAT attributes not restored: %s\n",
                        node->path);
                result = BP_ERR_PARTIAL;
            }
        }
        free(node);
        node = next;
    }
    return result;
}

static int win_apply_fat_attributes(const char *path, bp_uint8 valid,
                                    bp_uint8 value)
{
    DWORD old_attributes = GetFileAttributesA(path);
    DWORD desired, actual;
    if (old_attributes == INVALID_FILE_ATTRIBUTES) return BP_ERR_PARTIAL;
    desired = (old_attributes & ~(DWORD)valid) | value;
    if (!SetFileAttributesA(path, desired)) return BP_ERR_PARTIAL;
    actual = GetFileAttributesA(path);
    if (actual == INVALID_FILE_ATTRIBUTES || (actual & valid) != value)
        return BP_ERR_PARTIAL;
    return BP_OK;
}

/* Strip leading '/' from an entry name (tar-style), warning once. */
static const char *extract_entry_name(const bp_entry_t *entry,
                                      extract_ctx_t *ctx)
{
    const char *name = entry->name;

    while (*name == '/') name++;
    if (name != entry->name && !ctx->slash_warned) {
        fprintf(stderr,
                "boltpack: removing leading '/' from member names\n");
        ctx->slash_warned = 1;
    }
    return name;
}

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[BP_MAX_PATH];
    char local_name[BP_MAX_PATH];
    FILE *fp;
    win_staged_file_t *staged;
    const char *name;
    int rc;

    /* Validate on the UTF-8 archive name before any codepage conversion:
     * a hostile archive must not choose where we write.  Rejects ".."
     * components, backslashes, and drive letters. */
    name = extract_entry_name(entry, ctx);
    if (!bp_name_is_safe(name)) {
        fprintf(stderr, "boltpack: skipping entry with unsafe name: %s\n",
                entry->name);
        ctx->unsafe_count++;
        return NULL;
    }

    /* Convert UTF-8 archive name to ACP for filesystem and display */
    if (win_utf8_copy(name, local_name, sizeof(local_name)) != 0) {
        fprintf(stderr, "boltpack: cannot represent archive name: %s\n", name);
        ctx->unsafe_count++;
        return NULL;
    }

    if (ctx->out_dir) {
        path_join(path, sizeof(path), ctx->out_dir, local_name);
    } else {
        strcpy(path, local_name);
    }

    if (entry->entry_type == BP_TYPE_DATA || entry->entry_type == BP_TYPE_DIR) {
        ctx->last_subject_valid = 0;
        ctx->last_subject_ok = 0;
        rc = win_register_output_path(ctx, local_name, entry->entry_type);
        if (rc != BP_OK) {
            if (rc == BP_ERR_PARTIAL)
                fprintf(stderr, "boltpack: destination name collision: %s\n", name);
            else
                fprintf(stderr, "boltpack: unsupported Windows filename: %s\n", name);
            ctx->unsafe_count++;
            return NULL;
        }
        if (strlen(path) >= sizeof(ctx->last_subject_path)) {
            ctx->unsafe_count++;
            return NULL;
        }
        strcpy(ctx->last_subject_path, path);
        ctx->last_subject_type = entry->entry_type;
        ctx->last_subject_valid = 1;
        ctx->last_subject_ok = 0;
    }

    if (entry->entry_type == BP_TYPE_META) {
        win_metadata_sink_t *sink;
        if (entry->platform != BP_META_PORTABLE) {
            ctx->unsafe_count++;
            return NULL;
        }
        if (!ctx->last_subject_valid || !ctx->last_subject_ok ||
            strcmp(ctx->last_subject_path, path) != 0) {
            ctx->unsafe_count++;
            return NULL;
        }
        sink = (win_metadata_sink_t *)calloc(1, sizeof(*sink));
        if (sink == NULL) { ctx->unsafe_count++; return NULL; }
        sink->io.read = NULL; sink->io.write = win_metadata_write;
        sink->io.seek = NULL; sink->io.tell = NULL; sink->io.handle = sink;
        return &sink->io;
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT) {
            printf("   creating: %s\n", local_name);
        }
        if (ctx->dry_run) ctx->last_subject_ok = 1;
        else {
            DWORD attrs;
            if (mkdirs(path) != 0) ctx->unsafe_count++;
            else {
                attrs = GetFileAttributesA(path);
                if (attrs == INVALID_FILE_ATTRIBUTES ||
                    !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
                    (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) ctx->unsafe_count++;
                else if (win_queue_directory_time(ctx, path, entry->mtime,
                                (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) != 0) != BP_OK)
                    ctx->unsafe_count++;
                else ctx->last_subject_ok = 1;
            }
        }
        return NULL;
    }

    /* skip non-data entries not supported on DOS */
    if (entry->entry_type == BP_TYPE_RSRC || entry->entry_type == BP_TYPE_META) {
        if (g_verbosity == VERB_DEFAULT) {
            printf("   skipping: %s [%s - not supported on this platform]\n",
                   local_name,
                   entry->entry_type == BP_TYPE_RSRC ? "resource fork"
                                                     : "metadata");
        }
        ctx->unsafe_count++;
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_SYMLINK) {
        if (g_verbosity == VERB_DEFAULT) {
            printf("   skipping: %s [symlink - not supported on DOS]\n",
                   local_name);
        }
        ctx->unsafe_count++;
        return NULL;
    }

    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT) {
        printf(" extracting: %s\n", local_name);
    }

    if (ctx->dry_run) {
        ctx->last_subject_ok = 1;
        return bp_archive_dry_run_stream();
    }

    /* check for existing file */
    if (!ctx->force) {
        struct _stat64 st_buf;
        if (_stat64(path, &st_buf) == 0) {
            fprintf(stderr,
                    "boltpack: file exists (use -f to overwrite): %s\n", path);
            ctx->unsafe_count++;
            return NULL;
        }
    }

    mkdirs_for_file(path);
    staged = (win_staged_file_t *)calloc(1, sizeof(*staged));
    if (staged == NULL) { ctx->unsafe_count++; return NULL; }
    rc = win_open_staged_file(path, staged->temporary,
                              sizeof(staged->temporary), &fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot create: %s\n", path);
        free(staged);
        ctx->unsafe_count++;
        return NULL;
    }
    strcpy(staged->target, path);
    staged->force = ctx->force;
    bp_io_from_file(&staged->io, fp);
    return &staged->io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char path[BP_MAX_PATH];
    char local_name[BP_MAX_PATH];
    const char *name = extract_entry_name(entry, ctx);
    int result = BP_OK;

    if (entry->entry_type == BP_TYPE_META) {
        win_metadata_sink_t *sink = (win_metadata_sink_t *)out;
        bp_portable_metadata_t parsed;
        int parse_rc;
        if (status != BP_OK) { free(sink); return status; }
        parse_rc = bp_metadata_parse(sink->data, sink->length, &parsed);
        if (parse_rc == BP_ERR_CORRUPT) { free(sink); return parse_rc; }
        if (!bp_metadata_matches_subject(&parsed, ctx->last_subject_type)) {
            free(sink);
            return BP_ERR_PARTIAL;
        }
        if (win_utf8_copy(name, local_name, sizeof(local_name)) != 0) {
            free(sink); return BP_ERR_PARTIAL;
        }
        if (ctx->out_dir) path_join(path, sizeof(path), ctx->out_dir, local_name);
        else strcpy(path, local_name);
        if (parsed.has_fat && !ctx->dry_run) {
            if (parsed.subject_type == BP_TYPE_DIR)
                result = win_queue_directory_attributes(ctx, path,
                                                        parsed.fat_valid,
                                                        parsed.fat_value);
            else
                result = win_apply_fat_attributes(path, parsed.fat_valid,
                                                  parsed.fat_value);
            if (result != BP_OK)
                fprintf(stderr, "boltpack: FAT attributes not restored: %s\n", path);
        }
        free(sink);
        if ((parse_rc == BP_ERR_PARTIAL || parsed.has_unknown) && result == BP_OK)
            result = BP_ERR_PARTIAL;
        return result;
    }

    {
        win_staged_file_t *staged = (win_staged_file_t *)out;
        FILE *fp = (FILE *)out->handle;
        if (win_utf8_copy(name, local_name, sizeof(local_name)) != 0) {
            fclose(fp); DeleteFileA(staged->temporary); free(staged);
            return BP_ERR_PARTIAL;
        }
        if (ctx->out_dir) path_join(path, sizeof(path), ctx->out_dir, local_name);
        else strcpy(path, local_name);
        if (fclose(fp) != 0 && status == BP_OK) status = BP_ERR_IO;
        if (status != BP_OK) {
            DeleteFileA(staged->temporary);
            free(staged);
            return status;
        }

        if (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) {
            bp_off_t seconds = bp_u64_to_off(entry->mtime);
            time_t t = (time_t)seconds;
            struct _stat64 st;
            struct _utimbuf ut;
            if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), entry->mtime) != 0 ||
                (bp_off_t)t != seconds || _stat64(staged->temporary, &st) != 0) {
                fprintf(stderr, "boltpack: cannot restore timestamp: %s\n", path);
                result = BP_ERR_PARTIAL;
            } else {
                ut.actime = (time_t)st.st_atime;
                ut.modtime = t;
                if (utime(staged->temporary, &ut) != 0) {
                    fprintf(stderr, "boltpack: cannot restore timestamp: %s\n", path);
                    result = BP_ERR_PARTIAL;
                }
            }
        }
        if (entry->flags & BP_ENTRY_FLAG_MODE_VALID) result = BP_ERR_PARTIAL;
        if (!MoveFileExA(staged->temporary, path,
                         MOVEFILE_WRITE_THROUGH |
                         (staged->force ? MOVEFILE_REPLACE_EXISTING : 0))) {
            fprintf(stderr, "boltpack: cannot commit extracted file: %s\n", path);
            DeleteFileA(staged->temporary);
            free(staged);
            return BP_ERR_IO;
        }
        ctx->last_subject_ok = 1;
        free(staged);
    }
    return result;
}

#ifdef BOLTPACK_NET
/* Extract an already-open archive (received over the network) using this
 * frontend's own extract callbacks.  Declared in bp_net.h; called by the
 * shared network glue in bp_net_cli.h.  Path names have already been
 * safety-checked by the caller. */
int bp_frontend_extract_io(bp_io_t *aio, const char *out_dir, int force)
{
    extract_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir;
    ctx.force = force;
    ctx.dry_run = 0;
    {
        int rc = win_preflight_archive_paths(aio, &ctx);
        if (rc == BP_OK && out_dir && out_dir[0] && mkdirs(out_dir) != 0)
            rc = BP_ERR_IO;
        if (rc == BP_OK)
            rc = bp_archive_extract_all_ex(aio, extract_open, extract_done,
                                           NULL, &ctx);
        int dir_rc = win_finish_directory_attributes(&ctx,
                              rc == BP_OK || rc == BP_ERR_PARTIAL);
        if (dir_rc != BP_OK && rc == BP_OK) rc = dir_rc;
        if (ctx.unsafe_count != 0 && rc == BP_OK) rc = BP_ERR_PARTIAL;
        win_path_free(ctx.projected_names);
        return rc;
    }
}
#endif

/* ---- test callback ----------------------------------------------------- */

typedef struct {
    bp_uint32 ok_count;
    bp_uint32 fail_count;
} test_ctx_t;

static void test_cb(const bp_entry_t *entry, int result, void *user_data)
{
    test_ctx_t *ctx = (test_ctx_t *)user_data;

    if (g_verbosity < VERB_SILENT) {
        char disp_name[BP_MAX_PATH];
        if (win_utf8_copy(entry->name, disp_name, sizeof(disp_name)) != 0) {
            strncpy(disp_name, entry->name, BP_MAX_PATH - 1);
            disp_name[BP_MAX_PATH - 1] = '\0';
        }
        printf("  testing: %s", disp_name);
        if (result == BP_OK) {
            printf(" OK\n");
        } else {
            printf(" FAILED -- %s\n", bp_strerror(result));
        }
    }

    if (result == BP_OK) {
        ctx->ok_count++;
    } else {
        ctx->fail_count++;
    }
}

/* ---- usage ------------------------------------------------------------- */

static void usage_basic(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- lightweight portable archiver (Windows)\n"
        "%s\n\n"
        "Usage: boltpack <command> [options] archive.blt [files...]\n\n"
        "Commands:\n"
        "  create (c)   Create archive from files\n"
        "  add (a)      Add files to existing archive\n"
        "  extract (x)  Extract archive\n"
        "  list (l)     List archive contents\n"
        "  test (t)     Test archive integrity\n"
        "  info (i)     Show archive information\n"
        "  delete (d)   Delete files from archive\n",
        BP_VERSION_MAJOR, BP_VERSION_MINOR, AUTHOR_MESSAGE);
    fprintf(stderr,
        "\nCommon options:\n"
        "  -b SIZE          Block size: 8k, 16k, 32k, 64k, 256k\n"
        "  -0               Store only (no compression)\n"
        "  -1               BoltLite compression\n"
        "  -2               boltlz compression (default)\n"
        "  -F               Use fluff compression\n");
#ifdef BOLTPACK_ZSTD
    fprintf(stderr, "  -Z               Use zstd compression\n");
#endif
#ifdef BOLTPACK_ZSTD
    fprintf(stderr,
        "  -l N, --level N  Exact codec level (BoltLZ: 1-2; Fluff/zstd: 1-9)\n"
        "  --light          Fast preset (BoltLZ 1; Fluff/zstd 1)\n"
        "  --standard       Default quality preset (BoltLZ 2; Fluff 8; zstd 3)\n"
        "  --best           Highest quality preset (BoltLZ 2; Fluff/zstd 9)\n");
#else
    fprintf(stderr,
        "  -l N, --level N  Exact codec level (BoltLZ: 1-2; Fluff: 1-9)\n"
        "  --light          Fast preset (BoltLZ 1; Fluff 1)\n"
        "  --standard       Default quality preset (BoltLZ 2; Fluff 8)\n"
        "  --best           Highest quality preset (BoltLZ 2; Fluff 9)\n");
#endif
    fprintf(stderr,
        "  --interchange-profile-1  Mini-compatible create/add limits\n"
        "                         (BoltLite or Stored, 16 KiB blocks)\n"
        "  --target-profile=windows-nt-lfn-v1  Check NT path compatibility\n"
        "  --target-profile=macos-classic-hfs-v1  Check Classic HFS MacRoman names and limits\n"
        "  --target-profile=amigaos-68k-latin1  Check AmigaOS 68k Latin-1 names and limits\n"
        "  --target-profile=atari-st  Check Atari ST GEMDOS 8.3 names and limits\n"
        "  --target-profile=sinclair-ql-latin1-v1  Check Sinclair QL names and limits\n"
        "  --target-profile=riscos-latin1-v1  Check RISC OS FileCore names and limits\n"
        "  --target-profile=x68000-sjis-v1  Check Human68k Shift-JIS names and limits\n"
        "  --target-profile=dos-cp850-83  Check DOS CP850 8.3 names and limits\n"
        "  --target-profile=dos-cp437-83  Check DOS CP437 8.3 names and limits\n"
        "  --target-profile=dos16-cp850-mini  Check DOS16 mini-reader limits\n"
        "  --target-profile=win16-cp437-mini  Check Win16 mini-reader limits\n"
        "  --target-profile=os2-16-cp850-mini  Check OS/2-16 mini-reader limits\n"
        "  --target-profile=os2-fat-cp850  Check OS/2 FAT 8.3 names and limits\n"
        "  --target-profile=dos-cp850-lfn-v1  Check DOS CP850 long filenames\n"
        "  --target-profile=cpm86-83  Project flat CP/M-86 uppercase 8.3 names\n"
        "  -o PATH          Output directory for extraction\n"
        "  -f               Force overwrite existing files\n"
        "  -n               Dry run\n"
        "  -q / -qq         Quiet / silent\n");
#ifdef BOLTPACK_FAST_FLUFF
    fprintf(stderr, "  --single-thread  Disable automatic parallel compression\n");
#endif
    fprintf(stderr,
        "\nExamples:\n"
        "  boltpack create backup.blt folder\n"
        "  boltpack extract -o out backup.blt\n");
#ifdef BOLTPACK_FAST_FLUFF
    fprintf(stderr, "  boltpack create -F --single-thread backup.blt folder\n");
#endif
    fprintf(stderr,
        "\nFor advanced compression options, run: boltpack help advanced\n");
#ifdef BOLTPACK_NET
    bp_net_cli_usage(stderr);
#endif
}

static void usage_advanced(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- advanced Windows CLI help\n"
        "%s\n\n"
        "Usage: boltpack <command> [options] archive.blt [files...]\n\n"
        "Commands:\n"
        "  create (c)       Create archive from files\n"
        "  add (a)          Add files to existing archive\n"
        "  extract (x)      Extract archive\n"
        "  list (l)         List archive contents\n"
        "  test (t)         Test archive integrity\n"
        "  info (i)         Show archive information\n"
        "  delete (d)       Delete files from archive\n",
        BP_VERSION_MAJOR, BP_VERSION_MINOR, AUTHOR_MESSAGE);
    fprintf(stderr, "\nOptions:\n");
#ifdef BOLTPACK_ZSTD
    fprintf(stderr,
        "  -b SIZE          Block size: 8k, 16k, 32k, 64k, 256k (default: 32k, 256k with -F/-Z)\n");
#else
    fprintf(stderr,
        "  -b SIZE          Block size: 8k, 16k, 32k, 64k, 256k (default: 32k, 256k with -F)\n");
#endif
    fprintf(stderr,
        "  -0               Store only (no compression)\n"
        "  -1               BoltLite compression\n"
        "  -2               boltlz compression (default)\n"
        "  -F               Use fluff compression (defaults to 256k blocks)\n");
#ifdef BOLTPACK_ZSTD
    fprintf(stderr,
        "  -Z               Use zstd compression (defaults to 256k blocks, method 4)\n");
#endif
#ifdef BOLTPACK_ZSTD
    fprintf(stderr,
        "  -l N, --level N  Exact codec level (BoltLZ: 1-2; Fluff: 1-9, 8=default; zstd: 1-9, 3=default)\n"
        "  --light          Fast preset (BoltLZ 1; Fluff/zstd 1)\n"
        "  --standard       Default quality preset (BoltLZ 2; Fluff 8; zstd 3)\n"
        "  --best           Highest quality preset (BoltLZ 2; Fluff/zstd 9)\n");
#else
    fprintf(stderr,
        "  -l N, --level N  Exact codec level (BoltLZ: 1-2; Fluff: 1-9, 8=default)\n"
        "  --light          Fast preset (BoltLZ 1; Fluff 1)\n"
        "  --standard       Default quality preset (BoltLZ 2; Fluff 8)\n"
        "  --best           Highest quality preset (BoltLZ 2; Fluff 9)\n");
#endif
#ifdef BOLTPACK_FAST_FLUFF
    fprintf(stderr,
        "  -j N             Parallel BoltLZ and fluff"
#ifdef BOLTPACK_ZSTD
        " and zstd"
#endif
        "\n"
        "  -g N             Fluff blocks per history group\n"
        "  --single-thread  Disable automatic parallel compression\n");
#endif
    fprintf(stderr,
        "  --interchange-profile-1  Enforce mini-compatible limits (create/add)\n"
        "  --target-profile=windows-nt-lfn-v1  Check NT path compatibility\n"
        "  --target-profile=macos-classic-hfs-v1  Check Classic HFS MacRoman names and limits\n"
        "  --target-profile=amigaos-68k-latin1  Check AmigaOS 68k Latin-1 names and limits\n"
        "  --target-profile=atari-st  Check Atari ST GEMDOS 8.3 names and limits\n"
        "  --target-profile=sinclair-ql-latin1-v1  Check Sinclair QL names and limits\n"
        "  --target-profile=riscos-latin1-v1  Check RISC OS FileCore names and limits\n"
        "  --target-profile=x68000-sjis-v1  Check Human68k Shift-JIS names and limits\n"
        "  --target-profile=dos-cp850-83  Check DOS CP850 8.3 names and limits\n"
        "  --target-profile=dos-cp437-83  Check DOS CP437 8.3 names and limits\n"
        "  --target-profile=dos16-cp850-mini  Check DOS16 mini-reader limits\n"
        "  --target-profile=win16-cp437-mini  Check Win16 mini-reader limits\n"
        "  --target-profile=os2-16-cp850-mini  Check OS/2-16 mini-reader limits\n"
        "  --target-profile=os2-fat-cp850  Check OS/2 FAT 8.3 names and limits\n"
        "  --target-profile=dos-cp850-lfn-v1  Check DOS CP850 long filenames\n"
        "  --target-profile=cpm86-83  Project flat CP/M-86 uppercase 8.3 names\n"
        "  -q               Quiet (summary only)\n"
        "  -qq              Silent (errors only)\n"
        "  -o PATH          Output directory for extraction\n"
        "  -f               Force overwrite existing files\n"
        "  -n               Dry run\n");
}

static void usage(void)
{
    usage_basic();
}

/* ---- commands ---------------------------------------------------------- */

static int cmd_create(int argc, char **argv, bp_uint32 block_size,
                      bp_uint8 method, int level, int interchange_profile,
                      int windows_target_profile, int classic_hfs_profile,
                      int amiga_profile, int dos_83_profile)
{
    const char *archive_name;
    add_job_list_t list;
    FILE *fp;
    bp_io_t aio;
    bp_archive_t ar;
    int rc;
    double start;
    double elapsed;
    char tmp_path[BP_MAX_PATH * 2];
    int fd;
    HANDLE h;

#ifdef BOLTPACK_NET
    /* --send/--name may appear among the file arguments; pull them out. */
    bp_net_cli_strip_create_args(&argc, argv);
#endif

    if (argc < 2) {
        fprintf(stderr, "boltpack: create requires archive name and files\n");
        return 1;
    }

    archive_name = argv[0];
    set_tmp_dir_from_archive(archive_name);
    memset(&list, 0, sizeof(list));

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }

    /*
     * Write to a temp file in the same directory as the archive, then
     * MoveFileEx atomically on success.  This ensures a failed or interrupted
     * run never leaves a partial .blt file at the final path.
     */
    if (!GetTempFileNameA(g_tmp_dir[0] ? g_tmp_dir : ".", "bpa", 0, tmp_path)) {
        fprintf(stderr, "boltpack: cannot create temp archive\n");
        return 1;
    }
    h = CreateFileA(tmp_path,
                    GENERIC_READ | GENERIC_WRITE,
                    0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "boltpack: cannot open temp archive: %s\n", tmp_path);
        DeleteFileA(tmp_path);
        return 1;
    }
    fd = _open_osfhandle((intptr_t)h, 0);
    if (fd < 0) {
        fprintf(stderr, "boltpack: _open_osfhandle failed\n");
        CloseHandle(h);
        DeleteFileA(tmp_path);
        return 1;
    }
    fp = _fdopen(fd, "w+b");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: _fdopen failed\n");
        _close(fd);
        DeleteFileA(tmp_path);
        return 1;
    }

    /* Register global path so ctrl handler can delete it on Ctrl+C. */
    memcpy(g_tmp_archive, tmp_path, strlen(tmp_path) + 1);
    SetConsoleCtrlHandler(ctrl_cleanup_handler, TRUE);

    bp_io_from_file(&aio, fp);
    rc = interchange_profile
        ? bp_archive_create_profile(&ar, &aio, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp);
        cleanup_tmp_archive_win();
        SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
        return 1;
    }
    if (interchange_profile && method == BP_METHOD_STORED)
        ar.method = BP_METHOD_STORED;
    if (level > 0) ar.method_level = (bp_uint8)level;
    bp_archive_set_log(&ar, cli_log_stderr, NULL);

    start = wall_time_seconds();

    rc = collect_arg_jobs(&list, argc - 1, argv + 1);
    if (rc != BP_OK) {
        bp_archive_close(&ar);
        fclose(fp);
        cleanup_tmp_archive_win();
        SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
        free_job_list(&list);
        return 1;
    }

    if (windows_target_profile) {
        rc = win_target_profile_preflight(&list, NULL);
        if (rc != BP_OK) {
            bp_archive_close(&ar);
            fclose(fp);
            cleanup_tmp_archive_win();
            SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
            free_job_list(&list);
            return 1;
        }
    }
    if (g_target_atari_st) {
        rc = win_atari_profile_preflight(&list, NULL);
        if (rc != BP_OK) {
            bp_archive_close(&ar);
            fclose(fp);
            cleanup_tmp_archive_win();
            SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
            free_job_list(&list);
            return 1;
        }
    }
    if (g_target_ql) {
        rc = win_ql_profile_preflight(&list, NULL);
        if (rc != BP_OK) {
            bp_archive_close(&ar);
            fclose(fp);
            cleanup_tmp_archive_win();
            SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
            free_job_list(&list);
            return 1;
        }
    }
    if (g_target_riscos) {
        rc = win_riscos_profile_preflight(&list, NULL);
        if (rc != BP_OK) {
            bp_archive_close(&ar);
            fclose(fp);
            cleanup_tmp_archive_win();
            SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
            free_job_list(&list);
            return 1;
        }
    }
    if (g_target_x68000) {
        rc = win_x68000_profile_preflight(&list, NULL);
        if (rc != BP_OK) {
            bp_archive_close(&ar);
            fclose(fp);
            cleanup_tmp_archive_win();
            SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
            free_job_list(&list);
            return 1;
        }
    }
    if (classic_hfs_profile) {
        rc = win_classic_hfs_profile_preflight(&list, NULL);
        if (rc != BP_OK) {
            bp_archive_close(&ar);
            fclose(fp);
            cleanup_tmp_archive_win();
            SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
            free_job_list(&list);
            return 1;
        }
    }
    if (amiga_profile) {
        rc = win_amiga_profile_preflight(&list, NULL);
        if (rc != BP_OK) {
            bp_archive_close(&ar);
            fclose(fp);
            cleanup_tmp_archive_win();
            SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
            free_job_list(&list);
            return 1;
        }
    }
    if (dos_83_profile != WIN_DOS_PROFILE_NONE) {
        int codepage = win_dos_profile_codepage(dos_83_profile);
        bp_uint8 metadata = win_dos_profile_metadata(dos_83_profile);
        const char *profile = win_dos_profile_label(dos_83_profile);
        rc = win_dos_83_profile_preflight(&list, NULL, codepage, metadata,
                                          profile,
                                          dos_83_profile == WIN_DOS_PROFILE_CP850_LFN);
        if (rc != BP_OK) {
            bp_archive_close(&ar);
            fclose(fp);
            cleanup_tmp_archive_win();
            SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
            free_job_list(&list);
            return 1;
        }
    }
    if (g_target_cpm86) {
        rc = win_cpm86_profile_preflight(&list, NULL);
        if (rc != BP_OK) {
            bp_archive_close(&ar);
            fclose(fp);
            cleanup_tmp_archive_win();
            SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
            free_job_list(&list);
            return 1;
        }
    }

    rc = add_job_list_with_policy(&ar, &list, method);
    free_job_list(&list);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: error adding input: %s\n",
                bp_strerror(rc));
        bp_archive_close(&ar);
        fclose(fp);
        cleanup_tmp_archive_win();
        SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
        return 1;
    }

    rc = bp_archive_finalize(&ar);
    if (rc == BP_OK && g_target_cpm86)
        rc = win_cpm86_archive_size_check(fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        bp_archive_close(&ar);
        fclose(fp);
        cleanup_tmp_archive_win();
        SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
        return 1;
    }

    elapsed = wall_time_seconds() - start;

    /* Close the file so MoveFileEx can rename it, but keep ar alive: the
     * summary below reads ar.entry_list, which bp_archive_close frees. */
    fclose(fp);

    /* Atomically publish the completed archive. */
    if (!MoveFileExA(tmp_path, archive_name,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fprintf(stderr, "boltpack: rename failed (error %lu)\n",
                (unsigned long)GetLastError());
        cleanup_tmp_archive_win();
        SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
        bp_archive_close(&ar);
        return 1;
    }
    g_tmp_archive[0] = '\0'; /* rename succeeded -- nothing to clean up */
    SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);

    /* summary line */
    if (g_verbosity < VERB_SILENT) {
        bp_uint64 total_orig = {0, 0};
        bp_uint64 total_comp = {0, 0};
        char numbuf[32], speedbuf[64];
        bp_uint32 file_count = 0, dir_count = 0;
        bp_uint32 j;

        for (j = 0; j < ar.entries_written; j++) {
            if (ar.entry_list[j].entry_type == BP_TYPE_DIR) {
                dir_count++;
            } else {
                file_count++;
            }
            total_orig = bp_u64_add(total_orig, ar.entry_list[j].original_size);
            total_comp = bp_u64_add(total_comp, ar.entry_list[j].compressed_size);
        }

        format_size64(total_orig, numbuf, sizeof(numbuf));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)file_count, file_count == 1 ? "" : "s",
               (unsigned long)dir_count, dir_count == 1 ? "y" : "ies",
               numbuf);

        if (!bp_u64_is_zero(total_orig) && !bp_u64_is_zero(total_comp)) {
            double orig_d = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            double comp_d = (double)total_comp.lo + (double)total_comp.hi * 4294967296.0;
            double pct = (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0;
            char compbuf[32];
            format_size64(total_comp, compbuf, sizeof(compbuf));
            printf(", %.0f%% (%s)", pct, compbuf);
        }

        if (elapsed > 0.0) {
            double orig_d = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            printf(", %.2fs", elapsed);
            format_speed(orig_d / elapsed, speedbuf, sizeof(speedbuf));
            printf(", %s", speedbuf);
        }
        printf("\n");
    }

    bp_archive_close(&ar);

#ifdef BOLTPACK_NET
    {
        int src = bp_net_cli_maybe_send(archive_name);
        if (src != 0) return src;
    }
#endif
    return 0;
}

static int cmd_list(const char *archive_name)
{
    FILE *fp;
    bp_io_t aio;
    int rc;

    fp = fopen(archive_name, "rb");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }

    bp_io_from_file(&aio, fp);

    printf("%10s %10s  %s  %s\n", "Size", "Compressed", "Type", "Name");

    {
        list_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));

        rc = bp_archive_list(&aio, list_cb, &ctx);
        fclose(fp);

        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
            return 1;
        }

        {
            char origbuf[32], compbuf[32];
            format_size64(ctx.total_orig, origbuf, sizeof(origbuf));
            format_size64(ctx.total_comp, compbuf, sizeof(compbuf));
            printf("%lu file%s, %lu director%s, %s",
                   (unsigned long)ctx.file_count,
                   ctx.file_count == 1 ? "" : "s",
                   (unsigned long)ctx.dir_count,
                   ctx.dir_count == 1 ? "y" : "ies",
                   origbuf);
            if (!bp_u64_is_zero(ctx.total_orig) && !bp_u64_is_zero(ctx.total_comp)) {
                double orig_d = (double)ctx.total_orig.lo + (double)ctx.total_orig.hi * 4294967296.0;
                double comp_d = (double)ctx.total_comp.lo + (double)ctx.total_comp.hi * 4294967296.0;
                double pct = (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0;
                printf(", %.0f%% (%s)", pct, compbuf);
            }
            printf("\n");
        }
    }
    return 0;
}

static int cmd_extract(const char *archive_name, const char *out_dir,
                       int force, int dry_run)
{
    FILE *fp;
    bp_io_t aio;
    extract_ctx_t ctx;
    int rc;
    double start;
    double elapsed;

    fp = fopen(archive_name, "rb");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        fclose(fp);
        return 1;
    }

    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir;
    ctx.force = force;
    ctx.dry_run = dry_run;

    rc = win_preflight_archive_paths(&aio, &ctx);
    if (rc != BP_OK) {
        fclose(fp);
        win_path_free(ctx.projected_names);
        fprintf(stderr, "boltpack: archive contains names that cannot be safely extracted: %s\n",
                bp_strerror(rc));
        return 1;
    }
    win_path_free(ctx.projected_names);
    ctx.projected_names = NULL;
    if (out_dir && !dry_run && mkdirs(out_dir) != 0) {
        fclose(fp);
        fprintf(stderr, "boltpack: cannot create output directory: %s\n", out_dir);
        return 1;
    }

    start = wall_time_seconds();

    rc = bp_archive_extract_all_ex(&aio, extract_open, extract_done,
                                   g_verbosity == VERB_DEFAULT ? progress_cb : NULL,
                                   &ctx);
    fclose(fp);

    {
        int dir_rc = win_finish_directory_attributes(&ctx,
                              rc == BP_OK || rc == BP_ERR_PARTIAL);
        if (dir_rc != BP_OK && rc == BP_OK) rc = dir_rc;
    }
    win_path_free(ctx.projected_names);

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }

    elapsed = wall_time_seconds() - start;

    if (g_verbosity < VERB_SILENT) {
        printf("%lu file%s, %lu director%s",
               (unsigned long)ctx.file_count,
               ctx.file_count == 1 ? "" : "s",
               (unsigned long)ctx.dir_count,
               ctx.dir_count == 1 ? "y" : "ies");
        if (elapsed > 0.0) {
            printf(", %.2fs", elapsed);
        }
        printf("\n");
    }

    if (ctx.unsafe_count != 0) {
        fprintf(stderr,
                "boltpack: %lu entr%s skipped or attributes not restored\n",
                (unsigned long)ctx.unsafe_count,
                ctx.unsafe_count == 1 ? "y" : "ies");
        return 1;
    }

    return 0;
}

static int cmd_test(const char *archive_name)
{
    FILE *fp;
    bp_io_t aio;
    test_ctx_t ctx;
    int rc;

    fp = fopen(archive_name, "rb");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        fclose(fp);
        return 1;
    }

    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));

    rc = bp_archive_test(&aio, test_cb, &ctx);
    fclose(fp);

    if (rc != BP_OK && rc != BP_ERR_CHECKSUM) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }

    if (g_verbosity < VERB_SILENT) {
        printf("\n%lu file%s OK",
               (unsigned long)ctx.ok_count,
               ctx.ok_count == 1 ? "" : "s");
        if (ctx.fail_count > 0) {
            printf(", %lu file%s FAILED",
                   (unsigned long)ctx.fail_count,
                   ctx.fail_count == 1 ? "" : "s");
        }
        printf("\n");
    }

    return ctx.fail_count > 0 ? 1 : 0;
}

/* ---- info -------------------------------------------------------------- */

static const char *os_name(bp_uint8 os)
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
    case BP_OS_BEOS:      return "BeOS";
    default:              return "Unknown";
    }
}

static const char *cpu_name(bp_uint8 cpu)
{
    switch (cpu) {
    case BP_CPU_UNKNOWN:  return "Unknown";
    case BP_CPU_X86:      return "x86";
    case BP_CPU_X86_64:   return "x86-64";
    case BP_CPU_PPC32:    return "PowerPC";
    case BP_CPU_PPC64:    return "PowerPC 64";
    case BP_CPU_M68K:     return "Motorola 68k";
    case BP_CPU_ARM32:    return "ARM";
    case BP_CPU_ARM64:    return "ARM64";
    case BP_CPU_MIPS32:   return "MIPS";
    case BP_CPU_MIPS64:   return "MIPS64";
    default:              return "Unknown";
    }
}

static const char *format_methods(const bp_uint32 *counts)
{
    static char buf[128];
    static const char * const names[5] = {"Stored", "BoltLite", "boltlz", "Fluff", "zstd"};
    int pos = 0;
    int first = 1;
    int i;
    buf[0] = '\0';
    for (i = 0; i < 5; i++) {
        if (counts[i] == 0) continue;
        if (!first) { buf[pos++] = ','; buf[pos++] = ' '; }
        pos += sprintf(buf + pos, "%s (%lu)", names[i], (unsigned long)counts[i]);
        first = 0;
    }
    if (first) strcpy(buf, "none");
    return buf;
}

static int cmd_info(const char *archive_name)
{
    FILE *fp;
    bp_io_t aio;
    bp_archive_info_t info;
    int rc;

    fp = fopen(archive_name, "rb");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }

    bp_io_from_file(&aio, fp);
    rc = bp_archive_info(&aio, &info);
    fclose(fp);

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }

    {
        bp_uint32 file_count = info.entry_count - info.dir_count;
        bp_uint64 total_orig = info.total_original;
        bp_uint64 total_comp = info.total_compressed;
        bp_uint64 arc_size = info.archive_size;
        char origbuf[32], compbuf[32], arcbuf[32];

        printf("Archive:        %s\n", archive_name);
        printf("Format version: %u\n", (unsigned)info.version);
        printf("Created by:     BoltPack %u.%u\n",
               (unsigned)info.creator_ver_major,
               (unsigned)info.creator_ver_minor);
        printf("Creator OS:     %s\n", os_name(info.creator_os));
        printf("Creator CPU:    %s\n", cpu_name(info.creator_cpu));
        printf("Method:         %s\n", format_methods(info.method_counts));

        format_size64(arc_size, arcbuf, sizeof(arcbuf));
        printf("Archive size:   %s\n", arcbuf);

        printf("Entries:        %lu (%lu file%s, %lu director%s)\n",
               (unsigned long)info.entry_count,
               (unsigned long)file_count, file_count == 1 ? "" : "s",
               (unsigned long)info.dir_count,
               info.dir_count == 1 ? "y" : "ies");

        format_size64(total_orig, origbuf, sizeof(origbuf));
        format_size64(total_comp, compbuf, sizeof(compbuf));
        printf("Original size:  %s\n", origbuf);
        printf("Compressed:     %s", compbuf);
        if (!bp_u64_is_zero(total_orig)) {
            double orig_d = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            double comp_d = (double)total_comp.lo + (double)total_comp.hi * 4294967296.0;
            double pct = (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0;
            printf(" (%.0f%%)", pct);
        }
        printf("\n");
    }

    return 0;
}

static int cmd_delete(int argc, char **argv)
{
    const char *archive_name;
    const char **del_names;
    int del_count;
    FILE *src_fp;
    FILE *dst_fp;
    HANDLE source_handle;
    HANDLE verify_handle;
    BY_HANDLE_FILE_INFORMATION original_info;
    BY_HANDLE_FILE_INFORMATION current_info;
    bp_io_t src_io, dst_io;
    char tmp_name[BP_MAX_PATH];
    char backup_name[BP_MAX_PATH + 32];
    bp_uint32 deleted = 0;
    int handlers_set = 0;
    int rc;
    int result = 1;

    if (argc < 2) {
        fprintf(stderr, "boltpack: delete requires archive name and file(s)\n");
        return 1;
    }

    archive_name = argv[0];
    del_names = (const char **)&argv[1];
    del_count = argc - 1;

    /* Archive names use forward slashes; normalize user input. */
    {
        int j;
        for (j = 0; j < del_count; j++)
            normalize_separators((char *)del_names[j]);
    }

    source_handle = INVALID_HANDLE_VALUE;
    verify_handle = INVALID_HANDLE_VALUE;
    src_fp = NULL;
    dst_fp = NULL;
    tmp_name[0] = '\0';
    backup_name[0] = '\0';

    if (!win_open_archive_snapshot(archive_name, &source_handle,
                                   &original_info)) {
        fprintf(stderr,
                "boltpack: cannot use archive target (must be a writable, regular, non-reparse, single-link file): %s\n",
                archive_name);
        goto delete_cleanup_win;
    }
    src_fp = fopen(archive_name, "rb");
    if (src_fp == NULL) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        goto delete_cleanup_win;
    }

    rc = win_create_archive_stage(INVALID_HANDLE_VALUE, tmp_name,
                                  sizeof(tmp_name));
    handlers_set = g_tmp_archive[0] != '\0';
    if (!rc) {
        fprintf(stderr, "boltpack: cannot create temporary archive\n");
        goto delete_cleanup_win;
    }
    dst_fp = fopen(tmp_name, "w+b");
    if (dst_fp == NULL) {
        fprintf(stderr, "boltpack: cannot create temp file: %s\n", tmp_name);
        goto delete_cleanup_win;
    }

    bp_io_from_file(&src_io, src_fp);
    bp_io_from_file(&dst_io, dst_fp);

    rc = bp_archive_delete(&src_io, &dst_io, del_names, (bp_uint32)del_count,
                           &deleted);

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: delete failed: %s\n", bp_strerror(rc));
        goto delete_cleanup_win;
    }

    if (deleted == 0) {
        fprintf(stderr, "boltpack: no matching entries found\n");
        goto delete_cleanup_win;
    }

    if (fflush(dst_fp) != 0 || _commit(_fileno(dst_fp)) != 0) {
        fprintf(stderr, "boltpack: cannot flush temporary archive\n");
        goto delete_cleanup_win;
    }
    if (fclose(dst_fp) != 0) {
        dst_fp = NULL;
        fprintf(stderr, "boltpack: cannot close temporary archive\n");
        goto delete_cleanup_win;
    }
    dst_fp = NULL;

    if (!GetFileInformationByHandle(source_handle, &current_info) ||
        !win_archive_info_equal(&original_info, &current_info) ||
        !win_open_archive_snapshot(archive_name, &verify_handle,
                                   &current_info) ||
        !win_archive_info_equal(&original_info, &current_info)) {
        fprintf(stderr, "boltpack: archive changed during delete: %s\n",
                archive_name);
        goto delete_cleanup_win;
    }
    CloseHandle(verify_handle);
    verify_handle = INVALID_HANDLE_VALUE;
    if (fclose(src_fp) != 0) {
        src_fp = NULL;
        fprintf(stderr, "boltpack: cannot close source archive\n");
        goto delete_cleanup_win;
    }
    src_fp = NULL;
    CloseHandle(source_handle);
    source_handle = INVALID_HANDLE_VALUE;

    if (!win_publish_archive_stage(archive_name, tmp_name, backup_name,
                                   sizeof(backup_name),
                                   original_info.dwFileAttributes)) {
        fprintf(stderr, "boltpack: cannot publish deleted archive (error %lu)\n",
                (unsigned long)GetLastError());
        goto delete_cleanup_win;
    }
    result = 0;

delete_cleanup_win:
    if (dst_fp != NULL) fclose(dst_fp);
    if (src_fp != NULL) fclose(src_fp);
    if (verify_handle != INVALID_HANDLE_VALUE) CloseHandle(verify_handle);
    if (source_handle != INVALID_HANDLE_VALUE) CloseHandle(source_handle);
    if (result != 0 && g_tmp_archive[0] != '\0') cleanup_tmp_archive_win();
    if (handlers_set) SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);

    if (result == 0 && g_verbosity < VERB_SILENT) {
        printf("%lu entr%s deleted\n",
               (unsigned long)deleted, deleted == 1 ? "y" : "ies");
    }
    return result;
}

static int cmd_add(int argc, char **argv, bp_uint32 block_size,
                   bp_uint8 method, int level, int interchange_profile,
                   int windows_target_profile, int classic_hfs_profile,
                   int amiga_profile, int dos_83_profile)
{
    const char *archive_name;
    add_job_list_t list;
    FILE *fp;
    bp_io_t io;
    bp_archive_t ar;
    HANDLE source_handle;
    HANDLE verify_handle;
    BY_HANDLE_FILE_INFORMATION original_info;
    BY_HANDLE_FILE_INFORMATION current_info;
    char tmp_name[BP_MAX_PATH];
    char backup_name[BP_MAX_PATH + 32];
    int rc;
    int fin_rc;
    int archive_open;
    int handlers_set;
    int result;

    if (argc < 2) {
        fprintf(stderr, "boltpack: add requires archive name and files\n");
        return 1;
    }

    archive_name = argv[0];
    set_tmp_dir_from_archive(archive_name);
    memset(&list, 0, sizeof(list));
    source_handle = INVALID_HANDLE_VALUE;
    verify_handle = INVALID_HANDLE_VALUE;
    fp = NULL;
    tmp_name[0] = '\0';
    backup_name[0] = '\0';
    archive_open = 0;
    handlers_set = 0;
    result = 1;

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        goto add_cleanup_win;
    }

    if (!win_open_archive_snapshot(archive_name, &source_handle,
                                   &original_info)) {
        fprintf(stderr,
                "boltpack: cannot use archive target (must be a writable, regular, non-reparse, single-link file): %s\n",
                archive_name);
        goto add_cleanup_win;
    }

    rc = win_create_archive_stage(source_handle, tmp_name,
                                  sizeof(tmp_name));
    handlers_set = g_tmp_archive[0] != '\0';
    if (!rc) {
        fprintf(stderr, "boltpack: cannot copy archive to a temporary stage\n");
        goto add_cleanup_win;
    }
    fp = fopen(tmp_name, "r+b");
    if (fp == NULL) {
        fprintf(stderr, "boltpack: cannot open temporary archive: %s\n",
                tmp_name);
        goto add_cleanup_win;
    }

    bp_io_from_file(&io, fp);
    rc = bp_archive_open_append(&ar, &io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        goto add_cleanup_win;
    }
    archive_open = 1;
    if (g_target_cpm86 && win_cpm86_archive_size_check(fp) != BP_OK) {
        fprintf(stderr, "boltpack: existing archive exceeds CP/M-86 2 GiB limit\n");
        goto add_cleanup_win;
    }
    if (interchange_profile) {
        rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
        if (rc != BP_OK) {
            fprintf(stderr,
                    "boltpack: archive does not meet Interchange Profile 1 limits: %s\n",
                    bp_strerror(rc));
            goto add_cleanup_win;
        }
        if (method == BP_METHOD_STORED) ar.method = BP_METHOD_STORED;
    }
    if (level > 0) ar.method_level = (bp_uint8)level;
    bp_archive_set_log(&ar, cli_log_stderr, NULL);

    rc = collect_arg_jobs(&list, argc - 1, argv + 1);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: error preparing input: %s\n",
                bp_strerror(rc));
        goto add_cleanup_win;
    }

    if (windows_target_profile) {
        rc = win_target_profile_preflight(&list, &ar);
        if (rc != BP_OK) goto add_cleanup_win;
    }
    if (g_target_atari_st) {
        rc = win_atari_profile_preflight(&list, &ar);
        if (rc != BP_OK) goto add_cleanup_win;
    }
    if (g_target_ql) {
        rc = win_ql_profile_preflight(&list, &ar);
        if (rc != BP_OK) goto add_cleanup_win;
    }
    if (g_target_riscos) {
        rc = win_riscos_profile_preflight(&list, &ar);
        if (rc != BP_OK) goto add_cleanup_win;
    }
    if (g_target_x68000) {
        rc = win_x68000_profile_preflight(&list, &ar);
        if (rc != BP_OK) goto add_cleanup_win;
    }
    if (classic_hfs_profile) {
        rc = win_classic_hfs_profile_preflight(&list, &ar);
        if (rc != BP_OK) goto add_cleanup_win;
    }
    if (amiga_profile) {
        rc = win_amiga_profile_preflight(&list, &ar);
        if (rc != BP_OK) goto add_cleanup_win;
    }
    if (dos_83_profile != WIN_DOS_PROFILE_NONE) {
        int codepage = win_dos_profile_codepage(dos_83_profile);
        bp_uint8 metadata = win_dos_profile_metadata(dos_83_profile);
        const char *profile = win_dos_profile_label(dos_83_profile);
        rc = win_dos_83_profile_preflight(&list, &ar, codepage, metadata,
                                          profile,
                                          dos_83_profile == WIN_DOS_PROFILE_CP850_LFN);
        if (rc != BP_OK) goto add_cleanup_win;
    }
    if (g_target_cpm86) {
        rc = win_cpm86_profile_preflight(&list, &ar);
        if (rc != BP_OK) goto add_cleanup_win;
    }

    rc = add_job_list_with_policy(&ar, &list, method);
    free_job_list(&list);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: error adding input: %s\n",
                bp_strerror(rc));
        goto add_cleanup_win;
    }

    fin_rc = bp_archive_finalize(&ar);
    if (fin_rc == BP_OK && g_target_cpm86)
        fin_rc = win_cpm86_archive_size_check(fp);
    bp_archive_close(&ar);
    archive_open = 0;

    if (fin_rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(fin_rc));
        goto add_cleanup_win;
    }

    if (fflush(fp) != 0 || _commit(_fileno(fp)) != 0) {
        fprintf(stderr, "boltpack: cannot flush staged archive\n");
        goto add_cleanup_win;
    }
    if (fclose(fp) != 0) {
        fp = NULL;
        fprintf(stderr, "boltpack: cannot close staged archive\n");
        goto add_cleanup_win;
    }
    fp = NULL;

    if (!GetFileInformationByHandle(source_handle, &current_info) ||
        !win_archive_info_equal(&original_info, &current_info) ||
        !win_open_archive_snapshot(archive_name, &verify_handle,
                                   &current_info) ||
        !win_archive_info_equal(&original_info, &current_info)) {
        fprintf(stderr, "boltpack: archive changed during append: %s\n",
                archive_name);
        goto add_cleanup_win;
    }
    CloseHandle(verify_handle);
    verify_handle = INVALID_HANDLE_VALUE;
    CloseHandle(source_handle);
    source_handle = INVALID_HANDLE_VALUE;

    if (!win_publish_archive_stage(archive_name, tmp_name, backup_name,
                                   sizeof(backup_name),
                                   original_info.dwFileAttributes)) {
        fprintf(stderr, "boltpack: cannot publish appended archive (error %lu)\n",
                (unsigned long)GetLastError());
        goto add_cleanup_win;
    }
    result = 0;

add_cleanup_win:
    if (archive_open) bp_archive_close(&ar);
    if (fp != NULL) fclose(fp);
    if (verify_handle != INVALID_HANDLE_VALUE) CloseHandle(verify_handle);
    if (source_handle != INVALID_HANDLE_VALUE) CloseHandle(source_handle);
    if (result != 0 && g_tmp_archive[0] != '\0') cleanup_tmp_archive_win();
    if (handlers_set) SetConsoleCtrlHandler(ctrl_cleanup_handler, FALSE);
    free_job_list(&list);

    if (result == 0 && g_verbosity < VERB_SILENT) {
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    }
    return result;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char command;
    bp_uint32 block_size = 0;
    bp_uint8 method = BP_METHOD_BOLTLZ;
    int opt_level = 0; /* 0 = use default (no -l flag given) */
    const char *out_dir = NULL;
    int force = 0;
    int dry_run = 0;
    int interchange_profile = 0;
    int windows_target_profile = 0;
    int cpm86_profile = 0;
    int classic_hfs_profile = 0;
    int amiga_profile = 0;
    int atari_st_profile = 0;
    int ql_profile = 0;
    int riscos_profile = 0;
    int x68000_profile = 0;
    int dos_83_profile = WIN_DOS_PROFILE_NONE;
    int codec_given = 0;
    int i;
    char **utf8_argv;

    (void)argv;
    if (win_make_utf8_argv(&argc, &utf8_argv) != 0) {
        fprintf(stderr, "boltpack: command line contains invalid Unicode\n");
        return 1;
    }
    argv = utf8_argv;

    /* Keep console path output in the same encoding used internally. */
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        usage();
        return 1;
    }

    if (strcmp(argv[1], "help") == 0 ||
        strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        if (argc >= 3 &&
            (strcmp(argv[2], "advanced") == 0 ||
             strcmp(argv[2], "--advanced") == 0 ||
             strcmp(argv[2], "--help-advanced") == 0)) {
            usage_advanced();
        } else {
            usage_basic();
        }
        return 0;
    }

    if (strcmp(argv[1], "--help-advanced") == 0) {
        usage_advanced();
        return 0;
    }

    /* first arg is the command */
    command = parse_command_name(argv[1]);
#ifdef BOLTPACK_NET
    if (command == '\0') {
        command = bp_net_cli_command(argv[1]);   /* 'L' listen, 'S' scan */
    }
#endif
    if (command == '\0') {
        fprintf(stderr, "boltpack: unknown command: %s\n", argv[1]);
        usage();
        return 1;
    }

    /* listen/scan take no archive argument; everything else needs one. */
    if (argc < 3
#ifdef BOLTPACK_NET
        && command != 'L' && command != 'S'
#endif
       ) {
        usage();
        return 1;
    }

    /* parse options starting from argv[2] */
    i = 2;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage_basic();
            return 0;
        } else if (strcmp(argv[i], "--help-advanced") == 0) {
            usage_advanced();
            return 0;
        } else if (strcmp(argv[i], "--interchange-profile-1") == 0) {
            interchange_profile = 1;
            i++;
        } else if (strncmp(argv[i], "--target-profile=", 17) == 0) {
            if (strcmp(argv[i] + 17, "windows-nt-lfn-v1") == 0) {
                if (classic_hfs_profile || amiga_profile || atari_st_profile ||
                    ql_profile || riscos_profile || x68000_profile || cpm86_profile ||
                    dos_83_profile != WIN_DOS_PROFILE_NONE) {
                    fprintf(stderr, "boltpack: select only one target profile\n");
                    return 1;
                }
                windows_target_profile = 1;
            } else if (strcmp(argv[i] + 17,
                              "macos-classic-hfs-v1") == 0) {
                if (windows_target_profile || amiga_profile || atari_st_profile ||
                    ql_profile || riscos_profile || x68000_profile || cpm86_profile ||
                    dos_83_profile != WIN_DOS_PROFILE_NONE) {
                    fprintf(stderr, "boltpack: select only one target profile\n");
                    return 1;
                }
                classic_hfs_profile = 1;
            } else if (strcmp(argv[i] + 17,
                              "amigaos-68k-latin1") == 0) {
                if (windows_target_profile || classic_hfs_profile || cpm86_profile ||
                    atari_st_profile || ql_profile || riscos_profile || x68000_profile ||
                    dos_83_profile != WIN_DOS_PROFILE_NONE) {
                    fprintf(stderr, "boltpack: select only one target profile\n");
                    return 1;
                }
                amiga_profile = 1;
            } else if (strcmp(argv[i] + 17, "atari-st") == 0) {
                if (windows_target_profile || classic_hfs_profile || amiga_profile ||
                    ql_profile || riscos_profile || x68000_profile || cpm86_profile ||
                    dos_83_profile != WIN_DOS_PROFILE_NONE) {
                    fprintf(stderr, "boltpack: select only one target profile\n");
                    return 1;
                }
                atari_st_profile = 1;
            } else if (strcmp(argv[i] + 17,
                              "sinclair-ql-latin1-v1") == 0) {
                if (windows_target_profile || classic_hfs_profile || amiga_profile ||
                    atari_st_profile || riscos_profile || x68000_profile ||
                    cpm86_profile ||
                    dos_83_profile != WIN_DOS_PROFILE_NONE) {
                    fprintf(stderr, "boltpack: select only one target profile\n");
                    return 1;
                }
                ql_profile = 1;
            } else if (strcmp(argv[i] + 17, "riscos-latin1-v1") == 0) {
                if (windows_target_profile || classic_hfs_profile || amiga_profile ||
                    atari_st_profile || ql_profile || x68000_profile || cpm86_profile ||
                    dos_83_profile != WIN_DOS_PROFILE_NONE) {
                    fprintf(stderr, "boltpack: select only one target profile\n");
                    return 1;
                }
                riscos_profile = 1;
            } else if (strcmp(argv[i] + 17, "x68000-sjis-v1") == 0) {
                if (windows_target_profile || classic_hfs_profile || amiga_profile ||
                    atari_st_profile || ql_profile || riscos_profile ||
                    cpm86_profile || dos_83_profile != WIN_DOS_PROFILE_NONE) {
                    fprintf(stderr, "boltpack: select only one target profile\n");
                    return 1;
                }
                x68000_profile = 1;
            } else if (strcmp(argv[i] + 17, "cpm86-83") == 0) {
                if (windows_target_profile || classic_hfs_profile || amiga_profile ||
                    atari_st_profile || ql_profile || riscos_profile || x68000_profile ||
                    dos_83_profile != WIN_DOS_PROFILE_NONE) {
                    fprintf(stderr, "boltpack: select only one target profile\n");
                    return 1;
                }
                cpm86_profile = 1;
            } else if (strcmp(argv[i] + 17, "dos-cp850-83") == 0 ||
                       strcmp(argv[i] + 17, "dos-cp437-83") == 0 ||
                       strcmp(argv[i] + 17, "dos16-cp850-mini") == 0 ||
                       strcmp(argv[i] + 17, "win16-cp437-mini") == 0 ||
                       strcmp(argv[i] + 17, "os2-16-cp850-mini") == 0 ||
                       strcmp(argv[i] + 17, "os2-fat-cp850") == 0 ||
                       strcmp(argv[i] + 17, "dos-cp850-lfn-v1") == 0) {
                if (windows_target_profile || classic_hfs_profile ||
                    amiga_profile || atari_st_profile || ql_profile || riscos_profile || x68000_profile ||
                    cpm86_profile ||
                    dos_83_profile != WIN_DOS_PROFILE_NONE) {
                    fprintf(stderr, "boltpack: select only one target profile\n");
                    return 1;
                }
                if (strcmp(argv[i] + 17, "dos-cp850-83") == 0)
                    dos_83_profile = WIN_DOS_PROFILE_CP850_83;
                else if (strcmp(argv[i] + 17, "dos-cp437-83") == 0)
                    dos_83_profile = WIN_DOS_PROFILE_CP437_83;
                else if (strcmp(argv[i] + 17, "dos16-cp850-mini") == 0)
                    dos_83_profile = WIN_DOS_PROFILE_DOS16_CP850;
                else if (strcmp(argv[i] + 17, "win16-cp437-mini") == 0)
                    dos_83_profile = WIN_DOS_PROFILE_WIN16_CP437;
                else if (strcmp(argv[i] + 17, "os2-16-cp850-mini") == 0)
                    dos_83_profile = WIN_DOS_PROFILE_OS2_16_CP850;
                else if (strcmp(argv[i] + 17, "dos-cp850-lfn-v1") == 0)
                    dos_83_profile = WIN_DOS_PROFILE_CP850_LFN;
                else
                    dos_83_profile = WIN_DOS_PROFILE_OS2_FAT_CP850;
            } else {
                fprintf(stderr,
                        "boltpack: unsupported Windows target profile: %s\n",
                        argv[i] + 17);
                return 1;
            }
            i++;
        } else if (strcmp(argv[i], "-b") == 0 ||
                   strcmp(argv[i], "--block-size") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: %s requires an argument\n", argv[i]);
                return 1;
            }
            block_size = parse_block_size(argv[i + 1]);
            if (block_size == 0) {
                fprintf(stderr, "boltpack: invalid block size: %s\n",
                        argv[i + 1]);
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-0") == 0 ||
                   strcmp(argv[i], "--store") == 0) {
            method = BP_METHOD_STORED;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "-1") == 0 ||
                   strcmp(argv[i], "--boltlite") == 0) {
            method = BP_METHOD_BOLTLITE;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "-2") == 0 ||
                   strcmp(argv[i], "--boltlz") == 0) {
            method = BP_METHOD_BOLTLZ;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "-F") == 0 ||
                   strcmp(argv[i], "--fluff") == 0) {
            method = BP_METHOD_FLUFF;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "-Z") == 0 ||
                   strcmp(argv[i], "--zstd") == 0) {
            method = BP_METHOD_ZSTD;
            codec_given = 1;
            i++;
        } else if (strcmp(argv[i], "-l") == 0 ||
                   strcmp(argv[i], "--level") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: %s requires an argument\n", argv[i]);
                return 1;
            }
            opt_level = atoi(argv[i + 1]);
            if (opt_level < 1 || opt_level > 9) {
                fprintf(stderr, "boltpack: -l level must be 1-9\n");
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--light") == 0) {
            opt_level = -1;
            i++;
        } else if (strcmp(argv[i], "--standard") == 0) {
            opt_level = -2;
            i++;
        } else if (strcmp(argv[i], "--best") == 0) {
            opt_level = -3;
            i++;
        } else if (strcmp(argv[i], "-j") == 0 ||
                   strcmp(argv[i], "--threads") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: %s requires an argument\n", argv[i]);
                return 1;
            }
            if (!parse_thread_count_arg(argv[i + 1])) {
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--single-thread") == 0) {
            g_fast_thread_policy = FAST_THREAD_SINGLE;
            g_fast_threads = 0;
            i++;
        } else if (strcmp(argv[i], "-g") == 0 ||
                   strcmp(argv[i], "--group-size") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: %s requires an argument\n", argv[i]);
                return 1;
            }
            g_fast_group_blocks = atoi(argv[i + 1]);
            if (g_fast_group_blocks < 0) {
                fprintf(stderr, "boltpack: -g group size must be >= 0\n");
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-qq") == 0 ||
                   strcmp(argv[i], "--silent") == 0) {
            g_verbosity = VERB_SILENT;
            i++;
        } else if (strcmp(argv[i], "-q") == 0 ||
                   strcmp(argv[i], "--quiet") == 0) {
            g_verbosity = VERB_QUIET;
            i++;
        } else if (strcmp(argv[i], "-o") == 0 ||
                   strcmp(argv[i], "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: %s requires an argument\n", argv[i]);
                return 1;
            }
            out_dir = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-f") == 0 ||
                   strcmp(argv[i], "--force") == 0) {
            force = 1;
            i++;
        } else if (strcmp(argv[i], "-n") == 0 ||
                   strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
            i++;
#ifdef BOLTPACK_NET
        } else if (bp_net_cli_take_option(&i, argc, argv)) {
            /* consumed a network option (--send/--name/--auto/...) */
#endif
        } else {
            fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]);
            usage();
            return 1;
        }
    }

    if (opt_level < 0) {
        if (method == BP_METHOD_BOLTLZ)
            opt_level = (opt_level == -1) ? 1 : 2;
        else if (method == BP_METHOD_FLUFF)
            opt_level = (opt_level == -1) ? 1 : (opt_level == -2 ? 8 : 9);
        else if (method == BP_METHOD_ZSTD)
            opt_level = (opt_level == -1) ? 1 : (opt_level == -2 ? 3 : 9);
    }
    if (opt_level < 0) {
        fprintf(stderr,
                "boltpack: quality presets are only supported for BoltLZ, Fluff, and zstd\n");
        return 1;
    }
    if (opt_level > 0) {
        if (method == BP_METHOD_BOLTLZ && opt_level > 2) {
            fprintf(stderr, "boltpack: BoltLZ level must be 1-2\n");
            return 1;
        }
        if (method != BP_METHOD_BOLTLZ && method != BP_METHOD_FLUFF &&
            method != BP_METHOD_ZSTD) {
            fprintf(stderr,
                    "boltpack: -l is only supported for BoltLZ, Fluff, and zstd\n");
            return 1;
        }
    }
    if (interchange_profile) {
        if ((command != 'c' && command != 'a') ||
            block_size > BP_BLOCK_32K ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED) || opt_level != 0) {
            fprintf(stderr,
                    "boltpack: Interchange Profile 1 requires create/add, BoltLite or Stored, 16 KiB blocks, and no codec level\n");
            return 1;
        }
        block_size = BP_BLOCK_16K;
        if (!codec_given || method != BP_METHOD_STORED)
            method = BP_METHOD_BOLTLITE;
    }
    if (dos_83_profile != WIN_DOS_PROFILE_NONE) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED) || opt_level != 0) {
            fprintf(stderr,
                    "boltpack: DOS-family target profiles require create/add, Stored or BoltLite, 16 KiB blocks, and no codec level\n");
            return 1;
        }
        interchange_profile = 1;
        block_size = BP_BLOCK_16K;
        if (!codec_given || method != BP_METHOD_STORED)
            method = BP_METHOD_BOLTLITE;
    }
    if (atari_st_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED) || opt_level != 0) {
            fprintf(stderr,
                    "boltpack: atari-st requires create/add, Stored or BoltLite, 16 KiB blocks, and no codec level\n");
            return 1;
        }
        interchange_profile = 1;
        block_size = BP_BLOCK_16K;
        if (!codec_given || method != BP_METHOD_STORED)
            method = BP_METHOD_BOLTLITE;
    }
    if (ql_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED) || opt_level != 0) {
            fprintf(stderr,
                    "boltpack: Sinclair QL profile requires create/add, Stored or BoltLite, 16 KiB blocks, and no codec level\n");
            return 1;
        }
        interchange_profile = 1;
        block_size = BP_BLOCK_16K;
        if (!codec_given || method != BP_METHOD_STORED)
            method = BP_METHOD_BOLTLITE;
    }
    if (riscos_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED) || opt_level != 0) {
            fprintf(stderr,
                    "boltpack: RISC OS profile requires create/add, Stored or BoltLite, 16 KiB blocks, and no codec level\n");
            return 1;
        }
        interchange_profile = 1;
        block_size = BP_BLOCK_16K;
        if (!codec_given || method != BP_METHOD_STORED)
            method = BP_METHOD_BOLTLITE;
    }
    if (x68000_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED) || opt_level != 0) {
            fprintf(stderr,
                    "boltpack: X68000 profile requires create/add, Stored or BoltLite, 16 KiB blocks, and no codec level\n");
            return 1;
        }
        interchange_profile = 1;
        block_size = BP_BLOCK_16K;
        if (!codec_given || method != BP_METHOD_STORED)
            method = BP_METHOD_BOLTLITE;
    }
    if (cpm86_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED) || opt_level != 0) {
            fprintf(stderr,
                    "boltpack: cpm86-83 requires create/add, Stored or BoltLite, blocks no larger than 32 KiB, and no codec level\n");
            return 1;
        }
        if (block_size == 0) block_size = BP_BLOCK_32K;
        if (!codec_given || method != BP_METHOD_STORED)
            method = BP_METHOD_BOLTLITE;
    }
    if (atari_st_profile && command != 'c' && command != 'a') {
        fprintf(stderr, "boltpack: atari-st is available only for create/add\n");
        return 1;
    }
    if (ql_profile && command != 'c' && command != 'a') {
        fprintf(stderr,
                "boltpack: Sinclair QL profile is available only for create/add\n");
        return 1;
    }
    if (riscos_profile && command != 'c' && command != 'a') {
        fprintf(stderr, "boltpack: RISC OS profile is available only for create/add\n");
        return 1;
    }
    if (x68000_profile && command != 'c' && command != 'a') {
        fprintf(stderr, "boltpack: X68000 profile is available only for create/add\n");
        return 1;
    }
    if (classic_hfs_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED) || opt_level != 0) {
            fprintf(stderr,
                    "boltpack: macos-classic-hfs-v1 requires create/add, Stored or BoltLite, 16 KiB blocks, and no codec level\n");
            return 1;
        }
        interchange_profile = 1;
        block_size = BP_BLOCK_16K;
        if (!codec_given || method != BP_METHOD_STORED)
            method = BP_METHOD_BOLTLITE;
    }
    if (amiga_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED) || opt_level != 0) {
            fprintf(stderr,
                    "boltpack: amigaos-68k-latin1 requires create/add, Stored or BoltLite, 16 KiB blocks, and no codec level\n");
            return 1;
        }
        interchange_profile = 1;
        block_size = BP_BLOCK_16K;
        if (!codec_given || method != BP_METHOD_STORED)
            method = BP_METHOD_BOLTLITE;
    }
    if (windows_target_profile && command != 'c' && command != 'a') {
        fprintf(stderr,
                "boltpack: windows-nt-lfn-v1 is available only for create/add\n");
        return 1;
    }
    if (classic_hfs_profile && command != 'c' && command != 'a') {
        fprintf(stderr,
                "boltpack: macos-classic-hfs-v1 is available only for create/add\n");
        return 1;
    }
    if (amiga_profile && command != 'c' && command != 'a') {
        fprintf(stderr,
                "boltpack: amigaos-68k-latin1 is available only for create/add\n");
        return 1;
    }
    if (dos_83_profile != WIN_DOS_PROFILE_NONE &&
        command != 'c' && command != 'a') {
        fprintf(stderr,
                "boltpack: DOS-family target profiles are available only for create/add\n");
        return 1;
    }
    if (cpm86_profile && command != 'c' && command != 'a') {
        fprintf(stderr, "boltpack: cpm86-83 target profile is available only for create/add\n");
        return 1;
    }
    if (command == 'x' && i + 1 < argc) {
        fprintf(stderr,
                "boltpack: extract options must precede the archive name\n");
        return 1;
    }

#ifndef BOLTPACK_FAST_FLUFF
    if (g_fast_thread_policy == FAST_THREAD_EXPLICIT && g_fast_threads > 0) {
        fprintf(stderr, "boltpack: -j requires an OpenMP-enabled Windows build\n");
        return 1;
    }
    if (g_fast_group_blocks > 0) {
        fprintf(stderr, "boltpack: -g requires a fast-fluff-enabled Windows build\n");
        return 1;
    }
#endif

#ifdef BOLTPACK_FAST_FLUFF
    if (g_fast_group_blocks > 0)
        fluff_fast_set_group_blocks(g_fast_group_blocks);
#endif

    g_target_classic_hfs = classic_hfs_profile;
    g_target_amiga_68k = amiga_profile;
    g_target_cpm86 = cpm86_profile;
    g_target_atari_st = atari_st_profile;
    g_target_ql = ql_profile;
    g_target_riscos = riscos_profile;
    g_target_x68000 = x68000_profile;

    switch (command) {
    case 'c':
        if ((method == BP_METHOD_FLUFF || method == BP_METHOD_ZSTD)
            && block_size == 0) {
            block_size = BP_BLOCK_256K;
        }
        return cmd_create(argc - i, argv + i, block_size, method, opt_level,
                          interchange_profile, windows_target_profile,
                          classic_hfs_profile, amiga_profile,
                          dos_83_profile);
    case 'a':
        if ((method == BP_METHOD_FLUFF || method == BP_METHOD_ZSTD)
            && block_size == 0) {
            block_size = BP_BLOCK_256K;
        }
        return cmd_add(argc - i, argv + i, block_size, method, opt_level,
                       interchange_profile, windows_target_profile,
                       classic_hfs_profile, amiga_profile,
                       dos_83_profile);
    case 'l':
        if (i >= argc) {
            fprintf(stderr, "boltpack: list requires archive name\n");
            return 1;
        }
        return cmd_list(argv[i]);
    case 'x':
        if (i >= argc) {
            fprintf(stderr, "boltpack: extract requires archive name\n");
            return 1;
        }
        return cmd_extract(argv[i], out_dir, force, dry_run);
    case 't':
        if (i >= argc) {
            fprintf(stderr, "boltpack: test requires archive name\n");
            return 1;
        }
        return cmd_test(argv[i]);
    case 'i':
        if (i >= argc) {
            fprintf(stderr, "boltpack: info requires archive name\n");
            return 1;
        }
        return cmd_info(argv[i]);
    case 'd':
        return cmd_delete(argc - i, argv + i);
#ifdef BOLTPACK_NET
    case 'L':
        return cmd_listen(out_dir, force);
    case 'S':
        return cmd_scan();
#endif
    default:
        fprintf(stderr, "boltpack: unknown command: %c\n", command);
        usage();
        return 1;
    }
}
