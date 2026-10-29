/*
 * main.c -- Sharp X68000 Human68k CLI frontend for BoltPack
 *
 * Target:   Sharp X68000 running Human68k (DOS-like OS, Motorola 68000).
 * Compiler: m68k-x680x-human-gcc (GCC cross-compiler for Human68k).
 *
 * File I/O uses the standard ANSI C runtime (fopen/fread/fwrite/fseek/ftell).
 * Directory traversal uses the Human68k _dos_files / _dos_nfiles calls
 * from <x68k/dos.h> which are similar to MS-DOS findfirst/findnext.
 *
 * Human68k conventions used here:
 *   - Path separator is '\' (backslash); converted to '/' for archive names.
 *   - Drive prefix stripped before archiving, e.g. "a:\foo" → "foo".
 *   - Filenames use Shift-JIS encoding (BP_CP_SJIS).
 *   - Timestamps use DOS packed date/time format (same as MS-DOS).
 *   - File attributes follow DOS FAT conventions (read-only/hidden/system/archive).
 *
 * Metadata: Human68k uses DOS-like attributes.  Per format.md, Human68k
 *   does NOT need a separate BP_TYPE_META blob.  If BP_TYPE_META entries
 *   from other platforms are present in an archive, they are silently skipped.
 *
 * TODO: verify Shift-JIS filename handling with native X68000 locale.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_X68K
#define BP_CREATOR_CPU BP_CPU_M68K

#include "../../core/archiver.h"
#include "../../core/compress.h"
#include "../../core/bp_codepage.h"
#include "../../core/bp_metadata.h"
#include "../mini_shared/dos_time.h"
#include "x68k_path_index.h"

/* Human68k system call interface */
#include <x68k/dos.h>

#include <stdio.h>
#ifdef X68K_HOST_TEST
extern FILE *x68k_host_fopen(const char *path, const char *mode);
extern int x68k_host_remove(const char *path);
extern int x68k_host_rename(const char *old_path, const char *new_path);
#define fopen x68k_host_fopen
#define remove x68k_host_remove
#define rename x68k_host_rename
#endif

/* snprintf is C99 but available in elf2x68k libc; provide prototype for -ansi */
#if defined(__STDC__) && !defined(__cplusplus)
extern int snprintf(char *buf, size_t n, const char *fmt, ...);
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* ---- constants -------------------------------------------------------- */

#define BP_MAX_PATH   256

/* Human68k uses Shift-JIS (Japanese) */
#define X68K_CP   BP_CP_SJIS

/* Human68k file attribute bits (DOS-compatible) */
#define FA_RDONLY   0x01
#define FA_HIDDEN   0x02
#define FA_SYSTEM   0x04
#define FA_LABEL    0x08
#define FA_SUBDIR   0x10
#define FA_ARCH     0x20
#define FA_ANY      0x37   /* all except volume label */

/* ---- verbosity -------------------------------------------------------- */

#define VERB_DEFAULT  0
#define VERB_QUIET    1
#define VERB_SILENT   2

static int g_verbosity = VERB_DEFAULT;

/* ======================================================================
 * bp_io_t adapters backed by FILE *
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
    case BP_ERR_PARTIAL:            return "partial success";
    default:                       return "unknown error";
    }
}

/* ---- formatting -------------------------------------------------------- */

static void format_size(bp_uint32 bytes, char *buf, size_t sz)
{
    if (bytes < 1024UL)         sprintf(buf, "%lu bytes", (unsigned long)bytes);
    else if (bytes < 1048576UL) sprintf(buf, "%.1f KB", (double)bytes/1024.0);
    else                        sprintf(buf, "%.2f MB", (double)bytes/1048576.0);
    (void)sz;
}

static void format_size64(bp_uint64 b, char *buf, size_t sz)
{
    if (b.hi == 0) { format_size(b.lo, buf, sz); return; }
    { double d = (double)b.lo + (double)b.hi * 4294967296.0;
      sprintf(buf, "%.2f MB", d / 1048576.0); }
    (void)sz;
}

/* DOS-packed dates are local wall time. Store them only when the caller has
   supplied the fixed offset required to map that wall time to format-6 UTC. */
static bp_uint16 x68k_archive_mtime(unsigned short dos_time,
                                   unsigned short dos_date,
                                   bp_uint64 *mtime)
{
    long offset;
    mtime->lo = 0;
    mtime->hi = 0;
    if (bp_dos_timezone(&offset) != 0 ||
        bp_dos_local_to_utc((unsigned)dos_date, (unsigned)dos_time,
                            offset, mtime) != 0) return 0;
    return BP_ENTRY_FLAG_MTIME_VALID;
}

static int x68k_restore_mtime(int fd, bp_uint64 mtime)
{
    long offset;
    unsigned date, time_value;
    int converted;
    unsigned long packed;
    if (bp_dos_timezone(&offset) != 0) return BP_ERR_PARTIAL;
    converted = bp_dos_utc_to_local(mtime, offset, &date, &time_value);
    if (converted < 0) return BP_ERR_OVERFLOW;
    packed = ((unsigned long)date << 16) | (unsigned long)time_value;
    if (_dos_filedate(fd, (int)packed) != 0) return BP_ERR_IO;
    return converted > 0 ? BP_ERR_PARTIAL : BP_OK;
}

typedef struct {
    bp_uint8 data[BP_META_FAT_SIZE];
    unsigned pos;
} x68k_meta_reader_t;

static long x68k_meta_read(void *handle, void *buffer, bp_uint32 count)
{
    x68k_meta_reader_t *reader = (x68k_meta_reader_t *)handle;
    unsigned available = BP_META_FAT_SIZE - reader->pos;
    unsigned amount = count < available ? (unsigned)count : available;
    if (amount == 0) return 0;
    memcpy(buffer, reader->data + reader->pos, amount);
    reader->pos += amount;
    return (long)amount;
}

