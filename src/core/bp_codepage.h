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
 * Licensed under the GNU General Public License v2
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
/* NOTE: BP_CP_SJIS supports ASCII and half-width katakana (U+FF61-U+FF9F).
 * Full JIS X 0208 double-byte (kanji/hiragana/katakana) mapping is not yet
 * implemented; double-byte sequences are approximated as '?' on conversion.
 * TODO: extend with complete JIS X 0208 table for full kanji support. */

/* ---- Conversion functions ---------------------------------------------- */

/*
 * Convert a local-encoded string to UTF-8.
 *
 * src:      null-terminated source string in the given codepage
 * dst:      output buffer for UTF-8 string (null-terminated)
 * dst_size: size of dst buffer in bytes
 * codepage: one of BP_CP_* constants
 *
 * Returns 0 on success, -1 on error (buffer too small or bad codepage).
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
 * Characters not representable in the target codepage are replaced
 * with '_'.  Returns 0 on success, -1 on error.
 */
int bp_utf8_to_local(const char *src, char *dst, int dst_size, int codepage);

#endif /* BP_CODEPAGE_H */
