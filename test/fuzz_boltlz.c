/*
 * fuzz_boltlz.c -- libFuzzer decoding and round-trip target
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#include "boltlz.h"
#include "boltlz_config.h"
#include "reference/boltlz_loop_base.h"
#include "reference/boltlz_scalar_decoder.h"
#include <stdlib.h>
#include <string.h>

static void compare_decode(const bp_uint8 *src, bp_uint32 length, bp_uint32 expected)
{
    bp_uint8 *input, *decoded, *frozen, *scalar;
    int actual, reference, accepted;
    /* Exact allocations expose reads/stores beyond declared input/output
     * lengths, even when a malformed stream requests a small output. */
    input = (bp_uint8 *)malloc(length ? length : 1);
    decoded = expected ? (bp_uint8 *)malloc(expected) : NULL;
    frozen = expected ? (bp_uint8 *)malloc(expected) : NULL;
    scalar = expected ? (bp_uint8 *)malloc(expected) : NULL;
    if (input == NULL || (expected &&
        (decoded == NULL || frozen == NULL || scalar == NULL))) abort();
    if (length) memcpy(input, src, length);
    actual = boltlz_decompress(input, length, decoded, expected);
    reference = boltlz_loop_reference_decompress(input, length, frozen, expected);
    accepted = reference_decode(input, length, scalar, expected);
    if (actual != reference || (actual == BP_OK && expected &&
                               memcmp(decoded, frozen, expected) != 0)) abort();
    if ((actual == BP_OK) != accepted || (accepted && expected &&
        memcmp(decoded, scalar, expected) != 0)) abort();
    free(scalar);
    free(frozen); free(decoded); free(input);
}

static void check_capacities(const bp_uint8 *input, bp_uint32 size,
                             const bp_uint8 *encoded, bp_uint32 length,
                             void *work, int level)
{
    bp_uint8 *exact, *short_buffer;
    bp_uint32 capacity;
    int result;
    /* These allocations end at the declared capacities. A smaller capacity
     * exercises the checked encoder path rather than the full-bound path. */
    exact = (bp_uint8 *)malloc(length);
    short_buffer = (bp_uint8 *)malloc(length > 1 ? length - 1 : 1);
    if (exact == NULL || short_buffer == NULL) abort();
    capacity = length;
    if (boltlz_compress(input, size, exact, &capacity, work, level) != BP_OK ||
        capacity != length || memcmp(exact, encoded, length) != 0) abort();
    capacity = length - 1;
    result = boltlz_compress(input, size, short_buffer, &capacity, work, level);
    if (result != BP_ERR_OVERFLOW || capacity != 0) abort();
    free(short_buffer); free(exact);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size)
{
    bp_uint8 *input, *encoded, *decoded;
    void *work;
    bp_uint32 cap, expected;
#if BOLTLZ_HASH_BITS == 13 && BOLTLZ_LAZY_LIMIT == 32 && \
    BOLTLZ_SECOND_LIMIT == 262144UL && !BOLTLZ_DUAL_HASH && \
    !BOLTLZ_EXACT_COST && BOLTLZ_BACKWARD_MATCH
    bp_uint8 *reference_encoded;
    bp_uint32 reference_length;
#endif
    int level;
    if (size > BOLTLZ_MAX_BLOCK) return 0;
    input = (bp_uint8 *)malloc(size ? size : 1);
    decoded = (bp_uint8 *)malloc(size ? size : 1);
    if (input == NULL || decoded == NULL) abort();
    if (size) memcpy(input, data, size);
    if (size > 1) {
        expected = ((bp_uint32)data[0] * 256 + data[1]) % 32769UL;
        compare_decode(data + 2, (bp_uint32)size - 2, expected);
        compare_decode(data, (bp_uint32)size, (bp_uint32)size);
    }
    if (size > 2) {
        expected = (((bp_uint32)data[0] << 16) +
                    ((bp_uint32)data[1] << 8) + data[2]) %
                   (BOLTLZ_MAX_BLOCK + 1);
        compare_decode(data + 3, (bp_uint32)size - 3, expected);
    }
    encoded = (bp_uint8 *)malloc((size_t)boltlz_compress_bound((bp_uint32)size));
    work = malloc((size_t)boltlz_work_size(BOLTLZ_BALANCED));
    if (encoded == NULL || work == NULL) abort();
#if BOLTLZ_HASH_BITS == 13 && BOLTLZ_LAZY_LIMIT == 32 && \
    BOLTLZ_SECOND_LIMIT == 262144UL && !BOLTLZ_DUAL_HASH && \
    !BOLTLZ_EXACT_COST && BOLTLZ_BACKWARD_MATCH
    reference_encoded = (bp_uint8 *)malloc((size_t)boltlz_loop_reference_compress_bound((bp_uint32)size));
    if (reference_encoded == NULL) abort();
#endif
    for (level = 1; level <= 2; level++) {
        cap = boltlz_compress_bound((bp_uint32)size);
        if (boltlz_compress(input, (bp_uint32)size, encoded, &cap, work, level) != BP_OK ||
            boltlz_decompress(encoded, cap, decoded, (bp_uint32)size) != BP_OK ||
            (size && memcmp(input, decoded, size) != 0)) abort();
        compare_decode(encoded, cap, (bp_uint32)size);
        if (size == 0 || (input[0] & 3) == 0)
            check_capacities(input, (bp_uint32)size, encoded, cap, work, level);
#if BOLTLZ_HASH_BITS == 13 && BOLTLZ_LAZY_LIMIT == 32 && \
    BOLTLZ_SECOND_LIMIT == 262144UL && !BOLTLZ_DUAL_HASH && \
    !BOLTLZ_EXACT_COST && BOLTLZ_BACKWARD_MATCH
        reference_length = boltlz_loop_reference_compress_bound((bp_uint32)size);
        if (boltlz_loop_reference_compress(input, (bp_uint32)size, reference_encoded,
                                            &reference_length, work, level) != BP_OK ||
            reference_length != cap || memcmp(reference_encoded, encoded, cap) != 0) abort();
#endif
    }
#if BOLTLZ_HASH_BITS == 13 && BOLTLZ_LAZY_LIMIT == 32 && \
    BOLTLZ_SECOND_LIMIT == 262144UL && !BOLTLZ_DUAL_HASH && \
    !BOLTLZ_EXACT_COST && BOLTLZ_BACKWARD_MATCH
    free(reference_encoded);
#endif
    free(work); free(encoded); free(decoded); free(input);
    return 0;
}
