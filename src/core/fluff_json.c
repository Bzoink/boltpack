/*
 * fluff_json.c -- Streaming JSON writer for fluff trace output
 *
 * Writes JSON incrementally without buffering entire structure in memory.
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fluff_json.h"

#define JSON_BUFFER_SIZE 4096
#define JSON_ERROR_MSG_SIZE 256

typedef struct fluff_json_s {
    FILE *fp;
    char *buf;
    size_t buf_size;
    size_t buf_pos;
    int in_object;      /* 1 if we are in an object and need comma */
    int in_array;       /* 1 if we are in an array and need comma */
    int first_event;    /* 1 before any event has been written */
    int has_error;
    char error_msg[JSON_ERROR_MSG_SIZE];
} fluff_json_t;

/* ---- Utility functions ---- */

static void json_error(fluff_json_t *w, const char *msg)
{
    w->has_error = 1;
    strncpy(w->error_msg, msg, JSON_ERROR_MSG_SIZE - 1);
    w->error_msg[JSON_ERROR_MSG_SIZE - 1] = '\0';
}

static int json_write_data(fluff_json_t *w, const char *data, size_t len)
{
    if (w->has_error) return -1;
    
    if (len > 0) {
        if (fwrite(data, 1, len, w->fp) != len) {
            json_error(w, "write failed");
            return -1;
        }
    }
    return 0;
}

static int json_write_string(fluff_json_t *w, const char *str)
{
    return json_write_data(w, str, strlen(str));
}

/* Escape value for JSON string (simple: handle quotes only) */
static int json_write_escape(fluff_json_t *w, const char *str)
{
    const char *p = str;
    
    while (*p) {
        if (*p == '"') {
            if (json_write_string(w, "\\\"") < 0) return -1;
        } else if (*p == '\\') {
            if (json_write_string(w, "\\\\") < 0) return -1;
        } else if (*p == '\n') {
            if (json_write_string(w, "\\n") < 0) return -1;
        } else if (*p == '\r') {
            if (json_write_string(w, "\\r") < 0) return -1;
        } else {
            char ch = *p;
            if (json_write_data(w, &ch, 1) < 0) return -1;
        }
        p++;
    }
    return 0;
}

/* ---- Context lifecycle ---- */

fluff_json_t *fluff_json_open(const char *filename)
{
    fluff_json_t *w;
    char buf[128];
    time_t now;
    struct tm *tm_info;
    
    w = (fluff_json_t *)malloc(sizeof(fluff_json_t));
    if (w == NULL) return NULL;
    
    memset(w, 0, sizeof(fluff_json_t));
    w->first_event = 1;
    
    w->fp = fopen(filename, "wb");
    if (w->fp == NULL) {
        json_error(w, "cannot open file");
        free(w);
        return NULL;
    }
    
    w->buf = (char *)malloc(JSON_BUFFER_SIZE);
    if (w->buf == NULL) {
        fclose(w->fp);
        free(w);
        return NULL;
    }
    
    w->buf_size = JSON_BUFFER_SIZE;
    w->buf_pos = 0;
    
    /* Begin root object and metadata */
    json_write_string(w, "{\"metadata\":{");
    
    time(&now);
    tm_info = localtime(&now);
    if (tm_info != NULL) {
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm_info);
        json_write_string(w, "\"timestamp\":\"");
        json_write_escape(w, buf);
        json_write_string(w, "\",");
    }
    
    json_write_string(w, "\"version\":1},");
    
    return w;
}

int fluff_json_close(fluff_json_t *w)
{
    int rc = 0;
    
    if (w == NULL) return 0;
    
    /* Close any open sections */
    json_write_string(w, "\"summary\":{}");
    json_write_string(w, "}");
    
    fluff_json_flush(w);
    
    if (w->fp != NULL) {
        if (fclose(w->fp) != 0) rc = -1;
    }
    
    if (w->buf != NULL) free(w->buf);
    free(w);
    
    return rc;
}

