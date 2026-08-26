/*
 * main.c -- AmigaOS 4.x PPC frontend for BoltPack
 *
 * Thin wrapper around amiga/main.c. AmigaOS 4.x is a PPC-only OS that
 * uses the same AmigaDOS API as AmigaOS 2.x/3.x: Lock, Examine,
 * SetProtection, SetComment, MakeLink, and opendir/readdir (via newlib).
 *
 * Cross-compile with adtools (https://github.com/sba1/adtools):
 *   ppc-amigaos-gcc -athread=native -ansi -Wall -O2 ...
 *
 * AmigaOS 4.x runs exclusively on PowerPC hardware (PPC603e through G4).
 * The default CPU is PPC32 (all AmigaOS 4.x hardware is 32-bit PPC).
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_AMIGA_PPC
#define BP_CREATOR_CPU BP_CPU_PPC32
#include "../amiga/main.c"
