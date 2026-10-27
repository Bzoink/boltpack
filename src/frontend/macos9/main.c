/*
 * main.c -- Mac OS 9 GUI frontend for BoltPack
 *
 * CodeWarrior Pro 8 (Metrowerks), PowerPC target.
 * Mac Toolbox / Appearance Manager / Drag Manager / Navigation Services.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <Dialogs.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Memory.h>
#include <Resources.h>
#include <ToolUtils.h>
#include <TextUtils.h>
#include <ControlDefinitions.h>
#include <Controls.h>
#include <Appearance.h>
#include <Drag.h>
#include <AppleEvents.h>
#include <AEDataModel.h>
#include <Devices.h>
#include <StandardFile.h>
#include <Files.h>
#include <Folders.h>
#include <Gestalt.h>
#include <Navigation.h>
#include <Processes.h>
#include <LowMem.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ==== Constants ========================================================= */

#define kCreatorCode    'BoPk'
#define kBoltFileType   'BOLT'

/* Resource IDs */
#define rMenuBar        128

#define mApple          128
#define mFile           129
#define mEdit           130

#define iAbout          1

#define iFileOpen       1
#define iFileCreate     2
/* separator = 3 */
#define iFilePrefs      4
/* separator = 5 */
#define iFileQuit       6

#define rMainWindow     128

#define rDlgProgress    128
#define rDlgMixed       129
#define rDlgName        130
#define rDlgPrefs       131
#define rDlgAbout       132
#define rDlgProfile     133
#define rAlrtError      128

/* Logo */
#define rLogoPict           128

/* Mode */
#define kModeExpand         0
#define kModeCompress       1

/* New progress dialog items */
#define kProgOverallLabel   6
#define kProgOverallBar     7

/* New STR# 129 indices */
#define kStrExpand          2
#define kStrCompress        3
#define kStrAskDest         4

/* Layout */
#define kLogoSize           96
#define kDropAreaInset      20
/* Progress dialog items */
#define kProgFileName   1
#define kProgBar        2
#define kProgFileCount  3
#define kProgRatio      4
#define kProgCancel     5

/* Mixed dialog items */
#define kMixedUnpack    1
#define kMixedPack      2
#define kMixedCancel    3

/* Name dialog items */
#define kNameField      2
#define kNameOK         1
#define kNameCancel     3

/* Prefs dialog items */
#define kPrefsCheck     1
#define kPrefsOK        2
#define kPrefsCancel    3

/* Archive profile dialog items */
#define kProfileDefault 1
#define kProfileInterchange 2
#define kProfileCancel  3

/* String list IDs */
#define rErrStrings     128
#define rUIStrings      129

/* UI string indices */
#define kStrDropHere    1

/* Maximum items we handle in a single drop */
#define kMaxDropItems   256

/* Mac path limit */
#define kMaxPath        1024

/* ==== Preferences ======================================================= */

#define kPrefVersion    2

typedef struct {
    short   version;
    char    ask_extract_location; /* 0 = same folder, 1 = ask */
    char    mode;                 /* 0 = kModeExpand, 1 = kModeCompress */
} prefs_t;

/* ==== Mac file handle wrapper for bp_io_t =============================== */

typedef struct {
    short refNum;
} mac_file_t;

/* ==== Globals =========================================================== */

static WindowPtr    gMainWindow = NULL;
static Boolean      gQuit = false;
static Boolean      gHasNavServices = false;
static prefs_t      gPrefs;
static FSSpec       gAppSpec; /* FSSpec of the application itself */

static ControlHandle  gRadioExpand   = NULL;
static ControlHandle  gRadioCompress = NULL;
static ControlHandle  gAskDestCheck  = NULL;
static short          gMode          = kModeExpand;

/* ==== Forward declarations ============================================== */

static void     InitToolbox(void);
static void     InitAppleEvents(void);
static void     InitDragManager(void);
static void     InitMenuBar(void);
static void     OpenMainWindow(void);
static void     DrawDropTarget(WindowPtr win);
static void     CheckNavServices(void);
static void     LoadPrefs(void);
static void     SavePrefs(void);
static void     EventLoop(void);
static void     HandleMouseDown(EventRecord *evt);
static void     HandleKeyDown(EventRecord *evt);
static void     HandleMenuChoice(long menuChoice);
static void     HandleUpdate(EventRecord *evt);
static void     HandleActivate(EventRecord *evt);

static void     DoAbout(void);
static void     DoPreferences(void);
static int      AskInterchangeProfile(void);
static void     ShowError(int bpErr);
static void     ShowErrorStr(const char *msg);

static void     ClassifyAndAct(FSSpec *items, long count);
static int      IsBoltArchive(const FSSpec *spec);
static int      IsDirectory(const FSSpec *spec);

static int      DoUnpack(FSSpec *archives, long count);
static int      DoPack(FSSpec *items, long count);

static int      AddFSSpecToArchive(bp_archive_t *ar, const FSSpec *spec,
                    const char *arcPrefix);
static int      AddDirectoryToArchive(bp_archive_t *ar, short vRefNum,
                    long dirID, const char *arcPrefix);

static bp_uint32 ChooseBlockSize(void);
static bp_uint8 ChooseCompressionMethod(void);

/* bp_io_t callbacks */
static long     MacFileRead(void *handle, void *buf, bp_uint32 count);
static long     MacFileWrite(void *handle, const void *buf, bp_uint32 count);
static int      MacFileSeek(void *handle, bp_off_t offset, int origin);
static bp_off_t MacFileTell(void *handle);

static void     MacIOFromRefNum(bp_io_t *io, mac_file_t *mf, short refNum);

/* Progress */
static int      ProgressCallback(const bp_progress_t *p, void *user_data);

/* Extraction callbacks */
static bp_io_t *ExtractOpen(const bp_entry_t *entry, void *user_data);
static int      ExtractDoneStatus(const bp_entry_t *entry, bp_io_t *out,
                    int status, void *user_data);

/* Text encoding */
static void     MacRomanToUTF8(const unsigned char *src, char *dst,
                    long maxDst);
static void     UTF8ToMacRoman(const char *src, unsigned char *dst,
                    long maxDst);
static int      UTF8ToMacRomanStrict(const char *src, unsigned char *dst,
                    long maxDst);
static int      ArchivePathToMac(const char *src, Str255 dst);

/* Path helpers */
static void     FSSpecToParentDir(const FSSpec *spec, short *vRefNum,
                    long *dirID);
static void     ConcatArchivePath(char *buf, long bufSize,
                    const char *prefix, const char *name);
static void     PascalToBuf(const unsigned char *pstr, unsigned char *buf,
                    long bufSize);

/* stdio-based bp_io_t for temporary metadata */
static long     MacTmpRead(void *handle, void *buf, bp_uint32 count);
static int      MacTmpSeek(void *handle, bp_off_t offset, int origin);
static bp_off_t MacTmpTell(void *handle);

/* ==== MacRoman <-> UTF-8 table ========================================== */

/*
 * MacRoman characters 0x80-0xFF mapped to their Unicode codepoints.
 * We encode these to UTF-8 during pack and reverse during extract.
 */
static const unsigned short kMacRomanToUnicode[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, /* 80-87 */
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8, /* 88-8F */
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, /* 90-97 */
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, /* 98-9F */
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, /* A0-A7 */
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, /* A8-AF */
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, /* B0-B7 */
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, /* B8-BF */
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, /* C0-C7 */
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, /* C8-CF */
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, /* D0-D7 */
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, /* D8-DF */
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, /* E0-E7 */
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, /* E8-EF */
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, /* F0-F7 */
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7  /* F8-FF */
};

/* ==== bp_io_t Implementation for Mac File Manager ======================= */

static long MacFileRead(void *handle, void *buf, bp_uint32 count)
{
    mac_file_t *mf = (mac_file_t *)handle;
    long ioCount = (long)count;
    OSErr err;

    err = FSRead(mf->refNum, &ioCount, buf);
    if (err != noErr && err != eofErr) return -1;
    return ioCount;
}

static long MacFileWrite(void *handle, const void *buf, bp_uint32 count)
{
    mac_file_t *mf = (mac_file_t *)handle;
    long ioCount = (long)count;
    OSErr err;

    err = FSWrite(mf->refNum, &ioCount, (void *)buf);
    if (err != noErr) return -1;
    return ioCount;
}

static int MacFileSeek(void *handle, bp_off_t offset, int origin)
{
    mac_file_t *mf = (mac_file_t *)handle;
    short posMode;

    switch (origin) {
    case 0: posMode = fsFromStart; break;  /* SEEK_SET */
    case 1: posMode = fsFromMark;  break;  /* SEEK_CUR */
    case 2: posMode = fsFromLEOF;  break;  /* SEEK_END */
    default: return -1;
    }
    return (SetFPos(mf->refNum, posMode, (long)offset) == noErr) ? 0 : -1;
}

static bp_off_t MacFileTell(void *handle)
{
    mac_file_t *mf = (mac_file_t *)handle;
    long pos;

    if (GetFPos(mf->refNum, &pos) != noErr) return -1;
    return (bp_off_t)pos;
}

static void MacIOFromRefNum(bp_io_t *io, mac_file_t *mf, short refNum)
{
    mf->refNum = refNum;
    io->read   = MacFileRead;
    io->write  = MacFileWrite;
    io->seek   = MacFileSeek;
    io->tell   = MacFileTell;
    io->handle = mf;
}

/* ==== Pascal String Helper ============================================== */

/*
 * Copy a Pascal string (length-prefixed) to a null-terminated C buffer.
 */
static void PascalToBuf(const unsigned char *pstr, unsigned char *buf,
                        long bufSize)
{
    long len = pstr[0];
    if (len > bufSize - 1) len = bufSize - 1;
    memcpy(buf, pstr + 1, len);
    buf[len] = '\0';
}

/* ==== Path Helpers ====================================================== */

static void FSSpecToParentDir(const FSSpec *spec, short *vRefNum, long *dirID)
{
    *vRefNum = spec->vRefNum;
    *dirID = spec->parID;
}

/* ==== Text Encoding ===================================================== */

static void MacRomanToUTF8(const unsigned char *src, char *dst, long maxDst)
{
    long di = 0;
    long i;

    for (i = 0; src[i] != 0 && di < maxDst - 4; i++) {
        unsigned char c = src[i];
        if (c < 0x80) {
            dst[di++] = (char)c;
        } else {
            unsigned short u = kMacRomanToUnicode[c - 0x80];
            if (u < 0x0080) {
                dst[di++] = (char)u;
            } else if (u < 0x0800) {
                dst[di++] = (char)(0xC0 | (u >> 6));
                dst[di++] = (char)(0x80 | (u & 0x3F));
            } else {
                dst[di++] = (char)(0xE0 | (u >> 12));
                dst[di++] = (char)(0x80 | ((u >> 6) & 0x3F));
                dst[di++] = (char)(0x80 | (u & 0x3F));
            }
        }
    }
    dst[di] = '\0';
}

