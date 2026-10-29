#define _GNU_SOURCE
#include "x68k/dos.h"

#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/xattr.h>

#define HOST_FA_RDONLY 0x01
#define HOST_FA_HIDDEN 0x02
#define HOST_FA_SYSTEM 0x04
#define HOST_FA_SUBDIR 0x10
#define HOST_FA_ARCH   0x20

static unsigned char host_attributes(const char *path, const char *name,
                                    const struct stat *st)
{
    unsigned char attr = 0;
    unsigned char stored;
    ssize_t length;
    const char *dot;
    if (S_ISDIR(st->st_mode)) attr |= HOST_FA_SUBDIR;
    else attr |= HOST_FA_ARCH;
    if (!(st->st_mode & S_IWUSR)) attr |= HOST_FA_RDONLY;
    if (name[0] == '.' && name[1] != '\0') attr |= HOST_FA_HIDDEN;
    dot = strrchr(name, '.');
    if (dot && strcasecmp(dot, ".SYS") == 0) attr |= HOST_FA_SYSTEM;
    length = getxattr(path, "user.bp_x68k_attr", &stored, sizeof(stored));
    if (length == 1) attr = stored;
    return attr;
}

static void host_dos_time(time_t value, unsigned short *date,
                          unsigned short *time_value)
{
    struct tm *parts = gmtime(&value);
    if (!parts || parts->tm_year < 80 || parts->tm_year > 207) {
        *date = 0;
        *time_value = 0;
        return;
    }
    *date = (unsigned short)(((unsigned)(parts->tm_year - 80) << 9) |
                             ((unsigned)(parts->tm_mon + 1) << 5) |
                             (unsigned)parts->tm_mday);
    *time_value = (unsigned short)(((unsigned)parts->tm_hour << 11) |
                                   ((unsigned)parts->tm_min << 5) |
                                   ((unsigned)parts->tm_sec / 2U));
}

static int host_dos_epoch(unsigned date, unsigned time_value, time_t *result)
{
    struct tm parts;
    unsigned year = 1980U + ((date >> 9) & 127U);
    memset(&parts, 0, sizeof(parts));
    parts.tm_year = (int)year - 1900;
    parts.tm_mon = (int)((date >> 5) & 15U) - 1;
    parts.tm_mday = (int)(date & 31U);
    parts.tm_hour = (int)((time_value >> 11) & 31U);
    parts.tm_min = (int)((time_value >> 5) & 63U);
    parts.tm_sec = (int)(time_value & 31U) * 2;
    parts.tm_isdst = 0;
    *result = timegm(&parts);
    return *result == (time_t)-1 ? -1 : 0;
}

static void host_path(char *dst, size_t size, const char *src)
{
    size_t i;
    if (strlen(src) >= size) {
        if (size) dst[0] = '\0';
        return;
    }
    strcpy(dst, src);
    for (i = 0; dst[i]; i++) {
        unsigned char lead = (unsigned char)dst[i];
        unsigned char trail = (unsigned char)dst[i + 1];
        if (((lead >= 0x81 && lead <= 0x9F) ||
             (lead >= 0xE0 && lead <= 0xEF)) &&
            ((trail >= 0x40 && trail <= 0x7E) ||
             (trail >= 0x80 && trail <= 0xFC))) {
            i++;
            continue;
        }
        if (dst[i] == '\\') dst[i] = '/';
    }
}

static long next_entry(struct dos_filbuf *fb)
{
    DIR *dir = (DIR *)fb->search_dir;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        char path[sizeof(fb->search_path) + sizeof(fb->name) + 2];
        struct stat st;
        if (strlen(de->d_name) >= sizeof(fb->name)) continue;
        snprintf(path, sizeof(path), "%s/%s", fb->search_path, de->d_name);
        if (stat(path, &st) != 0) continue;
        strcpy(fb->name, de->d_name);
        fb->atr = host_attributes(path, de->d_name, &st);
        fb->filelen = (long)st.st_size;
        host_dos_time(st.st_mtime, &fb->date, &fb->time);
        return 0;
    }
    closedir(dir);
    fb->search_dir = NULL;
    return -1;
}

