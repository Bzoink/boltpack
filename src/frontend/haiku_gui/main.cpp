/*
 * main.cpp -- Haiku BeAPI GUI frontend for BoltPack
 *
 * Target:   Haiku R1 (x86, x86_64)
 * Toolkit:  BeAPI (C++) -- BApplication/BWindow/BMenuBar/BColumnListView
 *
 * Cross-compilation from Linux:
 *   x86_64-unknown-haiku-g++ -O2 -lbe -lcolumnlistview -ltracker \
 *       -I src/core -I lib/minilzo-2.10                          \
 *       src/core/compress.c src/core/archiver.c                  \
 *       src/core/bp_codepage.c                                   \
 *       src/core/fluff.c src/core/lzss.c                         \
 *       src/frontend/haiku_gui/main.cpp                          \
 *       lib/minilzo-2.10/minilzo.c                               \
 *       -o build/haiku/BoltPack
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#include <Application.h>
#include <Window.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <FilePanel.h>
#include <Alert.h>
#include <LayoutBuilder.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <StringView.h>
#include <StatusBar.h>
#include <Entry.h>
#include <Path.h>
#include <Directory.h>
#include <Roster.h>
#include <String.h>
#include <FindDirectory.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#include <map>
#include <set>
#include <vector>
#include <string>

extern "C" {
#include "archiver.h"
#include "compress.h"
#include "fluff.h"
#include "lzss.h"
}
#include "help_text.h"

/* ====================================================================== */
/*  Constants                                                             */
/* ====================================================================== */

enum {
    MSG_OPEN       = 'OPEN',
    MSG_EXTRACT    = 'EXTR',
    MSG_EXTR_SEL   = 'EXSL',
    MSG_CLOSE      = 'CLOS',
    MSG_VALIDATE   = 'VLDT',
    MSG_INFO       = 'INFO',
    MSG_CREATE     = 'CREA',
    MSG_ABOUT      = 'ABOT',
    MSG_DOC        = 'DOCS',
    MSG_OPEN_REFS  = 'OREF',
    MSG_SAVE_REF   = 'SREF',
    MSG_EXTR_REF   = 'EREF',
    MSG_EXSL_REF   = 'ESRF',
    MSG_CREA_REFS  = 'CREF',
    MSG_COMP_STORE = 'CSTO',
    MSG_COMP_LZSS  = 'CLZS',
    MSG_COMP_LZO   = 'CLZO',
    MSG_COMP_FLUFF = 'CFLF',
    MSG_LEVEL_FAST = 'LVLF',
    MSG_LEVEL_NORM = 'LVLN',
    MSG_LEVEL_BEST = 'LVLB',
    MSG_FLUFF_MT   = 'CFMT'
};

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

/* ====================================================================== */
/*  Helpers                                                               */
/* ====================================================================== */

static double u64_to_dbl(bp_uint64 v)
{
    return (double)v.lo + (double)v.hi * 4294967296.0;
}

static const char *method_name(int m)
{
    switch (m) {
        case BP_METHOD_STORED: return "Stored";
        case BP_METHOD_LZSS:   return "LZSS";
        case BP_METHOD_LZO1X:  return "miniLZO";
        case BP_METHOD_FLUFF:  return "fluff";
        case BP_METHOD_ZSTD:   return "zstd";
    }
    return "?";
}

static const char *format_methods(const bp_uint32 *counts)
{
    static char buf[128];
    static const char * const names[5] = {"Stored", "LZSS", "miniLZO", "Fluff", "zstd"};
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

static int g_compression_method = BP_METHOD_LZO1X;
static int g_compression_level  = 1;
static int g_fluff_multithread  = 0;

static const int s_method_default_level[] = { 1, 1, 1, 2 };

static bp_uint32 level_to_block_size(int method, int level)
{
    if (method == BP_METHOD_LZSS) {
        switch (level) {
            case 0: return BP_BLOCK_8K;
            case 2: return BP_BLOCK_32K;
            default: return BP_BLOCK_16K;
        }
    }

    if (method == BP_METHOD_FLUFF)
        return BP_BLOCK_256K;

    switch (level) {
        case 0: return BP_BLOCK_16K;
        case 2: return BP_BLOCK_64K;
        default: return BP_BLOCK_32K;
    }
}

static int fluff_gui_level(int level)
{
    switch (level) {
        case 0: return 3;
        case 2: return 9;
        default: return 6;
    }
}

#ifdef BOLTPACK_FAST_FLUFF
static int fast_fluff_thread_count(void)
{
    long nthreads = sysconf(_SC_NPROCESSORS_ONLN);

    if (nthreads < 0)
        return 2;
    if (nthreads <= 1)
        return 0;
    return (int)nthreads;
}
#endif

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
    char buf[32];
    if (bytes < 1024.0)
        snprintf(buf, sizeof(buf), "%.0f B", bytes);
    else if (bytes < 1024.0 * 1024.0)
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024.0 * 1024.0 * 1024.0)
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else
        snprintf(buf, sizeof(buf), "%.1f GB",
                 bytes / (1024.0 * 1024.0 * 1024.0));
    return std::string(buf);
}

static std::string format_time(bp_uint64 mtime)
{
    double secs = u64_to_dbl(mtime);
    if (secs <= 0) return "-";
    time_t t = (time_t)secs;
    struct tm *tm = localtime(&t);
    if (!tm) return "-";
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm);
    return std::string(buf);
}