static int x68k_add_fat_metadata(bp_archive_t *ar, const char *name,
                                 bp_uint8 subject_type, bp_uint8 attributes)
{
    x68k_meta_reader_t reader;
    bp_io_t io;
    int rc = bp_metadata_fat_encode(reader.data, sizeof(reader.data),
                                    subject_type, BP_META_FAT_MASK,
                                    (bp_uint8)(attributes & BP_META_FAT_MASK));
    if (rc != BP_OK) return rc;
    reader.pos = 0;
    io.read = x68k_meta_read; io.write = NULL; io.seek = NULL; io.tell = NULL;
    io.handle = &reader;
    return bp_archive_add_entry_ex(ar, name, BP_TYPE_META, BP_META_PORTABLE,
                                   &io, bp_u64_from_u32(BP_META_FAT_SIZE),
                                   bp_u64_from_u32(0), 0);
}

/* ---- path utilities --------------------------------------------------- */

static int x68k_sjis_lead(unsigned char c)
{
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xEF);
}

static int x68k_sjis_pair_at(const char *p)
{
    unsigned char trail;
    if (!x68k_sjis_lead((unsigned char)p[0]) || p[1] == '\0') return 0;
    trail = (unsigned char)p[1];
    return (trail >= 0x40 && trail <= 0x7E) ||
           (trail >= 0x80 && trail <= 0xFC);
}

static const char *x68k_last_separator(const char *path)
{
    const char *last = NULL;
    const char *p = path;
    while (*p) {
        if (x68k_sjis_pair_at(p)) { p += 2; continue; }
        if (*p == '/' || *p == '\\') last = p;
        p++;
    }
    return last;
}

/*
 * Convert backslashes to forward slashes (archive format uses '/').
 */
static void normalise_seps(char *p)
{
    while (*p) {
        if (x68k_sjis_pair_at(p)) { p += 2; continue; }
        if (*p == '\\') *p = '/';
        p++;
    }
}

/*
 * Strip a leading drive letter and any leading slashes/backslashes.
 * "a:\\foo\\bar" → "foo/bar",  "\\foo" → "foo"
 */
static const char *clean_path(const char *p)
{
    if (p[0] && p[1] == ':') p += 2;   /* skip drive letter + colon */
    while (*p == '/' || *p == '\\') p++;
    while (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) p += 2;
    return p;
}

static int ends_with_sep(const char *s)
{
    const char *last = x68k_last_separator(s);
    return last != NULL && last[1] == '\0';
}

static int path_join(char *buf, size_t sz, const char *dir, const char *name)
{
    int n;
    if (ends_with_sep(dir)) n = snprintf(buf, sz, "%s%s", dir, name);
    else                    n = snprintf(buf, sz, "%s\\%s", dir, name);
    return n < 0 || (size_t)n >= sz ? -1 : 0;
}

/* ---- mkdir -p ---------------------------------------------------------- */

static int x68k_mkdir(const char *path)
{
    return _dos_mkdir(path) == 0 ? 0 : -1;
}

static int mkdirs(const char *path)
{
    char tmp[BP_MAX_PATH]; char *p;
    /* Convert slashes to backslashes for _dos_mkdir */
    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    for (p = tmp + 1; *p; p++) {
        if (x68k_sjis_pair_at(p)) { p++; continue; }
        if (*p == '/' || *p == '\\') {
            char sv = *p; *p = '\0';
            if (x68k_mkdir(tmp) != 0) {
                /* Ignore already-exists; check if it exists */
                struct dos_filbuf fb2;
                if (_dos_files(&fb2, tmp, FA_SUBDIR) < 0) { *p = sv; return -1; }
            }
            *p = sv;
        }
    }
    if (x68k_mkdir(tmp) != 0) {
        struct dos_filbuf fb2;
        if (_dos_files(&fb2, tmp, FA_SUBDIR) < 0) return -1;
    }
    return 0;
}

