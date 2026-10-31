/*
 * main.c -- Sinclair QL (QDOS) CLI frontend for BoltPack
 *
 * Target:   Sinclair QL / SMSQ/E, QPC2, Q68.
 * Compiler: m68k-qdos-gcc (GCC port for QDOS/SMSQ).
 *
 * File I/O uses the standard ANSI C runtime (fopen/fread/fwrite).
 * Directory traversal uses QDOS IO_OPEN with Q_DIR and reads qdirect records
 * through IO_FSTRG; standard fopen opens the path as a regular file instead.
 *
 * QDOS conventions used here:
 *   - Directory paths end with '_':  "win1_mydir_"
 *   - Files do not end with '_':     "win1_mydir_file"
 *   - The device prefix is stripped before building archive entry names.
 *   - Timestamps are seconds since 1961-01-01 (QDOS epoch) in local wall
 *     time. BP_TZ_OFFSET supplies the offset east of UTC for conversion.
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
#include "ql_time.h"
#include "ql_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef BP_QL_HOST_TEST
#include <utime.h>
#else
/* Keep the QDOS calls local: qdos.h also declares qdirect_t, which this
 * frontend defines from the on-disk 64-byte directory record. */
extern long _io_open(const char *, long);
extern int _fs_date(long, short, char, long *);
extern int _io_close(long);
extern int _io_fstrg(long, short, void *, short);
#endif

/* QDOS unistd.h maps these names to OS calls; keep bp_io_t member names
 * intact while retaining the close declaration and the close() mapping. */
#undef read
#undef write

/* fdopen is part of the C68 runtime but hidden by strict host ANSI headers. */
extern FILE *fdopen(int, const char *);

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
#define QDOS_EPOCH_OFFSET   BP_QL_EPOCH_OFFSET
#define QL_OPEN_DIR         4L
#define QL_EOF              (-10)

/* Maximum QDOS filename length */
#define QDOS_NAME_MAX   QL_QDOS_PATH_MAX

/* ---- qdirect: QDOS directory entry (64 bytes, big-endian) ------------- */

/*
 * Read from a Q_DIR channel. All multi-byte fields are big-endian
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
static int g_interchange_profile = 0;


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

/* ---- QDOS time conversion ---------------------------------------------- */

static int ql_tz_warning_reported;
static int ql_tz_override_set;
static long ql_tz_override_seconds;

static int ql_tz_offset_seconds(long *offset_seconds)
{
    if (ql_tz_override_set) {
        *offset_seconds = ql_tz_override_seconds;
        return 1;
    }
    if (bp_ql_parse_tz_offset(getenv("BP_TZ_OFFSET"), offset_seconds))
        return 1;
    if (!ql_tz_warning_reported) {
        fprintf(stderr, "boltpack: invalid BP_TZ_OFFSET; use minutes east of UTC in -1439..1439\n");
        ql_tz_warning_reported = 1;
    }
    return 0;
}

static bp_uint16 qdos_to_archive_time(long qdos_time, bp_uint64 *mtime)
{
    long offset_seconds;
    *mtime = bp_u64_from_u32(0);
    if (!ql_tz_offset_seconds(&offset_seconds) ||
        !bp_ql_qdos_to_utc(qdos_time, offset_seconds, mtime))
        return 0;
    return BP_ENTRY_FLAG_MTIME_VALID;
}

/* Apply an archived Unix timestamp through the QDOS filing-system date API.
 * QDOS stores seconds since 1961 in a signed long; unsupported devices and
 * values outside that range are reported to the caller as partial metadata. */
