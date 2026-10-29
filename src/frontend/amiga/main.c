/*
 * main.c -- AmigaOS 2.x/3.x CLI frontend for BoltPack
 *
 * Target:   AmigaOS 2.x/3.x, 68k (A500, A600, A1200, A2000, A3000, A4000)
 * Compiler: m68k-amigaos-gcc (bebbo's fork) with -noixemul
 *           SAS/C 6.x (native)
 *           VBCC (native or cross)
 *
 * File I/O uses stdio.h (provided by libnix/clib2).
 * Directory traversal uses POSIX opendir/readdir/stat (provided by libnix).
 * Protection bits and file comments are archived and restored using native
 * AmigaDOS Lock/Examine/SetProtection/SetComment calls.
 *
 * Metadata (BP_TYPE_META) payload for BP_OS_AMIGA_68K:
 *   Bytes 0-3 : protection bits (little-endian uint32, raw fib_Protection)
 *   Byte  4   : comment length  (0..79)
 *   Bytes 5..N: comment text    (ISO-8859-1 / Latin-1)
 *
 * Cross-compilation from Linux (bebbo's m68k-amigaos-gcc):
 *   m68k-amigaos-gcc -noixemul -ansi -Wall -O2        \
 *       -I src/core                 \
 *       src/core/compress.c src/core/archiver.c        \
 *       src/core/bp_codepage.c                         \
 *       src/frontend/amiga/main.c                      \
 *       src/core/boltlz.c                     \
 *       -o boltpack
 *
 * NOTE: -pedantic is omitted because AmigaOS NDK headers use K&R-style
 *       declarations and other non-strict-C89 constructs.
 *
 * ANSI C (C89/C90) throughout the portable code sections.
 *
 * Copyright (C) 2026 BoltPack contributors
 * Licensed under the BSD 2-Clause License
 */

#include "../../core/archiver.h"
#include "../../core/bp_time.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_codepage.h"
#include "amiga_meta.h"

#ifndef BP_CREATOR_OS
#  define BP_CREATOR_OS BP_OS_AMIGA_68K
#endif
#ifndef BP_CREATOR_CPU
#  define BP_CREATOR_CPU BP_CPU_M68K
#endif

/* AmigaOS system headers */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <utime.h>

/* AmigaDOS does not grow a process stack on demand.  The cookie is honored by
 * newer shells; libnix also gets an explicit startup stack expansion below so
 * this CLI remains safe on the Workbench 2.04 shell used by our baseline. */
#if defined(__GNUC__)
static const char amiga_min_stack_cookie[] __attribute__((used)) = "$STACK:8192";
#endif

#if defined(__GNUC__) && defined(__libnix__)
/* libnix's swapstack support moves this process to a larger stack if needed. */
unsigned long __stack = 8192UL;
extern void __stkinit(void);
extern void __stkexit(void);

static void amiga_restore_stack(void)
{
    __stkexit();
}

static int amiga_expand_stack(void)
{
    __stkinit();
    if (atexit(amiga_restore_stack) != 0) {
        __stkexit();
        return -1;
    }
    return 0;
}
#endif

/* ---- constants --------------------------------------------------------- */

/* AmigaOS supports long names (up to 107 chars per component), give room
 * for nested paths. */
#define BP_MAX_PATH  512

/* AmigaOS Western-European locale uses ISO-8859-1 (Latin-1).
 * Change to BP_CP_437 or another constant for non-Western builds. */
#define AMIGA_CP     BP_CP_8859_1

/* Metadata payload: 4 bytes prot + 1 byte comment-len + up to 79 bytes */
#define AMIGA_META_PAYLOAD_MAX  84

/* ---- verbosity --------------------------------------------------------- */

#define VERB_DEFAULT 0
#define VERB_QUIET   1
#define VERB_SILENT  2

static int g_verbosity = VERB_DEFAULT;
#ifdef BP_TEST_FAIL_PUBLISH
static int g_test_fail_publish = 0;
#endif

/* ---- bp_io_t adapters for FILE * --------------------------------------- */

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
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)ftell((FILE *)handle);
}

static void bp_io_from_file(bp_io_t *io, FILE *fp)
{
    io->read   = file_read;
    io->write  = file_write;
    io->seek   = file_seek;
    io->tell   = file_tell;
    io->handle = fp;
}

/* ---- read-only memory buffer I/O (for sending metadata payload) --------- */

typedef struct {
    const unsigned char *buf;
    bp_uint32            size;
    bp_uint32            pos;
} rbuf_ctx_t;

static long rbuf_read_fn(void *handle, void *buf, bp_uint32 count)
{
    rbuf_ctx_t *ctx = (rbuf_ctx_t *)handle;
    if (ctx->pos >= ctx->size) return 0;
    if (count > ctx->size - ctx->pos) count = ctx->size - ctx->pos;
    memcpy(buf, ctx->buf + ctx->pos, (size_t)count);
    ctx->pos += count;
    return (long)count;
}

static long rbuf_write_fn(void *handle, const void *buf, bp_uint32 count)
{
    (void)handle; (void)buf; (void)count;
    return -1; /* read-only */
}

static int rbuf_seek_fn(void *handle, bp_off_t offset, int origin)
{
    rbuf_ctx_t *ctx = (rbuf_ctx_t *)handle;
    long newpos;
    if (origin == SEEK_SET)       newpos = (long)offset;
    else if (origin == SEEK_CUR)  newpos = (long)ctx->pos  + (long)offset;
    else if (origin == SEEK_END)  newpos = (long)ctx->size + (long)offset;
    else return -1;
    if (newpos < 0 || (bp_uint32)newpos > ctx->size) return -1;
    ctx->pos = (bp_uint32)newpos;
    return 0;
}

static bp_off_t rbuf_tell_fn(void *handle)
{
    return (bp_off_t)((rbuf_ctx_t *)handle)->pos;
}

static void bp_io_from_rbuf(bp_io_t *io, rbuf_ctx_t *ctx)
{
    io->read   = rbuf_read_fn;
    io->write  = rbuf_write_fn;
    io->seek   = rbuf_seek_fn;
    io->tell   = rbuf_tell_fn;
    io->handle = ctx;
}

/* ---- write-only memory buffer I/O (for receiving metadata payload) ----- */

typedef struct {
    unsigned char *buf;
    bp_uint32      cap;
    bp_uint32      len; /* bytes written */
    bp_uint32      pos; /* current position */
} wbuf_ctx_t;

static long wbuf_read_fn(void *handle, void *buf, bp_uint32 count)
{
    (void)handle; (void)buf; (void)count;
    return -1; /* write-only */
}

static long wbuf_write_fn(void *handle, const void *data, bp_uint32 count)
{
    wbuf_ctx_t *ctx = (wbuf_ctx_t *)handle;
    if (ctx->pos + count > ctx->cap) count = ctx->cap - ctx->pos;
    if (count == 0) return 0;
    memcpy(ctx->buf + ctx->pos, data, (size_t)count);
    ctx->pos += count;
    if (ctx->pos > ctx->len) ctx->len = ctx->pos;
    return (long)count;
}

static int wbuf_seek_fn(void *handle, bp_off_t offset, int origin)
{
    wbuf_ctx_t *ctx = (wbuf_ctx_t *)handle;
    long newpos;
    if (origin == SEEK_SET)       newpos = (long)offset;
    else if (origin == SEEK_CUR)  newpos = (long)ctx->pos + (long)offset;
    else if (origin == SEEK_END)  newpos = (long)ctx->len + (long)offset;
    else return -1;
    if (newpos < 0 || (bp_uint32)newpos > ctx->cap) return -1;
    ctx->pos = (bp_uint32)newpos;
    return 0;
}

static bp_off_t wbuf_tell_fn(void *handle)
{
    return (bp_off_t)((wbuf_ctx_t *)handle)->pos;
}

/*
 * meta_extract_t: used when extracting a BP_TYPE_META entry.
 * io MUST be the first member so that (meta_extract_t *)out_io is valid
 * (address of first member equals address of struct in C89).
 */
typedef struct {
    bp_io_t       io;                              /* MUST be first */
    wbuf_ctx_t    wctx;
    unsigned char buf[AMIGA_META_PAYLOAD_MAX + 4]; /* payload storage */
} meta_extract_t;

/* ---- error messages ---------------------------------------------------- */

static const char *bp_strerror(int code)
{
    switch (code) {
    case BP_OK:                    return "success";
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
    case BP_ERR_PARTIAL:           return "some attributes were not restored";
    default:                       return "unknown error";
    }
}

/* ---- speed formatting -------------------------------------------------- */

static void format_speed(unsigned long bytes_per_sec, char *buf, size_t bufsize)
{
    unsigned long val, frac;
    (void)bufsize;
    if (bytes_per_sec < 1024UL) {
        sprintf(buf, "%lu bytes/s", bytes_per_sec);
    } else if (bytes_per_sec < 1048576UL) {
        val  = bytes_per_sec / 1024UL;
        frac = (bytes_per_sec % 1024UL) * 100UL / 1024UL;
        sprintf(buf, "%lu.%02lu KB/s", val, frac);
    } else {
        val  = bytes_per_sec / 1048576UL;
        frac = (bytes_per_sec % 1048576UL) * 100UL / 1048576UL;
        sprintf(buf, "%lu.%02lu MB/s", val, frac);
    }
}

/* ---- size formatting --------------------------------------------------- */

static void format_size(bp_uint32 bytes, char *buf, size_t bufsize)
{
    unsigned long val, frac;
    (void)bufsize;
    if (bytes < 1024UL) {
        sprintf(buf, "%lu bytes", (unsigned long)bytes);
    } else if (bytes < 1048576UL) {
        val  = (unsigned long)(bytes / 1024UL);
        frac = (unsigned long)((bytes % 1024UL) * 10UL / 1024UL);
        sprintf(buf, "%lu.%lu KB", val, frac);
    } else if (bytes < 1073741824UL) {
        val  = (unsigned long)(bytes / 1048576UL);
        frac = (unsigned long)((bytes % 1048576UL) * 10UL / 1048576UL);
        sprintf(buf, "%lu.%lu MB", val, frac);
    } else {
        val  = (unsigned long)(bytes / 1073741824UL);
        frac = (unsigned long)((bytes % 1073741824UL) / 107374182UL);
        sprintf(buf, "%lu.%lu GB", val, frac);
    }
}

static void format_size64(bp_uint64 bytes, char *buf, size_t bufsize)
{
    unsigned long gb, frac;
    (void)bufsize;
    if (bytes.hi == 0) {
        format_size(bytes.lo, buf, bufsize);
        return;
    }
    gb   = (unsigned long)bytes.hi * 4UL + (unsigned long)(bytes.lo / 1073741824UL);
    frac = (unsigned long)((bytes.lo % 1073741824UL) / 107374182UL);
    if (gb < 1024UL) {
        sprintf(buf, "%lu.%lu GB", gb, frac);
    } else {
        sprintf(buf, "%lu TB", gb / 1024UL);
    }
}

/* ---- number formatting ------------------------------------------------- */

static void format_number(bp_uint32 n, char *buf, size_t bufsize)
{
    char tmp[32];
    int  len, i, j, commas;

    sprintf(tmp, "%lu", (unsigned long)n);
    len    = (int)strlen(tmp);
    commas = (len - 1) / 3;
    if ((size_t)(len + commas + 1) > bufsize) {
        strcpy(buf, tmp);
        return;
    }
    j = len + commas;
    buf[j] = '\0';
    for (i = len - 1; i >= 0; i--) {
        buf[--j] = tmp[i];
        if (i > 0 && (len - i) % 3 == 0) buf[--j] = ',';
    }
}

/* ---- Amiga protection bits display ------------------------------------- */

/*
 * Format an AmigaOS protection value as an 8-character string.
 * Bit order (high to low): h s p a r w e d
 *   r/w/e/d are inverted: lowercase = permission granted (bit clear)
 *                         uppercase = permission denied  (bit set)
 *   h/s/p/a are flags:    uppercase = flag set, '-' = flag clear
 */