static int mkdirs_for_file(const char *path)
{
    char  tmp[BP_MAX_PATH]; const char *sl;
    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    sl = x68k_last_separator(tmp);
    if (sl && sl != tmp) {
        char *x;
        tmp[sl - tmp] = '\0';
        for (x = tmp; *x; x++) {
            if (x68k_sjis_pair_at(x)) { x++; continue; }
            if (*x == '/') *x = '\\';
        }
        return mkdirs(tmp);
    }
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

static int add_path(bp_archive_t *ar, const char *path, const char *arcname);

static int add_directory_contents(bp_archive_t *ar, const char *dir_path,
                                  const char *arc_prefix)
{
    struct dos_filbuf fb;
    char   pattern[BP_MAX_PATH];
    char   child_path[BP_MAX_PATH];
    char   child_arc[BP_MAX_PATH];
    int    rc = BP_OK;
    long   r;

    if (snprintf(pattern, sizeof(pattern), "%s\\*.*", dir_path) < 0 ||
        strlen(dir_path) + 4 >= sizeof(pattern))
        return BP_ERR_NAME_TOO_LONG;
    r = _dos_files(&fb, pattern, FA_ANY);
    while (r == 0) {
        if (rc == BP_OK && strcmp(fb.name, ".") != 0 &&
            strcmp(fb.name, "..") != 0) {
            if (path_join(child_path, sizeof(child_path), dir_path, fb.name) != 0 ||
                path_join(child_arc, sizeof(child_arc), arc_prefix, fb.name) != 0) {
                rc = BP_ERR_NAME_TOO_LONG;
            } else {
                normalise_seps(child_arc);
                rc = add_path(ar, child_path, child_arc);
            }
        }
        r = _dos_nfiles(&fb);
    }
    return rc;
}

static int add_path(bp_archive_t *ar, const char *path, const char *arcname)
{
    struct dos_filbuf fb;

    /* Use _dos_files to stat the path */
    if (_dos_files(&fb, path, FA_ANY) != 0) {
        fprintf(stderr, "boltpack: cannot stat: %s\n", path);
        return BP_ERR_IO;
    }

    if (fb.atr & FA_SUBDIR) {
        /* Directory */
        char dir_arc[BP_MAX_PATH];
        char dir_arc_utf8[BP_MAX_PATH * 3];
        int  al; int rc;
        if (strlen(arcname) >= sizeof(dir_arc)) return BP_ERR_NAME_TOO_LONG;
        strcpy(dir_arc, arcname);
        al = (int)strlen(dir_arc);
        if (al > 0 && dir_arc[al - 1] != '/')
            {
                if ((size_t)al + 1 >= sizeof(dir_arc))
                    return BP_ERR_NAME_TOO_LONG;
                dir_arc[al] = '/'; dir_arc[al+1] = '\0';
            }

        /* Directory names use the same strict Shift-JIS conversion as file
           names; storing the local bytes directly would create invalid UTF-8. */
        if (bp_local_to_utf8(dir_arc, dir_arc_utf8, sizeof(dir_arc_utf8),
                             X68K_CP) != 0)
            return BP_ERR_INVALID_ARG;

        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", dir_arc);
        {
            bp_uint64 mtime;
            bp_uint16 flags = x68k_archive_mtime(fb.time, fb.date, &mtime);
            rc = bp_archive_add_entry_ex(ar, dir_arc_utf8, BP_TYPE_DIR,
                                         BP_OS_X68K, NULL,
                                         bp_u64_from_u32(0), mtime, flags);
        }
        if (rc == BP_OK)
            rc = x68k_add_fat_metadata(ar, dir_arc_utf8, BP_TYPE_DIR,
                                       (bp_uint8)fb.atr);
        if (rc != BP_OK) return rc;
        return add_directory_contents(ar, path, dir_arc);
    }

    /* Regular file */
    {
        FILE     *fp;
        bp_io_t   fio;
        bp_uint64 fsize = bp_u64_from_ulong((unsigned long)fb.filelen);
        bp_uint64 mtime;
        bp_uint16 mtime_flags = x68k_archive_mtime(fb.time, fb.date, &mtime);
        char      arcname_sjis[BP_MAX_PATH];
        char      arcname_utf8[BP_MAX_PATH * 3];
        int       rc;

        /* Convert Shift-JIS archive name to UTF-8 for storage */
        strncpy(arcname_sjis, arcname, sizeof(arcname_sjis) - 1);
        arcname_sjis[sizeof(arcname_sjis) - 1] = '\0';
        if (bp_local_to_utf8(arcname_sjis, arcname_utf8, sizeof(arcname_utf8), X68K_CP) != 0) {
            return BP_ERR_INVALID_ARG;
        }

        fp = fopen(path, "rb");
        if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", path); return BP_ERR_IO; }
        bp_io_from_file(&fio, fp);

        if (g_verbosity == VERB_DEFAULT) { printf("compressing: %s", arcname_sjis); fflush(stdout); }

        rc = bp_archive_add_entry_ex(ar, arcname_utf8, BP_TYPE_DATA, BP_OS_X68K,
                                     &fio, fsize, mtime, mtime_flags);
        fclose(fp);
        if (rc == BP_OK)
            rc = x68k_add_fat_metadata(ar, arcname_utf8, BP_TYPE_DATA,
                                       (bp_uint8)fb.atr);

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

    /* Convert UTF-8 archive name to Shift-JIS for display */
    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name), X68K_CP) != 0)
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
    int         partial;
    bp_uint32   file_count;
    bp_uint32   dir_count;
    char        subject_name[BP_MAX_PATH * 3];
    char        subject_path[BP_MAX_PATH];
    bp_uint8     subject_type;
    int          subject_ok;
    bp_metadata_sink_t meta_sink;
    struct x68k_dir_attr_s *dir_attrs;
    unsigned int dir_attr_count;
    unsigned long dir_attr_bytes;
    char        current_target[BP_MAX_PATH];
    char        current_stage[BP_MAX_PATH];
} extract_ctx_t;

typedef struct x68k_dir_attr_s {
    struct x68k_dir_attr_s *next;
    char *path;
    bp_uint8 valid;
    bp_uint8 value;
} x68k_dir_attr_t;

static unsigned long g_extract_stage_counter;

static int x68k_path_exists(const char *path, struct dos_filbuf *found)
{
    struct dos_filbuf local;
    return _dos_files(found ? found : &local, path, FA_ANY) == 0;
}

static int x68k_apply_attributes(const char *path, bp_uint8 valid,
                                 bp_uint8 value)
{
    struct dos_filbuf current;
    bp_uint8 desired;
    if (_dos_files(&current, path, FA_ANY) != 0) return BP_ERR_PARTIAL;
    valid = (bp_uint8)(valid & BP_META_FAT_MASK);
    desired = (bp_uint8)((current.atr & (bp_uint8)(BP_META_FAT_MASK & ~valid)) |
                         (value & valid) |
                         (current.atr & (bp_uint8)~BP_META_FAT_MASK));
    if (_dos_chmod(path, (int)desired) < 0) return BP_ERR_PARTIAL;
    if (_dos_files(&current, path, FA_ANY) != 0 ||
        (current.atr & valid) != (value & valid)) return BP_ERR_PARTIAL;
    return BP_OK;
}

static int x68k_queue_dir_attributes(extract_ctx_t *ctx, const char *path,
                                     bp_uint8 valid, bp_uint8 value)
{
    x68k_dir_attr_t *node;
    size_t length = strlen(path) + 1U;
    if (ctx->dir_attr_count >= 256U ||
        length > 32768UL - ctx->dir_attr_bytes) return BP_ERR_PARTIAL;
    node = (x68k_dir_attr_t *)calloc(1, sizeof(*node));
    if (!node) return BP_ERR_PARTIAL;
    node->path = (char *)malloc(length);
    if (!node->path) { free(node); return BP_ERR_PARTIAL; }
    memcpy(node->path, path, length);
    node->valid = valid; node->value = value;
    node->next = ctx->dir_attrs; ctx->dir_attrs = node;
    ctx->dir_attr_count++;
    ctx->dir_attr_bytes += (unsigned long)length;
    return BP_OK;
}

