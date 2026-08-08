/*
 * main.c -- NeXTSTEP / OPENSTEP CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. NeXTSTEP and OPENSTEP provide a BSD
 * POSIX environment; lstat, readlink, opendir/readdir, and symlink are
 * all available.
 *
 * NeXTSTEP shipped on 68k (NeXT Cube/Slab) and later on x86, SPARC, and
 * PA-RISC. OPENSTEP shipped on x86 and SPARC. The default CPU is 68k
 * (the original and most historically significant target); override with
 * -DBP_CREATOR_CPU=BP_CPU_X86, BP_CPU_SPARC, or BP_CPU_PARISC.
 *
 * Note: NeXTSTEP / OPENSTEP are end-of-life. Cross-compilation requires
 * a legacy toolchain or a native NeXT / OPENSTEP host.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#define BP_CREATOR_OS  BP_OS_NEXTSTEP
#define BP_CREATOR_CPU BP_CPU_M68K
#include "../linux/main.c"
