/*
 * main.cpp -- Windows FLTK GUI frontend for BoltPack (FLTK 1.4)
 *
 * Build (cross-compile from Linux with MinGW-w64):
 *   x86_64-w64-mingw32-g++ -O2 -mwindows -o boltpack-gui.exe main.cpp \
 *       -I../../../build/fltk-win64/include \
 *       -L../../../build/fltk-win64/lib \
 *       core_objects... \
 *       -lfltk_images -lfltk_png -lfltk_jpeg -lfltk_z -lfltk \
 *       -lgdi32 -lole32 -luuid -lcomctl32 -lcomdlg32 -lshell32 \
 *       -lwinmm -lwinspool -static-libgcc -static-libstdc++
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

/* ---- Windows / POSIX compatibility shims ------------------------------- */

#ifdef _WIN32
#  include <windows.h>
#  include <ole2.h>
#  include <shellapi.h>
#  include <direct.h>       /* _mkdir */
#  include <io.h>           /* _access */
#  include <fcntl.h>
#  include <sys/utime.h>    /* _utime, _utimbuf */
#  include "resource.h"
/* map POSIX names to Windows equivalents */
#  define utimbuf        _utimbuf
#  define strcasecmp     _stricmp
#  define strncasecmp    _strnicmp
#else
/* Shims for Windows-specific identifiers used in this file so that
 * non-MinGW toolchains (e.g. Linux IntelliSense) can parse the file. */
#  ifndef __int64
     typedef long long __int64;
#  endif
#  define _fseeki64(f, o, w)  fseeko((f), (off_t)(o), (w))
#  define _ftelli64(f)         ((long long)ftello(f))
#  define _stat64              stat
#endif


#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/platform.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <set>
#include <map>
#include <cwchar>
#include <algorithm>
#include <stdint.h>
#include <new>

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>

#ifndef _WIN32
#  include <unistd.h>
#  include <utime.h>
#endif

/* BoltPack core (C linkage) */
extern "C" {
#include "../../core/archiver.h"
#include "../../core/bp_metadata.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/boltlite.h"
#ifdef BOLTPACK_ZSTD
#include "../../core/zstd_wrap.h"
#endif
#ifdef BOLTPACK_NET
#include "../linux/bp_net.h"
#endif
}

#include "../gui_common/help_text.h"
#include "../gui_common/logo_png.h"

#ifdef _WIN32
/* FLTK returns UTF-8 paths; keep that encoding through every filesystem call. */
static bool win_utf8_to_wide(const char *utf8, std::wstring *wide)
{
    int length, bytes;
    std::string roundtrip;
    if (utf8 == NULL || wide == NULL) return false;
    length = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (length <= 0) return false;
    wide->resize((size_t)length);
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &(*wide)[0], length) <= 0)
        return false;
    bytes = WideCharToMultiByte(CP_UTF8, 0, wide->c_str(), -1,
                                NULL, 0, NULL, NULL);
    if (bytes <= 0) return false;
    roundtrip.resize((size_t)bytes);
    if (WideCharToMultiByte(CP_UTF8, 0, wide->c_str(), -1, &roundtrip[0],
                            bytes, NULL, NULL) != bytes)
        return false;
    roundtrip.resize((size_t)bytes - 1);
    if (roundtrip != utf8) return false;
    wide->resize((size_t)length - 1);
    return true;
}

static bool win_wide_to_utf8(const WCHAR *wide, std::string *utf8)
{
    const WCHAR *p;
    int length;
    if (wide == NULL || utf8 == NULL) return false;
    for (p = wide; *p; ++p) {
        if (*p >= 0xD800 && *p <= 0xDBFF) {
            if (p[1] < 0xDC00 || p[1] > 0xDFFF) return false;
            ++p;
        } else if (*p >= 0xDC00 && *p <= 0xDFFF) return false;
    }
    length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    if (length <= 0) return false;
    utf8->resize((size_t)length);
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, &(*utf8)[0], length,
                            NULL, NULL) != length) return false;
    utf8->resize((size_t)length - 1);
    return true;
}

static FILE *open_utf8_file(const std::string &path, const wchar_t *mode);
static FILE *win_fopen_utf8(const char *path, const char *mode)
{
    wchar_t wide_mode[8];
    std::string utf8_path(path ? path : "");
    size_t i;
    for (i = 0; mode && mode[i] && i < 7; ++i)
        wide_mode[i] = (wchar_t)(unsigned char)mode[i];
    if (mode == NULL || mode[i] != '\0') return NULL;
    wide_mode[i] = L'\0';
    return open_utf8_file(utf8_path, wide_mode);
}

static int win_stat64_utf8(const char *path, struct _stat64 *st)
{
    std::wstring wide;
    return win_utf8_to_wide(path, &wide) ? _wstat64(wide.c_str(), st) : -1;
}

static int win_utime_utf8(const char *path, struct _utimbuf *times)
{
    std::wstring wide;
    return win_utf8_to_wide(path, &wide) ? _wutime(wide.c_str(), times) : -1;
}

static int win_access_utf8(const char *path, int mode)
{
    std::wstring wide;
    return win_utf8_to_wide(path, &wide) ? _waccess(wide.c_str(), mode) : -1;
}

static int win_mkdir_utf8(const char *path, int mode)
{
    std::wstring wide;
    (void)mode;
    if (!win_utf8_to_wide(path, &wide)) return -1;
    if (CreateDirectoryW(wide.c_str(), NULL)) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) errno = EEXIST;
    return -1;
}

static int win_unlink_utf8(const char *path)
{
    std::wstring wide;
    return win_utf8_to_wide(path, &wide) && DeleteFileW(wide.c_str()) ? 0 : -1;
}

static DWORD win_GetFileAttributesA_utf8(const char *path)
{
    std::wstring wide;
    return win_utf8_to_wide(path, &wide) ? GetFileAttributesW(wide.c_str())
                                         : INVALID_FILE_ATTRIBUTES;
}

static BOOL win_SetFileAttributesA_utf8(const char *path, DWORD attributes)
{
    std::wstring wide;
    return win_utf8_to_wide(path, &wide) &&
           SetFileAttributesW(wide.c_str(), attributes);
}

static BOOL win_CreateDirectoryA_utf8(const char *path, LPSECURITY_ATTRIBUTES sa)
{
    std::wstring wide;
    return win_utf8_to_wide(path, &wide) && CreateDirectoryW(wide.c_str(), sa);
}

static HANDLE win_CreateFileA_utf8(const char *path, DWORD access, DWORD share,
                                  LPSECURITY_ATTRIBUTES sa, DWORD create,
                                  DWORD flags, HANDLE template_file)
{
    std::wstring wide;
    if (!win_utf8_to_wide(path, &wide)) return INVALID_HANDLE_VALUE;
    return CreateFileW(wide.c_str(), access, share, sa, create, flags, template_file);
}

static BOOL win_DeleteFileA_utf8(const char *path)
{
    std::wstring wide;
    return win_utf8_to_wide(path, &wide) && DeleteFileW(wide.c_str());
}

static BOOL win_RemoveDirectoryA_utf8(const char *path)
{
    std::wstring wide;
    return win_utf8_to_wide(path, &wide) && RemoveDirectoryW(wide.c_str());
}

static BOOL win_MoveFileExA_utf8(const char *from, const char *to, DWORD flags)
{
    std::wstring wide_from, wide_to;
    return win_utf8_to_wide(from, &wide_from) && win_utf8_to_wide(to, &wide_to) &&
           MoveFileExW(wide_from.c_str(), wide_to.c_str(), flags);
}

static BOOL win_ReplaceFileA_utf8(const char *replaced, const char *replacement,
                                 const char *backup, DWORD flags,
                                 LPVOID exclude, LPVOID reserved)
{
    std::wstring wide_replaced, wide_replacement, wide_backup;
    if (!win_utf8_to_wide(replaced, &wide_replaced) ||
        !win_utf8_to_wide(replacement, &wide_replacement) ||
        !win_utf8_to_wide(backup, &wide_backup)) return FALSE;
    return ReplaceFileW(wide_replaced.c_str(), wide_replacement.c_str(),
                        wide_backup.c_str(), flags, exclude, reserved);
}

#  define fopen(path, mode) win_fopen_utf8((path), (mode))
#  define _stat64(path, st) win_stat64_utf8((path), (st))
#  define utime(path, times) win_utime_utf8((path), (times))
#  define access(path, mode) win_access_utf8((path), (mode))
#  define mkdir(path, mode) win_mkdir_utf8((path), (mode))
#  define unlink(path) win_unlink_utf8((path))
#  define GetFileAttributesA(path) win_GetFileAttributesA_utf8((path))
#  define SetFileAttributesA(path, attrs) win_SetFileAttributesA_utf8((path), (attrs))
#  define CreateDirectoryA(path, sa) win_CreateDirectoryA_utf8((path), (sa))
#  define CreateFileA(path, a, s, sa, c, f, t) \
    win_CreateFileA_utf8((path), (a), (s), (sa), (c), (f), (t))
#  define DeleteFileA(path) win_DeleteFileA_utf8((path))
#  define RemoveDirectoryA(path) win_RemoveDirectoryA_utf8((path))
#  define MoveFileExA(from, to, flags) win_MoveFileExA_utf8((from), (to), (flags))
#  define ReplaceFileA(a, b, c, f, e, r) win_ReplaceFileA_utf8((a), (b), (c), (f), (e), (r))
#endif


/* ---- constants --------------------------------------------------------- */

#define MAIN_W  720
#define MAIN_H  520
#define MENU_H   25
#define STATUS_H 25

#define COL_NAME       0
#define COL_ORIG_SIZE  1
#define COL_COMP_SIZE  2
#define COL_RATIO      3
#define COL_METHOD     4
#define COL_DATE       5
#define NUM_COLS       6

/* ---- bp_io_t adapters -------------------------------------------------- */

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
    case BP_ERR_PARTIAL:             return "some names or attributes were not restored";
    case BP_ERR_NOT_FOUND:          return "entry not found";
    case BP_ERR_INVALID_ARG:        return "invalid argument";
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
    case BP_METHOD_ZSTD:   return "zstd";
    default:               return "?";
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
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos, "%s (%lu)",
                        names[i], (unsigned long)counts[i]);
        if (pos >= (int)sizeof(buf) - 1) break;
        first = 0;
    }
    if (first) strcpy(buf, "none");
    return buf;
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

static std::string format_size(double bytes)
{
    char buf[64];
    if (bytes < 1024.0)
        snprintf(buf, sizeof(buf), "%.0f B", bytes);
    else if (bytes < 1048576.0)
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1073741824.0)
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    else
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
    return std::string(buf);
}

static int ratio_percent(double orig_size, double comp_size)
{
    if (orig_size <= 0.0)
        return 0;
    return (int)(comp_size / orig_size * 100.0);
}

static double u64_to_double(bp_uint64 v)
{
    return (double)v.lo + (double)v.hi * 4294967296.0;
}

static std::string format_time(bp_uint64 mtime)
{
    char buf[64];
    time_t t = (time_t)bp_u64_to_ulong(mtime);
    struct tm *tm = localtime(&t);
    if (tm)
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm);
    else
        snprintf(buf, sizeof(buf), "-");
    return std::string(buf);
}

/* ---- entry storage ----------------------------------------------------- */

struct EntryInfo {
    std::string name;
    double      orig_size;
    double      comp_size;
    int         ratio;
    int         method;
    std::string date;
    int         entry_type;
};

static std::vector<EntryInfo> g_entries;
static std::string g_archive_path;
static bp_archive_info_t g_info;
static bool g_archive_open = false;

enum ShellActionType {
    SHELL_ACTION_NONE = 0,
    SHELL_ACTION_OPEN_ARCHIVE,
    SHELL_ACTION_CREATE_BOLTLZ,
    SHELL_ACTION_CREATE_FLUFF,
    SHELL_ACTION_EXTRACT_ARCHIVE
};

struct ShellAction {
    ShellActionType type;
    std::vector<std::string> paths;

    ShellAction() : type(SHELL_ACTION_NONE) {}
};

static ShellAction g_shell_action;

/* ---- forward declarations ---------------------------------------------- */

class FileTable;

static Fl_Double_Window *g_main_window = NULL;
static Fl_Menu_Bar      *g_menu       = NULL;
static FileTable        *g_table      = NULL;
static Fl_Box           *g_logo_box   = NULL;
static Fl_Box           *g_status     = NULL;
static bool              g_have_persisted_col_widths = false;
static int               g_persisted_col_widths[NUM_COLS] = { 0, 0, 0, 0, 0, 0 };
static bool              g_have_window_geometry = false;
static int               g_persisted_window_x = 0;
static int               g_persisted_window_y = 0;
static int               g_persisted_window_w = MAIN_W;
static int               g_persisted_window_h = MAIN_H;

static void update_ui();
static void update_selection_menu();
static void do_open_archive(const char *path);
static void classify_and_act(const std::vector<std::string> &paths);
static void save_prefs();
static void do_extract();
static bool has_bolt_extension(const char *path);

#ifdef _WIN32
static WNDPROC g_main_prev_wndproc = NULL;

static bool process_is_elevated()
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD size = 0;
    BOOL ok;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    memset(&elevation, 0, sizeof(elevation));
    ok = GetTokenInformation(token, TokenElevation, &elevation,
                             (DWORD)sizeof(elevation), &size);
    CloseHandle(token);

    return ok && elevation.TokenIsElevated != 0;
}

static void allow_drop_message(HWND hwnd, UINT message)
{
    ChangeWindowMessageFilterEx(hwnd, message, MSGFLT_ALLOW, NULL);
}

static std::vector<std::string> paths_from_hdrop(HDROP hdrop)
{
    std::vector<std::string> paths;
    UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);

    for (UINT i = 0; i < count; i++) {
        UINT len = DragQueryFileW(hdrop, i, NULL, 0);
        if (len > 0) {
            std::vector<WCHAR> wide(len + 1, L'\0');
            std::string utf8;
            if (DragQueryFileW(hdrop, i, &wide[0], len + 1) != 0 &&
                win_wide_to_utf8(&wide[0], &utf8)) {
                paths.push_back(utf8);
            } else {
                paths.clear();
                fl_alert("A dropped path contains invalid Unicode and cannot be opened.");
                return paths;
            }
        }
    }

    return paths;
}

static void handle_win32_drop(void *data)
{
    std::vector<std::string> *paths = (std::vector<std::string> *)data;

    if (paths) {
        if (!paths->empty())
            classify_and_act(*paths);
        delete paths;
    }
}

static LRESULT CALLBACK boltpack_main_wndproc(HWND hwnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_DROPFILES) {
        HDROP hdrop = (HDROP)wParam;
        std::vector<std::string> *paths =
            new std::vector<std::string>(paths_from_hdrop(hdrop));

        DragFinish(hdrop);

        if (!paths->empty())
            Fl::add_timeout(0.0, handle_win32_drop, paths);
        else
            delete paths;

        return 0;
    }

    return CallWindowProc(g_main_prev_wndproc, hwnd, msg, wParam, lParam);
}

static void install_win32_drop_fallback(Fl_Window *window)
{
    HWND hwnd;

    if (!window || g_main_prev_wndproc || !process_is_elevated())
        return;

    hwnd = fl_xid(window);
    if (!hwnd)
        return;

    /* Explorer cannot use FLTK's OLE drop target across integrity levels. */
    RevokeDragDrop(hwnd);
    DragAcceptFiles(hwnd, TRUE);
    allow_drop_message(hwnd, WM_DROPFILES);
    allow_drop_message(hwnd, WM_COPYDATA);
    allow_drop_message(hwnd, 0x0049);

    g_main_prev_wndproc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC,
                                                    (LONG_PTR)boltpack_main_wndproc);
}

static void install_win32_app_icon(Fl_Window *window)
{
    HWND hwnd;
    HICON big_icon;
    HICON small_icon;

    if (!window)
        return;

    hwnd = fl_xid(window);
    if (!hwnd)
        return;

    big_icon = (HICON)LoadImageA(GetModuleHandleA(NULL),
                                 MAKEINTRESOURCEA(IDI_BOLTPACK),
                                 IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON),
                                 LR_DEFAULTCOLOR | LR_SHARED);
    small_icon = (HICON)LoadImageA(GetModuleHandleA(NULL),
                                   MAKEINTRESOURCEA(IDI_BOLTPACK),
                                   IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON),
                                   LR_DEFAULTCOLOR | LR_SHARED);

    if (big_icon)
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big_icon);
    if (small_icon)
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small_icon);
}
#endif

/* ---- file list table --------------------------------------------------- */

class FileTable : public Fl_Group {
    static const int SB_W = 16; /* scrollbar width */

    int col_widths[NUM_COLS];
    int scroll_x;
    int scroll_y;
    int row_h;
    int header_h;
    int visible_rows;
    Fl_Scrollbar      *sb;
    Fl_Scrollbar      *hsb;
    std::vector<bool>  sel;
    int                anchor_row;
    bool               dnd_incoming; /* true only between FL_DND_RELEASE and FL_PASTE */
    bool               manual_col_widths;
    bool               resizing_cols;
    int                resize_col;
    int                resize_start_x;
    int                resize_widths[NUM_COLS];

    static int min_col_width(int col)
    {
        static const int mins[NUM_COLS] = { 120, 70, 80, 50, 70, 110 };
        if (col < 0 || col >= NUM_COLS) return 60;
        return mins[col];
    }

    void set_default_column_widths()
    {
        col_widths[COL_NAME]      = 260;
        col_widths[COL_ORIG_SIZE] =  90;
        col_widths[COL_COMP_SIZE] =  90;
        col_widths[COL_RATIO]     =  60;
        col_widths[COL_METHOD]    =  70;
        col_widths[COL_DATE]      = 130;
    }

    int table_width() const
    {
        return w() - SB_W;
    }

    int table_height() const
    {
        return h() - SB_W;
    }

    int content_width() const
    {
        int total = 0;
        for (int c = 0; c < NUM_COLS; c++) total += col_widths[c];
        return total;
    }

    int visible_row_count() const
    {
        int rows = (table_height() - header_h) / row_h;
        if (rows < 0) rows = 0;
        return rows;
    }

    void clamp_widths_to_table()
    {
        for (int c = 0; c < NUM_COLS; c++) {
            if (col_widths[c] < min_col_width(c))
                col_widths[c] = min_col_width(c);
        }
    }

    void update_scrollbars()
    {
        int total_rows;
        int total_width;
        int page_rows;
        int page_width;
        int max_scroll_y;
        int max_scroll_x;

        page_rows = visible_row_count();
        page_width = table_width();
        total_rows = (int)g_entries.size();
        total_width = content_width();
        max_scroll_y = total_rows - page_rows;
        max_scroll_x = total_width - page_width;

        if (max_scroll_y < 0) max_scroll_y = 0;
        if (max_scroll_x < 0) max_scroll_x = 0;

        if (scroll_y < 0) scroll_y = 0;
        if (scroll_y > max_scroll_y) scroll_y = max_scroll_y;
        if (scroll_x < 0) scroll_x = 0;
        if (scroll_x > max_scroll_x) scroll_x = max_scroll_x;

        visible_rows = page_rows;

        if (total_rows < page_rows) total_rows = page_rows;
        if (total_width < page_width) total_width = page_width;

        sb->value(scroll_y, page_rows > 0 ? page_rows : 1, 0, total_rows > 0 ? total_rows : 1);
        hsb->value(scroll_x, page_width > 0 ? page_width : 1, 0, total_width > 0 ? total_width : 1);

        if (max_scroll_y > 0) sb->activate(); else sb->deactivate();
        if (max_scroll_x > 0) hsb->activate(); else hsb->deactivate();
    }

    void remember_column_widths()
    {
        for (int c = 0; c < NUM_COLS; c++) g_persisted_col_widths[c] = col_widths[c];
        g_have_persisted_col_widths = true;
    }

    int divider_content_x(int col) const
    {
        int pos = 0;
        for (int c = 0; c <= col && c < NUM_COLS; c++) pos += col_widths[c];
        return pos;
    }

    void keep_resize_divider_visible()
    {
        int margin;
        int divider_x;

        if (resize_col < 0 || resize_col >= NUM_COLS - 1) return;

        margin = 24;
        divider_x = divider_content_x(resize_col);

        if (divider_x - scroll_x > table_width() - margin)
            scroll_x = divider_x - (table_width() - margin);
        else if (divider_x - scroll_x < margin)
            scroll_x = divider_x - margin;

        if (scroll_x < 0) scroll_x = 0;
    }

    void autosize_name_column()
    {
        int total = (int)g_entries.size();
        int best = col_widths[COL_NAME];

        if (best < min_col_width(COL_NAME)) best = min_col_width(COL_NAME);

        fl_font(FL_HELVETICA, 12);
        for (int i = 0; i < total; i++) {
            std::string display = g_entries[i].name;
            if (g_entries[i].entry_type == BP_TYPE_DIR) display += "/";
            int tw = (int)fl_width(display.c_str()) + 16;
            if (tw > best) best = tw;
        }

        if (best > max_name_width()) best = max_name_width();
        col_widths[COL_NAME] = best;
    }