static void makedirs(const char *path)
{
    char tmp[4096];
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

static const char *path_leaf(const char *path)
{
    const char *slash;

    if (path == NULL) return "archive";
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static std::string suggested_archive_name(const std::vector<std::string> &paths)
{
    std::string base;
    size_t dot;

    if (!paths.empty())
        base = path_leaf(paths[0].c_str());

    while (!base.empty() && base[base.size() - 1] == '/')
        base.erase(base.size() - 1);

    dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0)
        base.erase(dot);

    if (base.empty())
        base = "archive";

    return base + ".blt";
}

static int has_archive_extension(const std::string &path)
{
    size_t len = path.size();

    if (len >= 5 && path.compare(len - 5, 5, ".bolt") == 0)
        return 1;
    if (len >= 4 && path.compare(len - 4, 4, ".blt") == 0)
        return 1;
    return 0;
}

enum ExtractMode {
    EXTRACT_KEEP_FOLDERS = 0,
    EXTRACT_FILES_ONLY   = 1
};

static bool selection_needs_extract_mode_choice(const std::vector<std::string> &names)
{
    for (size_t i = 0; i < names.size(); i++) {
        const std::string &name = names[i];

        if (name.empty())
            continue;
        if (name[name.size() - 1] == '/')
            return true;
        if (name.find('/') != std::string::npos)
            return true;
    }

    return false;
}

static std::string leaf_name(const std::string &path)
{
    size_t end = path.size();
    size_t pos;

    while (end > 0 && path[end - 1] == '/')
        end--;

    pos = path.find_last_of('/', end == 0 ? 0 : end - 1);
    if (pos == std::string::npos)
        return path.substr(0, end);
    return path.substr(pos + 1, end - pos - 1);
}

static std::string path_with_suffix(const std::string &filename,
                                    unsigned int index)
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
        ext = filename.substr(dot);
    }

    snprintf(suffix, sizeof(suffix), "-%02u", index);
    return base + suffix + ext;
}

static std::string join_dest_path(const std::string &dir,
                                  const std::string &name)
{
    if (dir.empty())
        return name;
    if (dir[dir.size() - 1] == '/')
        return dir + name;
    return dir + "/" + name;
}

class BPEntryRow : public BRow {
public:
    BPEntryRow(const std::string &archiveName, int entryType)
        : fArchiveName(archiveName), fEntryType(entryType)
    {
    }

    const std::string &ArchiveName() const
    {
        return fArchiveName;
    }

    int EntryType() const
    {
        return fEntryType;
    }

private:
    std::string fArchiveName;
    int         fEntryType;
};

/* ====================================================================== */
/*  Main window                                                           */
/* ====================================================================== */

class BPWindow : public BWindow {
public:
    BPWindow();
    virtual ~BPWindow();

    virtual void MessageReceived(BMessage *msg);
    virtual bool QuitRequested();

    void OpenArchive(const char *path);

private:
    void DoExtract(const char *destDir);
    void DoExtractSelected(const char *destDir);
    void DoCreate(const std::vector<std::string> &paths,
                  const char *outPath);
    void DoClose();
    void DoValidate();
    void DoInfo();
    void ClassifyAndAct(BMessage *msg);
    void GatherFiles(const char *dir, std::vector<std::string> &out);
    int AddFileToArchive(bp_archive_t *ar, const char *path,
                         const char *arcName);
    int AddDirRecursive(bp_archive_t *ar, const char *dirPath,
                        const char *prefix);
    std::vector<std::string> SelectedArchiveNames() const;
    void UpdateCompressionMenu();
    void UpdateArchiveStatus();

    BMenuBar       *fMenuBar;
    BColumnListView *fListView;
    BStatusBar     *fProgressBar;
    BStringView    *fStatusView;
    BStringView    *fWelcomeView;

    BMenuItem      *fMethodItems[4];
    BMenuItem      *fLevelItems[3];
    BMenuItem      *fFastFluffItem;

    BFilePanel     *fOpenPanel;
    BFilePanel     *fExtractPanel;
    BFilePanel     *fExtractSelectedPanel;
    BFilePanel     *fSavePanel;
    BFilePanel     *fCreatePanel;

    FILE           *fArchiveFp;
    std::string     fArchivePath;
    bp_archive_info_t fArchiveInfo;
    std::vector<std::string> fPendingExtractSelection;
    ExtractMode     fPendingExtractMode;
};

