/*
 * main.c -- Sinclair QL (QDOS) CLI frontend for BoltPack
 *
 * Target:   Sinclair QL / SMSQ/E, QPC2, Q68.
 * Compiler: m68k-qdos-gcc (GCC port for QDOS/SMSQ).
 *
 * File I/O uses the standard ANSI C runtime (fopen/fread/fwrite).
 * Directory traversal reads QDOS channel data as raw qdirect entries.
 *
 * QDOS conventions used here:
 *   - Directory paths end with '_':  "win1_mydir_"
 *   - Files do not end with '_':     "win1_mydir_file"
 *   - The device prefix is stripped before building archive entry names.
 *   - Timestamps are seconds since 1961-01-01 00:00:00 UTC (QDOS epoch).
 *     Converted to Unix seconds by subtracting 283,996,800.
 *
 * Metadata: QL provides only a simple 3-bit "protection" field embedded in
 *   the directory entry's access byte.  Per format.md, this does NOT need a
 *   separate BP_TYPE_META blob; the protection information is intentionally
 *   omitted from the archive (the receiving OS can restore reasonable
 *   defaults).  If BP_TYPE_META entries from other platforms are present,
 *   they are silently skipped.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_QL
#define BP_CREATOR_CPU BP_CPU_M68K

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_codepage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* ---- constants -------------------------------------------------------- */

#define BP_MAX_PATH   256

/* QL uses ISO-8859-1 / Latin-1 for filenames */
#define QL_CP   BP_CP_8859_1

/*
 * QDOS epoch offset: seconds from QDOS epoch (1961-01-01)
 * to Unix epoch (1970-01-01):
 *   9 years: 7 × 365 + 2 × 366 (1964, 1968 are leap) = 3287 days
 *   3287 × 86400 = 283,996,800 seconds
 */
#define QDOS_EPOCH_OFFSET   283996800UL

/* Maximum QDOS filename length */
#define QDOS_NAME_MAX   36

/* ---- qdirect: QDOS directory entry (64 bytes, big-endian) ------------- */

/*
 * Read from a directory channel (fopen on a QDOS device path ending
 * with '_') using fread.  All multi-byte fields are big-endian
 * (M68000 native) -- no byte-swap needed when compiled for M68k QDOS.
 */
#pragma pack(1)
typedef struct {
    long  d_length;         /* +0  file length                             */
    unsigned char  d_access; /* +4  file access flags                      */
    unsigned char  d_type;  /* +5  file type (0=BASIC,1=DATA,2=CODE,3=REL)*/
    long  d_datalen;        /* +6  data space (EXEC/REL files)             */
    short d_extra1;         /* +10 reserved                                */
    short d_extra2;         /* +12 reserved                                */
    short d_szname;         /* +14 name length (0..36, big-endian)         */
    char  d_name[36];       /* +16 name (not NUL-terminated, space-padded) */
    long  d_update;         /* +52 last update (secs from 1961-01-01)      */
    long  d_refdate;        /* +56 reference date (same epoch)             */
    long  d_backup;         /* +60 backup date (same epoch)                */
} qdirect_t;                /* Total = 64 bytes                            */
#pragma pack()

/* ---- verbosity -------------------------------------------------------- */

#define VERB_DEFAULT  0
#define VERB_QUIET    1
#define VERB_SILENT   2

static int g_verbosity = VERB_DEFAULT;

/* ======================================================================
 * bp_io_t  adapters  --  backed by FILE *
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
    io->read = file_read; io->write = file_write;
    io->seek = file_seek; io->tell  = file_tell;
    io->handle = fp;
}

/* ======================================================================
 * Read buffer I/O  (metadata supply -- currently unused, retained for
 * future use and structural consistency with other frontends).
 * ====================================================================== */

typedef struct {
    const unsigned char *buf;
    bp_uint32 size;
    bp_uint32 pos;
} rbuf_ctx_t;