    int max_name_width() const
    {
        int tbl_w = table_width();
        int other_widths = 0;
        for (int c = 0; c < NUM_COLS; c++) {
            if (c != COL_NAME) other_widths += col_widths[c];
        }
        tbl_w -= other_widths;
        if (tbl_w < min_col_width(COL_NAME)) tbl_w = min_col_width(COL_NAME);
        return tbl_w;
    }

    int header_divider_hit(int ex, int ey) const
    {
        int tbl_w = table_width();
        int cx = 0;
        int local_x = ex - x() + scroll_x;
        const int grip = 4;

        if (ey < y() || ey >= y() + header_h) return -1;
        if (ex < x() || ex >= x() + tbl_w) return -1;

        for (int c = 0; c < NUM_COLS - 1; c++) {
            cx += col_widths[c];
            if (local_x >= cx - grip && local_x <= cx + grip) return c;
        }
        return -1;
    }

    void set_resize_cursor(bool active)
    {
        Fl_Window *win = window();
        if (win) win->cursor(active ? FL_CURSOR_WE : FL_CURSOR_DEFAULT);
    }

    void apply_column_resize(int event_x)
    {
        int delta;
        int min_delta;
        int c;

        if (resize_col < 0 || resize_col >= NUM_COLS - 1) return;

        delta = event_x - resize_start_x;
        min_delta = min_col_width(resize_col) - resize_widths[resize_col];

        if (delta < min_delta) delta = min_delta;

        for (c = 0; c < NUM_COLS; c++)
            col_widths[c] = resize_widths[c];

        col_widths[resize_col] = resize_widths[resize_col] + delta;
        keep_resize_divider_visible();
        update_scrollbars();
        redraw();
    }

    static void sb_cb(Fl_Widget *w, void *data)
    {
        FileTable *t = (FileTable *)data;
        t->scroll_y = ((Fl_Scrollbar *)w)->value();
        t->redraw();
    }

    static void hsb_cb(Fl_Widget *w, void *data)
    {
        FileTable *t = (FileTable *)data;
        t->scroll_x = ((Fl_Scrollbar *)w)->value();
        t->redraw();
    }

public:
    FileTable(int X, int Y, int W, int H)
        : Fl_Group(X, Y, W, H), scroll_x(0), scroll_y(0), row_h(20), header_h(24), visible_rows(0),
          anchor_row(-1), dnd_incoming(false), manual_col_widths(false), resizing_cols(false),
          resize_col(-1), resize_start_x(0)
    {
        set_default_column_widths();
        for (int c = 0; c < NUM_COLS; c++) resize_widths[c] = 0;

        sb = new Fl_Scrollbar(X + W - SB_W, Y, SB_W, H - SB_W);
        sb->type(FL_VERTICAL);
        sb->linesize(1);
        sb->callback(sb_cb, this);

        hsb = new Fl_Scrollbar(X, Y + H - SB_W, W - SB_W, SB_W);
        hsb->type(FL_HORIZONTAL);
        hsb->linesize(16);
        hsb->callback(hsb_cb, this);

        end();
        resizable(0); /* children don't auto-scale; we manage resize() manually */
        update_scrollbars();
    }

    void resize(int X, int Y, int W, int H)
    {
        Fl_Group::resize(X, Y, W, H);
        clamp_widths_to_table();
        if (manual_col_widths) remember_column_widths();
        sb->resize(X + W - SB_W, Y, SB_W, H - SB_W);
        hsb->resize(X, Y + H - SB_W, W - SB_W, SB_W);
        update_scrollbars();
    }

    /* Call after g_entries changes to refresh scrollbar range and column width. */
    void after_load()
    {
        scroll_x     = 0;
        scroll_y     = 0;
        visible_rows = visible_row_count();
        sel.assign(g_entries.size(), false);
        anchor_row = -1;

        if (!manual_col_widths) {
            /* measure widest name while keeping all columns visible */
            autosize_name_column();
        }
        clamp_widths_to_table();
        update_scrollbars();

        redraw();
    }

    void set_column_widths(const int *widths)
    {
        for (int c = 0; c < NUM_COLS; c++) {
            int width = widths[c];
            if (width < min_col_width(c)) width = min_col_width(c);
            col_widths[c] = width;
        }
        manual_col_widths = true;
        clamp_widths_to_table();
        remember_column_widths();
        update_scrollbars();
        redraw();
    }

    void draw_cell_text(const char *text, int cell_x, int cell_y,
                        int cell_w, int cell_h, Fl_Align align) const
    {
        int clip_w;
        int draw_w;

        if (!text || cell_w <= 2) return;

        clip_w = cell_w - 2;
        draw_w = cell_w - 8;
        if (draw_w < 0) draw_w = 0;

        fl_push_clip(cell_x + 1, cell_y, clip_w, cell_h);
        fl_draw(text, cell_x + 4, cell_y, draw_w, cell_h, align);
        fl_pop_clip();
    }

    void reset_column_widths()
    {
        manual_col_widths = false;
        set_default_column_widths();
        autosize_name_column();
        clamp_widths_to_table();
        update_scrollbars();
        redraw();
    }

