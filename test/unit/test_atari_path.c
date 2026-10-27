/* Host checks for Atari TOS filename projection and collision bounds. */
#include "../../src/frontend/atari/atari_path.h"
#include <stdio.h>

static unsigned int tests_run;
static unsigned int tests_failed;

static void check(int condition, const char *label)
{
    tests_run++;
    if (condition) {
        printf("  PASS  %s\n", label);
    } else {
        printf("  FAIL  %s\n", label);
        tests_failed++;
    }
}

static int project_equals(const char *name, int is_directory,
                          const char *expected)
{
    char key[BP_ATARI_GEMDOS_PATH_MAX + 2];
    unsigned int length = 0;
    int rc = bp_atari_project_name(name, is_directory, key, sizeof(key),
                                   &length);
    return rc == BP_OK && length == strlen(expected) &&
           strcmp(key, expected) == 0;
}

static int project_rejects(const char *name, int is_directory)
{
    char key[BP_ATARI_GEMDOS_PATH_MAX + 2];
    unsigned int length = 0;
    return bp_atari_project_name(name, is_directory, key, sizeof(key),
                                 &length) != BP_OK;
}

static void make_deep_name(char *name, size_t final_component_length)
{
    static const char component[] = "ABCDEFGH.XYZ";
    static const char short_final[] = "ABCDEFGH";
    static const char long_final[] = "ABCDEFG.X";
    size_t pos = 0;
    unsigned int i;

    for (i = 0; i < 9U; i++) {
        if (i != 0U) name[pos++] = '/';
        memcpy(name + pos, component, sizeof(component) - 1U);
        pos += sizeof(component) - 1U;
    }
    name[pos++] = '/';
    if (final_component_length == sizeof(short_final) - 1U) {
        memcpy(name + pos, short_final, sizeof(short_final) - 1U);
        pos += sizeof(short_final) - 1U;
    } else {
        memcpy(name + pos, long_final, sizeof(long_final) - 1U);
        pos += sizeof(long_final) - 1U;
    }
    name[pos] = '\0';
}

static void test_collision_set(void)
{
    bp_atari_name_set_t set;
    char key[BP_ATARI_GEMDOS_PATH_MAX + 1];
    unsigned int length;
    int first_rc, second_rc;

    bp_atari_name_set_init(&set);
    first_rc = bp_atari_project_name("tree/leaf.txt", 0, key, sizeof(key),
                                     &length);
    if (first_rc == BP_OK)
        first_rc = bp_atari_name_set_add(&set, key, length);
    second_rc = bp_atari_project_name("TREE/LEAF.TXT", 0, key, sizeof(key),
                                      &length);
    if (second_rc == BP_OK)
        second_rc = bp_atari_name_set_add(&set, key, length);
    check(first_rc == BP_OK && second_rc == BP_ERR_CORRUPT,
          "case-folded slash-path aliases collide");
    bp_atari_name_set_free(&set);
}

static void test_index_bounds(void)
{
    bp_atari_name_set_t set;
    char key[126];
    char prefix[5];
    unsigned int i;
    int rc = BP_OK;

    bp_atari_name_set_init(&set);
    for (i = 0; i < 261U; i++) {
        (void)sprintf(prefix, "%04X", i);
        memcpy(key, prefix, 4U);
        memset(key + 4, 'A', 121U);
        key[125] = '\0';
        rc = bp_atari_name_set_add(&set, key, 125U);
        if (i < 260U && rc != BP_OK) break;
        if (i == 260U) break;
    }
    check(i == 260U && rc == BP_ERR_OVERFLOW && set.bytes == 32760UL,
          "path-key byte bound refuses the first key over 32 KiB");
    bp_atari_name_set_free(&set);

    bp_atari_name_set_init(&set);
    rc = BP_OK;
    for (i = 0; i < BP_ATARI_PATH_MAX_ENTRIES + 1U; i++) {
        (void)sprintf(key, "K%04u", i);
        rc = bp_atari_name_set_add(&set, key, (unsigned int)strlen(key));
        if (rc != BP_OK) break;
    }
    check(i == BP_ATARI_PATH_MAX_ENTRIES && rc == BP_ERR_OVERFLOW &&
          set.count == BP_ATARI_PATH_MAX_ENTRIES,
          "entry bound refuses name 1,025");
    bp_atari_name_set_free(&set);
}

int main(void)
{
    char deep[BP_ATARI_GEMDOS_PATH_MAX + 2];
    unsigned int length = 0;
    char key[BP_ATARI_GEMDOS_PATH_MAX + 2];
    int rc;

    printf("BoltPack Atari path projection tests\n\n");
    check(project_equals("readme.txt", 0, "README.TXT"),
          "ASCII filename is uppercased");
    check(project_equals("folder/subdir/file.bin", 0,
                         "FOLDER\\SUBDIR\\FILE.BIN"),
          "directory separators use the GEMDOS form");
    check(project_equals("empty/", 1, "EMPTY"),
          "directory trailing slash is canonicalized");
    check(project_rejects("empty/", 0),
          "file path with a trailing separator is refused");
    check(project_rejects("longfilename.txt", 0),
          "base name longer than eight characters is refused");
    check(project_rejects("name.toolong", 0),
          "extension longer than three characters is refused");
    check(project_rejects("a..b", 0),
          "multiple dots in one component are refused");
    check(project_rejects("bad?.txt", 0),
          "DOS wildcard punctuation is refused");
    check(project_rejects("bad:name.txt", 0),
          "volume separator is refused");
    check(project_rejects("bad\\name.txt", 0),
          "backslash in an archive component is refused");
    check(project_rejects("caf\x82.txt", 0),
          "non-ASCII filename bytes are refused by the ASCII profile");

    make_deep_name(deep, 8U);
    rc = bp_atari_project_name(deep, 0, key, sizeof(key), &length);
    check(rc == BP_OK && length == BP_ATARI_GEMDOS_PATH_MAX,
          "125-byte GEMDOS path is accepted");
    make_deep_name(deep, 9U);
    check(bp_atari_project_name(deep, 0, key, sizeof(key), &length) ==
          BP_ERR_OVERFLOW,
          "126-byte GEMDOS path is refused");

    test_collision_set();
    test_index_bounds();

    printf("\nResults: %u passed, %u failed\n", tests_run - tests_failed,
           tests_failed);
    return tests_failed == 0U ? 0 : 1;
}
