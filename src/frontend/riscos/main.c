/*
 * main.c -- RISC OS CLI frontend for BoltPack
 *
 * Target:   RISC OS 3 and later (ARM; compiled under GCCSDK 4.x)
 * Compiler: arm-unknown-riscos-gcc (GCCSDK cross-compiler from Linux)
 *           arm-unknown-riscos-cc  (Norcroft native compiler on RISC OS)
 *
 * THIS FILE DOES NOT USE POSIX.
 * All I/O is performed via RISC OS SWI calls using the GCCSDK <kernel.h> and
 * <swis.h> headers.  No <stdio.h> FILE* is used directly for archive data;
 * however printf/fprintf are kept for status output because they are provided
 * by the GCCSDK C runtime.
 *
 * RISC OS I/O SWIs used:
 *   OS_Find  (0x0D)  -- open / close files
 *   OS_GBPB  (0x0C)  -- read / write / enumerate directory
 *   OS_Args  (0x09)  -- seek / tell / set EOF
 *   OS_File  (0x08)  -- stat / write catalogue info / delete / rename
 *   OS_FSControl (0x19) reason 26 -- create directory
 *
 * Metadata payload format (from format.md, platform = BP_OS_RISCOS = 0x12):
 *   Offset  Size  Field       Description
 *   0       4     load_addr   Load address / filetype encoding (uint32 LE)
 *   4       4     exec_addr   Exec address / timestamp low word (uint32 LE)
 *   8       4     attributes  RISC OS file attributes (RWLR/rw, uint32 LE)
 *   Total: 12 bytes (fixed)
 *
 * RISC OS filetype:
 *   When load_addr bits 20–31 == 0xFFF, the file has a datestamp:
 *     bits 19-8 of load_addr = 12-bit filetype
 *     exec_addr  = low 32 bits of 5-byte centisecond timestamp
 *     bits  7-0  of load_addr = high 8 bits of timestamp
 *
 * RISC OS timestamp epoch: 1900-01-01 00:00:00 in centiseconds.
 * Unix epoch: 1970-01-01 00:00:00 in seconds.
 * Difference: 2208988800 seconds = 220898880000 centiseconds.
 *
 * Filename encoding: Latin-1 (ISO 8859-1) — BP_CP_8859_1 = 5.
 * RISC OS uses '.' as the directory separator.
 * This frontend uses '.' internally and maps to '/' for archive names.
 *
 * No symlinks: RISC OS has no symlinks; BP_TYPE_SYMLINK entries are skipped
 * silently on extraction with an informational message.
 *
 * Cross-compilation from Linux:
 *   arm-unknown-riscos-gcc -ansi -pedantic -Wall -Wextra -O2 \
 *       -I src/core                       \
 *       src/core/compress.c src/core/archiver.c              \
 *       src/core/bp_codepage.c                               \
 *       src/frontend/riscos/main.c                           \
 *       src/core/boltlz.c                           \
 *       -o boltpack,ff8
 *
 * The output filename uses RISC OS filetype-in-name convention.
 * Filetype 0xFF8 = Absolute (executable).
 *
 * ANSI C (C89/C90) throughout.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_RISCOS
#define BP_CREATOR_CPU BP_CPU_ARM32

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_codepage.h"

#include <kernel.h>
#include <swis.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Fallback for systems without CLOCKS_PER_SEC (e.g. some RISC OS builds) */
#ifndef CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 100  /* RISC OS timer typically 100 Hz */
#endif

/* ---- RISC OS metadata size -------------------------------------------- */

#define RISCOS_META_SIZE   12   /* load(4) + exec(4) + attr(4) */

/* ---- RISC OS SWI reason codes ----------------------------------------- */

/* OS_Find reasons */
#define OS_Find_OpenIn    0x40  /* open existing file for read */
#define OS_Find_OpenOut   0x80  /* create/truncate file for write */
#define OS_Find_OpenUp    0xC0  /* open existing file for read/write */
#define OS_Find_Close     0x00  /* close file handle */

/* OS_GBPB reasons */
#define OS_GBPB_WriteAt   1     /* write to explicit position */
#define OS_GBPB_Write     2     /* write sequential */
#define OS_GBPB_ReadAt    3     /* read from explicit position */
#define OS_GBPB_Read      4     /* read sequential */
#define OS_GBPB_Enum      12    /* enumerate directory entries */

/* OS_Args reasons */
#define OS_Args_ReadPos   0     /* read current file pointer */
#define OS_Args_SetPos    1     /* set file pointer */
#define OS_Args_ReadExt   2     /* read extent (file size) */
#define OS_Args_SetExt    3     /* set extent */
#define OS_Args_Flush     255   /* flush and return stream type */

/* OS_File reasons */
#define OS_File_Load      255   /* load file entirely */
#define OS_File_Save      0     /* save memory block to file */
#define OS_File_WriteCat  1     /* write catalogue info (load/exec/attr) */
#define OS_File_ReadCat   5     /* read catalogue info */
#define OS_File_Delete    6     /* delete file or directory */
#define OS_File_CreateDir 8     /* create directory */
#define OS_File_ReadCatNoPath 17 /* read catalogue info, no path */

/* OS_FSControl reasons */
#define OS_FSControl_CreateDir 26

/* RISC OS object types returned by OS_File */
#define RISCOS_OBJ_NONE   0
#define RISCOS_OBJ_FILE   1
#define RISCOS_OBJ_DIR    2

/* I/O chunk size for SWI calls: 64 KB */
#define RISCOS_IO_CHUNK   65536U

/* RISC OS timestamp epoch offset from Unix epoch, in centiseconds */
#define RISCOS_EPOCH_CS   220898880000UL

/* verbosity */
#define VERB_DEFAULT 0
#define VERB_QUIET   1
#define VERB_SILENT  2

static int g_verbosity = VERB_DEFAULT;

/* ========================================================================
 * RISC OS bp_io_t implementation
 * Each handle is an integer RISC OS file handle (1..255).
 * We store it as a pointer-sized integer cast.
 * ======================================================================== */

/*
 * riscos_fhandle: cast a RISC OS integer file handle to/from void*.
 * Using an intermediate union avoids strict-aliasing UB on C89.
 */
typedef union { void *ptr; int fh; } riscos_handle_t;

static int riscos_handle_to_fh(void *h)
{
    riscos_handle_t u;
    u.ptr = h;
    return u.fh;
}

static void *riscos_fh_to_handle(int fh)
{
    riscos_handle_t u;
    u.fh = fh;
    return u.ptr;
}

/* -- bp_io_t read -------------------------------------------------------- */

