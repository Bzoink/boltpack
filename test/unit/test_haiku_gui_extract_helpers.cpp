#define _DEFAULT_SOURCE 1
#include "../../src/frontend/haiku_gui/extract_helpers.h"
extern "C" {
#include "../../src/core/compress.h"
}

#include <assert.h>
#include <dirent.h>
#include <algorithm>
#include <stdlib.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

struct CoreExtractFixture {
    int root_fd;
    int opened;
    int completed;
    bp_haiku_extract_output_t output;
    bp_io_t io;
};

static long test_file_read(void *handle, void *buffer, bp_uint32 count)
{
    return (long)fread(buffer, 1, (size_t)count, (FILE *)handle);
}

static long test_file_write(void *handle, const void *buffer, bp_uint32 count)
{
    return (long)fwrite(buffer, 1, (size_t)count, (FILE *)handle);
}

static int test_file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseeko((FILE *)handle, offset, origin);
}

static bp_off_t test_file_tell(void *handle)
{
    return (bp_off_t)ftello((FILE *)handle);
}

static void test_file_io(bp_io_t *io, FILE *file)
{
    io->read = test_file_read;
    io->write = test_file_write;
    io->seek = test_file_seek;
    io->tell = test_file_tell;
    io->handle = file;
}

static bp_io_t *core_extract_open(const bp_entry_t *entry, void *user_data)
{
    CoreExtractFixture *ctx = (CoreExtractFixture *)user_data;
    if (entry->entry_type != BP_TYPE_DATA) return NULL;
    assert(bp_haiku_extract_output_open(ctx->root_fd, entry->name,
                                        &ctx->output) == BP_OK);
    test_file_io(&ctx->io, ctx->output.stream);
    ctx->opened++;
    return &ctx->io;
}

static int core_extract_done(const bp_entry_t *entry, bp_io_t *out,
                             int status, void *user_data)
{
    CoreExtractFixture *ctx = (CoreExtractFixture *)user_data;
    assert(out == &ctx->io);
    ctx->completed++;
    return bp_haiku_extract_output_complete(&ctx->output, status,
                                            entry->flags, entry->mtime);
}

static void write_text(FILE *fp, const char *text)
{
    size_t length = strlen(text);
    assert(fwrite(text, 1, length, fp) == length);
}

static std::string read_text(const std::string &path)
{
    char buffer[128];
    FILE *fp = fopen(path.c_str(), "rb");
    size_t length;
    assert(fp != NULL);
    length = fread(buffer, 1, sizeof(buffer), fp);
    assert(!ferror(fp));
    assert(fclose(fp) == 0);
    return std::string(buffer, length);
}

static void assert_no_stage(const char *directory)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    assert(dir != NULL);
    while ((entry = readdir(dir)) != NULL)
        assert(strncmp(entry->d_name, ".boltpack-gui-", 13) != 0);
    closedir(dir);
}

static void test_preflight_case_fold(void)
{
    bp_posix_preflight_t paths;
    bp_entry_t entry;
    char first[] = "Case/Name.txt";
    char second[] = "case/name.TXT";
    memset(&paths, 0, sizeof(paths));
    memset(&entry, 0, sizeof(entry));
    entry.entry_type = BP_TYPE_DATA;
    entry.name = first;
    entry.name_len = (bp_uint16)strlen(first);
    assert(bp_haiku_preflight_add(&paths, &entry) == BP_OK);
    entry.name = second;
    entry.name_len = (bp_uint16)strlen(second);
    assert(bp_haiku_preflight_add(&paths, &entry) == BP_OK);
    assert(bp_posix_preflight_finish(&paths) == BP_ERR_PARTIAL);
    bp_posix_preflight_free(&paths);
}

static void create_stored_archive(const std::string &archive_path,
                                  const std::string &source_path,
                                  const std::string &payload)
{
    FILE *archive = fopen(archive_path.c_str(), "w+b");
    FILE *source = fopen(source_path.c_str(), "wb");
    bp_io_t archive_io, source_io;
    bp_archive_t writer;
    assert(archive != NULL && source != NULL);
    write_text(source, payload.c_str());
    assert(fclose(source) == 0);
    test_file_io(&archive_io, archive);
    assert(bp_compress_init() == BP_OK);
    assert(bp_archive_create(&writer, &archive_io, BP_BLOCK_8K,
                             BP_METHOD_STORED) == BP_OK);
    source = fopen(source_path.c_str(), "rb");
    assert(source != NULL);
    test_file_io(&source_io, source);
    assert(bp_archive_add_entry_ex(&writer, "file.bin", BP_TYPE_DATA,
             BP_OS_LINUX, &source_io, bp_u64_from_u32((bp_uint32)payload.size()),
             bp_u64_from_u32(0), 0) == BP_OK);
    assert(fclose(source) == 0);
    assert(bp_archive_finalize(&writer) == BP_OK);
    bp_archive_close(&writer);
    assert(fclose(archive) == 0);
}