static int ql_set_archive_time(const char *path, bp_uint64 mtime)
{
    if (path == NULL || mtime.hi != 0) return BP_ERR_PARTIAL;
#ifdef BP_QL_HOST_TEST
    {
        struct utimbuf times;
        if ((unsigned long)mtime.lo >
            BP_QL_MAX_QDOS_SECONDS - QDOS_EPOCH_OFFSET)
            return BP_ERR_PARTIAL;
        times.actime = (time_t)mtime.lo;
        times.modtime = (time_t)mtime.lo;
        return utime(path, &times) == 0 ? BP_OK : BP_ERR_PARTIAL;
    }
#else
    {
        long offset_seconds;
        long qdos_seconds;
        long channel;
        int result;
        if (!ql_tz_offset_seconds(&offset_seconds) ||
            !bp_ql_utc_to_qdos(mtime, offset_seconds, &qdos_seconds))
            return BP_ERR_PARTIAL;
        channel = _io_open(path, 0);
        if (channel < 0) return BP_ERR_PARTIAL;
        result = _fs_date(channel, -1, 0, &qdos_seconds);
        if (_io_close(channel) != 0 && result == 0)
            result = BP_ERR_IO;
        return result == 0 ? BP_OK : BP_ERR_PARTIAL;
    }
#endif
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
    return ql_strip_disk_device(qdos_path);
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
static int ql_to_arcname(const char *qdos_rel, int is_directory, char *out,
                         size_t outsz, const char *parent_arc)
{
    char component[QDOS_NAME_MAX + 1];
    size_t parent_len;
    size_t component_len;
    if (ql_project_component(qdos_rel, is_directory, component,
                             sizeof(component)) != 0)
        return BP_ERR_INVALID_ARG;
    parent_len = parent_arc ? strlen(parent_arc) : 0;
    component_len = strlen(component);
    /* `parent_arc` is built by our recursive descent so uses '/'.
     * Just append the entry name to it. */
    if (parent_len != 0) {
        int has_separator = parent_arc[parent_len - 1] == '/';
        size_t needed = parent_len + (has_separator ? 0U : 1U) + component_len;
        if (needed >= outsz) return BP_ERR_NAME_TOO_LONG;
        memcpy(out, parent_arc, parent_len);
        if (!has_separator) out[parent_len++] = '/';
        memcpy(out + parent_len, component, component_len + 1U);
    } else {
        if (component_len >= outsz) return BP_ERR_NAME_TOO_LONG;
        memcpy(out, component, component_len + 1U);
    }
    return BP_OK;
}

/*
 * Extract the QDOS name field from a qdirect entry.
 * d_szname is big-endian but on M68k host it's already correct.
 * Space-pad is stripped; a NUL is appended.
 */
static int qdirect_get_name(const qdirect_t *e, const char *qdos_dir,
                            char *buf, int bufsz)
{
    int n = (int)(unsigned short)e->d_szname;  /* big-endian on M68k */
    const char *parent;
    size_t parent_length;
    if (n == 0) return 0; /* QDOS directory-stream end marker */
    if (n > QDOS_NAME_MAX || n >= bufsz) return -1;
    strncpy(buf, e->d_name, (size_t)n);
    buf[n] = '\0';
    /* Some qdirect providers return a device-relative full path, while
     * others return only the child component. Normalize both forms. */
    parent = ql_strip_device(qdos_dir);
    parent_length = strlen(parent);
    if (parent_length != 0 && (size_t)n >= parent_length &&
        ql_ascii_equal_prefix(buf, parent, parent_length)) {
        memmove(buf, buf + parent_length, (size_t)n - parent_length + 1U);
        n -= (int)parent_length;
    }
    if (n == 0) return -1;
    return n;
}

/*
 * True if the qdirect entry represents a sub-directory.
 * QDOS identifies directories by the trailing '_' in their name.
 */
static int qdirect_is_dir(const qdirect_t *e, const char *qdos_dir)
{
    char name[QDOS_NAME_MAX + 1]; int n;
    n = (int)(unsigned short)e->d_szname;
    if (n <= 0 || n > QDOS_NAME_MAX) return 0;
    if (e->d_type == 255U) return 1; /* QF_DIR_TYPE */
    n = qdirect_get_name(e, qdos_dir, name, sizeof(name));
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

static int mkdirs_for_file(const char *target, const char *member_path)
{
    const char *slash = strrchr(member_path, '/');
    const char *leaf;
    char parent[BP_MAX_PATH];
    size_t target_length, leaf_length, parent_length;
    if (slash == NULL) return 0;
    leaf = strrchr(member_path, '/');
    leaf = leaf ? leaf + 1 : member_path;
    target_length = strlen(target);
    leaf_length = strlen(leaf);
    if (target_length < leaf_length) return -1;
    parent_length = target_length - leaf_length;
    if (parent_length == 0 || parent_length >= sizeof(parent)) return -1;
    memcpy(parent, target, parent_length);
    parent[parent_length] = '\0';
    return ql_mkdir(parent);
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
                         const char *arcname, long qdos_time,
                         int qdos_time_known);

static int add_directory_contents(bp_archive_t *ar, const char *qdos_dir,
                                  const char *arc_prefix)
{
#ifdef BP_QL_HOST_TEST
    FILE      *dp;
#else
    long        dp;
    int         io_result;
#endif
    qdirect_t  ent;
    char       ent_name[QDOS_NAME_MAX + 1];
    char       child_component[QDOS_NAME_MAX + 2];
    char       child_qdos[BP_MAX_PATH];
    char       child_arc[BP_MAX_PATH];
    int        rc = BP_OK;
    int        is_directory;
    int        name_length;
#ifdef BP_QL_HOST_TEST
    size_t     record_bytes;
#endif

#ifdef BP_QL_HOST_TEST
    dp = fopen(qdos_dir, "r");
    if (!dp) {
        fprintf(stderr, "boltpack: cannot open directory: %s\n", qdos_dir);
        return BP_ERR_IO;
    }
#else
    dp = _io_open(qdos_dir, QL_OPEN_DIR);
    if (dp < 0) {
        fprintf(stderr, "boltpack: cannot open directory: %s\n", qdos_dir);
        return BP_ERR_IO;
    }
#endif

    while (rc == BP_OK) {
#ifdef BP_QL_HOST_TEST
        record_bytes = fread(&ent, 1, sizeof(ent), dp);
        if (record_bytes == 0) break;
        if (record_bytes != sizeof(ent)) {
            rc = BP_ERR_CORRUPT;
            break;
        }
#else
        io_result = _io_fstrg(dp, (short)-1, &ent, (short)sizeof(ent));
        if (io_result == QL_EOF) break;
        if (io_result < 0) {
            rc = BP_ERR_IO;
            break;
        }
        if (io_result != (int)sizeof(ent)) {
            rc = BP_ERR_CORRUPT;
            break;
        }
#endif
        name_length = qdirect_get_name(&ent, qdos_dir, ent_name,
                                       sizeof(ent_name));
        if (name_length == 0) break;
        if (name_length < 0) {
            rc = BP_ERR_CORRUPT;
            break;
        }

        is_directory = qdirect_is_dir(&ent, qdos_dir);
        strcpy(child_component, ent_name);
        if (is_directory && child_component[name_length - 1] != '_') {
            child_component[name_length] = '_';
            child_component[name_length + 1] = '\0';
        }
        /* Build full QDOS path: device path ends with '_', so just append */
        if (ql_path_join2(child_qdos, sizeof(child_qdos), qdos_dir,
                          child_component) != 0) {
            rc = BP_ERR_NAME_TOO_LONG;
            break;
        }

        /* Use the qdirect parent relation, rather than guessing that an
         * embedded underscore is a path separator. */
        rc = ql_to_arcname(ent_name, is_directory, child_arc,
                           sizeof(child_arc), arc_prefix);
        if (rc != BP_OK) break;

        if (is_directory) {
            /* Recurse into subdirectory.
             * qdos_path ends with '_', so child is qdos_dir + "subname_"
             * which is already in child_qdos. */
            char dir_arc[BP_MAX_PATH];
            bp_uint64 mtime;
            bp_uint16 mtime_flags;
            /* Ensure arc path ends with '/' */
            if (ql_path_join2(dir_arc, sizeof(dir_arc), child_arc, "/") != 0) {
                rc = BP_ERR_NAME_TOO_LONG;
                break;
            }

            if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", dir_arc);
            mtime_flags = qdos_to_archive_time(ent.d_update, &mtime);
            rc = bp_archive_add_entry_ex(ar, dir_arc, BP_TYPE_DIR, BP_OS_QL,
                                         NULL, bp_u64_from_u32(0), mtime,
                                         mtime_flags);
            if (rc == BP_OK)
                rc = add_directory_contents(ar, child_qdos, dir_arc);
        } else {
            rc = add_path_qdos(ar, child_qdos, child_arc, ent.d_update, 1);
        }
    }

#ifdef BP_QL_HOST_TEST
    fclose(dp);
#else
    if (_io_close(dp) < 0 && rc == BP_OK) rc = BP_ERR_IO;
#endif
    return rc;
}

static int add_path_qdos(bp_archive_t *ar, const char *qdos_path,
                         const char *arcname, long qdos_time,
                         int qdos_time_known)
{
    FILE      *fp;
    long       sz;
    bp_io_t    fio;
    bp_uint64  fsize;
    bp_uint64  mtime;
    bp_uint16  mtime_flags;
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

    /* A direct file argument has no parent qdirect record, so its timestamp
     * is unknown. Recursive callers pass the timestamp from that record. */
    if (qdos_time_known)
        mtime_flags = qdos_to_archive_time(qdos_time, &mtime);
    else {
        mtime = bp_u64_from_u32(0);
        mtime_flags = 0;
    }

    bp_io_from_file(&fio, fp);

    if (g_verbosity == VERB_DEFAULT) { printf("compressing: %s", arcname); fflush(stdout); }

    rc = bp_archive_add_entry_ex(ar, arcname, BP_TYPE_DATA, BP_OS_QL,
                                 &fio, fsize, mtime, mtime_flags);
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
        bp_uint64 mtime = bp_u64_from_u32(0);
        bp_uint16 mtime_flags = 0;
        int  al;
        int  rc;

        if (ql_project_component(arcname, 1, dir_arc,
                                 sizeof(dir_arc) - 2) != 0)
            return BP_ERR_INVALID_ARG;
        al = (int)strlen(dir_arc);
        if (al == 0 || dir_arc[al - 1] != '/')
            { dir_arc[al] = '/'; dir_arc[al + 1] = '\0'; }

        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", dir_arc);
        rc = bp_archive_add_entry_ex(ar, dir_arc, BP_TYPE_DIR, BP_OS_QL,
                                     NULL, bp_u64_from_u32(0), mtime,
                                     mtime_flags);
        if (rc != BP_OK) return rc;
        return add_directory_contents(ar, qdos_path, dir_arc);
    }
    return add_path_qdos(ar, qdos_path, arcname, 0, 0);
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
    const char *archive_path;
    int         force;
    int         dry_run;
    int         failed;
    unsigned long stage_serial;
    char        target[BP_MAX_PATH];
    char        temporary[BP_MAX_PATH];
    char        backup[BP_MAX_PATH];
    bp_uint32   file_count;
    bp_uint32   dir_count;
} extract_ctx_t;

typedef struct {
    char path[QDOS_NAME_MAX + 1];
    unsigned char type;
} ql_extract_name_t;

typedef struct {
    const char *out_dir;
    const char *archive_path;
    ql_extract_name_t *names;
    bp_uint32 count;
    bp_uint32 capacity;
    int error;
} ql_extract_index_t;

static int ql_build_output_path(const char *out_dir, const char *local_name,
                                char *path, size_t path_size)
{
    size_t out_length;
    if (out_dir == NULL || out_dir[0] == '\0') {
        if (strlen(local_name) >= path_size) return -1;
        strcpy(path, local_name);
        return 0;
    }
    out_length = strlen(out_dir);
    if (out_dir[out_length - 1] == '/')
        return ql_path_join2(path, path_size, out_dir, local_name);
    return ql_path_join3(path, path_size, out_dir, "/", local_name);
}

static int ql_build_extract_path(const char *out_dir, const char *archive_path,
                                 const char *member_path, char *path,
                                 size_t path_size)
{
#ifdef BP_QL_HOST_TEST
    (void)archive_path;
    return ql_build_output_path(out_dir, member_path, path, path_size);
#else
    return ql_native_output_path(out_dir, archive_path, member_path, path,
                                 path_size);
#endif
}

typedef struct {
    const char *out_dir;
    const char *archive_path;
    int partial;
    int find_depth;
    int max_depth;
    int restore_depth;
} ql_dir_time_ctx_t;

static int ql_restore_directory_time(const bp_entry_t *entry, void *user_data)
{
    ql_dir_time_ctx_t *ctx = (ql_dir_time_ctx_t *)user_data;
    char local_name[BP_MAX_PATH];
    char path[BP_MAX_PATH];
    int depth = 0;
    const char *p;
    if (entry->entry_type != BP_TYPE_DIR ||
        !(entry->flags & BP_ENTRY_FLAG_MTIME_VALID))
        return 0;
    for (p = entry->name; *p != '\0'; p++)
        if (*p == '/') depth++;
    if (ctx->find_depth) {
        if (depth > ctx->max_depth) ctx->max_depth = depth;
        return 0;
    }
    if (depth != ctx->restore_depth) return 0;
    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name),
                         QL_CP) != 0 ||
        ql_build_extract_path(ctx->out_dir, ctx->archive_path, local_name,
                              path, sizeof(path)) != 0 ||
        ql_set_archive_time(path, entry->mtime) != BP_OK) {
        fprintf(stderr, "boltpack: cannot restore QDOS directory date: %s\n",
                entry->name);
        ctx->partial = 1;
    }
    return 0;
}

