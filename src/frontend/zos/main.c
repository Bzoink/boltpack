/*
 * main.c -- z/OS (USS) CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. z/OS 3.1 with the June 2024 refresh (or
 * later) provides the POSIX.1-2008 directory-relative APIs used by hardened
 * extraction. Expose z/OS O_DIRECTORY/O_NOFOLLOW flags with _XPLATFORM_SOURCE.
 * z/OS provides a POSIX.1 environment
 * through the Unix System Services (USS) subsystem; lstat, readlink,
 * opendir/readdir, symlink, and fseeko/ftello are all available.
 *
 * Important notes for z/OS:
 * - File data is stored and read in binary mode (fopen "rb"/"wb"), so
 *   BoltPack avoids EBCDIC translation of archive bytes.
 * - The shared POSIX frontend passes filename bytes through without
 *   conversion. The z/OS filename encoding and compiler execution mode have
 *   not been qualified; do not assume Unicode or cross-platform filename
 *   interoperability from this wrapper yet.
 *
 * z/OS runs exclusively on z/Architecture (IBM zSeries mainframe).
 * This build must be compiled on a z/OS USS host using IBM Open XL C/C++
 * (xlc) or z/OS XL C/C++ (cc). Cross-compilation from Linux is not
 * practical without a licensed IBM toolchain.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define _XPLATFORM_SOURCE 1
#define BP_CREATOR_OS  BP_OS_ZOS
#define BP_CREATOR_CPU BP_CPU_ZARCH
#define BP_POSIX_USE_DIRFD_EXTRACTION 1
#include "../linux/main.c"
