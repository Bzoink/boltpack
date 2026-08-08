/*
 * main.c -- MorphOS PPC frontend for BoltPack
 *
 * Thin wrapper around amiga/main.c. MorphOS is a PPC-based OS that
 * maintains AmigaOS API compatibility; Lock, Examine, SetProtection,
 * SetComment, MakeLink, and opendir/readdir (via libnix/clib2) are
 * all available unchanged.
 *
 * Cross-compile with the MorphOS SDK (ppc-morphos-gcc):
 *   ppc-morphos-gcc -noixemul -ansi -Wall -O2 ...
 *
 * MorphOS runs on PowerPC hardware (Pegasos, Efika, and G4/G5 Macs
 * running MorphOS 3.x). All MorphOS hardware is 32-bit PPC.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#define BP_CREATOR_OS  BP_OS_MORPHOS
#define BP_CREATOR_CPU BP_CPU_PPC32
#include "../amiga/main.c"
