/*
 * boltpack_dll.c -- thin DLL export wrapper for BoltPack archiver core
 *
 * Provides __stdcall-compatible exports for use from C# via P/Invoke.
 * Build with the core sources and miniLZO sources into boltpack_core.dll.
 *
 * ANSI C (C89/C90) - compiled with the core but lives in the GUI layer.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#include "boltpack_dll.h"
#include "fluff.h"
#include "lzss.h"
#include <stdio.h>

/* Re-export core functions with DLL linkage.
 * These are thin wrappers so P/Invoke can find them by name. */

BP_EXPORT int bp_dll_compress_init(void)
{
    return bp_compress_init();
}

BP_EXPORT void bp_dll_archive_set_level(bp_archive_t *ar, int level)
{
    if (ar == NULL) return;
    if (level < 1 || level > 9) level = 0;
    ar->method_level = (bp_uint8)level;
}

BP_EXPORT int bp_dll_archive_info(bp_io_t *io, bp_archive_info_t *info)
{
    return bp_archive_info(io, info);
}

BP_EXPORT int bp_dll_archive_list(bp_io_t *io, bp_list_fn fn, void *user_data)
{
    return bp_archive_list(io, fn, user_data);
}

BP_EXPORT int bp_dll_archive_extract_all(bp_io_t *io,
    bp_extract_fn open_fn, bp_extract_done_fn done_fn,
    bp_progress_fn progress_fn, void *user_data)
{
    return bp_archive_extract_all(io, open_fn, done_fn, progress_fn, user_data);
}

BP_EXPORT int bp_dll_archive_test_progress(bp_io_t *io,
    bp_test_fn fn, void *user_data,
    bp_progress_fn progress_fn, void *progress_data)
{
    return bp_archive_test_progress(io, fn, user_data,
                                    progress_fn, progress_data);
}

BP_EXPORT int bp_dll_archive_create(bp_archive_t *ar, bp_io_t *io,
    bp_uint32 block_size, bp_uint8 method)
{
    return bp_archive_create(ar, io, block_size, method);
}

BP_EXPORT int bp_dll_archive_add_entry(bp_archive_t *ar, const char *name,
    bp_uint8 entry_type, bp_uint8 platform,
    bp_io_t *file_io, bp_uint64 file_size, bp_uint64 mtime)
{
    return bp_archive_add_entry(ar, name, entry_type, platform,
                                file_io, file_size, mtime);
}

BP_EXPORT int bp_dll_archive_finalize(bp_archive_t *ar)
{
    return bp_archive_finalize(ar);
}

BP_EXPORT void bp_dll_archive_close(bp_archive_t *ar)
{
    bp_archive_close(ar);
}

/* Helper: create a bp_io_t from a C FILE pointer (for use from C#) */
static long dll_file_read(void *handle, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)handle);
}
static long dll_file_write(void *handle, const void *buf, bp_uint32 count)
{
    return (long)fwrite(buf, 1, (size_t)count, (FILE *)handle);
}
static int dll_file_seek(void *handle, bp_off_t offset, int origin)
{
#ifdef _WIN32
    return _fseeki64((FILE *)handle, (__int64)offset, origin);
#else
    return fseek((FILE *)handle, (long)offset, origin);
#endif
}
static bp_off_t dll_file_tell(void *handle)
{
#ifdef _WIN32
    return (bp_off_t)_ftelli64((FILE *)handle);
#else
    return (bp_off_t)ftell((FILE *)handle);
#endif
}

BP_EXPORT void bp_dll_io_from_file(bp_io_t *io, void *file_handle)
{
    io->read   = dll_file_read;
    io->write  = dll_file_write;
    io->seek   = dll_file_seek;
    io->tell   = dll_file_tell;
    io->handle = file_handle;
}

/* Open/close file helpers so C# doesn't need to manage FILE* directly */
BP_EXPORT void *bp_dll_fopen(const char *path, const char *mode)
{
    return (void *)fopen(path, mode);
}

BP_EXPORT void bp_dll_fclose(void *fp)
{
    if (fp) fclose((FILE *)fp);
}

BP_EXPORT void bp_dll_fseek_start(void *fp)
{
#ifdef _WIN32
    if (fp) _fseeki64((FILE *)fp, 0, 0);
#else
    if (fp) rewind((FILE *)fp);
#endif
}
