/*
 * archiver.c -- archive format read/write for BoltPack
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "archiver.h"
#include "archive_name.h"
#include "bp_metadata.h"
#include "fluff.h"
#include "compress.h"
#include "boltlite.h"
#ifdef BOLTPACK_ZSTD
#include "zstd_wrap.h"
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef BOLTPACK_FAST_FLUFF
#include <omp.h>
#endif

/*
 * Sanity ceiling for block sizes read from entry headers during
 * extraction.  The format specification caps blocks at 256 KB
 * (BP_BLOCK_256K); we allow generous headroom for nonstandard archives
 * while preventing a crafted header from demanding multi-gigabyte
 * allocations.
 */
#define BP_BLOCK_READ_CAP  (16UL * 1024UL * 1024UL)

#ifdef BOLTPACK_FAST_FLUFF
#include "fluff_fast.h"
/*
 * Maximum number of blocks staged for one fast-fluff compression call.
 * Caps stack-allocated pointer arrays inside bp_archive_add_entry.
 * 64 blocks x 256 KB = 16 MB input + 16 MB output per file — well within
 * typical modern RAM budgets.
 */
#define FAST_FLUFF_MAX_BATCH 64
#define FAST_BOLTLZ_MAX_BATCH 64
#endif /* BOLTPACK_FAST_FLUFF */

/* ---- helpers ----------------------------------------------------------- */

static int io_write(bp_io_t *io, const void *buf, bp_uint32 count)
{
    long n = io->write(io->handle, buf, count);
    return (n == (long)count) ? BP_OK : BP_ERR_IO;
}

static int io_read(bp_io_t *io, void *buf, bp_uint32 count)
{
    long n = io->read(io->handle, buf, count);
    return (n == (long)count) ? BP_OK : BP_ERR_TRUNCATED;
}

static int io_seek(bp_io_t *io, bp_off_t offset, int origin)
{
    return io->seek(io->handle, offset, origin);
}

static bp_off_t io_tell(bp_io_t *io)
{
    return io->tell(io->handle);
}

/* Route a diagnostic message through ar->log_fn.  The core performs no
 * I/O of its own: when no callback is installed the message is dropped
 * (errors are still reported through return codes). */
static void bp_core_log(const bp_archive_t *ar, const char *msg)
{
    if (ar != NULL && ar->log_fn != NULL)
        ar->log_fn(msg, ar->log_data);
}

static int write_buf(bp_io_t *io, const bp_uint8 *buf, bp_uint32 len)
{
    return io_write(io, buf, len);
}

static int bp_method_is_supported(bp_uint8 method)
{
    if (method == BP_METHOD_STORED) return 1;
    if (method == BP_METHOD_BOLTLITE)   return 1;
    if (method == BP_METHOD_BOLTLZ) return 1;
    if (method == BP_METHOD_FLUFF)  return 1;
#ifdef BOLTPACK_ZSTD
    if (method == BP_METHOD_ZSTD)   return 1;
#endif
    return 0;
}

static bp_uint32 bp_method_compress_bound(bp_uint8 method, bp_uint32 src_len)
{
    if (method == BP_METHOD_BOLTLITE) {
        return src_len + src_len / 8 + 16;
    }
    if (method == BP_METHOD_BOLTLZ) {
        return boltlz_compress_bound(src_len);
    }
    if (method == BP_METHOD_FLUFF) {
        return fluff_compress_bound(src_len);
    }
#ifdef BOLTPACK_ZSTD
    if (method == BP_METHOD_ZSTD) {
        return bp_zstd_compress_bound(src_len);
    }
#endif
    return src_len;
}

void bp_packed_entry_dispose(bp_packed_entry_t *packed)
{
    if (packed == NULL) return;
    free(packed->name);
    memset(packed, 0, sizeof(*packed));
}

/* ---- entry name safety -------------------------------------------------- */

/*
 * Return 1 if an archive entry name is safe to use as a relative
 * extraction path, 0 otherwise.  Rejects empty names, absolute paths,
 * Windows drive letters, backslashes (archive names are '/'-separated
 * by specification), and any ".." path component.  Frontends should
 * strip leading '/' characters (tar-style) before calling this, then
 * skip or abort on unsafe names.
 */
int bp_name_is_safe(const char *name)
{
    size_t length;
    if (name == NULL) return 0;
    length = strlen(name);
    if (length == 0 || length > (size_t)BP_MAX_NAME_LEN) return 0;
    return bp_archive_name_valid(name, (bp_uint32)length);
}

/*
 * Return 1 if a symlink target cannot escape the extraction root when
 * the link itself lives at entry_name (a safe, '/'-separated relative
 * path), 0 otherwise.  Absolute targets are rejected; relative targets
 * are resolved against the link's directory, and each ".." component
 * must never rise above the extraction root.
 */
int bp_symlink_target_is_safe(const char *entry_name, const char *target)
{
    int depth;
    const char *p;

    if (entry_name == NULL || target == NULL) return 0;
    if (target[0] == '\0') return 0;
    if (target[0] == '/') return 0;                  /* absolute */
    if (target[1] == ':') return 0;                  /* drive letter */
    if (strchr(target, '\\') != NULL) return 0;      /* backslash */

    /* depth of the directory containing the link */
    depth = 0;
    for (p = entry_name; *p; p++) {
        if (*p == '/' && p[1] != '\0') depth++;
    }

    /* walk target components, tracking depth relative to the root */
    p = target;
    while (*p) {
        const char *comp;
        unsigned int comp_len;

        comp = p;
        comp_len = 0;
        while (p[comp_len] != '/' && p[comp_len] != '\0') comp_len++;

        if (comp_len == 2 && comp[0] == '.' && comp[1] == '.') {
            depth--;
            if (depth < 0) return 0;                 /* escapes root */
        } else if (comp_len == 1 && comp[0] == '.') {
            /* current directory: no change */
        } else if (comp_len > 0) {
            depth++;
        }

        p += comp_len;
        if (*p == '/') p++;
    }
    return 1;
}

/* ---- parallel fluff helper --------------------------------------------- */

#ifdef BOLTPACK_FAST_FLUFF
static void bp_fast_fluff_release(bp_archive_t *ar)
{
    if (ar == NULL) return;

    if (ar->fast_fluff_ctx != NULL) {
        fluff_fast_ctx_free((fluff_fast_ctx_t *)ar->fast_fluff_ctx);
        ar->fast_fluff_ctx = NULL;
    }
    free(ar->fast_fluff_in_base);
    free(ar->fast_fluff_out_base);
    free(ar->fast_fluff_hist_base);
    ar->fast_fluff_in_base = NULL;
    ar->fast_fluff_out_base = NULL;
    ar->fast_fluff_hist_base = NULL;
    ar->fast_fluff_ctx_threads = 0;
    ar->fast_fluff_batch_blocks = 0;
    ar->fast_fluff_block_size = 0;
    ar->fast_fluff_out_size = 0;
    ar->fast_fluff_hist_size = 0;
}

static int bp_fast_fluff_choose_batch_blocks(int nthreads_ff)
{
    int group_blocks;
    int batch_blocks;

    if (nthreads_ff < 2) {
        return 0;
    }

    group_blocks = fluff_fast_get_group_blocks();
    if (group_blocks < 1) {
        group_blocks = 1;
    }

    batch_blocks = FAST_FLUFF_MAX_BATCH;
    if (batch_blocks < nthreads_ff) {
        batch_blocks = nthreads_ff;
    }
    if (batch_blocks > FAST_FLUFF_MAX_BATCH) {
        batch_blocks = FAST_FLUFF_MAX_BATCH;
    }
    return batch_blocks;
}

static int bp_fast_fluff_ensure(bp_archive_t *ar, bp_uint32 block_size,
                                bp_uint32 out_size)
{
    int nthreads_ff;
    int batch_blocks;

    if (ar == NULL) return BP_ERR_INVALID_ARG;

    nthreads_ff = ar->fast_fluff_threads;
    if (nthreads_ff > FAST_FLUFF_MAX_BATCH)
        nthreads_ff = FAST_FLUFF_MAX_BATCH;
    batch_blocks = bp_fast_fluff_choose_batch_blocks(nthreads_ff);

    if (nthreads_ff < 2 || batch_blocks < 2) {
        bp_fast_fluff_release(ar);
        return BP_OK;
    }

    if (ar->fast_fluff_ctx != NULL &&
        ar->fast_fluff_ctx_threads == (bp_uint32)nthreads_ff &&
        ar->fast_fluff_batch_blocks == (bp_uint32)batch_blocks &&
        ar->fast_fluff_block_size == block_size &&
        ar->fast_fluff_out_size == out_size) {
        return BP_OK;
    }

    bp_fast_fluff_release(ar);

    ar->fast_fluff_ctx = (void *)fluff_fast_ctx_alloc(nthreads_ff);
    if (ar->fast_fluff_ctx == NULL) {
        return BP_ERR_NOMEM;
    }

    ar->fast_fluff_in_base = (bp_uint8 *)malloc(
        (size_t)batch_blocks * (size_t)block_size);
    if (ar->fast_fluff_in_base == NULL) {
        bp_fast_fluff_release(ar);
        return BP_ERR_NOMEM;
    }

    ar->fast_fluff_out_base = (bp_uint8 *)malloc(
        (size_t)batch_blocks * (size_t)out_size);
    if (ar->fast_fluff_out_base == NULL) {
        bp_fast_fluff_release(ar);
        return BP_ERR_NOMEM;
    }

    ar->fast_fluff_hist_size = fluff_window_size();
    ar->fast_fluff_hist_base = (bp_uint8 *)malloc(
        (size_t)ar->fast_fluff_hist_size);
    if (ar->fast_fluff_hist_base == NULL) {
        bp_fast_fluff_release(ar);
        return BP_ERR_NOMEM;
    }

    ar->fast_fluff_ctx_threads = (bp_uint32)nthreads_ff;
    ar->fast_fluff_batch_blocks = (bp_uint32)batch_blocks;
    ar->fast_fluff_block_size = block_size;
    ar->fast_fluff_out_size = out_size;
    return BP_OK;
}

static void bp_fast_boltlz_release(bp_archive_t *ar)
{
    if (ar == NULL) return;
    free(ar->fast_boltlz_in_base);
    free(ar->fast_boltlz_out_base);
    free(ar->fast_boltlz_work_base);
    ar->fast_boltlz_in_base = NULL;
    ar->fast_boltlz_out_base = NULL;
    ar->fast_boltlz_work_base = NULL;
    ar->fast_boltlz_ctx_threads = 0;
    ar->fast_boltlz_batch_blocks = 0;
    ar->fast_boltlz_block_size = 0;
    ar->fast_boltlz_out_size = 0;
    ar->fast_boltlz_work_size = 0;
}

static int bp_fast_boltlz_ensure(bp_archive_t *ar, bp_uint32 block_size,
                                 bp_uint32 out_size, int nthreads)
{
    int batch_blocks;
    bp_uint32 work_size;

    if (ar == NULL) return BP_ERR_INVALID_ARG;
    if (nthreads > FAST_BOLTLZ_MAX_BATCH)
        nthreads = FAST_BOLTLZ_MAX_BATCH;
    if (nthreads < 2) {
        bp_fast_boltlz_release(ar);
        return BP_OK;
    }
    batch_blocks = nthreads;
    work_size = bp_compress_work_size();
    if (work_size == 0) return BP_ERR_INVALID_ARG;

    if (ar->fast_boltlz_in_base != NULL &&
        ar->fast_boltlz_ctx_threads == (bp_uint32)nthreads &&
        ar->fast_boltlz_batch_blocks == (bp_uint32)batch_blocks &&
        ar->fast_boltlz_block_size == block_size &&
        ar->fast_boltlz_out_size == out_size &&
        ar->fast_boltlz_work_size == work_size) {
        return BP_OK;
    }

    bp_fast_boltlz_release(ar);
    ar->fast_boltlz_in_base = (bp_uint8 *)malloc(
        (size_t)batch_blocks * (size_t)block_size);
    ar->fast_boltlz_out_base = (bp_uint8 *)malloc(
        (size_t)batch_blocks * (size_t)out_size);
    ar->fast_boltlz_work_base = malloc(
        (size_t)nthreads * (size_t)work_size);
    if (ar->fast_boltlz_in_base == NULL ||
        ar->fast_boltlz_out_base == NULL ||
        ar->fast_boltlz_work_base == NULL) {
        bp_fast_boltlz_release(ar);
        return BP_ERR_NOMEM;
    }

    ar->fast_boltlz_ctx_threads = (bp_uint32)nthreads;
    ar->fast_boltlz_batch_blocks = (bp_uint32)batch_blocks;
    ar->fast_boltlz_block_size = block_size;
    ar->fast_boltlz_out_size = out_size;
    ar->fast_boltlz_work_size = work_size;
    return BP_OK;
}

static int bp_fast_boltlz_effective_threads(bp_uint64 file_size,
                                            bp_uint32 block_size,
                                            int nthreads)
{
    bp_uint32 min_bytes;
    if (nthreads > FAST_BOLTLZ_MAX_BATCH)
        nthreads = FAST_BOLTLZ_MAX_BATCH;
    while (nthreads > 1) {
        /* At least two blocks per worker amortizes staging and OpenMP startup
         * costs while still using available cores on medium-sized files. */
        min_bytes = block_size * (bp_uint32)(2 * nthreads);
        if (bp_u64_gte(file_size, bp_u64_from_u32(min_bytes)))
            return nthreads;
        nthreads--;
    }
    return 0;
}
#endif

void bp_set_fast_fluff(bp_archive_t *ar, int nthreads)
{
    if (ar == NULL) return;
#ifdef BOLTPACK_FAST_FLUFF
    bp_fast_fluff_release(ar);
    bp_fast_boltlz_release(ar);
    ar->fast_fluff_threads = (nthreads > 0) ? nthreads : 0;
#else
    (void)nthreads;
    ar->fast_fluff_threads = 0;
#endif
}

void bp_set_fast_boltlz(bp_archive_t *ar, int nthreads)
{
    if (ar == NULL) return;
#ifdef BOLTPACK_FAST_FLUFF
    bp_fast_fluff_release(ar);
    bp_fast_boltlz_release(ar);
    ar->fast_fluff_threads = (nthreads > 0) ? nthreads : 0;
#else
    (void)nthreads;
    ar->fast_fluff_threads = 0;
#endif
}

/* ---- archive creation -------------------------------------------------- */

