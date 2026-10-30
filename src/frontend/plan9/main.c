/*
 * main.c -- Plan 9 CLI frontend for BoltPack
 *
 * Target:   Plan 9 Fourth Edition / 9front.
 * Compiler: 8c/6c/5c (Plan 9 native C compiler or 9cc cross-compiler).
 *
 * This file uses the Plan 9 C dialect and the Plan 9 standard library.
 * It MUST be compiled with the Plan 9 toolchain, NOT with a POSIX or
 * ANSI C compiler.  The includes, types, and I/O functions below are
 * all Plan 9-specific and do not exist in standard POSIX or C99 headers.
 *
 * Build on a Plan 9 / 9front system:
 *   8c -FTVw -I /sys/include -I <src/core> \
 *       src/core/archiver.c src/core/compress.c             \
 *       src/core/bp_codepage.c                               \
 *       src/frontend/plan9/main.c                            \
 *       src/core/boltlz.c                           \
 *   8l -o /386/boltpack *.8
 *   (Use 6c/6l for AMD64, arm/arm64 as appropriate.)
 *
 * Metadata (BP_TYPE_META) payload for BP_OS_PLAN9 (Unix family):
 *   Bytes 0-1 : lower 16 bits of Dir.mode (rwxrwxrwx + Plan 9 extension bits)
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_PLAN9
#define BP_CREATOR_CPU BP_CPU_UNKNOWN

/* Plan 9 headers — u.h MUST come first */
#include <u.h>
#include <libc.h>

/* BoltPack core — included after Plan 9 headers */
#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/fluff.h"
#include "../../core/bp_codepage.h"
#include "p9_path.h"

/* Plan 9 uses UTF-8 natively; archive names are already UTF-8 */
#define P9_CP  BP_CP_UTF8

/* Metadata payload: 2 bytes (mode, Unix family) */
#define P9_META_PAYLOAD    2

/* ---- constants -------------------------------------------------------- */

#define P9_MAX_PATH   512

/* ---- verbosity -------------------------------------------------------- */

#define VERB_DEFAULT  0
#define VERB_QUIET    1
#define VERB_SILENT   2

static int g_verbosity = VERB_DEFAULT;

/* ======================================================================
 * bp_io_t adapters -- Plan 9 file descriptor based I/O.
 * ====================================================================== */

typedef struct {
    bp_io_t io;  /* MUST be first -- callers cast bp_io_t* to p9_io_t* */
    int     fd;
} p9_io_t;

static long p9_read(void *h, void *buf, bp_uint32 n)
{
    p9_io_t *c = (p9_io_t *)h;
    return read(c->fd, buf, (long)n);
}

static long p9_write(void *h, const void *buf, bp_uint32 n)
{
    p9_io_t *c = (p9_io_t *)h;
    return write(c->fd, (void *)buf, (long)n);
}

static int p9_seek(void *h, bp_off_t off, int org)
{
    p9_io_t *c = (p9_io_t *)h;
    vlong r = seek(c->fd, (vlong)off, org);   /* org: 0=start, 1=cur, 2=end */
    return (r < 0) ? -1 : 0;
}

static bp_off_t p9_tell(void *h)
{
    p9_io_t *c = (p9_io_t *)h;
    return (bp_off_t)seek(c->fd, 0LL, 1);
}

static void bp_io_from_p9(bp_io_t *io, int fd, p9_io_t *ctx)
{
    ctx->fd       = fd;
    ctx->io.read  = p9_read;
    ctx->io.write = p9_write;
    ctx->io.seek  = p9_seek;
    ctx->io.tell  = p9_tell;
    ctx->io.handle = ctx;   /* io.handle points to the enclosing p9_io_t */
    *io = ctx->io;          /* caller may use the bp_io_t copy directly  */
}

/* ======================================================================
 * Read-only memory buffer I/O  (for metadata supply).
 * ====================================================================== */

typedef struct {
    const uchar *buf;
    bp_uint32    size;
    bp_uint32    pos;
} rbuf_ctx_t;

static long rbuf_read(void *h, void *buf, bp_uint32 n)
{
    rbuf_ctx_t *c = (rbuf_ctx_t *)h;
    if (c->pos >= c->size) return 0;
    if (n > c->size - c->pos) n = c->size - c->pos;
    memmove(buf, c->buf + c->pos, n);
    c->pos += n;
    return (long)n;
}
static long   rbuf_write(void *h, const void *b, bp_uint32 n){ (void)h;(void)b;(void)n; return -1L; }
static int    rbuf_seek(void *h, bp_off_t o, int org)
{
    rbuf_ctx_t *c = (rbuf_ctx_t *)h;
    long np;
    if (org == 0)      np = (long)o;
    else if (org == 1) np = (long)c->pos + (long)o;
    else if (org == 2) np = (long)c->size + (long)o;
    else return -1;
    if (np < 0 || (bp_uint32)np > c->size) return -1;
    c->pos = (bp_uint32)np;
    return 0;
}
static bp_off_t rbuf_tell(void *h){ return (bp_off_t)((rbuf_ctx_t *)h)->pos; }

static void bp_io_from_rbuf(bp_io_t *io, rbuf_ctx_t *ctx)
{
    io->read = rbuf_read; io->write = rbuf_write;
    io->seek = rbuf_seek; io->tell  = rbuf_tell;
    io->handle = ctx;
}

/* ======================================================================
 * Write-only memory buffer I/O  (for metadata capture on extraction).
 * ====================================================================== */

typedef struct {
    uchar    *buf;
    bp_uint32 cap;
    bp_uint32 len;
    bp_uint32 pos;
} wbuf_ctx_t;

