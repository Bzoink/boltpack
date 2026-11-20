/*
 * archiver.c -- archive format 7 (volume sets) read/write for BoltPack
 *
 * Implements docs/specification-2.0.md.  Every archive is a set of one or
 * more volume files; a single-file archive is a set of one volume.  The
 * writer never revisits bytes it has written, so a set can be written
 * directly onto removable media one volume after another.
 *
 * This file provides the archiver.h API used by every full CLI and GUI.
 * (Format 6, the layout used before volume sets, is no longer written or
 * read; the 16-bit and small targets use mini/archiver.c.)
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
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef BOLTPACK_FAST_FLUFF
#include <omp.h>
#include "fluff_fast.h"
#define FAST_FLUFF_MAX_BATCH 64
#define FAST_BOLTLZ_MAX_BATCH 64
#endif

/* ---- format 7 layout constants ----------------------------------------- */

#define V7_VOL_HDR        32U   /* volume header */
#define V7_VOL_TRL        32U   /* volume trailer */
#define V7_END_REC        48U   /* end record */
#define V7_ENTRY_FIXED    34U   /* entry header without the name */
#define V7_ENTRY_TRL      16U   /* entry trailer */
#define V7_BLOCK_HDR      12U   /* block header */
#define V7_TABLE_HDR      16U   /* file table header */
#define V7_REC_TAIL       45U   /* table record after name_len + name */
#define V7_MAX_NAME       65501U
#define V7_MAX_VOLNUM     999U
#define V7_FLAG_LAST      0x0001U
#define V7_MIN_EOV_SPACE  512U  /* body space worth filling with an end-of-volume block (spec 2.0 §2.3.2) */
#define V7_MIN_VOLUME     4096UL
#define V7_BLOCK_CAP      (16UL * 1024UL * 1024UL)

/* "BVOL" trailer magic, "TLOB" end-record magic */
static const bp_uint8 v7_trl_magic[4] = { 0x42, 0x56, 0x4F, 0x4C };
static const bp_uint8 v7_end_magic[4] = { 0x54, 0x4C, 0x4F, 0x42 };

/* ---- small helpers ----------------------------------------------------- */

static int io_write(bp_io_t *io, const void *buf, bp_uint32 count)
{
    long n;
    if (count == 0) return BP_OK;
    n = io->write(io->handle, buf, count);
    return (n == (long)count) ? BP_OK : BP_ERR_IO;
}

static int io_read(bp_io_t *io, void *buf, bp_uint32 count)
{
    long n;
    if (count == 0) return BP_OK;
    n = io->read(io->handle, buf, count);
    return (n == (long)count) ? BP_OK : BP_ERR_TRUNCATED;
}

static int io_seek_u64(bp_io_t *io, bp_uint64 offset)
{
    bp_off_t off = bp_u64_to_off(offset);
    if (off < 0) return BP_ERR_OVERFLOW;
    return io->seek(io->handle, off, 0) == 0 ? BP_OK : BP_ERR_IO;
}

static long discard_write(void *handle, const void *buf, bp_uint32 count)
{
    (void)handle;
    (void)buf;
    return (long)count;
}

bp_io_t *bp_archive_dry_run_stream(void)
{
    static bp_io_t discard_io = { NULL, discard_write, NULL, NULL, NULL };
    return &discard_io;
}

static void bp_core_log(const bp_archive_t *ar, const char *msg)
{
    if (ar != NULL && ar->log_fn != NULL)
        ar->log_fn(msg, ar->log_data);
}

static bp_uint64 u64_add32(bp_uint64 a, bp_uint32 b)
{
    return bp_u64_add(a, bp_u64_from_u32(b));
}

static int u64_eq(bp_uint64 a, bp_uint64 b)
{
    return a.lo == b.lo && a.hi == b.hi;
}

/* min(v, cap) as a 32-bit value */
static bp_uint32 u64_cap32(bp_uint64 v, bp_uint32 cap)
{
    if (v.hi != 0 || v.lo > cap) return cap;
    return v.lo;
}

static int bp_method_is_supported(bp_uint8 method)
{
    return method == BP_METHOD_STORED || method == BP_METHOD_BOLTLITE ||
           method == BP_METHOD_BOLTLZ || method == BP_METHOD_FLUFF;
}

static bp_uint32 bp_method_compress_bound(bp_uint8 method, bp_uint32 src_len)
{
    if (method == BP_METHOD_BOLTLITE) return src_len + src_len / 8 + 16;
    if (method == BP_METHOD_BOLTLZ) return boltlz_compress_bound(src_len);
    if (method == BP_METHOD_FLUFF) return fluff_compress_bound(src_len);
    return src_len;
}

void bp_packed_entry_dispose(bp_packed_entry_t *packed)
{
    if (packed == NULL) return;
    free(packed->name);
    memset(packed, 0, sizeof(*packed));
}

void bp_archive_set_log(bp_archive_t *ar, bp_log_fn fn, void *user_data)
{
    if (ar == NULL) return;
    ar->log_fn = fn;
    ar->log_data = user_data;
}

/* ---- entry name safety ------------------------------------------------- */

int bp_name_is_safe(const char *name)
{
    size_t length;
    if (name == NULL) return 0;
    length = strlen(name);
    if (length == 0 || length > (size_t)BP_MAX_NAME_LEN) return 0;
    return bp_archive_name_valid(name, (bp_uint32)length);
}

int bp_symlink_target_is_safe(const char *entry_name, const char *target)
{
    int depth;
    const char *p;

    if (entry_name == NULL || target == NULL) return 0;
    if (target[0] == '\0') return 0;
    if (target[0] == '/') return 0;
    if (target[1] == ':') return 0;
    if (strchr(target, '\\') != NULL) return 0;

    depth = 0;
    for (p = entry_name; *p; p++) {
        if (*p == '/' && p[1] != '\0') depth++;
    }
    p = target;
    while (*p) {
        const char *comp = p;
        unsigned int comp_len = 0;
        while (p[comp_len] != '/' && p[comp_len] != '\0') comp_len++;
        if (comp_len == 2 && comp[0] == '.' && comp[1] == '.') {
            depth--;
            if (depth < 0) return 0;
        } else if (comp_len == 1 && comp[0] == '.') {
            /* no change */
        } else if (comp_len > 0) {
            depth++;
        }
        p += comp_len;
        if (*p == '/') p++;
    }
    return 1;
}

/* ---- parallel compression helpers (modern OpenMP builds) --------------- */

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

static int bp_fast_fluff_ensure(bp_archive_t *ar, bp_uint32 block_size,
                                bp_uint32 out_size)
{
    int nthreads_ff;
    int batch_blocks;

    nthreads_ff = ar->fast_fluff_threads;
    if (nthreads_ff > FAST_FLUFF_MAX_BATCH) nthreads_ff = FAST_FLUFF_MAX_BATCH;
    if (nthreads_ff < 2) {
        bp_fast_fluff_release(ar);
        return BP_OK;
    }
    batch_blocks = FAST_FLUFF_MAX_BATCH;

    if (ar->fast_fluff_ctx != NULL &&
        ar->fast_fluff_ctx_threads == (bp_uint32)nthreads_ff &&
        ar->fast_fluff_batch_blocks == (bp_uint32)batch_blocks &&
        ar->fast_fluff_block_size == block_size &&
        ar->fast_fluff_out_size == out_size)
        return BP_OK;

    bp_fast_fluff_release(ar);
    ar->fast_fluff_ctx = (void *)fluff_fast_ctx_alloc(nthreads_ff);
    if (ar->fast_fluff_ctx == NULL) return BP_ERR_NOMEM;
    ar->fast_fluff_in_base = (bp_uint8 *)malloc(
        (size_t)batch_blocks * (size_t)block_size);
    ar->fast_fluff_out_base = (bp_uint8 *)malloc(
        (size_t)batch_blocks * (size_t)out_size);
    ar->fast_fluff_hist_size = fluff_window_size();
    ar->fast_fluff_hist_base = (bp_uint8 *)malloc(
        (size_t)ar->fast_fluff_hist_size);
    if (ar->fast_fluff_in_base == NULL || ar->fast_fluff_out_base == NULL ||
        ar->fast_fluff_hist_base == NULL) {
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
    bp_uint32 work_size;

    if (nthreads > FAST_BOLTLZ_MAX_BATCH) nthreads = FAST_BOLTLZ_MAX_BATCH;
    if (nthreads < 2) {
        bp_fast_boltlz_release(ar);
        return BP_OK;
    }
    work_size = bp_compress_work_size();
    if (work_size == 0) return BP_ERR_INVALID_ARG;
    if (ar->fast_boltlz_in_base != NULL &&
        ar->fast_boltlz_ctx_threads == (bp_uint32)nthreads &&
        ar->fast_boltlz_block_size == block_size &&
        ar->fast_boltlz_out_size == out_size &&
        ar->fast_boltlz_work_size == work_size)
        return BP_OK;

    bp_fast_boltlz_release(ar);
    ar->fast_boltlz_in_base = (bp_uint8 *)malloc(
        (size_t)nthreads * (size_t)block_size);
    ar->fast_boltlz_out_base = (bp_uint8 *)malloc(
        (size_t)nthreads * (size_t)out_size);
    ar->fast_boltlz_work_base = malloc((size_t)nthreads * (size_t)work_size);
    if (ar->fast_boltlz_in_base == NULL || ar->fast_boltlz_out_base == NULL ||
        ar->fast_boltlz_work_base == NULL) {
        bp_fast_boltlz_release(ar);
        return BP_ERR_NOMEM;
    }
    ar->fast_boltlz_ctx_threads = (bp_uint32)nthreads;
    ar->fast_boltlz_batch_blocks = (bp_uint32)nthreads;
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
    if (nthreads > FAST_BOLTLZ_MAX_BATCH) nthreads = FAST_BOLTLZ_MAX_BATCH;
    while (nthreads > 1) {
        min_bytes = block_size * (bp_uint32)(2 * nthreads);
        if (bp_u64_gte(file_size, bp_u64_from_u32(min_bytes)))
            return nthreads;
        nthreads--;
    }
    return 0;
}
#endif /* BOLTPACK_FAST_FLUFF */

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
    bp_set_fast_fluff(ar, nthreads);
}

/* ---- set IDs ----------------------------------------------------------- */

static bp_uint32 g_v7_entropy;
static bp_uint32 g_v7_counter;

void bp_archive_add_entropy(bp_uint32 value)
{
    bp_uint8 b[8];
    bp_write_le32(b, g_v7_entropy);
    bp_write_le32(b + 4, value);
    g_v7_entropy = bp_crc32(g_v7_entropy ^ 0x5A17C0DEUL, b, 8);
}

/* libnix's time() links its time-zone and locale startup code, which opens
 * locale.library and stops every program on AmigaOS 2.04; the Amiga frontend
 * feeds bp_archive_add_entropy() the DateStamp instead. */
#if defined(__AMIGA__) && defined(__libnix__)
#define V7_TIME() 0UL
#else
#define V7_TIME() ((unsigned long)time(NULL))
#endif

/* A new set ID in *id.  The stack address goes through a volatile: the
 * RISC OS GCC 4.7 otherwise turns its low byte into a move from sp and
 * stops with an internal compiler error. */
static void v7_new_set_id(const void *salt, bp_uint64 *id)
{
    bp_uint8 b[32];
    volatile unsigned long stack_addr = (unsigned long)(size_t)&b;
    unsigned long t = V7_TIME();
    unsigned long c = (unsigned long)clock();
    unsigned long a = (unsigned long)(size_t)salt;
    unsigned long s = stack_addr;

    g_v7_counter++;
    memset(b, 0, sizeof(b));
    bp_write_le32(b + 0, (bp_uint32)(t & 0xFFFFFFFFUL));
    bp_write_le32(b + 4, (bp_uint32)(c & 0xFFFFFFFFUL));
    bp_write_le32(b + 8, (bp_uint32)(a & 0xFFFFFFFFUL));
    bp_write_le32(b + 12, (bp_uint32)(s & 0xFFFFFFFFUL));
    bp_write_le32(b + 16, g_v7_counter);
    bp_write_le32(b + 20, g_v7_entropy);
#if (ULONG_MAX > 0xFFFFFFFFUL)
    bp_write_le32(b + 24, (bp_uint32)((t >> 32) ^ (a >> 32)));
    bp_write_le32(b + 28, (bp_uint32)((s >> 32) ^ (c >> 32)));
#endif
    id->lo = bp_crc32(0, b, sizeof(b));
    id->hi = bp_crc32(id->lo ^ 0x9E3779B9UL, b, sizeof(b));
    if (id->lo == 0 && id->hi == 0) id->lo = 1;
}

/* ---- structure encoders ------------------------------------------------ */

static void v7_build_vol_header(bp_uint8 *h, bp_uint64 set_id,
                                bp_uint16 number, bp_uint8 os, bp_uint8 cpu)
{
    memset(h, 0, V7_VOL_HDR);
    h[0] = BP_MAGIC_0; h[1] = BP_MAGIC_1; h[2] = BP_MAGIC_2; h[3] = BP_MAGIC_3;
    bp_write_le16(h + 4, BP_FORMAT7_VERSION);
    bp_write_le64(h + 8, set_id);
    bp_write_le16(h + 16, number);
    h[18] = os;
    h[19] = cpu;
    h[20] = BP_VERSION_MAJOR;
    h[21] = BP_VERSION_MINOR;
    bp_write_le32(h + 28, bp_crc32(0, h, 28));
}

static void v7_build_vol_trailer(bp_uint8 *t, bp_uint64 set_id,
                                 bp_uint16 number, int last, bp_uint64 size)
{
    memset(t, 0, V7_VOL_TRL);
    bp_write_le64(t, set_id);
    bp_write_le16(t + 8, number);
    bp_write_le16(t + 10, (bp_uint16)(last ? V7_FLAG_LAST : 0));
    bp_write_le64(t + 16, size);
    bp_write_le32(t + 24, bp_crc32(0, t, 24));
    memcpy(t + 28, v7_trl_magic, 4);
}

/* ---- writer state ------------------------------------------------------ */

typedef struct {
    int              multi;      /* volumes through a sink */
    bp_volume_sink_t sink;
    bp_uint64        limit;      /* max bytes per volume, 0 = no limit */
    bp_io_t         *io;         /* current volume stream */
    int              vol_open;
    bp_uint16        number;     /* current volume number */
    bp_uint64        pos;        /* bytes written to the current volume */
    bp_uint64        set_id;
    int              finalized;
    bp_uint64        final_size;
    /* end-of-volume block splitting */
    fluff_ctx_t     *split_fctx;
    void            *split_lz_work;
    bp_uint8        *split_bl_work;
    bp_uint8        *split_out;
    bp_uint32        split_out_size;
} v7w_t;

static v7w_t *W(bp_archive_t *ar) { return (v7w_t *)ar->v7; }

static void v7w_free(bp_archive_t *ar)
{
    v7w_t *w = W(ar);
    if (w == NULL) return;
    if (w->split_fctx != NULL) fluff_ctx_free(w->split_fctx);
    free(w->split_lz_work);
    free(w->split_bl_work);
    free(w->split_out);
    free(w);
    ar->v7 = NULL;
}

static int vw_write(v7w_t *w, const void *buf, bp_uint32 len)
{
    int rc = io_write(w->io, buf, len);
    if (rc == BP_OK) w->pos = u64_add32(w->pos, len);
    return rc;
}

static bp_uint64 vw_space(const v7w_t *w)
{
    bp_uint64 used, big;
    if (w->limit.lo == 0 && w->limit.hi == 0) {
        big.lo = 0xFFFFFFFFUL;
        big.hi = 0x3FFFFFFFUL;
        return big;
    }
    used = u64_add32(w->pos, V7_VOL_TRL);
    if (bp_u64_cmp(used, w->limit) >= 0) return bp_u64_from_u32(0);
    return bp_u64_sub(w->limit, used);
}

static int vw_fits(const v7w_t *w, bp_uint32 len)
{
    return bp_u64_cmp(vw_space(w), bp_u64_from_u32(len)) >= 0;
}

static int vw_limited(const v7w_t *w)
{
    return w->limit.lo != 0 || w->limit.hi != 0;
}

static int vw_body_empty(const v7w_t *w)
{
    return w->pos.hi == 0 && w->pos.lo == V7_VOL_HDR;
}

static int vw_open_volume(bp_archive_t *ar)
{
    v7w_t *w = W(ar);
    bp_uint8 h[V7_VOL_HDR];
    bp_io_t *io;

    io = w->sink.create(w->sink.user, w->number);
    if (io == NULL) return BP_ERR_IO;
    w->io = io;
    ar->io = io;
    w->vol_open = 1;
    w->pos = bp_u64_from_u32(0);
    v7_build_vol_header(h, w->set_id, w->number, ar->creator_os,
                        ar->creator_cpu);
    return vw_write(w, h, V7_VOL_HDR);
}

