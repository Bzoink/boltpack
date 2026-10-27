/*
 * boltpack_dll.h -- exported DLL API for BoltPack archiver core
 *
 * Declarations for all BP_EXPORT functions in boltpack_dll.c.
 * Include this header in any translation unit that calls these functions.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_BOLTPACK_DLL_H
#define BP_BOLTPACK_DLL_H

#include <wchar.h>
#include "archiver.h"
#include "compress.h"

#ifdef _WIN32
#define BP_EXPORT __declspec(dllexport)
#else
#define BP_EXPORT
#endif

BP_EXPORT int   bp_dll_compress_init(void);

/* Set the compression level (1-9) for Fluff and zstd on an archive context.
 * Must be called after bp_dll_archive_create and before bp_dll_archive_add_entry.
 * 0 or out-of-range values are treated as "use codec default". */
BP_EXPORT void  bp_dll_archive_set_level(bp_archive_t *ar, int level);

BP_EXPORT int   bp_dll_archive_info(bp_io_t *io, bp_archive_info_t *info);

BP_EXPORT int   bp_dll_archive_list(bp_io_t *io, bp_list_fn fn,
                                     void *user_data);

BP_EXPORT int   bp_dll_archive_extract_all(bp_io_t *io,
                    bp_extract_fn open_fn, bp_extract_done_fn done_fn,
                    bp_progress_fn progress_fn, void *user_data);

BP_EXPORT int   bp_dll_archive_extract_all_ex(bp_io_t *io,
                    bp_extract_fn open_fn, bp_extract_done_status_fn done_fn,
                    bp_progress_fn progress_fn, void *user_data);

BP_EXPORT int   bp_dll_archive_test_progress(bp_io_t *io,
                    bp_test_fn fn, void *user_data,
                    bp_progress_fn progress_fn, void *progress_data);

BP_EXPORT int   bp_dll_archive_create(bp_archive_t *ar, bp_io_t *io,
                    bp_uint32 block_size, bp_uint8 method);

/* Heap-owned context helpers for managed bindings. */
BP_EXPORT bp_archive_t *bp_dll_archive_alloc(void);
BP_EXPORT void  bp_dll_archive_free(bp_archive_t *ar);

BP_EXPORT int   bp_dll_archive_add_entry(bp_archive_t *ar, const char *name,
                    bp_uint8 entry_type, bp_uint8 platform,
                    bp_io_t *file_io, bp_uint64 file_size, bp_uint64 mtime);

BP_EXPORT int   bp_dll_archive_add_entry_ex(bp_archive_t *ar, const char *name,
                    bp_uint8 entry_type, bp_uint8 platform,
                    bp_io_t *file_io, bp_uint64 file_size, bp_uint64 mtime,
                    bp_uint16 flags);

BP_EXPORT int   bp_dll_archive_add_fat_metadata(bp_archive_t *ar,
                    const char *name, bp_uint8 subject_type,
                    bp_uint8 valid, bp_uint8 value);

BP_EXPORT int   bp_dll_archive_finalize(bp_archive_t *ar);

BP_EXPORT void  bp_dll_archive_close(bp_archive_t *ar);

BP_EXPORT void  bp_dll_io_from_file(bp_io_t *io, void *file_handle);

BP_EXPORT void *bp_dll_fopen(const char *path, const char *mode);

/* Unicode filesystem path variant for Windows managed frontends. */
BP_EXPORT void *bp_dll_fopen_w(const wchar_t *path, const wchar_t *mode);

BP_EXPORT void *bp_dll_fopen_w_exclusive(const wchar_t *path);

BP_EXPORT void  bp_dll_fclose(void *fp);

/* Flush, commit, and close a writer; returns zero only if all steps succeed. */
BP_EXPORT int   bp_dll_fclose_checked(void *fp);

BP_EXPORT void  bp_dll_fseek_start(void *fp);

#endif /* BP_BOLTPACK_DLL_H */
