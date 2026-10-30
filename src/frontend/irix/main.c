/*
 * main.c -- IRIX CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. IRIX provides a POSIX.1 environment
 * with lstat, readlink, opendir/readdir, and symlink available.
 *
 * IRIX ran exclusively on MIPS hardware (R4000 through R16000).
 * The default CPU is MIPS64 (the most common production configuration);
 * override with -DBP_CREATOR_CPU=BP_CPU_MIPS32 for 32-bit builds.
 *
 * Cross-compile with mips-sgi-irix6.5-gcc from the free Nekoware / MIPSpro
 * SDK mirror, or build natively on IRIX 6.5.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_IRIX
#define BP_CREATOR_CPU BP_CPU_MIPS64
#define BP_REFUSE_PATH_RACE_EXTRACTION 1
#include "../linux/main.c"