static void UTF8ToMacRoman(const char *src, unsigned char *dst, long maxDst)
{
    long di = 0;
    long i;

    for (i = 0; src[i] != '\0' && di < maxDst - 1; ) {
        unsigned char c = (unsigned char)src[i];
        unsigned short u;
        int j;

        if (c < 0x80) {
            dst[di++] = c;
            i++;
        } else if ((c & 0xE0) == 0xC0) {
            u = ((unsigned short)(c & 0x1F) << 6)
              | ((unsigned short)((unsigned char)src[i+1]) & 0x3F);
            i += 2;
            goto lookup;
        } else if ((c & 0xF0) == 0xE0) {
            u = ((unsigned short)(c & 0x0F) << 12)
              | (((unsigned short)((unsigned char)src[i+1]) & 0x3F) << 6)
              | ((unsigned short)((unsigned char)src[i+2]) & 0x3F);
            i += 3;
            goto lookup;
        } else {
            /* 4-byte UTF-8 or invalid — skip, replace with underscore */
            dst[di++] = '_';
            i++;
            while (((unsigned char)src[i] & 0xC0) == 0x80) i++;
            continue;
        }
        continue;

    lookup:
        /* search MacRoman table for this codepoint */
        for (j = 0; j < 128; j++) {
            if (kMacRomanToUnicode[j] == u) {
                dst[di++] = (unsigned char)(0x80 + j);
                goto next;
            }
        }
        dst[di++] = '_'; /* unmappable */
    next:
        ;
    }
    dst[di] = '\0';
}

/* Map one UTF-8 path component to MacRoman without substitution or
 * truncation. Refusing lossy mappings prevents distinct archive names from
 * silently collapsing onto the same HFS name. */
static int UTF8ToMacRomanStrict(const char *src, unsigned char *dst,
                                long maxDst)
{
    long i = 0, di = 0;

    if (src == nil || dst == nil || maxDst < 1) return 0;
    while (src[i] != '\0') {
        unsigned char c = (unsigned char)src[i];
        unsigned long u;
        long consumed;
        int j;

        if (c < 0x80) {
            u = c;
            consumed = 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            unsigned char c1 = (unsigned char)src[i + 1];
            if (c1 == 0 || (c1 & 0xC0) != 0x80) return 0;
            u = ((unsigned long)(c & 0x1F) << 6) | (c1 & 0x3F);
            consumed = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            unsigned char c1 = (unsigned char)src[i + 1];
            unsigned char c2;
            if (c1 == 0 || (c1 & 0xC0) != 0x80) return 0;
            c2 = (unsigned char)src[i + 2];
            if (c2 == 0 || (c2 & 0xC0) != 0x80) return 0;
            if ((c == 0xE0 && c1 < 0xA0) ||
                (c == 0xED && c1 >= 0xA0)) return 0;
            u = ((unsigned long)(c & 0x0F) << 12) |
                ((unsigned long)(c1 & 0x3F) << 6) | (c2 & 0x3F);
            consumed = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            unsigned char c1 = (unsigned char)src[i + 1];
            unsigned char c2, c3;
            if (c1 == 0 || (c1 & 0xC0) != 0x80) return 0;
            c2 = (unsigned char)src[i + 2];
            if (c2 == 0 || (c2 & 0xC0) != 0x80) return 0;
            c3 = (unsigned char)src[i + 3];
            if (c3 == 0 || (c3 & 0xC0) != 0x80) return 0;
            if ((c == 0xF0 && c1 < 0x90) ||
                (c == 0xF4 && c1 > 0x8F)) return 0;
            /* Valid supplementary characters have no MacRoman mapping. */
            return 0;
        } else {
            return 0;
        }

        if (u == '/' || u == ':' || u < 0x20 || u == 0x7F) return 0;
        if (u < 0x80) {
            if (di >= maxDst - 1) return 0;
            dst[di++] = (unsigned char)u;
        } else {
            for (j = 0; j < 128; j++)
                if (kMacRomanToUnicode[j] == u) break;
            if (j == 128 || di >= maxDst - 1) return 0;
            dst[di++] = (unsigned char)(0x80 + j);
        }
        i += consumed;
    }
    dst[di] = '\0';
    return 1;
}

/*
 * Convert a UTF-8 archive path (using '/') to a Mac Pascal string
 * (using ':') suitable for use with File Manager calls.
 */
static int ArchivePathToMac(const char *src, Str255 dst)
{
    char segment[256];
    unsigned char macSegment[256];
    const char *p = src;
    long outLen = 0;

    if (src == nil || dst == nil) return 0;
    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        long len = slash != nil ? (long)(slash - p) : (long)strlen(p);
        long macLen;

        if (len <= 0 || len > 255) return 0;
        memcpy(segment, p, len);
        segment[len] = '\0';
        if (!UTF8ToMacRomanStrict(segment, macSegment,
                                  (long)sizeof(macSegment))) return 0;
        macLen = (long)strlen((char *)macSegment);
        if (macLen <= 0 || macLen > 31 ||
            (macLen == 1 && macSegment[0] == '.') ||
            (macLen == 2 && macSegment[0] == '.' && macSegment[1] == '.'))
            return 0;
        if (outLen != 0) {
            if (outLen >= 255) return 0;
            dst[++outLen] = ':';
        }
        if (outLen + macLen > 255) return 0;
        memcpy(dst + 1 + outLen, macSegment, macLen);
        outLen += macLen;
        if (slash == nil) break;
        p = slash + 1;
        if (*p == '\0') break; /* directory trailing slash */
    }
    if (outLen == 0) return 0;
    dst[0] = (unsigned char)outLen;
    return 1;
}

static void ConcatArchivePath(char *buf, long bufSize, const char *prefix,
                              const char *name)
{
    long plen = (long)strlen(prefix);
    long nlen = (long)strlen(name);

    if (plen + nlen + 2 > bufSize) {
        buf[0] = '\0';
        return;
    }

    memcpy(buf, prefix, plen);
    if (plen > 0 && prefix[plen - 1] != '/') {
        buf[plen++] = '/';
    }
    memcpy(buf + plen, name, nlen);
    buf[plen + nlen] = '\0';
}

/* ==== Block Size Selection ============================================== */

static bp_uint32 ChooseBlockSize(void)
{
    long freeMem;

    freeMem = TempFreeMem();
    if (freeMem <= 0) freeMem = FreeMem();

    /* need block_size * 2 + 64K for BoltLZ stack/hash overhead + 512K headroom */
    if (freeMem > (long)(BP_BLOCK_256K * 2 + 65536 + 524288))
        return BP_BLOCK_256K;
    if (freeMem > (long)(BP_BLOCK_64K * 2 + 65536 + 524288))
        return BP_BLOCK_64K;
    if (freeMem > (long)(BP_BLOCK_32K * 2 + 65536 + 524288))
        return BP_BLOCK_32K;
    if (freeMem > (long)(BP_BLOCK_16K * 2 + 65536 + 524288))
        return BP_BLOCK_16K;
    return BP_BLOCK_8K;
}

/* ---- ChooseCompressionMethod ------------------------------------------
 * The PowerPC GUI keeps boltlz as its standard-format default.
 */
static bp_uint8 ChooseCompressionMethod(void)
{
    return BP_METHOD_BOLTLZ;  /* Default: boltlz */
}

/* Ask whether a new archive should use the cross-platform profile.
 * Returns 1 for Interchange Profile 1, 0 for the existing default, or -1
 * when the user cancels. */
static int AskInterchangeProfile(void)
{
    DialogPtr dlg;
    short item;
    int result;

    dlg = GetNewDialog(rDlgProfile, nil, (WindowPtr)-1);
    if (dlg == nil) return -1;

    SetDialogDefaultItem(dlg, kProfileDefault);
    SetDialogCancelItem(dlg, kProfileCancel);
    ShowWindow(GetDialogWindow(dlg));

    result = -1;
    for (;;) {
        ModalDialog(nil, &item);
        if (item == kProfileDefault) { result = 0; break; }
        if (item == kProfileInterchange) { result = 1; break; }
        if (item == kProfileCancel) break;
    }

    DisposeDialog(dlg);
    return result;
}

/* ==== Error Display ===================================================== */

static void ShowError(int bpErr)
{
    short idx;

    /* Map BP_ERR_* codes to STR# 128 indices (1-based) */
    if (bpErr >= BP_ERR_INVALID_ARG && bpErr <= BP_ERR_IO) {
        idx = (short)(1 - bpErr); /* BP_ERR_IO=-1 -> 1, etc. */
    } else {
        idx = 16; /* "unknown error" */
    }
    /* use NoteAlert with the string */
    ParamText("\pBoltPack Error", "\p", "\p", "\p");
    NoteAlert(rAlrtError, nil);
}

static void ShowErrorStr(const char *msg)
{
    Str255 pstr;
    long len = (long)strlen(msg);
    if (len > 255) len = 255;
    pstr[0] = (unsigned char)len;
    memcpy(pstr + 1, msg, len);
    ParamText(pstr, "\p", "\p", "\p");
    NoteAlert(rAlrtError, nil);
}

/* ==== Preferences ======================================================= */

static void GetPrefsFileSpec(FSSpec *spec)
{
    short vRefNum;
    long dirID;

    FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder,
               &vRefNum, &dirID);
    FSMakeFSSpec(vRefNum, dirID, "\pBoltPack Preferences", spec);
}

static void LoadPrefs(void)
{
    FSSpec spec;
    short refNum;
    long count;
    OSErr err;

    gPrefs.version = kPrefVersion;
    gPrefs.ask_extract_location = 0;
    gPrefs.mode = (char)kModeExpand;

    GetPrefsFileSpec(&spec);
    err = FSpOpenDF(&spec, fsRdPerm, &refNum);
    if (err != noErr) {
        gMode = kModeExpand;
        return;
    }

    count = sizeof(prefs_t);
    FSRead(refNum, &count, &gPrefs);
    FSClose(refNum);

    if (gPrefs.version != kPrefVersion) {
        /* migrate: reset to safe defaults and rewrite */
        gPrefs.version = kPrefVersion;
        gPrefs.ask_extract_location = 0;
        gPrefs.mode = (char)kModeExpand;
        SavePrefs();
    }

    /* disable Ask if no NavServices */
    if (!gHasNavServices) {
        gPrefs.ask_extract_location = 0;
    }

    gMode = (short)gPrefs.mode;
}

static void SavePrefs(void)
{
    FSSpec spec;
    short refNum;
    long count;
    OSErr err;

    GetPrefsFileSpec(&spec);

    err = FSpOpenDF(&spec, fsWrPerm, &refNum);
    if (err == fnfErr) {
        FSpCreate(&spec, kCreatorCode, 'pref', 0);
        err = FSpOpenDF(&spec, fsWrPerm, &refNum);
    }
    if (err != noErr) return;

    count = sizeof(prefs_t);
    FSWrite(refNum, &count, &gPrefs);
    FSClose(refNum);
}

/* ==== Preferences Dialog ================================================ */

static void DoPreferences(void)
{
    DialogPtr dlg;
    short item;
    short itemType;
    Handle itemHandle;
    Rect itemRect;
    short checkVal;

    dlg = GetNewDialog(rDlgPrefs, nil, (WindowPtr)-1);
    if (dlg == nil) return;

    /* set initial checkbox state */
    GetDialogItem(dlg, kPrefsCheck, &itemType, &itemHandle, &itemRect);
    SetControlValue((ControlHandle)itemHandle,
                    gPrefs.ask_extract_location ? 1 : 0);

    /* grey out if no NavServices */
    if (!gHasNavServices) {
        HiliteControl((ControlHandle)itemHandle, 255);
    }

    SetDialogDefaultItem(dlg, kPrefsOK);
    SetDialogCancelItem(dlg, kPrefsCancel);
    ShowWindow(GetDialogWindow(dlg));

    for (;;) {
        ModalDialog(nil, &item);
        if (item == kPrefsOK) {
            GetDialogItem(dlg, kPrefsCheck, &itemType, &itemHandle, &itemRect);
            checkVal = GetControlValue((ControlHandle)itemHandle);
            gPrefs.ask_extract_location = (checkVal != 0) ? 1 : 0;
            SavePrefs();
            break;
        }
        if (item == kPrefsCancel) break;
        if (item == kPrefsCheck && gHasNavServices) {
            GetDialogItem(dlg, kPrefsCheck, &itemType, &itemHandle, &itemRect);
            checkVal = GetControlValue((ControlHandle)itemHandle);
            SetControlValue((ControlHandle)itemHandle, checkVal ? 0 : 1);
        }
    }

    DisposeDialog(dlg);
}

