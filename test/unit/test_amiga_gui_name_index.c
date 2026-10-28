/* Amiga GUI preflight for implicit parents, aliases and bounded storage. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define BP_MAX_PATH 512
#define GUI_NAME_INDEX_MAX 1024U
#define GUI_NAME_INDEX_BYTES_MAX 32768UL
#include "../../src/frontend/amiga_gui/amiga_name_index.h"

static int add(gui_name_index_t *index, const char *name, bp_uint8 type)
{
    bp_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.name = (char *)name; entry.entry_type = type;
    return gui_preflight_name(&entry, index);
}
static void reset(gui_name_index_t *index)
{
    gui_name_index_clear(index); memset(index, 0, sizeof(*index)); index->dest = "";
}
int main(void)
{
    gui_name_index_t *index = (gui_name_index_t *)calloc(1, sizeof(*index));
    char name[220]; int i, failed = 0, refused = 0;
    if (!index) return 2;
    index->dest = "";
    if (add(index, "Foo/a", BP_TYPE_DATA) || !add(index, "foo/b", BP_TYPE_DATA)) failed++;
    reset(index);
    if (add(index, "\xC3\x89/a", BP_TYPE_DATA) ||
        !add(index, "\xC3\xA9/b", BP_TYPE_DATA)) failed++;
    reset(index);
    if (add(index, "dir/a", BP_TYPE_DATA) || add(index, "dir/b", BP_TYPE_DATA) ||
        add(index, "dir/", BP_TYPE_DIR) || !add(index, "dir/", BP_TYPE_DIR)) failed++;
    reset(index);
    if (add(index, "file", BP_TYPE_DATA) || !add(index, "file/child", BP_TYPE_DATA)) failed++;
    reset(index);
    if (add(index, "file/child", BP_TYPE_DATA) || !add(index, "file", BP_TYPE_DATA)) failed++;
    reset(index);
    if (!add(index, "\xE4\xB8\xAD.txt", BP_TYPE_DATA) || index->count != 0) failed++;
    reset(index);
    for (i = 0; i < 1024; i++) {
        sprintf(name, "file%04d", i);
        if (add(index, name, BP_TYPE_DATA)) { failed++; break; }
    }
    if (!add(index, "overflow", BP_TYPE_DATA) || index->count != GUI_NAME_INDEX_MAX) failed++;
    reset(index);
    for (i = 0; i < 1024; i++) {
        sprintf(name, "file%04d", i);
        memset(name + 8, 'x', 192); name[200] = '\0';
        if (add(index, name, BP_TYPE_DATA)) { refused = 1; break; }
    }
    if (!refused || index->bytes > GUI_NAME_INDEX_BYTES_MAX || index->count >= GUI_NAME_INDEX_MAX) failed++;
    gui_name_index_clear(index); free(index);
    printf("Amiga GUI name preflight: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
