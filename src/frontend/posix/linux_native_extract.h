/* Shared Linux CLI/GUI native metadata restoration.
 * BP_LXA_CONTEXT supplies the frontend context with root_fd, subject identity
 * and the table of files published by this extraction.
 */
#ifndef BP_LINUX_NATIVE_EXTRACT_H
#define BP_LINUX_NATIVE_EXTRACT_H

/* ---- native attributes and hard links: extract side -------------------- */

typedef struct lxa_extracted_s {
    dev_t dev;
    ino_t ino;
    bp_uint32 size;       /* low 32 bits of the size; enough with the checksum */
    bp_uint32 checksum;
    int used;
} lxa_extracted_t;

static void lxa_extracted_free(BP_LXA_CONTEXT *ctx)
{
    free(ctx->extracted);
    ctx->extracted = NULL;
    ctx->extracted_count = ctx->extracted_cap = 0;
}

static unsigned long lxa_slot(dev_t dev, ino_t ino, unsigned long cap)
{
    return (((unsigned long)ino * 2654435761UL) ^ (unsigned long)dev) & (cap - 1UL);
}

static int lxa_extracted_add(BP_LXA_CONTEXT *ctx, const struct stat *st,
                             const bp_entry_t *entry)
{
    unsigned long i;
    if (ctx->extracted_count * 2UL >= ctx->extracted_cap) {
        unsigned long newcap = ctx->extracted_cap ? ctx->extracted_cap * 2UL : 256UL;
        lxa_extracted_t *table = (lxa_extracted_t *)calloc(newcap, sizeof(*table));
        if (table == NULL) return -1;
        for (i = 0; i < ctx->extracted_cap; i++) {
            lxa_extracted_t *old = &ctx->extracted[i];
            unsigned long j;
            if (!old->used) continue;
            j = lxa_slot(old->dev, old->ino, newcap);
            while (table[j].used) j = (j + 1UL) & (newcap - 1UL);
            table[j] = *old;
        }
        free(ctx->extracted);
        ctx->extracted = table;
        ctx->extracted_cap = newcap;
    }
    i = lxa_slot(st->st_dev, st->st_ino, ctx->extracted_cap);
    while (ctx->extracted[i].used) {
        if (ctx->extracted[i].dev == st->st_dev && ctx->extracted[i].ino == st->st_ino)
            return 0;
        i = (i + 1UL) & (ctx->extracted_cap - 1UL);
    }
    ctx->extracted[i].used = 1;
    ctx->extracted[i].dev = st->st_dev;
    ctx->extracted[i].ino = st->st_ino;
    ctx->extracted[i].size = entry->original_size.lo;
    ctx->extracted[i].checksum = entry->checksum;
    ctx->extracted_count++;
    return 0;
}

static const lxa_extracted_t *lxa_extracted_find(const BP_LXA_CONTEXT *ctx,
                                                 const struct stat *st)
{
    unsigned long i;
    if (ctx->extracted_cap == 0) return NULL;
    i = lxa_slot(st->st_dev, st->st_ino, ctx->extracted_cap);
    while (ctx->extracted[i].used) {
        if (ctx->extracted[i].dev == st->st_dev && ctx->extracted[i].ino == st->st_ino)
            return &ctx->extracted[i];
        i = (i + 1UL) & (ctx->extracted_cap - 1UL);
    }
    return NULL;
}

/* Record the file or directory just extracted as the subject of any attribute
 * entry that follows. */
static void lxa_set_subject(BP_LXA_CONTEXT *ctx, const char *name, int type,
                            const struct stat *st)
{
    if (strlen(name) >= sizeof(ctx->subject_name)) { ctx->subject_ok = 0; return; }
    strcpy(ctx->subject_name, name);
    ctx->subject_type = type;
    ctx->subject_dev = st->st_dev;
    ctx->subject_ino = st->st_ino;
    ctx->subject_ok = 1;
}

/* An attribute container being received. */
typedef struct {
    bp_io_t io; /* first member */
    unsigned char *buf;
    bp_uint32 cap, len, pos;
} lxa_sink_t;

static long lxa_sink_write(void *handle, const void *data, bp_uint32 count)
{
    lxa_sink_t *k = (lxa_sink_t *)handle;
    if (count > k->cap - k->pos) return -1;
    memcpy(k->buf + k->pos, data, count);
    k->pos += count;
    if (k->pos > k->len) k->len = k->pos;
    return (long)count;
}