static long riscos_io_read(void *handle, void *buf, bp_uint32 count)
{
    _kernel_swi_regs r;
    _kernel_oserror *err;
    int fh = riscos_handle_to_fh(handle);

    r.r[0] = OS_GBPB_Read;
    r.r[1] = fh;
    r.r[2] = (int)buf;
    r.r[3] = (int)count;
    err = _kernel_swi(OS_GBPB, &r, &r);
    if (err != NULL) return -1;
    /* r.r[3] holds the number of bytes NOT transferred */
    return (long)((int)count - r.r[3]);
}

/* -- bp_io_t write ------------------------------------------------------- */

static long riscos_io_write(void *handle, const void *buf, bp_uint32 count)
{
    _kernel_swi_regs r;
    _kernel_oserror *err;
    int fh = riscos_handle_to_fh(handle);

    r.r[0] = OS_GBPB_Write;
    r.r[1] = fh;
    r.r[2] = (int)buf;
    r.r[3] = (int)count;
    err = _kernel_swi(OS_GBPB, &r, &r);
    if (err != NULL) return -1;
    return (long)((int)count - r.r[3]);
}

/* -- bp_io_t seek -------------------------------------------------------- */

static int riscos_io_seek(void *handle, bp_off_t offset, int origin)
{
    _kernel_swi_regs r;
    _kernel_oserror *err;
    int fh  = riscos_handle_to_fh(handle);
    int pos;

    if (origin == SEEK_SET) {
        pos = (int)offset;
    } else if (origin == SEEK_CUR) {
        r.r[0] = OS_Args_ReadPos;
        r.r[1] = fh;
        err = _kernel_swi(OS_Args, &r, &r);
        if (err != NULL) return -1;
        pos = r.r[2] + (int)offset;
    } else if (origin == SEEK_END) {
        r.r[0] = OS_Args_ReadExt;
        r.r[1] = fh;
        err = _kernel_swi(OS_Args, &r, &r);
        if (err != NULL) return -1;
        pos = r.r[2] + (int)offset;
    } else {
        return -1;
    }

    r.r[0] = OS_Args_SetPos;
    r.r[1] = fh;
    r.r[2] = pos;
    err = _kernel_swi(OS_Args, &r, &r);
    return (err != NULL) ? -1 : 0;
}

/* -- bp_io_t tell -------------------------------------------------------- */

static bp_off_t riscos_io_tell(void *handle)
{
    _kernel_swi_regs r;
    _kernel_oserror *err;
    int fh = riscos_handle_to_fh(handle);

    r.r[0] = OS_Args_ReadPos;
    r.r[1] = fh;
    err = _kernel_swi(OS_Args, &r, &r);
    if (err != NULL) return (bp_off_t)-1;
    return (bp_off_t)r.r[2];
}

/* -- open / close helpers ------------------------------------------------ */

static int riscos_open(const char *path, int reason)
{
    _kernel_swi_regs r;
    _kernel_oserror *err;

    r.r[0] = reason;
    r.r[1] = (int)path;
    err = _kernel_swi(OS_Find, &r, &r);
    if (err != NULL) return 0;
    return r.r[0];
}

static void riscos_close(int fh)
{
    _kernel_swi_regs r;
    r.r[0] = OS_Find_Close;
    r.r[1] = fh;
    _kernel_swi(OS_Find, &r, &r);
}

static void bp_io_from_riscos(bp_io_t *io, int fh)
{
    io->read   = riscos_io_read;
    io->write  = riscos_io_write;
    io->seek   = riscos_io_seek;
    io->tell   = riscos_io_tell;
    io->handle = riscos_fh_to_handle(fh);
}

/* ---- read-only memory buffer I/O (for metadata payloads) -------------- */

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
    return -1;
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

/* ---- write-only memory buffer I/O (for receiving metadata on extract) - */

typedef struct {
    unsigned char *buf;
    bp_uint32      cap;
    bp_uint32      len;
    bp_uint32      pos;
} wbuf_ctx_t;

static long wbuf_read_fn(void *handle, void *buf, bp_uint32 count)
{
    (void)handle; (void)buf; (void)count;
    return -1;
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
 * meta_extract_t: captures metadata payload bytes during extraction.
 * io MUST be first so (meta_extract_t *)out_io is a valid cast.
 */
typedef struct {
    bp_io_t       io;                      /* MUST be first */
    wbuf_ctx_t    wctx;
    unsigned char buf[RISCOS_META_SIZE + 4];
} meta_extract_t;

/* ---- error strings ----------------------------------------------------- */

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
    default:                       return "unknown error";
    }
}

/* ---- size formatting -------------------------------------------------- */

static void format_size(bp_uint32 bytes, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes < 1024UL)
        sprintf(buf, "%lu bytes", (unsigned long)bytes);
    else if (bytes < 1048576UL)
        sprintf(buf, "%.1f KB", (double)bytes / 1024.0);
    else if (bytes < 1073741824UL)
        sprintf(buf, "%.1f MB", (double)bytes / 1048576.0);
    else
        sprintf(buf, "%.2f GB", (double)bytes / 1073741824.0);
}

static void format_size64(bp_uint64 bytes, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes.hi == 0) {
        format_size(bytes.lo, buf, bufsize);
    } else {
        double d = (double)bytes.lo + (double)bytes.hi * 4294967296.0;
        if (d < 1048576.0)         sprintf(buf, "%.1f KB", d / 1024.0);
        else if (d < 1073741824.0) sprintf(buf, "%.1f MB", d / 1048576.0);
        else                       sprintf(buf, "%.2f GB", d / 1073741824.0);
    }
}

static void format_speed(double bytes_per_sec, char *buf, size_t bufsize)
{
    (void)bufsize;
    if (bytes_per_sec < 1024.0)
        sprintf(buf, "%.0f bytes/s", bytes_per_sec);
    else if (bytes_per_sec < 1048576.0)
        sprintf(buf, "%.2f KB/s", bytes_per_sec / 1024.0);
    else
        sprintf(buf, "%.2f MB/s", bytes_per_sec / 1048576.0);
}

static void format_number(bp_uint32 n, char *buf, size_t bufsize)
{
    char tmp[32];
    int len, i, j, commas;
    sprintf(tmp, "%lu", (unsigned long)n);
    len = (int)strlen(tmp);
    commas = (len - 1) / 3;
    if ((size_t)(len + commas + 1) > bufsize) { strcpy(buf, tmp); return; }
    j = len + commas;
    buf[j] = '\0';
    for (i = len - 1; i >= 0; i--) {
        buf[--j] = tmp[i];
        if (i > 0 && (len - i) % 3 == 0) buf[--j] = ',';
    }
}

/* ---- progress callback ------------------------------------------------- */

static int progress_cb(const bp_progress_t *p, void *user_data)
{
    (void)p; (void)user_data;
    return 0;
}

/* ====================================================================
 * Path utilities
 *
 * RISC OS uses '.' as the directory separator.
 * Archive names use '/'.  We convert:
 *   RISC OS path  →  archive name:  replace '.' with '/'
 *   archive name  →  RISC OS path:  replace '/' with '.'
 * ==================================================================== */