static long rbuf_read(void *h, void *b, bp_uint32 n)
{
    rbuf_ctx_t *c = (rbuf_ctx_t *)h;
    if (c->pos >= c->size) return 0;
    if (n > c->size - c->pos) n = c->size - c->pos;
    memcpy(b, c->buf + c->pos, n); c->pos += n; return (long)n;
}
static long rbuf_write(void *h, const void *b, bp_uint32 n) { (void)h;(void)b;(void)n; return -1; }
static int  rbuf_seek(void *h, bp_off_t o, int org)
{
    rbuf_ctx_t *c = (rbuf_ctx_t *)h;
    long np = (org==0)?(long)o:(org==1)?((long)c->pos+(long)o):((long)c->size+(long)o);
    if (np<0||(bp_uint32)np>c->size) return -1;
    c->pos=(bp_uint32)np; return 0;
}
static bp_off_t rbuf_tell(void *h){ return (bp_off_t)((rbuf_ctx_t*)h)->pos; }

static void bp_io_from_rbuf(bp_io_t *io, rbuf_ctx_t *ctx)
{
    io->read = rbuf_read; io->write = rbuf_write;
    io->seek = rbuf_seek; io->tell  = rbuf_tell;
    io->handle = ctx;
}

/* ---- error messages --------------------------------------------------- */

static const char *bp_strerror(int code)
{
    switch (code) {
    case BP_OK:                    return "success";
    case BP_ERR_IO:                return "I/O error";
    case BP_ERR_NOMEM:             return "out of memory";
    case BP_ERR_MAGIC:             return "not a BoltPack archive";
    case BP_ERR_VERSION:           return "unsupported version";
    case BP_ERR_CHECKSUM:          return "checksum mismatch";
    case BP_ERR_CORRUPT:           return "archive is corrupt";
    case BP_ERR_TRUNCATED:         return "unexpected end of file";
    case BP_ERR_OVERFLOW:          return "size overflow";
    case BP_ERR_NAME_TOO_LONG:     return "name too long";
    case BP_ERR_UNSUPPORTED_METHOD:return "unsupported method";
    case BP_ERR_COMPRESS:          return "compression error";
    case BP_ERR_DECOMPRESS:        return "decompression error";
    case BP_ERR_ABORTED:           return "aborted";
    case BP_ERR_NOT_FOUND:         return "not found";
    case BP_ERR_INVALID_ARG:       return "invalid argument";
    default:                       return "unknown error";
    }
}

/* ---- formatting -------------------------------------------------------- */

static void format_size(bp_uint32 bytes, char *buf, size_t sz)
{
    if (bytes < 1024UL)         sprintf(buf, "%lu bytes", (unsigned long)bytes);
    else if (bytes < 1048576UL) sprintf(buf, "%.1f KB", (double)bytes / 1024.0);
    else                        sprintf(buf, "%.2f MB", (double)bytes / 1048576.0);
    (void)sz;
}

static void format_size64(bp_uint64 b, char *buf, size_t sz)
{
    if (b.hi == 0) { format_size(b.lo, buf, sz); return; }
    { double d = (double)b.lo + (double)b.hi * 4294967296.0;
      sprintf(buf, "%.2f MB", d / 1048576.0); }
    (void)sz;
}

static void ql_strcpy_trunc(char *dst, size_t dstsz, const char *src)
{
    if (dstsz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dstsz - 1);
    dst[dstsz - 1] = '\0';
}

static void ql_strcat_trunc(char *dst, size_t dstsz, const char *src)
{
    size_t len;

    if (dstsz == 0 || !src) return;
    len = strlen(dst);
    if (len >= dstsz - 1) return;
    strncpy(dst + len, src, dstsz - len - 1);
    dst[dstsz - 1] = '\0';
}

static void ql_join2(char *dst, size_t dstsz, const char *left,
                     const char *right)
{
    ql_strcpy_trunc(dst, dstsz, left);
    ql_strcat_trunc(dst, dstsz, right);
}

static void ql_join3(char *dst, size_t dstsz, const char *left,
                     const char *middle, const char *right)
{
    ql_strcpy_trunc(dst, dstsz, left);
    ql_strcat_trunc(dst, dstsz, middle);
    ql_strcat_trunc(dst, dstsz, right);
}

/* ---- QDOS time conversion ---------------------------------------------- */

