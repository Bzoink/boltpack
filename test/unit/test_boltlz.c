/*
 * test_boltlz.c -- independent decoding and boundary tests for BoltLZ
 *
 * ANSI C (C89/C90) only.
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#include "boltlz.h"
#include "boltlz_config.h"
#include "../reference/boltlz_loop_base.h"
#include "../reference/boltlz_scalar_decoder.h"
#include "test_runner.h"
#include <stdlib.h>
#include <string.h>

static bp_uint32 random_state = 123456789UL;

static bp_uint32 next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}


static void round_trips(void)
{
    static const bp_uint32 boundaries[] = {
        0, 1, 2, 3, 4, 9, 10, 11, 33, 34, 35, 63, 64, 65, 66, 67,
        127, 128, 191, 192, 2047, 2048, 2049, 8192, 32768, 65535,
        65536, 65537, 131072, 262144
    };
    bp_uint8 *src, *encoded, *decoded, *reference, *reference_encoded;
    void *work;
    bp_uint32 n, length, cap, i, trial, pattern, period, original_len;
    int level, good, rc, capacity_good;
    int identical;
    bp_uint32 reference_length;
    src = (bp_uint8 *)malloc(BOLTLZ_MAX_BLOCK);
    cap = boltlz_compress_bound(BOLTLZ_MAX_BLOCK);
    encoded = (bp_uint8 *)malloc(cap + 1);
    decoded = (bp_uint8 *)malloc(BOLTLZ_MAX_BLOCK);
    reference = (bp_uint8 *)malloc(BOLTLZ_MAX_BLOCK);
    reference_encoded = (bp_uint8 *)malloc(cap);
    n = boltlz_work_size(BOLTLZ_BALANCED);
    if (n < boltlz_loop_reference_work_size(BOLTLZ_BALANCED))
        n = boltlz_loop_reference_work_size(BOLTLZ_BALANCED);
    work = malloc((size_t)n);
    if (!src || !encoded || !decoded || !reference || !reference_encoded || !work) exit(2);
    good = capacity_good = 1;
    identical = 1;
    for (trial = 0; trial < 700; trial++) {
        n = trial < sizeof(boundaries) / sizeof(boundaries[0]) ?
            boundaries[trial] : next_random() % 65537UL;
        pattern = trial % 7;
        period = 1 + next_random() % 3000;
        for (i = 0; i < n; i++) {
            if (pattern == 0) src[i] = 0;
            else if (pattern == 1) src[i] = (bp_uint8)(i % period);
            else if (pattern == 2 && i > period && i % 17 != 0)
                src[i] = src[i - period];
            else if (pattern == 3) src[i] = (bp_uint8)(next_random() % 6);
            else if (pattern == 4 && i > 8192 && i % 11 != 0)
                src[i] = src[i - 8192];
            else src[i] = (bp_uint8)next_random();
        }
        for (level = 1; level <= 2; level++) {
            length = boltlz_compress_bound(n);
            rc = boltlz_compress(src, n, encoded, &length, work, level);
            if (rc != BP_OK || !reference_decode(encoded, length, reference, n) ||
                boltlz_decompress(encoded, length, decoded, n) != BP_OK ||
                memcmp(decoded, src, (size_t)n) != 0 ||
                memcmp(reference, src, (size_t)n) != 0) {
                good = 0;
                printf("round trip failed: size=%lu pattern=%lu level=%d\n",
                       (unsigned long)n, (unsigned long)pattern, level);
                break;
            }
            original_len = length;
            if (boltlz_loop_reference_decompress(encoded, length, reference, n) != BP_OK ||
                memcmp(reference, src, (size_t)n) != 0) good = 0;
#if BOLTLZ_HASH_BITS == 13 && BOLTLZ_LAZY_LIMIT == 32 && \
    BOLTLZ_SECOND_LIMIT == 262144UL && !BOLTLZ_DUAL_HASH && \
    !BOLTLZ_EXACT_COST && BOLTLZ_BACKWARD_MATCH
            reference_length = boltlz_loop_reference_compress_bound(n);
            if (boltlz_loop_reference_compress(src, n, reference_encoded, &reference_length,
                                                work, level) != BP_OK ||
                reference_length != original_len ||
                memcmp(reference_encoded, encoded, original_len) != 0) identical = 0;
#else
            (void)reference_length;
#endif
            memset(encoded, 0xa5, (size_t)(cap + 1));
            length = original_len;
            rc = boltlz_compress(src, n, encoded, &length, work, level);
            if (rc != BP_OK || length != original_len || encoded[length] != 0xa5)
                capacity_good = 0;
            memset(encoded, 0xa5, (size_t)(cap + 1));
            length = original_len - 1;
            rc = boltlz_compress(src, n, encoded, &length, work, level);
            if (rc != BP_ERR_OVERFLOW || length != 0 ||
                encoded[original_len - 1] != 0xa5) capacity_good = 0;
        }
        if (!good) break;
    }
    UNIT_CHECK(good, "1,400 round trips agree with independent byte decoder");
    UNIT_CHECK(capacity_good, "exact and one-byte-short encoder capacities");
#if BOLTLZ_HASH_BITS == 13 && BOLTLZ_LAZY_LIMIT == 32 && \
    BOLTLZ_SECOND_LIMIT == 262144UL && !BOLTLZ_DUAL_HASH && \
    !BOLTLZ_EXACT_COST && BOLTLZ_BACKWARD_MATCH
    UNIT_CHECK(identical, "speed-only encoder changes preserve frozen baseline bytes");
#else
    (void)identical;
#endif
    free(reference_encoded); free(src); free(encoded); free(decoded); free(reference); free(work);
}

static void malformed_and_golden(void)
{
    static const bp_uint8 golden[] = {2, 64, 'a', 'b', 'c', 'd', 4, 128};
    static const bp_uint8 invalid_repeat[] = {2, 128, 0};
    static const bp_uint8 invalid_distance[] = {2, 16, 'a', 2};
    static const bp_uint8 invalid_var[] = {2, 112, 128, 128, 128};
    static const bp_uint8 overlong_var[] = {2, 112, 128, 0};
    static const bp_uint8 long_run[] = {2, 31, 'x', 15, 1};
    bp_uint8 output[128], reference[128], bytes[256];
    bp_uint32 i, j, size, expected, cap;
    int ok, actual, wanted;
    UNIT_CHECK(boltlz_decompress(golden, sizeof(golden), output, 12) == BP_OK &&
               memcmp(output, "abcdabcdabcd", 12) == 0,
               "golden short match and repeated distance");
    UNIT_CHECK(boltlz_decompress(long_run, sizeof(long_run), output, 35) == BP_OK &&
               output[34] == 'x', "long-match escape and overlapping run");
    UNIT_CHECK(boltlz_decompress(invalid_repeat, sizeof(invalid_repeat), output, 4)
               == BP_ERR_CORRUPT, "repeat before first explicit distance rejected");
    UNIT_CHECK(boltlz_decompress(invalid_distance, sizeof(invalid_distance), output, 5)
               == BP_ERR_CORRUPT, "distance beyond output rejected");
    UNIT_CHECK(boltlz_decompress(invalid_var, sizeof(invalid_var), output, 64)
               == BP_ERR_CORRUPT, "unbounded length extension rejected");
    UNIT_CHECK(boltlz_decompress(overlong_var, sizeof(overlong_var), output, 64)
               == BP_ERR_CORRUPT, "noncanonical length extension rejected");
    ok = 1;
    for (i = 0; i < sizeof(golden); i++)
        if (boltlz_decompress(golden, i, output, 12) == BP_OK) ok = 0;
    UNIT_CHECK(ok, "all truncated golden prefixes rejected");
    UNIT_CHECK(boltlz_decompress(golden, sizeof(golden), output, 11) != BP_OK,
               "undersized output rejected");
    UNIT_CHECK(boltlz_decompress(golden, sizeof(golden), output, 13) != BP_OK,
               "oversized expected output rejected");
    bytes[0] = 3;
    UNIT_CHECK(boltlz_decompress(bytes, 1, NULL, 0) == BP_ERR_VERSION,
               "unknown version rejected");
    bytes[0] = 2;
    UNIT_CHECK(boltlz_decompress(bytes, 1, NULL, 0) == BP_OK,
               "version-only empty block accepted");
    bytes[1] = 0;
    UNIT_CHECK(boltlz_decompress(bytes, 2, NULL, 0) != BP_OK,
               "trailing data after empty block rejected");
    ok = 1;
    for (i = 0; i < 25000; i++) {
        size = 1 + next_random() % 255;
        expected = next_random() % 128;
        for (j = 0; j < size; j++) bytes[j] = (bp_uint8)next_random();
        bytes[0] = 2;
        actual = boltlz_decompress(bytes, size, output, expected) == BP_OK;
        wanted = reference_decode(bytes, size, reference, expected);
        if (actual != wanted ||
            (actual && memcmp(output, reference, (size_t)expected) != 0)) ok = 0;
    }
    UNIT_CHECK(ok, "25,000 random streams agree with independent decoder");
    cap = 0;
    UNIT_CHECK(boltlz_compress(NULL, 0, bytes, &cap, output, 1) == BP_ERR_OVERFLOW,
               "zero destination capacity rejected");
    UNIT_CHECK(boltlz_compress_bound(BOLTLZ_MAX_BLOCK + 1) == 0 &&
               boltlz_work_size(3) == 0, "unsupported sizes and levels rejected");
}

static void distant_and_overlapping(void)
{
    bp_uint8 *encoded, *decoded, *expected, *source;
    void *work;
    bp_uint32 i, size, in, out, distance, length, header, cap;
    int ok, level;
    encoded = (bp_uint8 *)malloc(270000);
    decoded = (bp_uint8 *)malloc(BOLTLZ_MAX_BLOCK);
    expected = (bp_uint8 *)malloc(BOLTLZ_MAX_BLOCK);
    source = (bp_uint8 *)malloc(BOLTLZ_MAX_BLOCK);
    work = malloc((size_t)boltlz_work_size(2));
    if (!encoded || !decoded || !expected || !source || !work) exit(2);
    ok = 1;
    /* Exercise overlap distances and explicit distance varint boundaries
     * with manually generated tokens, independent of encoder choices. */
    for (i = 0; i < 9; i++) {
        static const bp_uint32 distances[] = {1, 2, 3, 7, 15, 16, 2048, 65536, 65537};
        distance = distances[i];
        encoded[0] = 2;
        in = 1;
        if (distance < 7) encoded[in++] = (bp_uint8)((distance << 4) | 15);
        else {
            encoded[in++] = 127;
            header = distance - 7;
            while (header >= 128) {
                encoded[in++] = (bp_uint8)((header & 127) | 128);
                header >>= 7;
            }
            encoded[in++] = (bp_uint8)header;
        }
        for (out = 0; out < distance; out++)
            encoded[in++] = expected[out] = (bp_uint8)(out * 17);
        /* Explicit length 19+200, then repeat length 19+262. */
        encoded[in++] = 200;
        encoded[in++] = 1;
        header = distance;
        while (header >= 128) {
            encoded[in++] = (bp_uint8)((header & 127) | 128);
            header >>= 7;
        }
        encoded[in++] = (bp_uint8)header;
        encoded[in++] = 143;
        encoded[in++] = 134;
        encoded[in++] = 2;
        size = distance + 500;
        for (; out < size; out++) expected[out] = expected[out - distance];
        if (boltlz_decompress(encoded, in, decoded, size) != BP_OK ||
            memcmp(decoded, expected, (size_t)size) != 0 ||
            !reference_decode(encoded, in, decoded, size)) ok = 0;
    }
    UNIT_CHECK(ok, "manual long copies: overlap and large-distance boundaries");
    for (i = 0; i < 90000; i++) source[i] = (bp_uint8)next_random();
    memcpy(source + 90000, source, 90000);
    ok = 1;
    for (level = 1; level <= 2; level++) {
        length = boltlz_compress_bound(180000);
        if (boltlz_compress(source, 180000, encoded, &length, work, level) != BP_OK ||
            boltlz_decompress(encoded, length, decoded, 180000) != BP_OK ||
            memcmp(decoded, source, 180000) != 0 ||
            !reference_decode(encoded, length, expected, 180000) ||
            memcmp(expected, source, 180000) != 0) ok = 0;
    }
    UNIT_CHECK(ok, "180 KiB random-prefix repetition in both encoders");
    /* Mutations of valid compressed blocks reach more decoder states than
     * uniformly random bytes. The independent decoder checks acceptance. */
    memset(source, 'r', 4096);
    for (i = 100; i < 4096; i += 103) source[i] = (bp_uint8)i;
    cap = boltlz_compress_bound(4096);
    if (boltlz_compress(source, 4096, encoded, &cap, work, 2) != BP_OK) abort();
    memcpy(source, encoded, (size_t)cap);
    ok = 1;
    for (i = 0; i < 5000; i++) {
        int actual, wanted;
        memcpy(encoded, source, (size_t)cap);
        encoded[next_random() % cap] ^= (bp_uint8)(1 + next_random() % 255);
        actual = boltlz_decompress(encoded, cap, decoded, 4096) == BP_OK;
        wanted = reference_decode(encoded, cap, expected, 4096);
        if (actual != wanted || (actual && memcmp(decoded, expected, 4096) != 0)) ok = 0;
    }
    UNIT_CHECK(ok, "5,000 mutated valid streams agree with reference decoder");
    free(work); free(source); free(expected); free(decoded); free(encoded);
}