/* Write the trailer of the current volume and hand it back to the sink. */
static int vw_close_volume(bp_archive_t *ar, int last)
{
    v7w_t *w = W(ar);
    bp_uint8 t[V7_VOL_TRL];
    int rc, frc;

    if (!w->vol_open) return BP_OK;
    v7_build_vol_trailer(t, w->set_id, w->number, last,
                         u64_add32(w->pos, V7_VOL_TRL));
    rc = vw_write(w, t, V7_VOL_TRL);
    if (!w->multi) {
        w->vol_open = 0;
        return rc;
    }
    frc = w->sink.finish(w->sink.user, w->io, w->number, last, rc);
    w->vol_open = 0;
    w->io = NULL;
    ar->io = NULL;
    if (rc != BP_OK) return rc;
    if (frc != 0) return frc < 0 ? frc : BP_ERR_IO;
    return BP_OK;
}

static int vw_next_volume(bp_archive_t *ar)
{
    v7w_t *w = W(ar);
    int rc;

    if (!w->multi || vw_body_empty(w)) return BP_ERR_OVERFLOW;
    if (w->number >= V7_MAX_VOLNUM) return BP_ERR_OVERFLOW;
    rc = vw_close_volume(ar, 0);
    if (rc != BP_OK) return rc;
    w->number++;
    return vw_open_volume(ar);
}

/* Make room for a whole structure of `len` bytes. */
static int vw_reserve(bp_archive_t *ar, bp_uint32 len)
{
    v7w_t *w = W(ar);
    int rc;
    if (vw_fits(w, len)) return BP_OK;
    rc = vw_next_volume(ar);
    if (rc != BP_OK) return rc;
    return vw_fits(w, len) ? BP_OK : BP_ERR_OVERFLOW;
}

static int vw_put_struct(bp_archive_t *ar, const bp_uint8 *buf, bp_uint32 len,
                         bp_uint16 *vol, bp_uint64 *off)
{
    v7w_t *w = W(ar);
    int rc = vw_reserve(ar, len);
    if (rc != BP_OK) return rc;
    if (vol != NULL) *vol = w->number;
    if (off != NULL) *off = w->pos;
    return vw_write(w, buf, len);
}

/* ---- profile checks ---------------------------------------------------- */

static int profile_check_entry_values(bp_uint8 profile, bp_uint8 method,
                                      bp_uint32 block_size,
                                      bp_uint8 entry_type,
                                      bp_uint64 original_size)
{
    if (profile == BP_PROFILE_NONE) return BP_OK;
    if (profile != BP_PROFILE_INTERCHANGE_1) return BP_ERR_INVALID_ARG;
    if (method != BP_METHOD_STORED && method != BP_METHOD_BOLTLITE)
        return BP_ERR_UNSUPPORTED_METHOD;
    if (original_size.hi != 0) return BP_ERR_OVERFLOW;
    if (entry_type == BP_TYPE_DIR)
        return original_size.lo == 0 ? BP_OK : BP_ERR_CORRUPT;
    if (original_size.lo == 0) return BP_OK;
    if (block_size > BP_PROFILE1_MAX_BLOCK) return BP_ERR_OVERFLOW;
    return BP_OK;
}

/* Interchange Profile 1 caps every volume at 2^31-1 bytes.  With a volume
 * limit at or below that, every volume complies; a single unlimited volume
 * is checked against the projected final size before each structure. */
static int profile_check_extra(bp_archive_t *ar, bp_uint32 extra,
                               bp_uint16 pending_name_len)
{
    v7w_t *w = W(ar);
    bp_uint64 total;
    bp_uint32 count;

    if (ar->profile == BP_PROFILE_NONE || w == NULL) return BP_OK;
    count = ar->entries_written + (pending_name_len != 0 ? 1U : 0U);
    if (count > BP_PROFILE1_MAX_ENTRIES) return BP_ERR_OVERFLOW;
    if (vw_limited(w)) return BP_OK;
    total = u64_add32(w->pos, extra);
    total = u64_add32(total, V7_TABLE_HDR + V7_END_REC + V7_VOL_TRL);
    total = bp_u64_add(total, ar->profile_table_bytes);
    if (pending_name_len != 0)
        total = u64_add32(total, 2U + pending_name_len + V7_REC_TAIL);
    if (total.hi != 0 || total.lo > BP_PROFILE1_MAX_ARCHIVE)
        return BP_ERR_OVERFLOW;
    return BP_OK;
}

/* ---- archive creation -------------------------------------------------- */

static void v7_detect_creator(bp_archive_t *ar)
{
    ar->creator_os = BP_OS_UNKNOWN;
    ar->creator_cpu = BP_CPU_UNKNOWN;
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
#elif defined(__PLAN9__)
    ar->creator_os = BP_OS_PLAN9;
#endif
#endif
#ifdef BP_CREATOR_CPU
    ar->creator_cpu = BP_CREATOR_CPU;
#else
#if defined(PLAN9_CPU_X86_64)
    ar->creator_cpu = BP_CPU_X86_64;
#elif defined(PLAN9_CPU_X86)
    ar->creator_cpu = BP_CPU_X86;
#elif defined(PLAN9_CPU_ARM32)
    ar->creator_cpu = BP_CPU_ARM32;
#elif defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
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
#elif defined(__riscv) && defined(__LP64__)
    ar->creator_cpu = BP_CPU_RISCV64;
#elif defined(__riscv)
    ar->creator_cpu = BP_CPU_RISCV32;
#elif defined(__sparc__) && defined(__arch64__)
    ar->creator_cpu = BP_CPU_SPARC64;
#elif defined(__sparc__)
    ar->creator_cpu = BP_CPU_SPARC;
#elif defined(__mips__) && defined(__LP64__)
    ar->creator_cpu = BP_CPU_MIPS64;
#elif defined(__mips__)
    ar->creator_cpu = BP_CPU_MIPS32;
#elif defined(__alpha__)
    ar->creator_cpu = BP_CPU_ALPHA;
#elif defined(__hppa__)
    ar->creator_cpu = BP_CPU_PARISC;
#elif defined(__s390x__)
    ar->creator_cpu = BP_CPU_ZARCH;
#elif defined(__ia64__)
    ar->creator_cpu = BP_CPU_IA64;
#endif
#endif
}

static int v7_init(bp_archive_t *ar, bp_uint32 block_size, bp_uint8 method)
{
    v7w_t *w;

    if (!bp_method_is_supported(method)) return BP_ERR_UNSUPPORTED_METHOD;
    if (block_size > V7_BLOCK_CAP) return BP_ERR_INVALID_ARG;
    if (method == BP_METHOD_BOLTLZ && block_size > BOLTLZ_MAX_BLOCK)
        return BP_ERR_INVALID_ARG;
    memset(ar, 0, sizeof(*ar));
    ar->block_size = block_size ? block_size : BP_BLOCK_DEFAULT;
    ar->method = method;
    v7_detect_creator(ar);
    w = (v7w_t *)calloc(1, sizeof(*w));
    if (w == NULL) return BP_ERR_NOMEM;
    ar->v7 = w;
    v7_new_set_id(ar, &w->set_id);
    return BP_OK;
}

int bp_archive_create(bp_archive_t *ar, bp_io_t *io,
                      bp_uint32 block_size, bp_uint8 method)
{
    v7w_t *w;
    bp_uint8 h[V7_VOL_HDR];
    int rc;

    if (ar == NULL || io == NULL) return BP_ERR_INVALID_ARG;
    rc = v7_init(ar, block_size, method);
    if (rc != BP_OK) return rc;
    w = W(ar);
    w->io = io;
    ar->io = io;
    w->vol_open = 1;
    v7_build_vol_header(h, w->set_id, 0, ar->creator_os, ar->creator_cpu);
    rc = vw_write(w, h, V7_VOL_HDR);
    if (rc != BP_OK) v7w_free(ar);
    return rc;
}

int bp_archive_create_volumes(bp_archive_t *ar, const bp_volume_sink_t *sink,
                              bp_uint64 volume_size, bp_uint32 block_size,
                              bp_uint8 method)
{
    v7w_t *w;
    int rc;

    if (ar == NULL || sink == NULL || sink->create == NULL ||
        sink->finish == NULL)
        return BP_ERR_INVALID_ARG;
    if (!bp_u64_is_zero(volume_size) &&
        bp_u64_cmp(volume_size, bp_u64_from_u32(V7_MIN_VOLUME)) < 0)
        return BP_ERR_INVALID_ARG;
    rc = v7_init(ar, block_size, method);
    if (rc != BP_OK) return rc;
    w = W(ar);
    w->multi = 1;
    w->sink = *sink;
    w->limit = volume_size;
    w->number = 0;
    rc = vw_open_volume(ar);
    if (rc != BP_OK) {
        if (w->vol_open)
            (void)w->sink.finish(w->sink.user, w->io, w->number, 0, rc);
        v7w_free(ar);
    }
    return rc;
}

int bp_archive_output_size(const bp_archive_t *ar, bp_uint64 *size)
{
    const v7w_t *w;
    if (ar == NULL || size == NULL || ar->v7 == NULL) return BP_ERR_INVALID_ARG;
    w = (const v7w_t *)ar->v7;
    if (!w->finalized || w->multi) return BP_ERR_INVALID_ARG;
    *size = w->final_size;
    return BP_OK;
}

static int entry_list_grow(bp_archive_t *ar)
{
    bp_uint32 new_cap;
    size_t byte_cap;
    bp_entry_t *new_list;

    if (ar->entries_written < ar->entry_list_cap) return BP_OK;
    new_cap = ar->entry_list_cap ? ar->entry_list_cap * 2 : 64;
    byte_cap = (size_t)new_cap * sizeof(bp_entry_t);
    if (new_cap <= ar->entry_list_cap ||
        byte_cap / sizeof(bp_entry_t) != (size_t)new_cap)
        return BP_ERR_NOMEM;
    new_list = (bp_entry_t *)realloc(ar->entry_list, byte_cap);
    if (new_list == NULL) return BP_ERR_NOMEM;
    ar->entry_list = new_list;
    ar->entry_list_cap = new_cap;
    return BP_OK;
}

static int record_entry_at(bp_archive_t *ar, bp_uint16 vol, bp_uint64 off,
                           const bp_packed_entry_t *packed)
{
    bp_entry_t *ent;
    int rc;

    rc = entry_list_grow(ar);
    if (rc != BP_OK) return rc;
    ent = &ar->entry_list[ar->entries_written];
    memset(ent, 0, sizeof(*ent));
    ent->name = (char *)malloc((size_t)packed->name_len + 1);
    if (ent->name == NULL) return BP_ERR_NOMEM;
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
    ent->volume = vol;
    ent->offset = off;
    ar->entries_written++;
    if (ar->profile != BP_PROFILE_NONE)
        ar->profile_table_bytes = u64_add32(ar->profile_table_bytes,
            2U + (bp_uint32)packed->name_len + V7_REC_TAIL);
    ar->archive_orig_total = bp_u64_add(ar->archive_orig_total,
                                        packed->original_size);
    ar->archive_comp_total = bp_u64_add(ar->archive_comp_total,
                                        packed->compressed_size);
    ar->archive_orig_done = bp_u64_add(ar->archive_orig_done,
                                       packed->original_size);
    return BP_OK;
}

/* ---- block compression for end-of-volume pieces ------------------------ */

static int split_ensure(bp_archive_t *ar, bp_uint8 method, bp_uint32 len)
{
    v7w_t *w = W(ar);
    bp_uint32 need = bp_method_compress_bound(method, len);

    if (need < len) need = len;
    if (w->split_out_size < need) {
        free(w->split_out);
        w->split_out = (bp_uint8 *)malloc((size_t)need);
        w->split_out_size = w->split_out ? need : 0;
        if (w->split_out == NULL) return BP_ERR_NOMEM;
    }
    if (method == BP_METHOD_FLUFF && w->split_fctx == NULL) {
        w->split_fctx = fluff_ctx_alloc();
        if (w->split_fctx == NULL) return BP_ERR_NOMEM;
    }
    if (method == BP_METHOD_BOLTLZ && w->split_lz_work == NULL) {
        w->split_lz_work = malloc((size_t)bp_compress_work_size());
        if (w->split_lz_work == NULL) return BP_ERR_NOMEM;
    }
    if (method == BP_METHOD_BOLTLITE && w->split_bl_work == NULL) {
        w->split_bl_work = (bp_uint8 *)malloc((size_t)BOLTLITE_WORK_SIZE);
        if (w->split_bl_work == NULL) return BP_ERR_NOMEM;
    }
    return BP_OK;
}

/* Compress `len` bytes on their own into w->split_out.  The piece is
 * compressed without history: for Fluff that yields an ordinary (non-reset)
 * block that the decoder can still place inside the entry's history.  Falls
 * back to a raw copy under the 99% rule. */
static int compress_piece(bp_archive_t *ar, bp_uint8 method,
                          const bp_uint8 *src, bp_uint32 len,
                          bp_uint32 *out_len)
{
    v7w_t *w = W(ar);
    bp_uint32 dl = 0;
    int rc = BP_OK;

    if (method == BP_METHOD_BOLTLITE) {
        dl = boltlite_compress(src, len, w->split_out, w->split_bl_work);
        if (dl == 0) dl = len;
    } else if (method == BP_METHOD_BOLTLZ) {
        dl = w->split_out_size;
        rc = bp_compress_block_work(src, len, w->split_out, &dl,
                                    w->split_lz_work, ar->method_level);
    } else if (method == BP_METHOD_FLUFF) {
        if (ar->method_level > 0)
            fluff_ctx_reset_level(w->split_fctx, (int)ar->method_level);
        else
            fluff_ctx_reset(w->split_fctx);
        rc = fluff_compress_block_ctx(src, len, w->split_out, &dl,
                                      w->split_fctx);
    } else {
        dl = len;
    }
    if (rc != BP_OK) return rc;
    if (method == BP_METHOD_STORED || dl >= len - len / 100) {
        memcpy(w->split_out, src, (size_t)len);
        dl = len;
    }
    *out_len = dl;
    return BP_OK;
}

static int write_block_now(bp_archive_t *ar, const bp_uint8 *comp,
                           bp_uint32 clen, bp_uint32 olen, bp_uint64 *total)
{
    v7w_t *w = W(ar);
    bp_uint8 hdr[V7_BLOCK_HDR];
    int rc;

    bp_write_le32(hdr, clen);
    bp_write_le32(hdr + 4, olen);
    bp_write_le32(hdr + 8, bp_crc32(0, comp, clen));
    rc = vw_write(w, hdr, V7_BLOCK_HDR);
    if (rc == BP_OK) rc = vw_write(w, comp, clen);
    if (rc == BP_OK && total != NULL) *total = u64_add32(*total, V7_BLOCK_HDR + clen);
    return rc;
}

/* floor(x * y / z) without floating point (some targets link no soft-float
 * library), saturating at 0xFFFFFFFF; z == 0 also saturates. */
static bp_uint32 v7_muldiv(bp_uint32 x, bp_uint32 y, bp_uint32 z)
{
    bp_uint32 xl = x & 0xFFFFUL, xh = (x >> 16) & 0xFFFFUL;
    bp_uint32 yl = y & 0xFFFFUL, yh = (y >> 16) & 0xFFFFUL;
    bp_uint32 ll = xl * yl, lh = xl * yh, hl = xh * yl, hh = xh * yh;
    bp_uint32 mid = (ll >> 16) + (lh & 0xFFFFUL) + (hl & 0xFFFFUL);
    bp_uint32 lo = (ll & 0xFFFFUL) | ((mid & 0xFFFFUL) << 16);
    bp_uint32 hi = hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
    int i;

    if (z == 0 || hi >= z) return 0xFFFFFFFFUL;
    for (i = 0; i < 32; i++) {          /* (hi:lo) / z, hi < z throughout */
        bp_uint32 carry = (hi >> 31) & 1UL;
        hi = ((hi << 1) | (lo >> 31)) & 0xFFFFFFFFUL;
        lo = (lo << 1) & 0xFFFFFFFFUL;
        if (carry || hi >= z) {
            hi = (hi - z) & 0xFFFFFFFFUL;
            lo |= 1UL;
        }
    }
    return lo;
}

/* Choose and write the largest piece of orig[0..olen) that, compressed on
 * its own, fits in `room` bytes of block data.  Returns its length. */
