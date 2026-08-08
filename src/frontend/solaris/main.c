/*
 * main.c -- Solaris / SunOS CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. Solaris provides a full POSIX.1-2001
 * environment; lstat, readlink, opendir/readdir, and symlink are all
 * available unchanged.
 *
 * Default CPU is x86-64; override with -DBP_CREATOR_CPU=BP_CPU_SPARC or
 * BP_CPU_SPARC64 when cross-compiling for SPARC.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#define BP_CREATOR_OS  BP_OS_SOLARIS
#define BP_CREATOR_CPU BP_CPU_X86_64
#include "../linux/main.c"
