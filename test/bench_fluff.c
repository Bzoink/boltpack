/* Direct, stateful Fluff benchmark. Inputs are loaded before timing.
 * Usage: bench_fluff LEVEL REPETITIONS BLOCK_SIZE FILE...
 * POSIX timing belongs to the harness; the codec remains C89.
 * Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.
 */
#define _POSIX_C_SOURCE 200809L
#include "fluff.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    bp_uint8 *data;
    bp_uint32 size, capacity, length;
} block_t;

typedef struct {
    const char *name;
    int (*init)(void);
    fluff_ctx_t *(*alloc)(void);
    void (*free_ctx)(fluff_ctx_t *);
    void (*reset)(fluff_ctx_t *, int);
    void (*reset_decode)(fluff_ctx_t *);
    int (*encode)(const bp_uint8 *, bp_uint32, bp_uint8 *, bp_uint32 *, fluff_ctx_t *);
    int (*decode)(const bp_uint8 *, bp_uint32, bp_uint8 *, bp_uint32 *, fluff_ctx_t *);
} codec_t;

#ifdef FLUFF_BENCH_PAIRED
extern int baseline_fluff_init(void);
extern fluff_ctx_t *baseline_fluff_ctx_alloc(void);
extern void baseline_fluff_ctx_free(fluff_ctx_t *);
extern void baseline_fluff_ctx_reset_level(fluff_ctx_t *, int);
extern void baseline_fluff_ctx_reset_decode(fluff_ctx_t *);
extern int baseline_fluff_compress_block_ctx(const bp_uint8 *, bp_uint32, bp_uint8 *, bp_uint32 *, fluff_ctx_t *);
extern int baseline_fluff_decompress_block_ctx(const bp_uint8 *, bp_uint32, bp_uint8 *, bp_uint32 *, fluff_ctx_t *);
#endif

static const codec_t codecs[] = {
#ifdef FLUFF_BENCH_PAIRED
    {"baseline", baseline_fluff_init, baseline_fluff_ctx_alloc, baseline_fluff_ctx_free,
     baseline_fluff_ctx_reset_level, baseline_fluff_ctx_reset_decode,
     baseline_fluff_compress_block_ctx, baseline_fluff_decompress_block_ctx},
#endif
    {"candidate", fluff_init, fluff_ctx_alloc, fluff_ctx_free,
     fluff_ctx_reset_level, fluff_ctx_reset_decode,
     fluff_compress_block_ctx, fluff_decompress_block_ctx}
};
#define CODEC_COUNT (sizeof(codecs) / sizeof(codecs[0]))

static double seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) { perror("clock_gettime"); exit(2); }
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void fail(const char *path, const char *reason)
{
    fprintf(stderr, "%s: %s\n", path, reason);
    exit(2);
}

