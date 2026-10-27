/*
 * main.m -- macOS Cocoa GUI frontend for BoltPack
 *
 * Default target: macOS 10.9 Mavericks and later
 * Legacy mode:    define BP_LEGACY_COCOA=1 for a Mac OS X 10.5-compatible
 *                 Cocoa build used by the separate pre-10.9 frontend.
 * Toolkit:        Cocoa (AppKit)
 * Build:          clang -framework Cocoa -ObjC -fobjc-arc -O2 ...
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "archiver.h"
#include "compress.h"
#include "fluff.h"
#include "boltlite.h"
#include "help_text.h"
#include "bp_time.h"

#ifndef BP_LEGACY_COCOA
#define BP_LEGACY_COCOA 0
#endif

#ifndef BP_GUI_DEV_LOGGING
#define BP_GUI_DEV_LOGGING 0
#endif

#if BP_LEGACY_COCOA
#define BP_PROPERTY_RETAIN retain
#define BP_UNSAFE_UNRETAINED
#define BP_INIT_RESULT                id
#define BP_WINDOW_MASK_TITLED         NSTitledWindowMask
#define BP_WINDOW_MASK_CLOSABLE       NSClosableWindowMask
#define BP_WINDOW_MASK_MINIATURIZABLE NSMiniaturizableWindowMask
#define BP_WINDOW_MASK_RESIZABLE      NSResizableWindowMask
#define BP_BEZEL_STYLE_ROUNDED        NSRoundedBezelStyle
#define BP_TEXT_ALIGNMENT_CENTER      NSCenterTextAlignment
#define BP_MODAL_RESPONSE_OK          NSOKButton
#define BP_TO_OBJC(type, value)       ((type)(value))
#define BP_TO_VOID(value)             ((void *)(value))
#else
#define BP_PROPERTY_RETAIN strong
#define BP_UNSAFE_UNRETAINED __unsafe_unretained
#define BP_INIT_RESULT                instancetype
#define BP_WINDOW_MASK_TITLED         NSWindowStyleMaskTitled
#define BP_WINDOW_MASK_CLOSABLE       NSWindowStyleMaskClosable
#define BP_WINDOW_MASK_MINIATURIZABLE NSWindowStyleMaskMiniaturizable
#define BP_WINDOW_MASK_RESIZABLE      NSWindowStyleMaskResizable
#define BP_BEZEL_STYLE_ROUNDED        NSBezelStyleRounded
#define BP_TEXT_ALIGNMENT_CENTER      NSTextAlignmentCenter
#define BP_MODAL_RESPONSE_OK          NSModalResponseOK
#define BP_TO_OBJC(type, value)       ((__bridge type)(value))
#define BP_TO_VOID(value)             ((__bridge void *)(value))
#endif

/* ====================================================================== */
/*  File I/O adapter (same pattern as CLI frontend)                       */
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
    return fseeko((FILE *)h, (off_t)offset, origin);
}
static bp_off_t file_tell(void *h)
{
    return (bp_off_t)ftello((FILE *)h);
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

static double u64_to_double(bp_uint64 v)
{
    return (double)v.lo + (double)v.hi * 4294967296.0;
}

static const char *bp_strerror_local(int code)
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
    case BP_ERR_UNSUPPORTED_METHOD: return "unsupported compression method";
    case BP_ERR_COMPRESS:           return "compression failed";
    case BP_ERR_DECOMPRESS:         return "decompression failed";
    case BP_ERR_ABORTED:            return "operation aborted";
    case BP_ERR_PARTIAL:             return "some files or attributes were not restored";
    case BP_ERR_NOT_FOUND:          return "entry not found";
    case BP_ERR_INVALID_ARG:        return "invalid argument";
    default:                        return "unknown error";
    }
}

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

#define BP_GUI_COL_COUNT 6

static int g_compression_method = BP_METHOD_BOLTLZ;
static int g_compression_level  = 1;
static int g_fluff_multithread  = 0;
static int g_interchange_profile = 0;

static const CGFloat s_default_col_widths[BP_GUI_COL_COUNT] = {
    240.0, 90.0, 90.0, 55.0, 70.0, 130.0
};

static const int s_method_default_level[] = { 1, 1, 1, 2 };

static NSString * const kPrefsMethod      = @"CompressionMethod";
static NSString * const kPrefsLevel       = @"CompressionLevel";
static NSString * const kPrefsFluffMT     = @"FluffMultithread";
static NSString * const kPrefsInterchange = @"InterchangeProfile1";
static NSString * const kPrefsWindowFrame = @"WindowFrame";
static NSString * const kPrefsColumnWidths = @"ColumnWidths";

static bp_uint32 level_to_block_size(int method, int level)
{
    if (method == BP_METHOD_BOLTLITE) {
        if (level <= 0) return BP_BLOCK_8K;
        if (level == 1) return BP_BLOCK_16K;
        return BP_BLOCK_32K;
    }
    if (method == BP_METHOD_FLUFF)
        return BP_BLOCK_256K;

    if (level <= 0) return BP_BLOCK_16K;
    if (level == 1) return BP_BLOCK_32K;
    return BP_BLOCK_64K;
}

static int fluff_gui_level(int level)
{
    if (level <= 0) return 3;
    if (level == 1) return 6;
    return 9;
}

#ifdef BOLTPACK_FAST_FLUFF
static int fast_fluff_thread_count(void)
{
    long nthreads = sysconf(_SC_NPROCESSORS_ONLN);

    if (nthreads < 0)
        return 2;
    if (nthreads <= 1)
        return 0;
    if (nthreads > (long)INT_MAX)
        return INT_MAX;
    return (int)nthreads;
}
#endif

static void load_compression_prefs(void)
{
    NSUserDefaults *defs = [NSUserDefaults standardUserDefaults];
    NSNumber *method = [defs objectForKey:kPrefsMethod];
    NSNumber *level = [defs objectForKey:kPrefsLevel];
    NSNumber *fluffMT = [defs objectForKey:kPrefsFluffMT];
    NSNumber *interchange = [defs objectForKey:kPrefsInterchange];

    g_compression_method = BP_METHOD_BOLTLZ;
    g_compression_level = 1;
    g_fluff_multithread = 0;
    g_interchange_profile = 0;

    if (method) {
        NSInteger v = [method integerValue];
        if (v >= BP_METHOD_STORED && v <= BP_METHOD_FLUFF)
            g_compression_method = (int)v;
    }

    if (level) {
        NSInteger v = [level integerValue];
        if (v >= 0 && v <= 2)
            g_compression_level = (int)v;
    }

    if (fluffMT) {
        NSInteger v = [fluffMT integerValue];
        g_fluff_multithread = (v != 0) ? 1 : 0;
    }
    if (interchange) {
        NSInteger v = [interchange integerValue];
        g_interchange_profile = (v != 0) ? 1 : 0;
    }
}

static void save_compression_prefs(void)
{
    NSUserDefaults *defs = [NSUserDefaults standardUserDefaults];
    [defs setInteger:g_compression_method forKey:kPrefsMethod];
    [defs setInteger:g_compression_level forKey:kPrefsLevel];
    [defs setInteger:g_fluff_multithread forKey:kPrefsFluffMT];
    [defs setInteger:g_interchange_profile forKey:kPrefsInterchange];
}

static void bp_begin_progress_window(NSWindow *parent, NSWindow *child)
{
#if BP_LEGACY_COCOA
    [NSApp beginSheet:child modalForWindow:parent modalDelegate:nil
       didEndSelector:NULL contextInfo:NULL];
#else
    [parent beginSheet:child completionHandler:^(NSModalResponse response) {
        (void)response;
    }];
#endif
}

static void bp_end_progress_window(NSWindow *parent, NSWindow *child)
{
#if BP_LEGACY_COCOA
    (void)parent;
    [NSApp endSheet:child];
#else
    [parent endSheet:child];
#endif
    [child orderOut:nil];
}

static void bp_set_panel_directory(id panel, NSString *dir)
{
#if BP_LEGACY_COCOA
    SEL sel = @selector(setDirectory:);
    if ([panel respondsToSelector:sel])
        [panel performSelector:sel withObject:dir];
#else
    [panel setDirectoryURL:[NSURL fileURLWithPath:dir]];
#endif
}

static NSString *bp_open_panel_first_path(NSOpenPanel *panel)
{
#if BP_LEGACY_COCOA
    NSArray *paths = [panel filenames];
    if (!paths || [paths count] == 0)
        return nil;
    return [paths objectAtIndex:0];
#else
    return [[panel URL] path];
#endif
}

static NSArray *bp_open_panel_paths(NSOpenPanel *panel)
{
#if BP_LEGACY_COCOA
    NSArray *paths = [panel filenames];
    return paths ? paths : [NSArray array];
#else
    NSMutableArray *paths = [NSMutableArray array];
    NSURL *url;

    for (url in [panel URLs])
        [paths addObject:[url path]];
    return paths;
#endif
}

static NSString *bp_save_panel_path(NSSavePanel *panel)
{
#if BP_LEGACY_COCOA
    return [panel filename];
#else
    return [[panel URL] path];
#endif
}

static void bp_set_save_panel_name(NSSavePanel *panel, NSString *name)
{
#if BP_LEGACY_COCOA
    SEL sel = @selector(setNameFieldStringValue:);
    if ([panel respondsToSelector:sel])
        [panel performSelector:sel withObject:name];
#else
    [panel setNameFieldStringValue:name];
#endif
}

static int bp_running_from_app_bundle(void)
{
    NSString *bundlePath = [[NSBundle mainBundle] bundlePath];
    if (!bundlePath || [bundlePath length] == 0)
        return 0;
    return ([[bundlePath pathExtension] caseInsensitiveCompare:@"app"]
            == NSOrderedSame);
}

static void bp_activate_application(void)
{
    [NSApp activateIgnoringOtherApps:YES];
}

static NSMenuItem *bp_add_targeted_menu_item(NSMenu *menu,
                                             NSString *title,
                                             SEL action,
                                             NSString *keyEquivalent,
                                             id target)
{
    NSMenuItem *item;

    item = [menu addItemWithTitle:title
                           action:action
                    keyEquivalent:keyEquivalent];
    [item setTarget:target];
    return item;
}

/* ====================================================================== */
/*  Development logging                                                   */
/* ====================================================================== */

#if BP_GUI_DEV_LOGGING

static FILE *g_bp_gui_dev_log_fp = NULL;
static char  g_bp_gui_dev_log_path[4096];

static void bp_gui_dev_make_dirs(const char *path)
{
    char tmp[4096];
    char *p;

    if (path == NULL || path[0] == '\0')
        return;

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

static const char *bp_gui_dev_log_filename(void)
{
#if BP_LEGACY_COCOA
    return "boltpack-legacy-gui-dev.log";
#else
    return "boltpack-gui-dev.log";
#endif
}

static void bp_gui_dev_log(const char *fmt, ...)
{
    va_list ap;
    time_t now;
    struct tm *tm_info;
    char ts[32];

    if (g_bp_gui_dev_log_fp == NULL)
        return;

    ts[0] = '\0';
    now = time(NULL);
    if (now != (time_t)-1) {
        tm_info = localtime(&now);
        if (tm_info != NULL)
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);
    }

    if (ts[0] != '\0')
        fprintf(g_bp_gui_dev_log_fp, "[%s] ", ts);

    va_start(ap, fmt);
    vfprintf(g_bp_gui_dev_log_fp, fmt, ap);
    va_end(ap);

    fputc('\n', g_bp_gui_dev_log_fp);
    fflush(g_bp_gui_dev_log_fp);
}