BPWindow::BPWindow()
    : BWindow(BRect(80, 60, 800, 540), "BoltPack",
              B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS),
    fArchiveFp(NULL),
    fPendingExtractMode(EXTRACT_KEEP_FOLDERS)
{
    int i;

    memset(&fArchiveInfo, 0, sizeof(fArchiveInfo));
    for (i = 0; i < 4; i++)
        fMethodItems[i] = NULL;
    for (i = 0; i < 3; i++)
        fLevelItems[i] = NULL;
    fFastFluffItem = NULL;

    /* Menu bar */
    fMenuBar = new BMenuBar("menubar");

    BMenu *archMenu = new BMenu("Archive");
    archMenu->AddItem(new BMenuItem("Open" B_UTF8_ELLIPSIS,
        new BMessage(MSG_OPEN), 'O'));
    archMenu->AddItem(new BMenuItem("Extract Files" B_UTF8_ELLIPSIS,
        new BMessage(MSG_EXTRACT), 'E'));
    archMenu->AddItem(new BMenuItem("Extract Selected" B_UTF8_ELLIPSIS,
        new BMessage(MSG_EXTR_SEL)));
    archMenu->AddItem(new BMenuItem("Close",
        new BMessage(MSG_CLOSE), 'W'));
    archMenu->AddSeparatorItem();
    archMenu->AddItem(new BMenuItem("Validate",
        new BMessage(MSG_VALIDATE), 'T'));
    archMenu->AddItem(new BMenuItem("Archive Information",
        new BMessage(MSG_INFO), 'I'));
    archMenu->AddSeparatorItem();
    archMenu->AddItem(new BMenuItem("Create New" B_UTF8_ELLIPSIS,
        new BMessage(MSG_CREATE), 'N'));
    archMenu->AddSeparatorItem();
    archMenu->AddItem(new BMenuItem("Quit",
        new BMessage(B_QUIT_REQUESTED), 'Q'));
    fMenuBar->AddItem(archMenu);

    BMenu *compMenu = new BMenu("Compression");
    BMenu *methodMenu = new BMenu("Method");
    BMenu *levelMenu = new BMenu("Level");

    fMethodItems[0] = new BMenuItem("Stored",
        new BMessage(MSG_COMP_STORE));
    fMethodItems[1] = new BMenuItem("LZSS",
        new BMessage(MSG_COMP_LZSS));
    fMethodItems[2] = new BMenuItem("miniLZO",
        new BMessage(MSG_COMP_LZO));
    fMethodItems[3] = new BMenuItem("fluff",
        new BMessage(MSG_COMP_FLUFF));
    for (i = 0; i < 4; i++)
        methodMenu->AddItem(fMethodItems[i]);

    fLevelItems[0] = new BMenuItem("Fast",
        new BMessage(MSG_LEVEL_FAST));
    fLevelItems[1] = new BMenuItem("Normal",
        new BMessage(MSG_LEVEL_NORM));
    fLevelItems[2] = new BMenuItem("Best",
        new BMessage(MSG_LEVEL_BEST));
    for (i = 0; i < 3; i++)
        levelMenu->AddItem(fLevelItems[i]);

    compMenu->AddItem(methodMenu);
    compMenu->AddItem(levelMenu);
#ifdef BOLTPACK_FAST_FLUFF
    compMenu->AddSeparatorItem();
    fFastFluffItem = new BMenuItem("Use Multi-threaded Fluff",
        new BMessage(MSG_FLUFF_MT));
    compMenu->AddItem(fFastFluffItem);
#endif
    fMenuBar->AddItem(compMenu);

    BMenu *helpMenu = new BMenu("Help");
    helpMenu->AddItem(new BMenuItem("Documentation",
        new BMessage(MSG_DOC)));
    helpMenu->AddItem(new BMenuItem("About BoltPack",
        new BMessage(MSG_ABOUT)));
    fMenuBar->AddItem(helpMenu);

    /* Column list view */
    fListView = new BColumnListView("filelist", 0);
    fListView->AddColumn(new BStringColumn("Name", 200, 80, 500,
        B_TRUNCATE_END), 0);
    fListView->AddColumn(new BStringColumn("Original", 80, 50, 150,
        B_TRUNCATE_END, B_ALIGN_RIGHT), 1);
    fListView->AddColumn(new BStringColumn("Packed", 80, 50, 150,
        B_TRUNCATE_END, B_ALIGN_RIGHT), 2);
    fListView->AddColumn(new BStringColumn("Ratio", 55, 40, 80,
        B_TRUNCATE_END, B_ALIGN_RIGHT), 3);
    fListView->AddColumn(new BStringColumn("Method", 65, 50, 100,
        B_TRUNCATE_END), 4);
    fListView->AddColumn(new BStringColumn("Date", 120, 80, 200,
        B_TRUNCATE_END), 5);
    fListView->SetSortingEnabled(false);
    fListView->SetSelectionMode(B_MULTIPLE_SELECTION_LIST);

    /* Welcome text */
    fWelcomeView = new BStringView("welcome",
        "Drop a .bolt file here to open it, or drop files to create an archive.");
    fWelcomeView->SetAlignment(B_ALIGN_CENTER);
    fWelcomeView->SetFont(be_bold_font);

    fProgressBar = new BStatusBar("progress", "", "");
    fProgressBar->SetMaxValue(100.0f);
    fProgressBar->Hide();

    /* Status bar */
    fStatusView = new BStringView("status", "Ready");

    /* Layout */
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(fMenuBar)
        .Add(fListView, 1.0f)
        .Add(fWelcomeView, 1.0f)
        .Add(fProgressBar)
        .Add(fStatusView)
    .End();

    fListView->Hide();

    /* File panels */
    BMessage openMsg(MSG_OPEN_REFS);
    fOpenPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
        NULL, B_FILE_NODE, false, &openMsg);
    fOpenPanel->SetPanelDirectory("/boot/home");

    BMessage extrMsg(MSG_EXTR_REF);
    fExtractPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
        NULL, B_DIRECTORY_NODE, false, &extrMsg);

    BMessage extrSelMsg(MSG_EXSL_REF);
    fExtractSelectedPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
        NULL, B_DIRECTORY_NODE, false, &extrSelMsg);

    BMessage saveMsg(MSG_SAVE_REF);
    fSavePanel = new BFilePanel(B_SAVE_PANEL, new BMessenger(this),
        NULL, B_FILE_NODE, false, &saveMsg);

    BMessage creaMsg(MSG_CREA_REFS);
    fCreatePanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
        NULL, B_FILE_NODE | B_DIRECTORY_NODE, true, &creaMsg);

    UpdateCompressionMenu();
}

BPWindow::~BPWindow()
{
    DoClose();
    delete fOpenPanel;
    delete fExtractPanel;
    delete fExtractSelectedPanel;
    delete fSavePanel;
    delete fCreatePanel;
}

bool BPWindow::QuitRequested()
{
    be_app->PostMessage(B_QUIT_REQUESTED);
    return true;
}

