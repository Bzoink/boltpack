/*
 * volume_size.h -- parse volume sizes such as "1.44M" or "700MiB"
 *
 * Units: K, M, G, T are powers of 1000; Ki, Mi, Gi, Ti are powers of 1024.
 * Case does not matter, a trailing "B" is allowed, and a decimal fraction
 * may be given ("1.44M" = 1,440,000 bytes).  A plain number is bytes.
 * Decimal units are the default because media are labelled that way and the
 * resulting sizes fit freshly formatted media: "1.44M" fits a 1.44 MB DOS
 * diskette (1,457,664 bytes free) where 1.44 MiB would not.
 *
 * Header-only, ANSI C (C89/C90), 16-bit int safe.
 *
 * With BP_VOLUME_SIZE_32 defined, the header instead provides the smaller
 * bp_parse_volume_size32(), for frontends whose volumes stay below 4 GiB:
 * the same syntax, fractions to six digits (further digits are ignored),
 * and -1 for any size of 4 GiB or more.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_VOLUME_SIZE_H
#define BP_VOLUME_SIZE_H

#include "../../core/bp_types.h"

/* Smallest volume a frontend accepts (16K = 16,000 bytes). */
#define BP_VOLUME_SIZE_MIN 16000UL

static int bp_vs_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* The unit suffix at p: sets the power of the base (0 = bytes) and the
 * base, and returns 0, or -1 if anything follows the unit. */
static int bp_vs_unit(const char *p, int *power, bp_uint32 *base)
{
    *power = 0;
    switch (bp_vs_lower(*p)) {
    case 'k': *power = 1; p++; break;
    case 'm': *power = 2; p++; break;
    case 'g': *power = 3; p++; break;
    case 't': *power = 4; p++; break;
    default: break;
    }
    *base = 1000UL;
    if (*power > 0 && bp_vs_lower(*p) == 'i') {
        *base = 1024UL;
        p++;
    }
    if (bp_vs_lower(*p) == 'b') p++;
    return *p == '\0' ? 0 : -1;
}

#ifdef BP_VOLUME_SIZE_32

/* Parse s into *bytes.  Returns 0 on success, -1 on a syntax error or a
 * size of 4 GiB or more.  The minimum size is left to the caller. */
static int bp_parse_volume_size32(const char *s, bp_uint32 *bytes)
{
    bp_uint32 total = 0, frac = 0, scale = 1, base;
    int digits = 0, frac_digits = 0, power, i;
    const char *p = s;

    if (s == NULL || bytes == NULL) return -1;
    while (*p >= '0' && *p <= '9') {
        bp_uint32 d = (bp_uint32)(*p - '0');
        if (total > (0xFFFFFFFFUL - d) / 10UL) return -1;
        total = total * 10UL + d;
        p++;
        digits++;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (frac_digits < 6) {
                frac = frac * 10UL + (bp_uint32)(*p - '0');
                scale *= 10UL;
                frac_digits++;
            }
            p++;
            digits++;
        }
    }
    if (digits == 0 || bp_vs_unit(p, &power, &base) != 0) return -1;
    /* total + frac / scale, multiplied by the base `power` times; the
     * fraction carries its whole part into total at each step (exact, as
     * frac * base < 10^6 * 1024 fits) */
    for (i = 0; i < power; i++) {
        bp_uint32 carry;
        if (total > 0xFFFFFFFFUL / base) return -1;
        frac *= base;
        carry = frac / scale;
        frac %= scale;
        total *= base;
        if (total > 0xFFFFFFFFUL - carry) return -1;
        total += carry;
    }
    *bytes = total;
    return 0;
}

#else /* !BP_VOLUME_SIZE_32 */