static int ql_restore_directory_times(const char *archive_path,
                                      const char *out_dir)
{
    ql_dir_time_ctx_t ctx;
    int depth;
    int rc;
    ctx.out_dir = out_dir;
    ctx.archive_path = archive_path;
    ctx.partial = 0;
    ctx.find_depth = 1;
    ctx.max_depth = 0;
    ctx.restore_depth = 0;
    {
        FILE *fp = fopen(archive_path, "rb");
        bp_io_t io;
        if (fp == NULL) return BP_ERR_PARTIAL;
        bp_io_from_file(&io, fp);
        rc = bp_archive_list(&io, ql_restore_directory_time, &ctx);
        fclose(fp);
    }
    if (rc != BP_OK) return BP_ERR_PARTIAL;
    ctx.find_depth = 0;
    for (depth = ctx.max_depth; depth > 0; depth--) {
        FILE *fp = fopen(archive_path, "rb");
        bp_io_t io;
        if (fp == NULL) return BP_ERR_PARTIAL;
        bp_io_from_file(&io, fp);
        ctx.restore_depth = depth;
        rc = bp_archive_list(&io, ql_restore_directory_time, &ctx);
        fclose(fp);
        if (rc != BP_OK) ctx.partial = 1;
    }
    if (ctx.partial) return BP_ERR_PARTIAL;
    return BP_OK;
}

