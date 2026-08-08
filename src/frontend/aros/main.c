/*
 * main.c -- AROS frontend for BoltPack
 *
 * Thin wrapper around amiga/main.c. AROS (Amiga Research OS) is an
 * open-source reimplementation of the AmigaOS API; Lock, Examine,
 * SetProtection, SetComment, MakeLink, and opendir/readdir are all
 * available via the AROS SDK.
 *
 * Cross-compile with the AROS SDK cross-compiler:
 *   - x86_64: aros-x86_64-gcc -ansi -Wall -O2 ...
 *   - i386:   aros-i386-gcc   -ansi -Wall -O2 ...
 *   - ARM:    aros-arm-gcc    -ansi -Wall -O2 ...
 *
 * AROS runs on x86, x86-64, ARM, and (experimentally) PPC and m68k.
 * The default CPU is x86-64 (the most widely used AROS port); override
 * with -DBP_CREATOR_CPU=BP_CPU_X86 or BP_CPU_ARM32 as appropriate.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#define BP_CREATOR_OS  BP_OS_AROS
#define BP_CREATOR_CPU BP_CPU_X86_64
#include "../amiga/main.c"
