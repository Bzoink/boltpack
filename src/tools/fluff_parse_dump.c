/*
 * fluff_parse_dump.c -- Dump fluff LZ77 parse decisions as CSV.
 *
 * Compiles with -DFLUFF_DEBUG_PARSE to activate the parse-trace hook
 * inside fluff.c, then runs the compressor on an input file and writes
 * one CSV row per token to stdout (or an output file):
 *
 *   abs_pos,action,length,distance,literal_byte,is_lazy
 *
 * action is one of: literal, match, rep_match
 *
 * Used for byte-accurate comparison against the Python prototype and
 * for standalone LZ77 parse decision analysis.
 *
 * Build:
 *   make parse-dump        (adds -DFLUFF_DEBUG_PARSE automatically)
 *
 * Usage:
 *   ./build/linux/fluff_parse_dump <input_file> [output.csv]
 *
 * ANSI C (C89/C90) only -- no compiler extensions.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#define FLUFF_DEBUG_PARSE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fluff.h"
#include "bp_types.h"

int main(int argc, char *argv[])
{
    FILE       *in_fp;
    FILE       *out_fp;
    long        file_size;
    size_t      read_n;
    bp_uint8   *src_buf;
    bp_uint8   *dst_buf;
    bp_uint32   dst_cap;
    bp_uint32   dst_len;
    bp_uint32   offset;
    bp_uint32   chunk;
    fluff_ctx_t *ctx;
    int         rc;

    /*
     * Block size: 256 KB — matches what the archiver uses for fluff.
     * Feeding the entire file as one block causes the FSE entropy coder
     * to exhaust its internal limits on large inputs.
     */
    const bp_uint32 BLOCK = 262144U;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <input_file> [output.csv]\n", argv[0]);
        fprintf(stderr, "  Writes parse trace CSV to stdout or output.csv\n");
        fprintf(stderr, "  Columns: abs_pos,action,length,distance,literal_byte,is_lazy\n");
        return 1;
    }

    in_fp = fopen(argv[1], "rb");
    if (!in_fp) {
        fprintf(stderr, "Error: cannot open '%s'\n", argv[1]);
        return 1;
    }

    if (fseek(in_fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "Error: fseek failed on '%s'\n", argv[1]);
        fclose(in_fp);
        return 1;
    }
    file_size = ftell(in_fp);
    if (file_size < 0) {
        fprintf(stderr, "Error: ftell failed on '%s'\n", argv[1]);
        fclose(in_fp);
        return 1;
    }
    rewind(in_fp);

    if (file_size == 0) {
        fprintf(stderr, "Error: input file is empty\n");
        fclose(in_fp);
        return 1;
    }

    src_buf = (bp_uint8 *)malloc((size_t)file_size);
    if (!src_buf) {
        fprintf(stderr, "Error: out of memory (src %ld bytes)\n", file_size);
        fclose(in_fp);
        return 1;
    }

    read_n = fread(src_buf, 1, (size_t)file_size, in_fp);
    fclose(in_fp);
    if ((long)read_n != file_size) {
        fprintf(stderr, "Error: read %lu of %ld bytes\n",
                (unsigned long)read_n, file_size);
        free(src_buf);
        return 1;
    }

    out_fp = stdout;
    if (argc == 3) {
        out_fp = fopen(argv[2], "w");
        if (!out_fp) {
            fprintf(stderr, "Error: cannot write '%s'\n", argv[2]);
            free(src_buf);
            return 1;
        }
    }

    dst_cap = fluff_compress_bound(BLOCK);
    dst_buf = (bp_uint8 *)malloc(dst_cap);
    if (!dst_buf) {
        fprintf(stderr, "Error: out of memory (dst %u bytes)\n",
                (unsigned)dst_cap);
        free(src_buf);
        if (out_fp != stdout) fclose(out_fp);
        return 1;
    }

    /* CSV header */
    fprintf(out_fp, "abs_pos,action,length,distance,literal_byte,is_lazy\n");
    fflush(out_fp);

    ctx = fluff_ctx_alloc();
    if (!ctx) {
        fprintf(stderr, "Error: fluff_ctx_alloc failed\n");
        free(src_buf);
        free(dst_buf);
        if (out_fp != stdout) fclose(out_fp);
        return 1;
    }
    fluff_ctx_reset(ctx);

    /* Process in BLOCK-sized chunks, matching the archiver's behaviour. */
    fluff_set_debug_fp(out_fp);
    rc = BP_OK;
    offset = 0;
    while (offset < (bp_uint32)file_size) {
        chunk = ((bp_uint32)file_size - offset > BLOCK)
                ? BLOCK : (bp_uint32)file_size - offset;
        dst_len = dst_cap;
        rc = fluff_compress_block_ctx(src_buf + offset, chunk,
                                      dst_buf, &dst_len, ctx);
        if (rc != BP_OK) break;
        offset += chunk;
    }
    fluff_set_debug_fp(NULL);

    fluff_ctx_free(ctx);
    free(src_buf);
    free(dst_buf);

    if (out_fp != stdout) {
        fclose(out_fp);
    }

    if (rc != BP_OK) {
        fprintf(stderr, "Error: fluff compression failed (rc=%d)\n", rc);
        return 1;
    }

    return 0;
}
