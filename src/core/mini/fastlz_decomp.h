/*
 * fastlz_decomp.h -- Self-contained fastlz decompressor for 16-bit targets
 *
 * Public interface for bp_fastlz_decompress().  This module re-implements
 * the decompression side of the fastlz (levels 1 and 2) format in pure
 * ANSI C89, without dependency on the vendored lib/fastlz library.
 *
 * The vendored fastlz compressor is not compiled for 16-bit targets because
 * it needs an 8192-entry hash table on the stack (32 KB on 32-bit, 64 KB on
 * 64-bit), far exceeding the 4-8 KB stacks of 8086/286 targets.  Fastlz
 * decompression needs no such buffer, so this module is compiled only for
 * 16-bit builds (SIZEOF_INT < 4) via compress.c; the full fastlz library is
 * still used for 32/64-bit targets.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_FASTLZ_DECOMP_H
#define BP_FASTLZ_DECOMP_H

#include "../bp_types.h"

/* Return codes */
#define FASTLZ_D_OK  0   /* decompression succeeded    */
#define FASTLZ_D_ERR 1   /* any error (overrun, etc.)  */

/*
 * bp_fastlz_decompress - decompress one fastlz block (level 1 or 2)
 *
 * src     - pointer to compressed data
 * src_len - length of compressed data (must be >= 2)
 * dst     - output buffer
 * dst_len - on entry: output buffer capacity (must be <= 65535)
 *           on exit:  number of bytes actually written
 *
 * Returns FASTLZ_D_OK (0) on success, FASTLZ_D_ERR (1) on any error.
 * On error *dst_len is set to 0.
 *
 * The bitstream format is identical to fastlz_decompress() in fastlz 0.5.0
 * and is fully compatible with data produced by any fastlz compressor at
 * level 1 or level 2.  The compression level is auto-detected from the
 * top bits of the first byte, exactly as upstream does.
 */
int bp_fastlz_decompress(const bp_uint8 *src, bp_uint32 src_len,
                         bp_uint8 *dst, bp_uint32 *dst_len);

#endif /* BP_FASTLZ_DECOMP_H */
