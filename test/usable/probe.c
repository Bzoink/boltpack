/* C89 payload for usable round trips: the restored program needs restored data. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    static const char expected[] = "BoltPack usable application data\n";
    char data[sizeof(expected) + 8];
    FILE *fp;
    size_t count;
    if (argc != 2 && argc != 3) return 2;
    fp = fopen(argv[1], "rb");
    if (!fp) return 3;
    count = fread(data, 1, sizeof(data), fp);
    if (ferror(fp)) { fclose(fp); return 4; }
    if (fclose(fp) != 0) return 5;
    if (count != sizeof(expected) - 1 || memcmp(data, expected, count)) return 6;
    if (argc == 3) {
        static const char marker[] = "BOLTPACK_USABLE_PROGRAM_OK\n";
        fp = fopen(argv[2], "wb");
        if (!fp) return 7;
        if (fwrite(marker, 1, sizeof(marker) - 1, fp) != sizeof(marker) - 1) {
            fclose(fp); return 8;
        }
        if (fclose(fp) != 0) return 9;
    }
    puts("BOLTPACK_USABLE_PROGRAM_OK");
    return 0;
}
