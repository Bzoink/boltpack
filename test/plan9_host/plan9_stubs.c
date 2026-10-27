#include "libc.h"

#include <dirent.h>
#include <utime.h>

int create(const char *path, int mode, unsigned int perm)
{
    int flags;
    if (perm & DMDIR) {
        if (mkdir(path, perm & 0777) != 0 && errno != EEXIST) return -1;
        return open(path, O_RDONLY | O_DIRECTORY);
    }
    if (mode == OREAD) flags = O_RDONLY;
    else if (mode == OWRITE) flags = O_WRONLY;
    else flags = O_RDWR;
    return open(path, flags | O_CREAT | O_TRUNC, perm & 0777);
}

vlong seek(int fd, vlong offset, int origin)
{
    off_t result = lseek(fd, (off_t)offset, origin);
    return result < 0 ? -1 : (vlong)result;
}

Dir *dirstat(const char *path)
{
    struct stat st;
    Dir *dir;
    const char *name;
    if (stat(path, &st) != 0) return nil;
    dir = (Dir *)calloc(1, sizeof(*dir));
    if (!dir) return nil;
    name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (strlen(name) >= sizeof(dir->name)) {
        free(dir);
        return nil;
    }
    strcpy(dir->name, name);
    if (S_ISDIR(st.st_mode)) dir->mode = DMDIR | (u32int)(st.st_mode & 0777);
    else dir->mode = (u32int)(st.st_mode & 0777);
    dir->length = (vlong)st.st_size;
    dir->mtime = (vlong)st.st_mtime;
    return dir;
}

long dirreadall(int fd, Dir **dirs)
{
    (void)fd;
    *dirs = nil;
    return 0;
}

void nulldir(Dir *dir)
{
    memset(dir, 0, sizeof(*dir));
}

int dirwstat(const char *path, const Dir *dir)
{
    int rc = 0;
    if (dir->mode != 0) rc = chmod(path, dir->mode & 0777);
    if (dir->mtime != 0) {
        struct utimbuf times;
        times.actime = 0;
        times.modtime = (time_t)dir->mtime;
        if (utime(path, &times) != 0) rc = -1;
    }
    return rc;
}

void rerrstr(char *buf, unsigned int n)
{
    if (n == 0) return;
    snprintf(buf, n, "%s", strerror(errno));
}

static int vout(FILE *fp, const char *fmt, va_list ap)
{
    return vfprintf(fp, fmt, ap);
}

int print(const char *fmt, ...)
{
    va_list ap;
    int rc;
    va_start(ap, fmt); rc = vout(stdout, fmt, ap); va_end(ap);
    return rc;
}

int fprint(int fd, const char *fmt, ...)
{
    va_list ap;
    FILE *fp = fd == 2 ? stderr : stdout;
    int rc;
    va_start(ap, fmt); rc = vout(fp, fmt, ap); va_end(ap);
    return rc;
}

int snprint(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int rc;
    va_start(ap, fmt); rc = vsnprintf(buf, size, fmt, ap); va_end(ap);
    return rc;
}

int sprint(char *buf, const char *fmt, ...)
{
    va_list ap;
    int rc;
    va_start(ap, fmt); rc = vsprintf(buf, fmt, ap); va_end(ap);
    return rc;
}

void exits(const char *status)
{
    exit(status && status[0] ? 1 : 0);
}
