/*
 * main.c -- OS/2 CLI frontend for BoltPack
 *
 * Target:   IBM OS/2 Warp 3/4 (32-bit), ArcaOS, eComStation.
 * Compiler: Open Watcom v2 (os2 target) or os2-gcc (GCC with EMX/kLIBC).
 *
 * File I/O uses the standard ANSI C runtime (fopen/fread/fwrite/fseek/ftell)
 * provided by kLIBC or Open Watcom's OS/2 C runtime.
 * Directory traversal uses the native OS/2 API (DosFindFirst/DosFindNext).
 * Optional HPFS extended attribute support reads/writes the ".TYPE" EA
 * to preserve file-type information across archives.
 *
 * Metadata (BP_TYPE_META) payload for BP_OS_OS2 (same as DOS family):
 *   Byte 0 : FAT/HPFS attribute byte (read-only/hidden/system/archive bits)
 *
 * Cross-compilation from Linux:
 *   os2-gcc -ansi -Wall -Wextra -O2                        \
 *       -I src/core -I lib/fastlz                    \
 *       src/core/archiver.c src/core/compress.c            \
 *       src/core/bp_codepage.c                             \
 *       src/frontend/os2/main.c                            \
 *       lib/fastlz/fastlz.c                         \
 *       -o build/os2/boltpack.exe
 *
 * ANSI C (C89/C90) throughout.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_OS2
#define BP_CREATOR_CPU BP_CPU_X86

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_codepage.h"

/* OS/2 Toolkit headers */
#define INCL_DOSFILEMGR
#define INCL_DOSERRORS
#define INCL_DOSPROCESS
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* ---- constants -------------------------------------------------------- */

#define BP_MAX_PATH   260

/* OS/2 uses CP850 for Western Europe */
#define OS2_CP  BP_CP_850

/* FAT/HPFS attribute bits */
#ifndef FILE_READONLY
#  define FILE_READONLY   0x0001
#  define FILE_HIDDEN     0x0002
#  define FILE_SYSTEM     0x0004
#  define FILE_DIRECTORY  0x0010
#  define FILE_ARCHIVED   0x0020
#endif

#define OS2_ATTR_MASK   (FILE_READONLY | FILE_HIDDEN | FILE_SYSTEM | FILE_ARCHIVED)

/* Metadata payload: 1 byte (FAT/HPFS attribute) */
#define OS2_META_PAYLOAD    1

/* EA name for file type (HPFS extended attribute) */
#define OS2_EA_TYPE         ".TYPE"
#define OS2_EA_VALUE_MAX    64

/* HPFS .TYPE EA value type codes */
#define EAT_ASCII   0xFFFD  /* ASCII string value */

/* HPFS EA on/off flag */
#define OS2_EA_ENABLED  1   /* set to 0 to skip EA operations */

/* ---- verbosity -------------------------------------------------------- */

#define VERB_DEFAULT  0
#define VERB_QUIET    1
#define VERB_SILENT   2

static int g_verbosity = VERB_DEFAULT;

/* ======================================================================
 * bp_io_t adapters for FILE *
 * ====================================================================== */

static long file_read(void *h, void *buf, bp_uint32 n)
{
    return (long)fread(buf, 1, (size_t)n, (FILE *)h);
}

static long file_write(void *h, const void *buf, bp_uint32 n)
{
    return (long)fwrite(buf, 1, (size_t)n, (FILE *)h);
}

static int file_seek(void *h, bp_off_t off, int org)
{
    return fseek((FILE *)h, (long)off, org);
}

static bp_off_t file_tell(void *h)
{
    return (bp_off_t)ftell((FILE *)h);
}

static void bp_io_from_file(bp_io_t *io, FILE *fp)
{
    io->read   = file_read;
    io->write  = file_write;
    io->seek   = file_seek;
    io->tell   = file_tell;
    io->handle = fp;
}

/* ======================================================================
 * Read-only memory buffer I/O  (for metadata payload supply).
 * ====================================================================== */

typedef struct {
    const unsigned char *buf;
    bp_uint32            size;
    bp_uint32            pos;
} rbuf_ctx_t;

static long rbuf_read_fn(void *h, void *buf, bp_uint32 n)
{
    rbuf_ctx_t *c = (rbuf_ctx_t *)h;
    if (c->pos >= c->size) return 0;
    if (n > c->size - c->pos) n = c->size - c->pos;
    memcpy(buf, c->buf + c->pos, (size_t)n);
    c->pos += n;
    return (long)n;
}
static long   rbuf_write_fn(void *h, const void *b, bp_uint32 n) { (void)h;(void)b;(void)n; return -1; }
static int    rbuf_seek_fn(void *h, bp_off_t off, int org)
{
    rbuf_ctx_t *c = (rbuf_ctx_t *)h;
    long np;
    if (org == SEEK_SET)      np = (long)off;
    else if (org == SEEK_CUR) np = (long)c->pos + (long)off;
    else if (org == SEEK_END) np = (long)c->size + (long)off;
    else return -1;
    if (np < 0 || (bp_uint32)np > c->size) return -1;
    c->pos = (bp_uint32)np;
    return 0;
}
static bp_off_t rbuf_tell_fn(void *h) { return (bp_off_t)((rbuf_ctx_t *)h)->pos; }

static void bp_io_from_rbuf(bp_io_t *io, rbuf_ctx_t *ctx)
{
    io->read   = rbuf_read_fn;
    io->write  = rbuf_write_fn;
    io->seek   = rbuf_seek_fn;
    io->tell   = rbuf_tell_fn;
    io->handle = ctx;
}