void BPWindow::MessageReceived(BMessage *msg)
{
    switch (msg->what) {
        case MSG_OPEN:
            fOpenPanel->Show();
            break;

        case MSG_OPEN_REFS: {
            entry_ref ref;
            if (msg->FindRef("refs", &ref) == B_OK) {
                BPath path(&ref);
                OpenArchive(path.Path());
            }
            break;
        }

        case MSG_EXTRACT:
            if (fArchiveFp == NULL) {
                BAlert *a = new BAlert("BoltPack",
                    "No archive is open.", "OK");
                a->Go();
            } else {
                if (!fArchivePath.empty()) {
                    BPath ap(fArchivePath.c_str());
                    ap.GetParent(&ap);
                    fExtractPanel->SetPanelDirectory(ap.Path());
                }
                fExtractPanel->Show();
            }
            break;

        case MSG_EXTR_SEL:
            if (fArchiveFp == NULL) {
                BAlert *a = new BAlert("BoltPack",
                    "No archive is open.", "OK");
                a->Go();
            } else {
                std::vector<std::string> selectedNames = SelectedArchiveNames();

                if (selectedNames.empty()) {
                    BAlert *a = new BAlert("BoltPack",
                        "No files or folders are selected.", "OK");
                    a->Go();
                    break;
                }

                fPendingExtractSelection = selectedNames;
                fPendingExtractMode = EXTRACT_KEEP_FOLDERS;

                if (selection_needs_extract_mode_choice(selectedNames)) {
                    BAlert *a = new BAlert("Extract Selected",
                        "Selected entries include folders or files inside folders.\n\n"
                        "How would you like to extract them?\n\n"
                        "Keep folders preserves the selected folder structure.\n"
                        "Files only extracts selected files into the chosen folder\n"
                        "and auto-renames name collisions.",
                        "Cancel", "Keep folders", "Files only");
                    int32 choice = a->Go();
                    if (choice == 0)
                        break;
                    if (choice == 2)
                        fPendingExtractMode = EXTRACT_FILES_ONLY;
                }

                if (!fArchivePath.empty()) {
                    BPath ap(fArchivePath.c_str());
                    ap.GetParent(&ap);
                    fExtractSelectedPanel->SetPanelDirectory(ap.Path());
                }
                fExtractSelectedPanel->Show();
            }
            break;

        case MSG_EXTR_REF: {
            entry_ref ref;
            if (msg->FindRef("refs", &ref) == B_OK) {
                BPath path(&ref);
                DoExtract(path.Path());
            }
            break;
        }

        case MSG_EXSL_REF: {
            entry_ref ref;
            if (msg->FindRef("refs", &ref) == B_OK) {
                BPath path(&ref);
                DoExtractSelected(path.Path());
            }
            break;
        }

        case MSG_CLOSE:
            DoClose();
            fListView->Clear();
            fListView->Hide();
            fWelcomeView->Show();
            fProgressBar->Hide();
            SetTitle("BoltPack");
            fStatusView->SetText("Ready");
            break;

        case MSG_VALIDATE:
            DoValidate();
            break;

        case MSG_INFO:
            DoInfo();
            break;

        case MSG_CREATE:
            fCreatePanel->Show();
            break;

        case MSG_COMP_STORE:
            g_compression_method = BP_METHOD_STORED;
            g_compression_level = s_method_default_level[g_compression_method];
            UpdateCompressionMenu();
            break;

        case MSG_COMP_LZSS:
            g_compression_method = BP_METHOD_LZSS;
            g_compression_level = s_method_default_level[g_compression_method];
            UpdateCompressionMenu();
            break;

        case MSG_COMP_LZO:
            g_compression_method = BP_METHOD_LZO1X;
            g_compression_level = s_method_default_level[g_compression_method];
            UpdateCompressionMenu();
            break;

        case MSG_COMP_FLUFF:
            g_compression_method = BP_METHOD_FLUFF;
            g_compression_level = s_method_default_level[g_compression_method];
            UpdateCompressionMenu();
            break;

        case MSG_LEVEL_FAST:
            g_compression_level = 0;
            UpdateCompressionMenu();
            break;

        case MSG_LEVEL_NORM:
            g_compression_level = 1;
            UpdateCompressionMenu();
            break;

        case MSG_LEVEL_BEST:
            g_compression_level = 2;
            UpdateCompressionMenu();
            break;

        case MSG_FLUFF_MT:
            g_fluff_multithread = g_fluff_multithread ? 0 : 1;
            UpdateCompressionMenu();
            break;

        case MSG_CREA_REFS: {
            std::vector<std::string> paths;
            entry_ref ref;
            int32 i = 0;
            while (msg->FindRef("refs", i, &ref) == B_OK) {
                BPath path(&ref);
                paths.push_back(path.Path());
                i++;
            }
            if (!paths.empty()) {
                BMessage *saveMsg = new BMessage(MSG_SAVE_REF);
                for (size_t j = 0; j < paths.size(); j++)
                    saveMsg->AddString("src_path", paths[j].c_str());
                fSavePanel->SetMessage(saveMsg);
                fSavePanel->SetSaveText(suggested_archive_name(paths).c_str());
                fSavePanel->Show();
            }
            break;
        }

        case MSG_SAVE_REF: {
            entry_ref dirRef;
            BString name;
            if (msg->FindRef("directory", &dirRef) == B_OK &&
                msg->FindString("name", &name) == B_OK) {

                BPath dir(&dirRef);
                std::string outFile = std::string(dir.Path()) + "/" + name.String();
                struct stat st;

                std::vector<std::string> paths;
                const char *src;
                int32 i = 0;
                while (msg->FindString("src_path", i, &src) == B_OK) {
                    paths.push_back(src);
                    i++;
                }

                if (!has_archive_extension(outFile))
                    outFile += ".bolt";

                if (!paths.empty()) {
                    if (stat(outFile.c_str(), &st) == 0) {
                        BAlert *a = new BAlert("Overwrite",
                            "File already exists. Overwrite it?",
                            "Cancel", "Overwrite");
                        if (a->Go() != 1)
                            break;
                    }
                    DoCreate(paths, outFile.c_str());
                }
            }
            break;
        }

        case MSG_ABOUT: {
            BAlert *a = new BAlert("About BoltPack",
                "BoltPack 1.1\n\n"
                "A lightweight file archiver for\n"
                "vintage and modern systems\n\n"
                "Copyright \xC2\xA9 2026 Tony Mattsson\n"
                "GNU General Public License v2",
                "OK");
            a->Go();
            break;
        }

        case MSG_DOC: {
            BAlert *a = new BAlert("Documentation",
                bp_help_text, "Close", NULL, NULL,
                B_WIDTH_AS_USUAL, B_INFO_ALERT);
            a->Go();
            break;
        }

        case B_SIMPLE_DATA:
            ClassifyAndAct(msg);
            break;

        default:
            BWindow::MessageReceived(msg);
            break;
    }
}