    void draw()
    {
        int tbl_w = table_width(); /* drawable area left of the scrollbar */
        int tbl_h = table_height();
        visible_rows = visible_row_count();

        fl_push_clip(x(), y(), tbl_w, tbl_h);

        /* background */
        fl_color(FL_WHITE);
        fl_rectf(x(), y(), tbl_w, tbl_h);

        /* column headers */
        static const char *headers[] = {
            "Name", "Original", "Compressed", "Ratio", "Method", "Date"
        };
        int cx = x() - scroll_x;
        fl_color(FL_BACKGROUND_COLOR);
        fl_rectf(x(), y(), tbl_w, header_h);
        fl_color(FL_BLACK);
        fl_font(FL_HELVETICA_BOLD, 12);
        static const Fl_Align header_align[] = {
            FL_ALIGN_LEFT  | FL_ALIGN_INSIDE, /* Name */
            FL_ALIGN_LEFT  | FL_ALIGN_INSIDE, /* Original */
            FL_ALIGN_LEFT  | FL_ALIGN_INSIDE, /* Compressed */
            FL_ALIGN_LEFT  | FL_ALIGN_INSIDE, /* Ratio */
            FL_ALIGN_LEFT  | FL_ALIGN_INSIDE, /* Method */
            FL_ALIGN_LEFT  | FL_ALIGN_INSIDE  /* Date */
        };
        for (int c = 0; c < NUM_COLS; c++) {
            draw_cell_text(headers[c], cx, y(), col_widths[c], header_h,
                           header_align[c]);
            cx += col_widths[c];
        }
        fl_color(FL_LIGHT2);
        cx = x() - scroll_x;
        for (int c = 0; c < NUM_COLS - 1; c++) {
            cx += col_widths[c];
            fl_line(cx, y() + 4, cx, y() + header_h - 4);
        }
        fl_line(x(), y() + header_h - 1, x() + tbl_w - 1, y() + header_h - 1);

        /* rows */
        fl_font(FL_HELVETICA, 12);
        int ry = y() + header_h;
        int max_rows = (int)g_entries.size();

        for (int r = scroll_y; r < max_rows && (r - scroll_y) < visible_rows; r++) {
            const EntryInfo &e = g_entries[r];
            cx = x() - scroll_x;

            /* row background — blue if selected, else alternating */
            bool row_sel = (r < (int)sel.size() && sel[r]);
            if (row_sel)
                fl_color(fl_rgb_color(0x19, 0x76, 0xD2)); /* Material blue */
            else
                fl_color(((r - scroll_y) % 2 == 0) ? FL_WHITE : fl_rgb_color(245, 245, 250));
            fl_rectf(x(), ry, tbl_w, row_h);
            fl_color(row_sel ? FL_WHITE : FL_BLACK);

            /* Name */
            if (e.entry_type == BP_TYPE_DIR) {
                std::string dir_name = e.name + "/";
                draw_cell_text(dir_name.c_str(), cx, ry, col_widths[COL_NAME], row_h,
                               FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            } else {
                draw_cell_text(e.name.c_str(), cx, ry, col_widths[COL_NAME], row_h,
                               FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            }
            cx += col_widths[COL_NAME];

            /* Original Size */
            if (e.entry_type == BP_TYPE_DATA) {
                std::string s = format_size(e.orig_size);
                draw_cell_text(s.c_str(), cx, ry, col_widths[COL_ORIG_SIZE], row_h,
                               FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            }
            cx += col_widths[COL_ORIG_SIZE];

            /* Compressed Size */
            if (e.entry_type == BP_TYPE_DATA) {
                std::string s = format_size(e.comp_size);
                draw_cell_text(s.c_str(), cx, ry, col_widths[COL_COMP_SIZE], row_h,
                               FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            }
            cx += col_widths[COL_COMP_SIZE];

            /* Ratio */
            if (e.entry_type == BP_TYPE_DATA && e.orig_size > 0) {
                char buf[16];
                snprintf(buf, sizeof(buf), "%d%%", e.ratio);
                draw_cell_text(buf, cx, ry, col_widths[COL_RATIO], row_h,
                               FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            }
            cx += col_widths[COL_RATIO];

            /* Method */
            if (e.entry_type == BP_TYPE_DATA) {
                draw_cell_text(method_name(e.method), cx, ry, col_widths[COL_METHOD], row_h,
                               FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
            }
            cx += col_widths[COL_METHOD];

            /* Date */
            draw_cell_text(e.date.c_str(), cx, ry, col_widths[COL_DATE], row_h,
                           FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

            ry += row_h;
        }

        fl_color(FL_LIGHT2);
        cx = x() - scroll_x;
        for (int c = 0; c < NUM_COLS - 1; c++) {
            cx += col_widths[c];
            fl_line(cx, y() + header_h, cx, y() + tbl_h - 1);
        }

        /* border */
        fl_color(FL_DARK2);
        fl_rect(x(), y(), tbl_w, tbl_h);

        fl_pop_clip();

        fl_color(FL_BACKGROUND_COLOR);
        fl_rectf(x() + tbl_w, y() + tbl_h, SB_W, SB_W);

        /* draw the scrollbar */
        draw_child(*sb);
        draw_child(*hsb);
    }

    int selection_count() const
    {
        int n = 0;
        for (size_t i = 0; i < sel.size(); i++) if (sel[i]) n++;
        return n;
    }

    std::vector<std::string> selected_names() const
    {
        std::vector<std::string> names;
        for (size_t i = 0; i < sel.size() && i < g_entries.size(); i++) {
            if (sel[i]) {
                std::string name = g_entries[i].name;
                if (g_entries[i].entry_type == BP_TYPE_DIR &&
                    !name.empty() && name[name.size() - 1] != '/')
                    name += "/";
                names.push_back(name);
            }
        }
        return names;
    }

    int handle(int event)
    {
        switch (event) {
        case FL_PUSH: {
            /* only left-button drives selection; ignore middle/right */
            if (Fl::event_button() != FL_LEFT_MOUSE)
                return Fl_Group::handle(event);
            int tbl_w = table_width();
            int tbl_h = table_height();
            /* ignore clicks on the scrollbar column */
            if (Fl::event_x() >= x() + tbl_w || Fl::event_y() >= y() + tbl_h)
                return Fl_Group::handle(event);
            resize_col = header_divider_hit(Fl::event_x(), Fl::event_y());
            if (resize_col >= 0) {
                resizing_cols = true;
                manual_col_widths = true;
                resize_start_x = Fl::event_x();
                for (int c = 0; c < NUM_COLS; c++)
                    resize_widths[c] = col_widths[c];
                set_resize_cursor(true);
                return 1;
            }
            /* ignore clicks on the column header */
            int rel_y = Fl::event_y() - y() - header_h;
            if (rel_y < 0) return 1;
            int clicked = scroll_y + rel_y / row_h;
            if (clicked < 0 || clicked >= (int)g_entries.size()) {
                /* click below last row: clear selection */
                sel.assign(g_entries.size(), false);
                anchor_row = -1;
                redraw();
                update_selection_menu();
                return 1;
            }
            if (Fl::event_state(FL_CTRL)) {
                /* Ctrl+click: toggle this row */
                if (clicked < (int)sel.size()) sel[clicked] = !sel[clicked];
                anchor_row = clicked;
            } else if (Fl::event_state(FL_SHIFT) && anchor_row >= 0) {
                /* Shift+click: range select anchor..clicked */
                int lo = anchor_row < clicked ? anchor_row : clicked;
                int hi = anchor_row > clicked ? anchor_row : clicked;
                sel.assign(g_entries.size(), false);
                for (int i = lo; i <= hi && i < (int)sel.size(); i++)
                    sel[i] = true;
            } else {
                /* plain click: select only this row */
                sel.assign(g_entries.size(), false);
                if (clicked < (int)sel.size()) sel[clicked] = true;
                anchor_row = clicked;
            }
            redraw();
            update_selection_menu();
            return 1;
        }
        case FL_DRAG:
            if (resizing_cols) {
                apply_column_resize(Fl::event_x());
                return 1;
            }
            return Fl_Group::handle(event);
        case FL_RELEASE:
            if (resizing_cols) {
                apply_column_resize(Fl::event_x());
                resizing_cols = false;
                resize_col = -1;
                remember_column_widths();
                save_prefs();
                set_resize_cursor(header_divider_hit(Fl::event_x(), Fl::event_y()) >= 0);
                return 1;
            }
            return Fl_Group::handle(event);
        case FL_ENTER:
        case FL_MOVE:
            if (!resizing_cols) set_resize_cursor(header_divider_hit(Fl::event_x(), Fl::event_y()) >= 0);
            return 1;
        case FL_LEAVE:
            if (!resizing_cols) set_resize_cursor(false);
            return 1;
        case FL_MOUSEWHEEL: {
            int total      = (int)g_entries.size();
            int max_scroll = total - visible_rows;
            if (max_scroll < 0) max_scroll = 0;
            scroll_y += Fl::event_dy() * 3;
            if (scroll_y < 0)          scroll_y = 0;
            if (scroll_y > max_scroll) scroll_y = max_scroll;
            sb->value(scroll_y, visible_rows, 0, total > 0 ? total : 1);
            redraw();
            return 1;
        }
        case FL_DND_ENTER:
        case FL_DND_DRAG:
            return 1;
        case FL_DND_RELEASE:
            dnd_incoming = true;
            return 1;
        case FL_PASTE:
            /* only process paste that came from a real DnD drop,
             * not from an X11 middle-click primary-selection paste */
            if (dnd_incoming) {
                dnd_incoming = false;
                handle_drop(Fl::event_text());
            }
            return 1;
        default:
            return Fl_Group::handle(event);
        }
    }

    void handle_drop(const char *text);
};

/* ---- mkdir -p ---------------------------------------------------------- */

static int mkdirs(const char *path)
{
    char *tmp = strdup(path);
    if (!tmp) return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
            *p = saved;
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
    free(tmp);
    return 0;
}

static int mkdirs_for_file(const char *path)
{
    char *tmp = strdup(path);
    if (!tmp) return -1;
    /* find last separator (forward or backward slash) */
    char *slash = NULL;
    for (char *p = tmp; *p; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    if (slash && slash != tmp) {
        *slash = '\0';
        int r = mkdirs(tmp);
        free(tmp);
        return r;
    }
    free(tmp);
    return 0;
}

/* ---- compression settings --------------------------------------------- */

static int g_compression_method = BP_METHOD_BOLTLZ;
static int g_compression_level  = 1; /* 0=fast, 1=normal (default), 2=best */
static int g_fluff_multithread  = 0;
static int g_interchange_profile = 0;

#ifdef BOLTPACK_NET
/* Create dialog action: Save to disk, or Send over the network. */
enum { CREATE_CANCEL = 0, CREATE_SAVE = 1, CREATE_SEND = 2 };
static std::string g_send_last_target;  /* remembered manual entry, in-session */
#endif
#ifndef BOLTPACK_NET
enum { CREATE_CANCEL = 0, CREATE_SAVE = 1 };
#endif

/* Block sizes vary per method:
 *   BoltLite   (16-bit): 8K / 16K / 32K  -- ring buffer is only 2 KB
 *   boltlz(32-bit): 16K / 32K / 64K -- fast codec, default is 32K
 *   Fluff  (32-bit): always 256K -- level controls chain depth, not block size
 *   zstd   (64-bit): always 256K -- level controls zstd compression level */
static bp_uint32 level_to_block_size(int method, int level)
{
    if (method == BP_METHOD_BOLTLITE) {
        switch (level) {
        case 0: return BP_BLOCK_8K;
        case 2: return BP_BLOCK_32K;
        default: return BP_BLOCK_16K;
        }
    } else if (method == BP_METHOD_FLUFF || method == BP_METHOD_ZSTD) {
        (void)level;
        return BP_BLOCK_256K; /* level controls compression quality, not block size */
    } else {
        /* boltlz and fallback */
        switch (level) {
        case 0: return BP_BLOCK_16K;
        case 2: return BP_BLOCK_64K;
        default: return BP_BLOCK_32K;
        }
    }
}

/* Map GUI level (0=fast, 1=normal, 2=best) to Fluff chain-depth level (1-9). */
static int fluff_gui_level(int level)
{
    switch (level) {
    case 0: return 3;
    case 2: return 9;
    default: return 6;
    }
}

#ifdef BOLTPACK_ZSTD
static int zstd_gui_level(int level)
{
    switch (level) {
    case 0: return 1; /* Fast */
    case 2: return 9; /* Best */
    default: return 3; /* Normal (default) */
    }
}
#endif

#ifdef BOLTPACK_FAST_FLUFF
static int fast_fluff_thread_count()
{
    unsigned int nthreads = std::thread::hardware_concurrency();

    if (nthreads == 0U)
        return 2;
    if (nthreads <= 1U)
        return 0;
    return (int)nthreads;
}
#endif

static void prefs_path_str(char *buf, size_t size)
{
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && appdata[0])
        snprintf(buf, size, "%s\\BoltPack\\gui.conf", appdata);
    else
        snprintf(buf, size, "gui.conf");
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        snprintf(buf, size, "%s/boltpack/gui.conf", xdg);
    else {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(buf, size, "%s/.config/boltpack/gui.conf", home);
    }
#endif
}

static int clamp_window_width(int width)
{
    if (width < 500) width = 500;
    return width;
}

static int clamp_window_height(int height)
{
    if (height < 300) height = 300;
    return height;
}

static int default_window_x()
{
    int x = (Fl::w() - MAIN_W) / 2;
    if (x < 0) x = 0;
    return x;
}

static int default_window_y()
{
    int y = (Fl::h() - MAIN_H) / 2;
    if (y < 0) y = 0;
    return y;
}

static void capture_window_geometry()
{
    if (!g_main_window) return;

    g_persisted_window_x = g_main_window->x();
    g_persisted_window_y = g_main_window->y();
    g_persisted_window_w = clamp_window_width(g_main_window->w());
    g_persisted_window_h = clamp_window_height(g_main_window->h());
    g_have_window_geometry = true;
}

static void close_main_window()
{
    save_prefs();
    if (g_main_window) g_main_window->hide();
}

static void load_prefs()
{
    char path[1024];
    prefs_path_str(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int v;
        int w0, w1, w2, w3, w4, w5;
        int wx, wy, ww, wh;
        if (sscanf(line, "method=%d", &v) == 1) {
            if (v >= BP_METHOD_STORED && v <= BP_METHOD_FLUFF)
                g_compression_method = v;
        } else if (sscanf(line, "level=%d", &v) == 1) {
            if (v >= 0 && v <= 2)
                g_compression_level = v;
        } else if (sscanf(line, "fluff_mt=%d", &v) == 1) {
            g_fluff_multithread = v ? 1 : 0;
        } else if (sscanf(line, "interchange_profile=%d", &v) == 1) {
            g_interchange_profile = v ? 1 : 0;
        } else if (sscanf(line, "col_widths=%d,%d,%d,%d,%d,%d",
                          &w0, &w1, &w2, &w3, &w4, &w5) == NUM_COLS) {
            g_persisted_col_widths[0] = w0;
            g_persisted_col_widths[1] = w1;
            g_persisted_col_widths[2] = w2;
            g_persisted_col_widths[3] = w3;
            g_persisted_col_widths[4] = w4;
            g_persisted_col_widths[5] = w5;
            g_have_persisted_col_widths = true;
        } else if (sscanf(line, "window=%d,%d,%d,%d", &wx, &wy, &ww, &wh) == 4) {
            g_persisted_window_x = wx;
            g_persisted_window_y = wy;
            g_persisted_window_w = clamp_window_width(ww);
            g_persisted_window_h = clamp_window_height(wh);
            g_have_window_geometry = true;
        }
    }
    fclose(f);
}

static void save_prefs()
{
    char path[1024];
    if (g_main_window) capture_window_geometry();
    prefs_path_str(path, sizeof(path));
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    /* find last separator */
    char *slash = NULL;
    for (char *p = dir; *p; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    if (slash) { *slash = '\0'; mkdirs(dir); }
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "method=%d\nlevel=%d\nfluff_mt=%d\n"
               "interchange_profile=%d\n",
            g_compression_method, g_compression_level, g_fluff_multithread,
            g_interchange_profile);
    if (g_have_persisted_col_widths) {
        fprintf(f, "col_widths=%d,%d,%d,%d,%d,%d\n",
                g_persisted_col_widths[0], g_persisted_col_widths[1],
                g_persisted_col_widths[2], g_persisted_col_widths[3],
                g_persisted_col_widths[4], g_persisted_col_widths[5]);
    }
    if (g_have_window_geometry) {
        fprintf(f, "window=%d,%d,%d,%d\n",
                g_persisted_window_x, g_persisted_window_y,
                g_persisted_window_w, g_persisted_window_h);
    }
    fclose(f);
}

/* ---- progress bar with outlined text ----------------------------------- */

class OutlinedProgress : public Fl_Progress {
public:
    OutlinedProgress(int X, int Y, int W, int H, const char *L = 0)
        : Fl_Progress(X, Y, W, H, L) {}
protected:
    void draw() {
        /* draw the normal progress bar (fill + track) */
        Fl_Progress::draw();
        /* draw percentage label with black outline for contrast */
        const char *lbl = label();
        if (!lbl || !*lbl) return;
        fl_font(labelfont(), labelsize());
        int tw = 0, th = 0;
        fl_measure(lbl, tw, th, 0);
        int cx = x() + (w() - tw) / 2;
        int cy = y() + (h() + th) / 2 - fl_descent();
        /* outline: draw in black at 8 surrounding offsets */
        fl_color(FL_BLACK);
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                if (dx || dy)
                    fl_draw(lbl, cx + dx, cy + dy);
        /* foreground: draw in white on top */
        fl_color(FL_WHITE);
        fl_draw(lbl, cx, cy);
    }
};

/* ---- progress window --------------------------------------------------- */

struct ProgressCtx {
    Fl_Window          *win;
    Fl_Box             *file_label;
    Fl_Box             *count_label;
    OutlinedProgress   *bar;
    Fl_Button          *cancel_btn;
    std::atomic<bool>   cancelled;
    std::atomic<bool>   finished;
    /* shared state written by worker, read by GUI update callback */
    std::mutex          mtx;
    char                file_buf[512];
    char                count_buf[128];
    float               pct;
    /* pre-calculated totals (set before worker starts, read-only after) */
    double              total_bytes;
    unsigned long       total_files;
};

static void cancel_cb(Fl_Widget *, void *data)
{
    ProgressCtx *ctx = (ProgressCtx *)data;
    ctx->cancelled.store(true);
}

/* Timer callback — runs on the main GUI thread every 50 ms, polls shared
 * progress state written by the worker thread and updates the widgets. */
static void progress_timer_cb(void *data)
{
    ProgressCtx *ctx = (ProgressCtx *)data;
    {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        ctx->file_label->copy_label(ctx->file_buf);
        ctx->count_label->copy_label(ctx->count_buf);
        ctx->bar->value(ctx->pct);
        /* update percentage text on the bar */
        static char pct_buf[8];
        snprintf(pct_buf, sizeof(pct_buf), "%.0f%%", ctx->pct);
        ctx->bar->copy_label(pct_buf);
    }
    if (!ctx->finished.load())
        Fl::repeat_timeout(0.05, progress_timer_cb, data);
}

static ProgressCtx *create_progress_window(const char *title)
{
    ProgressCtx *ctx = new ProgressCtx;
    ctx->cancelled.store(false);
    ctx->finished.store(false);
    ctx->file_buf[0] = '\0';
    ctx->count_buf[0] = '\0';
    ctx->pct = 0.0f;
    ctx->total_bytes = 0.0;
    ctx->total_files = 0;

    ctx->win = new Fl_Double_Window(400, 168, title);
    ctx->win->set_non_modal();

    ctx->file_label  = new Fl_Box(10, 10, 380, 25, "");
    ctx->file_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    ctx->count_label = new Fl_Box(10, 40, 380, 25, "");
    ctx->count_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    ctx->count_label->labelsize(12);

    ctx->bar = new OutlinedProgress(10, 75, 380, 25);
    ctx->bar->minimum(0.0f);
    ctx->bar->maximum(100.0f);
    ctx->bar->value(0.0f);
    ctx->bar->color(fl_rgb_color(0xE0, 0xE0, 0xE0));  /* track: light grey */
    ctx->bar->selection_color(fl_rgb_color(0x19, 0x76, 0xD2)); /* fill: Material blue */
    ctx->bar->labelfont(FL_HELVETICA_BOLD);
    ctx->bar->labelsize(12);
    ctx->bar->copy_label("0%");

    ctx->cancel_btn = new Fl_Button(155, 122, 90, 30, "Cancel");
    ctx->cancel_btn->callback(cancel_cb, ctx);

    ctx->win->end();
    ctx->win->show();
    Fl::add_timeout(0.05, progress_timer_cb, ctx);
    return ctx;
}

static void destroy_progress_window(ProgressCtx *ctx)
{
    Fl::remove_timeout(progress_timer_cb, ctx);
    ctx->win->hide();
    Fl::delete_widget(ctx->win);
    delete ctx;
}

/* ---- compression dialog ----------------------------------------------- */

struct CompDlgState {
    int             action;     /* CREATE_CANCEL / CREATE_SAVE / CREATE_SEND */
    int             method;
    int             level;
    int             interchange_profile;
    int             n_methods;
    int             method_ids[5];
    Fl_Round_Button *method_btns[5];
    Fl_Check_Button *interchange_btn;
#ifdef BOLTPACK_FAST_FLUFF
    int             fast_fluff;
#endif
    Fl_Window      *win;
    Fl_Box         *level_sep;   /* separator line above level section */
    Fl_Box         *level_title; /* section heading */
    Fl_Group       *level_grp;
    Fl_Round_Button *level_btns[3];
#ifdef BOLTPACK_FAST_FLUFF
    Fl_Check_Button *fast_fluff_btn;
#endif
};

/* Radio labels per method (indices 0=fast 1=normal 2=best) */
static const char * const s_level_labels[5][3] = {
    /* Stored  */ { "",                                    "",                                    ""                                       },
    /* BoltLite    */ { "Fast \xe2\x80\x94 8 KB blocks",      "Normal \xe2\x80\x94 16 KB blocks",    "Best \xe2\x80\x94 32 KB blocks"         },
    /* boltlz */ { "Fast \xe2\x80\x94 16 KB blocks",     "Normal \xe2\x80\x94 32 KB blocks",    "Best \xe2\x80\x94 64 KB blocks"         },
    /* Fluff   */ { "Fast \xe2\x80\x94 level 3 (quick)",  "Normal \xe2\x80\x94 level 6",          "Best \xe2\x80\x94 level 9 (slow)"       },
    /* zstd    */ { "Fast \xe2\x80\x94 level 1",          "Normal \xe2\x80\x94 level 3 (default)","Best \xe2\x80\x94 level 9 (ultra)"      }
};

/* Section heading per method */
static const char * const s_level_title[5] = {
    /* Stored  */ "",
    /* BoltLite    */ "Block Size",
    /* boltlz */ "Block Size",
    /* Fluff   */ "Compression Level (block size fixed at 256 KB)",
    /* zstd    */ "Compression Level (block size fixed at 256 KB)"
};

/* Default level (0/1/2) when switching to a method */
static const int s_method_default_level[] = { 1, 1, 1, 2, 1 }; /* Fluff defaults to Best; zstd defaults to Normal */

static void cdlg_update_level_labels(CompDlgState *s)
{
    int m = (s->method >= 0 && s->method <= 4) ? s->method : 2;
    int i;

    for (i = 0; i < s->n_methods; i++) {
        s->method_btns[i]->value(s->method_ids[i] == s->method ? 1 : 0);
        if (s->interchange_profile)
            s->method_btns[i]->deactivate();
        else
            s->method_btns[i]->activate();
    }

    if (s->interchange_profile || s->method == BP_METHOD_STORED) {
        s->level_sep->hide();
        s->level_title->hide();
        s->level_grp->hide();
    } else {
        s->level_sep->show();
        s->level_title->show();
        s->level_title->copy_label(s_level_title[m]);
        s->level_grp->show();
        for (i = 0; i < 3; i++) {
            s->level_btns[i]->copy_label(s_level_labels[m][i]);
            s->level_btns[i]->value(i == s->level ? 1 : 0);
        }
    }
#ifdef BOLTPACK_FAST_FLUFF
    if (!s->interchange_profile && s->method == BP_METHOD_FLUFF) {
        s->fast_fluff_btn->value(s->fast_fluff ? 1 : 0);
        s->fast_fluff_btn->show();
    } else {
        s->fast_fluff_btn->hide();
    }
#endif
    s->win->redraw();
}

static void cdlg_interchange_cb(Fl_Widget *w, void *data)
{
    CompDlgState *s = (CompDlgState *)data;
    s->interchange_profile = ((Fl_Check_Button *)w)->value() ? 1 : 0;
    if (s->interchange_profile) {
        s->method = BP_METHOD_BOLTLITE;
        s->level = 1;
    }
    cdlg_update_level_labels(s);
}

static void cdlg_method_cb(Fl_Widget *w, void *data)
{
    CompDlgState *s = (CompDlgState *)data;
    const char *lbl = ((Fl_Round_Button *)w)->label();
    if      (strncmp(lbl, "Stored",  6) == 0) s->method = BP_METHOD_STORED;
    else if (strncmp(lbl, "BoltLite",    4) == 0) s->method = BP_METHOD_BOLTLITE;
    else if (strncmp(lbl, "boltlz", 6) == 0) s->method = BP_METHOD_BOLTLZ;
    else if (strncmp(lbl, "Fluff",   5) == 0) s->method = BP_METHOD_FLUFF;
    else if (strncmp(lbl, "zstd",    4) == 0) s->method = BP_METHOD_ZSTD;
    /* reset to per-method default level */
    s->level = s_method_default_level[s->method];
    cdlg_update_level_labels(s);
}

static void cdlg_level_cb(Fl_Widget *w, void *data)
{
    CompDlgState *s = (CompDlgState *)data;
    int i;
    for (i = 0; i < 3; i++) {
        if ((Fl_Round_Button *)w == s->level_btns[i]) {
            s->level = i;
            break;
        }
    }
}

#ifdef BOLTPACK_FAST_FLUFF
static void cdlg_fast_fluff_cb(Fl_Widget *w, void *data)
{
    CompDlgState *s = (CompDlgState *)data;
    s->fast_fluff = ((Fl_Check_Button *)w)->value() ? 1 : 0;
}
#endif

static void cdlg_save_cb(Fl_Widget *, void *data)
{
    CompDlgState *s = (CompDlgState *)data;
    s->action = CREATE_SAVE;
    s->win->hide();
}

#ifdef BOLTPACK_NET
static void cdlg_send_cb(Fl_Widget *, void *data)
{
    CompDlgState *s = (CompDlgState *)data;
    s->action = CREATE_SEND;
    s->win->hide();
}
#endif

static void cdlg_cancel_cb(Fl_Widget *, void *data)
{
    CompDlgState *s = (CompDlgState *)data;
    s->action = CREATE_CANCEL;
    s->win->hide();
}

/* Returns the chosen action (CREATE_*); updates compression settings. */
static int show_compression_dialog()
{
#ifdef BOLTPACK_ZSTD
    int n_methods_for_height = 5;
#else
    int n_methods_for_height = 4;
#endif
    int W = 460;
    int ff_y_for_height = (32 + n_methods_for_height * 24 + 6) + 6 + 8 + 20 + 80;
    int profile_y = ff_y_for_height;
#ifdef BOLTPACK_FAST_FLUFF
    profile_y += 24;
#endif
    int H = profile_y + 72;
    int cx = g_main_window ? g_main_window->x() + (g_main_window->w() - W) / 2 : 200;
    int cy = g_main_window ? g_main_window->y() + (g_main_window->h() - H) / 2 : 200;

    CompDlgState s;
    s.action = CREATE_CANCEL;
    s.method = g_compression_method;
    s.level  = g_compression_level;
    s.interchange_profile = g_interchange_profile;
    if (s.interchange_profile) {
        s.method = BP_METHOD_BOLTLITE;
        s.level = 1;
    }
    s.n_methods = 0;
#ifdef BOLTPACK_FAST_FLUFF
    s.fast_fluff = g_fluff_multithread;
#endif

    s.win = new Fl_Window(cx, cy, W, H, "Create Archive");
    s.win->set_modal();

    /* method section */
    Fl_Box *ml = new Fl_Box(10, 10, W - 20, 20, "Compression Method");
    ml->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    ml->labelfont(FL_HELVETICA_BOLD);

    static const char *method_lbl[] = {
        "Stored \xe2\x80\x94 No compression \xe2\x80\x94 For all systems",
        "BoltLite \xe2\x80\x94 Lighter compression \xe2\x80\x94 16-bit systems and up",
        "boltlz \xe2\x80\x94 Fast, light compression \xe2\x80\x94 32-bit / 64-bit",
        "Fluff \xe2\x80\x94 Medium compression \xe2\x80\x94 32-bit / 64-bit"
#ifdef BOLTPACK_ZSTD
       ,"zstd \xe2\x80\x94 Best compression \xe2\x80\x94 64-bit"
#endif
    };
    static const int method_ids[] = {
        BP_METHOD_STORED, BP_METHOD_BOLTLITE, BP_METHOD_BOLTLZ, BP_METHOD_FLUFF
#ifdef BOLTPACK_ZSTD
       ,BP_METHOD_ZSTD
#endif
    };
    int n_methods = (int)(sizeof(method_ids) / sizeof(method_ids[0]));
    s.n_methods = n_methods;
    for (int i = 0; i < n_methods; i++) {
        s.method_ids[i] = method_ids[i];
        s.method_btns[i] = NULL;
    }
    int mgrp_h    = n_methods * 24 + 6;
    int sep_y     = 32 + mgrp_h + 6;
    int ttl_y     = sep_y + 8;
    int lvl_grp_y = ttl_y + 20;
    int ff_y      = lvl_grp_y + 80;

    Fl_Group *mgrp = new Fl_Group(10, 32, W - 20, mgrp_h);
    for (int i = 0; i < n_methods; i++) {
        Fl_Round_Button *rb = new Fl_Round_Button(18, 34 + i * 24, W - 28, 22,
                                                  method_lbl[i]);
        rb->type(FL_RADIO_BUTTON);
        rb->callback(cdlg_method_cb, &s);
        s.method_btns[i] = rb;
        if (method_ids[i] == s.method)
            rb->value(1);
    }
    mgrp->end();

    /* separator + level section (hidden when Stored is selected) */
    s.level_sep = new Fl_Box(10, sep_y, W - 20, 2);
    s.level_sep->box(FL_ENGRAVED_BOX);

    s.level_title = new Fl_Box(10, ttl_y, W - 20, 20, "");
    s.level_title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    s.level_title->labelfont(FL_HELVETICA_BOLD);

    s.level_grp = new Fl_Group(10, lvl_grp_y, W - 20, 76);
    for (int i = 0; i < 3; i++) {
        Fl_Round_Button *rb = new Fl_Round_Button(18, lvl_grp_y + 2 + i * 24, W - 28, 22, "");
        rb->type(FL_RADIO_BUTTON);
        rb->callback(cdlg_level_cb, &s);
        s.level_btns[i] = rb;
    }
    s.level_grp->end();

#ifdef BOLTPACK_FAST_FLUFF
    s.fast_fluff_btn = new Fl_Check_Button(18, ff_y, W - 28, 22,
                                           "Use multi-threaded Fluff (faster, slightly larger archives)");
    s.fast_fluff_btn->callback(cdlg_fast_fluff_cb, &s);
    s.fast_fluff_btn->value(s.fast_fluff ? 1 : 0);
#else
    (void)ff_y;
#endif

    s.interchange_btn = new Fl_Check_Button(18, profile_y, W - 28, 22,
        "Interchange Profile 1 (BoltLite, 16 KB; mini-reader compatible)");
    s.interchange_btn->callback(cdlg_interchange_cb, &s);
    s.interchange_btn->value(s.interchange_profile ? 1 : 0);

    /* apply initial labels / visibility based on current method */
    cdlg_update_level_labels(&s);

    /* action buttons */
#ifdef BOLTPACK_NET
    {
        Fl_Button *save_btn = new Fl_Button(60, H - 38, 90, 28, "Save");
        Fl_Button *send_btn = new Fl_Button(160, H - 38, 150, 28,
                                            "Send over Network");
        Fl_Button *cn_btn   = new Fl_Button(320, H - 38, 80, 28, "Cancel");
        save_btn->callback(cdlg_save_cb, &s);
        send_btn->callback(cdlg_send_cb, &s);
        cn_btn->callback(cdlg_cancel_cb, &s);
    }
#else
    {
        Fl_Button *save_btn = new Fl_Button(W / 2 - 90, H - 38, 80, 28, "Save");
        Fl_Button *cn_btn   = new Fl_Button(W / 2 + 10, H - 38, 80, 28, "Cancel");
        save_btn->callback(cdlg_save_cb, &s);
        cn_btn->callback(cdlg_cancel_cb, &s);
    }
#endif

    s.win->end();
    s.win->show();
    while (s.win->shown())
        Fl::wait();

    int action = s.action;
    delete s.win;

    if (action != CREATE_CANCEL) {
        g_compression_method = s.method;
        g_compression_level  = s.level;
        g_interchange_profile = s.interchange_profile;
#ifdef BOLTPACK_FAST_FLUFF
        g_fluff_multithread  = s.fast_fluff;
#endif
        save_prefs();
    }
    return action;
}

/* ---- progress callback ------------------------------------------------- */

/* Runs on the WORKER thread — must not call FLTK APIs directly.
 * Just writes to shared state; the timer on the main thread polls it. */
static int progress_cb(const bp_progress_t *p, void *user_data)
{
    ProgressCtx *ctx = (ProgressCtx *)user_data;

    {
        std::lock_guard<std::mutex> lk(ctx->mtx);

        snprintf(ctx->file_buf, sizeof(ctx->file_buf), "%.500s",
                 p->file_name ? p->file_name : "");

        /* Use pre-calculated total_files if archiver doesn't know the count */
        unsigned long fc = p->file_count ? (unsigned long)p->file_count
                                         : ctx->total_files;
        snprintf(ctx->count_buf, sizeof(ctx->count_buf), "Entry %lu of %lu",
                 (unsigned long)(p->file_index + 1), fc);

        /* archive_orig_done already includes file_bytes_done from the
         * archiver, so use it directly as the numerator.  For creation
         * the archiver's archive_orig_total grows with each byte (not a
         * pre-known total), so use our pre-calculated total_bytes. */
        double done  = u64_to_double(p->archive_orig_done);
        double total = ctx->total_bytes > 0 ? ctx->total_bytes
                                            : u64_to_double(p->archive_orig_total);
        ctx->pct = (total > 0) ? (float)(done / total * 100.0) : 0.0f;
        if (ctx->pct > 100.0f) ctx->pct = 100.0f;
    }

    return ctx->cancelled.load() ? 1 : 0;
}

/* ---- list callback ----------------------------------------------------- */

static int list_cb(const bp_entry_t *entry, void *user_data)
{
    (void)user_data;

    /* skip meta/rsrc entries in display */
    if (entry->entry_type == BP_TYPE_META || entry->entry_type == BP_TYPE_RSRC)
        return 0;

    EntryInfo e;
    e.name      = entry->name ? entry->name : "";
    e.orig_size = u64_to_double(entry->original_size);
    e.comp_size = u64_to_double(entry->compressed_size);
    e.ratio     = ratio_percent(e.orig_size, e.comp_size);
    e.method    = entry->method;
    e.date      = format_time(entry->mtime);
    e.entry_type = entry->entry_type;

    g_entries.push_back(e);
    return 0;
}

/* ---- open archive ------------------------------------------------------ */

static void do_open_archive(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fl_alert("Cannot open file:\n%s", path);
        return;
    }

    bp_io_t io;
    bp_io_from_file(&io, fp);

    bp_archive_info_t info;
    memset(&info, 0, sizeof(info));
    int rc = bp_archive_info(&io, &info);
    if (rc != BP_OK) {
        fclose(fp);
        fl_alert("Error reading archive:\n%s", bp_strerror(rc));
        return;
    }

    /* list entries */
    g_entries.clear();
    rewind(fp);
    rc = bp_archive_list(&io, list_cb, NULL);
    fclose(fp);

    if (rc != BP_OK) {
        g_entries.clear();
        fl_alert("Error listing archive:\n%s", bp_strerror(rc));
        return;
    }

    g_archive_path = path;
    g_info = info;
    g_archive_open = true;
    update_ui();
}

/* ---- close archive ----------------------------------------------------- */

/* ---- validate archive -------------------------------------------------- */

struct ValidateCtx {
    std::string log;
    int ok_count;
    int fail_count;
    ProgressCtx *progress;
    unsigned long file_total;
    unsigned long dir_total;
    unsigned long entry_total;
    ValidateCtx() : ok_count(0), fail_count(0), progress(NULL),
                    file_total(0), dir_total(0), entry_total(0) {}
};

struct ValidateWork {
    std::string archive_path;
    ValidateCtx vctx;
    int result;
};

static void validate_cb(const bp_entry_t *entry, int result, void *user_data)
{
    ValidateCtx *ctx = (ValidateCtx *)user_data;
    char line[512];
    if (result == BP_OK) {
        snprintf(line, sizeof(line), "  OK      %s\n", entry->name);
        ctx->ok_count++;
    } else {
        snprintf(line, sizeof(line), "  FAILED  %s  (%s)\n",
                 entry->name, bp_strerror(result));
        ctx->fail_count++;
    }
    ctx->log += line;
}

static int validate_progress_cb(const bp_progress_t *p, void *user_data)
{
    ValidateCtx *ctx = (ValidateCtx *)user_data;
    ProgressCtx *progress = ctx->progress;
    if (!progress)
        return 0;

    {
        std::lock_guard<std::mutex> lk(progress->mtx);

        snprintf(progress->file_buf, sizeof(progress->file_buf), "%.500s",
                 p->file_name ? p->file_name : "");

        snprintf(progress->count_buf, sizeof(progress->count_buf),
             "%lu files, %lu directories; validating entry %lu of %lu",
             ctx->file_total,
             ctx->dir_total,
             (unsigned long)(p->file_index + 1),
             ctx->entry_total ? ctx->entry_total
                      : (p->file_count ? (unsigned long)p->file_count
                               : progress->total_files));

        {
            double done  = u64_to_double(p->archive_orig_done);
            double total = progress->total_bytes > 0
                         ? progress->total_bytes
                         : u64_to_double(p->archive_orig_total);
            progress->pct = (total > 0) ? (float)(done / total * 100.0) : 0.0f;
            if (progress->pct > 100.0f)
                progress->pct = 100.0f;
        }
    }

    return progress->cancelled.load() ? 1 : 0;
}

static void validate_worker(ValidateWork *w)
{
    FILE *fp = fopen(w->archive_path.c_str(), "rb");
    if (!fp) {
        w->result = BP_ERR_IO;
        if (w->vctx.progress)
            w->vctx.progress->finished.store(true);
        return;
    }

    bp_io_t io;
    bp_io_from_file(&io, fp);

    w->result = bp_archive_test_progress(&io, validate_cb, &w->vctx,
                                         validate_progress_cb, &w->vctx);
    fclose(fp);

    if (w->vctx.progress)
        w->vctx.progress->finished.store(true);
}

static void do_validate()
{
    if (!g_archive_open) return;

    ValidateWork work;
    ProgressCtx *pctx = create_progress_window("Validating archive...");

    work.archive_path = g_archive_path;
    work.vctx.progress = pctx;
    work.result = BP_OK;
    work.vctx.file_total = (unsigned long)(g_info.entry_count - g_info.dir_count);
    work.vctx.dir_total = (unsigned long)g_info.dir_count;
    work.vctx.entry_total = (unsigned long)g_info.entry_count;

    pctx->total_bytes = u64_to_double(g_info.total_original);
    pctx->total_files = (unsigned long)g_info.entry_count;
    {
        std::lock_guard<std::mutex> lk(pctx->mtx);
        snprintf(pctx->count_buf, sizeof(pctx->count_buf),
                 "%lu files, %lu directories; preparing validation...",
                 work.vctx.file_total, work.vctx.dir_total);
        snprintf(pctx->file_buf, sizeof(pctx->file_buf), "Preparing validation...");
    }

    std::thread worker(validate_worker, &work);

    while (!pctx->finished.load())
        Fl::wait(0.05);

    worker.join();

    int rc = work.result;
    bool was_cancelled = pctx->cancelled.load();
    destroy_progress_window(pctx);

    if (was_cancelled || rc == BP_ERR_ABORTED) {
        fl_message("Validation cancelled.");
        return;
    }

    if (rc != BP_OK && rc != BP_ERR_CHECKSUM) {
        fl_alert("Test failed: %s", bp_strerror(rc));
        return;
    }

    char summary[128];
    if (work.vctx.fail_count == 0) {
        snprintf(summary, sizeof(summary),
                 "%d entr%s tested; all OK",
                 work.vctx.ok_count,
                 work.vctx.ok_count == 1 ? "y" : "ies");
    } else {
        snprintf(summary, sizeof(summary),
                 "%d OK, %d FAILED",
                 work.vctx.ok_count, work.vctx.fail_count);
    }

    Fl_Window *dlg = new Fl_Window(520, 420, "Validate Archive");
    dlg->set_modal();

    Fl_Text_Buffer *buf = new Fl_Text_Buffer();
    buf->text(work.vctx.log.c_str());
    Fl_Text_Display *disp = new Fl_Text_Display(10, 10, 500, 360);
    disp->buffer(buf);
    disp->textfont(FL_COURIER);
    disp->textsize(12);

    Fl_Box *sum_box = new Fl_Box(10, 376, 400, 24, "");
    sum_box->copy_label(summary);
    sum_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    Fl_Button *ok_btn = new Fl_Button(420, 374, 90, 30, "OK");
    ok_btn->callback([](Fl_Widget *w, void *) { w->window()->hide(); });

    dlg->end();
    dlg->show();
    while (dlg->shown()) Fl::wait();
    Fl::delete_widget(dlg);
}

/* ---- archive info ------------------------------------------------------ */

static void do_info()
{
    if (!g_archive_open) return;

    const bp_archive_info_t &info = g_info;
    bp_uint32 file_count = info.entry_count - info.dir_count;
    double orig_d = u64_to_double(info.total_original);
    double comp_d = u64_to_double(info.total_compressed);
    double arc_d  = u64_to_double(info.archive_size);
    int pct = (orig_d > 0.0) ? (int)(comp_d / orig_d * 100.0) : 0;

    char text[2048];
    snprintf(text, sizeof(text),
        "Archive:         %s\n"
        "Format version:  %u\n"
        "Created by:      BoltPack %u.%u\n"
        "Creator OS:      %s\n"
        "Creator CPU:     %s\n"
        "Method:          %s\n"
        "\n"
        "Archive size:    %s\n"
        "Entries:         %lu  (%lu file%s, %lu director%s)\n"
        "Original size:   %s\n"
        "Compressed:      %s  (%d%% of original)",
        g_archive_path.c_str(),
        (unsigned)info.version,
        (unsigned)info.creator_ver_major,
        (unsigned)info.creator_ver_minor,
        os_name(info.creator_os),
        cpu_name(info.creator_cpu),
        format_methods(info.method_counts),
        format_size(arc_d).c_str(),
        (unsigned long)info.entry_count,
        (unsigned long)file_count, file_count == 1 ? "" : "s",
        (unsigned long)info.dir_count, info.dir_count == 1 ? "y" : "ies",
        format_size(orig_d).c_str(),
        format_size(comp_d).c_str(),
        pct);

    Fl_Window *dlg = new Fl_Window(440, 300, "Archive Information");
    dlg->set_modal();

    Fl_Text_Buffer *buf = new Fl_Text_Buffer();
    buf->text(text);
    Fl_Text_Display *disp = new Fl_Text_Display(10, 10, 420, 240);
    disp->buffer(buf);
    disp->textfont(FL_COURIER);
    disp->textsize(13);

    Fl_Button *ok_btn = new Fl_Button(170, 258, 100, 30, "OK");
    ok_btn->callback([](Fl_Widget *w, void *) { w->window()->hide(); });

    dlg->end();
    dlg->show();
    while (dlg->shown()) Fl::wait();
    Fl::delete_widget(dlg);
}

static void do_close_archive()
{
    g_entries.clear();
    g_archive_path.clear();
    memset(&g_info, 0, sizeof(g_info));
    g_archive_open = false;
    update_ui();
}

/* ---- extract ----------------------------------------------------------- */

enum ExtractMode {
    EXTRACT_KEEP_FOLDERS = 0,
    EXTRACT_FILES_ONLY   = 1
};

struct ExtractCtx {
    std::string           dest_dir;
    ProgressCtx          *progress;
    std::set<std::string> filter; /* empty = extract all; non-empty = only these names */
    ExtractMode           mode;
    std::set<std::string> reserved_flat_paths;
    std::map<std::string, std::string> flattened_paths;
    struct DirectoryRestore {
        std::string path;
        bp_uint64 mtime;
        bool mtime_valid;
        bp_uint8 fat_valid;
        bp_uint8 fat_value;
    };
    std::vector<DirectoryRestore> directories;
    bp_metadata_sink_t metadata;
    std::string last_subject_name;
    std::string last_subject_path;
    bp_uint8 last_subject_type;
    bool last_subject_valid;
    bool last_subject_ok;
    int open_error;
    unsigned partial_count;
    bool preflight_refused;

    ExtractCtx()
        : progress(NULL), mode(EXTRACT_KEEP_FOLDERS), last_subject_type(0),
          last_subject_valid(false), last_subject_ok(false), open_error(BP_OK),
          partial_count(0), preflight_refused(false)
    {
    }
};

struct StagedExtractFile {
    bp_io_t io;                 /* must remain first: callbacks receive &io */
    std::string target;
    std::string temporary;
};

static LONG g_extract_temp_counter = 0;

static bool win_component_is_safe(const WCHAR *component, size_t length)
{
    size_t i, base_length = length;
    WCHAR base[16];
    if (length == 0 || component[length - 1] == L'.' ||
        component[length - 1] == L' ') return false;
    for (i = 0; i < length; ++i) {
        WCHAR c = component[i];
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'|' || c == L'?' || c == L'*' || c == L'\\') return false;
        if (c == L'.' && base_length == length) base_length = i;
    }
    if (base_length == 0 || base_length >= sizeof(base) / sizeof(base[0])) return true;
    memcpy(base, component, base_length * sizeof(WCHAR));
    base[base_length] = L'\0';
    if (_wcsicmp(base, L"CON") == 0 || _wcsicmp(base, L"PRN") == 0 ||
        _wcsicmp(base, L"AUX") == 0 || _wcsicmp(base, L"NUL") == 0) return false;
    if (base_length == 4 &&
        (_wcsnicmp(base, L"COM", 3) == 0 || _wcsnicmp(base, L"LPT", 3) == 0) &&
        base[3] >= L'1' && base[3] <= L'9') return false;
    return true;
}

/* Validate archive UTF-8 against Windows naming rules without ACP projection. */
static int project_windows_name(const char *utf8, char *local, size_t capacity)
{
    std::wstring wide;
    size_t start, i, length;
    if (utf8 == NULL || local == NULL || capacity == 0 || !bp_name_is_safe(utf8))
        return BP_ERR_PARTIAL;
    if (!win_utf8_to_wide(utf8, &wide)) return BP_ERR_PARTIAL;
    length = wide.size();
    if (length == 0 || length >= MAX_PATH) return BP_ERR_NAME_TOO_LONG;
    start = 0;
    for (i = 0; i <= length; ++i) {
        size_t component_length;
        if (i < length && wide[i] != L'/') continue;
        component_length = i - start;
        if (component_length == 0) {
            if (i == length && length > 0 && wide[length - 1] == L'/') break;
            return BP_ERR_INVALID_ARG;
        }
        if (!win_component_is_safe(wide.data() + start, component_length))
            return BP_ERR_INVALID_ARG;
        start = i + 1;
    }
    if (strlen(utf8) + 1 > capacity) return BP_ERR_NAME_TOO_LONG;
    memcpy(local, utf8, strlen(utf8) + 1);
    for (char *p = local; *p; ++p) if (*p == '/') *p = '\\';
    return BP_OK;
}

static int utf8_windows_path(const char *utf8, std::string *local)
{
    std::wstring wide;
    if (utf8 == NULL || local == NULL) return BP_ERR_INVALID_ARG;
    if (!win_utf8_to_wide(utf8, &wide)) return BP_ERR_PARTIAL;
    if (wide.size() >= MAX_PATH) return BP_ERR_NAME_TOO_LONG;
    local->assign(utf8);
    return BP_OK;
}

static FILE *open_utf8_file(const std::string &path, const wchar_t *mode)
{
    std::wstring wide;
    if (!win_utf8_to_wide(path.c_str(), &wide)) return NULL;
    return _wfopen(wide.c_str(), mode);
}

/* Open source bytes from the checked Windows file object itself. Opening with
   FILE_FLAG_OPEN_REPARSE_POINT and checking the handle closes the pathname
   attribute-check/open gap for file symlinks and other reparse files. */
static FILE *open_windows_source_file(
    const char *path, struct _stat64 *st,
    const BY_HANDLE_FILE_INFORMATION *expected)
{
    std::wstring wide;
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION info;
    int fd;
    FILE *fp;

    if (path == NULL || st == NULL || !win_utf8_to_wide(path, &wide))
        return NULL;
    handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                         OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                         NULL);
    if (handle == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileInformationByHandle(handle, &info) || expected == NULL ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT |
                                  FILE_ATTRIBUTE_DIRECTORY)) != 0) {
        CloseHandle(handle);
        return NULL;
    }
    if (info.dwVolumeSerialNumber != expected->dwVolumeSerialNumber ||
        info.nFileIndexHigh != expected->nFileIndexHigh ||
        info.nFileIndexLow != expected->nFileIndexLow) {
        CloseHandle(handle);
        return NULL;
    }
    fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        return NULL;
    }
    if (_fstat64(fd, st) != 0 || !S_ISREG(st->st_mode)) {
        _close(fd);
        return NULL;
    }
    fp = _fdopen(fd, "rb");
    if (fp == NULL) _close(fd);
    return fp;
}

static int ensure_windows_directories(const std::string &root,
                                     const std::string &relative,
                                     bool include_leaf)
{
    std::string current = root;
    size_t pos = 0, end = relative.size();
    DWORD attr = GetFileAttributesA(current.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
        (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return BP_ERR_INVALID_ARG;
    while (end > 0 && (relative[end - 1] == '\\' || relative[end - 1] == '/')) --end;
    while (pos < end) {
        size_t next = relative.find('\\', pos);
        bool leaf;
        if (next == std::string::npos || next > end) next = end;
        leaf = next == end;
        if (!include_leaf && leaf) break;
        if (!current.empty() && current[current.size() - 1] != '\\' &&
            current[current.size() - 1] != '/') current += '\\';
        current.append(relative, pos, next - pos);
        attr = GetFileAttributesA(current.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) ||
                (!CreateDirectoryA(current.c_str(), NULL) &&
                 GetLastError() != ERROR_ALREADY_EXISTS)) return BP_ERR_IO;
            attr = GetFileAttributesA(current.c_str());
        }
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY) ||
            (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return BP_ERR_INVALID_ARG;
        pos = next + 1;
    }
    return BP_OK;
}

static int open_windows_staged_file(const std::string &target,
                                    std::string *temporary, FILE **file)
{
    std::string directory = target;
    size_t slash = directory.find_last_of("\\/");
    int attempt;
    if (slash == std::string::npos) return BP_ERR_INVALID_ARG;
    directory.resize(slash + 1);
    for (attempt = 0; attempt < 256; ++attempt) {
        char suffix[48];
        LONG serial = InterlockedIncrement(&g_extract_temp_counter);
        HANDLE handle;
        int fd;
        FILE *fp;
        snprintf(suffix, sizeof(suffix), ".boltpack-%08lx.tmp",
                 (unsigned long)(GetTickCount() ^ GetCurrentProcessId() ^ serial));
        *temporary = directory + suffix;
        handle = CreateFileA(temporary->c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT,
                             NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            return BP_ERR_IO;
        }
        fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_WRONLY);
        if (fd < 0) { CloseHandle(handle); DeleteFileA(temporary->c_str()); return BP_ERR_IO; }
        fp = _fdopen(fd, "wb");
        if (fp == NULL) { _close(fd); DeleteFileA(temporary->c_str()); return BP_ERR_IO; }
        *file = fp;
        return BP_OK;
    }
    return BP_ERR_IO;
}