static unsigned char ql_name_fold(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z') return (unsigned char)(ch + ('a' - 'A'));
    /* QDOS/SMS filenames are normally case-insensitive. Fold the one-byte
     * Latin-1 uppercase ranges as well as ASCII before collision checks. */
    if ((ch >= 0xC0 && ch <= 0xD6) || (ch >= 0xD8 && ch <= 0xDE))
        return (unsigned char)(ch + 0x20);
    return ch;
}

static int ql_extract_name_compare(const void *left, const void *right)
{
    const ql_extract_name_t *a = (const ql_extract_name_t *)left;
    const ql_extract_name_t *b = (const ql_extract_name_t *)right;
    return strcmp(a->path, b->path);
}

static int ql_extract_index_add(const bp_entry_t *entry, void *user_data)
{
    ql_extract_index_t *index = (ql_extract_index_t *)user_data;
    ql_extract_name_t *grown;
    char local_name[BP_MAX_PATH];
    char output_path[BP_MAX_PATH];
    size_t length, i, allocation_size;
    bp_uint32 capacity;

    if (entry->entry_type != BP_TYPE_DATA && entry->entry_type != BP_TYPE_DIR)
        return 0;
    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name), QL_CP) != 0) {
        fprintf(stderr, "boltpack: name is not representable on QL: %s\n",
                entry->name);
        index->error = BP_ERR_INVALID_ARG;
        return 1;
    }
    length = strlen(local_name);
    if (ql_build_extract_path(index->out_dir, index->archive_path, local_name,
                              output_path, sizeof(output_path)) != 0) {
        fprintf(stderr, "boltpack: output path is too long for QL: %s\n",
                entry->name);
        index->error = BP_ERR_NAME_TOO_LONG;
        return 1;
    }
    /* The final slash maps to QDOS's trailing underscore for directories and
     * occupies a byte in the native pathname limit. */
    if (length > QDOS_NAME_MAX) {
        fprintf(stderr, "boltpack: output path is too long for QL: %s\n",
                entry->name);
        index->error = BP_ERR_NAME_TOO_LONG;
        return 1;
    }
    if (entry->entry_type == BP_TYPE_DIR && length != 0 &&
        local_name[length - 1] == '/')
        local_name[--length] = '\0';
    /* QDOS/SMS stores the complete relative directory and file path in a
     * 36-byte name field. Refuse it before creating any output. */
    if (length == 0) {
        fprintf(stderr, "boltpack: output path is too long for QL: %s\n",
                entry->name);
        index->error = BP_ERR_NAME_TOO_LONG;
        return 1;
    }
    if (index->count == index->capacity) {
        capacity = index->capacity == 0 ? 16U : index->capacity * 2U;
        allocation_size = (size_t)capacity * sizeof(*grown);
        if (capacity < index->capacity ||
            allocation_size / sizeof(*grown) != (size_t)capacity) {
            index->error = BP_ERR_OVERFLOW;
            return 1;
        }
        grown = (ql_extract_name_t *)realloc(
            index->names, allocation_size);
        if (grown == NULL) {
            index->error = BP_ERR_NOMEM;
            return 1;
        }
        index->names = grown;
        index->capacity = capacity;
    }
    memcpy(index->names[index->count].path, local_name, length + 1U);
    for (i = 0; i < length; i++)
        index->names[index->count].path[i] =
            (char)ql_name_fold((unsigned char)index->names[index->count].path[i]);
    index->names[index->count].type = entry->entry_type;
    index->count++;
    return 0;
}

