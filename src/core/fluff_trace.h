/*
 * fluff_trace.h -- Tracing infrastructure for fluff compression instrumentation
 *
 * Records detailed events during compression/decompression for stream analysis:
 * - Hash table lookups and chain exploration
 * - LZ77 match findings and decisions (literals vs. matches)
 * - FSE/tANS symbol encoding/decoding
 * - Byte-level input/output and round-trip verification
 *
 * All events are streamed to a JSON writer to avoid memory overhead.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#ifndef BP_FLUFF_TRACE_H
#define BP_FLUFF_TRACE_H

#include "bp_types.h"

/* Trace context (opaque) */
typedef struct fluff_trace_s fluff_trace_t;

/* ---- Trace context lifecycle ---- */

/* Allocate tracing context. Pairs with fluff_trace_free(). */
fluff_trace_t *fluff_trace_alloc(void);

/* Free tracing context and release associated resources. */
void fluff_trace_free(fluff_trace_t *ctx);

/* Reset trace for a new compression/decompression session. */
void fluff_trace_reset(fluff_trace_t *ctx);

/* ---- Enable/disable tracing globally ---- */

void fluff_trace_set_enabled(int enabled);
int  fluff_trace_is_enabled(void);

/* ---- Output file handling ---- */

/* Open JSON output file. Must be called before any trace events.
 * Returns 0 on success, -1 on error. */
int fluff_trace_open_output(fluff_trace_t *ctx, const char *filename);

/* Close JSON output file and finalize output. Returns 0 on success. */
int fluff_trace_close_output(fluff_trace_t *ctx);

/* Flush pending events to disk. */
int fluff_trace_flush(fluff_trace_t *ctx);

/* ---- Compression phase events ---- */

/* Log a hash table lookup. */
void fluff_trace_hash_lookup(fluff_trace_t *ctx, bp_uint32 position,
                              bp_uint32 hash, int found,
                              bp_uint32 chain_depth);

/* Log a match found during search. */
void fluff_trace_match_found(fluff_trace_t *ctx, bp_uint32 position,
                              bp_uint32 distance, bp_uint32 length,
                              bp_uint32 chain_explored);

/* Log a parse decision (literal or match). */
void fluff_trace_parse_decision(fluff_trace_t *ctx, bp_uint32 position,
                                 const char *decision,  /* "literal" or "match" */
                                 bp_uint32 byte_value,  /* for literals */
                                 bp_uint32 distance,    /* for matches */
                                 bp_uint32 length,      /* for matches */
                                 int lazy_matched);     /* 1 if lazy match took priority */

/* ---- Decompression phase events ---- */

/* Log symbol decoded from bitstream. */
void fluff_trace_decompress_symbol(fluff_trace_t *ctx,
                                    bp_uint32 bit_position,
                                    bp_uint32 symbol,
                                    const char *sym_type,  /* "literal", "length", "distance", "eob" */
                                    bp_uint32 value);

/* Log a byte written during decompression. */
void fluff_trace_decompress_output(fluff_trace_t *ctx,
                                    bp_uint32 position,
                                    bp_uint8 byte);

/* ---- Compression statistics ---- */

/*
 * Compression statistics structure.
 * Accumulated across all blocks in a file.
 */
typedef struct {
    bp_uint32 literal_count;        /* number of literals emitted */
    bp_uint32 match_count;          /* number of matches emitted */
    bp_uint32 total_chain_probes;   /* total hash chain nodes explored */
    bp_uint32 max_chain_depth;      /* maximum chain depth used */
    bp_uint32 total_bytes_input;    /* total input bytes processed */
    bp_uint32 total_bytes_output;   /* total output bytes generated */
} fluff_stats_t;

/* Get compression statistics from trace context. */
void fluff_trace_get_stats(fluff_trace_t *ctx, fluff_stats_t *stats);

/* Add to existing statistics (for accumulating across blocks). */
void fluff_trace_add_stats(fluff_trace_t *ctx, const fluff_stats_t *stats);

#endif /* BP_FLUFF_TRACE_H */
