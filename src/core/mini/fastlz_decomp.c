/*
 * fastlz_decomp.c -- Self-contained fastlz decompressor for 16-bit targets
 *
 * Implements bp_fastlz_decompress(), a clean ANSI C89 re-implementation of
 * the fastlz (levels 1 and 2) decompression algorithm that compiles on
 * 8086 / 286 targets (DOS, Win16, OS/2 1.x, CP/M-86) using Open Watcom
 * 16-bit compact model.
 *
 * Why this exists
 * ---------------
 * The vendored lib/fastlz/fastlz.c compressor declares an 8192-entry hash
 * table (HASH_SIZE) on the stack -- 32 KB on a 32-bit target, 64 KB on
 * 64-bit.  That is far too large for the 4-8 KB stacks of 8086/286 targets,
 * so fastlz *compression* is not compiled there.  Fastlz *decompression*
 * needs no such buffer, so it is provided here as a small self-contained
 * module that mirrors the fastlz 0.5.0 bitstream exactly.
 *
 * Decompression design for 16-bit
 * --------------------------------
 * - All internal work is done with array indices (unsigned int, 16-bit on
 *   Watcom) rather than raw pointer arithmetic, avoiding segment-crossing
 *   issues entirely.
 * - bp_uint32 (unsigned long, always 32-bit) is used only for the public
 *   API sizes, which are validated and narrowed to unsigned int on entry.
 * - Block sizes <= 32 KB are guaranteed by the bpmini archiver.  Bounds and
 *   far-distance arithmetic are checked without unsigned-16-bit overflow.
 * - Back-reference copies are always byte-by-byte so overlapping (self-
 *   referential) matches -- used for RLE sequences -- are handled correctly.
 * - No standard library headers are needed.
 *
 * Bitstream compatibility
 * -----------------------
 * The format decoded here is identical to fastlz_decompress() in fastlz
 * 0.5.0.  Archives created by the full boltpack (or any other fastlz
 * compressor at level 1 or 2) will decompress correctly.  The compression
 * level is auto-detected from the top bits of the first byte, exactly as
 * upstream does.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "fastlz_decomp.h"

/* fastlz constant: maximum L2 (far) distance before the far-distance
 * encoding kicks in.  Mirrors MAX_L2_DISTANCE in lib/fastlz/fastlz.c. */
#define FLZ_MAX_L2_DISTANCE 8191

/* Bounds-check macros.  All operands are unsigned int; subtraction is safe
 * because in_pos <= in_end and out_pos <= out_end are loop invariants. */
#define NEED_IP(n) \
    do { if ((unsigned int)(n) > in_end - in_pos) goto input_overrun; } while (0)

#define NEED_OP(n) \
    do { if ((unsigned int)(n) > out_end - out_pos) goto output_overrun; } while (0)

/* Decode a stream with fastlz level-1 opcodes (the ip/op limits differ
 * slightly from level 2 in the loop-exit test). */
static int decode_level1(const bp_uint8 *src, unsigned int src_len,
                         bp_uint8 *dst, unsigned int dst_cap,
                         unsigned int *out_written)
{
    unsigned int in_pos;   /* current read index into src */
    unsigned int in_end;   /* src_len                     */
    unsigned int in_bound; /* in_end - 2                  */
    unsigned int out_pos;  /* current write index into dst */
    unsigned int out_end;  /* dst capacity                */
    unsigned int ctrl;     /* current control byte value  */
    unsigned int len;      /* match or literal length     */
    unsigned int ofs;      /* back-reference offset (raw) */
    unsigned int m_src;    /* back-reference source index */

    in_end   = src_len;
    in_bound = (in_end >= 2u) ? in_end - 2u : 0u;
    in_pos   = 0;
    out_pos  = 0;
    out_end  = dst_cap;

    NEED_IP(1u);
    ctrl = (unsigned int)(src[in_pos++] & 31u);

    for (;;) {
        if (ctrl >= 32u) {
            len = (ctrl >> 5) - 1u;
            ofs = (ctrl & 31u) << 8;
            if (len == 7u - 1u) {
                NEED_IP(1u);
                if (in_pos > in_bound) goto input_overrun;
                len += (unsigned int)src[in_pos++];
            }
            NEED_IP(1u);
            if (in_pos > in_bound) goto input_overrun;
            ofs += (unsigned int)src[in_pos++];
            len += 3u;

            /* ref = out_pos - ofs - 1 must be within produced output */
            if (out_pos < ofs + 1u) goto lb_overrun;
            if ((unsigned int)(len) > out_end - out_pos) goto output_overrun;
            m_src = out_pos - ofs - 1u;
            while (len-- > 0u) { dst[out_pos++] = dst[m_src++]; }
        } else {
            ctrl++;
            NEED_OP(ctrl);
            NEED_IP(ctrl);
            while (ctrl-- > 0u) { dst[out_pos++] = src[in_pos++]; }
        }

        if (in_pos > in_bound) break;
        NEED_IP(1u);
        ctrl = (unsigned int)src[in_pos++];
    }

    *out_written = out_pos;
    return FASTLZ_D_OK;

input_overrun:
output_overrun:
lb_overrun:
    return FASTLZ_D_ERR;
}

