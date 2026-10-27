#include "x68k/dos.h"

#include <sys/stat.h>
#include <string.h>
#include <errno.h>

long _dos_files(struct dos_filbuf *fb, const char *path, int attr)
{
    struct stat st;
    const char *name;
    (void)attr;
    if (stat(path, &st) != 0) return -1;
    memset(fb, 0, sizeof(*fb));
    name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (strlen(name) >= sizeof(fb->name)) return -1;
    strcpy(fb->name, name);
    fb->atr = (st.st_mode & S_IFDIR) ? 0x10 : 0x20;
    fb->filelen = (long)st.st_size;
    return 0;
}

long _dos_nfiles(struct dos_filbuf *fb)
{
    (void)fb;
    return -1;
}

int _dos_mkdir(const char *path)
{
    return mkdir(path, 0777);
}