static void format_prot(unsigned long prot, char *buf)
{
    /* Upper 4 flags: not inverted (uppercase = set) */
    buf[0] = (prot & FIBF_HOLD)    ? 'H' : '-';
    buf[1] = (prot & FIBF_SCRIPT)  ? 'S' : '-';
    buf[2] = (prot & FIBF_PURE)    ? 'P' : '-';
    buf[3] = (prot & FIBF_ARCHIVE) ? 'A' : '-';
    /* Lower 4 permissions: inverted (lowercase = bit clear = allowed) */
    buf[4] = (prot & FIBF_READ)    ? 'R' : 'r';
    buf[5] = (prot & FIBF_WRITE)   ? 'W' : 'w';
    buf[6] = (prot & FIBF_EXECUTE) ? 'E' : 'e';
    buf[7] = (prot & FIBF_DELETE)  ? 'D' : 'd';
    buf[8] = '\0';
}

/* ---- progress callback ------------------------------------------------- */

static int progress_cb(const bp_progress_t *p, void *user_data)
{
    (void)p;
    (void)user_data;
    return 0;
}

/* ---- path utilities ---------------------------------------------------- */

static int ends_with_slash(const char *s)
{
    size_t len = strlen(s);
    return len > 0 && s[len - 1] == '/';
}

/* Concatenate dir + "/" + name into buf. */
static void path_join(char *buf, size_t bufsize, const char *dir,
                      const char *name)
{
    if (ends_with_slash(dir)) {
        snprintf(buf, bufsize, "%s%s", dir, name);
    } else {
        snprintf(buf, bufsize, "%s/%s", dir, name);
    }
}

/*
 * Strip volume prefix ("VOL:") and any leading path separators so that
 * "DH0:Work/myproject" becomes "Work/myproject".
 */
static const char *clean_path(const char *path)
{
    const char *colon = strchr(path, ':');
    if (colon) path = colon + 1;   /* skip "VOL:" */
    while (*path == '/') path++;   /* skip leading separators */
    return path;
}

/* ---- mkdir -p ---------------------------------------------------------- */

