/*
 * legacy_at.h -- descriptor-anchored emulation of the *at() calls
 *
 * Mac OS X before 10.10 has no openat/fstatat/mkdirat/unlinkat/symlinkat/
 * renameat/linkat, and before 10.13 no futimens/utimensat.  The macOS CLI's
 * extraction code resolves archive-controlled paths one component at a time
 * relative to directory descriptors it already holds.  This header keeps that
 * safety property on older systems: each emulated call does fchdir(dirfd),
 * performs the plain call on a single path component, and returns to the
 * previous working directory.  Because the directory is named by a held
 * descriptor and the name is one component, a concurrent rename of a parent
 * path or a symlink swapped into the path cannot redirect the operation.
 *
 * Rules enforced here instead of trusted:
 *   - the name must be one component: no '/', not "." and not ".."; anything
 *     else fails with EINVAL, so a future caller cannot silently lose the
 *     guarantee;
 *   - renameat/linkat need the same directory descriptor on both sides
 *     (ENOSYS otherwise);
 *   - the process is single threaded (the working directory is process-wide).
 *
 * Include this header AFTER the system headers and BEFORE the code that uses
 * the *at names; it renames them with macros.  Selected automatically on
 * Apple compilers targeting a system before 10.10; define BP_FORCE_LEGACY_AT
 * to use it on any host (the macOS host tests do, to run it on Linux).
 * Define BP_NO_LEGACY_AT to disable the automatic selection.
 *
 * Mac OS X 10.5 is the oldest system this has been written for.  It relies on
 * fchdir, lutimes and futimes, which that system provides.  It has been run
 * only on a Linux host (see test/check_macosx_xattr_roundtrip.py), not on a
 * Mac.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#ifndef BP_LEGACY_AT_H
#define BP_LEGACY_AT_H

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef AT_FDCWD
#define AT_FDCWD (-2)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x20
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x80
#endif
#ifndef UTIME_NOW
#define UTIME_NOW  ((long)((1L << 30) - 1L))
#endif
#ifndef UTIME_OMIT
#define UTIME_OMIT ((long)((1L << 30) - 2L))
#endif
/* Older systems have no O_DIRECTORY.  Without it, opening a regular file as a
 * "directory" succeeds, but every later fchdir() on it fails with ENOTDIR, so
 * nothing is ever resolved through it. */
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

/* One path component only; see the file comment. */
static int bp_legacy_name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        errno = EINVAL;
        return 0;
    }
    return 1;
}

/* Make dirfd the working directory.  *saved receives a descriptor for the
 * previous one (or -1 for AT_FDCWD) to pass to bp_legacy_leave(). */
static int bp_legacy_enter(int dirfd, int *saved)
{
    int cwd, err;
    *saved = -1;
    if (dirfd == AT_FDCWD) return 0;
    cwd = open(".", O_RDONLY);
    if (cwd < 0) return -1;
    if (fchdir(dirfd) != 0) {
        err = errno;
        close(cwd);
        errno = err;
        return -1;
    }
    *saved = cwd;
    return 0;
}

/* Return to the previous working directory; errno is preserved. */
static void bp_legacy_leave(int saved)
{
    int err = errno;
    if (saved >= 0) {
        if (fchdir(saved) != 0) {
            /* Relative paths would now resolve somewhere unexpected; say so
             * instead of failing later with an unrelated message. */
            err = errno != 0 ? errno : EIO;
            fprintf(stderr,
                    "boltpack: cannot return to the working directory: %s\n",
                    strerror(err));
        }
        close(saved);
    }
    errno = err;
}

static int bp_legacy_openat(int dirfd, const char *name, int flags, ...)
{
    int saved, fd, mode = 0;
    if ((flags & O_CREAT) != 0) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!bp_legacy_name_ok(name)) return -1;
    if (bp_legacy_enter(dirfd, &saved) != 0) return -1;
    fd = open(name, flags, (mode_t)mode);
    bp_legacy_leave(saved);
    return fd;
}

static int bp_legacy_mkdirat(int dirfd, const char *name, mode_t mode)
{
    int saved, rc;
    if (!bp_legacy_name_ok(name)) return -1;
    if (bp_legacy_enter(dirfd, &saved) != 0) return -1;
    rc = mkdir(name, mode);
    bp_legacy_leave(saved);
    return rc;
}

static int bp_legacy_unlinkat(int dirfd, const char *name, int flags)
{
    int saved, rc;
    if (!bp_legacy_name_ok(name)) return -1;
    if (bp_legacy_enter(dirfd, &saved) != 0) return -1;
    rc = (flags & AT_REMOVEDIR) != 0 ? rmdir(name) : unlink(name);
    bp_legacy_leave(saved);
    return rc;
}

