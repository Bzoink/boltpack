#include <stdio.h>
#include <string.h>
#include "../../src/frontend/os2/os2_publish.h"

#define FILES_MAX 12
#define PATH_MAX_TEST 260

typedef struct {
    char path[PATH_MAX_TEST];
    char bytes[32];
    int exists;
    int is_directory;
} test_file_t;

typedef struct {
    test_file_t files[FILES_MAX];
    int move_calls;
    int fail_move_call;
    int fail_move_call2;
    int remove_calls;
    int fail_remove_call;
} test_fs_t;

static int failures;

static void check(int condition, const char *name)
{
    if (condition) printf("PASS  %s\n", name);
    else { printf("FAIL  %s\n", name); failures++; }
}

static test_file_t *find_file(test_fs_t *fs, const char *path)
{
    int i;
    for (i = 0; i < FILES_MAX; i++)
        if (fs->files[i].exists && strcmp(fs->files[i].path, path) == 0)
            return &fs->files[i];
    return NULL;
}

static test_file_t *free_file(test_fs_t *fs)
{
    int i;
    for (i = 0; i < FILES_MAX; i++)
        if (!fs->files[i].exists) return &fs->files[i];
    return NULL;
}

static int file_bytes_equal(test_fs_t *fs, const char *path,
                            const char *expected)
{
    test_file_t *file = find_file(fs, path);
    return file != NULL && strcmp(file->bytes, expected) == 0;
}

static void add_file(test_fs_t *fs, const char *path, const char *bytes)
{
    test_file_t *file = free_file(fs);
    if (file == NULL) { failures++; return; }
    strcpy(file->path, path);
    strcpy(file->bytes, bytes);
    file->is_directory = 0;
    file->exists = 1;
}

static int test_query(void *context, const char *path, int *is_directory)
{
    test_file_t *file = find_file((test_fs_t *)context, path);
    if (file == NULL) return 0;
    *is_directory = file->is_directory;
    return 1;
}

static int test_move(void *context, const char *from, const char *to)
{
    test_fs_t *fs = (test_fs_t *)context;
    test_file_t *source, *destination;
    fs->move_calls++;
    if (fs->move_calls == fs->fail_move_call ||
        fs->move_calls == fs->fail_move_call2) return -1;
    source = find_file(fs, from);
    if (source == NULL || find_file(fs, to) != NULL) return -1;
    destination = source;
    strcpy(destination->path, to);
    return 0;
}

static int test_remove(void *context, const char *path)
{
    test_fs_t *fs = (test_fs_t *)context;
    test_file_t *file;
    fs->remove_calls++;
    if (fs->remove_calls == fs->fail_remove_call) return -1;
    file = find_file(fs, path);
    if (file == NULL) return -1;
    file->exists = 0;
    return 0;
}

static os2_publish_ops_t test_ops(void)
{
    os2_publish_ops_t ops;
    ops.query = test_query;
    ops.move = test_move;
    ops.remove = test_remove;
    return ops;
}

static void test_first_publish(void)
{
    test_fs_t fs;
    os2_publish_ops_t ops = test_ops();
    char recovery[PATH_MAX_TEST];
    unsigned long sequence = 0;
    int committed = 0, rc;
    memset(&fs, 0, sizeof(fs));
    add_file(&fs, "C:\\OUT\\BP000010.TMP", "new bytes");
    rc = os2_publish_staged_file("C:\\OUT\\BP000010.TMP",
                                 "C:\\OUT\\FILE.TXT", 0, recovery,
                                 sizeof(recovery), &sequence, &ops, &fs,
                                 &committed);
    check(rc == BP_OK && committed &&
          file_bytes_equal(&fs, "C:\\OUT\\FILE.TXT", "new bytes") &&
          find_file(&fs, "C:\\OUT\\BP000010.TMP") == NULL,
          "first publication renames the complete stage to the target");
}

static void test_forced_replace(void)
{
    test_fs_t fs;
    os2_publish_ops_t ops = test_ops();
    char recovery[PATH_MAX_TEST];
    unsigned long sequence = 0;
    int committed = 0, rc;
    memset(&fs, 0, sizeof(fs));
    add_file(&fs, "C:\\OUT\\FILE.TXT", "old bytes");
    add_file(&fs, "C:\\OUT\\BP000020.TMP", "new bytes");
    add_file(&fs, "C:\\OUT\\BO000000.BAK", "occupied backup");
    rc = os2_publish_staged_file("C:\\OUT\\BP000020.TMP",
                                 "C:\\OUT\\FILE.TXT", 1, recovery,
                                 sizeof(recovery), &sequence, &ops, &fs,
                                 &committed);
    check(rc == BP_OK && committed &&
          file_bytes_equal(&fs, "C:\\OUT\\FILE.TXT", "new bytes") &&
          file_bytes_equal(&fs, "C:\\OUT\\BO000000.BAK",
                           "occupied backup") &&
          find_file(&fs, "C:\\OUT\\BO000001.BAK") == NULL &&
          recovery[0] == '\0',
          "forced replacement preserves a colliding backup and retires its own");
}

