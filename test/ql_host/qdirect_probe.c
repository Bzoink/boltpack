#include <stdio.h>
#include <qdos.h>

#define QDIRECT_SIZE 64
#define NAME_MAXIMUM 36
#define RECORD_LIMIT 16

static unsigned int be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

static void describe(FILE *out, unsigned int index,
                     const unsigned char *record)
{
    unsigned int name_length = be16(record + 14);
    unsigned int i;
    if (name_length > NAME_MAXIMUM) name_length = NAME_MAXIMUM;
    fprintf(out, "record=%u length=%02x%02x%02x%02x type=%u name_length=%u name=",
            index, record[0], record[1], record[2], record[3],
            (unsigned int)record[5], name_length);
    for (i = 0; i < name_length; i++) {
        unsigned char c = record[16 + i];
        fputc(c >= 32 && c < 127 ? c : '.', out);
    }
    fputc('\n', out);
}

int main(void)
{
    FILE *out;
    FILE *directory;
    FILE *nested_file;
    chanid_t channel;
    unsigned char record[QDIRECT_SIZE];
    unsigned int i;
    size_t count;
    int rc;

    out = fopen("win1_QRES", "w");
    if (out == NULL) return 2;

    nested_file = fopen("win1_QDTEST_ONEFILE", "rb");
    if (nested_file == NULL) {
        fputs("nested_file_open=failed\n", out);
    } else {
        count = fread(record, 1, sizeof(record), nested_file);
        fprintf(out, "nested_file_read=%lu first_byte=%u\n",
                (unsigned long)count, count != 0 ? (unsigned int)record[0] : 0U);
        fclose(nested_file);
    }

    directory = fopen("win1_QDTEST_", "r");
    if (directory == NULL) {
        fputs("stdio_open=failed\n", out);
    } else {
        for (i = 0; i < RECORD_LIMIT; i++) {
            count = fread(record, 1, sizeof(record), directory);
            fprintf(out, "stdio_read=%lu eof=%d error=%d\n",
                    (unsigned long)count, feof(directory), ferror(directory));
            if (count != sizeof(record)) break;
            describe(out, i + 1, record);
            if (be16(record + 14) == 0) break;
        }
        fclose(directory);
    }

    channel = io_open("win1_QDTEST_", 0);
    fprintf(out, "raw_open=%ld\n", (long)channel);
    if (channel >= 0) {
        for (i = 0; i < RECORD_LIMIT; i++) {
            rc = io_fstrg(channel, -1, record, QDIRECT_SIZE);
            fprintf(out, "raw_read=%d\n", rc);
            if (rc != 0) break;
            describe(out, i + 1, record);
            if (be16(record + 14) == 0) break;
        }
        (void)io_close(channel);
    }
    fclose(out);
    return 0;
}