static void x68k_finish_dir_attributes(extract_ctx_t *ctx, int restore)
{
    while (ctx->dir_attrs) {
        x68k_dir_attr_t **best_link = &ctx->dir_attrs;
        x68k_dir_attr_t **link;
        x68k_dir_attr_t *node;
        unsigned best_depth = 0;
        for (link = &ctx->dir_attrs; *link; link = &(*link)->next) {
            const char *p;
            unsigned depth = 0;
            for (p = (*link)->path; *p; p++) {
                if (x68k_sjis_pair_at(p)) { p++; continue; }
                if (*p == '/' || *p == '\\') depth++;
            }
            if (depth >= best_depth) { best_depth = depth; best_link = link; }
        }
        node = *best_link; *best_link = node->next;
        if (restore &&
            x68k_apply_attributes(node->path, node->valid, node->value) != BP_OK) {
            fprintf(stderr, "boltpack: cannot restore directory attributes: %s\n",
                    node->path);
            ctx->partial = 1;
        }
        ctx->dir_attr_count--;
        ctx->dir_attr_bytes -= (unsigned long)strlen(node->path) + 1UL;
        free(node->path); free(node);
    }
}

static int x68k_sibling_name(const char *target, const char *suffix,
                             char *out, size_t capacity,
                             unsigned long sequence)
{
    const char *slash = x68k_last_separator(target);
    size_t parent_len;
    int n;
    parent_len = slash ? (size_t)(slash - target + 1) : 0;
    n = snprintf(out, capacity, "%.*sBP%04lX.%s", (int)parent_len,
                 target, sequence & 0xFFFFUL, suffix);
    return n < 0 || (size_t)n >= capacity ? -1 : 0;
}

static int x68k_create_archive_stage(const char *target, char *stage,
                                     size_t capacity)
{
    unsigned long attempt;
    for (attempt = 0; attempt < 65536UL; attempt++) {
        unsigned long sequence = g_extract_stage_counter++ & 0xFFFFUL;
        FILE *fp;
        if (x68k_sibling_name(target, "TMP", stage, capacity, sequence) != 0)
            return BP_ERR_NAME_TOO_LONG;
        fp = fopen(stage, "wbx");
        if (fp != NULL) {
            if (fclose(fp) != 0) { remove(stage); return BP_ERR_IO; }
            return BP_OK;
        }
        if (errno != EEXIST) return BP_ERR_IO;
    }
    return BP_ERR_PARTIAL;
}

