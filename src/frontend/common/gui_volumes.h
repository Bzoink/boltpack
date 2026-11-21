/*
 * gui_volumes.h -- volume sets (archive format 7) for the Amiga and Atari ST GUIs
 *
 * The GUIs reach every archive through a volume source, so an archive of one
 * file and a set of diskettes are opened, listed, validated and extracted the
 * same way: nothing keeps a file open between operations, which matters when
 * the user has to swap disks.  New archives are written through a volume sink
 * when a disk size is chosen.
 *
 * Provided here, on top of volume_files.h with BP_VOL_OWN_PROMPT:
 *   gui_vol / gui_vol_src      the one volume set in use (static: the
 *                              buffers are large and GEM stacks are small)
 *   gui_vol_begin_read()       prepare gui_vol_src for an archive path
 *   gui_vol_begin_write()      prepare gui_vol for writing a set
 *   gui_vol_failure()          text for the reason the last call failed
 *   gui_vol_prompt_text()      the text a media prompt shows
 *
 * The includer supplies, before including this header, the hooks of
 * volume_files.h (see amiga_volumes.h and atari_volumes.h) and
 *   int vol_hook_prompt(int writing, const char *what, const char *name,
 *                       const char *problem)
 * and BP_VOL_OWN_PROMPT must be defined before those headers.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_COMMON_GUI_VOLUMES_H
#define BP_COMMON_GUI_VOLUMES_H

/* A disk size offered in a menu: the bytes one volume may hold.  These are
 * usable bytes on a freshly formatted disk, a little under the nominal
 * capacity (a 880K Amiga OFS disk holds 844 KB of file data, a 720K FAT disk
 * 730 KB, and so on), so that a volume always fits. */
typedef struct {
    const char *label;
    bp_uint32   bytes;
} gui_disk_size_t;

static bp_vol_files_t gui_vol;
static bp_volume_source_t gui_vol_src;

/* Prepare gui_vol_src for the archive at path (any volume of a set, or a
 * single-volume archive).  Returns 0, or -1 if the name cannot be used. */
static int gui_vol_begin_read(const char *path)
{
    if (vol_files_init(&gui_vol, path) != 0) return -1;
    gui_vol.interactive = 1;
    gui_vol_src.open = vol_src_open;
    gui_vol_src.close = vol_src_close;
    gui_vol_src.user = &gui_vol;
    return 0;
}

/* Prepare gui_vol for writing a set named path (the last volume carries
 * that name; the others are path with .000, .001, ...).  The user is asked
 * for media before every volume but the first, which goes where path says.
 * Returns 0, or -1 if the name cannot be used. */
static int gui_vol_begin_write(const char *path)
{
    if (vol_files_init(&gui_vol, path) != 0 || vol_has_number_ext(path))
        return -1;
    gui_vol.pause = 1;
    gui_vol.no_first_prompt = 1;
    return 0;
}

/* After a failed volume call: the reason as text, or NULL when the user
 * cancelled a media prompt.  Valid until the next volume call. */
static const char *gui_vol_failure(int rc)
{
    if (gui_vol.aborted) return NULL;
    if (gui_vol.problem[0] != '\0') return gui_vol.problem;
    return bp_strerror(rc);
}

/* The text of a media prompt, lines separated by '\n': what is wrong with
 * the file the volume code tried, if anything, then what to do.  Short, for
 * the five 30-character lines of a GEM alert. */
static const char *gui_vol_prompt_text(int writing, const char *what,
                                       const char *name, const char *problem)
{
    static char text[BP_VOL_PATH_MAX + 300];
    const char *base = vol_base(name);

    if (problem != NULL && problem[0] != '\0')
        sprintf(text, "%.*s %.120s.\n%s %s and\npress OK.",
                (int)BP_VOL_PATH_MAX, base, problem,
                writing ? "Insert an empty disk for" : "Insert", what);
    else if (writing)
        sprintf(text, "Insert an empty, formatted\ndisk for %s\n(%.*s), then press OK.",
                what, (int)BP_VOL_PATH_MAX, base);
    else
        sprintf(text, "Insert the disk with\n%s (%.*s),\nthen press OK.",
                what, (int)BP_VOL_PATH_MAX, base);
    return text;
}

#endif /* BP_COMMON_GUI_VOLUMES_H */
