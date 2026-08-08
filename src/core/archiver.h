/*
 * archiver.h -- archive format read/write interface for BoltPack
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#ifndef BP_ARCHIVER_H
#define BP_ARCHIVER_H

#include "bp_types.h"

/* Maximum file name length in bytes */
#define BP_MAX_NAME_LEN  65535

/* --- Entry info (used for listing and callbacks) ------------------------ */

typedef struct {
    char       *name;            /* UTF-8 file name (malloc'd) */
    bp_uint16   name_len;
    bp_uint64   original_size;
    bp_uint64   compressed_size;
    bp_uint32   checksum;
    bp_uint8    method;
    bp_uint8    checksum_type;
    bp_uint32   block_size;
    bp_uint8    entry_type;
    bp_uint8    platform;
    bp_uint64   mtime;
    bp_uint16   flags;
    bp_uint16   header_size;
    bp_uint64   offset;          /* byte offset in archive (set during list) */
} bp_entry_t;

/* Packed entry blob plus metadata, used by queued cross-entry compression.
 * The serialized entry bytes are written to a caller-provided bp_io_t via
 * bp_archive_pack_entry(), then committed to the real archive later with
 * bp_archive_commit_packed_entry(). */
typedef struct {
    char       *name;            /* UTF-8 file name (malloc'd) */
    bp_uint16   name_len;
    bp_uint16   header_size;
    bp_uint64   original_size;
    bp_uint64   compressed_size;
    bp_uint32   checksum;
    bp_uint8    method;
    bp_uint8    checksum_type;
    bp_uint32   block_size;
    bp_uint8    entry_type;
    bp_uint8    platform;
    bp_uint64   mtime;
    bp_uint16   flags;
    bp_uint64   serialized_size; /* header_size + compressed_size bytes */
} bp_packed_entry_t;

/* Optional diagnostic log callback.
 * Receives a pre-formatted, NUL-terminated message string (no trailing
 * newline guaranteed).  user_data is whatever was passed to
 * bp_archive_set_log.  When NULL, diagnostics are discarded (errors are
 * still reported through return codes). */
typedef void (*bp_log_fn)(const char *msg, void *user_data);

/* --- Archive context ---------------------------------------------------- */

typedef struct {
    bp_io_t        *io;
    bp_uint32       file_count;
    bp_uint32       block_size;
    bp_uint8        method;        /* BP_METHOD_STORED, BP_METHOD_LZSS, BP_METHOD_LZO1X, or BP_METHOD_FLUFF */
    bp_uint8        method_level;  /* compression level 1-9 for Fluff/zstd; 0 = codec default */
    bp_uint8        creator_os;
    bp_uint8        creator_cpu;
    bp_progress_fn  progress_fn;
    void           *progress_data;
    bp_log_fn       log_fn;         /* optional; NULL = write to stderr */
    void           *log_data;

    /* internal state for create */
    bp_uint32       entries_written;
    bp_entry_t     *entry_list;    /* array of entries for file table */
    bp_uint32       entry_list_cap;
    bp_uint64       archive_orig_total;
    bp_uint64       archive_comp_total;
    bp_uint64       archive_orig_done;  /* running sum of completed entries */
    int             fast_fluff_threads; /* 0 = sequential; >1 = requested worker count */
    void           *fast_fluff_ctx;     /* internal cached modern context */
    bp_uint8       *fast_fluff_in_base; /* internal cached input slab */
    bp_uint8       *fast_fluff_out_base;/* internal cached output slab */
    bp_uint8       *fast_fluff_hist_base;/* internal cached history tail */
    bp_uint32       fast_fluff_ctx_threads; /* cached OpenMP worker count */
    bp_uint32       fast_fluff_batch_blocks; /* cached slab capacity, in blocks */
    bp_uint32       fast_fluff_block_size;
    bp_uint32       fast_fluff_out_size;
    bp_uint32       fast_fluff_hist_size;
    void           *seq_fluff_ctx;       /* cached sequential Fluff compress context */
    bp_uint8       *seq_in_buf;          /* cached sequential compress input buffer */
    bp_uint8       *seq_out_buf;         /* cached sequential compress output buffer */
    bp_uint32       seq_buf_block_size;  /* block_size when seq bufs were allocated */
} bp_archive_t;

/* --- Archive creation --------------------------------------------------- */

/* Initialize archive context for creation.
 * block_size: one of BP_BLOCK_* constants (0 = auto).
 * method: BP_METHOD_STORED, BP_METHOD_LZSS, BP_METHOD_LZO1X, or
 * BP_METHOD_FLUFF. */