/* ---- Drag and drop ---- */

void BPWindow::ClassifyAndAct(BMessage *msg)
{
    entry_ref ref;
    int32 count = 0;
    while (msg->FindRef("refs", count, &ref) == B_OK)
        count++;

    if (count <= 0) return;

    /* Single .bolt/.blt file -> open */
    if (count == 1) {
        msg->FindRef("refs", 0, &ref);
        BPath path(&ref);
        const char *p = path.Path();
        size_t len = strlen(p);
        if ((len > 5 && strcmp(p + len - 5, ".bolt") == 0) ||
            (len > 4 && strcmp(p + len - 4, ".blt") == 0)) {
            OpenArchive(p);
            return;
        }
    }

    /* Otherwise create archive from dropped items */
    std::vector<std::string> paths;
    for (int32 i = 0; i < count; i++) {
        if (msg->FindRef("refs", i, &ref) == B_OK) {
            BPath path(&ref);
            paths.push_back(path.Path());
        }
    }

    /* Show save panel */
    BMessage *saveMsg = new BMessage(MSG_SAVE_REF);
    for (size_t j = 0; j < paths.size(); j++)
        saveMsg->AddString("src_path", paths[j].c_str());
    fSavePanel->SetSaveText(suggested_archive_name(paths).c_str());
    fSavePanel->SetMessage(saveMsg);
    fSavePanel->Show();
}

std::vector<std::string> BPWindow::SelectedArchiveNames() const
{
    std::vector<std::string> names;
    BRow *row = NULL;

    while ((row = fListView->CurrentSelection(row)) != NULL) {
        BPEntryRow *entryRow = static_cast<BPEntryRow *>(row);
        std::string name = entryRow->ArchiveName();

        if (entryRow->EntryType() == BP_TYPE_META ||
            entryRow->EntryType() == BP_TYPE_RSRC) {
            continue;
        }

        if (entryRow->EntryType() == BP_TYPE_DIR &&
            !name.empty() && name[name.size() - 1] != '/') {
            name += "/";
        }

        names.push_back(name);
    }

    return names;
}

void BPWindow::UpdateCompressionMenu()
{
    int i;
    int levelEnabled = g_compression_method != BP_METHOD_STORED;
    int fluffEnabled = g_compression_method == BP_METHOD_FLUFF;

    for (i = 0; i < 4; i++) {
        if (fMethodItems[i])
            fMethodItems[i]->SetMarked(i == g_compression_method);
    }

    for (i = 0; i < 3; i++) {
        if (fLevelItems[i]) {
            fLevelItems[i]->SetMarked(i == g_compression_level);
            fLevelItems[i]->SetEnabled(levelEnabled);
        }
    }

    if (fFastFluffItem) {
        fFastFluffItem->SetMarked(g_fluff_multithread != 0);
        fFastFluffItem->SetEnabled(fluffEnabled);
    }
}

void BPWindow::UpdateArchiveStatus()
{
    char status[128];

    if (fArchiveFp == NULL) {
        fStatusView->SetText("Ready");
        return;
    }

    snprintf(status, sizeof(status), "%u entries, %s total",
             fArchiveInfo.entry_count,
             format_size(u64_to_dbl(fArchiveInfo.total_original)).c_str());
    fStatusView->SetText(status);
}

/* ---- Archive operations ---- */

struct ListCtx {
    BColumnListView *lv;
    int count;
};

struct ExtractCtx {
    std::string           destDir;
    std::set<std::string> filter;
    ExtractMode           mode;
    std::map<void *, std::string> resolvedPaths;
    std::set<std::string> reservedFlatPaths;
    unsigned long         extractedFileCount;

    ExtractCtx()
        : mode(EXTRACT_KEEP_FOLDERS), extractedFileCount(0)
    {
    }
};

struct ValidateCtx {
    BWindow      *window;
    BStatusBar   *progressBar;
    BStringView  *statusView;
    bp_uint32     fileTotal;
    bp_uint32     dirTotal;
    bp_uint32     entryTotal;
    int           okCount;
    int           failCount;
};

static bool extract_filter_matches(const ExtractCtx *ctx,
                                   const std::string &entryName)
{
    std::set<std::string>::const_iterator it;

    if (ctx->filter.empty())
        return true;

    for (it = ctx->filter.begin(); it != ctx->filter.end(); ++it) {
        const std::string &filter = *it;

        if (entryName == filter)
            return true;
        if (!filter.empty() && filter[filter.size() - 1] == '/' &&
            entryName.size() > filter.size() &&
            entryName.substr(0, filter.size()) == filter) {
            return true;
        }
    }

    return false;
}

static std::string reserve_flattened_path(ExtractCtx *ctx,
                                          const std::string &entryName)
{
    std::string filename = leaf_name(entryName);
    std::string path;
    unsigned int index;

    if (filename.empty())
        filename = "unnamed";

    path = join_dest_path(ctx->destDir, filename);
    if (ctx->reservedFlatPaths.find(path) == ctx->reservedFlatPaths.end() &&
        access(path.c_str(), 0) != 0) {
        ctx->reservedFlatPaths.insert(path);
        return path;
    }

    for (index = 1;; index++) {
        std::string candidate = join_dest_path(ctx->destDir,
            path_with_suffix(filename, index));
        if (ctx->reservedFlatPaths.find(candidate) == ctx->reservedFlatPaths.end() &&
            access(candidate.c_str(), 0) != 0) {
            ctx->reservedFlatPaths.insert(candidate);
            return candidate;
        }
    }
}