static int ql_extract_preflight(bp_io_t *archive, const char *out_dir,
                                const char *archive_path)
{
    ql_extract_index_t index;
    bp_uint32 i;
    int rc;

    memset(&index, 0, sizeof(index));
    index.out_dir = out_dir;
    index.archive_path = archive_path;
    rc = bp_archive_list(archive, ql_extract_index_add, &index);
    if (rc == BP_ERR_ABORTED && index.error != 0) rc = index.error;
    if (rc == BP_OK) {
        if (index.count > 1U)
            qsort(index.names, index.count, sizeof(*index.names),
                  ql_extract_name_compare);
        for (i = 0; i < index.count; i++) {
            size_t length = strlen(index.names[i].path);
            if (i != 0 && strcmp(index.names[i - 1].path,
                                 index.names[i].path) == 0) {
                fprintf(stderr, "boltpack: destination name collision: %s\n",
                        index.names[i].path);
                rc = BP_ERR_PARTIAL;
                break;
            }
            if (index.names[i].type == BP_TYPE_DATA && length + 1U <
                sizeof(index.names[i].path)) {
                char prefix[QDOS_NAME_MAX + 2];
                bp_uint32 low = i + 1U, high = index.count;
                size_t prefix_len;
                memcpy(prefix, index.names[i].path, length);
                prefix[length] = '/';
                prefix[length + 1U] = '\0';
                prefix_len = length + 1U;
                while (low < high) {
                    bp_uint32 middle = low + (high - low) / 2U;
                    if (strcmp(index.names[middle].path, prefix) < 0)
                        low = middle + 1U;
                    else
                        high = middle;
                }
                if (low < index.count &&
                    strncmp(index.names[low].path, prefix, prefix_len) == 0) {
                    fprintf(stderr,
                            "boltpack: file/directory name conflict: %s\n",
                            index.names[i].path);
                    rc = BP_ERR_PARTIAL;
                    break;
                }
            }
        }
    }
    free(index.names);
    return rc;
}

