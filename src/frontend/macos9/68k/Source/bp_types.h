/*
 * bp_types.h -- portable types, constants, and helpers for BoltPack
 *
 * ANSI C (C89/C90) only. No compiler extensions.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#ifndef BP_TYPES_H
#define BP_TYPES_H

#include <stddef.h>

/* --- Portable fixed-width types ----------------------------------------- */

typedef unsigned char      bp_uint8;
typedef unsigned short     bp_uint16;   /* C89: short >= 16 bits */
typedef unsigned long      bp_uint32;   /* C89: long  >= 32 bits */

typedef struct {
    bp_uint32 lo;
    bp_uint32 hi;
} bp_uint64;

/* --- BoltPack version --------------------------------------------------- */

#define BP_VERSION_MAJOR  0
#define BP_VERSION_MINOR  1
#define AUTHOR_MESSAGE    "Copyright (C) 2026 Tony Mattsson; Released under GNU GPLv2"

/* --- Format constants --------------------------------------------------- */

/* Version this build writes; v2 also accepted on read (see main bp_types.h
 * for the full v3 description).  This legacy 68k copy historically wrote
 * version 1; it now writes v3 and still reads 1/2/3. */
#define BP_FORMAT_VERSION      3
#define BP_FORMAT_VERSION_V2   2

#define BP_MAGIC_0  0x42  /* 'B' */
#define BP_MAGIC_1  0x4F  /* 'O' */
#define BP_MAGIC_2  0x4C  /* 'L' */
#define BP_MAGIC_3  0x54  /* 'T' */

#define BP_TABLE_MAGIC_0  0x54  /* 'T' */
#define BP_TABLE_MAGIC_1  0x42  /* 'B' */
#define BP_TABLE_MAGIC_2  0x4C  /* 'L' */
#define BP_TABLE_MAGIC_3  0x54  /* 'T' */

#define BP_FOOTER_MAGIC_0  0x54  /* 'T' */
#define BP_FOOTER_MAGIC_1  0x4C  /* 'L' */
#define BP_FOOTER_MAGIC_2  0x4F  /* 'O' */
#define BP_FOOTER_MAGIC_3  0x42  /* 'B' */

#define BP_HEADER_SIZE   32
#define BP_FOOTER_SIZE   20

/* Entry header fixed part (excluding name): 42 bytes total with name */
#define BP_ENTRY_FIXED   42  /* header_size(2) + name_len(2) + fields(38) */

/* File-table record fixed tail (after name_len + name).
 * v2: 34 bytes; v3 adds method(1) + header_size(2) + checksum(4) = 41. */
#define BP_TABLE_REC_TAIL_V2   34
#define BP_TABLE_REC_TAIL_V3   41

/* v3 CRC-32 field offsets inside the 32-byte header (former reserved area). */
#define BP_HEADER_TABLE_CRC_OFF   24
#define BP_HEADER_CRC_OFF         28

/* Compression methods */
#define BP_METHOD_STORED   0
#define BP_METHOD_LZSS     1
#define BP_METHOD_LZO1X    2
#define BP_METHOD_FLUFF    3

/* Checksum types */
#define BP_CKSUM_ADLER32   0
#define BP_CKSUM_CRC32     1

/* Entry types */
#define BP_TYPE_DATA       0
#define BP_TYPE_RSRC       1
#define BP_TYPE_META       2
#define BP_TYPE_SYMLINK    3
#define BP_TYPE_DIR        4

/* creator_os values */
#define BP_OS_UNKNOWN      0x00
#define BP_OS_LINUX        0x01
#define BP_OS_MACOS9       0x02
#define BP_OS_MACOSX       0x03
#define BP_OS_AMIGA_68K    0x04
#define BP_OS_AMIGA_PPC    0x05
#define BP_OS_MORPHOS      0x06
#define BP_OS_AROS         0x07
#define BP_OS_DOS          0x08
#define BP_OS_WINDOWS      0x09
#define BP_OS_FREEBSD      0x0A
#define BP_OS_NETBSD       0x0B
#define BP_OS_OPENBSD      0x0C
#define BP_OS_SOLARIS      0x0D
#define BP_OS_IRIX         0x0E
#define BP_OS_HPUX         0x0F
#define BP_OS_AIX          0x10
#define BP_OS_ATARI        0x11
#define BP_OS_RISCOS       0x12
#define BP_OS_OS2          0x13
#define BP_OS_BEOS         0x14

