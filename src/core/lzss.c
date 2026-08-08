/*
 * lzss.c -- LZSS compress/decompress for BoltPack (method 1).
 *
 * Distance-based LZSS.  The encoder uses Okumura's (1989) binary-search-tree
 * matcher over a sliding ring window, but emits a back-distance rather than
 * an absolute ring index, so the decoder reads matches straight out of the
 * output buffer and needs no scratch memory or pre-fill.  Strict ANSI C89:
 * no //  comments, all vars declared at block top, no <stdint.h>, no VLAs,
 * no long long.
 *
 * Token encoding (BoltPack format, 2 bytes, 12+4 bit split):
 *   Byte 0 = (distance-1) bits 11..4  (high 8)
 *   Byte 1 = (distance-1) bits 3..0 (upper 4) | (match_len - 3) (lower 4)
 *
 * Decoding:
 *   dist = (((byte0 << 4) | (byte1 >> 4)) & 0xFFF) + 1   (1..4096)
 *   mlen = (byte1 & 0x0F) + LZSS_MIN_MATCH               (3..18)
 *   The matched run is copied from dst[dst_pos - dist] forward.
 *
 * Flag byte covers 8 symbols; bit 0 = first symbol.
 *   bit = 1  =>  next byte is a literal
 *   bit = 0  =>  next 2 bytes are a match token
 *
 * Compressor work buffer layout (see LZSS_WORK_SIZE in lzss.h):
 *   [0 .. N-1]                           ring_buf  (unsigned char)
 *   [N .. N + (N+1)*2 - 1]               lson      (unsigned short)
 *   [N+(N+1)*2 .. N+(N+1)*2+(N+257)*2-1] rson      (unsigned short)
 *   [remaining up to LZSS_WORK_SIZE-1]   dad       (unsigned short)
 * lzss_decompress() uses no work buffer at all.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#include "lzss.h"
#include "archiver.h"  /* BP_OK, BP_ERR_CORRUPT */
#include <string.h>

/* ---- algorithm parameters ---------------------------------------------- */

#define N  LZSS_RING_SIZE   /* 2048 */
#define F  LZSS_MAX_MATCH   /* 34   */
#define THRESHOLD LZSS_MIN_MATCH  /* 3 */

/* Sentinel: "no parent / no child".  Fits in unsigned short (2048 < 65535). */
#define NIL  ((unsigned short)N)

/* Stop BST search once a match this long is found.  We never emit tokens
 * shorter than THRESHOLD or longer than F, so GOOD_ENOUGH must satisfy
 * THRESHOLD <= GOOD_ENOUGH <= F.  Setting it to F-2 avoids spending extra
 * BST traversal steps hunting for rare length-33 and length-34 matches;
 * the ratio impact is below measurement noise on typical data. */
#define GOOD_ENOUGH  (F - 2)

/* ---- tree helpers ------------------------------------------------------- */

/*
 * Insert ring_buf[r..r+F-1] into the BST.
 * rson[N+1..N+256] are the 256 roots (one per possible first byte).
 * dad values of N+1..N+256 identify virtual root parents.
 * Sets *match_pos and *match_len to the best match found (<= F chars).
 */