static void short_copy_boundaries(void)
{
    bp_uint8 encoded[512], expected[512];
    bp_uint8 *input, *decoded, *frozen;
    bp_uint32 distance, length, in, out, i, total, sequence, literals, match_length;
    int ok;
    ok = 1;
    for (distance = 1; distance <= 40; distance++) {
        for (length = 4; length <= 32; length++) {
            in = 0;
            encoded[in++] = BOLTLZ_FORMAT_VERSION;
            encoded[in++] = (bp_uint8)(((distance < 7 ? distance : 7) << 4) |
                                      (length < 19 ? length - 4 : 15));
            if (distance >= 7) encoded[in++] = (bp_uint8)(distance - 7);
            for (i = 0; i < distance; i++) encoded[in++] = expected[i] = (bp_uint8)(i * 23 + 7);
            if (length >= 19) encoded[in++] = (bp_uint8)(length - 19);
            encoded[in++] = (bp_uint8)distance;
            encoded[in++] = (bp_uint8)(128 | ((length % 15)));
            total = distance + length + (length % 15) + 4;
            for (out = distance; out < total; out++) expected[out] = expected[out - distance];
            /* Enough sequences to enter the wide shortcut, then approach
             * both exact buffer ends through its fallback boundaries. */
            for (sequence = 0; sequence < 8; sequence++) {
                literals = sequence % 7;
                match_length = sequence + 4;
                encoded[in++] = (bp_uint8)(128 | (literals << 4) | (match_length - 4));
                for (i = 0; i < literals; i++)
                    encoded[in++] = expected[total++] = (bp_uint8)(sequence * 11 + i);
                for (i = 0; i < match_length; i++) {
                    expected[total] = expected[total - distance];
                    total++;
                }
            }
            /* Final literal-only sequence, exercising exact input tails. */
            encoded[in++] = (bp_uint8)(((length % 6) + 1) << 4);
            for (i = 0; i < (length % 6) + 1; i++) encoded[in++] = expected[total++] = (bp_uint8)i;
            input = (bp_uint8 *)malloc(in);
            decoded = (bp_uint8 *)malloc(total);
            frozen = (bp_uint8 *)malloc(total);
            if (!input || !decoded || !frozen) exit(2);
            memcpy(input, encoded, (size_t)in);
            if (boltlz_decompress(input, in, decoded, total) != BP_OK ||
                boltlz_loop_reference_decompress(input, in, frozen, total) != BP_OK ||
                memcmp(decoded, expected, (size_t)total) != 0 ||
                memcmp(frozen, expected, (size_t)total) != 0) ok = 0;
            free(frozen); free(decoded); free(input);
        }
    }
    UNIT_CHECK(ok, "1,160 short-copy/overlap fixtures use exact-sized input and output buffers");
}

