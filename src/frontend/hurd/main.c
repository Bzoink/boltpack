/*
 * main.c -- GNU Hurd CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. GNU Hurd provides a POSIX.1 environment
 * via the GNU C Library (glibc); lstat, readlink, opendir/readdir, symlink,
 * and fseeko/ftello are all available unchanged.
 *
 * Hurd runs on x86 (and experimentally x86-64). The default CPU is x86-64;
 * override with -DBP_CREATOR_CPU=BP_CPU_X86 for 32-bit Hurd builds.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_HURD
#define BP_CREATOR_CPU BP_CPU_X86_64
#include "../linux/main.c"