static int mkdirs(const char *path)
{
    char  tmp[BP_MAX_PATH];
    char *p;

    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int mkdirs_for_file(const char *path)
{
    char  tmp[BP_MAX_PATH];
    char *slash;

    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    slash = strrchr(tmp, '/');
    if (slash != NULL && slash != tmp) {
        *slash = '\0';
        if (mkdirs(tmp) != 0) return -1;
    }
    return 0;
}

/* ---- parse block size -------------------------------------------------- */

static bp_uint32 parse_block_size(const char *s)
{
    if (strcmp(s, "8k")   == 0 || strcmp(s, "8K")   == 0) return BP_BLOCK_8K;
    if (strcmp(s, "16k")  == 0 || strcmp(s, "16K")  == 0) return BP_BLOCK_16K;
    if (strcmp(s, "32k")  == 0 || strcmp(s, "32K")  == 0) return BP_BLOCK_32K;
    if (strcmp(s, "64k")  == 0 || strcmp(s, "64K")  == 0) return BP_BLOCK_64K;
    if (strcmp(s, "256k") == 0 || strcmp(s, "256K") == 0) return BP_BLOCK_256K;
    return 0;
}

/* ---- AmigaOS metadata helpers ----------------------------------------- */

/*
 * Encode Amiga metadata into buf[].
 * Returns the number of bytes written (5..84).
 */
static int amiga_encode_meta(unsigned char *buf, unsigned long prot,
                             const char *comment)
{
    int clen;
    buf[0] = (unsigned char)( prot        & 0xFF);
    buf[1] = (unsigned char)((prot >>  8) & 0xFF);
    buf[2] = (unsigned char)((prot >> 16) & 0xFF);
    buf[3] = (unsigned char)((prot >> 24) & 0xFF);
    clen = (int)strlen(comment);
    if (clen > 79) clen = 79;
    buf[4] = (unsigned char)clen;
    if (clen > 0) memcpy(buf + 5, comment, (size_t)clen);
    return 5 + clen;
}

/*
 * Decode Amiga metadata from buf[0..len-1].
 * prot and comment (null-terminated, max 80 bytes) are filled in.
 */
/*
 * Query protection bits and file comment for fs_path using AmigaDOS
 * Lock() + Examine().  The caller must report failure rather than archiving
 * invented metadata.
 */
static int amiga_get_meta(const char *fs_path, unsigned long *prot,
                          char *comment)
{
    BPTR lock;
    struct FileInfoBlock *fib;

    lock = Lock((STRPTR)fs_path, ACCESS_READ);
    if (!lock) {
        *prot = FIBF_ARCHIVE; comment[0] = '\0'; return -1;
    }
    fib = (struct FileInfoBlock *)AllocVec(
        sizeof(struct FileInfoBlock), MEMF_ANY | MEMF_CLEAR);
    if (!fib) {
        UnLock(lock); *prot = FIBF_ARCHIVE; comment[0] = '\0'; return -1;
    }
    if (!Examine(lock, fib)) {
        FreeVec(fib); UnLock(lock);
        *prot = FIBF_ARCHIVE; comment[0] = '\0'; return -1;
    }
    *prot = (unsigned long)(ULONG)fib->fib_Protection;
    {
        int i;
        for (i = 0; i < 79 && fib->fib_Comment[i] != '\0'; i++)
            comment[i] = (char)fib->fib_Comment[i];
        comment[i] = '\0';
    }
    FreeVec(fib);
    UnLock(lock);
    return 0;
}

/*
 * Restore protection bits and file comment for fs_path using AmigaDOS
 * SetProtection() and SetComment().
 */
static int amiga_apply_meta(const char *fs_path, unsigned long prot,
                            const char *comment)
{
    if (!SetComment((STRPTR)fs_path, (STRPTR)comment)) return -1;
    if (!SetProtection((STRPTR)fs_path, (LONG)prot)) return -1;
    return 0;
}

/* Preserve epoch zero; mark negative or host-unrepresentable times unknown. */
static bp_uint16 amiga_entry_mtime(time_t source, bp_uint64 *mtime)
{
    unsigned long seconds;
    *mtime = bp_u64_from_u32(0);
    if (source < (time_t)0) return 0;
    seconds = (unsigned long)source;
    if ((time_t)seconds != source) return 0;
    *mtime = bp_u64_from_ulong(seconds);
    return BP_ENTRY_FLAG_MTIME_VALID;
}

/*
 * Auto-detect the best block size based on AmigaDOS available memory.
 * Uses exec.library AvailMem(MEMF_ANY | MEMF_LARGEST).
 * Returns a conservative estimate: reserves 64 KB for the decompressor,
 * stack, and other overhead.
 */
static bp_uint32 amiga_auto_block_size(void)
{
    ULONG avail = AvailMem(MEMF_ANY | MEMF_LARGEST);
    /* Reserve ~64 KB overhead; boltlz decompress needs no extra RAM beyond
     * the block itself, but we leave room for the C stack and buffers. */
    if      (avail >= 640UL * 1024UL) return BP_BLOCK_256K;
    else if (avail >= 256UL * 1024UL) return BP_BLOCK_64K;
    else if (avail >= 128UL * 1024UL) return BP_BLOCK_32K;
    else if (avail >=  64UL * 1024UL) return BP_BLOCK_16K;
    return BP_BLOCK_8K;
}

/* ---- recursive add ----------------------------------------------------- */

/*
 * Add a BP_TYPE_META entry immediately after the corresponding data or
 * directory entry.  Reads fib_Protection and fib_Comment from the
 * filesystem via Lock/Examine, encodes them into a small payload, and
 * stores it under the same archive name as the parent entry.
 */
static int add_meta_entry(bp_archive_t *ar, const char *arcname,
                          const char *fs_path)
{
    unsigned char meta_buf[AMIGA_META_PAYLOAD_MAX];
    unsigned long prot;
    char          comment[80];
    int           meta_len;
    bp_io_t       meta_io;
    rbuf_ctx_t    rctx;

    if (amiga_get_meta(fs_path, &prot, comment) != 0) {
        fprintf(stderr, "boltpack: cannot read Amiga metadata: %s\n", fs_path);
        return BP_ERR_IO;
    }
    meta_len = amiga_encode_meta(meta_buf, prot, comment);

    rctx.buf  = meta_buf;
    rctx.size = (bp_uint32)meta_len;
    rctx.pos  = 0;
    bp_io_from_rbuf(&meta_io, &rctx);

    return bp_archive_add_entry_ex(ar, arcname, BP_TYPE_META, BP_CREATOR_OS,
                                &meta_io, bp_u64_from_u32((bp_uint32)meta_len),
                                bp_u64_from_u32(0), 0);
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname);

static int amiga_stat_same_file(const struct stat *a, const struct stat *b)
{
    return S_ISREG(a->st_mode) && S_ISREG(b->st_mode) &&
           a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime &&
           a->st_ctime == b->st_ctime;
}

static int amiga_stat_same_directory(const struct stat *a,
                                     const struct stat *b)
{
    return S_ISDIR(a->st_mode) && S_ISDIR(b->st_mode) &&
           a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime;
}

#ifdef BP_TEST_PAUSE_AFTER_SOURCE_OPEN
static void amiga_test_pause_after_source_open(void)
{
    FILE *fp = fopen("DH0:SOURCE-OPENED.TXT", "wb");
    if (fp == NULL) return;
    (void)fputs("source opened\n", fp);
    (void)fclose(fp);
    for (;;) {
        fp = fopen("DH0:CONTINUE.TXT", "rb");
        if (fp != NULL) {
            (void)fclose(fp);
            break;
        }
        Delay(10);
    }
}
#endif

typedef struct amiga_add_dir_frame {
    DIR *dir;
    char *fs_path;
    char *arc_prefix;
    struct stat identity;
    struct amiga_add_dir_frame *parent;
} amiga_add_dir_frame_t;

static int add_directory_record(bp_archive_t *ar, const char *path,
                                const char *arcname, const struct stat *st,
                                char *dir_arc_out)
{
    char *names, *dir_arc, *dir_arc_utf8;
    bp_uint64 mtime;
    bp_uint16 mtime_flags;
    size_t arc_len = strlen(arcname);
    int rc;

    if (arc_len + 2 > BP_MAX_PATH) return BP_ERR_NAME_TOO_LONG;
    names = (char *)malloc(BP_MAX_PATH * 3U);
    if (names == NULL) return BP_ERR_NOMEM;
    dir_arc = names;
    dir_arc_utf8 = names + BP_MAX_PATH;
    if (!ends_with_slash(arcname)) sprintf(dir_arc, "%s/", arcname);
    else strcpy(dir_arc, arcname);
    if (g_verbosity == VERB_DEFAULT)
        printf("   creating: %s\n", dir_arc);
    if (bp_local_to_utf8(dir_arc, dir_arc_utf8, BP_MAX_PATH * 2U,
                         AMIGA_CP) != 0) {
        free(names);
        return BP_ERR_INVALID_ARG;
    }
    mtime_flags = amiga_entry_mtime(st->st_mtime, &mtime);
    if (mtime_flags == 0 && st->st_mtime != (time_t)0)
        fprintf(stderr,
                "boltpack: source timestamp is out of range; storing unknown: %s\n",
                path);
    rc = bp_archive_add_entry_ex(ar, dir_arc_utf8, BP_TYPE_DIR,
                                 BP_CREATOR_OS, NULL, bp_u64_from_u32(0),
                                 mtime, mtime_flags);
    if (rc == BP_OK) rc = add_meta_entry(ar, dir_arc_utf8, path);
    if (rc == BP_OK) {
        struct stat current;
        if (lstat(path, &current) != 0 ||
            !amiga_stat_same_directory(st, &current)) rc = BP_ERR_IO;
    }
    if (rc == BP_OK) strcpy(dir_arc_out, dir_arc);
    free(names);
    return rc;
}

static int add_directory(bp_archive_t *ar, const char *dir_path,
                         const char *arc_prefix,
                         const struct stat *expected_stat)
{
    amiga_add_dir_frame_t *top;
    struct stat dir_stat;
    int rc = BP_OK;

    if (lstat(dir_path, &dir_stat) != 0 || S_ISLNK(dir_stat.st_mode) ||
        !S_ISDIR(dir_stat.st_mode) ||
        !amiga_stat_same_directory(expected_stat, &dir_stat)) return BP_ERR_IO;

    top = (amiga_add_dir_frame_t *)calloc(1, sizeof(*top));
    if (top == NULL) return BP_ERR_NOMEM;
    top->fs_path = (char *)malloc(strlen(dir_path) + 1);
    top->arc_prefix = (char *)malloc(strlen(arc_prefix) + 1);
    if (top->fs_path == NULL || top->arc_prefix == NULL) {
        free(top->fs_path); free(top->arc_prefix); free(top);
        return BP_ERR_NOMEM;
    }
    strcpy(top->fs_path, dir_path);
    strcpy(top->arc_prefix, arc_prefix);
    top->identity = dir_stat;
    top->dir = opendir(top->fs_path);
    if (top->dir == NULL) {
        fprintf(stderr, "boltpack: cannot open directory: %s\n", top->fs_path);
        free(top->fs_path); free(top->arc_prefix); free(top);
        return BP_ERR_IO;
    }

    while (top != NULL && rc == BP_OK) {
        struct dirent *ent;
        struct stat current_dir;
        char *child_path = NULL, *child_arc = NULL;
        struct stat child_stat;

        if (lstat(top->fs_path, &current_dir) != 0 ||
            !amiga_stat_same_directory(&top->identity, &current_dir)) {
            rc = BP_ERR_IO;
            break;
        }

        errno = 0;
        ent = readdir(top->dir);
        if (ent == NULL) {
            int read_error = errno != 0;
            amiga_add_dir_frame_t *done = top;
            if (lstat(done->fs_path, &current_dir) != 0 ||
                !amiga_stat_same_directory(&done->identity, &current_dir))
                read_error = 1;
            top = done->parent;
            if (closedir(done->dir) != 0 || read_error) rc = BP_ERR_IO;
            free(done->fs_path); free(done->arc_prefix); free(done);
            continue;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (strlen(top->fs_path) + strlen(ent->d_name) + 2 > BP_MAX_PATH ||
            strlen(top->arc_prefix) + strlen(ent->d_name) + 2 > BP_MAX_PATH) {
            rc = BP_ERR_NAME_TOO_LONG;
            continue;
        }
        child_path = (char *)malloc(BP_MAX_PATH);
        child_arc = (char *)malloc(BP_MAX_PATH);
        if (child_path == NULL || child_arc == NULL) {
            free(child_path); free(child_arc);
            rc = BP_ERR_NOMEM;
            continue;
        }
        path_join(child_path, BP_MAX_PATH, top->fs_path, ent->d_name);
        path_join(child_arc, BP_MAX_PATH, top->arc_prefix, ent->d_name);
        if (lstat(child_path, &child_stat) != 0 || S_ISLNK(child_stat.st_mode)) {
            fprintf(stderr, "boltpack: cannot archive source or symbolic link: %s\n",
                    child_path);
            rc = BP_ERR_IO;
        } else if (S_ISDIR(child_stat.st_mode)) {
            char *dir_arc = (char *)malloc(BP_MAX_PATH);
            char *nested_path = NULL, *nested_arc = NULL;
            DIR *nested_dir = NULL;
            amiga_add_dir_frame_t *frame = NULL;
            if (dir_arc == NULL) rc = BP_ERR_NOMEM;
            else rc = add_directory_record(ar, child_path, child_arc,
                                            &child_stat, dir_arc);
            if (rc == BP_OK) {
                nested_path = child_path;
                nested_arc = dir_arc;
                nested_dir = opendir(nested_path);
                if (nested_dir == NULL) {
                    fprintf(stderr, "boltpack: cannot open directory: %s\n",
                            nested_path);
                    rc = BP_ERR_IO;
                } else {
                    struct stat opened_dir;
                    if (lstat(nested_path, &opened_dir) != 0 ||
                        !amiga_stat_same_directory(&child_stat, &opened_dir)) {
                        (void)closedir(nested_dir);
                        nested_dir = NULL;
                        rc = BP_ERR_IO;
                    }
                }
            }
            if (rc == BP_OK) {
                frame = (amiga_add_dir_frame_t *)calloc(1, sizeof(*frame));
                if (frame == NULL) rc = BP_ERR_NOMEM;
            }
            if (rc == BP_OK) {
                frame->dir = nested_dir;
                frame->fs_path = nested_path;
                frame->arc_prefix = nested_arc;
                frame->identity = child_stat;
                frame->parent = top;
                top = frame;
                child_path = NULL;
                dir_arc = NULL;
            } else if (nested_dir != NULL) {
                closedir(nested_dir);
            }
            free(dir_arc);
        } else {
            rc = add_path(ar, child_path, child_arc);
        }
        free(child_path);
        free(child_arc);
    }

    while (top != NULL) {
        amiga_add_dir_frame_t *parent = top->parent;
        if (closedir(top->dir) != 0 && rc == BP_OK) rc = BP_ERR_IO;
        free(top->fs_path); free(top->arc_prefix); free(top);
        top = parent;
    }
    return rc;
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    struct stat st;
    int         rc;

    if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) {
        fprintf(stderr, "boltpack: cannot archive source or symbolic link: %s\n", path);
        return BP_ERR_IO;
    }

    if (S_ISDIR(st.st_mode)) {
        char *dir_arc;
        if (strlen(arcname) + 2 > BP_MAX_PATH) return BP_ERR_NAME_TOO_LONG;
        dir_arc = (char *)malloc(BP_MAX_PATH);
        if (dir_arc == NULL) return BP_ERR_NOMEM;
        rc = add_directory_record(ar, path, arcname, &st, dir_arc);
                if (rc == BP_OK) rc = add_directory(ar, path, dir_arc, &st);
        free(dir_arc);
        return rc;

    } else if (S_ISREG(st.st_mode)) {
        FILE      *fp;
        bp_io_t    fio;
        struct stat opened_st, final_st, path_st;
        bp_uint64  file_size;
        bp_uint64  mtime;
        bp_uint16  mtime_flags;
        char      *arcname_utf8;

        if (st.st_size < 0) return BP_ERR_OVERFLOW;
        file_size = bp_u64_from_ulong((unsigned long)st.st_size);
        arcname_utf8 = (char *)malloc(BP_MAX_PATH * 2U);
        if (!arcname_utf8) return BP_ERR_NOMEM;
        fp = fopen(path, "rb");
        if (fp == NULL) {
            fprintf(stderr, "boltpack: cannot open: %s\n", path);
            free(arcname_utf8);
            return BP_ERR_IO;
        }
        if (fstat(fileno(fp), &opened_st) != 0 ||
            !amiga_stat_same_file(&st, &opened_st)) {
            (void)fclose(fp);
            free(arcname_utf8);
            fprintf(stderr, "boltpack: source changed while opening: %s\n", path);
            return BP_ERR_IO;
        }
#ifdef BP_TEST_PAUSE_AFTER_SOURCE_OPEN
        amiga_test_pause_after_source_open();
#endif
        bp_io_from_file(&fio, fp);

        if (g_verbosity == VERB_DEFAULT) {
            printf("compressing: %s", arcname);
            fflush(stdout);
        }

        if (bp_local_to_utf8(arcname, arcname_utf8, BP_MAX_PATH * 2U,
                             AMIGA_CP) != 0) {
            fclose(fp);
            free(arcname_utf8);
            return BP_ERR_INVALID_ARG;
        }

        mtime_flags = amiga_entry_mtime(st.st_mtime, &mtime);
        if (mtime_flags == 0 && st.st_mtime != (time_t)0)
            fprintf(stderr,
                    "boltpack: source timestamp is out of range; storing unknown: %s\n",
                    path);
        rc = bp_archive_add_entry_ex(ar, arcname_utf8, BP_TYPE_DATA,
                                     BP_CREATOR_OS, &fio, file_size,
                                     mtime, mtime_flags);
        if (rc == BP_OK &&
            (fstat(fileno(fp), &final_st) != 0 ||
             !amiga_stat_same_file(&opened_st, &final_st) ||
             lstat(path, &path_st) != 0 ||
             !amiga_stat_same_file(&opened_st, &path_st)))
            rc = BP_ERR_IO;
        if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
        if (rc != BP_OK) { free(arcname_utf8); return rc; }

        if (g_verbosity == VERB_DEFAULT) {
            bp_entry_t *e    = &ar->entry_list[ar->entries_written - 1];
            bp_uint32   comp = bp_u64_to_u32(e->compressed_size);
            bp_uint32   fsz  = bp_u64_to_u32(file_size);
            char        sizebuf[32];
            if (comp == 0 || e->method == BP_METHOD_STORED || comp >= fsz) {
                format_size(fsz, sizebuf, sizeof(sizebuf));
                printf("  %s (stored)\n", sizebuf);
            } else {
                bp_uint32 pct_int = (fsz > 0) ? (bp_uint32)((unsigned long)comp * 100UL / (unsigned long)fsz) : 0;
                format_size(comp, sizebuf, sizeof(sizebuf));
                printf("  %s (%lu%%)\n", sizebuf, (unsigned long)pct_int);
            }
        }

        /* Preserve file protection bits and comment */
        rc = add_meta_entry(ar, arcname_utf8, path);
        if (rc == BP_OK &&
            (lstat(path, &path_st) != 0 ||
             !amiga_stat_same_file(&opened_st, &path_st)))
            rc = BP_ERR_IO;
        free(arcname_utf8);
        return rc;
    }

    fprintf(stderr, "boltpack: unsupported source type: %s\n", path);
    return BP_ERR_IO;
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
    list_ctx_t   *ctx = (list_ctx_t *)user_data;
    const char   *type_str;
    char          orig_buf[32], comp_buf[32], disp_name[BP_MAX_PATH];

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
    } else if (entry->entry_type == BP_TYPE_DATA ||
               entry->entry_type == BP_TYPE_SYMLINK) {
        ctx->file_count++;
    }
    ctx->total_orig = bp_u64_add(ctx->total_orig, entry->original_size);
    ctx->total_comp = bp_u64_add(ctx->total_comp, entry->compressed_size);

    if (bp_utf8_to_local(entry->name, disp_name, sizeof(disp_name), AMIGA_CP) != 0) strcpy(disp_name, "<unrepresentable>");

    format_number(bp_u64_to_u32(entry->original_size),   orig_buf, sizeof(orig_buf));
    format_number(bp_u64_to_u32(entry->compressed_size),  comp_buf, sizeof(comp_buf));

    /* For META entries from this platform, decode and show protection bits. */
    if (entry->entry_type == BP_TYPE_META &&
        entry->platform   == BP_OS_AMIGA_68K) {
        /* Skip displaying META as a separate entry; its info is shown
         * inline when listing with -v (not yet implemented).
         * For now, show a compact indication. */
        printf("%10s %10s  %s  %s [prot+comment]\n",
               orig_buf, comp_buf, type_str, disp_name);
    } else {
        printf("%10s %10s  %s  %s\n",
               orig_buf, comp_buf, type_str, disp_name);
    }
    return 0;
}

/* ---- extract callbacks ------------------------------------------------- */

#define AMIGA_DIR_QUEUE_MAX       256U
#define AMIGA_DIR_QUEUE_BYTES_MAX 32768UL

typedef struct amiga_dir_restore {
    struct amiga_dir_restore *next;
    char                     *path;
    unsigned long             path_len;
    bp_uint64                 mtime;
    int                       mtime_valid;
    int                       native_seen;
    unsigned long             protection;
    char                      comment[80];
} amiga_dir_restore_t;

