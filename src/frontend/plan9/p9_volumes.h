/*
 * p9_volumes.h -- Plan 9 hooks for volume set files (archive format 7)
 *
 * Supplies the platform hooks of src/frontend/common/volume_files.h for the
 * Plan 9 CLI.  Volumes are plain file descriptors (create/open/read/write).
 * A new volume is created with OEXCL as bpXXXXXX.tmp beside its final name
 * and renamed into place with dirwstat() once complete, after any older
 * file of that name has been removed.
 *
 * volume_files.h is written for ANSI C stdio.  Plan 9's libc has no
 * fprintf/sprintf and spells unsigned conversions %ud, so while that header
 * is included, fprintf, sprintf, stderr and fflush are mapped to small
 * wrappers below that turn %u into %ud and write through the Plan 9 libc.
 *
 * Included by src/frontend/plan9/main.c after bp_io_from_p9() and
 * g_verbosity.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_P9_VOLUMES_H
#define BP_P9_VOLUMES_H

#include <stdarg.h>

#define BP_VOL_OWN_STREAMS
#define BP_VOL_DIR_SEPS "/"
#define BP_VOL_PATH_MAX P9_MAX_PATH

typedef int bp_vol_stream_t;
#define BP_VOL_NO_STREAM (-1)

static p9_io_t g_p9_vol_ctx;

/* ---- formatting ------------------------------------------------------- */

/* Copy a C format string, turning each %u conversion (with any flags, width,
 * precision or length) into Plan 9's %ud. */
static void p9v_format(char *out, int size, const char *fmt)
{
    int n = 0;
    while (*fmt != '\0' && n + 3 < size) {
        if (*fmt != '%') { out[n++] = *fmt++; continue; }
        out[n++] = *fmt++;
        if (*fmt == '%') { out[n++] = *fmt++; continue; }
        while (*fmt != '\0' && strchr("#-+ 0123456789.*lh", *fmt) != nil &&
               n + 3 < size)
            out[n++] = *fmt++;
        if (*fmt == 'u') {
            out[n++] = 'u';
            out[n++] = 'd';
            fmt++;
        } else if (*fmt != '\0') {
            out[n++] = *fmt++;
        }
    }
    out[n] = '\0';
}

static int p9v_sprintf(char *buf, const char *fmt, ...)
{
    char f[256];
    va_list ap;
    int n;
    p9v_format(f, (int)sizeof(f), fmt);
    va_start(ap, fmt);
    n = vsnprint(buf, 4096, f, ap);
    va_end(ap);
    return n;
}

static int p9v_fprintf(int fd, const char *fmt, ...)
{
    static char text[4096];
    char f[256];
    va_list ap;
    int n;
    p9v_format(f, (int)sizeof(f), fmt);
    va_start(ap, fmt);
    n = vsnprint(text, (int)sizeof(text), f, ap);
    va_end(ap);
    if (n > 0) write(fd, text, strlen(text));
    return n;
}

static char *p9v_errtext(void)
{
    static char text[128];
    text[0] = '\0';
    rerrstr(text, (unsigned int)sizeof(text));
    return text;
}

#undef fprintf
#undef sprintf
#undef stderr
#undef fflush
#define fprintf p9v_fprintf
#define sprintf p9v_sprintf
#define stderr 2
#define fflush(x) 0
#define BP_VOL_ERRTEXT() p9v_errtext()

/* ---- hooks ------------------------------------------------------------ */

static bp_vol_stream_t vol_hook_open_read(const char *path)
{
    return open((char *)path, OREAD);
}

static int vol_hook_close(bp_vol_stream_t s)
{
    return close(s) < 0 ? -1 : 0;
}

/* writes go straight to the file server */
static int vol_hook_flush(bp_vol_stream_t s)
{
    (void)s;
    return 0;
}

static int vol_hook_sync(bp_vol_stream_t s)
{
    (void)s;
    return 0;
}

static long vol_hook_read(bp_vol_stream_t s, void *buf, unsigned n)
{
    return read(s, buf, (long)n);
}

static void vol_hook_io(bp_io_t *io, bp_vol_stream_t s)
{
    bp_io_from_p9(io, s, &g_p9_vol_ctx);
}

/* one line from standard input; 0 at end of file */
static int vol_hook_getline(char *buf, int size)
{
    int n = 0;
    char c;
    while (n + 1 < size && read(0, &c, 1) == 1) {
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return n > 0;
}

static bp_vol_stream_t vol_hook_create(const char *target, char *tmp,
                                       size_t tmp_size)
{
    static unsigned long sequence = 0;
    const char *slash = strrchr(target, '/');
    size_t prefix = slash != nil ? (size_t)(slash - target + 1) : 0;
    int attempt, fd;

    if (prefix + 16 >= tmp_size) return BP_VOL_NO_STREAM;
    for (attempt = 0; attempt < 100; attempt++) {
        memcpy(tmp, target, prefix);
        snprint(tmp + prefix, 16, "bp%06lux.tmp",
                (ulong)((((ulong)getpid()) * 31UL + sequence++) & 0xFFFFFFUL));
        fd = create(tmp, OWRITE | OEXCL, 0666);
        if (fd >= 0) return fd;
    }
    return BP_VOL_NO_STREAM;
}

/* A volume replaces a file of its name (its set is being rewritten), but
 * never a directory.  tmp is beside target, so a rename within the
 * directory (a name change through dirwstat) moves it into place. */
static int vol_hook_publish(const char *tmp, const char *target)
{
    Dir d;
    Dir *old = dirstat((char *)target);
    const char *base = strrchr(target, '/');

    if (old != nil) {
        int is_dir = (old->mode & DMDIR) != 0;
        free(old);
        if (is_dir || remove((char *)target) != 0) return -1;
    }
    base = base != nil ? base + 1 : target;
    nulldir(&d);
    d.name = (char *)base;
    return dirwstat((char *)tmp, &d) < 0 ? -1 : 0;
}

/* Plan 9 has no isatty: a terminal is the console file */
static int vol_hook_interactive(void)
{
    char path[128];
    return fd2path(0, path, (int)sizeof(path)) == 0 &&
           strcmp(path, "/dev/cons") == 0;
}

static int vol_hook_exists(const char *path)
{
    Dir *d = dirstat((char *)path);
    if (d == nil) return 0;
    free(d);
    return 1;
}

static void vol_hook_note_tmp(const char *tmp)
{
    (void)tmp;   /* Plan 9: no signal-time cleanup */
}

static int vol_hook_verbose(void)
{
    return g_verbosity < VERB_SILENT;
}

#include "../common/volume_size.h"
#include "../common/volume_files.h"

#undef fprintf
#undef sprintf
#undef stderr
#undef fflush

#endif /* BP_P9_VOLUMES_H */
