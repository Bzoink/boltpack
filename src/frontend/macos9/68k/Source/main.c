/*
 * main.c -- Mac OS 9 68k GUI frontend for BoltPack
 *
 * Retro68 / m68k target.
 * Mac Toolbox / Appearance Manager / Drag Manager.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */


#include "../../../../core/archiver.h"
#include "../../../../core/compress.h"
#include "../../../../core/fluff.h"

#include <StandardFile.h>
#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <Dialogs.h>
#include <Menus.h>
#include <Devices.h>
#include <TextEdit.h>
#include <Memory.h>
#include <Resources.h>
#include <ToolUtils.h>
#include <TextUtils.h>
#include <Controls.h>
#include <Appearance.h>
#include <Drag.h>
#include <AppleEvents.h>
#include <Files.h>
#include <Folders.h>
#include <Processes.h>
#include <LowMem.h>


#include <stdlib.h>
#include <string.h>

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
#define iFileQuit       4

#define rMainWindow     128

#define rDlgProgress    128
#define rDlgMixed       129
#define rDlgName        130
#define rDlgAbout       132
#define rAlrtError      128

/* Logo */
#define rLogoPict           128

/* Method selection dialog items (rDlgMixed = 129 repurposed) */
#define kMethodStored       1
#define kMethodLZSS         2
#define kMethodBoltLZ          3
#define kMethodFluff        4
#define kMethodCancel       5

/* New progress dialog items */
#define kProgOverallLabel   6
#define kProgOverallBar     7

/* Layout */
#define kLogoSize           96
#define kDropAreaInset      20
/* Progress dialog items */
#define kProgFileName   1
#define kProgBar        2
#define kProgFileCount  3
#define kProgRatio      4
#define kProgCancel     5

/* Name dialog items */
#define kNameField      2
#define kNameOK         1
#define kNameCancel     3

/* String list IDs */
#define rErrStrings     128
#define rUIStrings      129

/* UI string indices */
#define kStrDropHere    1

/* Maximum items we handle in a single drop */
#define kMaxDropItems   256

/* Fluff needs a persistent ~2 MB context plus working buffers. Mirror the
 * low-memory guard used by the Atari and Amiga GUI frontends so 68k Macs
 * fail early instead of running out of heap mid-archive. */
#define kFluffMinLargestBlock   (2304L * 1024L)

/* Mac path limit */
#define kMaxPath        1024

/* Development-only GUI logging. Enable with -DBP_GUI_DEV_LOGGING=1. */
#ifndef BP_GUI_DEV_LOGGING
#define BP_GUI_DEV_LOGGING 0
#endif

/* ==== Mac file handle wrapper for bp_io_t =============================== */

typedef struct {
    short refNum;
} mac_file_t;

typedef struct {
    Handle tempHandle;
} mac_fluff_alloc_t;

/* ==== Globals =========================================================== */

static WindowPtr    gMainWindow = NULL;
static Boolean      gQuit = false;
static FSSpec       gAppSpec; /* FSSpec of the application itself */

#if BP_GUI_DEV_LOGGING
static short          gLogRefNum     = -1;
static short          gLogVRefNum    = 0;
#endif

/* Deferred drop: DragReceiveHandler stores items here and returns immediately.
 * EventLoop picks them up once the Drag Manager has fully released. */
static FSSpec         gPendingDrop[kMaxDropItems];
static long           gPendingDropCount = 0;

/* ==== Forward declarations ============================================== */

static void     InitToolbox(void);
static void     InitAppleEvents(void);
static void     InitDragManager(void);
static void     InitMenuBar(void);
static void     OpenMainWindow(void);
static void     DrawDropTarget(WindowPtr win);
static void     EventLoop(void);
static void     HandleMouseDown(EventRecord *evt);
static void     HandleKeyDown(EventRecord *evt);
static void     HandleMenuChoice(long menuChoice);
static void     HandleUpdate(EventRecord *evt);
static void     HandleActivate(EventRecord *evt);

static void     DoAbout(void);
static void     ShowError(int bpErr);
static void     ShowErrorStr(const char *msg);
static void     OpenLog(void);
static void     LogWrite(const char *msg);
static void     CloseLog(void);

static void     ClassifyAndAct(FSSpec *items, long count);
static int      IsBoltArchive(const FSSpec *spec);
static int      IsDirectory(const FSSpec *spec);

static int      DoUnpack(FSSpec *archives, long count);
static int      DoPack(FSSpec *items, long count);

static int      AddFSSpecToArchive(bp_archive_t *ar, const FSSpec *spec,
                    const char *arcPrefix);
static int      AddDirectoryToArchive(bp_archive_t *ar, short vRefNum,
                    long dirID, const char *arcPrefix);
static int      CountArchiveWorkForFSSpec(const FSSpec *spec,
                    bp_uint32 *entryCount, bp_uint64 *totalBytes);
static int      CountArchiveWorkForDirectory(short vRefNum, long dirID,
                    bp_uint32 *entryCount, bp_uint64 *totalBytes);
static int      CountArchiveWorkForFile(short vRefNum, long dirID,
                    Str255 fileName, bp_uint32 *entryCount,
                    bp_uint64 *totalBytes);

static bp_uint32 ChooseBlockSize(void);
static long     LargestFreeBlock(void);
static long     LargestTempBlock(void);
static int      FluffMemoryOK(void);
static int      EnsureFluffMemoryAvailable(const char *logPrefix);
static void    *MacFluffAlloc(bp_uint32 size);
static void     MacFluffFree(void *ptr);

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
static void     ExtractDone(const bp_entry_t *entry, bp_io_t *out,
                    void *user_data);

/* Text encoding */
static void     MacRomanToUTF8(const unsigned char *src, char *dst,
                    long maxDst);
static void     UTF8ToMacRoman(const char *src, unsigned char *dst,
                    long maxDst);
static void     ArchivePathToMac(const char *src, Str255 dst);

/* Path helpers */
static void     FSSpecToParentDir(const FSSpec *spec, short *vRefNum,
                    long *dirID);
static void     ConcatArchivePath(char *buf, long bufSize,
                    const char *prefix, const char *name);
static void     PascalToBuf(const unsigned char *pstr, unsigned char *buf,
                    long bufSize);

/* In-memory bp_io_t for temporary metadata (no stdio FILE* needed). */
typedef struct {
    const unsigned char *data;
    long size;
    long pos;
} meta_mem_t;

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

/* ==== Minimal string formatting (avoid sprintf / stdio) ================ */
static void StrAppendChar(char *dst, long *pos, long dstSize, char c)
{
    if (*pos >= dstSize - 1) return;
    dst[*pos] = c;
    (*pos)++;
    dst[*pos] = '\0';
}

static void StrAppendStr(char *dst, long *pos, long dstSize, const char *src)
{
    while (*src != '\0') {
        StrAppendChar(dst, pos, dstSize, *src);
        src++;
    }
}

static void StrAppendU32Dec(char *dst, long *pos, long dstSize,
                             unsigned long v)
{
    char tmp[16];
    long len = 0;

    if (v == 0) {
        StrAppendChar(dst, pos, dstSize, '0');
        return;
    }

    while (v > 0 && len < (long)sizeof(tmp)) {
        tmp[len++] = (char)('0' + (v % 10));
        v /= 10;
    }

    while (len > 0) {
        StrAppendChar(dst, pos, dstSize, tmp[--len]);
    }
}

static void StrAppendLongDec(char *dst, long *pos, long dstSize, long v)
{
    unsigned long magnitude;

    if (v < 0) {
        StrAppendChar(dst, pos, dstSize, '-');
        magnitude = (unsigned long)(-v);
    } else {
        magnitude = (unsigned long)v;
    }
    StrAppendU32Dec(dst, pos, dstSize, magnitude);
}

static int BpOffToLong(bp_off_t value, long *out)
{
    if (value < (bp_off_t)(-2147483647L - 1L) ||
        value > (bp_off_t)2147483647L) {
        return 0;
    }
    *out = (long)value;
    return 1;
}

