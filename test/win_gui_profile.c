/* Exercise the production Win32 GUI create-name preflight under Wine. */
#define WinMain boltpack_gui_winmain_unused
#include "../src/frontend/win_gui/main.c"
#undef WinMain

static int expect_name(const char *label, const char *name, int is_directory,
                       int expected)
{
    CreateNameIndex index;
    int rc;
    if (win_gui_create_names_init(&index) != BP_OK) return 1;
    rc = win_gui_create_name_add(&index, name, is_directory);
    win_gui_create_names_free(&index);
    if (rc != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", label, expected, rc);
        return 1;
    }
    return 0;
}

static int expect_collision(const char *label, const char *first,
                            const char *second)
{
    CreateNameIndex index;
    int rc;
    if (win_gui_create_names_init(&index) != BP_OK) return 1;
    rc = win_gui_create_name_add(&index, first, 0);
    if (rc == BP_OK) rc = win_gui_create_name_add(&index, second, 0);
    win_gui_create_names_free(&index);
    if (rc != BP_ERR_PARTIAL) {
        fprintf(stderr, "%s: expected collision refusal, got %d\n", label, rc);
        return 1;
    }
    return 0;
}

static int expect_source_tree(const char *label, const char *path,
                              const char *archive_name, int expected)
{
    CreateNameIndex index;
    int rc;
    g_create_reparse_refused = 0;
    if (win_gui_create_names_init(&index) != BP_OK) return 1;
    rc = win_gui_preflight_create_path(&index, path, archive_name);
    win_gui_create_names_free(&index);
    if (rc != expected) {
        fprintf(stderr, "%s: expected %d, got %d\n", label, expected, rc);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char component_255[256];
    char component_256[257];
    char path_259[260];
    char path_260[261];
    int failures = 0;

    memset(component_255, 'a', sizeof(component_255) - 1);
    component_255[sizeof(component_255) - 1] = '\0';
    memset(component_256, 'a', sizeof(component_256) - 1);
    component_256[sizeof(component_256) - 1] = '\0';
    memset(path_259, 'a', 128);
    path_259[128] = '/';
    memset(path_259 + 129, 'b', 130);
    path_259[259] = '\0';
    memcpy(path_260, path_259, sizeof(path_259));
    path_260[259] = 'c';
    path_260[260] = '\0';

    failures += expect_name("ordinary name", "Folder/Readme.txt", 0, BP_OK);
    failures += expect_name("255-unit component", component_255, 0, BP_OK);
    failures += expect_name("256-unit component", component_256, 0,
                            BP_ERR_INVALID_ARG);
    failures += expect_name("259-unit path", path_259, 0, BP_OK);
    failures += expect_name("260-unit path", path_260, 0,
                            BP_ERR_NAME_TOO_LONG);
    failures += expect_name("device name", "CON.txt", 0, BP_ERR_INVALID_ARG);
    failures += expect_name("superscript device alias", "COM\xc2\xb9.txt", 0,
                            BP_ERR_INVALID_ARG);
    failures += expect_name("trailing dot", "name.", 0, BP_ERR_INVALID_ARG);
    failures += expect_name("forbidden character", "bad:name", 0,
                            BP_ERR_INVALID_ARG);
    failures += expect_collision("case-insensitive collision", "Readme.txt",
                                 "README.TXT");
    failures += expect_collision("file/directory prefix", "folder",
                                 "folder/child.txt");

    /* If this Windows runtime has NormalizeString, canonical equivalents
     * must collide. On older supported systems the documented fallback is to
     * refuse both non-ASCII names instead of accepting an unchecked alias. */
    {
        CreateNameIndex index;
        int first, second = BP_OK;
        if (win_gui_create_names_init(&index) != BP_OK) return 2;
        first = win_gui_create_name_add(&index, "caf\xc3\xa9.txt", 0);
        if (first == BP_OK)
            second = win_gui_create_name_add(&index, "cafe\xcc\x81.txt", 0);
        win_gui_create_names_free(&index);
        if (!((first == BP_OK && second == BP_ERR_PARTIAL) ||
              first == BP_ERR_INVALID_ARG)) {
            fprintf(stderr, "NFC collision/fallback policy failed: %d, %d\n",
                    first, second);
            failures++;
        }
    }

    if (argc == 3) {
        failures += expect_source_tree("valid recursive source tree", argv[1],
                                       "Selected", BP_OK);
        failures += expect_source_tree("recursive case collision", argv[2],
                                       "Selected", BP_ERR_PARTIAL);
    } else if (argc != 1) {
        fprintf(stderr, "usage: win_gui_profile.exe [valid-tree collision-tree]\n");
        return 2;
    }

    if (failures != 0) return 1;
    puts("Win32 GUI create-profile preflight checks passed");
    return 0;
}