/*
 * arc_to_riscos: convert '/' → '.' in archive name.
 * (Operates in-place on a copy; caller must supply dest buffer.)
 */
static void arc_to_riscos(const char *arc, char *riscos, size_t bufsize)
{
    size_t i;
    for (i = 0; i < bufsize - 1 && arc[i] != '\0'; i++)
        riscos[i] = (arc[i] == '/') ? '.' : arc[i];
    riscos[i] = '\0';
}

/*
 * riscos_to_arc: convert '.' → '/' in RISC OS name.
 */
static void riscos_to_arc(const char *riscos, char *arc, size_t bufsize)
{
    size_t i;
    for (i = 0; i < bufsize - 1 && riscos[i] != '\0'; i++)
        arc[i] = (riscos[i] == '.') ? '/' : riscos[i];
    arc[i] = '\0';
}

/*
 * riscos_join: join a RISC OS directory path with a leafname using '.'.
 */
static void riscos_join(char *buf, size_t bufsize,
                        const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    if (dlen > 0 && dir[dlen - 1] == '.')
        snprintf(buf, bufsize, "%s%s", dir, name);
    else
        snprintf(buf, bufsize, "%s.%s", dir, name);
}

/*
 * riscos_create_dir: create a directory (and parents if needed).
 * Uses OS_File reason 8.
 */
static void riscos_create_dir(const char *path)
{
    _kernel_swi_regs r;
    char tmp[1024];
    char *p;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Walk path, creating each component */
    for (p = tmp + 1; *p; p++) {
        if (*p == '.') {
            *p = '\0';
            r.r[0] = OS_File_CreateDir;
            r.r[1] = (int)tmp;
            r.r[4] = 10; /* default number of entries */
            _kernel_swi(OS_File, &r, &r);
            *p = '.';
        }
    }
    r.r[0] = OS_File_CreateDir;
    r.r[1] = (int)path;
    r.r[4] = 10;
    _kernel_swi(OS_File, &r, &r);
}

/*
 * riscos_create_parent: create the parent directory for a file path.
 */
static void riscos_create_parent(const char *path)
{
    char tmp[1024];
    char *dot;

    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    dot = strrchr(tmp, '.');
    if (dot != NULL && dot != tmp) {
        *dot = '\0';
        riscos_create_dir(tmp);
    }
}

/* ====================================================================
 * RISC OS stat: read catalogue info for a file/directory.
 * Returns RISCOS_OBJ_NONE (0) if not found,
 *         RISCOS_OBJ_FILE (1), or RISCOS_OBJ_DIR (2).
 * ==================================================================== */

typedef struct {
    int obj_type;
    unsigned int load_addr;
    unsigned int exec_addr;
    unsigned int length;
    unsigned int attributes;
} riscos_stat_t;

static int riscos_stat(const char *path, riscos_stat_t *st)
{
    _kernel_swi_regs r;
    _kernel_oserror *err;

    r.r[0] = OS_File_ReadCat;
    r.r[1] = (int)path;
    err = _kernel_swi(OS_File, &r, &r);
    if (err != NULL) return RISCOS_OBJ_NONE;

    st->obj_type   = r.r[0];
    st->load_addr  = (unsigned int)r.r[2];
    st->exec_addr  = (unsigned int)r.r[3];
    st->length     = (unsigned int)r.r[4];
    st->attributes = (unsigned int)r.r[5];
    return r.r[0];
}

/* ====================================================================
 * RISC OS timestamp helpers
 * ==================================================================== */

/*
 * riscos_get_mtime: extract mtime as Unix seconds from a catalogued file.
 * If the file has a datestamp (load_addr bits 20-31 == 0xFFF), decode it.
 * Returns 0 if the file has no datestamp.
 */
static bp_uint32 riscos_get_mtime(unsigned int load_addr, unsigned int exec_addr)
{
    if ((load_addr & 0xFFF00000U) == 0xFFF00000U) {
        /* 5-byte centisecond timestamp:
         *   hi byte = low 8 bits of load_addr
         *   lo 4 bytes = exec_addr
         * Combine into 64-bit value in centiseconds */
        unsigned long long cs_hi = (unsigned long long)(load_addr & 0xFF);
        unsigned long long cs_lo = (unsigned long long)exec_addr;
        unsigned long long cs = (cs_hi << 32) | cs_lo;
        /* Subtract RISC OS epoch to get centiseconds since 1970 */
        if (cs < RISCOS_EPOCH_CS) return 0;
        cs -= RISCOS_EPOCH_CS;
        /* Convert centiseconds to seconds */
        return (bp_uint32)(cs / 100ULL);
    }
    return 0;
}

/*
 * riscos_mtime_from_unix: convert Unix seconds to a 5-byte RISC OS timestamp.
 * The 5-byte value (40 bits) covers centiseconds since 1900-01-01.
 */
static void riscos_mtime_from_unix(bp_uint32 unix_secs,
                                   unsigned int *load_out,
                                   unsigned int *exec_out,
                                   unsigned int filetype)
{
    unsigned long long cs;
    cs = (unsigned long long)unix_secs * 100ULL + RISCOS_EPOCH_CS;
    *exec_out = (unsigned int)(cs & 0xFFFFFFFFULL);
    /* Encode load_addr: 0xFFF + filetype + high byte of timestamp */
    *load_out = 0xFFF00000U
              | ((filetype & 0xFFFU) << 8)
              | (unsigned int)((cs >> 32) & 0xFFU);
}

/* ====================================================================
 * RISC OS metadata encode / decode / apply
 * ==================================================================== */

/*
 * riscos_encode_meta: build the 12-byte BP_TYPE_META payload.
 */
static void riscos_encode_meta(unsigned char *buf,
                               unsigned int load_addr,
                               unsigned int exec_addr,
                               unsigned int attributes)
{
    buf[0]  = (unsigned char)(load_addr & 0xFF);
    buf[1]  = (unsigned char)((load_addr >>  8) & 0xFF);
    buf[2]  = (unsigned char)((load_addr >> 16) & 0xFF);
    buf[3]  = (unsigned char)((load_addr >> 24) & 0xFF);
    buf[4]  = (unsigned char)(exec_addr & 0xFF);
    buf[5]  = (unsigned char)((exec_addr >>  8) & 0xFF);
    buf[6]  = (unsigned char)((exec_addr >> 16) & 0xFF);
    buf[7]  = (unsigned char)((exec_addr >> 24) & 0xFF);
    buf[8]  = (unsigned char)(attributes & 0xFF);
    buf[9]  = (unsigned char)((attributes >>  8) & 0xFF);
    buf[10] = (unsigned char)((attributes >> 16) & 0xFF);
    buf[11] = (unsigned char)((attributes >> 24) & 0xFF);
}

