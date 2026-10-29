/*
 * mini_common.c -- utilities shared by all BoltPack mini frontends
 *
 * ANSI C (C89/C90) only.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "mini_common.h"
#include "../../core/bp_codepage.h"
#include "../../core/mini/stack_probe.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int bp_mini_path_buffers_alloc(bp_mini_path_buffers_t *buffers, size_t size)
{
    if (size == 0 || size > 32767U) return BP_ERR_INVALID_ARG;
    return bp_mini_path_buffers_alloc_sized(buffers, (unsigned int)size,
                                            (unsigned int)size,
                                            (unsigned int)size);
}

int bp_mini_path_buffers_alloc_sized(bp_mini_path_buffers_t *buffers,
                                     unsigned int pattern_size,
                                     unsigned int child_fs_size,
                                     unsigned int child_arc_size)
{
    if (buffers == NULL || pattern_size == 0 || child_fs_size == 0 ||
        child_arc_size == 0 || pattern_size > 32767U ||
        child_fs_size > 32767U || child_arc_size > 32767U)
        return BP_ERR_INVALID_ARG;
    buffers->pattern = NULL;
    buffers->child_fs = NULL;
    buffers->child_arc = NULL;
    buffers->pattern = (char *)malloc((size_t)pattern_size);
    buffers->child_fs = (char *)malloc((size_t)child_fs_size);
    buffers->child_arc = (char *)malloc((size_t)child_arc_size);
    if (buffers->pattern == NULL || buffers->child_fs == NULL ||
        buffers->child_arc == NULL) {
        bp_mini_path_buffers_free(buffers);
        return BP_ERR_NOMEM;
    }
    return BP_OK;
}

void bp_mini_path_buffers_free(bp_mini_path_buffers_t *buffers)
{
    if (buffers == NULL) return;
    free(buffers->pattern);
    free(buffers->child_fs);
    free(buffers->child_arc);
    buffers->pattern = NULL;
    buffers->child_fs = NULL;
    buffers->child_arc = NULL;
}

int bp_mini_local_to_utf8(const char *src, char *dst, unsigned int dst_size)
{
    size_t i, n;
    BP_STACK_PROBE();
    if (src == NULL || dst == NULL || dst_size == 0 || dst_size > 32767U)
        return -1;
    if (BP_MINI_CODEPAGE != -1)
        return bp_local_to_utf8(src, dst, (int)dst_size, BP_MINI_CODEPAGE);
    n = strlen(src);
    if (n >= dst_size) return -1;
    for (i = 0; i < n; i++) if ((unsigned char)src[i] >= 0x80) return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

int bp_mini_utf8_to_local(const char *src, char *dst, unsigned int dst_size)
{
    size_t n;
    char *roundtrip;
    int rc;
    BP_STACK_PROBE();
    if (src == NULL || dst == NULL || dst_size == 0 || dst_size > 32767U)
        return -1;
    if (BP_MINI_CODEPAGE == -1)
        return bp_mini_local_to_utf8(src, dst, dst_size);
    n = strlen(src);
    if (n >= 32767U) return -1;
    if (bp_utf8_to_local(src, dst, (int)dst_size, BP_MINI_CODEPAGE) != 0)
        return -1;
    roundtrip = (char *)malloc(n + 1);
    if (roundtrip == NULL) return BP_ERR_NOMEM;
    rc = bp_local_to_utf8(dst, roundtrip, (int)n + 1, BP_MINI_CODEPAGE);
    if (rc == 0 && strcmp(src, roundtrip) != 0) rc = -1;
    free(roundtrip);
    return rc;
}

int bp_mini_projected_index_init(bp_mini_projected_index_t *index)
{
    size_t meta_size;
    unsigned char *meta;
    if (index == NULL) return BP_ERR_INVALID_ARG;
    memset(index, 0, sizeof(*index));
    meta_size = (size_t)BP_MINI_PROJECTED_INDEX_MAX *
                (sizeof(unsigned short) * 2U + sizeof(unsigned char));
    index->allocation = (char *)malloc(meta_size + BP_MINI_PROJECTED_INDEX_BYTES);
    if (index->allocation == NULL) return BP_ERR_NOMEM;
    meta = (unsigned char *)index->allocation;
    index->exact_offset = (unsigned short *)meta;
    meta += sizeof(unsigned short) * BP_MINI_PROJECTED_INDEX_MAX;
    index->folded_offset = (unsigned short *)meta;
    meta += sizeof(unsigned short) * BP_MINI_PROJECTED_INDEX_MAX;
    index->is_directory = meta;
    meta += sizeof(unsigned char) * BP_MINI_PROJECTED_INDEX_MAX;
    index->storage = (char *)meta;
    return BP_OK;
}

void bp_mini_projected_index_free(bp_mini_projected_index_t *index)
{
    if (index == NULL) return;
    free(index->allocation);
    memset(index, 0, sizeof(*index));
}

static int mini_projected_index_add_key(bp_mini_projected_index_t *index,
                                        const char *exact,
                                        const char *folded,
                                        int is_directory)
{
    size_t length = strlen(exact);
    unsigned i;
    unsigned needed;
    if (length != strlen(folded)) return BP_ERR_INVALID_ARG;
    for (i = 0; i < index->count; i++) {
        const char *old_exact = index->storage + index->exact_offset[i];
        const char *old_folded = index->storage + index->folded_offset[i];
        if (strcmp(old_folded, folded) == 0) {
            if (strcmp(old_exact, exact) != 0 ||
                index->is_directory[i] != (unsigned char)(is_directory != 0) ||
                !is_directory)
                return BP_ERR_PARTIAL;
            return BP_OK;
        }
    }
    if (index->count >= BP_MINI_PROJECTED_INDEX_MAX)
        return BP_ERR_PARTIAL;
    needed = (unsigned)(length + 1U) * 2U;
    if (needed > BP_MINI_PROJECTED_INDEX_BYTES - index->used)
        return BP_ERR_PARTIAL;
    index->exact_offset[index->count] = (unsigned short)index->used;
    memcpy(index->storage + index->used, exact, length + 1U);
    index->used += (unsigned)(length + 1U);
    index->folded_offset[index->count] = (unsigned short)index->used;
    memcpy(index->storage + index->used, folded, length + 1U);
    index->used += (unsigned)(length + 1U);
    index->is_directory[index->count] = (unsigned char)(is_directory != 0);
    index->count++;
    return BP_OK;
}

int bp_mini_projected_index_add(bp_mini_projected_index_t *index,
                                const char *local_path, int is_directory)
{
    char *scratch;
    char *exact;
    char *folded;
    size_t length, i;
    unsigned saved_count, saved_used;
    int rc = BP_OK;
    BP_STACK_PROBE();
    if (index == NULL || index->storage == NULL || local_path == NULL)
        return BP_ERR_INVALID_ARG;
    length = strlen(local_path);
    if (length == 0 || length >= 260U) return BP_ERR_NAME_TOO_LONG;
    scratch = (char *)malloc(520U);
    if (scratch == NULL) return BP_ERR_NOMEM;
    exact = scratch;
    folded = scratch + 260U;
    memcpy(exact, local_path, length + 1U);
    for (i = 0; i < length; i++) if (exact[i] == '/') exact[i] = '\\';
    while (length != 0 && exact[length - 1U] == '\\') length--;
    if (length == 0) {
        free(scratch);
        return BP_ERR_INVALID_ARG;
    }
    exact[length] = '\0';
    saved_count = index->count;
    saved_used = index->used;

    for (i = 0; i <= length; i++) {
        int final_component = i == length;
        if (!final_component && exact[i] != '\\') continue;
        if (i == 0 || exact[i - 1U] == '\\') {
            rc = BP_ERR_INVALID_ARG;
            break;
        }
        {
            char saved = exact[i];
            exact[i] = '\0';
            rc = bp_local_casefold(exact, folded, 260,
                                   BP_MINI_CODEPAGE);
            if (rc == 0)
                rc = mini_projected_index_add_key(index, exact, folded,
                                                   !final_component || is_directory);
            exact[i] = saved;
        }
        if (rc != BP_OK) break;
    }
    if (rc != BP_OK) {
        index->count = saved_count;
        index->used = saved_used;
        rc = rc == BP_CP_UNREPRESENTABLE ? BP_ERR_PARTIAL : rc;
    }
    free(scratch);
    return rc;
}

int bp_mini_projected_name_preflight(const bp_mini_entry_t *entry,
                                     void *user_data)
{
    bp_mini_projected_preflight_t *preflight =
        (bp_mini_projected_preflight_t *)user_data;
    char local_name[260];
    int rc;
    BP_STACK_PROBE();
    if (entry == NULL || preflight == NULL || preflight->index == NULL) {
        if (preflight != NULL) preflight->result = BP_ERR_INVALID_ARG;
        return 1;
    }
    if (entry->entry_type == BP_TYPE_META || entry->entry_type == BP_TYPE_RSRC)
        return 0;
    if (!bp_mini_name_is_safe(entry->name)) {
        fprintf(stderr, "bpmini: destination cannot represent archive name: %s\n",
                entry->name);
        preflight->result = BP_ERR_PARTIAL;
        return 1;
    }
    rc = bp_mini_utf8_to_local(entry->name, local_name, sizeof(local_name));
    if (rc != 0) {
        if (rc == BP_ERR_NOMEM) {
            fprintf(stderr, "bpmini: out of memory checking archive name: %s\n",
                    entry->name);
            preflight->result = rc;
        } else {
            fprintf(stderr,
                    "bpmini: destination cannot represent archive name: %s\n",
                    entry->name);
            preflight->result = BP_ERR_PARTIAL;
        }
        return 1;
    }
    rc = bp_mini_projected_index_add(preflight->index, local_name,
                                     entry->entry_type == BP_TYPE_DIR);
    if (rc != BP_OK) {
        fprintf(stderr,
                "bpmini: projected name collision or preflight limit: %s\n",
                entry->name);
        preflight->result = rc == BP_ERR_NOMEM ? rc : BP_ERR_PARTIAL;
        return 1;
    }
    return 0;
}

int bp_mini_add_local_entry(bp_mini_archive_t *ar, const char *name,
                            bp_uint8 entry_type, bp_uint8 platform,
                            bp_io_t *file_io, bp_uint64 file_size,
                            bp_uint64 mtime, bp_uint16 flags)
{
    unsigned long n;
    unsigned int utf8_capacity;
    char *utf8;
    int rc;
    BP_STACK_PROBE();
    if (name == NULL) return BP_ERR_INVALID_ARG;
    n = (unsigned long)strlen(name);
    if (n > (entry_type == BP_TYPE_DIR ? 10921UL : 10922UL))
        return BP_ERR_NAME_TOO_LONG;
    utf8_capacity = (unsigned int)(n * 3UL +
                                  (entry_type == BP_TYPE_DIR ? 2UL : 1UL));
    utf8 = (char *)malloc((size_t)utf8_capacity);
    if (utf8 == NULL) return BP_ERR_NOMEM;
    if (bp_mini_local_to_utf8(name, utf8, utf8_capacity) != 0)
        rc = BP_ERR_INVALID_ARG;
    else {
        if (entry_type == BP_TYPE_DIR) {
            size_t utf8_len = strlen(utf8);
            if (utf8_len == 0) {
                rc = BP_ERR_INVALID_ARG;
            } else {
                if (utf8[utf8_len - 1] != '/') {
                    utf8[utf8_len++] = '/';
                    utf8[utf8_len] = '\0';
                }
                rc = BP_OK;
            }
        } else {
            rc = BP_OK;
        }
    }
    if (rc == BP_OK)
        rc = bp_mini_archive_add_entry_ex(ar, utf8, entry_type, platform,
                                          file_io, file_size, mtime, flags);
    free(utf8);
    return rc;
}

typedef struct {
    bp_uint8 bytes[BP_META_FAT_SIZE];
    unsigned pos;
} bp_mini_meta_source_t;

static long bp_mini_meta_read(void *handle, void *buffer, bp_uint32 count)
{
    bp_mini_meta_source_t *source = (bp_mini_meta_source_t *)handle;
    unsigned remaining = BP_META_FAT_SIZE - source->pos;
    if (count > remaining) count = remaining;
    memcpy(buffer, source->bytes + source->pos, (size_t)count);
    source->pos += (unsigned)count;
    return (long)count;
}

int bp_mini_add_fat_metadata(bp_mini_archive_t *ar, const char *name,
                             bp_uint8 subject_type, bp_uint8 valid, bp_uint8 value)
{
    bp_mini_meta_source_t source;
    bp_io_t io;
    const char *subject_name = name;
    char *owned_subject_name = NULL;
    size_t name_len;
    int rc = bp_metadata_fat_encode(source.bytes, sizeof(source.bytes),
                                    subject_type, valid, value);
    if (rc != BP_OK) return rc;
    if (name == NULL) return BP_ERR_INVALID_ARG;
    name_len = strlen(name);
    if (subject_type == BP_TYPE_DIR && name_len == 0)
        return BP_ERR_INVALID_ARG;
    if (subject_type == BP_TYPE_DIR && name[name_len - 1] != '/') {
        if (name_len > 10921UL) return BP_ERR_NAME_TOO_LONG;
        owned_subject_name = (char *)malloc(name_len + 2);
        if (owned_subject_name == NULL) return BP_ERR_NOMEM;
        memcpy(owned_subject_name, name, name_len);
        owned_subject_name[name_len] = '/';
        owned_subject_name[name_len + 1] = '\0';
        subject_name = owned_subject_name;
    }
    source.pos = 0;
    io.read = bp_mini_meta_read; io.write = NULL; io.seek = NULL; io.tell = NULL;
    io.handle = &source;
    rc = bp_mini_add_local_entry(ar, subject_name, BP_TYPE_META,
                                 BP_META_PORTABLE, &io,
                                 bp_u64_from_u32(BP_META_FAT_SIZE),
                                 bp_u64_from_u32(0), 0);
    free(owned_subject_name);
    return rc;
}

/* A small bound limits heap use on constrained systems. Longer paths are
 * inserted first, so parents are finalized only after their descendants. */