typedef struct {
    bp_io_t    io;
    int        fd;
    char       target[BP_MAX_PATH];
    char       temporary[BP_MAX_PATH];
    int        target_existed;
    int        recovery_hint;
    struct stat target_snapshot;
} amiga_staged_output_t;

static int amiga_path_from_name(const char *out_dir, const char *local_name,
                                char *path, size_t path_size)
{
    size_t len;
    if (out_dir != NULL) {
        if (strlen(out_dir) + strlen(local_name) + 2 > path_size) return -1;
        path_join(path, path_size, out_dir, local_name);
    } else {
        if (strlen(local_name) + 1 > path_size) return -1;
        strcpy(path, local_name);
    }
    len = strlen(path);
    while (len > 0 && path[len - 1] == '/') path[--len] = '\0';
    return 0;
}

static int amiga_local_name_safe(const char *name)
{
    const unsigned char *p = (const unsigned char *)name;
    while (*p != '\0') {
        if (*p == ':') return 0; /* AmigaDOS volume/device separator */
        p++;
    }
    return 1;
}

static unsigned char amiga_fold_name_byte(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return (unsigned char)(c + ('a' - 'A'));
    if ((c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xDE))
        return (unsigned char)(c + 0x20);
    return c;
}

static void amiga_fold_name(char *name)
{
    unsigned char *p = (unsigned char *)name;
    while (*p != '\0') {
        *p = amiga_fold_name_byte(*p);
        p++;
    }
}

#define AMIGA_NAME_INDEX_MAX       1024U
#define AMIGA_NAME_INDEX_BYTES_MAX 32768UL

typedef struct {
    char *key;
    int   is_directory;
    int   explicit_entry;
} amiga_name_key_t;

typedef struct {
    const char *out_dir;
    amiga_name_key_t names[AMIGA_NAME_INDEX_MAX];
    unsigned int count;
    unsigned long key_bytes;
    const char *error_reason;
    char local_scratch[BP_MAX_PATH];
    char path_scratch[BP_MAX_PATH];
    char folded_scratch[BP_MAX_PATH];
} amiga_name_index_t;

static int amiga_index_path(amiga_name_index_t *index, const char *exact,
                            int is_directory, int explicit_entry)
{
    char *folded = index->folded_scratch;
    size_t len = strlen(exact);
    unsigned long needed = ((unsigned long)len + 1UL) * 2UL;
    unsigned int i;
    char *key;
    if (len == 0 || len >= BP_MAX_PATH) return 1;
    memcpy(folded, exact, len + 1);
    amiga_fold_name(folded);
    for (i = 0; i < index->count; i++) {
        amiga_name_key_t *old = &index->names[i];
        if (strcmp(old->key, folded) != 0) continue;
        if (strcmp(old->key + len + 1, exact) != 0 ||
            old->is_directory != is_directory ||
            (explicit_entry && old->explicit_entry)) {
            index->error_reason = "Amiga case-insensitive path collision";
            return 1;
        }
        if (explicit_entry) old->explicit_entry = 1;
        return 0;
    }
    if (index->count >= AMIGA_NAME_INDEX_MAX ||
        needed > AMIGA_NAME_INDEX_BYTES_MAX - index->key_bytes) {
        index->error_reason = "Amiga collision index limit reached";
        return 1;
    }
    key = (char *)malloc((size_t)needed);
    if (!key) { index->error_reason = "not enough memory for Amiga collision checks"; return 1; }
    memcpy(key, folded, len + 1);
    memcpy(key + len + 1, exact, len + 1);
    index->names[index->count].key = key;
    index->names[index->count].is_directory = is_directory;
    index->names[index->count].explicit_entry = explicit_entry;
    index->count++;
    index->key_bytes += needed;
    return 0;
}

static int amiga_preflight_name(const bp_entry_t *entry, void *user_data)
{
    amiga_name_index_t *index = (amiga_name_index_t *)user_data;
    char *local_name = index->local_scratch;
    char *full_path = index->path_scratch;
    size_t len, pos;
    int is_directory;

    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR &&
        entry->entry_type != BP_TYPE_SYMLINK) return 0;
    if (!bp_name_is_safe(entry->name) ||
        bp_utf8_to_local(entry->name, local_name, BP_MAX_PATH, AMIGA_CP) != 0 ||
        !amiga_local_name_safe(local_name) ||
        amiga_path_from_name(index->out_dir, local_name, full_path, BP_MAX_PATH) != 0) {
        index->error_reason = "unsafe or unrepresentable Amiga destination name";
        return 1;
    }
    len = strlen(local_name);
    while (len > 0 && local_name[len - 1] == '/') local_name[--len] = '\0';
    if (len == 0) { index->error_reason = "empty Amiga destination name"; return 1; }
    is_directory = entry->entry_type == BP_TYPE_DIR;
    for (pos = 0; pos <= len; pos++) {
        char saved;
        int final, rc;
        if (pos < len && local_name[pos] != '/') continue;
        final = pos == len;
        saved = local_name[pos]; local_name[pos] = '\0';
        rc = amiga_index_path(index, local_name, final ? is_directory : 1, final);
        local_name[pos] = saved;
        if (rc) return 1;
    }
    return 0;
}

static void amiga_name_index_clear(amiga_name_index_t *index)
{
    unsigned int i;
    for (i = 0; i < index->count; i++) free(index->names[i].key);
    index->count = 0;
    index->key_bytes = 0;
}

static int amiga_check_directory(const char *path, int create)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno != ENOENT || !create) return -1;
        if (mkdir(path, 0755) != 0 && errno != EEXIST) return -1;
        if (lstat(path, &st) != 0) return -1;
    }
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return -1;
    return 0;
}

/* Create or verify each path component without following existing links. */
static int amiga_make_directories_checked(const char *path)
{
    char tmp[BP_MAX_PATH];
    char *p;
    size_t len;
    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    len = strlen(tmp);
    while (len > 0 && tmp[len - 1] == '/') tmp[--len] = '\0';
    if (len == 0) return 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (*tmp != '\0' && strcmp(tmp, "/") != 0 &&
                !(p > tmp && p[-1] == ':') &&
                amiga_check_directory(tmp, 1) != 0) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    if (tmp[len - 1] == ':') return 0; /* volume root */
    return amiga_check_directory(tmp, 1);
}

static int amiga_make_parent_directories(const char *path)
{
    char parent[BP_MAX_PATH];
    char *slash;
    char *colon;
    if (strlen(path) >= sizeof(parent)) return -1;
    strcpy(parent, path);
    slash = strrchr(parent, '/');
    if (slash != NULL) {
        if (slash == parent) return 0;
        *slash = '\0';
        return amiga_make_directories_checked(parent);
    }
    colon = strchr(parent, ':');
    if (colon != NULL) {
        colon[1] = '\0';
        return 0; /* current directory of the named volume */
    }
    return 0;
}

static int amiga_make_sibling_template(const char *path, const char *leaf,
                                       char *out, size_t out_size)
{
    const char *slash = strrchr(path, '/');
    const char *colon = strchr(path, ':');
    size_t prefix = 0;
    if (colon != NULL && (slash == NULL || colon > slash))
        prefix = (size_t)(colon - path) + 1;
    else if (slash != NULL)
        prefix = (size_t)(slash - path) + 1;
    if (prefix + strlen(leaf) + 1 > out_size) return -1;
    memcpy(out, path, prefix);
    strcpy(out + prefix, leaf);
    return 0;
}

static int amiga_make_archive_sibling_template(const char *path,
                                               const char *prefix,
                                               char *out, size_t out_size)
{
    const char *slash = strrchr(path, '/');
    const char *colon = strrchr(path, ':');
    const char *base;
    size_t parent = 0, prefix_len, hint_len = 0, i;
    if (colon != NULL && (slash == NULL || colon > slash))
        parent = (size_t)(colon - path) + 1U;
    else if (slash != NULL)
        parent = (size_t)(slash - path) + 1U;
    base = path + parent;
    prefix_len = strlen(prefix);
    if (parent + prefix_len + 6U + 1U > out_size) return -1;
    if (parent != 0) memcpy(out, path, parent);
    memcpy(out + parent, prefix, prefix_len);
    for (i = 0; base[i] != '\0' && hint_len < 32U; i++) {
        unsigned char ch = (unsigned char)base[i];
        if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
            out[parent + prefix_len + hint_len++] = (char)ch;
    }
    if (hint_len == 0) {
        memcpy(out + parent + prefix_len, "ARCHIVE", 7);
        hint_len = 7;
    }
    if (parent + prefix_len + hint_len + 6U + 1U > out_size) return -1;
    memset(out + parent + prefix_len + hint_len, 'X', 6U);
    out[parent + prefix_len + hint_len + 6U] = '\0';
    return 0;
}

static int amiga_stat_same_moved_file(const struct stat *a, const struct stat *b)
{
    return S_ISREG(a->st_mode) && S_ISREG(b->st_mode) &&
           a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime;
}

static long amiga_staged_write(void *handle, const void *data, bp_uint32 count)
{
    amiga_staged_output_t *stage = (amiga_staged_output_t *)handle;
    const unsigned char *p = (const unsigned char *)data;
    bp_uint32 written = 0;
    while (written < count) {
        long n = (long)write(stage->fd, p + written, (size_t)(count - written));
        if (n <= 0) return -1;
        written += (bp_uint32)n;
    }
    return (long)written;
}

static void amiga_discard_staged(amiga_staged_output_t *stage)
{
    if (stage->fd >= 0) {
        (void)close(stage->fd);
        stage->fd = -1;
    }
    if (stage->temporary[0] != '\0') {
        (void)unlink(stage->temporary);
        stage->temporary[0] = '\0';
    }
}

static int amiga_set_default_file_mode(const char *path)
{
    mode_t mask = umask(0);
    (void)umask(mask);
    return chmod(path, (mode_t)(0666 & ~mask));
}