static int apply_windows_fat(const std::string &path, bp_uint8 valid, bp_uint8 value)
{
    DWORD old_attributes = GetFileAttributesA(path.c_str());
    DWORD actual, desired;
    if (old_attributes == INVALID_FILE_ATTRIBUTES) return BP_ERR_PARTIAL;
    desired = (old_attributes & ~(DWORD)valid) | (DWORD)value;
    if (!SetFileAttributesA(path.c_str(), desired)) return BP_ERR_PARTIAL;
    actual = GetFileAttributesA(path.c_str());
    return actual != INVALID_FILE_ATTRIBUTES && (actual & valid) == value
         ? BP_OK : BP_ERR_PARTIAL;
}

static int queue_directory_restore(ExtractCtx *ctx, const std::string &path,
                                   bp_uint64 mtime, bool mtime_valid)
{
    size_t i;
    if (ctx->directories.size() >= 65535U) return BP_ERR_OVERFLOW;
    for (i = 0; i < ctx->directories.size(); ++i) {
        if (ctx->directories[i].path == path) {
            if (mtime_valid) {
                ctx->directories[i].mtime = mtime;
                ctx->directories[i].mtime_valid = true;
            }
            return BP_OK;
        }
    }
    ExtractCtx::DirectoryRestore node;
    node.path = path;
    node.mtime = mtime;
    node.mtime_valid = mtime_valid;
    node.fat_valid = 0;
    node.fat_value = 0;
    ctx->directories.push_back(node);
    return BP_OK;
}

static int win_set_path_mtime_utf8(const std::string &path, bp_uint64 mtime)
{
    const ULONGLONG ticks_per_second = 10000000ULL;
    const ULONGLONG unix_epoch_filetime = 11644473600ULL * ticks_per_second;
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
    handle = win_CreateFileA_utf8(path.c_str(), FILE_WRITE_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) return BP_ERR_PARTIAL;
    if (SetFileTime(handle, NULL, NULL, &file_time)) result = BP_OK;
    if (!CloseHandle(handle)) result = BP_ERR_PARTIAL;
    return result;
}

static int finish_directory_restores(ExtractCtx *ctx)
{
    int result = BP_OK;
    std::sort(ctx->directories.begin(), ctx->directories.end(),
              [](const ExtractCtx::DirectoryRestore &a,
                 const ExtractCtx::DirectoryRestore &b) {
                  return a.path.size() > b.path.size();
              });
    for (size_t i = 0; i < ctx->directories.size(); ++i) {
        const ExtractCtx::DirectoryRestore &node = ctx->directories[i];
        if (node.mtime_valid &&
            win_set_path_mtime_utf8(node.path, node.mtime) != BP_OK)
            result = BP_ERR_PARTIAL;
        if (node.fat_valid && apply_windows_fat(node.path, node.fat_valid,
                                                node.fat_value) != BP_OK)
            result = BP_ERR_PARTIAL;
    }
    ctx->directories.clear();
    return result;
}

static bool selection_needs_extract_mode_choice(const std::vector<std::string> &names)
{
    for (size_t i = 0; i < names.size(); i++) {
        const std::string &name = names[i];
        if (name.empty())
            continue;
        if (name[name.size() - 1] == '/')
            return true;
        if (name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos)
            return true;
    }
    return false;
}

static bool extract_filter_matches(const ExtractCtx *ctx, const std::string &ename)
{
    std::set<std::string>::const_iterator fit;

    if (ctx->filter.empty())
        return true;

    for (fit = ctx->filter.begin(); fit != ctx->filter.end(); ++fit) {
        const std::string &f = *fit;
        if (ename == f)
            return true;
        if (!f.empty() && f[f.size() - 1] == '/' &&
            ename.size() > f.size() && ename.substr(0, f.size()) == f)
            return true;
    }

    return false;
}

static std::string leaf_name(const std::string &path)
{
    size_t end = path.size();
    size_t pos;

    while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\'))
        end--;
    pos = path.find_last_of("/\\", end == 0 ? 0 : end - 1);
    if (pos == std::string::npos)
        return path.substr(0, end);
    return path.substr(pos + 1, end - pos - 1);
}

static std::string path_with_suffix(const std::string &filename, unsigned int index)
{
    size_t dot;
    std::string base;
    std::string ext;
    char suffix[16];

    dot = filename.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        base = filename;
    } else {
        base = filename.substr(0, dot);
        ext  = filename.substr(dot);
    }

    snprintf(suffix, sizeof(suffix), "-%02u", index);
    return base + suffix + ext;
}

