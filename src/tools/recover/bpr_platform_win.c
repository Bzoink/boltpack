/*
 * bpr_platform_win.c -- Windows file access and host rules for boltpack-recover.
 *
 * Paths are UTF-8 inside the tool and UTF-16 at the Win32 boundary.  Reads are
 * positional (ReadFile with an explicit offset), so they are independent of any
 * shared file position.  Output files are created with CREATE_NEW (an existing
 * file is never replaced), directories are never entered through a reparse
 * point, and archive names are checked against the Windows rules by the module
 * the Windows CLI uses (src/frontend/common/win_names.h): reserved device
 * names, forbidden characters, trailing dots and spaces, over-long names, names
 * that differ only in case or Unicode normalisation, and a file and a directory
 * wanting the same path.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#include "bpr.h"

#include "bp_types.h"
#include "../../frontend/common/win_names.h"

#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WPATH_MAX 32768

/* ---- paths ------------------------------------------------------------- */

/* UTF-8 to a freshly allocated UTF-16 path with '\\' separators; NULL if the
 * string is not valid UTF-8. */
static wchar_t *to_wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    wchar_t *w;
    int i;
    if (n <= 0 || n > WPATH_MAX) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w == NULL) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    for (i = 0; i < n; i++)
        if (w[i] == L'/') w[i] = L'\\';
    return w;
}

/* root + '\\' + name (name in archive style), as UTF-16. */
static wchar_t *join_wide(const char *root, const char *name)
{
    size_t a = strlen(root), b = strlen(name);
    char *p = (char *)malloc(a + b + 2);
    wchar_t *w;
    if (p == NULL) return NULL;
    memcpy(p, root, a);
    if (a > 0 && root[a - 1] != '/' && root[a - 1] != '\\') p[a++] = '/';
    memcpy(p + a, name, b + 1);
    w = to_wide(p);
    free(p);
    return w;
}

static DWORD attrs_of(const wchar_t *w)
{
    return GetFileAttributesW(w);
}

/* ---- reading volumes ------------------------------------------------------ */

struct bpr_file {
    HANDLE   h;
    uint64_t size;
};

bpr_file *bpr_file_open(const char *path)
{
    wchar_t *w = to_wide(path);
    bpr_file *f;
    HANDLE h;
    LARGE_INTEGER sz;
    DWORD a;
    if (w == NULL) return NULL;
    a = attrs_of(w);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
        free(w);
        return NULL;
    }
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                    NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        return NULL;
    }
    f = (bpr_file *)malloc(sizeof *f);
    if (f == NULL) {
        CloseHandle(h);
        return NULL;
    }
    f->h = h;
    f->size = (uint64_t)sz.QuadPart;
    return f;
}

void bpr_file_close(bpr_file *f)
{
    if (f == NULL) return;
    CloseHandle(f->h);
    free(f);
}

uint64_t bpr_file_size(const bpr_file *f) { return f->size; }

int bpr_file_read(const bpr_file *f, uint64_t off, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    if (off > f->size || n > f->size - off) return -1;
    while (n > 0) {
        OVERLAPPED ov;
        DWORD got = 0, want = n > 0x40000000u ? 0x40000000u : (DWORD)n;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)(off & 0xFFFFFFFFu);
        ov.OffsetHigh = (DWORD)(off >> 32);
        if (!ReadFile(f->h, p, want, &got, &ov) || got == 0) return -1;
        p += got;
        off += got;
        n -= got;
    }
    return 0;
}

/* ---- output (extract) ------------------------------------------------------- */

struct bpr_out { HANDLE h; };

static win_path_node_t *g_names;        /* names written in this run */

