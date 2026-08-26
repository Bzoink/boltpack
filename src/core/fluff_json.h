/*
 * fluff_json.h -- Streaming JSON writer for fluff trace output
 *
 * Writes well-formed JSON incrementally to a file without buffering
 * the entire structure in memory. Supports metadata, event arrays, and summaries.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_FLUFF_JSON_H
#define BP_FLUFF_JSON_H

#include <stdio.h>
#include "bp_types.h"

/* JSON writer context (opaque) */
typedef struct fluff_json_s fluff_json_t;

/* Open JSON output file for streaming. */
fluff_json_t *fluff_json_open(const char *filename);

/* Close JSON file and finalize structure. Returns 0 on success. */
int fluff_json_close(fluff_json_t *w);

/* Flush pending writes to disk. */
int fluff_json_flush(fluff_json_t *w);

/* ---- Metadata section (before events) ---- */

/* Begin metadata section. */
void fluff_json_begin_metadata(fluff_json_t *w);

/* Write a string metadata field. */
void fluff_json_write_meta_string(fluff_json_t *w, const char *key,
                                   const char *value);

/* Write an integer metadata field. */
void fluff_json_write_meta_uint32(fluff_json_t *w, const char *key,
                                   bp_uint32 value);

/* End metadata section. */
void fluff_json_end_metadata(fluff_json_t *w);

/* ---- Events array ---- */

/* Begin events array. */
void fluff_json_begin_events(fluff_json_t *w);

/* Write an event object. Each call writes one complete JSON object. */
void fluff_json_write_event(fluff_json_t *w, const char *event_json);

/* End events array. */
void fluff_json_end_events(fluff_json_t *w);

/* ---- Summary section ---- */

/* Begin summary section. */
void fluff_json_begin_summary(fluff_json_t *w);

/* Write a summary field. */
void fluff_json_write_summary_uint32(fluff_json_t *w, const char *key,
                                      bp_uint32 value);

void fluff_json_write_summary_int64(fluff_json_t *w, const char *key,
                                     long value);

void fluff_json_write_summary_double(fluff_json_t *w, const char *key,
                                      double value);

void fluff_json_write_summary_bool(fluff_json_t *w, const char *key,
                                    int value);

/* End summary section. */
void fluff_json_end_summary(fluff_json_t *w);

/* ---- Error checking ---- */

/* Check if write errors have occurred. */
int fluff_json_has_error(fluff_json_t *w);

/* Get last error message (if any). */
const char *fluff_json_error_msg(fluff_json_t *w);

#endif /* BP_FLUFF_JSON_H */