static int amiga_commit_staged(amiga_staged_output_t *stage, int force)
{
    struct stat current;
    struct stat backup_stat;
    char backup[BP_MAX_PATH];
    int backup_fd;
    int has_backup = 0;
    int publish_ok;

    if (lstat(stage->target, &current) == 0) {
        if (!stage->target_existed ||
            !amiga_stat_same_file(&stage->target_snapshot, &current))
            return BP_ERR_PARTIAL;
    } else if (errno != ENOENT || stage->target_existed) {
        return BP_ERR_IO;
    }

    if (stage->target_existed) {
        if (!force) return BP_ERR_PARTIAL;
        if (stage->recovery_hint) {
            const char *slash = strrchr(stage->temporary, '/');
            const char *colon = strrchr(stage->temporary, ':');
            const char *leaf;
            size_t leaf_offset;
            if (colon != NULL && (slash == NULL || colon > slash))
                slash = colon;
            leaf_offset = slash != NULL
                ? (size_t)(slash - stage->temporary + 1) : 0U;
            leaf = stage->temporary + leaf_offset;
            if (strncmp(leaf, "BP", 2) != 0 ||
                strlen(stage->temporary) >= sizeof(backup))
                return BP_ERR_IO;
            strcpy(backup, stage->temporary);
            backup[leaf_offset + 1U] = 'R';
            backup_fd = open(backup, O_WRONLY | O_CREAT | O_EXCL, 0600);
        } else {
            if (amiga_make_sibling_template(stage->target, "BRXXXXXX",
                                            backup, sizeof(backup)) != 0)
                return BP_ERR_NAME_TOO_LONG;
            backup_fd = mkstemp(backup);
        }
        if (backup_fd < 0) return BP_ERR_IO;
        if (close(backup_fd) != 0 || unlink(backup) != 0) {
            (void)unlink(backup);
            return BP_ERR_IO;
        }
        if (!Rename((STRPTR)stage->target, (STRPTR)backup)) return BP_ERR_IO;
        has_backup = 1;
        if (lstat(backup, &backup_stat) != 0 ||
            !amiga_stat_same_moved_file(&stage->target_snapshot, &backup_stat)) {
            if (!Rename((STRPTR)backup, (STRPTR)stage->target))
                fprintf(stderr,
                        "boltpack: changed destination remains at recovery path: %s\n",
                        backup);
            return BP_ERR_PARTIAL;
        }
    }

#ifdef BP_TEST_PAUSE_AFTER_BACKUP
    if (has_backup) {
#ifdef BP_TEST_PAUSE_MARKER
        FILE *marker = fopen("DH0:RECOVERY-WINDOW.TXT", "wb");
        if (marker != NULL) {
            (void)fputs("original moved to recovery name\n", marker);
            (void)fclose(marker);
        }
        /* Use a host-visible release file instead of a timer: FS-UAE warp mode
           can advance guest delays much faster than wall-clock time. */
        for (;;) {
            FILE *release = fopen("DH0:CONTINUE.TXT", "rb");
            if (release != NULL) {
                (void)fclose(release);
                break;
            }
            Delay(50);
        }
#else
        Delay(250);
#endif
    }
#endif

#ifdef BP_TEST_FAIL_PUBLISH
    if (has_backup && g_test_fail_publish) {
        g_test_fail_publish = 0;
        publish_ok = 0;
    } else {
        publish_ok = Rename((STRPTR)stage->temporary,
                            (STRPTR)stage->target) != 0;
    }
#else
    publish_ok = Rename((STRPTR)stage->temporary, (STRPTR)stage->target) != 0;
#endif
    if (!publish_ok) {
        if (has_backup && !Rename((STRPTR)backup, (STRPTR)stage->target))
            fprintf(stderr,
                    "boltpack: original file remains at recovery path: %s\n",
                    backup);
        return BP_ERR_IO;
    }
    stage->temporary[0] = '\0';
    if (has_backup && !DeleteFile((STRPTR)backup)) {
        fprintf(stderr, "boltpack: cannot remove replaced-file backup: %s\n",
                backup);
        return BP_ERR_PARTIAL;
    }
    return BP_OK;
}

typedef struct {
    amiga_staged_output_t stage;
    unsigned long protection;
    char comment[80];
} amiga_archive_stage_t;

static int amiga_archive_stage_open(amiga_archive_stage_t *archive,
                                    const char *target, int allow_missing,
                                    FILE **out)
{
    amiga_staged_output_t *stage = &archive->stage;
    memset(archive, 0, sizeof(*archive));
    stage->fd = -1;
    *out = NULL;
    if (strlen(target) >= sizeof(stage->target)) return BP_ERR_NAME_TOO_LONG;
    strcpy(stage->target, target);
    if (lstat(target, &stage->target_snapshot) == 0) {
        stage->target_existed = 1;
        if (!S_ISREG(stage->target_snapshot.st_mode) ||
            amiga_get_meta(target, &archive->protection, archive->comment) != 0 ||
            (archive->protection & (FIBF_WRITE | FIBF_DELETE)) != 0)
            return BP_ERR_IO;
    } else if (errno != ENOENT || !allow_missing) return BP_ERR_IO;
    stage->recovery_hint = 1;
    if (amiga_make_archive_sibling_template(target, "BP", stage->temporary,
                                            sizeof(stage->temporary)) != 0)
        return BP_ERR_NAME_TOO_LONG;
    stage->fd = mkstemp(stage->temporary);
    if (stage->fd < 0) { stage->temporary[0] = '\0'; return BP_ERR_IO; }
    if (amiga_set_default_file_mode(stage->temporary) != 0) {
        amiga_discard_staged(stage);
        return BP_ERR_IO;
    }
    *out = fdopen(stage->fd, "w+b");
    if (*out == NULL) { amiga_discard_staged(stage); return BP_ERR_IO; }
    return BP_OK;
}

static void amiga_archive_stage_discard(amiga_archive_stage_t *archive, FILE **fp)
{
    if (*fp != NULL) { (void)fclose(*fp); *fp = NULL; archive->stage.fd = -1; }
    amiga_discard_staged(&archive->stage);
}

static int amiga_archive_stage_finish(amiga_archive_stage_t *archive, FILE **fp)
{
    int rc;
    if (fflush(*fp) != 0) { amiga_archive_stage_discard(archive, fp); return BP_ERR_IO; }
    rc = fclose(*fp);
    *fp = NULL;
    archive->stage.fd = -1;
    if (rc != 0 || (archive->stage.target_existed &&
        amiga_apply_meta(archive->stage.temporary, archive->protection,
                         archive->comment) != 0)) {
        amiga_discard_staged(&archive->stage);
        return BP_ERR_IO;
    }
    rc = amiga_commit_staged(&archive->stage, 1);
    if (rc != BP_OK) amiga_discard_staged(&archive->stage);
    return rc;
}

typedef struct extract_ctx extract_ctx_t;

static amiga_dir_restore_t *amiga_queue_directory(extract_ctx_t *ctx,
                                                   const char *path,
                                                   const bp_entry_t *entry);

struct extract_ctx {
    const char *out_dir;
    int         force;
    int         dry_run;
    bp_uint32   file_count;
    bp_uint32   dir_count;
    int         partial;
    int         invalid_entry;
    int         subject_ok;
    int         subject_type;
    char        subject_name[BP_MAX_PATH];
    amiga_dir_restore_t *subject_directory;
    amiga_dir_restore_t *directories;
    unsigned int directory_count;
    unsigned long directory_path_bytes;
    char *path_scratch;
    char *name_scratch;
};

static amiga_dir_restore_t *amiga_queue_directory(extract_ctx_t *ctx,
                                                   const char *path,
                                                   const bp_entry_t *entry)
{
    amiga_dir_restore_t *node;
    size_t len = strlen(path);
    if (ctx->directory_count >= AMIGA_DIR_QUEUE_MAX ||
        ctx->directory_path_bytes + (unsigned long)len + 1UL >
            AMIGA_DIR_QUEUE_BYTES_MAX) return NULL;
    node = (amiga_dir_restore_t *)calloc(1, sizeof(*node));
    if (node == NULL) return NULL;
    node->path = (char *)malloc(len + 1);
    if (node->path == NULL) { free(node); return NULL; }
    memcpy(node->path, path, len + 1);
    node->path_len = (unsigned long)len;
    node->mtime = entry->mtime;
    node->mtime_valid = (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) != 0;
    node->next = ctx->directories;
    ctx->directories = node;
    ctx->directory_count++;
    ctx->directory_path_bytes += (unsigned long)len + 1UL;
    return node;
}

static int amiga_path_depth(const char *path)
{
    int depth = 0;
    while (*path != '\0') {
        if (*path++ == '/') depth++;
    }
    return depth;
}

