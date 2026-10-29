/* Exercise the RISC OS staged extraction callbacks through a host SWI shim. */
#define _DEFAULT_SOURCE 1
#define main riscos_frontend_main
#include "../../src/frontend/riscos/main.c"
#undef main
#include "../../src/core/bp_metadata.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static FILE *handles[32];
static int fail_stage_publish;
static int fail_close;
static int metadata_writes;
static _kernel_oserror swi_error = { 1, "host SWI shim failure" };

static int host_path(const char *riscos, char *host, size_t capacity)
{
    size_t i, n = strlen(riscos);
    if (n >= capacity) return -1;
    for (i = 0; i <= n; i++)
        host[i] = riscos[i] == '.' ? '/' : riscos[i];
    return 0;
}

_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in,
                             _kernel_swi_regs *out)
{
    char a[2048], b[2048];
    struct stat st;
    int i;
    (void)out;

    if (swi == OS_File && in->r[0] == OS_File_ReadCat) {
        if (host_path((const char *)in->r[1], a, sizeof(a)) != 0 ||
            stat(a, &st) != 0) return &swi_error;
        in->r[0] = S_ISDIR(st.st_mode) ? RISCOS_OBJ_DIR : RISCOS_OBJ_FILE;
        in->r[2] = 0;
        in->r[3] = 0;
        in->r[4] = (int)st.st_size;
        in->r[5] = 0;
        return NULL;
    }
    if (swi == OS_File && in->r[0] == OS_File_CreateDir) {
        if (host_path((const char *)in->r[1], a, sizeof(a)) != 0)
            return &swi_error;
        if (mkdir(a, 0700) != 0) {
            if (errno != EEXIST || stat(a, &st) != 0 || !S_ISDIR(st.st_mode))
                return &swi_error;
        }
        return NULL;
    }
    if (swi == OS_File && in->r[0] == OS_File_WriteCat) {
        metadata_writes++;
        return NULL;
    }
    if (swi == OS_File && in->r[0] == OS_File_Delete) {
        if (host_path((const char *)in->r[1], a, sizeof(a)) != 0 ||
            unlink(a) != 0) return &swi_error;
        return NULL;
    }
    if (swi == OS_Find && in->r[0] == OS_Find_OpenOut) {
        if (host_path((const char *)in->r[1], a, sizeof(a)) != 0)
            return &swi_error;
        for (i = 1; i < 32 && handles[i] != NULL; i++) {}
        if (i == 32) return &swi_error;
        handles[i] = fopen(a, "wb");
        if (handles[i] == NULL) return &swi_error;
        in->r[0] = i;
        return NULL;
    }
    if (swi == OS_Find && in->r[0] == OS_Find_Close) {
        i = in->r[1];
        if (i <= 0 || i >= 32 || handles[i] == NULL) return &swi_error;
        if (fclose(handles[i]) != 0) {
            handles[i] = NULL;
            return &swi_error;
        }
        handles[i] = NULL;
        if (fail_close) return &swi_error;
        return NULL;
    }
    if (swi == OS_GBPB && in->r[0] == OS_GBPB_Write) {
        i = in->r[1];
        if (i <= 0 || i >= 32 || handles[i] == NULL) return &swi_error;
        in->r[3] -= (int)fwrite((const void *)in->r[2], 1,
                                (size_t)in->r[3], handles[i]);
        return NULL;
    }
    if (swi == OS_FSControl && in->r[0] == 25) {
        if (host_path((const char *)in->r[1], a, sizeof(a)) != 0 ||
            host_path((const char *)in->r[2], b, sizeof(b)) != 0)
            return &swi_error;
        if (fail_stage_publish && strstr(a, "/BPT") != NULL &&
            strcmp(b, "out/data") == 0) return &swi_error;
        return rename(a, b) == 0 ? NULL : &swi_error;
    }
    return &swi_error;
}