static void bp_gui_dev_open_log(void)
{
    NSArray *paths;
    NSString *library_path = nil;
    NSString *log_dir = nil;
    NSString *log_path = nil;
    NSString *filename;
    NSString *temp_dir;
    const char *fs_dir;
    const char *fs_path;

    if (g_bp_gui_dev_log_fp != NULL)
        return;

    filename = [NSString stringWithUTF8String:bp_gui_dev_log_filename()];
    paths = NSSearchPathForDirectoriesInDomains(NSLibraryDirectory,
                                                NSUserDomainMask,
                                                YES);
    if (paths != nil && [paths count] > 0)
        library_path = [paths objectAtIndex:0];

    if (library_path != nil) {
        log_dir = [library_path stringByAppendingPathComponent:@"Logs/BoltPack"];
        fs_dir = [log_dir fileSystemRepresentation];
        if (fs_dir != NULL)
            bp_gui_dev_make_dirs(fs_dir);
        log_path = [log_dir stringByAppendingPathComponent:filename];
    }

    if (log_path == nil) {
        temp_dir = NSTemporaryDirectory();
        if (temp_dir != nil && [temp_dir length] > 0)
            log_path = [temp_dir stringByAppendingPathComponent:filename];
        else
            log_path = filename;
    }

    fs_path = [log_path fileSystemRepresentation];
    if (fs_path == NULL)
        return;

    g_bp_gui_dev_log_fp = fopen(fs_path, "a");
    if (g_bp_gui_dev_log_fp == NULL)
        return;

    setvbuf(g_bp_gui_dev_log_fp, NULL, _IONBF, 0);
    strncpy(g_bp_gui_dev_log_path, fs_path, sizeof(g_bp_gui_dev_log_path) - 1);
    g_bp_gui_dev_log_path[sizeof(g_bp_gui_dev_log_path) - 1] = '\0';
    bp_gui_dev_log("development logging enabled: %s", g_bp_gui_dev_log_path);
}

static void bp_gui_dev_close_log(void)
{
    if (g_bp_gui_dev_log_fp != NULL) {
        fclose(g_bp_gui_dev_log_fp);
        g_bp_gui_dev_log_fp = NULL;
    }
}

static const char *bp_gui_dev_path(NSString *path)
{
    const char *result;

    if (path == nil)
        return "(nil)";

    result = [path fileSystemRepresentation];
    if (result != NULL)
        return result;

    result = [path UTF8String];
    return result != NULL ? result : "(unavailable)";
}

#else

static void bp_gui_dev_log(const char *fmt, ...)
{
    (void)fmt;
}

static void bp_gui_dev_open_log(void)
{
}

static void bp_gui_dev_close_log(void)
{
}

static const char *bp_gui_dev_path(NSString *path)
{
    (void)path;
    return "(logging disabled)";
}

#endif

/* ====================================================================== */
/*  Forward declarations                                                  */
/* ====================================================================== */

@class BPMainWindow;
@class BPProgressPanel;

/* ====================================================================== */
/*  Application delegate                                                  */
/* ====================================================================== */

@interface BPAppDelegate : NSObject
{
    BPMainWindow *_mainWindow;
}
@property (BP_PROPERTY_RETAIN) BPMainWindow *mainWindow;
@end

/* ====================================================================== */
/*  Progress sheet / panel                                                */
/* ====================================================================== */

@interface BPProgressPanel : NSWindow
{
    NSProgressIndicator *_bar;
    NSTextField *_fileLabel;
    NSTextField *_countLabel;
    BOOL _cancelled;
}
@property (BP_PROPERTY_RETAIN) NSProgressIndicator *bar;
@property (BP_PROPERTY_RETAIN) NSTextField *fileLabel;
@property (BP_PROPERTY_RETAIN) NSTextField *countLabel;
@property (assign) BOOL cancelled;
- (void)updateFile:(NSString *)name percent:(int)pct
           current:(int)cur total:(int)tot;
- (void)updateFile:(NSString *)name percent:(int)pct
    statusText:(NSString *)statusText;
@end

@implementation BPProgressPanel

@synthesize bar = _bar;
@synthesize fileLabel = _fileLabel;
@synthesize countLabel = _countLabel;
@synthesize cancelled = _cancelled;

- (BP_INIT_RESULT)init
{
    NSRect frame = NSMakeRect(0, 0, 420, 120);
    self = [super initWithContentRect:frame
                                                        styleMask:BP_WINDOW_MASK_TITLED
                              backing:NSBackingStoreBuffered
                                defer:NO];
    if (!self) return nil;

    [self setTitle:@"Progress"];
    [self center];

    _fileLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 84, 380, 18)];
    [_fileLabel setBezeled:NO];
    [_fileLabel setDrawsBackground:NO];
    [_fileLabel setEditable:NO];
    [_fileLabel setSelectable:NO];
    [[self contentView] addSubview:_fileLabel];

    _bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 56, 380, 20)];
    [_bar setIndeterminate:NO];
    [_bar setMinValue:0];
    [_bar setMaxValue:100];
    [[self contentView] addSubview:_bar];

    _countLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 30, 380, 18)];
    [_countLabel setBezeled:NO];
    [_countLabel setDrawsBackground:NO];
    [_countLabel setEditable:NO];
    [_countLabel setSelectable:NO];
    [[self contentView] addSubview:_countLabel];

    NSButton *cancelBtn = [[NSButton alloc] initWithFrame:NSMakeRect(320, 8, 80, 28)];
    [cancelBtn setTitle:@"Cancel"];
    [cancelBtn setBezelStyle:BP_BEZEL_STYLE_ROUNDED];
    [cancelBtn setTarget:self];
    [cancelBtn setAction:@selector(cancelClicked:)];
    [[self contentView] addSubview:cancelBtn];

    _cancelled = NO;
    return self;
}

#if BP_LEGACY_COCOA
- (void)dealloc
{
    [_bar release];
    [_fileLabel release];
    [_countLabel release];
    [super dealloc];
}
#endif

- (void)cancelClicked:(id)sender
{
    (void)sender;
    _cancelled = YES;
}

- (void)updateFile:(NSString *)name percent:(int)pct
           current:(int)cur total:(int)tot
{
    [self updateFile:name percent:pct
          statusText:[NSString stringWithFormat:@"File %d of %d", cur, tot]];
}

- (void)updateFile:(NSString *)name percent:(int)pct
        statusText:(NSString *)statusText
{
    [_fileLabel setStringValue:name ? name : @""];
    [_bar setDoubleValue:(double)pct];
    [_countLabel setStringValue:statusText ? statusText : @""];
    [self display]; /* flush to screen */
}

@end

/* ====================================================================== */
/*  Main window                                                           */
/* ====================================================================== */

#if BP_LEGACY_COCOA
@interface BPMainWindow : NSWindow
#else
@interface BPMainWindow : NSWindow <NSDraggingDestination, NSTableViewDataSource,
                                    NSTableViewDelegate, NSWindowDelegate>
#endif
{
    NSTableView   *_tableView;
    NSTextField   *_welcomeLabel;
    NSScrollView  *_scrollView;
    NSMutableArray *_entries;      /* array of NSDictionary per entry */
    FILE          *_archiveFp;
    NSString      *_archivePath;
    bp_archive_info_t _archiveInfo;
}
- (void)doOpenPath:(NSString *)path;
- (void)doExtract;
- (void)doExtractSelected;
- (void)doValidate;
- (void)doInfo;
- (void)doClose;
- (void)closeArchive;
- (void)doCreateFromPaths:(NSArray *)sourcePaths;
- (void)classifyAndAct:(NSArray *)paths;
- (void)layoutWelcomeLabel;
- (void)resetGUISettings;
- (BOOL)showCompressionDialog;
+ (NSString *)formatSize:(double)bytes;
@end

/* Table data source */
@interface BPMainWindow (DataSource)
@end

/* ====================================================================== */
/*  Extract callbacks (C static functions)                                */
/* ====================================================================== */

typedef struct {
    bp_io_t           io;             /* MUST be first */
    char              path[4096];
    char              temporary[4096];
} cocoa_extract_output;

typedef struct {
    char      *path;
    bp_uint64  mtime;
    int        mtime_valid;
} cocoa_dir_restore;

typedef struct {
    const char      *dest;
    BPProgressPanel *panel;
    BP_UNSAFE_UNRETAINED NSArray *selected_names;
    BP_UNSAFE_UNRETAINED NSMutableSet *flat_output_paths;
    int               extract_flat;
    int               open_error;
    unsigned long     partial_count;
    cocoa_dir_restore *directories;
    unsigned long     directory_count;
    double            selected_total_bytes;
    double            selected_done_bytes;
    unsigned long     selected_file_total;
    unsigned long     selected_files_done;
    unsigned long     extracted_file_count;
} cocoa_extract_ctx;

enum {
    COCOA_EXTRACT_WITH_PATHS = 0,
    COCOA_EXTRACT_FLAT = 1
};

static int cocoa_safe_makedirs(const char *path)
{
    char tmp[4096];
    char *p;
    size_t n;

    if (path == NULL) { errno = EINVAL; return -1; }
    n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    memcpy(tmp, path, n + 1);
    for (p = tmp + (tmp[0] == '/' ? 1 : 0); ; p++) {
        if (*p == '/' || *p == '\0') {
            char saved = *p;
            struct stat st;
            if (p != tmp && p[-1] != '/') {
                *p = '\0';
                if (lstat(tmp, &st) == 0) {
                    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
                        errno = ENOTDIR;
                        return -1;
                    }
                } else if (errno == ENOENT) {
                    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                        return -1;
                    if (lstat(tmp, &st) != 0 || !S_ISDIR(st.st_mode) ||
                        S_ISLNK(st.st_mode)) {
                        errno = ENOTDIR;
                        return -1;
                    }
                } else {
                    return -1;
                }
                *p = saved;
            }
            if (saved == '\0') break;
        }
    }
    return 0;
}

