/* Exercise the Amiga GUI publication helper with host filesystem shims.
 * This checks the lifecycle logic, not native Amiga Rename semantics. */
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../src/core/archiver.h"

#define BP_MAX_PATH 512
typedef char *STRPTR;
static int rename_count, fail_rename, fail_delete, delay_rename;

static int Rename(STRPTR from, STRPTR to)
{
    rename_count++;
    if (delay_rename && rename_count == 1) sleep(2);
    if (rename_count == fail_rename) return 0;
    return rename(from, to) == 0;
}
static int DeleteFile(STRPTR path)
{
    return !fail_delete && unlink(path) == 0;
}
static void log_message(const char *format, ...) { (void)format; }

#include "../../src/frontend/amiga_gui/amiga_stage.h"

static int write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(data, 1, strlen(data), f) == strlen(data);
    return fclose(f) == 0 && ok;
}
static int has_contents(const char *path, const char *expected)
{
    char data[64]; size_t n; FILE *f = fopen(path, "rb");
    if (!f) return 0;
    n = fread(data, 1, sizeof(data), f);
    fclose(f);
    return n == strlen(expected) && memcmp(data, expected, n) == 0;
}
static int setup(gui_staged_output_t *stage, const char *dir, int existing)
{
    memset(stage, 0, sizeof(*stage)); stage->fd = -1;
    sprintf(stage->target, "%s/target", dir);
    sprintf(stage->temporary, "%s/stage", dir);
    unlink(stage->target); unlink(stage->temporary);
    rename_count = fail_rename = fail_delete = delay_rename = 0;
    if (existing && !write_file(stage->target, "original")) return 0;
    if (!write_file(stage->temporary, "replacement")) return 0;
    stage->target_existed = existing;
    return !existing || lstat(stage->target, &stage->target_snapshot) == 0;
}
static void remove_files(const char *dir)
{
    DIR *dp = opendir(dir); struct dirent *entry; char path[BP_MAX_PATH];
    if (!dp) return;
    while ((entry = readdir(dp)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        sprintf(path, "%s/%s", dir, entry->d_name); unlink(path);
    }
    closedir(dp); rmdir(dir);
}
int main(void)
{
    char dir[] = "/tmp/bp-amiga-stage-XXXXXX";
    gui_staged_output_t stage; int rc, failed = 0;
    if (!mkdtemp(dir)) return 2;

    if (!setup(&stage, dir, 1)) return 2;
    delay_rename = 1; /* Moving an inode can change its ctime. */
    rc = gui_commit_stage(&stage);
    if (rc != BP_OK || stage.temporary[0] || !has_contents(stage.target, "replacement")) failed++;

    if (!setup(&stage, dir, 1)) return 2;
    fail_rename = 2;
    rc = gui_commit_stage(&stage);
    if (rc != BP_ERR_IO || !has_contents(stage.target, "original")) failed++;
    gui_discard_stage(&stage);

    if (!setup(&stage, dir, 1)) return 2;
    unlink(stage.target); if (!write_file(stage.target, "changed")) return 2;
    rc = gui_commit_stage(&stage);
    if (rc != BP_ERR_PARTIAL || !has_contents(stage.target, "changed")) failed++;
    gui_discard_stage(&stage);

    if (!setup(&stage, dir, 0)) return 2;
    if (!write_file(stage.target, "concurrent")) return 2;
    rc = gui_commit_stage(&stage);
    if (rc != BP_ERR_PARTIAL || !has_contents(stage.target, "concurrent")) failed++;
    gui_discard_stage(&stage);

    if (!setup(&stage, dir, 0)) return 2;
    rc = gui_commit_stage(&stage);
    if (rc != BP_OK || !has_contents(stage.target, "replacement")) failed++;

    if (!setup(&stage, dir, 1)) return 2;
    fail_delete = 1;
    rc = gui_commit_stage(&stage);
    if (rc != BP_ERR_PARTIAL || stage.temporary[0] || !has_contents(stage.target, "replacement")) failed++;

    remove_files(dir);
    printf("Amiga GUI staging: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