int bp_mini_directory_add(bp_mini_directory_queue_t *queue, const char *path,
                          const bp_mini_entry_t *entry)
{
    bp_mini_directory_t *node, **at;
    size_t n;
    if (queue == NULL || path == NULL || entry == NULL) return BP_ERR_INVALID_ARG;
    if (queue->count >= 256U) return BP_ERR_OVERFLOW;
    n = strlen(path);
    node = (bp_mini_directory_t *)calloc(1, sizeof(*node));
    if (node == NULL) return BP_ERR_NOMEM;
    node->path = (char *)malloc(n + 1);
    if (node->path == NULL) { free(node); return BP_ERR_NOMEM; }
    memcpy(node->path, path, n + 1);
    node->mtime = entry->mtime;
    node->flags = entry->flags;
    at = &queue->head;
    while (*at != NULL && strlen((*at)->path) >= n) at = &(*at)->next;
    node->next = *at; *at = node; queue->count++;
    return BP_OK;
}

int bp_mini_directory_set_fat(bp_mini_directory_queue_t *queue, const char *path,
                              bp_uint8 valid, bp_uint8 value)
{
    bp_mini_directory_t *node;
    if (queue == NULL || path == NULL) return BP_ERR_INVALID_ARG;
    for (node = queue->head; node != NULL; node = node->next) {
        if (strcmp(node->path, path) == 0) {
            node->fat_valid = valid; node->fat_value = value;
            return BP_OK;
        }
    }
    return BP_ERR_CORRUPT;
}