static int cocoa_safe_parent(const char *path)
{
    char parent[4096];
    char *slash;
    size_t n;
    if (path == NULL) { errno = EINVAL; return -1; }
    n = strlen(path);
    if (n >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
    memcpy(parent, path, n + 1);
    slash = strrchr(parent, '/');
    if (slash == NULL) return 0;
    if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    return cocoa_safe_makedirs(parent);
}

static int cocoa_make_extract_stage(const char *path,
                                    char *temporary, size_t cap)
{
    const char *slash;
    int n;
    if (path == NULL || temporary == NULL) { errno = EINVAL; return -1; }
    slash = strrchr(path, '/');
    if (slash == NULL)
        n = snprintf(temporary, cap, ".boltpack-XXXXXX");
    else if (slash == path)
        n = snprintf(temporary, cap, "/.boltpack-XXXXXX");
    else
        n = snprintf(temporary, cap, "%.*s/.boltpack-XXXXXX",
                     (int)(slash - path), path);
    if (n < 0 || (size_t)n >= cap) { errno = ENAMETOOLONG; return -1; }
    return mkstemp(temporary);
}

static int cocoa_queue_directory(cocoa_extract_ctx *ctx, const char *path,
                                 const bp_entry_t *entry)
{
    cocoa_dir_restore *items;
    cocoa_dir_restore *item;
    size_t n;
    if (ctx->directory_count >= 256) {
        ctx->partial_count++;
        return BP_ERR_PARTIAL;
    }
    items = (cocoa_dir_restore *)realloc(ctx->directories,
        (size_t)(ctx->directory_count + 1) * sizeof(*items));
    if (items == NULL) return BP_ERR_NOMEM;
    ctx->directories = items;
    item = &ctx->directories[ctx->directory_count];
    n = strlen(path);
    item->path = (char *)malloc(n + 1);
    if (item->path == NULL) return BP_ERR_NOMEM;
    memcpy(item->path, path, n + 1);
    item->mtime = entry->mtime;
    item->mtime_valid = (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) != 0;
    ctx->directory_count++;
    return BP_OK;
}

static void cocoa_finish_directories(cocoa_extract_ctx *ctx)
{
    unsigned long done;
    for (done = 0; done < ctx->directory_count; done++) {
        unsigned long i, best = done;
        size_t best_len = strlen(ctx->directories[best].path);
        for (i = done + 1; i < ctx->directory_count; i++) {
            size_t len = strlen(ctx->directories[i].path);
            if (len > best_len) { best = i; best_len = len; }
        }
        if (best != done) {
            cocoa_dir_restore swap = ctx->directories[done];
            ctx->directories[done] = ctx->directories[best];
            ctx->directories[best] = swap;
        }
        if (ctx->directories[done].mtime_valid &&
            bp_restore_mtime(ctx->directories[done].path,
                             ctx->directories[done].mtime) != BP_OK)
            ctx->partial_count++;
    }
}

static void cocoa_destroy_extract_ctx(cocoa_extract_ctx *ctx)
{
    unsigned long i;
    for (i = 0; i < ctx->directory_count; i++)
        free(ctx->directories[i].path);
    free(ctx->directories);
    ctx->directories = NULL;
    ctx->directory_count = 0;
}

static int cocoa_finish_extract(cocoa_extract_ctx *ctx, int rc)
{
    if ((rc == BP_OK || rc == BP_ERR_PARTIAL) &&
        ctx->open_error != BP_OK)
        rc = ctx->open_error;
    if (rc == BP_OK || rc == BP_ERR_PARTIAL)
        cocoa_finish_directories(ctx);
    if (rc == BP_OK && ctx->partial_count != 0)
        rc = BP_ERR_PARTIAL;
    return rc;
}

static int cocoa_name_matches_filters(const char *entry_name, NSArray *filters)
{
    NSUInteger i;
    if (!filters || [filters count] == 0) return 1;
    if (!entry_name) return 0;

    NSString *name = [NSString stringWithUTF8String:entry_name];
    for (i = 0; i < [filters count]; i++) {
        NSString *filter = [filters objectAtIndex:i];
        if ([filter hasSuffix:@"/"]) {
            if ([name hasPrefix:filter]) return 1;
        } else if ([name isEqualToString:filter]) {
            return 1;
        }
    }
    return 0;
}

static int cocoa_entry_writes_file(int entry_type)
{
    return entry_type == BP_TYPE_DATA;
}

static int cocoa_selection_needs_extract_mode_choice(NSArray *names)
{
    NSUInteger i;

    if (!names) return 0;
    for (i = 0; i < [names count]; i++) {
        NSString *name = [names objectAtIndex:i];
        if (!name || [name length] == 0) continue;
        if ([name hasSuffix:@"/"]) return 1;
        if ([name rangeOfString:@"/"].location != NSNotFound) return 1;
    }
    return 0;
}

static const char *cocoa_basename(const char *path)
{
    const char *slash;

    if (path == NULL) return NULL;
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int cocoa_path_exists(const char *path)
{
    struct stat st;
    return path != NULL && lstat(path, &st) == 0;
}

static void cocoa_split_flat_name(const char *name,
                                  char *stem, size_t stem_size,
                                  char *ext, size_t ext_size)
{
    const char *dot;
    size_t stem_len;

    if (stem_size == 0 || ext_size == 0) return;
    stem[0] = '\0';
    ext[0] = '\0';
    if (name == NULL) return;

    dot = strrchr(name, '.');
    if (dot != NULL && dot != name) {
        stem_len = (size_t)(dot - name);
        if (stem_len >= stem_size) stem_len = stem_size - 1;
        memcpy(stem, name, stem_len);
        stem[stem_len] = '\0';
        strncpy(ext, dot, ext_size - 1);
        ext[ext_size - 1] = '\0';
    } else {
        strncpy(stem, name, stem_size - 1);
        stem[stem_size - 1] = '\0';
    }
}

static int cocoa_make_flat_output_path(cocoa_extract_ctx *ctx,
                                       const char *entry_name,
                                       char *fullpath,
                                       size_t fullpath_size)
{
    const char *base_name;
    char stem[512];
    char ext[256];
    unsigned long suffix;

    if (ctx == NULL || entry_name == NULL || fullpath == NULL ||
        fullpath_size == 0)
        return 0;

    base_name = cocoa_basename(entry_name);
    if (base_name == NULL || base_name[0] == '\0') return 0;
    if (strcmp(base_name, ".") == 0 || strcmp(base_name, "..") == 0)
        return 0;

    cocoa_split_flat_name(base_name, stem, sizeof(stem), ext, sizeof(ext));
    if (stem[0] == '\0') {
        strncpy(stem, base_name, sizeof(stem) - 1);
        stem[sizeof(stem) - 1] = '\0';
    }

    suffix = 0;
    for (;;) {
        char candidate[1024];
        NSString *candidate_path;

        if (suffix == 0) {
            snprintf(candidate, sizeof(candidate), "%s%s", stem, ext);
        } else {
            snprintf(candidate, sizeof(candidate), "%s-%02lu%s",
                     stem, suffix, ext);
        }

        {
            int n = snprintf(fullpath, fullpath_size, "%s/%s",
                             ctx->dest, candidate);
            if (n < 0 || (size_t)n >= fullpath_size) return 0;
        }

        candidate_path = [NSString stringWithUTF8String:fullpath];
        if (candidate_path == nil) return 0;
        if (![ctx->flat_output_paths containsObject:candidate_path] &&
            !cocoa_path_exists(fullpath)) {
            return 1;
        }
        suffix++;
    }
}

static void cocoa_selected_extract_stats(NSArray *entries, NSArray *filters,
                                         double *total_bytes,
                                         unsigned long *file_count)
{
    NSUInteger i;

    if (total_bytes) *total_bytes = 0.0;
    if (file_count) *file_count = 0;
    if (entries == nil || filters == nil || [filters count] == 0) return;

    for (i = 0; i < [entries count]; i++) {
        NSDictionary *entry;
        NSString *name;
        NSNumber *type_num;
        NSNumber *orig_num;
        int entry_type;

        entry = [entries objectAtIndex:i];
        name = [entry objectForKey:@"Name"];
        type_num = [entry objectForKey:@"type"];
        orig_num = [entry objectForKey:@"origBytes"];
        if (name == nil || type_num == nil || orig_num == nil) continue;

        entry_type = [type_num intValue];
        if (!cocoa_entry_writes_file(entry_type)) continue;
        if (!cocoa_name_matches_filters([name UTF8String], filters)) continue;

        if (total_bytes) *total_bytes += [orig_num doubleValue];
        if (file_count) (*file_count)++;
    }
}

static bp_io_t *cocoa_extract_open(const bp_entry_t *entry, void *ud)
{
    cocoa_extract_ctx *ctx = (cocoa_extract_ctx *)ud;
    char fullpath[4096];
    NSString *resolved_path;
    FILE *fp;
    cocoa_extract_output *output;
    int fd;

    if (!entry->name) return NULL;
    if (entry->entry_type == BP_TYPE_RSRC ||
        entry->entry_type == BP_TYPE_META) {
        if (cocoa_name_matches_filters(entry->name, ctx->selected_names))
            ctx->partial_count++;
        return NULL;
    }
    if (entry->entry_type == BP_TYPE_SYMLINK) {
        if (cocoa_name_matches_filters(entry->name, ctx->selected_names))
            ctx->partial_count++;
        return NULL;
    }
    if (entry->entry_type != BP_TYPE_DATA &&
        entry->entry_type != BP_TYPE_DIR) {
        if (cocoa_name_matches_filters(entry->name, ctx->selected_names))
            ctx->partial_count++;
        return NULL;
    }
    if (!cocoa_name_matches_filters(entry->name, ctx->selected_names))
        return NULL;

    if (ctx->extract_flat) {
        if (entry->entry_type == BP_TYPE_DIR) return NULL;
        if (!cocoa_make_flat_output_path(ctx, entry->name,
                                         fullpath, sizeof(fullpath))) {
            ctx->open_error = BP_ERR_NAME_TOO_LONG;
            return NULL;
        }
    } else {
        int n = snprintf(fullpath, sizeof(fullpath), "%s/%s",
                         ctx->dest, entry->name);
        if (n < 0 || (size_t)n >= sizeof(fullpath)) {
            ctx->open_error = BP_ERR_NAME_TOO_LONG;
            return NULL;
        }
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        if (ctx->selected_names && [ctx->selected_names count] > 0)
            return NULL;
        if (cocoa_safe_makedirs(fullpath) != 0) {
            ctx->open_error = BP_ERR_IO;
            return NULL;
        }
        {
            int queue_rc = cocoa_queue_directory(ctx, fullpath, entry);
            if (queue_rc != BP_OK) ctx->open_error = queue_rc;
        }
        return NULL;
    }

    if (cocoa_safe_parent(fullpath) != 0) {
        ctx->open_error = BP_ERR_IO;
        return NULL;
    }

    output = (cocoa_extract_output *)calloc(1, sizeof(*output));
    if (output == NULL) {
        ctx->open_error = BP_ERR_NOMEM;
        return NULL;
    }
    if (strlen(fullpath) >= sizeof(output->path)) {
        free(output);
        ctx->open_error = BP_ERR_NAME_TOO_LONG;
        return NULL;
    }
    strcpy(output->path, fullpath);
    fd = cocoa_make_extract_stage(fullpath, output->temporary,
                                  sizeof(output->temporary));
    if (fd < 0) {
        free(output);
        ctx->open_error = BP_ERR_IO;
        return NULL;
    }
    fp = fdopen(fd, "wb");
    if (fp == NULL) {
        close(fd);
        unlink(output->temporary);
        free(output);
        ctx->open_error = BP_ERR_IO;
        return NULL;
    }
    output->io = make_io(fp);

    resolved_path = [NSString stringWithUTF8String:fullpath];
    if (resolved_path != nil) {
        if (ctx->extract_flat && ctx->flat_output_paths) {
            [ctx->flat_output_paths addObject:resolved_path];
        }
    }

    return &output->io;
}

static int cocoa_extract_done(const bp_entry_t *entry, bp_io_t *out,
                              int status, void *ud)
{
    cocoa_extract_ctx *ctx = (cocoa_extract_ctx *)ud;
    cocoa_extract_output *output;
    FILE *fp;
    int rc = BP_OK;
    mode_t mask;

    if (out == NULL) return BP_OK;
    output = (cocoa_extract_output *)out;
    fp = (FILE *)out->handle;
    if (status != BP_OK) {
        if (fp != NULL) (void)fclose(fp);
        if (output->temporary[0] != '\0') (void)unlink(output->temporary);
        free(output);
        return BP_OK;
    }

    if (fp == NULL || fflush(fp) != 0) rc = BP_ERR_IO;
    if (rc == BP_OK) {
        mask = umask(0);
        (void)umask(mask);
        if (fchmod(fileno(fp), (mode_t)(0666 & ~mask)) != 0)
            rc = BP_ERR_IO;
    }
    if (fp != NULL && fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    output->io.handle = NULL;

    if (rc == BP_OK && (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
        bp_restore_mtime(output->temporary, entry->mtime) != BP_OK)
        rc = BP_ERR_PARTIAL;
    if (rc == BP_OK || rc == BP_ERR_PARTIAL) {
        if (rename(output->temporary, output->path) != 0) {
            (void)unlink(output->temporary);
            rc = BP_ERR_IO;
        }
    } else {
        (void)unlink(output->temporary);
    }

    if (rc == BP_OK || rc == BP_ERR_PARTIAL) {
        ctx->extracted_file_count++;
        if (ctx->selected_names &&
            cocoa_name_matches_filters(entry->name, ctx->selected_names)) {
            ctx->selected_done_bytes += u64_to_double(entry->original_size);
            ctx->selected_files_done++;
        }
        if (rc == BP_ERR_PARTIAL) ctx->partial_count++;
    }
    free(output);
    return rc;
}

static int cocoa_extract_progress(const bp_progress_t *p, void *ud)
{
    cocoa_extract_ctx *ctx = (cocoa_extract_ctx *)ud;
    double total;
    double done;
    int pct;
    NSString *fname;
    int current;
    int total_count;

    if (ctx->selected_names && [ctx->selected_names count] > 0) {
        if (!cocoa_name_matches_filters(p->file_name, ctx->selected_names)) {
            [[NSRunLoop currentRunLoop] runUntilDate:
                [NSDate dateWithTimeIntervalSinceNow:0.001]];
            return ctx->panel.cancelled ? 1 : 0;
        }
        total = ctx->selected_total_bytes;
        done = ctx->selected_done_bytes + u64_to_double(p->file_bytes_done);
        current = (int)ctx->selected_files_done + 1;
        total_count = (int)ctx->selected_file_total;
    } else {
        total = p->archive_orig_total.lo
              + (double)p->archive_orig_total.hi * 4294967296.0;
        done = p->archive_orig_done.lo
             + (double)p->archive_orig_done.hi * 4294967296.0
             + p->file_bytes_done.lo
             + (double)p->file_bytes_done.hi * 4294967296.0;
        current = (int)p->file_index;
        total_count = (int)p->file_count;
    }

    pct = total > 0 ? (int)(done / total * 100) : 0;
    if (pct > 100) pct = 100;

    if (ctx->selected_names && total > 0.0 && done >= total &&
        (p->file_index + 1) < p->file_count) {
        pct = 99;
    }

    fname = p->file_name
        ? [NSString stringWithUTF8String:p->file_name] : @"";
    [ctx->panel updateFile:fname percent:pct
                   current:current
                     total:total_count];

    /* Process events to keep the UI responsive */
    [[NSRunLoop currentRunLoop] runUntilDate:
        [NSDate dateWithTimeIntervalSinceNow:0.001]];

    return ctx->panel.cancelled ? 1 : 0;
}

typedef struct {
    BP_UNSAFE_UNRETAINED BPProgressPanel *panel;
    BP_UNSAFE_UNRETAINED NSMutableString *log;
    bp_uint32 file_total;
    bp_uint32 dir_total;
    bp_uint32 entry_total;
    int ok_count;
    int fail_count;
} cocoa_validate_ctx;

static void cocoa_validate_entry(const bp_entry_t *entry, int result, void *ud)
{
    cocoa_validate_ctx *ctx = (cocoa_validate_ctx *)ud;
    const char *name = (entry && entry->name) ? entry->name : "?";

    if (result == BP_OK) {
        [ctx->log appendFormat:@"  OK      %s\n", name];
        ctx->ok_count++;
    } else {
        [ctx->log appendFormat:@"  FAILED  %s  (%s)\n",
                               name, bp_strerror_local(result)];
        ctx->fail_count++;
    }
}

static int cocoa_validate_progress(const bp_progress_t *p, void *ud)
{
    cocoa_validate_ctx *ctx = (cocoa_validate_ctx *)ud;
    double total = u64_to_double(p->archive_orig_total);
    double done = u64_to_double(p->archive_orig_done);
    int pct = total > 0.0 ? (int)(done / total * 100.0) : 0;
    bp_uint32 entry_total = ctx->entry_total ? ctx->entry_total : p->file_count;
    NSString *fname = p->file_name
        ? [NSString stringWithUTF8String:p->file_name] : @"";
    NSString *status = [NSString stringWithFormat:
        @"%u files, %u directories; validating entry %u of %u",
        (unsigned)ctx->file_total,
        (unsigned)ctx->dir_total,
        (unsigned)(p->file_index + 1),
        (unsigned)entry_total];

    if (pct > 100) pct = 100;
    [ctx->panel updateFile:fname percent:pct statusText:status];

    [[NSRunLoop currentRunLoop] runUntilDate:
        [NSDate dateWithTimeIntervalSinceNow:0.001]];

    return ctx->panel.cancelled ? 1 : 0;
}

/* ====================================================================== */
/*  Main window implementation                                            */
/* ====================================================================== */

@implementation BPMainWindow

- (void)applySavedFrame
{
    NSString *frameString = [[NSUserDefaults standardUserDefaults]
        stringForKey:kPrefsWindowFrame];
    if (frameString) {
        NSRect frame = NSRectFromString(frameString);
        if (frame.size.width >= 500 && frame.size.height >= 300) {
            [self setFrame:frame display:NO];
            return;
        }
    }
    [self center];
}

- (void)applySavedColumnWidths
{
    NSArray *widths = [[NSUserDefaults standardUserDefaults]
        arrayForKey:kPrefsColumnWidths];
    NSArray *columns = [_tableView tableColumns];
    NSUInteger count = [columns count] < BP_GUI_COL_COUNT
        ? [columns count] : BP_GUI_COL_COUNT;
    NSUInteger i;

    if (!widths || [widths count] != BP_GUI_COL_COUNT) {
        for (i = 0; i < count; i++) {
            NSTableColumn *col = [columns objectAtIndex:i];
            [col setWidth:s_default_col_widths[i]];
        }
        return;
    }

    for (i = 0; i < count; i++) {
        NSTableColumn *col = [columns objectAtIndex:i];
        CGFloat width = [[widths objectAtIndex:i] doubleValue];
        if (width < 50.0) width = s_default_col_widths[i];
        [col setWidth:width];
    }
}

- (void)updateTableWidth
{
    NSArray *columns = [_tableView tableColumns];
    NSUInteger i;
    CGFloat totalWidth = 0.0;
    CGFloat clipWidth = [[_scrollView contentView] bounds].size.width;
    NSRect frame = [_tableView frame];

    for (i = 0; i < [columns count]; i++) {
        NSTableColumn *col = [columns objectAtIndex:i];
        totalWidth += [col width];
    }

    if (totalWidth < clipWidth)
        totalWidth = clipWidth;

    frame.size.width = totalWidth;
    [_tableView setFrameSize:frame.size];
}

- (void)saveGUISettings
{
    NSUserDefaults *defs = [NSUserDefaults standardUserDefaults];
    NSMutableArray *widths = [NSMutableArray array];
    NSArray *columns = [_tableView tableColumns];
    NSUInteger i;

    for (i = 0; i < [columns count]; i++) {
        NSTableColumn *col = [columns objectAtIndex:i];
        [widths addObject:[NSNumber numberWithDouble:[col width]]];
    }

    [defs setObject:NSStringFromRect([self frame]) forKey:kPrefsWindowFrame];
    [defs setObject:widths forKey:kPrefsColumnWidths];
    save_compression_prefs();
}

- (void)tableColumnDidResize:(NSNotification *)note
{
    (void)note;
    [self updateTableWidth];
    [self saveGUISettings];
}

- (void)windowDidMove:(NSNotification *)note
{
    (void)note;
    [self saveGUISettings];
}

- (void)windowDidResize:(NSNotification *)note
{
    (void)note;
    [self layoutWelcomeLabel];
    [self updateTableWidth];
    [self saveGUISettings];
}

- (void)layoutWelcomeLabel
{
    NSRect bounds = [[self contentView] bounds];
    NSRect frame;
    CGFloat margin = 24.0;
    CGFloat height = 88.0;

    if (bounds.size.width < margin * 2.0)
        margin = 0.0;
    if (height > bounds.size.height)
        height = bounds.size.height;

    frame.size.width = bounds.size.width - margin * 2.0;
    if (frame.size.width < 0.0)
        frame.size.width = bounds.size.width;
    frame.size.height = height;
    frame.origin.x = (bounds.size.width - frame.size.width) / 2.0;
    frame.origin.y = (bounds.size.height - frame.size.height) / 2.0;
    [_welcomeLabel setFrame:frame];
}

- (NSArray *)selectedArchiveNames
{
    NSIndexSet *sel = [_tableView selectedRowIndexes];
    NSMutableArray *names = [NSMutableArray array];

    NSUInteger idx = [sel firstIndex];
    while (idx != NSNotFound) {
        if (idx < (NSUInteger)[_entries count]) {
            NSDictionary *entry = [_entries objectAtIndex:idx];
            NSString *name = [entry objectForKey:@"Name"];
            NSNumber *type_num = [entry objectForKey:@"type"];
            int entry_type = type_num ? [type_num intValue] : BP_TYPE_DATA;

            if (name) {
                if (entry_type == BP_TYPE_DIR && ![name hasSuffix:@"/"])
                    name = [name stringByAppendingString:@"/"];
                [names addObject:name];
            }
        }
        idx = [sel indexGreaterThanIndex:idx];
    }

    return names;
}

- (void)showTextWindowWithTitle:(NSString *)title body:(NSString *)body
{
    NSWindow *w = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 620, 440)
                                    styleMask:(BP_WINDOW_MASK_TITLED |
                                                         BP_WINDOW_MASK_CLOSABLE |
                                                         BP_WINDOW_MASK_RESIZABLE)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [w setTitle:title];
    [w center];

    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:
        [[w contentView] bounds]];
    [sv setHasVerticalScroller:YES];
    [sv setHasHorizontalScroller:YES];
    [sv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    NSTextView *tv = [[NSTextView alloc] initWithFrame:
        [[sv contentView] bounds]];
    [tv setEditable:NO];
    [tv setFont:[NSFont userFixedPitchFontOfSize:11]];
    [tv setString:body ? body : @""];
    [sv setDocumentView:tv];
    [[w contentView] addSubview:sv];

    [w makeKeyAndOrderFront:nil];
}

- (BP_INIT_RESULT)init
{
    NSRect frame = NSMakeRect(0, 0, 720, 480);
    self = [super initWithContentRect:frame
        styleMask:(BP_WINDOW_MASK_TITLED |
                   BP_WINDOW_MASK_CLOSABLE |
                   BP_WINDOW_MASK_MINIATURIZABLE |
                   BP_WINDOW_MASK_RESIZABLE)
        backing:NSBackingStoreBuffered
        defer:NO];
    if (!self) return nil;

    [self setTitle:@"BoltPack"];
    [self setReleasedWhenClosed:NO];
    [self setDelegate:self];

    _entries = [[NSMutableArray alloc] init];
    _archiveFp = NULL;
    _archivePath = nil;
    memset(&_archiveInfo, 0, sizeof(_archiveInfo));

    /* Table view inside a scroll view */
    _scrollView = [[NSScrollView alloc] initWithFrame:
        [[self contentView] bounds]];
    [_scrollView setHasVerticalScroller:YES];
    [_scrollView setHasHorizontalScroller:YES];
    [_scrollView setAutoresizingMask:
        NSViewWidthSizable | NSViewHeightSizable];

    _tableView = [[NSTableView alloc] initWithFrame:
        [[_scrollView contentView] bounds]];
    [_tableView setUsesAlternatingRowBackgroundColors:YES];
    [_tableView setAllowsMultipleSelection:YES];
    [_tableView setDelegate:self];
    [_tableView setAutoresizingMask:NSViewHeightSizable];

    NSArray *titles = [NSArray arrayWithObjects:
        @"Name", @"Original", @"Packed", @"Ratio", @"Method", @"Date", nil];
    NSUInteger i;

    for (i = 0; i < [titles count]; i++) {
        NSTableColumn *col = [[NSTableColumn alloc]
            initWithIdentifier:[titles objectAtIndex:i]];
        [[col headerCell] setStringValue:[titles objectAtIndex:i]];
        [col setWidth:s_default_col_widths[i]];
        [col setMinWidth:50.0];
        [col setResizingMask:NSTableColumnUserResizingMask];
        [_tableView addTableColumn:col];
    }

    [_tableView setDataSource:self];
    [_scrollView setDocumentView:_tableView];
    [_scrollView setHidden:YES];

    /* Welcome label */
    _welcomeLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 100, 88)];
    [_welcomeLabel setStringValue:
        @"BoltPack\n\nDrop a .bolt file here to open it,\n"
        @"or drop files to create an archive."];
    [_welcomeLabel setAlignment:BP_TEXT_ALIGNMENT_CENTER];
    [_welcomeLabel setBezeled:NO];
    [_welcomeLabel setDrawsBackground:NO];
    [_welcomeLabel setEditable:NO];
    [_welcomeLabel setSelectable:NO];
    [_welcomeLabel setFont:[NSFont systemFontOfSize:14]];
    [_welcomeLabel setTextColor:[NSColor grayColor]];
    [[_welcomeLabel cell] setWraps:YES];
    [[_welcomeLabel cell] setScrollable:NO];
    [[_welcomeLabel cell] setLineBreakMode:NSLineBreakByWordWrapping];
    [_welcomeLabel setAutoresizingMask:NSViewWidthSizable];

    [[self contentView] addSubview:_scrollView];
    [[self contentView] addSubview:_welcomeLabel];

    /* Register for drag and drop */
    [self registerForDraggedTypes:
#if BP_LEGACY_COCOA
        [NSArray arrayWithObject:NSFilenamesPboardType]
#else
        [NSArray arrayWithObjects:NSPasteboardTypeFileURL,
                                  NSFilenamesPboardType, nil]
#endif
    ];

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(tableColumnDidResize:)
                                                 name:NSTableViewColumnDidResizeNotification
                                               object:_tableView];

    [self applySavedFrame];
    [self applySavedColumnWidths];
    [self layoutWelcomeLabel];
    [self updateTableWidth];

    return self;
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [self closeArchive];
#if BP_LEGACY_COCOA
    [_entries release];
    [_welcomeLabel release];
    [_scrollView release];
    [_tableView release];
    [super dealloc];
#endif
}

