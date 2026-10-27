/*
 * bp_codepage.h -- codepage <-> UTF-8 conversion for BoltPack
 *
 * Provides portable conversion between legacy single-byte codepages
 * and UTF-8.  Each codepage is a 128-entry table mapping bytes 0x80-0xFF
 * to Unicode codepoints; ASCII (0x00-0x7F) is identity-mapped.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_CODEPAGE_H
#define BP_CODEPAGE_H

#include "bp_types.h"

/* ---- Codepage identifiers ---------------------------------------------- */

#define BP_CP_UTF8       0   /* pass-through, no conversion needed         */
#define BP_CP_437        1   /* DOS US (CP437)                             */
#define BP_CP_850        2   /* DOS Western Europe (CP850)                 */
#define BP_CP_1252       3   /* Windows Latin-1 (Windows-1252)             */
#define BP_CP_MACROMAN   4   /* Mac OS Roman                               */
#define BP_CP_8859_1     5   /* ISO 8859-1 (Latin-1)                       */
#define BP_CP_ATARIST    6   /* Atari ST (Western European, factory ROM)    */
#define BP_CP_SJIS       7   /* Shift-JIS (Japanese; X68000 Human68k)       */
/* Mini builds use one configured local encoding to conserve 16-bit space. */
#ifndef BP_MINI_CODEPAGE
# if defined(BP_CREATOR_OS) && BP_CREATOR_OS == BP_OS_OS2
#  define BP_MINI_CODEPAGE BP_CP_850
# elif defined(BP_CREATOR_OS) && BP_CREATOR_OS == BP_OS_CPM86
#  define BP_MINI_CODEPAGE (-1)
# elif defined(BP_CREATOR_OS) && BP_CREATOR_OS == BP_OS_LINUX
#  define BP_MINI_CODEPAGE BP_CP_UTF8
# else
#  define BP_MINI_CODEPAGE BP_CP_437
# endif
#endif

/* NOTE: BP_CP_SJIS supports ASCII and half-width katakana (U+FF61-U+FF9F).
 * Full JIS X 0208 double-byte (kanji/hiragana/katakana) mapping is not yet
 * implemented; double-byte sequences return BP_CP_UNREPRESENTABLE.
 * TODO: extend with complete JIS X 0208 table for full kanji support. */

/* Nonzero results remain compatible with existing callers. */
#define BP_CP_INVALID         (-1)
#define BP_CP_UNREPRESENTABLE (-2)
#define BP_CP_NO_SPACE        (-3)

/* ---- Conversion functions ---------------------------------------------- */

/*
 * Convert a local-encoded string to UTF-8.
 *
 * src:      null-terminated source string in the given codepage
 * dst:      output buffer for UTF-8 string (null-terminated)
 * dst_size: size of dst buffer in bytes
 * codepage: one of BP_CP_* constants
 *
 * Returns 0 on success, BP_CP_UNREPRESENTABLE if unsupported, BP_CP_NO_SPACE
 * if the buffer is too small, or BP_CP_INVALID for invalid input or codepage.
 * If codepage is BP_CP_UTF8, src is copied verbatim.
 */
int bp_local_to_utf8(const char *src, char *dst, int dst_size, int codepage);

/*
 * Convert a UTF-8 string to a local codepage.
 *
 * src:      null-terminated UTF-8 source string
 * dst:      output buffer for local-encoded string (null-terminated)
 * dst_size: size of dst buffer in bytes
 * codepage: one of BP_CP_* constants
 *
 * Returns 0 for an exact conversion, BP_CP_UNREPRESENTABLE if a character
 * cannot be represented, BP_CP_NO_SPACE if the buffer is too small, or
 * BP_CP_INVALID for invalid input or codepage.
 */
int bp_utf8_to_local(const char *src, char *dst, int dst_size, int codepage);

/* Convert a native single-byte path to a case-folded key for filesystems
 * whose configured legacy codepage defines case-insensitive names. ASCII is
 * supported for every codepage; CP437 and CP850 also fold their mapped Latin
 * letter pairs. Other non-ASCII codepages return BP_CP_UNREPRESENTABLE rather
 * than claiming collision-safe comparison without a target case map. */
int bp_local_casefold(const char *src, char *dst, int dst_size, int codepage);

#endif /* BP_CODEPAGE_H */