long _dos_files(struct dos_filbuf *fb, const char *path, int attr)
{
    struct stat st;
    (void)attr;
    memset(fb, 0, sizeof(*fb));
    if (strstr(path, "\\*.*") != NULL) {
        char pattern[sizeof(fb->search_path) + 8];
        char *wild;
        host_path(pattern, sizeof(pattern), path);
        wild = strrchr(pattern, '/');
        if (!wild) return -1;
        *wild = '\0';
        if (strlen(pattern) >= sizeof(fb->search_path)) return -1;
        strcpy(fb->search_path, pattern);
        fb->search_dir = (void *)opendir(pattern[0] ? pattern : "/");
        if (!fb->search_dir) return -1;
        return next_entry(fb);
    }
    {
        char native_path[sizeof(fb->search_path)];
        const char *name;
        host_path(native_path, sizeof(native_path), path);
        if (!native_path[0] || stat(native_path, &st) != 0) return -1;
        name = strrchr(native_path, '/');
        name = name ? name + 1 : native_path;
        if (strlen(name) >= sizeof(fb->name)) return -1;
        strcpy(fb->name, name);
        fb->atr = host_attributes(native_path, name, &st);
        fb->filelen = (long)st.st_size;
        host_dos_time(st.st_mtime, &fb->date, &fb->time);
        return 0;
    }
}

long _dos_nfiles(struct dos_filbuf *fb)
{
    if (!fb->search_dir) return -1;
    return next_entry(fb);
}

FILE *x68k_host_fopen(const char *path, const char *mode)
{
    char native_path[PATH_MAX];
    const char *fail_edit = getenv("BP_X68K_FAIL_STAGE_EDIT");
    host_path(native_path, sizeof(native_path), path);
    if (!native_path[0]) return NULL;
    if (fail_edit && strchr(mode, '+') != NULL &&
        strstr(path, "BP") != NULL && strstr(path, ".TMP") != NULL) {
        errno = EIO;
        return NULL;
    }
    return fopen(native_path, mode);
}

int x68k_host_remove(const char *path)
{
    char native_path[PATH_MAX];
    host_path(native_path, sizeof(native_path), path);
    if (!native_path[0]) return -1;
    return remove(native_path);
}

int x68k_host_rename(const char *old_path, const char *new_path)
{
    char native_old[PATH_MAX], native_new[PATH_MAX];
    const char *fail_publish = getenv("BP_X68K_FAIL_STAGE_PUBLISH");
    if (fail_publish && strstr(old_path, "BP") != NULL &&
        strstr(old_path, ".TMP") != NULL) {
        errno = EIO;
        return -1;
    }
    host_path(native_old, sizeof(native_old), old_path);
    host_path(native_new, sizeof(native_new), new_path);
    if (!native_old[0] || !native_new[0]) return -1;
    return rename(native_old, native_new);
}

int _dos_mkdir(const char *path)
{
    char native_path[PATH_MAX];
    host_path(native_path, sizeof(native_path), path);
    if (!native_path[0]) return -1;
    return mkdir(native_path, 0777);
}

int _dos_filedate(int fd, int datetime)
{
    struct timespec times[2];
    time_t value;
    struct stat st;
    if (datetime == 0) return -1;
    if (host_dos_epoch(((unsigned long)datetime >> 16) & 0xFFFFU,
                       (unsigned long)datetime & 0xFFFFU, &value) != 0 ||
        fstat(fd, &st) != 0) return -1;
    times[0].tv_sec = st.st_atime;
    times[0].tv_nsec = 0;
    times[1].tv_sec = value;
    times[1].tv_nsec = 0;
    return futimens(fd, times);
}

int _dos_chmod(const char *path, int attributes)
{
    char native_path[PATH_MAX];
    struct stat st;
    mode_t permissions;
    unsigned char value;
    host_path(native_path, sizeof(native_path), path);
    if (!native_path[0] || stat(native_path, &st) != 0) return -1;
    if (attributes == -1)
        return (int)host_attributes(native_path, strrchr(native_path, '/') ?
            strrchr(native_path, '/') + 1 : native_path, &st);
    value = (unsigned char)attributes;
    if (setxattr(native_path, "user.bp_x68k_attr", &value, sizeof(value), 0) != 0)
        return -1;
    permissions = st.st_mode;
    if (value & HOST_FA_RDONLY) permissions &= (mode_t)~0222;
    else permissions |= 0200;
    return chmod(native_path, permissions);
}
