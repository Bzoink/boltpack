/* Exercise the Amiga GUI traversal with host filesystem shims. */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BP_MAX_PATH 512
#define MAX_ENTRIES 2048
static int gather_count, dotfile_seen, dotdir_seen, empty_seen, add_failure;
static int add_gather_file(const char *path)
{
    if (add_failure) return -1;
    if (strstr(path, "/.hidden")) dotfile_seen = 1;
    if (strstr(path, "/.folder/file")) dotdir_seen = 1;
    if (strstr(path, "/empty")) empty_seen = 1;
    gather_count++;
    return 0;
}
static void log_message(const char *format, ...) { (void)format; }

#include "../../src/frontend/amiga_gui/amiga_gather.h"

static int touch(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    return fclose(f) == 0;
}
int main(void)
{
    char root[] = "/tmp/bp-amiga-gather-XXXXXX";
    char good[1024], hidden[1024], subdir[1024], file[1024], empty[1024];
    char links[1024], linkpath[1024], missing[1024], longroot[1024];
    char levels[6][1024], component[100];
    int i, failed = 0;
    if (!mkdtemp(root)) return 2;
    snprintf(good, sizeof(good), "%s/good", root);
    snprintf(hidden, sizeof(hidden), "%s/good/.hidden", root);
    snprintf(subdir, sizeof(subdir), "%s/good/.folder", root);
    snprintf(file, sizeof(file), "%s/good/.folder/file", root);
    snprintf(empty, sizeof(empty), "%s/good/empty", root);
    snprintf(links, sizeof(links), "%s/links", root);
    snprintf(linkpath, sizeof(linkpath), "%s/links/link", root);
    snprintf(missing, sizeof(missing), "%s/missing", root);
    snprintf(longroot, sizeof(longroot), "%s/long", root);
    if (mkdir(good, 0700) || mkdir(subdir, 0700) || !touch(hidden) ||
        !touch(file) || mkdir(empty, 0700) || mkdir(links, 0700) || symlink(file, linkpath) ||
        mkdir(longroot, 0700)) return 2;

    if (gather_dir(good) != 0 || gather_count != 4 || !dotfile_seen || !dotdir_seen || !empty_seen) failed++;
    if (gather_dir(links) != -3) failed++;
    if (gather_dir(missing) != -3) failed++;
    gather_count = MAX_ENTRIES;
    if (gather_dir(good) != -2) failed++;
    gather_count = 0; add_failure = 1;
    if (gather_dir(good) != -1) failed++;
    add_failure = 0;

    memset(component, 'd', sizeof(component) - 1); component[sizeof(component) - 1] = '\0';
    for (i = 0; i < 6; i++) {
        snprintf(levels[i], sizeof(levels[i]), "%s/%s", i ? levels[i - 1] : longroot, component);
        if (mkdir(levels[i], 0700)) return 2;
    }
    if (gather_dir(longroot) != -4) failed++;
    for (i = 5; i >= 0; i--) rmdir(levels[i]);
    rmdir(longroot); unlink(linkpath); rmdir(links); unlink(file);
    rmdir(subdir); rmdir(empty); unlink(hidden); rmdir(good); rmdir(root);
    printf("Amiga GUI source traversal: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
