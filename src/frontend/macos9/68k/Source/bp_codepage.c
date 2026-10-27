/*
 * bp_codepage.c -- codepage <-> UTF-8 conversion for BoltPack
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "bp_codepage.h"
#include <string.h>

/* ======================================================================
 * Codepage tables -- each maps bytes 0x80-0xFF to Unicode codepoints.
 * ASCII (0x00-0x7F) is always identity.
 * ====================================================================== */

/* CP437 -- DOS US */
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_437
static const unsigned short cp437_to_unicode[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, /* 80 */
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, /* 88 */
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, /* 90 */
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, /* 98 */
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, /* A0 */
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, /* A8 */
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, /* B0 */
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, /* B8 */
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, /* C0 */
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, /* C8 */
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, /* D0 */
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, /* D8 */
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, /* E0 */
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, /* E8 */
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, /* F0 */
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0  /* F8 */
};
#endif

/* CP850 -- DOS Western Europe */
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_850
static const unsigned short cp850_to_unicode[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, /* 80 */
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, /* 88 */
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, /* 90 */
    0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192, /* 98 */
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, /* A0 */
    0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, /* A8 */
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2, 0x00C0, /* B0 */
    0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510, /* B8 */
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3, /* C0 */
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4, /* C8 */
    0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE, /* D0 */
    0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580, /* D8 */
    0x00D3, 0x00DF, 0x00D4, 0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE, /* E0 */
    0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD, 0x00AF, 0x00B4, /* E8 */
    0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8, /* F0 */
    0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0  /* F8 */
};
#endif

/* Windows-1252 */
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_1252
static const unsigned short cp1252_to_unicode[128] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, /* 80 */
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F, /* 88 */
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, /* 90 */
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178, /* 98 */
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, /* A0 */
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF, /* A8 */
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, /* B0 */
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF, /* B8 */
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, /* C0 */
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF, /* C8 */
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, /* D0 */
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF, /* D8 */
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, /* E0 */
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF, /* E8 */
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7, /* F0 */
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF  /* F8 */
};
#endif

/* MacRoman */
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_MACROMAN
static const unsigned short macroman_to_unicode[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, /* 80 */
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8, /* 88 */
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, /* 90 */
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, /* 98 */
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, /* A0 */
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, /* A8 */
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, /* B0 */
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, /* B8 */
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, /* C0 */
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, /* C8 */
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, /* D0 */
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, /* D8 */
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, /* E0 */
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, /* E8 */
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, /* F0 */
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7  /* F8 */
};
#endif

/* ISO 8859-1 -- identity for 0x80-0xFF (codepoints == byte values) */
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_8859_1
static const unsigned short iso8859_1_to_unicode[128] = {
    0x0080, 0x0081, 0x0082, 0x0083, 0x0084, 0x0085, 0x0086, 0x0087, /* 80 */
    0x0088, 0x0089, 0x008A, 0x008B, 0x008C, 0x008D, 0x008E, 0x008F, /* 88 */
    0x0090, 0x0091, 0x0092, 0x0093, 0x0094, 0x0095, 0x0096, 0x0097, /* 90 */
    0x0098, 0x0099, 0x009A, 0x009B, 0x009C, 0x009D, 0x009E, 0x009F, /* 98 */
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, /* A0 */
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF, /* A8 */
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, /* B0 */
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF, /* B8 */
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, /* C0 */
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF, /* C8 */
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, /* D0 */
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF, /* D8 */
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, /* E0 */
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF, /* E8 */
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7, /* F0 */
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF  /* F8 */
};
#endif