/* ==== About Dialog ====================================================== */

static void DoAbout(void)
{
    DialogPtr dlg;
    short item;

    dlg = GetNewDialog(rDlgAbout, nil, (WindowPtr)-1);
    if (dlg == nil) return;
    SetDialogDefaultItem(dlg, 1);
    ShowWindow(GetDialogWindow(dlg));
    ModalDialog(nil, &item);
    DisposeDialog(dlg);
}

/* ==== Progress Dialog =================================================== */

typedef struct {
    DialogPtr   dlg;
    Boolean     cancelled;
    /* extraction context */
    short       outVRefNum;
    long        outDirID;
} progress_ctx_t;

static void ShowProgressDialog(progress_ctx_t *ctx)
{
    ctx->dlg = GetNewDialog(rDlgProgress, nil, (WindowPtr)-1);
    ctx->cancelled = false;
    if (ctx->dlg != nil) {
        SetDialogDefaultItem(ctx->dlg, kProgCancel);
        ShowWindow(GetDialogWindow(ctx->dlg));
        DrawDialog(ctx->dlg);
    }
}

static void HideProgressDialog(progress_ctx_t *ctx)
{
    if (ctx->dlg != nil) {
        DisposeDialog(ctx->dlg);
        ctx->dlg = nil;
    }
}

static void UpdateProgressText(DialogPtr dlg, short item, const char *text)
{
    short itemType;
    Handle itemHandle;
    Rect itemRect;
    Str255 pstr;
    long len;

    GetDialogItem(dlg, item, &itemType, &itemHandle, &itemRect);

    len = (long)strlen(text);
    if (len > 255) len = 255;
    pstr[0] = (unsigned char)len;
    memcpy(pstr + 1, text, len);

    SetDialogItemText(itemHandle, pstr);
}

static int ProgressCallback(const bp_progress_t *p, void *user_data)
{
    progress_ctx_t *ctx = (progress_ctx_t *)user_data;
    EventRecord evt;
    DialogPtr whichDlg;
    short item;
    short itemType;
    Handle itemHandle;
    Rect itemRect;
    char buf[256];
    long pct;

    if (ctx->dlg == nil) return 0;
    if (ctx->cancelled) return 1;

    /* update file name */
    if (p->file_name != nil) {
        char nameBuf[256];
        long nameLen = (long)strlen(p->file_name);
        if (nameLen > 255) nameLen = 255;
        memcpy(nameBuf, p->file_name, nameLen);
        nameBuf[nameLen] = '\0';
        UpdateProgressText(ctx->dlg, kProgFileName, nameBuf);
    }

    /* update "File X of Y" */
    sprintf(buf, "File %lu of %lu",
            (unsigned long)(p->file_index + 1),
            (unsigned long)p->file_count);
    UpdateProgressText(ctx->dlg, kProgFileCount, buf);

    /* update progress bar */
    if (!bp_u64_is_zero(p->file_orig_size)) {
        pct = (long)((double)bp_u64_to_u32(p->file_bytes_done) /
              (double)bp_u64_to_u32(p->file_orig_size) * 100.0);
    } else if (p->file_count > 0) {
        pct = (long)((double)(p->file_index) /
              (double)p->file_count * 100.0);
    } else {
        pct = 0;
    }
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;

    GetDialogItem(ctx->dlg, kProgBar, &itemType, &itemHandle, &itemRect);
    SetControlValue((ControlHandle)itemHandle, (short)pct);

    /* update ratio */
    if (!bp_u64_is_zero(p->archive_orig_total)) {
        long ratio_pct;
        ratio_pct = (long)((double)bp_u64_to_u32(p->archive_comp_total) /
                    (double)bp_u64_to_u32(p->archive_orig_total) * 100.0);
        sprintf(buf, "Ratio: %ld%%", ratio_pct);
    } else {
        strcpy(buf, "Ratio: --");
    }
    UpdateProgressText(ctx->dlg, kProgRatio, buf);

    /* update overall progress bar */
    if (!bp_u64_is_zero(p->archive_orig_total)) {
        long overall_pct;
        overall_pct = (long)(
            (double)bp_u64_to_u32(p->archive_orig_done) /
            (double)bp_u64_to_u32(p->archive_orig_total) * 100.0);
        if (overall_pct > 100) overall_pct = 100;
        if (overall_pct < 0)   overall_pct = 0;
        GetDialogItem(ctx->dlg, kProgOverallBar,
                      &itemType, &itemHandle, &itemRect);
        SetControlValue((ControlHandle)itemHandle, (short)overall_pct);
        sprintf(buf, "Overall: %ld%%", overall_pct);
        UpdateProgressText(ctx->dlg, kProgOverallLabel, buf);
    }

    /* pump events briefly to keep UI responsive and check Cancel */
    if (WaitNextEvent(everyEvent, &evt, 1, nil)) {
        if (IsDialogEvent(&evt)) {
            if (DialogSelect(&evt, &whichDlg, &item)) {
                if (whichDlg == ctx->dlg && item == kProgCancel) {
                    ctx->cancelled = true;
                    return 1;
                }
            }
        }
    }

    return 0;
}

/* ==== Classification ==================================================== */

static int IsBoltArchive(const FSSpec *spec)
{
    FInfo finfo;
    if (FSpGetFInfo(spec, &finfo) != noErr) return 0;
    return (finfo.fdType == kBoltFileType);
}

static int IsDirectory(const FSSpec *spec)
{
    CInfoPBRec cpb;
    Str255 name;

    memcpy(name, spec->name, spec->name[0] + 1);
    memset(&cpb, 0, sizeof(cpb));
    cpb.hFileInfo.ioNamePtr = name;
    cpb.hFileInfo.ioVRefNum = spec->vRefNum;
    cpb.hFileInfo.ioDirID = spec->parID;

    if (PBGetCatInfoSync(&cpb) != noErr) return 0;
    return (cpb.hFileInfo.ioFlAttrib & 0x10) != 0;
}

/* ==== Ask for Output Folder ============================================= */

static int AskOutputFolder(short *vRefNum, long *dirID)
{
    NavReplyRecord reply;
    NavDialogOptions opts;
    AEDesc desc;
    FSSpec folderSpec;
    OSErr err;

    if (!gHasNavServices) return 0;

    NavGetDefaultDialogOptions(&opts);
    opts.dialogOptionFlags |= kNavNoTypePopup;
    memcpy(opts.windowTitle, "\pChoose output folder", 21);

    err = NavChooseFolder(nil, &reply, &opts, nil, nil, nil);
    if (err != noErr || !reply.validRecord) {
        if (err == noErr) NavDisposeReply(&reply);
        return 0;
    }

    err = AEGetNthDesc(&reply.selection, 1, typeFSS, nil, &desc);
    if (err == noErr) {
        if (desc.dataHandle == nil ||
            GetHandleSize(desc.dataHandle) < (Size)sizeof(FSSpec)) {
            AEDisposeDesc(&desc);
            NavDisposeReply(&reply);
            return 0;
        }
        HLock(desc.dataHandle);
        memcpy(&folderSpec, *desc.dataHandle, sizeof(FSSpec));
        HUnlock(desc.dataHandle);
        AEDisposeDesc(&desc);
        *vRefNum = folderSpec.vRefNum;
        *dirID = folderSpec.parID;
        /* resolve to actual directory ID */
        {
            CInfoPBRec cpb;
            memset(&cpb, 0, sizeof(cpb));
            cpb.dirInfo.ioNamePtr = folderSpec.name;
            cpb.dirInfo.ioVRefNum = folderSpec.vRefNum;
            cpb.dirInfo.ioDrDirID = folderSpec.parID;
            if (PBGetCatInfoSync(&cpb) == noErr) {
                *dirID = cpb.dirInfo.ioDrDirID;
            }
        }
    }

    NavDisposeReply(&reply);
    return (err == noErr) ? 1 : 0;
}

/* ==== Mixed Items Dialog ================================================ */

/* returns: 1 = unpack, 2 = pack, 0 = cancel */
static int AskMixedAction(void)
{
    DialogPtr dlg;
    short item;
    int result = 0;

    dlg = GetNewDialog(rDlgMixed, nil, (WindowPtr)-1);
    if (dlg == nil) return 0;

    SetDialogDefaultItem(dlg, kMixedUnpack);
    SetDialogCancelItem(dlg, kMixedCancel);
    ShowWindow(GetDialogWindow(dlg));

    for (;;) {
        ModalDialog(nil, &item);
        if (item == kMixedUnpack)  { result = 1; break; }
        if (item == kMixedPack)    { result = 2; break; }
        if (item == kMixedCancel)  { result = 0; break; }
    }

    DisposeDialog(dlg);
    return result;
}

/* ==== Archive Name Dialog =============================================== */

static int AskArchiveName(const char *suggestion, char *outName, long maxLen)
{
    DialogPtr dlg;
    short item;
    short itemType;
    Handle itemHandle;
    Rect itemRect;
    Str255 pstr;
    long slen;

    dlg = GetNewDialog(rDlgName, nil, (WindowPtr)-1);
    if (dlg == nil) return 0;

    /* set suggested name in edit field */
    GetDialogItem(dlg, kNameField, &itemType, &itemHandle, &itemRect);
    slen = (long)strlen(suggestion);
    if (slen > 255) slen = 255;
    pstr[0] = (unsigned char)slen;
    memcpy(pstr + 1, suggestion, slen);
    SetDialogItemText(itemHandle, pstr);
    SelectDialogItemText(dlg, kNameField, 0, 32767);

    SetDialogDefaultItem(dlg, kNameOK);
    SetDialogCancelItem(dlg, kNameCancel);
    ShowWindow(GetDialogWindow(dlg));

    for (;;) {
        ModalDialog(nil, &item);
        if (item == kNameOK) {
            Str255 result;
            long len;
            GetDialogItem(dlg, kNameField, &itemType, &itemHandle, &itemRect);
            GetDialogItemText(itemHandle, result);
            len = result[0];
            if (len > maxLen - 1) len = maxLen - 1;
            memcpy(outName, result + 1, len);
            outName[len] = '\0';
            DisposeDialog(dlg);
            return 1;
        }
        if (item == kNameCancel) {
            DisposeDialog(dlg);
            return 0;
        }
    }
}

/* ==== Extraction ======================================================== */

typedef struct {
    FSSpec          spec;
    bp_uint64       mtime;
    bp_uint16       flags;
    unsigned short  depth;
} mac_dir_restore_t;

#define kMacExtractIndexCount 1024
#define kMacExtractIndexBytes 32768
typedef struct {
    unsigned short offsets[kMacExtractIndexCount];
    unsigned char types[kMacExtractIndexCount];
    unsigned short count;
    unsigned long used;
    int error;
    short rootVRefNum;
    long rootDirID;
    char names[kMacExtractIndexBytes];
} mac_extract_index_t;

