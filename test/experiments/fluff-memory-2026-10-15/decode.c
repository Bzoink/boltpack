/* Longer paired decoder timings for the byte-identical memory experiment.
 * Link with the two codec objects built by test/bench_fluff.py.
 * Usage: decode REPETITIONS PAIRS FILE...
 * Compression and verification are outside the decoder timing. */
#define main unused_codec_benchmark_main
#include "../../bench_fluff.c"
#undef main

int main(int argc, char **argv)
{
    fluff_ctx_t *enc, *dec[CODEC_COUNT];
    int arg, pair, repetitions, pairs, round;
    size_t variant, turn;
    const bp_uint32 block_size = 262144;
    if (argc < 4 || CODEC_COUNT != 2) return 2;
    repetitions = atoi(argv[1]); pairs = atoi(argv[2]);
    if (repetitions < 1 || pairs < 1) return 2;
    enc = fluff_ctx_alloc();
    if (!enc) return 2;
    for (variant = 0; variant < CODEC_COUNT; variant++) {
        codecs[variant].init();
        dec[variant] = codecs[variant].alloc();
        if (!dec[variant]) return 2;
    }
    puts("file,variant,pair,repetitions,input_bytes,decode_seconds,verified");
    for (arg = 3; arg < argc; arg++) {
        FILE *f = fopen(argv[arg], "rb");
        long length = 0;
        size_t count, j, offset;
        bp_uint8 *src, *out;
        block_t *blocks;
        if (!f) fail(argv[arg], "open failed");
        if (fseek(f, 0, SEEK_END) || (length = ftell(f)) < 0 ||
            (unsigned long)length > 0xffffffffUL || fseek(f, 0, SEEK_SET))
            fail(argv[arg], "invalid size");
        src = (bp_uint8 *)malloc((size_t)length + 1);
        out = (bp_uint8 *)malloc(block_size);
        count = length ? ((size_t)length - 1) / block_size + 1 : 1;
        blocks = (block_t *)calloc(count, sizeof(*blocks));
        if (!src || !out || !blocks) return 2;
        if (fread(src, 1, (size_t)length, f) != (size_t)length) return 2;
        fclose(f);
        fluff_ctx_reset_level(enc, 8);
        for (j = 0, offset = 0; j < count; j++) {
            size_t n = (size_t)length - offset;
            if (n > block_size) n = block_size;
            blocks[j].size = (bp_uint32)n;
            blocks[j].capacity = fluff_compress_bound((bp_uint32)n);
            blocks[j].length = blocks[j].capacity;
            blocks[j].data = (bp_uint8 *)malloc(blocks[j].capacity);
            if (!blocks[j].data || fluff_compress_block_ctx(src + offset,
                blocks[j].size, blocks[j].data, &blocks[j].length, enc)) return 2;
            offset += n;
        }
        for (pair = -1; pair < pairs; pair++) {
            for (turn = 0; turn < CODEC_COUNT; turn++) {
                double start, elapsed;
                const codec_t *codec;
                variant = pair >= 0 && pair % 2 ? CODEC_COUNT - 1 - turn : turn;
                codec = &codecs[variant];
                start = seconds();
                for (round = 0; round < repetitions; round++) {
                    codec->reset_decode(dec[variant]);
                    for (j = 0; j < count; j++) {
                        bp_uint32 n = block_size;
                        if (codec->decode(blocks[j].data, blocks[j].length,
                            out, &n, dec[variant]) || n != blocks[j].size)
                            fail(argv[arg], "decode failed");
                    }
                }
                elapsed = (seconds() - start) / repetitions;
                codec->reset_decode(dec[variant]);
                for (j = 0, offset = 0; j < count; j++) {
                    bp_uint32 n = block_size;
                    if (codec->decode(blocks[j].data, blocks[j].length,
                        out, &n, dec[variant]) || n != blocks[j].size ||
                        memcmp(src + offset, out, n)) fail(argv[arg], "verification failed");
                    offset += n;
                }
                if (pair >= 0) {
                    printf("\"%s\",%s,%d,%d,%lu,%.9f,1\n", argv[arg],
                        codec->name, pair, repetitions, (unsigned long)length, elapsed);
                    fflush(stdout);
                }
            }
        }
        for (j = 0; j < count; j++) free(blocks[j].data);
        free(blocks); free(src); free(out);
    }
    fluff_ctx_free(enc);
    for (variant = 0; variant < CODEC_COUNT; variant++) codecs[variant].free_ctx(dec[variant]);
    return 0;
}