/* --- Drag and drop --- */

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    NSPasteboard *pb = [sender draggingPasteboard];
#if BP_LEGACY_COCOA
    NSArray *files = [pb propertyListForType:NSFilenamesPboardType];
    if (files && [files count] > 0)
        return NSDragOperationCopy;
#else
    if ([pb canReadObjectForClasses:@[[NSURL class]]
                            options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}])
        return NSDragOperationCopy;
#endif
    return NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    NSPasteboard *pb = [sender draggingPasteboard];
#if BP_LEGACY_COCOA
    NSArray *files = [pb propertyListForType:NSFilenamesPboardType];
    if (!files || [files count] == 0) return NO;

    [self classifyAndAct:files];
#else
    NSArray *urls = [pb readObjectsForClasses:@[[NSURL class]]
                                      options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    if (!urls || [urls count] == 0) return NO;

    NSMutableArray *paths = [NSMutableArray array];
    for (NSURL *u in urls)
        [paths addObject:[u path]];

    [self classifyAndAct:paths];
#endif
    return YES;
}

- (void)classifyAndAct:(NSArray *)paths
{
    bp_gui_dev_log("classifyAndAct: %lu input path(s)",
                   (unsigned long)[paths count]);

    if ([paths count] == 1) {
        NSString *firstPath = [paths objectAtIndex:0];
        NSString *ext = [[firstPath pathExtension] lowercaseString];
        if ([ext isEqualToString:@"bolt"] || [ext isEqualToString:@"blt"]) {
            bp_gui_dev_log("classifyAndAct: opening archive %s",
                           bp_gui_dev_path(firstPath));
            [self doOpenPath:firstPath];
            return;
        }
    }
    if (![self showCompressionDialog]) {
        bp_gui_dev_log("classifyAndAct: create cancelled at compression dialog");
        return;
    }
    [self doCreateFromPaths:paths];
}

