/* POSIX file access for boltpack-recover.  pread() keeps reads independent of
 * a shared file position, so later phases can read from several threads. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "bpr.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

struct bpr_file {
    int      fd;
    uint64_t size;
};

bpr_file *bpr_file_open(const char *path)
{
    struct stat st;
    bpr_file *f;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return NULL;
    }
    f = (bpr_file *)malloc(sizeof(*f));
    if (f == NULL) {
        close(fd);
        return NULL;
    }
    f->fd = fd;
    f->size = (uint64_t)st.st_size;
    return f;
}

void bpr_file_close(bpr_file *f)
{
    if (f == NULL) return;
    close(f->fd);
    free(f);
}

uint64_t bpr_file_size(const bpr_file *f) { return f->size; }

int bpr_file_read(const bpr_file *f, uint64_t off, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    if (off > f->size || n > f->size - off) return -1;
    while (n > 0) {
        ssize_t got = pread(f->fd, p, n, (off_t)off);
        if (got <= 0) return -1;
        p += got;
        off += (uint64_t)got;
        n -= (size_t)got;
    }
    return 0;
}

/* ---- output --------------------------------------------------------- */

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>

#include "bp_types.h"

struct bpr_out { int fd; };

static char *join(const char *root, const char *name)
{
    size_t a = strlen(root), b = strlen(name);
    char *p = (char *)malloc(a + b + 2);
    if (p == NULL) return NULL;
    memcpy(p, root, a);
    if (a > 0 && root[a - 1] != '/') p[a++] = '/';
    memcpy(p + a, name, b + 1);
    return p;
}

int bpr_fs_prepare_root(const char *dir)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    if (lstat(dir, &st) != 0) {
        if (errno != ENOENT) return -1;
        return mkdir(dir, 0700) == 0 ? 0 : -1;
    }
    if (!S_ISDIR(st.st_mode)) return -1;
    d = opendir(dir);
    if (d == NULL) return -1;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return 0;
}