/* Build a same-directory stage/backup name within the frontend path buffer. */
static int extract_sibling_name(const char *target, size_t target_leaf_length,
                                const char *leaf, char *out, size_t out_size)
{
    size_t target_length = strlen(target);
    size_t parent_length;
    size_t leaf_length = strlen(leaf);
    if (target_length < target_leaf_length) return -1;
    parent_length = target_length - target_leaf_length;
    if (parent_length + leaf_length >= out_size) return -1;
    memcpy(out, target, parent_length);
    memcpy(out + parent_length, leaf, leaf_length + 1U);
    return 0;
}

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char  path[BP_MAX_PATH];
    char  local_name[BP_MAX_PATH];
    const char *local_leaf;
    FILE *fp; bp_io_t *io;

    if (!bp_name_is_safe(entry->name)) return NULL;

    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name),
                         QL_CP) != 0) return NULL;

    if (ql_build_extract_path(ctx->out_dir, ctx->archive_path, local_name,
                              path, sizeof(path)) != 0) {
        fprintf(stderr, "boltpack: output path too long: %s\n", entry->name);
        return NULL;
    }

    if (entry->entry_type == BP_TYPE_DIR) {
        ctx->dir_count++;
        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", local_name);
        if (!ctx->dry_run && mkdirs(path) != 0) {
            fprintf(stderr, "boltpack: cannot create directory: %s\n", path);
            ctx->failed = 1;
        }
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
    if (ctx->dry_run) return bp_archive_dry_run_stream();

    if (!ctx->force) {
        FILE *chk = fopen(path, "rb");
        if (chk) { fclose(chk); fprintf(stderr, "boltpack: exists (use -f): %s\n", path); return NULL; }
    }

    if (mkdirs_for_file(path, local_name) != 0) {
        fprintf(stderr, "boltpack: cannot create parent directory: %s\n", path);
        ctx->failed = 1;
        return NULL;
    }
    /* Keep incomplete data invisible until the core reports a verified
     * checksum and fclose succeeds. Create each short sibling name
     * exclusively so concurrent extractions cannot truncate the same stage. */
    {
        unsigned long tries;
        unsigned long candidate_limit;
        size_t leaf_length;
        int opened = 0;
        local_leaf = strrchr(local_name, '/');
        local_leaf = local_leaf ? local_leaf + 1 : local_name;
        leaf_length = ql_stage_leaf_length_for_target(path,
                                                     strlen(local_leaf));
        if (leaf_length == 0U) {
            fprintf(stderr, "boltpack: no room for QL staging name: %s\n", path);
            ctx->failed = 1;
            return NULL;
        }
        candidate_limit = ql_stage_candidate_count(leaf_length);
        for (tries = 0; tries < candidate_limit; tries++) {
            char temp_leaf[16], backup_leaf[16];
            FILE *probe;
            int fd;
            int flags;
            unsigned long serial = ctx->stage_serial++ % candidate_limit;
            ql_stage_leaf('B', serial, leaf_length, temp_leaf);
            if (leaf_length == 1U)
                ql_stage_leaf('K', (serial + 1UL) % 36UL,
                              leaf_length, backup_leaf);
            else
                ql_stage_leaf('K', serial, leaf_length, backup_leaf);
            if (extract_sibling_name(path, strlen(local_leaf), temp_leaf,
                                     ctx->temporary,
                                     sizeof(ctx->temporary)) != 0 ||
                extract_sibling_name(path, strlen(local_leaf), backup_leaf,
                                     ctx->backup,
                                     sizeof(ctx->backup)) != 0) {
                fprintf(stderr, "boltpack: staging path too long: %s\n", path);
                return NULL;
            }
            probe = fopen(ctx->backup, "rb");
            if (probe) { fclose(probe); continue; }
            flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_BINARY
            flags |= O_BINARY;
#endif
            fd = open(ctx->temporary, flags, 0666);
            if (fd < 0) continue;
            fp = fdopen(fd, "wb");
            if (!fp) {
                close(fd);
                remove(ctx->temporary);
                continue;
            }
            opened = 1;
            break;
        }
        if (!opened) {
            fprintf(stderr, "boltpack: cannot create staging file for: %s\n", path);
            ctx->failed = 1;
            return NULL;
        }
    }
    strcpy(ctx->target, path);
    io = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (!io) { fclose(fp); remove(ctx->temporary); ctx->failed = 1; return NULL; }
    bp_io_from_file(io, fp);
    return io;
}

