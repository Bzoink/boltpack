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

/* --- Archive context ---------------------------------------------------- */

typedef struct {
    bp_io_t        *io;
    bp_uint32       file_count;
    bp_uint32       block_size;
    bp_uint8        method;        /* BP_METHOD_STORED, BP_METHOD_LZSS, BP_METHOD_LZO1X, or BP_METHOD_FLUFF */
    bp_uint8        method_level;  /* reserved; ignored by all current methods */
    bp_uint8        creator_os;
    bp_uint8        creator_cpu;
    bp_progress_fn  progress_fn;
    void           *progress_data;

    /* internal state for create */
    bp_uint32       entries_written;
    bp_entry_t     *entry_list;    /* array of entries for file table */
    bp_uint32       entry_list_cap;
    bp_uint64       archive_orig_total;
    bp_uint64       archive_comp_total;
    bp_uint64       archive_orig_done;  /* running sum of completed entries */
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

/* Finalize: write file table, footer, patch header. */
int bp_archive_finalize(bp_archive_t *ar);

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
} bp_archive_info_t;

/* Read archive header and file table to fill info struct. */
int bp_archive_info(bp_io_t *io, bp_archive_info_t *info);

#endif /* BP_ARCHIVER_H */