/* ======================================================================
 * Write-only memory buffer I/O  (for metadata capture on extraction).
 * ====================================================================== */

typedef struct {
    unsigned char *buf;
    bp_uint32      cap;
    bp_uint32      len;
    bp_uint32      pos;
} wbuf_ctx_t;

static long wbuf_read_fn(void *h, void *b, bp_uint32 n) { (void)h;(void)b;(void)n; return -1; }
static long wbuf_write_fn(void *h, const void *data, bp_uint32 n)
{
    wbuf_ctx_t *c = (wbuf_ctx_t *)h;
    if (c->pos + n > c->cap) n = c->cap - c->pos;
    if (!n) return 0;
    memcpy(c->buf + c->pos, data, (size_t)n);
    c->pos += n;
    if (c->pos > c->len) c->len = c->pos;
    return (long)n;
}
static int wbuf_seek_fn(void *h, bp_off_t off, int org)
{
    wbuf_ctx_t *c = (wbuf_ctx_t *)h;
    long np;
    if (org == SEEK_SET)      np = (long)off;
    else if (org == SEEK_CUR) np = (long)c->pos + (long)off;
    else if (org == SEEK_END) np = (long)c->len + (long)off;
    else return -1;
    if (np < 0 || (bp_uint32)np > c->cap) return -1;
    c->pos = (bp_uint32)np;
    return 0;
}
static bp_off_t wbuf_tell_fn(void *h) { return (bp_off_t)((wbuf_ctx_t *)h)->pos; }

typedef struct {
    bp_io_t       io;       /* MUST be first */
    wbuf_ctx_t    wctx;
    unsigned char buf[4];   /* 1-byte attribute + padding */
} meta_extract_t;

/* ---- error messages --------------------------------------------------- */

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

/* ---- speed / size / number formatting --------------------------------- */

static void format_speed(double bps, char *buf, size_t sz)
{
    (void)sz;
    if (bps < 1024.0)         sprintf(buf, "%.0f bytes/s", bps);
    else if (bps < 1048576.0) sprintf(buf, "%.2f KB/s", bps / 1024.0);
    else                      sprintf(buf, "%.2f MB/s", bps / 1048576.0);
}

static void format_size(bp_uint32 bytes, char *buf, size_t sz)
{
    (void)sz;
    if (bytes < 1024UL)         sprintf(buf, "%lu bytes", (unsigned long)bytes);
    else if (bytes < 1048576UL) sprintf(buf, "%.1f KB", (double)bytes / 1024.0);
    else if (bytes < 1073741824UL) sprintf(buf, "%.1f MB", (double)bytes / 1048576.0);
    else                        sprintf(buf, "%.2f GB", (double)bytes / 1073741824.0);
}

static void format_size64(bp_uint64 bytes, char *buf, size_t sz)
{
    (void)sz;
    if (bytes.hi == 0) { format_size(bytes.lo, buf, sz); return; }
    { double d = (double)bytes.lo + (double)bytes.hi * 4294967296.0;
      if (d < 1048576.0)     sprintf(buf, "%.1f KB", d / 1024.0);
      else if (d < 1073741824.0) sprintf(buf, "%.1f MB", d / 1048576.0);
      else                   sprintf(buf, "%.2f GB", d / 1073741824.0); }
}

static void format_number(bp_uint32 n, char *buf, size_t sz)
{
    char tmp[32]; int len, i, j, commas;
    sprintf(tmp, "%lu", (unsigned long)n);
    len    = (int)strlen(tmp);
    commas = (len - 1) / 3;
    if ((size_t)(len + commas + 1) > sz) { strcpy(buf, tmp); return; }
    j = len + commas; buf[j] = '\0';
    for (i = len - 1; i >= 0; i--) {
        buf[--j] = tmp[i];
        if (i > 0 && (len - i) % 3 == 0) buf[--j] = ',';
    }
}

/* ---- progress callback ------------------------------------------------ */

static int progress_cb(const bp_progress_t *p, void *ud)
{
    (void)p; (void)ud; return 0;
}

/* ---- path utilities --------------------------------------------------- */

static int ends_with_sep(const char *s)
{
    size_t n = strlen(s);
    return n > 0 && (s[n - 1] == '/' || s[n - 1] == '\\');
}

static void path_join(char *buf, size_t sz, const char *dir, const char *name)
{
    (void)sz;
    if (ends_with_sep(dir)) sprintf(buf, "%s%s", dir, name);
    else                    sprintf(buf, "%s/%s", dir, name);
}

static const char *clean_path(const char *p)
{
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '/' || *p == '\\') p++;
    while (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) p += 2;
    return p;
}

static void normalise_seps(char *p)
{
    for (; *p; p++) if (*p == '\\') *p = '/';
}

/* ---- mkdir -p --------------------------------------------------------- */

static int os2_mkdir(const char *path)
{
    return DosCreateDir((PSZ)path, NULL) == NO_ERROR ? 0 : -1;
}

static int mkdirs(const char *path)
{
    char  tmp[BP_MAX_PATH]; char *p;
    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sv = *p; *p = '\0';
            if (os2_mkdir(tmp) != 0 && DosQueryPathInfo((PSZ)tmp,
                    FIL_STANDARD, NULL, 0) != NO_ERROR)
            { *p = sv; return -1; }
            *p = sv;
        }
    }
    if (os2_mkdir(tmp) != 0) {
        /* Ignore EEXIST */
        FILESTATUS3 fs; APIRET rc2 = DosQueryPathInfo((PSZ)tmp, FIL_STANDARD, &fs, sizeof(fs));
        if (rc2 != NO_ERROR) return -1;
    }
    return 0;
}