/* Atari ST -- Western European (factory ROM character set) */
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_ATARIST
static const unsigned short atarist_to_unicode[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, /* 80 */
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, /* 88 */
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, /* 90 */
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x00DF, 0x0192, /* 98 */
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, /* A0 */
    0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, /* A8 */
    0x00E3, 0x00F5, 0x00D8, 0x00F8, 0x0153, 0x0152, 0x00C0, 0x00C3, /* B0 */
    0x00D5, 0x00A8, 0x00B4, 0x2020, 0x00B6, 0x00A9, 0x00AE, 0x2122, /* B8 */
    0x0133, 0x0132, 0x05D0, 0x05D1, 0x05D2, 0x05D3, 0x05D4, 0x05D5, /* C0 */
    0x05D6, 0x05D7, 0x05D8, 0x05D9, 0x05DB, 0x05DC, 0x05DE, 0x05E0, /* C8 */
    0x05E1, 0x05E2, 0x05E4, 0x05E6, 0x05E7, 0x05E8, 0x05E9, 0x05EA, /* D0 */
    0x05DF, 0x05DA, 0x05DD, 0x05E3, 0x05E5, 0x00A7, 0x2038, 0x221E, /* D8 */
    0x03B1, 0x03B2, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, /* E0 */
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x222E, 0x03C6, 0x2208, 0x2229, /* E8 */
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, /* F0 */
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x00B3, 0x00AF  /* F8 */
};
#endif

/* ======================================================================
 * Table lookup helper
 * ====================================================================== */

static const unsigned short *get_table(int codepage)
{
    switch (codepage) {
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_437
    case BP_CP_437:      return cp437_to_unicode;
#endif
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_850
    case BP_CP_850:      return cp850_to_unicode;
#endif
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_1252
    case BP_CP_1252:     return cp1252_to_unicode;
#endif
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_MACROMAN
    case BP_CP_MACROMAN: return macroman_to_unicode;
#endif
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_8859_1
    case BP_CP_8859_1:   return iso8859_1_to_unicode;
#endif
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_ATARIST
    case BP_CP_ATARIST:   return atarist_to_unicode;
#endif
    default:             return NULL;
    }
}

#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_UTF8
static int bp_cp_valid_utf8(const char *src)
{
    size_t i = 0;
    while (src[i] != '\0') {
        unsigned char a = (unsigned char)src[i];
        if (a < 0x80) { i++; continue; }
        if (a >= 0xC2 && a <= 0xDF) {
            unsigned char b = (unsigned char)src[i + 1];
            if (b == 0 || (b & 0xC0) != 0x80) return 0;
            i += 2;
        } else if (a >= 0xE0 && a <= 0xEF) {
            unsigned char b = (unsigned char)src[i + 1];
            unsigned char c;
            if (b == 0 || (b & 0xC0) != 0x80 || src[i + 2] == '\0') return 0;
            c = (unsigned char)src[i + 2];
            if ((c & 0xC0) != 0x80 || (a == 0xE0 && b < 0xA0) ||
                (a == 0xED && b >= 0xA0)) return 0;
            i += 3;
        } else if (a >= 0xF0 && a <= 0xF4) {
            unsigned char b = (unsigned char)src[i + 1];
            unsigned char c, d;
            if (b == 0 || (b & 0xC0) != 0x80 || src[i + 2] == '\0' ||
                src[i + 3] == '\0') return 0;
            c = (unsigned char)src[i + 2]; d = (unsigned char)src[i + 3];
            if ((c & 0xC0) != 0x80 || (d & 0xC0) != 0x80 ||
                (a == 0xF0 && b < 0x90) || (a == 0xF4 && b > 0x8F)) return 0;
            i += 4;
        } else return 0;
    }
    return 1;
}
#endif

#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_SJIS
/* Classify a supplementary-plane sequence without reading past a NUL. */
static int bp_cp_four_byte(const char *src)
{
    unsigned char a = (unsigned char)src[0], b, c, d;
    if (a < 0xF0 || a > 0xF4 || src[1] == '\0') return BP_CP_INVALID;
    b = (unsigned char)src[1];
    if ((b & 0xC0) != 0x80 || (a == 0xF0 && b < 0x90) ||
        (a == 0xF4 && b > 0x8F) || src[2] == '\0') return BP_CP_INVALID;
    c = (unsigned char)src[2];
    if ((c & 0xC0) != 0x80 || src[3] == '\0') return BP_CP_INVALID;
    d = (unsigned char)src[3];
    if ((d & 0xC0) != 0x80) return BP_CP_INVALID;
    return BP_CP_UNREPRESENTABLE;
}
#endif

/* ======================================================================
 * local -> UTF-8
 * ====================================================================== */