/*
 * riscos_decode_meta: parse the 12-byte BP_TYPE_META payload.
 * Returns 0 on success, -1 on truncation.
 */
static int riscos_decode_meta(const unsigned char *buf, int len,
                              unsigned int *load_out,
                              unsigned int *exec_out,
                              unsigned int *attr_out)
{
    if (len < RISCOS_META_SIZE) return -1;
    *load_out = (unsigned int)buf[0]
              | ((unsigned int)buf[1] <<  8)
              | ((unsigned int)buf[2] << 16)
              | ((unsigned int)buf[3] << 24);
    *exec_out = (unsigned int)buf[4]
              | ((unsigned int)buf[5] <<  8)
              | ((unsigned int)buf[6] << 16)
              | ((unsigned int)buf[7] << 24);
    *attr_out = (unsigned int)buf[8]
              | ((unsigned int)buf[9] <<  8)
              | ((unsigned int)buf[10] << 16)
              | ((unsigned int)buf[11] << 24);
    return 0;
}

/*
 * riscos_apply_meta: restore load_addr, exec_addr, attributes to a file
 * via OS_File reason 1 (write catalogue info).
 * R0=1, R1=name, R2=load, R3=exec, R5=attributes
 */
static void riscos_apply_meta(const char *path,
                              unsigned int load_addr,
                              unsigned int exec_addr,
                              unsigned int attributes)
{
    _kernel_swi_regs r;
    r.r[0] = OS_File_WriteCat;
    r.r[1] = (int)path;
    r.r[2] = (int)load_addr;
    r.r[3] = (int)exec_addr;
    r.r[4] = 0;             /* don't change length */
    r.r[5] = (int)attributes;
    _kernel_swi(OS_File, &r, &r);
}

/* ====================================================================
 * Directory enumeration
 * We get entries via OS_GBPB 12 which returns length-prefixed strings
 * in a 256-byte buffer; we call it repeatedly until done.
 * ==================================================================== */

#define ENUM_BUF_SIZE 256

/*
 * riscos_enum_dir: iterate all entries in a directory, calling cb for each.
 *   cb(leaf_name, obj_type, parent_riscos_path, parent_arc_prefix, user_data)
 *   returns BP_OK to continue, non-zero to stop.
 * We define this signature via a function pointer typedef.
 */
typedef int (*riscos_enum_cb)(const char *leaf, int obj_type,
                              const char *ros_parent, const char *arc_parent,
                              void *user_data);

static int riscos_enum_dir(const char *ros_path, const char *arc_prefix,
                           riscos_enum_cb cb, void *user_data)
{
    _kernel_swi_regs r;
    _kernel_oserror *err;
    char enum_buf[ENUM_BUF_SIZE];
    int offset = 0;
    int count, i, rc;

    while (offset != -1) {
        r.r[0] = OS_GBPB_Enum;
        r.r[1] = (int)ros_path;
        r.r[2] = (int)enum_buf;
        r.r[3] = 64;           /* max entries per call */
        r.r[4] = offset;
        r.r[5] = sizeof(enum_buf);
        r.r[6] = (int)"*";     /* wildcard: all entries */
        err = _kernel_swi(OS_GBPB, &r, &r);
        if (err != NULL) break;

        count  = r.r[3];
        offset = r.r[4];       /* -1 when done */

        /* Parse the returned block.
         * OS_GBPB 12 (RISC OS 3.5+) returns 20-byte fixed records:
         *   Offset  Size  Field
         *   0       4     load_addr
         *   4       4     exec_addr
         *   8       4     length
         *   12      4     attributes
         *   16      4     obj_type
         *   20+     N+1   name (null-terminated)
         * For older RISC OS the records may differ; we use the
         * obj_type-aware version.
         */
        {
            const char *p = enum_buf;
            for (i = 0; i < count; i++) {
                const unsigned int *rec = (const unsigned int *)p;
                int obj_type_entry;
                const char *leaf;
                char ros_child[512], arc_child[512];

                obj_type_entry = (int)rec[4];
                leaf = (const char *)(rec + 5);  /* 20 bytes in */

                riscos_join(ros_child, sizeof(ros_child), ros_path, leaf);
                {
                    /* build arc_child = arc_prefix + '/' + leaf_as_arc */
                    char leaf_arc[256];
                    riscos_to_arc(leaf, leaf_arc, sizeof(leaf_arc));
                    if (strlen(arc_prefix) > 0)
                        snprintf(arc_child, sizeof(arc_child), "%s/%s",
                                 arc_prefix, leaf_arc);
                    else
                        snprintf(arc_child, sizeof(arc_child), "%s", leaf_arc);
                }

                rc = cb(leaf, obj_type_entry, ros_path, arc_prefix, user_data);
                if (rc != BP_OK) return rc;

                /* Advance to next record.
                 * Size = 20 + strlen(leaf) + 1, rounded up to 4 bytes */
                {
                    size_t namelen = strlen(leaf) + 1;
                    size_t recsize = 20 + namelen;
                    if (recsize & 3) recsize = (recsize + 3) & ~3U;
                    p += recsize;
                }
                (void)ros_child; (void)arc_child;
            }
        }
    }
    return BP_OK;
}

/* ====================================================================
 * File addition
 * ==================================================================== */

/*
 * add_ctx_t: context for the recursive add operation.
 */
typedef struct {
    bp_archive_t *ar;
} add_ctx_t;

static int add_riscos_path(bp_archive_t *ar, const char *ros_path,
                           const char *arcname);

/* Callback for riscos_enum_dir during add */
static int add_enum_cb(const char *leaf, int obj_type,
                       const char *ros_parent, const char *arc_parent,
                       void *user_data)
{
    add_ctx_t *ctx = (add_ctx_t *)user_data;
    char ros_child[512], arc_child[512];
    char leaf_arc[256];

    riscos_join(ros_child, sizeof(ros_child), ros_parent, leaf);
    riscos_to_arc(leaf, leaf_arc, sizeof(leaf_arc));
    if (strlen(arc_parent) > 0)
        snprintf(arc_child, sizeof(arc_child), "%s/%s", arc_parent, leaf_arc);
    else
        snprintf(arc_child, sizeof(arc_child), "%s", leaf_arc);

    (void)obj_type; /* resolved again inside add_riscos_path */
    return add_riscos_path(ctx->ar, ros_child, arc_child);
}

