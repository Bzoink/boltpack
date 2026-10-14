/*
 * boltlz_scalar_decoder.h -- independent validation-only format-2 decoder
 * Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
 * Returns one on success, zero on rejection; never shares production helpers.
 */
#ifndef TEST_BOLTLZ_SCALAR_DECODER_H
#define TEST_BOLTLZ_SCALAR_DECODER_H
#include "boltlz.h"

/* Intentionally simple decoder: little-endian fields and bytewise copies.
 * Do not share parsing or copying helpers with the production decoder. */
static int reference_var(const bp_uint8 *src, bp_uint32 size, bp_uint32 *in,
                          bp_uint32 *value)
{
    bp_uint32 v, byte, shift;
    v = shift = 0;
    do {
        if (*in >= size) return 0;
        byte = src[(*in)++];
        if (shift == 14 && (byte & 127) > 16) return 0;
        v += (byte & 127) << shift;
        if (shift != 0 && byte == 0) return 0;
        if ((byte & 128) != 0 && shift == 14) return 0;
        shift += 7;
    } while ((byte & 128) != 0);
    *value = v;
    return 1;
}

static int reference_decode(const bp_uint8 *src, bp_uint32 size,
                            bp_uint8 *dst, bp_uint32 expected)
{
    bp_uint32 i, o, tag, length, distance, previous, v, j;
    if (size == 0 || src[0] != 2) return 0;
    i = 1;
    o = previous = 0;
    while (i < size) {
        tag = src[i++];
        length = (tag / 16) % 8;
        if (length == 7) {
            if (!reference_var(src, size, &i, &v)) return 0;
            length += v;
        }
        if (length > expected - o) return 0;
        if (length > size - i) return 0;
        for (j = 0; j < length; j++) dst[o++] = src[i++];
        if (i == size && tag < 128)
            return o == expected && length != 0 && (tag & 143) == 0;
        length = tag % 16 + 4;
        if (length == 19) {
            if (!reference_var(src, size, &i, &v)) return 0;
            length += v;
        }
        if (length > expected - o) return 0;
        distance = previous;
        if (tag < 128 && !reference_var(src, size, &i, &distance)) return 0;
        if (distance == 0 || distance > o) return 0;
        previous = distance;
        for (j = 0; j < length; j++) { dst[o] = dst[o - distance]; o++; }
    }
    return o == expected;
}


#endif
