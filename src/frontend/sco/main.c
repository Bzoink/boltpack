/*
 * main.c -- SCO OpenServer / UnixWare CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. SCO OpenServer 5/6 and UnixWare 7
 * provide a POSIX.1 environment with lstat, readlink, opendir/readdir,
 * and symlink available.
 *
 * SCO UNIX ran on x86 hardware only. The default CPU is x86 (32-bit);
 * override with -DBP_CREATOR_CPU=BP_CPU_X86_64 for UnixWare 7 x86-64
 * configurations.
 *
 * Note: SCO OpenServer is effectively end-of-life. Cross-compilation
 * requires a legacy SCO toolchain or a native SCO host.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_SCO
#define BP_CREATOR_CPU BP_CPU_X86
#define BP_REFUSE_PATH_RACE_EXTRACTION 1
#include "../linux/main.c"