static int add_riscos_path(bp_archive_t *ar, const char *ros_path,
                           const char *arcname)
{
    riscos_stat_t st;
    int obj_type;
    int rc;

    obj_type = riscos_stat(ros_path, &st);
    if (obj_type == RISCOS_OBJ_NONE) {
        fprintf(stderr, "boltpack: not found: %s\n", ros_path);
        return BP_ERR_IO;
    }

    if (obj_type == RISCOS_OBJ_DIR) {
        /* Add directory entry */
        char dir_arc[512];
        unsigned char meta_buf[RISCOS_META_SIZE];
        bp_io_t    meta_io;
        rbuf_ctx_t rctx;
        add_ctx_t  sub;

        if (arcname[strlen(arcname) - 1] != '/')
            snprintf(dir_arc, sizeof(dir_arc), "%s/", arcname);
        else
            snprintf(dir_arc, sizeof(dir_arc), "%s", arcname);

        if (g_verbosity == VERB_DEFAULT)
            printf("   creating: %s\n", dir_arc);

        rc = bp_archive_add_entry(ar, dir_arc, BP_TYPE_DIR, BP_CREATOR_OS,
                                  NULL, bp_u64_from_u32(0),
                                  bp_u64_from_u32(riscos_get_mtime(
                                      st.load_addr, st.exec_addr)));
        if (rc != BP_OK) return rc;

        /* Add META for the directory */
        riscos_encode_meta(meta_buf, st.load_addr, st.exec_addr,
                           st.attributes);
        rctx.buf = meta_buf; rctx.size = RISCOS_META_SIZE; rctx.pos = 0;
        bp_io_from_rbuf(&meta_io, &rctx);
        rc = bp_archive_add_entry(ar, dir_arc, BP_TYPE_META, BP_OS_RISCOS,
                                  &meta_io,
                                  bp_u64_from_u32(RISCOS_META_SIZE),
                                  bp_u64_from_u32(0));
        if (rc != BP_OK) return rc;

        /* Recurse into directory */
        sub.ar = ar;
        return riscos_enum_dir(ros_path, arcname, add_enum_cb, &sub);

    } else {
        /* Regular file */
        int fh;
        bp_io_t fio;
        bp_uint64 file_size;
        unsigned char meta_buf[RISCOS_META_SIZE];
        bp_io_t    meta_io;
        rbuf_ctx_t rctx;

        fh = riscos_open(ros_path, OS_Find_OpenIn);
        if (fh == 0) {
            fprintf(stderr, "boltpack: cannot open: %s\n", ros_path);
            return BP_ERR_IO;
        }
        bp_io_from_riscos(&fio, fh);
        file_size = bp_u64_from_u32(st.length);

        if (g_verbosity == VERB_DEFAULT) {
            printf("compressing: %s", arcname);
            fflush(stdout);
        }

        rc = bp_archive_add_entry(ar, arcname, BP_TYPE_DATA, BP_CREATOR_OS,
                                  &fio, file_size,
                                  bp_u64_from_u32(riscos_get_mtime(
                                      st.load_addr, st.exec_addr)));
        riscos_close(fh);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", ros_path,
                    bp_strerror(rc));
            return rc;
        }

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
                double orig = (double)file_size.lo + (double)file_size.hi * 4294967296.0;
                double cmp  = (double)comp.lo      + (double)comp.hi      * 4294967296.0;
                double pct  = (orig > 0.0) ? (cmp / orig * 100.0) : 0.0;
                format_size64(comp, sizebuf, sizeof(sizebuf));
                printf("  %s (%.0f%%)\n", sizebuf, pct);
            }
        }

        /* Add META entry (load/exec/attr) */
        riscos_encode_meta(meta_buf, st.load_addr, st.exec_addr,
                           st.attributes);
        rctx.buf = meta_buf; rctx.size = RISCOS_META_SIZE; rctx.pos = 0;
        bp_io_from_rbuf(&meta_io, &rctx);
        return bp_archive_add_entry(ar, arcname, BP_TYPE_META, BP_OS_RISCOS,
                                    &meta_io,
                                    bp_u64_from_u32(RISCOS_META_SIZE),
                                    bp_u64_from_u32(0));
    }
}

/* ====================================================================
 * List callback
 * ==================================================================== */

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

    if (entry->entry_type == BP_TYPE_DIR)
        ctx->dir_count++;
    else if (entry->entry_type == BP_TYPE_DATA ||
             entry->entry_type == BP_TYPE_SYMLINK)
        ctx->file_count++;
    ctx->total_orig = bp_u64_add(ctx->total_orig, entry->original_size);
    ctx->total_comp = bp_u64_add(ctx->total_comp, entry->compressed_size);

    format_number(bp_u64_to_u32(entry->original_size),  orig_buf, sizeof(orig_buf));
    format_number(bp_u64_to_u32(entry->compressed_size), comp_buf, sizeof(comp_buf));
    printf("%10s %10s  %s  %s\n", orig_buf, comp_buf, type_str, entry->name);
    return 0;
}

/* ====================================================================
 * Extract callbacks
 * ==================================================================== */

typedef struct {
    const char *out_dir;     /* RISC OS path to extract root (may be NULL) */
    int         force;
    int         dry_run;
    bp_uint32   file_count;
    bp_uint32   dir_count;
} extract_ctx_t;

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t  *ctx = (extract_ctx_t *)user_data;
    char            ros_path[1024];
    char            ros_name[512];
    int             fh;
    bp_io_t        *io;
    meta_extract_t *mex;
    bp_uint32       meta_sz;

    /* Convert archive name (/) to RISC OS name (.) */
    arc_to_riscos(entry->name, ros_name, sizeof(ros_name));

    if (ctx->out_dir)
        riscos_join(ros_path, sizeof(ros_path), ctx->out_dir, ros_name);
    else
        snprintf(ros_path, sizeof(ros_path), "%s", ros_name);

    /* --- Directory ---------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        /* Strip trailing '.' (was '/' in archive) */
        {
            size_t len = strlen(ros_path);
            if (len > 0 && ros_path[len - 1] == '.')
                ros_path[len - 1] = '\0';
        }
        if (g_verbosity == VERB_DEFAULT)
            printf("   creating: %s\n", entry->name);
        if (!ctx->dry_run) riscos_create_dir(ros_path);
        return NULL;
    }

    /* --- Metadata ----------------------------------------------------- */
    if (entry->entry_type == BP_TYPE_META) {
        if (entry->platform != BP_OS_RISCOS) return NULL;

        meta_sz = bp_u64_to_u32(entry->original_size);
        if (meta_sz == 0 || meta_sz > RISCOS_META_SIZE + 4) return NULL;

        mex = (meta_extract_t *)malloc(sizeof(meta_extract_t));
        if (!mex) return NULL;

        mex->wctx.buf = mex->buf;
        mex->wctx.cap = sizeof(mex->buf);
        mex->wctx.len = 0;
        mex->wctx.pos = 0;

        mex->io.read   = wbuf_read_fn;
        mex->io.write  = wbuf_write_fn;
        mex->io.seek   = wbuf_seek_fn;
        mex->io.tell   = wbuf_tell_fn;
        mex->io.handle = &mex->wctx;
        return &mex->io;
    }

    /* --- Symlink (not supported on RISC OS) --------------------------- */
    if (entry->entry_type == BP_TYPE_SYMLINK) {
        if (g_verbosity < VERB_SILENT)
            printf("   skipping: %s [symlink - not supported on RISC OS]\n",
                   entry->name);
        return NULL;
    }

    /* --- Resource fork: write as separate file ----------------------- */
    if (entry->entry_type == BP_TYPE_RSRC) {
        char rsrc_path[1100];
        snprintf(rsrc_path, sizeof(rsrc_path), "%s/rsrc", ros_path);
        ctx->file_count++;
        if (g_verbosity == VERB_DEFAULT)
            printf(" extracting: %s [resource fork]\n", entry->name);
        if (ctx->dry_run) return NULL;
        riscos_create_parent(rsrc_path);
        fh = riscos_open(rsrc_path, OS_Find_OpenOut);
        if (fh == 0) {
            fprintf(stderr, "boltpack: cannot create: %s\n", rsrc_path);
            return NULL;
        }
        io = (bp_io_t *)malloc(sizeof(bp_io_t));
        if (!io) { riscos_close(fh); return NULL; }
        bp_io_from_riscos(io, fh);
        return io;
    }

    /* --- Regular file ------------------------------------------------- */
    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT)
        printf(" extracting: %s\n", entry->name);
    if (ctx->dry_run) return NULL;

    if (!ctx->force) {
        riscos_stat_t st_buf;
        if (riscos_stat(ros_path, &st_buf) != RISCOS_OBJ_NONE) {
            fprintf(stderr,
                    "boltpack: file exists (use -f to overwrite): %s\n",
                    ros_path);
            return NULL;
        }
    }
    riscos_create_parent(ros_path);
    fh = riscos_open(ros_path, OS_Find_OpenOut);
    if (fh == 0) {
        fprintf(stderr, "boltpack: cannot create: %s\n", ros_path);
        return NULL;
    }
    io = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (!io) { riscos_close(fh); return NULL; }
    bp_io_from_riscos(io, fh);
    return io;
}

