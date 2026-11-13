/* Hatari test launcher: run one BoltPack command per line and record status. */
#include <stdio.h>
#include <string.h>
#include <mint/osbind.h>

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
    char executable[128];

    if (!input || !output) return 2;
    while (fgets(line, sizeof(line), input)) {
        n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;
        if (n == 0 || n > 125) return 3;
        strcpy(executable, "C:\\BOLTPACK.TOS");
        if (line[0] == '@') {
            char *space = strchr(line, ' ');
            if (!space || (size_t)(space - line) >= sizeof(executable)) return 3;
            memcpy(executable, line + 1, (size_t)(space - line - 1));
            executable[space - line - 1] = '\0';
            memmove(line, space + 1, strlen(space + 1) + 1);
            n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;
        }
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
        result = Pexec(0, executable, tail, 0L);
        Fclose((short)guest_log);
        fprintf(output, "%ld\n", result);
        fflush(output);
    }
    fclose(input);
    fclose(output);
    output = fopen("C:\\FINISH.TXT", "w");
    if (!output) return 2;
    fputs("FINISHED\n", output);
    if (fclose(output) != 0) return 2;
    return 0;
}
