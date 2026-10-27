/*
 * bp_types.h -- portable types, constants, and helpers for BoltPack
 *
 * ANSI C (C89/C90) only. No compiler extensions.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_TYPES_H
#define BP_TYPES_H

#include <stddef.h>

/*
 * Plan 9 compatibility: Plan 9 doesn't provide <limits.h>.
 * We include it conditionally for platforms that have it, and define
 * fallback constants for Plan 9 and other embedded systems.
 */
#if !defined(__PLAN9__)
  #include <limits.h>
#else
  /*
   * Plan 9 fallback constants. These are based on common C implementations.
   * ULONG_MAX is used to detect LP64 (64-bit long) vs 32-bit systems.
   */
  #ifndef ULONG_MAX
    #define ULONG_MAX  0xFFFFFFFFUL  /* 32-bit default for vintage systems */
  #endif
#endif

/* --- Portable fixed-width types ----------------------------------------- */

typedef unsigned char   bp_uint8;
typedef unsigned short  bp_uint16;  /* C89: short >= 16 bits */

/* bp_uint32: on 16-bit Watcom/Aztec (sizeof(int)==2), SIZEOF_INT is passed as
 * -DSIZEOF_INT=2 by the build system; we then use unsigned long (always 4 bytes
 * on these compilers) so that all 32-bit archive fields are encoded correctly. */
#if defined(SIZEOF_INT) && (SIZEOF_INT < 4) && ULONG_MAX <= 0xFFFFFFFFUL
  typedef unsigned long  bp_uint32;  /* 16-bit override: int=2, long=4 */
#else
  typedef unsigned int   bp_uint32;  /* C89: int >= 16 bits; 32-bit on modern platforms */
#endif

typedef struct {
    bp_uint32 lo;
    bp_uint32 hi;
} bp_uint64;

/* --- Signed 64-bit file offset type ------------------------------------ */
/*
 * bp_off_t is used for seek() offsets and tell() return values.
 * On modern 64-bit hosts (Linux LP64, Windows via __int64) it is 64 bits,
 * which allows correct operation on files/archives larger than 2 GB.
 * On 32-bit and 16-bit hosts (DOS, Atari, Amiga, classic Mac OS) it falls
 * back to 32-bit `long`, limiting practical I/O to ~2 GB -- which is
 * acceptable on those platforms given their filesystem constraints.
 */
#if defined(_MSC_VER)
  typedef __int64 bp_off_t;
#elif defined(__GNUC__) || defined(__clang__)
  __extension__ typedef long long bp_off_t;
#elif (ULONG_MAX > 0xFFFFFFFFUL)
  typedef long bp_off_t; /* LP64: long is already 64 bits */
#else
  typedef long bp_off_t; /* 32-bit fallback for vintage platforms */
#endif

/* --- BoltPack version --------------------------------------------------- */
/* Fallback values for direct/standalone builds.  The Makefile overrides
 * these via -DBP_VERSION_MAJOR=N / -DBP_VERSION_MINOR=N. */
#ifndef BP_VERSION_MAJOR
#define BP_VERSION_MAJOR  1
#endif
#ifndef BP_VERSION_MINOR
# ifdef BP_MINI
#  define BP_VERSION_MINOR 3
# else
#  define BP_VERSION_MINOR 9
# endif
#endif

/* Version author message */
#define AUTHOR_MESSAGE  "Copyright (C) 2026 Tony Mattsson; Released under BSD-2-Clause"

/* --- Format constants --------------------------------------------------- */

/* This build writes and reads archive format 6 only. Every other version
 * is rejected before table or block data is interpreted. */
#define BP_FORMAT_VERSION      6

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

/*
 * File-table record fixed tail (the bytes after name_len + name).
 * v2: original_size(8) + compressed_size(8) + entry_type(1) + platform(1) +
 *     mtime(8) + offset(8)                                        = 34 bytes.
 * v3: v2 tail + method(1) + header_size(2) + checksum(4)          = 41 bytes.
 */