static int write_fitting_piece(bp_archive_t *ar, bp_uint8 method,
                               const bp_uint8 *orig, bp_uint32 olen,
                               bp_uint32 room, bp_uint32 hint_clen,
                               bp_uint64 *total, bp_uint32 *taken)
{
    v7w_t *w = W(ar);
    bp_uint32 a, best_a = 0, pl;
    int tries, rc;
    bp_uint32 est;

    rc = split_ensure(ar, method, olen);
    if (rc != BP_OK) return rc;

    if (method == BP_METHOD_STORED) {
        a = room < olen ? room : olen;
        memcpy(w->split_out, orig, (size_t)a);
        rc = write_block_now(ar, w->split_out, a, a, total);
        *taken = a;
        return rc;
    }

    /* Bracketing search: best_a fits, too_big does not.  Each step
     * estimates from the last compression ratio and stays inside the
     * bracket; stop within 256 bytes of a full volume. */
    {
        bp_uint32 too_big = olen + 1U;
        bp_uint32 best_pl = 0, too_pl = 0;
        bp_uint32 slack = room / 256U < 128U ? 128U : room / 256U;
        int out_holds_best = 0;

        est = hint_clen > 0 ? v7_muldiv(room, olen, hint_clen) : room;
        est -= est / 50U;                          /* 98% */
        for (tries = 0; tries < 12; tries++) {
            if (best_a > 0 && too_pl > 0) {
                /* Both ends known: interpolate between them (the local
                 * ratio, which differs from the block average when the
                 * data changes character), bisecting every third try. */
                bp_uint32 want = room - best_pl;   /* > slack: not yet full */
                if (tries % 3 == 2 || too_pl <= best_pl || want <= slack / 2U)
                    est = best_a + (too_big - best_a) / 2U;
                else
                    est = best_a + v7_muldiv(want - slack / 2U,
                                             too_big - best_a,
                                             too_pl - best_pl);
                if (est < best_a) est = 0xFFFFFFFFUL;   /* wrapped */
            }
            if (est >= too_big) a = too_big - 1U;
            else if (est < 1U) a = 1;
            else a = est;
            if (a <= best_a || a >= too_big) a = best_a + (too_big - best_a) / 2U;
            if (a <= best_a || a >= too_big) break;
            rc = compress_piece(ar, method, orig, a, &pl);
            if (rc != BP_OK) return rc;
            if (pl <= room) {
                best_a = a;
                best_pl = pl;
                out_holds_best = 1;
                if (a == olen || room - pl <= slack) break;
                est = v7_muldiv(a, room, pl ? pl : 1U);
            } else {
                too_big = a;
                too_pl = pl;
                out_holds_best = 0;
                est = v7_muldiv(a, room - slack / 2U, pl);
            }
        }
        if (best_a < room && best_a < olen) {
            /* a raw piece always fits exactly and carries more data */
            best_a = room < olen ? room : olen;
            memcpy(w->split_out, orig, (size_t)best_a);
            pl = best_a;
        } else if (!out_holds_best) {
            rc = compress_piece(ar, method, orig, best_a, &pl);
            if (rc != BP_OK) return rc;
            if (pl > room) return BP_ERR_COMPRESS;
        }
    }
    rc = write_block_now(ar, w->split_out, pl, best_a, total);
    *taken = best_a;
    return rc;
}

/* Place one block.  comp/clen is the encoded block (raw when clen == olen).
 * orig may be NULL when the original bytes are not at hand; such a block can
 * only move to the next volume, never be split. */
static int vw_put_block(bp_archive_t *ar, bp_uint8 method,
                        const bp_uint8 *orig, bp_uint32 olen,
                        const bp_uint8 *comp, bp_uint32 clen,
                        bp_uint64 *total)
{
    v7w_t *w = W(ar);
    int rc;

    for (;;) {
        bp_uint32 room;
        bp_uint32 taken;

        if (vw_fits(w, V7_BLOCK_HDR + clen))
            return write_block_now(ar, comp, clen, olen, total);
        if (!w->multi) return BP_ERR_OVERFLOW;
        room = u64_cap32(vw_space(w), 0x7FFFFFFFUL);
        if (orig != NULL && room >= V7_MIN_EOV_SPACE) {
            room -= V7_BLOCK_HDR;
            rc = write_fitting_piece(ar, method, orig, olen, room, clen,
                                     total, &taken);
            if (rc != BP_OK) return rc;
            orig += taken;
            olen -= taken;
            if (olen == 0) return BP_OK;
            rc = vw_next_volume(ar);
            if (rc != BP_OK) return rc;
            rc = split_ensure(ar, method, olen);
            if (rc != BP_OK) return rc;
            rc = compress_piece(ar, method, orig, olen, &clen);
            if (rc != BP_OK) return rc;
            comp = W(ar)->split_out;
            continue;
        }
        if (vw_body_empty(w)) return BP_ERR_OVERFLOW;
        rc = vw_next_volume(ar);
        if (rc != BP_OK) return rc;
    }
}

/* ---- entry output: into the archive or into a packed blob -------------- */

typedef struct {
    bp_archive_t *ar;
    bp_io_t      *plain;   /* pack mode */
    bp_uint16     name_len;
} eout_t;

static int eo_struct(eout_t *o, const bp_uint8 *buf, bp_uint32 len,
                     bp_uint16 *vol, bp_uint64 *off)
{
    int rc;
    if (o->plain != NULL) return io_write(o->plain, buf, len);
    rc = profile_check_extra(o->ar, len, o->name_len);
    if (rc != BP_OK) return rc;
    return vw_put_struct(o->ar, buf, len, vol, off);
}

static int eo_block(eout_t *o, bp_uint8 method, const bp_uint8 *orig,
                    bp_uint32 olen, const bp_uint8 *comp, bp_uint32 clen,
                    bp_uint64 *total)
{
    int rc;
    if (o->plain != NULL) {
        bp_uint8 hdr[V7_BLOCK_HDR];
        bp_write_le32(hdr, clen);
        bp_write_le32(hdr + 4, olen);
        bp_write_le32(hdr + 8, bp_crc32(0, comp, clen));
        rc = io_write(o->plain, hdr, V7_BLOCK_HDR);
        if (rc == BP_OK) rc = io_write(o->plain, comp, clen);
        if (rc == BP_OK) *total = u64_add32(*total, V7_BLOCK_HDR + clen);
        return rc;
    }
    rc = profile_check_extra(o->ar, V7_BLOCK_HDR + clen, o->name_len);
    if (rc != BP_OK) return rc;
    return vw_put_block(o->ar, method, orig, olen, comp, clen, total);
}

static int read_failed(bp_archive_t *ar, const char *name, bp_uint32 chunk,
                       long n)
{
    char lbuf[512];
    sprintf(lbuf, "boltpack: low-level read failed for entry %.200s: "
            "requested=%u read=%ld errno=%d (%s)",
            name, (unsigned)chunk, n, errno, strerror(errno));
    bp_core_log(ar, lbuf);
    return BP_ERR_IO;
}

static int report_write_progress(bp_archive_t *ar, const char *name,
                                 bp_uint64 file_size, bp_uint64 done,
                                 bp_uint64 total_comp)
{
    bp_progress_t p;
    if (ar->progress_fn == NULL) return BP_OK;
    memset(&p, 0, sizeof(p));
    p.file_index = ar->entries_written;
    p.file_name = name;
    p.file_orig_size = file_size;
    p.file_bytes_done = done;
    p.file_comp_size = total_comp;
    p.archive_orig_total = bp_u64_add(ar->archive_orig_total, done);
    p.archive_comp_total = bp_u64_add(ar->archive_comp_total, total_comp);
    p.archive_orig_done = bp_u64_add(ar->archive_orig_done, done);
    return ar->progress_fn(&p, ar->progress_data) != 0 ? BP_ERR_ABORTED : BP_OK;
}

/* Allocate the sequential codec buffers and state for `method`.  The
 * BoltLite workspace is returned in *boltlite_work for the caller to free. */
static int seq_setup(bp_archive_t *ar, bp_uint8 method,
                     bp_uint8 **boltlite_work, fluff_ctx_t **fctx)
{
    bp_uint32 bsize = ar->block_size;

    *boltlite_work = NULL;
    *fctx = NULL;
    if (ar->seq_buf_block_size != bsize) {
        free(ar->seq_in_buf);  ar->seq_in_buf = NULL;
        free(ar->seq_out_buf); ar->seq_out_buf = NULL;
        ar->seq_buf_block_size = 0;
    }
    if (ar->seq_in_buf == NULL) {
        ar->seq_in_buf = (bp_uint8 *)malloc((size_t)bsize);
        if (ar->seq_in_buf == NULL) return BP_ERR_NOMEM;
        ar->seq_buf_block_size = bsize;
    }
    if (ar->seq_out_buf == NULL) {
        /* sized for the largest method bound at this block size */
        bp_uint32 sz = bp_method_compress_bound(BP_METHOD_FLUFF, bsize);
        bp_uint32 s2 = bp_method_compress_bound(BP_METHOD_BOLTLZ, bsize);
        bp_uint32 s3 = bp_method_compress_bound(BP_METHOD_BOLTLITE, bsize);
        if (s2 > sz) sz = s2;
        if (s3 > sz) sz = s3;
        if (bsize > sz) sz = bsize;
        ar->seq_out_buf = (bp_uint8 *)malloc((size_t)sz);
        if (ar->seq_out_buf == NULL) return BP_ERR_NOMEM;
    }
    if (method == BP_METHOD_BOLTLITE) {
        *boltlite_work = (bp_uint8 *)malloc((size_t)BOLTLITE_WORK_SIZE);
        if (*boltlite_work == NULL) return BP_ERR_NOMEM;
    }
    if (method == BP_METHOD_BOLTLZ && ar->seq_boltlz_work == NULL) {
        ar->seq_boltlz_work = malloc((size_t)bp_compress_work_size());
        if (ar->seq_boltlz_work == NULL) {
            free(*boltlite_work);
            *boltlite_work = NULL;
            return BP_ERR_NOMEM;
        }
    }
    if (method == BP_METHOD_FLUFF) {
        if (ar->seq_fluff_ctx == NULL) {
            ar->seq_fluff_ctx = (void *)fluff_ctx_alloc();
            if (ar->seq_fluff_ctx == NULL) {
                free(*boltlite_work);
                *boltlite_work = NULL;
                return BP_ERR_NOMEM;
            }
        }
        *fctx = (fluff_ctx_t *)ar->seq_fluff_ctx;
        if (ar->method_level > 0) fluff_ctx_reset_level(*fctx, (int)ar->method_level);
        else fluff_ctx_reset(*fctx);
    }
    return BP_OK;
}

/* Compress one chunk of ar->seq_in_buf into ar->seq_out_buf.  *data points
 * at the bytes to store: the input itself when compression saves less than
 * 1% (a raw block, clen == chunk). */
static int seq_compress(bp_archive_t *ar, bp_uint8 method, bp_uint32 chunk,
                        bp_uint8 *boltlite_work, fluff_ctx_t *fctx,
                        const bp_uint8 **data, bp_uint32 *clen)
{
    bp_uint8 *in_buf = ar->seq_in_buf;
    bp_uint8 *out_buf = ar->seq_out_buf;
    bp_uint32 comp_len = 0;
    int rc;

    if (method == BP_METHOD_BOLTLITE) {
        comp_len = boltlite_compress(in_buf, chunk, out_buf, boltlite_work);
        if (comp_len == 0) comp_len = chunk;
    } else if (method == BP_METHOD_BOLTLZ) {
        comp_len = bp_method_compress_bound(BP_METHOD_BOLTLZ, ar->block_size);
        if (comp_len < ar->block_size) comp_len = ar->block_size;
        rc = bp_compress_block_work(in_buf, chunk, out_buf, &comp_len,
                                    ar->seq_boltlz_work, ar->method_level);
        if (rc != BP_OK) return rc;
    } else if (method == BP_METHOD_FLUFF) {
        rc = fluff_compress_block_ctx(in_buf, chunk, out_buf, &comp_len, fctx);
        if (rc != BP_OK) return rc;
    } else {
        comp_len = chunk;
    }
    if (method == BP_METHOD_STORED || comp_len >= chunk - chunk / 100) {
        *data = in_buf;
        *clen = chunk;
    } else {
        *data = out_buf;
        *clen = comp_len;
    }
    return BP_OK;
}

/* Compress and write the payload blocks of one entry. */
static int write_entry_blocks(bp_archive_t *ar, eout_t *o, const char *name,
                              bp_uint8 method, bp_io_t *file_io,
                              bp_uint64 file_size, int record_entry,
                              bp_uint32 *checksum, bp_uint64 *total_comp)
{
    bp_uint64 remaining = file_size;
    bp_uint32 bsize = ar->block_size;
    bp_uint32 out_buf_size = bp_method_compress_bound(method, bsize);
    bp_uint32 crc = 0;
    bp_uint8 *in_buf;
    bp_uint8 *boltlite_work = NULL;
    fluff_ctx_t *fctx = NULL;
    int rc = BP_OK;

    if (out_buf_size < bsize) out_buf_size = bsize;

#ifdef BOLTPACK_FAST_FLUFF
    if (ar->fast_fluff_threads > 1 && method == BP_METHOD_FLUFF) {
        fluff_fast_ctx_t *ffctx;
        bp_uint8 *batch_srcs[FAST_FLUFF_MAX_BATCH];
        bp_uint8 *batch_dsts[FAST_FLUFF_MAX_BATCH];
        bp_uint32 batch_src_lens[FAST_FLUFF_MAX_BATCH];
        bp_uint32 batch_dst_lens[FAST_FLUFF_MAX_BATCH];
        bp_uint32 batch_total, fast_hist_len, fast_hist_cap, fast_stream_pos;
        int batch_cap, batch_n, b;

        rc = bp_fast_fluff_ensure(ar, bsize, out_buf_size);
        if (rc != BP_OK) return rc;
        if (ar->fast_fluff_ctx == NULL) goto sequential;
        ffctx = (fluff_fast_ctx_t *)ar->fast_fluff_ctx;
        fast_hist_cap = ar->fast_fluff_hist_size;
        fast_hist_len = 0;
        fast_stream_pos = 0;
        batch_cap = (int)ar->fast_fluff_batch_blocks;
        for (b = 0; b < batch_cap; b++) {
            batch_srcs[b] = ar->fast_fluff_in_base + (size_t)b * (size_t)bsize;
            batch_dsts[b] = ar->fast_fluff_out_base + (size_t)b * (size_t)out_buf_size;
        }
        while (!bp_u64_is_zero(remaining)) {
            batch_n = 0;
            batch_total = 0;
            while (batch_n < batch_cap && !bp_u64_is_zero(remaining)) {
                bp_uint32 chunk = u64_cap32(remaining, bsize);
                long n = file_io->read(file_io->handle, batch_srcs[batch_n], chunk);
                if (n != (long)chunk) return read_failed(ar, name, chunk, n);
                crc = bp_crc32(crc, batch_srcs[batch_n], chunk);
                batch_src_lens[batch_n] = chunk;
                batch_total += chunk;
                remaining = bp_u64_sub(remaining, bp_u64_from_u32(chunk));
                batch_n++;
            }
            rc = fluff_fast_compress_blocks_with_history(
                (const bp_uint8 **)batch_srcs, batch_src_lens, batch_dsts,
                batch_dst_lens, batch_n, ffctx, ar->fast_fluff_hist_base,
                fast_hist_len, fast_stream_pos);
            if (rc != BP_OK) return rc;
            for (b = 0; b < batch_n; b++) {
                rc = eo_block(o, method, batch_srcs[b], batch_src_lens[b],
                              batch_dsts[b], batch_dst_lens[b], total_comp);
                if (rc != BP_OK) return rc;
            }
            if (record_entry) {
                rc = report_write_progress(ar, name, file_size,
                         bp_u64_sub(file_size, remaining), *total_comp);
                if (rc != BP_OK) return rc;
            }
            if (fast_hist_cap != 0) {
                if (batch_total >= fast_hist_cap) {
                    memcpy(ar->fast_fluff_hist_base,
                           ar->fast_fluff_in_base + (batch_total - fast_hist_cap),
                           fast_hist_cap);
                    fast_hist_len = fast_hist_cap;
                } else {
                    bp_uint32 keep_old = fast_hist_len;
                    if (keep_old > fast_hist_cap - batch_total)
                        keep_old = fast_hist_cap - batch_total;
                    if (keep_old != 0 && keep_old < fast_hist_len)
                        memmove(ar->fast_fluff_hist_base,
                                ar->fast_fluff_hist_base + (fast_hist_len - keep_old),
                                keep_old);
                    memcpy(ar->fast_fluff_hist_base + keep_old,
                           ar->fast_fluff_in_base, batch_total);
                    fast_hist_len = keep_old + batch_total;
                }
            }
            fast_stream_pos += batch_total;
        }
        *checksum = crc;
        return BP_OK;
    }
    if (ar->fast_fluff_threads > 1 && method == BP_METHOD_BOLTLZ &&
        bp_fast_boltlz_effective_threads(file_size, bsize,
                                         ar->fast_fluff_threads) > 1) {
        bp_uint8 *batch_srcs[FAST_BOLTLZ_MAX_BATCH];
        bp_uint8 *batch_dsts[FAST_BOLTLZ_MAX_BATCH];
        bp_uint32 batch_src_lens[FAST_BOLTLZ_MAX_BATCH];
        bp_uint32 batch_dst_lens[FAST_BOLTLZ_MAX_BATCH];
        int batch_status[FAST_BOLTLZ_MAX_BATCH];
        void *work_base;
        bp_uint32 work_size;
        int nthreads_bz, batch_cap, batch_n, b;

        nthreads_bz = bp_fast_boltlz_effective_threads(file_size, bsize,
                                                       ar->fast_fluff_threads);
        rc = bp_fast_boltlz_ensure(ar, bsize, out_buf_size, nthreads_bz);
        if (rc != BP_OK) return rc;
        if (ar->fast_boltlz_in_base == NULL) goto sequential;
        work_base = ar->fast_boltlz_work_base;
        work_size = ar->fast_boltlz_work_size;
        nthreads_bz = (int)ar->fast_boltlz_ctx_threads;
        batch_cap = (int)ar->fast_boltlz_batch_blocks;
        for (b = 0; b < batch_cap; b++) {
            batch_srcs[b] = ar->fast_boltlz_in_base + (size_t)b * (size_t)bsize;
            batch_dsts[b] = ar->fast_boltlz_out_base + (size_t)b * (size_t)out_buf_size;
        }
        while (!bp_u64_is_zero(remaining)) {
            batch_n = 0;
            while (batch_n < batch_cap && !bp_u64_is_zero(remaining)) {
                bp_uint32 chunk = u64_cap32(remaining, bsize);
                long n = file_io->read(file_io->handle, batch_srcs[batch_n], chunk);
                if (n != (long)chunk) return read_failed(ar, name, chunk, n);
                crc = bp_crc32(crc, batch_srcs[batch_n], chunk);
                batch_src_lens[batch_n] = chunk;
                remaining = bp_u64_sub(remaining, bp_u64_from_u32(chunk));
                batch_n++;
            }
#pragma omp parallel for schedule(dynamic) num_threads(nthreads_bz)
            for (b = 0; b < batch_n; b++) {
                int tid = omp_get_thread_num();
                bp_uint32 comp_len = out_buf_size;
                if (tid < 0 || tid >= nthreads_bz) tid = 0;
                batch_status[b] = bp_compress_block_work(
                    batch_srcs[b], batch_src_lens[b], batch_dsts[b], &comp_len,
                    (bp_uint8 *)work_base + (size_t)tid * (size_t)work_size,
                    ar->method_level);
                batch_dst_lens[b] = comp_len;
            }
            for (b = 0; b < batch_n; b++)
                if (batch_status[b] != BP_OK) return batch_status[b];
            for (b = 0; b < batch_n; b++) {
                const bp_uint8 *data = batch_dsts[b];
                bp_uint32 dl = batch_dst_lens[b];
                if (dl >= batch_src_lens[b] - batch_src_lens[b] / 100) {
                    data = batch_srcs[b];
                    dl = batch_src_lens[b];
                }
                rc = eo_block(o, method, batch_srcs[b], batch_src_lens[b],
                              data, dl, total_comp);
                if (rc != BP_OK) return rc;
            }
            if (record_entry) {
                rc = report_write_progress(ar, name, file_size,
                         bp_u64_sub(file_size, remaining), *total_comp);
                if (rc != BP_OK) return rc;
            }
        }
        *checksum = crc;
        return BP_OK;
    }
sequential:
#endif /* BOLTPACK_FAST_FLUFF */

    rc = seq_setup(ar, method, &boltlite_work, &fctx);
    if (rc != BP_OK) return rc;
    in_buf = ar->seq_in_buf;

    while (!bp_u64_is_zero(remaining)) {
        bp_uint32 chunk = u64_cap32(remaining, bsize);
        bp_uint32 comp_len = 0;
        const bp_uint8 *data;
        long n;

        n = file_io->read(file_io->handle, in_buf, chunk);
        if (n != (long)chunk) { rc = read_failed(ar, name, chunk, n); break; }
        crc = bp_crc32(crc, in_buf, chunk);
        rc = seq_compress(ar, method, chunk, boltlite_work, fctx,
                          &data, &comp_len);
        if (rc != BP_OK) break;
        rc = eo_block(o, method, in_buf, chunk, data, comp_len, total_comp);
        if (rc != BP_OK) break;
        remaining = bp_u64_sub(remaining, bp_u64_from_u32(chunk));
        if (record_entry) {
            rc = report_write_progress(ar, name, file_size,
                     bp_u64_sub(file_size, remaining), *total_comp);
            if (rc != BP_OK) break;
        }
    }
    free(boltlite_work);
    if (rc == BP_OK) *checksum = crc;
    return rc;
}

