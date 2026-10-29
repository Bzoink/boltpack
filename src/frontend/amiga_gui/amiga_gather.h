/* Amiga GUI source traversal; include after platform helpers. */
#ifndef BP_AMIGA_GUI_GATHER_H
#define BP_AMIGA_GUI_GATHER_H

#ifndef BP_GUI_GATHER_MALLOC
#define BP_GUI_GATHER_MALLOC malloc
#endif
#ifndef BP_GUI_GATHER_CALLOC
#define BP_GUI_GATHER_CALLOC calloc
#endif
#ifndef BP_GUI_GATHER_FREE
#define BP_GUI_GATHER_FREE free
#endif

typedef struct gui_gather_frame {
    DIR *dir;
    char *path;
    struct gui_gather_frame *parent;
} gui_gather_frame_t;

static void gather_close_frames(gui_gather_frame_t *frame)
{
    while (frame != NULL) {
        gui_gather_frame_t *parent = frame->parent;
        if (frame->dir != NULL) closedir(frame->dir);
        BP_GUI_GATHER_FREE(frame->path);
        BP_GUI_GATHER_FREE(frame);
        frame = parent;
    }
}

static int gather_push_dir(gui_gather_frame_t **top, const char *path)
{
    gui_gather_frame_t *frame;
    frame = (gui_gather_frame_t *)BP_GUI_GATHER_CALLOC(1, sizeof(*frame));
    if (frame == NULL) return -1;
    frame->path = (char *)BP_GUI_GATHER_MALLOC(strlen(path) + 1U);
    if (frame->path == NULL) { BP_GUI_GATHER_FREE(frame); return -1; }
    strcpy(frame->path, path);
    frame->dir = opendir(path);
    if (frame->dir == NULL) {
        BP_GUI_GATHER_FREE(frame->path);
        BP_GUI_GATHER_FREE(frame);
        log_message("Cannot open source directory: %s", path);
        return -3;
    }
    frame->parent = *top;
    *top = frame;
    return 0;
}

static int gather_dir(const char *dir)
{
    gui_gather_frame_t *top = NULL;
    int rc;

    if (dir == NULL) return -3;
    rc = gather_push_dir(&top, dir);
    if (rc != 0) return rc;

    while (top != NULL) {
        char full[BP_MAX_PATH];
        struct stat st;
        struct dirent *de;
        size_t dir_len, name_len;

        errno = 0;
        de = readdir(top->dir);
        if (de == NULL) {
            int read_error = errno != 0;
            gui_gather_frame_t *done = top;
            top = done->parent;
            if (closedir(done->dir) != 0 || read_error) rc = -3;
            else rc = 0;
            BP_GUI_GATHER_FREE(done->path);
            BP_GUI_GATHER_FREE(done);
            if (rc != 0) break;
            continue;
        }
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) continue;
        dir_len = strlen(top->path);
        name_len = strlen(de->d_name);
        if (dir_len + name_len + 2U > sizeof(full)) { rc = -4; break; }
        memcpy(full, top->path, dir_len);
        full[dir_len] = '/';
        memcpy(full + dir_len + 1U, de->d_name, name_len + 1U);

        if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode) ||
            (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
            log_message("Cannot archive source: %s", full);
            rc = -3;
            break;
        }
        if (gather_count >= MAX_ENTRIES) { rc = -2; break; }
        rc = add_gather_file(full);
        if (rc != 0) break;
        if (S_ISDIR(st.st_mode)) {
            rc = gather_push_dir(&top, full);
            if (rc != 0) break;
        } else if (gather_count == 1 || (gather_count % 64) == 0) {
            log_message("Gathered %d file(s) so far, latest='%s'",
                        gather_count, full);
        }
    }
    if (top != NULL) gather_close_frames(top);
    return rc;
}

#endif /* BP_AMIGA_GUI_GATHER_H */