int bp_archive_create(bp_archive_t *ar, bp_io_t *io,
                      bp_uint32 block_size, bp_uint8 method)
{
    bp_uint8 hdr[BP_HEADER_SIZE];

    if (ar == NULL || io == NULL) {
        return BP_ERR_INVALID_ARG;
    }
    if (!bp_method_is_supported(method)) {
        return BP_ERR_UNSUPPORTED_METHOD;
    }
    if (method == BP_METHOD_BOLTLZ && block_size > BOLTLZ_MAX_BLOCK)
        return BP_ERR_INVALID_ARG;

    memset(ar, 0, sizeof(*ar));
    ar->io = io;
    ar->block_size = block_size ? block_size : BP_BLOCK_DEFAULT;
    ar->method = method;
    ar->method_level = 0;
    ar->creator_os = BP_OS_UNKNOWN;
    ar->creator_cpu = BP_CPU_UNKNOWN;

    /* detect OS at compile time (override with -DBP_CREATOR_OS=BP_OS_xxx) */
#ifdef BP_CREATOR_OS
    ar->creator_os = BP_CREATOR_OS;
#else
#if defined(__DJGPP__) || defined(__DOS__) || defined(MSDOS)
    ar->creator_os = BP_OS_DOS;
#elif defined(_WIN32) || defined(_WIN64)
    ar->creator_os = BP_OS_WINDOWS;
#elif defined(__linux__)
    ar->creator_os = BP_OS_LINUX;
#elif defined(__FreeBSD__)
    ar->creator_os = BP_OS_FREEBSD;
#elif defined(__NetBSD__)
    ar->creator_os = BP_OS_NETBSD;
#elif defined(__OpenBSD__)
    ar->creator_os = BP_OS_OPENBSD;
#elif defined(__APPLE__)
    ar->creator_os = BP_OS_MACOS;
#elif defined(__amigaos__) || defined(AMIGA)
    ar->creator_os = BP_OS_AMIGA;
#elif defined(__atarist__)
    ar->creator_os = BP_OS_ATARI;
#endif
#endif /* BP_CREATOR_OS */

    /* detect CPU at compile time (override with -DBP_CREATOR_CPU=BP_CPU_xxx) */
#ifdef BP_CREATOR_CPU
    ar->creator_cpu = BP_CREATOR_CPU;
#else
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    ar->creator_cpu = BP_CPU_X86_64;
#elif defined(__i386__) || defined(_M_IX86)
    ar->creator_cpu = BP_CPU_X86;
#elif defined(__aarch64__)
    ar->creator_cpu = BP_CPU_ARM64;
#elif defined(__arm__)
    ar->creator_cpu = BP_CPU_ARM32;
#elif defined(__powerpc64__)
    ar->creator_cpu = BP_CPU_PPC64;
#elif defined(__powerpc__)
    ar->creator_cpu = BP_CPU_PPC32;
#elif defined(__m68k__)
    ar->creator_cpu = BP_CPU_M68K;
#endif
#endif /* BP_CREATOR_CPU */

    ar->entries_written = 0;
    ar->entry_list = NULL;
    ar->entry_list_cap = 0;

    {
        bp_uint64 z = {0, 0};
        ar->archive_orig_total = z;
        ar->archive_comp_total = z;
        ar->archive_orig_done  = z;
    }

    /* write placeholder header (patched in finalize) */
    memset(hdr, 0, BP_HEADER_SIZE);
    hdr[0] = BP_MAGIC_0;
    hdr[1] = BP_MAGIC_1;
    hdr[2] = BP_MAGIC_2;
    hdr[3] = BP_MAGIC_3;
    bp_write_le16(hdr + 4, BP_FORMAT_VERSION);
    /* flags = 0, file_count = 0 (patched later) */
    hdr[12] = ar->creator_os;
    hdr[13] = ar->creator_cpu;
    hdr[14] = BP_VERSION_MAJOR;
    hdr[15] = BP_VERSION_MINOR;
    /* archive_size = 0 (patched later), reserved = 0 */

    return write_buf(ar->io, hdr, BP_HEADER_SIZE);
}

/* grow entry list */
static int entry_list_grow(bp_archive_t *ar)
{
    bp_uint32 new_cap;
    size_t byte_cap;
    bp_entry_t *new_list;

    if (ar->entries_written < ar->entry_list_cap) {
        return BP_OK;
    }
    new_cap = ar->entry_list_cap ? ar->entry_list_cap * 2 : 64;
    byte_cap = (size_t)new_cap * sizeof(bp_entry_t);
    /* Guard both the doubling wrap and the byte-product overflow on 32-bit
     * size_t: verify the multiplication round-trips rather than comparing
     * against SIZE_MAX (which produces a spurious warning on 64-bit). */
    if (new_cap <= ar->entry_list_cap ||
        byte_cap / sizeof(bp_entry_t) != (size_t)new_cap) {
        return BP_ERR_NOMEM;
    }
    new_list = (bp_entry_t *)realloc(ar->entry_list, byte_cap);
    if (new_list == NULL) {
        return BP_ERR_NOMEM;
    }
    ar->entry_list = new_list;
    ar->entry_list_cap = new_cap;
    return BP_OK;
}