static bp_uint32 qdos_to_unix(long qdos_time)
{
    unsigned long t = (unsigned long)qdos_time;
    if (t > QDOS_EPOCH_OFFSET) return (bp_uint32)(t - QDOS_EPOCH_OFFSET);
    return 0;
}

/* ---- QDOS path utilities ----------------------------------------------- */

/*
 * Strip the device prefix from a QDOS path.
 * "win1_mydir_file" → "mydir_file"
 * "win1_" → "" (root of device)
 * "myfile" (no device prefix) → "myfile"
 */
static const char *ql_strip_device(const char *qdos_path)
{
    const char *p = strchr(qdos_path, '_');
    return p ? p + 1 : qdos_path;
}

/*
 * Convert a QDOS relative path to an archive name.
 * "mydir_file" → "mydir/file"
 * "mydir_subdir_file" → "mydir/subdir/file"
 * Trailing '_' indicates a directory: "mydir_" → "mydir/"
 *
 * Since QDOS allows '_' in genuine filenames too, this conversion is
 * applied only at known directory-separator positions tracked during
 * recursive descent.  We pass `arcname` directly from the caller.
 */
static void ql_to_arcname(const char *qdos_rel, char *out, size_t outsz,
                           const char *parent_arc)
{
    /* `parent_arc` is built by our recursive descent so uses '/'.
     * Just append the entry name to it. */
    if (parent_arc && parent_arc[0]) {
        int plen = (int)strlen(parent_arc);
        if (parent_arc[plen - 1] == '/')
            ql_join2(out, outsz, parent_arc, qdos_rel);
        else
            ql_join3(out, outsz, parent_arc, "/", qdos_rel);
    } else {
        strncpy(out, qdos_rel, outsz - 1); out[outsz - 1] = '\0';
    }
}

/*
 * Extract the QDOS name field from a qdirect entry.
 * d_szname is big-endian but on M68k host it's already correct.
 * Space-pad is stripped; a NUL is appended.
 */
static int qdirect_get_name(const qdirect_t *e, char *buf, int bufsz)
{
    int n = (int)(unsigned short)e->d_szname;  /* big-endian on M68k */
    if (n <= 0 || n > QDOS_NAME_MAX || n >= bufsz) return 0;
    strncpy(buf, e->d_name, (size_t)n);
    buf[n] = '\0';
    return n;
}

/*
 * True if the qdirect entry represents a sub-directory.
 * QDOS identifies directories by the trailing '_' in their name.
 */
static int qdirect_is_dir(const qdirect_t *e)
{
    char name[QDOS_NAME_MAX + 1]; int n;
    n = qdirect_get_name(e, name, sizeof(name));
    return n > 0 && name[n - 1] == '_';
}

/* ---- mkdir -p ---------------------------------------------------------- */

static int ql_mkdir(const char *path)
{
    /* QDOS directory creation: create an empty file with a trailing '_' */
    FILE *fp;
    char qdos_path[BP_MAX_PATH];
    size_t plen = strlen(path);
    if (plen + 2 >= sizeof(qdos_path)) return -1;
    strcpy(qdos_path, path);
    if (qdos_path[plen - 1] != '_') { qdos_path[plen] = '_'; qdos_path[plen + 1] = '\0'; }
    fp = fopen(qdos_path, "w");
    if (!fp) return -1;
    fclose(fp);
    return 0;
}

static int mkdirs(const char *path)
{
    char tmp[BP_MAX_PATH]; char *p;
    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    /* Replace '/' seps with QDOS '_' seps */
    for (p = tmp; *p; p++) if (*p == '/') *p = '_';
    /* Ensure trailing '_' for directory create */
    { int n = (int)strlen(tmp); if (n > 0 && tmp[n-1] != '_') { tmp[n] = '_'; tmp[n+1] = '\0'; } }
    return ql_mkdir(tmp);
}

static int mkdirs_for_file(const char *arcname)
{
    /* Extract directory component from archive name and create it */
    char tmp[BP_MAX_PATH]; char *sl;
    if (strlen(arcname) >= sizeof(tmp)) return -1;
    strcpy(tmp, arcname);
    sl = strrchr(tmp, '/');
    if (sl && sl != tmp) { *sl = '\0'; return mkdirs(tmp); }
    return 0;
}