static int bp_archive_write_entry(bp_archive_t *ar, const char *name,
                                  bp_uint8 entry_type, bp_uint8 platform,
                                  bp_io_t *file_io, bp_uint64 file_size,
                                  bp_uint64 mtime, bp_uint16 flags,
                                  bp_io_t *dst_io,
                                  bp_packed_entry_t *packed,
                                  int record_entry)
{
    bp_packed_entry_t local;
    bp_packed_entry_t *out;
    eout_t o;
    bp_uint16 name_len, hdr_size;
    bp_uint8 *hdr;
    bp_uint8 trl[V7_ENTRY_TRL];
    bp_uint8 method;
    bp_uint32 block_size;
    bp_uint32 checksum = 0;
    bp_uint64 total_comp = BP_UINT64_ZERO;
    bp_uint16 loc_vol = 0;
    bp_uint64 loc_off = BP_UINT64_ZERO;
    const bp_uint8 *single_data = NULL;
    bp_uint32 single_len = 0, single_clen = 0;
    int rc;

    /* Packing into a blob needs no writer state: parallel frontends pack
     * with bare worker contexts that copy only the codec settings. */
    if (ar == NULL || name == NULL) return BP_ERR_INVALID_ARG;
    if (!record_entry && (packed == NULL || dst_io == NULL))
        return BP_ERR_INVALID_ARG;
    if (record_entry && (ar->v7 == NULL || W(ar)->finalized))
        return BP_ERR_INVALID_ARG;
    if (strlen(name) > BP_MAX_NAME_LEN) return BP_ERR_NAME_TOO_LONG;
    name_len = (bp_uint16)strlen(name);
    if (!bp_archive_name_valid(name, (bp_uint32)name_len) ||
        !bp_archive_name_type_valid(name, (bp_uint32)name_len, entry_type) ||
        (!(flags & BP_ENTRY_FLAG_MTIME_VALID) && !bp_u64_is_zero(mtime)))
        return BP_ERR_INVALID_ARG;
    if (entry_type == BP_TYPE_DIR && !bp_u64_is_zero(file_size))
        return BP_ERR_INVALID_ARG;
    if (!bp_u64_is_zero(file_size) && file_io == NULL) return BP_ERR_INVALID_ARG;

    method = (entry_type == BP_TYPE_DIR || entry_type == BP_TYPE_SYMLINK ||
              bp_u64_is_zero(file_size)) ? BP_METHOD_STORED : ar->method;
    block_size = bp_u64_is_zero(file_size) ? 0 : ar->block_size;

    if (ar->profile != BP_PROFILE_NONE) {
        rc = profile_check_entry_values(ar->profile, method, block_size,
                                        entry_type, file_size);
        if (rc != BP_OK) return rc;
    }

    out = packed != NULL ? packed : &local;
    memset(out, 0, sizeof(*out));

    /* An entry that fits in one block is compressed before its header is
     * written, and recorded as Stored when the block stays raw (as format 6
     * recorded every entry without a compressed block), so readers without
     * the codec, such as the mini frontends reading Fluff, can extract it.
     * Larger entries keep the requested method: the header precedes their
     * blocks and the writer never seeks back. */
    if (method != BP_METHOD_STORED && !bp_u64_is_zero(file_size) &&
        bp_u64_cmp(file_size, bp_u64_from_u32(ar->block_size)) <= 0) {
        bp_uint8 *boltlite_work;
        fluff_ctx_t *fctx;
        long n;
        single_len = u64_cap32(file_size, ar->block_size);
        rc = seq_setup(ar, method, &boltlite_work, &fctx);
        if (rc != BP_OK) return rc;
        n = file_io->read(file_io->handle, ar->seq_in_buf, single_len);
        if (n != (long)single_len) {
            free(boltlite_work);
            return read_failed(ar, name, single_len, n);
        }
        checksum = bp_crc32(0, ar->seq_in_buf, single_len);
        rc = seq_compress(ar, method, single_len, boltlite_work, fctx,
                          &single_data, &single_clen);
        free(boltlite_work);
        if (rc != BP_OK) return rc;
        if (single_clen == single_len) method = BP_METHOD_STORED;
        /* Record the smallest standard block size that holds the block
         * (readers only allocate block_size), unless an interchange profile
         * fixes the block size. */
        if (ar->profile == BP_PROFILE_NONE) {
            static const bp_uint32 sizes[] = {
                BP_BLOCK_8K, BP_BLOCK_16K, BP_BLOCK_32K, BP_BLOCK_64K
            };
            unsigned i;
            for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
                if (sizes[i] >= single_len && sizes[i] < block_size) {
                    block_size = sizes[i];
                    break;
                }
            }
        }
    }

    hdr_size = (bp_uint16)(V7_ENTRY_FIXED + name_len);
    hdr = (bp_uint8 *)malloc((size_t)hdr_size);
    if (hdr == NULL) return BP_ERR_NOMEM;
    bp_write_le16(hdr, hdr_size);
    bp_write_le16(hdr + 2, name_len);
    memcpy(hdr + 4, name, name_len);
    bp_write_le64(hdr + 4 + name_len, file_size);
    hdr[12 + name_len] = method;
    hdr[13 + name_len] = BP_CKSUM_CRC32;
    bp_write_le32(hdr + 14 + name_len, block_size);
    hdr[18 + name_len] = entry_type;
    hdr[19 + name_len] = platform;
    bp_write_le64(hdr + 20 + name_len, mtime);
    bp_write_le16(hdr + 28 + name_len, flags);
    bp_write_le32(hdr + 30 + name_len, bp_crc32(0, hdr, (bp_uint32)hdr_size - 4U));

    o.ar = ar;
    o.plain = record_entry ? NULL : dst_io;
    o.name_len = name_len;

    rc = eo_struct(&o, hdr, hdr_size, &loc_vol, &loc_off);
    free(hdr);
    if (rc != BP_OK) return rc;

    if (single_data != NULL) {
        rc = eo_block(&o, method, ar->seq_in_buf, single_len, single_data,
                      single_clen, &total_comp);
        if (rc == BP_OK && record_entry)
            rc = report_write_progress(ar, name, file_size, file_size,
                                       total_comp);
        if (rc != BP_OK) return rc;
    } else if (!bp_u64_is_zero(file_size)) {
        rc = write_entry_blocks(ar, &o, name, method, file_io, file_size,
                                record_entry, &checksum, &total_comp);
        if (rc != BP_OK) return rc;
    }

    bp_write_le64(trl, total_comp);
    bp_write_le32(trl + 8, checksum);
    bp_write_le32(trl + 12, bp_crc32(0, trl, 12));
    rc = eo_struct(&o, trl, V7_ENTRY_TRL, NULL, NULL);
    if (rc != BP_OK) return rc;

    out->name = (char *)malloc((size_t)name_len + 1);
    if (out->name == NULL) return BP_ERR_NOMEM;
    memcpy(out->name, name, name_len);
    out->name[name_len] = '\0';
    out->name_len = name_len;
    out->header_size = hdr_size;
    out->original_size = file_size;
    out->compressed_size = total_comp;
    out->checksum = checksum;
    out->method = method;
    out->checksum_type = BP_CKSUM_CRC32;
    out->block_size = block_size;
    out->entry_type = entry_type;
    out->platform = platform;
    out->mtime = mtime;
    out->flags = flags;
    out->serialized_size = bp_u64_add(bp_u64_from_u32(hdr_size + V7_ENTRY_TRL),
                                      total_comp);
    if (!record_entry) return BP_OK;
    rc = record_entry_at(ar, loc_vol, loc_off, out);
    if (packed == NULL) bp_packed_entry_dispose(&local);
    return rc;
}

int bp_archive_add_entry(bp_archive_t *ar, const char *name,
                         bp_uint8 entry_type, bp_uint8 platform,
                         bp_io_t *file_io, bp_uint64 file_size,
                         bp_uint64 mtime)
{
    return bp_archive_write_entry(ar, name, entry_type, platform, file_io,
                                  file_size, mtime, BP_ENTRY_FLAG_MTIME_VALID,
                                  NULL, NULL, 1);
}

int bp_archive_add_entry_ex(bp_archive_t *ar, const char *name,
                            bp_uint8 entry_type, bp_uint8 platform,
                            bp_io_t *file_io, bp_uint64 file_size,
                            bp_uint64 mtime, bp_uint16 flags)
{
    return bp_archive_write_entry(ar, name, entry_type, platform, file_io,
                                  file_size, mtime, flags, NULL, NULL, 1);
}

int bp_archive_pack_entry(bp_archive_t *ar, const char *name,
                          bp_uint8 entry_type, bp_uint8 platform,
                          bp_io_t *file_io, bp_uint64 file_size,
                          bp_uint64 mtime, bp_io_t *dst_io,
                          bp_packed_entry_t *packed)
{
    return bp_archive_write_entry(ar, name, entry_type, platform, file_io,
                                  file_size, mtime, BP_ENTRY_FLAG_MTIME_VALID,
                                  dst_io, packed, 0);
}

int bp_archive_pack_entry_ex(bp_archive_t *ar, const char *name,
                             bp_uint8 entry_type, bp_uint8 platform,
                             bp_io_t *file_io, bp_uint64 file_size,
                             bp_uint64 mtime, bp_io_t *dst_io,
                             bp_packed_entry_t *packed, bp_uint16 flags)
{
    return bp_archive_write_entry(ar, name, entry_type, platform, file_io,
                                  file_size, mtime, flags, dst_io, packed, 0);
}

/* ---- file table, end record, finalize ---------------------------------- */

static bp_uint32 table_record_size(const bp_entry_t *e)
{
    return 2U + (bp_uint32)e->name_len + V7_REC_TAIL;
}

static void build_table_record(bp_uint8 *r, const bp_entry_t *e)
{
    bp_uint8 *t;
    bp_write_le16(r, e->name_len);
    memcpy(r + 2, e->name, e->name_len);
    t = r + 2 + e->name_len;
    bp_write_le64(t + 0, e->original_size);
    bp_write_le64(t + 8, e->compressed_size);
    t[16] = e->entry_type;
    t[17] = e->platform;
    bp_write_le64(t + 18, e->mtime);
    bp_write_le16(t + 26, e->volume);
    bp_write_le64(t + 28, e->offset);
    t[36] = e->method;
    bp_write_le16(t + 37, e->header_size);
    bp_write_le32(t + 39, e->checksum);
    bp_write_le16(t + 43, e->flags);
}

/* Write table, end record and the last trailer for the given entries. */
static int write_table_and_end(bp_archive_t *ar, const bp_entry_t *list,
                               bp_uint32 count)
{
    v7w_t *w = W(ar);
    bp_uint8 buf[V7_END_REC];
    bp_uint8 *rec;
    bp_uint64 table_bytes = bp_u64_from_u32(V7_TABLE_HDR);
    bp_uint64 total_orig = BP_UINT64_ZERO;
    bp_uint16 tvol;
    bp_uint64 toff;
    bp_uint32 crc = 0, i, rec_cap = 0;
    int rc;

    for (i = 0; i < count; i++) {
        table_bytes = u64_add32(table_bytes, table_record_size(&list[i]));
        total_orig = bp_u64_add(total_orig, list[i].original_size);
    }

    /* Keep the whole table in the last volume when a fresh volume can hold
     * it but the current one cannot, so listing needs one volume only. */
    if (vw_limited(w) && w->multi && !vw_body_empty(w)) {
        bp_uint64 need = u64_add32(table_bytes, V7_END_REC);
        bp_uint64 cap = bp_u64_sub(w->limit, bp_u64_from_u32(V7_VOL_HDR + V7_VOL_TRL));
        if (bp_u64_cmp(need, vw_space(w)) > 0 && bp_u64_cmp(need, cap) <= 0) {
            rc = vw_next_volume(ar);
            if (rc != BP_OK) return rc;
        }
    }

    buf[0] = BP_TABLE_MAGIC_0; buf[1] = BP_TABLE_MAGIC_1;
    buf[2] = BP_TABLE_MAGIC_2; buf[3] = BP_TABLE_MAGIC_3;
    bp_write_le32(buf + 4, count);
    bp_write_le64(buf + 8, total_orig);
    rc = vw_put_struct(ar, buf, V7_TABLE_HDR, &tvol, &toff);
    if (rc != BP_OK) return rc;
    crc = bp_crc32(crc, buf, V7_TABLE_HDR);

    rec = NULL;
    for (i = 0; i < count; i++) {
        bp_uint32 rs = table_record_size(&list[i]);
        if (rs > rec_cap) {
            free(rec);
            rec = (bp_uint8 *)malloc((size_t)rs);
            if (rec == NULL) return BP_ERR_NOMEM;
            rec_cap = rs;
        }
        build_table_record(rec, &list[i]);
        rc = vw_put_struct(ar, rec, rs, NULL, NULL);
        if (rc != BP_OK) { free(rec); return rc; }
        crc = bp_crc32(crc, rec, rs);
    }
    free(rec);

    rc = vw_reserve(ar, V7_END_REC);
    if (rc != BP_OK) return rc;
    memset(buf, 0, sizeof(buf));
    bp_write_le64(buf, w->set_id);
    bp_write_le16(buf + 8, (bp_uint16)(w->number + 1));
    bp_write_le16(buf + 10, tvol);
    bp_write_le32(buf + 12, crc);
    bp_write_le64(buf + 16, toff);
    bp_write_le64(buf + 24, table_bytes);
    bp_write_le32(buf + 32, count);
    bp_write_le32(buf + 40, bp_crc32(0, buf, 40));
    memcpy(buf + 44, v7_end_magic, 4);
    rc = vw_write(w, buf, V7_END_REC);
    if (rc != BP_OK) return rc;

    w->final_size = u64_add32(w->pos, V7_VOL_TRL);
    rc = vw_close_volume(ar, 1);
    if (rc != BP_OK) return rc;
    w->finalized = 1;
    return BP_OK;
}

