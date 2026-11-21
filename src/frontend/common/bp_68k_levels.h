/*
 * bp_68k_levels.h -- compression levels for the 68000-class frontends
 * (Amiga, Atari). Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
 *
 * bp_archive_t.method_level is 0 (the codec's default) unless a frontend sets
 * it. On a 68000 at 7 to 8 MHz:
 *
 *  - Fluff runs at 1.1 to 1.4 KB/s at every level (measured in FS-UAE on
 *    Calgary files: level 1 1.36 KB/s, 3 1.34, 6 1.21, 8 1.13 KB/s), so the
 *    codec's default level 8 gives nothing for its time. Level 3 is the
 *    knee: 19% faster than level 8 for output 1% larger (27.3% against
 *    27.0%). Level 1 is no faster and 8% larger, so there is no lighter
 *    Fluff setting worth offering.
 *  - BoltLZ Fast (level 1) compresses 1.6 times as fast as Balanced for
 *    output 7% larger, and decompresses at the same speed.
 *
 * "Light" therefore means BoltLZ Fast. The archive format does not record
 * the level, so every reader decodes these archives.
 */
#ifndef BP_68K_LEVELS_H
#define BP_68K_LEVELS_H

#include "../../core/bp_types.h"

#define BP_68K_FLUFF_LEVEL 3

/* The level to set after bp_archive_create for the given method. */
static bp_uint8 bp_68k_method_level(bp_uint8 method, int light)
{
    if (method == BP_METHOD_BOLTLZ) return light ? 1 : 0;
    if (method == BP_METHOD_FLUFF) return BP_68K_FLUFF_LEVEL;
    return 0;
}

#endif