static long lxa_sink_read(void *handle, void *data, bp_uint32 count)
{
    (void)handle; (void)data; (void)count;
    return -1;
}

static int lxa_sink_seek(void *handle, bp_off_t offset, int origin)
{
    lxa_sink_t *k = (lxa_sink_t *)handle;
    long position;
    if (origin == SEEK_SET) position = (long)offset;
    else if (origin == SEEK_CUR) position = (long)k->pos + (long)offset;
    else if (origin == SEEK_END) position = (long)k->len + (long)offset;
    else return -1;
    if (position < 0 || (unsigned long)position > k->cap) return -1;
    k->pos = (bp_uint32)position;
    return 0;
}

static bp_off_t lxa_sink_tell(void *handle)
{
    return (bp_off_t)((lxa_sink_t *)handle)->pos;
}

static void lxa_sink_free(lxa_sink_t *k)
{
    if (k == NULL) return;
    free(k->buf);
    free(k);
}

/* Replace the extracted copy of name by a hard link to target, which must be a
 * regular file this run extracted with the same size and checksum.  On any
 * refusal the extracted copy stays as it is: the data is complete either way. */
static int lxa_make_hardlink(BP_LXA_CONTEXT *ctx, const char *name,
                             const char *target, const struct stat *self,
                             const bp_entry_t *entry)
{
    char leaf[4096], tleaf[4096], temporary[64];
    int pfd = -1, tfd = -1, ok = 0, tries;
    struct stat tst;
    const lxa_extracted_t *known;
#ifdef __cplusplus
    static thread_local unsigned long counter;
#else
    static unsigned long counter;
#endif

#ifdef BP_LXA_TARGET_NAME
    target = BP_LXA_TARGET_NAME(ctx, target);
    if (target == NULL) {
        fprintf(stderr, "boltpack: hard link %s not recreated: target was not "
                "extracted; it stays a separate copy\n", entry->name);
        return -1;
    }
#endif
    while (*target == '/') target++;
    if (!bp_name_is_safe(target) || strcmp(target, name) == 0) {
        fprintf(stderr,
                "boltpack: hard link %s not recreated: the target name %s is "
                "not usable; it stays a separate copy\n", entry->name, target);
        return -1;
    }
    pfd = extract_open_parent(ctx->root_fd, name, leaf, sizeof(leaf));
    tfd = extract_open_parent(ctx->root_fd, target, tleaf, sizeof(tleaf));
    if (pfd < 0 || tfd < 0 ||
        fstatat(tfd, tleaf, &tst, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(tst.st_mode)) {
        fprintf(stderr,
                "boltpack: hard link %s not recreated: %s was not extracted; it "
                "stays a separate copy\n", entry->name, target);
        goto done;
    }
    if (tst.st_dev == self->st_dev && tst.st_ino == self->st_ino) { ok = 1; goto done; }
    known = lxa_extracted_find(ctx, &tst);
    if (known == NULL || known->size != ctx->subject_size ||
        known->checksum != ctx->subject_checksum ||
        (bp_uint32)tst.st_size != ctx->subject_size) {
        fprintf(stderr,
                "boltpack: hard link %s not recreated: %s is not an identical "
                "file extracted by this run; it stays a separate copy\n",
                entry->name, target);
        goto done;
    }
    for (tries = 0; tries < 256; tries++) {
        sprintf(temporary, ".boltpack-hl-%08lx-%08lx", (unsigned long)getpid(),
                ++counter);
        if (linkat(tfd, tleaf, pfd, temporary, 0) == 0) break;
        if (errno != EEXIST) {
            fprintf(stderr,
                    "boltpack: hard link %s not recreated: %s (it stays a "
                    "separate copy)\n", entry->name, strerror(errno));
            goto done;
        }
    }
    if (tries == 256) goto done;
    if (renameat(pfd, temporary, pfd, leaf) != 0) {
        fprintf(stderr,
                "boltpack: hard link %s not recreated: %s (it stays a separate "
                "copy)\n", entry->name, strerror(errno));
        unlinkat(pfd, temporary, 0);
        goto done;
    }
    ok = 1;
done:
    if (pfd >= 0) close(pfd);
    if (tfd >= 0) close(tfd);
    return ok ? 0 : -1;
}

/* Apply a received container to the subject extracted just before it.  Every
 * refusal and failure is reported with the item and the reason.  Returns
 * BP_ERR_PARTIAL when anything could not be applied. */
static int lxa_apply_entry(BP_LXA_CONTEXT *ctx, const bp_entry_t *entry,
                           const char *name, const unsigned char *data,
                           unsigned long size)
{
    bp_nattr_info_t info;
    bp_nattr_iter_t it;
    struct stat st;
    char leaf[4096];
    int parse_rc, fd = -1, pfd = -1, bad = 0;
    const char *aname;
    const unsigned char *avalue;
    unsigned long alen, vlen;

    if (!ctx->subject_ok || strcmp(name, ctx->subject_name) != 0) {
        fprintf(stderr,
                "boltpack: attributes for %s not applied: the item itself was "
                "not extracted\n", entry->name);
        return BP_ERR_PARTIAL;
    }
    parse_rc = bp_nattr_parse(data, size, &info);
    if (parse_rc == BP_NATTR_UNSUPPORTED) {
        fprintf(stderr,
                "boltpack: attributes for %s not applied: container version %u is "
                "newer than this boltpack understands\n", entry->name,
                (unsigned)info.version);
        return BP_ERR_PARTIAL;
    }
    if (parse_rc != BP_NATTR_OK) {
        fprintf(stderr,
                "boltpack: attributes for %s not applied: malformed container "
                "(%lu bytes)\n", entry->name, size);
        return BP_ERR_PARTIAL;
    }
    if (info.origin != BP_OS_LINUX) {
        fprintf(stderr,
                "boltpack: attributes for %s not applied: they come from platform "
                "0x%02X, not Linux\n", entry->name, (unsigned)info.origin);
        return BP_ERR_PARTIAL;
    }
    if ((info.subject_type == 0 && ctx->subject_type != BP_TYPE_DATA) ||
        (info.subject_type == 4 && ctx->subject_type != BP_TYPE_DIR)) {
        fprintf(stderr,
                "boltpack: attributes for %s not applied: container is for a %s "
                "but the item is a %s\n", entry->name,
                info.subject_type == 0 ? "file" : "directory",
                ctx->subject_type == BP_TYPE_DATA ? "file" : "directory");
        return BP_ERR_PARTIAL;
    }

    /* Reach the item without following links and confirm it is the one extracted. */
    if (ctx->subject_type == BP_TYPE_DATA) {
        pfd = extract_open_parent(ctx->root_fd, name, leaf, sizeof(leaf));
        if (pfd >= 0)
            fd = openat(pfd, leaf, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    } else {
        fd = extract_open_directory(ctx->root_fd, name, 0);
    }
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_dev != ctx->subject_dev ||
        st.st_ino != ctx->subject_ino ||
        (ctx->subject_type == BP_TYPE_DATA ? !S_ISREG(st.st_mode)
                                           : !S_ISDIR(st.st_mode))) {
        fprintf(stderr,
                "boltpack: attributes for %s not applied: the extracted item "
                "could not be reopened or changed identity\n", entry->name);
        if (fd >= 0) close(fd);
        if (pfd >= 0) close(pfd);
        g_lxa.failed += info.count;
        return BP_ERR_PARTIAL;
    }

    if (bp_lxa_apply(fd, &info, entry->name, g_security_attrs, &g_lxa,
                     g_nattr_debug) > 0)
        bad = 1;
    g_lxa.subjects++;

    /* A hard-link record: replace the copy just written by a link. */
    if (ctx->subject_type == BP_TYPE_DATA) {
        bp_nattr_iter_begin(&info, &it);
        while (bp_nattr_iter_next(&it, &aname, &alen, &avalue, &vlen)) {
            if (alen == strlen(BP_LXA_HARDLINK_ITEM) &&
                memcmp(aname, BP_LXA_HARDLINK_ITEM, (size_t)alen) == 0) {
                char target[4096];
                if (vlen == 0 || vlen >= sizeof(target)) { bad = 1; break; }
                memcpy(target, avalue, (size_t)vlen);
                target[vlen] = '\0';
                if (lxa_make_hardlink(ctx, name, target, &st, entry) == 0)
                    g_lxa.hardlinks++;
                else
                    bad = 1;
                break;
            }
        }
    }
    close(fd);
    if (pfd >= 0) close(pfd);
    return bad ? BP_ERR_PARTIAL : BP_OK;
}

#endif