static int mkdirs_for_file(const char *path)
{
    char tmp[BP_MAX_PATH]; char *sl, *bsl;
    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    sl  = strrchr(tmp, '/');
    bsl = strrchr(tmp, '\\');
    if (bsl && (!sl || bsl > sl)) sl = bsl;
    if (sl && sl != tmp) { *sl = '\0'; return mkdirs(tmp); }
    return 0;
}

/* ---- parse block size ------------------------------------------------- */

static bp_uint32 parse_block_size(const char *s)
{
    if (!strcmp(s,"8k")   || !strcmp(s,"8K"))   return BP_BLOCK_8K;
    if (!strcmp(s,"16k")  || !strcmp(s,"16K"))  return BP_BLOCK_16K;
    if (!strcmp(s,"32k")  || !strcmp(s,"32K"))  return BP_BLOCK_32K;
    if (!strcmp(s,"64k")  || !strcmp(s,"64K"))  return BP_BLOCK_64K;
    if (!strcmp(s,"256k") || !strcmp(s,"256K")) return BP_BLOCK_256K;
    return 0;
}

/* ---- HPFS .TYPE EA helpers -------------------------------------------- */

/*
 * Try to read the ".TYPE" extended attribute from a file.
 * On HPFS volumes, this contains an ASCII string like "Plain Text".
 * Returns the EA value length (0 if not present or not readable).
 *
 * The GEA2LIST/FEA2LIST structures follow the OS/2 EA API.
 */
static int os2_get_ea_type(const char *path, char *val, int val_max)
{
#pragma pack(1)
    /* Get EA Areas: list of EAs to query */
    struct {
        ULONG  cbList;
        ULONG  oNext;
        BYTE   cbName;
        CHAR   szName[sizeof(OS2_EA_TYPE) + 1];
    } gea2;
    /* Found EA Areas */
    struct {
        ULONG  cbList;
        ULONG  oNext;
        BYTE   fEA;
        BYTE   cbName;
        USHORT cbValue;
        CHAR   szNameValue[sizeof(OS2_EA_TYPE) + 1 + 4 + OS2_EA_VALUE_MAX];
    } fea2;
    EAOP2  eaop;
#pragma pack()
    APIRET rc;
    int    vlen;

    memset(&gea2, 0, sizeof(gea2));
    gea2.cbList = sizeof(gea2);
    gea2.oNext  = 0;
    gea2.cbName = (BYTE)(sizeof(OS2_EA_TYPE) - 1);
    strcpy(gea2.szName, OS2_EA_TYPE);

    memset(&fea2, 0, sizeof(fea2));
    fea2.cbList = sizeof(fea2);

    eaop.fpGEA2List = (GEA2LIST *)&gea2;
    eaop.fpFEA2List = (FEA2LIST *)&fea2;
    eaop.oError     = 0;

    rc = DosQueryPathInfo((PSZ)path, FIL_QUERYEASFROMLIST, &eaop, sizeof(eaop));
    if (rc != NO_ERROR) return 0;
    if (fea2.cbValue < 4) return 0;  /* too short for type + length + data */

    /* FEA2 value layout: USHORT type (EAT_ASCII etc.) + USHORT length + data */
    {
        USHORT ea_type, ea_len;
        memcpy(&ea_type, fea2.szNameValue + fea2.cbName + 1, 2);
        memcpy(&ea_len,  fea2.szNameValue + fea2.cbName + 1 + 2, 2);
        if (ea_len == 0 || ea_len > (USHORT)val_max) return 0;
        memcpy(val, fea2.szNameValue + fea2.cbName + 1 + 4, ea_len);
        val[ea_len] = '\0';
        vlen = (int)ea_len;
    }
    return vlen;
}

/* OS/2 mtime from FDATE/FTIME (DOS-packed format) */
static bp_uint32 os2_mtime(FDATE d, FTIME t)
{
    unsigned yr  = (unsigned)(d.year + 1980);
    unsigned mon = (unsigned)d.month;
    unsigned day = (unsigned)d.day;
    unsigned hr  = (unsigned)t.hours;
    unsigned min = (unsigned)t.minutes;
    unsigned sec = (unsigned)t.twosecs * 2;
    /* Simple approximation to Unix epoch seconds */
    unsigned long days = (yr - 1970) * 365UL + (yr - 1969) / 4
                       + (mon > 1 ? (mon - 1) * 30 : 0) + day;
    return (bp_uint32)(days * 86400UL + hr * 3600UL + min * 60UL + sec);
}

/* Set OS/2 file attribute via DosSetPathInfo */
static void os2_apply_attr(const char *path, unsigned char attr)
{
    FILESTATUS3 fs;
    if (DosQueryPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs)) != NO_ERROR)
        return;
    fs.attrFile = (ULONG)(attr & OS2_ATTR_MASK);
    DosSetPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs), DSPI_WRTTHRU);
}

/* ---- recursive add ---------------------------------------------------- */

static int add_meta_entry(bp_archive_t *ar, const char *arcname,
                          unsigned char attr)
{
    unsigned char meta_buf[OS2_META_PAYLOAD];
    bp_io_t       meta_io;
    rbuf_ctx_t    rctx;

    meta_buf[0] = attr & OS2_ATTR_MASK;
    rctx.buf  = meta_buf;
    rctx.size = OS2_META_PAYLOAD;
    rctx.pos  = 0;
    bp_io_from_rbuf(&meta_io, &rctx);

    return bp_archive_add_entry(ar, arcname, BP_TYPE_META, BP_OS_OS2,
                                &meta_io,
                                bp_u64_from_u32(OS2_META_PAYLOAD),
                                bp_u64_from_u32(0));
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname);