static int x68k_copy_file(const char *source, const char *destination)
{
    FILE *in = fopen(source, "rb");
    FILE *out;
    unsigned char buffer[8192];
    int rc = BP_OK;
    if (in == NULL) return BP_ERR_IO;
    out = fopen(destination, "wb");
    if (out == NULL) { fclose(in); return BP_ERR_IO; }
    for (;;) {
        size_t count = fread(buffer, 1, sizeof(buffer), in);
        if (count != 0 && fwrite(buffer, 1, count, out) != count) {
            rc = BP_ERR_IO;
            break;
        }
        if (count < sizeof(buffer)) {
            if (ferror(in)) rc = BP_ERR_IO;
            break;
        }
    }
    if (fclose(in) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (fclose(out) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    return rc;
}

static int x68k_archive_target(const char *path, int *exists,
                               bp_uint8 *attributes)
{
    struct dos_filbuf found;
    *exists = x68k_path_exists(path, &found);
    *attributes = 0;
    if (!*exists) return BP_OK;
    if (found.atr & FA_SUBDIR) return BP_ERR_IO;
    if (found.atr & FA_RDONLY) return BP_ERR_IO;
    *attributes = (bp_uint8)(found.atr & BP_META_FAT_MASK);
    return BP_OK;
}

static int x68k_publish_archive(const char *target, char *stage,
                                int expected_exists, bp_uint8 attributes)
{
    struct dos_filbuf current;
    int exists = x68k_path_exists(target, &current);
    char backup[BP_MAX_PATH];
    unsigned long attempt;

    if (exists != expected_exists || (exists && (current.atr & FA_RDONLY)))
        return BP_ERR_IO;
    if (exists && _dos_chmod(stage, (int)attributes) < 0)
        return BP_ERR_PARTIAL;
    if (!exists) {
        if (rename(stage, target) != 0) return BP_ERR_IO;
        stage[0] = '\0';
        return BP_OK;
    }

    for (attempt = 0; attempt < 65536UL; attempt++) {
        unsigned long sequence = g_extract_stage_counter++ & 0xFFFFUL;
        if (x68k_sibling_name(target, "BAK", backup, sizeof(backup),
                              sequence) != 0) return BP_ERR_NAME_TOO_LONG;
        if (!x68k_path_exists(backup, NULL)) break;
    }
    if (attempt == 65536UL || rename(target, backup) != 0) return BP_ERR_IO;
    if (rename(stage, target) != 0) {
        if (rename(backup, target) != 0) {
            fprintf(stderr, "boltpack: original archive is preserved as %s\n",
                    backup);
            return BP_ERR_PARTIAL;
        }
        return BP_ERR_IO;
    }
    stage[0] = '\0';
    if (remove(backup) != 0) {
        fprintf(stderr, "boltpack: previous archive remains as %s\n", backup);
        return BP_ERR_PARTIAL;
    }
    return BP_OK;
}

static FILE *x68k_create_output_stage(extract_ctx_t *ctx,
                                      const char *target)
{
    unsigned long attempt;

    for (attempt = 0; attempt < 65536UL; attempt++) {
        unsigned long sequence = g_extract_stage_counter++ & 0xFFFFUL;
        FILE *fp;
        if (x68k_sibling_name(target, "TMP", ctx->current_stage,
                              sizeof(ctx->current_stage), sequence) != 0)
            return NULL;
        fp = fopen(ctx->current_stage, "wbx");
        if (fp != NULL) {
            strcpy(ctx->current_target, target);
            return fp;
        }
        if (errno != EEXIST) return NULL;
    }
    return NULL;
}

static int x68k_publish_output(extract_ctx_t *ctx)
{
    struct dos_filbuf target_info;
    int target_exists = x68k_path_exists(ctx->current_target, &target_info);

    if (target_exists) {
        char backup[BP_MAX_PATH];
        unsigned long attempt;

        if (target_info.atr & FA_SUBDIR) return -1;
        if (!ctx->force) return -1;
        for (attempt = 0; attempt < 65536UL; attempt++) {
            unsigned long sequence = g_extract_stage_counter++ & 0xFFFFUL;
            if (x68k_sibling_name(ctx->current_target, "BAK", backup,
                                  sizeof(backup), sequence) != 0)
                return -1;
            if (!x68k_path_exists(backup, NULL)) break;
        }
        if (attempt == 65536UL || rename(ctx->current_target, backup) != 0)
            return -1;
        if (rename(ctx->current_stage, ctx->current_target) != 0) {
            if (rename(backup, ctx->current_target) != 0) {
                fprintf(stderr,
                        "boltpack: original destination is preserved as %s\n",
                        backup);
                return BP_ERR_PARTIAL;
            }
            return -1;
        }
        ctx->current_stage[0] = '\0';
        if (remove(backup) != 0) {
            fprintf(stderr,
                    "boltpack: previous destination remains as %s\n",
                    backup);
            return BP_ERR_PARTIAL;
        }
        return BP_OK;
    }

    if (rename(ctx->current_stage, ctx->current_target) != 0)
        return -1;
    ctx->current_stage[0] = '\0';
    return BP_OK;
}

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char  path[BP_MAX_PATH];
    char  local_name[BP_MAX_PATH];
    char  qdos_path[BP_MAX_PATH];  /* with backslashes */
    FILE *fp; bp_io_t *io;

    if (entry->entry_type == BP_TYPE_META) {
        if (entry->platform != BP_META_PORTABLE) return NULL;
        if (ctx->dry_run) return bp_archive_dry_run_stream();
        if (!ctx->subject_ok || strcmp(entry->name, ctx->subject_name) != 0)
            return NULL;
        bp_metadata_sink_init(&ctx->meta_sink);
        return &ctx->meta_sink.io;
    }

    if (!bp_name_is_safe(entry->name)) return NULL;

    /* Convert UTF-8 archive name to SJIS */
    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name),
                         X68K_CP) != 0) return NULL;

    /* Build output path with backslashes */
    if (ctx->out_dir) {
        if (path_join(path, sizeof(path), ctx->out_dir, local_name) != 0) {
            fprintf(stderr, "boltpack: output path too long: %s\n",
                    entry->name);
            return NULL;
        }
    } else {
        strncpy(path, local_name, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }
    strncpy(qdos_path, path, sizeof(qdos_path) - 1);
    qdos_path[sizeof(qdos_path) - 1] = '\0';
    /* Convert '/' to '\' for Human68k */
    { char *xp; for (xp = qdos_path; *xp; xp++) if (*xp == '/') *xp = '\\'; }

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", local_name);
        if (!ctx->dry_run) {
          char dmk[BP_MAX_PATH];
          strcpy(dmk, qdos_path);
          { int dn = (int)strlen(dmk); if (dn > 0 && dmk[dn-1]=='\\') dmk[dn-1]='\0'; }
          if (mkdirs(dmk) != 0) ctx->partial = 1;
          if (entry->flags & BP_ENTRY_FLAG_MTIME_VALID) ctx->partial = 1;
          strncpy(ctx->subject_name, entry->name,
                  sizeof(ctx->subject_name) - 1);
          ctx->subject_name[sizeof(ctx->subject_name) - 1] = '\0';
          strncpy(ctx->subject_path, dmk, sizeof(ctx->subject_path) - 1);
          ctx->subject_path[sizeof(ctx->subject_path) - 1] = '\0';
          ctx->subject_type = BP_TYPE_DIR;
          ctx->subject_ok = strlen(entry->name) < sizeof(ctx->subject_name) &&
                            strlen(dmk) < sizeof(ctx->subject_path) &&
                            x68k_path_exists(dmk, NULL);
        } else ctx->subject_ok = 0;
        return NULL;
    }

    /* Skip META, RSRC, SYMLINK on X68K (no metadata blob needed) */
    if (entry->entry_type != BP_TYPE_DATA) {
        if (g_verbosity == VERB_DEFAULT)
            printf("   skipping: %s [type not supported on X68K]\n", local_name);
        return NULL;
    }

    ctx->file_count++;
    if (g_verbosity == VERB_DEFAULT) printf(" extracting: %s\n", local_name);
    if (ctx->dry_run) return bp_archive_dry_run_stream();

    {
        struct dos_filbuf target_info;
        if (x68k_path_exists(qdos_path, &target_info)) {
            if ((target_info.atr & FA_SUBDIR) || !ctx->force) {
                fprintf(stderr, "boltpack: exists (use -f): %s\n", local_name);
                return NULL;
            }
        }
    }

    /* Create parent directories */
    { char tmp2[BP_MAX_PATH]; const char *sl; strcpy(tmp2, qdos_path);
      sl = x68k_last_separator(tmp2);
      if (sl && sl != tmp2) { tmp2[sl - tmp2] = '\0'; if (mkdirs(tmp2) != 0) return NULL; } }

    fp = x68k_create_output_stage(ctx, qdos_path);
    if (!fp) { fprintf(stderr, "boltpack: cannot create: %s\n", local_name); return NULL; }
    io = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (!io) {
        fclose(fp);
        remove(ctx->current_stage);
        ctx->current_stage[0] = '\0';
        return NULL;
    }
    bp_io_from_file(io, fp);
    return io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    int rc = status;
    if (entry->entry_type == BP_TYPE_META) {
        bp_portable_metadata_t parsed;
        if (status != BP_OK) return BP_OK;
        rc = bp_metadata_parse(ctx->meta_sink.data, ctx->meta_sink.length,
                               &parsed);
        if (rc == BP_ERR_CORRUPT) return rc;
        if (!ctx->subject_ok || strcmp(entry->name, ctx->subject_name) != 0 ||
            !bp_metadata_matches_subject(&parsed, ctx->subject_type)) {
            ctx->partial = 1;
            return BP_ERR_PARTIAL;
        }
        if (parsed.has_fat && !ctx->dry_run) {
            if (ctx->subject_type == BP_TYPE_DIR)
                rc = x68k_queue_dir_attributes(ctx, ctx->subject_path,
                                               parsed.fat_valid,
                                               parsed.fat_value);
            else
                rc = x68k_apply_attributes(ctx->subject_path,
                                           parsed.fat_valid,
                                           parsed.fat_value);
            if (rc != BP_OK) {
                fprintf(stderr, "boltpack: cannot restore attributes: %s\n",
                        ctx->subject_path);
                ctx->partial = 1;
                return BP_ERR_PARTIAL;
            }
        }
        if (rc == BP_ERR_PARTIAL || parsed.has_unknown) ctx->partial = 1;
        return (rc == BP_OK || rc == BP_ERR_PARTIAL) ? rc : BP_ERR_PARTIAL;
    }
    if (!out) return BP_OK;
    if (rc == BP_OK && (entry->flags & BP_ENTRY_FLAG_MTIME_VALID)) {
        if (fflush((FILE *)out->handle) != 0 ||
            x68k_restore_mtime(fileno((FILE *)out->handle), entry->mtime) != BP_OK) {
            fprintf(stderr, "boltpack: could not fully restore timestamp: %s\n",
                    ctx->current_target);
            ctx->partial = 1;
        }
    }
    if (fclose((FILE *)out->handle) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    free(out);
    if (rc == BP_OK) {
        rc = x68k_publish_output(ctx);
        if (rc != BP_OK)
            fprintf(stderr, "boltpack: cannot publish extracted file: %s\n",
                    ctx->current_target);
        else {
            strncpy(ctx->subject_name, entry->name,
                    sizeof(ctx->subject_name) - 1);
            ctx->subject_name[sizeof(ctx->subject_name) - 1] = '\0';
            strncpy(ctx->subject_path, ctx->current_target,
                    sizeof(ctx->subject_path) - 1);
            ctx->subject_path[sizeof(ctx->subject_path) - 1] = '\0';
            ctx->subject_type = BP_TYPE_DATA;
            ctx->subject_ok = strlen(entry->name) < sizeof(ctx->subject_name) &&
                              strlen(ctx->current_target) < sizeof(ctx->subject_path);
        }
    }
    if (rc != BP_OK && ctx->current_stage[0] != '\0') {
        if (remove(ctx->current_stage) != 0) rc = BP_ERR_PARTIAL;
        ctx->current_stage[0] = '\0';
    }
    ctx->current_target[0] = '\0';
    return rc;
}

