/* Compare the frozen archive-format-4 LZSS with the current BoltLite codec.
 * Usage: bench_boltlite [--rounds N] file ...
 */

#include "../src/core/boltlite.h"
#include "reference/lzss_method1_v4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_BLOCK 32768U
#define TEST_OLD_WORK LZSS_V4_WORK_SIZE
#define TEST_OLD_BOUND(n) ((n) + (n) / 8U + 16U)

static unsigned char *read_file(const char *path, unsigned long *size)
{
    FILE *fp;
    long n;
    unsigned char *data;
    fp = fopen(path, "rb");
    if (fp == NULL) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)n + 1U);
    if (data == NULL) { fclose(fp); return NULL; }
    if (fread(data, 1, (size_t)n, fp) != (size_t)n) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *size = (unsigned long)n;
    return data;
}

static int benchmark_file(const char *path, unsigned int rounds)
{
    unsigned long file_size;
    unsigned char *input;
    unsigned char *old_comp;
    unsigned char *new_comp;
    unsigned char *decoded;
    unsigned char *old_work;
    unsigned char *new_work;
    unsigned long old_size;
    unsigned long new_size;
    unsigned long old_effective;
    unsigned long new_effective;
    unsigned long off;
    unsigned long amount;
    unsigned long clen;
    unsigned int round;
    clock_t start;
    double old_encode_seconds;
    double new_encode_seconds;
    double old_decode_seconds;
    double new_decode_seconds;
    bp_uint32 got;
    int ok;
    int new_decode_rc;
    unsigned long mismatch;

    input = read_file(path, &file_size);
    if (input == NULL) {
        fprintf(stderr, "cannot read %s\n", path);
        return 0;
    }
    old_comp = (unsigned char *)malloc((size_t)TEST_OLD_BOUND(TEST_BLOCK));
    new_comp = (unsigned char *)malloc((size_t)BOLTLITE_COMPRESS_BOUND(TEST_BLOCK));
    decoded = (unsigned char *)malloc(TEST_BLOCK);
    old_work = (unsigned char *)malloc(TEST_OLD_WORK);
    new_work = (unsigned char *)malloc(BOLTLITE_WORK_SIZE);
    if (!old_comp || !new_comp || !decoded || !old_work || !new_work) {
        fprintf(stderr, "allocation failed\n");
        free(input); free(old_comp); free(new_comp); free(decoded);
        free(old_work); free(new_work);
        return 0;
    }

    old_size = new_size = old_effective = new_effective = 0;
    old_encode_seconds = new_encode_seconds = 0.0;
    old_decode_seconds = new_decode_seconds = 0.0;
    ok = 1;

    for (off = 0; off < file_size; off += TEST_BLOCK) {
        amount = file_size - off;
        if (amount > TEST_BLOCK) amount = TEST_BLOCK;
        start = clock();
        for (round = 0; round < rounds; round++) {
            got = lzss_v4_compress(input + off, (bp_uint32)amount, old_comp, old_work);
            if (got == 0) { fprintf(stderr, "old encoder returned zero\n"); ok = 0; break; }
        }
        old_encode_seconds += (double)(clock() - start) / CLOCKS_PER_SEC;
        if (!ok) break;
        old_size += got;
        old_effective += (got * 100UL < amount * 99UL) ? got : amount;
        start = clock();
        for (round = 0; round < rounds; round++) {
            if (lzss_v4_decompress(old_comp, got, decoded, (bp_uint32)amount, NULL) != 0 ||
                memcmp(decoded, input + off, (size_t)amount) != 0) {
                fprintf(stderr, "old decoder/memcmp failed: encoded=%lu\n", (unsigned long)got);
                ok = 0;
                break;
            }
        }
        old_decode_seconds += (double)(clock() - start) / CLOCKS_PER_SEC;
        if (!ok) break;

        start = clock();
        for (round = 0; round < rounds; round++) {
            clen = boltlite_compress(input + off, (bp_uint32)amount,
                                 new_comp, new_work);
            if (clen == 0) { fprintf(stderr, "new encoder returned zero\n"); ok = 0; break; }
        }
        new_encode_seconds += (double)(clock() - start) / CLOCKS_PER_SEC;
        if (!ok) break;
        new_size += clen;
        new_effective += (clen * 100UL < amount * 99UL) ? clen : amount;
        start = clock();
        for (round = 0; round < rounds; round++) {
            new_decode_rc = boltlite_decompress(new_comp, (bp_uint32)clen,
                                            decoded, (bp_uint32)amount, NULL);
            mismatch = 0;
            while (mismatch < amount && decoded[mismatch] == input[off + mismatch])
                mismatch++;
            if (new_decode_rc != BP_OK || mismatch != amount) {
                fprintf(stderr, "new decoder/memcmp failed: encoded=%lu rc=%d mismatch=%lu\n",
                        (unsigned long)clen, new_decode_rc, mismatch);
                ok = 0;
                break;
            }
        }
        new_decode_seconds += (double)(clock() - start) / CLOCKS_PER_SEC;
        if (!ok) break;
    }

    if (!ok) {
        fprintf(stderr, "roundtrip/encode failed for %s at offset %lu\n", path, off);
    } else {
        printf("%s,%lu,%lu,%lu,%lu,%lu,%.2f,%.2f,%.2f,%.2f,%u,%u\n",
               path, file_size, old_size, new_size, old_effective, new_effective,
               (double)file_size * rounds / old_encode_seconds / 1048576.0,
               (double)file_size * rounds / new_encode_seconds / 1048576.0,
               (double)file_size * rounds / old_decode_seconds / 1048576.0,
               (double)file_size * rounds / new_decode_seconds / 1048576.0,
               TEST_OLD_WORK, BOLTLITE_WORK_SIZE);
    }

    free(input); free(old_comp); free(new_comp); free(decoded);
    free(old_work); free(new_work);
    return ok;
}

int main(int argc, char **argv)
{
    unsigned int rounds;
    int i;
    int first_file;
    int ok;
    rounds = 3;
    first_file = 1;
    if (argc > 2 && strcmp(argv[1], "--rounds") == 0) {
        rounds = (unsigned int)atoi(argv[2]);
        if (rounds == 0) return 2;
        first_file = 3;
    }
    if (argc <= first_file) {
        fprintf(stderr, "usage: %s [--rounds N] file ...\n", argv[0]);
        return 2;
    }
    printf("file,input_bytes,old_raw,new_raw,old_effective,new_effective,old_enc_mib_s,new_enc_mib_s,old_dec_mib_s,new_dec_mib_s,old_work,new_work\n");
    ok = 1;
    for (i = first_file; i < argc; i++)
        if (!benchmark_file(argv[i], rounds)) ok = 0;
    return ok ? 0 : 1;
}
