#ifndef BP_AMIGA_META_H
#define BP_AMIGA_META_H

#include <stddef.h>
#include <string.h>

/* Amiga native metadata payload: protection (LE32), comment length, bytes. */
static int bp_amiga_meta_decode(const unsigned char *data, size_t len,
                                unsigned long *protection, char comment[80])
{
    size_t comment_len;
    size_t i;
    unsigned long decoded;

    if (data == NULL || protection == NULL || comment == NULL || len < 5)
        return -1;
    comment_len = data[4];
    if (comment_len > 79 || len != 5U + comment_len) return -1;
    for (i = 0; i < comment_len; ++i)
        if (data[5U + i] == 0) return -1;

    decoded = (unsigned long)data[0] |
              ((unsigned long)data[1] << 8) |
              ((unsigned long)data[2] << 16) |
              ((unsigned long)data[3] << 24);
    if (comment_len != 0) memcpy(comment, data + 5, comment_len);
    comment[comment_len] = '\0';
    *protection = decoded;
    return 0;
}

#endif