static int add_directory_contents(bp_archive_t *ar, const char *dir_path,
                                  const char *arc_prefix)
{
    HDIR          hDir = HDIR_CREATE;
    FILEFINDBUF3  fb;
    ULONG         cFound = 1;
    APIRET        rc;
    char          pattern[BP_MAX_PATH];
    char          child_path[BP_MAX_PATH];
    char          child_arc[BP_MAX_PATH];
    int           brc;

    sprintf(pattern, "%s\\*", dir_path);

    rc = DosFindFirst((PSZ)pattern, &hDir,
                      FILE_NORMAL | FILE_DIRECTORY | FILE_HIDDEN | FILE_SYSTEM,
                      &fb, sizeof(fb), &cFound, FIL_STANDARD);
    while (rc == NO_ERROR) {
        if (strcmp(fb.achName, ".") != 0 && strcmp(fb.achName, "..") != 0) {
            path_join(child_path, sizeof(child_path), dir_path, fb.achName);
            path_join(child_arc,  sizeof(child_arc),  arc_prefix, fb.achName);
            brc = add_path(ar, child_path, child_arc);
            if (brc != BP_OK) { DosFindClose(hDir); return brc; }
        }
        cFound = 1;
        rc = DosFindNext(hDir, &fb, sizeof(fb), &cFound);
    }
    DosFindClose(hDir);
    return BP_OK;
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    FILESTATUS3 fs;
    APIRET      apc;
    int         rc;

    apc = DosQueryPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs));
    if (apc != NO_ERROR) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (fs.attrFile & FILE_DIRECTORY) {
        /* Directory */
        char dir_arc[BP_MAX_PATH];
        char dir_arc_utf8[BP_MAX_PATH * 2];

        if (!ends_with_sep(arcname)) sprintf(dir_arc, "%s/", arcname);
        else                         strcpy(dir_arc, arcname);

        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", dir_arc);

        if (bp_local_to_utf8(dir_arc, dir_arc_utf8, sizeof(dir_arc_utf8), OS2_CP) != 0)
            strcpy(dir_arc_utf8, dir_arc);

        rc = bp_archive_add_entry(ar, dir_arc_utf8, BP_TYPE_DIR, BP_OS_OS2,
                                  NULL, bp_u64_from_u32(0),
                                  bp_u64_from_u32(os2_mtime(fs.fdateLastWrite,
                                                             fs.ftimeLastWrite)));
        if (rc != BP_OK) return rc;

        rc = add_meta_entry(ar, dir_arc_utf8, (unsigned char)fs.attrFile);
        if (rc != BP_OK) return rc;

        return add_directory_contents(ar, path, dir_arc);
    }

    /* Regular file */
    {
        FILE     *fp;
        bp_io_t   fio;
        bp_uint64 fsize = bp_u64_from_ulong(fs.cbFile);
        char      arcname_utf8[BP_MAX_PATH * 2];
        bp_uint32 mtime = os2_mtime(fs.fdateLastWrite, fs.ftimeLastWrite);

        fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, "boltpack: cannot open: %s\n", path);
            return BP_ERR_IO;
        }
        bp_io_from_file(&fio, fp);

        if (g_verbosity == VERB_DEFAULT) {
            printf("compressing: %s", arcname); fflush(stdout);
        }

        if (bp_local_to_utf8(arcname, arcname_utf8, sizeof(arcname_utf8), OS2_CP) != 0)
            strcpy(arcname_utf8, arcname);

        rc = bp_archive_add_entry(ar, arcname_utf8, BP_TYPE_DATA, BP_OS_OS2,
                                  &fio, fsize, bp_u64_from_u32(mtime));
        fclose(fp);
        if (rc != BP_OK) return rc;

        if (g_verbosity == VERB_DEFAULT) {
            bp_entry_t *e = &ar->entry_list[ar->entries_written - 1];
            bp_uint32 comp = bp_u64_to_u32(e->compressed_size);
            bp_uint32 fsz  = bp_u64_to_u32(fsize);
            char sbuf[32];
            if (comp == 0 || e->method == BP_METHOD_STORED || comp >= fsz) {
                format_size(fsz, sbuf, sizeof(sbuf)); printf("  %s (stored)\n", sbuf);
            } else {
                format_size(comp, sbuf, sizeof(sbuf));
                printf("  %s (%.0f%%)\n", sbuf, (double)comp / (double)fsz * 100.0);
            }
        }

        return add_meta_entry(ar, arcname_utf8, (unsigned char)fs.attrFile);
    }
}

/* ---- list callback ---------------------------------------------------- */

typedef struct {
    bp_uint32 file_count;
    bp_uint32 dir_count;
    bp_uint64 total_orig;
    bp_uint64 total_comp;
} list_ctx_t;

static int list_cb(const bp_entry_t *entry, void *user_data)
{
    list_ctx_t *ctx = (list_ctx_t *)user_data;
    const char *type;
    char ob[32], cb[32], dn[BP_MAX_PATH];

    switch (entry->entry_type) {
    case BP_TYPE_DATA:    type = "DATA"; break;
    case BP_TYPE_RSRC:    type = "RSRC"; break;
    case BP_TYPE_META:    type = "META"; break;
    case BP_TYPE_SYMLINK: type = "LINK"; break;
    case BP_TYPE_DIR:     type = "DIR "; break;
    default:              type = "??? "; break;
    }
    if (entry->entry_type == BP_TYPE_DIR) ctx->dir_count++;
    else if (entry->entry_type == BP_TYPE_DATA || entry->entry_type == BP_TYPE_SYMLINK)
        ctx->file_count++;
    ctx->total_orig = bp_u64_add(ctx->total_orig, entry->original_size);
    ctx->total_comp = bp_u64_add(ctx->total_comp, entry->compressed_size);
    if (bp_utf8_to_local(entry->name, dn, sizeof(dn), OS2_CP) != 0) {
        strncpy(dn, entry->name, sizeof(dn) - 1); dn[sizeof(dn) - 1] = '\0';
    }
    format_number(bp_u64_to_u32(entry->original_size),  ob, sizeof(ob));
    format_number(bp_u64_to_u32(entry->compressed_size), cb, sizeof(cb));
    printf("%10s %10s  %s  %s\n", ob, cb, type, dn);
    return 0;
}