static long wbuf_read(void *h, void *b, bp_uint32 n){ (void)h;(void)b;(void)n; return -1L; }
static long wbuf_write(void *h, const void *data, bp_uint32 n)
{
    wbuf_ctx_t *c = (wbuf_ctx_t *)h;
    if (c->pos + n > c->cap) n = c->cap - c->pos;
    if (!n) return 0;
    memmove(c->buf + c->pos, data, n);
    c->pos += n;
    if (c->pos > c->len) c->len = c->pos;
    return (long)n;
}
static int wbuf_seek(void *h, bp_off_t o, int org)
{
    wbuf_ctx_t *c = (wbuf_ctx_t *)h;
    long np;
    if (org == 0)      np = (long)o;
    else if (org == 1) np = (long)c->pos + (long)o;
    else if (org == 2) np = (long)c->len + (long)o;
    else return -1;
    if (np < 0 || (bp_uint32)np > c->cap) return -1;
    c->pos = (bp_uint32)np;
    return 0;
}
static bp_off_t wbuf_tell(void *h){ return (bp_off_t)((wbuf_ctx_t *)h)->pos; }

typedef struct {
    bp_io_t    io;      /* MUST be first */
    wbuf_ctx_t wctx;
    uchar      buf[4];  /* 2 mode bytes + padding */
} meta_extract_t;

/* ---- error messages --------------------------------------------------- */

static const char *bp_strerror(int code)
{
    switch (code) {
    case BP_OK:                    return "success";
    case BP_ERR_IO:                return "I/O error";
    case BP_ERR_NOMEM:             return "out of memory";
    case BP_ERR_MAGIC:             return "not a BoltPack archive";
    case BP_ERR_VERSION:           return "unsupported archive version";
    case BP_ERR_CHECKSUM:          return "checksum mismatch";
    case BP_ERR_CORRUPT:           return "archive is corrupt";
    case BP_ERR_TRUNCATED:         return "unexpected end of file";
    case BP_ERR_OVERFLOW:          return "size overflow";
    case BP_ERR_NAME_TOO_LONG:     return "name too long";
    case BP_ERR_UNSUPPORTED_METHOD:return "unsupported compression method";
    case BP_ERR_COMPRESS:          return "compression failed";
    case BP_ERR_DECOMPRESS:        return "decompression failed";
    case BP_ERR_ABORTED:           return "operation aborted";
    case BP_ERR_NOT_FOUND:         return "entry not found";
    case BP_ERR_INVALID_ARG:       return "invalid argument";
    default:                       return "unknown error";
    }
}

/* ---- formatting helpers ----------------------------------------------- */

static void format_size(bp_uint32 bytes, char *buf, int sz)
{
    if (bytes < 1024UL)
        snprint(buf, sz, "%lud bytes", (ulong)bytes);
    else if (bytes < 1048576UL)
        snprint(buf, sz, "%.1f KB", (double)bytes / 1024.0);
    else
        snprint(buf, sz, "%.2f MB", (double)bytes / 1048576.0);
}

static void format_size64(bp_uint64 b, char *buf, int sz)
{
    if (b.hi == 0) { format_size(b.lo, buf, sz); return; }
    { double d = (double)b.lo + (double)b.hi * 4294967296.0;
      if (d < 1048576.0)     snprint(buf, sz, "%.1f KB", d / 1024.0);
      else                   snprint(buf, sz, "%.2f MB", d / 1048576.0); }
}

/* ---- progress (unused -- Plan 9 output is synchronous) --------------- */

static int progress_cb(const bp_progress_t *p, void *ud)
{
    (void)p; (void)ud; return 0;
}

/* ---- path utilities --------------------------------------------------- */

static int ends_with_sep(const char *s)
{
    int n = strlen(s);
    return n > 0 && s[n - 1] == '/';
}

static int output_path_fits(const char *dir, const char *name)
{
    size_t length = strlen(name);
    if (dir != nil) {
        size_t dir_length = strlen(dir);
        length += dir_length;
        if (!ends_with_sep(dir)) length++;
    }
    return length < P9_MAX_PATH;
}

static void path_join(char *buf, int sz, const char *dir, const char *name)
{
    if (ends_with_sep(dir)) snprint(buf, sz, "%s%s", dir, name);
    else                    snprint(buf, sz, "%s/%s", dir, name);
}

static const char *clean_path(const char *p)
{
    while (*p == '/') p++;              /* strip leading slashes */
    while (p[0] == '.' && p[1] == '/') p += 2;
    return p;
}

/* ---- mkdir -p --------------------------------------------------------- */

static int p9_mkdir(const char *path)
{
    int fd = create(path, OREAD, DMDIR | 0777);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

static int mkdirs(const char *path)
{
    char tmp[P9_MAX_PATH]; char *p;
    if (strlen(path) >= P9_MAX_PATH) return -1;
    strcpy(tmp, path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            char sv = *p; *p = '\0';
            if (p9_mkdir(tmp) < 0) {
                Dir *d = dirstat(tmp);
                if (!d) { *p = sv; return -1; }
                free(d);
            }
            *p = sv;
        }
    }
    if (p9_mkdir(tmp) < 0) {
        Dir *d = dirstat(tmp);
        if (!d) return -1;
        free(d);
    }
    return 0;
}

