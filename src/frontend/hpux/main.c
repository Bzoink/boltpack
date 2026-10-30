/*
 * main.c -- HP-UX CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. HP-UX provides a POSIX.1 environment
 * with lstat, readlink, opendir/readdir, and symlink available.
 *
 * HP-UX shipped on PA-RISC hardware (PA-7xxx, PA-8xxx) and later on
 * Itanium (IA-64). The default CPU is PA-RISC; override with
 * -DBP_CREATOR_CPU=BP_CPU_IA64 for Itanium builds.
 *
 * Cross-compile with hppa-hp-hpux-gcc, or build natively on HP-UX 11i.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_HPUX
#define BP_CREATOR_CPU BP_CPU_PARISC
#define BP_REFUSE_PATH_RACE_EXTRACTION 1
#include "../linux/main.c"