static void amiga_finalize_directories(extract_ctx_t *ctx)
{
    while (ctx->directories != NULL) {
        amiga_dir_restore_t **best_link = &ctx->directories;
        amiga_dir_restore_t **link;
        int best_depth = -1;
        amiga_dir_restore_t *node;
        for (link = &ctx->directories; *link != NULL; link = &(*link)->next) {
            int depth = amiga_path_depth((*link)->path);
            if (depth > best_depth) {
                best_depth = depth;
                best_link = link;
            }
        }
        node = *best_link;
        *best_link = node->next;
        if (node->mtime_valid &&
            bp_restore_mtime(node->path, node->mtime) != BP_OK) {
            fprintf(stderr, "boltpack: cannot restore directory timestamp: %s\n",
                    node->path);
            ctx->partial = 1;
        }
        if (node->native_seen &&
            amiga_apply_meta(node->path, node->protection,
                             node->comment) != 0) {
            fprintf(stderr, "boltpack: cannot restore Amiga metadata: %s\n",
                    node->path);
            ctx->partial = 1;
        }
        if (ctx->directory_count != 0) ctx->directory_count--;
        if (ctx->directory_path_bytes >= node->path_len + 1UL)
            ctx->directory_path_bytes -= node->path_len + 1UL;
        free(node->path);
        free(node);
    }
}

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t  *ctx = (extract_ctx_t *)user_data;
    char           *path = ctx->path_scratch;
    char           *local_name = ctx->name_scratch;
    meta_extract_t *mex;
    amiga_staged_output_t *stage;
    amiga_dir_restore_t *directory;
    struct stat st;
    bp_uint32       meta_sz;

    if (entry->entry_type == BP_TYPE_DATA || entry->entry_type == BP_TYPE_DIR ||
        entry->entry_type == BP_TYPE_SYMLINK) {
        ctx->subject_ok = 0;
        ctx->subject_type = entry->entry_type;
        ctx->subject_directory = NULL;
        ctx->subject_name[0] = '\0';
    } else if (entry->entry_type != BP_TYPE_META &&
               entry->entry_type != BP_TYPE_RSRC) {
        ctx->subject_ok = 0;
    }

    if (!bp_name_is_safe(entry->name)) {
        fprintf(stderr, "boltpack: refusing unsafe archive path\n");
        ctx->partial = 1;
        return NULL;
    }

    /* Convert UTF-8 archive name to local codepage for filesystem access */
    if (bp_utf8_to_local(entry->name, local_name, BP_MAX_PATH,
                         AMIGA_CP) != 0) {
        fprintf(stderr, "boltpack: filename is not representable on Amiga: %s\n",
                entry->name);
        ctx->partial = 1;
        return NULL;
    }
    if (!amiga_local_name_safe(local_name)) {
        fprintf(stderr, "boltpack: filename uses AmigaDOS volume syntax: %s\n",
                entry->name);
        ctx->partial = 1;
        return NULL;
    }
    if (amiga_path_from_name(ctx->out_dir, local_name, path, BP_MAX_PATH) != 0) {
        fprintf(stderr, "boltpack: destination path is too long: %s\n", entry->name);
        ctx->partial = 1;
        return NULL;
    }

    /* --- Directory ---------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_DIR) {
        if (!bp_u64_is_zero(entry->original_size) ||
            !bp_u64_is_zero(entry->compressed_size) || entry->checksum != 0) {
            ctx->invalid_entry = 1;
            return NULL;
        }
        if (g_verbosity == VERB_DEFAULT)
            printf("   creating: %s\n", local_name);
        if (strlen(entry->name) >= sizeof(ctx->subject_name)) {
            ctx->partial = 1;
            return NULL;
        }
        strcpy(ctx->subject_name, entry->name);
        if (ctx->dry_run) {
            ctx->subject_ok = 1;
            ctx->dir_count++;
            return NULL;
        }
        if (amiga_make_directories_checked(path) != 0 ||
            lstat(path, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "boltpack: cannot safely create directory: %s\n", path);
            ctx->partial = 1;
            return NULL;
        }
        directory = amiga_queue_directory(ctx, path, entry);
        if (directory == NULL) {
            fprintf(stderr, "boltpack: directory metadata queue limit reached: %s\n",
                    path);
            ctx->partial = 1;
            return NULL;
        }
        ctx->subject_directory = directory;
        ctx->subject_ok = 1;
        ctx->dir_count++;
        return NULL;
    }

    /* --- AmigaOS metadata --------------------------------------------- */
    if (entry->entry_type == BP_TYPE_META) {
        if (entry->platform != BP_CREATOR_OS) {
            if (g_verbosity == VERB_DEFAULT)
                printf("   skipping: %s [foreign metadata]\n", local_name);
            ctx->partial = 1;
            return NULL;
        }

        meta_sz = bp_u64_to_u32(entry->original_size);
        if (entry->original_size.hi != 0 || meta_sz < 5 ||
            meta_sz > AMIGA_META_PAYLOAD_MAX) {
            ctx->invalid_entry = 1;
            return NULL;
        }

        mex = (meta_extract_t *)malloc(sizeof(meta_extract_t));
        if (!mex) {
            ctx->partial = 1;
            return NULL;
        }

        mex->wctx.buf = mex->buf;
        mex->wctx.cap = sizeof(mex->buf);
        mex->wctx.len = 0;
        mex->wctx.pos = 0;

        mex->io.read   = wbuf_read_fn;
        mex->io.write  = wbuf_write_fn;
        mex->io.seek   = wbuf_seek_fn;
        mex->io.tell   = wbuf_tell_fn;
        mex->io.handle = &mex->wctx;

        return &mex->io; /* io is first field; cast back is valid */
    }

    /* --- Resource fork / non-data types (skip on extraction) ---------- */
    if (entry->entry_type == BP_TYPE_RSRC) {
        if (g_verbosity == VERB_DEFAULT)
            printf("   skipping: %s [resource fork - not an Amiga archive]\n",
                   local_name);
        ctx->partial = 1;
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_SYMLINK) {
        if (g_verbosity == VERB_DEFAULT)
            printf("   skipping: %s [symbolic links are not supported on Amiga]\n",
                   local_name);
        ctx->partial = 1;
        return NULL;
    }
    if (entry->entry_type != BP_TYPE_DATA) {
        fprintf(stderr, "boltpack: unsupported entry type for %s\n", local_name);
        ctx->partial = 1;
        return NULL;
    }

    /* --- Regular file ------------------------------------------------- */
    if (g_verbosity == VERB_DEFAULT)
        printf(" extracting: %s\n", local_name);

    if (strlen(entry->name) >= sizeof(ctx->subject_name)) {
        ctx->partial = 1;
        return NULL;
    }
    strcpy(ctx->subject_name, entry->name);
    if (ctx->dry_run) {
        ctx->subject_ok = 1;
        ctx->file_count++;
        return bp_archive_dry_run_stream();
    }

    if (lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            fprintf(stderr,
                    "boltpack: refusing non-regular destination: %s\n", path);
            ctx->partial = 1;
            return NULL;
        }
        if (!ctx->force) {
            fprintf(stderr,
                    "boltpack: file exists (use -f to overwrite): %s\n", path);
            ctx->partial = 1;
            return NULL;
        }
    } else if (errno != ENOENT) {
        fprintf(stderr, "boltpack: cannot inspect destination: %s\n", path);
        ctx->partial = 1;
        return NULL;
    }

    if (amiga_make_parent_directories(path) != 0) {
        fprintf(stderr, "boltpack: refusing unsafe destination parent: %s\n", path);
        ctx->partial = 1;
        return NULL;
    }
    stage = (amiga_staged_output_t *)calloc(1, sizeof(*stage));
    if (stage == NULL) {
        ctx->partial = 1;
        return NULL;
    }
    stage->fd = -1;
    strcpy(stage->target, path);
    if (lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            free(stage);
            ctx->partial = 1;
            return NULL;
        }
        if (!ctx->force) {
            free(stage);
            ctx->partial = 1;
            return NULL;
        }
        stage->target_existed = 1;
        stage->target_snapshot = st;
    } else if (errno != ENOENT) {
        free(stage);
        ctx->partial = 1;
        return NULL;
    }
    if (amiga_make_sibling_template(path, "BPXXXXXX", stage->temporary,
                                    sizeof(stage->temporary)) != 0 ||
        (stage->fd = mkstemp(stage->temporary)) < 0) {
        fprintf(stderr, "boltpack: cannot create staging file for: %s\n", path);
        free(stage);
        ctx->partial = 1;
        return NULL;
    }
    stage->io.read = NULL;
    stage->io.write = amiga_staged_write;
    stage->io.seek = NULL;
    stage->io.tell = NULL;
    stage->io.handle = stage;
    return &stage->io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;

    /* --- AmigaOS metadata: apply protection bits and comment ---------- */
    if (entry->entry_type == BP_TYPE_META) {
        meta_extract_t *mex = (meta_extract_t *)out; /* io is first field */
        unsigned long prot;
        char comment[80];
        int rc = BP_OK;
        if (status == BP_OK) {
            if (bp_amiga_meta_decode(mex->wctx.buf, (size_t)mex->wctx.len,
                                     &prot, comment) != 0) {
                free(mex);
                return BP_ERR_CORRUPT;
            }
        }
        if (status == BP_OK && !ctx->dry_run && ctx->subject_ok &&
            strcmp(ctx->subject_name, entry->name) == 0) {
            if (ctx->subject_type == BP_TYPE_DIR) {
                if (ctx->subject_directory == NULL) rc = BP_ERR_PARTIAL;
                else {
                    ctx->subject_directory->native_seen = 1;
                    ctx->subject_directory->protection = prot;
                    strcpy(ctx->subject_directory->comment, comment);
                }
            } else {
                char *path = ctx->path_scratch;
                char *local_name = ctx->name_scratch;
                if (bp_utf8_to_local(entry->name, local_name, BP_MAX_PATH,
                                     AMIGA_CP) != 0 ||
                    amiga_path_from_name(ctx->out_dir, local_name,
                                         path, BP_MAX_PATH) != 0 ||
                    amiga_apply_meta(path, prot, comment) != 0) {
                    fprintf(stderr, "boltpack: cannot restore Amiga metadata: %s\n",
                            entry->name);
                    rc = BP_ERR_PARTIAL;
                }
            }
        } else if (status == BP_OK && !ctx->dry_run) {
            rc = BP_ERR_PARTIAL;
        }
        free(mex);
        if (rc == BP_ERR_PARTIAL) ctx->partial = 1;
        return rc;
    }

    /* --- Regular file: verify, close, timestamp, and publish ---------- */
    {
        amiga_staged_output_t *stage = (amiga_staged_output_t *)out->handle;
        int close_rc = close(stage->fd);
        int rc;
        stage->fd = -1;
        if (close_rc != 0) {
            amiga_discard_staged(stage);
            free(stage);
            ctx->subject_ok = 0;
            ctx->partial = 1;
            return status == BP_OK ? BP_ERR_IO : BP_OK;
        }
        if (status != BP_OK) {
            amiga_discard_staged(stage);
            free(stage);
            ctx->subject_ok = 0;
            return BP_OK;
        }
        if (amiga_set_default_file_mode(stage->temporary) != 0) {
            fprintf(stderr, "boltpack: cannot set default permissions: %s\n",
                    stage->target);
            ctx->partial = 1;
        }
        if (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) {
            if (bp_restore_mtime(stage->temporary, entry->mtime) != BP_OK) {
                fprintf(stderr, "boltpack: cannot restore timestamp: %s\n",
                        stage->target);
                ctx->partial = 1;
            }
        }
        rc = amiga_commit_staged(stage, ctx->force);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: cannot commit extracted file: %s\n",
                    stage->target);
            amiga_discard_staged(stage);
            free(stage);
            ctx->subject_ok = 0;
            ctx->partial = 1;
            return rc == BP_ERR_IO ? BP_ERR_PARTIAL : rc;
        }
        ctx->subject_ok = 1;
        ctx->file_count++;
        if (strlen(entry->name) >= sizeof(ctx->subject_name)) {
            ctx->subject_ok = 0;
            ctx->partial = 1;
        }
        free(stage);
        return BP_OK;
    }
}

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
        if (bp_utf8_to_local(entry->name, disp_name, sizeof(disp_name), AMIGA_CP) != 0) strcpy(disp_name, "<unrepresentable>");
        printf("  testing: %s", disp_name);
        if (result == BP_OK) {
            printf(" OK\n");
        } else {
            printf(" FAILED -- %s\n", bp_strerror(result));
        }
    }
    if (result == BP_OK) ctx->ok_count++;
    else                  ctx->fail_count++;
}

/* ---- usage ------------------------------------------------------------- */

static void usage(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- lightweight portable archiver (AmigaOS)\n"
        "%s\n\n"
        "Usage: boltpack <command> [options] archive.blt [files...]\n\n"
        "Commands:\n"
        "  c   Create archive from files\n"
        "  a   Add files to existing archive\n"
        "  x   Extract archive\n"
        "  l   List archive contents\n"
        "  t   Test archive integrity\n"
        "  r   Recover an interrupted archive replacement\n"
        "      Usage: boltpack r archive.blt [backup-name]\n"
        "  i   Show archive information\n"
        "  d   Delete files from archive\n",
        BP_VERSION_MAJOR, BP_VERSION_MINOR, AUTHOR_MESSAGE);
    fprintf(stderr,
        "\nOptions:\n"
        "  -b SIZE   Block size: 8k, 16k, 32k, 64k, 256k (auto-detected)\n"
        "  --interchange-profile-1  Mini-compatible limits for create/add\n"
        "  -0        Store only (no compression)\n"
        "  -1        Use BoltLite compression\n"
        "  -F        Use fluff LZ77+Huffman compression (better ratio than boltlz)\n"
        "  -q        Quiet (summary only)\n"
        "  -qq       Silent (errors only)\n"
        "  -o PATH   Output directory for extraction\n"
        "  -f        Force overwrite existing files\n"
        "  -n        Dry run\n"
        "\nFile paths: volume prefixes (e.g. DH0:) are stripped from archive\n"
        "names.  ISO-8859-1 (Latin-1) filenames are stored as UTF-8.\n"
        "Protection bits and file comments are preserved in each archive entry.\n");
}

/* ---- commands ---------------------------------------------------------- */

static int cmd_create(int argc, char **argv, bp_uint32 block_size, bp_uint8 method,
                      int interchange_profile)
{
    const char  *archive_name;
    FILE        *fp;
    bp_io_t      aio;
    bp_archive_t ar;
    amiga_archive_stage_t archive_stage;
    int          rc, i;
    clock_t      start;
    clock_t      elapsed_clocks;

    if (argc < 2) {
        fprintf(stderr, "boltpack: create requires archive name and files\n");
        return 1;
    }
    archive_name = argv[0];

    if (block_size == 0) block_size = amiga_auto_block_size();

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }

    rc = amiga_archive_stage_open(&archive_stage, archive_name, 1, &fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot stage archive: %s\n", archive_name);
        return 1;
    }
    bp_io_from_file(&aio, fp);

    rc = interchange_profile
        ? bp_archive_create_profile(&ar, &aio, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        amiga_archive_stage_discard(&archive_stage, &fp); return 1;
    }
    if (interchange_profile && method == BP_METHOD_STORED)
        ar.method = BP_METHOD_STORED;

    start = clock();

    for (i = 1; i < argc; i++) {
        const char *path    = argv[i];
        const char *arcname = clean_path(path);
        rc = add_path(&ar, path, arcname);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", path,
                    bp_strerror(rc));
            bp_archive_close(&ar); amiga_archive_stage_discard(&archive_stage, &fp); return 1;
        }
    }

    rc = bp_archive_finalize(&ar);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        bp_archive_close(&ar); amiga_archive_stage_discard(&archive_stage, &fp); return 1;
    }

    elapsed_clocks = clock() - start;

    if (g_verbosity < VERB_SILENT) {
        bp_uint64 total_orig = {0, 0};
        bp_uint64 total_comp = {0, 0};
        bp_uint32 file_count = 0, dir_count = 0, j;
        char      numbuf[32], speedbuf[64];

        for (j = 0; j < ar.entries_written; j++) {
            if (ar.entry_list[j].entry_type == BP_TYPE_DIR) dir_count++;
            else if (ar.entry_list[j].entry_type == BP_TYPE_DATA) file_count++;
            total_orig = bp_u64_add(total_orig, ar.entry_list[j].original_size);
            total_comp = bp_u64_add(total_comp, ar.entry_list[j].compressed_size);
        }
        format_size64(total_orig, numbuf, sizeof(numbuf));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)file_count, file_count == 1 ? "" : "s",
               (unsigned long)dir_count,  dir_count  == 1 ? "y" : "ies",
               numbuf);
        if (!bp_u64_is_zero(total_orig) && !bp_u64_is_zero(total_comp)) {
            char compbuf[32];
            bp_uint32 pct_int;
            if (total_orig.hi == 0 && total_orig.lo > 0UL) {
                pct_int = (bp_uint32)((unsigned long)total_comp.lo * 100UL / (unsigned long)total_orig.lo);
            } else if (total_orig.hi > 0) {
                pct_int = (total_orig.hi > 0) ? (bp_uint32)((unsigned long)total_comp.hi * 100UL / (unsigned long)(total_orig.hi + 1UL)) : 0;
            } else {
                pct_int = 0;
            }
            format_size64(total_comp, compbuf, sizeof(compbuf));
            printf(", %lu%% (%s)", (unsigned long)pct_int, compbuf);
        }
        if (elapsed_clocks > 0) {
            unsigned long secs = (unsigned long)(elapsed_clocks / (unsigned long)CLOCKS_PER_SEC);
            unsigned long cs   = (unsigned long)((elapsed_clocks % (unsigned long)CLOCKS_PER_SEC) * 100UL / (unsigned long)CLOCKS_PER_SEC);
            printf(", %lu.%02lus", secs, cs);
            if (elapsed_clocks > 0 && total_orig.hi == 0 && total_orig.lo > 0UL) {
                unsigned long bps;
                unsigned long ticks = (unsigned long)elapsed_clocks;
                unsigned long clk   = (unsigned long)CLOCKS_PER_SEC;
                bps = (total_orig.lo / ticks) * clk + (total_orig.lo % ticks) * clk / ticks;
                format_speed(bps, speedbuf, sizeof(speedbuf));
                printf(", %s", speedbuf);
            }
        }
        printf("\n");
    }

    bp_archive_close(&ar);
    rc = amiga_archive_stage_finish(&archive_stage, &fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot publish archive: %s\n", bp_strerror(rc));
        return 1;
    }
    return 0;
}

