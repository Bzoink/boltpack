/*
 * lzo_decomp.c -- Self-contained LZO1X-1 decompressor for 16-bit targets
 *
 * Implements bp_lzo1x_decompress(), a clean ANSI C89 re-implementation of
 * the LZO1X-1 decompression algorithm that compiles on 8086 / 286 targets
 * (DOS, Win16, OS/2 1.x, CP/M-86) using Open Watcom 16-bit compact model.
 *
 * Why this exists
 * ---------------
 * The vendored lib/minilzo-2.10/minilzo.c contains:
 *
 *   #error "LZO_ARCH_I086 is unsupported"
 *
 * in two separate locations that are unconditionally reached when building
 * for the 8086 target, preventing any use of the library there.  The error
 * fires in the PTR_LINEAR pointer-normalisation code, which is only needed
 * for the compressor and for unaligned-word optimisations in the hot copy
 * loops -- none of which matter for our decompressor.
 *
 * Decompression design for 16-bit
 * --------------------------------
 * - All internal work is done with array indices (unsigned int, 16-bit on
 *   Watcom) rather than raw pointer arithmetic, avoiding segment-crossing
 *   issues entirely.
 * - bp_uint32 (unsigned long, always 32-bit) is used only for the public
 *   API sizes, which are validated and narrowed to unsigned int on entry.
 * - Block sizes <= 32 KB are guaranteed by the bpmini archiver, so all
 *   indices, offsets (max M4 = 0xBFFF = 49151), and lengths fit within
 *   unsigned int (0 .. 65535).
 * - Back-reference copies are always byte-by-byte so overlapping (self-
 *   referential) matches -- used for RLE sequences -- are handled correctly.
 * - No standard library headers are needed.
 *
 * Bitstream compatibility
 * -----------------------
 * The format decoded here is identical to lzo1x_decompress_safe() in
 * minilzo 2.10.  Archives created by the full boltpack (or any other
 * LZO1X-1 compressor) will decompress correctly.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#include "lzo_decomp.h"

/* LZO1X match-offset base for the inline match that immediately follows
 * a literal run.  Equal to (1 + M2_MAX_OFFSET) where M2_MAX_OFFSET = 0x0800. */
#define FIRST_LIT_MBASE  ((unsigned int)(1u + 0x0800u))

/* Bounds-check macros.  All operands are unsigned int; subtraction is safe
 * because in_pos <= in_end and out_pos <= out_end are loop invariants. */
#define NEED_IP(n) \
    do { if ((unsigned int)(n) > in_end - in_pos) goto input_overrun; } while (0)

#define NEED_OP(n) \
    do { if ((unsigned int)(n) > out_end - out_pos) goto output_overrun; } while (0)

/* TEST_LB: verify that a back-reference of 'off' bytes is within the
 * produced output so far.  off must be >= 1 and <= out_pos. */
#define TEST_LB(off) \
    do { if ((off) == 0u || (off) > out_pos) goto lb_overrun; } while (0)