typedef struct {
    progress_ctx_t  prog;
    short           outVRefNum;
    long            outDirID;
    FSSpec          lastDataSpec;
    FSSpec          lastDataFinalSpec;
    char            lastDataName[256];
    unsigned long   stageCounter;
    int             lastDataCreated;
    int             lastDataVerified;
    int             lastDataFinalized;
    bp_uint64       lastDataMTime;
    bp_uint16       lastDataFlags;
    mac_dir_restore_t *directories;
    unsigned short  directoryCount;
    mac_extract_index_t *nameIndex;
    unsigned char   metaBytes[10];
    long            metaPos;
    bp_io_t         metaIO;
    int             partial;
} extract_user_t;

#define kMacToUnixEpochOffset 2082844800UL
#define kMacDirectoryQueueCount 256

static bp_uint16 MacDateToArchive(unsigned long macDate, bp_uint64 *mtime)
{
    if (macDate < kMacToUnixEpochOffset) {
        *mtime = bp_u64_from_u32(0);
        return 0;
    }
    *mtime = bp_u64_from_u32((bp_uint32)(macDate - kMacToUnixEpochOffset));
    return BP_ENTRY_FLAG_MTIME_VALID;
}

static int MacRestoreFileDate(const FSSpec *spec, bp_uint64 mtime,
                             bp_uint16 flags)
{
    CInfoPBRec cpb;
    bp_uint32 macDate;
    OSErr err;

    if (!(flags & BP_ENTRY_FLAG_MTIME_VALID)) return BP_OK;
    if (mtime.hi != 0 || mtime.lo > 0xFFFFFFFFUL - kMacToUnixEpochOffset)
        return BP_ERR_PARTIAL;
    macDate = mtime.lo + kMacToUnixEpochOffset;
    memset(&cpb, 0, sizeof(cpb));
    cpb.hFileInfo.ioNamePtr = (StringPtr)spec->name;
    cpb.hFileInfo.ioVRefNum = spec->vRefNum;
    cpb.hFileInfo.ioDirID = spec->parID;
    err = PBGetCatInfoSync(&cpb);
    if (err != noErr) return BP_ERR_PARTIAL;
    cpb.hFileInfo.ioFlMdDat = macDate;
    return PBSetCatInfoSync(&cpb) == noErr ? BP_OK : BP_ERR_PARTIAL;
}

static int MacRestoreDirectoryDate(const FSSpec *spec, bp_uint64 mtime,
                                   bp_uint16 flags)
{
    CInfoPBRec cpb;
    bp_uint32 macDate;
    OSErr err;

    if (!(flags & BP_ENTRY_FLAG_MTIME_VALID)) return BP_OK;
    if (mtime.hi != 0 || mtime.lo > 0xFFFFFFFFUL - kMacToUnixEpochOffset)
        return BP_ERR_PARTIAL;
    macDate = mtime.lo + kMacToUnixEpochOffset;
    memset(&cpb, 0, sizeof(cpb));
    cpb.dirInfo.ioNamePtr = (StringPtr)spec->name;
    cpb.dirInfo.ioVRefNum = spec->vRefNum;
    cpb.dirInfo.ioDrDirID = spec->parID;
    err = PBGetCatInfoSync(&cpb);
    if (err != noErr) return BP_ERR_PARTIAL;
    cpb.dirInfo.ioDrMdDat = macDate;
    return PBSetCatInfoSync(&cpb) == noErr ? BP_OK : BP_ERR_PARTIAL;
}

static int MacQueueDirectoryTime(extract_user_t *eu,
                                 const bp_entry_t *entry,
                                 const FSSpec *spec)
{
    const char *p;
    unsigned short depth = 0;

    if (!(entry->flags & BP_ENTRY_FLAG_MTIME_VALID)) return BP_OK;
    if (eu->directories == nil ||
        eu->directoryCount >= kMacDirectoryQueueCount)
        return BP_ERR_PARTIAL;
    for (p = entry->name; *p != '\0'; p++)
        if (*p == '/') depth++;
    eu->directories[eu->directoryCount].spec = *spec;
    eu->directories[eu->directoryCount].mtime = entry->mtime;
    eu->directories[eu->directoryCount].flags = entry->flags;
    eu->directories[eu->directoryCount].depth = depth;
    eu->directoryCount++;
    return BP_OK;
}

static int MacRestoreDirectoryTimes(extract_user_t *eu)
{
    unsigned short i, j;
    int result = BP_OK;

    if (eu->directories == nil)
        return eu->directoryCount ? BP_ERR_PARTIAL : BP_OK;
    for (i = 0; i < eu->directoryCount; i++) {
        for (j = (unsigned short)(i + 1); j < eu->directoryCount; j++) {
            if (eu->directories[j].depth > eu->directories[i].depth) {
                mac_dir_restore_t tmp = eu->directories[i];
                eu->directories[i] = eu->directories[j];
                eu->directories[j] = tmp;
            }
        }
        if (MacRestoreDirectoryDate(&eu->directories[i].spec,
                                    eu->directories[i].mtime,
                                    eu->directories[i].flags) != BP_OK)
            result = BP_ERR_PARTIAL;
    }
    if (result != BP_OK) eu->partial = 1;
    return result;
}

static int MacStageNameReserved(extract_user_t *eu, const char *archiveName,
                                const char *stageName)
{
    char projected[256];
    const char *slash = strrchr(archiveName, '/');
    long parentLen = slash != nil ? (long)(slash - archiveName + 1) : 0;
    long stageLen = (long)strlen(stageName);
    Str255 key;
    const char *name;
    long len;
    unsigned short i;

    if (parentLen + stageLen >= (long)sizeof(projected)) return 1;
    memcpy(projected, archiveName, parentLen);
    memcpy(projected + parentLen, stageName, stageLen + 1);
    if (!ArchivePathToMac(projected, key)) return 1;
    UpperString(key, true);
    name = (const char *)(key + 1);
    len = key[0];
    if (eu->nameIndex == nil) return 1;
    for (i = 0; i < eu->nameIndex->count; i++) {
        const char *other = eu->nameIndex->names +
                            eu->nameIndex->offsets[i];
        long otherLen = (long)strlen(other);
        if ((len == otherLen && memcmp(name, other, len) == 0) ||
            (len < otherLen && memcmp(name, other, len) == 0 &&
             other[len] == ':') ||
            (otherLen < len && memcmp(other, name, otherLen) == 0 &&
             name[otherLen] == ':')) return 1;
    }
    return 0;
}

static OSErr MacCreateStagingFile(extract_user_t *eu,
                                  const FSSpec *finalSpec,
                                  FSSpec *stageSpec,
                                  const char *archiveName)
{
    unsigned long attempt;

    for (attempt = 0; attempt < 10000; attempt++) {
        char nameBuf[11];
        Str255 stageName;
        unsigned long number = eu->stageCounter++ % 10000UL;
        long nameLen = 10;
        OSErr err;

        nameBuf[0] = 'B'; nameBuf[1] = 'P';
        nameBuf[2] = (char)('0' + (number / 1000UL) % 10UL);
        nameBuf[3] = (char)('0' + (number / 100UL) % 10UL);
        nameBuf[4] = (char)('0' + (number / 10UL) % 10UL);
        nameBuf[5] = (char)('0' + number % 10UL);
        nameBuf[6] = '.'; nameBuf[7] = 'T'; nameBuf[8] = 'M';
        nameBuf[9] = 'P'; nameBuf[10] = '\0';
        if (MacStageNameReserved(eu, archiveName, nameBuf)) continue;
        stageName[0] = (unsigned char)nameLen;
        memcpy(stageName + 1, nameBuf, nameLen);
        err = FSMakeFSSpec(finalSpec->vRefNum, finalSpec->parID,
                           stageName, stageSpec);
        if (err == noErr) continue;
        if (err != fnfErr) return err;
        err = FSpCreate(stageSpec, kCreatorCode, '????', 0);
        if (err == dupFNErr) continue;
        return err;
    }
    return dupFNErr;
}

static int MacFinalizePendingFile(extract_user_t *eu)
{
    int result = BP_OK;
    FSSpec existing;
    OSErr err;

    if (!eu->lastDataVerified || eu->lastDataFinalized) return BP_OK;
    if (MacRestoreFileDate(&eu->lastDataSpec, eu->lastDataMTime,
                           eu->lastDataFlags) != BP_OK)
        result = BP_ERR_PARTIAL;
    err = FSMakeFSSpec(eu->lastDataFinalSpec.vRefNum,
                       eu->lastDataFinalSpec.parID,
                       eu->lastDataFinalSpec.name, &existing);
    if (err != fnfErr) {
        (void)FSpDelete(&eu->lastDataSpec);
        eu->lastDataCreated = 0;
        eu->partial = 1;
        eu->lastDataFinalized = 1;
        return BP_ERR_PARTIAL;
    }
    err = FSpRename(&eu->lastDataSpec, eu->lastDataFinalSpec.name);
    if (err != noErr) {
        (void)FSpDelete(&eu->lastDataSpec);
        eu->lastDataCreated = 0;
        result = BP_ERR_PARTIAL;
    } else {
        eu->lastDataCreated = 0;
    }
    if (result != BP_OK) eu->partial = 1;
    eu->lastDataFinalized = 1;
    return result;
}

static long MacExtractMetaWrite(void *handle, const void *buf,
                                bp_uint32 count)
{
    extract_user_t *eu = (extract_user_t *)handle;
    if (count > sizeof(eu->metaBytes) - eu->metaPos) return -1;
    memcpy(eu->metaBytes + eu->metaPos, buf, count);
    eu->metaPos += (long)count;
    return (long)count;
}

static int MacApplyFinderInfo(extract_user_t *eu)
{
    FInfo finfo;

    if (eu->metaPos != 10 || !eu->lastDataVerified) return BP_ERR_PARTIAL;
    if (FSpGetFInfo(&eu->lastDataSpec, &finfo) != noErr)
        return BP_ERR_PARTIAL;
    finfo.fdType = ((unsigned long)eu->metaBytes[0] << 24) |
                   ((unsigned long)eu->metaBytes[1] << 16) |
                   ((unsigned long)eu->metaBytes[2] << 8) |
                   (unsigned long)eu->metaBytes[3];
    finfo.fdCreator = ((unsigned long)eu->metaBytes[4] << 24) |
                      ((unsigned long)eu->metaBytes[5] << 16) |
                      ((unsigned long)eu->metaBytes[6] << 8) |
                      (unsigned long)eu->metaBytes[7];
    finfo.fdFlags = (short)(((unsigned short)eu->metaBytes[8] << 8) |
                            (unsigned short)eu->metaBytes[9]);
    return FSpSetFInfo(&eu->lastDataSpec, &finfo) == noErr
               ? BP_OK : BP_ERR_PARTIAL;
}

static OSErr MacComponentName(const char *utf8Name, long utf8Length,
                              Str255 macName)
{
    char segment[256];
    unsigned char macBuf[256];
    long macLen;

    if (utf8Length <= 0 || utf8Length > 255) return paramErr;
    memcpy(segment, utf8Name, utf8Length);
    segment[utf8Length] = '\0';
    if (!UTF8ToMacRomanStrict(segment, macBuf, (long)sizeof(macBuf)))
        return paramErr;
    macLen = (long)strlen((char *)macBuf);
    if (macLen <= 0 || macLen > 31) return paramErr;
    macName[0] = (unsigned char)macLen;
    memcpy(macName + 1, macBuf, macLen);
    return noErr;
}