/* --- Archive operations --- */

- (void)closeArchive
{
    if (_archivePath != nil)
        bp_gui_dev_log("closeArchive: %s", bp_gui_dev_path(_archivePath));

    if (_archiveFp) {
        fclose(_archiveFp);
        _archiveFp = NULL;
    }
#if BP_LEGACY_COCOA
    [_archivePath release];
#endif
    _archivePath = nil;
    memset(&_archiveInfo, 0, sizeof(_archiveInfo));
}

static int list_callback(const bp_entry_t *entry, void *user_data)
{
    NSMutableArray *arr = BP_TO_OBJC(NSMutableArray *, user_data);

    NSString *name = @"(unknown)";
    if (entry->name)
        name = [NSString stringWithUTF8String:entry->name];

    double orig = u64_to_double(entry->original_size);
    double comp = u64_to_double(entry->compressed_size);

    NSString *ratio = @"-";
    if (orig > 0)
        ratio = [NSString stringWithFormat:@"%.0f%%",
                 (comp / orig) * 100.0];

    NSString *method = [NSString stringWithUTF8String:method_name(entry->method)];

    NSString *date = @"-";
    {
        double secs = u64_to_double(entry->mtime);
        if (secs > 0) {
            time_t t = (time_t)secs;
            struct tm *tm = localtime(&t);
            if (tm) {
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm);
                date = [NSString stringWithUTF8String:buf];
            }
        }
    }

    NSDictionary *d = [NSDictionary dictionaryWithObjectsAndKeys:
        name, @"Name",
        [BPMainWindow formatSize:orig], @"Original",
        [BPMainWindow formatSize:comp], @"Packed",
        ratio, @"Ratio",
        method, @"Method",
        date, @"Date",
        [NSNumber numberWithDouble:orig], @"origBytes",
        [NSNumber numberWithInt:(int)entry->entry_type], @"type",
        nil];
    [arr addObject:d];
    return 0;
}

- (void)doOpenPath:(NSString *)path
{
    bp_gui_dev_log("doOpenPath: %s", bp_gui_dev_path(path));

    [self closeArchive];
    [_entries removeAllObjects];

    _archiveFp = fopen([path fileSystemRepresentation], "rb");
    if (!_archiveFp) {
        bp_gui_dev_log("doOpenPath: fopen failed for %s",
                       bp_gui_dev_path(path));
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Cannot open file"];
        [a setInformativeText:path];
        [a runModal];
        return;
    }
    _archivePath = [path copy];

    bp_io_t io = make_io(_archiveFp);
    bp_archive_info_t info;
    memset(&info, 0, sizeof(info));

    int rc = bp_archive_info(&io, &info);
    if (rc != BP_OK) {
        bp_gui_dev_log("doOpenPath: bp_archive_info failed rc=%d (%s)",
                       rc, bp_strerror_local(rc));
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Not a valid BoltPack archive"];
        [a runModal];
        [self closeArchive];
        return;
    }

    _archiveInfo = info;

    fseeko(_archiveFp, 0, SEEK_SET);
    io = make_io(_archiveFp);
    rc = bp_archive_list(&io, list_callback,
                         BP_TO_VOID(_entries));
    if (rc != BP_OK) {
        bp_gui_dev_log("doOpenPath: bp_archive_list failed rc=%d (%s)",
                       rc, bp_strerror_local(rc));
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Error reading archive contents"];
        [a runModal];
        [self closeArchive];
        return;
    }

    [_tableView reloadData];
    [self updateTableWidth];
    [_welcomeLabel setHidden:YES];
    [_scrollView setHidden:NO];
    [self setTitle:[NSString stringWithFormat:@"BoltPack - %@",
                    [path lastPathComponent]]];

    bp_gui_dev_log("doOpenPath: opened entries=%u dirs=%u methods=%s",
                   (unsigned)_archiveInfo.entry_count,
                   (unsigned)_archiveInfo.dir_count,
                   format_methods(_archiveInfo.method_counts));
}

- (void)doExtract
{
    if (!_archiveFp) return;

    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setCanChooseDirectories:YES];
    [panel setCanChooseFiles:NO];
    [panel setCanCreateDirectories:YES];
    [panel setPrompt:@"Extract Here"];
    if (_archivePath) {
        NSString *dir = [_archivePath stringByDeletingLastPathComponent];
        bp_set_panel_directory(panel, dir);
    }

    if ([panel runModal] != BP_MODAL_RESPONSE_OK) {
        bp_gui_dev_log("doExtract: destination selection cancelled");
        return;
    }

    NSString *destDir = bp_open_panel_first_path(panel);
    if (!destDir) return;
    bp_gui_dev_log("doExtract: archive=%s dest=%s",
                   bp_gui_dev_path(_archivePath),
                   bp_gui_dev_path(destDir));
    fseeko(_archiveFp, 0, SEEK_SET);
    bp_io_t io = make_io(_archiveFp);

    BPProgressPanel *prog = [[BPProgressPanel alloc] init];
    bp_begin_progress_window(self, prog);

    cocoa_extract_ctx ctx;
    ctx.dest  = [destDir fileSystemRepresentation];
    ctx.panel = prog;
    ctx.selected_names = nil;
    ctx.flat_output_paths = nil;
    ctx.extract_flat = COCOA_EXTRACT_WITH_PATHS;
    ctx.open_error = BP_OK;
    ctx.partial_count = 0;
    ctx.directories = NULL;
    ctx.directory_count = 0;
    ctx.selected_total_bytes = 0.0;
    ctx.selected_done_bytes = 0.0;
    ctx.selected_file_total = 0;
    ctx.selected_files_done = 0;
    ctx.extracted_file_count = 0;

    int rc = bp_archive_extract_all_ex(&io,
        cocoa_extract_open, cocoa_extract_done,
        cocoa_extract_progress, &ctx);
    rc = cocoa_finish_extract(&ctx, rc);
    cocoa_destroy_extract_ctx(&ctx);

    bp_end_progress_window(self, prog);

    if (rc == BP_ERR_ABORTED) {
        bp_gui_dev_log("doExtract: cancelled after %lu file(s)",
                       ctx.extracted_file_count);
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Extraction cancelled"];
        [a runModal];
    } else if (rc == BP_ERR_PARTIAL) {
        bp_gui_dev_log("doExtract: partial files=%lu items=%lu",
                       ctx.extracted_file_count, ctx.partial_count);
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Extraction partially complete"];
        [a setInformativeText:[NSString stringWithFormat:
            @"%u files were extracted. %u items or attributes could not be restored.",
            (unsigned)ctx.extracted_file_count, (unsigned)ctx.partial_count]];
        [a runModal];
    } else if (rc != BP_OK) {
        bp_gui_dev_log("doExtract: failed rc=%d (%s)",
                       rc, bp_strerror_local(rc));
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Extraction error"];
        [a setInformativeText:
            [NSString stringWithFormat:@"%s", bp_strerror_local(rc)]];
        [a runModal];
    } else {
        bp_gui_dev_log("doExtract: complete files=%lu",
                       ctx.extracted_file_count);
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Extraction complete"];
        [a setInformativeText:[NSString stringWithFormat:@"%u files extracted.",
                               (unsigned)ctx.extracted_file_count]];
        [a runModal];
    }
}

