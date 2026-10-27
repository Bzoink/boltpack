/*
 * archiver.h -- mini archive read/write interface for BoltPack
 *
 * Subset of the full archiver API.  Fluff codec is absent.
 * Capability limits are enforced at *create* time only; list and info
 * always iterate and display all entries regardless of archive size.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_MINI_ARCHIVER_H
#define BP_MINI_ARCHIVER_H

#include "../bp_types.h"

/* Maximum file name length in bytes (same as full archiver) */
#define BP_MAX_NAME_LEN  65493U

/* ---- Entry info -------------------------------------------------------- */

typedef struct {
    char       *name;          /* UTF-8 path (malloc'd, NUL-terminated) */
    bp_uint16   name_len;
    bp_uint64   original_size;
    bp_uint64   compressed_size;
    bp_uint32   checksum;      /* CRC-32 of uncompressed data */
    bp_uint8    method;        /* BP_METHOD_STORED, BP_METHOD_BOLTLITE, or BP_METHOD_BOLTLZ */
    bp_uint8    checksum_type; /* BP_CKSUM_CRC32 */
    bp_uint32   block_size;    /* 0 for dirs/empty; nonzero for data */
    bp_uint8    entry_type;    /* BP_TYPE_* */
    bp_uint8    platform;      /* BP_OS_* */
    bp_uint64   mtime;         /* Unix epoch seconds */
    bp_uint16   flags;
    bp_uint16   header_size;   /* 42 + name_len */
    bp_uint64   offset;        /* byte offset of this entry in the archive */
} bp_mini_entry_t;

/* ---- Archive create context ------------------------------------------- */

typedef struct {
    bp_io_t          *io;
    bp_uint32         block_size;
    bp_uint8          method;
    bp_uint8          creator_os;
    bp_uint8          creator_cpu;
    bp_uint32         entries_written;
    bp_mini_entry_t  *entry_list;    /* malloc'd array for file table */
    bp_uint32         entry_list_cap;
    bp_uint64         archive_orig_total;
    bp_uint64         archive_comp_total;
} bp_mini_archive_t;

/* ---- Archive info (for 'i' command) ------------------------------------ */

typedef struct {
    bp_uint16  version;
    bp_uint16  flags;
    bp_uint32  file_count;     /* from archive header */
    bp_uint8   creator_os;
    bp_uint8   creator_cpu;
    bp_uint8   creator_ver_major;
    bp_uint8   creator_ver_minor;
    bp_uint64  archive_size;
    bp_uint32  entry_count;    /* total entries visited in file table */
    bp_uint32  dir_count;
    bp_uint64  total_original;
    bp_uint64  total_compressed;
    bp_uint32  method_counts[5]; /* file count per method: [0]=stored [1]=BoltLite [2]=boltlz [3]=fluff [4]=zstd */
} bp_mini_archive_info_t;

/* ---- Utility ----------------------------------------------------------- */

/* Print a bp_uint64 value as decimal digits to stdout (no newline).
 * Works on 16-bit targets: uses only unsigned long (32-bit) arithmetic.
 * Not available when BP_MINI_QUIET is defined (size-constrained builds). */
#ifndef BP_MINI_QUIET
void bp_mini_u64_print(bp_uint64 v);
#endif

/* ---- Create ------------------------------------------------------------ */

/*
 * Initialise mini archive context and write the archive header placeholder.
 *   block_size: one of BP_BLOCK_8K..BP_BLOCK_32K (capped to BP_MINI_MAX_BLOCK_SIZE).
 *   method:     BP_METHOD_STORED, BP_METHOD_BOLTLITE, or BP_METHOD_BOLTLZ;
 *               returns BP_ERR_UNSUPPORTED_METHOD for method > BP_METHOD_BOLTLZ.
 */
int bp_mini_archive_create(bp_mini_archive_t *ar, bp_io_t *io,
                           bp_uint32 block_size, bp_uint8 method);