int bp_mini_directory_finish(bp_mini_directory_queue_t *queue, int apply,
                             bp_mini_directory_apply_fn apply_fn, void *user_data)
{
    int result = BP_OK;
    BP_STACK_PROBE();
    if (queue == NULL || (apply && apply_fn == NULL)) return BP_ERR_INVALID_ARG;
    while (queue->head != NULL) {
        bp_mini_directory_t *node = queue->head;
        int rc = BP_OK;
        queue->head = node->next;
        if (apply) rc = apply_fn(node, user_data);
        if (rc != BP_OK && (result == BP_OK || result == BP_ERR_PARTIAL)) result = rc;
        free(node->path); free(node);
    }
    queue->count = 0;
    return result;
}

int bp_mini_name_is_safe(const char *name)
{
    const char *p;
    if (name == NULL || name[0] == '\0' || name[0] == '/' ||
        name[1] == ':' || strchr(name, '\\') != NULL) return 0;
    for (p = name; *p; p++) {
        if (p[0] == '.' && p[1] == '.' &&
            (p == name || p[-1] == '/') &&
            (p[2] == '/' || p[2] == '\0')) return 0;
    }
    return 1;
}

/* ---- bp_mini_ultoa ----------------------------------------------------- */

/*
 * Convert unsigned long n to decimal in buf[11].
 * Returns a pointer to the first digit (somewhere in buf).
 * buf must be at least 11 bytes.
 */