static void test_refusal_and_rollback(void)
{
    test_fs_t fs;
    os2_publish_ops_t ops = test_ops();
    char recovery[PATH_MAX_TEST];
    unsigned long sequence;
    int committed, rc;

    memset(&fs, 0, sizeof(fs));
    add_file(&fs, "C:\\OUT\\FILE.TXT", "old bytes");
    add_file(&fs, "C:\\OUT\\BP000030.TMP", "new bytes");
    sequence = 0; committed = 0;
    rc = os2_publish_staged_file("C:\\OUT\\BP000030.TMP",
                                 "C:\\OUT\\FILE.TXT", 0, recovery,
                                 sizeof(recovery), &sequence, &ops, &fs,
                                 &committed);
    check(rc == BP_ERR_PARTIAL && !committed &&
          file_bytes_equal(&fs, "C:\\OUT\\FILE.TXT", "old bytes") &&
          find_file(&fs, "C:\\OUT\\BP000030.TMP") != NULL,
          "no-force refuses an existing target without changing either file");

    memset(&fs, 0, sizeof(fs));
    add_file(&fs, "C:\\OUT\\FILE.TXT", "old bytes");
    add_file(&fs, "C:\\OUT\\BP000031.TMP", "new bytes");
    fs.fail_move_call = 2;
    sequence = 0; committed = 0;
    rc = os2_publish_staged_file("C:\\OUT\\BP000031.TMP",
                                 "C:\\OUT\\FILE.TXT", 1, recovery,
                                 sizeof(recovery), &sequence, &ops, &fs,
                                 &committed);
    check(rc == BP_ERR_IO && !committed &&
          file_bytes_equal(&fs, "C:\\OUT\\FILE.TXT", "old bytes") &&
          find_file(&fs, "C:\\OUT\\BO000000.BAK") == NULL &&
          recovery[0] == '\0',
          "failed stage rename restores the original target");
}

static void test_recovery_and_cleanup_failure(void)
{
    test_fs_t fs;
    os2_publish_ops_t ops = test_ops();
    char recovery[PATH_MAX_TEST];
    unsigned long sequence;
    int committed, rc;

    memset(&fs, 0, sizeof(fs));
    add_file(&fs, "C:\\OUT\\FILE.TXT", "old bytes");
    add_file(&fs, "C:\\OUT\\BP000040.TMP", "new bytes");
    fs.fail_move_call = 2;
    fs.fail_move_call2 = 3;
    sequence = 0; committed = 0;
    rc = os2_publish_staged_file("C:\\OUT\\BP000040.TMP",
                                 "C:\\OUT\\FILE.TXT", 1, recovery,
                                 sizeof(recovery), &sequence, &ops, &fs,
                                 &committed);
    check(rc == BP_ERR_IO && !committed &&
          find_file(&fs, "C:\\OUT\\FILE.TXT") == NULL &&
          file_bytes_equal(&fs, "C:\\OUT\\BO000000.BAK", "old bytes") &&
          strcmp(recovery, "C:\\OUT\\BO000000.BAK") == 0,
          "failed rollback reports the retained original backup path");

    memset(&fs, 0, sizeof(fs));
    add_file(&fs, "C:\\OUT\\FILE.TXT", "old bytes");
    add_file(&fs, "C:\\OUT\\BP000041.TMP", "new bytes");
    fs.fail_remove_call = 1;
    sequence = 0; committed = 0;
    rc = os2_publish_staged_file("C:\\OUT\\BP000041.TMP",
                                 "C:\\OUT\\FILE.TXT", 1, recovery,
                                 sizeof(recovery), &sequence, &ops, &fs,
                                 &committed);
    check(rc == BP_ERR_PARTIAL && committed &&
          file_bytes_equal(&fs, "C:\\OUT\\FILE.TXT", "new bytes") &&
          file_bytes_equal(&fs, "C:\\OUT\\BO000000.BAK", "old bytes") &&
          strcmp(recovery, "C:\\OUT\\BO000000.BAK") == 0,
          "backup cleanup failure reports the retained original");
}

int main(void)
{
    printf("OS/2 staged publication policy host tests\n");
    test_first_publish();
    test_forced_replace();
    test_refusal_and_rollback();
    test_recovery_and_cleanup_failure();
    printf("Results: %s\n", failures ? "failed" : "passed");
    return failures ? 1 : 0;
}
