/*
 * main.c -- FreeBSD CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. All POSIX I/O, directory recursion,
 * and symlink handling is identical to the Linux frontend.
 *
 * Default CPU is x86-64; override with -DBP_CREATOR_CPU=BP_CPU_xxx when
 * cross-compiling for ARM64, i386, etc.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#define BP_CREATOR_OS  BP_OS_FREEBSD
#define BP_CREATOR_CPU BP_CPU_X86_64
#include "../linux/main.c"
