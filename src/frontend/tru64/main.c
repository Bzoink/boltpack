/*
 * main.c -- Tru64 UNIX (Digital UNIX / OSF/1) CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. Tru64 UNIX provides a POSIX.1
 * environment with lstat, readlink, opendir/readdir, and symlink available.
 *
 * Tru64 UNIX ran exclusively on DEC / Compaq / HP Alpha hardware.
 * The default CPU is Alpha; there is no other CPU option for this OS.
 *
 * Note: Tru64 UNIX is end-of-life (final release 2004). Cross-compilation
 * requires a legacy Alpha-Linux cross-compiler or a native Alpha system.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_TRU64
#define BP_CREATOR_CPU BP_CPU_ALPHA
#include "../linux/main.c"