static OSErr MacLookupDirectory(short vRefNum, long parentDirID,
                                const char *utf8Name, long utf8Length,
                                long *childDirID)
{
    Str255 macName;
    CInfoPBRec cpb;
    OSErr err;

    err = MacComponentName(utf8Name, utf8Length, macName);
    if (err != noErr) return err;
    memset(&cpb, 0, sizeof(cpb));
    cpb.dirInfo.ioNamePtr = macName;
    cpb.dirInfo.ioVRefNum = vRefNum;
    cpb.dirInfo.ioDrDirID = parentDirID;
    err = PBGetCatInfoSync(&cpb);
    if (err != noErr) return err;
    if (!(cpb.dirInfo.ioFlAttrib & 0x10)) return dupFNErr;
    *childDirID = cpb.dirInfo.ioDrDirID;
    return noErr;
}

static int MacCheckExistingPath(mac_extract_index_t *index,
                                const bp_entry_t *entry)
{
    const char *p = entry->name;
    long dirID = index->rootDirID;

    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        long len = slash != nil ? (long)(slash - p) : (long)strlen(p);
        int terminal = slash == nil ||
                       (entry->entry_type == BP_TYPE_DIR &&
                        slash[1] == '\0');
        OSErr err;

        if (len <= 0 || len > 255) return BP_ERR_PARTIAL;
        if (terminal && entry->entry_type != BP_TYPE_DIR) {
            Str255 name;
            FSSpec spec;
            err = MacComponentName(p, len, name);
            if (err != noErr) return BP_ERR_PARTIAL;
            err = FSMakeFSSpec(index->rootVRefNum, dirID, name, &spec);
            if (err == noErr) return BP_ERR_PARTIAL;
            return err == fnfErr ? BP_OK : BP_ERR_PARTIAL;
        }

        {
            long childDirID;
            err = MacLookupDirectory(index->rootVRefNum, dirID, p, len,
                                     &childDirID);
            if (err == fnfErr) return BP_OK;
            if (err != noErr) return BP_ERR_PARTIAL;
            dirID = childDirID;
        }
        if (terminal) return BP_OK;
        p = slash + 1;
    }
    return BP_ERR_PARTIAL;
}

static int MacExtractPreflight(const bp_entry_t *entry, void *user_data)
{
    mac_extract_index_t *index = (mac_extract_index_t *)user_data;
    Str255 key;
    const char *name;
    long len;
    unsigned short i;

    if (entry->entry_type != BP_TYPE_DATA &&
        entry->entry_type != BP_TYPE_DIR &&
        entry->entry_type != BP_TYPE_SYMLINK) return 0;
    if (!ArchivePathToMac(entry->name, key)) {
        index->error = BP_ERR_PARTIAL;
        return 1;
    }
    if (MacCheckExistingPath(index, entry) != BP_OK) {
        index->error = BP_ERR_PARTIAL;
        return 1;
    }
    UpperString(key, true);
    name = (const char *)(key + 1);
    len = key[0];
    for (i = 0; i < index->count; i++) {
        const char *oldName = index->names + index->offsets[i];
        long oldLen = (long)strlen(oldName);

        if ((oldLen == len && memcmp(oldName, name, len) == 0) ||
            (oldLen < len &&
             memcmp(oldName, name, oldLen) == 0 && name[oldLen] == ':' &&
             index->types[i] != BP_TYPE_DIR) ||
            (len < oldLen &&
             memcmp(name, oldName, len) == 0 && oldName[len] == ':' &&
             entry->entry_type != BP_TYPE_DIR)) {
            index->error = BP_ERR_PARTIAL;
            return 1;
        }
    }
    if (index->count >= kMacExtractIndexCount ||
        index->used + (unsigned long)len + 1UL > kMacExtractIndexBytes) {
        index->error = BP_ERR_PARTIAL;
        return 1;
    }
    index->offsets[index->count] = (unsigned short)index->used;
    index->types[index->count] = entry->entry_type;
    memcpy(index->names + index->used, name, len);
    index->names[index->used + len] = '\0';
    index->used += (unsigned long)len + 1UL;
    index->count++;
    return 0;
}

static OSErr MacEnsureDirectory(short vRefNum, long parentDirID,
                                const char *utf8Name, long utf8Length,
                                long *childDirID)
{
    Str255 macName;
    OSErr err;

    err = MacLookupDirectory(vRefNum, parentDirID, utf8Name, utf8Length,
                             childDirID);
    if (err == noErr) return noErr;
    if (err != fnfErr) return err;
    err = MacComponentName(utf8Name, utf8Length, macName);
    if (err != noErr) return err;
    return DirCreate(vRefNum, parentDirID, macName, childDirID);
}

static OSErr MacCreateArchiveDirectory(extract_user_t *eu,
                                       const char *archivePath,
                                       FSSpec *directorySpec)
{
    const char *p = archivePath;
    short vRefNum = eu->outVRefNum;
    long dirID = eu->outDirID;
    long finalParentDirID = eu->outDirID;
    Str255 finalName;
    int haveFinal = 0;

    while (*p != '\0') {
        const char *slash = strchr(p, '/');
        long len = slash != nil ? (long)(slash - p) : (long)strlen(p);
        long childDirID;
        OSErr err;

        if (len <= 0) return paramErr;
        if (slash == nil || slash[1] == '\0') {
            err = MacComponentName(p, len, finalName);
            if (err != noErr) return err;
            finalParentDirID = dirID;
            haveFinal = 1;
        }
        err = MacEnsureDirectory(vRefNum, dirID, p, len, &childDirID);
        if (err != noErr) return err;
        dirID = childDirID;
        if (slash == nil) break;
        p = slash + 1;
        if (*p == '\0') break;
    }
    if (!haveFinal) return paramErr;
    return FSMakeFSSpec(vRefNum, finalParentDirID, finalName, directorySpec);
}

static bp_io_t *ExtractOpen(const bp_entry_t *entry, void *user_data)
{
    extract_user_t *eu = (extract_user_t *)user_data;
    Str255 macName;
    FSSpec outSpec;
    short refNum;
    mac_file_t *mf;
    bp_io_t *io;
    OSErr err;
    const char *lastSlash;
    char nameOnly[256];
    long nlen;

    if (entry->entry_type == BP_TYPE_DIR) {
        FSSpec directorySpec;
        (void)MacFinalizePendingFile(eu);
        eu->lastDataVerified = 0;
        eu->lastDataCreated = 0;
        eu->lastDataName[0] = '\0';
        err = MacCreateArchiveDirectory(eu, entry->name, &directorySpec);
        if (err != noErr) eu->partial = 1;
        else if (MacQueueDirectoryTime(eu, entry, &directorySpec) != BP_OK)
            eu->partial = 1;
        return nil;
    }

    if (entry->entry_type == BP_TYPE_DATA) {
        (void)MacFinalizePendingFile(eu);
        eu->lastDataVerified = 0;
        eu->lastDataCreated = 0;
        eu->lastDataFinalized = 0;
        eu->lastDataName[0] = '\0';
        if (strlen(entry->name) >= sizeof(eu->lastDataName)) {
            eu->partial = 1;
            return nil;
        }
    } else if (entry->entry_type == BP_TYPE_RSRC) {
        if (!eu->lastDataVerified ||
            strcmp(eu->lastDataName, entry->name) != 0) {
            eu->partial = 1;
            return nil;
        }
        outSpec = eu->lastDataSpec;
        goto open_fork;
    } else if (entry->entry_type == BP_TYPE_META &&
               entry->platform == BP_OS_MACOS9 &&
               entry->original_size.hi == 0 &&
               entry->original_size.lo == 10 && eu->lastDataVerified) {
        eu->metaPos = 0;
        eu->metaIO.read = nil;
        eu->metaIO.write = MacExtractMetaWrite;
        eu->metaIO.seek = nil;
        eu->metaIO.tell = nil;
        eu->metaIO.handle = eu;
        return &eu->metaIO;
    } else {
        /* Foreign metadata and links need target-specific handling. */
        eu->partial = 1;
        return nil;
    }

    /* get just the filename from the archive path */
    lastSlash = strrchr(entry->name, '/');
    if (lastSlash != nil) {
        lastSlash++;
    } else {
        lastSlash = entry->name;
    }
    nlen = (long)strlen(lastSlash);
    if (nlen > 255) { eu->partial = 1; return nil; }
    memcpy(nameOnly, lastSlash, nlen);
    nameOnly[nlen] = '\0';

    /* convert to Pascal string */
    {
        unsigned char macBuf[256];
        if (!UTF8ToMacRomanStrict(nameOnly, macBuf, 256)) {
            eu->partial = 1;
            return nil;
        }
        nlen = (long)strlen((char *)macBuf);
        if (nlen <= 0 || nlen > 31) { eu->partial = 1; return nil; }
        macName[0] = (unsigned char)nlen;
        memcpy(macName + 1, macBuf, nlen);
    }

    /* build the output FSSpec */
    /* for nested paths, we need to resolve or create parent directories */
    {
        short parentVRef = eu->outVRefNum;
        long parentDirID = eu->outDirID;
        const char *p = entry->name;
        const char *seg;

        /* walk the path, creating directories as needed */
        while ((seg = strchr(p, '/')) != nil) {
            long nlen2;
            long createdDirID2;
            nlen2 = (long)(seg - p);
            if (nlen2 == 0 || nlen2 > 255) {
                eu->partial = 1;
                return nil;
            }
            err = MacEnsureDirectory(parentVRef, parentDirID, p, nlen2,
                                     &createdDirID2);
            if (err != noErr) { eu->partial = 1; return nil; }
            parentDirID = createdDirID2;
            p = seg + 1;
        }

        err = FSMakeFSSpec(parentVRef, parentDirID, macName, &outSpec);
        if (err == noErr) {
            /* Never truncate or replace an existing destination. */
            eu->partial = 1;
            return nil;
        }
    if (err != fnfErr) { eu->partial = 1; return nil; }
    }

    eu->lastDataFinalSpec = outSpec;
    err = MacCreateStagingFile(eu, &eu->lastDataFinalSpec, &outSpec,
                               entry->name);
    if (err != noErr) { eu->partial = 1; return nil; }
    eu->lastDataSpec = outSpec;
    strcpy(eu->lastDataName, entry->name);
    eu->lastDataCreated = 1;

    /* open the appropriate fork */
open_fork:
    if (entry->entry_type == BP_TYPE_RSRC) {
        err = FSpOpenRF(&outSpec, fsWrPerm, &refNum);
    } else {
        err = FSpOpenDF(&outSpec, fsWrPerm, &refNum);
    }
    if (err != noErr) {
        eu->partial = 1;
        if (entry->entry_type == BP_TYPE_DATA && eu->lastDataCreated)
            (void)FSpDelete(&outSpec);
        return nil;
    }
    err = SetEOF(refNum, 0);
    if (err != noErr) {
        (void)FSClose(refNum);
        eu->partial = 1;
        if (entry->entry_type == BP_TYPE_DATA && eu->lastDataCreated)
            (void)FSpDelete(&outSpec);
        return nil;
    }

    mf = (mac_file_t *)malloc(sizeof(mac_file_t));
    if (mf == nil) {
        FSClose(refNum);
        eu->partial = 1;
        if (entry->entry_type == BP_TYPE_DATA && eu->lastDataCreated)
            (void)FSpDelete(&outSpec);
        return nil;
    }

    io = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (io == nil) {
        FSClose(refNum); free(mf); eu->partial = 1;
        if (entry->entry_type == BP_TYPE_DATA && eu->lastDataCreated)
            (void)FSpDelete(&outSpec);
        return nil;
    }

    MacIOFromRefNum(io, mf, refNum);
    return io;
}

