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

/* Human68k system call interface */
#include <x68k/dos.h>

#include <stdio.h>

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

/* ---- DOS-packed timestamp to Unix seconds ----------------------------- */

static bp_uint32 dos_to_unix_time(unsigned short dos_time, unsigned short dos_date)
{
    unsigned yr  = ((unsigned)dos_date >> 9) + 1980;
    unsigned mon = ((unsigned)dos_date >> 5) & 0x0F;
    unsigned day = (unsigned)dos_date & 0x1F;
    unsigned hr  = (unsigned)dos_time >> 11;
    unsigned min = ((unsigned)dos_time >> 5) & 0x3F;
    unsigned sec = ((unsigned)dos_time & 0x1F) * 2u;
    unsigned long days = (yr - 1970UL) * 365UL + (yr - 1969UL) / 4UL
                       + (mon > 1 ? (mon - 1) * 30UL : 0UL) + day;
    return (bp_uint32)(days * 86400UL + hr * 3600UL + min * 60UL + sec);
}

/* ---- path utilities --------------------------------------------------- */

/*
 * Convert backslashes to forward slashes (archive format uses '/').
 */
static void normalise_seps(char *p)
{
    for (; *p; p++) if (*p == '\\') *p = '/';
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
    size_t n = strlen(s);
    return n > 0 && (s[n-1] == '/' || s[n-1] == '\\');
}