int bp_lzo1x_decompress(const bp_uint8 *src, bp_uint32 src_len,
                        bp_uint8 *dst, bp_uint32 *dst_len)
{
    unsigned int in_pos;   /* current read index into src */
    unsigned int in_end;   /* src_len as unsigned int      */
    unsigned int out_pos;  /* current write index into dst */
    unsigned int out_end;  /* output buffer capacity       */
    unsigned int t;        /* current token / match length */
    unsigned int m_off;    /* back-reference offset (>= 1) */
    unsigned int m_src;    /* back-reference source index  */

    /* Validate sizes before accessing *dst_len as capacity */
    if (src_len == 0u || src_len > 65535ul ||
        *dst_len == 0u || *dst_len > 65535ul) {
        *dst_len = 0;
        return LZO1X_D_ERR;
    }

    in_end  = (unsigned int)src_len;
    out_end = (unsigned int)*dst_len;
    *dst_len = 0;   /* will be set to actual on success */

    in_pos  = 0;
    out_pos = 0;

    /* ------------------------------------------------------------------
     * Phase 1: decode the first byte.
     *
     * If it is > 17, it encodes an initial literal run (byte - 17 bytes).
     * If it is <= 17, there is no initial literal run; the outer loop
     * reads that byte as the first opcode, so we must NOT consume it here.
     * ------------------------------------------------------------------ */
    NEED_IP(1u);

    if (src[in_pos] > 17u) {
        t = (unsigned int)src[in_pos++] - 17u;
        if (t < 4u) {
            /* 1..3 trailing literals from an implicit preceding match,
             * then the first real match opcode follows. */
            NEED_OP(t);
            NEED_IP(t + 3u);
            do { dst[out_pos++] = src[in_pos++]; } while (--t > 0u);
            t = (unsigned int)src[in_pos++];
            goto do_match;
        }
        /* Full initial literal run */
        NEED_OP(t);
        NEED_IP(t + 3u);
        do { dst[out_pos++] = src[in_pos++]; } while (--t > 0u);
        goto first_literal_run;
    }
    /* else: first byte <= 17 -- fall through to outer_loop which reads it */

    /* ------------------------------------------------------------------
     * Outer loop: read a full opcode.
     *
     * Opcodes < 16  are literal runs (copy t+3 bytes).
     * Opcodes >= 16 are matches (jump to inner match handler).
     * ------------------------------------------------------------------ */
outer_loop:
    NEED_IP(3u);
    t = (unsigned int)src[in_pos++];
    if (t >= 16u) goto do_match;

    /* Literal run of (t + 3) bytes; zero-byte extension when t == 0. */
    if (t == 0u) {
        while (src[in_pos] == 0u) {
            t += 255u;
            in_pos++;
            if ((unsigned int)(in_end - in_pos) < 1u) goto input_overrun;
        }
        t += 15u + (unsigned int)src[in_pos++];
    }
    NEED_OP(t + 3u);
    NEED_IP(t + 6u);
    t += 3u;
    do { dst[out_pos++] = src[in_pos++]; } while (--t > 0u);

    /* After every literal run, one byte follows that is either the opcode
     * for the next match (>= 16) or encodes a short inline match (< 16). */
first_literal_run:
    NEED_IP(1u);
    t = (unsigned int)src[in_pos++];
    if (t >= 16u) goto do_match;

    /* Inline 3-byte match using the high-offset M2 encoding.
     * Offset = FIRST_LIT_MBASE + (t >> 2) + (next_byte << 2). */
    NEED_IP(1u);
    m_off = FIRST_LIT_MBASE + (t >> 2) + ((unsigned int)src[in_pos++] << 2);
    TEST_LB(m_off);
    NEED_OP(3u);
    m_src = out_pos - m_off;
    dst[out_pos++] = dst[m_src++];
    dst[out_pos++] = dst[m_src++];
    dst[out_pos++] = dst[m_src];

    /* match_done: trailing literal count from low 2 bits of ip[-2]
     * (second-to-last byte consumed = the opcode byte = t). */
    t = (unsigned int)(src[in_pos - 2u] & 3u);
    if (t == 0u) goto outer_loop;
    goto trailing_literals;

    /* ------------------------------------------------------------------
     * Inner match handler.
     * 't' holds the match opcode on entry.
     * All paths lead to copy_match (or M2-inline) then to match_done.
     * ------------------------------------------------------------------ */
do_match:
    if (t >= 64u) {
        /* M1 -- short match, small offset, 1 extra byte.
         *   Length  = (opcode >> 5) - 1        (>= 1, so copy = len+2 >= 3)
         *   Offset  = 1 + ((opcode >> 2) & 7) + (extra_byte << 3)   (1..2048)
         * ip[-2] = opcode byte; low 2 bits = trailing literal count. */
        NEED_IP(1u);
        m_off = 1u + ((t >> 2) & 7u) + ((unsigned int)src[in_pos++] << 3);
        t = (t >> 5) - 1u;
        TEST_LB(m_off);
        NEED_OP(t + 2u);
        goto copy_match;
    }

    if (t >= 32u) {
        /* M3 -- medium-distance match, 2 extra bytes.
         *   Length  = (opcode & 31) with zero-byte extension (>= 1)
         *   Offset  = 1 + (byte0 >> 2) + (byte1 << 6)              (1..16383)
         * ip[-2] = byte0; low 2 bits = trailing literal count. */
        t &= 31u;
        if (t == 0u) {
            while (src[in_pos] == 0u) {
                t += 255u;
                in_pos++;
                if ((unsigned int)(in_end - in_pos) < 1u) goto input_overrun;
            }
            t += 31u + (unsigned int)src[in_pos++];
        }
        NEED_IP(2u);
        m_off = 1u + ((unsigned int)src[in_pos] >> 2)
                   + ((unsigned int)src[in_pos + 1u] << 6);
        in_pos += 2u;
        TEST_LB(m_off);
        NEED_OP(t + 2u);
        goto copy_match;
    }

    if (t >= 16u) {
        /* M4 -- long-distance match, 2 extra bytes, or end-of-stream.
         *   Offset high bit = (opcode & 8) << 11   (adds 0 or 0x4000)
         *   Length = (opcode & 7) with zero-byte extension (>= 1)
         *   Offset += (byte0 >> 2) + (byte1 << 6); then += 0x4000
         *   If combined offset before bias == 0 => end of stream.
         * ip[-2] = byte0; low 2 bits = trailing literal count. */
        m_off = (unsigned int)(t & 8u) << 11;  /* 0 or 0x4000 */
        t &= 7u;
        if (t == 0u) {
            while (src[in_pos] == 0u) {
                t += 255u;
                in_pos++;
                if ((unsigned int)(in_end - in_pos) < 1u) goto input_overrun;
            }
            t += 7u + (unsigned int)src[in_pos++];
        }
        NEED_IP(2u);
        m_off += ((unsigned int)src[in_pos] >> 2)
               + ((unsigned int)src[in_pos + 1u] << 6);
        in_pos += 2u;
        if (m_off == 0u) goto eof_found;   /* end-of-stream marker */
        m_off += 0x4000u;
        TEST_LB(m_off);
        NEED_OP(t + 2u);
        goto copy_match;
    }

    {
        /* M2 (inner loop) -- short match with small offset, always 2 bytes.
         *   Offset = 1 + (opcode >> 2) + (extra_byte << 2)          (1..1024)
         * ip[-2] = opcode byte; low 2 bits = trailing literal count. */
        NEED_IP(1u);
        m_off = 1u + (t >> 2) + ((unsigned int)src[in_pos++] << 2);
        TEST_LB(m_off);
        NEED_OP(2u);
        m_src = out_pos - m_off;
        dst[out_pos++] = dst[m_src++];
        dst[out_pos++] = dst[m_src];
        t = (unsigned int)(src[in_pos - 2u] & 3u);
        if (t == 0u) goto outer_loop;
        goto trailing_literals;
    }

copy_match:
    /* Copy t+2 bytes from dst[out_pos - m_off].
     * Byte-by-byte to handle overlapping (self-referential) matches. */
    m_src = out_pos - m_off;
    dst[out_pos++] = dst[m_src++];
    dst[out_pos++] = dst[m_src++];
    while (t-- > 0u) { dst[out_pos++] = dst[m_src++]; }

    /* match_done: extract trailing literal count from second-to-last byte. */
    t = (unsigned int)(src[in_pos - 2u] & 3u);
    if (t == 0u) goto outer_loop;
    /* fall through */

trailing_literals:
    /* Copy 1..3 trailing literals that immediately follow the match,
     * encoded in the low 2 bits of the preceding offset/opcode byte.
     * Then read the next match opcode and continue the inner loop. */
    NEED_OP(t);
    NEED_IP(t + 3u);
    do { dst[out_pos++] = src[in_pos++]; } while (--t > 0u);
    t = (unsigned int)src[in_pos++];
    goto do_match;

eof_found:
    *dst_len = (bp_uint32)out_pos;
    return LZO1X_D_OK;

input_overrun:
    *dst_len = 0;
    return LZO1X_D_ERR;

output_overrun:
    *dst_len = 0;
    return LZO1X_D_ERR;

lb_overrun:
    *dst_len = 0;
    return LZO1X_D_ERR;
}