static int ExtractDoneStatus(const bp_entry_t *entry, bp_io_t *out,
                            int status, void *user_data)
{
    extract_user_t *eu = (extract_user_t *)user_data;
    mac_file_t *mf;
    OSErr err;

    if (out == nil) return status;

    if (entry->entry_type == BP_TYPE_META && out == &eu->metaIO) {
        if (status != BP_OK) return status;
        status = MacApplyFinderInfo(eu);
        if (status != BP_OK) eu->partial = 1;
        return status;
    }

    mf = (mac_file_t *)out->handle;
    err = FSClose(mf->refNum);
    free(mf);
    free(out);
    if (err != noErr && status == BP_OK) status = BP_ERR_IO;

    if (entry->entry_type == BP_TYPE_DATA &&
        strcmp(eu->lastDataName, entry->name) == 0) {
        if (status == BP_OK) {
            eu->lastDataVerified = 1;
            eu->lastDataMTime = entry->mtime;
            eu->lastDataFlags = entry->flags;
        } else if (eu->lastDataCreated) {
            (void)FSpDelete(&eu->lastDataSpec);
            eu->lastDataCreated = 0;
            eu->lastDataName[0] = '\0';
        }
    } else if (entry->entry_type == BP_TYPE_RSRC && status != BP_OK &&
               eu->lastDataCreated &&
               strcmp(eu->lastDataName, entry->name) == 0) {
        (void)FSpDelete(&eu->lastDataSpec);
        eu->lastDataCreated = 0;
        eu->lastDataVerified = 0;
        eu->lastDataName[0] = '\0';
    }
    return status;
}

/* ==== Unpack Operation ================================================== */

static int DoUnpack(FSSpec *archives, long count)
{
    long i;
    int rc;
    int overall = BP_OK;

    rc = bp_compress_init();
    if (rc != BP_OK) {
        ShowError(rc);
        return rc;
    }

    for (i = 0; i < count; i++) {
        short refNum;
        mac_file_t mf;
        bp_io_t aio;
        extract_user_t eu;
        mac_extract_index_t *index;
        OSErr err;

        err = FSpOpenDF(&archives[i], fsRdPerm, &refNum);
        if (err != noErr) {
            ShowErrorStr("Cannot open archive file.");
            if (overall == BP_OK) overall = BP_ERR_IO;
            continue;
        }
        MacIOFromRefNum(&aio, &mf, refNum);

        memset(&eu, 0, sizeof(eu));
        eu.outVRefNum = archives[i].vRefNum;
        eu.outDirID = archives[i].parID;
        if (gPrefs.ask_extract_location && gHasNavServices) {
            if (!AskOutputFolder(&eu.outVRefNum, &eu.outDirID)) {
                FSClose(refNum);
                continue;
            }
        }

        eu.directories = (mac_dir_restore_t *)malloc(
            sizeof(mac_dir_restore_t) * kMacDirectoryQueueCount);
        if (eu.directories == nil) eu.partial = 1;

        index = (mac_extract_index_t *)malloc(sizeof(mac_extract_index_t));
        if (index == nil) {
            ShowError(BP_ERR_NOMEM);
            free(eu.directories);
            FSClose(refNum);
            if (overall == BP_OK) overall = BP_ERR_NOMEM;
            continue;
        }
        memset(index, 0, sizeof(*index));
        index->rootVRefNum = eu.outVRefNum;
        index->rootDirID = eu.outDirID;
        rc = bp_archive_list(&aio, MacExtractPreflight, index);
        if (rc != BP_OK) {
            if (index->error != BP_OK) rc = index->error;
            free(index);
            free(eu.directories);
            FSClose(refNum);
            if (rc == BP_ERR_PARTIAL)
                ShowErrorStr("Archive names or destinations cannot be represented safely.");
            else
                ShowError(rc);
            if (overall == BP_OK) overall = rc;
            continue;
        }
        eu.nameIndex = index;

        ShowProgressDialog(&eu.prog);

        rc = bp_archive_extract_all_ex(&aio, ExtractOpen, ExtractDoneStatus,
                                       ProgressCallback, &eu);
        if (MacFinalizePendingFile(&eu) != BP_OK && rc == BP_OK)
            rc = BP_ERR_PARTIAL;
        if (MacRestoreDirectoryTimes(&eu) != BP_OK && rc == BP_OK)
            rc = BP_ERR_PARTIAL;
        if (eu.partial && rc == BP_OK) rc = BP_ERR_PARTIAL;

        HideProgressDialog(&eu.prog);
        free(index);
        free(eu.directories);
        FSClose(refNum);

        if (rc != BP_OK) {
            if (rc == BP_ERR_PARTIAL)
                ShowErrorStr("Some files or attributes could not be restored.");
            else if (rc != BP_ERR_ABORTED)
                ShowError(rc);
            if (overall == BP_OK) overall = rc;
        }
    }

    return overall;
}

/* ==== Pack: Directory Recursion ========================================= */

static int AddFileToArchive(bp_archive_t *ar, short vRefNum, long dirID,
                            Str255 fileName, const char *arcName,
                            bp_uint64 mtime)
{
    FSSpec fileSpec;
    short refNum;
    mac_file_t mf;
    bp_io_t fio;
    CInfoPBRec cpb;
    long dataSize, rsrcSize;
    int rc;
    FInfo finfo;
    OSErr err;
    bp_uint16 mtimeFlags;

    /* get file info: sizes and Finder info */
    memset(&cpb, 0, sizeof(cpb));
    cpb.hFileInfo.ioNamePtr = fileName;
    cpb.hFileInfo.ioVRefNum = vRefNum;
    cpb.hFileInfo.ioDirID = dirID;
    if (PBGetCatInfoSync(&cpb) != noErr) return BP_ERR_IO;

    dataSize = cpb.hFileInfo.ioFlLgLen;
    rsrcSize = cpb.hFileInfo.ioFlRLgLen;
    mtimeFlags = MacDateToArchive(cpb.hFileInfo.ioFlMdDat, &mtime);

    FSMakeFSSpec(vRefNum, dirID, fileName, &fileSpec);

    /* archive data fork */
    if (dataSize > 0) {
        err = FSpOpenDF(&fileSpec, fsRdPerm, &refNum);
        if (err != noErr) return BP_ERR_IO;
        MacIOFromRefNum(&fio, &mf, refNum);

        rc = bp_archive_add_entry_ex(ar, arcName, BP_TYPE_DATA, BP_OS_MACOS9,
                                    &fio, bp_u64_from_u32((bp_uint32)dataSize),
                                    mtime, mtimeFlags);
        FSClose(refNum);
        if (rc != BP_OK) return rc;
    } else {
        /* zero-length data fork — still add an entry */
        rc = bp_archive_add_entry_ex(ar, arcName, BP_TYPE_DATA, BP_OS_MACOS9,
                                     nil, bp_u64_from_u32(0), mtime,
                                     mtimeFlags);
        if (rc != BP_OK) return rc;
    }

    /* archive resource fork if non-empty */
    if (rsrcSize > 0) {
        char rsrcArcName[kMaxPath];
        ConcatArchivePath(rsrcArcName, kMaxPath, "", arcName);

        err = FSpOpenRF(&fileSpec, fsRdPerm, &refNum);
        if (err != noErr) return BP_ERR_IO;
        MacIOFromRefNum(&fio, &mf, refNum);

        rc = bp_archive_add_entry_ex(ar, arcName, BP_TYPE_RSRC, BP_OS_MACOS9,
                                     &fio, bp_u64_from_u32((bp_uint32)rsrcSize),
                                     mtime, mtimeFlags);
        FSClose(refNum);
        if (rc != BP_OK) return rc;
    }

    /* archive Finder info as metadata */
    if (FSpGetFInfo(&fileSpec, &finfo) == noErr) {
        /* Finder info: type(4) + creator(4) + flags(2) = 10 bytes */
        unsigned char metaBuf[10];
        FILE *tmpFile;

        metaBuf[0] = (unsigned char)((finfo.fdType >> 24) & 0xFF);
        metaBuf[1] = (unsigned char)((finfo.fdType >> 16) & 0xFF);
        metaBuf[2] = (unsigned char)((finfo.fdType >> 8) & 0xFF);
        metaBuf[3] = (unsigned char)(finfo.fdType & 0xFF);
        metaBuf[4] = (unsigned char)((finfo.fdCreator >> 24) & 0xFF);
        metaBuf[5] = (unsigned char)((finfo.fdCreator >> 16) & 0xFF);
        metaBuf[6] = (unsigned char)((finfo.fdCreator >> 8) & 0xFF);
        metaBuf[7] = (unsigned char)(finfo.fdCreator & 0xFF);
        metaBuf[8] = (unsigned char)((finfo.fdFlags >> 8) & 0xFF);
        metaBuf[9] = (unsigned char)(finfo.fdFlags & 0xFF);

        /*
         * We need a bp_io_t to pass 10 bytes. Use a temporary file
         * since we cannot use a memory stream in C89.
         */
        tmpFile = tmpfile();
        if (tmpFile != nil) {
            fwrite(metaBuf, 1, 10, tmpFile);
            rewind(tmpFile);

            /* wrap the temp FILE* with the stdio-based approach.
             * We reuse the Mac I/O but tmpfile() returns a FILE* which
             * does not have a Mac refNum. We create a simple wrapper. */
            {
                bp_io_t tmpIO;
                tmpIO.read = MacTmpRead;
                tmpIO.write = nil;
                tmpIO.seek = MacTmpSeek;
                tmpIO.tell = MacTmpTell;
                tmpIO.handle = tmpFile;

                rc = bp_archive_add_entry_ex(ar, arcName, BP_TYPE_META,
                                             BP_OS_MACOS9, &tmpIO,
                                             bp_u64_from_u32(10), mtime,
                                             mtimeFlags);
            }
            fclose(tmpFile);
            if (rc != BP_OK) return rc;
        }
    }

    return BP_OK;
}

/*
 * Simple stdio-based bp_io_t callbacks for metadata temp files.
 * CodeWarrior's MSL C library provides stdio on Mac OS 9.
 */

static long MacTmpRead(void *handle, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)handle);
}

