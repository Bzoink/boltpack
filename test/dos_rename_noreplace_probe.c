/* Verify the production no-replace rename helper on DOS FAT12. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../src/frontend/dos/dos_rename.h"

static int write_text(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    int ok;
    if (fp == NULL) return -1;
    ok = fwrite(text, 1, strlen(text), fp) == strlen(text);
    if (fclose(fp) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int has_text(const char *path, const char *expected)
{
    char buffer[32];
    FILE *fp = fopen(path, "rb");
    size_t count;
    if (fp == NULL) return 0;
    count = fread(buffer, 1, sizeof(buffer) - 1, fp);
    buffer[count] = '\0';
    if (ferror(fp) || fclose(fp) != 0) return 0;
    return strcmp(buffer, expected) == 0;
}

int main(void)
{
    FILE *report;
    int rename_rc, rename_errno, source_preserved, target_preserved;
    if (write_text("OUT/RNSRC.TXT", "source bytes\n") != 0 ||
        write_text("OUT/RNTGT.TXT", "existing sentinel\n") != 0)
        return 2;
    errno = 0;
    rename_rc = dos_rename_no_replace("OUT/RNSRC.TXT", "OUT/RNTGT.TXT");
    rename_errno = errno;
    source_preserved = has_text("OUT/RNSRC.TXT", "source bytes\n");
    target_preserved = has_text("OUT/RNTGT.TXT", "existing sentinel\n");
    report = fopen("RENAMER.TXT", "wb");
    if (report == NULL) return 3;
    fprintf(report, "rename_rc=%d errno=%d source_preserved=%d "
                    "target_preserved=%d\n",
            rename_rc, rename_errno, source_preserved, target_preserved);
    if (fclose(report) != 0) return 4;
    return 0;
}
