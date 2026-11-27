/*
 * bpr_decode.c -- decode one archive block with the core codecs.
 *
 * Shared by whole-entry extraction and partial recovery.  The caller reads and
 * CRC-checks the block into d->in; bpr_dec_block() produces d->out (orig
 * bytes).  A block with comp == orig is raw under any method; under Fluff it is
 * also fed into the history window, as the CLI does.
 */
#include "bpr.h"

#include "boltlite.h"
#include "compress.h"
#include "fluff.h"

#include <stdlib.h>
#include <string.h>

int bpr_grow(uint8_t **p, size_t *cap, size_t need)
{
    uint8_t *n;
    if (need <= *cap) return 0;
    n = (uint8_t *)realloc(*p, need);
    if (n == NULL) return -1;
    *p = n;
    *cap = need;
    return 0;
}

int bpr_dec_begin(bpr_decoder *d, int method)
{
    memset(d, 0, sizeof(*d));
    d->method = method;
    if (method > 3) return -1;
    if (method == BP_METHOD_FLUFF) {
        d->fctx = fluff_ctx_alloc();
        if (d->fctx == NULL) return -1;
        fluff_ctx_reset_decode((fluff_ctx_t *)d->fctx);
    }
    return 0;
}

void bpr_dec_end(bpr_decoder *d)
{
    if (d->fctx != NULL) fluff_ctx_free((fluff_ctx_t *)d->fctx);
    free(d->in);
    free(d->out);
}

/* Decode one block already read into d->in (comp bytes) into d->out. */
const char *bpr_dec_block(bpr_decoder *d, uint32_t comp, uint32_t orig)
{
    int rc;
    if (bpr_grow(&d->out, &d->out_cap, orig) != 0) return "no-memory";
    if (comp == orig) {
        memcpy(d->out, d->in, orig);
        if (d->fctx != NULL) fluff_ctx_feed_raw((fluff_ctx_t *)d->fctx, d->out, orig);
        return NULL;
    }
    switch (d->method) {
    case BP_METHOD_BOLTLITE:
        rc = boltlite_decompress(d->in, comp, d->out, orig, NULL);
        break;
    case BP_METHOD_BOLTLZ: {
        bp_uint32 dl = orig;
        rc = bp_decompress_block(d->in, comp, d->out, &dl);
        if (rc == BP_OK && dl != orig) rc = BP_ERR_CORRUPT;
        break;
    }
    case BP_METHOD_FLUFF: {
        bp_uint32 dl = orig;
        rc = fluff_decompress_block_ctx(d->in, comp, d->out, &dl, (fluff_ctx_t *)d->fctx);
        if (rc == BP_OK && dl != orig) rc = BP_ERR_CORRUPT;
        break;
    }
    default:
        rc = BP_ERR_CORRUPT;            /* stored entries have raw blocks only */
    }
    return rc == BP_OK ? NULL : "decode-error";
}