/* ---- test callback ---------------------------------------------------- */

typedef struct { bp_uint32 ok; bp_uint32 fail; } test_ctx_t;

static void test_cb(const bp_entry_t *entry, int result, void *user_data)
{
    test_ctx_t *ctx = (test_ctx_t *)user_data;
    char local_name[BP_MAX_PATH];
    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name), X68K_CP) != 0)
        strncpy(local_name, entry->name, sizeof(local_name) - 1);
    if (g_verbosity < VERB_SILENT)
        printf("  testing: %s %s\n", local_name,
               result == BP_OK ? "OK" : bp_strerror(result));
    if (result == BP_OK) ctx->ok++; else ctx->fail++;
}

/* ---- usage ------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr,
        "BoltPack v%d.%d (Sharp X68000 Human68k)\n"
        "%s\n\n"
        "Usage: boltpack.x <cmd> [opts] archive.blt [files...]\n\n"
        "  c create   a add   x extract\n"
        "  l list     t test  i info   d delete\n\n"
        "Options:\n"
        "  -b SIZE   block size: 8k 16k 32k 64k 256k\n"
        "  -0        store only    -q quiet   -qq silent\n"
        "  --interchange-profile-1  create/add mini-compatible archive\n"
        "  -o PATH   output path for extraction\n"
        "  -f        force overwrite    -n dry run\n",
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
        printf("Method:        %s\n",  format_methods(info.method_counts));
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

static int x68k_preflight_path(const char *path, const char *arcname)
{
    struct dos_filbuf source;
    if (_dos_files(&source, path, FA_ANY) != 0)
        return BP_ERR_IO;

    if (source.atr & FA_SUBDIR) {
        struct dos_filbuf child;
        char dir_arc[BP_MAX_PATH];
        char utf8_name[BP_MAX_PATH * 3];
        char pattern[BP_MAX_PATH];
        size_t name_len = strlen(arcname);
        long r;
        int rc = BP_OK;

        if (name_len >= sizeof(dir_arc)) return BP_ERR_NAME_TOO_LONG;
        strcpy(dir_arc, arcname);
        if (name_len > 0 && dir_arc[name_len - 1] != '/') {
            if (name_len + 1 >= sizeof(dir_arc)) return BP_ERR_NAME_TOO_LONG;
            dir_arc[name_len++] = '/';
            dir_arc[name_len] = '\0';
        }
        if (bp_local_to_utf8(dir_arc, utf8_name, sizeof(utf8_name), X68K_CP) != 0 ||
            !bp_name_is_safe(utf8_name))
            return BP_ERR_INVALID_ARG;

        if (snprintf(pattern, sizeof(pattern), "%s\\*.*", path) < 0 ||
            strlen(path) + 4 >= sizeof(pattern))
            return BP_ERR_NAME_TOO_LONG;
        r = _dos_files(&child, pattern, FA_ANY);
        while (r == 0) {
            if (rc == BP_OK && strcmp(child.name, ".") != 0 &&
                strcmp(child.name, "..") != 0) {
                char child_path[BP_MAX_PATH];
                char child_arc[BP_MAX_PATH];
                if (path_join(child_path, sizeof(child_path), path, child.name) != 0 ||
                    path_join(child_arc, sizeof(child_arc), dir_arc, child.name) != 0) {
                    rc = BP_ERR_NAME_TOO_LONG;
                } else {
                    normalise_seps(child_arc);
                    rc = x68k_preflight_path(child_path, child_arc);
                }
            }
            r = _dos_nfiles(&child);
        }
        return rc;
    }

    {
        char utf8_name[BP_MAX_PATH * 3];
        if (bp_local_to_utf8(arcname, utf8_name, sizeof(utf8_name), X68K_CP) != 0 ||
            !bp_name_is_safe(utf8_name))
            return BP_ERR_INVALID_ARG;
    }
    return BP_OK;
}

static int x68k_preflight_source_names(int argc, char **argv)
{
    int i;
    for (i = 0; i < argc; i++) {
        char local_name[BP_MAX_PATH];
        const char *source_name = clean_path(argv[i]);
        int rc;
        if (strlen(source_name) >= sizeof(local_name))
            return BP_ERR_NAME_TOO_LONG;
        strcpy(local_name, source_name);
        normalise_seps(local_name);
        rc = x68k_preflight_path(argv[i], local_name);
        if (rc != BP_OK) return rc;
    }
    return BP_OK;
}

/* ---- cmd_create ------------------------------------------------------- */