int bp_local_to_utf8(const char *src, char *dst, int dst_size, int codepage)
{
    const unsigned short *tbl;
    int si, di;

    if (src == NULL || dst == NULL || dst_size < 1) return -1;

#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_UTF8
    /* UTF-8 pass-through */
    if (codepage == BP_CP_UTF8) {
        if (!bp_cp_valid_utf8(src)) return BP_CP_INVALID;
        if (strlen(src) >= (size_t)dst_size) return BP_CP_NO_SPACE;
        strcpy(dst, src);
        return 0;
    }

#endif

#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_SJIS
    /* Shift-JIS: multi-byte encoding; handled separately */
    if (codepage == BP_CP_SJIS) {
        int si2, di2;
        di2 = 0;
        for (si2 = 0; src[si2] != '\0'; ) {
            unsigned char c2 = (unsigned char)src[si2];
            if (c2 < 0x80) {
                /* ASCII: pass through */
                if (di2 + 1 >= dst_size) return BP_CP_NO_SPACE;
                dst[di2++] = (char)c2;
                si2++;
            } else if (c2 >= 0xA1 && c2 <= 0xDF) {
                /* Half-width katakana: U+FF61..U+FF9F (3-byte UTF-8) */
                unsigned short u2 = (unsigned short)(0xFF61 + (c2 - 0xA1));
                if (di2 + 3 >= dst_size) return BP_CP_NO_SPACE;
                dst[di2++] = (char)(0xE0 | (u2 >> 12));
                dst[di2++] = (char)(0x80 | ((u2 >> 6) & 0x3F));
                dst[di2++] = (char)(0x80 | (u2 & 0x3F));
                si2++;
            } else {
                /* Double-byte Shift-JIS is not supported by this table. */
                return BP_CP_UNREPRESENTABLE;
            }
        }
        dst[di2] = '\0';
        return 0;
    }

#endif

    tbl = get_table(codepage);
    if (tbl == NULL) return -1;

    di = 0;
    for (si = 0; src[si] != '\0'; si++) {
        unsigned char c = (unsigned char)src[si];
        unsigned short u;

        if (c < 0x80) {
            /* ASCII -- direct copy */
            if (di + 1 >= dst_size) return BP_CP_NO_SPACE;
            dst[di++] = (char)c;
        } else {
            u = tbl[c - 0x80];
            if (u < 0x0080) {
                if (di + 1 >= dst_size) return BP_CP_NO_SPACE;
                dst[di++] = (char)u;
            } else if (u < 0x0800) {
                if (di + 2 >= dst_size) return BP_CP_NO_SPACE;
                dst[di++] = (char)(0xC0 | (u >> 6));
                dst[di++] = (char)(0x80 | (u & 0x3F));
            } else {
                if (di + 3 >= dst_size) return BP_CP_NO_SPACE;
                dst[di++] = (char)(0xE0 | (u >> 12));
                dst[di++] = (char)(0x80 | ((u >> 6) & 0x3F));
                dst[di++] = (char)(0x80 | (u & 0x3F));
            }
        }
    }

    dst[di] = '\0';
    return 0;
}

/* ======================================================================
 * UTF-8 -> local
 * ====================================================================== */