/* Decode a stream with fastlz level-2 opcodes. */
static unsigned int decode_level2(const bp_uint8 *src, unsigned int src_len,
                                  bp_uint8 *dst, unsigned int out_cap,
                                  unsigned int *out_written)
{
    unsigned int in_pos;   /* current read index into src */
    unsigned int in_end;   /* src length                   */
    unsigned int in_bound; /* in_end - 2                   */
    unsigned int out_pos;  /* current write index into dst */
    unsigned int out_end;  /* dst capacity                 */
    unsigned int ctrl;     /* current code value           */
    unsigned int len;      /* length                       */
    unsigned int ofs;      /* back-reference offset (raw)  */
    unsigned int code;     /* extra length/offset byte     */
    unsigned int m_src;    /* back-reference source index  */

    in_end   = src_len;
    in_bound = (in_end >= 2u) ? in_end - 2u : 0u;
    in_pos   = 0;
    out_pos  = 0;
    out_end  = out_cap;

    NEED_IP(1u);
    ctrl = (unsigned int)src[in_pos++] & 31u;

    for (;;) {
        if (ctrl >= 32u) {
            len = (ctrl >> 5) - 1u;
            ofs = (ctrl & 31u) << 8;
            if (len == 7u - 1u) {
                NEED_IP(1u);
                if (in_pos > in_bound) goto input_overrun;
                code = (unsigned int)src[in_pos++];
                if ((unsigned long)len + (unsigned long)code > 65535UL)
                    goto output_overrun;
                len += code;
                while (code == 255u) {
                    NEED_IP(1u);
                    if (in_pos > in_bound) goto input_overrun;
                    code = (unsigned int)src[in_pos++];
                    if ((unsigned long)len + (unsigned long)code > 65535UL)
                        goto output_overrun;
                    len += code;
                }
            }
            NEED_IP(1u);
            code = (unsigned int)src[in_pos++];
            ofs += code;
            if ((unsigned long)len + 3UL > 65535UL)
                goto output_overrun;
            len += 3u;
            if (code == 255u) {
                /* far-distance match: only when the ctrl low bits were 31 */
                if ((ctrl & 31u) == 31u) {
                    NEED_IP(2u);
                    if (in_pos >= in_bound) goto input_overrun;
                    ofs = ((unsigned int)src[in_pos] << 8);
                    ofs += (unsigned int)src[in_pos + 1u];
                    in_pos += 2u;
                    if (out_pos <= FLZ_MAX_L2_DISTANCE ||
                        ofs > out_pos - FLZ_MAX_L2_DISTANCE - 1u)
                        goto lb_overrun;
                    m_src = out_pos - ofs - FLZ_MAX_L2_DISTANCE - 1u;
                } else {
                    if (out_pos < ofs + 1u) goto lb_overrun;
                    m_src = out_pos - ofs - 1u;
                }
            } else {
                if (out_pos < ofs + 1u) goto lb_overrun;
                m_src = out_pos - ofs - 1u;
            }
            if ((unsigned int)len > out_end - out_pos) goto output_overrun;
            while (len-- > 0u) { dst[out_pos++] = dst[m_src++]; }
        } else {
            ctrl++;
            NEED_OP(ctrl);
            NEED_IP(ctrl);
            while (ctrl-- > 0u) { dst[out_pos++] = src[in_pos++]; }
        }

        if (in_pos >= in_end) break;
        NEED_IP(1u);
        ctrl = (unsigned int)src[in_pos++];
    }

    *out_written = out_pos;
    return FASTLZ_D_OK;

input_overrun:
output_overrun:
lb_overrun:
    return FASTLZ_D_ERR;
}

int bp_fastlz_decompress(const bp_uint8 *src, bp_uint32 src_len,
                         bp_uint8 *dst, bp_uint32 *dst_len)
{
    unsigned int level;

    if (dst_len == NULL) return FASTLZ_D_ERR;
    if (src == NULL || dst == NULL) {
        *dst_len = 0;
        return FASTLZ_D_ERR;
    }

    /* Validate sizes before accessing src[0] or narrowing to 16-bit. */
    if (src_len == 0u || src_len > 65535ul ||
        *dst_len == 0u || *dst_len > 65535ul) {
        *dst_len = 0;
        return FASTLZ_D_ERR;
    }

    /* Auto-detect compression level from the top bits of the first byte. */
    level = ((unsigned int)src[0] >> 5) + 1u;

    if (level == 1u) {
        unsigned int out_written = 0;
        if (decode_level1(src, (unsigned int)src_len, dst,
                          (unsigned int)*dst_len, &out_written) != FASTLZ_D_OK) {
            *dst_len = 0;
            return FASTLZ_D_ERR;
        }
        *dst_len = (bp_uint32)out_written;
        return FASTLZ_D_OK;
    }

    if (level == 2u) {
        unsigned int out_written = 0;
        if (decode_level2(src, (unsigned int)src_len, dst,
                          (unsigned int)*dst_len, &out_written) != FASTLZ_D_OK) {
            *dst_len = 0;
            return FASTLZ_D_ERR;
        }
        *dst_len = (bp_uint32)out_written;
        return FASTLZ_D_OK;
    }

    *dst_len = 0;
    return FASTLZ_D_ERR;
}