int bp_archive_create(bp_archive_t *ar, bp_io_t *io,
                      bp_uint32 block_size, bp_uint8 method);

/* Add a file entry to the archive.
 * name: UTF-8 path, entry_type: BP_TYPE_*, platform: BP_OS_*,
 * file_io: source data (NULL for dirs), file_size: size in bytes,
 * mtime: Unix epoch seconds. */
int bp_archive_add_entry(bp_archive_t *ar, const char *name,
                         bp_uint8 entry_type, bp_uint8 platform,
                         bp_io_t *file_io, bp_uint64 file_size,
                         bp_uint64 mtime);

/* Like bp_archive_add_entry but also sets the entry flags field.
 * flags: combine BP_ENTRY_FLAG_MODE_VALID with (mode & BP_ENTRY_FLAG_MODE_MASK)
 * to store Unix permission bits; pass 0 for the same behaviour as
 * bp_archive_add_entry. */
int bp_archive_add_entry_ex(bp_archive_t *ar, const char *name,
                            bp_uint8 entry_type, bp_uint8 platform,
                            bp_io_t *file_io, bp_uint64 file_size,
                            bp_uint64 mtime, bp_uint16 flags);

/* Pack a single entry into dst_io without committing it to ar yet.
 * The packed bytes start at the current position of dst_io.
 * On success, packed is filled and must later be released with
 * bp_packed_entry_dispose(). */
int bp_archive_pack_entry(bp_archive_t *ar, const char *name,
                          bp_uint8 entry_type, bp_uint8 platform,
                          bp_io_t *file_io, bp_uint64 file_size,
                          bp_uint64 mtime, bp_io_t *dst_io,
                          bp_packed_entry_t *packed);

/* Like bp_archive_pack_entry but also sets the entry flags field. */
int bp_archive_pack_entry_ex(bp_archive_t *ar, const char *name,
                             bp_uint8 entry_type, bp_uint8 platform,
                             bp_io_t *file_io, bp_uint64 file_size,
                             bp_uint64 mtime, bp_io_t *dst_io,
                             bp_packed_entry_t *packed, bp_uint16 flags);

/* Commit a previously packed entry into ar in archive order.
 * src_io must be positioned at the start of the packed entry blob. */
int bp_archive_commit_packed_entry(bp_archive_t *ar, bp_io_t *src_io,
                                   const bp_packed_entry_t *packed);

/* Free any heap-owned fields inside packed and zero the structure. */
void bp_packed_entry_dispose(bp_packed_entry_t *packed);

/* Finalize: write file table, footer, patch header. */
int bp_archive_finalize(bp_archive_t *ar);

/* Set optional diagnostic log callback.
 * fn may be NULL to disable diagnostics (the default).
 * user_data is passed through to fn unchanged. */
void bp_archive_set_log(bp_archive_t *ar, bp_log_fn fn, void *user_data);

/* Open an existing archive for in-place append.
 * Reads the archive header and file table, loads existing entries into ar,
 * and seeks to the position where new entries can be written (overwriting
 * the old file table).  After this call, use bp_archive_add_entry to
 * append new files, then bp_archive_finalize to rewrite the file table,
 * footer, and patch the header.
 * The io must be opened for both reading and writing (e.g. "r+b"). */
int bp_archive_open_append(bp_archive_t *ar, bp_io_t *io,
                           bp_uint32 block_size, bp_uint8 method);

/* Copy all entries from an existing archive (src_io) into the archive
 * currently being created (ar).  Entry data is raw-copied without
 * decompression.  Call between bp_archive_create and bp_archive_finalize.
 * After this, bp_archive_add_entry may be called to append new files. */
int bp_archive_copy_from(bp_archive_t *ar, bp_io_t *src_io);

/* Free internal resources. */
void bp_archive_close(bp_archive_t *ar);

/* --- Entry name safety --------------------------------------------------- */

/* Return 1 if an archive entry name is safe to use as a relative
 * extraction path.  Rejects empty names, absolute paths, drive letters,
 * backslashes, and ".." components.  Frontends should strip leading '/'
 * (tar-style) before calling this, then skip or abort on unsafe names. */
int bp_name_is_safe(const char *name);

/* Return 1 if a symlink target cannot escape the extraction root when the
 * link lives at entry_name.  Rejects absolute targets and targets whose
 * ".." components rise above the extraction root. */
int bp_symlink_target_is_safe(const char *entry_name, const char *target);

/* --- Listing callback --------------------------------------------------- */