- (void)doExtractSelected
{
    if (!_archiveFp || [_tableView numberOfSelectedRows] <= 0) return;

    NSArray *selectedNames = [self selectedArchiveNames];
    NSInteger choice = NSAlertFirstButtonReturn;
    int extract_mode = COCOA_EXTRACT_WITH_PATHS;
    double selectedBytes;
    unsigned long selectedFiles;

    if (!selectedNames || [selectedNames count] == 0) return;

    if (cocoa_selection_needs_extract_mode_choice(selectedNames)) {
        NSAlert *modeChoice = [[NSAlert alloc] init];

        [modeChoice setMessageText:
            @"Selected entries include folders or files inside folders."];
        [modeChoice setInformativeText:
            @"Keep folders preserves the selected folder structure. Files only extracts selected files into one folder and renames duplicates as -01, -02, and so on."];
        [modeChoice addButtonWithTitle:@"Keep folders"];
        [modeChoice addButtonWithTitle:@"Files only"];
        [modeChoice addButtonWithTitle:@"Cancel"];

        choice = [modeChoice runModal];
        if (choice != NSAlertFirstButtonReturn &&
            choice != NSAlertSecondButtonReturn) {
            bp_gui_dev_log("doExtractSelected: extract mode selection cancelled");
            return;
        }

        if (choice == NSAlertSecondButtonReturn)
            extract_mode = COCOA_EXTRACT_FLAT;
    }

    NSOpenPanel *panel = [NSOpenPanel openPanel];
    [panel setCanChooseDirectories:YES];
    [panel setCanChooseFiles:NO];
    [panel setCanCreateDirectories:YES];
    [panel setPrompt:@"Extract Here"];

    if ([panel runModal] != BP_MODAL_RESPONSE_OK) {
        bp_gui_dev_log("doExtractSelected: destination selection cancelled");
        return;
    }

    NSString *destDir = bp_open_panel_first_path(panel);
    if (!destDir) return;

    bp_gui_dev_log("doExtractSelected: archive=%s selected=%lu mode=%s dest=%s",
                   bp_gui_dev_path(_archivePath),
                   (unsigned long)[selectedNames count],
                   extract_mode == COCOA_EXTRACT_FLAT ? "flat" : "paths",
                   bp_gui_dev_path(destDir));

    selectedBytes = 0.0;
    selectedFiles = 0;
    cocoa_selected_extract_stats(_entries, selectedNames,
                                 &selectedBytes, &selectedFiles);

    fseeko(_archiveFp, 0, SEEK_SET);
    bp_io_t io = make_io(_archiveFp);

    BPProgressPanel *prog = [[BPProgressPanel alloc] init];
    bp_begin_progress_window(self, prog);

    cocoa_extract_ctx ctx;
    ctx.dest = [destDir fileSystemRepresentation];
    ctx.panel = prog;
    ctx.selected_names = selectedNames;
    ctx.flat_output_paths = [NSMutableSet set];
    ctx.extract_flat = extract_mode;
    ctx.open_error = BP_OK;
    ctx.partial_count = 0;
    ctx.directories = NULL;
    ctx.directory_count = 0;
    ctx.selected_total_bytes = selectedBytes;
    ctx.selected_done_bytes = 0.0;
    ctx.selected_file_total = selectedFiles;
    ctx.selected_files_done = 0;
    ctx.extracted_file_count = 0;

    int rc = bp_archive_extract_all_ex(&io,
        cocoa_extract_open, cocoa_extract_done,
        cocoa_extract_progress, &ctx);
    rc = cocoa_finish_extract(&ctx, rc);
    cocoa_destroy_extract_ctx(&ctx);

    bp_end_progress_window(self, prog);

    if (rc == BP_ERR_ABORTED) {
        bp_gui_dev_log("doExtractSelected: cancelled after %lu file(s)",
                       ctx.extracted_file_count);
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Extraction cancelled"];
        [a runModal];
    } else if (rc == BP_ERR_PARTIAL) {
        bp_gui_dev_log("doExtractSelected: partial files=%lu items=%lu",
                       ctx.extracted_file_count, ctx.partial_count);
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Extraction partially complete"];
        [a setInformativeText:[NSString stringWithFormat:
            @"%u selected files were extracted. %u items or attributes could not be restored.",
            (unsigned)ctx.extracted_file_count, (unsigned)ctx.partial_count]];
        [a runModal];
    } else if (rc != BP_OK) {
        bp_gui_dev_log("doExtractSelected: failed rc=%d (%s)",
                       rc, bp_strerror_local(rc));
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Extraction error"];
        [a setInformativeText:
            [NSString stringWithFormat:@"%s", bp_strerror_local(rc)]];
        [a runModal];
    } else {
        bp_gui_dev_log("doExtractSelected: complete files=%lu",
                       ctx.extracted_file_count);
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Extraction complete"];
        [a setInformativeText:[NSString stringWithFormat:
            @"%u selected file%@ extracted.",
            (unsigned)ctx.extracted_file_count,
            ctx.extracted_file_count == 1 ? @"" : @"s"]];
        [a runModal];
    }
}

- (void)doValidate
{
    if (!_archiveFp) return;

    bp_gui_dev_log("doValidate: archive=%s entries=%u",
                   bp_gui_dev_path(_archivePath),
                   (unsigned)_archiveInfo.entry_count);

    fseeko(_archiveFp, 0, SEEK_SET);
    bp_io_t io = make_io(_archiveFp);

    BPProgressPanel *prog = [[BPProgressPanel alloc] init];
    [prog setTitle:@"Validating archive..."];
    [prog updateFile:@"Preparing validation..."
             percent:0
          statusText:[NSString stringWithFormat:
            @"%u files, %u directories; preparing validation...",
            (unsigned)(_archiveInfo.entry_count - _archiveInfo.dir_count),
            (unsigned)_archiveInfo.dir_count]];
    bp_begin_progress_window(self, prog);

    NSMutableString *log = [[NSMutableString alloc] init];
    cocoa_validate_ctx ctx;
    ctx.panel = prog;
    ctx.log = log;
    ctx.file_total = _archiveInfo.entry_count - _archiveInfo.dir_count;
    ctx.dir_total = _archiveInfo.dir_count;
    ctx.entry_total = _archiveInfo.entry_count;
    ctx.ok_count = 0;
    ctx.fail_count = 0;

    int rc = bp_archive_test_progress(&io, cocoa_validate_entry, &ctx,
                                      cocoa_validate_progress, &ctx);

    bp_end_progress_window(self, prog);

    if (rc == BP_ERR_ABORTED) {
        bp_gui_dev_log("doValidate: cancelled ok=%d fail=%d",
                       ctx.ok_count, ctx.fail_count);
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Validation cancelled"];
        [a runModal];
        return;
    }

    if (rc != BP_OK && rc != BP_ERR_CHECKSUM) {
        bp_gui_dev_log("doValidate: failed rc=%d (%s) ok=%d fail=%d",
                       rc, bp_strerror_local(rc),
                       ctx.ok_count, ctx.fail_count);
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Validation error"];
        [a setInformativeText:
            [NSString stringWithFormat:@"%s", bp_strerror_local(rc)]];
        [a runModal];
        return;
    }

    NSString *summary;
    if (ctx.fail_count == 0) {
        summary = [NSString stringWithFormat:@"%d entr%@ tested; all OK",
                   ctx.ok_count, ctx.ok_count == 1 ? @"y" : @"ies"];
    } else {
        summary = [NSString stringWithFormat:@"%d OK, %d FAILED",
                   ctx.ok_count, ctx.fail_count];
    }

    bp_gui_dev_log("doValidate: complete rc=%d ok=%d fail=%d",
                   rc, ctx.ok_count, ctx.fail_count);

    [self showTextWindowWithTitle:@"Validate Archive"
                             body:[NSString stringWithFormat:@"%@\n\n%@",
                                  summary, log]];
}

- (void)doInfo
{
    if (!_archiveFp) return;

    bp_uint32 fileCount = _archiveInfo.entry_count - _archiveInfo.dir_count;
    double orig = u64_to_double(_archiveInfo.total_original);
    double comp = u64_to_double(_archiveInfo.total_compressed);
    double arc = u64_to_double(_archiveInfo.archive_size);
    int pct = orig > 0.0 ? (int)(comp / orig * 100.0) : 0;

    NSString *text = [NSString stringWithFormat:
        @"Archive:         %@\n"
         "Format version:  %u\n"
         "Created by:      BoltPack %u.%u\n"
         "Creator OS:      %s\n"
         "Creator CPU:     %s\n"
         "Method:          %s\n"
         "\n"
         "Archive size:    %@\n"
         "Entries:         %u  (%u file%@, %u director%@)\n"
         "Original size:   %@\n"
         "Compressed:      %@  (%d%% of original)",
        _archivePath ? _archivePath : @"",
        (unsigned)_archiveInfo.version,
        (unsigned)_archiveInfo.creator_ver_major,
        (unsigned)_archiveInfo.creator_ver_minor,
        os_name(_archiveInfo.creator_os),
        cpu_name(_archiveInfo.creator_cpu),
        format_methods(_archiveInfo.method_counts),
        [BPMainWindow formatSize:arc],
        (unsigned)_archiveInfo.entry_count,
        (unsigned)fileCount, fileCount == 1 ? @"" : @"s",
        (unsigned)_archiveInfo.dir_count,
        _archiveInfo.dir_count == 1 ? @"y" : @"ies",
        [BPMainWindow formatSize:orig],
        [BPMainWindow formatSize:comp],
        pct];

    [self showTextWindowWithTitle:@"Archive Information" body:text];
}

typedef struct {
    BP_UNSAFE_UNRETAINED BPProgressPanel *panel;
    double total_bytes;
    unsigned long total_files;
    unsigned long current_file;
} cocoa_create_ctx;

static void cocoa_count_input_recursive(const char *path, double *bytes,
                                        unsigned long *files)
{
    struct stat st;
    if (stat(path, &st) != 0) return;

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *ent;
        if (!d) return;
        while ((ent = readdir(d)) != NULL) {
            char full[4096];
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
            cocoa_count_input_recursive(full, bytes, files);
        }
        closedir(d);
    } else {
        *bytes += (double)st.st_size;
        (*files)++;
    }
}

static int cocoa_update_create_panel(cocoa_create_ctx *ctx,
                                     const char *file_name,
                                     double done_bytes)
{
    double total;
    int pct;
    unsigned long current;
    unsigned long total_files;
    NSString *fname;

    if (ctx == NULL || ctx->panel == nil)
        return 0;

    total = ctx->total_bytes;
    if (total > 0.0) {
        pct = (int)((done_bytes / total) * 100.0);
    } else if (ctx->total_files > 0) {
        pct = (int)((ctx->current_file * 100UL) / ctx->total_files);
    } else {
        pct = 0;
    }

    if (pct > 100)
        pct = 100;

    current = ctx->current_file;
    total_files = ctx->total_files;
    fname = file_name ? [NSString stringWithUTF8String:file_name] : @"";

    [ctx->panel updateFile:fname percent:pct
                   current:(int)current
                     total:(int)total_files];

    [[NSRunLoop currentRunLoop] runUntilDate:
        [NSDate dateWithTimeIntervalSinceNow:0.001]];

    return ctx->panel.cancelled ? 1 : 0;
}

static int cocoa_create_progress(const bp_progress_t *p, void *ud)
{
    cocoa_create_ctx *ctx = (cocoa_create_ctx *)ud;
    return cocoa_update_create_panel(ctx, p->file_name,
                                     u64_to_double(p->archive_orig_done));
}

static int cocoa_add_path_to_archive(bp_archive_t *ar, const char *disk_path,
                                     const char *arc_name,
                                     cocoa_create_ctx *ctx);
static int cocoa_add_dir_recursive(bp_archive_t *ar, const char *dirpath,
                                   const char *prefix,
                                   cocoa_create_ctx *ctx);

static int cocoa_add_path_to_archive(bp_archive_t *ar, const char *disk_path,
                                     const char *arc_name,
                                     cocoa_create_ctx *ctx)
{
    struct stat st;
    if (stat(disk_path, &st) != 0) {
        bp_gui_dev_log("cocoa_add_path_to_archive: stat failed path=%s",
                       disk_path ? disk_path : "(null)");
        return BP_ERR_IO;
    }

    if (S_ISDIR(st.st_mode)) {
        char dir_name[4096];
        bp_uint64 mtime;
        int rc;

        snprintf(dir_name, sizeof(dir_name), "%s%s",
                 arc_name,
                 (arc_name[0] != '\0' && arc_name[strlen(arc_name) - 1] == '/')
                    ? "" : "/");
        mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
        rc = bp_archive_add_entry(ar, dir_name, BP_TYPE_DIR, BP_OS_MACOSX,
                                  NULL, bp_u64_from_u32(0), mtime);
        if (rc != BP_OK) {
            bp_gui_dev_log("cocoa_add_path_to_archive: add dir failed arc=%s rc=%d (%s)",
                           dir_name, rc, bp_strerror_local(rc));
            return rc;
        }
        return cocoa_add_dir_recursive(ar, disk_path, dir_name, ctx);
    }

    {
        FILE *fp = fopen(disk_path, "rb");
        bp_io_t fio;
        bp_uint64 fsize;
        bp_uint64 mtime;
        int rc;

        if (!fp) {
            bp_gui_dev_log("cocoa_add_path_to_archive: fopen failed path=%s",
                           disk_path ? disk_path : "(null)");
            return BP_ERR_IO;
        }
        fio = make_io(fp);
        fsize = bp_off_to_u64((bp_off_t)st.st_size);
        mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
        if (ctx != NULL)
            ctx->current_file++;
        rc = bp_archive_add_entry(ar, arc_name, BP_TYPE_DATA, BP_OS_MACOSX,
                                  &fio, fsize, mtime);
        fclose(fp);
        if (rc != BP_OK) {
            bp_gui_dev_log("cocoa_add_path_to_archive: add file failed arc=%s rc=%d (%s)",
                           arc_name ? arc_name : "(null)",
                           rc, bp_strerror_local(rc));
        }
        if (rc == BP_OK && ctx != NULL && bp_u64_is_zero(fsize)) {
            if (cocoa_update_create_panel(ctx, arc_name,
                                          u64_to_double(ar->archive_orig_done)) != 0)
                return BP_ERR_ABORTED;
        }
        return rc;
    }
}