#define BP_TABLE_REC_TAIL_V2   34
#define BP_TABLE_REC_TAIL_V3   41
#define BP_TABLE_REC_TAIL_V6   43 /* v3 tail plus entry flags (2) */

/*
 * Byte offsets of the v3 CRC-32 fields inside the 32-byte archive header
 * (the v2 "reserved" area at offset 24).  table_crc covers the file-table
 * region; header_crc covers header bytes [0, BP_HEADER_CRC_OFF).
 */
#define BP_HEADER_TABLE_CRC_OFF   24
#define BP_HEADER_CRC_OFF         28

/* Compression methods */
#define BP_METHOD_STORED   0
#define BP_METHOD_BOLTLITE     1
#define BP_METHOD_BOLTLZ   2  /* BoltLZ format 2 */
#define BP_METHOD_FLUFF    3
#define BP_METHOD_ZSTD     4  /* Zstandard; modern targets only (BOLTPACK_ZSTD) */

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
#define BP_OS_OPENVMS      0x15
#define BP_OS_ZOS          0x16
#define BP_OS_WASM         0x17
#define BP_OS_NEXTSTEP     0x18
#define BP_OS_QNX          0x19
#define BP_OS_PLAN9        0x1A
#define BP_OS_TRU64        0x1B
#define BP_OS_SCO          0x1C
#define BP_OS_CPM86        0x1D  /* creator_os only, no frontend */
#define BP_OS_QL           0x1E  /* Sinclair QL (QDOS) */
#define BP_OS_WIN16        0x1F  /* Windows 3.x (16-bit) */
#define BP_OS_HURD         0x20
#define BP_OS_X68K         0x21  /* Sharp X68000 (Human68k) */
#define BP_META_PORTABLE    0xFE  /* BP_TYPE_META namespace, not a creator OS */
/* aliases used by archiver.c auto-detection and by frontend guards */
#define BP_OS_HAIKU        BP_OS_BEOS
#define BP_OS_MACOS        BP_OS_MACOSX
#define BP_OS_AMIGA        BP_OS_AMIGA_68K

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
#define BP_CPU_SPARC       0x0A
#define BP_CPU_ALPHA       0x0B
#define BP_CPU_PARISC      0x0C
#define BP_CPU_RISCV32     0x0D
#define BP_CPU_RISCV64     0x0E
#define BP_CPU_SH          0x0F  /* SuperH */
#define BP_CPU_6502        0x10  /* 6502 / 65C816 */
#define BP_CPU_Z80         0x11
#define BP_CPU_8086        0x12  /* 8086 / 8088 (16-bit x86) */
#define BP_CPU_E2K         0x13  /* Elbrus */
#define BP_CPU_ZARCH       0x14  /* z/Architecture */
#define BP_CPU_SPARC64     0x15
#define BP_CPU_IA64        0x16  /* Itanium */

/* Block header sizes (bytes preceding each compressed block) */
#define BP_BLOCK_HDR_V2   8   /* v2: comp_size(4) + orig_size(4) */
#define BP_BLOCK_HDR_V3  12   /* v3: comp_size(4) + orig_size(4) + block_crc(4) */

/* File-table section header sizes */
#define BP_TABLE_HDR_V2   8   /* v2: magic(4) + entry_count(4) */
#define BP_TABLE_HDR_V3  16   /* v3: magic(4) + entry_count(4) + total_orig(8) */

/* Entry flags field (entry header offset 40+N, 2 bytes) */
#define BP_ENTRY_FLAG_MODE_VALID  0x8000U /* bit 15: mode bits below are valid */
#define BP_ENTRY_FLAG_MTIME_VALID 0x4000U /* bit 14: mtime is known, including zero */
#define BP_ENTRY_FLAG_MODE_MASK   0x0FFFU /* bits 0-11: Unix mode bits (rwxrwxrwx + sticky/sgid/suid) */

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
#define BP_ERR_PARTIAL            -16 /* data or metadata could not be restored */