/* ---- extract callbacks ------------------------------------------------ */

typedef struct {
    const char *out_dir;
    int         force;
    int         dry_run;
    bp_uint32   file_count;
    bp_uint32   dir_count;
} extract_ctx_t;

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t  *ctx = (extract_ctx_t *)user_data;
    char            path[BP_MAX_PATH];
    char            local_name[BP_MAX_PATH];
    meta_extract_t *mex;
    bp_io_t        *io;
    FILE           *fp;
    bp_uint32       meta_sz;

    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name), OS2_CP) != 0) {
        strncpy(local_name, entry->name, sizeof(local_name) - 1);
        local_name[sizeof(local_name) - 1] = '\0';
    }
    if (ctx->out_dir) path_join(path, sizeof(path), ctx->out_dir, local_name);
    else              strcpy(path, local_name);

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", local_name);
        if (!ctx->dry_run) mkdirs(path);
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_META) {
        if (entry->platform != BP_OS_OS2) return NULL;
        meta_sz = bp_u64_to_u32(entry->original_size);
        if (meta_sz == 0 || meta_sz > OS2_META_PAYLOAD + 4) return NULL;
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

    if (entry->entry_type == BP_TYPE_RSRC || entry->entry_type == BP_TYPE_SYMLINK) {
        if (g_verbosity == VERB_DEFAULT)
            printf("   skipping: %s [not supported on OS/2]\n", local_name);
        return NULL;
    }

    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT) printf(" extracting: %s\n", local_name);
    if (ctx->dry_run) return NULL;

    if (!ctx->force) {
        FILESTATUS3 tmp_fs;
        if (DosQueryPathInfo((PSZ)path, FIL_STANDARD, &tmp_fs, sizeof(tmp_fs))
            == NO_ERROR) {
            fprintf(stderr, "boltpack: file exists (use -f to overwrite): %s\n", path);
            return NULL;
        }
    }

    mkdirs_for_file(path);
    fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "boltpack: cannot create: %s\n", path); return NULL; }
    io = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (!io) { fclose(fp); return NULL; }
    bp_io_from_file(io, fp);
    return io;
}

static void extract_done(const bp_entry_t *entry, bp_io_t *out, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;

    if (entry->entry_type == BP_TYPE_META) {
        meta_extract_t *mex = (meta_extract_t *)out;
        if (entry->platform == BP_OS_OS2 && !ctx->dry_run
            && mex->wctx.len >= OS2_META_PAYLOAD) {
            char path[BP_MAX_PATH], local_name[BP_MAX_PATH];
            if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name), OS2_CP) != 0)
                strncpy(local_name, entry->name, sizeof(local_name) - 1);
            if (ctx->out_dir) path_join(path, sizeof(path), ctx->out_dir, local_name);
            else              strcpy(path, local_name);
            os2_apply_attr(path, mex->buf[0]);
        }
        free(mex);
        return;
    }

    {
        FILE *fp = (FILE *)out->handle;
        char  path[BP_MAX_PATH], local_name[BP_MAX_PATH];
        FILESTATUS3 fs;
        fclose(fp);
        if (!ctx->dry_run) {
            if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name), OS2_CP) != 0)
                strncpy(local_name, entry->name, sizeof(local_name) - 1);
            if (ctx->out_dir) path_join(path, sizeof(path), ctx->out_dir, local_name);
            else              strcpy(path, local_name);
            /* Restore mtime via DosSetPathInfo */
            if (DosQueryPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs)) == NO_ERROR) {
                time_t t = (time_t)entry->mtime.lo;
                struct tm *tm_p = gmtime(&t);
                if (tm_p) {
                    fs.fdateLastWrite.year    = (USHORT)(tm_p->tm_year - 80);
                    fs.fdateLastWrite.month   = (USHORT)(tm_p->tm_mon + 1);
                    fs.fdateLastWrite.day     = (USHORT)tm_p->tm_mday;
                    fs.ftimeLastWrite.hours   = (USHORT)tm_p->tm_hour;
                    fs.ftimeLastWrite.minutes = (USHORT)tm_p->tm_min;
                    fs.ftimeLastWrite.twosecs = (USHORT)(tm_p->tm_sec / 2);
                    DosSetPathInfo((PSZ)path, FIL_STANDARD, &fs, sizeof(fs), DSPI_WRTTHRU);
                }
            }
        }
        free(out);
    }
}

/* ---- test callback ---------------------------------------------------- */

typedef struct { bp_uint32 ok; bp_uint32 fail; } test_ctx_t;

static void test_cb(const bp_entry_t *entry, int result, void *user_data)
{
    test_ctx_t *ctx = (test_ctx_t *)user_data;
    if (g_verbosity < VERB_SILENT) {
        char dn[BP_MAX_PATH];
        if (bp_utf8_to_local(entry->name, dn, sizeof(dn), OS2_CP) != 0)
            strncpy(dn, entry->name, sizeof(dn) - 1);
        printf("  testing: %s %s\n", dn,
               result == BP_OK ? "OK" : bp_strerror(result));
    }
    if (result == BP_OK) ctx->ok++; else ctx->fail++;
}