static bp_io_t *extract_open_cb(const bp_entry_t *entry, void *ud)
{
    ExtractCtx *ctx = (ExtractCtx *)ud;
    std::string entryName(entry->name ? entry->name : "");
    std::string path;
    FILE *fp;
    bp_io_t *out;

    if (entry->entry_type == BP_TYPE_DIR) {
        if (!ctx->filter.empty())
            return NULL;
        path = join_dest_path(ctx->destDir, entryName);
        if (strstr(path.c_str(), "/../") != NULL)
            return NULL;
        makedirs(path.c_str());
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_META ||
        entry->entry_type == BP_TYPE_RSRC)
        return NULL;

    if (!extract_filter_matches(ctx, entryName))
        return NULL;

    if (!ctx->filter.empty() && ctx->mode == EXTRACT_FILES_ONLY)
        path = reserve_flattened_path(ctx, entryName);
    else
        path = join_dest_path(ctx->destDir, entryName);

    if (strstr(path.c_str(), "/../") != NULL)
        return NULL;

    {
        size_t slash = path.find_last_of('/');
        if (slash != std::string::npos) {
            std::string dir = path.substr(0, slash);
            if (!dir.empty())
                makedirs(dir.c_str());
        }
    }

    fp = fopen(path.c_str(), "wb");
    if (!fp) return NULL;

    out = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (!out) {
        fclose(fp);
        return NULL;
    }

    *out = make_io(fp);
    ctx->resolvedPaths[out->handle] = path;
    return out;
}

static void extract_done_cb(const bp_entry_t *entry, bp_io_t *out, void *ud)
{
    ExtractCtx *ctx = (ExtractCtx *)ud;

    if (out) {
        std::string path;
        std::map<void *, std::string>::iterator it = ctx->resolvedPaths.find(out->handle);

        if (it != ctx->resolvedPaths.end()) {
            path = it->second;
            ctx->resolvedPaths.erase(it);
        } else {
            path = join_dest_path(ctx->destDir, entry->name ? entry->name : "");
        }

        fclose((FILE *)out->handle);

        if (entry->entry_type != BP_TYPE_DIR &&
            entry->entry_type != BP_TYPE_META &&
            entry->entry_type != BP_TYPE_RSRC) {
            ctx->extractedFileCount++;
        }

        if (!bp_u64_is_zero(entry->mtime) && !path.empty()) {
            struct utimbuf ut;
            ut.actime = (time_t)bp_u64_to_ulong(entry->mtime);
            ut.modtime = (time_t)bp_u64_to_ulong(entry->mtime);
            utime(path.c_str(), &ut);
        }

        free(out);
    }
}

static void validate_entry_cb(const bp_entry_t *entry, int result, void *ud)
{
    ValidateCtx *ctx = (ValidateCtx *)ud;
    (void)entry;

    if (result == BP_OK)
        ctx->okCount++;
    else
        ctx->failCount++;
}

static int validate_progress_cb(const bp_progress_t *p, void *ud)
{
    ValidateCtx *ctx = (ValidateCtx *)ud;
    double total = u64_to_dbl(p->archive_orig_total);
    double done = u64_to_dbl(p->archive_orig_done);
    int pct = total > 0.0 ? (int)(done / total * 100.0) : 0;
    bp_uint32 entryTotal = ctx->entryTotal ? ctx->entryTotal : p->file_count;
    char trailing[16];
    char status[160];

    if (pct > 100) pct = 100;

    snprintf(trailing, sizeof(trailing), "%d%%", pct);
    snprintf(status, sizeof(status),
             "%u files, %u directories; validating entry %u of %u",
             (unsigned)ctx->fileTotal,
             (unsigned)ctx->dirTotal,
             (unsigned)(p->file_index + 1),
             (unsigned)entryTotal);

    ctx->progressBar->SetTo((float)pct,
        p->file_name ? p->file_name : "", trailing);
    ctx->statusView->SetText(status);
    ctx->progressBar->Invalidate();
    ctx->statusView->Invalidate();
    ctx->window->UpdateIfNeeded();

    return 0;
}

static int list_cb(const bp_entry_t *entry, void *ud)
{
    ListCtx *ctx = (ListCtx *)ud;

    const char *name = entry->name ? entry->name : "(unknown)";
    double orig = u64_to_dbl(entry->original_size);
    double comp = u64_to_dbl(entry->compressed_size);

    std::string ratio = "-";
    if (orig > 0) {
        char rbuf[8];
        snprintf(rbuf, sizeof(rbuf), "%.0f%%",
                 (1.0 - comp / orig) * 100.0);
        ratio = rbuf;
    }

    BPEntryRow *row = new BPEntryRow(name, entry->entry_type);
    row->SetField(new BStringField(name), 0);
    row->SetField(new BStringField(format_size(orig).c_str()), 1);
    row->SetField(new BStringField(format_size(comp).c_str()), 2);
    row->SetField(new BStringField(ratio.c_str()), 3);
    row->SetField(new BStringField(method_name(entry->method)), 4);
    row->SetField(new BStringField(format_time(entry->mtime).c_str()), 5);
    ctx->lv->AddRow(row);
    ctx->count++;
    return 0;
}

void BPWindow::OpenArchive(const char *path)
{
    DoClose();
    fListView->Clear();

    fArchiveFp = fopen(path, "rb");
    if (!fArchiveFp) {
        BAlert *a = new BAlert("Error",
            "Cannot open file.", "OK");
        a->Go();
        return;
    }
    fArchivePath = path;

    bp_io_t io = make_io(fArchiveFp);
    bp_archive_info_t info;
    memset(&info, 0, sizeof(info));

    int rc = bp_archive_info(&io, &info);
    if (rc != BP_OK) {
        BAlert *a = new BAlert("Error",
            "Not a valid BoltPack archive.", "OK");
        a->Go();
        DoClose();
        return;
    }
    fArchiveInfo = info;

    fseeko(fArchiveFp, 0, SEEK_SET);
    io = make_io(fArchiveFp);

    ListCtx ctx;
    ctx.lv = fListView;
    ctx.count = 0;
    rc = bp_archive_list(&io, list_cb, &ctx);
    if (rc != BP_OK) {
        BAlert *a = new BAlert("Error",
            "Error reading archive.", "OK");
        a->Go();
        DoClose();
        return;
    }

    fWelcomeView->Hide();
    fListView->Show();

    BString title("BoltPack - ");
    BPath p(path);
    title << p.Leaf();
    SetTitle(title.String());
    UpdateArchiveStatus();
}