static int cocoa_add_dir_recursive(bp_archive_t *ar, const char *dirpath,
                                   const char *prefix,
                                   cocoa_create_ctx *ctx)
{
    DIR *d = opendir(dirpath);
    struct dirent *ent;
    if (!d) return BP_ERR_IO;

    while ((ent = readdir(d)) != NULL) {
        char full[4096];
        char arc[4096];
        int rc;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        snprintf(full, sizeof(full), "%s/%s", dirpath, ent->d_name);
        snprintf(arc, sizeof(arc), "%s%s", prefix, ent->d_name);

        rc = cocoa_add_path_to_archive(ar, full, arc, ctx);
        if (rc != BP_OK) {
            closedir(d);
            return rc;
        }
    }

    closedir(d);
    return BP_OK;
}

- (void)doCreateFromPaths:(NSArray *)sourcePaths
{
    NSUInteger i;

    bp_gui_dev_log("doCreateFromPaths: %lu input path(s)",
                   (unsigned long)[sourcePaths count]);
    for (i = 0; i < [sourcePaths count]; i++) {
        NSString *sourcePath = [sourcePaths objectAtIndex:i];
        bp_gui_dev_log("doCreateFromPaths: source[%lu]=%s",
                       (unsigned long)i,
                       bp_gui_dev_path(sourcePath));
    }

    /* Ask for save location */
    NSSavePanel *panel = [NSSavePanel savePanel];
#if BP_LEGACY_COCOA
    [panel setRequiredFileType:@"bolt"];
#else
    [panel setAllowedFileTypes:[NSArray arrayWithObject:@"bolt"]];
#endif
    bp_set_save_panel_name(panel, @"archive.bolt");

    if ([panel runModal] != BP_MODAL_RESPONSE_OK) {
        bp_gui_dev_log("doCreateFromPaths: save panel cancelled");
        return;
    }

    NSString *outPath = bp_save_panel_path(panel);
    if (!outPath) return;

    bp_gui_dev_log("doCreateFromPaths: output=%s method=%s level=%d profile=%d",
                   bp_gui_dev_path(outPath),
                   method_name(g_compression_method),
                   g_compression_level,
                   g_interchange_profile);

    FILE *outFp = fopen([outPath fileSystemRepresentation], "wb");
    if (!outFp) {
        bp_gui_dev_log("doCreateFromPaths: cannot open output %s",
                       bp_gui_dev_path(outPath));
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Cannot create output file"];
        [a runModal];
        return;
    }

    bp_io_t outIo = make_io(outFp);
    bp_archive_t ar;
    memset(&ar, 0, sizeof(ar));

    int rc = g_interchange_profile
        ? bp_archive_create_profile(&ar, &outIo, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &outIo,
                            level_to_block_size(g_compression_method,
                                                g_compression_level),
                            (bp_uint8)g_compression_method);
    if (rc != BP_OK) {
        bp_gui_dev_log("doCreateFromPaths: bp_archive_create failed rc=%d (%s)",
                       rc, bp_strerror_local(rc));
        fclose(outFp);
        return;
    }
    /* Fast preset selects Fast; Normal and Best select Balanced. */
    if (!g_interchange_profile) {
        if (g_compression_method == BP_METHOD_BOLTLZ)
            ar.method_level = (bp_uint8)(g_compression_level == 0 ? 1 : 2);
        else
            ar.method_level = (bp_uint8)fluff_gui_level(g_compression_level);
    }

#ifdef BOLTPACK_FAST_FLUFF
    if (!g_interchange_profile &&
        g_compression_method == BP_METHOD_FLUFF && g_fluff_multithread) {
        int nthreads = fast_fluff_thread_count();
        if (nthreads > 1)
            bp_set_fast_fluff(&ar, nthreads);
    } else if (!g_interchange_profile &&
               g_compression_method == BP_METHOD_BOLTLZ) {
        int nthreads = fast_fluff_thread_count();
        if (nthreads > 1)
            bp_set_fast_boltlz(&ar, nthreads);
    }
#endif

    BPProgressPanel *prog = [[BPProgressPanel alloc] init];
    bp_begin_progress_window(self, prog);

    cocoa_create_ctx ctx;
    ctx.panel = prog;
    ctx.total_bytes = 0.0;
    ctx.total_files = 0;
    ctx.current_file = 0;

    for (i = 0; i < [sourcePaths count]; i++) {
        NSString *path = [sourcePaths objectAtIndex:i];
        cocoa_count_input_recursive([path fileSystemRepresentation],
                                    &ctx.total_bytes, &ctx.total_files);
    }

    bp_gui_dev_log("doCreateFromPaths: counted files=%lu bytes=%.0f",
                   ctx.total_files, ctx.total_bytes);

    ar.progress_fn = cocoa_create_progress;
    ar.progress_data = &ctx;

    for (i = 0; i < [sourcePaths count]; i++) {
        NSString *path = [sourcePaths objectAtIndex:i];
        NSString *base = [path lastPathComponent];
        rc = cocoa_add_path_to_archive(&ar,
                                       [path fileSystemRepresentation],
                                       [base UTF8String],
                                       &ctx);
        if (rc != BP_OK || prog.cancelled) break;
    }

    if (!prog.cancelled)
        rc = bp_archive_finalize(&ar);
    else
        rc = BP_ERR_ABORTED;
    bp_archive_close(&ar);
    fclose(outFp);

    bp_end_progress_window(self, prog);

    if (rc == BP_ERR_ABORTED) {
        bp_gui_dev_log("doCreateFromPaths: cancelled after %lu/%lu file(s)",
                       ctx.current_file, ctx.total_files);
        unlink([outPath fileSystemRepresentation]);
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Archive creation cancelled"];
        [a runModal];
    } else if (rc != BP_OK) {
        bp_gui_dev_log("doCreateFromPaths: failed rc=%d (%s)",
                       rc, bp_strerror_local(rc));
        unlink([outPath fileSystemRepresentation]);
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Error creating archive"];
        [a setInformativeText:
            [NSString stringWithFormat:@"%s", bp_strerror_local(rc)]];
        [a runModal];
    } else {
        bp_gui_dev_log("doCreateFromPaths: complete output=%s",
                       bp_gui_dev_path(outPath));
        [self doOpenPath:outPath];
    }
}

+ (void)gatherFiles:(NSString *)dir into:(NSMutableArray *)arr
{
    NSFileManager *fm = [NSFileManager defaultManager];
#if BP_LEGACY_COCOA
    NSArray *items = [fm directoryContentsAtPath:dir];
#else
    NSArray *items = [fm contentsOfDirectoryAtPath:dir error:nil];
#endif
    NSUInteger i;

    for (i = 0; i < [items count]; i++) {
        NSString *item = [items objectAtIndex:i];
        NSString *full = [dir stringByAppendingPathComponent:item];
        BOOL isDir = NO;
        [fm fileExistsAtPath:full isDirectory:&isDir];
        if (isDir)
            [BPMainWindow gatherFiles:full into:arr];
        else
            [arr addObject:full];
    }
}

+ (NSString *)formatSize:(double)bytes
{
    if (bytes < 1024)
        return [NSString stringWithFormat:@"%.0f B", bytes];
    if (bytes < 1024 * 1024)
        return [NSString stringWithFormat:@"%.1f KB", bytes / 1024];
    if (bytes < 1024 * 1024 * 1024)
        return [NSString stringWithFormat:@"%.1f MB",
                bytes / (1024 * 1024)];
    return [NSString stringWithFormat:@"%.1f GB",
            bytes / (1024 * 1024 * 1024)];
}

- (void)doClose
{
    [self closeArchive];
    [_entries removeAllObjects];
    [_tableView reloadData];
    [_scrollView setHidden:YES];
    [_welcomeLabel setHidden:NO];
    [self setTitle:@"BoltPack"];
}

- (void)resetGUISettings
{
    NSArray *columns = [_tableView tableColumns];
    NSUInteger limit = [columns count] < BP_GUI_COL_COUNT
        ? [columns count] : BP_GUI_COL_COUNT;
    NSUInteger i;

    for (i = 0; i < limit; i++) {
        NSTableColumn *col = [columns objectAtIndex:i];
        [col setWidth:s_default_col_widths[i]];
    }

    g_compression_method = BP_METHOD_BOLTLZ;
    g_compression_level = 1;
    g_fluff_multithread = 0;
    g_interchange_profile = 0;

    [self setFrame:NSMakeRect(0, 0, 720, 480) display:YES];
    [self center];
    [self updateTableWidth];
    [self saveGUISettings];
}

- (BOOL)showCompressionDialog
{
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Compression Settings"];
    [alert setInformativeText:@"Choose a compression method and level, or select Interchange Profile 1 for mini-reader compatible output."];
    [alert addButtonWithTitle:@"Continue"];
    [alert addButtonWithTitle:@"Cancel"];

    CGFloat viewHeight = 164.0;
    CGFloat methodLabelY = 136.0;
    CGFloat methodPopupY = 132.0;
    CGFloat levelLabelY = 104.0;
    CGFloat levelPopupY = 100.0;
    CGFloat profileButtonY = 64.0;
    CGFloat noteHeight = 56.0;
    NSButton *profileButton;
#ifdef BOLTPACK_FAST_FLUFF
    NSButton *fastFluffButton;
    viewHeight = 192.0;
    methodLabelY = 164.0;
    methodPopupY = 160.0;
    levelLabelY = 132.0;
    levelPopupY = 128.0;
#endif

    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, viewHeight)];

    NSTextField *methodLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, methodLabelY, 110, 20)];
    [methodLabel setBezeled:NO];
    [methodLabel setDrawsBackground:NO];
    [methodLabel setEditable:NO];
    [methodLabel setSelectable:NO];
    [methodLabel setStringValue:@"Compression Method"];

    NSPopUpButton *methodPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(120, methodPopupY, 190, 26) pullsDown:NO];
    [methodPopup addItemsWithTitles:[NSArray arrayWithObjects:
        @"Stored", @"BoltLite", @"boltlz", @"fluff", nil]];
    [methodPopup selectItemAtIndex:g_compression_method];

    NSTextField *levelLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, levelLabelY, 110, 20)];
    [levelLabel setBezeled:NO];
    [levelLabel setDrawsBackground:NO];
    [levelLabel setEditable:NO];
    [levelLabel setSelectable:NO];
    [levelLabel setStringValue:@"Level"];

    NSPopUpButton *levelPopup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(120, levelPopupY, 190, 26) pullsDown:NO];
    [levelPopup addItemsWithTitles:[NSArray arrayWithObjects:
        @"Fast", @"Normal", @"Best", nil]];
    [levelPopup selectItemAtIndex:g_compression_level];

    NSTextField *noteLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 310, noteHeight)];
    [noteLabel setBezeled:NO];
    [noteLabel setDrawsBackground:NO];
    [noteLabel setEditable:NO];
    [noteLabel setSelectable:NO];
    [noteLabel setFont:[NSFont systemFontOfSize:11]];
    [noteLabel setStringValue:
        @"Stored: no compression, for all systems.  BoltLite: lighter compression for 16-bit systems and up.  boltlz: fast, light compression for 32-bit and 64-bit systems.  fluff: medium compression for 32-bit and 64-bit systems."];
    [[noteLabel cell] setWraps:YES];
    [[noteLabel cell] setScrollable:NO];
    [[noteLabel cell] setLineBreakMode:NSLineBreakByWordWrapping];

#ifdef BOLTPACK_FAST_FLUFF
    fastFluffButton = [[NSButton alloc] initWithFrame:NSMakeRect(0, 94, 310, 22)];
    [fastFluffButton setButtonType:NSSwitchButton];
    [fastFluffButton setTitle:@"Use multi-threaded Fluff (faster, slightly larger archives)"];
    [fastFluffButton setState:g_fluff_multithread ? NSOnState : NSOffState];
