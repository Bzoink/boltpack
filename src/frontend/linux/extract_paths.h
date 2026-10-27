/* Linux directory-relative extraction helpers. */
#ifndef BP_LINUX_EXTRACT_PATHS_H
#define BP_LINUX_EXTRACT_PATHS_H

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Walk archive-controlled components relative to an already-open root.
 * Never follow a symlink, including when a concurrent process replaces one. */
static int extract_open_directory(int root_fd, const char *name, int create)
{
    char path[4096];
    char *p, *component;
    int fd, next;
    if (strlen(name) >= sizeof(path)) return -1;
    strcpy(path, name);
    fd = dup(root_fd);
    if (fd < 0) return -1;
    p = path;
    while (*p != '\0') {
        while (*p == '/') p++;
        if (*p == '\0') break;
        component = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = '\0';
        if (strcmp(component, "..") == 0) { close(fd); errno = EINVAL; return -1; }
        if (strcmp(component, ".") == 0) continue;
        next = openat(fd, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(fd, component, 0755) != 0 && errno != EEXIST) {
                close(fd); return -1;
            }
            next = openat(fd, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) { close(fd); return -1; }
        close(fd);
        fd = next;
    }
    return fd;
}

static int extract_open_parent(int root_fd, const char *name, char *leaf, size_t leaf_size)
{
    char path[4096], *slash, *base;
    size_t n = strlen(name);
    if (n == 0 || n >= sizeof(path) || name[n - 1] == '/') return -1;
    memcpy(path, name, n + 1);
    slash = strrchr(path, '/');
    base = slash ? slash + 1 : path;
    if (*base == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0 ||
        strlen(base) >= leaf_size) return -1;
    strcpy(leaf, base);
    if (slash) *slash = '\0';
    else path[0] = '\0';
    return extract_open_directory(root_fd, path, 1);
}

/* renameat2 preserves the no-clobber guarantee on filesystems without hard
 * links. Older kernels/filesystems fall back to atomic link-and-unlink. */
static int extract_commit_noreplace(int parent_fd, const char *temporary,
                                    const char *destination)
{
#ifdef SYS_renameat2
    if (syscall(SYS_renameat2, parent_fd, temporary, parent_fd, destination, 1U) == 0)
        return 0; /* RENAME_NOREPLACE */
    if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) return -1;
#endif
    if (linkat(parent_fd, temporary, parent_fd, destination, 0) != 0) return -1;
    return unlinkat(parent_fd, temporary, 0);
}

#endif
