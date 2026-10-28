/* Amiga GUI source traversal; include after platform helpers. */
#ifndef BP_AMIGA_GUI_GATHER_H
#define BP_AMIGA_GUI_GATHER_H

static int gather_dir(const char *dir)
{
    DIR *dp;
    struct dirent *de;
    int rc;

    dp = opendir(dir);
    if (!dp) {
        log_message("Cannot open source directory: %s",
                    dir ? dir : "(null)");
        return -3;
    }

    for (;;) {
        char full[BP_MAX_PATH];
        struct stat st;
        size_t dir_len, name_len;

        errno = 0;
        de = readdir(dp);
        if (de == NULL) {
            rc = errno ? -3 : 0;
            if (closedir(dp) != 0) rc = -3;
            return rc;
        }
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0) continue;
        dir_len = strlen(dir);
        name_len = strlen(de->d_name);
        if (dir_len + name_len + 2 > sizeof(full)) {
            closedir(dp);
            return -4;
        }
        memcpy(full, dir, dir_len);
        full[dir_len] = '/';
        memcpy(full + dir_len + 1, de->d_name, name_len + 1);

        if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode) ||
            (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
            log_message("Cannot archive source: %s", full);
            closedir(dp);
            return -3;
        }
        if (S_ISDIR(st.st_mode)) {
            if (gather_count >= MAX_ENTRIES) { closedir(dp); return -2; }
            rc = add_gather_file(full);
            if (rc != 0) { closedir(dp); return rc; }
            rc = gather_dir(full);
            if (rc != 0) {
                closedir(dp);
                return rc;
            }
        } else if (gather_count < MAX_ENTRIES) {
            rc = add_gather_file(full);
            if (rc != 0) {
                closedir(dp);
                return rc;
            }
            if (gather_count == 1 || (gather_count % 64) == 0)
                log_message("Gathered %d file(s) so far, latest='%s'",
                            gather_count, full);
        } else {
            closedir(dp);
            return -2;
        }
    }
}

#endif /* BP_AMIGA_GUI_GATHER_H */