static int bp_legacy_fstatat(int dirfd, const char *name, struct stat *st,
                             int flags)
{
    int saved, rc;
    if (!bp_legacy_name_ok(name)) return -1;
    if (bp_legacy_enter(dirfd, &saved) != 0) return -1;
    rc = (flags & AT_SYMLINK_NOFOLLOW) != 0 ? lstat(name, st) : stat(name, st);
    bp_legacy_leave(saved);
    return rc;
}

static int bp_legacy_symlinkat(const char *target, int dirfd, const char *name)
{
    int saved, rc;
    if (!bp_legacy_name_ok(name)) return -1;
    if (bp_legacy_enter(dirfd, &saved) != 0) return -1;
    rc = symlink(target, name);
    bp_legacy_leave(saved);
    return rc;
}

static int bp_legacy_renameat(int olddirfd, const char *oldname, int newdirfd,
                              const char *newname)
{
    int saved, rc;
    if (!bp_legacy_name_ok(oldname) || !bp_legacy_name_ok(newname)) return -1;
    if (olddirfd != newdirfd) { errno = ENOSYS; return -1; }
    if (bp_legacy_enter(olddirfd, &saved) != 0) return -1;
    rc = rename(oldname, newname);
    bp_legacy_leave(saved);
    return rc;
}

static int bp_legacy_linkat(int olddirfd, const char *oldname, int newdirfd,
                            const char *newname, int flags)
{
    int saved, rc;
    if (flags != 0) { errno = ENOSYS; return -1; }
    if (!bp_legacy_name_ok(oldname) || !bp_legacy_name_ok(newname)) return -1;
    if (olddirfd != newdirfd) { errno = ENOSYS; return -1; }
    if (bp_legacy_enter(olddirfd, &saved) != 0) return -1;
    rc = link(oldname, newname);
    bp_legacy_leave(saved);
    return rc;
}

/* Combine requested timespecs with the current times of the target for any
 * UTIME_OMIT slot, and convert to the microsecond pairs lutimes/futimes use.
 * cur_atime/cur_mtime are the target's present times. */
static int bp_legacy_times(const struct timespec *times, time_t cur_atime,
                           time_t cur_mtime, struct timeval tv[2])
{
    struct timeval now;
    int i;
    gettimeofday(&now, NULL);
    for (i = 0; i < 2; i++) {
        if (times == NULL || times[i].tv_nsec == UTIME_NOW) {
            tv[i] = now;
        } else if (times[i].tv_nsec == UTIME_OMIT) {
            tv[i].tv_sec = i == 0 ? cur_atime : cur_mtime;
            tv[i].tv_usec = 0;
        } else {
            tv[i].tv_sec = times[i].tv_sec;
            tv[i].tv_usec = (int)(times[i].tv_nsec / 1000L);
        }
    }
    return 0;
}

static int bp_legacy_futimens(int fd, const struct timespec times[2])
{
    struct stat st;
    struct timeval tv[2];
    if (fstat(fd, &st) != 0) return -1;
    bp_legacy_times(times, st.st_atime, st.st_mtime, tv);
    return futimes(fd, tv);
}

static int bp_legacy_utimensat(int dirfd, const char *name,
                               const struct timespec times[2], int flags)
{
    struct stat st;
    struct timeval tv[2];
    int saved, rc;
    if (!bp_legacy_name_ok(name)) return -1;
    if (bp_legacy_enter(dirfd, &saved) != 0) return -1;
    if ((flags & AT_SYMLINK_NOFOLLOW) != 0) {
        rc = lstat(name, &st);
        if (rc == 0) {
            bp_legacy_times(times, st.st_atime, st.st_mtime, tv);
            rc = lutimes(name, tv);
        }
    } else {
        rc = stat(name, &st);
        if (rc == 0) {
            bp_legacy_times(times, st.st_atime, st.st_mtime, tv);
            rc = utimes(name, tv);
        }
    }
    bp_legacy_leave(saved);
    return rc;
}

#define openat      bp_legacy_openat
#define mkdirat     bp_legacy_mkdirat
#define unlinkat    bp_legacy_unlinkat
#define fstatat     bp_legacy_fstatat
#define symlinkat   bp_legacy_symlinkat
#define renameat    bp_legacy_renameat
#define linkat      bp_legacy_linkat
#define futimens    bp_legacy_futimens
#define utimensat   bp_legacy_utimensat

#endif /* BP_LEGACY_AT_H */
