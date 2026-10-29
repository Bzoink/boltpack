#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "restore_mode.h"

static int check_mode(const char *path, mode_t expected)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (st.st_mode & 07777) == expected;
}

int main(void)
{
    char path[] = "/tmp/boltpack-mode-XXXXXX";
    bp_uint16 flags;
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    if (fchmod(fd, 0600) != 0) return 2;
    flags = (bp_uint16)(BP_ENTRY_FLAG_MODE_VALID | 0640);
    if (bp_posix_restore_mode_fd(fd, flags) != BP_OK ||
        !check_mode(path, 0640)) return 3;

    flags = (bp_uint16)(BP_ENTRY_FLAG_MODE_VALID | 04755);
    if (bp_posix_restore_mode_fd(fd, flags) != BP_ERR_PARTIAL ||
        !check_mode(path, 0755)) return 4;

    if (fchmod(fd, 0600) != 0 ||
        bp_posix_restore_mode_fd(fd, 0) != BP_OK ||
        !check_mode(path, 0600)) return 5;

    flags = (bp_uint16)(BP_ENTRY_FLAG_MODE_VALID | 0700);
    if (bp_posix_restore_mode_path(path, flags) != BP_OK ||
        !check_mode(path, 0700)) return 6;

    close(fd);
    unlink(path);
    puts("POSIX mode restoration checks passed");
    return 0;
}