/*
 * Add one file or directory entry.
 *   Returns BP_ERR_UNSUPPORTED_METHOD if method > BP_METHOD_BOLTLZ.
 *   Returns BP_ERR_OVERFLOW if file_size.hi != 0 (> 4 GB).
 *   Returns BP_ERR_OVERFLOW if entry count would exceed BP_MINI_MAX_ENTRIES.
 *   file_io may be NULL for directory entries.
 */
int bp_mini_archive_add_entry(bp_mini_archive_t *ar, const char *name,
                              bp_uint8 entry_type, bp_uint8 platform,
                              bp_io_t *file_io, bp_uint64 file_size,
                              bp_uint64 mtime);

/* Explicit entry flags; clear MTIME_VALID and pass zero for an unknown time.
 * The original entry point treats the supplied mtime as known. */
int bp_mini_archive_add_entry_ex(bp_mini_archive_t *ar, const char *name,
                                 bp_uint8 entry_type, bp_uint8 platform,
                                 bp_io_t *file_io, bp_uint64 file_size,
                                 bp_uint64 mtime, bp_uint16 flags);

/* Write file table and footer, patch the header.  Must call before close. */
int bp_mini_archive_finalize(bp_mini_archive_t *ar);

/* Free internal resources (does not close the io). */
void bp_mini_archive_close(bp_mini_archive_t *ar);

/* ---- List -------------------------------------------------------------- */

typedef int (*bp_mini_list_fn)(const bp_mini_entry_t *entry, void *user_data);

/*
 * Iterate every entry in the archive and call fn.
 * No capability filtering: entries with any method or size are reported.
 */
int bp_mini_archive_list(bp_io_t *io, bp_mini_list_fn fn, void *user_data);

/* ---- Extract ----------------------------------------------------------- */

/*
 * Called before each entry is extracted.  Return a bp_io_t* to write output
 * to, or NULL to skip this entry.  The frontend is responsible for creating
 * any required directories.
 */
typedef bp_io_t *(*bp_mini_extract_fn)(const bp_mini_entry_t *entry,
                                       void *user_data);

/* Called after each entry has been fully written (so frontend can close/flush). */
typedef void (*bp_mini_extract_done_fn)(const bp_mini_entry_t *entry,
                                        bp_io_t *out, void *user_data);

/* Called exactly once for each opened output, on success or failure. */
typedef int (*bp_mini_extract_done_status_fn)(const bp_mini_entry_t *entry,
                                               bp_io_t *out, int status,
                                               void *user_data);

/*
 * Extract all entries.  Per-entry capability checks:
 *   method > BP_METHOD_BOLTLZ  -> prints warning, skips entry.
 *   original_size.hi != 0     -> prints warning, skips entry.
 * open_fn returns NULL to skip; done_fn is called only for non-skipped entries.
 */
int bp_mini_archive_extract_all(bp_io_t *io,
                                bp_mini_extract_fn open_fn,
                                bp_mini_extract_done_fn done_fn,
                                void *user_data);

/* A completion callback may return BP_ERR_PARTIAL to report metadata loss;
 * extraction continues and the overall result is BP_ERR_PARTIAL. */
int bp_mini_archive_extract_all_ex(bp_io_t *io, bp_mini_extract_fn open_fn,
                                   bp_mini_extract_done_status_fn done_fn,
                                   void *user_data);

/* ---- Test ------------------------------------------------------------- */

typedef void (*bp_mini_test_fn)(const bp_mini_entry_t *entry, int result,
                                void *user_data);

/* Test archive integrity.  Method > 1 entries are flagged as unsupported.
 * Entries with original_size.hi != 0 are skipped with a warning. */
int bp_mini_archive_test(bp_io_t *io, bp_mini_test_fn fn, void *user_data);

/* ---- Info -------------------------------------------------------------- */

/* Read archive header and file table to fill *info. */
int bp_mini_archive_info(bp_io_t *io, bp_mini_archive_info_t *info);

#endif /* BP_MINI_ARCHIVER_H */