/* ---- usage ------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d -- lightweight portable archiver (OS/2)\n"
        "%s\n\n"
        "Usage: boltpack <command> [options] archive.blt [files...]\n\n"
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
        "  -F        Use fluff LZ77+Huffman compression (better ratio than fastlz)\n"
        "  -q        Quiet (summary only)\n"
        "  -qq       Silent (errors only)\n"
        "  -o PATH   Output directory for extraction\n"
        "  -f        Force overwrite existing files\n"
        "  -n        Dry run\n");
}

/* ---- os_name / cpu_name ----------------------------------------------- */

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
    case BP_OS_DOS:       return "MS-DOS";
    case BP_OS_WINDOWS:   return "Windows";
    case BP_OS_FREEBSD:   return "FreeBSD";
    case BP_OS_NETBSD:    return "NetBSD";
    case BP_OS_OPENBSD:   return "OpenBSD";
    case BP_OS_SOLARIS:   return "Solaris";
    case BP_OS_IRIX:      return "IRIX";
    case BP_OS_HPUX:      return "HP-UX";
    case BP_OS_AIX:       return "AIX";
    case BP_OS_ATARI:     return "Atari TOS/MiNT";
    case BP_OS_RISCOS:    return "RISC OS";
    case BP_OS_OS2:       return "OS/2";
    case BP_OS_BEOS:      return "BeOS/Haiku";
    case BP_OS_OPENVMS:   return "OpenVMS";
    case BP_OS_ZOS:       return "z/OS";
    case BP_OS_WASM:      return "WebAssembly";
    case BP_OS_NEXTSTEP:  return "NeXTSTEP/OPENSTEP";
    case BP_OS_QNX:       return "QNX Neutrino";
    case BP_OS_PLAN9:     return "Plan 9";
    case BP_OS_TRU64:     return "Tru64 UNIX";
    case BP_OS_SCO:       return "SCO OpenServer";
    case BP_OS_CPM86:     return "CP/M-86";
    case BP_OS_QL:        return "Sinclair QL";
    case BP_OS_WIN16:     return "Windows 3.x (Win16)";
    case BP_OS_HURD:      return "GNU Hurd";
    case BP_OS_X68K:      return "Sharp X68000";
    default:              return "Unknown";
    }
}

static const char *cpu_name(bp_uint8 cpu)
{
    switch (cpu) {
    case BP_CPU_UNKNOWN: return "Unknown";
    case BP_CPU_X86:     return "x86";
    case BP_CPU_X86_64:  return "x86-64";
    case BP_CPU_PPC32:   return "PowerPC";
    case BP_CPU_PPC64:   return "PowerPC 64";
    case BP_CPU_M68K:    return "Motorola 68k";
    case BP_CPU_ARM32:   return "ARM";
    case BP_CPU_ARM64:   return "ARM64";
    case BP_CPU_MIPS32:  return "MIPS";
    case BP_CPU_MIPS64:  return "MIPS64";
    case BP_CPU_SPARC:   return "SPARC";
    case BP_CPU_ALPHA:   return "Alpha";
    case BP_CPU_PARISC:  return "PA-RISC";
    case BP_CPU_RISCV32: return "RISC-V 32";
    case BP_CPU_RISCV64: return "RISC-V 64";
    case BP_CPU_SH:      return "SuperH";
    case BP_CPU_6502:    return "6502";
    case BP_CPU_Z80:     return "Z80";
    case BP_CPU_8086:    return "8086/80286+";
    case BP_CPU_E2K:     return "Elbrus (e2k)";
    case BP_CPU_ZARCH:   return "z/Architecture";
    case BP_CPU_SPARC64: return "SPARC64";
    case BP_CPU_IA64:    return "Itanium (IA-64)";
    default:             return "Unknown";
    }
}

static const char *format_methods(const bp_uint32 *counts)
{
    static char buf[128];
    static const char * const names[4] = {"Stored", "LZSS", "fastlz", "Fluff"};
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
    FILE *fp; bp_io_t aio; bp_archive_info_t info; int rc;
    fp = fopen(archive_name, "rb");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", archive_name); return 1; }
    bp_io_from_file(&aio, fp);
    rc = bp_archive_info(&aio, &info);
    fclose(fp);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    {
        bp_uint32 fc = info.entry_count - info.dir_count;
        char ob[32], cb[32], ab[32];
        format_size64(info.total_original,   ob, sizeof(ob));
        format_size64(info.total_compressed, cb, sizeof(cb));
        format_size64(info.archive_size,     ab, sizeof(ab));
        printf("Archive:        %s\n",  archive_name);
        printf("Format version: %u\n",  (unsigned)info.version);
        printf("Created by:     BoltPack %u.%u\n",
               (unsigned)info.creator_ver_major, (unsigned)info.creator_ver_minor);
        printf("Creator OS:     %s\n",  os_name(info.creator_os));
        printf("Creator CPU:    %s\n",  cpu_name(info.creator_cpu));
        printf("Method:         %s\n", format_methods(info.method_counts));
        printf("Archive size:   %s\n",  ab);
        printf("Entries:        %lu (%lu file%s, %lu director%s)\n",
               (unsigned long)info.entry_count,
               (unsigned long)fc, fc == 1 ? "" : "s",
               (unsigned long)info.dir_count, info.dir_count == 1 ? "y" : "ies");
        printf("Original size:  %s\n",  ob);
        printf("Compressed:     %s", cb);
        if (!bp_u64_is_zero(info.total_original)) {
            double od = (double)info.total_original.lo
                      + (double)info.total_original.hi * 4294967296.0;
            double cd = (double)info.total_compressed.lo
                      + (double)info.total_compressed.hi * 4294967296.0;
            printf(" (%.0f%%)", od > 0.0 ? cd / od * 100.0 : 0.0);
        }
        printf("\n");
    }
    return 0;
}