static int MacTmpSeek(void *handle, bp_off_t offset, int origin)
{
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t MacTmpTell(void *handle)
{
    return (bp_off_t)ftell((FILE *)handle);
}

static int AddDirectoryToArchive(bp_archive_t *ar, short vRefNum, long dirID,
                                 const char *arcPrefix)
{
    CInfoPBRec cpb;
    Str255 itemName;
    short index;

    for (index = 1; ; index++) {
        char utf8Name[512];
        char childArc[kMaxPath];
        int isDir;
        bp_uint64 archiveMTime;
        bp_uint16 archiveMTimeFlags;

        memset(&cpb, 0, sizeof(cpb));
        itemName[0] = 0;
        cpb.hFileInfo.ioNamePtr = itemName;
        cpb.hFileInfo.ioVRefNum = vRefNum;
        cpb.hFileInfo.ioDirID = dirID;
        cpb.hFileInfo.ioFDirIndex = index;

        if (PBGetCatInfoSync(&cpb) != noErr) break; /* no more items */

        /* convert Mac name to UTF-8 */
        {
            unsigned char nameBuf[256];
            PascalToBuf(itemName, nameBuf, sizeof(nameBuf));
            MacRomanToUTF8(nameBuf, utf8Name, sizeof(utf8Name));
        }

        isDir = (cpb.hFileInfo.ioFlAttrib & 0x10) != 0;

        if (isDir) {
            long childDirID = cpb.dirInfo.ioDrDirID;
            int rc;

            ConcatArchivePath(childArc, kMaxPath, arcPrefix, utf8Name);
            /* append '/' for directory */
            {
                long len = (long)strlen(childArc);
                if (len < kMaxPath - 1) {
                    childArc[len] = '/';
                    childArc[len + 1] = '\0';
                }
            }

            archiveMTimeFlags = MacDateToArchive(cpb.dirInfo.ioDrMdDat,
                                                 &archiveMTime);
            rc = bp_archive_add_entry_ex(ar, childArc, BP_TYPE_DIR,
                                         BP_OS_MACOS9, nil,
                                         bp_u64_from_u32(0), archiveMTime,
                                         archiveMTimeFlags);
            if (rc != BP_OK) return rc;

            rc = AddDirectoryToArchive(ar, vRefNum, childDirID, childArc);
            if (rc != BP_OK) return rc;

        } else {
            int rc;
            ConcatArchivePath(childArc, kMaxPath, arcPrefix, utf8Name);

            rc = AddFileToArchive(ar, vRefNum, dirID, itemName, childArc,
                                  bp_u64_from_u32(cpb.hFileInfo.ioFlMdDat));
            if (rc != BP_OK) return rc;
        }
    }

    return BP_OK;
}

static int AddFSSpecToArchive(bp_archive_t *ar, const FSSpec *spec,
                              const char *arcPrefix)
{
    if (IsDirectory(spec)) {
        CInfoPBRec cpb;
        Str255 name;
        long theDirID;
        char dirArc[kMaxPath];
        char utf8Name[512];
        int rc;

        /* get directory ID */
        memcpy(name, spec->name, spec->name[0] + 1);
        memset(&cpb, 0, sizeof(cpb));
        cpb.dirInfo.ioNamePtr = name;
        cpb.dirInfo.ioVRefNum = spec->vRefNum;
        cpb.dirInfo.ioDrDirID = spec->parID;
        if (PBGetCatInfoSync(&cpb) != noErr) return BP_ERR_IO;
        theDirID = cpb.dirInfo.ioDrDirID;

        {
            unsigned char nameBuf[256];
            PascalToBuf(spec->name, nameBuf, sizeof(nameBuf));
            MacRomanToUTF8(nameBuf, utf8Name, sizeof(utf8Name));
        }
        ConcatArchivePath(dirArc, kMaxPath, arcPrefix, utf8Name);
        {
            long len = (long)strlen(dirArc);
            if (len < kMaxPath - 1) {
                dirArc[len] = '/';
                dirArc[len + 1] = '\0';
            }
        }

        {
            bp_uint64 archiveMTime;
            bp_uint16 archiveMTimeFlags;
            archiveMTimeFlags = MacDateToArchive(cpb.dirInfo.ioDrMdDat,
                                                 &archiveMTime);
            rc = bp_archive_add_entry_ex(ar, dirArc, BP_TYPE_DIR,
                                         BP_OS_MACOS9, nil,
                                         bp_u64_from_u32(0), archiveMTime,
                                         archiveMTimeFlags);
        }
        if (rc != BP_OK) return rc;

        return AddDirectoryToArchive(ar, spec->vRefNum, theDirID, dirArc);

    } else {
        char utf8Name[512];
        char fileArc[kMaxPath];
        Str255 nameCopy;
        unsigned char nameBuf[256];

        PascalToBuf(spec->name, nameBuf, sizeof(nameBuf));
        MacRomanToUTF8(nameBuf, utf8Name, sizeof(utf8Name));
        ConcatArchivePath(fileArc, kMaxPath, arcPrefix, utf8Name);

        memcpy(nameCopy, spec->name, spec->name[0] + 1);
        return AddFileToArchive(ar, spec->vRefNum, spec->parID,
                                nameCopy, fileArc, bp_u64_from_u32(0));
    }
}

/* ==== Pack Operation ==================================================== */

static int DoPack(FSSpec *items, long count)
{
    char archiveName[256];
    Str255 macArchiveName;
    FSSpec outSpec;
    short refNum;
    mac_file_t mf;
    bp_io_t aio;
    bp_archive_t ar;
    progress_ctx_t prog;
    bp_uint32 blockSize;
    int rc, interchangeProfile;
    long i;
    OSErr err;

    rc = bp_compress_init();
    if (rc != BP_OK) {
        ShowError(rc);
        return rc;
    }

    interchangeProfile = AskInterchangeProfile();
    if (interchangeProfile < 0) return BP_ERR_ABORTED;

    /* determine archive name */
    if (count == 1) {
        /* use the item's name + .bolt */
        char utf8Name[256];
        unsigned char nameBuf[256];
        PascalToBuf(items[0].name, nameBuf, sizeof(nameBuf));
        MacRomanToUTF8(nameBuf, utf8Name, sizeof(utf8Name));
        sprintf(archiveName, "%s.bolt", utf8Name);
    } else {
        /* ask user for a name */
        if (!AskArchiveName("Archive.bolt", archiveName, sizeof(archiveName))) {
            return BP_ERR_ABORTED;
        }
    }

    /* ensure .bolt extension */
    {
        long len = (long)strlen(archiveName);
        if (len < 5 || strcmp(archiveName + len - 5, ".bolt") != 0) {
            if (len + 5 < (long)sizeof(archiveName)) {
                strcat(archiveName, ".bolt");
            }
        }
    }

    /* create the output file in the same folder as the first item */
    {
        unsigned char macBuf[256];
        long nlen;
        UTF8ToMacRoman(archiveName, macBuf, 255);
        nlen = (long)strlen((char *)macBuf);
        if (nlen > 31) nlen = 31; /* HFS name limit */
        macArchiveName[0] = (unsigned char)nlen;
        memcpy(macArchiveName + 1, macBuf, nlen);
    }

    err = FSMakeFSSpec(items[0].vRefNum, items[0].parID,
                       macArchiveName, &outSpec);
    if (err != noErr && err != fnfErr) {
        ShowErrorStr("Cannot create archive file path.");
        return BP_ERR_IO;
    }

    FSpDelete(&outSpec); /* remove if exists */
    FSpCreate(&outSpec, kCreatorCode, kBoltFileType, 0);

    err = FSpOpenDF(&outSpec, fsRdWrPerm, &refNum);
    if (err != noErr) {
        ShowErrorStr("Cannot open archive for writing.");
        return BP_ERR_IO;
    }

    MacIOFromRefNum(&aio, &mf, refNum);

    if (interchangeProfile) {
        blockSize = BP_BLOCK_16K;
        rc = bp_archive_create_profile(&ar, &aio,
                                       BP_PROFILE_INTERCHANGE_1);
    } else {
        blockSize = ChooseBlockSize();
        rc = bp_archive_create(&ar, &aio, blockSize,
                               ChooseCompressionMethod());
    }
    if (rc != BP_OK) {
        FSClose(refNum);
        ShowError(rc);
        return rc;
    }

    ar.creator_os = BP_OS_MACOS9;
    ar.creator_cpu = BP_CPU_PPC32;

    ShowProgressDialog(&prog);
    ar.progress_fn = ProgressCallback;
    ar.progress_data = &prog;

    for (i = 0; i < count; i++) {
        rc = AddFSSpecToArchive(&ar, &items[i], "");
        if (rc != BP_OK) break;
    }

    if (rc == BP_OK) {
        rc = bp_archive_finalize(&ar);
    }

    HideProgressDialog(&prog);
    bp_archive_close(&ar);
    FSClose(refNum);

    if (rc != BP_OK && rc != BP_ERR_ABORTED) {
        ShowError(rc);
        FSpDelete(&outSpec);
    }

    return rc;
}

/* ==== Classify and Act ================================================== */

static void ClassifyAndAct(FSSpec *items, long count)
{
    long archiveCount = 0;
    long i;
    FSSpec archives[kMaxDropItems];

    for (i = 0; i < count && i < kMaxDropItems; i++) {
        if (IsBoltArchive(&items[i])) {
            archives[archiveCount++] = items[i];
        }
    }

    if (gMode == kModeExpand) {
        /* Expand mode: unpack .bolt archives; ignore non-archives */
        if (archiveCount > 0)
            DoUnpack(archives, archiveCount);
    } else {
        /* Compress mode: pack everything dropped */
        if (count > 0)
            DoPack(items, count);
    }
}

/* ==== Apple Event Handlers ============================================== */

static pascal OSErr AEOpenApp(const AppleEvent *evt, AppleEvent *reply,
                              long refcon)
{
    (void)evt; (void)reply; (void)refcon;
    return noErr;
}

static pascal OSErr AEOpenDocs(const AppleEvent *evt, AppleEvent *reply,
                               long refcon)
{
    AEDescList docList;
    long itemCount;
    FSSpec items[kMaxDropItems];
    long i;
    OSErr err;

    (void)reply; (void)refcon;

    err = AEGetParamDesc(evt, keyDirectObject, typeAEList, &docList);
    if (err != noErr) return err;

    err = AECountItems(&docList, &itemCount);
    if (err != noErr) {
        AEDisposeDesc(&docList);
        return err;
    }

    if (itemCount > kMaxDropItems) itemCount = kMaxDropItems;

    for (i = 1; i <= itemCount; i++) {
        AEKeyword keyword;
        DescType typeCode;
        Size actualSize;

        err = AEGetNthPtr(&docList, i, typeFSS, &keyword, &typeCode,
                          &items[i - 1], sizeof(FSSpec), &actualSize);
        if (err != noErr) {
            AEDisposeDesc(&docList);
            return err;
        }
    }

    AEDisposeDesc(&docList);

    ClassifyAndAct(items, itemCount);
    return noErr;
}

static pascal OSErr AEPrintDocs(const AppleEvent *evt, AppleEvent *reply,
                                long refcon)
{
    (void)evt; (void)reply; (void)refcon;
    return errAEEventNotHandled;
}

static pascal OSErr AEQuit(const AppleEvent *evt, AppleEvent *reply,
                           long refcon)
{
    (void)evt; (void)reply; (void)refcon;
    gQuit = true;
    return noErr;
}

static void InitAppleEvents(void)
{
    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication,
                          NewAEEventHandlerUPP(AEOpenApp), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments,
                          NewAEEventHandlerUPP(AEOpenDocs), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments,
                          NewAEEventHandlerUPP(AEPrintDocs), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication,
                          NewAEEventHandlerUPP(AEQuit), 0, false);
}

/* ==== Drag Manager ====================================================== */

static pascal OSErr DragTrackingHandler(DragTrackingMessage message,
                                        WindowPtr window, void *refcon,
                                        DragReference drag)
{
    RgnHandle hiliteRgn;

    (void)refcon; (void)drag;

    switch (message) {
    case kDragTrackingEnterWindow:
        hiliteRgn = NewRgn();
        if (hiliteRgn != nil) {
            Rect portRect;
            GetWindowPortBounds(window, &portRect);
            RectRgn(hiliteRgn, &portRect);
            SetPort((GrafPtr)GetWindowPort(window));
            ShowDragHilite(drag, hiliteRgn, true);
            DisposeRgn(hiliteRgn);
        }
        break;

    case kDragTrackingLeaveWindow:
        SetPort((GrafPtr)GetWindowPort(window));
        HideDragHilite(drag);
        break;
    }

    return noErr;
}

