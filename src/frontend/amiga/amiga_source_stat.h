#ifndef BP_AMIGA_SOURCE_STAT_H
#define BP_AMIGA_SOURCE_STAT_H

#include <sys/stat.h>

static int bp_amiga_stat_same_file(const struct stat *a,
                                  const struct stat *b)
{
    return a != NULL && b != NULL &&
           S_ISREG(a->st_mode) && S_ISREG(b->st_mode) &&
           a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size && a->st_mtime == b->st_mtime &&
           a->st_ctime == b->st_ctime;
}

static int bp_amiga_stat_same_directory(const struct stat *a,
                                        const struct stat *b)
{
    return a != NULL && b != NULL &&
           S_ISDIR(a->st_mode) && S_ISDIR(b->st_mode) &&
           a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime;
}

#endif