static int write_text(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    int ok;
    if (fp == NULL) return -1;
    ok = fwrite(text, 1, strlen(text), fp) == strlen(text) && fclose(fp) == 0;
    return ok ? 0 : -1;
}

static int file_is(const char *path, const char *text)
{
    char buf[64];
    FILE *fp = fopen(path, "rb");
    size_t n;
    if (fp == NULL) return 0;
    n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    return n == strlen(text) && memcmp(buf, text, n) == 0;
}

static int has_stage_file(void);

static long test_file_read(void *handle, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)handle);
}

static long test_file_write(void *handle, const void *buf, bp_uint32 count)
{
    return (long)fwrite(buf, 1, (size_t)count, (FILE *)handle);
}

static int test_file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseek((FILE *)handle, (long)offset, origin);
}

static bp_off_t test_file_tell(void *handle)
{
    long position = ftell((FILE *)handle);
    return position < 0 ? (bp_off_t)-1 : (bp_off_t)position;
}

static bp_io_t test_file_io(FILE *fp)
{
    bp_io_t io;
    io.read = test_file_read;
    io.write = test_file_write;
    io.seek = test_file_seek;
    io.tell = test_file_tell;
    io.handle = fp;
    return io;
}

static int test_unsupported_portable_metadata(extract_ctx_t *ctx)
{
    static const char payload[] = "portable metadata subject";
    FILE *archive_fp = NULL, *payload_fp = NULL, *metadata_fp = NULL;
    bp_io_t archive_io, payload_io, metadata_io;
    bp_archive_t archive;
    bp_uint8 portable[BP_META_FAT_SIZE];
    bp_uint64 zero = bp_u64_from_u32(0);
    int rc;

    archive_fp = tmpfile();
    payload_fp = tmpfile();
    metadata_fp = tmpfile();
    if (archive_fp == NULL || payload_fp == NULL || metadata_fp == NULL) {
        rc = BP_ERR_IO;
        goto done;
    }
    memset(&archive, 0, sizeof(archive));
    if (fwrite(payload, 1, sizeof(payload) - 1U, payload_fp) !=
            sizeof(payload) - 1U ||
        bp_metadata_fat_encode(portable, sizeof(portable), BP_TYPE_DATA,
                               BP_META_FAT_MASK, BP_META_FAT_MASK) != BP_OK ||
        fwrite(portable, 1, sizeof(portable), metadata_fp) != sizeof(portable) ||
        fseek(payload_fp, 0, SEEK_SET) != 0 ||
        fseek(metadata_fp, 0, SEEK_SET) != 0) {
        rc = BP_ERR_IO;
        goto done;
    }

    archive_io = test_file_io(archive_fp);
    payload_io = test_file_io(payload_fp);
    metadata_io = test_file_io(metadata_fp);
    rc = bp_archive_create(&archive, &archive_io, BP_BLOCK_8K,
                           BP_METHOD_STORED);
    if (rc == BP_OK)
        rc = bp_archive_add_entry_ex(&archive, "foreign", BP_TYPE_DATA,
                                     BP_OS_LINUX, &payload_io,
                                     bp_u64_from_u32((bp_uint32)(sizeof(payload) - 1U)),
                                     zero, 0);
    if (rc == BP_OK)
        rc = bp_archive_add_entry_ex(&archive, "foreign", BP_TYPE_META,
                                     BP_OS_LINUX, &metadata_io,
                                     bp_u64_from_u32((bp_uint32)sizeof(portable)),
                                     zero, 0);
    if (rc == BP_OK && fseek(metadata_fp, 0, SEEK_SET) != 0)
        rc = BP_ERR_IO;
    if (rc == BP_OK)
        rc = bp_archive_add_entry_ex(&archive, "foreign", BP_TYPE_META,
                                     BP_META_PORTABLE, &metadata_io,
                                     bp_u64_from_u32((bp_uint32)sizeof(portable)),
                                     zero, 0);
    if (rc == BP_OK) rc = bp_archive_finalize(&archive);
    bp_archive_close(&archive);
    if (rc != BP_OK || fseek(archive_fp, 0, SEEK_SET) != 0) {
        if (rc == BP_OK) rc = BP_ERR_IO;
        goto done;
    }

    rc = bp_archive_extract_all_ex(&archive_io, extract_open,
                                   extract_done_status, NULL, ctx);
    if (rc == BP_ERR_PARTIAL && file_is("out/foreign", payload) &&
        !has_stage_file() && metadata_writes == 1)
        rc = BP_OK;
    else if (rc == BP_OK)
        rc = BP_ERR_CORRUPT;
    else if (rc == BP_ERR_PARTIAL)
        rc = BP_ERR_IO;

done:
    if (archive_fp != NULL) fclose(archive_fp);
    if (payload_fp != NULL) fclose(payload_fp);
    if (metadata_fp != NULL) fclose(metadata_fp);
    return rc;
}

