/*
 * fluff_trace.c -- Tracing infrastructure for fluff compression instrumentation
 *
 * Logs all compression/decompression operations to JSON for stream analysis.
 * Events are buffered and written incrementally to avoid memory overhead.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fluff_trace.h"
#include "fluff_json.h"

/* Global tracing enabled flag */
static int g_trace_enabled = 0;

typedef struct fluff_trace_s {
    fluff_json_t *json;
    int started_events;     /* 1 if events array has begun */
    int started_summary;    /* 1 if summary section has begun */
    /* Statistics collected during trace */
    bp_uint32 input_size;
    bp_uint32 output_size;
    bp_uint32 match_count;
    bp_uint32 literal_count;
    bp_uint64 total_match_len;
    bp_uint32 max_distance;
} fluff_trace_t;

/* ---- Global control ---- */

void fluff_trace_set_enabled(int enabled)
{
    g_trace_enabled = enabled;
}

int fluff_trace_is_enabled(void)
{
    return g_trace_enabled;
}

/* ---- Context lifecycle ---- */

fluff_trace_t *fluff_trace_alloc(void)
{
    fluff_trace_t *ctx;
    if (!g_trace_enabled) return NULL;
    
    ctx = (fluff_trace_t *)malloc(sizeof(fluff_trace_t));
    if (ctx == NULL) return NULL;
    
    memset(ctx, 0, sizeof(fluff_trace_t));
    return ctx;
}

void fluff_trace_free(fluff_trace_t *ctx)
{
    if (ctx == NULL) return;
    free(ctx);
}

void fluff_trace_reset(fluff_trace_t *ctx)
{
    if (ctx == NULL) return;
    
    memset(ctx, 0, sizeof(fluff_trace_t));
}

/* ---- Output file handling ---- */

int fluff_trace_open_output(fluff_trace_t *ctx, const char *filename)
{
    if (ctx == NULL || !g_trace_enabled) return 0;
    
    ctx->json = fluff_json_open(filename);
    if (ctx->json == NULL) {
        return -1;
    }
    
    fluff_json_begin_events(ctx->json);
    ctx->started_events = 1;
    
    return 0;
}

int fluff_trace_close_output(fluff_trace_t *ctx)
{
    int rc = 0;
    
    if (ctx == NULL || ctx->json == NULL) return 0;
    
    if (ctx->started_events) {
        fluff_json_end_events(ctx->json);
    }
    
    fluff_json_begin_summary(ctx->json);
    
    if (ctx->started_summary) {
        /* Summary was already begun by caller, just need to write final stats */
        fluff_json_write_summary_uint32(ctx->json, "total_events",
                                         ctx->match_count + ctx->literal_count);
    }
    
    fluff_json_end_summary(ctx->json);
    
    if (fluff_json_close(ctx->json) != 0) {
        rc = -1;
    }
    
    ctx->json = NULL;
    return rc;
}

int fluff_trace_flush(fluff_trace_t *ctx)
{
    if (ctx == NULL || ctx->json == NULL) return 0;
    return fluff_json_flush(ctx->json);
}

/* ---- Compression phase events ---- */

void fluff_trace_hash_lookup(fluff_trace_t *ctx, bp_uint32 position,
                              bp_uint32 hash, int found,
                              bp_uint32 chain_depth)
{
    char event_json[256];
    
    if (ctx == NULL || ctx->json == NULL) return;
    
    sprintf(event_json,
            "{\"type\":\"hash_lookup\",\"position\":%lu,\"hash\":%lu,"
            "\"found\":%d,\"chain_depth\":%lu}",
            (unsigned long)position,
            (unsigned long)hash,
            found,
            (unsigned long)chain_depth);
    
    fluff_json_write_event(ctx->json, event_json);
}

