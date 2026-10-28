/* Source path and Latin-1 name projection used by the Amiga GUI. */
#include <stdio.h>
#include <string.h>
#include "../../src/frontend/amiga_gui/amiga_source_name.h"

int main(void)
{
    char base[128], name[256], tiny[3];
    int failed = 0;
    const char local_name[] = "DH0:caf\xE9.txt";
    if (gui_source_parent(local_name, base, sizeof(base)) || strcmp(base, "DH0:")) failed++;
    if (gui_source_archive_name(base, local_name, BP_TYPE_DATA, name, sizeof(name)) ||
        strcmp(name, "caf\xC3\xA9.txt")) failed++;
    if (gui_source_parent("DH0:dir/file", base, sizeof(base)) || strcmp(base, "DH0:dir")) failed++;
    if (gui_source_archive_name(base, "DH0:dir/sub/file", BP_TYPE_DATA, name, sizeof(name)) ||
        strcmp(name, "sub/file")) failed++;
    if (gui_source_archive_name(base, "DH0:directory/file", BP_TYPE_DATA, name, sizeof(name)) == 0) failed++;
    if (gui_source_archive_name(base, "DH0:dir/../file", BP_TYPE_DATA, name, sizeof(name)) == 0) failed++;
    if (gui_source_archive_name(base, "DH0:dir/other:device", BP_TYPE_DATA, name, sizeof(name)) == 0) failed++;
    if (gui_source_archive_name(base, "DH0:dir/file", BP_TYPE_DATA, tiny, sizeof(tiny)) == 0) failed++;
    if (gui_source_archive_name(base, "DH0:dir/empty", BP_TYPE_DIR, name, sizeof(name)) || strcmp(name, "empty/")) failed++;
    if (gui_source_archive_name(base, "DH0:dir/file", BP_TYPE_DIR, tiny, sizeof(tiny)) == 0) failed++;
    if (gui_source_parent("/file", base, sizeof(base)) || strcmp(base, "/")) failed++;
    if (gui_source_archive_name(base, "/file", BP_TYPE_DATA, name, sizeof(name)) || strcmp(name, "file")) failed++;
    printf("Amiga GUI source names: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
