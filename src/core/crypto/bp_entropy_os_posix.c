/*
 * bp_entropy_os_posix.c -- the OS random source on Linux (getrandom) and other
 * POSIX systems (/dev/urandom).  Platform file; not strict C89.
 *
 * getrandom: flags 0, retried on EINTR and on short reads; /dev/urandom only when
 * the system call does not exist (ENOSYS) or the header is missing.  Phase 1
 * runs this on Linux only; macOS and the BSDs get their own branch in a later
 * phase (arc4random_buf / getentropy) after being run there.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#include "bp_entropy.h"

static int urandom_read(bp_uint8 *buf, bp_uint32 n)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    while (n > 0) {
        ssize_t r = read(fd, buf, (size_t)n);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (r == 0) { close(fd); return -1; }
        buf += r;
        n -= (bp_uint32)r;
    }
    close(fd);
    return 0;
}

int bp_entropy_os_read(bp_uint8 *buf, bp_uint32 n)
{
#if defined(__linux__)
    while (n > 0) {
        ssize_t r = getrandom(buf, (size_t)n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSYS) return urandom_read(buf, n);
            return -1;
        }
        buf += r;
        n -= (bp_uint32)r;
    }
    return 0;
#else
    return urandom_read(buf, n);
#endif
}