/* ---- block size parsing ----------------------------------------------- */

static bp_uint32 parse_block_size(const char *s)
{
    if (!strcmp(s,"8k")   || !strcmp(s,"8K"))   return BP_BLOCK_8K;
    if (!strcmp(s,"16k")  || !strcmp(s,"16K"))  return BP_BLOCK_16K;
    if (!strcmp(s,"32k")  || !strcmp(s,"32K"))  return BP_BLOCK_32K;
    if (!strcmp(s,"64k")  || !strcmp(s,"64K"))  return BP_BLOCK_64K;
    if (!strcmp(s,"256k") || !strcmp(s,"256K")) return BP_BLOCK_256K;
    return 0;
}

/* ---- recursive add ---------------------------------------------------- */

static int add_path_qdos(bp_archive_t *ar, const char *qdos_path,
                         const char *arcname);

static int add_directory_contents(bp_archive_t *ar, const char *qdos_dir,
                                  const char *arc_prefix)
{
    FILE      *dp;
    qdirect_t  ent;
    char       ent_name[QDOS_NAME_MAX + 1];
    char       child_qdos[BP_MAX_PATH];
    char       child_arc[BP_MAX_PATH];
    int        rc = BP_OK;

    dp = fopen(qdos_dir, "r");
    if (!dp) {
        fprintf(stderr, "boltpack: cannot open directory: %s\n", qdos_dir);
        return BP_ERR_IO;
    }

    while (rc == BP_OK &&
           fread(&ent, 1, sizeof(ent), dp) == sizeof(ent)) {
        if (!qdirect_get_name(&ent, ent_name, sizeof(ent_name))) continue;

        /* Build full QDOS path: device path ends with '_', so just append */
        if (strlen(qdos_dir) + strlen(ent_name) >= sizeof(child_qdos)) continue;
        strcpy(child_qdos, qdos_dir); strcat(child_qdos, ent_name);

        /* Archive name */
        ql_to_arcname(ent_name, child_arc, sizeof(child_arc), arc_prefix);

        if (qdirect_is_dir(&ent)) {
            /* Recurse into subdirectory.
             * qdos_path ends with '_', so child is qdos_dir + "subname_"
             * which is already in child_qdos. */
            char dir_arc[BP_MAX_PATH];
            /* Ensure arc path ends with '/' */
            ql_strcpy_trunc(dir_arc, sizeof(dir_arc), child_arc);
            {
                int al = (int)strlen(dir_arc);
                if (al > 0 && dir_arc[al - 1] != '/')
                    { dir_arc[al] = '/'; dir_arc[al + 1] = '\0'; }
            }

            if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", dir_arc);
            rc = bp_archive_add_entry(ar, dir_arc, BP_TYPE_DIR, BP_OS_QL,
                                      NULL, bp_u64_from_u32(0),
                                      bp_u64_from_u32(qdos_to_unix(ent.d_update)));
            if (rc == BP_OK)
                rc = add_directory_contents(ar, child_qdos, dir_arc);
        } else {
            rc = add_path_qdos(ar, child_qdos, child_arc);
        }
    }

    fclose(dp);
    return rc;
}

static int add_path_qdos(bp_archive_t *ar, const char *qdos_path,
                         const char *arcname)
{
    FILE      *fp;
    long       sz;
    bp_io_t    fio;
    bp_uint32  mtime;
    bp_uint64  fsize;
    int        rc;
    /* Get file size and mtime via reading a directory entry.
     * QDOS C rt provides fopen/fseek/ftell for regular files. */

    fp = fopen(qdos_path, "rb");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot open: %s\n", qdos_path);
        return BP_ERR_IO;
    }

    fseek(fp, 0L, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0L, SEEK_SET);
    fsize = bp_u64_from_ulong((unsigned long)sz);

    /* QDOS doesn't expose mtime easily via fopen; default to 0. */
    mtime = 0;

    bp_io_from_file(&fio, fp);

    if (g_verbosity == VERB_DEFAULT) { printf("compressing: %s", arcname); fflush(stdout); }

    rc = bp_archive_add_entry(ar, arcname, BP_TYPE_DATA, BP_OS_QL,
                              &fio, fsize, bp_u64_from_u32(mtime));
    fclose(fp);

    if (rc == BP_OK && g_verbosity == VERB_DEFAULT) {
        bp_entry_t *e = &ar->entry_list[ar->entries_written - 1];
        bp_uint32 comp = bp_u64_to_u32(e->compressed_size);
        bp_uint32 fsz  = bp_u64_to_u32(fsize);
        char sb[32];
        if (comp == 0 || e->method == BP_METHOD_STORED || comp >= fsz) {
            format_size(fsz, sb, sizeof(sb)); printf("  %s (stored)\n", sb);
        } else {
            format_size(comp, sb, sizeof(sb));
            printf("  %s (%.0f%%)\n", sb, (double)comp / (double)fsz * 100.0);
        }
    }
    return rc;
}