static pascal OSErr DragReceiveHandler(WindowPtr window, void *refcon,
                                       DragReference drag)
{
    UInt16 itemCount;
    UInt16 i;
    FSSpec items[kMaxDropItems];
    long collected = 0;

    (void)window; (void)refcon;

    CountDragItems(drag, &itemCount);
    if (itemCount > kMaxDropItems) itemCount = kMaxDropItems;

    for (i = 1; i <= itemCount; i++) {
        DragItemRef itemRef;
        HFSFlavor hfsData;
        Size dataSize;

        GetDragItemReferenceNumber(drag, i, &itemRef);
        dataSize = sizeof(HFSFlavor);

        if (GetFlavorData(drag, itemRef, flavorTypeHFS, &hfsData,
                          &dataSize, 0) == noErr) {
            items[collected++] = hfsData.fileSpec;
        }
    }

    if (collected > 0) {
        ClassifyAndAct(items, collected);
    }

    return noErr;
}

static void InitDragManager(void)
{
    if (gMainWindow == nil) return;
    InstallTrackingHandler(
        NewDragTrackingHandlerUPP(DragTrackingHandler),
        gMainWindow, nil);
    InstallReceiveHandler(
        NewDragReceiveHandlerUPP(DragReceiveHandler),
        gMainWindow, nil);
}

/* ==== Menu Handling ===================================================== */

static void InitMenuBar(void)
{
    Handle mbar;

    mbar = GetNewMBar(rMenuBar);
    if (mbar != nil) {
        SetMenuBar(mbar);
        DisposeHandle(mbar);
    }
    AppendResMenu(GetMenuHandle(mApple), 'DRVR');
    DrawMenuBar();
}

static void HandleMenuChoice(long menuChoice)
{
    short menuID = HiWord(menuChoice);
    short menuItem = LoWord(menuChoice);

    switch (menuID) {
    case mApple:
        if (menuItem == iAbout) {
            DoAbout();
        } else {
            Str255 daName;
            GetMenuItemText(GetMenuHandle(mApple), menuItem, daName);
            OpenDeskAcc(daName);
        }
        break;

    case mFile:
        switch (menuItem) {
        case iFileOpen:
            /* Open archive via standard file dialog */
            {
                SFTypeList types;
                StandardFileReply reply;
                types[0] = kBoltFileType;
                StandardGetFile(nil, 1, types, &reply);
                if (reply.sfGood) {
                    FSSpec spec = reply.sfFile;
                    DoUnpack(&spec, 1);
                }
            }
            break;
        case iFileCreate:
            /* Create archive via standard file dialog */
            {
                StandardFileReply reply;
                SFTypeList types;
                types[0] = 0;
                StandardGetFile(nil, -1, types, &reply);
                if (reply.sfGood) {
                    FSSpec spec = reply.sfFile;
                    DoPack(&spec, 1);
                }
            }
            break;
        case iFilePrefs:
            DoPreferences();
            break;
        case iFileQuit:
            gQuit = true;
            break;
        }
        break;

    case mEdit:
        /* DA support — SystemEdit handles Cut/Copy/Paste for DAs */
        SystemEdit(menuItem - 1);
        break;
    }

    HiliteMenu(0);
}

/* ==== Window Drawing ==================================================== */

static void DrawDropTarget(WindowPtr win)
{
    Rect portRect, logoRect, dropRect;
    PicHandle logo;
    Str255 msg;
    short strWidth, x, y;
    GrafPtr oldPort;
    short winW;

    GetPort(&oldPort);
    SetPort((GrafPtr)GetWindowPort(win));

    GetWindowPortBounds(win, &portRect);
    EraseRect(&portRect);

    winW = portRect.right - portRect.left;

    /* 1. Draw centred logo */
    logo = (PicHandle)GetPicture(rLogoPict);
    if (logo != nil) {
        SetRect(&logoRect,
                (short)((winW - kLogoSize) / 2),
                20,
                (short)((winW - kLogoSize) / 2 + kLogoSize),
                20 + kLogoSize);
        DrawPicture(logo, &logoRect);
        ReleaseResource((Handle)logo);
    }

    /* 2. Mode label below logo */
    {
        Str255 modeStr;
        GetIndString(modeStr, rUIStrings,
                     (gMode == kModeExpand) ? kStrExpand : kStrCompress);
        if (modeStr[0] > 0) {
            TextFont(systemFont);
            TextSize(12);
            strWidth = StringWidth(modeStr);
            MoveTo((short)((winW - strWidth) / 2), 135);
            DrawString(modeStr);
        }
    }

    /* 3. Square drop area */
    SetRect(&dropRect,
            kDropAreaInset, 148,
            winW - kDropAreaInset, 235);
    FrameRoundRect(&dropRect, 8, 8);

    /* 4. Instruction text centred inside drop area */
    GetIndString(msg, rUIStrings, kStrDropHere);
    if (msg[0] == 0) {
        /* fallback if string resource is missing */
        memcpy(msg, "\pDrop files here to pack or unpack", 34);
    }
    TextFont(systemFont);
    TextSize(12);
    strWidth = StringWidth(msg);
    x = (short)((winW - strWidth) / 2);
    y = (short)((dropRect.top + dropRect.bottom) / 2 + 5);
    MoveTo(x, y);
    DrawString(msg);

    /* Controls (radio buttons, checkbox) are redrawn by the Toolbox. */

    SetPort(oldPort);
}

/* ==== Event Loop ======================================================== */

static void HandleMouseDown(EventRecord *evt)
{
    WindowPtr win;
    short part;

    part = FindWindow(evt->where, &win);

    switch (part) {
    case inMenuBar:
        HandleMenuChoice(MenuSelect(evt->where));
        break;

    case inSysWindow:
        SystemClick(evt, win);
        break;

    case inDrag:
        if (win != nil) {
            Rect bounds;
            bounds = (**GetGrayRgn()).rgnBBox;
            DragWindow(win, evt->where, &bounds);
        }
        break;

    case inGoAway:
        if (win == gMainWindow && TrackGoAway(win, evt->where)) {
            gQuit = true;
        }
        break;

    case inContent:
        if (win != FrontWindow()) {
            SelectWindow(win);
        } else if (win == gMainWindow) {
            ControlHandle hitCtl = nil;
            Point localPt = evt->where;
            SetPort((GrafPtr)GetWindowPort(gMainWindow));
            GlobalToLocal(&localPt);
            if (FindControl(localPt, gMainWindow, &hitCtl) != 0
                    && hitCtl != nil) {
                if (TrackControl(hitCtl, localPt, nil) != 0) {
                    if (hitCtl == gRadioExpand) {
                        Rect modeRect;
                        gMode = kModeExpand;
                        SetControlValue(gRadioExpand,   1);
                        SetControlValue(gRadioCompress, 0);
                        gPrefs.mode = (char)kModeExpand;
                        SavePrefs();
                        SetRect(&modeRect, 0, 120, 360, 150);
                        InvalWindowRect(gMainWindow, &modeRect);
                    } else if (hitCtl == gRadioCompress) {
                        Rect modeRect;
                        gMode = kModeCompress;
                        SetControlValue(gRadioExpand,   0);
                        SetControlValue(gRadioCompress, 1);
                        gPrefs.mode = (char)kModeCompress;
                        SavePrefs();
                        SetRect(&modeRect, 0, 120, 360, 150);
                        InvalWindowRect(gMainWindow, &modeRect);
                    } else if (hitCtl == gAskDestCheck && gHasNavServices) {
                        short v = GetControlValue(gAskDestCheck);
                        SetControlValue(gAskDestCheck, v ? 0 : 1);
                        gPrefs.ask_extract_location =
                            (GetControlValue(gAskDestCheck) != 0) ? 1 : 0;
                        SavePrefs();
                    }
                }
            }
        }
        break;
    }
}

static void HandleKeyDown(EventRecord *evt)
{
    char key = (char)(evt->message & charCodeMask);

    if (evt->modifiers & cmdKey) {
        HandleMenuChoice(MenuKey(key));
    }
}

static void HandleUpdate(EventRecord *evt)
{
    WindowPtr win = (WindowPtr)evt->message;

    BeginUpdate(win);
    if (win == gMainWindow) {
        DrawDropTarget(win);
    }
    EndUpdate(win);
}

static void HandleActivate(EventRecord *evt)
{
    (void)evt;
    /* nothing special needed */
}

static void EventLoop(void)
{
    EventRecord evt;

    while (!gQuit) {
        if (WaitNextEvent(everyEvent, &evt, 30, nil)) {
            switch (evt.what) {
            case mouseDown:
                HandleMouseDown(&evt);
                break;
            case keyDown:
            case autoKey:
                HandleKeyDown(&evt);
                break;
            case updateEvt:
                HandleUpdate(&evt);
                break;
            case activateEvt:
                HandleActivate(&evt);
                break;
            case kHighLevelEvent:
                AEProcessAppleEvent(&evt);
                break;
            }
        }
    }
}

/* ==== Navigation Services Check ========================================= */

static void CheckNavServices(void)
{
    gHasNavServices = false;
    if (NavServicesAvailable() && NavServicesCanRun() &&
        NavLibraryVersion() >= 0x01100000UL)
        gHasNavServices = true;
}

/* ==== Initialization ==================================================== */

static void InitToolbox(void)
{
    MaxApplZone();
    MoreMasters();
    MoreMasters();

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(nil);
    InitCursor();
    FlushEvents(everyEvent, 0);

    RegisterAppearanceClient();
}

static void OpenMainWindow(void)
{
    static const Rect kRectRadioExpand   = {155,  60, 173, 170};
    static const Rect kRectRadioCompress = {155, 190, 173, 310};
    static const Rect kRectAskDest       = {248,  20, 266, 260};

    gMainWindow = GetNewCWindow(rMainWindow, nil, (WindowPtr)-1);
    if (gMainWindow == nil) return;

    gRadioExpand = NewControl(gMainWindow, &kRectRadioExpand,
                              "\pExpand", true,
                              (gMode == kModeExpand) ? 1 : 0,
                              0, 1, radioButProc, 0L);

    gRadioCompress = NewControl(gMainWindow, &kRectRadioCompress,
                                "\pCompress", true,
                                (gMode == kModeCompress) ? 1 : 0,
                                0, 1, radioButProc, 0L);

    gAskDestCheck = NewControl(gMainWindow, &kRectAskDest,
                               "\pAsk for destination", true,
                               gPrefs.ask_extract_location ? 1 : 0,
                               0, 1, checkBoxProc, 0L);

    if (!gHasNavServices) {
        HiliteControl(gAskDestCheck, 255);
    }

    ShowWindow(gMainWindow);
}

/* ==== Main ============================================================== */

int main(void)
{
    int rc;

    InitToolbox();
    InitAppleEvents();
    InitMenuBar();
    CheckNavServices();
    LoadPrefs();
    OpenMainWindow();
    InitDragManager();

    rc = bp_compress_init();
    if (rc != BP_OK) {
        ShowErrorStr("Failed to initialize compression engine.");
        return 1;
    }

    EventLoop();

    if (gMainWindow != nil) {
        DisposeWindow(gMainWindow);
    }

    return 0;
}
