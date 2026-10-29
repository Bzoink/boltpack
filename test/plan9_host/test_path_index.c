#include <stdio.h>
#include <string.h>

#include "../../src/frontend/plan9/p9_path.h"

/* The real predicate is covered by core tests; this test links the index alone. */
int bp_name_is_safe(const char *name)
{
    const char *p;
    if (name == NULL || name[0] == '\0' || name[0] == '/' ||
        strchr(name, '\\') != NULL || strchr(name, ':') != NULL)
        return 0;
    for (p = name; *p; p++)
        if (p[0] == '.' && p[1] == '.' &&
            (p == name || p[-1] == '/') && (p[2] == '\0' || p[2] == '/'))
            return 0;
    return 1;
}

static int expect_add(p9_path_index_t *index, const char *name, int is_dir,
                      int expected)
{
    int rc = p9_path_index_add(index, name, is_dir);
    if (rc != expected) {
        fprintf(stderr, "path index %s: got %d, expected %d\n", name, rc,
                expected);
        return 1;
    }
    return 0;
}

int main(void)
{
    p9_path_index_t index;
    int failed = 0;
    unsigned int i;
    char name[32];

    if (p9_path_index_init(&index) != BP_OK) return 1;
    failed |= expect_add(&index, "node", 0, BP_OK);
    failed |= expect_add(&index, "node/child", 0, BP_ERR_PARTIAL);
    p9_path_index_free(&index);

    if (p9_path_index_init(&index) != BP_OK) return 1;
    failed |= expect_add(&index, "node/child", 0, BP_OK);
    failed |= expect_add(&index, "node", 0, BP_ERR_PARTIAL);
    failed |= expect_add(&index, "node/", 1, BP_OK);
    failed |= expect_add(&index, "node/child", 0, BP_ERR_PARTIAL);
    p9_path_index_free(&index);

    if (p9_path_index_init(&index) != BP_OK) return 1;
    failed |= expect_add(&index, "../escape", 0, BP_ERR_PARTIAL);
    failed |= expect_add(&index, "safe/name", 0, BP_OK);
    failed |= expect_add(&index, "safe/name", 0, BP_ERR_PARTIAL);
    for (i = 0; i < 1500; i++) {
        sprintf(name, "directory-%u/file", i);
        if (expect_add(&index, name, 0, BP_OK)) failed = 1;
    }
    p9_path_index_free(&index);

    if (failed) return 1;
    puts("Plan 9 path preflight index checks passed");
    return 0;
}