char *bp_mini_ultoa(unsigned long n, char *buf)
{
    char *p;
    buf[10] = '\0';
    p = buf + 10;
    do { *--p = (char)('0' + (int)(n % 10UL)); n /= 10UL; } while (n);
    return p;
}

/* ---- bp_io_from_file --------------------------------------------------- */

static long file_read(void *handle, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)handle);
}

static long file_write(void *handle, const void *buf, bp_uint32 count)
{
    return (long)fwrite(buf, 1, (size_t)count, (FILE *)handle);
}

static int file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t file_tell(void *handle)
{
    return (bp_off_t)ftell((FILE *)handle);
}

void bp_io_from_file(bp_io_t *io, FILE *fp)
{
    io->read   = file_read;
    io->write  = file_write;
    io->seek   = file_seek;
    io->tell   = file_tell;
    io->handle = fp;
}

/* ---- print_utf8_safe --------------------------------------------------- */

void print_utf8_safe(const char *name, unsigned int len)
{
#ifdef __WATCOMC__
    char buf[256];
    unsigned int i;
    unsigned int n;
    n = (len < 255) ? len : 255;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        buf[i] = (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
    }
    write(1, buf, n);
#else
    unsigned int i;
    unsigned char c;
    for (i = 0; i < len; i++) {
        c = (unsigned char)name[i];
        putchar((c >= 0x20 && c <= 0x7E) ? (int)c : (int)'?');
    }
#endif
}

