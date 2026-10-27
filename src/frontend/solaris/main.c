/*
 * main.c -- Solaris / SunOS CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. Solaris 11 provides the directory-
 * relative POSIX interfaces used by the safer extraction path.
 *
 * Default CPU is x86-64; override with -DBP_CREATOR_CPU=BP_CPU_SPARC or
 * BP_CPU_SPARC64 when cross-compiling for SPARC.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_SOLARIS
#define BP_CREATOR_CPU BP_CPU_X86_64
#define BP_POSIX_USE_DIRFD_EXTRACTION 1
#include "../linux/main.c"