static void append_var(bp_uint8 *bytes, bp_uint32 *pos, bp_uint32 value)
{
    while (value >= 128) {
        bytes[(*pos)++] = (bp_uint8)((value & 127) | 128);
        value >>= 7;
    }
    bytes[(*pos)++] = (bp_uint8)value;
}

/* Exact allocations expose overreads/stores at the fast-loop margins to
 * sanitizers. Compare errors as well as successful bytes with the snapshot. */
static int decode_agrees(const bp_uint8 *bytes, bp_uint32 length,
                         bp_uint32 expected, const bp_uint8 *wanted)
{
    bp_uint8 *input, *output, *frozen;
    int actual, reference, ok;
    input = (bp_uint8 *)malloc(length);
    output = (bp_uint8 *)malloc(expected);
    frozen = (bp_uint8 *)malloc(expected);
    if (!input || !output || !frozen) exit(2);
    memcpy(input, bytes, (size_t)length);
    actual = boltlz_decompress(input, length, output, expected);
    reference = boltlz_loop_reference_decompress(input, length, frozen, expected);
    ok = actual == reference &&
         (actual != BP_OK || memcmp(output, frozen, (size_t)expected) == 0);
    if (wanted != NULL)
        ok = ok && actual == BP_OK && memcmp(output, wanted, (size_t)expected) == 0;
    else ok = ok && actual != BP_OK;
    free(frozen); free(output); free(input);
    return ok;
}