int bp_archive_finalize(bp_archive_t *ar)
{
    bp_uint32 i;
    int rc;

    if (ar == NULL || ar->v7 == NULL || W(ar)->finalized || !W(ar)->vol_open)
        return BP_ERR_INVALID_ARG;
    {
        bp_metadata_group_t group = { NULL, 0, 0, NULL, 0 };
        for (i = 0; i < ar->entries_written; i++) {
            char *name = ar->entry_list[i].name;
            rc = bp_metadata_group_record(&group, &name,
                                          ar->entry_list[i].entry_type,
                                          ar->entry_list[i].platform, 0);
            if (rc != BP_OK) {
                bp_metadata_group_close(&group);
                return rc == BP_ERR_CORRUPT ? BP_ERR_INVALID_ARG : rc;
            }
        }
        bp_metadata_group_close(&group);
    }
    rc = profile_check_extra(ar, 0, 0);
    if (rc != BP_OK) return rc;
    return write_table_and_end(ar, ar->entry_list, ar->entries_written);
}

void bp_archive_close(bp_archive_t *ar)
{
    bp_uint32 i;
    v7w_t *w;

    if (ar == NULL) return;
#ifdef BOLTPACK_FAST_FLUFF
    bp_fast_fluff_release(ar);
    bp_fast_boltlz_release(ar);
#endif
    w = W(ar);
    if (w != NULL && w->multi && w->vol_open) {
        (void)w->sink.finish(w->sink.user, w->io, w->number, 0,
                             BP_ERR_ABORTED);
        w->vol_open = 0;
    }
    v7w_free(ar);
    if (ar->entry_list) {
        for (i = 0; i < ar->entries_written; i++) free(ar->entry_list[i].name);
        free(ar->entry_list);
        ar->entry_list = NULL;
    }
    if (ar->seq_fluff_ctx) {
        fluff_ctx_free((fluff_ctx_t *)ar->seq_fluff_ctx);
        ar->seq_fluff_ctx = NULL;
    }
    free(ar->seq_in_buf);  ar->seq_in_buf = NULL;
    free(ar->seq_out_buf); ar->seq_out_buf = NULL;
    ar->seq_buf_block_size = 0;
    free(ar->seq_boltlz_work); ar->seq_boltlz_work = NULL;
}

/* ---- reading ----------------------------------------------------------- */

typedef struct {
    bp_volume_source_t src;
    bp_io_t   *io;          /* current volume */
    int        cur;         /* current volume number, -1 = none */
    bp_uint64  size;        /* size of the current volume */
    bp_uint64  body_end;    /* size - trailer */
    bp_uint64  pos;         /* offset in the current volume */
    int        have_set;
    bp_uint64  set_id;
    bp_uint16  count;       /* volume count, 0 until the end record is read */
    bp_uint16  last;        /* number of the last volume */
    bp_uint8   creator_os, creator_cpu, ver_major, ver_minor;
    bp_uint64  last_size;
    bp_uint64  end_offset;  /* end record offset in the last volume */
    bp_uint16  table_volume;
    bp_uint32  table_crc;
    bp_uint64  table_offset;
    bp_uint64  table_size;
    bp_uint32  entry_count;
    bp_uint64  total_orig;
    bp_entry_t *entries;    /* validated table */
} vreader_t;

typedef struct {
    bp_uint16 number;
    int       last;
    bp_uint64 size;
    bp_uint64 set_id;
    bp_uint8  hdr[V7_VOL_HDR];
} volinfo_t;

/* Validate the header and trailer of one volume stream. */
static int v7_check_volume(bp_io_t *io, volinfo_t *vi)
{
    bp_uint8 t[V7_VOL_TRL];
    bp_uint8 *h = vi->hdr;
    bp_off_t end;
    bp_uint16 tflags;
    int i;

    if (io->seek(io->handle, 0, 0) != 0) return BP_ERR_IO;
    if (io_read(io, h, V7_VOL_HDR) != BP_OK) return BP_ERR_MAGIC;
    if (h[0] != BP_MAGIC_0 || h[1] != BP_MAGIC_1 || h[2] != BP_MAGIC_2 ||
        h[3] != BP_MAGIC_3)
        return BP_ERR_MAGIC;
    if (bp_read_le16(h + 4) != BP_FORMAT7_VERSION) return BP_ERR_VERSION;
    if (bp_read_le32(h + 28) != bp_crc32(0, h, 28)) return BP_ERR_CORRUPT;
    if (bp_read_le16(h + 6) != 0) return BP_ERR_CORRUPT;
    for (i = 22; i < 28; i++) if (h[i] != 0) return BP_ERR_CORRUPT;
    vi->set_id = bp_read_le64(h + 8);
    vi->number = bp_read_le16(h + 16);
    if (bp_u64_is_zero(vi->set_id) || vi->number > V7_MAX_VOLNUM)
        return BP_ERR_CORRUPT;

    if (io->seek(io->handle, 0, 2) != 0) return BP_ERR_IO;
    end = io->tell(io->handle);
    if (end < 0) return BP_ERR_IO;
    vi->size = bp_off_to_u64(end);
    if (bp_u64_cmp(vi->size, bp_u64_from_u32(V7_VOL_HDR + V7_VOL_TRL + 1)) < 0)
        return BP_ERR_TRUNCATED;
    if (io_seek_u64(io, bp_u64_sub(vi->size, bp_u64_from_u32(V7_VOL_TRL))) != BP_OK)
        return BP_ERR_IO;
    if (io_read(io, t, V7_VOL_TRL) != BP_OK) return BP_ERR_TRUNCATED;
    if (memcmp(t + 28, v7_trl_magic, 4) != 0 ||
        bp_read_le32(t + 24) != bp_crc32(0, t, 24))
        return BP_ERR_TRUNCATED;   /* most often a cut-off volume */
    tflags = bp_read_le16(t + 10);
    if ((tflags & ~V7_FLAG_LAST) != 0 || bp_read_le32(t + 12) != 0)
        return BP_ERR_CORRUPT;
    if (!u64_eq(bp_read_le64(t), vi->set_id) ||
        bp_read_le16(t + 8) != vi->number)
        return BP_ERR_CORRUPT;
    if (!u64_eq(bp_read_le64(t + 16), vi->size)) return BP_ERR_TRUNCATED;
    vi->last = (tflags & V7_FLAG_LAST) != 0;
    return BP_OK;
}

static void vr_close_io(vreader_t *vr)
{
    if (vr->io != NULL && vr->src.close != NULL)
        vr->src.close(vr->src.user, vr->io);
    vr->io = NULL;
    vr->cur = -1;
}

static void vr_free(vreader_t *vr)
{
    bp_uint32 i;
    vr_close_io(vr);
    if (vr->entries != NULL) {
        for (i = 0; i < vr->entry_count; i++) free(vr->entries[i].name);
        free(vr->entries);
        vr->entries = NULL;
    }
}

static void vr_init(vreader_t *vr, const bp_volume_source_t *src)
{
    memset(vr, 0, sizeof(*vr));
    vr->src = *src;
    vr->cur = -1;
}

/* Make volume `req` (or the last volume) current.  Retries through the
 * source with the reason a stream was rejected. */
static int vr_load(vreader_t *vr, bp_uint16 req)
{
    int reason = BP_OK;
    volinfo_t vi;

    if (vr->io != NULL) {
        if (req == BP_VOLUME_LAST_REQUEST && vr->count != 0 &&
            vr->cur == (int)vr->last)
            return BP_OK;
        if (req != BP_VOLUME_LAST_REQUEST && vr->cur == (int)req)
            return BP_OK;
    }
    vr_close_io(vr);
    if (req == BP_VOLUME_LAST_REQUEST && vr->count != 0) req = vr->last;
    for (;;) {
        /* The last volume is named differently from the numbered ones, so
         * the source is always asked for it as "the last volume". */
        bp_uint16 ask = (vr->count != 0 && req == vr->last)
                        ? (bp_uint16)BP_VOLUME_LAST_REQUEST : req;
        bp_io_t *io = vr->src.open(vr->src.user, ask, reason);
        int rc;
        if (io == NULL) return reason != BP_OK ? reason : BP_ERR_VOLUME;
        rc = v7_check_volume(io, &vi);
        if (rc == BP_OK && vr->have_set && !u64_eq(vi.set_id, vr->set_id))
            rc = BP_ERR_VOLUME;
        if (rc == BP_OK && req == BP_VOLUME_LAST_REQUEST && !vi.last)
            rc = BP_ERR_VOLUME;
        if (rc == BP_OK && req != BP_VOLUME_LAST_REQUEST && vi.number != req)
            rc = BP_ERR_VOLUME;
        if (rc == BP_OK && vr->count != 0 &&
            (vi.last != 0) != (vi.number == vr->last))
            rc = BP_ERR_CORRUPT;
        if (rc == BP_OK) {
            vr->io = io;
            vr->cur = (int)vi.number;
            vr->size = vi.size;
            vr->body_end = bp_u64_sub(vi.size, bp_u64_from_u32(V7_VOL_TRL));
            vr->pos = vi.size; /* unknown until positioned */
            if (!vr->have_set) {
                vr->have_set = 1;
                vr->set_id = vi.set_id;
            }
            if (vi.last) {
                vr->creator_os = vi.hdr[18];
                vr->creator_cpu = vi.hdr[19];
                vr->ver_major = vi.hdr[20];
                vr->ver_minor = vi.hdr[21];
            }
            return BP_OK;
        }
        if (vr->src.close != NULL) vr->src.close(vr->src.user, io);
        reason = rc;
    }
}

static int vr_seek(vreader_t *vr, bp_uint16 vol, bp_uint64 off)
{
    int rc = vr_load(vr, vol);
    if (rc != BP_OK) return rc;
    if (bp_u64_cmp(off, bp_u64_from_u32(V7_VOL_HDR)) < 0 ||
        bp_u64_cmp(off, vr->body_end) > 0)
        return BP_ERR_CORRUPT;
    rc = io_seek_u64(vr->io, off);
    if (rc != BP_OK) return rc;
    vr->pos = off;
    return BP_OK;
}

/* Position at the start of the next structure, moving to the next volume
 * when the current body is used up. */
static int vr_begin(vreader_t *vr)
{
    if (bp_u64_cmp(vr->pos, vr->body_end) < 0) return BP_OK;
    if (vr->cur < 0 || vr->count == 0 || vr->cur >= (int)vr->last)
        return BP_ERR_CORRUPT;
    return vr_seek(vr, (bp_uint16)(vr->cur + 1), bp_u64_from_u32(V7_VOL_HDR));
}

static int vr_avail(const vreader_t *vr, bp_uint32 len)
{
    bp_uint64 left = bp_u64_sub(vr->body_end, vr->pos);
    return bp_u64_cmp(left, bp_u64_from_u32(len)) >= 0;
}

static int vr_read(vreader_t *vr, void *buf, bp_uint32 len)
{
    int rc;
    if (!vr_avail(vr, len)) return BP_ERR_CORRUPT;
    rc = io_read(vr->io, buf, len);
    if (rc != BP_OK) return rc;
    vr->pos = u64_add32(vr->pos, len);
    return BP_OK;
}

static int vr_skip(vreader_t *vr, bp_uint32 len)
{
    if (!vr_avail(vr, len)) return BP_ERR_CORRUPT;
    vr->pos = u64_add32(vr->pos, len);
    return io_seek_u64(vr->io, vr->pos);
}

static int vr_at_body_start(const vreader_t *vr)
{
    return vr->pos.hi == 0 && vr->pos.lo == V7_VOL_HDR;
}

/* Open the set: last volume, end record. */
static int vr_open_set(vreader_t *vr)
{
    bp_uint8 e[V7_END_REC];
    bp_uint64 off;
    int rc;

    rc = vr_load(vr, BP_VOLUME_LAST_REQUEST);
    if (rc != BP_OK) return rc;
    vr->last = (bp_uint16)vr->cur;
    vr->last_size = vr->size;
    if (bp_u64_cmp(vr->body_end, bp_u64_from_u32(V7_VOL_HDR + V7_END_REC)) < 0)
        return BP_ERR_CORRUPT;
    off = bp_u64_sub(vr->body_end, bp_u64_from_u32(V7_END_REC));
    rc = io_seek_u64(vr->io, off);
    if (rc != BP_OK) return rc;
    rc = io_read(vr->io, e, V7_END_REC);
    if (rc != BP_OK) return rc;
    vr->pos = vr->body_end;
    if (memcmp(e + 44, v7_end_magic, 4) != 0 ||
        bp_read_le32(e + 40) != bp_crc32(0, e, 40) ||
        bp_read_le32(e + 36) != 0)
        return BP_ERR_CORRUPT;
    if (!u64_eq(bp_read_le64(e), vr->set_id)) return BP_ERR_CORRUPT;
    vr->count = bp_read_le16(e + 8);
    if (vr->count == 0 || vr->count > BP_VOLUME_MAX_COUNT ||
        vr->count != (bp_uint16)(vr->last + 1))
        return BP_ERR_CORRUPT;
    vr->table_volume = bp_read_le16(e + 10);
    vr->table_crc = bp_read_le32(e + 12);
    vr->table_offset = bp_read_le64(e + 16);
    vr->table_size = bp_read_le64(e + 24);
    vr->entry_count = bp_read_le32(e + 32);
    vr->end_offset = off;
    if (vr->table_volume >= vr->count ||
        bp_u64_cmp(vr->table_offset, bp_u64_from_u32(V7_VOL_HDR)) < 0 ||
        bp_u64_cmp(vr->table_size, bp_u64_from_u32(V7_TABLE_HDR)) < 0)
        return BP_ERR_CORRUPT;
    if (vr->table_volume == vr->last &&
        bp_u64_cmp(vr->table_offset, off) > 0)
        return BP_ERR_CORRUPT;
    /* every record is at least 48 bytes */
    {
        bp_uint64 maxrec = bp_u64_sub(vr->table_size,
                                      bp_u64_from_u32(V7_TABLE_HDR));
        bp_uint32 cap = u64_cap32(maxrec, 0xFFFFFFFFUL) / (3U + V7_REC_TAIL);
        if (vr->entry_count > cap) return BP_ERR_CORRUPT;
    }
    return BP_OK;
}

/* Location helper: after a structure ends at (cur,pos), where does the next
 * one begin? */
static void vr_next_location(const vreader_t *vr, bp_uint16 *vol,
                             bp_uint64 *off)
{
    if (bp_u64_cmp(vr->pos, vr->body_end) >= 0) {
        *vol = (bp_uint16)(vr->cur + 1);
        *off = bp_u64_from_u32(V7_VOL_HDR);
    } else {
        *vol = (bp_uint16)vr->cur;
        *off = vr->pos;
    }
}

static int loc_cmp(bp_uint16 av, bp_uint64 ao, bp_uint16 bv, bp_uint64 bo)
{
    if (av != bv) return av < bv ? -1 : 1;
    return bp_u64_cmp(ao, bo);
}