static std::string join_dest_path(const std::string &dir, const std::string &name)
{
    if (dir.empty())
        return name;
    if (dir[dir.size() - 1] == '/' || dir[dir.size() - 1] == '\\')
        return dir + name;
    return dir + "/" + name;
}

static std::string reserve_flattened_path(ExtractCtx *ctx, const std::string &entry_name)
{
    std::string filename = leaf_name(entry_name);
    std::string path;
    unsigned int index;

    if (filename.empty())
        filename = "unnamed";

    path = join_dest_path(ctx->dest_dir, filename);
    if (ctx->reserved_flat_paths.find(path) == ctx->reserved_flat_paths.end() &&
        access(path.c_str(), 0) != 0) {
        ctx->reserved_flat_paths.insert(path);
        return path;
    }

    for (index = 1;; index++) {
        std::string candidate = join_dest_path(ctx->dest_dir,
                                               path_with_suffix(filename, index));
        if (ctx->reserved_flat_paths.find(candidate) == ctx->reserved_flat_paths.end() &&
            access(candidate.c_str(), 0) != 0) {
            ctx->reserved_flat_paths.insert(candidate);
            return candidate;
        }
    }
}

struct ExtractPreflight {
    ExtractCtx *ctx;
    std::map<std::wstring, int> nodes; /* 1 = directory, 2 = non-directory */
    std::set<std::wstring> primaries;
    unsigned int primary_count;
    int result;
    ExtractPreflight() : ctx(NULL), primary_count(0), result(BP_OK) {}
};

static bp_io_t *extract_open_cb(const bp_entry_t *entry, void *user_data);
static int extract_done_cb(const bp_entry_t *entry, bp_io_t *out, int status,
                           void *user_data);

static std::wstring windows_case_key(const std::string &local)
{
    std::wstring wide;
    std::wstring key;
    if (!win_utf8_to_wide(local.c_str(), &wide)) return key;
    key.assign(wide);
    while (!key.empty() && key[key.size() - 1] == L'\\') key.resize(key.size() - 1);
    if (!key.empty()) CharLowerBuffW(&key[0], (DWORD)key.size());
    return key;
}

static int extract_preflight_entry(const bp_entry_t *entry, void *user_data)
{
    ExtractPreflight *preflight = (ExtractPreflight *)user_data;
    ExtractCtx *ctx = preflight->ctx;
    std::string ename(entry->name ? entry->name : "");
    char projected[MAX_PATH * 4];
    std::string local, path;
    int rc;
    if (!entry->name || !bp_name_is_safe(entry->name)) {
        preflight->result = BP_ERR_INVALID_ARG;
        return 1;
    }
    if (entry->entry_type == BP_TYPE_META || entry->entry_type == BP_TYPE_RSRC)
        return 0;
    if (!extract_filter_matches(ctx, ename) ||
        (entry->entry_type == BP_TYPE_DIR && !ctx->filter.empty() &&
         ctx->mode == EXTRACT_FILES_ONLY)) return 0;
    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR &&
        entry->entry_type != BP_TYPE_SYMLINK) return 0;
    if (entry->entry_type == BP_TYPE_SYMLINK &&
        !ctx->filter.empty() && ctx->mode == EXTRACT_FILES_ONLY) return 0;
    if (++preflight->primary_count > 65535U) {
        preflight->result = BP_ERR_OVERFLOW;
        return 1;
    }
    rc = project_windows_name(entry->name, projected, sizeof(projected));
    if (rc != BP_OK) { preflight->result = rc; return 1; }
    local = projected;
    if (!ctx->filter.empty() && ctx->mode == EXTRACT_FILES_ONLY) {
        if (entry->entry_type != BP_TYPE_DATA) return 0;
        path = reserve_flattened_path(ctx, local);
        ctx->flattened_paths[ename] = path;
        return 0;
    }

    std::wstring key = windows_case_key(local);
    if (key.empty()) { preflight->result = BP_ERR_PARTIAL; return 1; }
    if (!preflight->primaries.insert(key).second) {
        preflight->result = BP_ERR_PARTIAL;
        return 1;
    }
    bool is_dir = entry->entry_type == BP_TYPE_DIR;
    size_t pos = 0;
    for (;;) {
        size_t slash = key.find(L'\\', pos);
        bool final = slash == std::wstring::npos;
        std::wstring prefix = final ? key : key.substr(0, slash);
        int kind = final ? (is_dir ? 1 : 2) : 1;
        std::map<std::wstring, int>::iterator found = preflight->nodes.find(prefix);
        if (found != preflight->nodes.end() && found->second != kind) {
            preflight->result = BP_ERR_PARTIAL;
            return 1;
        }
        if (found == preflight->nodes.end() && preflight->nodes.size() >= 65535U) {
            preflight->result = BP_ERR_OVERFLOW;
            return 1;
        }
        preflight->nodes[prefix] = kind;
        if (final) break;
        pos = slash + 1;
    }
    return 0;
}

static int extract_with_preflight(bp_io_t *io, ExtractCtx *ctx,
                                  bp_progress_fn progress_fn)
{
    ExtractPreflight preflight;
    int rc;
    preflight.ctx = ctx;
    rc = bp_archive_list(io, extract_preflight_entry, &preflight);
    if (preflight.result != BP_OK) {
        ctx->preflight_refused = true;
        return preflight.result;
    }
    if (rc != BP_OK) return rc;
    if (io->seek == NULL || io->seek(io->handle, 0, 0) != 0) return BP_ERR_IO;
    rc = bp_archive_extract_all_ex(io, extract_open_cb, extract_done_cb,
                                   progress_fn, ctx);
    {
        int dir_rc = finish_directory_restores(ctx);
        if (rc == BP_OK || rc == BP_ERR_PARTIAL) {
            if (ctx->open_error != BP_OK) rc = ctx->open_error;
            else if (dir_rc != BP_OK || ctx->partial_count != 0)
                rc = BP_ERR_PARTIAL;
        }
    }
    return rc;
}

static int extract_progress_cb(const bp_progress_t *p, void *user_data)
{
    ExtractCtx *ectx = (ExtractCtx *)user_data;
    return progress_cb(p, ectx->progress);
}

static bp_io_t *extract_open_cb(const bp_entry_t *entry, void *user_data)
{
    ExtractCtx *ctx = (ExtractCtx *)user_data;
    std::string ename(entry->name ? entry->name : "");
    std::string local_name;
    std::string path;
    char projected[MAX_PATH * 4];
    int rc;

    if (!entry->name || !bp_name_is_safe(entry->name)) {
        ctx->partial_count++;
        ctx->last_subject_valid = false;
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        if (!extract_filter_matches(ctx, ename) ||
            (!ctx->filter.empty() && ctx->mode == EXTRACT_FILES_ONLY)) {
            ctx->last_subject_valid = false;
            return NULL;
        }
        rc = project_windows_name(entry->name, projected, sizeof(projected));
        if (rc != BP_OK) { ctx->partial_count++; ctx->last_subject_valid = false; return NULL; }
        local_name = projected;
        path = join_dest_path(ctx->dest_dir, local_name);
        rc = ensure_windows_directories(ctx->dest_dir, local_name, true);
        if (rc != BP_OK) { ctx->open_error = rc; return NULL; }
        ctx->last_subject_name = ename;
        ctx->last_subject_path = path;
        ctx->last_subject_type = entry->entry_type;
        ctx->last_subject_valid = true;
        ctx->last_subject_ok = true;
        rc = queue_directory_restore(ctx, path, entry->mtime,
                     (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) != 0);
        if (rc != BP_OK) { ctx->open_error = rc; ctx->last_subject_ok = false; }
        if (entry->flags & BP_ENTRY_FLAG_MODE_VALID) ctx->partial_count++;
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_META) {
        if (!extract_filter_matches(ctx, ename)) return NULL;
        if (!ctx->filter.empty() && ctx->mode == EXTRACT_FILES_ONLY &&
            ctx->flattened_paths.find(ename) == ctx->flattened_paths.end()) return NULL;
        if (entry->platform != BP_META_PORTABLE || !ctx->last_subject_valid ||
            !ctx->last_subject_ok || ctx->last_subject_name != ename) {
            ctx->partial_count++;
            return NULL;
        }
        bp_metadata_sink_init(&ctx->metadata);
        return &ctx->metadata.io;
    }
    if (entry->entry_type == BP_TYPE_RSRC) {
        if (extract_filter_matches(ctx, ename) &&
            (ctx->filter.empty() || ctx->mode != EXTRACT_FILES_ONLY ||
             ctx->flattened_paths.find(ename) != ctx->flattened_paths.end()))
            ctx->partial_count++;
        return NULL;
    }

    if (!extract_filter_matches(ctx, ename)) {
        ctx->last_subject_valid = false;
        return NULL;
    }

    if (entry->entry_type != BP_TYPE_DATA) {
        ctx->partial_count++;
        ctx->last_subject_valid = false;
        return NULL;
    }
    rc = project_windows_name(entry->name, projected, sizeof(projected));
    if (rc != BP_OK) { ctx->partial_count++; ctx->last_subject_valid = false; return NULL; }
    local_name = projected;
    if (!ctx->filter.empty() && ctx->mode == EXTRACT_FILES_ONLY) {
        std::map<std::string, std::string>::const_iterator mapped =
            ctx->flattened_paths.find(ename);
        path = mapped == ctx->flattened_paths.end()
             ? reserve_flattened_path(ctx, local_name) : mapped->second;
        rc = ensure_windows_directories(ctx->dest_dir, "", false);
    } else {
        path = join_dest_path(ctx->dest_dir, local_name);
        rc = ensure_windows_directories(ctx->dest_dir, local_name, false);
    }
    if (rc != BP_OK) { ctx->open_error = rc; ctx->last_subject_valid = false; return NULL; }

    StagedExtractFile *staged = new (std::nothrow) StagedExtractFile();
    if (staged == NULL) { ctx->open_error = BP_ERR_NOMEM; return NULL; }
    FILE *fp = NULL;
    rc = open_windows_staged_file(path, &staged->temporary, &fp);
    if (rc != BP_OK) {
        delete staged;
        ctx->open_error = rc;
        ctx->last_subject_valid = false;
        return NULL;
    }
    staged->target = path;
    bp_io_from_file(&staged->io, fp);
    ctx->last_subject_name = ename;
    ctx->last_subject_path = path;
    ctx->last_subject_type = entry->entry_type;
    ctx->last_subject_valid = true;
    ctx->last_subject_ok = false;
    return &staged->io;
}

static int extract_done_cb(const bp_entry_t *entry, bp_io_t *out, int status,
                           void *user_data)
{
    ExtractCtx *ctx = (ExtractCtx *)user_data;
    if (entry->entry_type == BP_TYPE_META) {
        bp_portable_metadata_t parsed;
        int rc;
        (void)out;
        if (status != BP_OK) return status;
        rc = bp_metadata_parse(ctx->metadata.data, ctx->metadata.length, &parsed);
        if (rc == BP_ERR_CORRUPT) return rc;
        if (parsed.supported_version && parsed.subject_type != ctx->last_subject_type)
            return BP_ERR_CORRUPT;
        if (parsed.has_fat) {
            if (ctx->last_subject_type == BP_TYPE_DIR) {
                size_t i;
                bool found = false;
                for (i = 0; i < ctx->directories.size(); ++i) {
                    if (ctx->directories[i].path == ctx->last_subject_path) {
                        ctx->directories[i].fat_valid = parsed.fat_valid;
                        ctx->directories[i].fat_value = parsed.fat_value;
                        found = true;
                        break;
                    }
                }
                if (!found) return BP_ERR_IO;
            } else if (apply_windows_fat(ctx->last_subject_path,
                                         parsed.fat_valid,
                                         parsed.fat_value) != BP_OK) {
                ctx->partial_count++;
            }
        }
        if (rc == BP_ERR_PARTIAL || parsed.has_unknown) ctx->partial_count++;
        return (rc == BP_ERR_PARTIAL || parsed.has_unknown) ? BP_ERR_PARTIAL : BP_OK;
    }

    if (entry->entry_type == BP_TYPE_DIR) return status;

    StagedExtractFile *staged = (StagedExtractFile *)out;
    FILE *fp = (FILE *)out->handle;
    int result = status;
    if (status != BP_OK) {
        fclose(fp);
        DeleteFileA(staged->temporary.c_str());
        delete staged;
        ctx->last_subject_ok = false;
        return status;
    }
    if (fclose(fp) != 0) result = BP_ERR_IO;
    if (result == BP_OK && (entry->flags & BP_ENTRY_FLAG_MTIME_VALID)) {
        bp_off_t seconds = bp_u64_to_off(entry->mtime);
        time_t t = (time_t)seconds;
        struct _stat64 st;
        struct utimbuf ut;
        if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), entry->mtime) != 0 ||
            (bp_off_t)t != seconds || _stat64(staged->temporary.c_str(), &st) != 0) {
            result = BP_ERR_PARTIAL;
        } else {
            ut.actime = (time_t)st.st_atime;
            ut.modtime = t;
            if (utime(staged->temporary.c_str(), &ut) != 0) result = BP_ERR_PARTIAL;
        }
    }
    if (entry->flags & BP_ENTRY_FLAG_MODE_VALID) result = BP_ERR_PARTIAL;
    if (result == BP_OK || result == BP_ERR_PARTIAL) {
        if (!MoveFileExA(staged->temporary.c_str(), staged->target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileA(staged->temporary.c_str());
            delete staged;
            ctx->last_subject_ok = false;
            return BP_ERR_IO;
        }
        ctx->last_subject_ok = true;
        delete staged;
        if (result == BP_ERR_PARTIAL) ctx->partial_count++;
        return result;
    }
    DeleteFileA(staged->temporary.c_str());
    delete staged;
    ctx->last_subject_ok = false;
    return result;
}

#ifdef BOLTPACK_NET
/*
 * Reusable extraction entry point for archives received over the network.
 * Reuses the GUI's own extract callbacks so files land exactly as a normal
 * extraction would.  Entry names are already safety-checked by bp_net
 * (absolute/.. paths rejected) before this runs.  Matches the
 * bp_net_ui_t::extract_io signature for use as a callback.
 */
static int gui_extract_io(bp_io_t *aio, const char *out_dir, int force,
                          void *ud)
{
    ExtractCtx ctx;
    int rc;
    (void)force;
    (void)ud;
    rc = utf8_windows_path((out_dir && *out_dir) ? out_dir : ".", &ctx.dest_dir);
    if (rc != BP_OK) return rc;
    ctx.progress = NULL;
    ctx.mode     = EXTRACT_KEEP_FOLDERS;
    return extract_with_preflight(aio, &ctx, NULL);
}
#endif

struct ExtractWork {
    std::string  archive_path;
    ExtractCtx   ectx;
    int          result;
};

static void extract_worker(ExtractWork *w)
{
    FILE *fp = open_utf8_file(w->archive_path, L"rb");
    if (!fp) { w->result = BP_ERR_IO; w->ectx.progress->finished.store(true); return; }

    bp_io_t io;
    bp_io_from_file(&io, fp);

    w->result = extract_with_preflight(&io, &w->ectx, extract_progress_cb);
    fclose(fp);
    w->ectx.progress->finished.store(true);
}

static void do_extract()
{
    if (!g_archive_open) return;

    Fl_Native_File_Chooser chooser;
    chooser.title("Select destination folder");
    chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
    {
        std::string dir = g_archive_path;
        size_t pos = dir.find_last_of("/\\");
        if (pos != std::string::npos) dir = dir.substr(0, pos);
        chooser.directory(dir.c_str());
    }
    if (chooser.show() != 0) return;

    const char *dest = chooser.filename();
    if (!dest || !*dest) return;

    ExtractWork work;
    work.archive_path       = g_archive_path;
    if (utf8_windows_path(dest, &work.ectx.dest_dir) != BP_OK) {
        fl_alert("The selected destination is not a valid UTF-8 Windows path or is too long.");
        return;
    }
    work.ectx.progress      = create_progress_window("Extracting...");
    work.result              = BP_OK;

    std::thread worker(extract_worker, &work);

    while (!work.ectx.progress->finished.load())
        Fl::wait(0.05);

    worker.join();

    int rc = work.result;
    destroy_progress_window(work.ectx.progress);

    if (work.ectx.preflight_refused) {
        fl_alert("Extraction was not started. A selected name is not representable on Windows or collides with another selected path.");
    } else if (rc == BP_ERR_ABORTED) {
        fl_message("Extraction cancelled.");
    } else if (rc == BP_ERR_PARTIAL) {
        fl_message("Extraction finished with some names or attributes not restored.");
    } else if (rc != BP_OK) {
        fl_alert("Extraction error:\n%s", bp_strerror(rc));
    } else {
        fl_message("Extraction complete.\n%lu files extracted.",
                   (unsigned long)g_info.file_count);
    }
}

static void do_extract_selected()
{
    if (!g_archive_open || !g_table || g_table->selection_count() == 0) return;

    std::vector<std::string> names = g_table->selected_names();
    ExtractMode mode = EXTRACT_KEEP_FOLDERS;

    if (names.empty())
        return;

    if (selection_needs_extract_mode_choice(names)) {
        int choice = fl_choice("Selected entries include folders or files inside folders.\n\n"
                               "How would you like to extract them?\n\n"
                               "Keep folders: preserve the selected folder structure.\n"
                               "Files only: extract selected files into the chosen folder\n"
                               "and auto-rename name collisions.",
                               "Cancel", "Keep folders", "Files only");
        if (choice == 0) return;
        if (choice == 2)
            mode = EXTRACT_FILES_ONLY;
    }

    Fl_Native_File_Chooser chooser;
    chooser.title("Select destination folder");
    chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
    if (chooser.show() != 0) return;

    const char *dest = chooser.filename();
    if (!dest || !*dest) return;

    ExtractWork work;
    work.archive_path  = g_archive_path;
    if (utf8_windows_path(dest, &work.ectx.dest_dir) != BP_OK) {
        fl_alert("The selected destination is not a valid UTF-8 Windows path or is too long.");
        return;
    }
    work.ectx.progress = create_progress_window("Extracting...");
    work.ectx.mode     = mode;
    work.result        = BP_OK;

    /* populate filter from current selection */
    for (size_t i = 0; i < names.size(); i++) {
        std::string n = names[i];
        if (!n.empty())
            work.ectx.filter.insert(n);
    }

    std::thread worker(extract_worker, &work);

    while (!work.ectx.progress->finished.load())
        Fl::wait(0.05);

    worker.join();

    int rc = work.result;
    destroy_progress_window(work.ectx.progress);

    if (work.ectx.preflight_refused) {
        fl_alert("Extraction was not started. A selected name is not representable on Windows or collides with another selected path.");
    } else if (rc == BP_ERR_ABORTED) {
        fl_message("Extraction cancelled.");
    } else if (rc == BP_ERR_PARTIAL) {
        fl_message("Extraction finished with some names or attributes not restored.");
    } else if (rc != BP_OK) {
        fl_alert("Extraction error:\n%s", bp_strerror(rc));
    } else {
        fl_message("Extraction complete.");
    }
}

/* ---- create archive ---------------------------------------------------- */

static int add_file_to_archive(bp_archive_t *ar, const char *filepath,
                               const char *arc_name);
static int add_dir_recursive(bp_archive_t *ar, const char *dirpath,
                             const char *prefix);

static bp_uint16 win_gui_archive_mtime(time_t source_time, bp_uint64 *mtime)
{
    bp_off_t seconds;

    *mtime = bp_u64_from_u32(0);
    if (source_time < (time_t)0)
        return 0;
    seconds = (bp_off_t)source_time;
    if (seconds < 0 || (time_t)seconds != source_time)
        return 0;
    *mtime = bp_off_to_u64(seconds);
    return BP_ENTRY_FLAG_MTIME_VALID;
}

#ifdef _WIN32
struct WinSourcePathGuard {
    HANDLE handle;
    BY_HANDLE_FILE_INFORMATION info;

    WinSourcePathGuard() : handle(INVALID_HANDLE_VALUE)
    {
        memset(&info, 0, sizeof(info));
    }

    ~WinSourcePathGuard()
    {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }

    bool open(const char *path)
    {
        std::wstring wide;
        if (!win_utf8_to_wide(path, &wide)) return false;
        handle = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS |
                             FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (handle == INVALID_HANDLE_VALUE) return false;
        if (!GetFileInformationByHandle(handle, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            return false;
        }
        return true;
    }

    bool still_names(const char *path) const
    {
        std::wstring wide;
        HANDLE current;
        BY_HANDLE_FILE_INFORMATION now;
        bool same;
        if (handle == INVALID_HANDLE_VALUE ||
            !win_utf8_to_wide(path, &wide)) return false;
        current = CreateFileW(wide.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS |
                               FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (current == INVALID_HANDLE_VALUE) return false;
        same = GetFileInformationByHandle(current, &now) &&
               (now.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
               info.dwVolumeSerialNumber == now.dwVolumeSerialNumber &&
               info.nFileIndexHigh == now.nFileIndexHigh &&
               info.nFileIndexLow == now.nFileIndexLow;
        CloseHandle(current);
        return same;
    }
};

#ifdef BP_TEST_WIN_FLTK_DIR_GUARD_PAUSE
static void win_test_pause_after_directory_guard(DWORD attributes)
{
    static bool paused = false;
    HANDLE marker;
    if (paused || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return;
    paused = true;
    marker = CreateFileW(L"source-guard.ready", GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
    while (GetFileAttributesW(L"source-guard.release") ==
           INVALID_FILE_ATTRIBUTES)
        Sleep(10);
}
#endif

struct WinFatMetadataSource {
    bp_uint8 data[BP_META_FAT_SIZE];
    unsigned pos;
};

static long win_fat_metadata_read(void *handle, void *buffer, bp_uint32 count)
{
    WinFatMetadataSource *source = (WinFatMetadataSource *)handle;
    unsigned remaining = BP_META_FAT_SIZE - source->pos;
    if (count > remaining) count = remaining;
    memcpy(buffer, source->data + source->pos, count);
    source->pos += count;
    return (long)count;
}

static int add_win_fat_metadata(bp_archive_t *ar, const char *name,
                                bp_uint8 subject_type, DWORD attributes)
{
    WinFatMetadataSource source;
    bp_io_t io;
    int rc;
    if (attributes == INVALID_FILE_ATTRIBUTES) return BP_ERR_IO;
    rc = bp_metadata_fat_encode(source.data, sizeof(source.data), subject_type,
                                BP_META_FAT_MASK,
                                (bp_uint8)(attributes & BP_META_FAT_MASK));
    if (rc != BP_OK) return rc;
    source.pos = 0;
    io.read = win_fat_metadata_read;
    io.write = NULL;
    io.seek = NULL;
    io.tell = NULL;
    io.handle = &source;
    return bp_archive_add_entry_ex(ar, name, BP_TYPE_META, BP_META_PORTABLE,
                                  &io, bp_u64_from_u32(BP_META_FAT_SIZE),
                                  bp_u64_from_u32(0), 0);
}
#endif

static int add_file_to_archive(bp_archive_t *ar, const char *filepath,
                               const char *arc_name)
{
    struct _stat64 st;
#ifdef _WIN32
    WinSourcePathGuard path_guard;
    DWORD attributes;
    /* Hold each source object without FILE_SHARE_DELETE while it is used.
       Filesystems honoring Win32 sharing then block rename; identity checks
       also catch host mappings that permit a pathname replacement anyway. */
    if (!path_guard.open(filepath))
        return BP_ERR_IO;
    attributes = path_guard.info.dwFileAttributes;
#ifdef BP_TEST_WIN_FLTK_DIR_GUARD_PAUSE
    win_test_pause_after_directory_guard(attributes);
#endif
#endif
    if (_stat64(filepath, &st) != 0) return BP_ERR_IO;

    if (S_ISDIR(st.st_mode)) {
#ifdef _WIN32
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            !path_guard.still_names(filepath)) return BP_ERR_IO;
#endif
        /* add directory entry */
        std::string dname = std::string(arc_name);
        if (dname.empty() || dname[dname.size()-1] != '/')
            dname += '/';
        bp_uint64 mtime;
        bp_uint16 flags = win_gui_archive_mtime(st.st_mtime, &mtime);
        int rc = bp_archive_add_entry_ex(ar, dname.c_str(), BP_TYPE_DIR,
                                         BP_OS_WINDOWS, NULL,
                                         bp_u64_from_u32(0), mtime, flags);
        if (rc != BP_OK) return rc;
#ifdef _WIN32
        rc = add_win_fat_metadata(ar, dname.c_str(), BP_TYPE_DIR, attributes);
        if (rc != BP_OK) return rc;
#endif
        int child_rc = add_dir_recursive(ar, filepath, dname.c_str());
#ifdef _WIN32
        if (child_rc == BP_OK && !path_guard.still_names(filepath))
            return BP_ERR_IO;
#endif
        return child_rc;
    }

#ifdef _WIN32
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        !path_guard.still_names(filepath)) return BP_ERR_IO;
    FILE *fp = open_windows_source_file(filepath, &st, &path_guard.info);
#else
    FILE *fp = fopen(filepath, "rb");
#endif
    if (!fp) return BP_ERR_IO;

    bp_io_t fio;
    bp_io_from_file(&fio, fp);

    bp_uint64 fsize = bp_off_to_u64((bp_off_t)st.st_size);
    bp_uint64 mtime;
    bp_uint16 flags = win_gui_archive_mtime(st.st_mtime, &mtime);
    int rc = bp_archive_add_entry_ex(ar, arc_name, BP_TYPE_DATA,
                                     BP_OS_WINDOWS, &fio, fsize, mtime, flags);
    if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (rc != BP_OK) return rc;
#ifdef _WIN32
    if (!path_guard.still_names(filepath)) return BP_ERR_IO;
    return add_win_fat_metadata(ar, arc_name, BP_TYPE_DATA, attributes);
#else
    return BP_OK;
#endif
}

/* Directory names stay UTF-8 in the GUI. MinGW's dirent implementation uses
   the process ANSI codepage, so use wide Win32 enumeration on Windows. */
static int list_directory_names(const char *dirpath,
                                std::vector<std::string> &names)
{
#ifdef _WIN32
    std::wstring wide_path;
    WIN32_FIND_DATAW data;
    HANDLE find_handle;
    DWORD error;
    int rc = BP_OK;

    if (!win_utf8_to_wide(dirpath, &wide_path)) return BP_ERR_NAME_TOO_LONG;
    while (wide_path.size() > 3 &&
           (wide_path[wide_path.size() - 1] == L'\\' ||
            wide_path[wide_path.size() - 1] == L'/'))
        wide_path.erase(wide_path.size() - 1);
    wide_path += L"\\*";
    find_handle = FindFirstFileW(wide_path.c_str(), &data);
    if (find_handle == INVALID_HANDLE_VALUE) return BP_ERR_IO;
    do {
        std::string utf8_name;
        if (wcscmp(data.cFileName, L".") == 0 ||
            wcscmp(data.cFileName, L"..") == 0)
            continue;
        /* Refuse the whole source tree instead of traversing outside it via
           a junction/symlink or recursing through a reparse-point cycle. */
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            rc = BP_ERR_IO;
            break;
        }
        if (!win_wide_to_utf8(data.cFileName, &utf8_name)) {
            rc = BP_ERR_NAME_TOO_LONG;
            break;
        }
        names.push_back(utf8_name);
    } while (FindNextFileW(find_handle, &data));
    error = GetLastError();
    FindClose(find_handle);
    if (rc == BP_OK && error != ERROR_NO_MORE_FILES) rc = BP_ERR_IO;
    return rc;
#else
    DIR *directory = opendir(dirpath);
    struct dirent *entry;
    int rc = BP_OK;

    if (directory == NULL) return BP_ERR_IO;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        names.push_back(entry->d_name);
    }
    if (errno != 0) rc = BP_ERR_IO;
    if (closedir(directory) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc;
#endif
}

static int add_dir_recursive(bp_archive_t *ar, const char *dirpath,
                             const char *prefix)
{
    std::vector<std::string> names;
    int rc = list_directory_names(dirpath, names);
    if (rc != BP_OK) return rc;

    for (size_t i = 0; i < names.size(); ++i) {
        std::string full = std::string(dirpath) + "/" + names[i];
        std::string arc  = std::string(prefix) + names[i];

        rc = add_file_to_archive(ar, full.c_str(), arc.c_str());
        if (rc != BP_OK) return rc;
    }
    return BP_OK;
}

/* recursively sum input bytes and count archive entries for progress */
static void count_input_recursive(const char *path, double &bytes, unsigned long &count)
{
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        count++;
        std::vector<std::string> names;
        if (list_directory_names(path, names) != BP_OK) return;
        for (size_t i = 0; i < names.size(); ++i) {
            std::string full = std::string(path) + "/" + names[i];
            count_input_recursive(full.c_str(), bytes, count);
        }
    } else {
        bytes += (double)st.st_size;
        count++;
    }
}

static void init_create_progress(ProgressCtx *ctx)
{
    std::lock_guard<std::mutex> lk(ctx->mtx);
    snprintf(ctx->file_buf, sizeof(ctx->file_buf), "Preparing archive...");
    snprintf(ctx->count_buf, sizeof(ctx->count_buf), "Entry 0 of %lu",
             ctx->total_files);
    ctx->pct = 0.0f;
}

/* worker data for threaded archive creation */
struct CreateWork {
    std::vector<std::string> input_files;
    std::string              out_path;
    FILE                    *output_fp;
    ProgressCtx             *pctx;
    int                      method;
    int                      level;
    bool                     use_interchange_profile;
    bool                     use_fast_fluff;
    int                      result;
};

static void create_worker(CreateWork *w);

static bool choose_create_inputs(std::vector<std::string> &input_files)
{
    int choice = fl_choice("What would you like to add to the new archive?",
                           "Cancel", "Files", "Folder");
    if (choice == 1) {
        Fl_Native_File_Chooser input_chooser;
        input_chooser.title("Select files to archive");
        input_chooser.type(Fl_Native_File_Chooser::BROWSE_MULTI_FILE);
        if (input_chooser.show() != 0)
            return false;

        int count = input_chooser.count();
        if (count <= 0)
            return false;

        for (int i = 0; i < count; i++)
            input_files.push_back(input_chooser.filename(i));
        return true;
    }

    if (choice == 2) {
        Fl_Native_File_Chooser input_chooser;
        input_chooser.title("Select folder to archive");
        input_chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
        if (input_chooser.show() != 0)
            return false;

        const char *folder = input_chooser.filename();
        if (!folder || !*folder)
            return false;

        input_files.push_back(folder);
        return true;
    }

    return false;
}

static std::string suggested_archive_name(const std::vector<std::string> &input_files)
{
    std::string suggested = "archive.blt";

    if (!input_files.empty()) {
        const char *first = input_files[0].c_str();
        const char *base = first;

        for (const char *p = first; *p; p++) {
            if (*p == '/' || *p == '\\')
                base = p + 1;
        }

        std::string basename(base);
        if (!basename.empty() &&
            (basename[basename.size() - 1] == '/' || basename[basename.size() - 1] == '\\'))
            basename.erase(basename.size() - 1);

        {
            size_t dot = basename.rfind('.');
            if (dot != std::string::npos && dot > 0)
                basename = basename.substr(0, dot);
        }

        if (!basename.empty())
            suggested = basename + ".blt";
    }

    return suggested;
}

struct WindowsTargetSnapshot {
    bool exists;
    BY_HANDLE_FILE_INFORMATION info;
};

static bool read_windows_target_snapshot(const std::string &path,
                                         WindowsTargetSnapshot *snapshot)
{
    DWORD attributes = GetFileAttributesA(path.c_str());
    HANDLE handle;
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            snapshot->exists = false;
            memset(&snapshot->info, 0, sizeof(snapshot->info));
            return true;
        }
        return false;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    handle = CreateFileA(path.c_str(), FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) return false;
    if (!GetFileInformationByHandle(handle, &snapshot->info)) {
        CloseHandle(handle);
        return false;
    }
    CloseHandle(handle);
    if ((snapshot->info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    snapshot->exists = true;
    return true;
}

static bool same_windows_target(const BY_HANDLE_FILE_INFORMATION *a,
                                const BY_HANDLE_FILE_INFORMATION *b)
{
    return a->dwVolumeSerialNumber == b->dwVolumeSerialNumber &&
           a->nFileIndexHigh == b->nFileIndexHigh &&
           a->nFileIndexLow == b->nFileIndexLow &&
           a->nFileSizeHigh == b->nFileSizeHigh &&
           a->nFileSizeLow == b->nFileSizeLow &&
           a->ftLastWriteTime.dwHighDateTime == b->ftLastWriteTime.dwHighDateTime &&
           a->ftLastWriteTime.dwLowDateTime == b->ftLastWriteTime.dwLowDateTime &&
           a->dwFileAttributes == b->dwFileAttributes;
}

static volatile LONG g_create_temp_counter = 0;

static int open_windows_archive_stage(const std::string &target,
                                      std::string *temporary, FILE **file)
{
    std::string directory = target;
    size_t slash = directory.find_last_of("\\/");
    int attempt;
    if (slash == std::string::npos)
        directory = ".\\";
    else
        directory.resize(slash + 1);

    for (attempt = 0; attempt < 256; ++attempt) {
        char suffix[64];
        LONG serial = InterlockedIncrement(&g_create_temp_counter);
        HANDLE handle;
        int fd;
        FILE *fp;
        snprintf(suffix, sizeof(suffix), ".boltpack-create-%08lx.tmp",
                 (unsigned long)(GetTickCount() ^ GetCurrentProcessId() ^ serial));
        *temporary = directory + suffix;
        handle = CreateFileA(temporary->c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                             NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            return BP_ERR_IO;
        }
        fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR);
        if (fd < 0) {
            CloseHandle(handle);
            DeleteFileA(temporary->c_str());
            return BP_ERR_IO;
        }
        fp = _fdopen(fd, "w+b");
        if (fp == NULL) {
            _close(fd);
            DeleteFileA(temporary->c_str());
            return BP_ERR_IO;
        }
        *file = fp;
        return BP_OK;
    }
    return BP_ERR_IO;
}

static bool publish_windows_archive_stage(const std::string &temporary,
                                         const std::string &target,
                                         const WindowsTargetSnapshot *expected,
                                         std::string *backup,
                                         int *result)
{
    WindowsTargetSnapshot current;
    *result = 0;
    if (!read_windows_target_snapshot(target, &current)) return false;
    if (expected->exists) {
        if (!current.exists || !same_windows_target(&current.info, &expected->info)) {
            SetLastError(ERROR_FILE_INVALID);
            return false;
        }
        *backup = temporary + ".previous";
        if (GetFileAttributesA(backup->c_str()) != INVALID_FILE_ATTRIBUTES) {
            SetLastError(ERROR_FILE_EXISTS);
            return false;
        }
        if (!ReplaceFileA(target.c_str(), temporary.c_str(), backup->c_str(),
                          0, NULL, NULL)) {
            DWORD error = GetLastError();
            if (GetFileAttributesA(backup->c_str()) != INVALID_FILE_ATTRIBUTES) {
                *result = 2; /* retain stage and backup for recovery */
                SetLastError(error);
            }
            return false;
        }
        if (!DeleteFileA(backup->c_str()))
            *result = 1; /* archive is published; old archive remains as backup */
        return true;
    }
    if (current.exists) {
        SetLastError(ERROR_FILE_EXISTS);
        return false;
    }
    return MoveFileExA(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
}

static bool choose_output_archive_path(const std::vector<std::string> &input_files,
                                       std::string &out_path,
                                       WindowsTargetSnapshot *target_snapshot)
{
    Fl_Native_File_Chooser save_chooser;
    std::string preset = suggested_archive_name(input_files);

    /* Derive the parent directory from the first input so the Save dialog
     * opens in the same location as what the user dragged in.
     * E.g. C:\Temp\SomeFolder  =>  directory C:\Temp\, name SomeFolder.blt */
    if (!input_files.empty()) {
        std::string first = input_files[0];
        /* strip trailing path separators (folder drops may include one) */
        while (!first.empty() &&
               (first[first.size() - 1] == '/' || first[first.size() - 1] == '\\'))
            first.erase(first.size() - 1);
        size_t sep = first.find_last_of("/\\");
        if (sep != std::string::npos)
            preset = first.substr(0, sep + 1) + preset;
    }

    save_chooser.title("Save archive as");
    save_chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    save_chooser.filter("BoltPack Archives\t*.{bolt,blt}");
    save_chooser.preset_file(preset.c_str());
    if (save_chooser.show() != 0)
        return false;

    out_path = save_chooser.filename();
    if (out_path.empty())
        return false;

    if (out_path.size() < 5 ||
        (out_path.substr(out_path.size() - 5) != ".bolt" &&
         out_path.substr(out_path.size() - 4) != ".blt"))
        out_path += ".bolt";

    if (!read_windows_target_snapshot(out_path, target_snapshot)) {
        fl_alert("The selected destination cannot be inspected or is not a regular file.");
        return false;
    }
    if (target_snapshot->exists) {
        int choice = fl_choice("File already exists:\n%s\n\nOverwrite?",
                               "Cancel", "Overwrite", NULL, out_path.c_str());
        if (choice != 1)
            return false;
    }

    return true;
}

static void do_create_with_inputs(const std::vector<std::string> &input_files,
                                  int method, int level)
{
    ProgressCtx *pctx;
    CreateWork work;
    std::string out_path;
    std::string temporary;
    FILE *stage_fp = NULL;
    WindowsTargetSnapshot target_snapshot;

    if (input_files.empty())
        return;

    if (!choose_output_archive_path(input_files, out_path, &target_snapshot))
        return;
    if (open_windows_archive_stage(out_path, &temporary, &stage_fp) != BP_OK) {
        fl_alert("Could not create a staging file beside the selected destination.");
        return;
    }

    pctx = create_progress_window("Creating archive...");

    {
        double bytes = 0.0;
        unsigned long files = 0;
        for (size_t i = 0; i < input_files.size(); i++)
            count_input_recursive(input_files[i].c_str(), bytes, files);
        pctx->total_bytes = bytes;
        pctx->total_files = files;
    }
    init_create_progress(pctx);

    work.input_files = input_files;
    work.out_path    = temporary;
    work.output_fp   = stage_fp;
    work.pctx        = pctx;
    work.method      = method;
    work.level       = level;
    work.use_interchange_profile = (g_interchange_profile != 0);
    work.use_fast_fluff = (g_fluff_multithread != 0);
    work.result      = BP_OK;

    std::thread worker(create_worker, &work);

    while (!pctx->finished.load()) {
        Fl::wait(0.05);
    }

    worker.join();

    {
        bool was_cancelled = pctx->cancelled.load();
        int rc = work.result;

        destroy_progress_window(pctx);

        if (was_cancelled || rc == BP_ERR_ABORTED) {
            DeleteFileA(temporary.c_str());
            fl_message("Archive creation cancelled.");
        } else if (rc != BP_OK) {
            DeleteFileA(temporary.c_str());
            fl_alert("Error creating archive:\n%s", bp_strerror(rc));
        } else {
            std::string backup;
            int publish_state = 0;
            if (publish_windows_archive_stage(temporary, out_path,
                                              &target_snapshot, &backup,
                                              &publish_state)) {
                do_open_archive(out_path.c_str());
                if (publish_state == 1)
                    fl_message("Archive saved. The previous archive remains at:\n%s",
                               backup.c_str());
            } else {
                DWORD error = GetLastError();
                if (publish_state == 2) {
                    fl_alert("Replacement did not finish cleanly (Windows error %lu). "
                             "Keep both recovery files:\n%s\n%s",
                             (unsigned long)error, temporary.c_str(), backup.c_str());
                } else {
                    DeleteFileA(temporary.c_str());
                    fl_alert("Could not publish the completed archive (Windows error %lu).",
                             (unsigned long)error);
                }
            }
        }
    }
}

static void create_worker(CreateWork *w)
{
    FILE *fp = w->output_fp;
    w->output_fp = NULL;
    if (!fp) fp = fopen(w->out_path.c_str(), "w+b");
    if (!fp) { w->result = BP_ERR_IO; w->pctx->finished.store(true); return; }

    bp_io_t io;
    bp_io_from_file(&io, fp);

    bp_archive_t ar;
    memset(&ar, 0, sizeof(ar));

    int method = w->use_interchange_profile ? BP_METHOD_BOLTLITE : w->method;
    int rc;
    if (w->use_interchange_profile)
        rc = bp_archive_create_profile(&ar, &io, BP_PROFILE_INTERCHANGE_1);
    else
        rc = bp_archive_create(&ar, &io, level_to_block_size(method, w->level),
                               (bp_uint8)method);

    /* Set progress callback and level AFTER bp_archive_create — it memsets the struct */
    if (!w->use_interchange_profile && method == BP_METHOD_BOLTLZ)
        ar.method_level = (bp_uint8)(w->level == 0 ? 1 : 2);
    else if (!w->use_interchange_profile && method == BP_METHOD_FLUFF)
        ar.method_level = (bp_uint8)fluff_gui_level(w->level);
#ifdef BOLTPACK_ZSTD
    else if (!w->use_interchange_profile && method == BP_METHOD_ZSTD)
        ar.method_level = (bp_uint8)zstd_gui_level(w->level);
#endif
    ar.progress_fn   = progress_cb;
    ar.progress_data  = w->pctx;

    if (rc == BP_OK) {
#ifdef BOLTPACK_FAST_FLUFF
        if (method == BP_METHOD_FLUFF && w->use_fast_fluff) {
            int nthreads = fast_fluff_thread_count();
            if (nthreads > 1)
                bp_set_fast_fluff(&ar, nthreads);
        } else if (method == BP_METHOD_BOLTLZ) {
            int nthreads = fast_fluff_thread_count();
            if (nthreads > 1)
                bp_set_fast_boltlz(&ar, nthreads);
        }
#endif
        for (size_t i = 0; i < w->input_files.size() && !w->pctx->cancelled.load(); i++) {
            const char *path = w->input_files[i].c_str();
            /* find basename: last '/' or '\' */
            const char *base = path;
            for (const char *p = path; *p; p++) {
                if (*p == '/' || *p == '\\') base = p + 1;
            }
            rc = add_file_to_archive(&ar, path, base);
            if (rc != BP_OK) break;
        }
        if (rc == BP_OK && !w->pctx->cancelled.load())
            rc = bp_archive_finalize(&ar);
    }
    bp_archive_close(&ar);
    if (rc == BP_OK && fflush(fp) != 0)
        rc = BP_ERR_IO;
    if (rc == BP_OK && _commit(_fileno(fp)) != 0)
        rc = BP_ERR_IO;
    if (fclose(fp) != 0 && rc == BP_OK)
        rc = BP_ERR_IO;
    w->result = rc;
    w->pctx->finished.store(true);
}

#ifdef BOLTPACK_NET
/* ---- network helpers --------------------------------------------------- */

static void gui_local_name(char *buf, size_t n)
{
    if (n == 0) return;
#ifdef _WIN32
    DWORD sz = (DWORD)n;
    if (!GetComputerNameA(buf, &sz))
        snprintf(buf, n, "boltpack");
#else
    if (gethostname(buf, n) != 0)
        snprintf(buf, n, "boltpack");
#endif
    buf[n - 1] = '\0';
}

/* ---- send a freshly created archive over the network ------------------- */

struct SendWork {
    std::string  target;
    std::string  path;
    std::string  name;
    ProgressCtx *pctx;
    int          result;
    std::mutex   emtx;
    char         errmsg[256];
};

static void send_net_progress(const char *label, double done, double total,
                              int finished, void *ud)
{
    SendWork *w = (SendWork *)ud;
    std::lock_guard<std::mutex> lk(w->pctx->mtx);
    (void)label;
    if (total > 0.0) {
        float pct = (float)(done / total * 100.0);
        if (pct > 100.0f) pct = 100.0f;
        w->pctx->pct = pct;
    }
    snprintf(w->pctx->file_buf, sizeof(w->pctx->file_buf),
             "Sending to %s", w->target.c_str());
    if (finished)
        snprintf(w->pctx->count_buf, sizeof(w->pctx->count_buf), "Complete");
    else
        snprintf(w->pctx->count_buf, sizeof(w->pctx->count_buf),
                 "%.1f MB of %.1f MB", done / 1048576.0, total / 1048576.0);
}

static void send_net_message(int is_error, const char *text, void *ud)
{
    SendWork *w = (SendWork *)ud;
    if (is_error) {
        std::lock_guard<std::mutex> lk(w->emtx);
        snprintf(w->errmsg, sizeof(w->errmsg), "%s", text);
    }
}

static void send_worker(SendWork *w)
{
    bp_net_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui.progress = send_net_progress;
    ui.message  = send_net_message;
    ui.ud       = w;
    w->result = bp_net_send(w->target.c_str(), w->path.c_str(),
                            w->name.c_str(), &ui);
    w->pctx->finished.store(true);
}

static void do_send_archive(const std::string &path, const std::string &target)
{
    SendWork work;
    char host[64];

    work.target = target;
    work.path   = path;
    gui_local_name(host, sizeof(host));
    work.name = host;
    work.result = 0;
    work.errmsg[0] = '\0';
    work.pctx = create_progress_window("Sending over network...");
    work.pctx->cancel_btn->hide();   /* send is not cancellable mid-transfer */

    std::thread worker(send_worker, &work);
    while (!work.pctx->finished.load())
        Fl::wait(0.05);
    worker.join();

    int rc = work.result;
    std::string err;
    { std::lock_guard<std::mutex> lk(work.emtx); err = work.errmsg; }
    destroy_progress_window(work.pctx);

    if (rc != 0)
        fl_alert("Network send failed:\n%s",
                 err.empty() ? "unknown error" : err.c_str());
    else
        fl_message("Sent to %s.", target.c_str());
}

/* ---- listener picker dialog (scan + choose a receiver) ----------------- */

struct PickerState {
    Fl_Window       *win;
    Fl_Hold_Browser *browser;
    Fl_Input        *manual;
    Fl_Button       *rescan_btn;
    std::vector<bp_net_peer_t> peers;
    int              action;     /* 0 = cancel, 1 = send */
};

static void picker_scan(PickerState *s)
{
    bp_net_peer_t found[32];
    int count = 0;
    std::atomic<bool> done(false);
    std::thread th([&] { count = bp_net_scan(found, 32, 2); done.store(true); });

    s->rescan_btn->label("Scanning...");
    s->rescan_btn->deactivate();
    s->rescan_btn->redraw();
    while (!done.load())
        Fl::wait(0.05);
    th.join();
    s->rescan_btn->label("Rescan");
    s->rescan_btn->activate();
    s->rescan_btn->redraw();

    s->peers.clear();
    s->browser->clear();
    for (int i = 0; i < count; i++) {
        char line[160];
        s->peers.push_back(found[i]);
        snprintf(line, sizeof(line), "%.40s  \xe2\x80\x94  %.46s  (%s)",
                 found[i].name, found[i].ip,
                 found[i].auto_accept ? "auto-accept" : "prompt");
        s->browser->add(line);
    }
    if (count > 0)
        s->browser->select(1);
    else
        s->browser->add("(no receivers found - click Rescan or type an address)");
}

static void picker_rescan_cb(Fl_Widget *, void *data)
{
    picker_scan((PickerState *)data);
}

static void picker_send_cb(Fl_Widget *, void *data)
{
    PickerState *s = (PickerState *)data;
    s->action = 1;
    s->win->hide();
}

static void picker_cancel_cb(Fl_Widget *, void *data)
{
    PickerState *s = (PickerState *)data;
    s->action = 0;
    s->win->hide();
}

static void picker_browser_cb(Fl_Widget *w, void *data)
{
    /* double-click a row = choose it and send */
    if (Fl::event_clicks()) {
        PickerState *s = (PickerState *)data;
        if (((Fl_Hold_Browser *)w)->value() >= 1) {
            s->action = 1;
            s->win->hide();
        }
    }
}

/* Show the picker; on return true, out_target holds the chosen IP/name. */
static bool show_listener_picker(std::string &out_target)
{
    int W = 440, H = 320;
    int cx = g_main_window ? g_main_window->x() + (g_main_window->w() - W) / 2 : 220;
    int cy = g_main_window ? g_main_window->y() + (g_main_window->h() - H) / 2 : 220;
    PickerState s;
    s.action = 0;

    s.win = new Fl_Window(cx, cy, W, H, "Send over Network");
    s.win->set_modal();

    Fl_Box *hdr = new Fl_Box(10, 10, W - 20, 20, "Receivers listening on the network:");
    hdr->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    hdr->labelfont(FL_HELVETICA_BOLD);

    s.browser = new Fl_Hold_Browser(10, 34, W - 20, 170);
    s.browser->callback(picker_browser_cb, &s);

    s.rescan_btn = new Fl_Button(10, 212, 90, 26, "Rescan");
    s.rescan_btn->callback(picker_rescan_cb, &s);

    s.manual = new Fl_Input(120, 212, W - 130, 26, "");
    s.manual->align(FL_ALIGN_LEFT);
    s.manual->tooltip("Optional: type a receiver name or IP address directly");
    if (!g_send_last_target.empty())
        s.manual->value(g_send_last_target.c_str());

    Fl_Box *hint = new Fl_Box(120, 240, W - 130, 18,
                              "or type a name / IP address");
    hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    hint->labelsize(11);
    hint->labelcolor(FL_DARK2);

    Fl_Button *send_btn = new Fl_Button(W / 2 - 90, H - 40, 90, 30, "Send");
    Fl_Button *cn_btn   = new Fl_Button(W / 2 + 10, H - 40, 80, 30, "Cancel");
    send_btn->callback(picker_send_cb, &s);
    cn_btn->callback(picker_cancel_cb, &s);

    s.win->end();
    s.win->show();
    picker_scan(&s);            /* auto-scan once on open */
    while (s.win->shown())
        Fl::wait();

    /* resolve choice before deleting widgets */
    std::string chosen;
    if (s.action == 1) {
        const char *m = s.manual->value();
        if (m && *m) {
            chosen = m;                 /* manual entry wins if provided */
        } else {
            int row = s.browser->value();
            if (row >= 1 && row <= (int)s.peers.size())
                chosen = s.peers[row - 1].ip;
        }
    }
    delete s.win;

    if (s.action == 1 && !chosen.empty()) {
        g_send_last_target = chosen;
        out_target = chosen;
        return true;
    }
    return false;
}

/* Create the archive to a temp folder and stream it to a chosen receiver. */
static void create_and_send(const std::vector<std::string> &input_files)
{
    std::string target;
    if (!show_listener_picker(target))
        return;

    /* sensible archive name from the first input (so the receiver sees it) */
    std::string name;
    {
        std::string b = leaf_name(input_files[0]);
        size_t dot = b.rfind('.');
        if (dot != std::string::npos && dot > 0) b = b.substr(0, dot);
        name = b + ".bolt";
    }

    /* unique temp folder so the receiver sees a sensible archive name */
    std::string tmpdir;
#ifdef _WIN32
    {
        WCHAR tp[MAX_PATH], td[MAX_PATH];
        std::string temp_file;
        DWORD n = GetTempPathW(MAX_PATH, tp);
        if (n != 0 && n < MAX_PATH &&
            GetTempFileNameW(tp, L"blt", 0, td) != 0 &&
            win_wide_to_utf8(td, &temp_file)) {
            DeleteFileA(temp_file.c_str()); /* drop the file GetTempFileName made */
            if (CreateDirectoryA(temp_file.c_str(), NULL))
                tmpdir = temp_file;
        }
    }
    std::string out_path = tmpdir.empty() ? std::string() : tmpdir + "\\" + name;
#else
    {
        char dirtmpl[] = "/tmp/boltpack-XXXXXX";
        char *d = mkdtemp(dirtmpl);
        if (d) tmpdir = d;
    }
    std::string out_path = tmpdir.empty() ? std::string() : tmpdir + "/" + name;
#endif
    if (tmpdir.empty()) {
        fl_alert("Could not create a temporary folder for sending.");
        return;
    }

    ProgressCtx *pctx = create_progress_window("Creating archive...");
    {
        double bytes = 0.0;
        unsigned long files = 0;
        for (size_t i = 0; i < input_files.size(); i++)
            count_input_recursive(input_files[i].c_str(), bytes, files);
        pctx->total_bytes = bytes;
        pctx->total_files = files;
    }
    init_create_progress(pctx);

    CreateWork work;
    work.input_files = input_files;
    work.out_path    = out_path;
    work.output_fp   = NULL;
    work.pctx        = pctx;
    work.method      = g_compression_method;
    work.level       = g_compression_level;
    work.use_interchange_profile = (g_interchange_profile != 0);
    work.use_fast_fluff = (g_fluff_multithread != 0);
    work.result      = BP_OK;

    std::thread worker(create_worker, &work);
    while (!pctx->finished.load())
        Fl::wait(0.05);
    worker.join();

    bool was_cancelled = pctx->cancelled.load();
    int rc = work.result;
    destroy_progress_window(pctx);

    if (was_cancelled || rc == BP_ERR_ABORTED)
        fl_message("Archive creation cancelled.");
    else if (rc != BP_OK)
        fl_alert("Error creating archive:\n%s", bp_strerror(rc));
    else
        do_send_archive(out_path, target);

    unlink(out_path.c_str());
#ifdef _WIN32
    RemoveDirectoryA(tmpdir.c_str());
#else
    rmdir(tmpdir.c_str());
#endif
}

/* ---- network: Receive (Listen) window ---------------------------------- */

struct ListenState {
    std::thread             worker;
    volatile int            stop;          /* polled by bp_net_listen */
    std::atomic<bool>       running;
    std::atomic<bool>       worker_done;
    int                     rc;

    std::mutex              mtx;
    /* progress (under mtx) */
    bool                    prog_active;
    bool                    prog_finished;
    double                  prog_done;
    double                  prog_total;
    char                    prog_label[32];
    /* last status message (under mtx) */
    char                    status_msg[512];
    bool                    status_is_error;
    bool                    status_dirty;
    /* accept-decision marshalling (under mtx) */
    bool                    decide_pending;
    bool                    decide_ready;
    int                     decide_answer;
    char                    decide_text[512];
    std::condition_variable decide_cv;
    /* config snapshot taken at Start */
    bool                    auto_accept;
    bool                    auto_verify;
    bool                    auto_extract;
    std::string             name;
    std::string             out_dir;

    ListenState()
        : stop(0), rc(0), prog_active(false), prog_finished(false),
          prog_done(0.0), prog_total(0.0), status_is_error(false),
          status_dirty(false), decide_pending(false), decide_ready(false),
          decide_answer(0), auto_accept(true), auto_verify(false),
          auto_extract(false)
    {
        running.store(false);
        worker_done.store(false);
        prog_label[0] = '\0';
        status_msg[0] = '\0';
        decide_text[0] = '\0';
    }
};

struct ListenWin {
    Fl_Double_Window *win;
    Fl_Input         *name_in;
    Fl_Input         *folder_in;
    Fl_Button        *browse_btn;
    Fl_Check_Button  *auto_btn;
    Fl_Check_Button  *extract_btn;
    Fl_Check_Button  *verify_btn;
    Fl_Button        *start_btn;
    Fl_Box           *status_box;
    OutlinedProgress *bar;
    ListenState      *st;
};

static ListenWin *g_listen = NULL;

static void listen_hsize(double b, char *buf, size_t n)
{
    if (b < 1024.0)            snprintf(buf, n, "%.0f B", b);
    else if (b < 1048576.0)    snprintf(buf, n, "%.1f KB", b / 1024.0);
    else if (b < 1073741824.0) snprintf(buf, n, "%.1f MB", b / 1048576.0);
    else                       snprintf(buf, n, "%.2f GB", b / 1073741824.0);
}

/* ---- worker-thread callbacks (must NOT touch FLTK) --------------------- */

static int listen_decide(const char *peer_name, const char *peer_ip,
                         const char *fname, double size_bytes, void *ud)
{
    ListenState *st = (ListenState *)ud;
    char sb[32];

    if (st->auto_accept)
        return 1;

    listen_hsize(size_bytes, sb, sizeof(sb));
    {
        std::unique_lock<std::mutex> lk(st->mtx);
        snprintf(st->decide_text, sizeof(st->decide_text),
                 "Accept '%s' (%s)\nfrom %s [%s]?",
                 fname, sb, peer_name, peer_ip);
        st->decide_ready = false;
        st->decide_pending = true;
        st->decide_cv.wait(lk, [st] { return st->decide_ready || st->stop; });
        st->decide_pending = false;
        if (st->stop && !st->decide_ready)
            return 0;
        return st->decide_answer;
    }
}

static void listen_progress(const char *label, double done, double total,
                            int finished, void *ud)
{
    ListenState *st = (ListenState *)ud;
    std::lock_guard<std::mutex> lk(st->mtx);
    snprintf(st->prog_label, sizeof(st->prog_label), "%s", label);
    st->prog_done = done;
    st->prog_total = total;
    st->prog_finished = finished ? true : false;
    st->prog_active = finished ? false : true;
}

static void listen_message(int is_error, const char *text, void *ud)
{
    ListenState *st = (ListenState *)ud;
    std::lock_guard<std::mutex> lk(st->mtx);
    snprintf(st->status_msg, sizeof(st->status_msg), "%s", text);
    st->status_is_error = is_error ? true : false;
    st->status_dirty = true;
}

static void listen_worker(ListenState *st)
{
    bp_net_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui.decide     = listen_decide;
    ui.progress   = listen_progress;
    ui.message    = listen_message;
    ui.extract_io = gui_extract_io;
    ui.stop       = &st->stop;
    ui.ud         = st;
    st->rc = bp_net_listen(st->name.c_str(), st->auto_accept ? 1 : 0,
                           st->auto_extract ? 1 : 0, st->auto_verify ? 1 : 0,
                           st->out_dir.c_str(), 0, &ui);
    st->worker_done.store(true);
}

/* ---- main-thread UI helpers ------------------------------------------- */

static void listen_set_ui_running(ListenWin *lw, bool running)
{
    if (running) {
        lw->name_in->deactivate();
        lw->folder_in->deactivate();
        lw->browse_btn->deactivate();
        lw->auto_btn->deactivate();
        lw->extract_btn->deactivate();
        lw->verify_btn->deactivate();
        lw->start_btn->label("Stop");
    } else {
        lw->name_in->activate();
        lw->folder_in->activate();
        lw->browse_btn->activate();
        lw->auto_btn->activate();
        lw->extract_btn->activate();
        lw->verify_btn->activate();
        lw->start_btn->label("Start Listening");
        lw->start_btn->activate();
        lw->bar->value(0.0f);
        lw->bar->copy_label("");
    }
    lw->start_btn->redraw();
}

static void listen_timer_cb(void *data)
{
    ListenWin *lw = (ListenWin *)data;
    ListenState *st = lw->st;
    bool pending_decide = false;
    char decide_text[512];

    {
        std::lock_guard<std::mutex> lk(st->mtx);
        if (st->prog_total > 0.0) {
            float pct = (float)(st->prog_done / st->prog_total * 100.0);
            char pb[48];
            if (pct > 100.0f) pct = 100.0f;
            lw->bar->value(pct);
            snprintf(pb, sizeof(pb), "%s %.0f%%",
                     st->prog_label[0] ? st->prog_label : "", pct);
            lw->bar->copy_label(pb);
        }
        if (st->prog_finished) {
            lw->bar->value(100.0f);
            st->prog_finished = false;
        }
        if (st->status_dirty) {
            lw->status_box->copy_label(st->status_msg);
            lw->status_box->labelcolor(st->status_is_error
                                       ? FL_RED : FL_FOREGROUND_COLOR);
            lw->status_box->redraw();
            st->status_dirty = false;
        }
        if (st->decide_pending && !st->decide_ready) {
            pending_decide = true;
            snprintf(decide_text, sizeof(decide_text), "%s", st->decide_text);
        }
    }

    if (pending_decide) {
        int ans = fl_choice("%s", "Reject", "Accept", NULL, decide_text);
        std::lock_guard<std::mutex> lk(st->mtx);
        st->decide_answer = (ans == 1) ? 1 : 0;
        st->decide_ready = true;
        st->decide_cv.notify_all();
    }

    if (st->worker_done.load()) {
        if (st->worker.joinable())
            st->worker.join();
        st->running.store(false);
        listen_set_ui_running(lw, false);
        Fl::remove_timeout(listen_timer_cb, data);
        return;
    }
    Fl::repeat_timeout(0.1, listen_timer_cb, data);
}

static void listen_browse_cb(Fl_Widget *, void *data)
{
    ListenWin *lw = (ListenWin *)data;
    Fl_Native_File_Chooser chooser;
    chooser.title("Select folder to receive into");
    chooser.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
    if (lw->folder_in->value() && *lw->folder_in->value())
        chooser.directory(lw->folder_in->value());
    if (chooser.show() != 0) return;
    if (chooser.filename() && *chooser.filename())
        lw->folder_in->value(chooser.filename());
}

static void listen_start_cb(Fl_Widget *, void *data)
{
    ListenWin *lw = (ListenWin *)data;
    ListenState *st = lw->st;

    if (!st->running.load()) {
        /* ---- start ---- */
        st->stop = 0;
        st->worker_done.store(false);
        {
            std::lock_guard<std::mutex> lk(st->mtx);
            st->prog_active = false;
            st->prog_finished = false;
            st->prog_done = 0.0;
            st->prog_total = 0.0;
            st->prog_label[0] = '\0';
            st->status_dirty = false;
            st->decide_pending = false;
            st->decide_ready = false;
        }
        st->auto_accept  = lw->auto_btn->value() ? true : false;
        st->auto_verify  = lw->verify_btn->value() ? true : false;
        st->auto_extract = lw->extract_btn->value() ? true : false;
        st->name    = lw->name_in->value() ? lw->name_in->value() : "";
        st->out_dir = lw->folder_in->value() ? lw->folder_in->value() : "";

        st->running.store(true);
        listen_set_ui_running(lw, true);
        st->worker = std::thread(listen_worker, st);
        Fl::add_timeout(0.1, listen_timer_cb, lw);
    } else {
        /* ---- stop ---- */
        st->stop = 1;
        st->decide_cv.notify_all();   /* unblock a pending decision */
        lw->start_btn->deactivate();  /* timer re-enables once stopped */
        lw->start_btn->label("Stopping...");
        lw->start_btn->redraw();
    }
}

static void listen_win_close_cb(Fl_Widget *w, void *data)
{
    ListenWin *lw = (ListenWin *)data;
    ListenState *st = lw->st;

    if (st->running.load()) {
        st->stop = 1;
        st->decide_cv.notify_all();
        if (st->worker.joinable())
            st->worker.join();
        st->running.store(false);
        Fl::remove_timeout(listen_timer_cb, lw);
        listen_set_ui_running(lw, false);
    }
    ((Fl_Window *)w)->hide();
}

static void open_listen_window()
{
    if (g_listen) {
        g_listen->win->show();
        return;
    }

    ListenWin *lw = new ListenWin;
    lw->st = new ListenState;

    int W = 460, H = 312;
    int cx = g_main_window ? g_main_window->x() + (g_main_window->w() - W) / 2 : 220;
    int cy = g_main_window ? g_main_window->y() + (g_main_window->h() - H) / 2 : 220;

    lw->win = new Fl_Double_Window(cx, cy, W, H, "Receive over Network");
    lw->win->callback(listen_win_close_cb, lw);

    lw->name_in = new Fl_Input(90, 12, W - 100, 26, "Name:");
    {
        char host[64];
        gui_local_name(host, sizeof(host));
        lw->name_in->value(host);
    }

    lw->folder_in = new Fl_Input(90, 46, W - 100 - 84, 26, "Save to:");
    {
#ifdef _WIN32
        const char *home = getenv("USERPROFILE");
#else
        const char *home = getenv("HOME");
#endif
        lw->folder_in->value(home && *home ? home : ".");
    }
    lw->browse_btn = new Fl_Button(W - 84, 46, 74, 26, "Browse...");
    lw->browse_btn->callback(listen_browse_cb, lw);

    lw->auto_btn = new Fl_Check_Button(90, 82, W - 100, 22,
                                       "Auto-accept incoming transfers");
    lw->auto_btn->value(1);
    lw->extract_btn = new Fl_Check_Button(90, 106, W - 100, 22,
                                          "Auto-extract contents (otherwise save the .bolt file)");
    lw->verify_btn = new Fl_Check_Button(90, 130, W - 100, 22,
                                         "Auto-verify the saved archive");

    lw->status_box = new Fl_Box(10, 162, W - 20, 50, "Not listening.");
    lw->status_box->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP |
                          FL_ALIGN_WRAP);
    lw->status_box->box(FL_FLAT_BOX);
    lw->status_box->labelsize(12);

    lw->bar = new OutlinedProgress(10, 218, W - 20, 24);
    lw->bar->minimum(0.0f);
    lw->bar->maximum(100.0f);
    lw->bar->value(0.0f);
    lw->bar->color(fl_rgb_color(0xE0, 0xE0, 0xE0));
    lw->bar->selection_color(fl_rgb_color(0x19, 0x76, 0xD2));
    lw->bar->labelfont(FL_HELVETICA_BOLD);
    lw->bar->labelsize(12);
    lw->bar->copy_label("");

    lw->start_btn = new Fl_Button(W / 2 - 80, H - 44, 160, 32,
                                  "Start Listening");
    lw->start_btn->callback(listen_start_cb, lw);

    lw->win->end();
    g_listen = lw;
    lw->win->show();
}
#endif /* BOLTPACK_NET */

static void do_create()
{
    int action = show_compression_dialog();
    if (action == CREATE_CANCEL)
        return;

    std::vector<std::string> input_files;
    if (!choose_create_inputs(input_files))
        return;

#ifdef BOLTPACK_NET
    if (action == CREATE_SEND) {
        create_and_send(input_files);
        return;
    }
#endif
    do_create_with_inputs(input_files, g_compression_method, g_compression_level);
}

/* ---- classify and act (drag-and-drop) ---------------------------------- */

static bool has_bolt_extension(const char *path)
{
    size_t len = strlen(path);
    if (len >= 5 && strcasecmp(path + len - 5, ".bolt") == 0) return true;
    if (len >= 4 && strcasecmp(path + len - 4, ".blt") == 0) return true;
    return false;
}

static std::string url_decode(const std::string &s)
{
    std::string result;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = { s[i+1], s[i+2], 0 };
            char *end;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                result += (char)val;
                i += 2;
                continue;
            }
        }
        result += s[i];
    }
    return result;
}