static void fast_distance_boundaries(void)
{
    static const bp_uint32 distances[] = {
        127, 128, 16383, 16384, 65535, 65536, 131071, 262000
    };
    static const bp_uint8 bad_third[] = {0, 17, 128, 255};
    bp_uint8 *encoded, *expected;
    bp_uint32 d, distance, tail, in, total, i, sequence, literals, length;
    bp_uint32 field, cut, mutation;
    bp_uint8 saved;
    int valid_ok, invalid_ok;
    encoded = (bp_uint8 *)malloc(BOLTLZ_MAX_BLOCK + 256);
    expected = (bp_uint8 *)malloc(BOLTLZ_MAX_BLOCK);
    if (!encoded || !expected) exit(2);
    valid_ok = invalid_ok = 1;
    for (d = 0; d < sizeof(distances) / sizeof(distances[0]); d++) {
        distance = distances[d];
        for (tail = 0; tail <= 33; tail++) {
            in = total = 0;
            encoded[in++] = BOLTLZ_FORMAT_VERSION;
            encoded[in++] = 112; /* Long literal prefix, then a four-byte match. */
            append_var(encoded, &in, distance - 7);
            for (i = 0; i < distance; i++)
                encoded[in++] = expected[total++] = (bp_uint8)(i * 23 + i / 251);
            append_var(encoded, &in, distance);
            for (i = 0; i < 4; i++) {
                expected[total] = expected[total - distance]; total++;
            }
            field = 0;
            for (sequence = 0; sequence < 8; sequence++) {
                literals = sequence % 7;
                length = sequence + 4;
                encoded[in++] = (bp_uint8)((literals << 4) | (length - 4) |
                                          (sequence % 2 ? 128 : 0));
                for (i = 0; i < literals; i++)
                    encoded[in++] = expected[total++] = (bp_uint8)(sequence + i);
                if (sequence % 2 == 0) {
                    if (sequence == 0) field = in;
                    append_var(encoded, &in, distance);
                }
                for (i = 0; i < length; i++) {
                    expected[total] = expected[total - distance]; total++;
                }
            }
            if (tail != 0) {
                encoded[in++] = (bp_uint8)((tail < 7 ? tail : 7) << 4);
                if (tail >= 7) append_var(encoded, &in, tail - 7);
                for (i = 0; i < tail; i++) encoded[in++] = expected[total++] = (bp_uint8)i;
            }
            if (!decode_agrees(encoded, in, total, expected)) valid_ok = 0;
            if (tail != 32 || distance < 16384) continue;
            /* The targeted zero-literal token has ample fast-path margins.
             * Noncanonical, oversized and unterminated third bytes must
             * retain the general decoder's precise failure behavior. */
            saved = encoded[field + 2];
            for (mutation = 0; mutation < sizeof(bad_third); mutation++) {
                encoded[field + 2] = bad_third[mutation];
                if (!decode_agrees(encoded, in, total, NULL)) invalid_ok = 0;
            }
            encoded[field + 2] = saved;
            for (cut = 0; cut < 3; cut++)
                if (!decode_agrees(encoded, field + cut, total, NULL)) invalid_ok = 0;
            encoded[field + 2] = 16; /* At least 262,144: outside produced history. */
            if (!decode_agrees(encoded, in, total, NULL)) invalid_ok = 0;
            encoded[field + 2] = saved;
            if (!decode_agrees(encoded, in, total - 1, NULL) ||
                !decode_agrees(encoded, in, total + 1, NULL)) invalid_ok = 0;
        }
    }
    UNIT_CHECK(valid_ok, "272 exact-buffer distance/zero-literal fixtures cross fast-loop margins");
    UNIT_CHECK(invalid_ok, "three-byte distance errors and truncations match frozen decoder");
    free(expected); free(encoded);
}