static int extract_done(const bp_entry_t *entry, bp_io_t *out, int status,
                        void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    int rc = BP_OK;
    int had_target = 0;
    if (out) {
        FILE *fp = (FILE *)out->handle;
        if (fclose(fp) != 0) status = BP_ERR_IO;
        free(out);
    }
    (void)entry;
    if (status != BP_OK) {
        remove(ctx->temporary);
        return status;
    }
    if (ctx->force) {
        FILE *probe = fopen(ctx->target, "rb");
        if (probe) {
            fclose(probe);
            if (rename(ctx->target, ctx->backup) != 0) {
                remove(ctx->temporary);
                fprintf(stderr, "boltpack: cannot preserve existing target: %s\n", ctx->target);
                return BP_ERR_IO;
            }
            had_target = 1;
        }
    } else {
        FILE *probe = fopen(ctx->target, "rb");
        if (probe) {
            fclose(probe);
            remove(ctx->temporary);
            fprintf(stderr, "boltpack: exists (use -f): %s\n", ctx->target);
            return BP_ERR_PARTIAL;
        }
    }
    {
        int publish_result;
        int rollback_result = 0;
#ifdef BP_QL_HOST_TEST
        /* Exercise recovery after the old target has moved aside without
         * changing the native QDOS implementation or its runtime. */
        const char *fail_publish = getenv("BP_QL_FAIL_PUBLISH");
        if (fail_publish != NULL && fail_publish[0] == '1' &&
            fail_publish[1] == '\0')
            publish_result = -1;
        else
#endif
            publish_result = rename(ctx->temporary, ctx->target);
        if (publish_result != 0) {
            if (had_target) {
#ifdef BP_QL_HOST_TEST
                const char *fail_rollback = getenv("BP_QL_FAIL_ROLLBACK");
                if (fail_rollback != NULL && fail_rollback[0] == '1' &&
                    fail_rollback[1] == '\0')
                    rollback_result = -1;
                else
#endif
                    rollback_result = rename(ctx->backup, ctx->target);
            }
            if (had_target && rollback_result != 0)
                fprintf(stderr, "boltpack: recovery copy retained at: %s\n", ctx->backup);
            else
                remove(ctx->temporary);
            fprintf(stderr, "boltpack: cannot publish extracted file: %s\n", ctx->target);
            return BP_ERR_IO;
        }
    }
    if (had_target && remove(ctx->backup) != 0) {
        fprintf(stderr, "boltpack: published file but cannot remove backup: %s\n", ctx->backup);
        rc = BP_ERR_PARTIAL;
    }
    if ((entry->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
        ql_set_archive_time(ctx->target, entry->mtime) != BP_OK) {
        fprintf(stderr, "boltpack: cannot restore QDOS file date: %s\n",
                entry->name);
        rc = BP_ERR_PARTIAL;
    }
    return rc;
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
        "  --interchange-profile-1  create/add mini-compatible archive\n"
        "  -o PATH   output path for extraction\n"
        "  -f        force overwrite    -n dry run\n",
        BP_VERSION_MAJOR, BP_VERSION_MINOR, AUTHOR_MESSAGE);
    fprintf(stderr,
        "  --tz-offset MINUTES     fixed local offset east of UTC\n"
        "\nNote: device prefixes (win1_, mdv1_ etc.) are stripped from archive names.\n");
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
    rc = g_interchange_profile
        ? bp_archive_create_profile(&ar, &aio, BP_PROFILE_INTERCHANGE_1)
        : bp_archive_create(&ar, &aio, block_size, method);
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    if (g_interchange_profile && method == BP_METHOD_STORED)
        ar.method = BP_METHOD_STORED;
    for (i = 1; i < argc; i++) {
        /* Strip device prefix for archive name */
        const char *arcname = ql_strip_device(argv[i]);
        /* Convert QDOS path separators '_' to '/' in archive name */
        char arcbuf[BP_MAX_PATH];
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
    if (g_interchange_profile) {
        rc = bp_archive_use_profile(&ar, BP_PROFILE_INTERCHANGE_1);
        if (rc != BP_OK) {
            fprintf(stderr, "boltpack: archive does not meet Interchange Profile 1 limits: %s\n",
                    bp_strerror(rc));
            bp_archive_close(&ar); fclose(fp); return 1;
        }
        if (method == BP_METHOD_STORED) ar.method = BP_METHOD_STORED;
    }
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
#ifndef BP_QL_HOST_TEST
    char output_root[BP_MAX_PATH];
#endif
    fp = fopen(archive_name, "rb");
    if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", archive_name); return 1; }
    rc = bp_compress_init();
    if (rc != BP_OK) { fprintf(stderr, "boltpack: %s\n", bp_strerror(rc)); fclose(fp); return 1; }
    bp_io_from_file(&aio, fp);
    rc = ql_extract_preflight(&aio, out_dir, archive_name);
    if (rc != BP_OK) {
        if (rc != BP_ERR_PARTIAL)
            fprintf(stderr, "boltpack: cannot preflight archive: %s\n",
                    bp_strerror(rc));
        fclose(fp);
        return 1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "boltpack: cannot rewind archive\n");
        fclose(fp);
        return 1;
    }
    if (out_dir && !dry_run) {
#ifdef BP_QL_HOST_TEST
        if (mkdirs(out_dir) != 0) {
#else
        if (ql_native_output_path(out_dir, archive_name, "", output_root,
                                  sizeof(output_root)) != 0 ||
            ql_mkdir(output_root) != 0) {
#endif
            fprintf(stderr, "boltpack: cannot create output directory: %s\n",
                    out_dir);
            fclose(fp);
            return 1;
        }
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.out_dir = out_dir; ctx.archive_path = archive_name;
    ctx.force = force; ctx.dry_run = dry_run;
    rc = bp_archive_extract_all_ex(&aio, extract_open, extract_done, NULL, &ctx);
    fclose(fp);
    if (ctx.failed && rc == BP_OK) rc = BP_ERR_PARTIAL;
    if (!dry_run && (rc == BP_OK || rc == BP_ERR_PARTIAL) &&
        ql_restore_directory_times(archive_name, out_dir) != BP_OK)
        rc = BP_ERR_PARTIAL;
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
    if (ql_path_join2(tmp, sizeof(tmp), argv[0], ".tmp") != 0) {
        fclose(sf);
        fprintf(stderr, "boltpack: archive path too long for delete stage\n");
        return 1;
    }
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
    int        codec_given = 0;

    if (argc < 3) { usage(); return 1; }
    if (strlen(argv[1]) != 1) {
        fprintf(stderr, "boltpack: unknown command: %s\n", argv[1]); usage(); return 1;
    }
    command = argv[1][0];
    i = 2;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "--tz-offset")) {
            if (++i >= argc ||
                !bp_ql_parse_tz_offset(argv[i], &ql_tz_override_seconds)) {
                fprintf(stderr,
                        "boltpack: --tz-offset needs minutes in -1439..1439\n");
                return 1;
            }
            ql_tz_override_set = 1;
            i++;
        } else if (!strcmp(argv[i], "-b")) {
            if (++i >= argc) { fprintf(stderr, "boltpack: -b needs arg\n"); return 1; }
            block_size = parse_block_size(argv[i++]);
            if (!block_size) { fprintf(stderr, "boltpack: invalid block size\n"); return 1; }
        } else if (!strcmp(argv[i], "--interchange-profile-1")) {
            g_interchange_profile = 1; i++;
        } else if (!strcmp(argv[i], "-0"))  { method = BP_METHOD_STORED; codec_given = 1; i++; }
        else if (!strcmp(argv[i], "-F"))  { method = BP_METHOD_FLUFF; codec_given = 1; i++; }
        else if (!strcmp(argv[i], "-qq")) { g_verbosity = VERB_SILENT; i++; }
        else if (!strcmp(argv[i], "-q"))  { g_verbosity = VERB_QUIET;  i++; }
        else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) { fprintf(stderr, "boltpack: -o needs arg\n"); return 1; }
            out_dir = argv[i++];
        } else if (!strcmp(argv[i], "-f")) { force = 1; i++; }
        else if (!strcmp(argv[i], "-n"))   { dry_run = 1; i++; }
        else { fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]); usage(); return 1; }
    }

    if (g_interchange_profile) {
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