static std::vector<std::string> parse_dnd_uris(const char *text)
{
    std::vector<std::string> paths;
    if (!text) return paths;

    std::string input(text);
    size_t pos = 0;
    while (pos < input.size()) {
        size_t nl = input.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? input.substr(pos) : input.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? input.size() : nl + 1;

        /* strip \r */
        if (!line.empty() && line[line.size()-1] == '\r')
            line.erase(line.size()-1);

        if (line.empty()) continue;

        /* strip file:// prefix (or file:/// on Windows) */
        if (line.compare(0, 8, "file:///") == 0)
            line = line.substr(7); /* keep leading / for absolute paths */
        else if (line.compare(0, 7, "file://") == 0)
            line = line.substr(7);

        line = url_decode(line);
        if (!line.empty())
            paths.push_back(line);
    }
    return paths;
}

void FileTable::handle_drop(const char *text)
{
    std::vector<std::string> paths = parse_dnd_uris(text);
    if (!paths.empty())
        classify_and_act(paths);
}

static void classify_and_act(const std::vector<std::string> &paths)
{
    std::vector<std::string> archives, others;

    for (size_t i = 0; i < paths.size(); i++) {
        if (has_bolt_extension(paths[i].c_str()))
            archives.push_back(paths[i]);
        else
            others.push_back(paths[i]);
    }

    if (archives.size() == 1 && others.empty()) {
        /* single archive: open it */
        do_open_archive(archives[0].c_str());
    } else if (!archives.empty() && others.empty()) {
        /* multiple archives: open first */
        do_open_archive(archives[0].c_str());
    } else if (archives.empty() && !others.empty()) {
        /* non-archive files: create archive from them (save or send) */
        int action = show_compression_dialog();
        if (action == CREATE_CANCEL)
            return;
#ifdef BOLTPACK_NET
        if (action == CREATE_SEND) {
            create_and_send(others);
            return;
        }
#endif
        do_create_with_inputs(others, g_compression_method, g_compression_level);
    } else {
        /* mixed: ask user */
        int choice = fl_choice("You dropped both archives and other files.\n"
                               "What would you like to do?",
                               "Cancel", "Open archive", "Create new archive");
        if (choice == 1 && !archives.empty())
            do_open_archive(archives[0].c_str());
        else if (choice == 2)
            classify_and_act(others);
    }
}