static int cmd_list(const char *archive_name)
{
    FILE      *fp;
    bp_io_t    aio;
    list_ctx_t ctx;
    int        rc;

    fp = fopen(archive_name, "rb");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    bp_io_from_file(&aio, fp);
    printf("%10s %10s  %s  %s\n", "Size", "Compressed", "Type", "Name");

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
               ctx.dir_count  == 1 ? "y" : "ies",
               origbuf);
        if (!bp_u64_is_zero(ctx.total_orig) && !bp_u64_is_zero(ctx.total_comp)) {
            bp_uint32 pct_int;
            if (ctx.total_orig.hi == 0 && ctx.total_orig.lo > 0UL) {
                pct_int = (bp_uint32)((unsigned long)ctx.total_comp.lo * 100UL / (unsigned long)ctx.total_orig.lo);
            } else if (ctx.total_orig.hi > 0) {
                pct_int = (bp_uint32)((unsigned long)ctx.total_comp.hi * 100UL / (unsigned long)(ctx.total_orig.hi + 1UL));
            } else {
                pct_int = 0;
            }
            printf(", %lu%% (%s)", (unsigned long)pct_int, compbuf);
        }
        printf("\n");
    }
    return 0;
}

static int cmd_extract(const char *archive_name, const char *out_dir,
                       int force, int dry_run)
{
    FILE          *fp;
    bp_io_t        aio;
    extract_ctx_t  ctx;
    amiga_name_index_t *name_index;
    int            rc;
    clock_t        start;
    clock_t        elapsed_clocks;

    fp = fopen(archive_name, "rb");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        fclose(fp); return 1;
    }
    bp_io_from_file(&aio, fp);
    name_index = (amiga_name_index_t *)calloc(1, sizeof(*name_index));
    if (name_index == NULL) {
        fprintf(stderr, "boltpack: not enough memory for name preflight\n");
        fclose(fp);
        return 1;
    }
    name_index->out_dir = out_dir;
    rc = bp_archive_list(&aio, amiga_preflight_name, name_index);
    if (rc != BP_OK) {
        if (name_index->error_reason != NULL)
            fprintf(stderr, "boltpack: %s\n", name_index->error_reason);
        else
            fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        amiga_name_index_clear(name_index);
        free(name_index);
        fclose(fp);
        return 1;
    }
    amiga_name_index_clear(name_index);
    free(name_index);
    if (aio.seek(aio.handle, 0, SEEK_SET) != 0) {
        fprintf(stderr, "boltpack: cannot rewind archive after name preflight\n");
        fclose(fp);
        return 1;
    }

    if (out_dir && !dry_run && amiga_make_directories_checked(out_dir) != 0) {
        fprintf(stderr, "boltpack: cannot safely create output directory: %s\n",
                out_dir);
        fclose(fp);
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir;
    ctx.force   = force;
    ctx.dry_run = dry_run;
    ctx.path_scratch = (char *)malloc(BP_MAX_PATH);
    ctx.name_scratch = (char *)malloc(BP_MAX_PATH);
    if (ctx.path_scratch == NULL || ctx.name_scratch == NULL) {
        free(ctx.path_scratch);
        free(ctx.name_scratch);
        fprintf(stderr, "boltpack: not enough memory for extraction paths\n");
        fclose(fp);
        return 1;
    }

    start = clock();
    rc = bp_archive_extract_all_ex(&aio, extract_open, extract_done,
                                   g_verbosity == VERB_DEFAULT ? progress_cb : NULL,
                                   &ctx);
    amiga_finalize_directories(&ctx);
    if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    free(ctx.path_scratch);
    free(ctx.name_scratch);

    if (ctx.invalid_entry && (rc == BP_OK || rc == BP_ERR_PARTIAL))
        rc = BP_ERR_CORRUPT;
    else if (rc == BP_OK && ctx.partial)
        rc = BP_ERR_PARTIAL;

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }

    elapsed_clocks = clock() - start;
    if (g_verbosity < VERB_SILENT) {
        printf("%lu file%s, %lu director%s",
               (unsigned long)ctx.file_count,
               ctx.file_count == 1 ? "" : "s",
               (unsigned long)ctx.dir_count,
               ctx.dir_count  == 1 ? "y" : "ies");
        if (elapsed_clocks > 0) {
            unsigned long secs = (unsigned long)(elapsed_clocks / (unsigned long)CLOCKS_PER_SEC);
            unsigned long cs   = (unsigned long)((elapsed_clocks % (unsigned long)CLOCKS_PER_SEC) * 100UL / (unsigned long)CLOCKS_PER_SEC);
            printf(", %lu.%02lus", secs, cs);
        }
        printf("\n");
    }
    return 0;
}

static int cmd_test(const char *archive_name)
{
    FILE       *fp;
    bp_io_t     aio;
    test_ctx_t  ctx;
    int         rc;

    fp = fopen(archive_name, "rb");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        fclose(fp); return 1;
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
        if (ctx.fail_count > 0)
            printf(", %lu file%s FAILED",
                   (unsigned long)ctx.fail_count,
                   ctx.fail_count == 1 ? "" : "s");
        printf("\n");
    }
    return ctx.fail_count > 0 ? 1 : 0;
}

static int amiga_ascii_n_case_equal(const char *a, const char *b, size_t count)
{
    unsigned char ca, cb;
    while (count-- != 0) {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
    }
    return 1;
}

static int amiga_ascii_case_equal(const char *a, const char *b)
{
    size_t a_len = strlen(a), b_len = strlen(b);
    return a_len == b_len && amiga_ascii_n_case_equal(a, b, a_len);
}

