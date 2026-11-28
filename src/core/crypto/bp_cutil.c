/*
 * bp_cutil.c -- wipe and constant-time compare.  Strict C89.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#include <string.h>
#include "bp_crypto.h"

/* Calling memset through a volatile pointer keeps the compiler from removing
 * the wipe of a buffer that is never read again. */
static void *(*volatile bp_memset_v)(void *, int, size_t) = memset;

void bp_wipe(void *p, bp_uint32 n)
{
    bp_uint8 *q = (bp_uint8 *)p;
    if (q == NULL) return;
    while (n != 0) {
        /* where size_t is 16 bits (DOS, 16-bit Windows) a long count is wiped in pieces */
        size_t chunk = (sizeof(size_t) < 4 && n > 0x4000UL) ? (size_t)0x4000U : (size_t)n;
        bp_memset_v(q, 0, chunk);
        q += chunk;
        n -= (bp_uint32)chunk;
    }
}

int bp_ct_equal(const bp_uint8 *a, const bp_uint8 *b, bp_uint32 n)
{
    volatile bp_uint8 d = 0;
    bp_uint32 i;
    for (i = 0; i < n; i++) d = (bp_uint8)(d | (a[i] ^ b[i]));
    return d == 0;
}