static void extended_copy_boundaries(void)
{
    static const bp_uint32 distances[] = {1, 2, 3, 4, 7, 15, 16, 17, 31, 32, 33, 128, 16384};
    static const bp_uint32 literal_lengths[] = {0, 6, 7, 8, 15, 16, 17, 31, 32, 33, 127, 134, 135};
    static const bp_uint32 match_lengths[] = {4, 18, 19, 20, 31, 32, 33, 63, 64, 65, 127, 146, 147};
    static const bp_uint32 tails[] = {0, 1, 14, 15, 16, 17, 30, 31, 32, 33};
    bp_uint8 *encoded, *expected;
    bp_uint32 d, l, m, repeat, distance, literals, length, seed, in, total, i;
    bp_uint32 count, following, tail;
    int ok, errors;
    encoded = (bp_uint8 *)malloc(20000);
    expected = (bp_uint8 *)malloc(20000);
    if (!encoded || !expected) exit(2);
    count = 0;
    ok = errors = 1;
    for (d = 0; d < sizeof(distances) / sizeof(distances[0]); d++) {
        distance = distances[d];
        seed = distance < 64 ? 64 : distance;
        for (l = 0; l < sizeof(literal_lengths) / sizeof(literal_lengths[0]); l++) {
            literals = literal_lengths[l];
            for (m = 0; m < sizeof(match_lengths) / sizeof(match_lengths[0]); m++) {
                length = match_lengths[m];
                for (repeat = 0; repeat < 2; repeat++) {
                    in = total = 0;
                    encoded[in++] = BOLTLZ_FORMAT_VERSION;
                    encoded[in++] = 112;
                    append_var(encoded, &in, seed - 7);
                    for (i = 0; i < seed; i++)
                        encoded[in++] = expected[total++] = (bp_uint8)(i * 23 + i / 251);
                    append_var(encoded, &in, distance);
                    for (i = 0; i < 4; i++) {
                        expected[total] = expected[total - distance]; total++;
                    }
                    encoded[in++] = (bp_uint8)(((literals < 7 ? literals : 7) << 4) |
                                              (length < 19 ? length - 4 : 15) |
                                              (repeat ? 128 : 0));
                    if (literals >= 7) append_var(encoded, &in, literals - 7);
                    for (i = 0; i < literals; i++)
                        encoded[in++] = expected[total++] = (bp_uint8)(i * 7 + m);
                    if (length >= 19) append_var(encoded, &in, length - 19);
                    if (!repeat) append_var(encoded, &in, distance);
                    for (i = 0; i < length; i++) {
                        expected[total] = expected[total - distance]; total++;
                    }
                    /* Vary each exact buffer's remaining margin independently
                     * of literal/match lengths and rounded copy widths. */
                    following = count % 9;
                    for (i = 0; i < following; i++) {
                        bp_uint32 j;
                        encoded[in++] = 128;
                        for (j = 0; j < 4; j++) {
                            expected[total] = expected[total - distance]; total++;
                        }
                    }
                    tail = tails[count % (sizeof(tails) / sizeof(tails[0]))];
                    if (tail != 0) {
                        encoded[in++] = (bp_uint8)((tail < 7 ? tail : 7) << 4);
                        if (tail >= 7) append_var(encoded, &in, tail - 7);
                        for (i = 0; i < tail; i++) encoded[in++] = expected[total++] = (bp_uint8)i;
                    }
                    if (!decode_agrees(encoded, in, total, expected)) ok = 0;
                    if (count % 32 == 0 && !decode_agrees(encoded, in - 1, total, NULL)) errors = 0;
                    if (count % 64 == 0 &&
                        (!decode_agrees(encoded, in, total - 1, NULL) ||
                         !decode_agrees(encoded, in, total + 1, NULL))) errors = 0;
                    count++;
                }
            }
        }
    }
    UNIT_CHECK(ok && count == 4394, "4,394 exact-buffer fixtures cover extended lengths and 16/32-byte copy margins");
    UNIT_CHECK(errors, "extended-copy tails and invalid output lengths match frozen decoder errors");
    free(expected); free(encoded);
}