/* ---- bp_mini_strerror -------------------------------------------------- */

const char *bp_mini_strerror(int code)
{
    switch (code) {
    case BP_OK:                      return "success";
    case BP_ERR_IO:                  return "I/O error";
    case BP_ERR_NOMEM:               return "out of memory";
    case BP_ERR_MAGIC:               return "not a valid BoltPack archive";
    case BP_ERR_VERSION:             return "unsupported archive version";
    case BP_ERR_CHECKSUM:            return "checksum mismatch";
    case BP_ERR_CORRUPT:             return "archive is corrupt";
    case BP_ERR_TRUNCATED:           return "archive is truncated";
    case BP_ERR_OVERFLOW:            return "block size too large; re-create archive with -b 32k";
    case BP_ERR_NAME_TOO_LONG:       return "entry name too long";
    case BP_ERR_UNSUPPORTED_METHOD:  return "compression method not supported by mini";
    case BP_ERR_COMPRESS:            return "compression failed";
    case BP_ERR_DECOMPRESS:          return "decompression failed";
    case BP_ERR_ABORTED:             return "operation aborted";
    case BP_ERR_NOT_FOUND:           return "entry not found";
    case BP_ERR_INVALID_ARG:         return "invalid argument";
    case BP_ERR_PARTIAL:             return "some entries or attributes were not restored";
    default:                          return "unknown error";
    }
}

