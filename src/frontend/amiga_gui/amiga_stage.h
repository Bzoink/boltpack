/* Amiga GUI sibling-stage helpers; include after platform and core types. */
#ifndef BP_AMIGA_GUI_STAGE_H
#define BP_AMIGA_GUI_STAGE_H

typedef struct {
    bp_io_t io;
    int fd;
    char target[BP_MAX_PATH];
    char temporary[BP_MAX_PATH];
    int target_existed;
    struct stat target_snapshot;
} gui_staged_output_t;

static int gui_make_temp_name(const char *target, const char *leaf,
                              char *out, size_t cap)
{
    const char *slash = strrchr(target, '/');
    const char *colon = strchr(target, ':');
    size_t prefix = 0;
    if (colon && (!slash || colon > slash)) prefix = (size_t)(colon - target) + 1;
    else if (slash) prefix = (size_t)(slash - target) + 1;
    if (prefix + strlen(leaf) + 1 > cap) return -1;
    memcpy(out, target, prefix);
    strcpy(out + prefix, leaf);
    return 0;
}

static int gui_same_file(const struct stat *a, const struct stat *b)
{
    return S_ISREG(a->st_mode) && S_ISREG(b->st_mode) &&
           a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime &&
           a->st_ctime == b->st_ctime;
}

/* A rename can change ctime even when it moved the original inode. */
static int gui_same_moved_file(const struct stat *a, const struct stat *b)
{
    return S_ISREG(a->st_mode) && S_ISREG(b->st_mode) &&
           a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime;
}

static void gui_discard_stage(gui_staged_output_t *stage)
{
    if (stage->fd >= 0) { (void)close(stage->fd); stage->fd = -1; }
    if (stage->temporary[0]) {
        (void)unlink(stage->temporary);
        stage->temporary[0] = '\0';
    }
}

static int gui_commit_stage(gui_staged_output_t *stage)
{
    struct stat current, backup_st;
    char backup[BP_MAX_PATH];
    int fd, backup_moved = 0;
    if (lstat(stage->target, &current) == 0) {
        if (!stage->target_existed || !gui_same_file(&stage->target_snapshot, &current))
            return BP_ERR_PARTIAL;
    } else if (errno != ENOENT || stage->target_existed) return BP_ERR_IO;
    if (stage->target_existed) {
        if (gui_make_temp_name(stage->target, "BRXXXXXX", backup,
                               sizeof(backup)) != 0) return BP_ERR_NAME_TOO_LONG;
        fd = mkstemp(backup);
        if (fd < 0) return BP_ERR_IO;
        if (close(fd) != 0 || unlink(backup) != 0) {
            (void)unlink(backup);
            return BP_ERR_IO;
        }
        if (!Rename((STRPTR)stage->target, (STRPTR)backup)) return BP_ERR_IO;
        backup_moved = 1;
        if (lstat(backup, &backup_st) != 0 ||
            !gui_same_moved_file(&stage->target_snapshot, &backup_st)) {
            if (!Rename((STRPTR)backup, (STRPTR)stage->target))
                log_message("Changed destination remains at recovery path: %s", backup);
            return BP_ERR_PARTIAL;
        }
    }
    if (!Rename((STRPTR)stage->temporary, (STRPTR)stage->target)) {
        if (backup_moved && !Rename((STRPTR)backup, (STRPTR)stage->target))
            log_message("Original file remains at recovery path: %s", backup);
        return BP_ERR_IO;
    }
    stage->temporary[0] = '\0';
    if (backup_moved && !DeleteFile((STRPTR)backup)) {
        log_message("Cannot remove replaced-file backup: %s", backup);
        return BP_ERR_PARTIAL;
    }
    return BP_OK;
}

#endif /* BP_AMIGA_GUI_STAGE_H */
