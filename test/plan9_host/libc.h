#ifndef BOLTPACK_TEST_PLAN9_LIBC_H
#define BOLTPACK_TEST_PLAN9_LIBC_H

#include "u.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define nil ((void *)0)
#define OREAD O_RDONLY
#define OWRITE O_WRONLY
#define ORDWR O_RDWR
#define DMDIR 0x80000000u

typedef struct Dir {
    char name[256];
    u32int mode;
    vlong length;
    vlong mtime;
} Dir;

int create(const char *path, int mode, unsigned int perm);
vlong seek(int fd, vlong offset, int origin);
Dir *dirstat(const char *path);
long dirreadall(int fd, Dir **dirs);
void nulldir(Dir *dir);
int dirwstat(const char *path, const Dir *dir);
void rerrstr(char *buf, unsigned int n);
int print(const char *fmt, ...);
int fprint(int fd, const char *fmt, ...);
int snprint(char *buf, size_t size, const char *fmt, ...);
int sprint(char *buf, const char *fmt, ...);
void exits(const char *status);

#endif