/* ---- format_size64_mini ------------------------------------------------ */

void format_size64_mini(bp_uint64 v, char *buf)
{
    if (v.hi == 0) {
        unsigned long n = (unsigned long)v.lo;
        char nbuf[11];
        char *p;
        const char *suffix;

        if (n < 1024UL) {
            p = bp_mini_ultoa(n, nbuf);
            suffix = " B";
        } else if (n < 1024UL * 1024UL) {
            p = bp_mini_ultoa(n / 1024UL, nbuf);
            suffix = " KB";
        } else if (n < 1024UL * 1024UL * 1024UL) {
            p = bp_mini_ultoa(n / (1024UL * 1024UL), nbuf);
            suffix = " MB";
        } else {
            p = bp_mini_ultoa(n / (1024UL * 1024UL * 1024UL), nbuf);
            suffix = " GB";
        }
        strcpy(buf, p);
        strcat(buf, suffix);
    } else {
        /* Value > 4 GB: print exact byte count using long division. */
        char tmp[21];
        int pos = 20;
        bp_uint32 nhi = v.hi;
        bp_uint32 nlo = v.lo;
        bp_uint32 d[4];
        bp_uint32 q[4];
        bp_uint32 rem;
        bp_uint32 combined;
        int i;

        tmp[pos] = '\0';
        do {
            d[0] = (nhi >> 16) & 0xFFFFU;
            d[1] = nhi & 0xFFFFU;
            d[2] = (nlo >> 16) & 0xFFFFU;
            d[3] = nlo & 0xFFFFU;
            rem = 0;
            for (i = 0; i < 4; i++) {
                combined = rem * 65536UL + d[i];
                q[i]     = combined / 10UL;
                rem      = combined % 10UL;
            }
            tmp[--pos] = (char)('0' + (int)rem);
            nhi = (q[0] << 16) | q[1];
            nlo = (q[2] << 16) | q[3];
        } while (nhi != 0 || nlo != 0);
        strcpy(buf, tmp + pos);
        strcat(buf, " B");
    }
}

/* ---- parse_block_size_mini --------------------------------------------- */

int parse_block_size_mini(const char *s, bp_uint32 *out)
{
    if (s == NULL || out == NULL) return 0;

    if (strcmp(s, "8k") == 0 || strcmp(s, "8K") == 0) {
        *out = 8192U;
        return 1;
    }
    if (strcmp(s, "16k") == 0 || strcmp(s, "16K") == 0) {
        *out = 16384U;
        return 1;
    }
    if (strcmp(s, "32k") == 0 || strcmp(s, "32K") == 0) {
        *out = BP_MINI_MAX_BLOCK_SIZE;
        return 1;
    }
    return 0;
}

/* ---- Watcom-specific helpers (DOS/Win16/OS2-16) ------------------------ */

#ifdef __WATCOMC__

/* handle I/O callbacks using low-level read/write/lseek */
static long handle_read(void *h, void *buf, bp_uint32 n)
{
    return (long)read((int)(long)h, buf, (unsigned)n);
}

static long handle_write(void *h, const void *buf, bp_uint32 n)
{
    return (long)write((int)(long)h, buf, (unsigned)n);
}

static int handle_seek(void *h, bp_off_t off, int org)
{
    return (lseek((int)(long)h, (long)off, org) == -1L) ? -1 : 0;
}

static bp_off_t handle_tell(void *h)
{
    return (bp_off_t)lseek((int)(long)h, 0L, SEEK_CUR);
}