static int cmd_create(int argc, char **argv, bp_uint32 block_size,
                      bp_uint8 method, int interchange_profile)
{
    FILE *fp; bp_io_t aio; bp_archive_t ar; int rc, i;
    char stage[BP_MAX_PATH];
    int target_exists;
    bp_uint8 target_attributes;
    stage[0] = '\0';
    if (argc < 2) { fprintf(stderr, "boltpack: create needs archive + files\n"); return 1; }
    rc = x68k_preflight_source_names(argc - 1, argv + 1);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: source name cannot be represented safely\n");
        return 1;
    }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    rc = x68k_archive_target(argv[0], &target_exists, &target_attributes);
    if (rc == BP_OK) rc = x68k_create_archive_stage(argv[0], stage, sizeof(stage));
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot stage archive: %s\n", argv[0]);
        return 1;
    }
    fp = fopen(stage, "w+b");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot write archive stage: %s\n", stage);
        remove(stage);
        return 1;
    }
    bp_io_from_file(&aio, fp);
    rc = interchange_profile
        ? bp_archive_create_profile(&ar, &aio, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp); remove(stage); return 1;
    }
    if (interchange_profile && method == BP_METHOD_STORED)
        ar.method = BP_METHOD_STORED;
    for (i = 1; i < argc; i++) {
        char buf[BP_MAX_PATH];
        strcpy(buf, clean_path(argv[i]));
        normalise_seps(buf);
        rc = add_path(&ar, argv[i], buf);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: error adding: %s\n", argv[i]);
            bp_archive_close(&ar); fclose(fp); remove(stage); return 1;
        }
    }
    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (rc == BP_OK)
        rc = x68k_publish_archive(argv[0], stage, target_exists,
                                  target_attributes);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        if (stage[0] != '\0') remove(stage);
        return 1;
    }
    if (g_verbosity < VERB_SILENT) printf("%d item%s archived\n", argc-1, argc-1==1?"":"s");
    return 0;
}

/* ---- cmd_add ---------------------------------------------------------- */

static int cmd_add(int argc, char **argv, bp_uint32 block_size,
                   bp_uint8 method, int interchange_profile)
{
    FILE *fp; bp_io_t aio; bp_archive_t ar; int rc, i;
    char stage[BP_MAX_PATH];
    int target_exists;
    bp_uint8 target_attributes;
    stage[0] = '\0';
    if (argc < 2) { fprintf(stderr, "boltpack: add needs archive + files\n"); return 1; }
    rc = x68k_preflight_source_names(argc - 1, argv + 1);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: source name cannot be represented safely\n");
        return 1;
    }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); return 1; }
    rc = x68k_archive_target(argv[0], &target_exists, &target_attributes);
    if (rc == BP_OK && !target_exists) rc = BP_ERR_IO;
    if (rc == BP_OK) rc = x68k_create_archive_stage(argv[0], stage, sizeof(stage));
    if (rc == BP_OK) rc = x68k_copy_file(argv[0], stage);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot stage archive: %s\n", argv[0]);
        if (stage[0] != '\0') remove(stage);
        return 1;
    }
    fp = fopen(stage, "r+b");
    if (!fp) {
        fprintf(stderr, "boltpack: cannot edit archive stage: %s\n", stage);
        remove(stage);
        return 1;
    }
    bp_io_from_file(&aio, fp);
    rc = bp_archive_open_append(&ar, &aio, block_size, method);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        fclose(fp); remove(stage); return 1;
    }
    if (interchange_profile) {
        rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: archive does not meet Interchange Profile 1 limits: %s\n",
                    bp_strerror(rc));
            bp_archive_close(&ar); fclose(fp); remove(stage); return 1;
        }
        if (method == BP_METHOD_STORED) ar.method = BP_METHOD_STORED;
    }
    for (i = 1; i < argc; i++) {
        char buf[BP_MAX_PATH]; strcpy(buf, clean_path(argv[i])); normalise_seps(buf);
        rc = add_path(&ar, argv[i], buf);
        if (rc != BP_OK) { fprintf(stderr, "boltpack: error adding: %s\n", argv[i]);
                           bp_archive_close(&ar); fclose(fp); remove(stage); return 1; }
    }
    rc = bp_archive_finalize(&ar);
    bp_archive_close(&ar);
    if (fclose(fp) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (rc == BP_OK)
        rc = x68k_publish_archive(argv[0], stage, target_exists,
                                  target_attributes);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        if (stage[0] != '\0') remove(stage);
        return 1;
    }
    if (g_verbosity < VERB_SILENT) printf("%d item%s added\n", argc-1, argc-1==1?"":"s");
    return 0;
}

/* ---- cmd_extract ------------------------------------------------------ */

typedef struct {
    x68k_path_index_t index;
    const char *out_dir;
    int error;
} x68k_extract_preflight_t;