static void table_and_capacity_boundaries(void)
{
    static const bp_uint32 sizes[] = {3, 65532, 65535, 65536, 65537, 262144, 65536, 31};
    static bp_uint32 typed_work[32768];
    bp_uint8 *source, *encoded, *frozen;
    bp_uint32 trial, size, bound, capacity, length, original, i, state;
    int level, good;
    source = (bp_uint8 *)malloc(BOLTLZ_MAX_BLOCK);
    encoded = (bp_uint8 *)malloc(boltlz_compress_bound(BOLTLZ_MAX_BLOCK) + 1);
    frozen = (bp_uint8 *)malloc(boltlz_compress_bound(BOLTLZ_MAX_BLOCK));
    if (!source || !encoded || !frozen || sizeof(typed_work) < boltlz_work_size(2)) exit(2);
    good = 1;
    for (i = 0; i < sizeof(typed_work) / sizeof(typed_work[0]); i++) typed_work[i] = 0x12345678UL;
    state = 123456789UL;
    for (trial = 0; trial < sizeof(sizes) / sizeof(sizes[0]); trial++) {
        size = sizes[trial];
        for (i = 0; i < size; i++) {
            state = state * (bp_uint32)1664525UL + (bp_uint32)1013904223UL;
            source[i] = i > 8192 && i % 47 < 30 ? source[i - 8192] : (bp_uint8)(state >> 24);
        }
        bound = boltlz_compress_bound(size);
        for (level = 1; level <= 2; level++) {
            original = bound;
            if (boltlz_loop_reference_compress(source, size, frozen, &original,
                                               typed_work, level) != BP_OK) { good = 0; continue; }
            for (capacity = bound - 1; capacity <= bound; capacity++) {
                memset(encoded, 0xa5, (size_t)(bound + 1));
                length = capacity;
                if (boltlz_compress(source, size, encoded, &length, typed_work, level) != BP_OK ||
                    length > capacity || encoded[capacity] != 0xa5) good = 0;
#if BOLTLZ_HASH_BITS == 13 && BOLTLZ_LAZY_LIMIT == 32 && \
    BOLTLZ_SECOND_LIMIT == 262144UL && !BOLTLZ_DUAL_HASH && \
    !BOLTLZ_EXACT_COST && BOLTLZ_BACKWARD_MATCH
                if (length != original || memcmp(encoded, frozen, (size_t)length) != 0) good = 0;
#endif
            }
        }
    }
    UNIT_CHECK(good, "typed work survives 64 KiB width transitions and bound/bound-minus-one capacities");
    free(frozen); free(encoded); free(source);
}