int main(int argc, char **argv)
{
    int level, reps, arg, rep;
    size_t variant, turn;
    bp_uint32 block_size;
    fluff_ctx_t *encoders[CODEC_COUNT], *decoders[CODEC_COUNT];
    if (argc < 5) {
        fputs("usage: bench_fluff LEVEL REPETITIONS BLOCK_SIZE FILE...\n", stderr);
        return 2;
    }
    level = atoi(argv[1]); reps = atoi(argv[2]);
    block_size = (bp_uint32)strtoul(argv[3], NULL, 10);
    if (level < 1 || level > 9 || reps < 1 || !block_size || block_size > 262144U)
        return 2;
    for (variant = 0; variant < CODEC_COUNT; variant++) {
        codecs[variant].init();
        encoders[variant] = codecs[variant].alloc();
        decoders[variant] = codecs[variant].alloc();
        if (!encoders[variant] || !decoders[variant]) return 2;
    }
    puts("file,variant,level,block_size,iteration,input_bytes,encoded_bytes,encode_seconds,decode_seconds,reset_seconds,encoded_hash,verified");
    for (arg = 4; arg < argc; arg++) {
        FILE *f = fopen(argv[arg], "rb");
        long length;
        size_t count, j, offset;
        bp_uint8 *src, *out;
        block_t *blocks;
        bp_uint32 hashes[CODEC_COUNT];
        if (!f) fail(argv[arg], "open failed");
        if (fseek(f, 0, SEEK_END) || (length = ftell(f)) < 0 ||
            (unsigned long)length > 0xffffffffUL || fseek(f, 0, SEEK_SET))
            fail(argv[arg], "invalid file size");
        src = (bp_uint8 *)malloc((size_t)length + 1);
        out = (bp_uint8 *)malloc(block_size);
        count = length ? ((size_t)length - 1) / block_size + 1 : 1;
        blocks = (block_t *)calloc(count, sizeof(*blocks));
        if (!src || !out || !blocks) fail(argv[arg], "allocation failed");
        if (fread(src, 1, (size_t)length, f) != (size_t)length) fail(argv[arg], "read failed");
        fclose(f);
        for (j = 0, offset = 0; j < count; j++) {
            size_t n = (size_t)length - offset;
            if (n > block_size) n = block_size;
            blocks[j].size = (bp_uint32)n;
            blocks[j].capacity = fluff_compress_bound((bp_uint32)n);
            blocks[j].data = (bp_uint8 *)malloc(blocks[j].capacity);
            if (!blocks[j].data) fail(argv[arg], "allocation failed");
            offset += n;
        }
        /* Iteration -1 warms tables, scratch allocations, pages and code. */
        for (rep = -1; rep < reps; rep++) {
          for (turn = 0; turn < CODEC_COUNT; turn++) {
            const codec_t *codec;
            fluff_ctx_t *enc, *dec;
            double start, reset_time, encode_time, decode_time;
            size_t encoded = 0;
            bp_uint32 hash = 2166136261UL;
            variant = rep >= 0 && rep % 2 ? CODEC_COUNT - 1 - turn : turn;
            codec = &codecs[variant];
            enc = encoders[variant]; dec = decoders[variant];
            start = seconds();
            codec->reset(enc, level);
            reset_time = seconds() - start;
            start = seconds();
            for (j = 0, offset = 0; j < count; j++) {
                blocks[j].length = blocks[j].capacity;
                if (codec->encode(src + offset, blocks[j].size,
                        blocks[j].data, &blocks[j].length, enc) != BP_OK)
                    fail(argv[arg], "encode failed");
                offset += blocks[j].size;
                encoded += blocks[j].length;
            }
            encode_time = seconds() - start;
            codec->reset_decode(dec);
            start = seconds();
            for (j = 0; j < count; j++) {
                bp_uint32 n = blocks[j].size;
                if (codec->decode(blocks[j].data, blocks[j].length,
                        out, &n, dec) != BP_OK || n != blocks[j].size)
                    fail(argv[arg], "decode failed");
            }
            decode_time = seconds() - start;
            /* Verification is deliberately outside both timed loops. */
            /* Candidate decoder also verifies frozen baseline streams. */
            dec = decoders[CODEC_COUNT - 1];
            codecs[CODEC_COUNT - 1].reset_decode(dec);
            for (j = 0, offset = 0; j < count; j++) {
                bp_uint32 n = blocks[j].size;
                bp_uint32 k;
                if (codecs[CODEC_COUNT - 1].decode(blocks[j].data, blocks[j].length,
                        out, &n, dec) != BP_OK || n != blocks[j].size ||
                        memcmp(src + offset, out, n)) fail(argv[arg], "verification failed");
                for (k = 0; k < blocks[j].length; k++)
                    hash = (hash ^ blocks[j].data[k]) * 16777619UL;
                offset += n;
            }
            if (rep == -1) hashes[variant] = hash;
            else if (hashes[variant] != hash) fail(argv[arg], "encoding is not deterministic");
            if (rep >= 0) {
                /* Quote paths to keep the output valid CSV. */
                const char *p;
                putchar('"');
                for (p = argv[arg]; *p; p++) { if (*p == '"') putchar('"'); putchar(*p); }
                printf("\",%s,%d,%u,%d,%lu,%lu,%.9f,%.9f,%.9f,%lu,1\n", codec->name, level,
                    (unsigned)block_size, rep, (unsigned long)length,
                    (unsigned long)encoded, encode_time, decode_time, reset_time, (unsigned long)hash);
                fflush(stdout);
            }
          }
        }
        for (j = 0; j < count; j++) free(blocks[j].data);
        free(blocks); free(src); free(out);
    }
    for (variant = 0; variant < CODEC_COUNT; variant++) {
        codecs[variant].free_ctx(encoders[variant]);
        codecs[variant].free_ctx(decoders[variant]);
    }
    return 0;
}