/* ---- cmd_list --------------------------------------------------------- */

static int cmd_list(const char *archive_name)
{
    FILE *fp; bp_io_t aio; list_ctx_t ctx; int rc;
    fp = fopen(archive_name, "rb");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", archive_name); return 1; }
    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    printf("%10s %10s  %s  %s\n", "Size", "Compressed", "Type", "Name");
    rc = bp_archive_list(&aio, list_cb, &ctx);
    fclose(fp);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    {
        char ob[32], cb[32];
        format_size64(ctx.total_orig, ob, sizeof(ob));
        format_size64(ctx.total_comp, cb, sizeof(cb));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)ctx.file_count, ctx.file_count == 1 ? "" : "s",
               (unsigned long)ctx.dir_count,  ctx.dir_count  == 1 ? "y" : "ies", ob);
        if (!bp_u64_is_zero(ctx.total_orig) && !bp_u64_is_zero(ctx.total_comp)) {
            double od = (double)ctx.total_orig.lo + (double)ctx.total_orig.hi * 4294967296.0;
            double cd = (double)ctx.total_comp.lo + (double)ctx.total_comp.hi * 4294967296.0;
            printf(", %.0f%% (%s)", od > 0.0 ? cd / od * 100.0 : 0.0, cb);
        }
        printf("\n");
    }
    return 0;
}

/* ---- cmd_create ------------------------------------------------------- */

static int cmd_create(int argc, char **argv, bp_uint32 block_size, bp_uint8 method)
{
    FILE *fp; bp_io_t aio; bp_archive_t ar; int rc, i; clock_t start;
    if (argc < 2) { fprintf(stderr, "boltpack: create requires archive and files\n"); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    fp = fopen(argv[0], "w+b");
    if (!fp) { fprintf(stderr, "boltpack: cannot create: %s\n", argv[0]); return 1; }
    bp_io_from_file(&aio, fp);
    rc = bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    start = clock();
    for (i = 1; i < argc; i++) {
        char buf[BP_MAX_PATH]; strcpy(buf, clean_path(argv[i])); normalise_seps(buf);
        rc = add_path(&ar, argv[i], buf);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", argv[i], bp_strerror(rc));
            bp_archive_close(&ar); fclose(fp); return 1;
        }
    }
    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar); fclose(fp);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: finalize: %s\n", bp_strerror(rc)); return 1; }
    if (g_verbosity < VERB_SILENT) {
        double elapsed = (double)(clock() - start) / (double)CLOCKS_PER_SEC;
        bp_uint64 to = {0,0}, tc = {0,0}; bp_uint32 j, fc = 0, dc = 0;
        char nb[32], sb[64];
        for (j = 0; j < ar.entries_written; j++) {
            if (ar.entry_list[j].entry_type == BP_TYPE_DIR) dc++;
            else fc++;
            to = bp_u64_add(to, ar.entry_list[j].original_size);
            tc = bp_u64_add(tc, ar.entry_list[j].compressed_size);
        }
        format_size64(to, nb, sizeof(nb));
        printf("%lu file%s, %lu director%s, %s",
               (unsigned long)fc, fc == 1 ? "" : "s",
               (unsigned long)dc, dc == 1 ? "y" : "ies", nb);
        if (!bp_u64_is_zero(to) && !bp_u64_is_zero(tc)) {
            double od = (double)to.lo + (double)to.hi * 4294967296.0;
            double cd = (double)tc.lo + (double)tc.hi * 4294967296.0;
            char cb[32]; format_size64(tc, cb, sizeof(cb));
            printf(", %.0f%% (%s)", od > 0.0 ? cd / od * 100.0 : 0.0, cb);
        }
        if (elapsed > 0.0) {
            double od = (double)to.lo + (double)to.hi * 4294967296.0;
            printf(", %.2fs", elapsed);
            format_speed(od / elapsed, sb, sizeof(sb));
            printf(", %s", sb);
        }
        printf("\n");
    }
    return 0;
}

/* ---- cmd_add ---------------------------------------------------------- */

static int cmd_add(int argc, char **argv, bp_uint32 block_size, bp_uint8 method)
{
    FILE *fp; bp_io_t aio; bp_archive_t ar; int rc, i;
    if (argc < 2) { fprintf(stderr, "boltpack: add requires archive and files\n"); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    fp = fopen(argv[0], "r+b");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", argv[0]); return 1; }
    bp_io_from_file(&aio, fp);
    rc = bp_archive_open_append(&ar, &aio, block_size, method);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    for (i = 1; i < argc; i++) {
        rc = add_path(&ar, argv[i], clean_path(argv[i]));
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding %s: %s\n", argv[i], bp_strerror(rc));
            bp_archive_close(&ar); fclose(fp); return 1;
        }
    }
    rc = bp_archive_finalize(&ar); bp_archive_close(&ar); fclose(fp);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: finalize: %s\n", bp_strerror(rc)); return 1; }
    if (g_verbosity < VERB_SILENT)
        printf("%d file%s added\n", argc - 1, (argc - 1) == 1 ? "" : "s");
    return 0;
}

/* ---- cmd_extract ------------------------------------------------------ */