static int is_real_dir(const wchar_t *w)
{
    DWORD a = attrs_of(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
           !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

int bpr_fs_prepare_root(const char *dir)
{
    wchar_t *w = to_wide(dir);
    DWORD a;
    int rc = -1;
    win_path_free(g_names);
    g_names = NULL;
    if (w == NULL) return -1;
    a = attrs_of(w);
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            rc = CreateDirectoryW(w, NULL) ? 0 : -1;
    } else if (is_real_dir(w)) {
        size_t n = wcslen(w);
        wchar_t *pat = (wchar_t *)malloc((n + 3) * sizeof(wchar_t));
        if (pat != NULL) {
            WIN32_FIND_DATAW fd;
            HANDLE h;
            memcpy(pat, w, n * sizeof(wchar_t));
            if (n > 0 && pat[n - 1] != L'\\') pat[n++] = L'\\';
            pat[n++] = L'*';
            pat[n] = L'\0';
            h = FindFirstFileW(pat, &fd);
            rc = 0;
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (wcscmp(fd.cFileName, L".") != 0 &&
                        wcscmp(fd.cFileName, L"..") != 0) { rc = -1; break; }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            free(pat);
        }
    }
    free(w);
    return rc;
}

/* root\\prefix must be a real directory (not a reparse point); create it. */
static int ensure_dir(const wchar_t *w)
{
    DWORD a = attrs_of(w);
    if (a != INVALID_FILE_ATTRIBUTES)
        return (a & FILE_ATTRIBUTE_DIRECTORY) &&
               !(a & FILE_ATTRIBUTE_REPARSE_POINT) ? 0 : -1;
    if (CreateDirectoryW(w, NULL)) return 0;
    return GetLastError() == ERROR_ALREADY_EXISTS && is_real_dir(w) ? 0 : -1;
}

int bpr_fs_make_parents(const char *root, const char *name)
{
    wchar_t *w = join_wide(root, name);
    size_t i, start;
    int rc = 0;
    wchar_t *rw = to_wide(root);
    if (w == NULL || rw == NULL) { free(w); free(rw); return -1; }
    start = wcslen(rw);
    for (i = start + 1; w[i] != L'\0'; i++) {
        if (w[i] == L'\\') {
            w[i] = L'\0';
            if (ensure_dir(w) != 0) { rc = -1; w[i] = L'\\'; break; }
            w[i] = L'\\';
        }
    }
    free(w);
    free(rw);
    return rc;
}

int bpr_fs_mkdir(const char *root, const char *name)
{
    wchar_t *w = join_wide(root, name);
    size_t n;
    int rc;
    if (w == NULL) return -1;
    if (bpr_fs_make_parents(root, name) != 0) { free(w); return -1; }
    n = wcslen(w);
    while (n > 0 && w[n - 1] == L'\\') w[--n] = L'\0';
    rc = ensure_dir(w);
    free(w);
    return rc;
}

bpr_out *bpr_out_create(const char *root, const char *name)
{
    wchar_t *w = join_wide(root, name);
    bpr_out *o;
    HANDLE h;
    if (w == NULL) return NULL;
    h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    o = (bpr_out *)malloc(sizeof *o);
    if (o == NULL) {
        CloseHandle(h);
        return NULL;
    }
    o->h = h;
    return o;
}

int bpr_out_write(bpr_out *o, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        DWORD put = 0, want = n > 0x40000000u ? 0x40000000u : (DWORD)n;
        if (!WriteFile(o->h, p, want, &put, NULL) || put == 0) return -1;
        p += put;
        n -= put;
    }
    return 0;
}

int bpr_out_close(bpr_out *o)
{
    BOOL ok = CloseHandle(o->h);
    free(o);
    return ok ? 0 : -1;
}

void bpr_out_abort(bpr_out *o)
{
    CloseHandle(o->h);
    free(o);
}

int bpr_fs_restore_meta(const char *root, const char *name, uint16_t flags,
                        uint64_t mtime, int is_symlink)
{
    wchar_t *w;
    size_t n;
    HANDLE h;
    FILETIME ft;
    uint64_t t;
    int rc = 0;
    /* Unix mode bits mean nothing on Windows and are not restored. */
    if (is_symlink || (flags & BP_ENTRY_FLAG_MTIME_VALID) == 0) return 0;
    if (mtime > 910692730085ull) return 1;          /* beyond the FILETIME range */
    w = join_wide(root, name);
    if (w == NULL) return -1;
    n = wcslen(w);
    while (n > 1 && w[n - 1] == L'\\') w[--n] = L'\0';
    t = (mtime + 11644473600ull) * 10000000ull;
    ft.dwLowDateTime = (DWORD)(t & 0xFFFFFFFFu);
    ft.dwHighDateTime = (DWORD)(t >> 32);
    h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h == INVALID_HANDLE_VALUE) rc = -1;
    else {
        if (!SetFileTime(h, NULL, NULL, &ft)) rc = -1;
        CloseHandle(h);
    }
    free(w);
    return rc;
}

int bpr_fs_symlink(const char *root, const char *name, const char *target)
{
    (void)root; (void)name; (void)target;
    return -1;                      /* not extracted on Windows, as in the CLI */
}

int bpr_fs_rename(const char *root, const char *from, const char *to)
{
    wchar_t *a = join_wide(root, from), *b = join_wide(root, to);
    int rc = -1;
    if (a != NULL && b != NULL) rc = MoveFileExW(a, b, 0) ? 0 : -1;
    free(a);
    free(b);
    return rc;
}

int bpr_fs_unlink(const char *root, const char *name)
{
    wchar_t *w = join_wide(root, name);
    int rc = -1;
    if (w != NULL) rc = DeleteFileW(w) ? 0 : -1;
    free(w);
    return rc;
}

int bpr_fs_exists(const char *root, const char *name)
{
    wchar_t *w = join_wide(root, name);
    int rc = 0;
    if (w == NULL) return 0;
    rc = attrs_of(w) != INVALID_FILE_ATTRIBUTES;
    free(w);
    return rc;
}

