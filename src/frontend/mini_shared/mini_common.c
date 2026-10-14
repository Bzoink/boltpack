/*
 * mini_common.c -- utilities shared by all BoltPack mini frontends
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "mini_common.h"
#include <string.h>
#include <stdio.h>

int bp_mini_name_is_safe(const char *name)
{
    const char *p;
    if (name == NULL || name[0] == '\0' || name[0] == '/' ||
        name[1] == ':' || strchr(name, '\\') != NULL) return 0;
    for (p = name; *p; p++) {
        if (p[0] == '.' && p[1] == '.' &&
            (p == name || p[-1] == '/') &&
            (p[2] == '/' || p[2] == '\0')) return 0;
    }
    return 1;
}

/* ---- bp_mini_ultoa ----------------------------------------------------- */

/*
 * Convert unsigned long n to decimal in buf[11].
 * Returns a pointer to the first digit (somewhere in buf).
 * buf must be at least 11 bytes.
 */
char *bp_mini_ultoa(unsigned long n, char *buf)
{
    char *p;
    buf[10] = '\0';
    p = buf + 10;
    do { *--p = (char)('0' + (int)(n % 10UL)); n /= 10UL; } while (n);
    return p;
}

/* ---- bp_io_from_file --------------------------------------------------- */

static long file_read(void *handle, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)handle);
}

static long file_write(void *handle, const void *buf, bp_uint32 count)
{
    return (long)fwrite(buf, 1, (size_t)count, (FILE *)handle);
}

static int file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)ftell((FILE *)handle);
}

void bp_io_from_file(bp_io_t *io, FILE *fp)
{
    io->read   = file_read;
    io->write  = file_write;
    io->seek   = file_seek;
    io->tell   = file_tell;
    io->handle = fp;
}

/* ---- print_utf8_safe --------------------------------------------------- */

