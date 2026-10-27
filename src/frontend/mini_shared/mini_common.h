/*
 * mini_common.h -- utilities shared by all BoltPack mini frontends
 *
 * Provides:
 *   bp_io_from_file     - FILE* adapter for bp_io_t (all platforms)
 *   bp_mini_ultoa       - unsigned long to decimal string (all platforms)
 *   print_utf8_safe     - write name to stdout replacing non-ASCII with '?'
 *   bp_mini_strerror    - BP_ERR_* to human-readable string
 *   format_size64_mini  - format bp_uint64 as human-readable size string
 *   parse_block_size_mini - parse "8k"/"16k"/"32k"
 *
 *   Under __WATCOMC__ only (DOS/Win16/OS2-16):
 *   bp_io_from_handle   - file-descriptor adapter for bp_io_t
 *   bp_wstr             - write C string to file descriptor
 *   bp_wulong           - write unsigned long as decimal to fd
 *   bp_wuint3           - write unsigned int right-aligned in 3 chars to fd
 *   bp_dos_to_epoch     - DOS packed date/time -> Unix epoch (seconds)
 *   bp_epoch_to_dos     - Unix epoch -> DOS packed date/time
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_MINI_COMMON_H
#define BP_MINI_COMMON_H

#include "../../core/mini/archiver.h"
#include "../../core/mini/compress.h"
#include <stdio.h>

/* Fill a bp_io_t from a stdio FILE*.
 * Uses fread/fwrite/fseek/ftell, which accept (long) offsets.
 * Archives > 2 GB cannot be processed on platforms where long is 32 bits. */
void bp_io_from_file(bp_io_t *io, FILE *fp);

/* Return 1 for a relative archive path safe to use during extraction. */
int bp_mini_name_is_safe(const char *name);

/* Mini filename conversion uses the configured native codepage. Conversion
 * fails rather than silently substituting an unrepresentable character. */
int bp_mini_local_to_utf8(const char *src, char *dst, unsigned int dst_size);
int bp_mini_utf8_to_local(const char *src, char *dst, unsigned int dst_size);
int bp_mini_add_local_entry(bp_mini_archive_t *ar, const char *name,
                            bp_uint8 entry_type, bp_uint8 platform,
                            bp_io_t *file_io, bp_uint64 file_size,
                            bp_uint64 mtime, bp_uint16 flags);

/* Convert unsigned long n to decimal in buf[11]; returns pointer to first digit.
 * buf must be at least 11 bytes. */
char *bp_mini_ultoa(unsigned long n, char *buf);

/* Write at most len bytes of name to stdout.
 * Characters 0x20-0x7E are printed verbatim; all others become '?'. */
void print_utf8_safe(const char *name, unsigned int len);

/* Return a constant string for a BP_ERR_* code. */
const char *bp_mini_strerror(int code);

/* Write a human-readable size (e.g. "4 KB", "1 MB") to buf[32].
 * buf must be at least 32 bytes. */
void format_size64_mini(bp_uint64 v, char *buf);

/* Parse a block size string ("8k", "16k", "32k").
 * Returns nonzero on success and writes the block size to *out.
 * Caps at BP_MINI_MAX_BLOCK_SIZE.  Returns 0 if unrecognised. */
int parse_block_size_mini(const char *s, bp_uint32 *out);

/* ---- Watcom-only helpers (DOS/Win16/OS2-16 builds) -------------------- */
#ifdef __WATCOMC__
#include <io.h>
#include <fcntl.h>
#include <dos.h>

/* Fill a bp_io_t from a Watcom file descriptor (from open()).
 * Uses read/write/lseek directly -- no stdio buffering. */
void bp_io_from_handle(bp_io_t *io, int fd);

/* Write a NUL-terminated string to file descriptor fd. */
void bp_wstr(int fd, const char *s);

/* Write unsigned long n as decimal digits to fd. */
void bp_wulong(int fd, unsigned long n);

/* Write unsigned int n right-aligned in a 3-character field (space-padded)
 * to fd.  n is clamped to 999. */
void bp_wuint3(int fd, unsigned int n);

/* Convert a DOS packed date/time to Unix epoch seconds (UTC approximation).
 * dos_date: bits 15-9 = year-1980, 8-5 = month, 4-0 = day.
 * dos_time: bits 15-11 = hours, 10-5 = minutes, 4-0 = seconds/2. */
unsigned long bp_dos_to_epoch(unsigned dos_date, unsigned dos_time);

/* Convert Unix epoch seconds to DOS packed date/time.
 * Clamps to DOS range 1980-01-01 .. 2107-12-31. */
void bp_epoch_to_dos(unsigned long epoch, unsigned *dos_date, unsigned *dos_time);

#endif /* __WATCOMC__ */

#endif /* BP_MINI_COMMON_H */
