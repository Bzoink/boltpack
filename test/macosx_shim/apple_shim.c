/*
 * apple_shim.c -- host runtime shim for the macOS CLI (test only)
 *
 * Lets src/frontend/macosx/main.c run on Linux with -D__APPLE__ by backing the
 * Apple extended-attribute calls with Linux user.* attributes (name N is stored
 * as "user.N") and by providing simple CoreFoundation string stand-ins.  It
 * exercises BoltPack's own create/extract logic, not macOS itself: Apple
 * kernel behaviour, HFS+/APFS limits and real resource forks are not modelled.
 *
 * Set BOLTPACK_SHIM_FAIL=<name> to make fsetxattr fail with EPERM for that
 * attribute name, so the failure reporting can be tested.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/xattr.h>

#define PREFIX "user."
#define PREFIX_LEN 5

static int build_name(const char *name, char *out, size_t out_size)
{
    if (strlen(name) + PREFIX_LEN + 1 > out_size) { errno = ERANGE; return -1; }
    strcpy(out, PREFIX);
    strcpy(out + PREFIX_LEN, name);
    return 0;
}

ssize_t apple_listxattr(const char *path, char *namebuf, size_t size, int options)
{
    ssize_t n, i, out = 0;
    char *raw;
    (void)options;
    n = llistxattr(path, NULL, 0);
    if (n < 0) return -1;
    raw = (char *)malloc((size_t)n + 1);
    if (raw == NULL) { errno = ENOMEM; return -1; }
    n = llistxattr(path, raw, (size_t)n);
    if (n < 0) { free(raw); return -1; }
    for (i = 0; i < n; i += (ssize_t)strlen(raw + i) + 1) {
        const char *name = raw + i;
        size_t len;
        if (strncmp(name, PREFIX, PREFIX_LEN) != 0) continue;
        name += PREFIX_LEN;
        len = strlen(name) + 1;
        if (namebuf != NULL) {
            if ((size_t)out + len > size) { free(raw); errno = ERANGE; return -1; }
            memcpy(namebuf + out, name, len);
        }
        out += (ssize_t)len;
    }
    free(raw);
    return out;
}

ssize_t apple_getxattr(const char *path, const char *name, void *value,
                       size_t size, unsigned position, int options)
{
    char full[512];
    (void)position; (void)options;
    if (build_name(name, full, sizeof(full)) != 0) return -1;
    return lgetxattr(path, full, value, size);
}

int apple_setxattr(const char *path, const char *name, const void *value,
                   size_t size, unsigned position, int options)
{
    char full[512];
    (void)position; (void)options;
    if (build_name(name, full, sizeof(full)) != 0) return -1;
    return lsetxattr(path, full, value, size, 0);
}

int apple_fsetxattr(int fd, const char *name, const void *value, size_t size,
                    unsigned position, int options)
{
    char full[512];
    const char *fail = getenv("BOLTPACK_SHIM_FAIL");
    (void)position; (void)options;
    if (fail != NULL && strcmp(fail, name) == 0) { errno = EPERM; return -1; }
    if (build_name(name, full, sizeof(full)) != 0) return -1;
    return fsetxattr(fd, full, value, size, 0);
}

/* ---- CoreFoundation stand-ins used for case/normalisation keys ---------- */

typedef struct { char *text; } fake_cf_t;

void *CFStringCreateWithCString(const void *a, const char *s, unsigned long e)
{
    fake_cf_t *f = (fake_cf_t *)malloc(sizeof(*f));
    (void)a; (void)e;
    if (f == NULL) return NULL;
    f->text = strdup(s);
    if (f->text == NULL) { free(f); return NULL; }
    return f;
}

void *CFStringCreateMutableCopy(const void *a, long max, const void *s)
{
    (void)max;
    return CFStringCreateWithCString(a, ((const fake_cf_t *)s)->text, 0);
}

void CFRelease(const void *o)
{
    fake_cf_t *f = (fake_cf_t *)o;
    if (f != NULL) { free(f->text); free(f); }
}

void CFStringNormalize(void *s, unsigned long form) { (void)s; (void)form; }

void CFStringFold(void *s, unsigned long flags, void *locale)
{
    char *p = ((fake_cf_t *)s)->text;
    (void)flags; (void)locale;
    for (; *p != '\0'; p++) *p = (char)tolower((unsigned char)*p);
}

long CFStringGetLength(const void *s)
{
    return (long)strlen(((const fake_cf_t *)s)->text);
}

long CFStringGetMaximumSizeForEncoding(long length, unsigned long enc)
{
    (void)enc;
    return length;
}

int CFStringGetCString(const void *s, char *buf, long size, unsigned long enc)
{
    const char *t = ((const fake_cf_t *)s)->text;
    (void)enc;
    if ((long)strlen(t) + 1 > size) return 0;
    strcpy(buf, t);
    return 1;
}