/* Read and validate the whole file table into vr->entries. */
static int vr_read_table(vreader_t *vr)
{
    bp_uint8 th[V7_TABLE_HDR];
    bp_uint8 tail[V7_REC_TAIL];
    bp_uint8 nl[2];
    bp_uint32 crc = 0, i;
    bp_uint64 bytes;
    bp_uint64 total_orig = BP_UINT64_ZERO;
    bp_metadata_group_t group = { NULL, 0, 0, NULL, 0 };
    bp_uint16 nv;
    bp_uint64 no;
    int rc;

    rc = vr_seek(vr, vr->table_volume, vr->table_offset);
    if (rc != BP_OK) return rc;
    rc = vr_read(vr, th, V7_TABLE_HDR);
    if (rc != BP_OK) return rc;
    if (th[0] != BP_TABLE_MAGIC_0 || th[1] != BP_TABLE_MAGIC_1 ||
        th[2] != BP_TABLE_MAGIC_2 || th[3] != BP_TABLE_MAGIC_3)
        return BP_ERR_CORRUPT;
    if (bp_read_le32(th + 4) != vr->entry_count) return BP_ERR_CORRUPT;
    crc = bp_crc32(crc, th, V7_TABLE_HDR);
    bytes = bp_u64_from_u32(V7_TABLE_HDR);

    if (vr->entry_count != 0) {
        vr->entries = (bp_entry_t *)calloc((size_t)vr->entry_count,
                                           sizeof(bp_entry_t));
        if (vr->entries == NULL) return BP_ERR_NOMEM;
    }
    for (i = 0; i < vr->entry_count; i++) {
        bp_entry_t *e = &vr->entries[i];

        rc = vr_begin(vr);
        if (rc != BP_OK) goto fail;
        rc = vr_read(vr, nl, 2);
        if (rc != BP_OK) goto fail;
        e->name_len = bp_read_le16(nl);
        if (e->name_len == 0 || e->name_len > V7_MAX_NAME ||
            !vr_avail(vr, (bp_uint32)e->name_len + V7_REC_TAIL)) {
            rc = BP_ERR_CORRUPT; goto fail;
        }
        e->name = (char *)malloc((size_t)e->name_len + 1);
        if (e->name == NULL) { rc = BP_ERR_NOMEM; goto fail; }
        rc = vr_read(vr, e->name, e->name_len);
        if (rc != BP_OK) goto fail;
        e->name[e->name_len] = '\0';
        rc = vr_read(vr, tail, V7_REC_TAIL);
        if (rc != BP_OK) goto fail;
        crc = bp_crc32(crc, nl, 2);
        crc = bp_crc32(crc, (const bp_uint8 *)e->name, e->name_len);
        crc = bp_crc32(crc, tail, V7_REC_TAIL);
        bytes = u64_add32(bytes, 2U + e->name_len + V7_REC_TAIL);

        e->original_size = bp_read_le64(tail + 0);
        e->compressed_size = bp_read_le64(tail + 8);
        e->entry_type = tail[16];
        e->platform = tail[17];
        e->mtime = bp_read_le64(tail + 18);
        e->volume = bp_read_le16(tail + 26);
        e->offset = bp_read_le64(tail + 28);
        e->method = tail[36];
        e->header_size = bp_read_le16(tail + 37);
        e->checksum = bp_read_le32(tail + 39);
        e->flags = bp_read_le16(tail + 43);
        e->checksum_type = BP_CKSUM_CRC32;
        total_orig = bp_u64_add(total_orig, e->original_size);

        if (!bp_archive_name_valid(e->name, e->name_len) ||
            !bp_archive_name_type_valid(e->name, e->name_len, e->entry_type) ||
            (!(e->flags & BP_ENTRY_FLAG_MTIME_VALID) && !bp_u64_is_zero(e->mtime)) ||
            e->entry_type > BP_TYPE_DIR ||
            (bp_uint32)e->header_size != V7_ENTRY_FIXED + (bp_uint32)e->name_len ||
            e->volume >= vr->count ||
            bp_u64_cmp(e->offset, bp_u64_from_u32(V7_VOL_HDR)) < 0 ||
            (e->entry_type == BP_TYPE_DIR && !bp_u64_is_zero(e->original_size)) ||
            (bp_u64_is_zero(e->original_size) && !bp_u64_is_zero(e->compressed_size))) {
            rc = BP_ERR_CORRUPT; goto fail;
        }
        if (i == 0) {
            if (e->volume != 0 || e->offset.hi != 0 || e->offset.lo != V7_VOL_HDR) {
                rc = BP_ERR_CORRUPT; goto fail;
            }
        } else if (loc_cmp(e->volume, e->offset, vr->entries[i - 1].volume,
                           vr->entries[i - 1].offset) <= 0) {
            rc = BP_ERR_CORRUPT; goto fail;
        }
        if (loc_cmp(e->volume, e->offset, vr->table_volume,
                    vr->table_offset) >= 0) {
            rc = BP_ERR_CORRUPT; goto fail;
        }
        {
            char *nm = e->name;
            rc = bp_metadata_group_record(&group, &nm, e->entry_type,
                                          e->platform, 0);
            if (rc != BP_OK) goto fail;
        }
    }
    bp_metadata_group_close(&group);

    if (crc != vr->table_crc || !u64_eq(bytes, vr->table_size))
        return BP_ERR_CORRUPT;
    vr_next_location(vr, &nv, &no);
    if (nv != vr->last || !u64_eq(no, vr->end_offset)) return BP_ERR_CORRUPT;
    if (vr->entry_count == 0 &&
        (vr->table_volume != 0 || vr->table_offset.hi != 0 ||
         vr->table_offset.lo != V7_VOL_HDR))
        return BP_ERR_CORRUPT;
    vr->total_orig = total_orig;
    return BP_OK;

fail:
    bp_metadata_group_close(&group);
    return rc;
}

static int vr_open_table(vreader_t *vr)
{
    int rc = vr_open_set(vr);
    if (rc == BP_OK) rc = vr_read_table(vr);
    return rc;
}

/* Read and check the entry header of table entry t at the current stream
 * position.  hdr receives the header fields (name owned by caller). */
static int vr_read_entry_header(vreader_t *vr, const bp_entry_t *t,
                                bp_entry_t *hdr)
{
    bp_uint8 pre[4];
    bp_uint8 *buf;
    bp_uint32 hs, n;
    const bp_uint8 *f;
    int rc;

    memset(hdr, 0, sizeof(*hdr));
    rc = vr_begin(vr);
    if (rc != BP_OK) return rc;
    if (vr->cur != (int)t->volume || !u64_eq(vr->pos, t->offset))
        return BP_ERR_CORRUPT;
    rc = vr_read(vr, pre, 4);
    if (rc != BP_OK) return rc;
    hs = bp_read_le16(pre);
    n = bp_read_le16(pre + 2);
    if (n == 0 || n > V7_MAX_NAME || hs != V7_ENTRY_FIXED + n ||
        !vr_avail(vr, hs - 4))
        return BP_ERR_CORRUPT;
    buf = (bp_uint8 *)malloc((size_t)hs);
    if (buf == NULL) return BP_ERR_NOMEM;
    memcpy(buf, pre, 4);
    rc = vr_read(vr, buf + 4, hs - 4);
    if (rc != BP_OK) { free(buf); return rc; }
    if (bp_read_le32(buf + hs - 4) != bp_crc32(0, buf, hs - 4)) {
        free(buf);
        return BP_ERR_CORRUPT;
    }
    f = buf + 4 + n;
    hdr->header_size = (bp_uint16)hs;
    hdr->name_len = (bp_uint16)n;
    hdr->original_size = bp_read_le64(f);
    hdr->method = f[8];
    hdr->checksum_type = f[9];
    hdr->block_size = bp_read_le32(f + 10);
    hdr->entry_type = f[14];
    hdr->platform = f[15];
    hdr->mtime = bp_read_le64(f + 16);
    hdr->flags = bp_read_le16(f + 24);
    hdr->volume = t->volume;
    hdr->offset = t->offset;
    rc = BP_OK;
    if (n != t->name_len || memcmp(buf + 4, t->name, n) != 0 ||
        hdr->header_size != t->header_size ||
        !u64_eq(hdr->original_size, t->original_size) ||
        hdr->method != t->method || hdr->entry_type != t->entry_type ||
        hdr->platform != t->platform || !u64_eq(hdr->mtime, t->mtime) ||
        hdr->flags != t->flags || hdr->checksum_type != BP_CKSUM_CRC32)
        rc = BP_ERR_CORRUPT;
    if (rc == BP_OK && !bp_u64_is_zero(hdr->original_size) &&
        (hdr->block_size == 0 || hdr->block_size > V7_BLOCK_CAP ||
         (hdr->method == BP_METHOD_BOLTLZ && hdr->block_size > BOLTLZ_MAX_BLOCK)))
        rc = BP_ERR_CORRUPT;
    free(buf);
    if (rc != BP_OK) return rc;
    hdr->name = (char *)malloc((size_t)n + 1);
    if (hdr->name == NULL) return BP_ERR_NOMEM;
    memcpy(hdr->name, t->name, n + 1);
    hdr->compressed_size = t->compressed_size;
    hdr->checksum = t->checksum;
    return BP_OK;
}

typedef struct {
    bp_progress_fn fn;
    void          *user;
    bp_uint32      index;
    bp_uint32      count;
    bp_uint64      archive_total;
    bp_uint64      archive_done;
} walk_progress_t;

/* Read the blocks and trailer of entry h.  decode: verify block CRCs and
 * decode into out (may be NULL); otherwise only check framing.  On success
 * *checksum is the payload CRC (decode mode). */
static int vr_read_entry_data(vreader_t *vr, const bp_entry_t *h, int decode,
                              bp_io_t *out, walk_progress_t *wp,
                              bp_uint32 *checksum)
{
    bp_uint64 remaining = h->original_size;
    bp_uint64 packed = BP_UINT64_ZERO;
    bp_uint32 crc = 0;
    bp_uint32 bsize = h->block_size;
    bp_uint32 alloc_in = 0;
    bp_uint8 *in_buf = NULL, *dec_buf = NULL;
    fluff_ctx_t *dfctx = NULL;
    bp_uint8 trl[V7_ENTRY_TRL];
    int rc = BP_OK;

    if (decode && !bp_u64_is_zero(remaining)) {
        if (!bp_method_is_supported(h->method)) return BP_ERR_UNSUPPORTED_METHOD;
        alloc_in = bp_method_compress_bound(h->method, bsize);
        if (alloc_in < bsize) alloc_in = bsize;
        in_buf = (bp_uint8 *)malloc((size_t)alloc_in);
        dec_buf = (bp_uint8 *)malloc((size_t)bsize);
        if (in_buf == NULL || dec_buf == NULL) { rc = BP_ERR_NOMEM; goto done; }
        if (h->method == BP_METHOD_FLUFF) {
            dfctx = fluff_ctx_alloc();
            if (dfctx == NULL) { rc = BP_ERR_NOMEM; goto done; }
            fluff_ctx_reset_decode(dfctx);
        }
    }

    while (!bp_u64_is_zero(remaining)) {
        bp_uint8 bh[V7_BLOCK_HDR];
        bp_uint32 comp, orig;
        int first_in_volume, last_in_volume;

        rc = vr_begin(vr);
        if (rc != BP_OK) goto done;
        first_in_volume = vr_at_body_start(vr);
        rc = vr_read(vr, bh, V7_BLOCK_HDR);
        if (rc != BP_OK) goto done;
        comp = bp_read_le32(bh);
        orig = bp_read_le32(bh + 4);
        if (comp == 0 || orig == 0 || comp > orig || orig > bsize ||
            bp_u64_cmp(bp_u64_from_u32(orig), remaining) > 0 ||
            !vr_avail(vr, comp)) {
            rc = BP_ERR_CORRUPT; goto done;
        }
        if (decode) {
            rc = vr_read(vr, in_buf, comp);
            if (rc != BP_OK) goto done;
        } else {
            rc = vr_skip(vr, comp);
            if (rc != BP_OK) goto done;
        }
        last_in_volume = bp_u64_cmp(vr->pos, vr->body_end) >= 0;
        packed = u64_add32(packed, V7_BLOCK_HDR + comp);
        remaining = bp_u64_sub(remaining, bp_u64_from_u32(orig));
        if (orig < bsize && !bp_u64_is_zero(remaining) &&
            !first_in_volume && !last_in_volume) {
            rc = BP_ERR_CORRUPT; goto done;
        }
        if (!decode) continue;

        if (bp_read_le32(bh + 8) != bp_crc32(0, in_buf, comp)) {
            rc = BP_ERR_CHECKSUM; goto done;
        }
        if (comp == orig) {
            memcpy(dec_buf, in_buf, (size_t)orig);
            if (dfctx != NULL) fluff_ctx_feed_raw(dfctx, dec_buf, orig);
        } else if (h->method == BP_METHOD_BOLTLITE) {
            rc = boltlite_decompress(in_buf, comp, dec_buf, orig, NULL);
        } else if (h->method == BP_METHOD_BOLTLZ) {
            bp_uint32 dl = orig;
            rc = bp_decompress_block(in_buf, comp, dec_buf, &dl);
            if (rc == BP_OK && dl != orig) rc = BP_ERR_CORRUPT;
        } else if (h->method == BP_METHOD_FLUFF) {
            bp_uint32 dl = orig;
            rc = fluff_decompress_block_ctx(in_buf, comp, dec_buf, &dl, dfctx);
            if (rc == BP_OK && dl != orig) rc = BP_ERR_CORRUPT;
        } else {
            rc = BP_ERR_CORRUPT; /* a stored entry has raw blocks only */
        }
        if (rc != BP_OK) goto done;
        crc = bp_crc32(crc, dec_buf, orig);
        if (out != NULL) {
            rc = io_write(out, dec_buf, orig);
            if (rc != BP_OK) goto done;
        }
        if (wp != NULL && wp->fn != NULL) {
            bp_progress_t p;
            bp_uint64 done_bytes = bp_u64_sub(h->original_size, remaining);
            memset(&p, 0, sizeof(p));
            p.file_index = wp->index;
            p.file_count = wp->count;
            p.file_name = h->name;
            p.file_orig_size = h->original_size;
            p.file_bytes_done = done_bytes;
            p.archive_orig_total = wp->archive_total;
            p.archive_orig_done = bp_u64_add(wp->archive_done, done_bytes);
            if (wp->fn(&p, wp->user) != 0) { rc = BP_ERR_ABORTED; goto done; }
        }
    }

    rc = vr_begin(vr);
    if (rc != BP_OK) goto done;
    rc = vr_read(vr, trl, V7_ENTRY_TRL);
    if (rc != BP_OK) goto done;
    if (bp_read_le32(trl + 12) != bp_crc32(0, trl, 12) ||
        !u64_eq(bp_read_le64(trl), packed) ||
        !u64_eq(packed, h->compressed_size) ||
        bp_read_le32(trl + 8) != h->checksum) {
        rc = BP_ERR_CORRUPT; goto done;
    }
    if (decode) {
        *checksum = crc;
        if (crc != h->checksum) rc = BP_ERR_CHECKSUM;
    }

done:
    if (dfctx != NULL) fluff_ctx_free(dfctx);
    free(in_buf);
    free(dec_buf);
    return rc;
}

/* After the last entry, the table must follow. */
static int vr_check_table_follows(vreader_t *vr)
{
    bp_uint16 v;
    bp_uint64 o;
    vr_next_location(vr, &v, &o);
    if (v != vr->table_volume || !u64_eq(o, vr->table_offset))
        return BP_ERR_CORRUPT;
    return BP_OK;
}

/* Framing walk over every entry (no decoding).  block_sizes, when not
 * NULL, receives each entry's block size. */
static int vr_verify_framing(vreader_t *vr, bp_uint32 *block_sizes)
{
    bp_uint32 i;
    int rc;

    if (vr->entry_count == 0) return BP_OK;
    rc = vr_seek(vr, 0, bp_u64_from_u32(V7_VOL_HDR));
    if (rc != BP_OK) return rc;
    for (i = 0; i < vr->entry_count; i++) {
        bp_entry_t h;
        rc = vr_read_entry_header(vr, &vr->entries[i], &h);
        if (rc != BP_OK) return rc;
        if (block_sizes != NULL) block_sizes[i] = h.block_size;
        rc = vr_read_entry_data(vr, &h, 0, NULL, NULL, NULL);
        free(h.name);
        if (rc != BP_OK) return rc;
    }
    return vr_check_table_follows(vr);
}

/* ---- single-stream source ---------------------------------------------- */

static bp_io_t *single_open(void *user, bp_uint16 number, int reason)
{
    (void)number;
    if (reason != BP_OK) return NULL;
    return (bp_io_t *)user;
}

static void single_close(void *user, bp_io_t *io)
{
    (void)user;
    (void)io;
}

static void single_source(bp_volume_source_t *src, bp_io_t *io)
{
    src->open = single_open;
    src->close = single_close;
    src->user = io;
}

/* ---- listing ----------------------------------------------------------- */

int bp_archive_list_volumes(const bp_volume_source_t *src, bp_list_fn fn,
                            void *user_data)
{
    vreader_t vr;
    bp_uint32 i;
    int rc;

    if (src == NULL || src->open == NULL || fn == NULL) return BP_ERR_INVALID_ARG;
    vr_init(&vr, src);
    rc = vr_open_table(&vr);
    vr_close_io(&vr);
    for (i = 0; rc == BP_OK && i < vr.entry_count; i++)
        if (fn(&vr.entries[i], user_data) != 0) rc = BP_ERR_ABORTED;
    vr_free(&vr);
    return rc;
}