static void insert_node(unsigned int r,
                        bp_uint8       *ring_buf,
                        unsigned short *lson,
                        unsigned short *rson,
                        unsigned short *dad,
                        unsigned int   *match_pos,
                        unsigned int   *match_len)
{
    unsigned int  i;
    int           cmp;
    unsigned int  p;
    unsigned int  cur;

    lson[r] = NIL;
    rson[r] = NIL;
    *match_len = 0;
    *match_pos = 0;

    cmp = 1;
    p   = (unsigned int)(N + 1 + ring_buf[r]);  /* virtual root for first byte */

    for (;;) {
        if (cmp >= 0) {
            if (rson[p] != NIL) {
                cur = (unsigned int)rson[p];
            } else {
                rson[p] = (unsigned short)r;
                dad[r]  = (unsigned short)p;
                return;
            }
        } else {
            if (lson[p] != NIL) {
                cur = (unsigned int)lson[p];
            } else {
                lson[p] = (unsigned short)r;
                dad[r]  = (unsigned short)p;
                return;
            }
        }
        p = cur;

        /* Compare strings starting at index 1 (index 0 matches by root selection) */
        for (i = 1; i < (unsigned int)F; i++) {
            cmp = (int)ring_buf[(r + i) & (N - 1)]
                - (int)ring_buf[(p + i) & (N - 1)];
            if (cmp != 0) break;
        }

        if (i > *match_len) {
            *match_pos = p;
            *match_len = i;
            if (*match_len >= (unsigned int)GOOD_ENOUGH) {
                /* Good enough: replace p with r and stop searching */
                dad[r]  = (unsigned short)dad[p];
                lson[r] = (unsigned short)lson[p];
                rson[r] = (unsigned short)rson[p];
                if (lson[p] != NIL) dad[lson[p]] = (unsigned short)r;
                if (rson[p] != NIL) dad[rson[p]] = (unsigned short)r;
                if (rson[dad[p]] == (unsigned short)p)
                    rson[dad[p]] = (unsigned short)r;
                else
                    lson[dad[p]] = (unsigned short)r;
                dad[p] = NIL;
                return;
            }
        }
    }
}

/*
 * Remove node p from the BST (called when it slides out of the window).
 */
static void delete_node(unsigned int    p,
                        unsigned short *lson,
                        unsigned short *rson,
                        unsigned short *dad)
{
    unsigned int q;

    if (dad[p] == NIL) return;  /* not in tree */

    if (rson[p] == NIL) {
        q = (unsigned int)lson[p];
    } else if (lson[p] == NIL) {
        q = (unsigned int)rson[p];
    } else {
        /* Both children: replace p with the rightmost node of its left subtree */
        q = (unsigned int)lson[p];
        if (rson[q] != NIL) {
            do {
                q = (unsigned int)rson[q];
            } while (rson[q] != NIL);
            /* Detach q: replace q with q's left child */
            rson[dad[q]] = lson[q];
            if (lson[q] != NIL) dad[lson[q]] = dad[q];
            /* Give q p's left subtree */
            lson[q] = lson[p];
            dad[lson[p]] = (unsigned short)q;
        }
        /* Give q p's right subtree */
        rson[q] = rson[p];
        dad[rson[p]] = (unsigned short)q;
    }

    /* q (or NIL if p had no children) takes p's place */
    dad[q] = (unsigned short)dad[p];
    if (rson[dad[p]] == (unsigned short)p) rson[dad[p]] = (unsigned short)q;
    else                                   lson[dad[p]] = (unsigned short)q;
    dad[p] = NIL;
}

/* ---- public API: decompress -------------------------------------------- */

int lzss_decompress(const bp_uint8 *src, bp_uint32 src_len,
                    bp_uint8 *dst, bp_uint32 dst_len,
                    bp_uint8 *work)
{
    bp_uint32    src_pos;
    bp_uint32    dst_pos;
    unsigned int flags;
    unsigned int flag_count;
    unsigned int b0;
    unsigned int b1;
    unsigned int dist;
    unsigned int mlen;
    unsigned int j;

    (void)work;  /* distance-based decode needs no scratch buffer */

    if (src == NULL || dst == NULL) return BP_ERR_CORRUPT;
    if (src_len == 0 && dst_len == 0) return BP_OK;
    if (src_len == 0) return BP_ERR_CORRUPT;

    src_pos    = 0;
    dst_pos    = 0;
    flags      = 0;
    flag_count = 0;

    while (dst_pos < dst_len) {
        if (flag_count == 0) {
            if (src_pos >= src_len) break;
            flags      = (unsigned int)src[src_pos++];
            flag_count = 8;
        }

        if (flags & 1) {
            /* Literal byte */
            if (src_pos >= src_len) return BP_ERR_CORRUPT;
            dst[dst_pos++] = src[src_pos++];  /* dst_pos < dst_len by while */
        } else {
            /* Match token (2 bytes, 12+4 bit split, back-distance) */
            if (src_pos + 1 >= src_len) return BP_ERR_CORRUPT;
            b0   = (unsigned int)src[src_pos++];
            b1   = (unsigned int)src[src_pos++];
            dist = (((b0 << 4) | (b1 >> 4)) & 0xFFF) + 1;
            mlen = (b1 & 0x0F) + THRESHOLD;
            /* Clamp to remaining output so the copy loop needs no per-byte check */
            if (mlen > dst_len - dst_pos) mlen = dst_len - dst_pos;
            /* Distance must point inside the data produced so far */
            if ((bp_uint32)dist > dst_pos) return BP_ERR_CORRUPT;

            for (j = 0; j < mlen; j++) {
                dst[dst_pos] = dst[dst_pos - dist];  /* overlap = RLE */
                dst_pos++;
            }
        }

        flags >>= 1;
        flag_count--;
    }

    return BP_OK;
}

