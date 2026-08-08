/*
 * lzo_decomp.h -- Self-contained LZO1X-1 decompressor for 16-bit targets
 *
 * Public interface for bp_lzo1x_decompress().  This module re-implements
 * only the decompression side of the LZO1X-1 algorithm in pure ANSI C89,
 * without dependency on the vendored minilzo library.
 *
 * The vendored lib/minilzo-2.10/minilzo.c explicitly rejects compilation
 * on LZO_ARCH_I086 (8086 segmented-memory) targets:
 *
 *   #error "LZO_ARCH_I086 is unsupported"
 *
 * This module avoids that guard entirely.  It is compiled only for 16-bit
 * builds (SIZEOF_INT < 4) via compress.c; the full minilzo library is
 * still used for 32/64-bit targets.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#ifndef BP_LZO_DECOMP_H
#define BP_LZO_DECOMP_H

#include "../bp_types.h"

/* Return codes */
#define LZO1X_D_OK  0   /* decompression succeeded    */
#define LZO1X_D_ERR 1   /* any error (overrun, etc.)  */

/*
 * bp_lzo1x_decompress - decompress one LZO1X-1 block
 *
 * src     - pointer to compressed data
 * src_len - length of compressed data (must be <= 65535)
 * dst     - output buffer
 * dst_len - on entry: output buffer capacity (must be <= 65535)
 *           on exit:  number of bytes actually written
 *
 * Returns LZO1X_D_OK (0) on success, LZO1X_D_ERR (1) on any error.
 * On error *dst_len is set to 0.
 *
 * The bitstream format is identical to lzo1x_decompress_safe() in
 * minilzo 2.10 and is fully compatible with data produced by any
 * standard LZO1X-1 compressor.
 */
int bp_lzo1x_decompress(const bp_uint8 *src, bp_uint32 src_len,
                        bp_uint8 *dst, bp_uint32 *dst_len);

#endif /* BP_LZO_DECOMP_H */
