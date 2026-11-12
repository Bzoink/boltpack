/*
 * linux_xattr.h -- Linux extended attributes in the native attribute container
 *
 * Stores a file's or directory's extended attributes in a BPNA container
 * (src/frontend/common/native_attrs.h, origin platform BP_OS_LINUX) and applies
 * them again on extraction.  Linux only; on other systems the functions report
 * "not supported" and store or apply nothing.
 *
 * Which attributes travel
 *   user.*                      stored and restored by default
 *   system.posix_acl_access     stored and restored by default (POSIX ACLs)
 *   system.posix_acl_default
 *   security.*, trusted.*       stored and restored only when the caller asks
 *                               (capabilities, SELinux labels, IMA values): a
 *                               label means nothing under another policy and
 *                               file capabilities hand out privilege, so an
 *                               archive must not apply them unasked
 *   anything else               not stored
 *
 * The reserved item "boltpack.hardlink" is not an extended attribute (a real
 * name always starts with a namespace and a dot): its value is the archive name
 * of the earlier entry this file is a hard link to.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_LINUX_XATTR_H
#define BP_LINUX_XATTR_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../common/native_attrs.h"

#ifdef __linux__
#include <sys/xattr.h>
#define BP_LXA_AVAILABLE 1
#endif

#define BP_LXA_HARDLINK_ITEM "boltpack.hardlink"

/* Counters for the end-of-run summary. */
typedef struct {
    unsigned long subjects;    /* containers written or applied */
    unsigned long stored;      /* attributes written */
    unsigned long skipped;     /* attributes not stored, each reported */
    unsigned long restored;    /* attributes applied */
    unsigned long failed;      /* attributes that could not be applied */
    unsigned long refused;     /* attributes left alone by policy */
    unsigned long hardlinks;   /* hard links recorded or recreated */
} bp_lxa_stats_t;

/* 0 = not carried, 1 = ordinary (user.* and ACLs), 2 = security (needs opt-in) */
static int bp_lxa_class(const char *name)
{
    if (strncmp(name, "user.", 5) == 0) return 1;
    if (strcmp(name, "system.posix_acl_access") == 0 ||
        strcmp(name, "system.posix_acl_default") == 0) return 1;
    if (strncmp(name, "security.", 9) == 0 ||
        strncmp(name, "trusted.", 8) == 0) return 2;
    return 0;
}

#ifdef BP_LXA_AVAILABLE

/* Append the attributes of path to the builder.  Problems with one attribute are
 * reported on stderr and counted; they never stop the archive. */
static void bp_lxa_collect(const char *path, bp_nattr_builder_t *b,
                           int include_security, bp_lxa_stats_t *stats,
                           int debug)
{
    ssize_t size, got, vsize, vgot;
    char *names, *cursor, *end;
    unsigned char *value;
    int tries;

    size = -1;
    names = NULL;
    for (tries = 0; tries < 3; tries++) {
        size = llistxattr(path, NULL, 0);
        if (size <= 0) break;
        names = (char *)malloc((size_t)size + 1);
        if (names == NULL) { stats->skipped++; return; }
        got = llistxattr(path, names, (size_t)size);
        if (got >= 0) { size = got; break; }
        free(names); names = NULL;
        if (errno != ERANGE) { size = -1; break; }
        size = -1;
    }
    if (size < 0) {
        if (errno != ENOTSUP && errno != ENODATA && errno != EPERM) {
            fprintf(stderr,
                    "boltpack: warning: cannot list extended attributes of %s: %s\n",
                    path, strerror(errno));
            stats->skipped++;
        }
        free(names);
        return;
    }
    if (size == 0) { free(names); return; }
    names[size] = '\0';
    value = (unsigned char *)malloc(BP_NATTR_MAX_VALUE + 1UL);
    if (value == NULL) { free(names); stats->skipped++; return; }

    end = names + size;
    for (cursor = names; cursor < end; cursor += strlen(cursor) + 1) {
        int cls = bp_lxa_class(cursor);
        int rc;
        if (cls == 0 || (cls == 2 && !include_security)) {
            if (debug)
                fprintf(stderr, "boltpack[debug]: %s: '%s' not stored (%s)\n",
                        path, cursor,
                        cls == 2 ? "security attribute; use --security-attrs"
                                 : "not carried");
            continue;
        }
        vsize = lgetxattr(path, cursor, NULL, 0);
        if (vsize < 0) {
            fprintf(stderr,
                    "boltpack: warning: cannot read attribute '%s' of %s: %s\n",
                    cursor, path, strerror(errno));
            stats->skipped++;
            continue;
        }
        if ((unsigned long)vsize > BP_NATTR_MAX_VALUE) {
            fprintf(stderr,
                    "boltpack: warning: attribute '%s' of %s is %lu bytes; values "
                    "over %lu bytes are not stored\n", cursor, path,
                    (unsigned long)vsize, BP_NATTR_MAX_VALUE);
            stats->skipped++;
            continue;
        }
        vgot = vsize == 0 ? 0 : lgetxattr(path, cursor, value, (size_t)vsize);
        if (vgot != vsize) {
            fprintf(stderr,
                    "boltpack: warning: attribute '%s' of %s changed while "
                    "reading: %s\n", cursor, path,
                    vgot < 0 ? strerror(errno) : "size mismatch");
            stats->skipped++;
            continue;
        }
        rc = bp_nattr_add(b, cursor, (unsigned long)strlen(cursor), value,
                          (unsigned long)vsize);
        if (rc != BP_NATTR_OK) {
            fprintf(stderr,
                    "boltpack: warning: attribute '%s' of %s not stored: %s\n",
                    cursor, path, bp_nattr_strerror(rc));
            stats->skipped++;
            continue;
        }
        stats->stored++;
        if (debug)
            fprintf(stderr, "boltpack[debug]: %s: stored '%s' (%ld bytes)\n",
                    path, cursor, (long)vsize);
    }
    free(names);
    free(value);
}

