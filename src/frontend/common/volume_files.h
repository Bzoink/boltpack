/*
 * volume_files.h -- volume set files for CLI frontends (archive format 7)
 *
 * Names: the last volume carries the archive name (backup.bolt); the others
 * replace the extension with the volume number (backup.000, backup.001, ...).
 * Writing creates each volume under a temporary name in the target
 * directory and gives it its final name when it is complete.  With --pause
 * the user is asked for new media before every volume.  Reading asks for a
 * volume that is missing or wrong when the frontend can ask (a terminal, or
 * --pause); otherwise it reports which volume is missing.
 *
 * The includer provides, before including this header:
 *   bp_io_from_file(bp_io_t *, FILE *)        its stdio stream adapter
 *   FILE *vol_hook_create(const char *target, char *tmp, size_t tmp_size)
 *       create a new, empty temporary file in the directory of `target`
 *       (which may not exist yet); store its name in tmp
 *   int  vol_hook_publish(const char *tmp, const char *target)
 *       give the finished temporary file its final name, replacing an
 *       existing file of that name; 0 on success
 *   int  vol_hook_sync(FILE *fp)              flush to the device; 0 = ok
 *   int  vol_hook_interactive(void)           1 if the user can be asked
 *   int  vol_hook_exists(const char *path)    1 if path exists
 *   void vol_hook_note_tmp(const char *tmp)   remember / forget ("") the
 *                                             temporary file for cleanup
 *   int  vol_hook_verbose(void)               1 to report finished volumes
 * and may define BP_VOL_DIR_SEPS (path separators, default "/") and
 * BP_VOL_PATH_MAX (default 4096) first.  ANSI C (C89) plus stdio.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_COMMON_VOLUME_FILES_H
#define BP_COMMON_VOLUME_FILES_H

#ifndef BP_VOL_PATH_MAX
#define BP_VOL_PATH_MAX 4096
#endif
#ifndef BP_VOL_DIR_SEPS
#define BP_VOL_DIR_SEPS "/"
#endif

/* printf precisions that keep every message inside its buffer */
#define BP_VOL_NAME_PREC ((int)BP_VOL_PATH_MAX)
#define BP_VOL_TEXT_PREC ((int)BP_VOL_PATH_MAX + 300)

typedef struct {
    char      last_name[BP_VOL_PATH_MAX];   /* the archive name */
    char      stem[BP_VOL_PATH_MAX];        /* name without its extension */
    int       last_given;                   /* last_name came from the user */
    int       interactive;                  /* may ask for media */
    int       pause;                        /* writing: ask before each volume */
    int       aborted;                      /* the user typed q */
    char      problem[BP_VOL_PATH_MAX + 400];
    /* the one open stream */
    FILE     *fp;
    bp_io_t   io;
    /* writing */
    char      tmp_name[BP_VOL_PATH_MAX + 32];
    unsigned  written;                      /* volumes finished */
    int       have_last;                    /* the last volume was written */
} bp_vol_files_t;

static int vol_is_digit(char c) { return c >= '0' && c <= '9'; }