#endif

    profileButton = [[NSButton alloc]
        initWithFrame:NSMakeRect(0, profileButtonY, 310, 22)];
    [profileButton setButtonType:NSSwitchButton];
    [profileButton setTitle:@"Interchange Profile 1 (mini-reader compatible)"];
    [profileButton setState:g_interchange_profile ? NSOnState : NSOffState];

    [view addSubview:methodLabel];
    [view addSubview:methodPopup];
    [view addSubview:levelLabel];
    [view addSubview:levelPopup];
#ifdef BOLTPACK_FAST_FLUFF
    [view addSubview:fastFluffButton];
#endif
    [view addSubview:profileButton];
    [view addSubview:noteLabel];
    [alert setAccessoryView:view];

    if ([alert runModal] != NSAlertFirstButtonReturn) {
        bp_gui_dev_log("showCompressionDialog: cancelled");
        return NO;
    }

    g_compression_method = (int)[methodPopup indexOfSelectedItem];
    g_compression_level = (int)[levelPopup indexOfSelectedItem];
    g_interchange_profile = ([profileButton state] == NSOnState) ? 1 : 0;
    if (g_interchange_profile) {
        g_compression_method = BP_METHOD_BOLTLITE;
        g_compression_level = 1;
    }
    if (g_compression_method < BP_METHOD_STORED ||
        g_compression_method > BP_METHOD_FLUFF)
        g_compression_method = BP_METHOD_BOLTLZ;
    if (g_compression_level < 0 || g_compression_level > 2)
        g_compression_level = s_method_default_level[g_compression_method];
#ifdef BOLTPACK_FAST_FLUFF
    g_fluff_multithread = ([fastFluffButton state] == NSOnState) ? 1 : 0;
#endif

    bp_gui_dev_log("showCompressionDialog: method=%s level=%d fast=%d profile=%d",
                   method_name(g_compression_method),
                   g_compression_level,
                   g_fluff_multithread,
                   g_interchange_profile);

    [self saveGUISettings];
    return YES;
}

@end

/* --- Table data source --- */

@implementation BPMainWindow (DataSource)

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv
{
    (void)tv;
    return (NSInteger)[_entries count];
}

- (id)tableView:(NSTableView *)tv
    objectValueForTableColumn:(NSTableColumn *)col
    row:(NSInteger)row
{
    (void)tv;
    NSDictionary *d = [_entries objectAtIndex:(NSUInteger)row];
    return [d objectForKey:[col identifier]];
}

@end

/* ====================================================================== */
/*  Application delegate                                                  */
/* ====================================================================== */

@implementation BPAppDelegate

@synthesize mainWindow = _mainWindow;

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
    (void)note;
    NSString *appName;

    bp_gui_dev_open_log();
    bp_gui_dev_log("startup: legacy=%d bundle=%d",
                   BP_LEGACY_COCOA,
                   bp_running_from_app_bundle());

    load_compression_prefs();
    bp_gui_dev_log("startup prefs: method=%s level=%d",
                   method_name(g_compression_method),
                   g_compression_level);

    if (bp_compress_init() != BP_OK) {
        bp_gui_dev_log("startup: bp_compress_init failed");
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"Failed to initialize compression engine"];
        [a runModal];
        [NSApp terminate:nil];
        return;
    }

    /* Build menu bar */
    NSMenu *menuBar = [[NSMenu alloc] init];
    appName = [[NSProcessInfo processInfo] processName];
    if (!appName || [appName length] == 0)
        appName = @"BoltPack";

    /* Application menu */
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:appName];
    bp_add_targeted_menu_item(appMenu,
                              [NSString stringWithFormat:@"About %@", appName],
                              @selector(showAbout:),
                              @"",
                              self);
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:[NSString stringWithFormat:@"Quit %@", appName]
                       action:@selector(terminate:) keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [menuBar addItem:appItem];

#if BP_LEGACY_COCOA
    if ([NSApp respondsToSelector:@selector(setAppleMenu:)]) {
        [NSApp performSelector:@selector(setAppleMenu:) withObject:appMenu];
    }
#endif

    /* Archive menu */
    NSMenuItem *archItem = [[NSMenuItem alloc] init];
    [archItem setTitle:@"Archive"];
    NSMenu *archMenu = [[NSMenu alloc] initWithTitle:@"Archive"];
    bp_add_targeted_menu_item(archMenu, @"Open...",
                              @selector(openArchive:), @"o", self);
    bp_add_targeted_menu_item(archMenu, @"Extract All",
                              @selector(extractFiles:), @"e", self);
    bp_add_targeted_menu_item(archMenu, @"Extract Selected",
                              @selector(extractSelected:), @"", self);
    bp_add_targeted_menu_item(archMenu, @"Close",
                              @selector(closeArchive:), @"w", self);
    [archMenu addItem:[NSMenuItem separatorItem]];
    bp_add_targeted_menu_item(archMenu, @"Validate",
                              @selector(validateArchive:), @"t", self);
    bp_add_targeted_menu_item(archMenu, @"Information",
                              @selector(showInfo:), @"i", self);
    [archMenu addItem:[NSMenuItem separatorItem]];
    bp_add_targeted_menu_item(archMenu, @"Create New...",
                              @selector(createArchive:), @"n", self);
    [archItem setSubmenu:archMenu];
    [menuBar addItem:archItem];

    /* Settings menu */
    NSMenuItem *settingsItem = [[NSMenuItem alloc] init];
    [settingsItem setTitle:@"Settings"];
    NSMenu *settingsMenu = [[NSMenu alloc] initWithTitle:@"Settings"];
    bp_add_targeted_menu_item(settingsMenu, @"Reset GUI Settings...",
                              @selector(resetGUISettings:), @"", self);
    [settingsItem setSubmenu:settingsMenu];
    [menuBar addItem:settingsItem];

    /* Help menu */
    NSMenuItem *helpItem = [[NSMenuItem alloc] init];
    [helpItem setTitle:@"Help"];
    NSMenu *helpMenu = [[NSMenu alloc] initWithTitle:@"Help"];
    bp_add_targeted_menu_item(helpMenu, @"Documentation",
                              @selector(showDoc:), @"", self);
    [helpItem setSubmenu:helpMenu];
    [menuBar addItem:helpItem];

    [NSApp setMainMenu:menuBar];
    bp_activate_application();

    /* Main window */
    _mainWindow = [[BPMainWindow alloc] init];
    [_mainWindow makeKeyAndOrderFront:nil];
    [_mainWindow orderFrontRegardless];

    bp_gui_dev_log("startup complete: window shown");
}

- (void)applicationWillTerminate:(NSNotification *)note
{
    (void)note;
    bp_gui_dev_log("shutdown");
    bp_gui_dev_close_log();
}

#if BP_LEGACY_COCOA
- (void)dealloc
{
    [_mainWindow release];
    [super dealloc];
}
#endif

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app
{
    (void)app;
    return YES;
}

/* Open file via Finder double-click or "open with" */
- (BOOL)application:(NSApplication *)app openFile:(NSString *)file
{
    (void)app;
    bp_gui_dev_log("application openFile: %s", bp_gui_dev_path(file));
    [_mainWindow doOpenPath:file];
    return YES;
}

/* --- Menu actions --- */

- (void)openArchive:(id)sender
{
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
#if !BP_LEGACY_COCOA
    [panel setAllowedFileTypes:[NSArray arrayWithObjects:@"bolt", @"blt", nil]];
#endif
    if ([panel runModal] == BP_MODAL_RESPONSE_OK) {
        NSString *path = bp_open_panel_first_path(panel);
        if (path)
            [_mainWindow doOpenPath:path];
    }
}

- (void)extractFiles:(id)sender
{
    (void)sender;
    [_mainWindow doExtract];
}

- (void)extractSelected:(id)sender
{
    (void)sender;
    [_mainWindow doExtractSelected];
}

- (void)closeArchive:(id)sender
{
    (void)sender;
    [_mainWindow doClose];
}

- (void)validateArchive:(id)sender
{
    (void)sender;
    [_mainWindow doValidate];
}

- (void)showInfo:(id)sender
{
    (void)sender;
    [_mainWindow doInfo];
}

- (void)createArchive:(id)sender
{
    (void)sender;

    if (![_mainWindow showCompressionDialog])
        return;

    NSAlert *choice = [[NSAlert alloc] init];
    [choice setMessageText:@"What would you like to add to the new archive?"];
    [choice addButtonWithTitle:@"Files"];
    [choice addButtonWithTitle:@"Folder"];
    [choice addButtonWithTitle:@"Cancel"];

    NSInteger result = [choice runModal];
    if (result != NSAlertFirstButtonReturn && result != NSAlertSecondButtonReturn)
        return;

    NSOpenPanel *panel = [NSOpenPanel openPanel];
    if (result == NSAlertFirstButtonReturn) {
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:YES];
        [panel setPrompt:@"Add Files"];
    } else {
        [panel setCanChooseFiles:NO];
        [panel setCanChooseDirectories:YES];
        [panel setAllowsMultipleSelection:NO];
        [panel setPrompt:@"Add Folder"];
    }

    if ([panel runModal] != BP_MODAL_RESPONSE_OK) return;

    NSMutableArray *paths = [NSMutableArray arrayWithArray:bp_open_panel_paths(panel)];

    [_mainWindow doCreateFromPaths:paths];
}

- (void)resetGUISettings:(id)sender
{
    (void)sender;

    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:@"Reset GUI settings to defaults?"];
#ifdef BOLTPACK_FAST_FLUFF
    [a setInformativeText:
        @"This restores the default compression mode, Interchange Profile 1,\nFluff threading, window size and position, and file-list columns."];
#else
    [a setInformativeText:
        @"This restores the default compression mode, Interchange Profile 1,\nwindow size and position, and file-list columns."];
#endif
    [a addButtonWithTitle:@"Reset"];
    [a addButtonWithTitle:@"Cancel"];

    if ([a runModal] == NSAlertFirstButtonReturn)
        [_mainWindow resetGUISettings];
}

- (void)showAbout:(id)sender
{
    (void)sender;
    NSAlert *a = [[NSAlert alloc] init];
    [a setMessageText:@"BoltPack 1.1"];
    [a setInformativeText:
        @"A lightweight file archiver for vintage and modern systems\n\n"
        @"Copyright (C) 2026 Tony Mattsson\n"
        @"Released under the BSD 2-Clause License"];
    [a runModal];
}

- (void)showDoc:(id)sender
{
    (void)sender;
    NSWindow *w = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 500, 400)
                                    styleMask:(BP_WINDOW_MASK_TITLED |
                                                         BP_WINDOW_MASK_CLOSABLE |
                                                         BP_WINDOW_MASK_RESIZABLE)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [w setTitle:@"BoltPack Documentation"];
    [w center];

    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:
        [[w contentView] bounds]];
    [sv setHasVerticalScroller:YES];
    [sv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    NSTextView *tv = [[NSTextView alloc] initWithFrame:
        [[sv contentView] bounds]];
    [tv setEditable:NO];
    [tv setFont:[NSFont userFixedPitchFontOfSize:11]];
    [tv setString:[NSString stringWithUTF8String:bp_help_text]];
    [sv setDocumentView:tv];
    [[w contentView] addSubview:sv];

    [w makeKeyAndOrderFront:nil];
}

@end

/* ====================================================================== */
/*  main                                                                  */
/* ====================================================================== */

int main(int argc, const char *argv[])
{
    (void)argc; (void)argv;
#if BP_LEGACY_COCOA
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
#endif
    NSApplication *app = [NSApplication sharedApplication];
    BPAppDelegate *del = [[BPAppDelegate alloc] init];
    [app setDelegate:del];
    [app run];
    bp_gui_dev_close_log();
#if BP_LEGACY_COCOA
    [pool drain];
#endif
    return 0;
}