/* ---- public API: compress ---------------------------------------------- */

bp_uint32 lzss_compress(const bp_uint8 *src, bp_uint32 src_len,
                        bp_uint8 *dst, bp_uint8 *work)
{
    bp_uint8       *ring_buf;
    unsigned short *lson;
    unsigned short *rson;
    unsigned short *dad;

    bp_uint32     src_pos;
    bp_uint32     dst_pos;
    unsigned int  r;
    unsigned int  s;
    unsigned int  len;
    unsigned int  match_pos;
    unsigned int  match_len;
    unsigned int  next_pos;   /* lazy: best-match position at r+1 */
    unsigned int  next_len;   /* lazy: best-match length  at r+1 */
    unsigned int  did_peek;   /* 1 if insert_node(r+1) was pre-called */
    unsigned int  last_match_len;
    unsigned int  i;
    unsigned int  c;
    unsigned int  cur_dist;   /* back-distance of current match (1..N-1) */
    unsigned int  next_dist;  /* back-distance of lazy match at r+1 */
    bp_uint32     out_count;  /* bytes emitted to output stream so far */

    bp_uint8      code_buf[17]; /* 1 flag byte + up to 16 data bytes */
    unsigned int  code_ptr;     /* next write index in code_buf */
    unsigned int  mask;         /* bit mask for current symbol */

    if (src == NULL || dst == NULL || work == NULL) return 0;
    if (src_len == 0) return 0;

    /*
     * Carve the work buffer.  Offsets (in bytes):
     *   ring_buf: 0            .. N-1              (2048 bytes)
     *   lson:     N            .. N+(N+1)*2-1      (4098 bytes)
     *   rson:     N+(N+1)*2    .. N+(N+1)*2+(N+257)*2-1  (4610 bytes)
     *   dad:      N+(N+1)*2+(N+257)*2 ...          (4098 bytes)
     */
    ring_buf = (bp_uint8 *)work;
    lson     = (unsigned short *)(void *)(work + N);
    rson     = (unsigned short *)(void *)(work + N + (N + 1) * 2);
    dad      = (unsigned short *)(void *)(work + N + (N + 1) * 2 + (N + 257) * 2);

    /* Initialise ring buffer with fill character */
    memset(ring_buf, LZSS_FILL_BYTE, N);

    /* Initialise tree: all entries NIL */
    for (i = 0; i <= (unsigned int)N; i++) {
        lson[i] = NIL;
        dad[i]  = NIL;
    }
    for (i = 0; i <= (unsigned int)(N + 256); i++) {
        rson[i] = NIL;
    }

    code_buf[0] = 0;
    code_ptr    = 1;
    mask        = 1;
    src_pos     = 0;
    dst_pos     = 0;
    match_pos   = 0;
    match_len   = 0;

    s         = 0;
    r         = N - F;
    len       = 0;
    out_count = 0;
    cur_dist  = 0;
    next_dist = 0;

    /* Pre-fill lookahead buffer from source */
    while (len < (unsigned int)F && src_pos < src_len) {
        ring_buf[r + len] = src[src_pos++];  /* r + len is in [N-F, N-1] */
        len++;
    }

    if (len == 0) return 0;

    /*
     * Insert F warm-up strings (r-F .. r-1) so the BST is populated with
     * the fill character before the first real match search.
     */
    for (i = 1; i <= (unsigned int)F; i++) {
        unsigned int tmp_pos, tmp_len;
        insert_node((r - i) & (N - 1), ring_buf, lson, rson, dad,
                    &tmp_pos, &tmp_len);
    }

    /* Insert the initial lookahead string to find the first match */
    insert_node(r, ring_buf, lson, rson, dad, &match_pos, &match_len);

    do {
        if (match_len > len) match_len = len;  /* can't match beyond lookahead */

        /*
         * Distance guard: a token references dst[out_pos - distance], so the
         * match must lie inside the bytes already emitted.  Early in the
         * stream the ring still holds phantom warm-up fill; reject any match
         * whose back-distance reaches past the real output produced so far.
         */
        cur_dist = 0;
        if (match_len >= (unsigned int)THRESHOLD) {
            cur_dist = (r - match_pos) & (N - 1);
            if (cur_dist == 0 || (bp_uint32)cur_dist > out_count)
                match_len = (unsigned int)THRESHOLD - 1;  /* demote to literal */
        }

        did_peek = 0;
        next_pos = 0;
        next_len = 0;
        if (match_len < (unsigned int)THRESHOLD) {
            /* Emit literal */
            match_len = 1;
            code_buf[0] |= (bp_uint8)mask;
            code_buf[code_ptr++] = ring_buf[r];
        } else {
            /*
             * 1-step lazy matching: peek at r+1 unless we already have a
             * perfect F-length match.  If r+1 yields a longer match, emit
             * ring_buf[r] as a literal now and use the better match next.
             */
            if (match_len < (unsigned int)F && len > 1) {
                insert_node((r + 1) & (N - 1), ring_buf, lson, rson, dad,
                            &next_pos, &next_len);
                did_peek = 1;
                /* clamp to the lookahead available at r+1 (one byte shorter) */
                if (next_len > len - 1) next_len = len - 1;
                /* apply the same distance guard to the lazy candidate */
                if (next_len >= (unsigned int)THRESHOLD) {
                    next_dist = ((r + 1) - next_pos) & (N - 1);
                    if (next_dist == 0 ||
                        (bp_uint32)next_dist > out_count + 1)
                        next_len = (unsigned int)THRESHOLD - 1;
                }
            }
            if (did_peek && next_len > match_len) {
                /* Defer: emit literal at r, advance by 1, use next match */
                code_buf[0] |= (bp_uint8)mask;
                code_buf[code_ptr++] = ring_buf[r];
                match_len = 1;
            } else {
                /*
             * Emit match token (12+4 bit split, back-distance):
             *   d = cur_dist - 1            (0..N-1, 12 bits)
             *   Byte 0 = d >> 4
             *   Byte 1 = ((d & 0xF) << 4) | (match_len - THRESHOLD)
             */
                unsigned int d = cur_dist - 1;
                code_buf[code_ptr++] = (bp_uint8)(d >> 4);
                code_buf[code_ptr++] = (bp_uint8)(((d & 0xF) << 4)
                                                  | (match_len - THRESHOLD));
            }
        }

        mask <<= 1;
        if (mask == 0x100u) {
            /* Flush 8 symbols to output */
            memcpy(dst + dst_pos, code_buf, code_ptr);
            dst_pos += (bp_uint32)code_ptr;
            code_buf[0] = 0;
            code_ptr    = 1;
            mask        = 1;
        }

        last_match_len = match_len;

        /* Slide the window: consume last_match_len bytes */
        for (i = 0; i < last_match_len; i++) {
            delete_node(s, lson, rson, dad);

            if (src_pos < src_len) {
                c = (unsigned int)src[src_pos++];
            } else {
                if (len > 0) len--;
                c = (unsigned int)LZSS_FILL_BYTE;
            }

            ring_buf[s] = (bp_uint8)c;
            s = (s + 1) & (N - 1);
            r = (r + 1) & (N - 1);

            if (len > 0) {
                if (i == 0 && did_peek) {
                    /* r is now r+1, which insert_node already visited above */
                    match_pos = next_pos;
                    match_len = next_len;
                } else {
                    insert_node(r, ring_buf, lson, rson, dad,
                                &match_pos, &match_len);
                }
            }
        }

        out_count += last_match_len;  /* bytes now part of the output stream */

    } while (len > 0);

    /* Flush any remaining symbols */
    if (code_ptr > 1) {
        memcpy(dst + dst_pos, code_buf, code_ptr);
        dst_pos += (bp_uint32)code_ptr;
    }

    return dst_pos;
}