void bp_io_from_handle(bp_io_t *io, int fd)
{
    io->read   = handle_read;
    io->write  = handle_write;
    io->seek   = handle_seek;
    io->tell   = handle_tell;
    io->handle = (void *)(long)fd;
}

void bp_wstr(int fd, const char *s)
{
    const char *p = s;
    while (*p) p++;
    if (p > s) write(fd, s, (unsigned)(p - s));
}

void bp_wulong(int fd, unsigned long n)
{
    char buf[11];
    char *p = bp_mini_ultoa(n, buf);
    write(fd, p, (unsigned)(buf + 10 - p));
}

void bp_wuint3(int fd, unsigned int n)
{
    char buf[3];
    if (n > 999) n = 999;
    buf[2] = (char)('0' + (n % 10)); n /= 10;
    buf[1] = (n > 0) ? (char)('0' + (n % 10)) : ' '; n /= 10;
    buf[0] = (n > 0) ? (char)('0' + n) : ' ';
    write(fd, buf, 3);
}

/*
 * Month-day table (shared by both time conversion functions).
 * Index 0 = January.  February (index 1) is 28 days; leap year adds 1 when
 * needed in the conversion logic.
 */
static unsigned char s_mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

static int is_leap(unsigned y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

unsigned long bp_dos_to_epoch(unsigned dos_date, unsigned dos_time)
{
    unsigned year  = 1980 + ((dos_date >> 9) & 0x7F);
    unsigned month = (dos_date >> 5) & 0x0F;
    unsigned day   = dos_date & 0x1F;
    unsigned hour  = (dos_time >> 11) & 0x1F;
    unsigned min   = (dos_time >> 5) & 0x3F;
    unsigned sec   = (dos_time & 0x1F) * 2;
    unsigned long  days;
    unsigned y;
    unsigned m;

    if (month < 1 || month > 12 || day < 1) return 0;

    /* Days from 1970-01-01 to the start of `year` */
    days = 0;
    for (y = 1970; y < year; y++) {
        days += is_leap(y) ? 366UL : 365UL;
    }
    /* Days within the year up to (but not including) this month */
    for (m = 1; m < month; m++) {
        days += s_mdays[m - 1];
        if (m == 2 && is_leap(year)) days++;
    }
    days += (unsigned long)(day - 1);

    return days * 86400UL
           + (unsigned long)hour * 3600UL
           + (unsigned long)min  * 60UL
           + (unsigned long)sec;
}

void bp_epoch_to_dos(unsigned long epoch, unsigned *dos_date, unsigned *dos_time)
{
    unsigned long days = epoch / 86400UL;
    unsigned long rem  = epoch % 86400UL;
    unsigned hour = (unsigned)(rem / 3600UL);
    unsigned min  = (unsigned)((rem % 3600UL) / 60UL);
    unsigned sec  = (unsigned)(rem % 60UL);
    unsigned year = 1970;
    unsigned month;
    unsigned day;
    unsigned long dy;
    unsigned dm;

    /* Find year */
    for (;;) {
        dy = is_leap(year) ? 366UL : 365UL;
        if (days < dy) break;
        days -= dy;
        year++;
    }
    /* Find month */
    month = 1;
    for (;;) {
        dm = s_mdays[month - 1];
        if (month == 2 && is_leap(year)) dm++;
        if (days < (unsigned long)dm) break;
        days -= (unsigned long)dm;
        month++;
    }
    day = (unsigned)days + 1;

    /* Clamp to DOS range (1980-01-01 .. 2107-12-31) */
    if (year < 1980) { year = 1980; month = 1; day = 1; }
    if (year > 2107) { year = 2107; month = 12; day = 31; }

    *dos_date = ((year - 1980) << 9) | (month << 5) | day;
    *dos_time = (hour << 11) | (min << 5) | (sec / 2);
}

#endif /* __WATCOMC__ */
