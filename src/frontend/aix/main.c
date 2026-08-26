/*
 * main.c -- AIX CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. AIX provides a POSIX.1 environment
 * with lstat, readlink, opendir/readdir, and symlink available.
 *
 * AIX ran on PowerPC (and earlier POWER) hardware. The default CPU is
 * PowerPC 64-bit; override with -DBP_CREATOR_CPU=BP_CPU_PPC32 for
 * 32-bit AIX builds.
 *
 * Cross-compile with powerpc-ibm-aix-gcc from the AIX GCC host toolchain,
 * or build natively on AIX 6.1 / 7.x.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_AIX
#define BP_CREATOR_CPU BP_CPU_PPC64
#include "../linux/main.c"
