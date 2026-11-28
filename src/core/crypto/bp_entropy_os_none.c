/*
 * bp_entropy_os_none.c -- stand-in for platforms without a known OS random
 * source: the pool then needs imported, hardware and gathered entropy.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#include "bp_entropy.h"

int bp_entropy_os_read(bp_uint8 *buf, bp_uint32 n)
{
    (void)buf; (void)n;
    return -1;
}