static void skipped_prefix(void)
{
    bp_uint8 source[4226], encoded[4400], decoded[4226];
    bp_uint32 i, state, length, literals, cursor, extra;
    void *work;
    int level, ok, recovered;
    /* After a long unrelated prefix, adaptive skipping can land inside the
     * repetition. A backward extension must recover its complete beginning,
     * including a reference that reaches byte zero. */
    state = 123456789UL;
    for (i = 0; i < 4097; i++) {
        state = state * (bp_uint32)1664525UL + (bp_uint32)1013904223UL;
        source[i] = (bp_uint8)(state >> 24);
    }
    memcpy(source + 4097, source, 128);
    source[4225] = (bp_uint8)(source[128] ^ 255);
    work = malloc((size_t)boltlz_work_size(BOLTLZ_BALANCED));
    if (work == NULL) exit(2);
    ok = recovered = 1;
    for (level = 1; level <= 2; level++) {
        length = sizeof(encoded);
        if (boltlz_compress(source, sizeof(source), encoded, &length, work, level) != BP_OK ||
            !reference_decode(encoded, length, decoded, sizeof(source)) ||
            memcmp(source, decoded, sizeof(source)) != 0) { ok = 0; continue; }
        literals = (encoded[1] >> 4) & 7;
        cursor = 2;
        if (literals == 7) {
            if (!reference_var(encoded, length, &cursor, &extra)) { recovered = 0; continue; }
            literals += extra;
        }
        if (literals != 4097) recovered = 0;
    }
    UNIT_CHECK(ok, "skipped random-prefix repetition decodes independently");
#if BOLTLZ_BACKWARD_MATCH
    UNIT_CHECK(recovered, "backward extension recovers repetition through reference byte zero");
#else
    (void)recovered;
#endif
    free(work);
}