static void extract_done(const bp_entry_t *entry, bp_io_t *out,
                         void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;

    /* --- RISC OS metadata: restore load/exec/attr --------------------- */
    if (entry->entry_type == BP_TYPE_META) {
        meta_extract_t *mex = (meta_extract_t *)out;

        if (entry->platform == BP_OS_RISCOS && !ctx->dry_run) {
            char            ros_name[512];
            char            ros_path[1024];
            unsigned int    load_addr, exec_addr, attributes;

            arc_to_riscos(entry->name, ros_name, sizeof(ros_name));
            if (ctx->out_dir)
                riscos_join(ros_path, sizeof(ros_path),
                            ctx->out_dir, ros_name);
            else
                snprintf(ros_path, sizeof(ros_path), "%s", ros_name);

            if (riscos_decode_meta(mex->wctx.buf, (int)mex->wctx.len,
                                   &load_addr, &exec_addr,
                                   &attributes) == 0) {
                riscos_apply_meta(ros_path, load_addr, exec_addr, attributes);
            }
        }
        free(mex);
        return;
    }

    /* --- Regular file / resource fork: close handle ------------------- */
    {
        int fh = riscos_handle_to_fh(out->handle);
        riscos_close(fh);
        free(out);
    }
}

/* ====================================================================
 * Test callback
 * ==================================================================== */

typedef struct {
    bp_uint32 ok_count;
    bp_uint32 fail_count;
} test_ctx_t;

static void test_cb(const bp_entry_t *entry, int result, void *user_data)
{
    test_ctx_t *ctx = (test_ctx_t *)user_data;
    if (g_verbosity < VERB_SILENT) {
        printf("  testing: %s", entry->name);
        printf(result == BP_OK ? " OK\n" : " FAILED -- %s\n",
               bp_strerror(result));
    }
    if (result == BP_OK) ctx->ok_count++;
    else                 ctx->fail_count++;
}

/* ====================================================================
 * os_name / cpu_name
 * ==================================================================== */

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
    case BP_OS_BEOS:      return "BeOS/Haiku";
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

/* ====================================================================
 * Archive-file bp_io_t (RISC OS file: open by path string)
 * ==================================================================== */

/*
 * riscos_archive_open_read: open a RISC OS file for reading (archive input).
 * Returns file handle > 0 on success, 0 on failure.
 */
static int riscos_archive_open_read(const char *path)
{
    return riscos_open(path, OS_Find_OpenIn);
}

/*
 * riscos_archive_open_write: create/truncate a RISC OS file for writing.
 * Returns file handle > 0 on success, 0 on failure.
 */
static int riscos_archive_open_write(const char *path)
{
    return riscos_open(path, OS_Find_OpenOut);
}

/*
 * riscos_archive_open_readwrite: open existing RISC OS file for read/write.
 */
static int riscos_archive_open_readwrite(const char *path)
{
    return riscos_open(path, OS_Find_OpenUp);
}

/* ====================================================================
 * cmd_info
 * ==================================================================== */

static int cmd_info(const char *archive_name)
{
    int fh;
    bp_io_t aio;
    bp_archive_info_t info;
    int rc;

    fh = riscos_archive_open_read(archive_name);
    if (fh == 0) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    bp_io_from_riscos(&aio, fh);
    rc = bp_archive_info(&aio, &info);
    riscos_close(fh);

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }

    {
        bp_uint32 file_count = info.entry_count - info.dir_count;
        char origbuf[32], compbuf[32], arcbuf[32];

        format_size64(info.archive_size, arcbuf, sizeof(arcbuf));
        format_size64(info.total_original, origbuf, sizeof(origbuf));
        format_size64(info.total_compressed, compbuf, sizeof(compbuf));

        printf("Archive:        %s\n", archive_name);
        printf("Format version: %u\n", (unsigned)info.version);
        printf("Created by:     BoltPack %u.%u\n",
               (unsigned)info.creator_ver_major,
               (unsigned)info.creator_ver_minor);
        printf("Creator OS:     %s\n", os_name(info.creator_os));
        printf("Creator CPU:    %s\n", cpu_name(info.creator_cpu));
        printf("Archive size:   %s\n", arcbuf);
        printf("Entries:        %lu (%lu file%s, %lu director%s)\n",
               (unsigned long)info.entry_count,
               (unsigned long)file_count, file_count == 1 ? "" : "s",
               (unsigned long)info.dir_count,
               info.dir_count == 1 ? "y" : "ies");
        printf("Original size:  %s\n", origbuf);
        printf("Compressed:     %s", compbuf);
        if (info.total_original.hi != 0 || info.total_original.lo != 0) {
            double orig_d = (double)info.total_original.lo
                          + (double)info.total_original.hi * 4294967296.0;
            double comp_d = (double)info.total_compressed.lo
                          + (double)info.total_compressed.hi * 4294967296.0;
            printf(" (%.0f%%)", (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0);
        }
        printf("\n");
    }
    return 0;
}