/* --- I/O callback interface --------------------------------------------- */

typedef struct {
    long     (*read)(void *handle, void *buf, bp_uint32 count);
    long     (*write)(void *handle, const void *buf, bp_uint32 count);
    int      (*seek)(void *handle, bp_off_t offset, int origin);
    bp_off_t (*tell)(void *handle);
    void    *handle;
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

static bp_uint64 bp_u64_from_ulong(unsigned long v)
{
    bp_uint64 r;
    r.lo = (bp_uint32)(v & 0xFFFFFFFFUL);
#if (ULONG_MAX > 0xFFFFFFFFUL)
    r.hi = (bp_uint32)((v >> 32) & 0xFFFFFFFFUL);
#else
    r.hi = 0;
#endif
    return r;
}

static unsigned long bp_u64_to_ulong(bp_uint64 v)
{
    unsigned long lo = v.lo;
    unsigned long hi = v.hi;

#if (ULONG_MAX > 0xFFFFFFFFUL)
    return (hi << 32) | lo;
#else
    /* Overflow for values > 4GB in 32-bit host; best-effort returns low 32 bits */
    (void)hi;
    return lo;
#endif
}

static bp_uint64 bp_u64_add(bp_uint64 a, bp_uint64 b)
{
    bp_uint64 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (bp_uint32)(r.lo < a.lo ? 1U : 0U);
    return r;
}

static bp_uint64 bp_u64_sub(bp_uint64 a, bp_uint64 b)
{
    bp_uint64 r;
    bp_uint32 borrow = (a.lo < b.lo) ? (bp_uint32)1 : (bp_uint32)0;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - borrow;
    return r;
}

static int bp_u64_is_zero(bp_uint64 v)
{
    return v.lo == 0 && v.hi == 0;
}

static int bp_u64_cmp(bp_uint64 a, bp_uint64 b)
{
    if (a.hi < b.hi) return -1;
    if (a.hi > b.hi) return 1;
    if (a.lo < b.lo) return -1;
    if (a.lo > b.lo) return 1;
    return 0;
}

static int bp_u64_gte(bp_uint64 a, bp_uint64 b)
{
    return bp_u64_cmp(a, b) >= 0;
}

static bp_uint32 bp_u64_to_u32(bp_uint64 v)
{
    return v.lo;
}

/* Convert a bp_uint64 archive offset to bp_off_t for seeking. Return -1
 * when it cannot be represented by the platform's signed offset type. */
static bp_off_t bp_u64_to_off(bp_uint64 v)
{
#if defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__) || (ULONG_MAX > 0xFFFFFFFFUL)
    if (v.hi & 0x80000000UL) return (bp_off_t)-1;
    return ((bp_off_t)v.hi << 32) | (bp_off_t)v.lo;
#else
    if (v.hi != 0 || v.lo > 0x7FFFFFFFUL) return (bp_off_t)-1;
    return (bp_off_t)v.lo;
#endif
}

/* Convert a bp_off_t (from io->tell) to bp_uint64 for storing in the archive. */
static bp_uint64 bp_off_to_u64(bp_off_t v)
{
    bp_uint64 r;
#if defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__) || (ULONG_MAX > 0xFFFFFFFFUL)
    r.lo = (bp_uint32)(v & (bp_off_t)0xFFFFFFFF);
    r.hi = (bp_uint32)((v >> 32) & (bp_off_t)0xFFFFFFFF);
#else
    r.lo = (bp_uint32)(unsigned long)v;
    r.hi = 0;
#endif
    return r;
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

/* --- Mini build limits (only visible when BP_MINI is defined) ----------- */
#ifdef BP_MINI
#  define BP_MINI_MAX_ENTRIES    65535U
#  define BP_MINI_MAX_BLOCK_SIZE 32768U
#  define BP_MINI_MAX_ARCHIVE_SIZE 2147483647UL
#endif

#endif /* BP_TYPES_H */