int bp_archive_list(bp_io_t *io, bp_list_fn fn, void *user_data)
{
    bp_volume_source_t src;
    if (io == NULL) return BP_ERR_INVALID_ARG;
    single_source(&src, io);
    return bp_archive_list_volumes(&src, fn, user_data);
}

/* ---- info -------------------------------------------------------------- */

int bp_archive_info_volumes(const bp_volume_source_t *src,
                            bp_archive_info_t *info,
                            bp_uint32 *sidecar_count)
{
    vreader_t vr;
    bp_uint32 i, sidecars = 0;
    int rc;

    if (sidecar_count != NULL) *sidecar_count = 0;
    if (src == NULL || src->open == NULL || info == NULL)
        return BP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    vr_init(&vr, src);
    rc = vr_open_table(&vr);
    vr_close_io(&vr);
    if (rc == BP_OK) {
        info->version = BP_FORMAT7_VERSION;
        info->file_count = vr.entry_count;
        info->creator_os = vr.creator_os;
        info->creator_cpu = vr.creator_cpu;
        info->creator_ver_major = vr.ver_major;
        info->creator_ver_minor = vr.ver_minor;
        info->archive_size = vr.last_size;
        info->volume_count = vr.count;
        info->set_id = vr.set_id;
        for (i = 0; i < vr.entry_count; i++) {
            const bp_entry_t *e = &vr.entries[i];
            if (e->entry_type == BP_TYPE_META || e->entry_type == BP_TYPE_RSRC)
                sidecars++;
            if (e->entry_type == BP_TYPE_DIR) info->dir_count++;
            else if (e->method < 8) info->method_counts[e->method]++;
            info->entry_count++;
            info->total_original = bp_u64_add(info->total_original,
                                              e->original_size);
            info->total_compressed = bp_u64_add(info->total_compressed,
                                                e->compressed_size);
        }
        if (sidecar_count != NULL) *sidecar_count = sidecars;
    }
    vr_free(&vr);
    return rc;
}

int bp_archive_info(bp_io_t *io, bp_archive_info_t *info)
{
    return bp_archive_info_ex(io, info, NULL);
}

int bp_archive_info_ex(bp_io_t *io, bp_archive_info_t *info,
                       bp_uint32 *sidecar_count)
{
    bp_volume_source_t src;
    if (io == NULL) return BP_ERR_INVALID_ARG;
    single_source(&src, io);
    return bp_archive_info_volumes(&src, info, sidecar_count);
}

/* ---- extraction -------------------------------------------------------- */

static int extract_impl(const bp_volume_source_t *src, bp_extract_fn open_fn,
                        bp_extract_done_fn done_fn,
                        bp_extract_done_status_fn status_fn,
                        bp_progress_fn progress_fn, void *user_data)
{
    vreader_t vr;
    walk_progress_t wp;
    bp_uint32 i;
    int rc;
    int partial = 0;
    int subject_extracted = 0;
    bp_uint8 subject_type = 0xFF;

    if (src == NULL || src->open == NULL || open_fn == NULL)
        return BP_ERR_INVALID_ARG;
    vr_init(&vr, src);
    rc = vr_open_table(&vr);
    /* A single volume is checked in full before anything is written; a
     * multi-volume set is read once, volume by volume (spec 2.0 §5.4). */
    if (rc == BP_OK && vr.count == 1) rc = vr_verify_framing(&vr, NULL);
    if (rc == BP_OK && vr.entry_count != 0)
        rc = vr_seek(&vr, 0, bp_u64_from_u32(V7_VOL_HDR));
    if (rc != BP_OK) { vr_free(&vr); return rc; }

    memset(&wp, 0, sizeof(wp));
    wp.fn = progress_fn;
    wp.user = user_data;
    wp.count = vr.entry_count;
    wp.archive_total = vr.total_orig;

    for (i = 0; i < vr.entry_count; i++) {
        bp_entry_t entry;
        bp_io_t *out;
        bp_uint32 checksum = 0;
        int portable;
        bp_metadata_sink_t metadata;
        bp_portable_metadata_t parsed;

        rc = vr_read_entry_header(&vr, &vr.entries[i], &entry);
        if (rc != BP_OK) break;
        if (entry.entry_type == BP_TYPE_DATA || entry.entry_type == BP_TYPE_DIR ||
            entry.entry_type == BP_TYPE_SYMLINK) {
            subject_type = entry.entry_type;
            subject_extracted = entry.entry_type == BP_TYPE_DIR;
        }
        portable = entry.entry_type == BP_TYPE_META &&
                   entry.platform == BP_META_PORTABLE;
        if (portable && (entry.original_size.hi != 0 ||
                         entry.original_size.lo > BP_META_MAX_SIZE)) {
            free(entry.name); rc = BP_ERR_CORRUPT; break;
        }
        if (portable) bp_metadata_sink_init(&metadata);
        if (portable) {
            out = NULL;
        } else if ((entry.entry_type == BP_TYPE_RSRC ||
                    entry.entry_type == BP_TYPE_META) && !subject_extracted) {
            out = NULL;
            partial = 1;
        } else {
            out = open_fn(&entry, user_data);
        }
        if (entry.entry_type == BP_TYPE_DATA || entry.entry_type == BP_TYPE_SYMLINK)
            subject_extracted = out != NULL;

        wp.index = i;
        rc = vr_read_entry_data(&vr, &entry, 1, portable ? &metadata.io : out,
                                &wp, &checksum);
        if (rc != BP_OK) {
            if (status_fn && out && out != bp_archive_dry_run_stream())
                (void)status_fn(&entry, out, rc, user_data);
            else if (done_fn && out && out != bp_archive_dry_run_stream() &&
                     rc == BP_ERR_CHECKSUM)
                done_fn(&entry, out, user_data);
            free(entry.name);
            break;
        }
        if (portable) {
            int prc = bp_metadata_parse(metadata.data, metadata.length, &parsed);
            if (prc == BP_ERR_CORRUPT ||
                (parsed.supported_version && parsed.subject_type != subject_type)) {
                free(entry.name); rc = BP_ERR_CORRUPT; break;
            }
            if (prc != BP_OK) partial = 1;
            if (parsed.supported_version && subject_extracted)
                out = open_fn(&entry, user_data);
            else if (parsed.supported_version)
                partial = 1;
            if (out == NULL) partial = 1;
            else if (io_write(out, metadata.data, metadata.length) != BP_OK) {
                if (status_fn && out != bp_archive_dry_run_stream())
                    (void)status_fn(&entry, out, BP_ERR_IO, user_data);
                else if (done_fn && out != bp_archive_dry_run_stream())
                    done_fn(&entry, out, user_data);
                free(entry.name); rc = BP_ERR_IO; break;
            }
        } else if (out == NULL && entry.entry_type != BP_TYPE_DIR) {
            partial = 1;
        }
        if (status_fn && out && out != bp_archive_dry_run_stream()) {
            int src_rc = status_fn(&entry, out, BP_OK, user_data);
            if (src_rc == BP_ERR_PARTIAL) partial = 1;
            else if (src_rc != BP_OK) { free(entry.name); rc = src_rc; break; }
        } else if (done_fn && out && out != bp_archive_dry_run_stream()) {
            done_fn(&entry, out, user_data);
        }
        wp.archive_done = bp_u64_add(wp.archive_done, entry.original_size);
        free(entry.name);
    }
    if (rc == BP_OK && vr.entry_count != 0) rc = vr_check_table_follows(&vr);
    vr_free(&vr);
    if (rc != BP_OK) return rc;
    return partial ? BP_ERR_PARTIAL : BP_OK;
}

int bp_archive_extract_volumes(const bp_volume_source_t *src,
                               bp_extract_fn open_fn,
                               bp_extract_done_status_fn done_fn,
                               bp_progress_fn progress_fn, void *user_data)
{
    if (done_fn == NULL) return BP_ERR_INVALID_ARG;
    return extract_impl(src, open_fn, NULL, done_fn, progress_fn, user_data);
}

int bp_archive_extract_all(bp_io_t *io, bp_extract_fn open_fn,
                           bp_extract_done_fn done_fn,
                           bp_progress_fn progress_fn, void *user_data)
{
    bp_volume_source_t src;
    if (io == NULL) return BP_ERR_INVALID_ARG;
    single_source(&src, io);
    return extract_impl(&src, open_fn, done_fn, NULL, progress_fn, user_data);
}

int bp_archive_extract_all_ex(bp_io_t *io, bp_extract_fn open_fn,
                              bp_extract_done_status_fn done_fn,
                              bp_progress_fn progress_fn, void *user_data)
{
    bp_volume_source_t src;
    if (io == NULL || done_fn == NULL) return BP_ERR_INVALID_ARG;
    single_source(&src, io);
    return extract_impl(&src, open_fn, NULL, done_fn, progress_fn, user_data);
}

/* ---- test -------------------------------------------------------------- */

int bp_archive_test_volumes(const bp_volume_source_t *src, bp_test_fn fn,
                            void *user_data, bp_progress_fn progress_fn,
                            void *progress_data)
{
    vreader_t vr;
    walk_progress_t wp;
    bp_uint32 i;
    bp_uint8 subject_type = 0xFF;
    int rc;

    if (src == NULL || src->open == NULL) return BP_ERR_INVALID_ARG;
    vr_init(&vr, src);
    rc = vr_open_table(&vr);
    if (rc == BP_OK && vr.entry_count != 0)
        rc = vr_seek(&vr, 0, bp_u64_from_u32(V7_VOL_HDR));
    if (rc != BP_OK) { vr_free(&vr); return rc; }

    memset(&wp, 0, sizeof(wp));
    wp.fn = progress_fn;
    wp.user = progress_data;
    wp.count = vr.entry_count;
    wp.archive_total = vr.total_orig;

    for (i = 0; i < vr.entry_count; i++) {
        bp_entry_t entry;
        bp_uint32 checksum = 0;
        int entry_rc;
        int portable;
        bp_metadata_sink_t metadata;
        bp_portable_metadata_t parsed;

        rc = vr_read_entry_header(&vr, &vr.entries[i], &entry);
        if (rc != BP_OK) break;
        if (entry.entry_type == BP_TYPE_DATA || entry.entry_type == BP_TYPE_DIR ||
            entry.entry_type == BP_TYPE_SYMLINK)
            subject_type = entry.entry_type;
        if (progress_fn) {
            bp_progress_t p;
            memset(&p, 0, sizeof(p));
            p.file_index = i;
            p.file_count = vr.entry_count;
            p.file_name = entry.name;
            p.file_orig_size = entry.original_size;
            p.archive_orig_total = vr.total_orig;
            p.archive_orig_done = wp.archive_done;
            if (progress_fn(&p, progress_data) != 0) {
                free(entry.name); rc = BP_ERR_ABORTED; break;
            }
        }
        portable = entry.entry_type == BP_TYPE_META &&
                   entry.platform == BP_META_PORTABLE;
        if (portable && (entry.original_size.hi != 0 ||
                         entry.original_size.lo > BP_META_MAX_SIZE))
            entry_rc = BP_ERR_CORRUPT;
        else {
            if (portable) bp_metadata_sink_init(&metadata);
            wp.index = i;
            entry_rc = vr_read_entry_data(&vr, &entry, 1,
                                          portable ? &metadata.io : NULL,
                                          &wp, &checksum);
        }
        if (entry_rc == BP_OK && portable) {
            int prc = bp_metadata_parse(metadata.data, metadata.length, &parsed);
            if (prc == BP_ERR_CORRUPT) entry_rc = prc;
            else if (parsed.supported_version && parsed.subject_type != subject_type)
                entry_rc = BP_ERR_CORRUPT;
            else if (prc != BP_OK) entry_rc = BP_ERR_PARTIAL;
        }
        if (fn) fn(&entry, entry_rc, user_data);
        wp.archive_done = bp_u64_add(wp.archive_done, entry.original_size);
        free(entry.name);
        if (entry_rc != BP_OK) { rc = entry_rc; break; }
    }
    if (rc == BP_OK && vr.entry_count != 0) rc = vr_check_table_follows(&vr);
    vr_free(&vr);
    return rc;
}

int bp_archive_test_progress(bp_io_t *io, bp_test_fn fn, void *user_data,
                             bp_progress_fn progress_fn,
                             void *progress_data)
{
    bp_volume_source_t src;
    if (io == NULL) return BP_ERR_INVALID_ARG;
    single_source(&src, io);
    return bp_archive_test_volumes(&src, fn, user_data, progress_fn,
                                   progress_data);
}

int bp_archive_test(bp_io_t *io, bp_test_fn fn, void *user_data)
{
    return bp_archive_test_progress(io, fn, user_data, NULL, NULL);
}

/* ---- in-place append (single-volume archives) -------------------------- */

int bp_archive_open_append(bp_archive_t *ar, bp_io_t *io,
                           bp_uint32 block_size, bp_uint8 method)
{
    bp_volume_source_t src;
    vreader_t vr;
    bp_uint32 *bsizes = NULL;
    bp_uint32 i;
    v7w_t *w;
    int rc;

    if (ar == NULL || io == NULL) return BP_ERR_INVALID_ARG;
    rc = v7_init(ar, block_size, method);
    if (rc != BP_OK) return rc;
    ar->append_mode = 1;
    w = W(ar);

    single_source(&src, io);
    vr_init(&vr, &src);
    rc = vr_open_table(&vr);
    if (rc == BP_OK && vr.count != 1) rc = BP_ERR_VOLUME;
    if (rc == BP_OK && vr.entry_count != 0) {
        bsizes = (bp_uint32 *)calloc((size_t)vr.entry_count, sizeof(bp_uint32));
        if (bsizes == NULL) rc = BP_ERR_NOMEM;
    }
    if (rc == BP_OK) rc = vr_verify_framing(&vr, bsizes);
    if (rc != BP_OK) goto fail;

    for (i = 0; i < vr.entry_count; i++) {
        rc = entry_list_grow(ar);
        if (rc != BP_OK) goto fail;
        ar->entry_list[ar->entries_written] = vr.entries[i];
        ar->entry_list[ar->entries_written].block_size = bsizes[i];
        vr.entries[i].name = NULL;   /* ownership moved */
        ar->entries_written++;
        ar->archive_orig_total = bp_u64_add(ar->archive_orig_total,
                                            vr.entries[i].original_size);
        ar->archive_comp_total = bp_u64_add(ar->archive_comp_total,
                                            vr.entries[i].compressed_size);
        ar->archive_orig_done = ar->archive_orig_total;
    }
    ar->creator_os = vr.creator_os;
    ar->creator_cpu = vr.creator_cpu;
    w->set_id = vr.set_id;
    w->io = io;
    ar->io = io;
    w->number = 0;
    w->pos = vr.table_offset;
    w->vol_open = 1;
    rc = io_seek_u64(io, vr.table_offset);
    if (rc != BP_OK) goto fail;
    free(bsizes);
    vr_free(&vr);
    return BP_OK;

fail:
    free(bsizes);
    vr_free(&vr);
    bp_archive_close(ar);
    return rc;
}

int bp_archive_use_profile(bp_archive_t *ar, bp_uint8 profile)
{
    bp_uint32 i;
    int rc;
    v7w_t *w;

    if (ar == NULL || ar->v7 == NULL) return BP_ERR_INVALID_ARG;
    w = W(ar);
    if (profile != BP_PROFILE_INTERCHANGE_1 || ar->profile != BP_PROFILE_NONE)
        return BP_ERR_INVALID_ARG;
    if (ar->entries_written != 0 && !ar->append_mode) return BP_ERR_INVALID_ARG;
    if (ar->entries_written > BP_PROFILE1_MAX_ENTRIES) return BP_ERR_OVERFLOW;
    if (vw_limited(w) &&
        (w->limit.hi != 0 || w->limit.lo > BP_PROFILE1_MAX_ARCHIVE))
        return BP_ERR_OVERFLOW;
    for (i = 0; i < ar->entries_written; i++) {
        const bp_entry_t *e = &ar->entry_list[i];
        rc = profile_check_entry_values(BP_PROFILE_INTERCHANGE_1, e->method,
                                        e->block_size, e->entry_type,
                                        e->original_size);
        if (rc != BP_OK) return rc;
    }
    ar->profile_table_bytes = bp_u64_from_u32(0);
    for (i = 0; i < ar->entries_written; i++)
        ar->profile_table_bytes = u64_add32(ar->profile_table_bytes,
            table_record_size(&ar->entry_list[i]));
    ar->profile = profile;
    rc = profile_check_extra(ar, 0, 0);
    if (rc != BP_OK) {
        ar->profile = BP_PROFILE_NONE;
        ar->profile_table_bytes = bp_u64_from_u32(0);
        return rc;
    }
    ar->method = BP_METHOD_BOLTLITE;
    ar->method_level = 0;
    ar->block_size = BP_BLOCK_16K;
    return BP_OK;
}

