/* Host tests for checked RISC OS/format pathname projections. */
#include <stdio.h>
#include <string.h>
#include "../../src/frontend/riscos/riscos_path.h"

int main(void)
{
    char path[32], arc[32], joined[32];
    static const char latin1_path[] = "dir.caf\351";
    static const char utf8_arc[] = "dir/caf\303\251";
    int failed = 0;

    if (bp_riscos_arc_to_path("dir/sub/file", path, sizeof(path)) != 0 ||
        strcmp(path, "dir.sub.file") != 0) failed++;
    if (bp_riscos_path_to_arc("dir.sub.file", arc, sizeof(arc)) != 0 ||
        strcmp(arc, "dir/sub/file") != 0) failed++;
    if (bp_riscos_path_to_arc(latin1_path, arc, sizeof(arc)) != 0 ||
        strcmp(arc, utf8_arc) != 0) failed++;
    if (bp_riscos_arc_to_path(utf8_arc, path, sizeof(path)) != 0 ||
        strcmp(path, latin1_path) != 0) failed++;
    if (bp_riscos_arc_join(arc, sizeof(arc), "dir", "file") != 0 ||
        strcmp(arc, "dir/file") != 0) failed++;
    if (bp_riscos_path_join(joined, sizeof(joined), "$.dir", "file") != 0 ||
        strcmp(joined, "$.dir.file") != 0) failed++;
    if (bp_riscos_path_join(joined, sizeof(joined), "$.dir.", "file") != 0 ||
        strcmp(joined, "$.dir.file") != 0) failed++;
    if (bp_riscos_sibling_path(joined, sizeof(joined), "$.dir.file", "BPT0001") != 0 ||
        strcmp(joined, "$.dir.BPT0001") != 0) failed++;
    if (bp_riscos_sibling_path(joined, sizeof(joined), "file", "BPT0001") != 0 ||
        strcmp(joined, "BPT0001") != 0) failed++;

    strcpy(path, "sentinel");
    if (bp_riscos_arc_to_path("file.name", path, sizeof(path)) == 0 ||
        path[0] != '\0') failed++;
    strcpy(path, "sentinel");
    if (bp_riscos_arc_to_path("this-name-is-too-long", path, 8) == 0 ||
        path[0] != '\0') failed++;
    strcpy(joined, "sentinel");
    if (bp_riscos_path_join(joined, 8, "$.long", "filename") == 0 ||
        joined[0] != '\0') failed++;
    strcpy(arc, "sentinel");
    if (bp_riscos_arc_join(arc, 8, "parent", "child") == 0 ||
        arc[0] != '\0') failed++;

    printf("RISC OS pathname projection: %s\n", failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
