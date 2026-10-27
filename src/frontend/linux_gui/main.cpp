/*
 * main.cpp -- Linux GUI frontend for BoltPack (FLTK 1.4)
 *
 * Build:
 *   g++ -O2 -o boltpack-gui main.cpp \
 *       $(fltk-config --cxxflags --ldflags) \
 *       -I../../core -L../../core \
 *       core objects... -lm
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <climits>
#include <algorithm>
#include <new>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <set>
#include <map>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/syscall.h>
#ifdef __linux__
#include <linux/fs.h>
#endif

#include "../linux/extract_paths.h"

/* BoltPack core (C linkage) */
extern "C" {
#include "../../core/archiver.h"
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

#if defined(BOLTPACK_FAST_FLUFF) && defined(BOLTPACK_ZSTD)
#include <omp.h>
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
    return fseeko((FILE *)handle, (off_t)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)ftello((FILE *)handle);
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
static void save_prefs();

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
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
            *p = '/';
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
    char *slash = strrchr(tmp, '/');
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
static int g_zstd_multithread   = 0;
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
/* Map GUI level (0=fast, 1=normal, 2=best) to zstd native level (1-19). */
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
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0])
        snprintf(buf, size, "%s/boltpack/gui.conf", xdg);
    else {
        const char *home = getenv("HOME");
        if (!home) home = "/tmp";
        snprintf(buf, size, "%s/.config/boltpack/gui.conf", home);
    }
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
            if (v >= BP_METHOD_STORED && v <= BP_METHOD_ZSTD)
                g_compression_method = v;
        } else if (sscanf(line, "level=%d", &v) == 1) {
            if (v >= 0 && v <= 2)
                g_compression_level = v;
        } else if (sscanf(line, "fluff_mt=%d", &v) == 1) {
            g_fluff_multithread = v ? 1 : 0;
        } else if (sscanf(line, "zstd_mt=%d", &v) == 1) {
            g_zstd_multithread = v ? 1 : 0;
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
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = '\0'; mkdirs(dir); }
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "method=%d\nlevel=%d\nfluff_mt=%d\nzstd_mt=%d\n"
               "interchange_profile=%d\n",
            g_compression_method, g_compression_level, g_fluff_multithread,
            g_zstd_multithread, g_interchange_profile);
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
#if defined(BOLTPACK_FAST_FLUFF) && defined(BOLTPACK_ZSTD)
    int             zstd_multithread;
#endif
    Fl_Window      *win;
    Fl_Box         *level_sep;   /* separator line above level section */
    Fl_Box         *level_title; /* section heading */
    Fl_Group       *level_grp;
    Fl_Round_Button *level_btns[3];
#ifdef BOLTPACK_FAST_FLUFF
    Fl_Check_Button *fast_fluff_btn;
