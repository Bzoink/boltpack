/* Shared Linux CLI/GUI native metadata capture.
 * Include after the add_job_t/list definitions and native-attribute state.
 */
#ifndef BP_LINUX_NATIVE_CREATE_H
#define BP_LINUX_NATIVE_CREATE_H

/* A read-only in-memory stream over a byte buffer, for metadata entries. */
typedef struct {
    const unsigned char *buf;
    bp_uint32 size;
    bp_uint32 pos;
} lxa_mem_t;

static long lxa_mem_read(void *handle, void *buffer, bp_uint32 count)
{
    lxa_mem_t *m = (lxa_mem_t *)handle;
    bp_uint32 left = m->size - m->pos;
    if (count > left) count = left;
    if (count == 0) return 0;
    memcpy(buffer, m->buf + m->pos, count);
    m->pos += count;
    return (long)count;
}

static long lxa_mem_write(void *handle, const void *buffer, bp_uint32 count)
{
    (void)handle; (void)buffer; (void)count;
    return -1L;
}

static int lxa_mem_seek(void *handle, bp_off_t offset, int origin)
{
    lxa_mem_t *m = (lxa_mem_t *)handle;
    long position;
    if (origin == SEEK_SET) position = (long)offset;
    else if (origin == SEEK_CUR) position = (long)m->pos + (long)offset;
    else if (origin == SEEK_END) position = (long)m->size + (long)offset;
    else return -1;
    if (position < 0 || position > (long)m->size) return -1;
    m->pos = (bp_uint32)position;
    return 0;
}

static bp_off_t lxa_mem_tell(void *handle)
{
    return (bp_off_t)((lxa_mem_t *)handle)->pos;
}
/* ---- native attributes and hard links: create side -------------------- */

/* Link every regular file whose inode was already seen to the first name that
 * used it.  The data is still archived for every name, so a reader that does
 * not understand the record simply gets an independent copy. */
static void mark_hard_links(add_job_list_t *list)
{
    int i, j, slots;
    int *table;
    if (list->count < 2) return;
    slots = 1;
    while (slots < list->count * 2) slots <<= 1;
    table = (int *)malloc((size_t)slots * sizeof(int));
    if (table == NULL) return;
    for (i = 0; i < slots; i++) table[i] = -1;
    for (i = 0; i < list->count; i++) {
        add_job_t *job = &list->items[i];
        unsigned long h;
        job->hardlink_to = NULL;
        if (job->entry_type != BP_TYPE_DATA || job->source_nlink < 2UL) continue;
        h = ((unsigned long)job->source_ino * 2654435761UL) ^
            (unsigned long)job->source_dev;
        j = (int)(h & (unsigned long)(slots - 1));
        for (;;) {
            if (table[j] < 0) { table[j] = i; break; }
            if (list->items[table[j]].source_ino == job->source_ino &&
                list->items[table[j]].source_dev == job->source_dev) {
                job->hardlink_to = list->items[table[j]].arcname;
                break;
            }
            j = (j + 1) & (slots - 1);
        }
    }
    free(table);
}

/* Write one BPNA entry after a file or directory: its hard-link record (if it
 * is another name for an earlier file) and its extended attributes.  Nothing is
 * written when there is nothing to say, and never for target profiles or
 * Interchange Profile 1, which describe other readers. */
static int add_native_attrs_metadata(bp_archive_t *ar, const add_job_t *job)
{
    unsigned char *payload;
    bp_nattr_builder_t builder;
    int rc = BP_OK;

    if (!g_nattr_enabled || ar->profile != BP_PROFILE_NONE) return BP_OK;
    if (job->entry_type != BP_TYPE_DATA && job->entry_type != BP_TYPE_DIR)
        return BP_OK;
    payload = (unsigned char *)malloc(BP_NATTR_MAX_PAYLOAD);
    if (payload == NULL) return BP_ERR_NOMEM;
    bp_nattr_begin(&builder, payload, BP_OS_LINUX,
                   job->entry_type == BP_TYPE_DIR ? 4 : 0);
    if (job->hardlink_to != NULL) {
        int add_rc = bp_nattr_add(&builder, BP_LXA_HARDLINK_ITEM,
                                  (unsigned long)strlen(BP_LXA_HARDLINK_ITEM),
                                  job->hardlink_to,
                                  (unsigned long)strlen(job->hardlink_to));
        if (add_rc == BP_NATTR_OK) {
            g_lxa.hardlinks++;
        } else {
            fprintf(stderr,
                    "boltpack: warning: hard link record for %s not stored: %s\n",
                    job->arcname, bp_nattr_strerror(add_rc));
            g_lxa.skipped++;
        }
    }
    bp_lxa_collect(job->path, &builder, g_security_attrs, &g_lxa, g_nattr_debug);
    if (builder.count > 0) {
        unsigned long length = bp_nattr_finish(&builder);
        lxa_mem_t mem;
        bp_io_t io;
        mem.buf = payload;
        mem.size = (bp_uint32)length;
        mem.pos = 0;
        io.read = lxa_mem_read;
        io.write = lxa_mem_write;
        io.seek = lxa_mem_seek;
        io.tell = lxa_mem_tell;
        io.handle = &mem;
        rc = bp_archive_add_entry_ex(ar, job->arcname, BP_TYPE_META,
                                     BP_META_NATIVE_ATTRS, &io,
                                     bp_u64_from_u32((bp_uint32)length),
                                     bp_u64_from_u32(0), 0);
        if (rc != BP_OK)
            fprintf(stderr, "boltpack: error storing attributes of %s: %s\n",
                    job->path, bp_strerror(rc));
        else
            g_lxa.subjects++;
    }
    free(payload);
    return rc;
}

#endif