static int cmd_extract(const char *archive_name, const char *out_dir,
                       int force, int dry_run)
{
    FILE *fp; bp_io_t aio; extract_ctx_t ctx; int rc;
    clock_t start; double elapsed;
    fp = fopen(archive_name, "rb");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", archive_name); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    if (out_dir && !dry_run) mkdirs(out_dir);
    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir; ctx.force = force; ctx.dry_run = dry_run;
    start = clock();
    rc = bp_archive_extract_all(&aio, extract_open, extract_done,
                                g_verbosity == VERB_DEFAULT ? progress_cb : NULL,
                                &ctx);
    fclose(fp);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
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

/* ---- cmd_test --------------------------------------------------------- */

static int cmd_test(const char *archive_name)
{
    FILE *fp; bp_io_t aio; test_ctx_t ctx; int rc;
    fp = fopen(archive_name, "rb");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", archive_name); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    rc = bp_archive_test(&aio, test_cb, &ctx);
    fclose(fp);
    if (rc != BP_OK && rc != BP_ERR_CHECKSUM) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1;
    }
    if (g_verbosity < VERB_SILENT) {
        printf("\n%lu file%s OK", (unsigned long)ctx.ok, ctx.ok == 1 ? "" : "s");
        if (ctx.fail) printf(", %lu file%s FAILED",
                             (unsigned long)ctx.fail, ctx.fail == 1 ? "" : "s");
        printf("\n");
    }
    return ctx.fail > 0 ? 1 : 0;
}

/* ---- cmd_delete ------------------------------------------------------- */

static int cmd_delete(int argc, char **argv)
{
    const char **del; int del_count;
    FILE *sf, *df; bp_io_t sio, dio;
    char tmp[BP_MAX_PATH]; bp_uint32 deleted = 0; int rc;

    if (argc < 2) { fprintf(stderr, "boltpack: delete requires archive and files\n"); return 1; }
    del = (const char **)&argv[1]; del_count = argc - 1;

    sf = fopen(argv[0], "rb");
    if (!sf) { fprintf(stderr, "boltpack: cannot open: %s\n", argv[0]); return 1; }
    sprintf(tmp, "%s.bp_tmp", argv[0]);
    df = fopen(tmp, "w+b");
    if (!df) { fprintf(stderr, "boltpack: cannot create temp file\n"); fclose(sf); return 1; }
    bp_io_from_file(&sio, sf);
    bp_io_from_file(&dio, df);
    rc = bp_archive_delete(&sio, &dio, del, (bp_uint32)del_count, &deleted);
    fclose(sf); fclose(df);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: delete failed: %s\n", bp_strerror(rc));
        remove(tmp); return 1;
    }
    if (!deleted) { fprintf(stderr, "boltpack: no matching entries found\n"); remove(tmp); return 1; }
    if (remove(argv[0]) != 0 || rename(tmp, argv[0]) != 0) {
        fprintf(stderr, "boltpack: cannot replace archive\n"); return 1;
    }
    if (g_verbosity < VERB_SILENT)
        printf("%lu entr%s deleted\n", (unsigned long)deleted, deleted == 1 ? "y" : "ies");
    return 0;
}

/* ---- main ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char       command;
    bp_uint32  block_size = 0;
    bp_uint8   method     = BP_METHOD_FASTLZ;
    const char *out_dir   = NULL;
    int        force = 0, dry_run = 0, i;

    if (argc < 3) { usage(); return 1; }
    if (strlen(argv[1]) != 1) {
        fprintf(stderr, "boltpack: unknown command: %s\n", argv[1]); usage(); return 1;
    }
    command = argv[1][0];
    i = 2;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-b")) {
            if (++i >= argc) { fprintf(stderr, "boltpack: -b needs argument\n"); return 1; }
            block_size = parse_block_size(argv[i++]);
            if (!block_size) { fprintf(stderr, "boltpack: invalid block size\n"); return 1; }
        } else if (!strcmp(argv[i], "-0"))  { method     = BP_METHOD_STORED; i++; }
        else if (!strcmp(argv[i], "-F"))  { method     = BP_METHOD_FLUFF;  i++; }
        else if (!strcmp(argv[i], "-qq")) { g_verbosity = VERB_SILENT;  i++; }
        else if (!strcmp(argv[i], "-q"))  { g_verbosity = VERB_QUIET;   i++; }
        else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) { fprintf(stderr, "boltpack: -o needs argument\n"); return 1; }
            out_dir = argv[i++];
        } else if (!strcmp(argv[i], "-f")) { force   = 1; i++; }
        else if (!strcmp(argv[i], "-n"))   { dry_run = 1; i++; }
        else { fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]); usage(); return 1; }
    }

    if (method == BP_METHOD_FLUFF && block_size == 0) {
        block_size = BP_BLOCK_256K;
    }

    switch (command) {
    case 'c': return cmd_create(argc - i, argv + i, block_size, method);
    case 'a': return cmd_add(argc - i, argv + i, block_size, method);
    case 'l': if (i >= argc) { fprintf(stderr, "boltpack: list needs archive\n"); return 1; }
              return cmd_list(argv[i]);
    case 'x': if (i >= argc) { fprintf(stderr, "boltpack: extract needs archive\n"); return 1; }
              return cmd_extract(argv[i], out_dir, force, dry_run);
    case 't': if (i >= argc) { fprintf(stderr, "boltpack: test needs archive\n"); return 1; }
              return cmd_test(argv[i]);
    case 'i': if (i >= argc) { fprintf(stderr, "boltpack: info needs archive\n"); return 1; }
              return cmd_info(argv[i]);
    case 'd': return cmd_delete(argc - i, argv + i);
    default:  fprintf(stderr, "boltpack: unknown command: %c\n", command); usage(); return 1;
    }
}