/* Apply the attributes of a validated container to the open file or directory.
 * label names the item in messages.  Returns the number of attributes that could
 * not be applied (0 = everything done).  A read-only file refuses user.*
 * attributes even to its owner, so a missing write bit is added for the duration
 * and put back. */
static unsigned long bp_lxa_apply(int fd, const bp_nattr_info_t *info,
                                  const char *label, int allow_security,
                                  bp_lxa_stats_t *stats, int debug)
{
    bp_nattr_iter_t it;
    const char *aname;
    const unsigned char *avalue;
    unsigned long alen, vlen, bad = 0;
    struct stat st;
    mode_t saved_mode = 0;
    int widened = 0;

    bp_nattr_iter_begin(info, &it);
    while (bp_nattr_iter_next(&it, &aname, &alen, &avalue, &vlen)) {
        char attr[BP_NATTR_MAX_NAME + 1];
        int cls, rc, err;
        memcpy(attr, aname, (size_t)alen);
        attr[alen] = '\0';
        if (strcmp(attr, BP_LXA_HARDLINK_ITEM) == 0) continue;
        cls = bp_lxa_class(attr);
        if (cls == 0) {
            fprintf(stderr,
                    "boltpack: attribute '%s' of %s not applied: not an "
                    "attribute this system restores\n", attr, label);
            stats->refused++;
            bad++;
            continue;
        }
        if (cls == 2 && !allow_security) {
            fprintf(stderr,
                    "boltpack: attribute '%s' of %s not applied: security "
                    "attributes are restored only with --security-attrs\n",
                    attr, label);
            stats->refused++;
            bad++;
            continue;
        }
        rc = fsetxattr(fd, attr, avalue, (size_t)vlen, 0);
        err = errno;
        if (rc != 0 && (err == EACCES || err == EPERM) && !widened &&
            fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_mode & S_IWUSR) == 0) {
            saved_mode = st.st_mode & 07777;
            if (fchmod(fd, (mode_t)(saved_mode | S_IWUSR)) == 0) {
                widened = 1;
                rc = fsetxattr(fd, attr, avalue, (size_t)vlen, 0);
                err = errno;
            }
        }
        if (rc != 0) {
            fprintf(stderr,
                    "boltpack: cannot set attribute '%s' (%lu bytes) on %s: %s "
                    "(errno %d)\n", attr, vlen, label, strerror(err), err);
            stats->failed++;
            bad++;
            continue;
        }
        stats->restored++;
        if (debug)
            fprintf(stderr, "boltpack[debug]: %s: restored '%s' (%lu bytes)\n",
                    label, attr, vlen);
    }
    if (widened) (void)fchmod(fd, saved_mode);
    return bad;
}

#else /* not Linux */

static void bp_lxa_collect(const char *path, bp_nattr_builder_t *b,
                           int include_security, bp_lxa_stats_t *stats,
                           int debug)
{
    (void)path; (void)b; (void)include_security; (void)stats; (void)debug;
}

static unsigned long bp_lxa_apply(int fd, const bp_nattr_info_t *info,
                                  const char *label, int allow_security,
                                  bp_lxa_stats_t *stats, int debug)
{
    (void)fd; (void)label; (void)allow_security; (void)debug;
    stats->failed += info->count;
    return info->count;
}

#endif /* BP_LXA_AVAILABLE */

#endif /* BP_LINUX_XATTR_H */
