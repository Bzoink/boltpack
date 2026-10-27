/*
 * main.c -- illumos CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. illumos is an open-source fork of
 * OpenSolaris and provides the directory-relative POSIX interfaces used by
 * the safer extraction path.
 *
 * illumos distributions (OpenIndiana, OmniOS, SmartOS) ship with GCC and
 * a full POSIX runtime, so the Linux wrapper is a direct fit.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_SOLARIS
#define BP_CREATOR_CPU BP_CPU_X86_64
#define BP_POSIX_USE_DIRFD_EXTRACTION 1
#include "../linux/main.c"