/*
 * Public entry point: add a QDOS path (file or directory) to the archive.
 * `qdos_path`  is the native QDOS path (may include device prefix).
 * `arcname`    is the desired archive entry name.
 */
static int add_path(bp_archive_t *ar, const char *qdos_path, const char *arcname)
{
    size_t plen = strlen(qdos_path);

    if (plen > 0 && qdos_path[plen - 1] == '_') {
        /* It's a directory */
        char dir_arc[BP_MAX_PATH];
        int  al;
        int  rc;

        strncpy(dir_arc, arcname, sizeof(dir_arc) - 2);
        dir_arc[sizeof(dir_arc) - 2] = '\0';
        al = (int)strlen(dir_arc);
        if (al == 0 || dir_arc[al - 1] != '/')
            { dir_arc[al] = '/'; dir_arc[al + 1] = '\0'; }

        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", dir_arc);
        rc = bp_archive_add_entry(ar, dir_arc, BP_TYPE_DIR, BP_OS_QL,
                                  NULL, bp_u64_from_u32(0),
                                  bp_u64_from_u32(0));
        if (rc != BP_OK) return rc;
        return add_directory_contents(ar, qdos_path, dir_arc);
    }
    return add_path_qdos(ar, qdos_path, arcname);
}

/* ---- list callback ---------------------------------------------------- */

typedef struct {
    bp_uint32 file_count; bp_uint32 dir_count;
    bp_uint64 total_orig; bp_uint64 total_comp;
} list_ctx_t;

static int list_cb(const bp_entry_t *entry, void *user_data)
{
    list_ctx_t *ctx = (list_ctx_t *)user_data;
    const char *type;
    char ob[32], cb[32];
    char local_name[BP_MAX_PATH];

    switch (entry->entry_type) {
    case BP_TYPE_DATA:    type = "DATA"; break;
    case BP_TYPE_RSRC:    type = "RSRC"; break;
    case BP_TYPE_META:    type = "META"; break;
    case BP_TYPE_SYMLINK: type = "LINK"; break;
    case BP_TYPE_DIR:     type = "DIR "; break;
    default:              type = "??? "; break;
    }
    if (entry->entry_type == BP_TYPE_DIR)  ctx->dir_count++;
    else if (entry->entry_type == BP_TYPE_DATA) ctx->file_count++;
    ctx->total_orig = bp_u64_add(ctx->total_orig, entry->original_size);
    ctx->total_comp = bp_u64_add(ctx->total_comp, entry->compressed_size);

    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name), QL_CP) != 0)
        strncpy(local_name, entry->name, sizeof(local_name) - 1);

    format_size(bp_u64_to_u32(entry->original_size),  ob, sizeof(ob));
    format_size(bp_u64_to_u32(entry->compressed_size), cb, sizeof(cb));
    printf("%10s %10s  %s  %s\n", ob, cb, type, local_name);
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
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char  path[BP_MAX_PATH];
    char  local_name[BP_MAX_PATH];
    FILE *fp; bp_io_t *io;

    if (!bp_name_is_safe(entry->name)) return NULL;

    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name), QL_CP) != 0) {
        strncpy(local_name, entry->name, sizeof(local_name) - 1);
        local_name[sizeof(local_name) - 1] = '\0';
    }

    if (ctx->out_dir && ctx->out_dir[0]) {
        size_t odn = strlen(ctx->out_dir);
        if (ctx->out_dir[odn - 1] == '/')
            ql_join2(path, sizeof(path), ctx->out_dir, local_name);
        else
            ql_join3(path, sizeof(path), ctx->out_dir, "/", local_name);
    } else {
        strncpy(path, local_name, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", local_name);
        if (!ctx->dry_run) mkdirs(path);
        return NULL;
    }

    /* Skip META, RSRC, SYMLINK on QL */
    if (entry->entry_type != BP_TYPE_DATA) {
        if (g_verbosity == VERB_DEFAULT)
            printf("   skipping: %s [type not supported on QL]\n", local_name);
        return NULL;
    }

    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT) printf(" extracting: %s\n", local_name);
    if (ctx->dry_run) return NULL;

    if (!ctx->force) {
        FILE *chk = fopen(path, "rb");
        if (chk) { fclose(chk); fprintf(stderr, "boltpack: exists (use -f): %s\n", path); return NULL; }
    }

    mkdirs_for_file(local_name);
    fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "boltpack: cannot create: %s\n", path); return NULL; }
    io = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (!io) { fclose(fp); return NULL; }
    bp_io_from_file(io, fp);
    return io;
}

