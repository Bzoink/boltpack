/*
 * bench_boltlz.c -- native in-memory BoltLZ/LZ4 comparison
 *
 * POSIX timing is confined to this test program; codecs remain C89.
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#define _POSIX_C_SOURCE 200809L
#include "boltlz.h"
#include "reference/boltlz_v2.h"
#include "reference/boltlz_speed_base.h"
#include "reference/boltlz_review_base.h"
#include "reference/boltlz_loop_base.h"
#include "reference/lz4/lz4.h"
#include "reference/lz4/lz4hc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const bp_uint8 *src;
    bp_uint8 *encoded;
    bp_uint32 size, capacity, length;
} bench_block;

static volatile unsigned long consumed;
static int review_baseline;
static int loop_baseline;
#define CODEC_COUNT 12

static double now_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { perror("clock_gettime"); exit(2); }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void encode_all(bench_block *blocks, size_t count, void *work, int codec)
{
    size_t i;
    int result;
    bp_uint32 length;
    for (i = 0; i < count; i++) {
        if (codec >= 10) {
            length = blocks[i].capacity;
            result = boltlz_loop_reference_compress(blocks[i].src, blocks[i].size,
                        blocks[i].encoded, &length, work, codec - 9);
            if (result != BP_OK) { fputs("loop baseline encode failed\n", stderr); exit(2); }
            blocks[i].length = length;
        } else if (codec >= 8) {
            length = blocks[i].capacity;
            result = boltlz_review_reference_compress(blocks[i].src, blocks[i].size,
                        blocks[i].encoded, &length, work, codec - 7);
            if (result != BP_OK) { fputs("review baseline encode failed\n", stderr); exit(2); }
            blocks[i].length = length;
        } else if (codec >= 6) {
            length = blocks[i].capacity;
            result = boltlz_speed_reference_compress(blocks[i].src, blocks[i].size,
                        blocks[i].encoded, &length, work, codec - 5);
            if (result != BP_OK) { fputs("speed baseline encode failed\n", stderr); exit(2); }
            blocks[i].length = length;
        } else if (codec >= 4) {
            result = codec == 4 ?
                LZ4_compress_default((const char *)blocks[i].src,
                                     (char *)blocks[i].encoded, (int)blocks[i].size,
                                     (int)blocks[i].capacity) :
                LZ4_compress_HC_extStateHC(work, (const char *)blocks[i].src,
                                          (char *)blocks[i].encoded, (int)blocks[i].size,
                                          (int)blocks[i].capacity, LZ4HC_CLEVEL_DEFAULT);
            if (result <= 0) { fputs("LZ4 encode failed\n", stderr); exit(2); }
            blocks[i].length = (bp_uint32)result;
        } else {
            length = blocks[i].capacity;
            if (codec < 2)
                result = boltlz_compress(blocks[i].src, blocks[i].size,
                                        blocks[i].encoded, &length, work, codec + 1);
            else
                result = boltlz_reference_compress(blocks[i].src, blocks[i].size,
                                                  blocks[i].encoded, &length, work, codec - 1);
            if (result != BP_OK) { fputs("BoltLZ encode failed\n", stderr); exit(2); }
            blocks[i].length = length;
        }
        consumed += blocks[i].length;
    }
}

static void decode_all(bench_block *blocks, size_t count, bp_uint8 *decoded,
                       int codec, int verify, int frozen)
{
    size_t i, offset;
    int result;
    offset = 0;
    for (i = 0; i < count; i++) {
        if ((frozen && loop_baseline) || codec >= 10) {
            result = boltlz_loop_reference_decompress(blocks[i].encoded, blocks[i].length,
                                                      decoded + offset, blocks[i].size);
            if (result != BP_OK) { fputs("loop baseline decode failed\n", stderr); exit(2); }
        } else if ((frozen && review_baseline) || codec >= 8) {
            result = boltlz_review_reference_decompress(blocks[i].encoded, blocks[i].length,
                                                        decoded + offset, blocks[i].size);
            if (result != BP_OK) { fputs("review baseline decode failed\n", stderr); exit(2); }
        } else if (frozen || codec >= 6) {
            result = boltlz_speed_reference_decompress(blocks[i].encoded, blocks[i].length,
                                                       decoded + offset, blocks[i].size);
            if (result != BP_OK) { fputs("speed baseline decode failed\n", stderr); exit(2); }
        } else if (codec >= 4) {
            result = LZ4_decompress_safe((const char *)blocks[i].encoded,
                                        (char *)decoded + offset,
                                        (int)blocks[i].length, (int)blocks[i].size);
            if (result != (int)blocks[i].size) {
                fputs("LZ4 decode failed\n", stderr); exit(2);
            }
        } else {
            result = boltlz_decompress(blocks[i].encoded, blocks[i].length,
                                      decoded + offset, blocks[i].size);
            if (result != BP_OK) { fputs("BoltLZ decode failed\n", stderr); exit(2); }
        }
        if (verify && memcmp(decoded + offset, blocks[i].src, blocks[i].size) != 0) {
            fputs("decoded bytes differ\n", stderr); exit(2);
        }
        consumed += decoded[offset];
        offset += blocks[i].size;
    }
}

static int same_encoding(bench_block *blocks, size_t count, bp_uint8 *scratch,
                          void *work, int level)
{
    size_t i;
    bp_uint32 length;
    for (i = 0; i < count; i++) {
        length = boltlz_speed_reference_compress_bound(blocks[i].size);
        if ((loop_baseline ?
             boltlz_loop_reference_compress(blocks[i].src, blocks[i].size, scratch,
                                             &length, work, level) : review_baseline ?
             boltlz_review_reference_compress(blocks[i].src, blocks[i].size, scratch,
                                               &length, work, level) :
             boltlz_speed_reference_compress(blocks[i].src, blocks[i].size, scratch,
                                              &length, work, level)) != BP_OK) exit(2);
        if (length != blocks[i].length || memcmp(scratch, blocks[i].encoded, length) != 0)
            return 0;
    }
    return 1;
}

static unsigned int codec_filter(const char *list, const char **names, int count)
{
    unsigned int mask;
    const char *end;
    size_t length;
    int i, found;
    mask = 0;
    while (*list != '\0') {
        end = strchr(list, ',');
        length = end ? (size_t)(end - list) : strlen(list);
        found = 0;
        for (i = 0; i < count; i++) {
            if (strlen(names[i]) == length && strncmp(list, names[i], length) == 0) {
                mask |= 1U << i; found = 1; break;
            }
        }
        if (!found) return 0;
        if (end == NULL) break;
        list = end + 1;
        if (*list == '\0') return 0;
    }
    return mask;
}

int main(int argc, char **argv)
{
    static const char *names[] = {
        "boltlz-fast", "boltlz-balanced",
        "boltlz-before-fast", "boltlz-before-balanced", "lz4", "lz4-hc-9",
        "boltlz-speed-before-fast", "boltlz-speed-before-balanced",
        "boltlz-review-before-fast", "boltlz-review-before-balanced",
        "boltlz-loop-before-fast", "boltlz-loop-before-balanced"
    };
    FILE *file;
    long file_length;
    bp_uint8 *source, *decoded, *encoded, *scratch;
    bench_block *blocks;
    void *work;
    size_t size, count, i, offset, allocation, encoded_bytes, effective_bytes;
    unsigned long block_size, iterations, round, rounds;
    double duration, start, elapsed, encode_time, decode_time;
    double encode_bytes, decode_bytes;
    double frozen_time, frozen_bytes;
    unsigned int mask;
    int codec, order, forward, identical[CODEC_COUNT], require_identical, frozen, paired;
    if (argc != 6 && argc != 8) {
        fputs("usage: bench_boltlz FILE BLOCK_SIZE SECONDS ROUNDS ORDER [REQUIRE_IDENTICAL CODECS]\n", stderr);
        return 2;
    }
    block_size = strtoul(argv[2], NULL, 10);
    duration = atof(argv[3]);
    rounds = strtoul(argv[4], NULL, 10);
    forward = atoi(argv[5]) == 0;
    require_identical = argc == 8 && atoi(argv[6]) != 0;
    mask = argc == 8 ? codec_filter(argv[7], names, CODEC_COUNT) : (1U << CODEC_COUNT) - 1;
    if (mask == 0) return 2;
    review_baseline = (mask & ((1U << 8) | (1U << 9))) != 0;
    loop_baseline = (mask & ((1U << 10) | (1U << 11))) != 0;
    for (codec = 0; codec < CODEC_COUNT; codec++) identical[codec] = 1;
    if (block_size < 4 || block_size > BOLTLZ_MAX_BLOCK ||
        duration <= 0 || duration > 60 || rounds == 0 || rounds > 100) return 2;
    file = fopen(argv[1], "rb");
    if (file == NULL) { perror(argv[1]); return 2; }
    if (fseek(file, 0, SEEK_END) != 0 || (file_length = ftell(file)) <= 0 ||
        file_length > 536870912L || fseek(file, 0, SEEK_SET) != 0) {
        fputs("input must contain 1..512 MiB\n", stderr); fclose(file); return 2;
    }
    size = (size_t)file_length;
    source = (bp_uint8 *)malloc(size);
    decoded = (bp_uint8 *)malloc(size);
    count = (size + block_size - 1) / block_size;
    blocks = (bench_block *)calloc(count, sizeof(*blocks));
    allocation = size + size / 63 + count * 80;
    encoded = (bp_uint8 *)malloc(allocation);
    scratch = (bp_uint8 *)malloc(boltlz_speed_reference_compress_bound(BOLTLZ_MAX_BLOCK));
    allocation = boltlz_work_size(BOLTLZ_BALANCED);
    if (allocation < boltlz_reference_work_size(BOLTLZ_BALANCED))
        allocation = boltlz_reference_work_size(BOLTLZ_BALANCED);
    if (allocation < (size_t)LZ4_sizeofStateHC()) allocation = (size_t)LZ4_sizeofStateHC();
    work = malloc(allocation);
    if (!source || !decoded || !blocks || !encoded || !work || !scratch) {
        fputs("allocation failed\n", stderr); return 2;
    }
    if (fread(source, 1, size, file) != size) { fputs("read failed\n", stderr); return 2; }
    fclose(file);
    offset = 0;
    allocation = 0;
    for (i = 0; i < count; i++) {
        blocks[i].size = (bp_uint32)(size - offset < block_size ? size - offset : block_size);
        blocks[i].src = source + offset;
        blocks[i].capacity = blocks[i].size + blocks[i].size / 63 + 80;
        blocks[i].encoded = encoded + allocation;
        offset += blocks[i].size;
        allocation += blocks[i].capacity;
    }
    for (round = 0; round < rounds; round++) {
        for (order = 0; order < CODEC_COUNT; order++) {
            codec = ((round % 2 == 0) == forward) ? order : CODEC_COUNT - 1 - order;
            if ((mask & (1U << codec)) == 0) continue;
            paired = codec == 0 || codec == 1;
            /* Untimed warmup, allocation, and exact-byte verification. */
            encode_all(blocks, count, work, codec);
            decode_all(blocks, count, decoded, codec, 1, 0);
            if (paired && round == 0) {
                identical[codec] = same_encoding(blocks, count, scratch, work, codec + 1);
                if (require_identical && !identical[codec]) {
                    fputs("candidate output differs from the frozen speed baseline\n", stderr);
                    return 2;
                }
            }
            iterations = 0;
            start = now_seconds();
            do {
                encode_all(blocks, count, work, codec);
                iterations++;
                elapsed = now_seconds() - start;
            } while (elapsed < duration);
            encode_time = elapsed;
            encode_bytes = (double)size * iterations;
            decode_time = decode_bytes = frozen_time = frozen_bytes = 0;
            /* Alternate decoder order on the very same encoded blocks. */
            for (frozen = 0; frozen < (paired ? 2 : 1); frozen++) {
                int use_frozen;
                use_frozen = paired ? ((((round % 2 == 0) == forward) ? frozen : 1 - frozen)) : 0;
                decode_all(blocks, count, decoded, codec, 1, use_frozen);
                iterations = 0;
                start = now_seconds();
                do {
                    decode_all(blocks, count, decoded, codec, 0, use_frozen);
                    iterations++;
                    elapsed = now_seconds() - start;
                } while (elapsed < duration);
                if (use_frozen) { frozen_time = elapsed; frozen_bytes = (double)size * iterations; }
                else { decode_time = elapsed; decode_bytes = (double)size * iterations; }
                decode_all(blocks, count, decoded, codec, 1, use_frozen);
            }
            encoded_bytes = effective_bytes = 0;
            for (i = 0; i < count; i++) {
                encoded_bytes += blocks[i].length;
                effective_bytes += blocks[i].length >= blocks[i].size - blocks[i].size / 100 ?
                                   blocks[i].size : blocks[i].length;
            }
            printf("{\"codec\":\"%s\",\"round\":%lu,\"input_bytes\":%.0f,"
                   "\"encoded_bytes\":%.0f,\"effective_bytes\":%.0f,"
                   "\"encode_seconds\":%.9f,\"encode_bytes\":%.0f,"
                   "\"decode_seconds\":%.9f,\"decode_bytes\":%.0f,"
                   "\"workspace_bytes\":%lu,\"encoder_identical\":%s,"
                   "\"frozen_decode_seconds\":%.9f,\"frozen_decode_bytes\":%.0f,"
                   "\"frozen_decoder\":\"%s\",\"valid\":true}\n",
                   names[codec], round, (double)size, (double)encoded_bytes,
                   (double)effective_bytes, encode_time, encode_bytes,
                   decode_time, decode_bytes, (unsigned long)(codec >= 10 ? boltlz_loop_reference_work_size(codec - 9) :
                                   codec >= 8 ? boltlz_review_reference_work_size(codec - 7) :
                                   codec >= 6 ? boltlz_speed_reference_work_size(codec - 5) :
                                   codec == 4 ? LZ4_sizeofState() :
                                   codec == 5 ? LZ4_sizeofStateHC() :
                                   codec < 2 ? boltlz_work_size(codec + 1) :
                                   boltlz_reference_work_size(codec - 1)),
                   paired ? (identical[codec] ? "true" : "false") : "null", frozen_time, frozen_bytes,
                   paired ? (loop_baseline ? "loop" : review_baseline ? "review" : "speed") : "none");
        }
    }
    free(scratch); free(work); free(encoded); free(blocks); free(decoded); free(source);
    return 0;
}
