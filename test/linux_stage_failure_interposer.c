#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

static int is_stage(FILE *stream)
{
    char proc_path[64];
    char path[PATH_MAX + 1];
    int fd = fileno(stream);
    ssize_t length;
    if (fd < 0) return 0;
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    length = readlink(proc_path, path, sizeof(path) - 1);
    if (length < 0) return 0;
    path[length] = '\0';
    return strstr(path, ".boltpack-gui-") != NULL ||
           strstr(path, "/.boltpack-") != NULL;
}

size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream)
{
    static size_t (*real_fwrite)(const void *, size_t, size_t, FILE *);
    static int delayed;
    if (!delayed && is_stage(stream) &&
        getenv("BOLTPACK_TEST_DELAY_FIRST_WRITE") != NULL) {
        delayed = 1;
        usleep(500000);
    }
    if (is_stage(stream) && getenv("BOLTPACK_TEST_FAIL_FWRITE") != NULL) {
        errno = EIO;
        return 0;
    }
    if (real_fwrite == NULL)
        real_fwrite = (size_t (*)(const void *, size_t, size_t, FILE *))
            dlsym(RTLD_NEXT, "fwrite");
    if (real_fwrite == NULL) {
        errno = ENOSYS;
        return 0;
    }
    return real_fwrite(ptr, size, count, stream);
}

int futimens(int fd, const struct timespec times[2])
{
    static int (*real_futimens)(int, const struct timespec [2]);
    struct stat st;
    if (getenv("BOLTPACK_TEST_FAIL_FUTIMENS") != NULL) {
        errno = EIO;
        return -1;
    }
    if (getenv("BOLTPACK_TEST_FAIL_DIR_FUTIMENS") != NULL &&
        fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
        errno = EIO;
        return -1;
    }
    if (real_futimens == NULL)
        real_futimens = (int (*)(int, const struct timespec [2]))
            dlsym(RTLD_NEXT, "futimens");
    if (real_futimens == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_futimens(fd, times);
}

int fflush(FILE *stream)
{
    static int (*real_fflush)(FILE *);
    static int failed;
    if (!failed && is_stage(stream) &&
        getenv("BOLTPACK_TEST_FAIL_FFLUSH") != NULL) {
        failed = 1;
        errno = EIO;
        return EOF;
    }
    if (real_fflush == NULL)
        real_fflush = (int (*)(FILE *))dlsym(RTLD_NEXT, "fflush");
    if (real_fflush == NULL) {
        errno = ENOSYS;
        return EOF;
    }
    return real_fflush(stream);
}

int fclose(FILE *stream)
{
    static int (*real_fclose)(FILE *);
    static int failed;
    int fail = !failed && is_stage(stream) &&
               getenv("BOLTPACK_TEST_FAIL_FCLOSE") != NULL;
    if (real_fclose == NULL)
        real_fclose = (int (*)(FILE *))dlsym(RTLD_NEXT, "fclose");
    if (real_fclose == NULL) {
        errno = ENOSYS;
        return EOF;
    }
    if (fail) failed = 1;
    if (real_fclose(stream) != 0) return EOF;
    if (fail) {
        errno = EIO;
        return EOF;
    }
    return 0;
}