int bp_utf8_to_local(const char *src, char *dst, int dst_size, int codepage)
{
    const unsigned short *tbl;
    int si, di, j;

    if (src == NULL || dst == NULL || dst_size < 1) return -1;

#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_UTF8
    /* UTF-8 pass-through */
    if (codepage == BP_CP_UTF8) {
        if (!bp_cp_valid_utf8(src)) return BP_CP_INVALID;
        if (strlen(src) >= (size_t)dst_size) return BP_CP_NO_SPACE;
        strcpy(dst, src);
        return 0;
    }

#endif

#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_SJIS
    /* Shift-JIS: multi-byte; handle separately */
    if (codepage == BP_CP_SJIS) {
        int si2, di2;
        di2 = 0;
        for (si2 = 0; src[si2] != '\0'; ) {
            unsigned char c2 = (unsigned char)src[si2];
            unsigned short u2;
            if (di2 + 1 >= dst_size) return BP_CP_NO_SPACE;
            if (c2 < 0x80) {
                dst[di2++] = (char)c2;
                si2++;
                continue;
            }
            /* Decode UTF-8 to codepoint u2 */
            if ((c2 & 0xE0) == 0xC0) {
                if (src[si2 + 1] == '\0' ||
                    ((unsigned char)src[si2 + 1] & 0xC0) != 0x80) return -1;
                u2 = (unsigned short)(((c2 & 0x1F) << 6) |
                     ((unsigned char)src[si2 + 1] & 0x3F));
                if (u2 < 0x80) return BP_CP_INVALID;
                si2 += 2;
            } else if ((c2 & 0xF0) == 0xE0) {
                if (src[si2 + 1] == '\0' || src[si2 + 2] == '\0' ||
                    ((unsigned char)src[si2 + 1] & 0xC0) != 0x80 ||
                    ((unsigned char)src[si2 + 2] & 0xC0) != 0x80) return -1;
                u2 = (unsigned short)(((c2 & 0x0F) << 12) |
                     (((unsigned char)src[si2 + 1] & 0x3F) << 6) |
                     ((unsigned char)src[si2 + 2] & 0x3F));
                if (u2 < 0x800 || (u2 >= 0xD800 && u2 <= 0xDFFF))
                    return BP_CP_INVALID;
                si2 += 3;
            } else if ((c2 >= 0x81 && c2 <= 0x9F) ||
                       (c2 >= 0xE0 && c2 <= 0xEF)) {
                return BP_CP_UNREPRESENTABLE;
            } else {
                return BP_CP_INVALID;
            }
            /* Half-width katakana U+FF61-U+FF9F -> single SJIS byte 0xA1-0xDF */
            if (u2 >= 0xFF61 && u2 <= 0xFF9F) {
                dst[di2++] = (char)(0xA1 + (u2 - 0xFF61));
            } else {
                return BP_CP_UNREPRESENTABLE;
            }
        }
        dst[di2] = '\0';
        return 0;
    }

#endif

    tbl = get_table(codepage);
    if (tbl == NULL) return -1;

    di = 0;
    for (si = 0; src[si] != '\0'; ) {
        unsigned char c = (unsigned char)src[si];
        unsigned short u;

        if (di + 1 >= dst_size) return BP_CP_NO_SPACE;

        if (c < 0x80) {
            /* ASCII */
            dst[di++] = (char)c;
            si++;
            continue;
        }

        /* Decode UTF-8 to Unicode codepoint */
        if ((c & 0xE0) == 0xC0) {
            if (src[si + 1] == '\0' ||
                ((unsigned char)src[si + 1] & 0xC0) != 0x80) return -1;
            u = ((unsigned short)(c & 0x1F) << 6)
              | ((unsigned short)((unsigned char)src[si + 1]) & 0x3F);
            if (u < 0x80) return BP_CP_INVALID;
            si += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (src[si + 1] == '\0' || src[si + 2] == '\0' ||
                ((unsigned char)src[si + 1] & 0xC0) != 0x80 ||
                ((unsigned char)src[si + 2] & 0xC0) != 0x80) return -1;
            u = ((unsigned short)(c & 0x0F) << 12)
              | (((unsigned short)((unsigned char)src[si + 1]) & 0x3F) << 6)
              | ((unsigned short)((unsigned char)src[si + 2]) & 0x3F);
            if (u < 0x800 || (u >= 0xD800 && u <= 0xDFFF))
                return BP_CP_INVALID;
            si += 3;
        } else {
            /* Four-byte characters have no single-byte mapping. */
#if !defined(BP_MINI) || BP_MINI_CODEPAGE == BP_CP_SJIS
            return bp_cp_four_byte(src + si);
#else
            return BP_CP_UNREPRESENTABLE;
#endif
        }

        /* If codepoint falls in 0x80-0xFF and equals its byte value
         * for the identity range, handle it. Otherwise, search the table. */
        for (j = 0; j < 128; j++) {
            if (tbl[j] == u) {
                dst[di++] = (unsigned char)(0x80 + j);
                goto found;
            }
        }
        /* No exact representation in this codepage. */
        return BP_CP_UNREPRESENTABLE;
    found:
        ;
    }

    dst[di] = '\0';
    return 0;
}