static void StrAppendOffDec(char *dst, long *pos, long dstSize, bp_off_t v)
{
    long lv;

    if (BpOffToLong(v, &lv)) {
        StrAppendLongDec(dst, pos, dstSize, lv);
    } else {
        StrAppendStr(dst, pos, dstSize, "<range>");
    }
}

/* ==== bp_io_t Implementation for Mac File Manager ======================= */

static long MacFileRead(void *handle, void *buf, bp_uint32 count)
{
    mac_file_t *mf = (mac_file_t *)handle;
    long ioCount = (long)count;
    long pos = -1;
    OSErr err;

    err = FSRead(mf->refNum, &ioCount, buf);
    if (err != noErr && err != eofErr) {
        char lbuf[128];
        long lpos = 0;

        GetFPos(mf->refNum, &pos);
        lbuf[0] = '\0';
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), "MacFileRead err=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), (long)err);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " ref=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), (long)mf->refNum);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " req=");
        StrAppendU32Dec(lbuf, &lpos, (long)sizeof(lbuf), (unsigned long)count);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " got=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), ioCount);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " pos=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), pos);
        LogWrite(lbuf);
        return -1;
    }
    return ioCount;
}

static long MacFileWrite(void *handle, const void *buf, bp_uint32 count)
{
    mac_file_t *mf = (mac_file_t *)handle;
    long ioCount = (long)count;
    long pos = -1;
    OSErr err;

    err = FSWrite(mf->refNum, &ioCount, (void *)buf);
    if (err != noErr) {
        char lbuf[128];
        long lpos = 0;

        GetFPos(mf->refNum, &pos);
        lbuf[0] = '\0';
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), "MacFileWrite err=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), (long)err);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " ref=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), (long)mf->refNum);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " req=");
        StrAppendU32Dec(lbuf, &lpos, (long)sizeof(lbuf), (unsigned long)count);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " wrote=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), ioCount);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " pos=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), pos);
        LogWrite(lbuf);
        return -1;
    }
    return ioCount;
}

static int MacFileSeek(void *handle, bp_off_t offset, int origin)
{
    mac_file_t *mf = (mac_file_t *)handle;
    long macOffset;
    short posMode;
    OSErr err;

    switch (origin) {
    case 0: posMode = fsFromStart; break;  /* SEEK_SET */
    case 1: posMode = fsFromMark;  break;  /* SEEK_CUR */
    case 2: posMode = fsFromLEOF;  break;  /* SEEK_END */
    default:
        LogWrite("MacFileSeek invalid origin");
        return -1;
    }
    if (!BpOffToLong(offset, &macOffset)) {
        char lbuf[128];
        long lpos = 0;

        lbuf[0] = '\0';
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), "MacFileSeek offset overflow ref=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), (long)mf->refNum);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " origin=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), (long)origin);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " off=");
        StrAppendOffDec(lbuf, &lpos, (long)sizeof(lbuf), offset);
        LogWrite(lbuf);
        return -1;
    }
    err = SetFPos(mf->refNum, posMode, macOffset);
    if (err != noErr) {
        char lbuf[128];
        long lpos = 0;

        lbuf[0] = '\0';
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), "MacFileSeek err=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), (long)err);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " ref=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), (long)mf->refNum);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " origin=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), (long)origin);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " off=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), macOffset);
        LogWrite(lbuf);
        return -1;
    }
    return 0;
}

static bp_off_t MacFileTell(void *handle)
{
    mac_file_t *mf = (mac_file_t *)handle;
    long pos;
    OSErr err;

    err = GetFPos(mf->refNum, &pos);
    if (err != noErr) {
        char lbuf[96];
        long lpos = 0;

        lbuf[0] = '\0';
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), "MacFileTell err=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), (long)err);
        StrAppendStr(lbuf, &lpos, (long)sizeof(lbuf), " ref=");
        StrAppendLongDec(lbuf, &lpos, (long)sizeof(lbuf), (long)mf->refNum);
        LogWrite(lbuf);
        return (bp_off_t)-1;
    }
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

/*
 * Convert a UTF-8 archive path (using '/') to a Mac Pascal string
 * (using ':') suitable for use with File Manager calls.
 */