void fluff_trace_match_found(fluff_trace_t *ctx, bp_uint32 position,
                              bp_uint32 distance, bp_uint32 length,
                              bp_uint32 chain_explored)
{
    char event_json[256];
    
    if (ctx == NULL || ctx->json == NULL) return;
    
    ctx->match_count++;
    if (length > 0) {
        ctx->total_match_len.lo += length;
    }
    if (distance > ctx->max_distance) {
        ctx->max_distance = distance;
    }
    
    sprintf(event_json,
            "{\"type\":\"match_found\",\"position\":%lu,\"distance\":%lu,"
            "\"length\":%lu,\"chain_explored\":%lu}",
            (unsigned long)position,
            (unsigned long)distance,
            (unsigned long)length,
            (unsigned long)chain_explored);
    
    fluff_json_write_event(ctx->json, event_json);
}

void fluff_trace_parse_decision(fluff_trace_t *ctx, bp_uint32 position,
                                 const char *decision,
                                 bp_uint32 byte_value,
                                 bp_uint32 distance,
                                 bp_uint32 length,
                                 int lazy_matched)
{
    char event_json[512];
    
    if (ctx == NULL || ctx->json == NULL || decision == NULL) return;
    
    if (strcmp(decision, "literal") == 0) {
        ctx->literal_count++;
        sprintf(event_json,
                "{\"type\":\"parse_decision\",\"position\":%lu,"
                "\"decision\":\"literal\",\"byte\":%lu}",
                (unsigned long)position,
                (unsigned long)byte_value);
    } else {
        /* match */
        ctx->match_count++;
        sprintf(event_json,
                "{\"type\":\"parse_decision\",\"position\":%lu,"
                "\"decision\":\"match\",\"distance\":%lu,\"length\":%lu,"
                "\"lazy_matched\":%d}",
                (unsigned long)position,
                (unsigned long)distance,
                (unsigned long)length,
                lazy_matched);
    }
    
    fluff_json_write_event(ctx->json, event_json);
}

/* ---- Decompression phase events ---- */

void fluff_trace_decompress_symbol(fluff_trace_t *ctx,
                                    bp_uint32 bit_position,
                                    bp_uint32 symbol,
                                    const char *sym_type,
                                    bp_uint32 value)
{
    char event_json[512];
    
    if (ctx == NULL || ctx->json == NULL || sym_type == NULL) return;
    
    sprintf(event_json,
            "{\"type\":\"decompress_symbol\",\"bit_position\":%lu,"
            "\"symbol\":%lu,\"sym_type\":\"%s\",\"value\":%lu}",
            (unsigned long)bit_position,
            (unsigned long)symbol,
            sym_type,
            (unsigned long)value);
    
    fluff_json_write_event(ctx->json, event_json);
}

void fluff_trace_decompress_output(fluff_trace_t *ctx,
                                    bp_uint32 position,
                                    bp_uint8 byte)
{
    char event_json[256];
    
    if (ctx == NULL || ctx->json == NULL) return;
    
    ctx->literal_count++;
    
    sprintf(event_json,
            "{\"type\":\"decompress_output\",\"position\":%lu,\"byte\":%d}",
            (unsigned long)position,
            byte);
    
    fluff_json_write_event(ctx->json, event_json);
}

/* ---- Compression statistics ---- */

void fluff_trace_get_stats(fluff_trace_t *ctx, fluff_stats_t *stats)
{
    if (ctx == NULL || stats == NULL) {
        return;
    }
    
    memset(stats, 0, sizeof(fluff_stats_t));
    stats->literal_count = ctx->literal_count;
    stats->match_count = ctx->match_count;
    stats->total_bytes_input = ctx->input_size;
    stats->total_bytes_output = ctx->output_size;
}

void fluff_trace_add_stats(fluff_trace_t *ctx, const fluff_stats_t *stats)
{
    if (ctx == NULL || stats == NULL) {
        return;
    }
    
    ctx->literal_count += stats->literal_count;
    ctx->match_count += stats->match_count;
    ctx->input_size += stats->total_bytes_input;
    ctx->output_size += stats->total_bytes_output;
}
