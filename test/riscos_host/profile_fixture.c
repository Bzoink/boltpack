/* Run the RISC OS CLI writer through a small host implementation of its SWIs. */
#define _DEFAULT_SOURCE 1
#define main riscos_frontend_main
#include "../../src/frontend/riscos/main.c"
#undef main

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

static FILE *handles[256];
static _kernel_oserror swi_error = { 1, "host SWI shim failure" };

static int host_path(const char *riscos, char *host, size_t capacity)
{
    size_t i, n = strlen(riscos);
    if (n >= capacity) return -1;
    for (i = 0; i <= n; i++) host[i] = riscos[i] == '.' ? '/' : riscos[i];
    return 0;
}

static int host_open(const char *path, int reason)
{
    char host[2048];
    int i;
    const char *mode;
    if (host_path(path, host, sizeof(host)) != 0) return 0;
    mode = reason == OS_Find_OpenIn ? "rb" :
           reason == OS_Find_OpenUp ? "r+b" : "w+b";
    for (i = 1; i < 256 && handles[i] != NULL; i++) {}
    if (i == 256) return 0;
    handles[i] = fopen(host, mode);
    return handles[i] == NULL ? 0 : i;
}

static _kernel_oserror *host_file_info(const char *path,
                                       _kernel_swi_regs *regs)
{
    char host[2048];
    struct stat st;
    unsigned int load = 0, exec = 0;
    if (host_path(path, host, sizeof(host)) != 0 || stat(host, &st) != 0)
        return &swi_error;
    if (st.st_mtime >= 0 && (unsigned long long)st.st_mtime <= 0xFFFFFFFFULL)
        riscos_mtime_from_unix((bp_uint32)st.st_mtime, &load, &exec, 0);
    regs->r[0] = S_ISDIR(st.st_mode) ? RISCOS_OBJ_DIR : RISCOS_OBJ_FILE;
    regs->r[2] = (int)load;
    regs->r[3] = (int)exec;
    regs->r[4] = (int)st.st_size;
    regs->r[5] = (int)(S_ISDIR(st.st_mode) ? 0x33U : 0x03U);
    return NULL;
}

static _kernel_oserror *host_enum_dir(_kernel_swi_regs *regs)
{
    char host[2048];
    DIR *dir;
    struct dirent *de;
    unsigned char *buffer = (unsigned char *)regs->r[2];
    size_t used = 0;
    int wanted = regs->r[3], offset = regs->r[4], count = 0, index = 0;
    int capacity = regs->r[5];
    if (host_path((const char *)regs->r[1], host, sizeof(host)) != 0)
        return &swi_error;
    dir = opendir(host);
    if (dir == NULL) return &swi_error;
    while ((de = readdir(dir)) != NULL) {
        struct stat st;
        size_t name_len, record_len, padded;
        unsigned int load = 0, exec = 0;
        unsigned int *record;
        char full[4096];
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (index++ < offset) continue;
        if (count >= wanted) break;
        if (snprintf(full, sizeof(full), "%s/%s", host, de->d_name) < 0 ||
            stat(full, &st) != 0) { closedir(dir); return &swi_error; }
        name_len = strlen(de->d_name) + 1U;
        record_len = 20U + name_len;
        padded = (record_len + 3U) & ~(size_t)3U;
        if (padded > (size_t)capacity - used) break;
        if (st.st_mtime >= 0 &&
            (unsigned long long)st.st_mtime <= 0xFFFFFFFFULL)
            riscos_mtime_from_unix((bp_uint32)st.st_mtime, &load, &exec, 0);
        record = (unsigned int *)(buffer + used);
        record[0] = load;
        record[1] = exec;
        record[2] = (unsigned int)st.st_size;
        record[3] = S_ISDIR(st.st_mode) ? 0x33U : 0x03U;
        record[4] = S_ISDIR(st.st_mode) ? RISCOS_OBJ_DIR : RISCOS_OBJ_FILE;
        memcpy(record + 5, de->d_name, name_len);
        memset(buffer + used + record_len, 0, padded - record_len);
        used += padded;
        count++;
    }
    closedir(dir);
    regs->r[3] = count;
    regs->r[4] = count == 0 ? -1 : offset + count;
    if (count != 0) {
        /* Determine completion without consuming a second enumeration. */
        dir = opendir(host);
        if (dir == NULL) return &swi_error;
        index = 0;
        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) index++;
        }
        closedir(dir);
        if (regs->r[4] >= index) regs->r[4] = -1;
    }
    return NULL;
}

_kernel_oserror *_kernel_swi(int swi, _kernel_swi_regs *in,
                             _kernel_swi_regs *out)
{
    int fh;
    (void)out;
    if (swi == OS_Find) {
        if (in->r[0] == OS_Find_Close) {
            fh = in->r[1];
            if (fh <= 0 || fh >= 256 || handles[fh] == NULL)
                return &swi_error;
            if (fclose(handles[fh]) != 0) { handles[fh] = NULL; return &swi_error; }
            handles[fh] = NULL;
            return NULL;
        }
        in->r[0] = host_open((const char *)in->r[1], in->r[0]);
        return in->r[0] ? NULL : &swi_error;
    }
    if (swi == OS_GBPB) {
        size_t n;
        fh = in->r[1];
        if (in->r[0] == OS_GBPB_Enum) return host_enum_dir(in);
        if (fh <= 0 || fh >= 256 || handles[fh] == NULL) return &swi_error;
        if (in->r[0] == OS_GBPB_Write) {
            n = fwrite((const void *)in->r[2], 1, (size_t)in->r[3], handles[fh]);
            if (ferror(handles[fh])) return &swi_error;
            in->r[3] -= (int)n;
            return NULL;
        }
        if (in->r[0] == OS_GBPB_Read) {
            n = fread((void *)in->r[2], 1, (size_t)in->r[3], handles[fh]);
            if (ferror(handles[fh])) return &swi_error;
            in->r[3] -= (int)n;
            return NULL;
        }
    }
    if (swi == OS_Args) {
        long pos;
        fh = in->r[1];
        if (fh <= 0 || fh >= 256 || handles[fh] == NULL) return &swi_error;
        if (in->r[0] == OS_Args_ReadPos) {
            pos = ftell(handles[fh]);
            if (pos < 0) return &swi_error;
            in->r[2] = (int)pos;
            return NULL;
        }
        if (in->r[0] == OS_Args_ReadExt) {
            long old = ftell(handles[fh]);
            if (fseek(handles[fh], 0, SEEK_END) != 0) return &swi_error;
            pos = ftell(handles[fh]);
            if (old < 0 || pos < 0 || fseek(handles[fh], old, SEEK_SET) != 0)
                return &swi_error;
            in->r[2] = (int)pos;
            return NULL;
        }
        if (in->r[0] == OS_Args_SetPos)
            return fseek(handles[fh], in->r[2], SEEK_SET) == 0 ? NULL : &swi_error;
    }
    if (swi == OS_File && in->r[0] == OS_File_ReadCat)
        return host_file_info((const char *)in->r[1], in);
    if (swi == OS_GBPB || swi == OS_Args) return &swi_error;
    return &swi_error;
}

int main(void)
{
    char *create_args[] = { "archive", "tree" };
    char *append_args[] = { "archive", "new" };
    int rc;
    rc = cmd_create(2, create_args, BP_BLOCK_16K, BP_METHOD_BOLTLITE, 1);
    if (rc != 0) return 1;
    rc = cmd_add(2, append_args, BP_BLOCK_16K, BP_METHOD_BOLTLITE, 1);
    if (rc != 0) return 2;
    return 0;
}
