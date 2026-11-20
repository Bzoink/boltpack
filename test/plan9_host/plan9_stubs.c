#include "libc.h"

#include <dirent.h>
#include <utime.h>

int create(const char *path, int mode, unsigned int perm)
{
    int flags;
    if (perm & DMDIR) {
        if (mkdir(path, perm & 0777) != 0) return -1;
        return open(path, O_RDONLY | O_DIRECTORY);
    }
    if ((mode & 3) == OREAD) flags = O_RDONLY;
    else if ((mode & 3) == OWRITE) flags = O_WRONLY;
    else flags = O_RDWR;
    flags |= O_CREAT | O_TRUNC;
    if (mode & OEXCL) flags |= O_EXCL;
    return open(path, flags, perm & 0777);
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
    if (strlen(name) >= sizeof(dir->namebuf)) {
        free(dir);
        return nil;
    }
    strcpy(dir->namebuf, name);
    dir->name = dir->namebuf;
    if (S_ISDIR(st.st_mode)) dir->mode = DMDIR | (u32int)(st.st_mode & 0777);
    else dir->mode = (u32int)(st.st_mode & 0777);
    dir->length = (vlong)st.st_size;
    dir->mtime = (vlong)st.st_mtime;
    return dir;
}

long dirreadall(int fd, Dir **dirs)
{
    int copy = dup(fd);
    DIR *dp;
    struct dirent *entry;
    Dir *list = nil;
    long count = 0, capacity = 0;
    *dirs = nil;
    if (copy < 0) return -1;
    dp = fdopendir(copy);
    if (!dp) { close(copy); return -1; }
    while ((entry = readdir(dp)) != nil) {
        struct stat st;
        Dir *dir;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (fstatat(copy, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
            strlen(entry->d_name) >= sizeof(list->namebuf)) continue;
        if (count == capacity) {
            Dir *grown;
            capacity = capacity ? capacity * 2 : 16;
            grown = (Dir *)realloc(list, (size_t)capacity * sizeof(*list));
            if (!grown) { free(list); closedir(dp); return -1; }
            list = grown;
        }
        dir = &list[count++];
        memset(dir, 0, sizeof(*dir));
        strcpy(dir->namebuf, entry->d_name);
        dir->mode = S_ISDIR(st.st_mode) ? (DMDIR | (u32int)(st.st_mode & 0777))
                                        : (u32int)(st.st_mode & 0777);
        dir->length = (vlong)st.st_size;
        dir->mtime = (vlong)st.st_mtime;
    }
    closedir(dp);
    /* the names point into each element, so fix them after the last realloc */
    {
        long i;
        for (i = 0; i < count; i++) list[i].name = list[i].namebuf;
    }
    *dirs = list;
    return count;
}

void nulldir(Dir *dir)
{
    memset(dir, 0, sizeof(*dir));
}

int dirwstat(const char *path, const Dir *dir)
{
    int rc = 0;
    if (dir->name != nil && dir->name[0] != '\0') {
        /* rename within the directory of path, as Plan 9 does */
        char target[4096];
        const char *slash = strrchr(path, '/');
        size_t prefix = slash ? (size_t)(slash - path + 1) : 0;
        if (strchr(dir->name, '/') != nil ||
            prefix + strlen(dir->name) >= sizeof(target)) return -1;
        memcpy(target, path, prefix);
        strcpy(target + prefix, dir->name);
        if (rename(path, target) != 0) return -1;
        path = target;
    }
    if (dir->mode != 0) {
        struct stat st;
        if (stat(path, &st) != 0) return -1;
        /* Match Plan 9's requirement that a mode update retain the object's
         * directory type bit. */
        if (!!(dir->mode & DMDIR) != !!S_ISDIR(st.st_mode)) return -1;
        rc = chmod(path, dir->mode & 0777);
    }
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

/* Plan 9 formats unsigned values as %ud, %lud and %llud; the host's printf
 * spells them %u, %lu and %llu. */
static void host_format(char *out, size_t size, const char *fmt)
{
    size_t n = 0;
    while (*fmt != '\0' && n + 2 < size) {
        if (*fmt != '%') { out[n++] = *fmt++; continue; }
        out[n++] = *fmt++;
        if (*fmt == '%') { out[n++] = *fmt++; continue; }
        while (*fmt != '\0' && strchr("#-+ 0123456789.*lh", *fmt) != nil &&
               n + 2 < size)
            out[n++] = *fmt++;
        if (*fmt == 'u' && fmt[1] == 'd') {
            out[n++] = 'u';
            fmt += 2;
        } else if (*fmt != '\0') {
            out[n++] = *fmt++;
        }
    }
    out[n] = '\0';
}

static int vout(FILE *fp, const char *fmt, va_list ap)
{
    char host[512];
    host_format(host, sizeof(host), fmt);
    return vfprintf(fp, host, ap);
}

int vsnprint(char *buf, int size, const char *fmt, va_list ap)
{
    char host[512];
    host_format(host, sizeof(host), fmt);
    return vsnprintf(buf, (size_t)size, host, ap);
}

char *bp_shim_getwd(char *buf, int nbuf)
{
    return getcwd(buf, (size_t)nbuf);
}

int fd2path(int fd, char *buf, int nbuf)
{
    const char *path = isatty(fd) ? "/dev/cons" : "/tmp/file";
    if ((int)strlen(path) >= nbuf) return -1;
    strcpy(buf, path);
    return 0;
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
    va_start(ap, fmt); rc = vsnprint(buf, (int)size, fmt, ap); va_end(ap);
    return rc;
}

int sprint(char *buf, const char *fmt, ...)
{
    va_list ap;
    int rc;
    va_start(ap, fmt); rc = vsnprint(buf, 0x7fffffff, fmt, ap); va_end(ap);
    return rc;
}

void exits(const char *status)
{
    exit(status && status[0] ? 1 : 0);
}