static int has_stage_file(void)
{
    DIR *dir = opendir("out");
    struct dirent *entry;
    int found = 0;
    if (dir == NULL) return 1;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "BPT", 3) == 0 ||
            strncmp(entry->d_name, "BPR", 3) == 0) found = 1;
    }
    closedir(dir);
    return found;
}

static int extract_one(extract_ctx_t *ctx, bp_entry_t *entry,
                       const char *payload, int status)
{
    bp_io_t *out = extract_open(entry, ctx);
    if (out == NULL) return BP_ERR_PARTIAL;
    if (out->write(out->handle, payload, (bp_uint32)strlen(payload)) !=
        (long)strlen(payload)) return BP_ERR_IO;
    return extract_done_status(entry, out, status, ctx);
}

int main(void)
{
    char temp[] = "/tmp/bp-riscos-stage-XXXXXX";
    extract_ctx_t ctx;
    bp_entry_t entry;
    bp_entry_t meta;
    bp_entry_t directory;
    unsigned char native_meta[RISCOS_META_SIZE];
    bp_io_t *meta_out;
    riscos_preflight_t preflight;
    bp_entry_t mapped;
    struct stat dir_stat;
    int failed = 0;

    if (mkdtemp(temp) == NULL || chdir(temp) != 0 || mkdir("out", 0700) != 0)
        return 2;
    memset(&ctx, 0, sizeof(ctx));
    memset(&entry, 0, sizeof(entry));
    ctx.out_dir = "out";
    ctx.force = 1;
    entry.name = "data";
    entry.entry_type = BP_TYPE_DATA;
    g_verbosity = VERB_SILENT;

    /* Whole-archive destination mapping checks are bounded and side-effect
     * free. FileSwitch case aliases and file/child conflicts are refused. */
    memset(&preflight, 0, sizeof(preflight));
    preflight.result = BP_OK;
    memset(&mapped, 0, sizeof(mapped));
    mapped.name = "Readme";
    mapped.entry_type = BP_TYPE_DATA;
    if (riscos_preflight_entry(&mapped, &preflight) != 0) failed++;
    mapped.name = "README";
    if (riscos_preflight_entry(&mapped, &preflight) != 0 ||
        riscos_preflight_finish(&preflight) == BP_OK) failed++;

    memset(&preflight, 0, sizeof(preflight));
    preflight.result = BP_OK;
    mapped.name = "node";
    if (riscos_preflight_entry(&mapped, &preflight) != 0) failed++;
    mapped.name = "node/child";
    if (riscos_preflight_entry(&mapped, &preflight) != 0 ||
        riscos_preflight_finish(&preflight) == BP_OK) failed++;

    memset(&preflight, 0, sizeof(preflight));
    preflight.result = BP_OK;
    mapped.name = "this-leaf-is-too-long";
    if (riscos_preflight_entry(&mapped, &preflight) == 0 ||
        riscos_preflight_finish(&preflight) == BP_OK) failed++;

    memset(&preflight, 0, sizeof(preflight));
    preflight.result = BP_OK;
    mapped.name = "data";
    mapped.entry_type = BP_TYPE_RSRC;
    if (riscos_preflight_entry(&mapped, &preflight) == 0 ||
        riscos_preflight_finish(&preflight) == BP_OK) failed++;
    mapped.entry_type = BP_TYPE_DATA;

    /* A directory creation failure must be recorded, not silently treated as
     * a successful directory callback. */
    memset(&directory, 0, sizeof(directory));
    directory.name = "notdir/";
    directory.entry_type = BP_TYPE_DIR;
    if (write_text("out/notdir", "file blocks directory") != 0) return 2;
    if (extract_open(&directory, &ctx) != NULL || !ctx.error) failed++;
    ctx.error = 0;
    if (unlink("out/notdir") != 0) return 2;
    directory.name = "nested/";
    if (extract_open(&directory, &ctx) != NULL || ctx.error ||
        stat("out/nested", &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode))
        failed++;

    if (write_text("out/data", "original") != 0) return 2;
    if (extract_one(&ctx, &entry, "unverified", BP_ERR_CHECKSUM) !=
        BP_ERR_CHECKSUM || !file_is("out/data", "original") || has_stage_file())
        failed++;

    if (extract_one(&ctx, &entry, "replacement", BP_OK) != BP_OK ||
        !file_is("out/data", "replacement") || has_stage_file()) failed++;

    if (write_text("out/data", "close-safe") != 0) return 2;
    fail_close = 1;
    if (extract_one(&ctx, &entry, "close-failure", BP_OK) != BP_ERR_IO ||
        !file_is("out/data", "close-safe") || has_stage_file()) failed++;
    fail_close = 0;

    if (write_text("out/data", "rollback") != 0) return 2;
    fail_stage_publish = 1;
    if (extract_one(&ctx, &entry, "not-published", BP_OK) != BP_ERR_IO ||
        !file_is("out/data", "rollback") || has_stage_file()) failed++;
    fail_stage_publish = 0;

    ctx.force = 0;
    if (extract_one(&ctx, &entry, "must-not-write", BP_OK) != BP_ERR_PARTIAL ||
        !file_is("out/data", "rollback") || has_stage_file()) failed++;

    if (unlink("out/data") != 0) return 2;
    if (extract_one(&ctx, &entry, "new-file", BP_OK) != BP_OK ||
        !file_is("out/data", "new-file") || has_stage_file()) failed++;

    memset(&meta, 0, sizeof(meta));
    memset(native_meta, 0, sizeof(native_meta));
    meta.name = "data";
    meta.entry_type = BP_TYPE_META;
    meta.platform = BP_OS_RISCOS;
    meta.original_size = bp_u64_from_u32(RISCOS_META_SIZE);
    meta_out = extract_open(&meta, &ctx);
    if (meta_out == NULL ||
        meta_out->write(meta_out->handle, native_meta, sizeof(native_meta)) !=
            (long)sizeof(native_meta) ||
        extract_done_status(&meta, meta_out, BP_ERR_CHECKSUM, &ctx) !=
            BP_ERR_CHECKSUM || metadata_writes != 0) failed++;

    meta_out = extract_open(&meta, &ctx);
    if (meta_out == NULL ||
        meta_out->write(meta_out->handle, native_meta, sizeof(native_meta)) !=
            (long)sizeof(native_meta) ||
        extract_done_status(&meta, meta_out, BP_OK, &ctx) != BP_OK ||
        metadata_writes != 1) failed++;

    if (test_unsupported_portable_metadata(&ctx) != BP_OK) failed++;

    printf("RISC OS staged extraction shim: %s\n",
           failed ? "FAILED" : "passed");
    return failed ? 1 : 0;
}