void print_utf8_safe(const char *name, unsigned int len)
{
#ifdef __WATCOMC__
    char buf[256];
    unsigned int i;
    unsigned int n;
    n = (len < 255) ? len : 255;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        buf[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    write(1, buf, n);
#else
    unsigned int i;
    unsigned char c;
    for (i = 0; i < len; i++) {
        c = (unsigned char)name[i];
        putchar((c >= 0x20 && c <= 0x7E) ? (int)c : (int)'?');
    }
#endif
}

/* ---- bp_mini_strerror -------------------------------------------------- */

const char *bp_mini_strerror(int code)
{
    switch (code) {
    case BP_OK:                      return "success";
    case BP_ERR_IO:                  return "I/O error";
    case BP_ERR_NOMEM:               return "out of memory";
    case BP_ERR_MAGIC:               return "not a valid BoltPack archive";
    case BP_ERR_VERSION:             return "unsupported archive version";
    case BP_ERR_CHECKSUM:            return "checksum mismatch";
    case BP_ERR_CORRUPT:             return "archive is corrupt";
    case BP_ERR_TRUNCATED:           return "archive is truncated";
    case BP_ERR_OVERFLOW:            return "block size too large; re-create archive with -b 32k";
    case BP_ERR_NAME_TOO_LONG:       return "entry name too long";
    case BP_ERR_UNSUPPORTED_METHOD:  return "compression method not supported by mini";
    case BP_ERR_COMPRESS:            return "compression failed";
    case BP_ERR_DECOMPRESS:          return "decompression failed";
    case BP_ERR_ABORTED:             return "operation aborted";
    case BP_ERR_NOT_FOUND:           return "entry not found";
    case BP_ERR_INVALID_ARG:         return "invalid argument";
    default:                          return "unknown error";
    }
}

/* ---- format_size64_mini ------------------------------------------------ */

void format_size64_mini(bp_uint64 v, char *buf)
{
    if (v.hi == 0) {
        unsigned long n = (unsigned long)v.lo;
        char nbuf[11];
        char *p;
        const char *suffix;

        if (n < 1024UL) {
            p = bp_mini_ultoa(n, nbuf);
            suffix = " B";
        } else if (n < 1024UL * 1024UL) {
            p = bp_mini_ultoa(n / 1024UL, nbuf);
            suffix = " KB";
        } else if (n < 1024UL * 1024UL * 1024UL) {
            p = bp_mini_ultoa(n / (1024UL * 1024UL), nbuf);
            suffix = " MB";
        } else {
            p = bp_mini_ultoa(n / (1024UL * 1024UL * 1024UL), nbuf);
            suffix = " GB";
        }
        strcpy(buf, p);
        strcat(buf, suffix);
    } else {
        /* Value > 4 GB: print exact byte count using long division. */
        char tmp[21];
        int pos = 20;
        bp_uint32 nhi = v.hi;
        bp_uint32 nlo = v.lo;
        bp_uint32 d[4];
        bp_uint32 q[4];
        bp_uint32 rem;
        bp_uint32 combined;
        int i;

        tmp[pos] = '\0';
        do {
            d[0] = (nhi >> 16) & 0xFFFFU;
            d[1] = nhi & 0xFFFFU;
            d[2] = (nlo >> 16) & 0xFFFFU;
            d[3] = nlo & 0xFFFFU;
            rem = 0;
            for (i = 0; i < 4; i++) {
                combined = rem * 65536UL + d[i];
                q[i]     = combined / 10UL;
                rem      = combined % 10UL;
            }
            tmp[--pos] = (char)('0' + (int)rem);
            nhi = (q[0] << 16) | q[1];
            nlo = (q[2] << 16) | q[3];
        } while (nhi != 0 || nlo != 0);
        strcpy(buf, tmp + pos);
        strcat(buf, " B");
    }
}

/* ---- parse_block_size_mini --------------------------------------------- */

int parse_block_size_mini(const char *s, bp_uint32 *out)
{
    if (s == NULL || out == NULL) return 0;

    if (strcmp(s, "8k") == 0 || strcmp(s, "8K") == 0) {
        *out = 8192U;
        return 1;
    }
    if (strcmp(s, "16k") == 0 || strcmp(s, "16K") == 0) {
        *out = 16384U;
        return 1;
    }
    if (strcmp(s, "32k") == 0 || strcmp(s, "32K") == 0) {
        *out = BP_MINI_MAX_BLOCK_SIZE;
        return 1;
    }
    return 0;
}

/* ---- Watcom-specific helpers (DOS/Win16/OS2-16) ------------------------ */

#ifdef __WATCOMC__

/* handle I/O callbacks using low-level read/write/lseek */
static long handle_read(void *h, void *buf, bp_uint32 n)
{
    return (long)read((int)(long)h, buf, (unsigned)n);
}

static long handle_write(void *h, const void *buf, bp_uint32 n)
{
    return (long)write((int)(long)h, buf, (unsigned)n);
}

static int handle_seek(void *h, bp_off_t off, int org)
{
    return (lseek((int)(long)h, (long)off, org) == -1L) ? -1 : 0;
}

static bp_off_t handle_tell(void *h)
{
    return (bp_off_t)lseek((int)(long)h, 0L, SEEK_CUR);
}

void bp_io_from_handle(bp_io_t *io, int fd)
{
    io->read   = handle_read;
    io->write  = handle_write;
    io->seek   = handle_seek;
    io->tell   = handle_tell;
    io->handle = (void *)(long)fd;
}

void bp_wstr(int fd, const char *s)
{
    const char *p = s;
    while (*p) p++;
    if (p > s) write(fd, s, (unsigned)(p - s));
}

void bp_wulong(int fd, unsigned long n)
{
    char buf[11];
    char *p = bp_mini_ultoa(n, buf);
    write(fd, p, (unsigned)(buf + 10 - p));
}

void bp_wuint3(int fd, unsigned int n)
{
    char buf[3];
    if (n > 999) n = 999;
    buf[2] = (char)('0' + (n % 10)); n /= 10;
    buf[1] = (n > 0) ? (char)('0' + (n % 10)) : ' '; n /= 10;
    buf[0] = (n > 0) ? (char)('0' + n) : ' ';
    write(fd, buf, 3);
}

/*
 * Month-day table (shared by both time conversion functions).
 * Index 0 = January.  February (index 1) is 28 days; leap year adds 1 when
 * needed in the conversion logic.
 */
static unsigned char s_mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

static int is_leap(unsigned y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

unsigned long bp_dos_to_epoch(unsigned dos_date, unsigned dos_time)
{
    unsigned year  = 1980 + ((dos_date >> 9) & 0x7F);
    unsigned month = (dos_date >> 5) & 0x0F;
    unsigned day   = dos_date & 0x1F;
    unsigned hour  = (dos_time >> 11) & 0x1F;
    unsigned min   = (dos_time >> 5) & 0x3F;
    unsigned sec   = (dos_time & 0x1F) * 2;
    unsigned long  days;
    unsigned y;
    unsigned m;

    if (month < 1 || month > 12 || day < 1) return 0;

    /* Days from 1970-01-01 to the start of `year` */
    days = 0;
    for (y = 1970; y < year; y++) {
        days += is_leap(y) ? 366UL : 365UL;
    }
    /* Days within the year up to (but not including) this month */
    for (m = 1; m < month; m++) {
        days += s_mdays[m - 1];
        if (m == 2 && is_leap(year)) days++;
    }
    days += (unsigned long)(day - 1);

    return days * 86400UL
           + (unsigned long)hour * 3600UL
           + (unsigned long)min  * 60UL
           + (unsigned long)sec;
}

void bp_epoch_to_dos(unsigned long epoch, unsigned *dos_date, unsigned *dos_time)
{
    unsigned long days = epoch / 86400UL;
    unsigned long rem  = epoch % 86400UL;
    unsigned hour = (unsigned)(rem / 3600UL);
    unsigned min  = (unsigned)((rem % 3600UL) / 60UL);
    unsigned sec  = (unsigned)(rem % 60UL);
    unsigned year = 1970;
    unsigned month;
    unsigned day;
    unsigned long dy;
    unsigned dm;

    /* Find year */
    for (;;) {
        dy = is_leap(year) ? 366UL : 365UL;
        if (days < dy) break;
        days -= dy;
        year++;
    }
    /* Find month */
    month = 1;
    for (;;) {
        dm = s_mdays[month - 1];
        if (month == 2 && is_leap(year)) dm++;
        if (days < (unsigned long)dm) break;
        days -= (unsigned long)dm;
        month++;
    }
    day = (unsigned)days + 1;

    /* Clamp to DOS range (1980-01-01 .. 2107-12-31) */
    if (year < 1980) { year = 1980; month = 1; day = 1; }
    if (year > 2107) { year = 2107; month = 12; day = 31; }

    *dos_date = ((year - 1980) << 9) | (month << 5) | day;
    *dos_time = (hour << 11) | (min << 5) | (sec / 2);
}

#endif /* __WATCOMC__ */