static void extract_done(const bp_entry_t *entry, bp_io_t *out, void *user_data)
{
    (void)user_data;
    if (out) {
        fclose((FILE *)out->handle);
        free(out);
    }
    (void)entry;
}

/* ---- test callback ---------------------------------------------------- */

typedef struct { bp_uint32 ok; bp_uint32 fail; } test_ctx_t;

static void test_cb(const bp_entry_t *entry, int result, void *user_data)
{
    test_ctx_t *ctx = (test_ctx_t *)user_data;
    char local_name[BP_MAX_PATH];
    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name), QL_CP) != 0)
        strncpy(local_name, entry->name, sizeof(local_name) - 1);
    if (g_verbosity < VERB_SILENT)
        printf("  testing: %s %s\n", local_name, result == BP_OK ? "OK" : bp_strerror(result));
    if (result == BP_OK) ctx->ok++; else ctx->fail++;
}

/* ---- usage ------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d (Sinclair QL)\n"
        "%s\n\n"
        "Usage: boltpack <cmd> [opts] archive.blt [files...]\n\n"
        "  c create   a add   x extract\n"
        "  l list     t test  i info   d delete\n\n"
        "Options:\n"
        "  -b SIZE   block size: 8k 16k 32k 64k 256k\n"
        "  -0        store only    -F fluff   -q quiet   -qq silent\n"
        "  -o PATH   output path for extraction\n"
        "  -f        force overwrite    -n dry run\n"
        "\nNote: device prefixes (win1_, mdv1_ etc.) are stripped from archive names.\n",
        BP_VERSION_MAJOR, BP_VERSION_MINOR, AUTHOR_MESSAGE);
}

/* ---- os_name / cpu_name ----------------------------------------------- */

static const char *os_name(bp_uint8 os)
{
    switch (os) {
    case BP_OS_UNKNOWN:  return "Unknown";
    case BP_OS_LINUX:    return "Linux";
    case BP_OS_MACOSX:   return "Mac OS X";
    case BP_OS_DOS:      return "MS-DOS";
    case BP_OS_WINDOWS:  return "Windows";
    case BP_OS_AMIGA_68K:return "AmigaOS";
    case BP_OS_ATARI:    return "Atari TOS";
    case BP_OS_OS2:      return "OS/2";
    case BP_OS_BEOS:     return "BeOS/Haiku";
    case BP_OS_PLAN9:    return "Plan 9";
    case BP_OS_QL:       return "Sinclair QL";
    case BP_OS_WIN16:    return "Windows 3.x";
    case BP_OS_X68K:     return "Sharp X68000";
    case BP_OS_FREEBSD:  return "FreeBSD";
    case BP_OS_NETBSD:   return "NetBSD";
    case BP_OS_OPENBSD:  return "OpenBSD";
    case BP_OS_SOLARIS:  return "Solaris";
    case BP_OS_IRIX:     return "IRIX";
    case BP_OS_HPUX:     return "HP-UX";
    case BP_OS_AIX:      return "AIX";
    case BP_OS_RISCOS:   return "RISC OS";
    case BP_OS_MACOS9:   return "Mac OS 9";
    default:             return "Unknown";
    }
}