static int x68k_extract_preflight_entry(const bp_entry_t *entry,
                                        void *user_data)
{
    x68k_extract_preflight_t *check =
        (x68k_extract_preflight_t *)user_data;
    char local_name[BP_MAX_PATH];
    char output_path[BP_MAX_PATH];
    int is_directory, rc;

    if (entry->entry_type == BP_TYPE_META || entry->entry_type == BP_TYPE_RSRC ||
        entry->entry_type == BP_TYPE_SYMLINK) return 0;
    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR)
        return 0;
    if (!bp_name_is_safe(entry->name) ||
        bp_utf8_to_local(entry->name, local_name, sizeof(local_name),
                         X68K_CP) != 0) {
        check->error = BP_ERR_INVALID_ARG;
        return 1;
    }
    is_directory = entry->entry_type == BP_TYPE_DIR;
    if (check->out_dir) {
        if (path_join(output_path, sizeof(output_path), check->out_dir,
                      local_name) != 0) {
            fprintf(stderr, "boltpack: output path too long: %s\n",
                    entry->name);
            check->error = BP_ERR_NAME_TOO_LONG;
            return 1;
        }
    } else if (strlen(local_name) >= sizeof(output_path)) {
        check->error = BP_ERR_NAME_TOO_LONG;
        return 1;
    } else strcpy(output_path, local_name);
    rc = x68k_path_index_add(&check->index, local_name, is_directory);
    if (rc != BP_OK) {
        check->error = rc;
        return 1;
    }
    return 0;
}

static int cmd_extract(const char *archive_name, const char *out_dir,
                       int force, int dry_run)
{
    FILE *fp; bp_io_t aio; extract_ctx_t ctx; int rc;
    x68k_extract_preflight_t preflight;
    fp = fopen(archive_name, "rb");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", archive_name); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    bp_io_from_file(&aio, fp);
    memset(&preflight, 0, sizeof(preflight));
    preflight.out_dir = out_dir;
    rc = x68k_path_index_init(&preflight.index);
    if (rc == BP_OK)
        rc = bp_archive_list(&aio, x68k_extract_preflight_entry, &preflight);
    if (preflight.error != 0) rc = preflight.error;
    x68k_path_index_free(&preflight.index);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: extraction preflight: %s\n", bp_strerror(rc));
        fclose(fp);
        return 1;
    }
    if (out_dir && !dry_run) {
        char od[BP_MAX_PATH];
        if (strlen(out_dir) >= sizeof(od)) {
            fprintf(stderr, "boltpack: output path too long\n");
            fclose(fp);
            return 1;
        }
        strcpy(od, out_dir);
        if (mkdirs(od) != 0) {
            fprintf(stderr, "boltpack: cannot create output path: %s\n", out_dir);
            fclose(fp);
            return 1;
        }
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir; ctx.force = force; ctx.dry_run = dry_run;
    rc = bp_archive_extract_all_ex(&aio, extract_open, extract_done, NULL, &ctx);
    x68k_finish_dir_attributes(&ctx, rc == BP_OK || rc == BP_ERR_PARTIAL);
    fclose(fp);
    if (ctx.partial && rc == BP_OK) rc = BP_ERR_PARTIAL;
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
    char stage[BP_MAX_PATH]; bp_uint32 deleted = 0; int rc;
    int target_exists;
    bp_uint8 target_attributes;
    if (argc < 2) { fprintf(stderr, "boltpack: delete needs archive + names\n"); return 1; }
    stage[0] = '\0';
    del = (const char **)&argv[1]; del_count = argc - 1;
    rc = x68k_archive_target(argv[0], &target_exists, &target_attributes);
    if (rc != BP_OK || !target_exists) {
        fprintf(stderr, "boltpack: cannot open: %s\n", argv[0]); return 1;
    }
    rc = x68k_create_archive_stage(argv[0], stage, sizeof(stage));
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: cannot stage archive: %s\n", argv[0]); return 1;
    }
    sf = fopen(argv[0], "rb");
    if (!sf) { remove(stage); fprintf(stderr, "boltpack: cannot open: %s\n", argv[0]); return 1; }
    df = fopen(stage, "w+b");
    if (!df) { fclose(sf); remove(stage); fprintf(stderr, "boltpack: cannot create archive stage\n"); return 1; }
    bp_io_from_file(&sio, sf); bp_io_from_file(&dio, df);
    rc = bp_archive_delete(&sio, &dio, del, (bp_uint32)del_count, &deleted);
    if (fclose(sf) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (fclose(df) != 0 && rc == BP_OK) rc = BP_ERR_IO;
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); remove(stage); return 1; }
    if (!deleted) { fprintf(stderr, "boltpack: no matching entries\n"); remove(stage); return 1; }
    rc = x68k_publish_archive(argv[0], stage, target_exists,
                              target_attributes);
    if (rc != BP_OK) {
        fprintf(stderr, "boltpack: %s\n", bp_strerror(rc));
        if (stage[0] != '\0') remove(stage);
        return 1;
    }
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
    int        interchange_profile = 0, codec_given = 0;

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
        } else if (!strcmp(argv[i], "--interchange-profile-1")) {
            interchange_profile = 1; i++;
        } else if (!strcmp(argv[i], "-0"))  { method = BP_METHOD_STORED; codec_given = 1; i++; }
        else if (!strcmp(argv[i], "-qq")) { g_verbosity = VERB_SILENT; i++; }
        else if (!strcmp(argv[i], "-q"))  { g_verbosity = VERB_QUIET;  i++; }
        else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) { fprintf(stderr, "boltpack: -o needs arg\n"); return 1; }
            out_dir = argv[i++];
        } else if (!strcmp(argv[i], "-f")) { force = 1; i++; }
        else if (!strcmp(argv[i], "-n"))   { dry_run = 1; i++; }
        else { fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]); usage(); return 1; }
    }

    if (interchange_profile) {
        if ((command != 'c' && command != 'a') ||
            (block_size != 0 && block_size != BP_BLOCK_16K) ||
            (codec_given && method != BP_METHOD_STORED &&
             method != BP_METHOD_BOLTLITE)) {
            fprintf(stderr,
                    "boltpack: Interchange Profile 1 requires create/add, Stored or BoltLite, and 16 KiB blocks\n");
            return 1;
        }
        block_size = BP_BLOCK_16K;
        if (!codec_given) method = BP_METHOD_BOLTLITE;
    }

    switch (command) {
    case 'c': return cmd_create(argc - i, argv + i, block_size, method,
                                interchange_profile);
    case 'a': return cmd_add(argc - i, argv + i, block_size, method,
                             interchange_profile);
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