static int record_packed_entry(bp_archive_t *ar, bp_off_t entry_offset,
                               const bp_packed_entry_t *packed)
{
    bp_entry_t *ent;

    if (ar == NULL || packed == NULL || packed->name == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    ent = &ar->entry_list[ar->entries_written];
    memset(ent, 0, sizeof(*ent));
    ent->name = (char *)malloc((size_t)(packed->name_len + 1));
    if (ent->name == NULL) {
        return BP_ERR_NOMEM;
    }
    memcpy(ent->name, packed->name, packed->name_len);
    ent->name[packed->name_len] = '\0';
    ent->name_len = packed->name_len;
    ent->original_size = packed->original_size;
    ent->compressed_size = packed->compressed_size;
    ent->checksum = packed->checksum;
    ent->method = packed->method;
    ent->checksum_type = packed->checksum_type;
    ent->block_size = packed->block_size;
    ent->entry_type = packed->entry_type;
    ent->platform = packed->platform;
    ent->mtime = packed->mtime;
    ent->flags = packed->flags;
    ent->header_size = packed->header_size;
    ent->offset = bp_off_to_u64(entry_offset);

    ar->entries_written++;
    ar->archive_orig_total = bp_u64_add(ar->archive_orig_total,
                                        packed->original_size);
    ar->archive_comp_total = bp_u64_add(ar->archive_comp_total,
                                        packed->compressed_size);
    ar->archive_orig_done  = bp_u64_add(ar->archive_orig_done,
                                        packed->original_size);
    return BP_OK;
}

static int bp_archive_write_entry(bp_archive_t *ar, const char *name,
                                  bp_uint8 entry_type, bp_uint8 platform,
                                  bp_io_t *file_io, bp_uint64 file_size,
                                  bp_uint64 mtime, bp_uint16 flags,
                                  bp_io_t *dst_io,
                                  bp_packed_entry_t *packed,
                                  int record_entry)
{
    bp_uint16 name_len;
    bp_uint16 hdr_size;
    bp_uint8 ehdr[BP_ENTRY_FIXED]; /* fixed portion, name written separately */
    int rc;
    bp_off_t entry_offset;
    bp_uint32 adler;
    bp_uint8 ck_type;
    bp_uint64 total_comp;
    bp_uint8 *in_buf = NULL;
    bp_uint8 *out_buf = NULL;
    bp_uint32 out_buf_size;
    fluff_ctx_t *fctx = NULL;
    bp_packed_entry_t local_packed;
    bp_packed_entry_t *packed_out;
    int use_local_packed;

    packed_out = packed;
    use_local_packed = 0;

    if (ar == NULL || name == NULL || dst_io == NULL) {
        return BP_ERR_INVALID_ARG;
    }
    if (!record_entry && packed == NULL) {
        return BP_ERR_INVALID_ARG;
    }
    if (record_entry && packed_out == NULL) {
        memset(&local_packed, 0, sizeof(local_packed));
        packed_out = &local_packed;
        use_local_packed = 1;
    } else if (packed_out != NULL) {
        memset(packed_out, 0, sizeof(*packed_out));
    }

    ck_type = BP_CKSUM_CRC32; /* CRC-32 is the only compiled archive checksum */

    if (strlen(name) > BP_MAX_NAME_LEN) {
        return BP_ERR_NAME_TOO_LONG;
    }
    name_len = (bp_uint16)strlen(name);
    if (!bp_archive_name_valid(name, (bp_uint32)name_len) ||
        !bp_archive_name_type_valid(name, (bp_uint32)name_len, entry_type) ||
        (!(flags & BP_ENTRY_FLAG_MTIME_VALID) && !bp_u64_is_zero(mtime)))
        return BP_ERR_INVALID_ARG;

    hdr_size = (bp_uint16)(BP_ENTRY_FIXED + name_len);

    entry_offset = 0;
    if (record_entry) {
        rc = entry_list_grow(ar);
        if (rc != BP_OK) return rc;

        entry_offset = io_tell(dst_io);
        if (entry_offset < 0) {
            return BP_ERR_IO;
        }
    }

    /* write entry header (compressed_size patched after compression) */
    memset(ehdr, 0, BP_ENTRY_FIXED);
    bp_write_le16(ehdr + 0, hdr_size);             /* header_size */
    bp_write_le16(ehdr + 2, name_len);             /* name_len */
    /* name written separately */
    /* original_size at offset 4+N -- written after name */
    /* other fields at their respective offsets after name */

    /* Write header_size and name_len */
    rc = write_buf(dst_io, ehdr, 4);
    if (rc != BP_OK) return rc;

    /* Write name */
    rc = write_buf(dst_io, (const bp_uint8 *)name, name_len);
    if (rc != BP_OK) return rc;

    /* Now write the remaining fixed fields as a block.
     * We build the remaining 38 bytes: original_size(8) + compressed_size(8) +
     * checksum(4) + method(1) + checksum_type(1) + block_size(4) +
     * entry_type(1) + platform(1) + mtime(8) + flags(2) = 38 bytes */
    {
        bp_uint8 fields[38];
        bp_uint64 orig64;
        bp_off_t fields_pos;

        memset(fields, 0, 38);
        orig64 = file_size;
        bp_write_le64(fields + 0, orig64);          /* original_size */
        /* compressed_size at fields+8: filled after compression */
        /* checksum at fields+16: filled after compression */
        fields[20] = (entry_type == BP_TYPE_DIR ||
                      entry_type == BP_TYPE_SYMLINK ||
                      bp_u64_is_zero(file_size))
                     ? BP_METHOD_STORED : ar->method;
        fields[21] = ck_type;                       /* checksum_type */
        if (!bp_u64_is_zero(file_size) &&
            entry_type != BP_TYPE_DIR &&
            entry_type != BP_TYPE_SYMLINK) {
            bp_write_le32(fields + 22, ar->block_size);
        }
        fields[26] = entry_type;
        fields[27] = platform;
        bp_write_le64(fields + 28, mtime);
        bp_write_le16(fields + 36, flags);

        fields_pos = io_tell(dst_io);
        if (fields_pos < 0) return BP_ERR_IO;

        rc = write_buf(dst_io, fields, 38);
        if (rc != BP_OK) return rc;

        /* Now compress and write data blocks */
        total_comp = bp_u64_from_u32(0);
        adler = bp_checksum_seed(ck_type); /* CRC-32 seed (0) */

        if (entry_type == BP_TYPE_SYMLINK && file_io != NULL &&
            !bp_u64_is_zero(file_size)) {
            /* Symlink payload is the target path, stored raw with no block headers. */
            bp_uint32 symlink_size = bp_u64_to_u32(file_size);

            in_buf = (bp_uint8 *)malloc((size_t)symlink_size);
            if (in_buf == NULL) return BP_ERR_NOMEM;

            if (file_io->read(file_io->handle, in_buf, symlink_size) != (long)symlink_size) {
                free(in_buf);
                return BP_ERR_IO;
            }
            adler = bp_checksum_typed(ck_type, adler, in_buf, symlink_size);
            total_comp = bp_u64_from_u32(symlink_size);

            rc = write_buf(dst_io, in_buf, symlink_size);
            free(in_buf);
            if (rc != BP_OK) return rc;
        } else if (!bp_u64_is_zero(file_size)) {
            bp_uint64 remaining = file_size;
            bp_uint32 bsize = ar->block_size;
            bp_uint8 actual_method = fields[20];
            int any_compressed_block = 0;

            out_buf_size = bp_method_compress_bound(ar->method, bsize);

#ifdef BOLTPACK_FAST_FLUFF
            if (ar->fast_fluff_threads > 1 && actual_method == BP_METHOD_FLUFF) {
                /*
                 * Fast parallel path: read up to the staged batch capacity,
                 * compress the blocks concurrently with OpenMP, write in order.
                 * All declarations must come first (C89).
                 */
                fluff_fast_ctx_t *ffctx;
                bp_uint8         *fast_in_base;
                bp_uint8         *fast_out_base;
                bp_uint8         *fast_hist_base;
                bp_uint8         *batch_srcs[FAST_FLUFF_MAX_BATCH];
                bp_uint8         *batch_dsts[FAST_FLUFF_MAX_BATCH];
                bp_uint32         batch_src_lens[FAST_FLUFF_MAX_BATCH];
                bp_uint32         batch_dst_lens[FAST_FLUFF_MAX_BATCH];
                bp_uint32         batch_total;
                bp_uint32         batch_written;
                bp_uint32         fast_hist_len;
                bp_uint32         fast_hist_cap;
                bp_uint32         fast_stream_pos;
                int               batch_cap_ff;
                int               batch_n;
                int               b;

                rc = bp_fast_fluff_ensure(ar, bsize, out_buf_size);
                if (rc != BP_OK) return rc;

                ffctx = (fluff_fast_ctx_t *)ar->fast_fluff_ctx;
                fast_in_base = ar->fast_fluff_in_base;
                fast_out_base = ar->fast_fluff_out_base;
                fast_hist_base = ar->fast_fluff_hist_base;
                fast_hist_cap = ar->fast_fluff_hist_size;
                fast_hist_len = 0;
                fast_stream_pos = 0;
                batch_cap_ff = (int)ar->fast_fluff_batch_blocks;

                for (b = 0; b < batch_cap_ff; b++) {
                    batch_srcs[b] = fast_in_base + (size_t)b * (size_t)bsize;
                    batch_dsts[b] = fast_out_base + (size_t)b * (size_t)out_buf_size;
                }

                while (!bp_u64_is_zero(remaining)) {
                    long n_read;
                    int  batch_rc;

                    batch_n = 0;
                    batch_total = 0;
                    while (batch_n < batch_cap_ff && !bp_u64_is_zero(remaining)) {
                        bp_uint32 chunk = (remaining.hi != 0 || remaining.lo > bsize)
                                          ? bsize : remaining.lo;

                        n_read = file_io->read(file_io->handle,
                                               batch_srcs[batch_n], chunk);
                        if (n_read != (long)chunk) {
                            char lbuf[512];
                            sprintf(lbuf,
                                "boltpack: low-level read failed for entry %.200s: "
                                "requested=%u read=%ld errno=%d (%s)",
                                name, (unsigned)chunk, n_read,
                                errno, strerror(errno));
                            bp_core_log(ar, lbuf);
                            return BP_ERR_IO;
                        }
                        adler = bp_checksum_typed(ck_type, adler,
                                                  batch_srcs[batch_n], chunk);
                        batch_src_lens[batch_n] = chunk;
                        batch_total += chunk;
                        remaining = bp_u64_sub(remaining, bp_u64_from_u32(chunk));
                        batch_n++;
                    }

                    /* Compress all blocks in the batch in parallel. */
                    batch_rc = fluff_fast_compress_blocks_with_history(
                        (const bp_uint8 **)batch_srcs, batch_src_lens,
                        batch_dsts, batch_dst_lens,
                        batch_n, ffctx,
                        fast_hist_base, fast_hist_len,
                        fast_stream_pos);
                    /* Non-fatal: stored fallback applied inside fluff_fast. */
                    if (batch_rc != BP_OK) {
                        return batch_rc;
                    }

                    /* Write blocks in order. */
                    batch_written = 0;
                    for (b = 0; b < batch_n; b++) {
                        bp_uint8  blk_hdr[BP_BLOCK_HDR_V3];
                        bp_uint64 bytes_done;

                        bp_write_le32(blk_hdr + 0, batch_dst_lens[b]);
                        bp_write_le32(blk_hdr + 4, batch_src_lens[b]);
                        bp_write_le32(blk_hdr + 8, bp_crc32(0, batch_dsts[b], batch_dst_lens[b]));
                        rc = write_buf(dst_io, blk_hdr, BP_BLOCK_HDR_V3);
                        if (rc != BP_OK) {
                            return rc;
                        }
                        rc = write_buf(dst_io, batch_dsts[b], batch_dst_lens[b]);
                        if (rc != BP_OK) {
                            return rc;
                        }

                        if (batch_dst_lens[b] != batch_src_lens[b]) {
                            any_compressed_block = 1;
                        }
                        total_comp = bp_u64_add(total_comp,
                            bp_u64_from_u32(batch_dst_lens[b] + BP_BLOCK_HDR_V3));
                        batch_written += batch_src_lens[b];

                        if (record_entry && ar->progress_fn) {
                            bp_progress_t p;
                            memset(&p, 0, sizeof(p));
                            bytes_done = bp_u64_sub(
                                file_size,
                                bp_u64_add(remaining,
                                           bp_u64_from_u32(batch_total - batch_written)));
                            p.file_index = ar->entries_written;
                            p.file_count = 0;
                            p.file_name  = name;
                            p.file_orig_size  = orig64;
                            p.file_bytes_done = bytes_done;
                            p.file_comp_size  = total_comp;
                            p.archive_orig_total = bp_u64_add(
                                ar->archive_orig_total, bytes_done);
                            p.archive_comp_total = bp_u64_add(
                                ar->archive_comp_total, total_comp);
                            p.archive_orig_done  = bp_u64_add(
                                ar->archive_orig_done, bytes_done);
                            if (ar->progress_fn(&p, ar->progress_data) != 0) {
                                return BP_ERR_ABORTED;
                            }
                        }
                    }

                    if (fast_hist_cap != 0) {
                        if (batch_total >= fast_hist_cap) {
                            memcpy(fast_hist_base,
                                   fast_in_base + (batch_total - fast_hist_cap),
                                   fast_hist_cap);
                            fast_hist_len = fast_hist_cap;
                        } else {
                            bp_uint32 keep_old;

                            keep_old = fast_hist_len;
                            if (keep_old > fast_hist_cap - batch_total) {
                                keep_old = fast_hist_cap - batch_total;
                            }
                            if (keep_old != 0 && keep_old < fast_hist_len) {
                                memmove(fast_hist_base,
                                        fast_hist_base + (fast_hist_len - keep_old),
                                        keep_old);
                            }
                            memcpy(fast_hist_base + keep_old,
                                   fast_in_base, batch_total);
                            fast_hist_len = keep_old + batch_total;
                        }
                    }
                    fast_stream_pos += batch_total;
                }

                if (!any_compressed_block) {
                    fields[20] = BP_METHOD_STORED;
                }
            } else if (ar->fast_fluff_threads > 1 &&
                       actual_method == BP_METHOD_BOLTLZ &&
                       bp_fast_boltlz_effective_threads(
                           file_size, bsize, ar->fast_fluff_threads) > 1) {
                bp_uint8 *fast_in_base;
                bp_uint8 *fast_out_base;
                void *fast_work_base;
                bp_uint8 *batch_srcs[FAST_BOLTLZ_MAX_BATCH];
                bp_uint8 *batch_dsts[FAST_BOLTLZ_MAX_BATCH];
                bp_uint32 batch_src_lens[FAST_BOLTLZ_MAX_BATCH];
                bp_uint32 batch_dst_lens[FAST_BOLTLZ_MAX_BATCH];
                int batch_status[FAST_BOLTLZ_MAX_BATCH];
                bp_uint32 batch_total;
                bp_uint32 batch_written;
                bp_uint32 work_size;
                int nthreads_bz;
                int batch_cap_bz;
                int batch_n;
                int b;

                nthreads_bz = bp_fast_boltlz_effective_threads(
                    file_size, bsize, ar->fast_fluff_threads);
                rc = bp_fast_boltlz_ensure(ar, bsize, out_buf_size,
                                           nthreads_bz);
                if (rc != BP_OK) return rc;

                fast_in_base = ar->fast_boltlz_in_base;
                fast_out_base = ar->fast_boltlz_out_base;
                fast_work_base = ar->fast_boltlz_work_base;
                work_size = ar->fast_boltlz_work_size;
                nthreads_bz = (int)ar->fast_boltlz_ctx_threads;
                batch_cap_bz = (int)ar->fast_boltlz_batch_blocks;

                for (b = 0; b < batch_cap_bz; b++) {
                    batch_srcs[b] = fast_in_base +
                                    (size_t)b * (size_t)bsize;
                    batch_dsts[b] = fast_out_base +
                                    (size_t)b * (size_t)out_buf_size;
                }

                while (!bp_u64_is_zero(remaining)) {
                    batch_n = 0;
                    batch_total = 0;
                    while (batch_n < batch_cap_bz &&
                           !bp_u64_is_zero(remaining)) {
                        bp_uint32 chunk =
                            (remaining.hi != 0 || remaining.lo > bsize)
                            ? bsize : remaining.lo;
                        long n_read;

                        n_read = file_io->read(file_io->handle,
                                               batch_srcs[batch_n], chunk);
                        if (n_read != (long)chunk) {
                            char lbuf[512];
                            sprintf(lbuf,
                                "boltpack: low-level read failed for entry %.200s: "
                                "requested=%u read=%ld errno=%d (%s)",
                                name, (unsigned)chunk, n_read,
                                errno, strerror(errno));
                            bp_core_log(ar, lbuf);
                            return BP_ERR_IO;
                        }
                        adler = bp_checksum_typed(ck_type, adler,
                                                  batch_srcs[batch_n], chunk);
                        batch_src_lens[batch_n] = chunk;
                        batch_total += chunk;
                        remaining = bp_u64_sub(remaining,
                                               bp_u64_from_u32(chunk));
                        batch_n++;
                    }

                    /* BoltLZ blocks have independent match histories. */
#pragma omp parallel for schedule(dynamic) num_threads(nthreads_bz)
                    for (b = 0; b < batch_n; b++) {
                        int tid;
                        bp_uint32 comp_len;
                        void *worker_work;

                        tid = omp_get_thread_num();
                        if (tid < 0 || tid >= nthreads_bz) tid = 0;
                        worker_work = (bp_uint8 *)fast_work_base +
                                      (size_t)tid * (size_t)work_size;
                        comp_len = out_buf_size;
                        batch_status[b] = bp_compress_block_work(
                            batch_srcs[b], batch_src_lens[b], batch_dsts[b],
                            &comp_len, worker_work, ar->method_level);
                        batch_dst_lens[b] = comp_len;
                    }

                    for (b = 0; b < batch_n; b++) {
                        if (batch_status[b] != BP_OK)
                            return batch_status[b];
                    }

                    batch_written = 0;
                    for (b = 0; b < batch_n; b++) {
                        bp_uint8 blk_hdr[BP_BLOCK_HDR_V3];
                        bp_uint64 bytes_done;

                        if (batch_dst_lens[b] >=
                            batch_src_lens[b] - batch_src_lens[b] / 100) {
                            batch_dst_lens[b] = batch_src_lens[b];
                            memcpy(batch_dsts[b], batch_srcs[b],
                                   (size_t)batch_src_lens[b]);
                        } else {
                            any_compressed_block = 1;
                        }

                        bp_write_le32(blk_hdr + 0, batch_dst_lens[b]);
                        bp_write_le32(blk_hdr + 4, batch_src_lens[b]);
                        bp_write_le32(blk_hdr + 8,
                            bp_crc32(0, batch_dsts[b], batch_dst_lens[b]));
                        rc = write_buf(dst_io, blk_hdr, BP_BLOCK_HDR_V3);
                        if (rc != BP_OK) return rc;
                        rc = write_buf(dst_io, batch_dsts[b],
                                       batch_dst_lens[b]);
                        if (rc != BP_OK) return rc;

                        total_comp = bp_u64_add(total_comp,
                            bp_u64_from_u32(batch_dst_lens[b] +
                                            BP_BLOCK_HDR_V3));
                        batch_written += batch_src_lens[b];

                        if (record_entry && ar->progress_fn) {
                            bp_progress_t p;
                            memset(&p, 0, sizeof(p));
                            bytes_done = bp_u64_sub(file_size,
                                bp_u64_add(remaining,
                                    bp_u64_from_u32(batch_total -
                                                    batch_written)));
                            p.file_index = ar->entries_written;
                            p.file_count = 0;
                            p.file_name = name;
                            p.file_orig_size = orig64;
                            p.file_bytes_done = bytes_done;
                            p.file_comp_size = total_comp;
                            p.archive_orig_total = bp_u64_add(
                                ar->archive_orig_total, bytes_done);
                            p.archive_comp_total = bp_u64_add(
                                ar->archive_comp_total, total_comp);
                            p.archive_orig_done = bp_u64_add(
                                ar->archive_orig_done, bytes_done);
                            if (ar->progress_fn(&p, ar->progress_data) != 0)
                                return BP_ERR_ABORTED;
                        }
                    }
                }

                if (!any_compressed_block)
                    fields[20] = BP_METHOD_STORED;
            } else
#endif /* BOLTPACK_FAST_FLUFF */
            {
            /* reuse or allocate cached sequential compress buffers */
            if (ar->seq_buf_block_size != bsize) {
                free(ar->seq_in_buf);  ar->seq_in_buf  = NULL;
                free(ar->seq_out_buf); ar->seq_out_buf = NULL;
                ar->seq_buf_block_size = 0;
            }
            if (ar->seq_in_buf == NULL) {
                ar->seq_in_buf = (bp_uint8 *)malloc((size_t)bsize);
                if (ar->seq_in_buf == NULL) { rc = BP_ERR_NOMEM; goto seq_done; }
                ar->seq_buf_block_size = bsize;
            }
            if (ar->seq_out_buf == NULL) {
                ar->seq_out_buf = (bp_uint8 *)malloc((size_t)out_buf_size);
                if (ar->seq_out_buf == NULL) { rc = BP_ERR_NOMEM; goto seq_done; }
            }
            in_buf  = ar->seq_in_buf;
            out_buf = ar->seq_out_buf;

            if (actual_method == BP_METHOD_BOLTLZ && ar->seq_boltlz_work == NULL) {
                ar->seq_boltlz_work = malloc((size_t)bp_compress_work_size());
                if (ar->seq_boltlz_work == NULL) { rc = BP_ERR_NOMEM; goto seq_done; }
            }

            if (actual_method == BP_METHOD_FLUFF) {
                if (ar->seq_fluff_ctx == NULL) {
                    ar->seq_fluff_ctx = (void *)fluff_ctx_alloc();
                    if (ar->seq_fluff_ctx == NULL) { rc = BP_ERR_NOMEM; goto seq_done; }
                }
                fctx = (fluff_ctx_t *)ar->seq_fluff_ctx;
                if (ar->method_level > 0)
                    fluff_ctx_reset_level(fctx, (int)ar->method_level);
                else
                    fluff_ctx_reset(fctx);
            }

            while (!bp_u64_is_zero(remaining)) {
                bp_uint32 chunk = (remaining.hi != 0 || remaining.lo > bsize)
                    ? bsize : remaining.lo;
                bp_uint32 comp_len = 0;
                bp_uint8 blk_hdr[BP_BLOCK_HDR_V3];
                long n;

                /* read from source */
                n = file_io->read(file_io->handle, in_buf, chunk);
                if (n != (long)chunk) {
                    char lbuf[512];
                    long pos = file_io->tell(file_io->handle);
                    sprintf(lbuf,
                        "boltpack: low-level read failed for entry %.200s: "
                        "requested=%u read=%ld pos=%ld errno=%d (%s)",
                        name, (unsigned)chunk, n, pos, errno, strerror(errno));
                    bp_core_log(ar, lbuf);
                    rc = BP_ERR_IO;
                    goto seq_done;
                }

                /* update checksum */
                adler = bp_checksum_typed(ck_type, adler, in_buf, chunk);

                if (actual_method == BP_METHOD_BOLTLITE) {
                    bp_uint8 boltlite_work[BOLTLITE_WORK_SIZE];
                    comp_len = boltlite_compress(in_buf, chunk, out_buf, boltlite_work);
                    if (comp_len == 0) {
                        rc = BP_ERR_COMPRESS;
                        goto seq_done;
                    }
                } else if (actual_method == BP_METHOD_BOLTLZ) {
                    comp_len = out_buf_size;
                    rc = bp_compress_block_work(in_buf, chunk, out_buf,
                                                &comp_len, ar->seq_boltlz_work,
                                                ar->method_level);
                    if (rc != BP_OK) goto seq_done;
                } else if (actual_method == BP_METHOD_FLUFF) {
                    rc = fluff_compress_block_ctx(in_buf, chunk, out_buf,
                                                  &comp_len, fctx);
                    if (rc != BP_OK) goto seq_done;
                }
#ifdef BOLTPACK_ZSTD
                else if (actual_method == BP_METHOD_ZSTD) {
                    bp_uint32 dst_cap = bp_zstd_compress_bound(chunk);
                    comp_len = dst_cap;
                    rc = bp_zstd_compress_block(in_buf, chunk, out_buf, &comp_len,
                                                (int)ar->method_level);
                    if (rc != BP_OK) goto seq_done;
                }
#endif /* BOLTPACK_ZSTD */

                /* Fallback to stored if compression saves < 1% (favors decompression speed).
                 * Written as chunk - chunk/100 (not chunk*99/100) so it cannot
                 * overflow for large block sizes; >= keeps comp_len == chunk on
                 * the stored path, which the block format requires. */
                if (actual_method != BP_METHOD_STORED
                    && comp_len >= chunk - chunk / 100) {
                    comp_len = chunk;
                    memcpy(out_buf, in_buf, (size_t)chunk);
                } else if (actual_method == BP_METHOD_STORED) {
                    /* stored */
                    comp_len = chunk;
                    memcpy(out_buf, in_buf, (size_t)chunk);
                }

                if (comp_len < chunk) {
                    any_compressed_block = 1;
                }

                /* write block header (v3: 12 bytes, includes CRC of compressed data) */
                bp_write_le32(blk_hdr + 0, comp_len);
                bp_write_le32(blk_hdr + 4, chunk);
                bp_write_le32(blk_hdr + 8, bp_crc32(0, out_buf, comp_len));
                rc = write_buf(dst_io, blk_hdr, BP_BLOCK_HDR_V3);
                if (rc != BP_OK) goto seq_done;

                /* write block data */
                rc = write_buf(dst_io, out_buf, comp_len);
                if (rc != BP_OK) goto seq_done;

                total_comp = bp_u64_add(total_comp, bp_u64_from_u32(comp_len + BP_BLOCK_HDR_V3));
                remaining = bp_u64_sub(remaining, bp_u64_from_u32(chunk));

                /* progress callback */
                if (record_entry && ar->progress_fn) {
                    bp_progress_t p;
                    memset(&p, 0, sizeof(p));
                    p.file_index = ar->entries_written;
                    p.file_count = 0; /* unknown during creation */
                    p.file_name = name;
                    p.file_orig_size = orig64;
                    p.file_bytes_done = bp_u64_sub(file_size, remaining);
                    p.file_comp_size = total_comp;
                    p.archive_orig_total = bp_u64_add(ar->archive_orig_total,
                                                      p.file_bytes_done);
                    p.archive_comp_total = bp_u64_add(ar->archive_comp_total,
                                                      total_comp);
                    p.archive_orig_done  = bp_u64_add(ar->archive_orig_done,
                                                      p.file_bytes_done);
                    if (ar->progress_fn(&p, ar->progress_data) != 0) {
                        rc = BP_ERR_ABORTED;
                        goto seq_done;
                    }
                }
            }

            if (actual_method != BP_METHOD_STORED
                && !any_compressed_block) {
                fields[20] = BP_METHOD_STORED;
            }

            rc = BP_OK;
seq_done:
            if (rc != BP_OK) return rc;
            /* buffers and context owned by ar; freed in bp_archive_close */
            }
        } else {
            /* empty file or directory: no data to write */
            adler = bp_checksum_seed(ck_type);
            total_comp = bp_u64_from_u32(0);
        }

        /* patch compressed_size, checksum, and effective method in the fields we wrote */
        {
            bp_uint8 patch[13]; /* compressed_size(8) + checksum(4) + method(1) */
            bp_off_t end_pos;

            /* The current position is the end of THIS entry's data.  During an
             * in-place append the previous archive's file table + footer still
             * follow it on disk, so seeking to SEEK_END here would skip past
             * that stale tail and write the next entry at the old end-of-file,
             * leaving a gap.  Capture the real end and return to it explicitly. */
            end_pos = io_tell(dst_io);
            if (end_pos < 0) return BP_ERR_IO;

            bp_write_le64(patch, total_comp);
            bp_write_le32(patch + 8, adler);
            patch[12] = fields[20];

            /* seek back to fields_pos + 8 (skip original_size) */
            rc = io_seek(dst_io, fields_pos + 8, 0); /* SEEK_SET */
            if (rc != 0) return BP_ERR_IO;
            rc = write_buf(dst_io, patch, 13);
            if (rc != BP_OK) return rc;

            /* return to the end of this entry's data (not the file's end) */
            rc = io_seek(dst_io, end_pos, 0); /* SEEK_SET */
            if (rc != 0) return BP_ERR_IO;
        }

        if (packed_out != NULL) {
            packed_out->name = (char *)malloc((size_t)(name_len + 1));
            if (packed_out->name == NULL) {
                return BP_ERR_NOMEM;
            }
            memcpy(packed_out->name, name, name_len);
            packed_out->name[name_len] = '\0';
            packed_out->name_len = name_len;
            packed_out->header_size = hdr_size;
            packed_out->original_size = orig64;
            packed_out->compressed_size = total_comp;
            packed_out->checksum = adler;
            packed_out->method = fields[20];
            packed_out->checksum_type = ck_type;
            packed_out->block_size = (!bp_u64_is_zero(file_size) &&
                          entry_type != BP_TYPE_DIR &&
                          entry_type != BP_TYPE_SYMLINK)
                                  ? ar->block_size : 0;
            packed_out->entry_type = entry_type;
            packed_out->platform = platform;
            packed_out->mtime = mtime;
            packed_out->flags = flags;
            packed_out->serialized_size = bp_u64_add(bp_u64_from_u32(hdr_size),
                                                     total_comp);
        }

        if (record_entry) {
            rc = record_packed_entry(ar, entry_offset, packed_out);
            if (use_local_packed) {
                bp_packed_entry_dispose(packed_out);
            }
            return rc;
        }
    }

    return BP_OK;
}

int bp_archive_add_entry(bp_archive_t *ar, const char *name,
                         bp_uint8 entry_type, bp_uint8 platform,
                         bp_io_t *file_io, bp_uint64 file_size,
                         bp_uint64 mtime)
{
    return bp_archive_write_entry(ar, name, entry_type, platform,
                                  file_io, file_size, mtime, BP_ENTRY_FLAG_MTIME_VALID,
                                  ar->io, NULL, 1);
}

int bp_archive_add_entry_ex(bp_archive_t *ar, const char *name,
                            bp_uint8 entry_type, bp_uint8 platform,
                            bp_io_t *file_io, bp_uint64 file_size,
                            bp_uint64 mtime, bp_uint16 flags)
{
    return bp_archive_write_entry(ar, name, entry_type, platform,
                                  file_io, file_size, mtime, flags,
                                  ar->io, NULL, 1);
}

int bp_archive_pack_entry(bp_archive_t *ar, const char *name,
                          bp_uint8 entry_type, bp_uint8 platform,
                          bp_io_t *file_io, bp_uint64 file_size,
                          bp_uint64 mtime, bp_io_t *dst_io,
                          bp_packed_entry_t *packed)
{
    return bp_archive_write_entry(ar, name, entry_type, platform,
                                  file_io, file_size, mtime, BP_ENTRY_FLAG_MTIME_VALID,
                                  dst_io, packed, 0);
}

int bp_archive_pack_entry_ex(bp_archive_t *ar, const char *name,
                             bp_uint8 entry_type, bp_uint8 platform,
                             bp_io_t *file_io, bp_uint64 file_size,
                             bp_uint64 mtime, bp_io_t *dst_io,
                             bp_packed_entry_t *packed, bp_uint16 flags)
{
    return bp_archive_write_entry(ar, name, entry_type, platform,
                                  file_io, file_size, mtime, flags,
                                  dst_io, packed, 0);
}

/* Write `len` bytes to io while folding them into the running CRC-32 *crc. */
static int write_crc(bp_io_t *io, bp_uint32 *crc,
                     const bp_uint8 *buf, bp_uint32 len)
{
    *crc = bp_crc32(*crc, buf, len);
    return io_write(io, buf, len);
}

/* Write one v3 file-table record for entry e, folding bytes into *crc.
 * v3 tail = v2 fields + method(1) + header_size(2) + checksum(4). */
static int write_table_record(bp_io_t *io, bp_uint32 *crc, const bp_entry_t *e)
{
    bp_uint8 rec_hdr[2];
    bp_uint8 rec_fields[BP_TABLE_REC_TAIL_V6];
    int rc;

    bp_write_le16(rec_hdr, e->name_len);
    rc = write_crc(io, crc, rec_hdr, 2);
    if (rc != BP_OK) return rc;

    rc = write_crc(io, crc, (const bp_uint8 *)e->name, e->name_len);
    if (rc != BP_OK) return rc;

    bp_write_le64(rec_fields + 0, e->original_size);
    bp_write_le64(rec_fields + 8, e->compressed_size);
    rec_fields[16] = e->entry_type;
    rec_fields[17] = e->platform;
    bp_write_le64(rec_fields + 18, e->mtime);
    bp_write_le64(rec_fields + 26, e->offset);
    rec_fields[34] = e->method;                    /* v3: method */
    bp_write_le16(rec_fields + 35, e->header_size);/* v3: header_size */
    bp_write_le32(rec_fields + 37, e->checksum);   /* v3: payload checksum */
    bp_write_le16(rec_fields + 41, e->flags);
    return write_crc(io, crc, rec_fields, BP_TABLE_REC_TAIL_V6);
}

/* Patch the v3 CRC fields into the 32-byte header already on disk, and
 * ensure the version field reads 3.  Assumes file_count / creator /
 * archive_size are already written.  table_crc is the CRC-32 of the
 * file-table region; header_crc covers header bytes [0, BP_HEADER_CRC_OFF). */
static int patch_header_crc(bp_io_t *io, bp_uint32 table_crc)
{
    bp_uint8 hdr[BP_HEADER_SIZE];
    bp_uint32 hcrc;
    int rc;

    rc = io_seek(io, 0, 0);
    if (rc != 0) return BP_ERR_IO;
    rc = io_read(io, hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) return rc;

    bp_write_le16(hdr + 4, BP_FORMAT_VERSION);
    bp_write_le32(hdr + BP_HEADER_TABLE_CRC_OFF, table_crc);
    hcrc = bp_crc32(0, hdr, BP_HEADER_CRC_OFF);
    bp_write_le32(hdr + BP_HEADER_CRC_OFF, hcrc);

    rc = io_seek(io, 0, 0);
    if (rc != 0) return BP_ERR_IO;
    return io_write(io, hdr, BP_HEADER_SIZE);
}

int bp_archive_finalize(bp_archive_t *ar)
{
    bp_uint32 table_crc;
    int rc;
    bp_off_t table_start;
    bp_off_t table_end;
    bp_off_t archive_end;
    bp_uint32 i;
    bp_uint8 buf[32];

    if (ar == NULL) return BP_ERR_INVALID_ARG;

    {
        bp_metadata_group_t group = { NULL, 0, 0, NULL, 0 };
        for (i = 0; i < ar->entries_written; i++) {
            bp_entry_t *entry = &ar->entry_list[i];
            char *name = entry->name;
            rc = bp_metadata_group_record(&group, &name, entry->entry_type, entry->platform, 0);
            if (rc != BP_OK) {
                bp_metadata_group_close(&group);
                return rc == BP_ERR_CORRUPT ? BP_ERR_INVALID_ARG : rc;
            }
        }
        bp_metadata_group_close(&group);
    }

    table_start = io_tell(ar->io);
    if (table_start < 0) return BP_ERR_IO;

    /* -- write file table (v3) ------------------------------------------- */

    table_crc = 0;

    /* table header: magic(4) + entry_count(4) + total_orig(8) = 16 bytes (v3) */
    {
        bp_uint32 i2;
        bp_uint64 total_orig;

        total_orig.lo = 0; total_orig.hi = 0;
        for (i2 = 0; i2 < ar->entries_written; i2++) {
            total_orig = bp_u64_add(total_orig, ar->entry_list[i2].original_size);
        }
        buf[0] = BP_TABLE_MAGIC_0;
        buf[1] = BP_TABLE_MAGIC_1;
        buf[2] = BP_TABLE_MAGIC_2;
        buf[3] = BP_TABLE_MAGIC_3;
        bp_write_le32(buf + 4, ar->entries_written);
        bp_write_le64(buf + 8, total_orig);
        rc = write_crc(ar->io, &table_crc, buf, BP_TABLE_HDR_V3);
        if (rc != BP_OK) return rc;
    }

    /* table records (v3: carry method + header_size + checksum) */
    for (i = 0; i < ar->entries_written; i++) {
        rc = write_table_record(ar->io, &table_crc, &ar->entry_list[i]);
        if (rc != BP_OK) return rc;
    }

    table_end = io_tell(ar->io);
    if (table_end < 0) return BP_ERR_IO;

    /* -- write footer ----------------------------------------------------- */
    {
        bp_uint8 footer[BP_FOOTER_SIZE];
        bp_uint64 tbl_off = bp_off_to_u64(table_start);
        bp_uint64 tbl_sz  = bp_off_to_u64(table_end - table_start);

        bp_write_le64(footer + 0, tbl_off);
        bp_write_le64(footer + 8, tbl_sz);
        footer[16] = BP_FOOTER_MAGIC_0;
        footer[17] = BP_FOOTER_MAGIC_1;
        footer[18] = BP_FOOTER_MAGIC_2;
        footer[19] = BP_FOOTER_MAGIC_3;

        rc = write_buf(ar->io, footer, BP_FOOTER_SIZE);
        if (rc != BP_OK) return rc;
    }

    archive_end = io_tell(ar->io);
    if (archive_end < 0) return BP_ERR_IO;

    /* -- rebuild the full 32-byte header (final counts + v3 CRCs) and write
     * it once.  Building in memory instead of reading the placeholder back
     * keeps finalize working on write-only (but seekable) output streams. */
    {
        bp_uint8  hdr[BP_HEADER_SIZE];
        bp_uint32 hcrc;

        memset(hdr, 0, BP_HEADER_SIZE);
        hdr[0] = BP_MAGIC_0;
        hdr[1] = BP_MAGIC_1;
        hdr[2] = BP_MAGIC_2;
        hdr[3] = BP_MAGIC_3;
        bp_write_le16(hdr + 4, BP_FORMAT_VERSION);
        bp_write_le32(hdr + 8, ar->entries_written);
        hdr[12] = ar->creator_os;
        hdr[13] = ar->creator_cpu;
        hdr[14] = BP_VERSION_MAJOR;
        hdr[15] = BP_VERSION_MINOR;
        bp_write_le64(hdr + 16, bp_off_to_u64(archive_end));
        bp_write_le32(hdr + BP_HEADER_TABLE_CRC_OFF, table_crc);
        hcrc = bp_crc32(0, hdr, BP_HEADER_CRC_OFF);
        bp_write_le32(hdr + BP_HEADER_CRC_OFF, hcrc);

        rc = io_seek(ar->io, 0, 0);
        if (rc != 0) return BP_ERR_IO;
        rc = write_buf(ar->io, hdr, BP_HEADER_SIZE);
        if (rc != BP_OK) return rc;
    }

    return BP_OK;
}

void bp_archive_set_log(bp_archive_t *ar, bp_log_fn fn, void *user_data)
{
    if (ar == NULL) return;
    ar->log_fn   = fn;
    ar->log_data = user_data;
}

void bp_archive_close(bp_archive_t *ar)
{
    bp_uint32 i;
    if (ar == NULL) return;
#ifdef BOLTPACK_FAST_FLUFF
    bp_fast_fluff_release(ar);
    bp_fast_boltlz_release(ar);
#endif
    if (ar->entry_list) {
        for (i = 0; i < ar->entries_written; i++) {
            free(ar->entry_list[i].name);
        }
        free(ar->entry_list);
        ar->entry_list = NULL;
    }
    if (ar->seq_fluff_ctx) {
        fluff_ctx_free((fluff_ctx_t *)ar->seq_fluff_ctx);
        ar->seq_fluff_ctx = NULL;
    }
    free(ar->seq_in_buf);  ar->seq_in_buf  = NULL;
    free(ar->seq_out_buf); ar->seq_out_buf = NULL;
    ar->seq_buf_block_size = 0;
    free(ar->seq_boltlz_work); ar->seq_boltlz_work = NULL;
}

/* ---- reading: shared helpers ------------------------------------------- */

/* Read footer to find file table offset */
static int read_footer(bp_io_t *io, bp_uint64 *table_offset, bp_uint64 *table_size)
{
    bp_uint8 buf[BP_FOOTER_SIZE];
    int rc;

    /* seek to end - 20 */
    rc = io_seek(io, -BP_FOOTER_SIZE, 2); /* SEEK_END */
    if (rc != 0) return BP_ERR_IO;

    rc = io_read(io, buf, BP_FOOTER_SIZE);
    if (rc != BP_OK) return rc;

    /* check footer magic */
    if (buf[16] != BP_FOOTER_MAGIC_0 || buf[17] != BP_FOOTER_MAGIC_1 ||
        buf[18] != BP_FOOTER_MAGIC_2 || buf[19] != BP_FOOTER_MAGIC_3) {
        return BP_ERR_MAGIC;
    }

    *table_offset = bp_read_le64(buf);
    *table_size = bp_read_le64(buf + 8);
    return BP_OK;
}

/* Read archive header and validate.  Accepts only archive format 6.
 * out_version (may be NULL) receives the on-disk version so callers can
 * parse version-specific structures (e.g. the file-table record layout).
 * The 32-byte header's own CRC-32 is verified here. */
static int read_header(bp_io_t *io, bp_uint32 *file_count,
                       bp_uint16 *out_version)
{
    bp_uint8 hdr[BP_HEADER_SIZE];
    bp_uint16 version;
    int rc;

    rc = io_seek(io, 0, 0); /* SEEK_SET */
    if (rc != 0) return BP_ERR_IO;

    rc = io_read(io, hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) return rc;

    if (hdr[0] != BP_MAGIC_0 || hdr[1] != BP_MAGIC_1 ||
        hdr[2] != BP_MAGIC_2 || hdr[3] != BP_MAGIC_3) {
        return BP_ERR_MAGIC;
    }

    version = bp_read_le16(hdr + 4);
    if (version != BP_FORMAT_VERSION) {
        return BP_ERR_VERSION;
    }

    /* Format 6 reserves the archive flags field at bytes 6-7. */
    if (bp_read_le16(hdr + 6) != 0) return BP_ERR_CORRUPT;

    if (version == BP_FORMAT_VERSION) {
        bp_uint32 stored = bp_read_le32(hdr + BP_HEADER_CRC_OFF);
        bp_uint32 calc   = bp_crc32(0, hdr, BP_HEADER_CRC_OFF);
        /* Metadata integrity failure is structural, not a data-block
         * checksum: use BP_ERR_CORRUPT so frontends treat it as a hard
         * error (BP_ERR_CHECKSUM is reserved for per-entry payloads). */
        if (stored != calc) return BP_ERR_CORRUPT;
    }

    *file_count = bp_read_le32(hdr + 8);
    if (out_version != NULL) *out_version = version;
    return BP_OK;
}

/* Seek to and validate the file table, return entry count.
 * Combines: read_footer -> seek -> read table header -> check magic.
 * v2: 8-byte header; v3: 16-byte header (extra 8 bytes are total_orig, skipped).
 * If out_tbl_off is non-NULL, it receives the byte offset of the table. */
static int open_file_table(bp_io_t *io, bp_uint32 *out_count,
                           bp_uint64 *out_tbl_off, bp_uint16 version)
{
    bp_uint64 tbl_off, tbl_sz;
    bp_uint8 tbl_hdr[BP_TABLE_HDR_V3]; /* large enough for either version */
    bp_uint32 hdr_sz;
    int rc;

    hdr_sz = (version >= BP_FORMAT_VERSION) ? BP_TABLE_HDR_V3 : BP_TABLE_HDR_V2;

    rc = read_footer(io, &tbl_off, &tbl_sz);
    if (rc != BP_OK) return rc;

    rc = io_seek(io, bp_u64_to_off(tbl_off), 0);
    if (rc != 0) return BP_ERR_IO;

    rc = io_read(io, tbl_hdr, hdr_sz);
    if (rc != BP_OK) return rc;

    if (tbl_hdr[0] != BP_TABLE_MAGIC_0 || tbl_hdr[1] != BP_TABLE_MAGIC_1 ||
        tbl_hdr[2] != BP_TABLE_MAGIC_2 || tbl_hdr[3] != BP_TABLE_MAGIC_3)
        return BP_ERR_CORRUPT;

    *out_count = bp_read_le32(tbl_hdr + 4);
    if (out_tbl_off != NULL) *out_tbl_off = tbl_off;
    return BP_OK;
}

/* For v3 archives, verify:
 *   1. the file-table CRC-32 stored in the header;
 *   2. the archive_size field matches the actual file size.
 * A no-op returning BP_OK for v2 archives. */
static int verify_table_entries(bp_io_t *io, bp_uint16 version,
                                bp_uint32 file_count);

static int verify_table_crc(bp_io_t *io, bp_uint16 version)
{
    bp_uint8 hdr[BP_HEADER_SIZE];
    bp_uint64 tbl_off, tbl_sz, rem;
    bp_uint32 stored, calc;
    bp_uint8 buf[512];
    int rc;

    if (version < BP_FORMAT_VERSION) return BP_OK;

    rc = io_seek(io, 0, 0);
    if (rc != 0) return BP_ERR_IO;
    rc = io_read(io, hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) return rc;
    stored = bp_read_le32(hdr + BP_HEADER_TABLE_CRC_OFF);

    rc = read_footer(io, &tbl_off, &tbl_sz);
    if (rc != BP_OK) return rc;

    /* archive_size normative check: stored value must equal actual file size */
    {
        bp_off_t actual_end = io_tell(io);
        bp_uint64 stored_sz  = bp_read_le64(hdr + 16);
        bp_uint64 table_end = bp_u64_add(tbl_off, tbl_sz);
        bp_uint64 expected = bp_u64_add(table_end,
                                bp_u64_from_u32((bp_uint32)BP_FOOTER_SIZE));
        if (actual_end < 0 || bp_u64_cmp(table_end, tbl_off) < 0 ||
            bp_u64_cmp(expected, table_end) < 0 ||
            bp_u64_cmp(expected, bp_off_to_u64(actual_end)) != 0 ||
            bp_u64_cmp(stored_sz, expected) != 0)
            return BP_ERR_TRUNCATED;
    }

    rc = io_seek(io, bp_u64_to_off(tbl_off), 0);
    if (rc != 0) return BP_ERR_IO;

    calc = 0;
    rem = tbl_sz;
    while (!bp_u64_is_zero(rem)) {
        bp_uint32 chunk = (rem.hi != 0 || rem.lo > (bp_uint32)sizeof(buf))
                          ? (bp_uint32)sizeof(buf) : rem.lo;
        rc = io_read(io, buf, chunk);
        if (rc != BP_OK) return rc;
        calc = bp_crc32(calc, buf, chunk);
        rem = bp_u64_sub(rem, bp_u64_from_u32(chunk));
    }
    if (calc != stored) return BP_ERR_CORRUPT; /* structural, not data checksum */
    return verify_table_entries(io, version, bp_read_le32(hdr + 8));
}

/* Read a single entry from the file table.
 * io must be positioned at the start of the table record.
 * version selects the record layout: v2 (34-byte tail) or v3 (41-byte tail
 * with method + header_size + checksum).  Caller must free entry->name. */
static int read_table_entry(bp_io_t *io, bp_entry_t *entry, bp_uint16 version)
{
    bp_uint8 buf[BP_TABLE_REC_TAIL_V6];
    bp_uint32 tail;
    int rc;

    memset(entry, 0, sizeof(*entry));
    tail = (version >= BP_FORMAT_VERSION) ? BP_TABLE_REC_TAIL_V6
                                          : BP_TABLE_REC_TAIL_V2;

    /* name_len (2 bytes) */
    rc = io_read(io, buf, 2);
    if (rc != BP_OK) return rc;
    entry->name_len = bp_read_le16(buf);
    if (entry->name_len == 0 || entry->name_len > BP_MAX_NAME_LEN)
        return BP_ERR_CORRUPT;

    /* name */
    entry->name = (char *)malloc((size_t)(entry->name_len + 1));
    if (entry->name == NULL) return BP_ERR_NOMEM;
    rc = io_read(io, entry->name, entry->name_len);
    if (rc != BP_OK) { free(entry->name); entry->name = NULL; return rc; }
    if (!bp_archive_name_valid(entry->name, (bp_uint32)entry->name_len)) {
        free(entry->name); entry->name = NULL; return BP_ERR_CORRUPT;
    }
    entry->name[entry->name_len] = '\0';

    /* fixed tail: original_size(8) + compressed_size(8) + entry_type(1) +
     * platform(1) + mtime(8) + offset(8) [+ v3: method(1) + header_size(2) +
     * checksum(4)] */
    rc = io_read(io, buf, tail);
    if (rc != BP_OK) { free(entry->name); entry->name = NULL; return rc; }

    entry->original_size  = bp_read_le64(buf + 0);
    entry->compressed_size = bp_read_le64(buf + 8);
    entry->entry_type     = buf[16];
    entry->platform       = buf[17];
    entry->mtime          = bp_read_le64(buf + 18);
    entry->offset         = bp_read_le64(buf + 26);
    if (tail >= BP_TABLE_REC_TAIL_V3) {
        entry->method      = buf[34];
        entry->header_size = bp_read_le16(buf + 35);
        entry->checksum    = bp_read_le32(buf + 37);
    }
    if (tail >= BP_TABLE_REC_TAIL_V6)
        entry->flags = bp_read_le16(buf + 41);
    if (!bp_archive_name_type_valid(entry->name, entry->name_len,
                                    entry->entry_type)) {
        free(entry->name); entry->name = NULL; return BP_ERR_CORRUPT;
    }
    if (!(entry->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
        !bp_u64_is_zero(entry->mtime)) {
        free(entry->name); entry->name = NULL; return BP_ERR_CORRUPT;
    }

    return BP_OK;
}

/* Read entry header from archive (at entry offset).
 * Fills entry fields from the on-disk header. */
static int read_entry_header(bp_io_t *io, bp_entry_t *entry)
{
    bp_uint8 buf[38];
    bp_uint8 pre[4];
    int rc;

    memset(entry, 0, sizeof(*entry));

    /* header_size(2) + name_len(2) */
    rc = io_read(io, pre, 4);
    if (rc != BP_OK) return rc;

    entry->header_size = bp_read_le16(pre);
    entry->name_len    = bp_read_le16(pre + 2);

    /* Fixed fields must fit inside the declared header before reading them. */
    if (entry->name_len == 0 || entry->name_len > BP_MAX_NAME_LEN ||
        (bp_uint32)entry->header_size !=
            (bp_uint32)BP_ENTRY_FIXED + (bp_uint32)entry->name_len)
        return BP_ERR_CORRUPT;

    entry->name = (char *)malloc((size_t)(entry->name_len + 1));
    if (entry->name == NULL) return BP_ERR_NOMEM;
    rc = io_read(io, entry->name, entry->name_len);
    if (rc != BP_OK) { free(entry->name); entry->name = NULL; return rc; }
    if (!bp_archive_name_valid(entry->name, (bp_uint32)entry->name_len)) {
        free(entry->name); entry->name = NULL; return BP_ERR_CORRUPT;
    }
    entry->name[entry->name_len] = '\0';

    /* remaining 38 bytes of fixed fields */
    rc = io_read(io, buf, 38);
    if (rc != BP_OK) { free(entry->name); entry->name = NULL; return rc; }

    entry->original_size   = bp_read_le64(buf + 0);
    entry->compressed_size = bp_read_le64(buf + 8);
    entry->checksum        = bp_read_le32(buf + 16);
    entry->method          = buf[20];
    entry->checksum_type   = buf[21];
    entry->block_size      = bp_read_le32(buf + 22);
    entry->entry_type      = buf[26];
    entry->platform        = buf[27];
    entry->mtime           = bp_read_le64(buf + 28);
    entry->flags           = bp_read_le16(buf + 36);

    if (!bp_archive_name_type_valid(entry->name, entry->name_len,
                                    entry->entry_type)) {
        free(entry->name); entry->name = NULL; return BP_ERR_CORRUPT;
    }
    if (!(entry->flags & BP_ENTRY_FLAG_MTIME_VALID) &&
        !bp_u64_is_zero(entry->mtime)) {
        free(entry->name); entry->name = NULL; return BP_ERR_CORRUPT;
    }

    /* Format 6 requires CRC-32; reject retired or unknown checksum kinds. */
    if (entry->checksum_type != BP_CKSUM_CRC32) {
        free(entry->name); entry->name = NULL;
        return BP_ERR_CORRUPT;
    }

    /* skip any unknown trailing header bytes (forward compatibility) */
    {
        bp_uint16 expected = (bp_uint16)(BP_ENTRY_FIXED + entry->name_len);
        if (entry->header_size > expected) {
            bp_uint16 skip = entry->header_size - expected;
            rc = io_seek(io, (long)skip, 1); /* SEEK_CUR */
            if (rc != 0) {
                free(entry->name); entry->name = NULL;
                return BP_ERR_IO;
            }
        }
    }

    return BP_OK;
}

/* Validate duplicated table and inline fields before any extraction or
 * archive edit can use them. Entry payloads occupy one contiguous region. */
static int verify_table_entries(bp_io_t *io, bp_uint16 version,
                                bp_uint32 file_count)
{
    bp_uint32 count, i;
    bp_uint64 table_off, table_size, next;
    int rc;
    bp_metadata_group_t group = { NULL, 0, 0, NULL, 0 };

    rc = read_footer(io, &table_off, &table_size);
    if (rc != BP_OK) return rc;
    rc = open_file_table(io, &count, NULL, version);
    if (rc != BP_OK) return rc;
    if (count != file_count) return BP_ERR_CORRUPT;
    next = bp_u64_from_u32(BP_HEADER_SIZE);
    for (i = 0; i < count; i++) {
        bp_entry_t table, header;
        bp_off_t resume;
        bp_uint64 end;
        int matches;

        rc = read_table_entry(io, &table, version);
        if (rc != BP_OK) { bp_metadata_group_close(&group); return rc; }
        resume = io_tell(io);
        if (resume < 0 || bp_u64_cmp(table.offset, next) != 0 ||
            bp_u64_cmp(table.offset, table_off) >= 0) {
            free(table.name); bp_metadata_group_close(&group); return BP_ERR_CORRUPT;
        }
        rc = io_seek(io, bp_u64_to_off(table.offset), 0);
        if (rc != 0) { free(table.name); bp_metadata_group_close(&group); return BP_ERR_IO; }
        rc = read_entry_header(io, &header);
        if (rc != BP_OK) { free(table.name); bp_metadata_group_close(&group); return rc; }
        matches = table.name_len == header.name_len &&
                  memcmp(table.name, header.name, table.name_len) == 0 &&
                  table.header_size == header.header_size &&
                  bp_u64_cmp(table.original_size, header.original_size) == 0 &&
                  bp_u64_cmp(table.compressed_size, header.compressed_size) == 0 &&
                  table.checksum == header.checksum &&
                  table.method == header.method &&
                  table.entry_type == header.entry_type &&
                  table.platform == header.platform &&
                  bp_u64_cmp(table.mtime, header.mtime) == 0 &&
                  table.flags == header.flags;
        end = bp_u64_add(table.offset, bp_u64_from_u32(table.header_size));
        if (bp_u64_cmp(end, table.offset) < 0) matches = 0;
        next = bp_u64_add(end, table.compressed_size);
        if (bp_u64_cmp(next, end) < 0 || bp_u64_cmp(next, table_off) > 0)
            matches = 0;
        free(header.name);
        if (matches) {
            rc = bp_metadata_group_record(&group, &table.name,
                                          table.entry_type, table.platform, 1);
            if (rc != BP_OK) {
                free(table.name); bp_metadata_group_close(&group); return rc;
            }
        }
        free(table.name);
        if (!matches) { bp_metadata_group_close(&group); return BP_ERR_CORRUPT; }
        if (io_seek(io, resume, 0) != 0) { bp_metadata_group_close(&group); return BP_ERR_IO; }
    }
    bp_metadata_group_close(&group);
    if (io_tell(io) < 0 ||
        bp_u64_cmp(bp_off_to_u64(io_tell(io)),
                   bp_u64_add(table_off, table_size)) != 0)
        return BP_ERR_CORRUPT;
    return bp_u64_cmp(next, table_off) == 0 ? BP_OK : BP_ERR_CORRUPT;
}

/* Populate method, checksum, checksum_type and header_size on `entry` by
 * reading its on-disk entry header at entry->offset.  Used to upgrade a v2
 * file-table record (which lacks these fields) to the v3 table layout.
 * Restores the I/O position afterward. */
static int backfill_entry_from_header(bp_io_t *io, bp_entry_t *entry)
{
    bp_off_t save;
    bp_entry_t h;
    int rc;

    save = io_tell(io);
    if (save < 0) return BP_ERR_IO;
    rc = io_seek(io, bp_u64_to_off(entry->offset), 0);
    if (rc != 0) return BP_ERR_IO;
    rc = read_entry_header(io, &h);
    if (rc != BP_OK) return rc;

    entry->method        = h.method;
    entry->checksum      = h.checksum;
    entry->checksum_type = h.checksum_type;
    entry->header_size   = h.header_size;
    free(h.name);

    rc = io_seek(io, save, 0);
    if (rc != 0) return BP_ERR_IO;
    return BP_OK;
}

/* ---- in-place append --------------------------------------------------- */

int bp_archive_query_tail(bp_io_t *io,
                         bp_uint64 *tail_off,
                         bp_uint64 *tail_size)
{
    bp_uint64 tbl_off, tbl_sz;
    int rc;

    if (io == NULL || tail_off == NULL || tail_size == NULL)
        return BP_ERR_INVALID_ARG;

    rc = read_footer(io, &tbl_off, &tbl_sz);
    if (rc != BP_OK) return rc;

    *tail_off  = tbl_off;
    /* tail includes file-table bytes plus the fixed-size footer */
    *tail_size = bp_u64_add(tbl_sz, bp_u64_from_u32((bp_uint32)BP_FOOTER_SIZE));
    return BP_OK;
}

int bp_archive_open_append(bp_archive_t *ar, bp_io_t *io,
                           bp_uint32 block_size, bp_uint8 method)
{
    bp_uint8 hdr[BP_HEADER_SIZE];
    bp_uint32 src_count;
    bp_uint64 tbl_off;
    bp_uint32 tbl_count, i;
    bp_uint16 src_version;
    int rc;

    if (ar == NULL || io == NULL) return BP_ERR_INVALID_ARG;
    if (!bp_method_is_supported(method)) return BP_ERR_UNSUPPORTED_METHOD;

    if (method == BP_METHOD_BOLTLZ && block_size > BOLTLZ_MAX_BLOCK)
        return BP_ERR_INVALID_ARG;

    memset(ar, 0, sizeof(*ar));
    ar->io = io;
    ar->block_size = block_size ? block_size : BP_BLOCK_DEFAULT;
    ar->method = method;
    ar->method_level = 0;
    ar->entries_written = 0;
    ar->entry_list = NULL;
    ar->entry_list_cap = 0;
    {
        bp_uint64 z = {0, 0};
        ar->archive_orig_total = z;
        ar->archive_comp_total = z;
        ar->archive_orig_done  = z;
    }

    /* read and validate header */
    rc = io_seek(io, 0, 0);
    if (rc != 0) return BP_ERR_IO;
    rc = io_read(io, hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) return rc;

    if (hdr[0] != BP_MAGIC_0 || hdr[1] != BP_MAGIC_1 ||
        hdr[2] != BP_MAGIC_2 || hdr[3] != BP_MAGIC_3)
        return BP_ERR_MAGIC;
    src_version = bp_read_le16(hdr + 4);
    if (src_version != BP_FORMAT_VERSION) return BP_ERR_VERSION;
    if (src_version == BP_FORMAT_VERSION) {
        bp_uint32 stored = bp_read_le32(hdr + BP_HEADER_CRC_OFF);
        bp_uint32 calc   = bp_crc32(0, hdr, BP_HEADER_CRC_OFF);
        if (stored != calc) return BP_ERR_CORRUPT;
    }
    src_count = bp_read_le32(hdr + 8);

    /* preserve creator fields from existing archive */
    ar->creator_os  = hdr[12];
    ar->creator_cpu = hdr[13];

    rc = verify_table_crc(io, src_version);
    if (rc != BP_OK) return rc;

    /* read file table */
    rc = open_file_table(io, &tbl_count, &tbl_off, src_version);
    if (rc != BP_OK) return rc;
    if (tbl_count != src_count) return BP_ERR_CORRUPT;

    /* load existing entries into ar->entry_list.  Entry data (and per-entry
     * headers) are byte-identical across v2/v3, so a v2 archive is upgraded
     * to v3 transparently when bp_archive_finalize rewrites the table. */
    for (i = 0; i < src_count; i++) {
        bp_entry_t e;

        rc = entry_list_grow(ar);
        if (rc != BP_OK) { bp_archive_close(ar); return rc; }

        rc = read_table_entry(io, &e, src_version);
        if (rc != BP_OK) { bp_archive_close(ar); return rc; }

        if (src_version < BP_FORMAT_VERSION) {
            rc = backfill_entry_from_header(io, &e);
            if (rc != BP_OK) { free(e.name); bp_archive_close(ar); return rc; }
        }

        ar->entry_list[ar->entries_written] = e; /* takes ownership of e.name */
        ar->entries_written++;

        ar->archive_orig_total = bp_u64_add(ar->archive_orig_total,
                                            e.original_size);
        ar->archive_comp_total = bp_u64_add(ar->archive_comp_total,
                                            e.compressed_size);
        ar->archive_orig_done  = bp_u64_add(ar->archive_orig_done,
                                            e.original_size);
    }

    /* Seek to where the file table was — this is where new entries will
     * be written, effectively overwriting the old table and footer. */
    rc = io_seek(io, bp_u64_to_off(tbl_off), 0);
    if (rc != 0) { bp_archive_close(ar); return BP_ERR_IO; }

    return BP_OK;
}

/* ---- listing ----------------------------------------------------------- */

int bp_archive_list(bp_io_t *io, bp_list_fn fn, void *user_data)
{
    bp_uint32 file_count, table_count;
    bp_uint32 i;
    bp_uint16 version;
    int rc;

    if (io == NULL || fn == NULL) return BP_ERR_INVALID_ARG;

    rc = read_header(io, &file_count, &version);
    if (rc != BP_OK) return rc;

    rc = verify_table_crc(io, version);
    if (rc != BP_OK) return rc;

    rc = open_file_table(io, &table_count, NULL, version);
    if (rc != BP_OK) return rc;
    if (table_count != file_count) return BP_ERR_CORRUPT;

    for (i = 0; i < table_count; i++) {
        bp_entry_t entry;

        rc = read_table_entry(io, &entry, version);
        if (rc != BP_OK) return rc;

        if (version < BP_FORMAT_VERSION) {
            /* v2 tables omit the method field; read it from the entry header.
             * Entry header layout at entry.offset:
             *   header_size(2) + name_len(2) + name(name_len) +
             *   orig_size(8) + comp_size(8) + checksum(4) + method(1) = 4+name_len+20 */
            bp_off_t tbl_resume = io_tell(io);
            bp_off_t method_off = bp_u64_to_off(entry.offset) +
                                  (bp_off_t)(4 + (int)entry.name_len + 20);
            bp_uint8 mbyte = 0;
            if (io_seek(io, method_off, 0) == 0)
                io_read(io, &mbyte, 1); /* best-effort; ignore error */
            entry.method = mbyte;
            io_seek(io, tbl_resume, 0); /* restore file table position */
        }
        /* v3: entry.method came straight from the table record — no seek. */

        rc = fn(&entry, user_data);
        free(entry.name);
        if (rc != 0) return BP_ERR_ABORTED;
    }

    return BP_OK;
}

/* ---- extraction -------------------------------------------------------- */

/* Pre-scan the file table to sum all original_size values.
 * Skips entry names via SEEK_CUR to avoid malloc.  The trailing skip depends
 * on the record layout: v2 records have 26 bytes after original_size, v3
 * records have 33 (the extra method + header_size + checksum). */
static bp_uint64 scan_archive_orig_total(bp_io_t *io, bp_uint16 version)
{
    bp_uint64 total;
    bp_uint32 count, i;
    bp_uint8 buf[10]; /* name_len(2) + original_size(8) */
    bp_uint16 name_len;
    bp_off_t rest_skip;

    total.lo = 0; total.hi = 0;

    /* v3: total_orig is cached at offset 8 in the 16-byte table header */
    if (version >= BP_FORMAT_VERSION) {
        bp_uint64 tbl_off, tbl_sz;
        bp_uint8 hdr[BP_TABLE_HDR_V3];
        if (read_footer(io, &tbl_off, &tbl_sz) != BP_OK) return total;
        if (io_seek(io, bp_u64_to_off(tbl_off), 0) != 0)  return total;
        if (io_read(io, hdr, BP_TABLE_HDR_V3) != BP_OK)    return total;
        if (hdr[0] != BP_TABLE_MAGIC_0 || hdr[1] != BP_TABLE_MAGIC_1 ||
            hdr[2] != BP_TABLE_MAGIC_2 || hdr[3] != BP_TABLE_MAGIC_3)
            return total;
        return bp_read_le64(hdr + 8);
    }

    /* v2: sum per-record (legacy path) */
    rest_skip = (bp_off_t)(BP_TABLE_REC_TAIL_V2 - 8);
    if (open_file_table(io, &count, NULL, version) != BP_OK) return total;
    for (i = 0; i < count; i++) {
        if (io_read(io, buf, 2) != BP_OK) break;
        name_len = bp_read_le16(buf);
        if (io_seek(io, (bp_off_t)name_len, 1) != 0) break; /* SEEK_CUR */
        if (io_read(io, buf + 2, 8) != BP_OK) break;
        total = bp_u64_add(total, bp_read_le64(buf + 2));
        if (io_seek(io, rest_skip, 1) != 0) break; /* skip rest of record */
    }
    return total;
}

/* Decompress/copy all data blocks for one entry */
static int extract_entry_data(bp_io_t *io, bp_entry_t *entry,
                              bp_io_t *out,
                              bp_progress_fn progress_fn,
                              void *user_data,
                              bp_uint32 file_index,
                              bp_uint32 file_count,
                              bp_uint64 archive_orig_total,
                              bp_uint64 archive_orig_done,
                              bp_uint32 *out_checksum,
                              bp_uint16 version)
{
    bp_uint8  ck_type     = BP_CKSUM_CRC32;
    bp_uint32 adler       = bp_checksum_seed(ck_type);
    bp_uint64 remaining   = entry->original_size;
    bp_uint32 blk_hdr_sz  = (version >= BP_FORMAT_VERSION)
                            ? BP_BLOCK_HDR_V3 : BP_BLOCK_HDR_V2;
    bp_uint8 *in_buf = NULL;
    bp_uint8 *dec_buf = NULL;
    int rc;

    if (bp_u64_is_zero(remaining)) {
        *out_checksum = bp_checksum_seed(ck_type);
        return BP_OK;
    }

    /* Symlink payloads are raw in fixed archives, but older archives wrote
     * them through the normal block path.  Treat equal compressed/original
     * sizes as raw and fall back to block decoding for legacy archives. */
    if (entry->entry_type == BP_TYPE_SYMLINK &&
        bp_u64_cmp(entry->compressed_size, entry->original_size) == 0) {
        bp_uint32 sz = bp_u64_to_u32(entry->original_size);
        in_buf = (bp_uint8 *)malloc((size_t)sz);
        if (in_buf == NULL) return BP_ERR_NOMEM;
        rc = io_read(io, in_buf, sz);
        if (rc != BP_OK) { free(in_buf); return rc; }
        adler = bp_checksum_typed(ck_type, adler, in_buf, sz);
        if (out) {
            rc = io_write(out, in_buf, sz);
            if (rc != BP_OK) { free(in_buf); return rc; }
        }
        free(in_buf);
        *out_checksum = adler;
        return BP_OK;
    }

    /* block-based decompression */
    {
        bp_uint32 bsize = entry->block_size;
        bp_uint32 alloc_in;
        fluff_ctx_t *dfctx = NULL;

        if (bsize == 0) {
            /* single blob mode */
            if (entry->original_size.hi != 0) {
                return BP_ERR_CORRUPT;
            }
            bsize = bp_u64_to_u32(entry->original_size);
        }

        /* Reject block sizes no conforming writer produces before they
         * reach malloc: both bsize and the compress bound below are
         * attacker-controlled via the entry header. */
        if (bsize > BP_BLOCK_READ_CAP ||
            (entry->method == BP_METHOD_BOLTLZ && bsize > BOLTLZ_MAX_BLOCK)) {
            return BP_ERR_CORRUPT;
        }

        /* worst-case compressed block can be slightly larger than original */
        alloc_in = bp_method_compress_bound(entry->method, bsize);
        in_buf = (bp_uint8 *)malloc((size_t)alloc_in);
        if (in_buf == NULL) return BP_ERR_NOMEM;

        dec_buf = (bp_uint8 *)malloc((size_t)bsize);
        if (dec_buf == NULL) { free(in_buf); return BP_ERR_NOMEM; }

        if (entry->method == BP_METHOD_FLUFF) {
            dfctx = fluff_ctx_alloc();
            if (dfctx == NULL) { free(in_buf); free(dec_buf); return BP_ERR_NOMEM; }
            /* Decode-only: the lightweight reset skips the compress-only
             * match-finder tables, so this context stays ~0.27 MB instead of
             * ~2 MB. */
            fluff_ctx_reset_decode(dfctx);
        }

        while (!bp_u64_is_zero(remaining)) {
            bp_uint8 blk_hdr[BP_BLOCK_HDR_V3]; /* large enough for either version */
            bp_uint32 comp_sz, orig_sz;
            bp_uint32 dec_len;

            rc = io_read(io, blk_hdr, blk_hdr_sz);
            if (rc != BP_OK) { if (dfctx) { fluff_ctx_free(dfctx); dfctx = NULL; } free(in_buf); free(dec_buf); return rc; }

            comp_sz = bp_read_le32(blk_hdr);
            orig_sz = bp_read_le32(blk_hdr + 4);

            if (orig_sz > bsize || comp_sz > alloc_in) {
                if (dfctx) { fluff_ctx_free(dfctx); dfctx = NULL; }
                free(in_buf); free(dec_buf);
                return BP_ERR_CORRUPT;
            }
            if (orig_sz == 0 ||
                bp_u64_cmp(bp_u64_from_u32(orig_sz), remaining) > 0) {
                if (dfctx) { fluff_ctx_free(dfctx); dfctx = NULL; }
                free(in_buf); free(dec_buf);
                return BP_ERR_CORRUPT;
            }

            rc = io_read(io, in_buf, comp_sz);
            if (rc != BP_OK) { if (dfctx) { fluff_ctx_free(dfctx); dfctx = NULL; } free(in_buf); free(dec_buf); return rc; }

            /* v3: verify block CRC before decompression */
            if (blk_hdr_sz == BP_BLOCK_HDR_V3) {
                bp_uint32 stored_crc = bp_read_le32(blk_hdr + 8);
                bp_uint32 calc_crc   = bp_crc32(0, in_buf, comp_sz);
                if (stored_crc != calc_crc) {
                    if (dfctx) { fluff_ctx_free(dfctx); dfctx = NULL; }
                    free(in_buf); free(dec_buf);
                    return BP_ERR_CHECKSUM;
                }
            }

            if (comp_sz == orig_sz) {
                /* stored block */
                memcpy(dec_buf, in_buf, (size_t)orig_sz);
                /* For fluff-method files the compressor advanced its context
                 * via fluff_compress_block_ctx before deciding to store the
                 * block verbatim.  Mirror that ring-buffer advance here so
                 * subsequent cross-block back-references resolve correctly. */
                if (entry->method == BP_METHOD_FLUFF && dfctx != NULL) {
                    fluff_ctx_feed_raw(dfctx, dec_buf, orig_sz);
                }
            } else if (entry->method == BP_METHOD_BOLTLITE) {
                /* Distance-based BoltLite decode needs no scratch buffer. */
                rc = boltlite_decompress(in_buf, comp_sz, dec_buf, orig_sz, NULL);
                if (rc != BP_OK) { free(in_buf); free(dec_buf); return rc; }
            } else if (entry->method == BP_METHOD_BOLTLZ) {
                dec_len = orig_sz;
                rc = bp_decompress_block(in_buf, comp_sz, dec_buf, &dec_len);
                if (rc != BP_OK) { free(in_buf); free(dec_buf); return rc; }
            } else if (entry->method == BP_METHOD_FLUFF) {
                dec_len = orig_sz;
                rc = fluff_decompress_block_ctx(in_buf, comp_sz, dec_buf,
                                                &dec_len, dfctx);
                if (rc != BP_OK) {
                    if (dfctx) { fluff_ctx_free(dfctx); dfctx = NULL; }
                    free(in_buf); free(dec_buf); return rc;
                }
                if (dec_len != orig_sz) {
                    if (dfctx) { fluff_ctx_free(dfctx); dfctx = NULL; }
                    free(in_buf); free(dec_buf);
                    return BP_ERR_CORRUPT;
                }
            }
#ifdef BOLTPACK_ZSTD
            else if (entry->method == BP_METHOD_ZSTD) {
                dec_len = orig_sz;
                rc = bp_zstd_decompress_block(in_buf, comp_sz, dec_buf, &dec_len);
                if (rc != BP_OK) {
                    free(in_buf); free(dec_buf); return rc;
                }
                if (dec_len != orig_sz) {
                    free(in_buf); free(dec_buf);
                    return BP_ERR_CORRUPT;
                }
            }
#endif /* BOLTPACK_ZSTD */
            else {
                if (dfctx) { fluff_ctx_free(dfctx); dfctx = NULL; }
                free(in_buf); free(dec_buf);
                return BP_ERR_UNSUPPORTED_METHOD;
            }

            adler = bp_checksum_typed(ck_type, adler, dec_buf, orig_sz);

            if (out) {
                rc = io_write(out, dec_buf, orig_sz);
                if (rc != BP_OK) { if (dfctx) { fluff_ctx_free(dfctx); dfctx = NULL; } free(in_buf); free(dec_buf); return rc; }
            }

            remaining = bp_u64_sub(remaining, bp_u64_from_u32(orig_sz));

            if (progress_fn) {
                bp_progress_t p;
                bp_uint64 done;
                memset(&p, 0, sizeof(p));
                done = bp_u64_sub(entry->original_size, remaining);
                p.file_index        = file_index;
                p.file_count        = file_count;
                p.file_name         = entry->name;
                p.file_orig_size    = entry->original_size;
                p.file_bytes_done   = done;
                p.archive_orig_total = archive_orig_total;
                p.archive_orig_done  = bp_u64_add(archive_orig_done, done);
                if (progress_fn(&p, user_data) != 0) {
                    if (dfctx) { fluff_ctx_free(dfctx); dfctx = NULL; }
                    free(in_buf); free(dec_buf);
                    return BP_ERR_ABORTED;
                }
            }
        }

        if (dfctx) { fluff_ctx_free(dfctx); dfctx = NULL; }
        free(in_buf);
        free(dec_buf);
    }

    *out_checksum = adler;
    return BP_OK;
}

static int bp_archive_extract_impl(bp_io_t *io, bp_extract_fn open_fn,
                                   bp_extract_done_fn done_fn,
                                   bp_extract_done_status_fn status_fn,
                                   bp_progress_fn progress_fn, void *user_data)
{
    bp_uint32 file_count;
    bp_uint32 i;
    bp_uint16 version;
    int rc;
    bp_uint64 archive_orig_total;
    bp_uint64 archive_orig_done;
    int partial = 0;
    bp_uint8 subject_type = 0xFF;

    if (io == NULL || open_fn == NULL) return BP_ERR_INVALID_ARG;

    rc = read_header(io, &file_count, &version);
    if (rc != BP_OK) return rc;

    rc = verify_table_crc(io, version);
    if (rc != BP_OK) return rc;

    /* pre-scan file table for overall progress total */
    archive_orig_total = scan_archive_orig_total(io, version);
    archive_orig_done.lo = 0;
    archive_orig_done.hi = 0;

    /* position after header */
    rc = io_seek(io, BP_HEADER_SIZE, 0);
    if (rc != 0) return BP_ERR_IO;

    for (i = 0; i < file_count; i++) {
        bp_entry_t entry;
        bp_io_t *out;
        bp_uint32 checksum;
        int portable;
        bp_metadata_sink_t metadata;
        bp_portable_metadata_t parsed;

        rc = read_entry_header(io, &entry);
        if (rc != BP_OK) return rc;
        if (entry.entry_type == BP_TYPE_DATA || entry.entry_type == BP_TYPE_DIR ||
            entry.entry_type == BP_TYPE_SYMLINK) subject_type = entry.entry_type;

        portable = entry.entry_type == BP_TYPE_META && entry.platform == BP_META_PORTABLE;
        if (portable && (entry.original_size.hi != 0 ||
                         entry.original_size.lo > BP_META_MAX_SIZE)) {
            free(entry.name); return BP_ERR_CORRUPT;
        }
        if (portable) bp_metadata_sink_init(&metadata);
        out = portable ? NULL : open_fn(&entry, user_data);

        rc = extract_entry_data(io, &entry, portable ? &metadata.io : out, progress_fn, user_data,
                                i, file_count,
                                archive_orig_total, archive_orig_done,
                                &checksum, version);
        if (rc != BP_OK) {
            if (status_fn && out) (void)status_fn(&entry, out, rc, user_data);
            free(entry.name);
            return rc;
        }

        /* verify checksum */
        if (checksum != entry.checksum) {
            if (status_fn && out)
                (void)status_fn(&entry, out, BP_ERR_CHECKSUM, user_data);
            else if (done_fn && out) done_fn(&entry, out, user_data);
            free(entry.name);
            return BP_ERR_CHECKSUM;
        }

        if (portable) {
            rc = bp_metadata_parse(metadata.data, metadata.length, &parsed);
            if (rc == BP_ERR_CORRUPT) { free(entry.name); return rc; }
            if (parsed.supported_version && parsed.subject_type != subject_type) {
                free(entry.name); return BP_ERR_CORRUPT;
            }
            if (rc != BP_OK) partial = 1;
            if (parsed.supported_version) out = open_fn(&entry, user_data);
            if (out == NULL) partial = 1;
            else if (io_write(out, metadata.data, metadata.length) != BP_OK) {
                if (status_fn) (void)status_fn(&entry, out, BP_ERR_IO, user_data);
                else if (done_fn) done_fn(&entry, out, user_data);
                free(entry.name); return BP_ERR_IO;
            }
        }

        if (status_fn && out) {
            rc = status_fn(&entry, out, BP_OK, user_data);
            if (rc == BP_ERR_PARTIAL) partial = 1;
            else if (rc != BP_OK) { free(entry.name); return rc; }
        } else if (done_fn && out) {
            done_fn(&entry, out, user_data);
        }

        archive_orig_done = bp_u64_add(archive_orig_done, entry.original_size);
        free(entry.name);
    }

    return partial ? BP_ERR_PARTIAL : BP_OK;
}

int bp_archive_extract_all(bp_io_t *io, bp_extract_fn open_fn,
                           bp_extract_done_fn done_fn,
                           bp_progress_fn progress_fn, void *user_data)
{
    return bp_archive_extract_impl(io, open_fn, done_fn, NULL,
                                   progress_fn, user_data);
}

int bp_archive_extract_all_ex(bp_io_t *io, bp_extract_fn open_fn,
                              bp_extract_done_status_fn done_fn,
                              bp_progress_fn progress_fn, void *user_data)
{
    if (done_fn == NULL) return BP_ERR_INVALID_ARG;
    return bp_archive_extract_impl(io, open_fn, NULL, done_fn,
                                   progress_fn, user_data);
}

/* ---- test -------------------------------------------------------------- */

int bp_archive_test_progress(bp_io_t *io, bp_test_fn fn, void *user_data,
                             bp_progress_fn progress_fn,
                             void *progress_data)
{
    bp_uint32 file_count;
    bp_uint32 i;
    bp_uint16 version;
    int rc;
    bp_uint64 archive_orig_total;
    bp_uint64 archive_orig_done;
    bp_uint8 subject_type = 0xFF;

    if (io == NULL) return BP_ERR_INVALID_ARG;

    rc = read_header(io, &file_count, &version);
    if (rc != BP_OK) return rc;

    /* v3: verify the header+file-table CRC-32 before trusting the table */
    rc = verify_table_crc(io, version);
    if (rc != BP_OK) return rc;

    archive_orig_total = scan_archive_orig_total(io, version);
    archive_orig_done.lo = 0;
    archive_orig_done.hi = 0;

    rc = io_seek(io, BP_HEADER_SIZE, 0);
    if (rc != 0) return BP_ERR_IO;

    for (i = 0; i < file_count; i++) {
        bp_entry_t entry;
        bp_uint32 checksum;
        int entry_rc;
        int portable;
        bp_metadata_sink_t metadata;
        bp_portable_metadata_t parsed;

        rc = read_entry_header(io, &entry);
        if (rc != BP_OK) return rc;
        if (entry.entry_type == BP_TYPE_DATA || entry.entry_type == BP_TYPE_DIR ||
            entry.entry_type == BP_TYPE_SYMLINK) subject_type = entry.entry_type;

        if (progress_fn) {
            bp_progress_t p;
            memset(&p, 0, sizeof(p));
            p.file_index         = i;
            p.file_count         = file_count;
            p.file_name          = entry.name;
            p.file_orig_size     = entry.original_size;
            p.file_bytes_done.lo = 0;
            p.file_bytes_done.hi = 0;
            p.archive_orig_total = archive_orig_total;
            p.archive_orig_done  = archive_orig_done;
            if (progress_fn(&p, progress_data) != 0) {
                free(entry.name);
                return BP_ERR_ABORTED;
            }
        }

        portable = entry.entry_type == BP_TYPE_META && entry.platform == BP_META_PORTABLE;
        if (portable && (entry.original_size.hi != 0 ||
                         entry.original_size.lo > BP_META_MAX_SIZE)) {
            free(entry.name); return BP_ERR_CORRUPT;
        }
        if (portable) bp_metadata_sink_init(&metadata);
        {
            rc = extract_entry_data(io, &entry, portable ? &metadata.io : NULL,
                                    progress_fn, progress_data,
                                    i, file_count,
                                    archive_orig_total, archive_orig_done,
                                    &checksum, version);
        }
        if (rc != BP_OK) {
            entry_rc = rc;
        } else if (checksum != entry.checksum) {
            entry_rc = BP_ERR_CHECKSUM;
        } else {
            entry_rc = BP_OK;
        }
        if (entry_rc == BP_OK && portable) {
            rc = bp_metadata_parse(metadata.data, metadata.length, &parsed);
            if (rc == BP_ERR_CORRUPT) entry_rc = rc;
            else if (parsed.supported_version && parsed.subject_type != subject_type)
                entry_rc = BP_ERR_CORRUPT;
            else if (rc != BP_OK) entry_rc = BP_ERR_PARTIAL;
        }

        if (fn) {
            fn(&entry, entry_rc, user_data);
        }

        free(entry.name);

        if (entry_rc != BP_OK) {
            return entry_rc;
        }

        archive_orig_done = bp_u64_add(archive_orig_done, entry.original_size);
    }

    /* verify footer */
    {
        bp_uint64 table_offset, table_size;
        rc = read_footer(io, &table_offset, &table_size);
        if (rc != BP_OK) return rc;
    }

    return BP_OK;
}

int bp_archive_test(bp_io_t *io, bp_test_fn fn, void *user_data)
{
    return bp_archive_test_progress(io, fn, user_data, NULL, NULL);
}

/* ---- info -------------------------------------------------------------- */

int bp_archive_info(bp_io_t *io, bp_archive_info_t *info)
{
    bp_uint8 hdr[BP_HEADER_SIZE];
    bp_uint16 version;
    bp_uint32 file_count, table_count, i;
    int rc;

    if (io == NULL || info == NULL) return BP_ERR_INVALID_ARG;

    memset(info, 0, sizeof(*info));

    /* read archive header */
    rc = io_seek(io, 0, 0);
    if (rc != 0) return BP_ERR_IO;

    rc = io_read(io, hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) return rc;

    if (hdr[0] != BP_MAGIC_0 || hdr[1] != BP_MAGIC_1 ||
        hdr[2] != BP_MAGIC_2 || hdr[3] != BP_MAGIC_3) {
        return BP_ERR_MAGIC;
    }

    version = bp_read_le16(hdr + 4);
    if (version != BP_FORMAT_VERSION) {
        return BP_ERR_VERSION;
    }

    if (version == BP_FORMAT_VERSION) {
        bp_uint32 stored = bp_read_le32(hdr + BP_HEADER_CRC_OFF);
        bp_uint32 calc   = bp_crc32(0, hdr, BP_HEADER_CRC_OFF);
        if (stored != calc) return BP_ERR_CORRUPT;
    }

    info->version          = version;
    info->flags            = bp_read_le16(hdr + 6);
    info->file_count       = bp_read_le32(hdr + 8);
    info->creator_os       = hdr[12];
    info->creator_cpu      = hdr[13];
    info->creator_ver_major = hdr[14];
    info->creator_ver_minor = hdr[15];
    info->archive_size     = bp_read_le64(hdr + 16);

    file_count = info->file_count;

    rc = verify_table_crc(io, version);
    if (rc != BP_OK) return rc;

    /* read footer to find file table */
    rc = open_file_table(io, &table_count, NULL, version);
    if (rc != BP_OK) return rc;
    if (table_count != file_count) return BP_ERR_CORRUPT;

    /* scan file table to accumulate totals */
    {
        bp_uint64 tot_orig = {0, 0};
        bp_uint64 tot_comp = {0, 0};
        for (i = 0; i < table_count; i++) {
            bp_entry_t entry;
            rc = read_table_entry(io, &entry, version);
            if (rc != BP_OK) return rc;

            if (entry.entry_type == BP_TYPE_DIR) {
                info->dir_count++;
            } else if (version >= BP_FORMAT_VERSION) {
                /* v3: the method is in the table record — no seek needed. */
                if (entry.method < 8) {
                    info->method_counts[entry.method]++;
                }
            } else {
                /* v2: read each file's entry header for its method */
                bp_off_t save_pos = io_tell(io);
                int hdr_rc = io_seek(io, bp_u64_to_off(entry.offset), 0);
                if (hdr_rc == 0) {
                    bp_entry_t file_header;
                    hdr_rc = read_entry_header(io, &file_header);
                    if (hdr_rc == BP_OK && file_header.method < 8) {
                        info->method_counts[file_header.method]++;
                    }
                    if (file_header.name != NULL) free(file_header.name);
                }
                io_seek(io, save_pos, 0);
            }

            info->entry_count++;
            tot_orig = bp_u64_add(tot_orig, entry.original_size);
            tot_comp = bp_u64_add(tot_comp, entry.compressed_size);

            free(entry.name);
        }

        info->total_original   = tot_orig;
        info->total_compressed = tot_comp;
    }

    return BP_OK;
}

/* ---- deletion (raw copy) ----------------------------------------------- */

/* Return 1 if entry name matches any of the del_names.
 * A del_name ending with '/' also matches any entry whose name starts
 * with that prefix (i.e. directory deletion removes contents). */
static int name_in_delete_set(const char *name, bp_uint16 name_len,
                              const char **del_names, bp_uint32 del_count)
{
    bp_uint32 i;
    for (i = 0; i < del_count; i++) {
        size_t dlen = strlen(del_names[i]);
        if (dlen == 0) continue;
        /* exact match */
        if (dlen == (size_t)name_len &&
            memcmp(name, del_names[i], dlen) == 0) {
            return 1;
        }
        /* directory prefix match: del_name "foo/" deletes "foo/bar.txt" */
        if (del_names[i][dlen - 1] == '/' &&
            (size_t)name_len >= dlen &&
            memcmp(name, del_names[i], dlen) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Copy 'count' bytes from src_io to dst_io using a temporary buffer. */
static int raw_copy(bp_io_t *src_io, bp_io_t *dst_io, bp_uint64 count,
                    bp_uint8 *buf, bp_uint32 bufsz)
{
    while (!bp_u64_is_zero(count)) {
        bp_uint32 chunk = (count.hi != 0 || count.lo > bufsz) ? bufsz : count.lo;
        int rc = io_read(src_io, buf, chunk);
        if (rc != BP_OK) return rc;
        rc = io_write(dst_io, buf, chunk);
        if (rc != BP_OK) return rc;
        count = bp_u64_sub(count, bp_u64_from_u32(chunk));
    }
    return BP_OK;
}

int bp_archive_commit_packed_entry(bp_archive_t *ar, bp_io_t *src_io,
                                   const bp_packed_entry_t *packed)
{
    bp_uint8 *copybuf;
    bp_uint32 copybuf_sz;
    bp_off_t entry_offset;
    int rc;

    if (ar == NULL || src_io == NULL || packed == NULL || packed->name == NULL) {
        return BP_ERR_INVALID_ARG;
    }

    rc = entry_list_grow(ar);
    if (rc != BP_OK) return rc;

    entry_offset = io_tell(ar->io);
    if (entry_offset < 0) return BP_ERR_IO;

    rc = io_seek(src_io, 0, 0);
    if (rc != 0) return BP_ERR_IO;

    copybuf_sz = ar->block_size ? ar->block_size : BP_BLOCK_DEFAULT;
    if (copybuf_sz < 4096U) {
        copybuf_sz = 4096U;
    }
    copybuf = (bp_uint8 *)malloc((size_t)copybuf_sz);
    if (copybuf == NULL) return BP_ERR_NOMEM;

    rc = raw_copy(src_io, ar->io, packed->serialized_size, copybuf, copybuf_sz);
    free(copybuf);
    if (rc != BP_OK) return rc;

    return record_packed_entry(ar, entry_offset, packed);
}

/* ---- append (copy existing entries into new archive) ------------------- */

int bp_archive_copy_from(bp_archive_t *ar, bp_io_t *src_io)
{
    bp_uint32 src_count;
    bp_uint32 i;
    bp_uint16 src_version = BP_FORMAT_VERSION;
    int rc;
    bp_uint8 *copybuf = NULL;
    bp_uint32 copybuf_sz = 32768;
    bp_entry_t *table = NULL;

    if (ar == NULL || src_io == NULL) return BP_ERR_INVALID_ARG;

    /* --- read source header --- */
    {
        bp_uint8 hdr[BP_HEADER_SIZE];

        rc = io_seek(src_io, 0, 0);
        if (rc != 0) return BP_ERR_IO;
        rc = io_read(src_io, hdr, BP_HEADER_SIZE);
        if (rc != BP_OK) return rc;

        if (hdr[0] != BP_MAGIC_0 || hdr[1] != BP_MAGIC_1 ||
            hdr[2] != BP_MAGIC_2 || hdr[3] != BP_MAGIC_3)
            return BP_ERR_MAGIC;
        src_version = bp_read_le16(hdr + 4);
        if (src_version != BP_FORMAT_VERSION) return BP_ERR_VERSION;
        if (src_version == BP_FORMAT_VERSION) {
            bp_uint32 stored = bp_read_le32(hdr + BP_HEADER_CRC_OFF);
            bp_uint32 calc   = bp_crc32(0, hdr, BP_HEADER_CRC_OFF);
            if (stored != calc) return BP_ERR_CORRUPT;
        }
        src_count = bp_read_le32(hdr + 8);
    }

    if (src_count == 0) return BP_OK; /* empty archive, nothing to copy */

    rc = verify_table_crc(src_io, src_version);
    if (rc != BP_OK) return rc;

    /* --- read source file table --- */
    {
        bp_uint32 tbl_count;

        rc = open_file_table(src_io, &tbl_count, NULL, src_version);
        if (rc != BP_OK) return rc;
        if (tbl_count != src_count) return BP_ERR_CORRUPT;

        table = (bp_entry_t *)calloc((size_t)src_count, sizeof(bp_entry_t));
        if (table == NULL) return BP_ERR_NOMEM;

        for (i = 0; i < src_count; i++) {
            rc = read_table_entry(src_io, &table[i], src_version);
            if (rc != BP_OK) goto fail;
            /* v2 tables lack method/header_size/checksum; the destination
             * finalizes as v3, so backfill them from the entry header. */
            if (src_version < BP_FORMAT_VERSION) {
                rc = backfill_entry_from_header(src_io, &table[i]);
                if (rc != BP_OK) goto fail;
            }
        }
    }

    /* --- allocate copy buffer --- */
    copybuf = (bp_uint8 *)malloc((size_t)copybuf_sz);
    if (copybuf == NULL) { rc = BP_ERR_NOMEM; goto fail; }

    /* --- raw-copy each entry --- */
    for (i = 0; i < src_count; i++) {
        bp_entry_t *e = &table[i];
        bp_uint64 entry_total;
        bp_off_t dst_offset;
        bp_entry_t *ent;

        /* header_size is already known (v3 table field or v2 backfill),
         * so the entry span needs no seek-and-read dance. */
        entry_total = bp_u64_add(bp_u64_from_u32(e->header_size),
                                 e->compressed_size);

        rc = io_seek(src_io, bp_u64_to_off(e->offset), 0);
        if (rc != 0) { rc = BP_ERR_IO; goto fail; }

        /* grow entry list in the destination archive context */
        rc = entry_list_grow(ar);
        if (rc != BP_OK) goto fail;

        dst_offset = io_tell(ar->io);
        if (dst_offset < 0) { rc = BP_ERR_IO; goto fail; }

        /* raw-copy entry */
        rc = raw_copy(src_io, ar->io, entry_total, copybuf, copybuf_sz);
        if (rc != BP_OK) goto fail;

        /* record in ar->entry_list (carry v3 table fields for finalize) */
        ent = &ar->entry_list[ar->entries_written];
        memset(ent, 0, sizeof(*ent));
        ent->name = (char *)malloc((size_t)(e->name_len + 1));
        if (ent->name == NULL) { rc = BP_ERR_NOMEM; goto fail; }
        memcpy(ent->name, e->name, e->name_len);
        ent->name[e->name_len] = '\0';
        ent->name_len        = e->name_len;
        ent->original_size   = e->original_size;
        ent->compressed_size = e->compressed_size;
        ent->entry_type      = e->entry_type;
        ent->platform        = e->platform;
        ent->mtime           = e->mtime;
        ent->flags           = e->flags;
        ent->offset          = bp_off_to_u64(dst_offset);
        ent->method          = e->method;
        ent->header_size     = e->header_size;
        ent->checksum        = e->checksum;
        ent->checksum_type   = e->checksum_type;
        ar->entries_written++;

        ar->archive_orig_total = bp_u64_add(ar->archive_orig_total,
                                            e->original_size);
        ar->archive_comp_total = bp_u64_add(ar->archive_comp_total,
                                            e->compressed_size);
        ar->archive_orig_done  = bp_u64_add(ar->archive_orig_done,
                                            e->original_size);
    }

    rc = BP_OK;

fail:
    if (table) {
        for (i = 0; i < src_count; i++) free(table[i].name);
        free(table);
    }
    free(copybuf);
    return rc;
}

/* ---- deletion (raw copy) ----------------------------------------------- */

int bp_archive_delete(bp_io_t *src_io, bp_io_t *dst_io,
                      const char **del_names, bp_uint32 del_count,
                      bp_uint32 *out_deleted)
{
    bp_uint32 src_count;
    bp_uint32 i, kept;
    bp_uint32 deleted = 0;
    bp_uint16 src_version = BP_FORMAT_VERSION;
    int rc;
    bp_uint8 src_hdr[BP_HEADER_SIZE];
    bp_entry_t *table = NULL;      /* file table from source */
    bp_entry_t *kept_entries = NULL;
    bp_uint8 *copybuf = NULL;
    bp_uint32 copybuf_sz = 32768;

    if (src_io == NULL || dst_io == NULL || del_names == NULL)
        return BP_ERR_INVALID_ARG;

    /* --- read source header --- */
    rc = io_seek(src_io, 0, 0);
    if (rc != 0) return BP_ERR_IO;
    rc = io_read(src_io, src_hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) return rc;

    if (src_hdr[0] != BP_MAGIC_0 || src_hdr[1] != BP_MAGIC_1 ||
        src_hdr[2] != BP_MAGIC_2 || src_hdr[3] != BP_MAGIC_3)
        return BP_ERR_MAGIC;
    src_version = bp_read_le16(src_hdr + 4);
    if (src_version != BP_FORMAT_VERSION) return BP_ERR_VERSION;
    if (src_version == BP_FORMAT_VERSION) {
        bp_uint32 stored = bp_read_le32(src_hdr + BP_HEADER_CRC_OFF);
        bp_uint32 calc   = bp_crc32(0, src_hdr, BP_HEADER_CRC_OFF);
        if (stored != calc) return BP_ERR_CORRUPT;
    }
    src_count = bp_read_le32(src_hdr + 8);

    rc = verify_table_crc(src_io, src_version);
    if (rc != BP_OK) return rc;

    /* --- read source file table --- */
    {
        bp_uint32 tbl_count;

        rc = open_file_table(src_io, &tbl_count, NULL, src_version);
        if (rc != BP_OK) return rc;
        if (tbl_count != src_count) return BP_ERR_CORRUPT;

        table = (bp_entry_t *)calloc((size_t)src_count, sizeof(bp_entry_t));
        if (table == NULL) return BP_ERR_NOMEM;

        for (i = 0; i < src_count; i++) {
            rc = read_table_entry(src_io, &table[i], src_version);
            if (rc != BP_OK) goto fail;
            /* output is written as v3; backfill v2 records so the new table
             * carries method/header_size/checksum. */
            if (src_version < BP_FORMAT_VERSION) {
                rc = backfill_entry_from_header(src_io, &table[i]);
                if (rc != BP_OK) goto fail;
            }
        }
    }

    /* --- write destination header (placeholder, patched later) --- */
    rc = write_buf(dst_io, src_hdr, BP_HEADER_SIZE);
    if (rc != BP_OK) goto fail;

    /* --- allocate copy buffer --- */
    copybuf = (bp_uint8 *)malloc((size_t)copybuf_sz);
    if (copybuf == NULL) { rc = BP_ERR_NOMEM; goto fail; }

    /* --- allocate kept_entries array (max = src_count) --- */
    kept_entries = (bp_entry_t *)calloc((size_t)src_count, sizeof(bp_entry_t));
    if (kept_entries == NULL) { rc = BP_ERR_NOMEM; goto fail; }
    kept = 0;

    /* --- iterate entries: raw-copy kept ones --- */
    for (i = 0; i < src_count; i++) {
        bp_entry_t *e = &table[i];
        bp_uint64 entry_total;  /* header_size + compressed_size */
        bp_off_t dst_offset;

        if (name_in_delete_set(e->name, e->name_len, del_names, del_count)) {
            deleted++;
            continue; /* skip this entry */
        }

        /* header_size is already known (v3 table field or v2 backfill). */
        entry_total = bp_u64_add(bp_u64_from_u32(e->header_size),
                                 e->compressed_size);

        /* seek source to entry offset for the raw copy */
        rc = io_seek(src_io, bp_u64_to_off(e->offset), 0);
        if (rc != 0) { rc = BP_ERR_IO; goto fail; }

        dst_offset = io_tell(dst_io);
        if (dst_offset < 0) { rc = BP_ERR_IO; goto fail; }

        /* raw-copy entry header + compressed data */
        rc = raw_copy(src_io, dst_io, entry_total, copybuf, copybuf_sz);
        if (rc != BP_OK) goto fail;

        /* record kept entry with updated offset */
        kept_entries[kept] = *e;
        kept_entries[kept].name = (char *)malloc((size_t)(e->name_len + 1));
        if (kept_entries[kept].name == NULL) { rc = BP_ERR_NOMEM; goto fail; }
        memcpy(kept_entries[kept].name, e->name, e->name_len);
        kept_entries[kept].name[e->name_len] = '\0';
        kept_entries[kept].offset = bp_off_to_u64(dst_offset);
        kept++;
    }

    /* --- write file table --- */
    {
        bp_off_t table_start, table_end, archive_end;
        bp_uint8 buf[BP_TABLE_HDR_V3];
        bp_uint32 table_crc = 0;
        bp_uint32 ki;
        bp_uint64 total_orig;

        total_orig.lo = 0; total_orig.hi = 0;
        for (ki = 0; ki < kept; ki++) {
            total_orig = bp_u64_add(total_orig, kept_entries[ki].original_size);
        }

        table_start = io_tell(dst_io);
        if (table_start < 0) { rc = BP_ERR_IO; goto fail; }

        buf[0] = BP_TABLE_MAGIC_0;
        buf[1] = BP_TABLE_MAGIC_1;
        buf[2] = BP_TABLE_MAGIC_2;
        buf[3] = BP_TABLE_MAGIC_3;
        bp_write_le32(buf + 4, kept);
        bp_write_le64(buf + 8, total_orig);
        rc = write_crc(dst_io, &table_crc, buf, BP_TABLE_HDR_V3);
        if (rc != BP_OK) goto fail;

        for (i = 0; i < kept; i++) {
            rc = write_table_record(dst_io, &table_crc, &kept_entries[i]);
            if (rc != BP_OK) goto fail;
        }

        table_end = io_tell(dst_io);
        if (table_end < 0) { rc = BP_ERR_IO; goto fail; }

        /* --- write footer --- */
        {
            bp_uint8 footer[BP_FOOTER_SIZE];
            bp_write_le64(footer + 0, bp_off_to_u64(table_start));
            bp_write_le64(footer + 8, bp_off_to_u64(table_end - table_start));
            footer[16] = BP_FOOTER_MAGIC_0;
            footer[17] = BP_FOOTER_MAGIC_1;
            footer[18] = BP_FOOTER_MAGIC_2;
            footer[19] = BP_FOOTER_MAGIC_3;
            rc = write_buf(dst_io, footer, BP_FOOTER_SIZE);
            if (rc != BP_OK) goto fail;
        }

        archive_end = io_tell(dst_io);
        if (archive_end < 0) { rc = BP_ERR_IO; goto fail; }

        /* --- patch header: file_count + archive_size --- */
        {
            bp_uint8 hdr[BP_HEADER_SIZE];
            rc = io_seek(dst_io, 0, 0);
            if (rc != 0) { rc = BP_ERR_IO; goto fail; }
            rc = io_read(dst_io, hdr, BP_HEADER_SIZE);
            if (rc != BP_OK) goto fail;
            bp_write_le32(hdr + 8, kept);
            bp_write_le64(hdr + 16, bp_off_to_u64(archive_end));
            rc = io_seek(dst_io, 0, 0);
            if (rc != 0) { rc = BP_ERR_IO; goto fail; }
            rc = write_buf(dst_io, hdr, BP_HEADER_SIZE);
            if (rc != BP_OK) goto fail;
        }

        /* --- patch v3 header + table CRC-32 (upgrades v2 source to v3) --- */
        rc = patch_header_crc(dst_io, table_crc);
        if (rc != BP_OK) goto fail;
    }

    if (out_deleted) *out_deleted = deleted;
    rc = BP_OK;

fail:
    if (table) {
        for (i = 0; i < src_count; i++) free(table[i].name);
        free(table);
    }
    if (kept_entries) {
        for (i = 0; i < kept; i++) free(kept_entries[i].name);
        free(kept_entries);
    }
    free(copybuf);
    return rc;
}