int fluff_json_flush(fluff_json_t *w)
{
    if (w == NULL || !w->fp) return 0;
    if (fflush(w->fp) != 0) {
        json_error(w, "flush failed");
        return -1;
    }
    return 0;
}

/* ---- Metadata section ---- */

void fluff_json_begin_metadata(fluff_json_t *w)
{
    if (w == NULL) return;
    /* Metadata is already begun in fluff_json_open() */
}

void fluff_json_write_meta_string(fluff_json_t *w, const char *key,
                                   const char *value)
{
    if (w == NULL || w->has_error) return;
    
    json_write_string(w, "\"");
    json_write_escape(w, key);
    json_write_string(w, "\":\"");
    json_write_escape(w, value);
    json_write_string(w, "\",");
}

void fluff_json_write_meta_uint32(fluff_json_t *w, const char *key,
                                   bp_uint32 value)
{
    char buf[32];
    
    if (w == NULL || w->has_error) return;
    
    sprintf(buf, "\"");
    json_write_string(w, buf);
    json_write_escape(w, key);
    sprintf(buf, "\":%lu,", (unsigned long)value);
    json_write_string(w, buf);
}

void fluff_json_end_metadata(fluff_json_t *w)
{
    /* Handled in open/close */
    (void)w;
}

/* ---- Events array ---- */

void fluff_json_begin_events(fluff_json_t *w)
{
    if (w == NULL || w->has_error) return;
    json_write_string(w, "\"events\":[");
}

void fluff_json_write_event(fluff_json_t *w, const char *event_json)
{
    if (w == NULL || w->has_error || event_json == NULL) return;
    
    if (!w->first_event) {
        json_write_string(w, ",");
    }
    w->first_event = 0;
    
    json_write_string(w, event_json);
}

void fluff_json_end_events(fluff_json_t *w)
{
    if (w == NULL || w->has_error) return;
    json_write_string(w, "],");
}

/* ---- Summary section ---- */

void fluff_json_begin_summary(fluff_json_t *w)
{
    if (w == NULL || w->has_error) return;
    json_write_string(w, "\"summary\":{");
}

void fluff_json_write_summary_uint32(fluff_json_t *w, const char *key,
                                      bp_uint32 value)
{
    char buf[64];
    
    if (w == NULL || w->has_error) return;
    
    sprintf(buf, "\"%s\":%lu,", key, (unsigned long)value);
    json_write_string(w, buf);
}

void fluff_json_write_summary_int64(fluff_json_t *w, const char *key,
                                     long value)
{
    char buf[64];
    
    if (w == NULL || w->has_error) return;
    
    sprintf(buf, "\"%s\":%ld,", key, value);
    json_write_string(w, buf);
}

void fluff_json_write_summary_double(fluff_json_t *w, const char *key,
                                      double value)
{
    char buf[64];
    
    if (w == NULL || w->has_error) return;
    
    sprintf(buf, "\"%s\":%.6f,", key, value);
    json_write_string(w, buf);
}

void fluff_json_write_summary_bool(fluff_json_t *w, const char *key,
                                    int value)
{
    char buf[64];
    
    if (w == NULL || w->has_error) return;
    
    sprintf(buf, "\"%s\":%s,", key, value ? "true" : "false");
    json_write_string(w, buf);
}

void fluff_json_end_summary(fluff_json_t *w)
{
    if (w == NULL || w->has_error) return;
    /* Remove trailing comma and close summary object */
    json_write_string(w, "\"_dummy\":0}");
}

/* ---- Error checking ---- */

int fluff_json_has_error(fluff_json_t *w)
{
    if (w == NULL) return 1;
    return w->has_error;
}

const char *fluff_json_error_msg(fluff_json_t *w)
{
    if (w == NULL || !w->has_error) return "";
    return w->error_msg;
}