static void path_join(char *buf, size_t sz, const char *dir, const char *name)
{
    if (ends_with_sep(dir)) snprintf(buf, sz, "%s%s", dir, name);
    else                    snprintf(buf, sz, "%s\\%s", dir, name);
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
    char  tmp[BP_MAX_PATH]; char *sl, *bsl;
    if (strlen(path) >= sizeof(tmp)) return -1;
    strcpy(tmp, path);
    sl  = strrchr(tmp, '/');
    bsl = strrchr(tmp, '\\');
    if (bsl && (!sl || bsl > sl)) sl = bsl;
    if (sl && sl != tmp) { char *x; *sl = '\0'; for (x = tmp; *x; x++) if (*x=='/') *x='\\'; return mkdirs(tmp); }
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

    snprintf(pattern, sizeof(pattern), "%s\\*.*", dir_path);
    r = _dos_files(&fb, pattern, FA_ANY);
    while (r == 0 && rc == BP_OK) {
        if (strcmp(fb.name, ".") != 0 && strcmp(fb.name, "..") != 0) {
            path_join(child_path, sizeof(child_path), dir_path, fb.name);
            path_join(child_arc,  sizeof(child_arc),  arc_prefix, fb.name);
            normalise_seps(child_arc);
            rc = add_path(ar, child_path, child_arc);
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
        int  al; int rc;
        snprintf(dir_arc, sizeof(dir_arc), "%s", arcname);
        al = (int)strlen(dir_arc);
        if (al > 0 && dir_arc[al - 1] != '/')
            { dir_arc[al] = '/'; dir_arc[al+1] = '\0'; }

        if (g_verbosity == VERB_DEFAULT) printf("   creating: %s\n", dir_arc);
        rc = bp_archive_add_entry(ar, dir_arc, BP_TYPE_DIR, BP_OS_X68K,
                                  NULL, bp_u64_from_u32(0),
                                  bp_u64_from_u32(dos_to_unix_time(fb.time, fb.date)));
        if (rc != BP_OK) return rc;
        return add_directory_contents(ar, path, dir_arc);
    }

    /* Regular file */
    {
        FILE     *fp;
        bp_io_t   fio;
        bp_uint64 fsize = bp_u64_from_ulong((unsigned long)fb.filelen);
        bp_uint32 mtime = dos_to_unix_time(fb.time, fb.date);
        char      arcname_sjis[BP_MAX_PATH];
        char      arcname_utf8[BP_MAX_PATH * 3];
        int       rc;

        fp = fopen(path, "rb");
        if (!fp) { fprintf(stderr, "boltpack: cannot open: %s\n", path); return BP_ERR_IO; }
        bp_io_from_file(&fio, fp);

        /* Convert Shift-JIS archive name to UTF-8 for storage */
        strncpy(arcname_sjis, arcname, sizeof(arcname_sjis) - 1);
        arcname_sjis[sizeof(arcname_sjis) - 1] = '\0';
        if (bp_local_to_utf8(arcname_sjis, arcname_utf8, sizeof(arcname_utf8), X68K_CP) != 0)
            strcpy(arcname_utf8, arcname_sjis);

        if (g_verbosity == VERB_DEFAULT) { printf("compressing: %s", arcname_sjis); fflush(stdout); }

        rc = bp_archive_add_entry(ar, arcname_utf8, BP_TYPE_DATA, BP_OS_X68K,
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
    bp_uint32   file_count;
    bp_uint32   dir_count;
} extract_ctx_t;

static bp_io_t *extract_open(const bp_entry_t *entry, void *user_data)
{
    extract_ctx_t *ctx = (extract_ctx_t *)user_data;
    char  path[BP_MAX_PATH];
    char  local_name[BP_MAX_PATH];
    char  qdos_path[BP_MAX_PATH];  /* with backslashes */
    FILE *fp; bp_io_t *io;

    /* Convert UTF-8 archive name to SJIS */
    if (bp_utf8_to_local(entry->name, local_name, sizeof(local_name), X68K_CP) != 0)
        strncpy(local_name, entry->name, sizeof(local_name) - 1);

    /* Build output path with backslashes */
    if (ctx->out_dir) {
        snprintf(path, sizeof(path), "%s\\%s", ctx->out_dir, local_name);
        path[sizeof(path) - 1] = '\0';
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
        if (!ctx->dry_run) { char dmk[BP_MAX_PATH]; strcpy(dmk, qdos_path);
          { int dn = (int)strlen(dmk); if (dn > 0 && dmk[dn-1]=='\\') dmk[dn-1]='\0'; }
          mkdirs(dmk); }
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
    if (ctx->dry_run) return NULL;

    if (!ctx->force) {
        FILE *chk = fopen(qdos_path, "rb");
        if (chk) { fclose(chk); fprintf(stderr, "boltpack: exists (use -f): %s\n", local_name); return NULL; }
    }

    /* Create parent directories */
    { char tmp2[BP_MAX_PATH]; char *sl; strcpy(tmp2, qdos_path);
      sl = strrchr(tmp2, '\\');
      if (sl && sl != tmp2) { *sl = '\0'; mkdirs(tmp2); } }

    fp = fopen(qdos_path, "wb");
    if (!fp) { fprintf(stderr, "boltpack: cannot create: %s\n", local_name); return NULL; }
    io = (bp_io_t *)malloc(sizeof(bp_io_t));
    if (!io) { fclose(fp); return NULL; }
    bp_io_from_file(io, fp);
    return io;
}

static void extract_done(const bp_entry_t *entry, bp_io_t *out, void *user_data)
{
    (void)user_data; (void)entry;
    if (out) { fclose((FILE *)out->handle); free(out); }
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
        char buf[BP_MAX_PATH];
        strcpy(buf, clean_path(argv[i]));
        normalise_seps(buf);
        rc = add_path(&ar, argv[i], buf);
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
        char buf[BP_MAX_PATH]; strcpy(buf, clean_path(argv[i])); normalise_seps(buf);
        rc = add_path(&ar, argv[i], buf);
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
    if (out_dir && !dry_run) {
        char od[BP_MAX_PATH]; strncpy(od, out_dir, sizeof(od)-1); mkdirs(od);
    }
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
    snprintf(tmp, sizeof(tmp), "%s.tmp", argv[0]);
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
            if (++i >= argc) { fprintf(stderr, "boltpack: -b needs arg\n"); return 1; }
            block_size = parse_block_size(argv[i++]);
            if (!block_size) { fprintf(stderr, "boltpack: invalid block size\n"); return 1; }
        } else if (!strcmp(argv[i], "-0"))  { method = BP_METHOD_STORED; i++; }
        else if (!strcmp(argv[i], "-qq")) { g_verbosity = VERB_SILENT; i++; }
        else if (!strcmp(argv[i], "-q"))  { g_verbosity = VERB_QUIET;  i++; }
        else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) { fprintf(stderr, "boltpack: -o needs arg\n"); return 1; }
            out_dir = argv[i++];
        } else if (!strcmp(argv[i], "-f")) { force = 1; i++; }
        else if (!strcmp(argv[i], "-n"))   { dry_run = 1; i++; }
        else { fprintf(stderr, "boltpack: unknown option: %s\n", argv[i]); usage(); return 1; }
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
