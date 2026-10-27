#ifndef BOLTPACK_TEST_X68K_DOS_H
#define BOLTPACK_TEST_X68K_DOS_H

struct dos_filbuf {
    char name[256];
    unsigned char atr;
    unsigned short time;
    unsigned short date;
    long filelen;
};

long _dos_files(struct dos_filbuf *fb, const char *path, int attr);
long _dos_nfiles(struct dos_filbuf *fb);
int _dos_mkdir(const char *path);

#endif
