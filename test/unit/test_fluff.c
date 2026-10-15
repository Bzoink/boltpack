/* Stateful Fluff roundtrip and buffer-boundary tests. C89.
 * Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License. */
#include "fluff.h"
#include "test_runner.h"
#include <stdlib.h>
#include <string.h>
/* Include the core here to exhaustively check its private ANS transitions. */
#include "fluff.c"
#undef malloc
#undef calloc
#undef free
#include "../reference/fluff_legacy_fixtures.h"

static bp_uint32 rng = 19387531UL;
static bp_uint32 random_word(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

static int block_roundtrip(fluff_ctx_t *enc, fluff_ctx_t *dec,
                            const bp_uint8 *src, bp_uint32 n)
{
    bp_uint32 cap = fluff_compress_bound(n), clen = cap, dlen = n;
    bp_uint8 *encoded = (bp_uint8 *)malloc(cap + 2);
    bp_uint8 *decoded = (bp_uint8 *)malloc(n + 2);
    int ok;
    if (!encoded || !decoded) exit(2);
    encoded[0] = encoded[cap + 1] = 0xa7;
    decoded[0] = decoded[n + 1] = 0x5c;
    ok = fluff_compress_block_ctx(src, n, encoded + 1, &clen, enc) == BP_OK &&
         encoded[3] == FLUFF_STREAM_VERSION_FSE3 &&
         clen <= cap && encoded[0] == 0xa7 && encoded[cap + 1] == 0xa7 &&
         fluff_decompress_block_ctx(encoded + 1, clen, decoded + 1, &dlen, dec) == BP_OK &&
         dlen == n && !memcmp(src, decoded + 1, n) &&
         decoded[0] == 0x5c && decoded[n + 1] == 0x5c;
    free(encoded); free(decoded);
    return ok;
}

static void boundaries(void)
{
    static const bp_uint32 sizes[] = {
        0,1,2,3,4,5,10,11,18,19,34,35,63,64,65,127,128,
        257,258,259,513,514,515,1281,1282,1283,4353,4354,4355,
        16383,16384,65535,65536,262143,262144
    };
    fluff_ctx_t *enc = fluff_ctx_alloc(), *dec = fluff_ctx_alloc();
    bp_uint8 *src = (bp_uint8 *)malloc(262144);
    bp_uint32 trial, i, n, pattern, period;
    int level, ok = 1;
    if (!enc || !dec || !src) exit(2);
    for (trial = 0; trial < 140 && ok; trial++) {
        n = trial < sizeof(sizes) / sizeof(sizes[0]) ? sizes[trial] : random_word() % 65537;
        pattern = trial % 7; period = 1 + random_word() % 7000;
        for (i = 0; i < n; i++) {
            if (pattern == 0) src[i] = 0;
            else if (pattern == 1) src[i] = (bp_uint8)(i % period);
            else if (pattern == 2 && i >= period && i % 23)
                src[i] = src[i - period];
            else if (pattern == 3) src[i] = (bp_uint8)(random_word() % 5);
            else src[i] = (bp_uint8)random_word();
        }
        for (level = 1; level <= 9 && ok; level++) {
            fluff_ctx_reset_level(enc, level); fluff_ctx_reset_decode(dec);
            ok = block_roundtrip(enc, dec, src, n);
            if (!ok) printf("size=%lu pattern=%lu level=%d\n",
                (unsigned long)n, (unsigned long)pattern, level);
        }
    }
    UNIT_CHECK(ok, "all levels: boundaries, random, runs and near-matches; exact bounds with canaries");
    free(src); fluff_ctx_free(enc); fluff_ctx_free(dec);
}

static void history(void)
{
    fluff_ctx_t *enc = fluff_ctx_alloc(), *dec = fluff_ctx_alloc();
    bp_uint32 window = fluff_window_size(), n = 65536, i, block;
    bp_uint8 *src = (bp_uint8 *)malloc(window + n);
    int level, ok = 1;
    if (!enc || !dec || !src) exit(2);
    for (i = 0; i < window + n; i++) src[i] = (bp_uint8)random_word();
    for (level = 1; level <= 9 && ok; level++) {
        fluff_ctx_reset_level(enc, level); fluff_ctx_reset_decode(dec);
        for (block = 0; block < 18 && ok; block++) {
            bp_uint8 *p = src + (block % 5) * 7919;
            if (block % 4 == 2) {
                /* Compression advances encoder history even when the archive
                 * chooses stored fallback; the decoder consumes raw bytes. */
                bp_uint32 cap = fluff_compress_bound(n), len = cap;
                bp_uint8 *encoded = (bp_uint8 *)malloc(cap);
                if (!encoded) exit(2);
                ok = fluff_compress_block_ctx(p, n, encoded, &len, enc) == BP_OK;
                fluff_ctx_feed_raw(dec, p, n);
                free(encoded);
            } else ok = block_roundtrip(enc, dec, p, block == 17 ? 259 : n);
        }
    }
    UNIT_CHECK(ok, "cross-block history, window wrap, stored fallback and short final block");
    free(src); fluff_ctx_free(enc); fluff_ctx_free(dec);
}

static void ans_transitions(void)
{
    fl_u16 norm[2], slots[FSE_LL_L];
    fse_sym_enc_t enc[2];
    fse_dtab_t dec[FSE_LL_L];
    fl_u32 log_l, count, sym, y, l;
    int ok = 1;
    for (log_l = FSE_D_BITS; log_l <= FSE_LL_BITS && ok; log_l++) {
        l = 1U << log_l;
        for (count = 1; count <= l && ok; count++) {
            norm[0] = (fl_u16)count; norm[1] = (fl_u16)(l - count);
            if (fse_build_ctab(norm, 2, slots, enc, log_l) ||
                fse_build_dtab(norm, 2, dec, log_l, 0)) { ok = 0; break; }
            for (sym = 0; sym < 2 && ok; sym++) {
                if (!norm[sym]) continue;
                for (y = 0; y < l; y++) {
                    fl_u8 data[4];
                    fl_u32 state, decoded_sym;
                    fse_bits_t w;
                    fse_bits_r_t r;
                    fse_bits_init_write(&w, data, sizeof(data));
                    state = fse_encode_sym_fast(y, sym, slots, enc, &w);
                    fse_bits_flush(&w);
                    if (state >= l || w.pos > sizeof(data)) { ok = 0; break; }
                    fse_bits_init_read(&r, data, w.pos);
                    decoded_sym = fse_decode_sym(&state, dec, &r, log_l);
                    if (decoded_sym != sym || state != y) { ok = 0; break; }
                }
            }
        }
    }
    UNIT_CHECK(ok, "ANS encode/decode inverses: every state and two-symbol distribution, including powers of two and singleton");
}

static void legacy_streams(void)
{
    const bp_uint8 *first[2] = {fluff_legacy_fse_0, fluff_legacy_compact_0};
    const bp_uint8 *second[2] = {fluff_legacy_fse_1, fluff_legacy_compact_1};
    bp_uint32 first_size[2] = {sizeof(fluff_legacy_fse_0), sizeof(fluff_legacy_compact_0)};
    bp_uint32 second_size[2] = {sizeof(fluff_legacy_fse_1), sizeof(fluff_legacy_compact_1)};
    bp_uint8 src[1024], decoded[1024], encoded[2048];
    fluff_ctx_t *ctx = fluff_ctx_alloc();
    bp_uint32 i, block, len;
    int compact, reset, ok = 1;
    if (!ctx) exit(2);
    for (compact = 0; compact < 2 && ok; compact++) {
        for (reset = 0; reset < 2 && ok; reset++) {
            fluff_ctx_reset_decode(ctx);
            if (reset) {
                memset(src, 0x91, sizeof(src));
                fluff_ctx_feed_raw(ctx, src, sizeof(src));
            }
            for (block = 0; block < 2 && ok; block++) {
                for (i = 0; i < sizeof(src); i++) {
                    src[i] = (bp_uint8)((i * 29 + (i >> 3) + block * 13) & 255);
                    if (i >= 64 && i % 47) src[i] = src[i - 32];
                }
                len = block ? second_size[compact] : first_size[compact];
                if (len > sizeof(encoded)) { ok = 0; break; }
                memcpy(encoded, block ? second[compact] : first[compact], len);
                if (!block && reset) encoded[2] |= 0x80;
                i = sizeof(decoded);
                ok = fluff_decompress_block_ctx(encoded, len, decoded, &i, ctx) == BP_OK &&
                     i == sizeof(src) && !memcmp(src, decoded, sizeof(src)) &&
                     ctx->file_pos == (block + 1) * sizeof(src);
            }
        }
    }
    UNIT_CHECK(ok, "frozen v0x07/0x08 two-block fixtures and v0x87/0x88 resets");
    fluff_ctx_free(ctx);
}

static void damaged_streams(void)
{
    bp_uint8 src[4096], encoded[8192], mutated[8192], decoded[4098];
    bp_uint32 i, n, trial, clen = sizeof(encoded);
    fluff_ctx_t *enc = fluff_ctx_alloc(), *dec = fluff_ctx_alloc();
    int ok = 1;
    if (!enc || !dec) exit(2);
    for (i = 0; i < sizeof(src); i++) src[i] = (bp_uint8)((i * 17 + (i >> 6)) % 31);
    fluff_ctx_reset_level(enc, 8);
    if (fluff_compress_block_ctx(src, sizeof(src), encoded, &clen, enc) != BP_OK) exit(2);
    for (trial = 0; trial < clen + 1200 && ok; trial++) {
        bp_uint32 input_len = clen, out_len = sizeof(src);
        int rc;
        memcpy(mutated, encoded, clen);
        if (trial < clen) input_len = trial;
        else {
            for (i = 0; i < 1 + trial % 4; i++) {
                n = random_word() % clen;
                mutated[n] ^= (bp_uint8)(1U << (random_word() % 8));
            }
            if (trial % 2) mutated[2] = FLUFF_STREAM_VERSION_FSE3_RESET;
        }
        decoded[0] = decoded[sizeof(decoded) - 1] = 0xa3;
        fluff_ctx_reset_decode(dec);
        rc = fluff_decompress_block_ctx(mutated, input_len, decoded + 1, &out_len, dec);
        ok = decoded[0] == 0xa3 && decoded[sizeof(decoded) - 1] == 0xa3 &&
             (rc != BP_OK || out_len <= sizeof(src));
    }
    UNIT_CHECK(ok, "truncated and mutated new streams: bounded output and intact canaries");
    fluff_ctx_free(enc); fluff_ctx_free(dec);
}

static void reset_stream(void)
{
    bp_uint8 src[2048], encoded[4096], decoded[2048];
    bp_uint32 i, clen = sizeof(encoded), dlen = sizeof(decoded);
    fluff_ctx_t *enc = fluff_ctx_alloc(), *dec = fluff_ctx_alloc();
    int ok;
    if (!enc || !dec) exit(2);
    for (i = 0; i < sizeof(src); i++) src[i] = (bp_uint8)(i % 53);
    fluff_ctx_reset_level(enc, 8);
    fluff_ctx_reset_decode(dec);
    fluff_ctx_feed_raw(dec, src, sizeof(src));
    ok = fluff_compress_block_ctx(src, sizeof(src), encoded, &clen, enc) == BP_OK;
    encoded[2] = FLUFF_STREAM_VERSION_FSE3_RESET;
    ok = ok && fluff_decompress_block_ctx(encoded, clen, decoded, &dlen, dec) == BP_OK &&
         dlen == sizeof(src) && !memcmp(src, decoded, sizeof(src)) &&
         dec->file_pos == sizeof(src);
    UNIT_CHECK(ok, "v0x89 discards preceding decoder history");
    fluff_ctx_free(enc); fluff_ctx_free(dec);
}

static fl_u32 reference_bits_read(fse_bits_r_t *b, fl_u32 n)
{
    while (b->nbits < n) {
        fl_u8 byte = b->pos ? b->buf[--b->pos] : 0;
        b->accum = (b->accum << 8) | byte;
        b->nbits += 8;
    }
    b->nbits -= n;
    return (b->accum >> b->nbits) & ((1U << n) - 1U);
}

static void reader_refills(void)
{
    fl_u8 bytes[67];
    fl_u32 trial, i, width, length;
    int ok = 1;
    for (trial = 0; trial < 500 && ok; trial++) {
        fse_bits_r_t fast, reference;
        length = trial % sizeof(bytes);
        for (i = 0; i < sizeof(bytes); i++) bytes[i] = (fl_u8)random_word();
        fse_bits_init_read(&fast, bytes, length);
        fse_bits_init_read(&reference, bytes, length);
        for (i = 0; i < 150; i++) {
            width = random_word() % 18;
            if (fse_bits_read(&fast, width) != reference_bits_read(&reference, width)) {
                ok = 0; break;
            }
        }
    }
    UNIT_CHECK(ok, "batched backward reader matches byte reader: widths 0..17, tails and zero padding");
}

typedef struct { void *ptr; bp_uint32 size; } test_allocation_t;
static test_allocation_t test_allocations[32];
static bp_uint32 test_live_bytes;
static int test_fail_after = -1, test_bad_free;

static void *test_alloc(bp_uint32 size)
{
    unsigned int i;
    void *ptr;
    if (test_fail_after == 0) return NULL;
    if (test_fail_after > 0) test_fail_after--;
    ptr = malloc(size);
    if (!ptr) return NULL;
    for (i = 0; i < 32; i++) if (!test_allocations[i].ptr) {
        test_allocations[i].ptr = ptr;
        test_allocations[i].size = size;
        test_live_bytes += size;
        return ptr;
    }
    exit(2);
    return NULL;
}

static void test_free(void *ptr)
{
    unsigned int i;
    if (!ptr) return;
    for (i = 0; i < 32; i++) if (test_allocations[i].ptr == ptr) {
        test_live_bytes -= test_allocations[i].size;
        test_allocations[i].ptr = NULL;
        free(ptr);
        return;
    }
    test_bad_free = 1;
    free(ptr);
}

static void allocation_lifecycle(void)
{
    fluff_ctx_t *enc, *dec;
    bp_uint8 *src = (bp_uint8 *)malloc(262144), *saved;
    bp_uint8 encoded[2048];
    bp_uint32 i, len, retained;
    int fail, ok = 1;
    if (!src) exit(2);
    for (i = 0; i < 262144; i++) src[i] = (bp_uint8)random_word();
    fluff_set_allocators(test_alloc, test_free);
    for (fail = 0; fail < 14; fail++) {
        test_fail_after = fail;
        enc = fluff_ctx_alloc();
        if (enc) {
            fluff_ctx_reset_level(enc, 8);
            len = sizeof(encoded);
            (void)fluff_compress_block_ctx(src, 128, encoded, &len, enc);
            fluff_ctx_free(enc);
        }
        ok = ok && test_live_bytes == 0 && !test_bad_free;
    }
    test_fail_after = -1;
    enc = fluff_ctx_alloc(); dec = fluff_ctx_alloc();
    if (!enc || !dec) exit(2);
    fluff_ctx_reset_level(enc, 8); fluff_ctx_reset_decode(dec);
    ok = ok && block_roundtrip(enc, dec, src, 128);
    saved = enc->scratch_tokens;
    retained = test_live_bytes;
    test_fail_after = 0;
    ok = ok && fl_ctx_ensure_scratch(enc, 262144) == BP_ERR_NOMEM &&
         enc->scratch_tokens == saved && enc->scratch_src_cap == 128 &&
         test_live_bytes == retained;
    test_fail_after = -1;
    ok = ok && block_roundtrip(enc, dec, src, 262144);
    saved = enc->scratch_tokens;
    ok = ok && block_roundtrip(enc, dec, src, 3) &&
         enc->scratch_tokens == saved && enc->scratch_src_cap == 262144;
    fluff_ctx_free(enc); fluff_ctx_free(dec);
    ok = ok && test_live_bytes == 0 && !test_bad_free;
    fluff_set_allocators(NULL, NULL);
    free(src);
    UNIT_CHECK(ok, "allocator ownership, failure cleanup, failed growth and scratch reuse");
}

int main(void)
{
    fluff_init();
    UNIT_SECTION("Fluff direct codec");
    reader_refills(); ans_transitions(); legacy_streams(); reset_stream(); boundaries(); history(); damaged_streams();
    allocation_lifecycle();
    return UNIT_REPORT();
}