/* a * b for a 64-bit a and 32-bit b; returns 0 on overflow */
static int bp_vs_mul(bp_uint64 a, bp_uint32 b, bp_uint64 *out)
{
    bp_uint32 al[4], bl[2];
    bp_uint32 r[6];
    int i, j;

    al[0] = a.lo & 0xFFFFUL; al[1] = (a.lo >> 16) & 0xFFFFUL;
    al[2] = a.hi & 0xFFFFUL; al[3] = (a.hi >> 16) & 0xFFFFUL;
    bl[0] = b & 0xFFFFUL;    bl[1] = (b >> 16) & 0xFFFFUL;
    for (i = 0; i < 6; i++) r[i] = 0;
    for (i = 0; i < 4; i++) {
        bp_uint32 carry = 0;
        for (j = 0; j < 2; j++) {
            bp_uint32 t = al[i] * bl[j];          /* < 2^32 */
            bp_uint32 lo = t & 0xFFFFUL;
            bp_uint32 hi = (t >> 16) & 0xFFFFUL;
            bp_uint32 s = r[i + j] + lo + carry;  /* < 2^18 */
            r[i + j] = s & 0xFFFFUL;
            carry = (s >> 16) + hi;
        }
        for (j = i + 2; carry != 0 && j < 6; j++) {
            bp_uint32 s = r[j] + carry;
            r[j] = s & 0xFFFFUL;
            carry = s >> 16;
        }
    }
    if (r[4] != 0 || r[5] != 0) return 0;
    out->lo = r[0] | (r[1] << 16);
    out->hi = r[2] | (r[3] << 16);
    return 1;
}

/* a / d for a 64-bit a and nonzero 32-bit d (bitwise long division) */
static bp_uint64 bp_vs_div(bp_uint64 a, bp_uint32 d)
{
    bp_uint64 q, r, dd;
    int i;

    q.lo = q.hi = 0;
    r.lo = r.hi = 0;
    dd.lo = d;
    dd.hi = 0;
    for (i = 63; i >= 0; i--) {
        bp_uint32 bit = i >= 32 ? (a.hi >> (i - 32)) & 1UL : (a.lo >> i) & 1UL;
        r = bp_u64_add(r, r);
        r.lo |= bit;
        if (bp_u64_cmp(r, dd) >= 0) {
            r = bp_u64_sub(r, dd);
            if (i >= 32) q.hi |= 1UL << (i - 32);
            else q.lo |= 1UL << i;
        }
    }
    return q;
}

/* Parse s into *bytes.  Returns 0 on success, -1 on a syntax error or a
 * value too large for 64 bits.  The minimum size is left to the caller. */
static int bp_parse_volume_size(const char *s, bp_uint64 *bytes)
{
    bp_uint64 whole, total, part;
    bp_uint32 frac = 0, frac_scale = 1, base;
    int digits = 0, frac_digits = 0, power = 0;
    const char *p = s;

    if (s == NULL || bytes == NULL) return -1;
    whole.lo = whole.hi = 0;
    while (*p >= '0' && *p <= '9') {
        if (!bp_vs_mul(whole, 10UL, &whole)) return -1;
        whole = bp_u64_add(whole, bp_u64_from_u32((bp_uint32)(*p - '0')));
        if (whole.hi == 0xFFFFFFFFUL) return -1;
        p++;
        digits++;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (frac_digits < 9) {
                frac = frac * 10UL + (bp_uint32)(*p - '0');
                frac_scale *= 10UL;
                frac_digits++;
            }
            p++;
            digits++;
        }
    }
    if (digits == 0) return -1;

    if (bp_vs_unit(p, &power, &base) != 0) return -1;

    /* whole * base^power + frac * base^power / frac_scale */
    total = whole;
    part = bp_u64_from_u32(frac);
    {
        int i;
        for (i = 0; i < power; i++) {
            if (!bp_vs_mul(total, base, &total)) return -1;
            if (!bp_vs_mul(part, base, &part)) return -1;
        }
    }
    part = bp_vs_div(part, frac_scale);
    total = bp_u64_add(total, part);
    if (bp_u64_cmp(total, part) < 0) return -1;
    *bytes = total;
    return 0;
}

#endif /* BP_VOLUME_SIZE_32 */

#endif /* BP_VOLUME_SIZE_H */