static void parse_shell_action(int argc, char **argv)
{
    int i;

    g_shell_action.type = SHELL_ACTION_NONE;
    g_shell_action.paths.clear();

    if (argc <= 1)
        return;

    if (strcmp(argv[1], "--shell-create-fluff") == 0) {
        g_shell_action.type = SHELL_ACTION_CREATE_FLUFF;
        for (i = 2; i < argc; i++) {
            if (argv[i] && argv[i][0])
                g_shell_action.paths.push_back(argv[i]);
        }
        return;
    }

    if (strcmp(argv[1], "--shell-create-boltlz") == 0) {
        g_shell_action.type = SHELL_ACTION_CREATE_BOLTLZ;
        for (i = 2; i < argc; i++) {
            if (argv[i] && argv[i][0])
                g_shell_action.paths.push_back(argv[i]);
        }
        return;
    }

    if (strcmp(argv[1], "--shell-extract") == 0) {
        if (argc > 2 && argv[2] && argv[2][0]) {
            g_shell_action.type = SHELL_ACTION_EXTRACT_ARCHIVE;
            g_shell_action.paths.push_back(argv[2]);
        }
        return;
    }

    g_shell_action.type = SHELL_ACTION_OPEN_ARCHIVE;
    g_shell_action.paths.push_back(argv[1]);
}

static void run_shell_action(void *)
{
    ShellAction action = g_shell_action;

    g_shell_action.type = SHELL_ACTION_NONE;
    g_shell_action.paths.clear();

    switch (action.type) {
    case SHELL_ACTION_OPEN_ARCHIVE:
        if (!action.paths.empty())
            do_open_archive(action.paths[0].c_str());
        break;
    case SHELL_ACTION_CREATE_BOLTLZ:
        do_create_with_inputs(action.paths, BP_METHOD_BOLTLZ, 1);
        break;
    case SHELL_ACTION_CREATE_FLUFF:
        do_create_with_inputs(action.paths, BP_METHOD_FLUFF, 2);
        break;
    case SHELL_ACTION_EXTRACT_ARCHIVE:
        if (!action.paths.empty()) {
            if (!has_bolt_extension(action.paths[0].c_str())) {
                fl_alert("Not a BoltPack archive:\n%s", action.paths[0].c_str());
                break;
            }
            do_open_archive(action.paths[0].c_str());
            if (g_archive_open)
                do_extract();
        }
        break;
    default:
        break;
    }
}

/* ---- menu callbacks ---------------------------------------------------- */

static void menu_open_cb(Fl_Widget *, void *)
{
    Fl_Native_File_Chooser chooser;
    chooser.title("Open BoltPack Archive");
    chooser.type(Fl_Native_File_Chooser::BROWSE_FILE);
    chooser.filter("BoltPack Archives\t*.{bolt,blt}");
    if (chooser.show() != 0) return;
    do_open_archive(chooser.filename());
}

static void menu_extract_all_cb(Fl_Widget *, void *)
{
    do_extract();
}

static void menu_extract_cb(Fl_Widget *, void *)
{
    do_extract_selected();
}

static void menu_close_cb(Fl_Widget *, void *)
{
    do_close_archive();
}

static void menu_validate_cb(Fl_Widget *, void *)
{
    do_validate();
}

static void menu_info_cb(Fl_Widget *, void *)
{
    do_info();
}

static void menu_create_cb(Fl_Widget *, void *)
{
    do_create();
}

#ifdef BOLTPACK_NET
static void menu_listen_cb(Fl_Widget *, void *)
{
    open_listen_window();
}
#endif

static void menu_exit_cb(Fl_Widget *, void *)
{
    close_main_window();
}

static void menu_reset_gui_settings_cb(Fl_Widget *, void *)
{
    int choice = fl_choice("Reset GUI settings to defaults?\n"
                           "This restores the default compression mode,\n"
                           "compression profile, Fluff threading,\n"
                           "window size and position,\n"
                           "and file-list columns.",
                           "Cancel", "Reset", NULL);
    if (choice != 1) return;

    g_compression_method = BP_METHOD_BOLTLZ;
    g_compression_level = 1;
    g_fluff_multithread = 0;
    g_interchange_profile = 0;

    g_have_persisted_col_widths = false;
    memset(g_persisted_col_widths, 0, sizeof(g_persisted_col_widths));
    if (g_table) g_table->reset_column_widths();

    g_have_window_geometry = false;
    g_persisted_window_x = 0;
    g_persisted_window_y = 0;
    g_persisted_window_w = MAIN_W;
    g_persisted_window_h = MAIN_H;
    if (g_main_window)
        g_main_window->resize(default_window_x(), default_window_y(), MAIN_W, MAIN_H);

    save_prefs();
    if (g_main_window) g_main_window->redraw();
}

static void menu_doc_cb(Fl_Widget *, void *)
{
    Fl_Window *doc_win = new Fl_Window(500, 400, "BoltPack Documentation");
    doc_win->set_non_modal();
    Fl_Text_Buffer *buf = new Fl_Text_Buffer();
    buf->text(bp_help_text);
    Fl_Text_Display *disp = new Fl_Text_Display(5, 5, 490, 390);
    disp->buffer(buf);
    disp->textfont(FL_COURIER);
    disp->textsize(13);
    doc_win->end();
    doc_win->show();
}

static void about_ok_cb(Fl_Widget *w, void *)
{
    w->window()->hide();
}

static void main_window_close_cb(Fl_Widget *, void *)
{
    close_main_window();
}

static void menu_about_cb(Fl_Widget *, void *)
{
    static Fl_PNG_Image *logo_img = NULL;
    static Fl_Image     *logo_sm  = NULL;
    if (!logo_img) {
        logo_img = new Fl_PNG_Image("logo", s_logo_png, (int)s_logo_png_len);
        logo_sm  = logo_img->copy(80, 80);
    }

    Fl_Window *dlg = new Fl_Window(380, 190, "About BoltPack");
    dlg->set_modal();

    Fl_Box *img_box = new Fl_Box(10, 10, 80, 80);
    img_box->image(logo_sm);

    static const char *about_text =
        BP_APP_NAME " " BP_APP_VERSION "\n\n"
        BP_DESCRIPTION "\n\n"
        BP_COPYRIGHT "\n"
        BP_LICENSE;
    Fl_Box *txt = new Fl_Box(102, 8, 268, 120, about_text);
    txt->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_TOP | FL_ALIGN_WRAP);
    txt->labelsize(12);

    Fl_Button *ok = new Fl_Button(145, 148, 90, 30, "OK");
    ok->callback(about_ok_cb);

    dlg->end();
    dlg->show();
    while (dlg->shown()) Fl::wait();
    Fl::delete_widget(dlg);
}

/* ---- update UI state --------------------------------------------------- */

static void update_selection_menu()
{
    if (!g_menu || !g_table) return;
    Fl_Menu_Item *items = (Fl_Menu_Item *)g_menu->menu();
    int idx = g_menu->find_index("Archive/Extract Selected");
    if (idx >= 0) {
        if (g_archive_open && g_table->selection_count() > 0)
            items[idx].activate();
        else
            items[idx].deactivate();
    }
}

static void update_ui()
{
    /* menu enable/disable */
    Fl_Menu_Item *items = (Fl_Menu_Item *)g_menu->menu();

    /* find Extract All, Extract Selected, and Close items */
    int idx_extract_all = g_menu->find_index("Archive/Extract All");
    int idx_extract     = g_menu->find_index("Archive/Extract Selected");
    int idx_close       = g_menu->find_index("Archive/Close");
    int idx_validate    = g_menu->find_index("Archive/Validate");
    int idx_info        = g_menu->find_index("Archive/Information");

    if (idx_extract_all >= 0) {
        if (g_archive_open)
            items[idx_extract_all].activate();
        else
            items[idx_extract_all].deactivate();
    }
    if (idx_extract >= 0) {
        /* active only when archive open and something selected */
        if (g_archive_open && g_table && g_table->selection_count() > 0)
            items[idx_extract].activate();
        else
            items[idx_extract].deactivate();
    }
    if (idx_close >= 0) {
        if (g_archive_open)
            items[idx_close].activate();
        else
            items[idx_close].deactivate();
    }
    if (idx_validate >= 0) {
        if (g_archive_open)
            items[idx_validate].activate();
        else
            items[idx_validate].deactivate();
    }
    if (idx_info >= 0) {
        if (g_archive_open)
            items[idx_info].activate();
        else
            items[idx_info].deactivate();
    }

    /* show/hide widgets */
    if (g_archive_open) {
        g_logo_box->hide();
        g_table->show();
        g_table->after_load();

        /* update status bar */
        char status_buf[512];
        double orig = u64_to_double(g_info.total_original);
        double comp = u64_to_double(g_info.total_compressed);
        int ratio = ratio_percent(orig, comp);
        snprintf(status_buf, sizeof(status_buf),
             "  %s  |  %lu files  |  %s -> %s  |  %d%% of original",
                 g_archive_path.c_str(),
                 (unsigned long)g_info.file_count,
                 format_size(orig).c_str(),
                 format_size(comp).c_str(),
                 ratio);
        g_status->copy_label(status_buf);
    } else {
        g_table->hide();
        g_logo_box->show();
        g_status->copy_label("  No archive open");
    }

    g_table->redraw();
    g_main_window->redraw();
}

/* ---- DnD handler on logo box ------------------------------------------- */

class DndBox : public Fl_Box {
public:
    DndBox(int X, int Y, int W, int H, const char *L = 0)
        : Fl_Box(X, Y, W, H, L) {}

    int handle(int event) {
        switch (event) {
        case FL_DND_ENTER:
        case FL_DND_DRAG:
        case FL_DND_RELEASE:
            return 1;
        case FL_PASTE: {
            std::vector<std::string> paths = parse_dnd_uris(Fl::event_text());
            if (!paths.empty())
                classify_and_act(paths);
            return 1;
        }
        default:
            return Fl_Box::handle(event);
        }
    }
};

/* ---- Windows entry point ----------------------------------------------- */

#ifdef _WIN32
/* On Windows with -mwindows we need WinMain; let FLTK handle it via main. */
int main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    int window_w;
    int window_h;

    parse_shell_action(argc, argv);

    load_prefs();
    bp_compress_init();

    window_w = g_have_window_geometry ? g_persisted_window_w : MAIN_W;
    window_h = g_have_window_geometry ? g_persisted_window_h : MAIN_H;

    Fl::background(0xf0, 0xf0, 0xf0);

    g_main_window = new Fl_Double_Window(window_w, window_h, "BoltPack");
    if (g_have_window_geometry)
        g_main_window->position(g_persisted_window_x, g_persisted_window_y);
    g_main_window->callback(main_window_close_cb);

    /* menu bar */
    g_menu = new Fl_Menu_Bar(0, 0, g_main_window->w(), MENU_H);
    g_menu->add("Archive/Open...",          FL_CTRL + 'o', menu_open_cb);
    g_menu->add("Archive/Extract All",       FL_CTRL + 'e', menu_extract_all_cb);
    g_menu->add("Archive/Extract Selected",   0,             menu_extract_cb);
    g_menu->add("Archive/Close",            FL_CTRL + 'w', menu_close_cb, 0, FL_MENU_DIVIDER);
    g_menu->add("Archive/Validate",         FL_CTRL + 't', menu_validate_cb);
    g_menu->add("Archive/Information",      FL_CTRL + 'i', menu_info_cb, 0, FL_MENU_DIVIDER);
    g_menu->add("Archive/Create New...",    FL_CTRL + 'n', menu_create_cb, 0, FL_MENU_DIVIDER);
    g_menu->add("Archive/Exit",             FL_CTRL + 'q', menu_exit_cb);
#ifdef BOLTPACK_NET
    g_menu->add("Network/Receive (Listen)...", FL_CTRL + 'r', menu_listen_cb);
#endif
    g_menu->add("Settings/Reset GUI Settings...", 0, menu_reset_gui_settings_cb);
    g_menu->add("Help/Documentation",       0, menu_doc_cb);
    g_menu->add("Help/About BoltPack",      0, menu_about_cb);

    /* logo box (shown when no archive is open) */
    g_logo_box = new DndBox(0, MENU_H, g_main_window->w(), g_main_window->h() - MENU_H - STATUS_H);
    g_logo_box->label(BP_APP_NAME " " BP_APP_VERSION "\n\nA lightweight file archiver\nfor vintage and modern systems\n\n"
                      "Drop .bolt files here to open\nDrop other files to create an archive");
    g_logo_box->labelsize(16);
    g_logo_box->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);

    /* file table (shown when archive is open) */
    g_table = new FileTable(0, MENU_H, g_main_window->w(), g_main_window->h() - MENU_H - STATUS_H);
    if (g_have_persisted_col_widths)
        g_table->set_column_widths(g_persisted_col_widths);
    g_table->hide();

    /* status bar */
    g_status = new Fl_Box(0, g_main_window->h() - STATUS_H, g_main_window->w(), STATUS_H, "  No archive open");
    g_status->box(FL_FLAT_BOX);
    g_status->color(FL_BACKGROUND_COLOR);
    g_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    g_status->labelsize(12);

    g_main_window->end();
    g_main_window->resizable(g_archive_open ? (Fl_Widget *)g_table : (Fl_Widget *)g_logo_box);
    g_main_window->size_range(500, 300);

    update_ui();

    /* Set window icon from embedded PNG before show() so FLTK's own icon
       mechanism registers it on all platforms (X11 _NET_WM_ICON and the
       WinAPI HICON path in Fl_WinAPI_Window_Driver::set_icons()). On
       Windows, install_win32_app_icon() then overrides this with the
       multi-resolution .ico resource for better quality. */
    {
        Fl_PNG_Image *icon_img = new Fl_PNG_Image("icon", s_logo_png, (int)s_logo_png_len);
        if (icon_img && icon_img->w() > 0)
            g_main_window->icon(icon_img);
        delete icon_img;
    }

    g_main_window->show();

#ifdef _WIN32
    install_win32_app_icon(g_main_window);
    install_win32_drop_fallback(g_main_window);
#endif

    if (g_shell_action.type != SHELL_ACTION_NONE)
        Fl::add_timeout(0.0, run_shell_action, NULL);

    return Fl::run();
}