/* strerror(errno), cut to a length the message buffers allow for */
static const char *vol_errtext(void)
{
    static char text[96];
    const char *e = strerror(errno);
    strncpy(text, e != NULL ? e : "error", sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    return text;
}

/* Start of the final path component. */
static const char *vol_base(const char *name)
{
    const char *base = name, *p;
    for (p = name; *p; p++)
        if (strchr(BP_VOL_DIR_SEPS, *p) != NULL) base = p + 1;
    return base;
}

/* Pointer to the extension dot of the final path component, or NULL. */
static const char *vol_ext_dot(const char *name)
{
    const char *base = vol_base(name);
    const char *dot = strrchr(base, '.');
    if (dot == NULL || dot == base) return NULL;
    return dot;
}

static int vol_has_number_ext(const char *name)
{
    const char *dot = vol_ext_dot(name);
    return dot != NULL && vol_is_digit(dot[1]) && vol_is_digit(dot[2]) &&
           vol_is_digit(dot[3]) && dot[4] == '\0';
}

static int vol_files_init(bp_vol_files_t *v, const char *name)
{
    const char *dot;
    size_t len;

    memset(v, 0, sizeof(*v));
    len = strlen(name);
    if (len == 0 || len >= BP_VOL_PATH_MAX - 8 || *vol_base(name) == '\0')
        return -1;
    dot = vol_ext_dot(name);
    len = dot ? (size_t)(dot - name) : strlen(name);
    memcpy(v->stem, name, len);
    v->stem[len] = '\0';
    if (!vol_has_number_ext(name)) {
        strcpy(v->last_name, name);
        v->last_given = 1;
    }
    return 0;
}

static void vol_numbered_name(const bp_vol_files_t *v, unsigned number,
                              char *out, size_t size)
{
    char num[8];
    sprintf(num, ".%03u", number % 1000U);
    if (strlen(v->stem) + 5 > size) { out[0] = '\0'; return; }
    strcpy(out, v->stem);
    strcat(out, num);
}

static int vol_exists(const char *path)
{
    return vol_hook_exists(path);
}

/* Name of the last volume: what the user typed, else stem.bolt or
 * stem.blt, whichever exists (stem.bolt when neither does). */
static void vol_last_name(const bp_vol_files_t *v, char *out, size_t size)
{
    if (v->last_given) {
        strncpy(out, v->last_name, size - 1);
        out[size - 1] = '\0';
        return;
    }
    if (strlen(v->stem) + 6 > size) { out[0] = '\0'; return; }
    strcpy(out, v->stem);
    strcat(out, ".blt");
    if (vol_exists(out)) return;
    strcpy(out, v->stem);
    strcat(out, ".bolt");
}

/* Ask the user to change media.  Returns 1 to retry, 0 to give up. */
static int vol_prompt(bp_vol_files_t *v, const char *message)
{
    char line[64];
    fprintf(stderr, "%s\nPress Enter when ready, or type q to quit: ", message);
    fflush(stderr);
    if (fgets(line, sizeof(line), stdin) == NULL ||
        line[0] == 'q' || line[0] == 'Q') {
        fprintf(stderr, "\n");
        v->aborted = 1;
        return 0;
    }
    return 1;
}

static void vol_close_fp(bp_vol_files_t *v)
{
    if (v->fp != NULL) fclose(v->fp);
    v->fp = NULL;
}

/* ---- reading ----------------------------------------------------------- */

static const char *vol_reason_text(int reason)
{
    switch (reason) {
    case BP_ERR_VOLUME:    return "belongs to another archive or is the wrong volume";
    case BP_ERR_MAGIC:     return "is not a BoltPack volume";
    case BP_ERR_VERSION:   return "is not archive format 7 (this build reads format 7 only)";
    case BP_ERR_TRUNCATED: return "is truncated or damaged";
    case BP_ERR_CORRUPT:   return "is damaged";
    default:               return "cannot be used";
    }
}

static bp_io_t *vol_src_open(void *user, bp_uint16 number, int reason)
{
    bp_vol_files_t *v = (bp_vol_files_t *)user;
    char name[BP_VOL_PATH_MAX + 8];
    char what[64];
    /* room for the problem text and the volume name together */
    char msg[2 * BP_VOL_PATH_MAX + 512];
    int named_last = number == BP_VOLUME_LAST_REQUEST && v->last_given;

    vol_close_fp(v);
    if (number == BP_VOLUME_LAST_REQUEST) {
        vol_last_name(v, name, sizeof(name));
        strcpy(what, "the last volume");
    } else {
        vol_numbered_name(v, number, name, sizeof(name));
        sprintf(what, "volume %u", (unsigned)number + 1U);
    }
    if (reason != BP_OK) {
        if (named_last)
            sprintf(v->problem, "%.*s %s", BP_VOL_NAME_PREC, name,
                    vol_reason_text(reason));
        else
            sprintf(v->problem, "%.*s (%s) %s", BP_VOL_NAME_PREC, name, what,
                    vol_reason_text(reason));
        /* A file the user named that is not a volume at all is an answer,
         * not a media problem: do not ask for another disk. */
        if (!v->interactive ||
            (named_last && (reason == BP_ERR_MAGIC || reason == BP_ERR_VERSION)))
            return NULL;
        sprintf(msg, "boltpack: %.*s.\nInsert or locate %s, %.*s.",
                BP_VOL_TEXT_PREC, v->problem, what, BP_VOL_NAME_PREC, name);
        if (!vol_prompt(v, msg)) return NULL;
        if (number == BP_VOLUME_LAST_REQUEST) vol_last_name(v, name, sizeof(name));
    }
    for (;;) {
        v->fp = fopen(name, "rb");
        if (v->fp != NULL) break;
        if (named_last)
            sprintf(v->problem, "cannot open %.*s: %s", BP_VOL_NAME_PREC, name,
                    vol_errtext());
        else
            sprintf(v->problem, "cannot open %.*s (%s): %s", BP_VOL_NAME_PREC, name,
                    what, vol_errtext());
        if (!v->interactive) return NULL;
        sprintf(msg, "boltpack: %.*s.\nInsert or locate %s, %.*s.",
                BP_VOL_TEXT_PREC, v->problem, what, BP_VOL_NAME_PREC, name);
        if (!vol_prompt(v, msg)) return NULL;
        if (number == BP_VOLUME_LAST_REQUEST) vol_last_name(v, name, sizeof(name));
    }
    v->problem[0] = '\0';
    bp_io_from_file(&v->io, v->fp);
    return &v->io;
}

static void vol_src_close(void *user, bp_io_t *io)
{
    bp_vol_files_t *v = (bp_vol_files_t *)user;
    if (io == &v->io) vol_close_fp(v);
}

/* Prepare a volume source for the archive name the user gave (any volume
 * of a set, or a single-volume archive). */
static int vol_source_open(bp_vol_files_t *v, bp_volume_source_t *src,
                           const char *name, int pause)
{
    if (vol_files_init(v, name) != 0) {
        fprintf(stderr, "boltpack: invalid or too long archive name: %s\n", name);
        return -1;
    }
    v->interactive = pause || vol_hook_interactive();
    src->open = vol_src_open;
    src->close = vol_src_close;
    src->user = v;
    return 0;
}

static void vol_source_done(bp_vol_files_t *v)
{
    vol_close_fp(v);
}

/* Print a read failure, naming the volume when one was at fault. */
static void vol_report(const bp_vol_files_t *v, int rc, const char *(*err)(int))
{
    if (v->aborted)
        fprintf(stderr, "boltpack: stopped at your request\n");
    else if (v->problem[0] != '\0')
        fprintf(stderr, "boltpack: %s\n", v->problem);
    else
        fprintf(stderr, "boltpack: %s\n", err(rc));
}

/* Before extracting without anyone to ask for media: is every numbered
 * volume of a set of `count` present?  Prints and returns 0 if not. */
static int vol_all_present(const bp_vol_files_t *v, unsigned count)
{
    unsigned i;
    for (i = 0; i + 1U < count; i++) {
        char name[BP_VOL_PATH_MAX + 8];
        vol_numbered_name(v, i, name, sizeof(name));
        if (!vol_exists(name)) {
            fprintf(stderr,
                    "boltpack: %s (volume %u of %u) is missing; nothing was extracted\n",
                    name, i + 1U, count);
            return 0;
        }
    }
    return 1;
}

/* ---- writing ----------------------------------------------------------- */

static bp_io_t *vol_sink_create(void *user, bp_uint16 number)
{
    bp_vol_files_t *v = (bp_vol_files_t *)user;
    char name[BP_VOL_PATH_MAX + 8];
    char msg[2 * BP_VOL_PATH_MAX + 512];

    vol_numbered_name(v, number, name, sizeof(name));
    if (v->pause) {
        sprintf(msg, "boltpack: insert the disk for volume %u (%.*s).",
                (unsigned)number + 1U, BP_VOL_NAME_PREC, name);
        if (!vol_prompt(v, msg)) return NULL;
    }
    for (;;) {
        v->fp = vol_hook_create(name, v->tmp_name, sizeof(v->tmp_name));
        if (v->fp != NULL) break;
        sprintf(v->problem, "cannot create volume %u beside %.*s: %s",
                (unsigned)number + 1U, BP_VOL_NAME_PREC, name, vol_errtext());
        if (!v->pause) return NULL;
        sprintf(msg, "boltpack: %.*s.", BP_VOL_TEXT_PREC, v->problem);
        if (!vol_prompt(v, msg)) return NULL;
    }
    vol_hook_note_tmp(v->tmp_name);
    bp_io_from_file(&v->io, v->fp);
    return &v->io;
}

static int vol_sink_finish(void *user, bp_io_t *io, bp_uint16 number,
                           int is_last, int status)
{
    bp_vol_files_t *v = (bp_vol_files_t *)user;
    char name[BP_VOL_PATH_MAX + 8];
    int ok;

    (void)io;
    if (v->fp == NULL) return BP_ERR_IO;
    if (status != BP_OK) {
        vol_close_fp(v);
        remove(v->tmp_name);
        vol_hook_note_tmp("");
        return 0;
    }
    ok = fflush(v->fp) == 0 && vol_hook_sync(v->fp) == 0;
    if (fclose(v->fp) != 0) ok = 0;
    v->fp = NULL;
    if (is_last) {
        strncpy(name, v->last_name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    } else {
        vol_numbered_name(v, number, name, sizeof(name));
    }
    if (!ok) {
        sprintf(v->problem, "cannot write volume %u (%.*s): %s",
                (unsigned)number + 1U, BP_VOL_NAME_PREC, name, vol_errtext());
        remove(v->tmp_name);
        vol_hook_note_tmp("");
        return BP_ERR_IO;
    }
    if (vol_hook_publish(v->tmp_name, name) != 0) {
        sprintf(v->problem, "cannot name volume %u %.*s: %s",
                (unsigned)number + 1U, BP_VOL_NAME_PREC, name, vol_errtext());
        remove(v->tmp_name);
        vol_hook_note_tmp("");
        return BP_ERR_IO;
    }
    vol_hook_note_tmp("");
    v->written = (unsigned)number + 1U;
    v->have_last = is_last;
    if (v->pause && vol_hook_verbose())
        fprintf(stderr, "boltpack: volume %u written: %s\n",
                (unsigned)number + 1U, name);
    return 0;
}

static void vol_sink_init(bp_vol_files_t *v, bp_volume_sink_t *sink)
{
    sink->create = vol_sink_create;
    sink->finish = vol_sink_finish;
    sink->user = v;
}

/* Remove the volumes of an abandoned set (not when media were changed). */
static void vol_remove_written(bp_vol_files_t *v)
{
    char name[BP_VOL_PATH_MAX + 8];
    unsigned i;
    if (v->pause) return;
    for (i = 0; i < v->written; i++) {
        if (v->have_last && i + 1U == v->written) {
            strncpy(name, v->last_name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        } else {
            vol_numbered_name(v, i, name, sizeof(name));
        }
        remove(name);
    }
    v->written = 0;
}

/* After a set of `count` volumes was written to one directory, remove
 * numbered volumes left behind by an older, larger set of the same name. */
static void vol_remove_stale(bp_vol_files_t *v, unsigned count)
{
    char name[BP_VOL_PATH_MAX + 8];
    unsigned i;
    if (v->pause || count == 0) return;
    for (i = count - 1U; i < 1000U; i++) {
        FILE *fp;
        unsigned char h[6];
        int stale;
        vol_numbered_name(v, i, name, sizeof(name));
        fp = fopen(name, "rb");
        if (fp == NULL) break;
        stale = fread(h, 1, 6, fp) == 6 && h[0] == 'B' && h[1] == 'O' &&
                h[2] == 'L' && h[3] == 'T' && h[4] == 7 && h[5] == 0;
        fclose(fp);
        if (!stale) break;
        if (remove(name) == 0 && vol_hook_verbose())
            fprintf(stderr, "boltpack: removed %s, left over from an older set\n",
                    name);
    }
}

#endif /* BP_COMMON_VOLUME_FILES_H */
