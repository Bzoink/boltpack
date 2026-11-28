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
    if (p != NULL && n != 0) bp_memset_v(p, 0, (size_t)n);
}

int bp_ct_equal(const bp_uint8 *a, const bp_uint8 *b, bp_uint32 n)
{
    volatile bp_uint8 d = 0;
    bp_uint32 i;
    for (i = 0; i < n; i++) d = (bp_uint8)(d | (a[i] ^ b[i]));
    return d == 0;
}