/* ====================================================================
 * cmd_delete
 * ==================================================================== */

static int cmd_delete(int argc, char **argv)
{
    const char  *archive_name;
    const char **del_names;
    int          del_count;
    int src_fh, dst_fh;
    bp_io_t src_io, dst_io;
    char tmp_name[1024];
    bp_uint32 deleted = 0;
    int rc;

    if (argc < 2) {
        fprintf(stderr, "boltpack: delete requires archive name and file(s)\n");
        return 1;
    }
    archive_name = argv[0];
    del_names    = (const char **)&argv[1];
    del_count    = argc - 1;

    src_fh = riscos_archive_open_read(archive_name);
    if (!src_fh) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    snprintf(tmp_name, sizeof(tmp_name), "%s_bp_tmp", archive_name);
    dst_fh = riscos_archive_open_write(tmp_name);
    if (!dst_fh) {
        fprintf(stderr, "boltpack: cannot create temp file: %s\n", tmp_name);
        riscos_close(src_fh);
        return 1;
    }

    bp_io_from_riscos(&src_io, src_fh);
    bp_io_from_riscos(&dst_io, dst_fh);
    rc = bp_archive_delete(&src_io, &dst_io, del_names, (bp_uint32)del_count,
                           &deleted);
    riscos_close(src_fh);
    riscos_close(dst_fh);

    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: delete failed: %s\n", bp_strerror(rc));
        {
            _kernel_swi_regs r;
            r.r[0] = OS_File_Delete;
            r.r[1] = (int)tmp_name;
            _kernel_swi(OS_File, &r, &r);
        }
        return 1;
    }
    if (deleted == 0) {
        fprintf(stderr, "boltpack: no matching entries found\n");
        {
            _kernel_swi_regs r;
            r.r[0] = OS_File_Delete;
            r.r[1] = (int)tmp_name;
            _kernel_swi(OS_File, &r, &r);
        }
        return 1;
    }
    /* Replace archive with temp file (delete then rename) */
    {
        _kernel_swi_regs r;
        r.r[0] = OS_File_Delete;
        r.r[1] = (int)archive_name;
        _kernel_swi(OS_File, &r, &r);
    }
    {
        _kernel_swi_regs r;
        r.r[0] = 25; /* OS_FSControl 25 = rename */
        r.r[1] = (int)tmp_name;
        r.r[2] = (int)archive_name;
        if (_kernel_swi(OS_FSControl, &r, &r) != NULL) {
            fprintf(stderr, "boltpack: cannot replace archive\n");
            return 1;
        }
    }
    if (g_verbosity < VERB_SILENT)
        printf("%lu entr%s deleted\n",
               (unsigned long)deleted, deleted == 1 ? "y" : "ies");
    return 0;
}

/* ====================================================================
 * cmd_create / cmd_add
 * ==================================================================== */

static bp_uint32 parse_block_size(const char *s)
{
    if (strcmp(s, "8k") == 0  || strcmp(s, "8K") == 0)   return BP_BLOCK_8K;
    if (strcmp(s, "16k") == 0 || strcmp(s, "16K") == 0)  return BP_BLOCK_16K;
    if (strcmp(s, "32k") == 0 || strcmp(s, "32K") == 0)  return BP_BLOCK_32K;
    if (strcmp(s, "64k") == 0 || strcmp(s, "64K") == 0)  return BP_BLOCK_64K;
    if (strcmp(s, "256k") == 0|| strcmp(s, "256K") == 0) return BP_BLOCK_256K;
    return 0;
}

static int cmd_create(int argc, char **argv, bp_uint32 block_size,
                      bp_uint8 method)
{
    const char *archive_name;
    int fh;
    bp_io_t aio;
    bp_archive_t ar;
    int rc, i;
    clock_t start;
    double elapsed;

    if (argc < 2) {
        fprintf(stderr, "boltpack: create requires archive name and files\n");
        return 1;
    }
    archive_name = argv[0];

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }
    fh = riscos_archive_open_write(archive_name);
    if (!fh) {
        fprintf(stderr, "boltpack: cannot create: %s\n", archive_name);
        return 1;
    }
    bp_io_from_riscos(&aio, fh);
    rc = bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        riscos_close(fh); return 1;
    }

    start = clock();
    for (i = 1; i < argc; i++) {
        const char *ros_path = argv[i];
        char arcname[512];
        riscos_to_arc(ros_path, arcname, sizeof(arcname));
        rc = add_riscos_path(&ar, ros_path, arcname);
        if (rc != BP_OK) {
            bp_archive_close(&ar); riscos_close(fh); return 1;
        }
    }
    rc = bp_archive_finalize(&ar);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        bp_archive_close(&ar); riscos_close(fh); return 1;
    }
    elapsed = (double)(clock() - start) / (double)CLOCKS_PER_SEC;

    if (g_verbosity < VERB_SILENT) {
        bp_uint64 total_orig = {0, 0};
        bp_uint64 total_comp = {0, 0};
        char numbuf[32], speedbuf[64];
        bp_uint32 file_count = 0, dir_count = 0, j;

        for (j = 0; j < ar.entries_written; j++) {
            if (ar.entry_list[j].entry_type == BP_TYPE_DIR) dir_count++;
            else                                            file_count++;
            total_orig = bp_u64_add(total_orig, ar.entry_list[j].original_size);
            total_comp = bp_u64_add(total_comp, ar.entry_list[j].compressed_size);
        }
        format_size64(total_orig, numbuf, sizeof(numbuf));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)file_count, file_count == 1 ? "" : "s",
               (unsigned long)dir_count,  dir_count  == 1 ? "y" : "ies",
               numbuf);
        if (!bp_u64_is_zero(total_orig) && !bp_u64_is_zero(total_comp)) {
            double orig_d = (double)total_orig.lo + (double)total_orig.hi * 4294967296.0;
            double comp_d = (double)total_comp.lo + (double)total_comp.hi * 4294967296.0;
            char compbuf[32];
            format_size64(total_comp, compbuf, sizeof(compbuf));
            printf(", %.0f%% (%s)", (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0, compbuf);
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
    riscos_close(fh);
    return 0;
}

static int cmd_add(int argc, char **argv, bp_uint32 block_size, bp_uint8 method)
{
    const char *archive_name;
    int fh;
    bp_io_t io;
    bp_archive_t ar;
    int rc, i;

    if (argc < 2) {
        fprintf(stderr, "boltpack: add requires archive name and files\n");
        return 1;
    }
    archive_name = argv[0];

    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        return 1;
    }
    fh = riscos_archive_open_readwrite(archive_name);
    if (!fh) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    bp_io_from_riscos(&io, fh);
    rc = bp_archive_open_append(&ar, &io, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        riscos_close(fh); return 1;
    }
    for (i = 1; i < argc; i++) {
        const char *ros_path = argv[i];
        char arcname[512];
        riscos_to_arc(ros_path, arcname, sizeof(arcname));
        rc = add_riscos_path(&ar, ros_path, arcname);
        if (rc != BP_OK) {
            bp_archive_close(&ar); riscos_close(fh); return 1;
        }
    }
    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    riscos_close(fh);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: finalize failed: %s\n", bp_strerror(rc));
        return 1;
    }
    if (g_verbosity < VERB_SILENT)
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    return 0;
}

