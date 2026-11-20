/*
 * amiga_volumes.h -- AmigaOS hooks for volume set files (archive format 7)
 *
 * Supplies the platform hooks of src/frontend/common/volume_files.h for the
 * AmigaOS CLI (also built for AmigaOS 4, MorphOS and AROS).  Volumes are
 * stdio streams.  A new volume is created as bpvolNNN.tmp beside its final
 * name and moved into place with Rename() after any older file of that name
 * has been deleted (Rename() does not replace).  The user is asked for media
 * when standard input is an interactive console.
 *
 * Included by src/frontend/amiga/main.c after bp_io_from_file() and
 * g_verbosity.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_AMIGA_VOLUMES_H
#define BP_AMIGA_VOLUMES_H

#define BP_VOL_OWN_GETLINE
#define BP_VOL_DIR_SEPS "/:"
#define BP_VOL_PATH_MAX BP_MAX_PATH

/* strerror() would link libnix's locale support, which opens locale.library
 * at startup and fails on AmigaOS 2.04; AmigaDOS has its own error texts. */
static const char *amiga_vol_errtext(void)
{
    static char text[96];
    text[0] = '\0';
    if (IoErr() != 0)
        (void)Fault(IoErr(), (STRPTR)"", (STRPTR)text, (LONG)sizeof(text));
    if (text[0] == '\0') return "error";
    return text[0] == ':' && text[1] == ' ' ? text + 2 : text;
}
#define BP_VOL_ERRTEXT() amiga_vol_errtext()

static int vol_hook_exists(const char *path)
{
    BPTR lock = Lock((STRPTR)path, ACCESS_READ);
    if (lock == 0) return 0;
    UnLock(lock);
    return 1;
}

static FILE *vol_hook_create(const char *target, char *tmp, size_t tmp_size)
{
    static unsigned long sequence = 0;
    const char *p, *sep = target;
    size_t prefix;
    int attempt;

    for (p = target; *p; p++)
        if (*p == '/' || *p == ':') sep = p + 1;
    prefix = (size_t)(sep - target);
    if (prefix + 16 >= tmp_size) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    for (attempt = 0; attempt < 1000; attempt++) {
        memcpy(tmp, target, prefix);
        sprintf(tmp + prefix, "bpvol%03lu.tmp", (sequence++) % 1000UL);
        if (!vol_hook_exists(tmp)) return fopen(tmp, "wb");
    }
    errno = EEXIST;
    return NULL;
}

/* A volume replaces any file of its name (its set is being rewritten);
 * AmigaDOS Rename() does not replace, so delete the old file first. */
static int vol_hook_publish(const char *tmp, const char *target)
{
    if (vol_hook_exists(target) && !DeleteFile((STRPTR)target)) return -1;
    return Rename((STRPTR)tmp, (STRPTR)target) ? 0 : -1;
}

static int vol_hook_sync(FILE *fp)
{
    return fflush(fp);
}

static int vol_hook_getline(char *buf, int size)
{
    int n = 0;
    char c;
    while (n + 1 < size && Read(Input(), &c, 1) == 1) {
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return n > 0;
}

static int vol_hook_interactive(void)
{
    return IsInteractive(Input()) ? 1 : 0;
}

static void vol_hook_note_tmp(const char *tmp)
{
    (void)tmp;   /* Amiga: no signal-time cleanup */
}

static int vol_hook_verbose(void)
{
    return g_verbosity < VERB_SILENT;
}

#include "../common/volume_size.h"
#include "../common/volume_files.h"

#endif /* BP_AMIGA_VOLUMES_H */