#endif
#if defined(BOLTPACK_FAST_FLUFF) && defined(BOLTPACK_ZSTD)
    Fl_Check_Button *zstd_mt_btn;
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
#if defined(BOLTPACK_FAST_FLUFF) && defined(BOLTPACK_ZSTD)
    if (!s->interchange_profile && s->method == BP_METHOD_ZSTD) {
        s->zstd_mt_btn->value(s->zstd_multithread ? 1 : 0);
        s->zstd_mt_btn->show();
    } else {
        s->zstd_mt_btn->hide();
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

#if defined(BOLTPACK_FAST_FLUFF) && defined(BOLTPACK_ZSTD)
static void cdlg_zstd_mt_cb(Fl_Widget *w, void *data)
{
    CompDlgState *s = (CompDlgState *)data;
    s->zstd_multithread = ((Fl_Check_Button *)w)->value() ? 1 : 0;
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
    int W = 460;
#ifdef BOLTPACK_ZSTD
    int n_methods = 5;
#else
    int n_methods = 4;
#endif
    int ff_y_for_height = (32 + n_methods * 24 + 6) + 6 + 8 + 20 + 80;
    int profile_y = ff_y_for_height;
#ifdef BOLTPACK_FAST_FLUFF
    profile_y += 24;
#endif
#if defined(BOLTPACK_FAST_FLUFF) && defined(BOLTPACK_ZSTD)
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
#if defined(BOLTPACK_FAST_FLUFF) && defined(BOLTPACK_ZSTD)
    s.zstd_multithread = g_zstd_multithread;
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
    n_methods = (int)(sizeof(method_ids) / sizeof(method_ids[0]));
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
#if defined(BOLTPACK_FAST_FLUFF) && defined(BOLTPACK_ZSTD)
    s.zstd_mt_btn = new Fl_Check_Button(18, ff_y + 24, W - 28, 22,
                                        "Use multi-threaded zstd (parallel file compression)");
    s.zstd_mt_btn->callback(cdlg_zstd_mt_cb, &s);
    s.zstd_mt_btn->value(s.zstd_multithread ? 1 : 0);
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
#if defined(BOLTPACK_FAST_FLUFF) && defined(BOLTPACK_ZSTD)
        g_zstd_multithread   = s.zstd_multithread;
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
    fseeko(fp, 0, SEEK_SET);
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

    /* Build summary */
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

    /* Show results in a scrollable text window */
    Fl_Window *dlg = new Fl_Window(520, 420, "Validate Archive");
    dlg->set_modal();

    Fl_Text_Buffer *buf = new Fl_Text_Buffer();
    buf->text(work.vctx.log.c_str());
    Fl_Text_Display *disp = new Fl_Text_Display(10, 10, 500, 360);
    disp->buffer(buf);
    disp->textfont(FL_COURIER);
    disp->textsize(12);

    Fl_Box *sum_box = new Fl_Box(10, 376, 400, 24, summary);
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

struct ExtractDirectoryRestore {
    std::string name;
    bp_uint64 mtime;
    bp_uint16 flags;
};

struct ExtractOutput {
    bp_io_t io; /* first member: callbacks return &io */
    int parent_fd;
    std::string temporary;
    std::string destination;
    bool is_symlink;

    ExtractOutput() : parent_fd(-1), is_symlink(false) {}
};

struct ExtractCtx {
    std::string           dest_dir;
    ProgressCtx          *progress;
    std::set<std::string> filter; /* empty = extract all; non-empty = only these names */
    ExtractMode           mode;
    std::set<std::string> reserved_flat_names;
    std::vector<ExtractDirectoryRestore> directories;
    int root_fd;
    int force;
    int open_error;
    unsigned int partial_count;

    ExtractCtx()
        : progress(NULL), mode(EXTRACT_KEEP_FOLDERS), root_fd(-1), force(1),
          open_error(BP_OK), partial_count(0)
    {
    }
};

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

static std::string reserve_flattened_name(ExtractCtx *ctx, const std::string &entry_name)
{
    std::string filename = leaf_name(entry_name);
    unsigned int index;
    struct stat st;
    int stat_rc;

    if (filename.empty())
        filename = "unnamed";

    if (ctx->reserved_flat_names.find(filename) == ctx->reserved_flat_names.end()) {
        stat_rc = fstatat(ctx->root_fd, filename.c_str(), &st, AT_SYMLINK_NOFOLLOW);
        if (stat_rc == 0) {
            /* Existing output names are kept by selecting a deterministic suffix. */
        } else if (errno != ENOENT) {
            return std::string();
        } else {
            ctx->reserved_flat_names.insert(filename);
            return filename;
        }
    }

    for (index = 1; index <= 65535U; index++) {
        std::string candidate = path_with_suffix(filename, index);
        if (ctx->reserved_flat_names.find(candidate) != ctx->reserved_flat_names.end())
            continue;
        stat_rc = fstatat(ctx->root_fd, candidate.c_str(), &st, AT_SYMLINK_NOFOLLOW);
        if (stat_rc == 0) continue;
        if (errno != ENOENT) return std::string();
        ctx->reserved_flat_names.insert(candidate);
        return candidate;
    }
    return std::string();
}

static int extract_open_root(ExtractCtx *ctx)
{
    if (ctx == NULL) return BP_ERR_INVALID_ARG;
    if (ctx->dest_dir.empty()) ctx->dest_dir = ".";
    if (mkdirs(ctx->dest_dir.c_str()) != 0) return BP_ERR_IO;
    ctx->root_fd = open(ctx->dest_dir.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    return ctx->root_fd < 0 ? BP_ERR_IO : BP_OK;
}

static int extract_queue_directory(ExtractCtx *ctx, const bp_entry_t *entry)
{
    ExtractDirectoryRestore directory;
    if (ctx->directories.size() >= 65535U) return BP_ERR_OVERFLOW;
    try {
        directory.name = entry->name;
        directory.mtime = entry->mtime;
        directory.flags = entry->flags;
        ctx->directories.push_back(directory);
    } catch (...) {
        return BP_ERR_NOMEM;
    }
    return BP_OK;
}

static int extract_restore_fd(int fd, const std::string &name,
                              bp_uint16 flags, bp_uint64 mtime,
                              ExtractCtx *ctx)
{
    if (flags & BP_ENTRY_FLAG_MODE_VALID) {
        mode_t mode = (mode_t)(flags & BP_ENTRY_FLAG_MODE_MASK);
        if (mode & 06000) {
            mode &= (mode_t)~06000;
            ctx->partial_count++;
        }
        if (fchmod(fd, mode) != 0) return BP_ERR_IO;
    }
    if (flags & BP_ENTRY_FLAG_MTIME_VALID) {
        bp_off_t seconds = bp_u64_to_off(mtime);
        time_t t = (time_t)seconds;
        struct timespec times[2];
        if (seconds < 0 || bp_u64_cmp(bp_off_to_u64(seconds), mtime) != 0 ||
            (bp_off_t)t != seconds) return BP_ERR_OVERFLOW;
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_OMIT;
        times[1].tv_sec = t;
        times[1].tv_nsec = 0;
        if (futimens(fd, times) != 0) return BP_ERR_IO;
    }
    (void)name;
    return BP_OK;
}

static int extract_finish_directories(ExtractCtx *ctx, int apply)
{
    int result = BP_OK;
    std::sort(ctx->directories.begin(), ctx->directories.end(),
              [](const ExtractDirectoryRestore &a,
                 const ExtractDirectoryRestore &b) {
                  return a.name.size() > b.name.size();
              });
    if (apply) {
        for (size_t i = 0; i < ctx->directories.size(); i++) {
            const ExtractDirectoryRestore &directory = ctx->directories[i];
            int fd = extract_open_directory(ctx->root_fd,
                                            directory.name.c_str(), 0);
            int rc = fd < 0 ? BP_ERR_IO :
                extract_restore_fd(fd, directory.name, directory.flags,
                                   directory.mtime, ctx);
            if (fd >= 0) close(fd);
            if (rc != BP_OK && result == BP_OK) result = rc;
        }
    }
    ctx->directories.clear();
    return result;
}

static int extract_make_temporary(int parent_fd, std::string *name,
                                  FILE **stream)
{
    static std::atomic<unsigned long> counter(0);
    char candidate[64];
    int fd, tries;
    for (tries = 0; tries < 256; tries++) {
        snprintf(candidate, sizeof(candidate), ".boltpack-gui-%08lx-%08lx",
                 (unsigned long)getpid(), ++counter);
        fd = openat(parent_fd, candidate,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return BP_ERR_IO;
    *stream = fdopen(fd, "wb");
    if (*stream == NULL) {
        close(fd);
        unlinkat(parent_fd, candidate, 0);
        return BP_ERR_IO;
    }
    try {
        *name = candidate;
    } catch (...) {
        fclose(*stream);
        *stream = NULL;
        unlinkat(parent_fd, candidate, 0);
        return BP_ERR_NOMEM;
    }
    return BP_OK;
}

static int extract_progress_cb(const bp_progress_t *p, void *user_data)
{
    ExtractCtx *ectx = (ExtractCtx *)user_data;
    return progress_cb(p, ectx->progress);
}

static void extract_dispose_output(ExtractOutput *output, FILE *fp,
                                   int remove_temporary)
{
    if (fp != NULL) fclose(fp);
    if (output->parent_fd >= 0) {
        if (remove_temporary && !output->temporary.empty())
            unlinkat(output->parent_fd, output->temporary.c_str(), 0);
        close(output->parent_fd);
    }
    delete output;
}

static int extract_make_temporary_symlink(int parent_fd, const char *target,
                                          std::string *name)
{
    static std::atomic<unsigned long> counter(0);
    char candidate[64];
    int tries;
    for (tries = 0; tries < 256; tries++) {
        snprintf(candidate, sizeof(candidate), ".boltpack-gui-link-%08lx-%08lx",
                 (unsigned long)getpid(), ++counter);
        if (symlinkat(target, parent_fd, candidate) == 0) {
            try {
                *name = candidate;
            } catch (...) {
                unlinkat(parent_fd, candidate, 0);
                return BP_ERR_NOMEM;
            }
            return BP_OK;
        }
        if (errno != EEXIST) return BP_ERR_IO;
    }
    return BP_ERR_IO;
}

static bp_io_t *extract_open_cb(const bp_entry_t *entry, void *user_data)
{
    ExtractCtx *ctx = (ExtractCtx *)user_data;
    std::string ename(entry->name ? entry->name : "");
    std::string output_name;
    ExtractOutput *output;
    FILE *fp;
    char leaf[4096];

    if (!entry->name || !bp_name_is_safe(entry->name)) {
        ctx->open_error = BP_ERR_CORRUPT;
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        if (!ctx->filter.empty() &&
            (ctx->mode == EXTRACT_FILES_ONLY ||
             !extract_filter_matches(ctx, ename)))
            return NULL;
        int fd = extract_open_directory(ctx->root_fd, ename.c_str(), 1);
        if (fd < 0) {
            ctx->open_error = BP_ERR_IO;
            return NULL;
        }
        close(fd);
        int rc = extract_queue_directory(ctx, entry);
        if (rc != BP_OK && ctx->open_error == BP_OK) ctx->open_error = rc;
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_META || entry->entry_type == BP_TYPE_RSRC) {
        if (extract_filter_matches(ctx, ename)) ctx->partial_count++;
        return NULL;
    }

    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_SYMLINK) {
        if (extract_filter_matches(ctx, ename)) ctx->partial_count++;
        return NULL;
    }

    if (!extract_filter_matches(ctx, ename)) return NULL;

    if (!ctx->filter.empty() && ctx->mode == EXTRACT_FILES_ONLY)
        output_name = reserve_flattened_name(ctx, ename);
    else
        output_name = ename;
    if (output_name.empty()) {
        ctx->open_error = BP_ERR_IO;
        return NULL;
    }

    output = new (std::nothrow) ExtractOutput();
    if (output == NULL) {
        ctx->open_error = BP_ERR_NOMEM;
        return NULL;
    }
    try {
        output->destination = output_name;
    } catch (...) {
        delete output;
        ctx->open_error = BP_ERR_NOMEM;
        return NULL;
    }
    output->is_symlink = entry->entry_type == BP_TYPE_SYMLINK;

    if (output->is_symlink) {
        fp = tmpfile();
        if (fp == NULL) {
            delete output;
            ctx->open_error = BP_ERR_IO;
            return NULL;
        }
    } else {
        output->parent_fd = extract_open_parent(ctx->root_fd,
                                               output_name.c_str(),
                                               leaf, sizeof(leaf));
        if (output->parent_fd < 0) {
            delete output;
            ctx->open_error = BP_ERR_IO;
            return NULL;
        }
        if (!ctx->force) {
            struct stat st;
            if (fstatat(output->parent_fd, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 ||
                errno != ENOENT) {
                close(output->parent_fd);
                delete output;
                ctx->open_error = BP_ERR_IO;
                return NULL;
            }
        }
        try {
            output->destination = leaf;
        } catch (...) {
            close(output->parent_fd);
            delete output;
            ctx->open_error = BP_ERR_NOMEM;
            return NULL;
        }
        int rc = extract_make_temporary(output->parent_fd,
                                       &output->temporary, &fp);
        if (rc != BP_OK) {
            close(output->parent_fd);
            delete output;
            ctx->open_error = rc;
            return NULL;
        }
    }
    bp_io_from_file(&output->io, fp);
    return &output->io;
}

static int extract_done_cb(const bp_entry_t *entry, bp_io_t *out, int status,
                           void *user_data)
{
    ExtractCtx *ctx = (ExtractCtx *)user_data;
    ExtractOutput *output = (ExtractOutput *)out;
    FILE *fp = (FILE *)out->handle;

    if (status != BP_OK) {
        extract_dispose_output(output, fp, 1);
        return BP_OK;
    }

    if (output->is_symlink) {
        char target[4096];
        long length = fseek(fp, 0, SEEK_END) == 0 ? ftell(fp) : -1;
        int rc = BP_OK;
        if (length <= 0 || length >= (long)sizeof(target)) {
            rc = BP_ERR_CORRUPT;
        } else {
            rewind(fp);
            if (fread(target, 1, (size_t)length, fp) != (size_t)length ||
                memchr(target, '\0', (size_t)length) != NULL) {
                rc = BP_ERR_CORRUPT;
            } else {
                target[length] = '\0';
                if (fclose(fp) != 0) rc = BP_ERR_IO;
                fp = NULL;
                if (rc == BP_OK &&
                    !bp_symlink_target_is_safe(output->destination.c_str(), target)) {
                    ctx->partial_count++;
                } else if (rc == BP_OK) {
                    char leaf[4096];
                    output->parent_fd = extract_open_parent(ctx->root_fd,
                                                           output->destination.c_str(),
                                                           leaf, sizeof(leaf));
                    if (output->parent_fd < 0) rc = BP_ERR_IO;
                    else {
                        try {
                            output->destination = leaf;
                        } catch (...) {
                            rc = BP_ERR_NOMEM;
                        }
                        if (rc == BP_OK && !ctx->force) {
                            struct stat st;
                            if (fstatat(output->parent_fd, leaf, &st,
                                        AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
                                rc = BP_ERR_IO;
                        }
                        if (rc == BP_OK)
                            rc = extract_make_temporary_symlink(output->parent_fd,
                                                               target,
                                                               &output->temporary);
                        if (rc == BP_OK && (entry->flags & BP_ENTRY_FLAG_MTIME_VALID)) {
                            bp_off_t seconds = bp_u64_to_off(entry->mtime);
                            time_t t = (time_t)seconds;
                            struct timespec times[2];
                            if (seconds < 0 ||
                                bp_u64_cmp(bp_off_to_u64(seconds), entry->mtime) != 0 ||
                                (bp_off_t)t != seconds) {
                                rc = BP_ERR_OVERFLOW;
                            } else {
                                times[0].tv_sec = 0;
                                times[0].tv_nsec = UTIME_OMIT;
                                times[1].tv_sec = t;
                                times[1].tv_nsec = 0;
                                if (utimensat(output->parent_fd,
                                              output->temporary.c_str(), times,
                                              AT_SYMLINK_NOFOLLOW) != 0)
                                    rc = BP_ERR_IO;
                            }
                        }
                        if (rc == BP_OK) {
                            if (ctx->force) {
                                if (renameat(output->parent_fd,
                                             output->temporary.c_str(),
                                             output->parent_fd,
                                             output->destination.c_str()) != 0)
                                    rc = BP_ERR_IO;
                            } else if (extract_commit_noreplace(
                                           output->parent_fd,
                                           output->temporary.c_str(),
                                           output->destination.c_str()) != 0) {
                                rc = BP_ERR_IO;
                            }
                        }
                    }
                }
            }
        }
        if (fp != NULL && fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
        extract_dispose_output(output, NULL, rc != BP_OK);
        return rc;
    }

    int rc = fflush(fp) == 0 ? BP_OK : BP_ERR_IO;
    if (rc == BP_OK)
        rc = extract_restore_fd(fileno(fp), output->destination, entry->flags,
                                entry->mtime, ctx);
    if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (rc == BP_OK) {
        if (ctx->force) {
            if (renameat(output->parent_fd, output->temporary.c_str(),
                         output->parent_fd, output->destination.c_str()) != 0)
                rc = BP_ERR_IO;
        } else if (extract_commit_noreplace(output->parent_fd,
                                             output->temporary.c_str(),
                                             output->destination.c_str()) != 0) {
            rc = BP_ERR_IO;
        }
    }
    extract_dispose_output(output, NULL, rc != BP_OK);
    return rc;
}

static int extract_run(bp_io_t *archive_io, ExtractCtx *ctx,
                       bp_progress_fn progress_fn)
{
    int rc, dir_rc;
    rc = extract_open_root(ctx);
    if (rc != BP_OK) return rc;
    rc = bp_archive_extract_all_ex(archive_io, extract_open_cb,
                                   extract_done_cb, progress_fn, ctx);
    if ((rc == BP_OK || rc == BP_ERR_PARTIAL) && ctx->open_error != BP_OK)
        rc = ctx->open_error;
    dir_rc = extract_finish_directories(
        ctx, (rc == BP_OK || rc == BP_ERR_PARTIAL) && ctx->open_error == BP_OK);
    if ((rc == BP_OK || rc == BP_ERR_PARTIAL) && dir_rc != BP_OK) rc = dir_rc;
    if (rc == BP_OK && ctx->partial_count != 0) rc = BP_ERR_PARTIAL;
    close(ctx->root_fd);
    ctx->root_fd = -1;
    return rc;
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
    (void)ud;
    ctx.dest_dir = (out_dir && *out_dir) ? out_dir : ".";
    ctx.progress = NULL;
    ctx.mode     = EXTRACT_KEEP_FOLDERS;
    ctx.force    = force;
    return extract_run(aio, &ctx, NULL);
}
#endif

struct ExtractWork {
    std::string  archive_path;
    ExtractCtx   ectx;
    int          result;
};

static void extract_worker(ExtractWork *w)
{
    FILE *fp = fopen(w->archive_path.c_str(), "rb");
    if (!fp) { w->result = BP_ERR_IO; w->ectx.progress->finished.store(true); return; }

    bp_io_t io;
    bp_io_from_file(&io, fp);

    w->result = extract_run(&io, &w->ectx, extract_progress_cb);
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
    work.ectx.dest_dir      = dest;
    work.ectx.progress      = create_progress_window("Extracting...");
    work.ectx.force         = 1;
    work.result              = BP_OK;

    std::thread worker(extract_worker, &work);

    while (!work.ectx.progress->finished.load())
        Fl::wait(0.05);

    worker.join();

    int rc = work.result;
    destroy_progress_window(work.ectx.progress);

    if (rc == BP_ERR_ABORTED) {
        fl_message("Extraction cancelled.");
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
    work.ectx.dest_dir = dest;
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

    if (rc == BP_ERR_ABORTED) {
        fl_message("Extraction cancelled.");
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

static int add_file_to_archive(bp_archive_t *ar, const char *filepath,
                               const char *arc_name)
{
    struct stat st;
    if (stat(filepath, &st) != 0) return BP_ERR_IO;

    if (S_ISDIR(st.st_mode)) {
        /* add directory entry */
        std::string dname = std::string(arc_name);
        if (dname.empty() || dname[dname.size()-1] != '/')
            dname += '/';
        bp_uint64 mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
        int rc = bp_archive_add_entry(ar, dname.c_str(), BP_TYPE_DIR,
                                      BP_OS_LINUX, NULL,
                                      bp_u64_from_u32(0), mtime);
        if (rc != BP_OK) return rc;
        return add_dir_recursive(ar, filepath, dname.c_str());
    }

    FILE *fp = fopen(filepath, "rb");
    if (!fp) return BP_ERR_IO;

    bp_io_t fio;
    bp_io_from_file(&fio, fp);

    bp_uint64 fsize = bp_off_to_u64((bp_off_t)st.st_size);
    bp_uint64 mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
    int rc = bp_archive_add_entry(ar, arc_name, BP_TYPE_DATA,
                                  BP_OS_LINUX, &fio, fsize, mtime);
    fclose(fp);
    return rc;
}

static int add_dir_recursive(bp_archive_t *ar, const char *dirpath,
                             const char *prefix)
{
    DIR *d = opendir(dirpath);
    if (!d) return BP_ERR_IO;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        std::string full = std::string(dirpath) + "/" + ent->d_name;
        std::string arc  = std::string(prefix) + ent->d_name;

        int rc = add_file_to_archive(ar, full.c_str(), arc.c_str());
        if (rc != BP_OK) { closedir(d); return rc; }
    }
    closedir(d);
    return BP_OK;
}

/* recursively sum input bytes and count archive entries for progress */
static void count_input_recursive(const char *path, double &bytes, unsigned long &count)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        count++;
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            std::string full = std::string(path) + "/" + ent->d_name;
            count_input_recursive(full.c_str(), bytes, count);
        }
        closedir(d);
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
    bool                     use_fast_zstd;
    int                      result;
};

#if defined(BOLTPACK_FAST_FLUFF) && defined(BOLTPACK_ZSTD)
/* Job descriptor for parallel zstd file collection */
struct GuiDataJob {
    std::string path;
    std::string arcname;
    bool        is_dir;
    bp_uint64   file_size;
    bp_uint64   mtime;
};

static void gui_collect_jobs(const std::string &fpath, const std::string &arcname,
                              std::vector<GuiDataJob> &jobs)
{
    struct stat st;
    if (lstat(fpath.c_str(), &st) != 0) return;

    if (S_ISDIR(st.st_mode)) {
        std::string dname = arcname;
        if (dname.empty() || dname[dname.size() - 1] != '/') dname += '/';
        GuiDataJob dj;
        dj.path = fpath; dj.arcname = dname; dj.is_dir = true;
        dj.file_size = bp_u64_from_u32(0);
        dj.mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
        jobs.push_back(dj);
        DIR *d = opendir(fpath.c_str());
        if (!d) return;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            gui_collect_jobs(fpath + "/" + ent->d_name, dname + ent->d_name, jobs);
        }
        closedir(d);
        return;
    }
    if (S_ISREG(st.st_mode)) {
        GuiDataJob j;
        j.path = fpath; j.arcname = arcname; j.is_dir = false;
        j.file_size = bp_off_to_u64((bp_off_t)st.st_size);
        j.mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
        jobs.push_back(j);
    }
}

/* Compress a batch of data files to temp files in parallel, then commit them. */
static int gui_zstd_batch(bp_archive_t *ar, const std::vector<GuiDataJob> &jobs,
                           int start, int batch_n)
{
    std::vector<FILE *>            tmp_fps(batch_n, nullptr);
    std::vector<bp_io_t>           tmp_ios(batch_n);
    std::vector<bp_packed_entry_t> packed(batch_n);
    std::vector<int>               rcs(batch_n, BP_OK);

    memset(tmp_ios.data(), 0, (size_t)batch_n * sizeof(bp_io_t));
    memset(packed.data(), 0, (size_t)batch_n * sizeof(bp_packed_entry_t));

#pragma omp parallel for schedule(dynamic) num_threads(batch_n)
    for (int j = 0; j < batch_n; j++) {
        const GuiDataJob &job = jobs[(size_t)(start + j)];
        bp_archive_t worker_ar;
        memset(&worker_ar, 0, sizeof(worker_ar));
        worker_ar.block_size   = ar->block_size;
        worker_ar.method       = ar->method;
        worker_ar.method_level = ar->method_level;
        worker_ar.creator_os   = ar->creator_os;
        worker_ar.creator_cpu  = ar->creator_cpu;

        FILE *src_fp = fopen(job.path.c_str(), "rb");
        FILE *tmp_fp = tmpfile();
        if (!src_fp || !tmp_fp) {
            if (src_fp) fclose(src_fp);
            if (tmp_fp) fclose(tmp_fp);
            rcs[(size_t)j] = BP_ERR_IO;
        } else {
            bp_io_t src_io;
            bp_io_from_file(&src_io, src_fp);
            bp_io_from_file(&tmp_ios[(size_t)j], tmp_fp);
            rcs[(size_t)j] = bp_archive_pack_entry(
                &worker_ar, job.arcname.c_str(), BP_TYPE_DATA,
                BP_OS_LINUX, &src_io, job.file_size, job.mtime,
                &tmp_ios[(size_t)j], &packed[(size_t)j]);
            fclose(src_fp);
            bp_archive_close(&worker_ar);
            if (rcs[(size_t)j] == BP_OK) {
                rewind(tmp_fp);
                tmp_fps[(size_t)j] = tmp_fp;
            } else {
                fclose(tmp_fp);
            }
        }
    }

    /* sequential commit */
    int rc = BP_OK;
    for (int j = 0; j < batch_n; j++) {
        if (rcs[(size_t)j] != BP_OK) {
            rc = rcs[(size_t)j];
            for (int k = j; k < batch_n; k++) {
                bp_packed_entry_dispose(&packed[(size_t)k]);
                if (tmp_fps[(size_t)k]) { fclose(tmp_fps[(size_t)k]); tmp_fps[(size_t)k] = nullptr; }
            }
            return rc;
        }
        rc = bp_archive_commit_packed_entry(ar, &tmp_ios[(size_t)j], &packed[(size_t)j]);
        bp_packed_entry_dispose(&packed[(size_t)j]);
        if (tmp_fps[(size_t)j]) { fclose(tmp_fps[(size_t)j]); tmp_fps[(size_t)j] = nullptr; }
        if (rc != BP_OK) {
            for (int k = j + 1; k < batch_n; k++) {
                bp_packed_entry_dispose(&packed[(size_t)k]);
                if (tmp_fps[(size_t)k]) { fclose(tmp_fps[(size_t)k]); tmp_fps[(size_t)k] = nullptr; }
            }
            return rc;
        }
    }
    return BP_OK;
}
#endif /* BOLTPACK_FAST_FLUFF && BOLTPACK_ZSTD */

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

static void create_worker(CreateWork *w)
{
    FILE *fp = w->output_fp;
    w->output_fp = NULL;
    if (!fp)
        fp = fopen(w->out_path.c_str(), "w+b");
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

#if defined(BOLTPACK_FAST_FLUFF) && defined(BOLTPACK_ZSTD)
        if (method == BP_METHOD_ZSTD && w->use_fast_zstd) {
            /* Collect all leaf files, compress batches in parallel, commit. */
            std::vector<GuiDataJob> jobs;
            for (size_t i = 0; i < w->input_files.size(); i++) {
                std::string base = leaf_name(w->input_files[i]);
                if (base.empty()) base = "archive";
                gui_collect_jobs(w->input_files[i], base, jobs);
            }
            int nthreads = fast_fluff_thread_count();
            int ji = 0;
            while (ji < (int)jobs.size() && !w->pctx->cancelled.load() && rc == BP_OK) {
                if (jobs[(size_t)ji].is_dir) {
                    rc = bp_archive_add_entry(&ar, jobs[(size_t)ji].arcname.c_str(),
                                              BP_TYPE_DIR, BP_OS_LINUX, nullptr,
                                              bp_u64_from_u32(0), jobs[(size_t)ji].mtime);
                    ji++;
                    continue;
                }
                /* batch of up to nthreads data files */
                int batch_n = 0;
                while (ji + batch_n < (int)jobs.size() &&
                       !jobs[(size_t)(ji + batch_n)].is_dir &&
                       batch_n < nthreads)
                    batch_n++;
                if (batch_n <= 0) break;
                rc = gui_zstd_batch(&ar, jobs, ji, batch_n);
                ji += batch_n;
            }
            if (w->pctx->cancelled.load() && rc == BP_OK) rc = BP_ERR_ABORTED;
        } else
#endif
        {
            /* sequential path for all other methods and single-threaded zstd */
            for (size_t i = 0; i < w->input_files.size() && !w->pctx->cancelled.load(); i++) {
                const char *path = w->input_files[i].c_str();
                std::string base_str = leaf_name(std::string(path));
                if (base_str.empty()) base_str = "archive";
                rc = add_file_to_archive(&ar, path, base_str.c_str());
                if (rc != BP_OK) break;
            }
            if (w->pctx->cancelled.load() && rc == BP_OK) rc = BP_ERR_ABORTED;
        }

        if (rc == BP_OK)
            rc = bp_archive_finalize(&ar);
    }
    bp_archive_close(&ar);
    if (rc == BP_OK && fflush(fp) != 0)
        rc = BP_ERR_IO;
    if (rc == BP_OK && fsync(fileno(fp)) != 0)
        rc = BP_ERR_IO;
    if (fclose(fp) != 0 && rc == BP_OK)
        rc = BP_ERR_IO;
    w->result = rc;
    w->pctx->finished.store(true);
}

#ifdef BOLTPACK_NET
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
    if (gethostname(host, sizeof(host)) != 0) strcpy(host, "boltpack");
    host[sizeof(host) - 1] = '\0';
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
#endif /* BOLTPACK_NET */

#ifdef BOLTPACK_NET
/* Create the archive to a temp file and stream it to a chosen receiver. */
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

    char dirtmpl[] = "/tmp/boltpack-XXXXXX";
    char *d = mkdtemp(dirtmpl);
    if (!d) {
        fl_alert("Could not create a temporary folder for sending.");
        return;
    }
    std::string out_path = std::string(d) + "/" + name;

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
    work.use_fast_zstd  = (g_zstd_multithread != 0);
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
    rmdir(d);
}
#endif /* BOLTPACK_NET */

/* Create a same-directory archive stage with normal 0666/umask permissions. */
static std::atomic<unsigned long> g_archive_stage_serial(0);

static int create_sibling_archive_stage(const std::string &out_path,
                                       const struct stat *existing,
                                       std::string &stage_path,
                                       FILE **stage_fp)
{
    size_t slash = out_path.find_last_of('/');
    std::string parent = slash == std::string::npos ? "." :
                         slash == 0 ? "/" : out_path.substr(0, slash);

    for (unsigned int attempt = 0; attempt < 1024; attempt++) {
        unsigned long serial = g_archive_stage_serial.fetch_add(1);
        char name[96];
        snprintf(name, sizeof(name), ".boltpack-stage-%ld-%lu",
                 (long)getpid(), serial);
        std::string candidate = parent + "/" + name;
        int fd = open(candidate.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
                      0666);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return -1;
        }

        if (existing) {
            struct stat staged;
            if ((fchown(fd, existing->st_uid, existing->st_gid) != 0 &&
                 (fstat(fd, &staged) != 0 || staged.st_uid != existing->st_uid ||
                  staged.st_gid != existing->st_gid)) ||
                fchmod(fd, existing->st_mode & 07777) != 0) {
                int saved_errno = errno ? errno : EPERM;
                close(fd);
                unlink(candidate.c_str());
                errno = saved_errno;
                return -1;
            }
        }

        FILE *fp = fdopen(fd, "w+b");
        if (!fp) {
            int saved_errno = errno;
            close(fd);
            unlink(candidate.c_str());
            errno = saved_errno;
            return -1;
        }
        stage_path = candidate;
        *stage_fp = fp;
        return 0;
    }

    errno = EEXIST;
    return -1;
}

static bool same_file_identity(const struct stat *a, const struct stat *b)
{
    return S_ISREG(a->st_mode) && S_ISREG(b->st_mode) &&
           a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

/* Publish only if the selected destination still matches the user's choice.
 * Returns 0 on success, -1 on a recoverable failure, and -2 if a failed
 * rollback leaves the displaced destination at stage_path. */
static int publish_sibling_archive_stage(const std::string &stage_path,
                                         const std::string &out_path,
                                         bool replace_existing,
                                         const struct stat *expected)
{
    struct stat current;
    if (replace_existing) {
        if (lstat(out_path.c_str(), &current) != 0 ||
            !same_file_identity(&current, expected)) {
            errno = ESTALE;
            return -1;
        }
    } else if (lstat(out_path.c_str(), &current) == 0 || errno != ENOENT) {
        errno = EEXIST;
        return -1;
    }

#if defined(__linux__) && defined(SYS_renameat2) && defined(RENAME_EXCHANGE)
    if (replace_existing) {
        if (syscall(SYS_renameat2, AT_FDCWD, stage_path.c_str(), AT_FDCWD,
                    out_path.c_str(), RENAME_EXCHANGE) == 0) {
            struct stat displaced;
            if (lstat(stage_path.c_str(), &displaced) == 0 &&
                same_file_identity(&displaced, expected)) {
                (void)unlink(stage_path.c_str());
                return 0;
            }
            int saved_errno = ESTALE;
            if (syscall(SYS_renameat2, AT_FDCWD, stage_path.c_str(), AT_FDCWD,
                        out_path.c_str(), RENAME_EXCHANGE) != 0)
                return -2;
            errno = saved_errno;
            return -1;
        }
        if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP)
            return -1;
    }
#endif

    if (replace_existing) {
        /* Best available fallback on filesystems without exchange support. */
        if (lstat(out_path.c_str(), &current) != 0 ||
            !same_file_identity(&current, expected)) {
            errno = ESTALE;
            return -1;
        }
        return rename(stage_path.c_str(), out_path.c_str());
    }

    /* link() gives new destinations no-replace publication semantics. */
    if (link(stage_path.c_str(), out_path.c_str()) != 0)
        return -1;
    (void)unlink(stage_path.c_str());
    return 0;
}

/* Create the archive and save it to a user-chosen path. */
static void create_to_save(const std::vector<std::string> &input_files)
{
    /* build suggested archive name from first input item */
    std::string suggested;
    {
        std::string b = leaf_name(input_files[0]);
        /* strip any existing extension */
        size_t dot = b.rfind('.');
        if (dot != std::string::npos && dot > 0)
            b = b.substr(0, dot);
        suggested = b + ".blt";
    }

    /* pick output archive */
    Fl_Native_File_Chooser save_chooser;
    save_chooser.title("Save archive as");
    save_chooser.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    save_chooser.filter("BoltPack Archives\t*.{bolt,blt}");
    save_chooser.preset_file(suggested.c_str());
    {
        /* pre-seed directory to the parent of the first chosen input */
        std::string first = input_files[0];
        while (first.size() > 1 && first[first.size()-1] == '/') first.erase(first.size()-1);
        size_t pos = first.find_last_of('/');
        if (pos != std::string::npos)
            save_chooser.directory(first.substr(0, pos > 0 ? pos : 1).c_str());
    }
    if (save_chooser.show() != 0) return;

    std::string out_path = save_chooser.filename();
    if (out_path.empty()) return;

    /* ensure .bolt extension */
    if (out_path.size() < 5 ||
        (out_path.substr(out_path.size()-5) != ".bolt" &&
         out_path.substr(out_path.size()-4) != ".blt"))
        out_path += ".bolt";

    /* Only replace a regular file the user confirmed; never follow a symlink. */
    struct stat target_stat;
    bool target_exists = false;
    if (lstat(out_path.c_str(), &target_stat) == 0) {
        if (!S_ISREG(target_stat.st_mode)) {
            fl_alert("The selected destination is not a regular file.");
            return;
        }
        int choice = fl_choice("File already exists:\n%s\n\nOverwrite?",
                               "Cancel", "Overwrite", NULL, out_path.c_str());
        if (choice != 1) return;
        target_exists = true;
    } else if (errno != ENOENT) {
        fl_alert("Could not inspect the selected destination:\n%s",
                 strerror(errno));
        return;
    }

    std::string stage_path;
    FILE *stage_fp = NULL;
    if (create_sibling_archive_stage(out_path,
                                    target_exists ? &target_stat : NULL,
                                    stage_path, &stage_fp) != 0) {
        fl_alert("Could not create a staging file beside the destination:\n%s",
                 strerror(errno));
        return;
    }

    ProgressCtx *pctx = create_progress_window("Creating archive...");

    /* pre-calculate total input size and file count for progress */
    {
        double bytes = 0.0;
        unsigned long files = 0;
        for (int i = 0; i < (int)input_files.size(); i++)
            count_input_recursive(input_files[i].c_str(), bytes, files);
        pctx->total_bytes = bytes;
        pctx->total_files = files;
    }
    init_create_progress(pctx);

    CreateWork work;
    work.input_files = input_files;
    work.out_path    = stage_path;
    work.output_fp   = stage_fp;
    work.pctx        = pctx;
    work.method      = g_compression_method;
    work.level       = g_compression_level;
    work.use_interchange_profile = (g_interchange_profile != 0);
    work.use_fast_fluff = (g_fluff_multithread != 0);
    work.use_fast_zstd  = (g_zstd_multithread != 0);
    work.result      = BP_OK;

    std::thread worker(create_worker, &work);

    /* local event loop — GUI stays responsive while worker compresses */
    while (!pctx->finished.load()) {
        Fl::wait(0.05);
    }

    worker.join();

    bool was_cancelled = pctx->cancelled.load();
    int rc = work.result;
    destroy_progress_window(pctx);

    if (was_cancelled || rc == BP_ERR_ABORTED) {
        unlink(stage_path.c_str());
        fl_message("Archive creation cancelled.");
    } else if (rc != BP_OK) {
        unlink(stage_path.c_str());
        fl_alert("Error creating archive:\n%s", bp_strerror(rc));
    } else {
        int publish_rc = publish_sibling_archive_stage(stage_path, out_path,
                                                       target_exists,
                                                       target_exists ? &target_stat : NULL);
        if (publish_rc == 0) {
            do_open_archive(out_path.c_str());
        } else if (publish_rc == -2) {
            fl_alert("Archive was created, but the previous destination could not be "
                     "restored after a concurrent change. The displaced file is at:\n%s",
                     stage_path.c_str());
        } else {
            int saved_errno = errno;
            unlink(stage_path.c_str());
            fl_alert("Could not publish the completed archive:\n%s",
                     strerror(saved_errno));
        }
    }
}

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
    create_to_save(input_files);
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

        /* strip file:// prefix */
        if (line.compare(0, 7, "file://") == 0)
            line = line.substr(7);

        line = url_decode(line);
        if (!line.empty())
            paths.push_back(line);
    }
    return paths;
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
        create_to_save(others);
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

void FileTable::handle_drop(const char *text)
{
    std::vector<std::string> paths = parse_dnd_uris(text);
    if (!paths.empty())
        classify_and_act(paths);
}

#ifdef BOLTPACK_NET
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
        if (gethostname(host, sizeof(host)) != 0) strcpy(host, "boltpack");
        host[sizeof(host) - 1] = '\0';
        lw->name_in->value(host);
    }

    lw->folder_in = new Fl_Input(90, 46, W - 100 - 84, 26, "Save to:");
    {
        const char *home = getenv("HOME");
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

static void menu_validate_cb(Fl_Widget *, void *)
{
    do_validate();
}

static void menu_info_cb(Fl_Widget *, void *)
{
    do_info();
}

static void menu_close_cb(Fl_Widget *, void *)
{
    do_close_archive();
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
                           "Fluff and zstd threading, window size and position,\n"
                           "and file-list columns.",
                           "Cancel", "Reset", NULL);
    if (choice != 1) return;

    g_compression_method = BP_METHOD_BOLTLZ;
    g_compression_level = 1;
    g_fluff_multithread = 0;
    g_zstd_multithread  = 0;

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

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    int window_w;
    int window_h;

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
    g_menu->add("Archive/Extract All",      FL_CTRL + 'e', menu_extract_all_cb);
    g_menu->add("Archive/Extract Selected", 0,             menu_extract_cb);
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

    /* set window icon from embedded logo (must be done before show()) */
    {
        Fl_PNG_Image *icon_img = new Fl_PNG_Image("icon", s_logo_png, (int)s_logo_png_len);
        if (icon_img && icon_img->w() > 0)
            g_main_window->icon(icon_img);
        delete icon_img; /* FLTK copied it internally; we can free ours */
    }

    /* if command-line argument is a .bolt file, open it */
    if (argc > 1)
        do_open_archive(argv[1]);

    g_main_window->show(argc, argv);
    return Fl::run();
}
