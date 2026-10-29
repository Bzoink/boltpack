/* Exercise the public Total Commander Unicode entry points through LoadLibrary. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WCX_PK_OM_LIST 0
#define WCX_PK_SKIP 0
#define WCX_PK_EXTRACT 2
#define WCX_E_SUCCESS 0
#define WCX_E_NOT_SUPPORTED 24
#define WCX_E_BAD_ARCHIVE 13
#define WCX_E_END_ARCHIVE 10

typedef struct {
    WCHAR *ArcName;
    int OpenMode;
    int OpenResult;
    WCHAR *CmtBuf;
    int CmtBufSize;
    int CmtSize;
    int CmtState;
} open_data_w;

typedef struct {
    char *ArcName;
    int OpenMode;
    int OpenResult;
    char *CmtBuf;
    int CmtBufSize;
    int CmtSize;
    int CmtState;
} open_data_a;

typedef struct {
    WCHAR ArcName[1024];
    WCHAR FileName[1024];
    int Flags;
    unsigned int PackSize;
    unsigned int PackSizeHigh;
    unsigned int UnpSize;
    unsigned int UnpSizeHigh;
    int HostOS;
    int FileCRC;
    int FileTime;
    int UnpVer;
    int Method;
    int FileAttr;
    char *CmtBuf;
    int CmtBufSize;
    int CmtSize;
    int CmtState;
    char Reserved[1024];
} header_data_ex_w;

typedef struct {
    char ArcName[1024];
    char FileName[1024];
    int Flags;
    unsigned int PackSize;
    unsigned int PackSizeHigh;
    unsigned int UnpSize;
    unsigned int UnpSizeHigh;
    int HostOS;
    int FileCRC;
    int FileTime;
    int UnpVer;
    int Method;
    int FileAttr;
    char *CmtBuf;
    int CmtBufSize;
    int CmtSize;
    int CmtState;
    char Reserved[1024];
} header_data_ex_a;

typedef HANDLE (WINAPI *open_archive_a_fn)(open_data_a *);
typedef HANDLE (WINAPI *open_archive_w_fn)(open_data_w *);
typedef int (WINAPI *read_header_ex_a_fn)(HANDLE, header_data_ex_a *);
typedef int (WINAPI *read_header_ex_w_fn)(HANDLE, header_data_ex_w *);
typedef int (WINAPI *process_file_w_fn)(HANDLE, int, WCHAR *, WCHAR *);
typedef int (WINAPI *close_archive_fn)(HANDLE);
typedef int (WINAPI *pack_files_w_fn)(WCHAR *, WCHAR *, WCHAR *, WCHAR *, int);
typedef int (WINAPI *delete_files_w_fn)(WCHAR *, WCHAR *);

static int append_path(WCHAR *out, size_t capacity, const WCHAR *root,
                       const WCHAR *suffix)
{
    size_t root_length = wcslen(root), suffix_length = wcslen(suffix);
    if (root_length + suffix_length + 1 > capacity) return 0;
    wcscpy(out, root);
    wcscat(out, suffix);
    return 1;
}

static int report(const char *message, DWORD error)
{
    fprintf(stderr, "FAIL: %s (error=%lu)\n", message,
            (unsigned long)error);
    return 1;
}

int wmain(int argc, wchar_t **argv)
{
    HMODULE module;
    open_archive_a_fn open_archive_a;
    open_archive_w_fn open_archive;
    read_header_ex_a_fn read_header_a;
    read_header_ex_w_fn read_header;
    process_file_w_fn process_file;
    close_archive_fn close_archive;
    pack_files_w_fn pack_files;
    delete_files_w_fn delete_files;
    WCHAR dll_path[1024], root[1024], archive_path[1024], ansi_archive_path[1024];
    WCHAR packed_archive_path[1024], pack_source_path[1024];
    WCHAR directory_archive_path[1024], directory_extract_path[1024];
    WCHAR extract_path[1024], nested_path[1024], output_path[1024];
    WCHAR expected_name[] = L"nested\\\x6771\x4EAC-\xD83D\xDE42.txt";
    WCHAR add_list[] = L"nested\\\x6771\x4EAC-\xD83D\xDE42.txt\0";
    WCHAR append_list[] = L"extra-\x6771\x4EAC.txt\0";
    WCHAR delete_list[] = L"nested\\\x6771\x4EAC-\xD83D\xDE42.txt\0";
    char acp_probe[1024];
    BOOL used_default = FALSE;
    open_data_w open_data;
    header_data_ex_w header;
    HANDLE archive;
    DWORD process_error;
    int ansi_refusal_checked = 0;
    int rc;

    if (argc != 3) {
        fprintf(stderr, "usage: wcx_unicode_wine <plugin.dll> <ascii-root>\n");
        return 2;
    }
    if (wcslen(argv[1]) >= sizeof(dll_path) / sizeof(dll_path[0]) ||
        wcslen(argv[2]) >= sizeof(root) / sizeof(root[0]))
        return report("test path too long", ERROR_BUFFER_OVERFLOW);
    wcscpy(dll_path, argv[1]);
    wcscpy(root, argv[2]);

    if (!append_path(archive_path, 1024, root,
                     L"\\archive-\xD83D\xDCBE.blt") ||
        !append_path(ansi_archive_path, 1024, root,
                     L"\\archive-ascii.blt") ||
        !append_path(packed_archive_path, 1024, root,
                     L"\\packed-\xD83D\xDCBE.blt") ||
        !append_path(directory_archive_path, 1024, root,
                     L"\\directory-archive.blt") ||
        !append_path(pack_source_path, 1024, root, L"\\pack-source") ||
        !append_path(directory_extract_path, 1024, root,
                     L"\\directory-extract") ||
        !append_path(extract_path, 1024, root, L"\\extract") ||
        !append_path(nested_path, 1024, root, L"\\extract\\nested") ||
        !append_path(output_path, 1024, root,
                     L"\\extract\\nested\\\x6771\x4EAC-\xD83D\xDE42.txt"))
        return report("build test paths", ERROR_BUFFER_OVERFLOW);

    module = LoadLibraryW(dll_path);
    if (module == NULL) return report("LoadLibraryW plugin", GetLastError());
    open_archive = (open_archive_w_fn)GetProcAddress(module, "OpenArchiveW");
    open_archive_a = (open_archive_a_fn)GetProcAddress(module, "OpenArchive");
    read_header = (read_header_ex_w_fn)GetProcAddress(module, "ReadHeaderExW");
    read_header_a = (read_header_ex_a_fn)GetProcAddress(module, "ReadHeaderEx");
    process_file = (process_file_w_fn)GetProcAddress(module, "ProcessFileW");
    close_archive = (close_archive_fn)GetProcAddress(module, "CloseArchive");
    pack_files = (pack_files_w_fn)GetProcAddress(module, "PackFilesW");
    delete_files = (delete_files_w_fn)GetProcAddress(module, "DeleteFilesW");
    if (open_archive == NULL || open_archive_a == NULL || read_header == NULL ||
        read_header_a == NULL || process_file == NULL || close_archive == NULL ||
        pack_files == NULL || delete_files == NULL)
        return report("resolve Unicode WCX exports", GetLastError());

    if (!CreateDirectoryW(extract_path, NULL) ||
        !CreateDirectoryW(nested_path, NULL))
        return report("create Unicode extraction parents", GetLastError());
    memset(&open_data, 0, sizeof(open_data));
    open_data.ArcName = archive_path;
    open_data.OpenMode = WCX_PK_OM_LIST;
    archive = open_archive(&open_data);
    if (archive == NULL || open_data.OpenResult != 0)
        return report("OpenArchiveW", GetLastError());

    memset(&header, 0, sizeof(header));
    rc = read_header(archive, &header);
    while (rc == 0 && wcscmp(header.FileName, expected_name) != 0) {
        rc = process_file(archive, WCX_PK_SKIP, NULL, NULL);
        if (rc != 0) break;
        memset(&header, 0, sizeof(header));
        rc = read_header(archive, &header);
    }
    if (rc != 0) {
        fwprintf(stderr, L"ReadHeaderExW did not return expected UTF-16 name; "
                 L"last name=[%ls]\n", header.FileName);
        close_archive(archive);
        return report("ReadHeaderExW returned an incorrect UTF-16 name",
                      (DWORD)rc);
    }

    rc = process_file(archive, WCX_PK_EXTRACT, NULL, output_path);
    process_error = GetLastError();
    if (rc != WCX_E_SUCCESS && rc != WCX_E_NOT_SUPPORTED) {
        close_archive(archive);
        fprintf(stderr, "ProcessFileW rc=%d ", rc);
        return report("ProcessFileW", process_error);
    }
    rc = close_archive(archive);
    if (rc != WCX_E_SUCCESS) return report("CloseArchive", (DWORD)rc);

    /* Directory timestamps are intentionally deferred until CloseArchive so
     * extracting children cannot overwrite the archived parent time. */
    if (!CreateDirectoryW(directory_extract_path, NULL))
        return report("create directory extraction root", GetLastError());
    memset(&open_data, 0, sizeof(open_data));
    open_data.ArcName = directory_archive_path;
    open_data.OpenMode = WCX_PK_OM_LIST;
    archive = open_archive(&open_data);
    if (archive == NULL || open_data.OpenResult != 0)
        return report("OpenArchiveW directory fixture", GetLastError());
    for (;;) {
        memset(&header, 0, sizeof(header));
        rc = read_header(archive, &header);
        if (rc == WCX_E_END_ARCHIVE) break;
        if (rc != WCX_E_SUCCESS) {
            close_archive(archive);
            return report("ReadHeaderExW directory fixture", (DWORD)rc);
        }
        rc = process_file(archive, WCX_PK_EXTRACT, directory_extract_path, NULL);
        if (rc != WCX_E_SUCCESS && rc != WCX_E_NOT_SUPPORTED) {
            close_archive(archive);
            return report("ProcessFileW directory fixture", (DWORD)rc);
        }
    }
    rc = close_archive(archive);
    if (rc != WCX_E_SUCCESS) return report("CloseArchive directory fixture", (DWORD)rc);

    if (GetACP() != CP_UTF8 &&
        (WideCharToMultiByte(GetACP(), WC_NO_BEST_FIT_CHARS, expected_name, -1,
                             acp_probe, sizeof(acp_probe), NULL,
                             &used_default) == 0 || !used_default))
        return report("entry name is not outside active ANSI code page", GetACP());

    if (GetACP() != CP_UTF8) {
        char ansi_archive[1024];
        open_data_a ansi_open;
        header_data_ex_a ansi_header;
        HANDLE ansi_handle;
        int ansi_rc, i;
        for (i = 0; ansi_archive_path[i] != L'\0'; ++i) {
            if (ansi_archive_path[i] > 0x7f || i >= (int)sizeof(ansi_archive) - 1)
                return report("ASCII alias path is not ASCII", ERROR_INVALID_NAME);
            ansi_archive[i] = (char)ansi_archive_path[i];
        }
        ansi_archive[i] = '\0';
        memset(&ansi_open, 0, sizeof(ansi_open));
        ansi_open.ArcName = ansi_archive;
        ansi_open.OpenMode = WCX_PK_OM_LIST;
        ansi_handle = open_archive_a(&ansi_open);
        if (ansi_handle == NULL || ansi_open.OpenResult != 0)
            return report("OpenArchive ANSI alias", GetLastError());
        memset(&ansi_header, 0, sizeof(ansi_header));
        ansi_rc = read_header_a(ansi_handle, &ansi_header);
        while (ansi_rc == 0 && strcmp(ansi_header.FileName, "nested\\") == 0) {
            if (process_file(ansi_handle, WCX_PK_SKIP, NULL, NULL) != WCX_E_SUCCESS)
                break;
            memset(&ansi_header, 0, sizeof(ansi_header));
            ansi_rc = read_header_a(ansi_handle, &ansi_header);
        }
        close_archive(ansi_handle);
        if (ansi_rc != WCX_E_BAD_ARCHIVE)
            return report("ANSI header did not refuse lossy name", (DWORD)ansi_rc);
        ansi_refusal_checked = 1;
    } else puts("SKIP: active ACP is UTF-8; legacy ANSI refusal is not applicable");

    rc = pack_files(packed_archive_path, NULL, pack_source_path, add_list, 2);
    if (rc != WCX_E_SUCCESS && rc != WCX_E_NOT_SUPPORTED)
        return report("PackFilesW", (DWORD)rc);
    rc = pack_files(packed_archive_path, NULL, pack_source_path, append_list, 2);
    if (rc != WCX_E_SUCCESS && rc != WCX_E_NOT_SUPPORTED)
        return report("PackFilesW append", (DWORD)rc);
    rc = delete_files(archive_path, delete_list);
    if (rc != WCX_E_SUCCESS && rc != WCX_E_NOT_SUPPORTED)
        return report("DeleteFilesW on Linux-created archive", (DWORD)rc);
    rc = delete_files(packed_archive_path, delete_list);
    if (rc != WCX_E_SUCCESS && rc != WCX_E_NOT_SUPPORTED)
        return report("DeleteFilesW", (DWORD)rc);

    FreeLibrary(module);
    puts(ansi_refusal_checked
         ? "WCX Unicode open/list/extract/pack/append/delete passed; ANSI header refused non-ACP name"
         : "WCX Unicode open/list/extract/pack/append/delete passed");
    return 0;
}