void BPWindow::DoClose()
{
    if (fArchiveFp) {
        fclose(fArchiveFp);
        fArchiveFp = NULL;
    }
    fArchivePath.clear();
    memset(&fArchiveInfo, 0, sizeof(fArchiveInfo));
}

void BPWindow::DoValidate()
{
    if (!fArchiveFp) return;

    fseeko(fArchiveFp, 0, SEEK_SET);
    bp_io_t io = make_io(fArchiveFp);

    ValidateCtx ctx;
    int rc;

    ctx.window = this;
    ctx.progressBar = fProgressBar;
    ctx.statusView = fStatusView;
    ctx.fileTotal = fArchiveInfo.entry_count - fArchiveInfo.dir_count;
    ctx.dirTotal = fArchiveInfo.dir_count;
    ctx.entryTotal = fArchiveInfo.entry_count;
    ctx.okCount = 0;
    ctx.failCount = 0;

    fProgressBar->Reset("Validating archive", "0%");
    fProgressBar->SetMaxValue(100.0f);
    fProgressBar->Show();
    fStatusView->SetText("Preparing validation...");
    fProgressBar->Invalidate();
    fStatusView->Invalidate();
    UpdateIfNeeded();

    rc = bp_archive_test_progress(&io, validate_entry_cb, &ctx,
                                  validate_progress_cb, &ctx);

    fProgressBar->Hide();
    UpdateArchiveStatus();

    if (rc != BP_OK && rc != BP_ERR_CHECKSUM) {
        BAlert *a = new BAlert("Validate Archive",
            "Validation error.", "OK");
        a->Go();
        return;
    }

    char msg[128];
    if (ctx.failCount == 0)
        snprintf(msg, sizeof(msg), "All %d file%s tested \xe2\x80\x94 all OK",
                 ctx.okCount, ctx.okCount == 1 ? "" : "s");
    else
        snprintf(msg, sizeof(msg), "%d OK,  %d FAILED",
                 ctx.okCount, ctx.failCount);

    BAlert *a = new BAlert("Validate Archive", msg, "OK");
    a->Go();
}

void BPWindow::DoInfo()
{
    if (!fArchiveFp) return;

    bp_uint32 file_count = fArchiveInfo.entry_count - fArchiveInfo.dir_count;
    double orig_d = u64_to_dbl(fArchiveInfo.total_original);
    double comp_d = u64_to_dbl(fArchiveInfo.total_compressed);
    double arc_d  = u64_to_dbl(fArchiveInfo.archive_size);
    int pct = (orig_d > 0.0) ? (int)(comp_d / orig_d * 100.0) : 0;

    char text[1024];
    snprintf(text, sizeof(text),
        "%s\n"
        "Format version: %u\n"
        "Created by: BoltPack %u.%u\n"
        "Creator OS: %s    CPU: %s\n"
        "Method: %s\n"
        "\n"
        "Archive size:  %s\n"
        "Entries:       %lu  (%lu file%s, %lu dir%s)\n"
        "Original size: %s\n"
        "Compressed:    %s  (%d%% of original)",
        fArchivePath.c_str(),
        (unsigned)fArchiveInfo.version,
        (unsigned)fArchiveInfo.creator_ver_major,
        (unsigned)fArchiveInfo.creator_ver_minor,
        os_name(fArchiveInfo.creator_os),
        cpu_name(fArchiveInfo.creator_cpu),
        format_methods(fArchiveInfo.method_counts),
        format_size(arc_d).c_str(),
        (unsigned long)fArchiveInfo.entry_count,
        (unsigned long)file_count, file_count == 1 ? "" : "s",
        (unsigned long)fArchiveInfo.dir_count,
        fArchiveInfo.dir_count == 1 ? "" : "s",
        format_size(orig_d).c_str(),
        format_size(comp_d).c_str(), pct);

    BAlert *a = new BAlert("Archive Information", text, "OK");
    a->Go();
}

void BPWindow::DoExtract(const char *destDir)
{
    if (!fArchiveFp) return;

    fseeko(fArchiveFp, 0, SEEK_SET);
    bp_io_t io = make_io(fArchiveFp);

    ExtractCtx ctx;
    ctx.destDir = destDir;

    int rc = bp_archive_extract_all(&io, extract_open_cb, extract_done_cb,
        NULL, &ctx);

    if (rc != BP_OK && rc != BP_ERR_ABORTED) {
        BAlert *a = new BAlert("Error",
            "Extraction error.", "OK");
        a->Go();
    } else if (rc == BP_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%lu files extracted.",
                 ctx.extractedFileCount);
        BAlert *a = new BAlert("Extraction complete", msg, "OK");
        a->Go();
    }
}

void BPWindow::DoExtractSelected(const char *destDir)
{
    if (!fArchiveFp || fPendingExtractSelection.empty())
        return;

    fseeko(fArchiveFp, 0, SEEK_SET);

    bp_io_t io = make_io(fArchiveFp);
    ExtractCtx ctx;
    size_t i;
    int rc;

    ctx.destDir = destDir;
    ctx.mode = fPendingExtractMode;
    for (i = 0; i < fPendingExtractSelection.size(); i++) {
        if (!fPendingExtractSelection[i].empty())
            ctx.filter.insert(fPendingExtractSelection[i]);
    }

    rc = bp_archive_extract_all(&io, extract_open_cb, extract_done_cb,
        NULL, &ctx);

    if (rc != BP_OK && rc != BP_ERR_ABORTED) {
        BAlert *a = new BAlert("Error",
            "Extraction error.", "OK");
        a->Go();
    } else if (rc == BP_OK) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%lu selected file%s extracted.",
                 ctx.extractedFileCount,
                 ctx.extractedFileCount == 1 ? "" : "s");
        BAlert *a = new BAlert("Extraction complete", msg, "OK");
        a->Go();
    }

    fPendingExtractSelection.clear();
    fPendingExtractMode = EXTRACT_KEEP_FOLDERS;
}

