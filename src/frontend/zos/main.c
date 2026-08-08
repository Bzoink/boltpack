/*
 * main.c -- z/OS (USS) CLI frontend for BoltPack
 *
 * Thin wrapper around linux/main.c. z/OS provides a POSIX.1 environment
 * through the Unix System Services (USS) subsystem; lstat, readlink,
 * opendir/readdir, symlink, and fseeko/ftello are all available.
 *
 * Important notes for z/OS:
 * - File data is stored and read in binary mode (fopen "rb"/"wb"), so
 *   BoltPack avoids EBCDIC translation of archive bytes.
 * - File names in the archive are tagged as BP_CP_8859_1 (Latin-1), which
 *   is an approximation: USS filenames are typically in the locale code
 *   page (often IBM-1047 / EBCDIC).
 *
 * TODO: Verify codepage handling with a real z/OS locale. If the USS
 *       locale is EBCDIC-based, BP_CREATOR_CODEPAGE should be set to the
 *       appropriate IBM code page constant once one is defined.
 *
 * z/OS runs exclusively on z/Architecture (IBM zSeries mainframe).
 * This build must be compiled on a z/OS USS host using IBM Open XL C/C++
 * (xlc) or z/OS XL C/C++ (cc). Cross-compilation from Linux is not
 * practical without a licensed IBM toolchain.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#define BP_CREATOR_OS  BP_OS_ZOS
#define BP_CREATOR_CPU BP_CPU_ZARCH
#include "../linux/main.c"
