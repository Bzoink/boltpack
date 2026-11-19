/* Compare the 8086 BoltLite path with the portable 16-bit C codec. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boltlite.h"

#define MAX_BLOCK 32768U
#define MAX_PACKED BOLTLITE_COMPRESS_BOUND(MAX_BLOCK)

bp_uint32 boltlite_compress_c(const bp_uint8 *, bp_uint32, bp_uint8 *, bp_uint8 *);
int boltlite_decompress_c(const bp_uint8 *, bp_uint32, bp_uint8 *, bp_uint32, bp_uint8 *);

static unsigned int cases;
static bp_uint8 *input, *packed, *packed_c, *out, *out_c, *work;
static bp_uint32 rng = 84637UL;

static void require(int ok, const char *check)
{
    if (!ok) {
        printf("FAIL: case %u: %s\n", cases, check);
        exit(1);
    }
}

static bp_uint8 random_byte(void)
{
    rng = 1664525UL * rng + 1013904223UL;
    return (bp_uint8)(rng >> 24);
}

static void check_decode(const bp_uint8 *src, unsigned int size, unsigned int length,
                         const bp_uint8 *expected)
{
    int rc, rc_c;
    ++cases;
    memset(out, 0xCD, length + 2U);
    memset(out_c, 0xCD, length + 2U);
    rc = boltlite_decompress(src, size, out + 1, length, NULL);
    rc_c = boltlite_decompress_c(src, size, out_c + 1, length, NULL);
    require(rc == rc_c, "decoder return code differs");
    require(memcmp(out + 1, out_c + 1, length) == 0, "decoder output differs");
    require(out[0] == 0xCD && out[length + 1] == 0xCD, "output bounds exceeded");
    if (expected != NULL)
        require(rc == BP_OK && memcmp(out + 1, expected, length) == 0, "round trip failed");
}

static void check_roundtrip(unsigned int length)
{
    bp_uint32 size, size_c;
    unsigned int pos;
    bp_uint8 saved;
    size = boltlite_compress(input + 1, length, packed + 1, work);
    size_c = boltlite_compress_c(input + 1, length, packed_c + 1, work);
    require(size > 0 && size == size_c, "encoded length differs");
    require(memcmp(packed + 1, packed_c + 1, (size_t)size) == 0, "encoded bytes differ");
    check_decode(packed + 1, (unsigned int)size, length, input + 1);
    check_decode(packed + 1, (unsigned int)size - 1U, length, NULL);
    packed[size + 1] = 0;
    check_decode(packed + 1, (unsigned int)size + 1U, length, NULL);
    pos = (unsigned int)size / 2U + 1U;
    saved = packed[pos];
    packed[pos] ^= 0x80U;
    check_decode(packed + 1, (unsigned int)size, length, NULL);
    packed[pos] = saved;
}

int main(void)
{
    unsigned int i, j, mode;
    bp_uint8 tokens[] = { 0x01, 'A', 0x00, 0x00, 0xFF };

    input = malloc(MAX_BLOCK + 1U);
    packed = malloc(MAX_PACKED + 2U);
    packed_c = malloc(MAX_PACKED + 2U);
    out = malloc(MAX_BLOCK + 2U);
    out_c = malloc(MAX_BLOCK + 2U);
    work = malloc(BOLTLITE_WORK_SIZE);
    require(input && packed && packed_c && out && out_c && work, "buffer allocation");

    /* Three token forms with and without history, and truncated long tokens. */
    for (i = 0; i < 256U; ++i) {
        tokens[0] = 0;
        tokens[1] = (bp_uint8)i;
        check_decode(tokens, 3, 16, NULL);
        tokens[0] = 1;
        tokens[1] = 'A';
        tokens[3] = (bp_uint8)i;
        check_decode(tokens, 4, 16, NULL);
        check_decode(tokens, 5, 66, NULL);
    }

    /* Full distance field, only valid after exactly 4096 output bytes. */
    for (i = 0; i < 512U; ++i) {
        packed[i * 9U] = 0xFF;
        for (j = 0; j < 8U; ++j) {
            input[i * 8U + j] = random_byte();
            packed[i * 9U + 1U + j] = input[i * 8U + j];
        }
    }
    packed[4608] = 0;
    packed[4609] = 0xFF;
    packed[4610] = 0xFF;
    memcpy(input + 4096, input, 10);
    check_decode(packed, 4611, 4106, input);
    check_decode(packed + 9, 4602, 4098, NULL);

    /* Exercise tail hashes, lazy parsing, overlapping matches and large blocks. */
    for (mode = 0; mode < 4U; ++mode) {
        for (i = 0; i < MAX_BLOCK; ++i) {
            switch (mode) {
                case 0: input[i + 1] = random_byte(); break;
                case 1: input[i + 1] = 'A'; break;
                case 2: input[i + 1] = (bp_uint8)(i % 3U); break;
                default: input[i + 1] = (bp_uint8)((i % 507U) ^ (i / 509U)); break;
            }
        }
        for (i = 1; i <= 72U; ++i) check_roundtrip(i);
        check_roundtrip(4097U);
        check_roundtrip(MAX_BLOCK);
    }

    free(work);
    free(out_c);
    free(out);
    free(packed_c);
    free(packed);
    free(input);
    printf("PASS: %u BoltLite C/8086 cases\n", cases);
    return 0;
}