int BPWindow::AddDirRecursive(bp_archive_t *ar, const char *dirPath,
                              const char *prefix)
{
    DIR *d = opendir(dirPath);
    struct dirent *ent;

    if (!d) return BP_ERR_IO;

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        std::string full = std::string(dirPath) + "/" + ent->d_name;
        std::string arc  = std::string(prefix) + ent->d_name;

        int rc = AddFileToArchive(ar, full.c_str(), arc.c_str());
        if (rc != BP_OK) {
            closedir(d);
            return rc;
        }
    }

    closedir(d);
    return BP_OK;
}

int BPWindow::AddFileToArchive(bp_archive_t *ar, const char *path,
                               const char *arcName)
{
    struct stat st;

    if (stat(path, &st) != 0) return BP_ERR_IO;

    if (S_ISDIR(st.st_mode)) {
        std::string dirName = std::string(arcName ? arcName : "");
        bp_uint64 mtime;
        int rc;

        if (dirName.empty() || dirName[dirName.size() - 1] != '/')
            dirName += "/";

        mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
        rc = bp_archive_add_entry(ar, dirName.c_str(), BP_TYPE_DIR,
                                  BP_OS_BEOS, NULL,
                                  bp_u64_from_u32(0), mtime);
        if (rc != BP_OK) return rc;

        return AddDirRecursive(ar, path, dirName.c_str());
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) return BP_ERR_IO;

    bp_io_t fio = make_io(fp);
    bp_uint64 fsize = bp_off_to_u64((bp_off_t)st.st_size);
    bp_uint64 mtime = bp_off_to_u64((bp_off_t)st.st_mtime);
    int rc = bp_archive_add_entry(ar, arcName, BP_TYPE_DATA,
                                  BP_OS_BEOS, &fio, fsize, mtime);
    fclose(fp);
    return rc;
}

void BPWindow::DoCreate(const std::vector<std::string> &paths,
                        const char *outPath)
{
    if (paths.empty() || outPath == NULL || outPath[0] == '\0')
        return;

    FILE *outFp = fopen(outPath, "wb");
    if (!outFp) {
        BAlert *a = new BAlert("Error",
            "Cannot create output file.", "OK");
        a->Go();
        return;
    }

    bp_io_t outIo = make_io(outFp);
    bp_archive_t ar;
    memset(&ar, 0, sizeof(ar));

    int rc = bp_archive_create(&ar, &outIo,
        level_to_block_size(g_compression_method, g_compression_level),
        (bp_uint8)g_compression_method);
    if (rc != BP_OK) {
        fclose(outFp);
        remove(outPath);
        BAlert *a = new BAlert("Error",
            "Failed to initialize archive creation.", "OK");
        a->Go();
        return;
    }
    ar.method_level = (bp_uint8)fluff_gui_level(g_compression_level);

#ifdef BOLTPACK_FAST_FLUFF
    if (g_compression_method == BP_METHOD_FLUFF && g_fluff_multithread) {
        int nthreads = fast_fluff_thread_count();
        if (nthreads > 1)
            bp_set_fast_fluff(&ar, nthreads);
    }
#endif

    for (size_t i = 0; i < paths.size(); i++) {
        const char *root = paths[i].c_str();
        const char *base = path_leaf(root);
        rc = AddFileToArchive(&ar, root, base);
        if (rc != BP_OK) break;
    }

    if (rc == BP_OK)
        rc = bp_archive_finalize(&ar);

    bp_archive_close(&ar);
    fclose(outFp);

    if (rc != BP_OK) {
        remove(outPath);
        BAlert *a = new BAlert("Error",
            "Error creating archive.", "OK");
        a->Go();
        return;
    }

    OpenArchive(outPath);
}

void BPWindow::GatherFiles(const char *dir,
                            std::vector<std::string> &out)
{
    BDirectory d(dir);
    BEntry entry;
    while (d.GetNextEntry(&entry) == B_OK) {
        BPath path;
        entry.GetPath(&path);
        if (entry.IsDirectory())
            GatherFiles(path.Path(), out);
        else
            out.push_back(path.Path());
    }
}

/* ====================================================================== */
/*  Application                                                           */
/* ====================================================================== */

class BPApp : public BApplication {
public:
    BPApp();
    virtual void ReadyToRun();
    virtual void RefsReceived(BMessage *msg);
private:
    BPWindow *fWindow;
};

BPApp::BPApp()
    : BApplication("application/x-vnd.BoltPack"),
      fWindow(NULL)
{
}

void BPApp::ReadyToRun()
{
    if (bp_compress_init() != BP_OK) {
        BAlert *a = new BAlert("Error",
            "Failed to initialize compression engine.", "Quit");
        a->Go();
        PostMessage(B_QUIT_REQUESTED);
        return;
    }

    fWindow = new BPWindow();
    fWindow->Show();
}

void BPApp::RefsReceived(BMessage *msg)
{
    if (fWindow) {
        /* Forward to window for DnD / open-with handling */
        entry_ref ref;
        if (msg->FindRef("refs", &ref) == B_OK) {
            BPath path(&ref);
            fWindow->Lock();
            fWindow->OpenArchive(path.Path());
            fWindow->Unlock();
        }
    }
}

/* ====================================================================== */
/*  main                                                                  */
/* ====================================================================== */

int main()
{
    BPApp app;
    app.Run();
    return 0;
}
