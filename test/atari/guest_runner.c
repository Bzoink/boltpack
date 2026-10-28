/* Hatari test launcher: run one BoltPack command per line and record status. */
#include <stdio.h>
#include <string.h>
#include <mint/osbind.h>

static int copy_file(const char *source, const char *destination)
{
    FILE *input = fopen(source, "rb");
    FILE *output;
    unsigned char buffer[1024];
    size_t count;
    if (!input) return -1;
    output = fopen(destination, "wb");
    if (!output) { fclose(input); return -1; }
    while ((count = fread(buffer, 1, sizeof(buffer), input)) != 0) {
        if (fwrite(buffer, 1, count, output) != count) {
            fclose(input); fclose(output); return -1;
        }
    }
    {
        int failed = ferror(input) ? 1 : 0;
        if (fclose(input) != 0) failed = 1;
        if (fclose(output) != 0) failed = 1;
        return failed ? -1 : 0;
    }
}

int main(void)
{
    char tail[128], line[125];
    FILE *input = fopen("C:\\COMMANDS.TXT", "r");
    FILE *output = fopen("C:\\RESULT.TXT", "w");
    size_t n;
    long result;
    long guest_log;
    int index = 0;
    char log_name[32];

    if (!input || !output) return 2;
    while (fgets(line, sizeof(line), input)) {
        n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;
        if (n == 0 || n > 125) return 3;
        tail[0] = (char)(n + 1);
        tail[1] = ' ';
        memcpy(tail + 2, line, n);
        tail[n + 2] = '\r';
        sprintf(log_name, "C:\\LOG%02d.TXT", index++);
        guest_log = Fcreate(log_name, 0);
        if (guest_log < 0 || Fforce(1, (short)guest_log) < 0 ||
            Fforce(2, (short)guest_log) < 0) return 2;
        Fwrite((short)guest_log, (long)n, line);
        Fwrite((short)guest_log, 2L, "\r\n");
        result = Pexec(0, "C:\\BOLTPACK.TOS", tail, 0L);
        Fclose((short)guest_log);
        fprintf(output, "%ld\n", result);
        fflush(output);
    }
    fclose(input);
    fclose(output);
    if (copy_file("A:\\OUT\\SRC.TXT", "C:\\CAPSRC.TXT") != 0 ||
        copy_file("A:\\RESTORE\\SRC.TXT", "C:\\CAPREST.TXT") != 0 ||
        copy_file("A:\\RESTORE\\NEW.TXT", "C:\\CAPNEW.TXT") != 0 ||
        copy_file("A:\\ATARI.BLT", "C:\\CAPATARI.BLT") != 0) return 4;
    return 0;
}