/* creator_cpu values */
#define BP_CPU_UNKNOWN     0x00
#define BP_CPU_X86         0x01
#define BP_CPU_X86_64      0x02
#define BP_CPU_PPC32       0x03
#define BP_CPU_PPC64       0x04
#define BP_CPU_M68K        0x05
#define BP_CPU_ARM32       0x06
#define BP_CPU_ARM64       0x07
#define BP_CPU_MIPS32      0x08
#define BP_CPU_MIPS64      0x09

/* Block sizes */
#define BP_BLOCK_8K    8192
#define BP_BLOCK_16K   16384
#define BP_BLOCK_32K   32768
#define BP_BLOCK_64K   65536
#define BP_BLOCK_256K  262144

/* Default block size */
#define BP_BLOCK_DEFAULT  BP_BLOCK_32K

/* --- Error codes -------------------------------------------------------- */

#define BP_OK                      0
#define BP_ERR_IO                 -1
#define BP_ERR_NOMEM              -2
#define BP_ERR_MAGIC              -3
#define BP_ERR_VERSION            -4
#define BP_ERR_CHECKSUM           -5
#define BP_ERR_CORRUPT            -6
#define BP_ERR_TRUNCATED          -7
#define BP_ERR_OVERFLOW           -8
#define BP_ERR_NAME_TOO_LONG      -9
#define BP_ERR_UNSUPPORTED_METHOD -10
#define BP_ERR_COMPRESS           -11
#define BP_ERR_DECOMPRESS         -12
#define BP_ERR_ABORTED            -13
#define BP_ERR_NOT_FOUND          -14
#define BP_ERR_INVALID_ARG        -15

/* --- I/O callback interface --------------------------------------------- */

typedef struct {
    long (*read)(void *handle, void *buf, bp_uint32 count);
    long (*write)(void *handle, const void *buf, bp_uint32 count);
    int  (*seek)(void *handle, long offset, int origin);
    long (*tell)(void *handle);
    void *handle;
} bp_io_t;

/* --- Progress reporting ------------------------------------------------- */

typedef struct {
    bp_uint32     file_index;
    bp_uint32     file_count;
    const char   *file_name;
    bp_uint64     file_orig_size;
    bp_uint64     file_bytes_done;
    bp_uint64     file_comp_size;
    bp_uint64     archive_orig_total;
    bp_uint64     archive_comp_total;
    bp_uint64     archive_orig_done;  /* original bytes of fully-completed entries */
} bp_progress_t;

typedef int (*bp_progress_fn)(const bp_progress_t *progress, void *user_data);

/* --- bp_uint64 helpers -------------------------------------------------- */

#define BP_UINT64_ZERO  { 0UL, 0UL }

static bp_uint64 bp_u64_from_u32(bp_uint32 v)
{
    bp_uint64 r;
    r.lo = v;
    r.hi = 0;
    return r;
}

static bp_uint64 bp_u64_add(bp_uint64 a, bp_uint64 b)
{
    bp_uint64 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo ? 1UL : 0UL);
    return r;
}

static int bp_u64_is_zero(bp_uint64 v)
{
    return v.lo == 0 && v.hi == 0;
}

static bp_uint32 bp_u64_to_u32(bp_uint64 v)
{
    return v.lo;
}

/* --- Little-endian read/write helpers ----------------------------------- */

static void bp_write_le16(bp_uint8 *p, bp_uint16 v)
{
    p[0] = (bp_uint8)(v & 0xFF);
    p[1] = (bp_uint8)((v >> 8) & 0xFF);
}

static void bp_write_le32(bp_uint8 *p, bp_uint32 v)
{
    p[0] = (bp_uint8)(v & 0xFF);
    p[1] = (bp_uint8)((v >> 8) & 0xFF);
    p[2] = (bp_uint8)((v >> 16) & 0xFF);
    p[3] = (bp_uint8)((v >> 24) & 0xFF);
}

static void bp_write_le64(bp_uint8 *p, bp_uint64 v)
{
    bp_write_le32(p, v.lo);
    bp_write_le32(p + 4, v.hi);
}

static bp_uint16 bp_read_le16(const bp_uint8 *p)
{
    return (bp_uint16)p[0] | ((bp_uint16)p[1] << 8);
}

static bp_uint32 bp_read_le32(const bp_uint8 *p)
{
    return (bp_uint32)p[0]
         | ((bp_uint32)p[1] << 8)
         | ((bp_uint32)p[2] << 16)
         | ((bp_uint32)p[3] << 24);
}

static bp_uint64 bp_read_le64(const bp_uint8 *p)
{
    bp_uint64 r;
    r.lo = bp_read_le32(p);
    r.hi = bp_read_le32(p + 4);
    return r;
}

#endif /* BP_TYPES_H */