static int mkdirs_for_file(const char *path)
{
    char tmp[P9_MAX_PATH]; char *sl;
    if (strlen(path) >= P9_MAX_PATH) return -1;
    strcpy(tmp, path);
    sl = strrchr(tmp, '/');
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

/* ---- metadata helpers ------------------------------------------------- */

static void encode_meta(uchar *buf, u32int mode)
{
    u16int m = (u16int)(mode & 0xFFFF);
    buf[0] = (uchar)(m & 0xFF);
    buf[1] = (uchar)((m >> 8) & 0xFF);
}

static u16int decode_meta(const uchar *buf)
{
    return (u16int)(buf[0] | ((u16int)buf[1] << 8));
}

static int add_meta_entry(bp_archive_t *ar, const char *arcname, u32int mode)
{
    uchar      meta_buf[P9_META_PAYLOAD];
    bp_io_t    meta_io;
    rbuf_ctx_t rctx;

    encode_meta(meta_buf, mode);
    rctx.buf  = meta_buf;
    rctx.size = P9_META_PAYLOAD;
    rctx.pos  = 0;
    bp_io_from_rbuf(&meta_io, &rctx);

    return bp_archive_add_entry_ex(ar, arcname, BP_TYPE_META, BP_OS_PLAN9,
                                &meta_io,
                                bp_u64_from_u32(P9_META_PAYLOAD),
                                bp_u64_from_u32(0), 0);
}

/* ---- restore mode via dirwstat --------------------------------------- */

static int p9_apply_mode(const char *path, u16int mode)
{
    Dir nd;
    Dir *current;
    u32int restored_mode = (u32int)(mode & 0x01FF);

    /* Plan 9 keeps the directory type in Dir.mode.  A wstat that supplies
     * only the Unix permission bits can be rejected for directories because
     * it appears to clear DMDIR.  Preserve the type of the object on disk. */
    current = dirstat(path);
    if (!current) return -1;
    if (current->mode & DMDIR) restored_mode |= DMDIR;
    free(current);

    nulldir(&nd);
    nd.mode = restored_mode;
    return dirwstat(path, &nd);
}

/* ---- recursive add ---------------------------------------------------- */

static int add_path(bp_archive_t *ar, const char *path, const char *arcname);

static int add_directory_contents(bp_archive_t *ar, const char *dir_path,
                                  const char *arc_prefix)
{
    int    fd;
    Dir   *dirs;
    long   nd;
    int    i, rc;
    char   child_path[P9_MAX_PATH];
    char   child_arc[P9_MAX_PATH];

    fd = open(dir_path, OREAD);
    if (fd < 0) {
        char eb[256]; rerrstr(eb, sizeof eb);
        fprint(2, "boltpack: cannot open dir %s: %s\n", dir_path, eb);
        return BP_ERR_IO;
    }
    nd = dirreadall(fd, &dirs);
    close(fd);
    if (nd < 0) return BP_ERR_IO;

    rc = BP_OK;
    for (i = 0; i < nd && rc == BP_OK; i++) {
        if (!strcmp(dirs[i].name, ".") || !strcmp(dirs[i].name, ".."))
            continue;
        path_join(child_path, P9_MAX_PATH, dir_path,  dirs[i].name);
        path_join(child_arc,  P9_MAX_PATH, arc_prefix, dirs[i].name);
        rc = add_path(ar, child_path, child_arc);
    }
    free(dirs);
    return rc;
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    Dir *d;
    int  rc;

    d = dirstat(path);
    if (!d) {
        fprint(2, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (d->mode & DMDIR) {
        /* Directory */
        char dir_arc[P9_MAX_PATH];
        if (!ends_with_sep(arcname)) snprint(dir_arc, P9_MAX_PATH, "%s/", arcname);
        else                         strcpy(dir_arc, arcname);

        if (g_verbosity == VERB_DEFAULT) print("   creating: %s\n", dir_arc);

        rc = bp_archive_add_entry(ar, dir_arc, BP_TYPE_DIR, BP_OS_PLAN9,
                                  nil, bp_u64_from_u32(0),
                                  bp_u64_from_u32((bp_uint32)d->mtime));
        if (rc == BP_OK) rc = add_meta_entry(ar, dir_arc, d->mode);
        free(d);
        if (rc != BP_OK) return rc;
        return add_directory_contents(ar, path, dir_arc);
    }

    /* Regular file */
    {
        bp_uint64 fsize = bp_u64_from_ulong((unsigned long)d->length);
        bp_uint32 mtime = (bp_uint32)d->mtime;
        u32int    mode  = d->mode;
        p9_io_t   fctx;
        int       fd;

        free(d);

        fd = open(path, OREAD);
        if (fd < 0) {
            fprint(2, "boltpack: cannot open: %s\n", path);
            return BP_ERR_IO;
        }
        bp_io_from_p9(&fctx.io, fd, &fctx);

        if (g_verbosity == VERB_DEFAULT) { print("compressing: %s", arcname); }

        rc = bp_archive_add_entry(ar, arcname, BP_TYPE_DATA, BP_OS_PLAN9,
                                  &fctx.io, fsize, bp_u64_from_u32(mtime));
        close(fd);
        if (rc != BP_OK) return rc;

        if (g_verbosity == VERB_DEFAULT) {
            bp_entry_t *e = &ar->entry_list[ar->entries_written - 1];
            bp_uint32 comp = bp_u64_to_u32(e->compressed_size);
            bp_uint32 fsz  = bp_u64_to_u32(fsize);
            char sb[32];
            if (comp == 0 || e->method == BP_METHOD_STORED || comp >= fsz) {
                format_size(fsz, sb, sizeof(sb)); print("  %s (stored)\n", sb);
            } else {
                format_size(comp, sb, sizeof(sb));
                print("  %s (%.0f%%)\n", sb, (double)comp / (double)fsz * 100.0);
            }
        }

        return add_meta_entry(ar, arcname, mode);
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
    char ob[32], cb[32];

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

    format_size(bp_u64_to_u32(entry->original_size),   ob, sizeof(ob));
    format_size(bp_u64_to_u32(entry->compressed_size),  cb, sizeof(cb));
    print("%10s %10s  %s  %s\n", ob, cb, type, entry->name);
    return 0;
}

/* ---- extract callbacks ------------------------------------------------ */

typedef struct {
    char       *name;
    u16int      mode;
    u32int      mtime;
    int         mode_valid;
    int         mtime_valid;
} p9_deferred_dir_t;

typedef struct {
    const char *out_dir;
    int         force;
    int         dry_run;
    bp_uint32   file_count;
    bp_uint32   dir_count;
    int         failed;
    p9_deferred_dir_t *deferred_dirs;
    bp_uint32   deferred_count;
    bp_uint32   deferred_capacity;
    unsigned long deferred_name_bytes;
} extract_ctx_t;

#define P9_DEFERRED_MAX_ENTRIES 65535U
#define P9_DEFERRED_MAX_NAME_BYTES (16UL * 1024UL * 1024UL)

static int defer_directory(extract_ctx_t *ctx, const bp_entry_t *entry)
{
    p9_deferred_dir_t *item;
    bp_uint32 capacity;
    size_t length = strlen(entry->name);
    if (ctx->deferred_count >= P9_DEFERRED_MAX_ENTRIES ||
        length + 1U > P9_DEFERRED_MAX_NAME_BYTES - ctx->deferred_name_bytes)
        return BP_ERR_PARTIAL;
    if (ctx->deferred_count == ctx->deferred_capacity) {
        capacity = ctx->deferred_capacity ? ctx->deferred_capacity * 2U : 16U;
        if (capacity > P9_DEFERRED_MAX_ENTRIES)
            capacity = P9_DEFERRED_MAX_ENTRIES;
#ifdef __PLAN9__
        item = (p9_deferred_dir_t *)mallocz(capacity * sizeof(*item), 1);
#else
        item = (p9_deferred_dir_t *)calloc(capacity, sizeof(*item));
#endif
        if (item == nil) return BP_ERR_NOMEM;
        if (ctx->deferred_count != 0)
            memcpy(item, ctx->deferred_dirs,
                   ctx->deferred_count * sizeof(*item));
        free(ctx->deferred_dirs);
        ctx->deferred_dirs = item;
        ctx->deferred_capacity = capacity;
    }
    item = &ctx->deferred_dirs[ctx->deferred_count];
    memset(item, 0, sizeof(*item));
    item->name = (char *)malloc(length + 1U);
    if (item->name == nil) return BP_ERR_NOMEM;
    memcpy(item->name, entry->name, length + 1U);
    if (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) {
        if (entry->mtime.hi != 0) {
            free(item->name);
            item->name = nil;
            return BP_ERR_PARTIAL;
        }
        item->mtime = entry->mtime.lo;
        item->mtime_valid = 1;
    }
    ctx->deferred_name_bytes += (unsigned long)(length + 1U);
    ctx->deferred_count++;
    return BP_OK;
}

static int defer_directory_mode(extract_ctx_t *ctx, const char *name,
                                u16int mode)
{
    bp_uint32 i;
    for (i = ctx->deferred_count; i > 0; i--) {
        p9_deferred_dir_t *item = &ctx->deferred_dirs[i - 1U];
        if (strcmp(item->name, name) == 0) {
            item->mode = mode;
            item->mode_valid = 1;
            return BP_OK;
        }
    }
    return BP_ERR_PARTIAL;
}

static int apply_deferred_directories(extract_ctx_t *ctx)
{
    bp_uint32 i;
    int rc = BP_OK;
    for (i = ctx->deferred_count; i > 0; i--) {
        p9_deferred_dir_t *item = &ctx->deferred_dirs[i - 1U];
        char name[P9_MAX_PATH];
        char path[P9_MAX_PATH];
        size_t length = strlen(item->name);
        Dir nd;
        if (length >= sizeof(name)) { rc = BP_ERR_PARTIAL; continue; }
        memcpy(name, item->name, length + 1U);
        if (length > 0 && name[length - 1U] == '/') name[length - 1U] = '\0';
        if (ctx->out_dir) path_join(path, P9_MAX_PATH, ctx->out_dir, name);
        else snprint(path, sizeof(path), "%s", name);
        /* Set the timestamp while the directory is still writable, then
         * restore its final permissions as the last operation. */
        if (item->mtime_valid) {
            nulldir(&nd);
            nd.mtime = item->mtime;
            if (dirwstat(path, &nd) < 0) rc = BP_ERR_PARTIAL;
        }
        if (item->mode_valid && p9_apply_mode(path, item->mode) < 0)
            rc = BP_ERR_PARTIAL;
    }
    return rc;
}

static void free_deferred_directories(extract_ctx_t *ctx)
{
    bp_uint32 i;
    for (i = 0; i < ctx->deferred_count; i++) free(ctx->deferred_dirs[i].name);
    free(ctx->deferred_dirs);
    ctx->deferred_dirs = nil;
    ctx->deferred_count = ctx->deferred_capacity = 0;
}

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t  *ctx = (extract_ctx_t *)user_data;
    char            path[P9_MAX_PATH];
    meta_extract_t *mex;
    p9_io_t        *fctx;
    int             fd;

    if (!bp_name_is_safe(entry->name)) return nil;
    if (!output_path_fits(ctx->out_dir, entry->name))
        return nil;

    if (ctx->out_dir) path_join(path, P9_MAX_PATH, ctx->out_dir, entry->name);
    else              snprint(path, sizeof(path), "%s", entry->name);

    if (entry->entry_type == BP_TYPE_DIR) {
        Dir *existing;
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT) print("   creating: %s\n", entry->name);
        if (ctx->dry_run) return bp_archive_dry_run_stream();
        if (mkdirs(path) < 0) {
            existing = dirstat(path);
            if (!existing || !(existing->mode & DMDIR)) {
                if (existing) free(existing);
                ctx->failed = 1;
                return nil;
            }
            free(existing);
        }
        fd = open(path, OREAD);
        if (fd < 0) { ctx->failed = 1; return nil; }
        fctx = (p9_io_t *)malloc(sizeof(p9_io_t));
        if (!fctx) { close(fd); ctx->failed = 1; return nil; }
        bp_io_from_p9(&fctx->io, fd, fctx);
        return &fctx->io;
    }

    if (entry->entry_type == BP_TYPE_META) {
        bp_uint32 meta_sz;
        if (entry->platform != BP_OS_PLAN9) return nil;
        meta_sz = bp_u64_to_u32(entry->original_size);
        if (meta_sz == 0 || meta_sz > (bp_uint32)(sizeof(mex->buf))) return nil;
        mex = (meta_extract_t *)malloc(sizeof(meta_extract_t));
        if (!mex) return nil;
        mex->wctx.buf = mex->buf;
        mex->wctx.cap = sizeof(mex->buf);
        mex->wctx.len = 0;
        mex->wctx.pos = 0;
        mex->io.read   = wbuf_read;
        mex->io.write  = wbuf_write;
        mex->io.seek   = wbuf_seek;
        mex->io.tell   = wbuf_tell;
        mex->io.handle = &mex->wctx;
        return &mex->io;
    }

    if (entry->entry_type == BP_TYPE_RSRC || entry->entry_type == BP_TYPE_SYMLINK) {
        if (g_verbosity == VERB_DEFAULT)
            print("   skipping: %s [not supported on Plan 9]\n", entry->name);
        return nil;
    }

    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT) print(" extracting: %s\n", entry->name);
    if (ctx->dry_run) return bp_archive_dry_run_stream();

    if (!ctx->force) {
        Dir *d2 = dirstat(path);
        if (d2) {
            free(d2);
            fprint(2, "boltpack: exists (use -f): %s\n", path);
            return nil;
        }
    }

    if (mkdirs_for_file(path) < 0) {
        fprint(2, "boltpack: cannot create parent directory for: %s\n", path);
        return nil;
    }
    fd = create(path, OWRITE, 0666);
    if (fd < 0) { fprint(2, "boltpack: cannot create: %s\n", path); return nil; }
    fctx = (p9_io_t *)malloc(sizeof(p9_io_t));
    if (!fctx) { close(fd); return nil; }
    bp_io_from_p9(&fctx->io, fd, fctx);
    return &fctx->io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;

    if (entry->entry_type == BP_TYPE_META) {
        meta_extract_t *mex = (meta_extract_t *)out;
        int rc = status;
        if (status == BP_OK && entry->platform == BP_OS_PLAN9 &&
            !ctx->dry_run && mex->wctx.len >= P9_META_PAYLOAD) {
            char path[P9_MAX_PATH];
            /* Strip trailing slash added to directory names */
            char nm[P9_MAX_PATH]; int nl;
            strcpy(nm, entry->name);
            nl = strlen(nm);
            if (nl > 1 && nm[nl-1] == '/') {
                if (defer_directory_mode(ctx, entry->name,
                                         decode_meta(mex->buf)) != BP_OK)
                    rc = BP_ERR_PARTIAL;
            } else {
                if (ctx->out_dir) path_join(path, P9_MAX_PATH, ctx->out_dir, nm);
                else              strcpy(path, nm);
                if (p9_apply_mode(path, decode_meta(mex->buf)) < 0)
                    rc = BP_ERR_PARTIAL;
            }
        }
        free(mex);
        return rc;
    }

    {
        p9_io_t *fctx = (p9_io_t *)out;
        char     path[P9_MAX_PATH];
        int rc = status;
        if (close(fctx->fd) < 0 && rc == BP_OK) rc = BP_ERR_PARTIAL;
        if (status == BP_OK && !ctx->dry_run &&
            entry->entry_type == BP_TYPE_DIR) {
            int deferred_rc = defer_directory(ctx, entry);
            if (deferred_rc != BP_OK) rc = deferred_rc;
        } else if (status == BP_OK && !ctx->dry_run &&
            entry->entry_type != BP_TYPE_DIR &&
            (entry->flags & BP_ENTRY_FLAG_MTIME_VALID)) {
            Dir nd;
            if (entry->mtime.hi != 0) {
                rc = BP_ERR_PARTIAL;
            } else {
                nulldir(&nd);
                nd.mtime = (u32int)entry->mtime.lo;
                if (ctx->out_dir) path_join(path, P9_MAX_PATH, ctx->out_dir, entry->name);
                else              strcpy(path, entry->name);
                if (dirwstat(path, &nd) < 0) rc = BP_ERR_PARTIAL;
            }
        }
        free(fctx);
        return rc;
    }
}

/* ---- test callback ---------------------------------------------------- */

typedef struct { bp_uint32 ok; bp_uint32 fail; } test_ctx_t;

static void test_cb(const bp_entry_t *entry, int result, void *user_data)
{
    test_ctx_t *ctx = (test_ctx_t *)user_data;
    if (g_verbosity < VERB_SILENT)
        print("  testing: %s %s\n", entry->name,
              result == BP_OK ? "OK" : bp_strerror(result));
    if (result == BP_OK) ctx->ok++; else ctx->fail++;
}

/* ---- usage ------------------------------------------------------------ */

static void usage(void)
{
    fprint(2,
        "BoltPack v%d.%d (Plan 9)\n"
        "%s\n\n"
        "usage: boltpack <cmd> [options] archive.blt [files...]\n\n"
        "  c  create   a  add     x  extract\n"
        "  l  list     t  test    i  info    d  delete\n"
        "\noptions:\n"
        "  -b SIZE   block size: 8k 16k 32k 64k 256k\n"
        "  -0        store only  -F fluff  -q quiet  -qq silent\n"
        "  --interchange-profile-1  create/add mini-compatible archive\n"
        "  -o PATH   output dir for extraction\n"
        "  -f        force overwrite  -n dry run\n",
        BP_VERSION_MAJOR, BP_VERSION_MINOR, AUTHOR_MESSAGE);
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
    case BP_OS_NEXTSTEP:  return "NeXTSTEP";
    case BP_OS_QNX:       return "QNX";
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
    case BP_CPU_PPC32:   return "PowerPC 32";
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
    case BP_CPU_8086:    return "8086";
    case BP_CPU_E2K:     return "Elbrus e2k";
    case BP_CPU_ZARCH:   return "z/Architecture";
    case BP_CPU_SPARC64: return "SPARC64";
    case BP_CPU_IA64:    return "Itanium";
    default:             return "Unknown";
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
        pos += sprint(buf + pos, "%s (%lud)", names[i], (ulong)counts[i]);
        first = 0;
    }
    if (first) strcpy(buf, "none");
    return buf;
}