static void ArchivePathToMac(const char *src, Str255 dst)
{
    unsigned char buf[256];
    long i, len;

    UTF8ToMacRoman(src, buf, 255);
    len = (long)strlen((char *)buf);

    /* replace '/' with ':' */
    for (i = 0; i < len; i++) {
        if (buf[i] == '/') buf[i] = ':';
    }

    /* strip trailing ':' */
    if (len > 0 && buf[len - 1] == ':') len--;

    if (len > 255) len = 255;
    dst[0] = (unsigned char)len;
    memcpy(dst + 1, buf, len);
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

    freeMem = LargestFreeBlock();

    /* need block_size * 2 + 512K headroom */
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

static long LargestFreeBlock(void)
{
    long maxBlock;

    maxBlock = MaxBlock();
    if (maxBlock <= 0) maxBlock = FreeMem();
    if (maxBlock < 0) maxBlock = 0;
    return maxBlock;
}

static long LargestTempBlock(void)
{
    Size grow;
    Size maxTemp;

    grow = 0;
    maxTemp = TempMaxMem(&grow);
    if (maxTemp < 0) maxTemp = 0;
    return (long)maxTemp;
}

static int FluffMemoryOK(void)
{
    return LargestFreeBlock() >= kFluffMinLargestBlock ||
           LargestTempBlock() >= kFluffMinLargestBlock;
}

static int EnsureFluffMemoryAvailable(const char *logPrefix)
{
    long appAvail;
    long tempAvail;
    char buf[128];
    long pos;

    if (FluffMemoryOK()) {
        return 1;
    }

    appAvail = LargestFreeBlock();
    tempAvail = LargestTempBlock();
    buf[0] = '\0';
    pos = 0;
    StrAppendStr(buf, &pos, (long)sizeof(buf), logPrefix);
    StrAppendStr(buf, &pos, (long)sizeof(buf), "app=");
    StrAppendLongDec(buf, &pos, (long)sizeof(buf), appAvail);
    StrAppendStr(buf, &pos, (long)sizeof(buf), " temp=");
    StrAppendLongDec(buf, &pos, (long)sizeof(buf), tempAvail);
    StrAppendStr(buf, &pos, (long)sizeof(buf), " need=");
    StrAppendLongDec(buf, &pos, (long)sizeof(buf), kFluffMinLargestBlock);
    LogWrite(buf);

    ShowErrorStr("Fluff needs about 2.25 MB in app heap or temporary memory. Increase memory or use boltlz.");
    return 0;
}

static void *MacFluffAlloc(bp_uint32 size)
{
    Size allocSize;
    Handle tempHandle;
    OSErr err;
    mac_fluff_alloc_t *hdr;
    OSErr disposeErr;

    if (size > (bp_uint32)(0x7FFFFFFFUL - (unsigned long)sizeof(mac_fluff_alloc_t))) {
        return NULL;
    }

    allocSize = (Size)(size + (bp_uint32)sizeof(mac_fluff_alloc_t));
    tempHandle = NULL;
    err = noErr;

    tempHandle = TempNewHandle(allocSize, &err);
    if (tempHandle != NULL && err == noErr) {
        TempHLock(tempHandle, &err);
        if (err == noErr && *tempHandle != NULL) {
            hdr = (mac_fluff_alloc_t *)(*tempHandle);
            hdr->tempHandle = tempHandle;
            return (void *)(hdr + 1);
        }
        disposeErr = noErr;
        TempDisposeHandle(tempHandle, &disposeErr);
    }

    hdr = (mac_fluff_alloc_t *)malloc((size_t)allocSize);
    if (hdr == NULL) {
        return NULL;
    }
    hdr->tempHandle = NULL;
    return (void *)(hdr + 1);
}

static void MacFluffFree(void *ptr)
{
    mac_fluff_alloc_t *hdr;
    OSErr err;

    if (ptr == NULL) return;

    hdr = ((mac_fluff_alloc_t *)ptr) - 1;
    if (hdr->tempHandle != NULL) {
        err = noErr;
        TempHUnlock(hdr->tempHandle, &err);
        err = noErr;
        TempDisposeHandle(hdr->tempHandle, &err);
    } else {
        free(hdr);
    }
}

/* ==== Error Display ===================================================== */

/* Resource-free error dialog: uses NewWindow so it works even if the
 * ALRT/DITL resources are unavailable (e.g. resource fork not yet open). */
static void ShowErrorStr(const char *msg)
{
    WindowPtr err_win;
    Rect r;
    EventRecord evt;
    Str255 pstr;
    long len;
    Boolean done = false;

    len = (long)strlen(msg);
    if (len > 255) len = 255;
    pstr[0] = (unsigned char)len;
    memcpy(pstr + 1, msg, (size_t)len);

    SetRect(&r, 80, 80, 440, 175);
    err_win = NewWindow(nil, &r, "\pBoltPack Error", true,
                        dBoxProc, (WindowPtr)-1, false, 0);
    if (err_win == nil) return;
    SetPort((GrafPtr)err_win);
    TextFont(0); TextSize(12);
    MoveTo(10, 28); DrawString("\pError:");
    MoveTo(10, 48); DrawString(pstr);
    MoveTo(10, 75); DrawString("\pClick or press a key to dismiss.");

    while (!done) {
        WaitNextEvent(everyEvent, &evt, 30, nil);
        switch (evt.what) {
        case mouseDown:
        case keyDown:
        case autoKey:
            done = true;
            break;
        case updateEvt:
            {
                WindowPtr uw = (WindowPtr)evt.message;
                BeginUpdate(uw);
                if (uw == err_win) {
                    SetPort((GrafPtr)err_win);
                    TextFont(0); TextSize(12);
                    MoveTo(10, 28); DrawString("\pError:");
                    MoveTo(10, 48); DrawString(pstr);
                    MoveTo(10, 75); DrawString("\pClick or press a key to dismiss.");
                }
                EndUpdate(uw);
            }
            break;
        case kHighLevelEvent:
            AEProcessAppleEvent(&evt);
            break;
        default:
            break;
        }
    }
    DisposeWindow(err_win);
}

static void ShowError(int bpErr)
{
    char msgBuf[256];
    long pos = 0;
    Str255 errStr;
    short idx;

    /* idx = 1 - bpErr: BP_ERR_IO=-1 -> 2, BP_ERR_NOMEM=-2 -> 3, etc. */
    idx = (short)(1 - bpErr);
    if (idx >= 1 && idx <= 17) {
        GetIndString(errStr, rErrStrings, idx);
    } else {
        errStr[0] = 0;
    }

    msgBuf[0] = '\0';
    StrAppendStr(msgBuf, &pos, (long)sizeof(msgBuf), "Error ");
    StrAppendLongDec(msgBuf, &pos, (long)sizeof(msgBuf), (long)bpErr);
    if (errStr[0] > 0) {
        long k;
        StrAppendStr(msgBuf, &pos, (long)sizeof(msgBuf), ": ");
        for (k = 1; k <= errStr[0] && k < 120; k++) {
            StrAppendChar(msgBuf, &pos, (long)sizeof(msgBuf), (char)errStr[k]);
        }
    }

    LogWrite(msgBuf);
    ShowErrorStr(msgBuf);
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
    bp_uint32   totalEntries;
    bp_uint64   totalBytes;
    long        filePct;
    long        overallPct;
    /* extraction context */
    short       outVRefNum;
    long        outDirID;
} progress_ctx_t;

/* Pointer to the active pack progress context; set during DoPack so that
 * AddDirectoryToArchive can yield events and check for Cancel. */
static progress_ctx_t *gCurrentPack = NULL;

static void UpdateProgressText(DialogPtr dlg, short item, const char *text);
static void DrawProgressBar(DialogPtr dlg, short item, long pct);
static void RedrawProgressBars(progress_ctx_t *ctx);

static void ShowProgressDialog(progress_ctx_t *ctx)
{
    ctx->dlg = GetNewDialog(rDlgProgress, nil, (WindowPtr)-1);
    ctx->cancelled = false;
    ctx->filePct = 0;
    ctx->overallPct = 0;
    if (ctx->dlg != nil) {
        char buf[64];
        long pos;

        SetDialogDefaultItem(ctx->dlg, kProgCancel);
        ShowWindow(GetDialogWindow(ctx->dlg));
        DrawDialog(ctx->dlg);

        UpdateProgressText(ctx->dlg, kProgFileName, "Preparing...");

        pos = 0;
        buf[0] = '\0';
        StrAppendStr(buf, &pos, (long)sizeof(buf), "Item 0 of ");
        if (ctx->totalEntries > 0) {
            StrAppendU32Dec(buf, &pos, (long)sizeof(buf),
                            (unsigned long)ctx->totalEntries);
        } else {
            StrAppendChar(buf, &pos, (long)sizeof(buf), '?');
        }
        UpdateProgressText(ctx->dlg, kProgFileCount, buf);
        UpdateProgressText(ctx->dlg, kProgRatio, "Ratio: --");
        if (!bp_u64_is_zero(ctx->totalBytes)) {
            UpdateProgressText(ctx->dlg, kProgOverallLabel, "Overall: 0%");
        } else {
            UpdateProgressText(ctx->dlg, kProgOverallLabel, "Overall: --");
        }
        RedrawProgressBars(ctx);
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

static void DrawProgressBar(DialogPtr dlg, short item, long pct)
{
    short itemType;
    Handle itemHandle;
    Rect itemRect;
    Rect fillRect;
    WindowPtr win;
    long width;

    if (dlg == nil) return;

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    GetDialogItem(dlg, item, &itemType, &itemHandle, &itemRect);
    win = GetDialogWindow(dlg);
    SetPort((GrafPtr)win);
    PenNormal();
    FrameRect(&itemRect);

    fillRect = itemRect;
    InsetRect(&fillRect, 2, 2);
    if (fillRect.right < fillRect.left) fillRect.right = fillRect.left;
    if (fillRect.bottom < fillRect.top) fillRect.bottom = fillRect.top;
    EraseRect(&fillRect);

    if (pct > 0 && fillRect.right > fillRect.left &&
        fillRect.bottom > fillRect.top) {
        width = fillRect.right - fillRect.left;
        fillRect.right = (short)(fillRect.left + ((width * pct) / 100L));
        if (fillRect.right < fillRect.left) fillRect.right = fillRect.left;
        PaintRect(&fillRect);
    }
}

static void RedrawProgressBars(progress_ctx_t *ctx)
{
    if (ctx == nil || ctx->dlg == nil) return;
    DrawProgressBar(ctx->dlg, kProgBar, ctx->filePct);
    DrawProgressBar(ctx->dlg, kProgOverallBar, ctx->overallPct);
}

static int ProgressCallback(const bp_progress_t *p, void *user_data)
{
    progress_ctx_t *ctx = (progress_ctx_t *)user_data;
    EventRecord evt;
    DialogPtr whichDlg;
    short item;
    char buf[256];
    long pct;
    unsigned long displayTotal;

    if (ctx->dlg == nil) return 0;
    if (ctx->cancelled) return 1;

    displayTotal = (ctx->totalEntries > 0)
                 ? (unsigned long)ctx->totalEntries
                 : (unsigned long)p->file_count;

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
    {
        long pos = 0;
        buf[0] = '\0';
        StrAppendStr(buf, &pos, (long)sizeof(buf), "Item ");
        StrAppendU32Dec(buf, &pos, (long)sizeof(buf),
                        (unsigned long)(p->file_index + 1));
        StrAppendStr(buf, &pos, (long)sizeof(buf), " of ");
        if (displayTotal > 0) {
            StrAppendU32Dec(buf, &pos, (long)sizeof(buf), displayTotal);
        } else {
            StrAppendChar(buf, &pos, (long)sizeof(buf), '?');
        }
    }
    UpdateProgressText(ctx->dlg, kProgFileCount, buf);

    /* update progress bar (integer math; avoids pulling in float runtime) */
    if (!bp_u64_is_zero(p->file_orig_size)) {
        unsigned long denom = bp_u64_to_u32(p->file_orig_size);
        unsigned long num = bp_u64_to_u32(p->file_bytes_done);

        if (denom == 0) {
            pct = 0;
        } else {
            if (num > denom) num = denom; /* clamp */
            if (denom < 100) {
                pct = (long)((num * 100UL) / denom);
            } else {
                unsigned long denom100 = denom / 100UL;
                pct = (long)(num / denom100);
            }
        }
    } else if (displayTotal > 0) {
        /* fall back to file index percent */
        pct = (long)(((unsigned long)(p->file_index + 1) * 100UL) / displayTotal);
    } else {
        pct = 0;
    }
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    ctx->filePct = pct;

    /* update ratio */
    if (!bp_u64_is_zero(p->archive_orig_total)) {
        long ratio_pct;
        {
            unsigned long denom = bp_u64_to_u32(p->archive_orig_total);
            unsigned long num = bp_u64_to_u32(p->archive_comp_total);

            if (denom == 0) {
                ratio_pct = 0;
            } else {
                if (num > denom) num = denom; /* clamp */
                if (denom < 100) {
                    ratio_pct = (long)((num * 100UL) / denom);
                } else {
                    unsigned long denom100 = denom / 100UL;
                    ratio_pct = (long)(num / denom100);
                }
            }
        }
        {
            long pos = 0;
            buf[0] = '\0';
            StrAppendStr(buf, &pos, (long)sizeof(buf), "Ratio: ");
            StrAppendLongDec(buf, &pos, (long)sizeof(buf), ratio_pct);
            StrAppendChar(buf, &pos, (long)sizeof(buf), '%');
        }
    } else {
        strcpy(buf, "Ratio: --");
    }
    UpdateProgressText(ctx->dlg, kProgRatio, buf);

    /* update overall progress bar */
    if (!bp_u64_is_zero(ctx->totalBytes)) {
        long overall_pct;
        {
            unsigned long denom = bp_u64_to_u32(ctx->totalBytes);
            unsigned long num = bp_u64_to_u32(p->archive_orig_done);

            if (denom == 0) {
                overall_pct = 0;
            } else {
                if (num > denom) num = denom; /* clamp */
                if (denom < 100) {
                    overall_pct = (long)((num * 100UL) / denom);
                } else {
                    unsigned long denom100 = denom / 100UL;
                    overall_pct = (long)(num / denom100);
                }
            }
        }
        if (overall_pct > 100) overall_pct = 100;
        if (overall_pct < 0)   overall_pct = 0;
        ctx->overallPct = overall_pct;
        {
            long pos = 0;
            buf[0] = '\0';
            StrAppendStr(buf, &pos, (long)sizeof(buf), "Overall: ");
            StrAppendLongDec(buf, &pos, (long)sizeof(buf), overall_pct);
            StrAppendChar(buf, &pos, (long)sizeof(buf), '%');
        }
        UpdateProgressText(ctx->dlg, kProgOverallLabel, buf);
    } else if (displayTotal > 0) {
        ctx->overallPct = (long)(((unsigned long)(p->file_index + 1) * 100UL) /
                                 displayTotal);
        if (ctx->overallPct > 100) ctx->overallPct = 100;
        if (ctx->overallPct < 0)   ctx->overallPct = 0;
        {
            long pos = 0;
            buf[0] = '\0';
            StrAppendStr(buf, &pos, (long)sizeof(buf), "Overall: ");
            StrAppendLongDec(buf, &pos, (long)sizeof(buf), ctx->overallPct);
            StrAppendChar(buf, &pos, (long)sizeof(buf), '%');
        }
        UpdateProgressText(ctx->dlg, kProgOverallLabel, buf);
    } else {
        ctx->overallPct = 0;
    }

    RedrawProgressBars(ctx);

    /* pump events briefly to keep UI responsive and check Cancel */
    if (WaitNextEvent(everyEvent, &evt, 1, nil)) {
        if (evt.what == updateEvt &&
            (WindowPtr)evt.message == GetDialogWindow(ctx->dlg)) {
            BeginUpdate((WindowPtr)evt.message);
            DrawDialog(ctx->dlg);
            RedrawProgressBars(ctx);
            EndUpdate((WindowPtr)evt.message);
        } else if (IsDialogEvent(&evt)) {
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

/* ==== Compression Method Dialog ========================================= */

/* Returns BP_METHOD_* (0-3) on success, or -1 if cancelled. */
static int AskCompressionMethod(void)
{
    DialogPtr dlg;
    short item;
    int result = -1;

    dlg = GetNewDialog(rDlgMixed, nil, (WindowPtr)-1);
    if (dlg == nil) return -1;

    SetDialogDefaultItem(dlg, kMethodBoltLZ);
    SetDialogCancelItem(dlg, kMethodCancel);
    ShowWindow(GetDialogWindow(dlg));

    for (;;) {
        ModalDialog(nil, &item);
        if (item == kMethodStored) { result = BP_METHOD_STORED; break; }
        if (item == kMethodLZSS)   { result = BP_METHOD_LZSS;   break; }
        if (item == kMethodBoltLZ)    { result = BP_METHOD_BOLTLZ;  break; }
        if (item == kMethodFluff)  {
            if (!EnsureFluffMemoryAvailable("AskCompressionMethod: fluff blocked largest=")) {
                continue;
            }
            result = BP_METHOD_FLUFF;
            break;
        }
        if (item == kMethodCancel) { result = -1;               break; }
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
    progress_ctx_t  prog;
    short           outVRefNum;
    long            outDirID;
} extract_user_t;

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
        /* create directory */
        long createdDirID;
        ArchivePathToMac(entry->name, macName);
        /* TODO: create nested directories from the archive path */
        DirCreate(eu->outVRefNum, eu->outDirID, macName, &createdDirID);
        return nil;
    }

    if (entry->entry_type == BP_TYPE_META) {
        /* metadata entries will be applied in ExtractDone;
         * we still need to receive the data so use a temp buffer approach.
         * For now, skip — Finder info restoration is done post-extraction. */
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
    if (nlen > 255) nlen = 255;
    memcpy(nameOnly, lastSlash, nlen);
    nameOnly[nlen] = '\0';

    /* convert to Pascal string */
    {
        unsigned char macBuf[256];
        UTF8ToMacRoman(nameOnly, macBuf, 255);
        nlen = (long)strlen((char *)macBuf);
        if (nlen > 63) nlen = 63; /* HFS name limit */
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
            Str255 dirName;
            long nlen2;
            long createdDirID2;
            char segBuf[256];
            unsigned char macBuf2[256];
            CInfoPBRec cpb2;

            nlen2 = (long)(seg - p);
            if (nlen2 > 255) nlen2 = 255;
            if (nlen2 == 0) { p = seg + 1; continue; }
            memcpy(segBuf, p, nlen2);
            segBuf[nlen2] = '\0';

            UTF8ToMacRoman(segBuf, macBuf2, 255);
            nlen2 = (long)strlen((char *)macBuf2);
            if (nlen2 > 31) nlen2 = 31;
            dirName[0] = (unsigned char)nlen2;
            memcpy(dirName + 1, macBuf2, nlen2);

            /* check if directory exists */
            memset(&cpb2, 0, sizeof(cpb2));
            cpb2.dirInfo.ioNamePtr = dirName;
            cpb2.dirInfo.ioVRefNum = parentVRef;
            cpb2.hFileInfo.ioDirID = parentDirID;
            if (PBGetCatInfoSync(&cpb2) == noErr &&
                (cpb2.dirInfo.ioFlAttrib & 0x10)) {
                parentDirID = cpb2.dirInfo.ioDrDirID;
            } else {
                DirCreate(parentVRef, parentDirID, dirName, &createdDirID2);
                parentDirID = createdDirID2;
            }
            p = seg + 1;
        }

        err = FSMakeFSSpec(parentVRef, parentDirID, macName, &outSpec);
        if (err != noErr && err != fnfErr) return nil;
    }

    /* create file if it doesn't exist */
    FSpCreate(&outSpec, kCreatorCode, '????', 0);

    /* open the appropriate fork */
    if (entry->entry_type == BP_TYPE_RSRC) {
        err = FSpOpenRF(&outSpec, fsWrPerm, &refNum);
    } else {
        err = FSpOpenDF(&outSpec, fsWrPerm, &refNum);
    }
    if (err != noErr) return nil;

    mf = (mac_file_t *)malloc(sizeof(mac_file_t));
    if (mf == nil) { FSClose(refNum); return nil; }

    io = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (io == nil) { FSClose(refNum); free(mf); return nil; }

    MacIOFromRefNum(io, mf, refNum);
    return io;
}

static void ExtractDone(const bp_entry_t *entry, bp_io_t *out,
                        void *user_data)
{
    mac_file_t *mf;

    (void)entry;
    (void)user_data;

    if (out == nil) return;

    mf = (mac_file_t *)out->handle;
    FSClose(mf->refNum);
    free(mf);
    free(out);
}

/* ==== Unpack Operation ================================================== */

static int DoUnpack(FSSpec *archives, long count)
{
    long i;
    int rc;

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
        OSErr err;

        memset(&eu, 0, sizeof(eu));

        err = FSpOpenDF(&archives[i], fsRdPerm, &refNum);
        if (err != noErr) {
            ShowErrorStr("Cannot open archive file.");
            continue;
        }
        MacIOFromRefNum(&aio, &mf, refNum);

        /* determine output location */
        eu.outVRefNum = archives[i].vRefNum;
        eu.outDirID = archives[i].parID;

        ShowProgressDialog(&eu.prog);

        rc = bp_archive_extract_all(&aio, ExtractOpen, ExtractDone,
                                    ProgressCallback, &eu);

        HideProgressDialog(&eu.prog);
        FSClose(refNum);

        if (rc != BP_OK && rc != BP_ERR_ABORTED) {
            ShowError(rc);
        }
    }

    return BP_OK;
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

    /* get file info: sizes and Finder info */
    memset(&cpb, 0, sizeof(cpb));
    cpb.hFileInfo.ioNamePtr = fileName;
    cpb.hFileInfo.ioVRefNum = vRefNum;
    cpb.hFileInfo.ioDirID = dirID;
    if (PBGetCatInfoSync(&cpb) != noErr) return BP_ERR_IO;

    dataSize = cpb.hFileInfo.ioFlLgLen;
    rsrcSize = cpb.hFileInfo.ioFlRLgLen;
    mtime = bp_u64_from_u32(cpb.hFileInfo.ioFlMdDat);
    {
        char fbuf[48];
        long fpos = 0;
        fbuf[0] = '\0';
        StrAppendStr(fbuf, &fpos, (long)sizeof(fbuf), "file: data=");
        StrAppendLongDec(fbuf, &fpos, (long)sizeof(fbuf), dataSize);
        StrAppendStr(fbuf, &fpos, (long)sizeof(fbuf), " rsrc=");
        StrAppendLongDec(fbuf, &fpos, (long)sizeof(fbuf), rsrcSize);
        LogWrite(fbuf);
    }

    FSMakeFSSpec(vRefNum, dirID, fileName, &fileSpec);

    /* archive data fork */
    if (dataSize > 0) {
        err = FSpOpenDF(&fileSpec, fsRdPerm, &refNum);
        if (err != noErr) return BP_ERR_IO;
        MacIOFromRefNum(&fio, &mf, refNum);

        rc = bp_archive_add_entry(ar, arcName, BP_TYPE_DATA, BP_OS_MACOS9,
                                  &fio, bp_u64_from_u32((bp_uint32)dataSize), mtime);
        FSClose(refNum);
        if (rc != BP_OK) return rc;
    } else {
        /* zero-length data fork — still add an entry */
        rc = bp_archive_add_entry(ar, arcName, BP_TYPE_DATA, BP_OS_MACOS9,
                                  nil, bp_u64_from_u32(0), mtime);
        if (rc != BP_OK) return rc;
    }

    /* archive resource fork if non-empty */
    if (rsrcSize > 0) {
        char rsrcArcName[kMaxPath];
        ConcatArchivePath(rsrcArcName, kMaxPath, "", arcName);

        err = FSpOpenRF(&fileSpec, fsRdPerm, &refNum);
        if (err != noErr) return BP_ERR_IO;
        MacIOFromRefNum(&fio, &mf, refNum);

        rc = bp_archive_add_entry(ar, arcName, BP_TYPE_RSRC, BP_OS_MACOS9,
                                  &fio, bp_u64_from_u32((bp_uint32)rsrcSize), mtime);
        FSClose(refNum);
        if (rc != BP_OK) return rc;
    }

    /* archive Finder info as metadata */
    if (FSpGetFInfo(&fileSpec, &finfo) == noErr) {
        /* Finder info: type(4) + creator(4) + flags(2) = 10 bytes */
        unsigned char metaBuf[10];

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
        {
            meta_mem_t tmpMem;
            bp_io_t tmpIO;

            tmpMem.data = metaBuf;
            tmpMem.size = 10;
            tmpMem.pos  = 0;

            /* Wrap the in-memory buffer with a bp_io_t. */
            tmpIO.read = MacTmpRead;
            tmpIO.write = nil;
            tmpIO.seek = MacTmpSeek;
            tmpIO.tell = MacTmpTell;
            tmpIO.handle = &tmpMem;

            rc = bp_archive_add_entry(ar, arcName, BP_TYPE_META,
                                      BP_OS_MACOS9, &tmpIO, bp_u64_from_u32(10), mtime);
            if (rc != BP_OK) return rc;
        }
    }

    return BP_OK;
}

/*
 * In-memory bp_io_t callbacks for the 10-byte metadata buffer.
 * We keep this stdio-free to avoid dragging in the full FILE/stdio
 * runtime into the 68k near-data segment.
 */

static long MacTmpRead(void *handle, void *buf, bp_uint32 count)
{
    meta_mem_t *s = (meta_mem_t *)handle;
    long remaining = s->size - s->pos;
    long toRead = (long)count;

    if (toRead > remaining) toRead = remaining;
    if (toRead < 0) toRead = 0;
    if (toRead == 0) return 0;

    memcpy(buf, s->data + s->pos, (size_t)toRead);
    s->pos += toRead;
    return toRead;
}

static int MacTmpSeek(void *handle, bp_off_t offset, int origin)
{
    meta_mem_t *s = (meta_mem_t *)handle;
    long longOffset;
    long newPos;

    if (!BpOffToLong(offset, &longOffset)) {
        return -1;
    }

    /* mimic the usual SEEK_* semantics for our usage */
    switch (origin) {
    case 0: /* from start */
        newPos = longOffset;
        break;
    case 1: /* from current */
        newPos = s->pos + longOffset;
        break;
    case 2: /* from end */
        newPos = s->size + longOffset;
        break;
    default:
        return -1;
    }

    if (newPos < 0) newPos = 0;
    if (newPos > s->size) newPos = s->size;

    s->pos = newPos;
    return 0;
}

static bp_off_t MacTmpTell(void *handle)
{
    meta_mem_t *s = (meta_mem_t *)handle;
    return (bp_off_t)s->pos;
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

        memset(&cpb, 0, sizeof(cpb));
        itemName[0] = 0;
        cpb.hFileInfo.ioNamePtr = itemName;
        cpb.hFileInfo.ioVRefNum = vRefNum;
        cpb.hFileInfo.ioDirID = dirID;
        cpb.hFileInfo.ioFDirIndex = index;

        if (PBGetCatInfoSync(&cpb) != noErr) break; /* no more items */

        /* yield to Mac OS between items to keep UI responsive and check Cancel */
        {
            EventRecord evt;
            DialogPtr whichDlg;
            short dlgItem;
            if (WaitNextEvent(everyEvent, &evt, 0, nil)) {
                if (gCurrentPack != nil && IsDialogEvent(&evt)) {
                    if (DialogSelect(&evt, &whichDlg, &dlgItem)) {
                        if (gCurrentPack->dlg != nil &&
                            whichDlg == gCurrentPack->dlg &&
                            dlgItem == kProgCancel) {
                            gCurrentPack->cancelled = true;
                            return BP_ERR_ABORTED;
                        }
                    }
                }
            }
            if (gCurrentPack != nil && gCurrentPack->cancelled) {
                return BP_ERR_ABORTED;
            }
        }
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

            rc = bp_archive_add_entry(ar, childArc, BP_TYPE_DIR,
                                      BP_OS_MACOS9, nil, bp_u64_from_u32(0),
                                      bp_u64_from_u32(cpb.dirInfo.ioDrMdDat));
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

static void CountArchiveProgressEntry(bp_uint32 *entryCount,
                                      bp_uint64 *totalBytes,
                                      bp_uint32 entrySize)
{
    *entryCount = *entryCount + 1;
    *totalBytes = bp_u64_add(*totalBytes, bp_u64_from_u32(entrySize));
}

static int CountArchiveWorkForFile(short vRefNum, long dirID, Str255 fileName,
                                   bp_uint32 *entryCount,
                                   bp_uint64 *totalBytes)
{
    CInfoPBRec cpb;
    FSSpec fileSpec;
    FInfo finfo;
    OSErr err;
    long dataSize;
    long rsrcSize;

    memset(&cpb, 0, sizeof(cpb));
    cpb.hFileInfo.ioNamePtr = fileName;
    cpb.hFileInfo.ioVRefNum = vRefNum;
    cpb.hFileInfo.ioDirID = dirID;
    if (PBGetCatInfoSync(&cpb) != noErr) return BP_ERR_IO;

    dataSize = cpb.hFileInfo.ioFlLgLen;
    rsrcSize = cpb.hFileInfo.ioFlRLgLen;
    if (dataSize < 0 || rsrcSize < 0) return BP_ERR_IO;

    CountArchiveProgressEntry(entryCount, totalBytes, (bp_uint32)dataSize);
    if (rsrcSize > 0) {
        CountArchiveProgressEntry(entryCount, totalBytes, (bp_uint32)rsrcSize);
    }

    err = FSMakeFSSpec(vRefNum, dirID, fileName, &fileSpec);
    if (err == noErr && FSpGetFInfo(&fileSpec, &finfo) == noErr) {
        CountArchiveProgressEntry(entryCount, totalBytes, 10);
    }

    return BP_OK;
}

static int CountArchiveWorkForDirectory(short vRefNum, long dirID,
                                        bp_uint32 *entryCount,
                                        bp_uint64 *totalBytes)
{
    CInfoPBRec cpb;
    Str255 itemName;
    short index;

    for (index = 1; ; index++) {
        int isDir;
        OSErr err;

        memset(&cpb, 0, sizeof(cpb));
        itemName[0] = 0;
        cpb.hFileInfo.ioNamePtr = itemName;
        cpb.hFileInfo.ioVRefNum = vRefNum;
        cpb.hFileInfo.ioDirID = dirID;
        cpb.hFileInfo.ioFDirIndex = index;

        err = PBGetCatInfoSync(&cpb);
        if (err != noErr) break;

        isDir = (cpb.hFileInfo.ioFlAttrib & 0x10) != 0;
        if (isDir) {
            long childDirID;
            int rc;

            childDirID = cpb.dirInfo.ioDrDirID;
            CountArchiveProgressEntry(entryCount, totalBytes, 0);
            rc = CountArchiveWorkForDirectory(vRefNum, childDirID,
                                             entryCount, totalBytes);
            if (rc != BP_OK) return rc;
        } else {
            int rc;

            rc = CountArchiveWorkForFile(vRefNum, dirID, itemName,
                                         entryCount, totalBytes);
            if (rc != BP_OK) return rc;
        }
    }

    return BP_OK;
}

static int CountArchiveWorkForFSSpec(const FSSpec *spec,
                                     bp_uint32 *entryCount,
                                     bp_uint64 *totalBytes)
{
    int isDir;

    isDir = IsDirectory(spec);
    if (isDir) {
        CInfoPBRec cpb;
        Str255 name;
        long theDirID;
        int rc;

        memcpy(name, spec->name, spec->name[0] + 1);
        memset(&cpb, 0, sizeof(cpb));
        cpb.dirInfo.ioNamePtr = name;
        cpb.dirInfo.ioVRefNum = spec->vRefNum;
        cpb.hFileInfo.ioDirID = spec->parID;
        if (PBGetCatInfoSync(&cpb) != noErr) return BP_ERR_IO;
        theDirID = cpb.dirInfo.ioDrDirID;

        CountArchiveProgressEntry(entryCount, totalBytes, 0);
        rc = CountArchiveWorkForDirectory(spec->vRefNum, theDirID,
                                         entryCount, totalBytes);
        if (rc != BP_OK) return rc;
    } else {
        int rc;
        Str255 nameCopy;

        memcpy(nameCopy, spec->name, spec->name[0] + 1);
        rc = CountArchiveWorkForFile(spec->vRefNum, spec->parID, nameCopy,
                                     entryCount, totalBytes);
        if (rc != BP_OK) return rc;
    }

    return BP_OK;
}

static int AddFSSpecToArchive(bp_archive_t *ar, const FSSpec *spec,
                              const char *arcPrefix)
{
    int isDir = IsDirectory(spec);

    {
        char dbuf[16];
        long dpos = 0;
        dbuf[0] = '\0';
        StrAppendStr(dbuf, &dpos, (long)sizeof(dbuf), "isDir=");
        StrAppendLongDec(dbuf, &dpos, (long)sizeof(dbuf), (long)isDir);
        LogWrite(dbuf);
    }
    if (isDir) {
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
        cpb.hFileInfo.ioDirID = spec->parID;
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
            char rbuf[160];
            long rpos = 0;
            bp_off_t apos;

            rbuf[0] = '\0';
            StrAppendStr(rbuf, &rpos, (long)sizeof(rbuf), "dir_add_entry name=");
            StrAppendStr(rbuf, &rpos, (long)sizeof(rbuf), dirArc);
            LogWrite(rbuf);

            apos = ar->io->tell(ar->io->handle);
            rpos = 0;
            rbuf[0] = '\0';
            StrAppendStr(rbuf, &rpos, (long)sizeof(rbuf), "dir_add_entry pos=");
            StrAppendOffDec(rbuf, &rpos, (long)sizeof(rbuf), apos);
            LogWrite(rbuf);
        }

        rc = bp_archive_add_entry(ar, dirArc, BP_TYPE_DIR, BP_OS_MACOS9,
                                  nil, bp_u64_from_u32(0), bp_u64_from_u32(cpb.dirInfo.ioDrMdDat));
        {
            char rbuf[32];
            long rpos = 0;
            rbuf[0] = '\0';
            StrAppendStr(rbuf, &rpos, (long)sizeof(rbuf), "dir_add_entry rc=");
            StrAppendLongDec(rbuf, &rpos, (long)sizeof(rbuf), (long)rc);
            LogWrite(rbuf);
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
    int method;
    int rc;
    long i;
    OSErr err;

    memset(&prog, 0, sizeof(prog));

    LogWrite("DoPack: start");

    rc = bp_compress_init();
    if (rc != BP_OK) {
        LogWrite("DoPack: bp_compress_init failed");
        ShowError(rc);
        return rc;
    }
    LogWrite("DoPack: compress_init OK");

    /* choose compression method — always prompt */
    method = AskCompressionMethod();
    if (method < 0) {
        LogWrite("DoPack: method cancelled");
        return BP_ERR_ABORTED;
    }
    if ((bp_uint8)method == BP_METHOD_FLUFF) {
        if (!EnsureFluffMemoryAvailable("DoPack: fluff blocked largest=")) {
            return BP_ERR_NOMEM;
        }
    }
    {
        char mbuf[32];
        long mpos = 0;
        mbuf[0] = '\0';
        StrAppendStr(mbuf, &mpos, (long)sizeof(mbuf), "DoPack: method=");
        StrAppendLongDec(mbuf, &mpos, (long)sizeof(mbuf), (long)method);
        LogWrite(mbuf);
    }

    /* build a suggested archive name from the first item */
    {
        char utf8Name[256];
        unsigned char nameBuf[256];
        long pos = 0;
        PascalToBuf(items[0].name, nameBuf, sizeof(nameBuf));
        MacRomanToUTF8(nameBuf, utf8Name, sizeof(utf8Name));
        archiveName[0] = '\0';
        StrAppendStr(archiveName, &pos, (long)sizeof(archiveName), utf8Name);
        StrAppendStr(archiveName, &pos, (long)sizeof(archiveName), ".bolt");
    }

    /* always ask user to confirm / change the archive name */
    if (!AskArchiveName(archiveName, archiveName, sizeof(archiveName))) {
        LogWrite("DoPack: name cancelled");
        return BP_ERR_ABORTED;
    }
    {
        char nbuf[256];
        long npos = 0;
        nbuf[0] = '\0';
        StrAppendStr(nbuf, &npos, (long)sizeof(nbuf), "DoPack: name=");
        StrAppendStr(nbuf, &npos, (long)sizeof(nbuf), archiveName);
        LogWrite(nbuf);
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
    {
        char ebuf[32];
        long epos = 0;
        ebuf[0] = '\0';
        StrAppendStr(ebuf, &epos, (long)sizeof(ebuf), "DoPack: FSMakeFSSpec err=");
        StrAppendLongDec(ebuf, &epos, (long)sizeof(ebuf), (long)err);
        LogWrite(ebuf);
    }
    if (err != noErr && err != fnfErr) {
        ShowErrorStr("Cannot create archive file path.");
        return BP_ERR_IO;
    }

    FSpDelete(&outSpec); /* remove if exists */
    err = FSpCreate(&outSpec, kCreatorCode, kBoltFileType, 0);
    {
        char ebuf[32];
        long epos = 0;
        ebuf[0] = '\0';
        StrAppendStr(ebuf, &epos, (long)sizeof(ebuf), "DoPack: FSpCreate err=");
        StrAppendLongDec(ebuf, &epos, (long)sizeof(ebuf), (long)err);
        LogWrite(ebuf);
    }

    err = FSpOpenDF(&outSpec, fsRdWrPerm, &refNum);
    {
        char ebuf[32];
        long epos = 0;
        ebuf[0] = '\0';
        StrAppendStr(ebuf, &epos, (long)sizeof(ebuf), "DoPack: FSpOpenDF err=");
        StrAppendLongDec(ebuf, &epos, (long)sizeof(ebuf), (long)err);
        LogWrite(ebuf);
    }
    if (err != noErr) {
        ShowErrorStr("Cannot open archive for writing.");
        return BP_ERR_IO;
    }

    MacIOFromRefNum(&aio, &mf, refNum);

    for (i = 0; i < count; i++) {
        rc = CountArchiveWorkForFSSpec(&items[i], &prog.totalEntries,
                                       &prog.totalBytes);
        if (rc != BP_OK) {
            prog.totalEntries = 0;
            prog.totalBytes = bp_u64_from_u32(0);
            break;
        }
    }

    blockSize = ChooseBlockSize();
    rc = bp_archive_create(&ar, &aio, blockSize, (bp_uint8)method);
    {
        char ebuf[32];
        long epos = 0;
        ebuf[0] = '\0';
        StrAppendStr(ebuf, &epos, (long)sizeof(ebuf), "DoPack: archive_create rc=");
        StrAppendLongDec(ebuf, &epos, (long)sizeof(ebuf), (long)rc);
        LogWrite(ebuf);
    }
    if (rc != BP_OK) {
        FSClose(refNum);
        ShowError(rc);
        return rc;
    }

    ar.creator_os  = BP_OS_MACOS9;
    ar.creator_cpu = BP_CPU_M68K;

    gCurrentPack = &prog;
    ShowProgressDialog(&prog);
    ar.progress_fn   = ProgressCallback;
    ar.progress_data = &prog;

    {
        char ebuf[64];
        long epos = 0;
        long freeBytes;
        long maxContig;
        freeBytes = FreeMem();
        maxContig = MaxBlock();
        ebuf[0] = '\0';
        StrAppendStr(ebuf, &epos, (long)sizeof(ebuf), "DoPack: mem free=");
        StrAppendLongDec(ebuf, &epos, (long)sizeof(ebuf), freeBytes);
        StrAppendStr(ebuf, &epos, (long)sizeof(ebuf), " max=");
        StrAppendLongDec(ebuf, &epos, (long)sizeof(ebuf), maxContig);
        LogWrite(ebuf);
    }
    for (i = 0; i < count; i++) {
        {
            char ebuf[64];
            long epos = 0;
            unsigned char nameBuf2[256];
            ebuf[0] = '\0';
            StrAppendStr(ebuf, &epos, (long)sizeof(ebuf), "DoPack: adding item ");
            StrAppendLongDec(ebuf, &epos, (long)sizeof(ebuf), i);
            StrAppendStr(ebuf, &epos, (long)sizeof(ebuf), ": ");
            PascalToBuf(items[i].name, nameBuf2, sizeof(nameBuf2));
            StrAppendStr(ebuf, &epos, (long)sizeof(ebuf), (const char *)nameBuf2);
            LogWrite(ebuf);
        }
        rc = AddFSSpecToArchive(&ar, &items[i], "");
        {
            char ebuf[32];
            long epos = 0;
            ebuf[0] = '\0';
            StrAppendStr(ebuf, &epos, (long)sizeof(ebuf), "DoPack: add rc=");
            StrAppendLongDec(ebuf, &epos, (long)sizeof(ebuf), (long)rc);
            LogWrite(ebuf);
        }
        if (rc != BP_OK) break;
    }

    if (rc == BP_OK) {
        rc = bp_archive_finalize(&ar);
        {
            char ebuf[32];
            long epos = 0;
            ebuf[0] = '\0';
            StrAppendStr(ebuf, &epos, (long)sizeof(ebuf), "DoPack: finalize rc=");
            StrAppendLongDec(ebuf, &epos, (long)sizeof(ebuf), (long)rc);
            LogWrite(ebuf);
        }
    }

    HideProgressDialog(&prog);
    gCurrentPack = NULL;
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

    /* All items are archives → unpack; any non-archive present → pack all */
    if (archiveCount == count && count > 0) {
        DoUnpack(archives, archiveCount);
    } else if (count > 0) {
        DoPack(items, count);
    }
}

/* ==== Apple Event Handlers ============================================== */

static pascal OSErr AEOpenApp(const AppleEvent *evt, AppleEvent *reply,
                              unsigned long refcon)
{
    (void)evt; (void)reply; (void)refcon;
    return noErr;
}

static pascal OSErr AEOpenDocs(const AppleEvent *evt, AppleEvent *reply,
                               unsigned long refcon)
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
                                unsigned long refcon)
{
    (void)evt; (void)reply; (void)refcon;
    return errAEEventNotHandled;
}

static pascal OSErr AEQuit(const AppleEvent *evt, AppleEvent *reply,
                           unsigned long refcon)
{
    (void)evt; (void)reply; (void)refcon;
    gQuit = true;
    return noErr;
}

/* Some CodeWarrior 68k header/toolchain combinations don't provide the
 * `NewAEEventHandlerUPP` / `NewDrag*HandlerUPP` helper macros/functions.
 * When they are missing, the compiler treats them as implicit `int`
 * and the callback parameter types then fail to match.
 */
typedef pascal short (*BoltAEEventHandlerUPP)(const struct AEDesc *,
                                                struct AEDesc *,
                                                long);
typedef pascal short (*BoltDragTrackingHandlerUPP)(DragTrackingMessage,
                                                      WindowPtr,
                                                      void *,
                                                      DragReference);
typedef pascal short (*BoltDragReceiveHandlerUPP)(WindowPtr,
                                                    void *,
                                                    DragReference);

static void InitAppleEvents(void)
{
    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication,
                          (BoltAEEventHandlerUPP)AEOpenApp, 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments,
                          (BoltAEEventHandlerUPP)AEOpenDocs, 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEPrintDocuments,
                          (BoltAEEventHandlerUPP)AEPrintDocs, 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication,
                          (BoltAEEventHandlerUPP)AEQuit, 0, false);
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
            portRect = window->portRect;
            RectRgn(hiliteRgn, &portRect);
            SetPort((GrafPtr)window);
            ShowDragHilite(drag, hiliteRgn, true);
            DisposeRgn(hiliteRgn);
        }
        break;

    case kDragTrackingLeaveWindow:
        SetPort((GrafPtr)window);
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
        ItemReference itemRef;
        HFSFlavor hfsData;
        Size dataSize;

        GetDragItemReferenceNumber(drag, i, &itemRef);
        dataSize = sizeof(HFSFlavor);

        if (GetFlavorData(drag, itemRef, flavorTypeHFS, &hfsData,
                          &dataSize, 0) == noErr) {
            items[collected++] = hfsData.fileSpec;
        }
    }

    /* Do NOT call ClassifyAndAct here: it calls WaitNextEvent via
     * ProgressCallback, which re-enters the event loop while the Drag
     * Manager is still in flight and causes IO errors.  Instead, stash
     * the items and let EventLoop process them after the drag completes. */
    if (collected > 0) {
        long j;
        gPendingDropCount = 0;
        for (j = 0; j < collected; j++) {
            gPendingDrop[gPendingDropCount++] = items[j];
        }
    }

    return noErr;
}

static void InitDragManager(void)
{
    OSErr err;
    long response;

    /* Check that the Drag Manager is available before calling it */
    if (Gestalt(gestaltDragMgrPresent, &response) != noErr || !(response & 1)) {
        return;
    }

    err = InstallTrackingHandler(
        (DragTrackingHandlerUPP)DragTrackingHandler,
        gMainWindow, nil);
    if (err != noErr) return;

    InstallReceiveHandler(
        (DragReceiveHandlerUPP)DragReceiveHandler,
        gMainWindow, nil);
}

/* ==== Menu Handling ===================================================== */

static void InitMenuBar(void)
{
    Handle mbar;
    MenuHandle apple_menu;

    mbar = GetNewMBar(rMenuBar);
    if (mbar != nil) {
        SetMenuBar(mbar);
        DisposeHandle(mbar);
    }
    /* Guard against nil: if MENU 128 wasn't loaded, GetMenuHandle returns nil */
    apple_menu = GetMenuHandle(mApple);
    if (apple_menu != nil) {
        AppendResMenu(apple_menu, 'DRVR');
    }
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
    SetPort((GrafPtr)win);

    portRect = win->portRect;
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

    /* 2. Square drop area */
    SetRect(&dropRect,
            kDropAreaInset, 128,
            winW - kDropAreaInset, 215);
    FrameRoundRect(&dropRect, 8, 8);

    /* 3. Instruction text centred inside drop area */
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

    /* Ensure controls are repainted after our custom background draw. */
    DrawControls(win);

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
            bounds = (*LMGetGrayRgn())->rgnBBox;
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

        /* Process any deferred drag-and-drop items now that the Drag Manager
         * has fully released and we are safely back in the event loop. */
        if (gPendingDropCount > 0) {
            long n = gPendingDropCount;
            FSSpec items[kMaxDropItems];
            long j;
            for (j = 0; j < n; j++) items[j] = gPendingDrop[j];
            gPendingDropCount = 0;
            ClassifyAndAct(items, n);
        }
    }
}

/* ==== Dev Log File ====================================================== */

/* Opens (or creates) boltpack.log in the Preferences folder and seeks to
 * the end so new entries are appended.  Plain text, Mac CR line endings. */
static void OpenLog(void)
{
#if BP_GUI_DEV_LOGGING
    FSSpec spec;
    ProcessSerialNumber psn;
    ProcessInfoRec pir;
    FSSpec appSpec;
    long fsize;
    OSErr err;

    /* locate the running application to put the log next to it */
    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN  = kCurrentProcess;
    memset(&pir, 0, sizeof(pir));
    pir.processInfoLength = sizeof(ProcessInfoRec);
    pir.processName       = nil;
    pir.processAppSpec    = &appSpec;
    err = GetProcessInformation(&psn, &pir);
    if (err != noErr) { gLogRefNum = -1; return; }

    FSMakeFSSpec(appSpec.vRefNum, appSpec.parID, "\pboltpack.log", &spec);
    FSpCreate(&spec, kCreatorCode, 'TEXT', 0); /* no-op if already exists */
    err = FSpOpenDF(&spec, fsWrPerm, &gLogRefNum);
    if (err != noErr) { gLogRefNum = -1; return; }
    gLogVRefNum = spec.vRefNum;
    GetEOF(gLogRefNum, &fsize);
    SetFPos(gLogRefNum, fsFromStart, fsize);
#else
    return;
#endif
}

/* Writes one log line: "[ticks] msg\r" */
static void LogWrite(const char *msg)
{
#if BP_GUI_DEV_LOGGING
    char buf[512];
    long pos = 0;
    long ioCount;
    unsigned long ticks;

    if (gLogRefNum < 0) return;

    ticks = (unsigned long)TickCount();
    buf[0] = '\0';
    StrAppendChar(buf, &pos, (long)sizeof(buf), '[');
    StrAppendU32Dec(buf, &pos, (long)sizeof(buf), ticks);
    StrAppendStr(buf, &pos, (long)sizeof(buf), "] ");
    StrAppendStr(buf, &pos, (long)sizeof(buf), msg);
    StrAppendChar(buf, &pos, (long)sizeof(buf), '\r');
    ioCount = pos;
    FSWrite(gLogRefNum, &ioCount, buf);
    FlushVol(nil, gLogVRefNum); /* ensure log reaches disk even if app freezes */
#else
    (void)msg;
#endif
}

static void CloseLog(void)
{
#if BP_GUI_DEV_LOGGING
    if (gLogRefNum >= 0) {
        FSClose(gLogRefNum);
        gLogRefNum = -1;
    }
#else
    return;
#endif
}

/* ==== Initialization ==================================================== */

static void InitToolbox(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(nil);
    InitCursor();
    FlushEvents(everyEvent, 0);
    /* RegisterAppearanceClient intentionally omitted:
     * Mac OS 9 applies Platinum appearance to all apps automatically.
     * The trap selector varies across AM versions and can crash on some builds. */
}

static void OpenMainWindow(void)
{
    gMainWindow = GetNewCWindow(rMainWindow, nil, (WindowPtr)-1);
    if (gMainWindow == nil) return;

    ShowWindow(gMainWindow);
    DrawDropTarget(gMainWindow);
}

/* ==== Main ============================================================== */

int main(void)
{
    int rc;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(nil);
    InitCursor();
    FlushEvents(everyEvent, 0);

    InitAppleEvents();
    InitMenuBar();
    fluff_set_allocators(MacFluffAlloc, MacFluffFree);
    OpenLog();
    LogWrite("BoltPack startup");
    OpenMainWindow();
    InitDragManager();

    rc = bp_compress_init();
    if (rc != BP_OK) {
        ShowErrorStr("Failed to initialize compression engine.");
        return 1;
    }

    EventLoop();

    LogWrite("BoltPack shutdown");
    CloseLog();

    if (gMainWindow != nil) {
        DisposeWindow(gMainWindow);
    }

    return 0;
}
