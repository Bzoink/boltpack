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

/* Extraction: this system has no openat() family, so extraction is by pathname,
 * which cannot stop another process from swapping a directory mid-extraction.
 * Extraction therefore only writes into a new directory that boltpack creates
 * itself (-o DIR, which must not exist) private to the user (mode 0700), and
 * gives it normal permissions when finished (see BP_PRIVATE_ROOT_EXTRACTION in
 * linux/main.c).  Listing, testing, dry-run, create, add and delete are
 * unaffected.  Not yet run on the real system. */

#define BP_CREATOR_OS  BP_OS_HPUX
#define BP_CREATOR_CPU BP_CPU_PARISC
#define BP_PRIVATE_ROOT_EXTRACTION 1
#include "../linux/main.c"