static int cmd_recover(const char *archive_name, const char *requested_backup)
{
    char pattern[BP_MAX_PATH];
    char parent[BP_MAX_PATH];
    char backup[BP_MAX_PATH];
    char stage[BP_MAX_PATH];
    const char *slash, *colon, *entry_prefix;
    size_t parent_len, pattern_len, prefix_len;
    DIR *dir;
    struct dirent *ent;
    struct stat st;
    int matches = 0;

    if (lstat(archive_name, &st) == 0) {
        fprintf(stderr, "boltpack: recovery refused; target already exists: %s\n",
                archive_name);
        return 1;
    }
    if (errno != ENOENT) {
        fprintf(stderr, "boltpack: cannot inspect recovery target: %s\n",
                archive_name);
        return 1;
    }
    if (amiga_make_archive_sibling_template(archive_name, "BR", pattern,
                                            sizeof(pattern)) != 0)
        return 1;
    pattern_len = strlen(pattern);
    if (pattern_len < 6U) return 1;
    slash = strrchr(pattern, '/');
    colon = strrchr(pattern, ':');
    if (colon != NULL && (slash == NULL || colon > slash))
        parent_len = (size_t)(colon - pattern) + 1U;
    else if (slash != NULL)
        parent_len = (size_t)(slash - pattern) + 1U;
    else
        parent_len = 0;
    if (parent_len == 0) {
        strcpy(parent, ".");
    } else {
        if (parent_len >= sizeof(parent)) return 1;
        memcpy(parent, pattern, parent_len);
        parent[parent_len] = '\0';
    }
    entry_prefix = pattern + parent_len;
    prefix_len = strlen(entry_prefix) - 6U;
    if (requested_backup != NULL &&
        (strchr(requested_backup, '/') != NULL ||
         strchr(requested_backup, ':') != NULL ||
         strlen(requested_backup) != prefix_len + 6U ||
         !amiga_ascii_n_case_equal(requested_backup, entry_prefix, prefix_len))) {
        fprintf(stderr, "boltpack: backup name does not match archive recovery hint: %s\n",
                requested_backup);
        return 1;
    }
    dir = opendir(parent);
    if (dir == NULL) {
        fprintf(stderr, "boltpack: cannot scan recovery directory: %s\n", parent);
        return 1;
    }
    while ((ent = readdir(dir)) != NULL) {
        size_t name_len = strlen(ent->d_name);
        if (requested_backup != NULL &&
            !amiga_ascii_case_equal(ent->d_name, requested_backup))
            continue;
        if (name_len != prefix_len + 6U ||
            strncmp(ent->d_name, entry_prefix, prefix_len) != 0)
            continue;
        if (parent_len + name_len + 1U > sizeof(backup)) continue;
        if (parent_len != 0) memcpy(backup, pattern, parent_len);
        memcpy(backup + parent_len, ent->d_name, name_len + 1U);
        if (lstat(backup, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        matches++;
    }
    if (closedir(dir) != 0) {
        fprintf(stderr, "boltpack: cannot finish scanning recovery directory: %s\n",
                parent);
        return 1;
    }
    if (matches != 1) {
        fprintf(stderr, matches == 0
                    ? "boltpack: no matching archive recovery backup for %s\n"
                    : "boltpack: multiple recovery backups match %s; inspect them manually\n",
                requested_backup != NULL ? requested_backup : archive_name);
        return 1;
    }

    strcpy(stage, backup);
    slash = strrchr(stage, '/');
    colon = strrchr(stage, ':');
    if (colon != NULL && (slash == NULL || colon > slash)) slash = colon;
    parent_len = slash != NULL ? (size_t)(slash - stage + 1U) : 0U;
    if (strncmp(stage + parent_len, "BR", 2) != 0) return 1;
    stage[parent_len + 1U] = 'P';
    if (lstat(stage, &st) != 0 || !S_ISREG(st.st_mode)) {
        fprintf(stderr, "boltpack: matching replacement stage is missing: %s\n",
                stage);
        return 1;
    }
    if (cmd_test(backup) != 0) {
        fprintf(stderr, "boltpack: recovery backup failed integrity check: %s\n",
                backup);
        return 1;
    }
    if (cmd_test(stage) != 0) {
        fprintf(stderr, "boltpack: replacement stage failed integrity check: %s\n",
                stage);
        return 1;
    }
    if (lstat(archive_name, &st) == 0 || errno != ENOENT) {
        fprintf(stderr, "boltpack: recovery target changed during verification: %s\n",
                archive_name);
        return 1;
    }
    if (!Rename((STRPTR)backup, (STRPTR)archive_name)) {
        fprintf(stderr, "boltpack: cannot restore recovery backup: %s\n", backup);
        return 1;
    }
    printf("Original archive restored to %s; verified replacement remains at %s\n",
           archive_name, stage);
    return 0;
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
    case BP_OS_BEOS:      return "BeOS";
    case BP_OS_OPENVMS:   return "OpenVMS";
    case BP_OS_ZOS:       return "z/OS";
    case BP_OS_WASM:      return "WebAssembly";
    case BP_OS_NEXTSTEP:  return "NeXTSTEP";
    case BP_OS_QNX:       return "QNX";
    case BP_OS_PLAN9:     return "Plan 9";
    case BP_OS_TRU64:     return "Tru64 UNIX";
    case BP_OS_SCO:       return "SCO UNIX";
    case BP_OS_CPM86:     return "CP/M-86";
    case BP_OS_QL:        return "Sinclair QL";
    case BP_OS_WIN16:     return "Windows 3.x";
    case BP_OS_HURD:      return "GNU Hurd";
    case BP_OS_X68K:      return "Sharp X68000";
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
    case BP_CPU_SPARC:    return "SPARC";
    case BP_CPU_ALPHA:    return "Alpha";
    case BP_CPU_PARISC:   return "PA-RISC";
    case BP_CPU_RISCV32:  return "RISC-V 32";
    case BP_CPU_RISCV64:  return "RISC-V 64";
    case BP_CPU_SH:       return "SuperH";
    case BP_CPU_6502:     return "6502/65C816";
    case BP_CPU_Z80:      return "Z80";
    case BP_CPU_8086:     return "8086";
    case BP_CPU_E2K:      return "Elbrus (e2k)";
    case BP_CPU_ZARCH:    return "z/Architecture";
    case BP_CPU_SPARC64:  return "SPARC64";
    case BP_CPU_IA64:     return "Itanium";
    default:              return "Unknown";
    }
}

static const char *format_methods(const bp_uint32 *counts)
{
    static char buf[128];
    static const char * const names[4] = {"Stored", "BoltLite", "boltlz", "Fluff"};
    int pos = 0;
    int first = 1;
    int i;
    buf[0] = '\0';
    for (i = 0; i < 4; i++) {
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
    FILE              *fp;
    bp_io_t           aio;
    bp_archive_info_t info;
    int               rc;

    fp = fopen(archive_name, "rb");
    if (!fp) {
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
        bp_uint64 arc_size   = info.archive_size;
        char      origbuf[32], compbuf[32], arcbuf[32];

        printf("Archive:        %s\n", archive_name);
        printf("Format version: %u\n", (unsigned)info.version);
        printf("Created by:     BoltPack %u.%u\n",
               (unsigned)info.creator_ver_major,(unsigned)info.creator_ver_minor);
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
            bp_uint32 pct_int;
            if (total_orig.hi == 0 && total_orig.lo > 0UL) {
                pct_int = (bp_uint32)((unsigned long)total_comp.lo * 100UL / (unsigned long)total_orig.lo);
            } else if (total_orig.hi > 0) {
                pct_int = (bp_uint32)((unsigned long)total_comp.hi * 100UL / (unsigned long)(total_orig.hi + 1UL));
            } else {
                pct_int = 0;
            }
            printf(" (%lu%%)", (unsigned long)pct_int);
        }
        printf("\n");
    }
    return 0;
}

static int cmd_delete(int argc, char **argv)
{
    const char  *archive_name;
    const char **del_names;
    int          del_count;
    FILE        *src_fp, *dst_fp;
    bp_io_t      src_io, dst_io;
    amiga_archive_stage_t archive_stage;
    bp_uint32    deleted = 0;
    int          rc;

    if (argc < 2) {
        fprintf(stderr, "boltpack: delete requires archive name and file(s)\n");
        return 1;
    }
    archive_name = argv[0];
    del_names    = (const char **)&argv[1];
    del_count    = argc - 1;

    rc = amiga_archive_stage_open(&archive_stage, archive_name, 0, &dst_fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot stage archive: %s\n", archive_name);
        return 1;
    }
    src_fp = fopen(archive_name, "rb");
    if (!src_fp) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        amiga_archive_stage_discard(&archive_stage, &dst_fp); return 1;
    }

    bp_io_from_file(&src_io, src_fp);
    bp_io_from_file(&dst_io, dst_fp);

    rc = bp_archive_delete(&src_io, &dst_io, del_names,
                           (bp_uint32)del_count, &deleted);
    if (fclose(src_fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: delete failed: %s\n", bp_strerror(rc));
        amiga_archive_stage_discard(&archive_stage, &dst_fp); return 1;
    }
    if (deleted == 0) {
        fprintf(stderr, "boltpack: no matching entries found\n");
        amiga_archive_stage_discard(&archive_stage, &dst_fp); return 1;
    }
    rc = amiga_archive_stage_finish(&archive_stage, &dst_fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot publish archive: %s\n", bp_strerror(rc));
        return 1;
    }
    if (g_verbosity < VERB_SILENT)
        printf("%lu entr%s deleted\n",
               (unsigned long)deleted, deleted == 1 ? "y" : "ies");
    return 0;
}

static int cmd_add(int argc, char **argv, bp_uint32 block_size,
                   bp_uint8 method, int interchange_profile)
{
    const char  *archive_name;
    FILE        *fp;
    FILE        *source;
    bp_io_t      io;
    bp_archive_t ar;
    amiga_archive_stage_t archive_stage;
    char         copy_buffer[512];
    size_t       copied;
    int          copy_failed = 0;
    int          rc, i;

    if (argc < 2) {
        fprintf(stderr, "boltpack: add requires archive name and files\n");
        return 1;
    }
    archive_name = argv[0];

    if (block_size == 0) block_size = amiga_auto_block_size();

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }

    rc = amiga_archive_stage_open(&archive_stage, archive_name, 0, &fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot stage archive: %s\n", archive_name);
        return 1;
    }
    source = fopen(archive_name, "rb");
    if (!source) { amiga_archive_stage_discard(&archive_stage, &fp); return 1; }
    while ((copied = fread(copy_buffer, 1, sizeof(copy_buffer), source)) != 0) {
        if (fwrite(copy_buffer, 1, copied, fp) != copied) {
            copy_failed = 1;
            break;
        }
    }
    rc = (copy_failed || ferror(source) || ferror(fp)) ? BP_ERR_IO : BP_OK;
    if (fclose(source) != 0) rc = BP_ERR_IO;
    if (rc == BP_OK && (fflush(fp) != 0 || fseek(fp, 0, SEEK_SET) != 0)) rc = BP_ERR_IO;
    if (rc != BP_OK) { amiga_archive_stage_discard(&archive_stage, &fp); return 1; }
    bp_io_from_file(&io, fp);

    rc = bp_archive_open_append(&ar, &io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        amiga_archive_stage_discard(&archive_stage, &fp); return 1;
    }
    if (interchange_profile) {
        rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: archive does not meet Interchange Profile 1: %s\n",
                    bp_strerror(rc));
            bp_archive_close(&ar);
            amiga_archive_stage_discard(&archive_stage, &fp);
            return 1;
        }
    }

    for (i = 1; i < argc; i++) {
        const char *path    = argv[i];
        const char *arcname = clean_path(path);
        rc = add_path(&ar, path, arcname);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", path,
                    bp_strerror(rc));
            bp_archive_close(&ar); amiga_archive_stage_discard(&archive_stage, &fp); return 1;
        }
    }

    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        amiga_archive_stage_discard(&archive_stage, &fp);
        return 1;
    }
    rc = amiga_archive_stage_finish(&archive_stage, &fp);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot publish archive: %s\n", bp_strerror(rc));
        return 1;
    }
    if (g_verbosity < VERB_SILENT)
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    return 0;
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char       command;
    bp_uint32  block_size = 0; /* 0 = auto-detect via AvailMem */
    bp_uint8   method    = BP_METHOD_BOLTLZ;
    const char *out_dir  = NULL;
    int        force     = 0;
    int        dry_run   = 0;
    int        interchange_profile = 0;
    int        codec_given = 0;
    int        i;

#if defined(__GNUC__) && defined(__libnix__)
    if (amiga_expand_stack() != 0) {
        fprintf(stderr, "boltpack: cannot register Amiga stack cleanup\n");
        return 1;
    }
#endif

    if (argc < 3) { usage(); return 1; }

    if (strlen(argv[1]) != 1) {
        fprintf(stderr, "boltpack: unknown command: %s\n", argv[1]);
        usage(); return 1;
    }
    command = argv[1][0];

    i = 2;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-b") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: -b requires an argument\n");
                return 1;
            }
            block_size = parse_block_size(argv[i + 1]);
            if (block_size == 0) {
                fprintf(stderr, "boltpack: invalid block size: %s\n",
                        argv[i + 1]);
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--interchange-profile-1") == 0) {
            interchange_profile = 1; i++;
        } else if (strcmp(argv[i], "-0") == 0) {
            method = BP_METHOD_STORED; codec_given = 1; i++;
        } else if (strcmp(argv[i], "-1") == 0) {
            method = BP_METHOD_BOLTLITE; codec_given = 1; i++;
        } else if (strcmp(argv[i], "-F") == 0) {
            method = BP_METHOD_FLUFF; codec_given = 1; i++;
        } else if (strcmp(argv[i], "-qq") == 0) {
            g_verbosity = VERB_SILENT; i++;
        } else if (strcmp(argv[i], "-q") == 0) {
            g_verbosity = VERB_QUIET; i++;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "boltpack: -o requires an argument\n");
                return 1;
            }
            out_dir = argv[i + 1]; i += 2;
        } else if (strcmp(argv[i], "-f") == 0) {
            force = 1; i++;
        } else if (strcmp(argv[i], "-n") == 0) {
            dry_run = 1; i++;
#ifdef BP_TEST_FAIL_PUBLISH
        } else if (strcmp(argv[i], "--test-fail-publish") == 0) {
            g_test_fail_publish = 1; i++;
#endif
        } else {
            fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]);
            usage(); return 1;
        }
    }

    if (interchange_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_BOLTLITE &&
             method != BP_METHOD_STORED)) {
            fprintf(stderr, "boltpack: Interchange Profile 1 requires create/add, BoltLite or Stored, and 16 KiB blocks\n");
            return 1;
        }
        block_size = BP_BLOCK_16K;
        if (method != BP_METHOD_STORED) method = BP_METHOD_BOLTLITE;
    }
    if (method == BP_METHOD_FLUFF && block_size == 0) {
        block_size = BP_BLOCK_256K;
    }

    switch (command) {
    case 'c': return cmd_create(argc - i, argv + i, block_size, method, interchange_profile);
    case 'a': return cmd_add(argc - i, argv + i, block_size, method, interchange_profile);
    case 'l':
        if (i >= argc) {
            fprintf(stderr, "boltpack: list requires archive name\n"); return 1;
        }
        return cmd_list(argv[i]);
    case 'x':
        if (i >= argc) {
            fprintf(stderr, "boltpack: extract requires archive name\n"); return 1;
        }
        return cmd_extract(argv[i], out_dir, force, dry_run);
    case 't':
        if (i >= argc) {
            fprintf(stderr, "boltpack: test requires archive name\n"); return 1;
        }
        return cmd_test(argv[i]);
    case 'r':
        if (i >= argc) {
            fprintf(stderr, "boltpack: recover requires archive name\n"); return 1;
        }
        if (argc - i > 2) {
            fprintf(stderr, "boltpack: recover accepts archive name and optional backup name\n");
            return 1;
        }
        return cmd_recover(argv[i], argc - i == 2 ? argv[i + 1] : NULL);
    case 'i':
        if (i >= argc) {
            fprintf(stderr, "boltpack: info requires archive name\n"); return 1;
        }
        return cmd_info(argv[i]);
    case 'd': return cmd_delete(argc - i, argv + i);
    default:
        fprintf(stderr, "boltpack: unknown command: %c\n", command);
        usage(); return 1;
    }
}