static void corrupt_payload(const std::string &archive_path,
                            const std::string &payload)
{
    std::ifstream input(archive_path.c_str(), std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                     std::istreambuf_iterator<char>());
    std::vector<unsigned char>::iterator found;
    assert(input.good() || input.eof());
    found = std::search(bytes.begin(), bytes.end(), payload.begin(), payload.end());
    assert(found != bytes.end());
    *found ^= 0x40;
    FILE *output = fopen(archive_path.c_str(), "wb");
    assert(output != NULL);
    assert(fwrite(&bytes[0], 1, bytes.size(), output) == bytes.size());
    assert(fclose(output) == 0);
}

static void test_core_checksum_preservation(const std::string &root)
{
    std::string archive_path = root + "/corrupt.blt";
    std::string source_path = root + "/source.bin";
    std::string target_path = root + "/file.bin";
    const std::string payload = "verified archive bytes must match checksum\n";
    const std::string original = "existing file survives checksum failure\n";
    FILE *target;
    CoreExtractFixture ctx;
    FILE *archive;
    bp_io_t archive_io;
    int rc;

    create_stored_archive(archive_path, source_path, payload);
    memset(&ctx, 0, sizeof(ctx));
    ctx.root_fd = bp_haiku_extract_root_open(root.c_str());
    assert(ctx.root_fd >= 0);
    archive = fopen(archive_path.c_str(), "rb");
    assert(archive != NULL);
    test_file_io(&archive_io, archive);
    rc = bp_archive_extract_all_ex(&archive_io, core_extract_open,
                                   core_extract_done, NULL, &ctx);
    assert(rc == BP_OK);
    assert(ctx.opened == 1 && ctx.completed == 1);
    assert(read_text(target_path) == payload);
    assert_no_stage(root.c_str());
    assert(fclose(archive) == 0);
    close(ctx.root_fd);

    corrupt_payload(archive_path, payload);
    target = fopen(target_path.c_str(), "wb");
    assert(target != NULL);
    write_text(target, original.c_str());
    assert(fclose(target) == 0);
    memset(&ctx, 0, sizeof(ctx));
    ctx.root_fd = bp_haiku_extract_root_open(root.c_str());
    assert(ctx.root_fd >= 0);
    archive = fopen(archive_path.c_str(), "rb");
    assert(archive != NULL);
    test_file_io(&archive_io, archive);
    rc = bp_archive_extract_all_ex(&archive_io, core_extract_open,
                                   core_extract_done, NULL, &ctx);
    assert(rc == BP_ERR_CHECKSUM);
    assert(ctx.opened == 1 && ctx.completed == 1);
    assert(read_text(target_path) == original);
    assert_no_stage(root.c_str());
    assert(fclose(archive) == 0);
    close(ctx.root_fd);
}

int main(void)
{
    char root_template[] = "/tmp/bp-haiku-gui-extract-XXXXXX";
    char *root = mkdtemp(root_template);
    char target[512], outside[512], link_path[512], occupied[512];
    int root_fd;
    bp_haiku_extract_output_t output;
    assert(root != NULL);
    test_preflight_case_fold();
    test_core_checksum_preservation(root);

    snprintf(target, sizeof(target), "%s/file.bin", root);
    {
        FILE *fp = fopen(target, "wb");
        assert(fp != NULL);
        write_text(fp, "original survives bad payload\n");
        assert(fclose(fp) == 0);
    }
    root_fd = bp_haiku_extract_root_open(root);
    assert(root_fd >= 0);

    assert(bp_haiku_extract_output_open(root_fd, "file.bin", &output) == BP_OK);
    write_text(output.stream, "unverified replacement\n");
    assert(bp_haiku_extract_output_finish(&output, 0) == BP_OK);
    assert(read_text(target) == "original survives bad payload\n");
    assert_no_stage(root);

    assert(bp_haiku_extract_output_open(root_fd, "file.bin", &output) == BP_OK);
    write_text(output.stream, "verified replacement\n");
    assert(bp_haiku_extract_output_finish(&output, 1) == BP_OK);
    assert(read_text(target) == "verified replacement\n");
    assert_no_stage(root);

    snprintf(outside, sizeof(outside), "%s-outside", root);
    assert(mkdir(outside, 0700) == 0);
    snprintf(link_path, sizeof(link_path), "%s/link", root);
    assert(symlink(outside, link_path) == 0);
    assert(bp_haiku_extract_output_open(root_fd, "link/escape.bin",
                                        &output) != BP_OK);
    assert(access((std::string(outside) + "/escape.bin").c_str(), F_OK) != 0);

    snprintf(occupied, sizeof(occupied), "%s/occupied", root);
    assert(mkdir(occupied, 0700) == 0);
    assert(bp_haiku_extract_output_open(root_fd, "occupied", &output) == BP_OK);
    write_text(output.stream, "cannot replace a directory\n");
    assert(bp_haiku_extract_output_finish(&output, 1) != BP_OK);
    {
        struct stat st;
        assert(stat(occupied, &st) == 0 && S_ISDIR(st.st_mode));
    }
    assert_no_stage(root);

    close(root_fd);
    unlink(link_path);
    unlink(target);
    rmdir(occupied);
    rmdir(root);
    rmdir(outside);
    return 0;
}