/* ====================================================================
 * cmd_list / cmd_extract / cmd_test
 * ==================================================================== */

static int cmd_list(const char *archive_name)
{
    int fh;
    bp_io_t aio;
    int rc;
    list_ctx_t ctx;

    fh = riscos_archive_open_read(archive_name);
    if (!fh) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    bp_io_from_riscos(&aio, fh);
    printf("%10s %10s  %s  %s\n", "Size", "Compressed", "Type", "Name");

    memset(&ctx, 0, sizeof(ctx));
    rc = bp_archive_list(&aio, list_cb, &ctx);
    riscos_close(fh);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }
    {
        char origbuf[32], compbuf[32];
        format_size64(ctx.total_orig, origbuf, sizeof(origbuf));
        format_size64(ctx.total_comp, compbuf, sizeof(compbuf));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)ctx.file_count, ctx.file_count == 1 ? "" : "s",
               (unsigned long)ctx.dir_count,  ctx.dir_count  == 1 ? "y" : "ies",
               origbuf);
        if (!bp_u64_is_zero(ctx.total_orig) && !bp_u64_is_zero(ctx.total_comp)) {
            double orig_d = (double)ctx.total_orig.lo + (double)ctx.total_orig.hi * 4294967296.0;
            double comp_d = (double)ctx.total_comp.lo + (double)ctx.total_comp.hi * 4294967296.0;
            printf(", %.0f%% (%s)", (orig_d > 0.0) ? (comp_d / orig_d * 100.0) : 0.0, compbuf);
        }
        printf("\n");
    }
    return 0;
}

static int cmd_extract(const char *archive_name, const char *out_dir,
                       int force, int dry_run)
{
    int fh;
    bp_io_t aio;
    extract_ctx_t ctx;
    int rc;
    clock_t start;
    double elapsed;

    fh = riscos_archive_open_read(archive_name);
    if (!fh) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        riscos_close(fh); return 1;
    }
    if (out_dir && !dry_run) riscos_create_dir(out_dir);

    bp_io_from_riscos(&aio, fh);
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir  = out_dir;
    ctx.force    = force;
    ctx.dry_run  = dry_run;
    start = clock();

    rc = bp_archive_extract_all(&aio, extract_open, extract_done,
                                g_verbosity == VERB_DEFAULT ? progress_cb : NULL,
                                &ctx);
    riscos_close(fh);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }
    elapsed = (double)(clock() - start) / (double)CLOCKS_PER_SEC;
    if (g_verbosity < VERB_SILENT) {
        printf("%lu file%s, %lu director%s",
               (unsigned long)ctx.file_count, ctx.file_count == 1 ? "" : "s",
               (unsigned long)ctx.dir_count,  ctx.dir_count  == 1 ? "y" : "ies");
        if (elapsed > 0.0) printf(", %.2fs", elapsed);
        printf("\n");
    }
    return 0;
}

static int cmd_test(const char *archive_name)
{
    int fh;
    bp_io_t aio;
    test_ctx_t ctx;
    int rc;

    fh = riscos_archive_open_read(archive_name);
    if (!fh) {
        fprintf(stderr, "boltpack: cannot open: %s\n", archive_name);
        return 1;
    }
    rc = bp_compress_init();
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: compression init failed: %s\n",
                bp_strerror(rc));
        riscos_close(fh); return 1;
    }
    bp_io_from_riscos(&aio, fh);
    memset(&ctx, 0, sizeof(ctx));
    rc = bp_archive_test(&aio, test_cb, &ctx);
    riscos_close(fh);

    if (rc != BP_OK && rc != BP_ERR_CHECKSUM) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        return 1;
    }
    if (g_verbosity < VERB_SILENT) {
        printf("\n%lu file%s OK",
               (unsigned long)ctx.ok_count, ctx.ok_count == 1 ? "" : "s");
        if (ctx.fail_count > 0)
            printf(", %lu file%s FAILED",
                   (unsigned long)ctx.fail_count,
                   ctx.fail_count == 1 ? "" : "s");
        printf("\n");
    }
    return ctx.fail_count > 0 ? 1 : 0;
}

/* ====================================================================
 * usage / main
 * ==================================================================== */

static void usage(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- lightweight portable archiver\n"
        "%s\n\n"
        "Usage: boltpack <command> [options] archive [files...]\n\n"
        "Commands:\n"
        "  c   Create archive from files\n"
        "  a   Add files to existing archive\n"
        "  x   Extract archive\n"
        "  l   List archive contents\n"
        "  t   Test archive integrity\n"
        "  i   Show archive information\n"
        "  d   Delete files from archive\n",
        BP_VERSION_MAJOR, BP_VERSION_MINOR, AUTHOR_MESSAGE);
    fprintf(stderr,
        "\nOptions:\n"
        "  -b SIZE   Block size: 8k, 16k, 32k, 64k, 256k (default: 32k)\n"
        "  -0        Store only (no compression)\n"
        "  -F        Use fluff LZ77+Huffman compression (better ratio than boltlz)\n"
        "  -q        Quiet (summary only)\n"
        "  -qq       Silent (errors only)\n"
        "  -o PATH   Output directory for extraction (RISC OS path with '.')\n"
        "  -f        Force overwrite existing files\n"
        "  -n        Dry run\n"
        "\nNote: File paths use '.' as directory separator (RISC OS convention).\n"
        "      Archive entry names use '/' (portable convention).\n");
}

int main(int argc, char **argv)
{
    char command;
    bp_uint32   block_size = 0;
    bp_uint8    method     = BP_METHOD_BOLTLZ;
    const char *out_dir    = NULL;
    int force   = 0;
    int dry_run = 0;
    int i;

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
        } else if (strcmp(argv[i], "-0") == 0) {
            method = BP_METHOD_STORED; i++;
        } else if (strcmp(argv[i], "-F") == 0) {
            method = BP_METHOD_FLUFF; i++;
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
        } else {
            fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]);
            usage(); return 1;
        }
    }

    if (method == BP_METHOD_FLUFF && block_size == 0) {
        block_size = BP_BLOCK_256K;
    }

    switch (command) {
    case 'c': return cmd_create(argc - i, argv + i, block_size, method);
    case 'a': return cmd_add(argc - i, argv + i, block_size, method);
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
    case 'd': return cmd_delete(argc - i, argv + i);
    default:
        fprintf(stderr, "boltpack: unknown command: %c\n", command);
        usage(); return 1;
    }
}