typedef int (*bp_list_fn)(const bp_entry_t *entry, void *user_data);

/* List archive contents. Calls fn for each entry. */
int bp_archive_list(bp_io_t *io, bp_list_fn fn, void *user_data);

/* --- Extraction --------------------------------------------------------- */

/* Callback: the frontend returns a bp_io_t to write to, or NULL to skip.
 * The frontend is responsible for creating directories as needed. */
typedef bp_io_t *(*bp_extract_fn)(const bp_entry_t *entry, void *user_data);

/* Called after each entry is fully extracted (so frontend can close file). */
typedef void (*bp_extract_done_fn)(const bp_entry_t *entry, bp_io_t *out,
                                   void *user_data);

/* Extract all entries from archive. */
int bp_archive_extract_all(bp_io_t *io,
                           bp_extract_fn open_fn,
                           bp_extract_done_fn done_fn,
                           bp_progress_fn progress_fn,
                           void *user_data);

/* --- Test --------------------------------------------------------------- */

/* Test callback: called for each entry with result (BP_OK or error). */
typedef void (*bp_test_fn)(const bp_entry_t *entry, int result,
                           void *user_data);

/* Test archive integrity. */
int bp_archive_test(bp_io_t *io, bp_test_fn fn, void *user_data);

/* Test archive integrity with optional byte-level progress reporting.
 * progress_fn may be NULL. Returning non-zero from progress_fn aborts with
 * BP_ERR_ABORTED. */
int bp_archive_test_progress(bp_io_t *io, bp_test_fn fn, void *user_data,
                             bp_progress_fn progress_fn,
                             void *progress_data);

/* --- Info --------------------------------------------------------------- */

typedef struct {
    bp_uint16   version;
    bp_uint16   flags;
    bp_uint32   file_count;
    bp_uint8    creator_os;
    bp_uint8    creator_cpu;
    bp_uint8    creator_ver_major;
    bp_uint8    creator_ver_minor;
    bp_uint64   archive_size;
    /* accumulated from file table */
    bp_uint32   entry_count;     /* files + dirs + symlinks etc. */
    bp_uint32   dir_count;
    bp_uint64   total_original;
    bp_uint64   total_compressed;
    bp_uint32   method_counts[8]; /* file count per method: [0]=stored [1]=LZSS [2]=LZO [3]=fluff [4]=zstd [5-7]=reserved */
} bp_archive_info_t;

/* Read archive header and file table to fill info struct. */
int bp_archive_info(bp_io_t *io, bp_archive_info_t *info);

/* --- Deletion ----------------------------------------------------------- */

/* Delete entries whose names match any string in del_names[].
 * Reads from src_io, writes a new archive (without the deleted entries) to
 * dst_io.  Entry data is raw-copied (no decompress/recompress).
 * Returns the number of deleted entries via *out_deleted (may be NULL).
 * Names are compared verbatim (UTF-8, forward slashes, no wildcards).
 * A name ending with '/' also deletes all entries under that directory. */
int bp_archive_delete(bp_io_t *src_io, bp_io_t *dst_io,
                      const char **del_names, bp_uint32 del_count,
                      bp_uint32 *out_deleted);

/* --- Append safety helper ---------------------------------------------- */

/* Return the byte offset and byte count of the archive's critical tail
 * (file-table + footer) — the region that bp_archive_open_append overwrites.
 * Frontends can read these bytes before the append and write them back to
 * restore the original archive on error.
 * Returns BP_OK on success; the I/O position after the call is undefined. */
int bp_archive_query_tail(bp_io_t *io,
                          bp_uint64 *tail_off,
                          bp_uint64 *tail_size);

/* --- Parallel fluff compression ----------------------------------------- */

/*
 * bp_set_fast_fluff: enable parallel fluff compression for subsequent
 * bp_archive_add_entry calls on ar.
 *
 * nthreads >1 : use up to nthreads OpenMP threads per block batch.
 * nthreads <=0: disable (fall back to sequential fluff).
 *
 * Requires the archive to have been built with -DBOLTPACK_FAST_FLUFF and
 * linked with -fopenmp.  On builds without BOLTPACK_FAST_FLUFF this
 * function is a no-op.
 *
 * Must be called AFTER bp_archive_create() and BEFORE bp_archive_add_entry().
 * The method on the archive must be BP_METHOD_FLUFF; calling with any other
 * method is allowed but has no effect.
 */
void bp_set_fast_fluff(bp_archive_t *ar, int nthreads);

#endif /* BP_ARCHIVER_H */