/* Make sure root/prefix is a real directory (not a symlink), creating it. */
static int ensure_dir(const char *path)
{
    struct stat st;
    if (lstat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    if (errno != ENOENT) return -1;
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST && lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    return -1;
}

int bpr_fs_make_parents(const char *root, const char *name)
{
    char *full = join(root, name);
    size_t i, start;
    int rc = 0;
    if (full == NULL) return -1;
    start = strlen(full) - strlen(name);
    for (i = start; full[i] != '\0'; i++) {
        if (full[i] == '/' && i > start) {
            full[i] = '\0';
            if (ensure_dir(full) != 0) { rc = -1; break; }
            full[i] = '/';
        }
    }
    free(full);
    return rc;
}

int bpr_fs_mkdir(const char *root, const char *name)
{
    char *full = join(root, name);
    size_t n;
    int rc;
    if (full == NULL) return -1;
    n = strlen(full);
    while (n > 0 && full[n - 1] == '/') full[--n] = '\0';
    if (bpr_fs_make_parents(root, name) != 0) { free(full); return -1; }
    rc = ensure_dir(full);
    free(full);
    return rc;
}

bpr_out *bpr_out_create(const char *root, const char *name)
{
    char *full = join(root, name);
    bpr_out *o;
    int fd;
    if (full == NULL) return NULL;
    fd = open(full, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
    free(full);
    if (fd < 0) return NULL;
    o = (bpr_out *)malloc(sizeof(*o));
    if (o == NULL) { close(fd); return NULL; }
    o->fd = fd;
    return o;
}

int bpr_out_write(bpr_out *o, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        ssize_t w = write(o->fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int bpr_out_close(bpr_out *o)
{
    int rc = close(o->fd);
    free(o);
    return rc == 0 ? 0 : -1;
}

void bpr_out_abort(bpr_out *o)
{
    close(o->fd);
    free(o);
}

int bpr_fs_restore_meta(const char *root, const char *name, uint16_t flags,
                        uint64_t mtime, int is_symlink)
{
    char *full = join(root, name);
    size_t n;
    int rc = 0;
    if (full == NULL) return -1;
    n = strlen(full);
    while (n > 1 && full[n - 1] == '/') full[--n] = '\0';
    if ((flags & BP_ENTRY_FLAG_MODE_VALID) != 0 && !is_symlink) {
        mode_t mode = (mode_t)(flags & BP_ENTRY_FLAG_MODE_MASK);
        if ((mode & (mode_t)06000) != 0) {
            mode &= (mode_t)~06000;     /* never restore set-id bits */
            rc = 1;
        }
        if (chmod(full, mode) != 0) rc = -1;
    }
    if ((flags & BP_ENTRY_FLAG_MTIME_VALID) != 0) {
        struct timeval tv[2];
        tv[0].tv_sec = tv[1].tv_sec = (time_t)mtime;
        tv[0].tv_usec = tv[1].tv_usec = 0;
        if ((uint64_t)tv[0].tv_sec != mtime) rc = rc < 0 ? rc : 1;
        else if (is_symlink ? lutimes(full, tv) != 0 : utimes(full, tv) != 0)
            rc = -1;
    }
    free(full);
    return rc;
}

int bpr_fs_symlink(const char *root, const char *name, const char *target)
{
    char *full = join(root, name);
    int rc;
    if (full == NULL) return -1;
    rc = symlink(target, full);
    free(full);
    return rc == 0 ? 0 : -1;
}

int bpr_fs_rename(const char *root, const char *from, const char *to)
{
    char *a = join(root, from), *b = join(root, to);
    int rc = -1;
    if (a != NULL && b != NULL) rc = rename(a, b);
    free(a);
    free(b);
    return rc == 0 ? 0 : -1;
}

int bpr_fs_unlink(const char *root, const char *name)
{
    char *full = join(root, name);
    int rc = -1;
    if (full != NULL) rc = unlink(full);
    free(full);
    return rc == 0 ? 0 : -1;
}

int bpr_fs_exists(const char *root, const char *name)
{
    struct stat st;
    char *full = join(root, name);
    int rc = 0;
    if (full == NULL) return 0;
    if (lstat(full, &st) == 0) rc = 1;
    free(full);
    return rc;
}

/* ---- output files (rebuild) -------------------------------------------- */

struct bpr_wfile {
    int fd;
};

bpr_wfile *bpr_wfile_create(const char *path)
{
    bpr_wfile *f;
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
    if (fd < 0) return NULL;
    f = (bpr_wfile *)malloc(sizeof(*f));
    if (f == NULL) {
        close(fd);
        unlink(path);
        return NULL;
    }
    f->fd = fd;
    return f;
}

long bpr_wfile_read(bpr_wfile *f, void *buf, size_t n)
{
    ssize_t r;
    size_t got = 0;
    while (got < n) {
        r = read(f->fd, (char *)buf + got, n - got);
        if (r < 0) return -1;
        if (r == 0) break;
        got += (size_t)r;
    }
    return (long)got;
}

long bpr_wfile_write(bpr_wfile *f, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(f->fd, (const char *)buf + done, n - done);
        if (w <= 0) return (long)done;
        done += (size_t)w;
    }
    return (long)done;
}

int bpr_wfile_seek(bpr_wfile *f, int64_t off, int origin)
{
    return lseek(f->fd, (off_t)off, origin == 0 ? SEEK_SET :
                 origin == 1 ? SEEK_CUR : SEEK_END) < 0 ? -1 : 0;
}

int64_t bpr_wfile_tell(bpr_wfile *f)
{
    return (int64_t)lseek(f->fd, 0, SEEK_CUR);
}

int bpr_wfile_close(bpr_wfile *f)
{
    int rc;
    if (f == NULL) return 0;
    rc = fsync(f->fd) != 0 ? -1 : 0;        /* the volume is on the medium */
    if (close(f->fd) != 0) rc = -1;
    free(f);
    return rc;
}

int bpr_path_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

/* Rename without ever replacing a file.  link() does that atomically but not
 * every filesystem has hard links (FAT and exFAT, many network shares, which
 * is where volume sets usually go); there it fails with something other than
 * "exists", and an existence check followed by rename() is used, as the CLI
 * does.  BPR_FORCE_LINK_FAIL makes link() fail so the fallback can be tested
 * without such a filesystem. */
int bpr_path_rename_new(const char *from, const char *to)
{
    struct stat st;
#ifdef BPR_FORCE_LINK_FAIL
    errno = EPERM;
    if (1) {
#else
    if (link(from, to) != 0) {
#endif
        if (errno == EEXIST) return -1;
        if (lstat(to, &st) == 0) return -1;
        return rename(from, to);
    }
    return unlink(from);
}

int bpr_path_remove(const char *path)
{
    return unlink(path);
}
