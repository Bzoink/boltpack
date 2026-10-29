#include <stdio.h>
#include <string.h>
#include "../../src/frontend/ql/ql_path.h"

static int expect(const char *input, int directory, const char *expected)
{
    char output[64];
    if (ql_project_component(input, directory, output, sizeof(output)) != 0 ||
        strcmp(output, expected) != 0) {
        fprintf(stderr, "QL path projection failed for '%s'\n", input);
        return 1;
    }
    return 0;
}

int main(void)
{
    char output[64];
    char boundary[8];
    char stage[16], backup[16];
    int failures = 0;
    if (strcmp(ql_strip_disk_device("win1_my_file"), "my_file") != 0)
        failures++;
    if (strcmp(ql_strip_disk_device("MDV2_dir_file"), "dir_file") != 0)
        failures++;
    if (strcmp(ql_strip_disk_device("flp8_dir_"), "dir_") != 0)
        failures++;
    if (strcmp(ql_strip_disk_device("my_file"), "my_file") != 0)
        failures++;
    if (strcmp(ql_strip_disk_device("data1_file"), "data1_file") != 0)
        failures++;
    failures += expect("dir_", 1, "dir");
    failures += expect("sub_dir_", 1, "sub_dir");
    failures += expect("file_name", 0, "file_name");
    failures += expect("file_", 0, "file_");
    if (ql_project_component("../escape", 0, output, sizeof(output)) == 0)
        failures++;
    if (ql_project_component("dir/name", 0, output, sizeof(output)) == 0)
        failures++;
    if (ql_project_component("dir:", 0, output, sizeof(output)) == 0)
        failures++;
    if (ql_project_component("_", 1, output, sizeof(output)) == 0)
        failures++;
    if (ql_native_output_path(NULL, "win1_archive", "dir/file", output,
                              sizeof(output)) != 0 ||
        strcmp(output, "win1_dir_file") != 0)
        failures++;
    if (ql_native_output_path("win1_out", "win1_archive", "dir/file",
                              output, sizeof(output)) != 0 ||
        strcmp(output, "win1_out_dir_file") != 0)
        failures++;
    if (ql_native_output_path("out", "MDV2_archive", "file", output,
                              sizeof(output)) != 0 ||
        strcmp(output, "MDV2_out_file") != 0)
        failures++;
    if (ql_native_output_path(NULL, "archive", "file_name", output,
                              sizeof(output)) != 0 ||
        strcmp(output, "file_name") != 0)
        failures++;
    if (ql_native_output_path(NULL, "win1_archive",
                              "1234567890123456789012345678901234567",
                              output, sizeof(output)) == 0)
        failures++;
    if (ql_path_join2(boundary, sizeof(boundary), "abc", "def") != 0 ||
        strcmp(boundary, "abcdef") != 0)
        failures++;
    strcpy(boundary, "safe");
    if (ql_path_join2(boundary, sizeof(boundary), "abcd", "efgh") == 0 ||
        strcmp(boundary, "safe") != 0)
        failures++;
    if (ql_path_join3(boundary, sizeof(boundary), "ab", "/", "cde") != 0 ||
        strcmp(boundary, "ab/cde") != 0)
        failures++;
    strcpy(boundary, "safe");
    if (ql_path_join3(boundary, sizeof(boundary), "abc", "/", "defg") == 0 ||
        strcmp(boundary, "safe") != 0)
        failures++;
    if (ql_stage_leaf_length("file") != 8U ||
        ql_stage_leaf_length("1234567890123456789012345678901234/a") != 1U ||
        ql_stage_leaf_length("123456789012345678901234567890123/a") != 2U)
        failures++;
    if (ql_stage_leaf_length_for_target("win1_dir_file", 4U) != 8U ||
        ql_stage_leaf_length_for_target(
            "win1_12345678901234567890123456789012345a", 1U) != 1U)
        failures++;
    ql_stage_leaf('B', 0UL, 1U, stage);
    ql_stage_leaf('K', 1UL, 1U, backup);
    if (strcmp(stage, "0") != 0 || strcmp(backup, "1") != 0 ||
        ql_stage_candidate_count(1U) != 36UL)
        failures++;
    ql_stage_leaf('B', 0UL, 2U, stage);
    ql_stage_leaf('K', 0UL, 2U, backup);
    if (strcmp(stage, "B0") != 0 || strcmp(backup, "K0") != 0 ||
        ql_stage_candidate_count(8U) != QL_STAGE_CANDIDATE_MAX)
        failures++;
    if (failures != 0) return 1;
    puts("QL native path projection and stage naming: passed");
    return 0;
}