int bp_archive_create_profile(bp_archive_t *ar, bp_io_t *io, bp_uint8 profile)
{
    int rc;
    if (profile != BP_PROFILE_INTERCHANGE_1) return BP_ERR_INVALID_ARG;
    rc = bp_archive_create(ar, io, BP_BLOCK_16K, BP_METHOD_BOLTLITE);
    if (rc != BP_OK) return rc;
    rc = bp_archive_use_profile(ar, profile);
    if (rc != BP_OK) bp_archive_close(ar);
    return rc;
}

int bp_archive_query_tail(bp_io_t *io, bp_uint64 *tail_off, bp_uint64 *tail_size)
{
    bp_volume_source_t src;
    vreader_t vr;
    int rc;

    if (io == NULL || tail_off == NULL || tail_size == NULL)
        return BP_ERR_INVALID_ARG;
    single_source(&src, io);
    vr_init(&vr, &src);
    rc = vr_open_set(&vr);
    if (rc == BP_OK && vr.count != 1) rc = BP_ERR_VOLUME;
    if (rc == BP_OK) {
        *tail_off = vr.table_offset;
        *tail_size = bp_u64_sub(vr.last_size, vr.table_offset);
    }
    vr_free(&vr);
    return rc;
}

/* ---- copying whole entries --------------------------------------------- */

/* Decoder state for re-splitting copied blocks at volume boundaries. */
typedef struct {
    bp_uint8     method;
    fluff_ctx_t *fctx;
    bp_uint8    *dec;
    bp_uint32    dec_size;
} redecode_t;

static void redecode_free(redecode_t *d)
{
    if (d->fctx != NULL) fluff_ctx_free(d->fctx);
    free(d->dec);
    memset(d, 0, sizeof(*d));
}

static int redecode_begin(redecode_t *d, bp_uint8 method, bp_uint32 bsize)
{
    redecode_free(d);
    d->method = method;
    if (bsize == 0) return BP_OK;
    d->dec = (bp_uint8 *)malloc((size_t)bsize);
    if (d->dec == NULL) return BP_ERR_NOMEM;
    d->dec_size = bsize;
    if (method == BP_METHOD_FLUFF) {
        d->fctx = fluff_ctx_alloc();
        if (d->fctx == NULL) return BP_ERR_NOMEM;
        fluff_ctx_reset_decode(d->fctx);
    }
    return BP_OK;
}

static int redecode_block(redecode_t *d, const bp_uint8 *comp, bp_uint32 clen,
                          bp_uint32 olen)
{
    bp_uint32 dl = olen;
    int rc = BP_OK;
    if (olen > d->dec_size) return BP_ERR_CORRUPT;
    if (clen == olen) {
        memcpy(d->dec, comp, (size_t)olen);
        if (d->fctx != NULL) fluff_ctx_feed_raw(d->fctx, d->dec, olen);
        return BP_OK;
    }
    if (d->method == BP_METHOD_BOLTLITE)
        rc = boltlite_decompress(comp, clen, d->dec, olen, NULL);
    else if (d->method == BP_METHOD_BOLTLZ)
        rc = bp_decompress_block(comp, clen, d->dec, &dl);
    else if (d->method == BP_METHOD_FLUFF)
        rc = fluff_decompress_block_ctx(comp, clen, d->dec, &dl, d->fctx);
    else
        rc = BP_ERR_CORRUPT;
    if (rc == BP_OK && dl != olen) rc = BP_ERR_CORRUPT;
    return rc;
}

/* Copy one serialized entry (header, blocks, trailer) from a volume reader
 * (vr != NULL) or a plain blob stream into the archive, placing each
 * structure by the volume rules.  When the archive has a volume limit,
 * blocks are decoded as they pass so that a block at a volume boundary can be
 * split; the trailer is then rebuilt with the new compressed size, which is
 * returned in *new_comp. */
static int copy_entry_structures(bp_archive_t *ar, bp_io_t *in,
                                 vreader_t *vr, const bp_entry_t *t,
                                 bp_uint16 *vol, bp_uint64 *off,
                                 bp_uint64 *new_comp)
{
    bp_uint8 pre[4];
    bp_uint8 *hdr = NULL;
    bp_uint8 *blk = NULL;
    bp_uint32 hs, blk_cap = 0, bsize;
    bp_uint64 remaining, old_comp = BP_UINT64_ZERO, comp_total = BP_UINT64_ZERO;
    bp_uint8 trl[V7_ENTRY_TRL];
    redecode_t rd;
    int split = vw_limited(W(ar)) && W(ar)->multi;
    int rc;

    memset(&rd, 0, sizeof(rd));
    if (vr != NULL) {
        rc = vr_begin(vr);
        if (rc == BP_OK && (vr->cur != (int)t->volume || !u64_eq(vr->pos, t->offset)))
            rc = BP_ERR_CORRUPT;
        if (rc == BP_OK) rc = vr_read(vr, pre, 4);
    } else {
        rc = io_read(in, pre, 4);
    }
    if (rc != BP_OK) return rc;
    hs = bp_read_le16(pre);
    if (hs != t->header_size || hs != V7_ENTRY_FIXED + (bp_uint32)t->name_len)
        return BP_ERR_CORRUPT;
    hdr = (bp_uint8 *)malloc((size_t)hs);
    if (hdr == NULL) return BP_ERR_NOMEM;
    memcpy(hdr, pre, 4);
    rc = vr != NULL ? vr_read(vr, hdr + 4, hs - 4) : io_read(in, hdr + 4, hs - 4);
    if (rc != BP_OK) goto done;
    if (bp_read_le32(hdr + hs - 4) != bp_crc32(0, hdr, hs - 4)) {
        rc = BP_ERR_CORRUPT; goto done;
    }
    bsize = bp_read_le32(hdr + 14 + t->name_len);
    rc = profile_check_extra(ar, hs, t->name_len);
    if (rc == BP_OK) rc = vw_put_struct(ar, hdr, hs, vol, off);
    if (rc != BP_OK) goto done;
    if (split && !bp_u64_is_zero(t->original_size)) {
        rc = redecode_begin(&rd, t->method, bsize);
        if (rc != BP_OK) goto done;
    }

    remaining = t->original_size;
    while (!bp_u64_is_zero(remaining)) {
        bp_uint8 bh[V7_BLOCK_HDR];
        bp_uint32 comp, orig;

        if (vr != NULL) {
            rc = vr_begin(vr);
            if (rc == BP_OK) rc = vr_read(vr, bh, V7_BLOCK_HDR);
        } else {
            rc = io_read(in, bh, V7_BLOCK_HDR);
        }
        if (rc != BP_OK) goto done;
        comp = bp_read_le32(bh);
        orig = bp_read_le32(bh + 4);
        if (comp == 0 || orig == 0 || comp > orig || orig > bsize ||
            bp_u64_cmp(bp_u64_from_u32(orig), remaining) > 0) {
            rc = BP_ERR_CORRUPT; goto done;
        }
        if (comp > blk_cap) {
            free(blk);
            blk = (bp_uint8 *)malloc((size_t)comp);
            if (blk == NULL) { rc = BP_ERR_NOMEM; goto done; }
            blk_cap = comp;
        }
        rc = vr != NULL ? vr_read(vr, blk, comp) : io_read(in, blk, comp);
        if (rc != BP_OK) goto done;
        if (bp_read_le32(bh + 8) != bp_crc32(0, blk, comp)) {
            rc = BP_ERR_CHECKSUM; goto done;
        }
        old_comp = u64_add32(old_comp, V7_BLOCK_HDR + comp);
        if (split) {
            rc = redecode_block(&rd, blk, comp, orig);
            if (rc != BP_OK) goto done;
        }
        rc = profile_check_extra(ar, V7_BLOCK_HDR + comp, t->name_len);
        if (rc == BP_OK)
            rc = vw_put_block(ar, t->method, split ? rd.dec : NULL, orig,
                              blk, comp, &comp_total);
        if (rc != BP_OK) goto done;
        remaining = bp_u64_sub(remaining, bp_u64_from_u32(orig));
    }

    if (vr != NULL) {
        rc = vr_begin(vr);
        if (rc == BP_OK) rc = vr_read(vr, trl, V7_ENTRY_TRL);
    } else {
        rc = io_read(in, trl, V7_ENTRY_TRL);
    }
    if (rc != BP_OK) goto done;
    if (bp_read_le32(trl + 12) != bp_crc32(0, trl, 12) ||
        !u64_eq(bp_read_le64(trl), old_comp) ||
        !u64_eq(old_comp, t->compressed_size) ||
        bp_read_le32(trl + 8) != t->checksum) {
        rc = BP_ERR_CORRUPT; goto done;
    }
    bp_write_le64(trl, comp_total);
    bp_write_le32(trl + 12, bp_crc32(0, trl, 12));
    rc = profile_check_extra(ar, V7_ENTRY_TRL, t->name_len);
    if (rc == BP_OK) rc = vw_put_struct(ar, trl, V7_ENTRY_TRL, NULL, NULL);
    if (rc == BP_OK) *new_comp = comp_total;
done:
    redecode_free(&rd);
    free(hdr);
    free(blk);
    return rc;
}

static int record_copied(bp_archive_t *ar, const bp_entry_t *t, bp_uint16 vol,
                         bp_uint64 off, bp_uint64 comp, bp_uint32 block_size)
{
    bp_packed_entry_t p;
    memset(&p, 0, sizeof(p));
    p.name = t->name;
    p.name_len = t->name_len;
    p.header_size = t->header_size;
    p.original_size = t->original_size;
    p.compressed_size = comp;
    p.checksum = t->checksum;
    p.method = t->method;
    p.checksum_type = BP_CKSUM_CRC32;
    p.block_size = block_size;
    p.entry_type = t->entry_type;
    p.platform = t->platform;
    p.mtime = t->mtime;
    p.flags = t->flags;
    return record_entry_at(ar, vol, off, &p);
}

int bp_archive_commit_packed_entry(bp_archive_t *ar, bp_io_t *src_io,
                                   const bp_packed_entry_t *packed)
{
    bp_entry_t t;
    bp_uint16 vol = 0;
    bp_uint64 off = BP_UINT64_ZERO, comp = BP_UINT64_ZERO;
    int rc;

    if (ar == NULL || ar->v7 == NULL || src_io == NULL || packed == NULL ||
        packed->name == NULL || W(ar)->finalized)
        return BP_ERR_INVALID_ARG;
    if (ar->profile != BP_PROFILE_NONE) {
        rc = profile_check_entry_values(ar->profile, packed->method,
                                        packed->block_size, packed->entry_type,
                                        packed->original_size);
        if (rc != BP_OK) return rc;
    }
    if (src_io->seek(src_io->handle, 0, 0) != 0) return BP_ERR_IO;
    memset(&t, 0, sizeof(t));
    t.name = packed->name;
    t.name_len = packed->name_len;
    t.header_size = packed->header_size;
    t.original_size = packed->original_size;
    t.compressed_size = packed->compressed_size;
    t.checksum = packed->checksum;
    t.method = packed->method;
    t.entry_type = packed->entry_type;
    rc = copy_entry_structures(ar, src_io, NULL, &t, &vol, &off, &comp);
    if (rc != BP_OK) return rc;
    {
        bp_packed_entry_t p = *packed;
        p.compressed_size = comp;
        return record_entry_at(ar, vol, off, &p);
    }
}

int bp_archive_copy_from(bp_archive_t *ar, bp_io_t *src_io)
{
    bp_volume_source_t src;
    vreader_t vr;
    bp_uint32 *bsizes = NULL;
    bp_uint32 i;
    int rc;

    if (ar == NULL || ar->v7 == NULL || src_io == NULL) return BP_ERR_INVALID_ARG;
    single_source(&src, src_io);
    vr_init(&vr, &src);
    rc = vr_open_table(&vr);
    if (rc == BP_OK && vr.entry_count != 0) {
        bsizes = (bp_uint32 *)calloc((size_t)vr.entry_count, sizeof(bp_uint32));
        if (bsizes == NULL) rc = BP_ERR_NOMEM;
    }
    if (rc == BP_OK) rc = vr_verify_framing(&vr, bsizes);
    if (rc == BP_OK && ar->profile != BP_PROFILE_NONE) {
        if (ar->entries_written > BP_PROFILE1_MAX_ENTRIES ||
            vr.entry_count > BP_PROFILE1_MAX_ENTRIES - ar->entries_written)
            rc = BP_ERR_OVERFLOW;
        for (i = 0; rc == BP_OK && i < vr.entry_count; i++)
            rc = profile_check_entry_values(ar->profile, vr.entries[i].method,
                                            bsizes[i], vr.entries[i].entry_type,
                                            vr.entries[i].original_size);
    }
    if (rc == BP_OK && vr.entry_count != 0)
        rc = vr_seek(&vr, 0, bp_u64_from_u32(V7_VOL_HDR));
    for (i = 0; rc == BP_OK && i < vr.entry_count; i++) {
        bp_uint16 vol = 0;
        bp_uint64 off = BP_UINT64_ZERO, comp = BP_UINT64_ZERO;
        rc = copy_entry_structures(ar, NULL, &vr, &vr.entries[i], &vol, &off,
                                   &comp);
        if (rc == BP_OK)
            rc = record_copied(ar, &vr.entries[i], vol, off, comp, bsizes[i]);
    }
    free(bsizes);
    vr_free(&vr);
    return rc;
}

/* ---- deletion ---------------------------------------------------------- */

static int name_in_delete_set(const char *name, bp_uint16 name_len,
                              const char **del_names, bp_uint32 del_count)
{
    bp_uint32 i;
    for (i = 0; i < del_count; i++) {
        size_t dlen = strlen(del_names[i]);
        if (dlen == 0) continue;
        if (dlen == (size_t)name_len && memcmp(name, del_names[i], dlen) == 0)
            return 1;
        if (del_names[i][dlen - 1] == '/' && (size_t)name_len >= dlen &&
            memcmp(name, del_names[i], dlen) == 0)
            return 1;
    }
    return 0;
}

int bp_archive_delete(bp_io_t *src_io, bp_io_t *dst_io,
                      const char **del_names, bp_uint32 del_count,
                      bp_uint32 *out_deleted)
{
    bp_volume_source_t src;
    vreader_t vr;
    bp_archive_t dst;
    bp_uint32 *bsizes = NULL;
    bp_uint32 i, deleted = 0;
    int dst_open = 0;
    int rc;

    if (src_io == NULL || dst_io == NULL || del_names == NULL)
        return BP_ERR_INVALID_ARG;
    single_source(&src, src_io);
    vr_init(&vr, &src);
    rc = vr_open_table(&vr);
    if (rc == BP_OK && vr.count != 1) rc = BP_ERR_VOLUME;
    if (rc == BP_OK && vr.entry_count != 0) {
        bsizes = (bp_uint32 *)calloc((size_t)vr.entry_count, sizeof(bp_uint32));
        if (bsizes == NULL) rc = BP_ERR_NOMEM;
    }
    if (rc == BP_OK) rc = vr_verify_framing(&vr, bsizes);
    if (rc == BP_OK) {
        rc = bp_archive_create(&dst, dst_io, BP_BLOCK_DEFAULT, BP_METHOD_STORED);
        dst_open = rc == BP_OK;
    }
    if (rc == BP_OK) {
        /* Keep the creator; the edited archive gets a new set ID.  The
         * header already written names this build, so rewrite it. */
        bp_uint8 h[V7_VOL_HDR];
        dst.creator_os = vr.creator_os;
        dst.creator_cpu = vr.creator_cpu;
        v7_build_vol_header(h, W(&dst)->set_id, 0, dst.creator_os,
                            dst.creator_cpu);
        rc = io_seek_u64(dst_io, bp_u64_from_u32(0));
        if (rc == BP_OK) rc = io_write(dst_io, h, V7_VOL_HDR);
    }
    if (rc == BP_OK && vr.entry_count != 0)
        rc = vr_seek(&vr, 0, bp_u64_from_u32(V7_VOL_HDR));
    for (i = 0; rc == BP_OK && i < vr.entry_count; i++) {
        const bp_entry_t *e = &vr.entries[i];
        bp_uint16 vol = 0;
        bp_uint64 off = BP_UINT64_ZERO, comp = BP_UINT64_ZERO;

        if (name_in_delete_set(e->name, e->name_len, del_names, del_count)) {
            bp_uint64 span = bp_u64_add(
                bp_u64_from_u32((bp_uint32)e->header_size + V7_ENTRY_TRL),
                e->compressed_size);
            deleted++;
            vr.pos = bp_u64_add(vr.pos, span);
            rc = io_seek_u64(vr.io, vr.pos);
            continue;
        }
        rc = copy_entry_structures(&dst, NULL, &vr, e, &vol, &off, &comp);
        if (rc == BP_OK) rc = record_copied(&dst, e, vol, off, comp, bsizes[i]);
    }
    if (rc == BP_OK)
        rc = write_table_and_end(&dst, dst.entry_list, dst.entries_written);
    if (dst_open) bp_archive_close(&dst);
    free(bsizes);
    vr_free(&vr);
    if (rc == BP_OK && out_deleted != NULL) *out_deleted = deleted;
    return rc;
}