static const char *cpu_name(bp_uint8 cpu)
{
    switch (cpu) {
    case BP_CPU_UNKNOWN: return "Unknown";
    case BP_CPU_X86:     return "x86";
    case BP_CPU_X86_64:  return "x86-64";
    case BP_CPU_M68K:    return "Motorola 68k";
    case BP_CPU_ARM32:   return "ARM";
    case BP_CPU_PPC32:   return "PowerPC";
    case BP_CPU_MIPS32:  return "MIPS";
    case BP_CPU_SPARC:   return "SPARC";
    default:             return "Unknown";
    }
}

/* ---- cmd_info --------------------------------------------------------- */

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
        char ob[32], cb[32], ab[32];
        format_size64(info.total_original,   ob, sizeof(ob));
        format_size64(info.total_compressed, cb, sizeof(cb));
        format_size64(info.archive_size,     ab, sizeof(ab));
        printf("Archive:       %s\n",  archive_name);
        printf("Format:        v%u\n", (unsigned)info.version);
        printf("Creator OS:    %s\n",  os_name(info.creator_os));
        printf("Creator CPU:   %s\n",  cpu_name(info.creator_cpu));
        printf("Archive size:  %s\n",  ab);
        printf("Entries:       %lu (%lu files, %lu dirs)\n",
               (unsigned long)info.entry_count,
               (unsigned long)(info.entry_count - info.dir_count),
               (unsigned long)info.dir_count);
        printf("Original:      %s\n",  ob);
        printf("Compressed:    %s\n",  cb);
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
        printf("%lu files, %lu dirs, %s\n",
               (unsigned long)ctx.file_count, (unsigned long)ctx.dir_count, ob);
    }
    return 0;
}

/* ---- cmd_create ------------------------------------------------------- */

static int cmd_create(int argc, char **argv, bp_uint32 block_size, bp_uint8 method)
{
    FILE *fp; bp_io_t aio; bp_archive_t ar; int rc, i;
    if (argc < 2) { fprintf(stderr, "boltpack: create needs archive + files\n"); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    fp = fopen(argv[0], "w+b");
    if (!fp) { fprintf(stderr, "boltpack: cannot create: %s\n", argv[0]); return 1; }
    bp_io_from_file(&aio, fp);
    rc = bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    for (i = 1; i < argc; i++) {
        /* Strip device prefix for archive name */
        const char *arcname = ql_strip_device(argv[i]);
        /* Convert QDOS path separators '_' to '/' in archive name */
        char arcbuf[BP_MAX_PATH]; int j;
        strncpy(arcbuf, arcname, sizeof(arcbuf) - 1); arcbuf[sizeof(arcbuf)-1]='\0';
        /* Only convert '_' that are directory separators (those that are
         * in subdirectory positions -- we leave filename '_' alone).
         * Simple heuristic: keep only the portion from the last non-dir
         * level. In practice we don't convert embedded '_'; the user
         * controls naming by passing appropriate QDOS paths. */
        rc = add_path(&ar, argv[i], arcbuf);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding: %s\n", argv[i]);
            bp_archive_close(&ar); fclose(fp); return 1;
        }
    }
    rc = bp_archive_finalize(&ar); bp_archive_close(&ar); fclose(fp);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    if (g_verbosity < VERB_SILENT) printf("%d item%s archived\n", argc-1, argc-1==1?"":"s");
    return 0;
}

/* ---- cmd_add ---------------------------------------------------------- */