/* ---- output files (rebuild) ----------------------------------------------- */

struct bpr_wfile { HANDLE h; };

bpr_wfile *bpr_wfile_create(const char *path)
{
    wchar_t *w = to_wide(path);
    bpr_wfile *f;
    HANDLE h;
    if (w == NULL) return NULL;
    h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    f = (bpr_wfile *)malloc(sizeof *f);
    if (f == NULL) {
        CloseHandle(h);
        return NULL;
    }
    f->h = h;
    return f;
}

long bpr_wfile_read(bpr_wfile *f, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        DWORD r = 0, want = n - got > 0x40000000u ? 0x40000000u : (DWORD)(n - got);
        if (!ReadFile(f->h, (char *)buf + got, want, &r, NULL)) return -1;
        if (r == 0) break;
        got += r;
    }
    return (long)got;
}

long bpr_wfile_write(bpr_wfile *f, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        DWORD put = 0, want = n - done > 0x40000000u ? 0x40000000u : (DWORD)(n - done);
        if (!WriteFile(f->h, (const char *)buf + done, want, &put, NULL) || put == 0)
            return (long)done;
        done += put;
    }
    return (long)done;
}

int bpr_wfile_seek(bpr_wfile *f, int64_t off, int origin)
{
    LARGE_INTEGER d;
    d.QuadPart = off;
    return SetFilePointerEx(f->h, d, NULL, (DWORD)origin) ? 0 : -1;
}

int64_t bpr_wfile_tell(bpr_wfile *f)
{
    LARGE_INTEGER z, pos;
    z.QuadPart = 0;
    return SetFilePointerEx(f->h, z, &pos, FILE_CURRENT) ? (int64_t)pos.QuadPart : -1;
}

int bpr_wfile_close(bpr_wfile *f)
{
    int rc;
    if (f == NULL) return 0;
    rc = FlushFileBuffers(f->h) ? 0 : -1;       /* the volume is on the medium */
    if (!CloseHandle(f->h)) rc = -1;
    free(f);
    return rc;
}

int bpr_path_exists(const char *path)
{
    wchar_t *w = to_wide(path);
    int rc;
    if (w == NULL) return 0;
    rc = attrs_of(w) != INVALID_FILE_ATTRIBUTES;
    free(w);
    return rc;
}

/* MoveFileEx without MOVEFILE_REPLACE_EXISTING never replaces a file and is
 * atomic, so no fallback is needed (unlike hard links on POSIX). */
int bpr_path_rename_new(const char *from, const char *to)
{
    wchar_t *a = to_wide(from), *b = to_wide(to);
    int rc = -1;
    if (a != NULL && b != NULL) rc = MoveFileExW(a, b, 0) ? 0 : -1;
    free(a);
    free(b);
    return rc;
}

int bpr_path_remove(const char *path)
{
    wchar_t *w = to_wide(path);
    int rc = -1;
    if (w != NULL) rc = DeleteFileW(w) ? 0 : -1;
    free(w);
    return rc;
}

/* ---- host rules ---------------------------------------------------------- */

const char *bpr_fs_check_name(const char *name, int is_dir)
{
    const char *why = NULL;
    int rc = win_names_probe(&g_names, name, is_dir ? BP_TYPE_DIR : BP_TYPE_DATA,
                             &why);
    return rc == BP_OK ? NULL : (why != NULL ? why : "not-valid-on-windows");
}

void bpr_fs_commit_name(const char *name, int is_dir)
{
    (void)win_names_register(&g_names, name, is_dir ? BP_TYPE_DIR : BP_TYPE_DATA,
                             NULL);
}

int bpr_fs_symlinks_supported(void)
{
    return 0;
}

FILE *bpr_fopen_write(const char *path)
{
    wchar_t *w = to_wide(path);
    FILE *f;
    if (w == NULL) return NULL;
    f = _wfopen(w, L"wb");
    free(w);
    return f;
}

/* ---- entry point ---------------------------------------------------------- */

/* Arguments arrive as UTF-16 and are handed to the engine as UTF-8; text goes
 * out as bytes, not translated to CRLF. */
int main(void)
{
    int wargc = 0, i, rc = 3;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    char **argv;
    if (wargv == NULL) return 3;
    argv = (char **)calloc((size_t)wargc + 1, sizeof *argv);
    if (argv == NULL) { LocalFree(wargv); return 3; }
    for (i = 0; i < wargc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = n > 0 ? (char *)malloc((size_t)n) : NULL;
        if (argv[i] == NULL ||
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL) != n)
            goto done;
    }
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    rc = bpr_main(wargc, argv);
done:
    for (i = 0; i < wargc; i++) free(argv[i]);
    free(argv);
    LocalFree(wargv);
    return rc;
}