static int cmd_info(const char *archive_name)
{
    p9_io_t ctx; int fd; bp_archive_info_t info; int rc;
    fd = open(archive_name, OREAD);
    if (fd < 0) { fprint(2, "boltpack: cannot open: %s\n", archive_name); return 1; }
    bp_io_from_p9(&ctx.io, fd, &ctx);
    rc = bp_archive_info(&ctx.io, &info);
    close(fd);
    if (rc != BP_OK) { fprint(2, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    {
        char ob[32], cb[32], ab[32];
        format_size64(info.total_original,   ob, sizeof(ob));
        format_size64(info.total_compressed, cb, sizeof(cb));
        format_size64(info.archive_size,     ab, sizeof(ab));
        print("Archive:       %s\n",  archive_name);
        print("Version:       %ud\n", (unsigned)info.version);
        print("Creator OS:    %s\n",  os_name(info.creator_os));
        print("Creator CPU:   %s\n",  cpu_name(info.creator_cpu));
        print("Method:        %s\n",  format_methods(info.method_counts));
        print("Archive size:  %s\n",  ab);
        print("Entries:       %lud files, %lud dirs\n",
              (ulong)(info.entry_count - info.dir_count),
              (ulong)info.dir_count);
        print("Original:      %s\n",  ob);
        print("Compressed:    %s\n",  cb);
    }
    return 0;
}

/* ---- cmd_list --------------------------------------------------------- */

static int cmd_list(const char *archive_name)
{
    p9_io_t ctx; int fd; list_ctx_t lctx; int rc;
    fd = open(archive_name, OREAD);
    if (fd < 0) { fprint(2, "boltpack: cannot open: %s\n", archive_name); return 1; }
    bp_io_from_p9(&ctx.io, fd, &ctx);
    memset(&lctx, 0, sizeof(lctx));
    print("%10s %10s  %s  %s\n", "Size", "Compressed", "Type", "Name");
    rc = bp_archive_list(&ctx.io, list_cb, &lctx);
    close(fd);
    if (rc != BP_OK) { fprint(2, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    {
        char ob[32], cb[32];
        format_size64(lctx.total_orig, ob, sizeof(ob));
        format_size64(lctx.total_comp, cb, sizeof(cb));
        print("%lud files, %lud dirs, %s",
              (ulong)lctx.file_count, (ulong)lctx.dir_count, ob);
        if (!bp_u64_is_zero(lctx.total_orig) && !bp_u64_is_zero(lctx.total_comp)) {
            double od = (double)lctx.total_orig.lo
                      + (double)lctx.total_orig.hi * 4294967296.0;
            double cd = (double)lctx.total_comp.lo
                      + (double)lctx.total_comp.hi * 4294967296.0;
            print(", %.0f%% (%s)", od > 0.0 ? cd / od * 100.0 : 0.0, cb);
        }
        print("\n");
    }
    return 0;
}

/* ---- cmd_create ------------------------------------------------------- */

static int cmd_create(int argc, char **argv, bp_uint32 block_size,
                      bp_uint8 method, int interchange_profile)
{
    p9_io_t arc_ctx; int fd; bp_archive_t ar; int rc, i;
    if (argc < 2) { fprint(2, "boltpack: create needs archive and files\n"); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprint(2, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    fd = create(argv[0], ORDWR, 0666);
    if (fd < 0) { fprint(2, "boltpack: cannot create: %s\n", argv[0]); return 1; }
    bp_io_from_p9(&arc_ctx.io, fd, &arc_ctx);
    rc = interchange_profile
        ? bp_archive_create_profile(&ar, &arc_ctx.io, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &arc_ctx.io, block_size, method);
    if (rc != BP_OK) { fprint(2, "boltpack: %s\n", bp_strerror(rc)); close(fd); return 1; }
    if (interchange_profile && method == BP_METHOD_STORED)
        ar.method = BP_METHOD_STORED;
    for (i = 1; i < argc; i++) {
        char buf[P9_MAX_PATH]; strcpy(buf, clean_path(argv[i]));
        rc = add_path(&ar, argv[i], buf);
        if (rc != BP_OK) {
            fprint(2, "boltpack: error adding %s: %s\n", argv[i], bp_strerror(rc));
            bp_archive_close(&ar); close(fd); return 1;
        }
    }
    rc = bp_archive_finalize(&ar);
    if (rc != BP_OK) {
        bp_archive_close(&ar);
        close(fd);
        fprint(2, "boltpack: finalize: %s\n", bp_strerror(rc));
        return 1;
    }
    if (g_verbosity < VERB_SILENT) {
        bp_uint32 j, fc = 0, dc = 0; bp_uint64 to = {0,0}, tc = {0,0};
        char nb[32];
        for (j = 0; j < ar.entries_written; j++) {
            if (ar.entry_list[j].entry_type == BP_TYPE_DIR) dc++;
            else if (ar.entry_list[j].entry_type == BP_TYPE_DATA ||
                     ar.entry_list[j].entry_type == BP_TYPE_SYMLINK) fc++;
            else continue;
            to = bp_u64_add(to, ar.entry_list[j].original_size);
            tc = bp_u64_add(tc, ar.entry_list[j].compressed_size);
        }
        format_size64(to, nb, sizeof(nb));
        print("%lud file%s, %lud dir%s, %s\n",
              (ulong)fc, fc == 1 ? "" : "s", (ulong)dc, dc == 1 ? "" : "s", nb);
    }
    bp_archive_close(&ar);
    close(fd);
    return 0;
}

/* ---- cmd_add ---------------------------------------------------------- */

static int cmd_add(int argc, char **argv, bp_uint32 block_size, bp_uint8 method,
                   int interchange_profile)
{
    p9_io_t arc_ctx; int fd; bp_archive_t ar; int rc, i;
    if (argc < 2) { fprint(2, "boltpack: add needs archive and files\n"); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprint(2, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    fd = open(argv[0], ORDWR);
    if (fd < 0) { fprint(2, "boltpack: cannot open: %s\n", argv[0]); return 1; }
    bp_io_from_p9(&arc_ctx.io, fd, &arc_ctx);
    rc = bp_archive_open_append(&ar, &arc_ctx.io, block_size, method);
    if (rc != BP_OK) { fprint(2, "boltpack: %s\n", bp_strerror(rc)); close(fd); return 1; }
    if (interchange_profile) {
        rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
        if (rc != BP_OK) {
            fprint(2, "boltpack: archive does not meet Interchange Profile 1 limits: %s\n",
                   bp_strerror(rc));
            bp_archive_close(&ar); close(fd); return 1;
        }
        if (method == BP_METHOD_STORED) ar.method = BP_METHOD_STORED;
    }
    for (i = 1; i < argc; i++) {
        rc = add_path(&ar, argv[i], clean_path(argv[i]));
        if (rc != BP_OK) {
            fprint(2, "boltpack: error adding %s\n", argv[i]);
            bp_archive_close(&ar); close(fd); return 1;
        }
    }
    rc = bp_archive_finalize(&ar); bp_archive_close(&ar); close(fd);
    if (rc != BP_OK) { fprint(2, "boltpack: finalize: %s\n", bp_strerror(rc)); return 1; }
    if (g_verbosity < VERB_SILENT)
        print("%d file%s added\n", argc-1, (argc-1)==1 ? "" : "s");
    return 0;
}

typedef struct {
    p9_path_index_t index;
    const char *out_dir;
    int error;
} p9_extract_preflight_t;

/* Validate every path Plan 9 will materialize before creating the output root.
 * Resource forks, metadata, and symlinks are skipped by this frontend. */
static int extract_preflight_entry(const bp_entry_t *entry, void *user_data)
{
    p9_extract_preflight_t *check = (p9_extract_preflight_t *)user_data;
    int is_directory;
    int rc;

    if (entry->entry_type == BP_TYPE_META ||
        entry->entry_type == BP_TYPE_RSRC ||
        entry->entry_type == BP_TYPE_SYMLINK)
        return 0;

    if (!bp_name_is_safe(entry->name)) {
        check->error = BP_ERR_PARTIAL;
        return 1;
    }
    if (!output_path_fits(check->out_dir, entry->name)) {
        check->error = BP_ERR_NAME_TOO_LONG;
        return 1;
    }

    is_directory = entry->entry_type == BP_TYPE_DIR;
    rc = p9_path_index_add(&check->index, entry->name, is_directory);
    if (rc != BP_OK) {
        check->error = rc;
        return 1;
    }
    return 0;
}

/* ---- cmd_extract ------------------------------------------------------ */

static int cmd_extract(const char *archive_name, const char *out_dir,
                       int force, int dry_run)
{
    p9_io_t arc_ctx; int fd; extract_ctx_t ctx; int rc;
    p9_extract_preflight_t preflight;
    fd = open(archive_name, OREAD);
    if (fd < 0) { fprint(2, "boltpack: cannot open: %s\n", archive_name); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprint(2, "boltpack: %s\n", bp_strerror(rc)); close(fd); return 1; }
    bp_io_from_p9(&arc_ctx.io, fd, &arc_ctx);
    memset(&preflight, 0, sizeof(preflight));
    preflight.out_dir = out_dir;
    rc = p9_path_index_init(&preflight.index);
    if (rc == BP_OK) rc = bp_archive_list(&arc_ctx.io, extract_preflight_entry,
                                          &preflight);
    if (rc != BP_OK || preflight.error != 0) {
        if (preflight.error != 0) rc = preflight.error;
        p9_path_index_free(&preflight.index);
        close(fd);
        fprint(2, "boltpack: extraction preflight: %s\n", bp_strerror(rc));
        return 1;
    }
    p9_path_index_free(&preflight.index);
    /* Plan 9 exposes pathname-based create/wstat operations here, but no
     * held-parent traversal or safe staged publication. Refuse real
     * extraction until an equivalent race-safe implementation exists. */
    if (!dry_run) {
        fprint(2,
               "boltpack: extraction is disabled because this platform cannot anchor output paths safely\n");
        close(fd);
        return 1;
    }
    if (out_dir && !dry_run && mkdirs(out_dir) < 0) {
        close(fd);
        fprint(2, "boltpack: cannot create output directory: %s\n", out_dir);
        return 1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir; ctx.force = force; ctx.dry_run = dry_run;
    rc = bp_archive_extract_all_ex(&arc_ctx.io, extract_open, extract_done,
                                   nil, &ctx);
    if (rc == BP_OK || rc == BP_ERR_PARTIAL) {
        if (apply_deferred_directories(&ctx) != BP_OK && rc == BP_OK)
            rc = BP_ERR_PARTIAL;
    }
    if (rc == BP_OK && ctx.failed) rc = BP_ERR_PARTIAL;
    free_deferred_directories(&ctx);
    close(fd);
    if (rc != BP_OK) { fprint(2, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    if (g_verbosity < VERB_SILENT)
        print("%lud file%s, %lud dir%s\n",
              (ulong)ctx.file_count, ctx.file_count==1?"":"s",
              (ulong)ctx.dir_count,  ctx.dir_count==1?"":"s");
    return 0;
}

/* ---- cmd_test --------------------------------------------------------- */

static int cmd_test(const char *archive_name)
{
    p9_io_t arc_ctx; int fd; test_ctx_t ctx; int rc;
    fd = open(archive_name, OREAD);
    if (fd < 0) { fprint(2, "boltpack: cannot open: %s\n", archive_name); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprint(2, "boltpack: %s\n", bp_strerror(rc)); close(fd); return 1; }
    bp_io_from_p9(&arc_ctx.io, fd, &arc_ctx);
    memset(&ctx, 0, sizeof(ctx));
    rc = bp_archive_test(&arc_ctx.io, test_cb, &ctx);
    close(fd);
    if (rc != BP_OK && rc != BP_ERR_CHECKSUM)
        { fprint(2, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    if (g_verbosity < VERB_SILENT) {
        print("\n%lud OK", (ulong)ctx.ok);
        if (ctx.fail) print(", %lud FAILED", (ulong)ctx.fail);
        print("\n");
    }
    return ctx.fail > 0 ? 1 : 0;
}

/* ---- cmd_delete ------------------------------------------------------- */

static int cmd_delete(int argc, char **argv)
{
    const char **del; int del_count;
    int sf, df; p9_io_t src_ctx, dst_ctx;
    char tmp[P9_MAX_PATH]; bp_uint32 deleted = 0; int rc;

    if (argc < 2) { fprint(2, "boltpack: delete needs archive and entries\n"); return 1; }
    del = (const char **)&argv[1]; del_count = argc - 1;

    sf = open(argv[0], OREAD);
    if (sf < 0) { fprint(2, "boltpack: cannot open: %s\n", argv[0]); return 1; }
    snprint(tmp, P9_MAX_PATH, "%s.bp_tmp", argv[0]);
    df = create(tmp, ORDWR, 0666);
    if (df < 0) { fprint(2, "boltpack: cannot create temp file\n"); close(sf); return 1; }

    bp_io_from_p9(&src_ctx.io, sf, &src_ctx);
    bp_io_from_p9(&dst_ctx.io, df, &dst_ctx);

    rc = bp_archive_delete(&src_ctx.io, &dst_ctx.io, del, (bp_uint32)del_count, &deleted);
    close(sf); close(df);

    if (rc != BP_OK) {
        fprint(2, "boltpack: delete failed: %s\n", bp_strerror(rc));
        remove(tmp); return 1;
    }
    if (!deleted) { fprint(2, "boltpack: no matching entries\n"); remove(tmp); return 1; }
    if (remove(argv[0]) != 0) {
        fprint(2, "boltpack: cannot remove archive\n"); return 1;
    }
    {
        /* Plan 9 rename = copy and remove for cross-directory; same dir = "bind" isn't available.
         * Use rename(3) from libc which is just a two-step remove+link on Plan 9. */
        int rsrc = open(tmp, OREAD);
        int rdst;
        char buf[8192]; long n;
        if (rsrc < 0) { fprint(2, "boltpack: renaming failed\n"); return 1; }
        rdst = create(argv[0], OWRITE, 0666);
        if (rdst < 0) { close(rsrc); fprint(2, "boltpack: cannot recreate archive\n"); return 1; }
        while ((n = read(rsrc, buf, sizeof(buf))) > 0) write(rdst, buf, n);
        close(rsrc); close(rdst);
        remove(tmp);
    }
    if (g_verbosity < VERB_SILENT)
        print("%lud entr%s deleted\n", (ulong)deleted, deleted==1?"y":"ies");
    return 0;
}

/* ---- void main -------------------------------------------------------- */

void
main(int argc, char *argv[])
{
    char       command;
    bp_uint32  block_size = 0;
    bp_uint8   method     = BP_METHOD_BOLTLZ;
    const char *out_dir   = nil;
    int        force = 0, dry_run = 0, i, r;
    int        interchange_profile = 0, codec_given = 0;

    if (argc < 3) { usage(); exits("usage"); }
    if (strlen(argv[1]) != 1) {
        fprint(2, "boltpack: unknown command: %s\n", argv[1]);
        usage(); exits("usage");
    }
    command = argv[1][0];
    i = 2;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-b")) {
            if (++i >= argc) { fprint(2, "boltpack: -b needs arg\n"); exits("usage"); }
            block_size = parse_block_size(argv[i++]);
            if (!block_size) { fprint(2, "boltpack: bad block size\n"); exits("error"); }
        } else if (!strcmp(argv[i], "--interchange-profile-1")) {
            interchange_profile = 1; i++;
        } else if (!strcmp(argv[i], "-0"))  { method = BP_METHOD_STORED; codec_given = 1; i++; }
        else if (!strcmp(argv[i], "-F"))  { method = BP_METHOD_FLUFF; codec_given = 1; i++; }
        else if (!strcmp(argv[i], "-qq"))  { g_verbosity = VERB_SILENT; i++; }
        else if (!strcmp(argv[i], "-q"))   { g_verbosity = VERB_QUIET;  i++; }
        else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) { fprint(2, "boltpack: -o needs arg\n"); exits("usage"); }
            out_dir = argv[i++];
        } else if (!strcmp(argv[i], "-f")) { force   = 1; i++; }
        else if (!strcmp(argv[i], "-n"))   { dry_run = 1; i++; }
        else { fprint(2, "boltpack: unknown option: %s\n", argv[i]); usage(); exits("usage"); }
    }

    if (interchange_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_STORED &&
             method != BP_METHOD_BOLTLITE)) {
            fprint(2,
                   "boltpack: Interchange Profile 1 requires create/add, Stored or BoltLite, and 16 KiB blocks\n");
            exits("usage");
        }
        block_size = BP_BLOCK_16K;
        if (!codec_given) method = BP_METHOD_BOLTLITE;
    }

    if (method == BP_METHOD_FLUFF && block_size == 0) {
        block_size = BP_BLOCK_256K;
    }

    switch (command) {
    case 'c': r = cmd_create(argc - i, argv + i, block_size, method,
                             interchange_profile); break;
    case 'a': r = cmd_add(argc - i, argv + i, block_size, method,
                          interchange_profile); break;
    case 'l':
        if (i >= argc) { fprint(2, "boltpack: list needs archive\n"); exits("usage"); }
        r = cmd_list(argv[i]); break;
    case 'x':
        if (i >= argc) { fprint(2, "boltpack: extract needs archive\n"); exits("usage"); }
        r = cmd_extract(argv[i], out_dir, force, dry_run); break;
    case 't':
        if (i >= argc) { fprint(2, "boltpack: test needs archive\n"); exits("usage"); }
        r = cmd_test(argv[i]); break;
    case 'i':
        if (i >= argc) { fprint(2, "boltpack: info needs archive\n"); exits("usage"); }
        r = cmd_info(argv[i]); break;
    case 'd': r = cmd_delete(argc - i, argv + i); break;
    default:
        fprint(2, "boltpack: unknown command: %c\n", command);
        usage(); exits("usage");
        r = 1; /* suppress uninitialized warning */
    }

    exits(r ? "error" : nil);
}
