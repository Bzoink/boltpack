/*
 * main.c -- illumos CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. illumos is an open-source fork of
 * OpenSolaris and provides the same POSIX.1-2001 environment; all POSIX
 * I/O, directory recursion, and symlink handling is identical to the
 * Linux frontend.
 *
 * illumos distributions (OpenIndiana, OmniOS, SmartOS) ship with GCC and
 * a full POSIX runtime, so the Linux wrapper is a direct fit.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define BP_CREATOR_OS  BP_OS_SOLARIS
#define BP_CREATOR_CPU BP_CPU_X86_64
#include "../linux/main.c"