static int cmd_add(int argc, char **argv, bp_uint32 block_size, bp_uint8 method)
{
    FILE *fp; bp_io_t aio; bp_archive_t ar; int rc, i;
    if (argc < 2) { fprintf(stderr, "boltpack: add needs archive + files\n"); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    fp = fopen(argv[0], "r+b");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", argv[0]); return 1; }
    bp_io_from_file(&aio, fp);
    rc = bp_archive_open_append(&ar, &aio, block_size, method);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    for (i = 1; i < argc; i++) {
        const char *arcname = ql_strip_device(argv[i]);
        rc = add_path(&ar, argv[i], arcname);
        if (rc != BP_OK) { fprintf(stderr, "boltpack: error adding: %s\n", argv[i]);
                           bp_archive_close(&ar); fclose(fp); return 1; }
    }
    rc = bp_archive_finalize(&ar); bp_archive_close(&ar); fclose(fp);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    if (g_verbosity < VERB_SILENT) printf("%d item%s added\n", argc-1, argc-1==1?"":"s");
    return 0;
}

/* ---- cmd_extract ------------------------------------------------------ */

static int cmd_extract(const char *archive_name, const char *out_dir,
                       int force, int dry_run)
{
    FILE *fp; bp_io_t aio; extract_ctx_t ctx; int rc;
    fp = fopen(archive_name, "rb");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", archive_name); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    if (out_dir && !dry_run) mkdirs(out_dir);
    bp_io_from_file(&aio, fp);
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir; ctx.force = force; ctx.dry_run = dry_run;
    rc = bp_archive_extract_all(&aio, extract_open, extract_done, NULL, &ctx);
    fclose(fp);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    if (g_verbosity < VERB_SILENT)
        printf("%lu file%s, %lu dir%s\n",
               (unsigned long)ctx.file_count, ctx.file_count==1?"":"s",
               (unsigned long)ctx.dir_count,  ctx.dir_count==1?"":"s");
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
    if (rc != BP_OK && rc != BP_ERR_CHECKSUM)
        { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    if (g_verbosity < VERB_SILENT) {
        printf("\n%lu OK", (unsigned long)ctx.ok);
        if (ctx.fail) printf(", %lu FAILED", (unsigned long)ctx.fail);
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
    if (argc < 2) { fprintf(stderr, "boltpack: delete needs archive + names\n"); return 1; }
    del = (const char **)&argv[1]; del_count = argc - 1;
    sf = fopen(argv[0], "rb");
    if (!sf) { fprintf(stderr, "boltpack: cannot open: %s\n", argv[0]); return 1; }
    ql_join2(tmp, sizeof(tmp), argv[0], ".tmp");
    df = fopen(tmp, "w+b");
    if (!df) { fclose(sf); fprintf(stderr, "boltpack: cannot create temp\n"); return 1; }
    bp_io_from_file(&sio, sf); bp_io_from_file(&dio, df);
    rc = bp_archive_delete(&sio, &dio, del, (bp_uint32)del_count, &deleted);
    fclose(sf); fclose(df);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); remove(tmp); return 1; }
    if (!deleted) { fprintf(stderr, "boltpack: no matching entries\n"); remove(tmp); return 1; }
    if (remove(argv[0]) != 0 || rename(tmp, argv[0]) != 0)
        { fprintf(stderr, "boltpack: replace failed\n"); return 1; }
    if (g_verbosity < VERB_SILENT)
        printf("%lu entr%s deleted\n", (unsigned long)deleted, deleted==1?"y":"ies");
    return 0;
}

/* ---- main ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    char       command;
    bp_uint32  block_size = 0;
    bp_uint8   method     = BP_METHOD_BOLTLZ;
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
            if (++i >= argc) { fprintf(stderr, "boltpack: -b needs arg\n"); return 1; }
            block_size = parse_block_size(argv[i++]);
            if (!block_size) { fprintf(stderr, "boltpack: invalid block size\n"); return 1; }
        } else if (!strcmp(argv[i], "-0"))  { method = BP_METHOD_STORED; i++; }
        else if (!strcmp(argv[i], "-F"))  { method = BP_METHOD_FLUFF;  i++; }
        else if (!strcmp(argv[i], "-qq")) { g_verbosity = VERB_SILENT; i++; }
        else if (!strcmp(argv[i], "-q"))  { g_verbosity = VERB_QUIET;  i++; }
        else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) { fprintf(stderr, "boltpack: -o needs arg\n"); return 1; }
            out_dir = argv[i++];
        } else if (!strcmp(argv[i], "-f")) { force = 1; i++; }
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
