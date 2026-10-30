/*
 * main.c -- QNX Neutrino CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. QNX Neutrino provides a full
 * POSIX.1-2001 environment; lstat, readlink, opendir/readdir, symlink,
 * and fseeko/ftello are all available unchanged.
 * Refuse extraction: QNX's filesystem resource-manager model does not make
 * multi-component openat traversal race-free, and path checks cannot close the
 * concurrent parent-substitution window. Listing, testing, and dry-run remain
 * available until a safe native traversal strategy is established.
 *
 * QNX Neutrino runs on x86, x86-64, ARM, and PPC. The default CPU is
 * x86; override with -DBP_CREATOR_CPU=BP_CPU_X86_64, BP_CPU_ARM32, or
 * BP_CPU_ARM64 as appropriate.
 *
 * Cross-compile using the QNX SDP (Software Development Platform); the
 * QNX Momentics IDE ships qcc / q++ for each supported target.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_QNX
#define BP_REFUSE_PATH_RACE_EXTRACTION 1
#define BP_CREATOR_CPU BP_CPU_X86
#include "../linux/main.c"