static int prefix_intact(const bp_uint8 *buffer, unsigned int prefix)
{
    unsigned int i;
    for (i = 0; i < prefix; i++) if (buffer[i] != 165) return 0;
    return 1;
}

static void unaligned_buffers(void)
{
    static const bp_uint32 lengths[] = {
        0, 1, 3, 4, 15, 16, 17, 31, 32, 33, 127, 128,
        65535, 65536, 65537, 262144
    };
    bp_uint8 *source, *encoded, *decoded, *scalar;
    void *work;
    bp_uint32 size, capacity, length, i;
    unsigned int trial, source_offset, encoded_offset, decoded_offset;
    int level, good, guards;
    good = guards = 1;
    work = malloc((size_t)boltlz_work_size(BOLTLZ_BALANCED));
    if (!work) exit(2);
    for (trial = 0; trial < sizeof(lengths) / sizeof(lengths[0]); trial++) {
        size = lengths[trial];
        capacity = boltlz_compress_bound(size);
        scalar = (bp_uint8 *)malloc(size ? size : 1);
        if (!scalar) exit(2);
        for (source_offset = 0; source_offset < 8; source_offset++) {
            source = (bp_uint8 *)malloc((size ? size : 1) + source_offset);
            if (!source) exit(2);
            memset(source, 165, source_offset);
            for (i = 0; i < size; i++)
                source[source_offset + i] = (i % 257 < 17) ?
                    (bp_uint8)next_random() : (bp_uint8)(i % 13);
            for (encoded_offset = 0; encoded_offset < 8; encoded_offset++) {
                decoded_offset = (source_offset + encoded_offset) % 8;
                encoded = (bp_uint8 *)malloc(capacity + encoded_offset);
                decoded = (bp_uint8 *)malloc((size ? size : 1) + decoded_offset);
                if (!encoded || !decoded) exit(2);
                for (level = BOLTLZ_FAST; level <= BOLTLZ_BALANCED; level++) {
                    memset(encoded, 165, encoded_offset);
                    memset(decoded, 165, decoded_offset);
                    length = capacity;
                    if (boltlz_compress(source + source_offset, size,
                                       encoded + encoded_offset, &length,
                                       work, level) != BP_OK ||
                        boltlz_decompress(encoded + encoded_offset, length,
                                          decoded + decoded_offset, size) != BP_OK ||
                        !reference_decode(encoded + encoded_offset, length, scalar, size) ||
                        memcmp(source + source_offset, decoded + decoded_offset, size) != 0 ||
                        memcmp(source + source_offset, scalar, size) != 0) good = 0;
                    if (!prefix_intact(source, source_offset) ||
                        !prefix_intact(encoded, encoded_offset) ||
                        !prefix_intact(decoded, decoded_offset)) guards = 0;
                }
                free(decoded); free(encoded);
            }
            free(source);
        }
        free(scalar);
    }
    free(work);
    UNIT_CHECK(good, "2,048 unaligned exact-buffer round trips decode independently");
    UNIT_CHECK(guards, "unaligned source and destination prefix guards remain intact");
}

int main(void)
{
    UNIT_SECTION("BoltLZ experimental codec");
    round_trips();
    malformed_and_golden();
    distant_and_overlapping();
    skipped_prefix();
    short_copy_boundaries();
    fast_distance_boundaries();
    extended_copy_boundaries();
    table_and_capacity_boundaries();
    unaligned_buffers();
    return UNIT_REPORT();
}
