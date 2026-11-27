/* CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320), slice-by-4.
 * Independent of the core's bp_crc32 on purpose: the recovery tool must not
 * share a defect with the code that wrote the archive. */
#include "bpr.h"

static uint32_t table[4][256];
static int table_ready;

static void init_table(void)
{
    uint32_t i, k;
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        table[0][i] = c;
    }
    for (i = 0; i < 256; i++) {
        uint32_t c = table[0][i];
        for (k = 1; k < 4; k++) {
            c = table[0][c & 0xFF] ^ (c >> 8);
            table[k][i] = c;
        }
    }
    table_ready = 1;
}

uint32_t bpr_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c;
    if (!table_ready) init_table();
    c = ~crc;
    while (len >= 4) {
        c ^= bpr_le32(p);
        c = table[3][c & 0xFF] ^ table[2][(c >> 8) & 0xFF] ^
            table[1][(c >> 16) & 0xFF] ^ table[0][c >> 24];
        p += 4;
        len -= 4;
    }
    while (len--) c = table[0][(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}
