#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    DWORD attributes;
    DWORD required;

    if (argc == 3 && wcscmp(argv[1], L"enumerate") == 0) {
        HANDLE directory;
        BYTE buffer[65536];
        FILE_INFO_BY_HANDLE_CLASS info_class = FileIdBothDirectoryRestartInfo;
        directory = CreateFileW(argv[2], FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (directory == INVALID_HANDLE_VALUE) {
            wprintf(L"open-error=%lu\n", GetLastError());
            return 1;
        }
        for (;;) {
            FILE_ID_BOTH_DIR_INFO *entry;
            if (!GetFileInformationByHandleEx(directory, info_class, buffer,
                                              sizeof(buffer))) {
                DWORD error = GetLastError();
                CloseHandle(directory);
                if (error == ERROR_NO_MORE_FILES) return 0;
                wprintf(L"enumerate-error=%lu\n", error);
                return 1;
            }
            entry = (FILE_ID_BOTH_DIR_INFO *)buffer;
            for (;;) {
                DWORD chars = entry->FileNameLength / sizeof(WCHAR);
                wprintf(L"name=%.*s id=%I64d attrs=%08lx size=%I64d write=%I64d\n",
                        (int)chars, entry->FileName, entry->FileId.QuadPart,
                        (unsigned long)entry->FileAttributes,
                        entry->EndOfFile.QuadPart, entry->LastWriteTime.QuadPart);
                if (entry->NextEntryOffset == 0) break;
                entry = (FILE_ID_BOTH_DIR_INFO *)((BYTE *)entry +
                                                  entry->NextEntryOffset);
            }
            info_class = FileIdBothDirectoryInfo;
        }
    }
    if (argc == 3 && wcscmp(argv[1], L"id") == 0) {
        BY_HANDLE_FILE_INFORMATION info;
        HANDLE file = CreateFileW(argv[2], FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE |
                                  FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS |
                                  FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (file == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(file, &info)) {
            wprintf(L"id-error=%lu\n", GetLastError());
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            return 1;
        }
        wprintf(L"volume=%08lx index=%08lx%08lx attrs=%08lx\n",
                (unsigned long)info.dwVolumeSerialNumber,
                (unsigned long)info.nFileIndexHigh,
                (unsigned long)info.nFileIndexLow,
                (unsigned long)info.dwFileAttributes);
        CloseHandle(file);
        return 0;
    }
    if (argc == 4 && wcscmp(argv[1], L"copytimes") == 0) {
        HANDLE original = CreateFileW(argv[2], FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE |
                                      FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, NULL);
        HANDLE replacement = CreateFileW(argv[3],
                                         FILE_READ_ATTRIBUTES |
                                         FILE_WRITE_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE |
                                         FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS, NULL);
        FILETIME creation, access, write;
        DWORD source_attributes;
        if (original == INVALID_HANDLE_VALUE ||
            replacement == INVALID_HANDLE_VALUE ||
            !GetFileTime(original, &creation, &access, &write)) {
            wprintf(L"copytimes-open-error=%lu\n", GetLastError());
            if (original != INVALID_HANDLE_VALUE) CloseHandle(original);
            if (replacement != INVALID_HANDLE_VALUE) CloseHandle(replacement);
            return 1;
        }
        source_attributes = GetFileAttributesW(argv[2]);
        if (source_attributes == INVALID_FILE_ATTRIBUTES ||
            !SetFileTime(replacement, &creation, &access, &write) ||
            !SetFileAttributesW(argv[3], source_attributes)) {
            wprintf(L"copytimes-set-error=%lu\n", GetLastError());
            CloseHandle(original);
            CloseHandle(replacement);
            return 1;
        }
        CloseHandle(original);
        CloseHandle(replacement);
        wprintf(L"times-and-attributes-copied\n");
        return 0;
    }
    if (argc == 4 && wcscmp(argv[1], L"same-metadata") == 0) {
        HANDLE first = CreateFileW(argv[2], FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE |
                                   FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, NULL);
        HANDLE second = CreateFileW(argv[3], FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS, NULL);
        BY_HANDLE_FILE_INFORMATION a, b;
        int equal;
        if (first == INVALID_HANDLE_VALUE || second == INVALID_HANDLE_VALUE ||
            !GetFileInformationByHandle(first, &a) ||
            !GetFileInformationByHandle(second, &b)) {
            wprintf(L"metadata-probe-error=%lu\n", GetLastError());
            if (first != INVALID_HANDLE_VALUE) CloseHandle(first);
            if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
            return 1;
        }
        equal = a.dwFileAttributes == b.dwFileAttributes &&
                a.nFileSizeHigh == b.nFileSizeHigh &&
                a.nFileSizeLow == b.nFileSizeLow &&
                CompareFileTime(&a.ftCreationTime, &b.ftCreationTime) == 0 &&
                CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
        wprintf(L"metadata_equal=%u id_equal=%u\n", equal,
                a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
                a.nFileIndexHigh == b.nFileIndexHigh &&
                a.nFileIndexLow == b.nFileIndexLow);
        CloseHandle(first);
        CloseHandle(second);
        return equal &&
               (a.dwVolumeSerialNumber != b.dwVolumeSerialNumber ||
                a.nFileIndexHigh != b.nFileIndexHigh ||
                a.nFileIndexLow != b.nFileIndexLow) ? 0 : 1;
    }
    if (argc == 4 && wcscmp(argv[1], L"move") == 0) {
        if (MoveFileExW(argv[2], argv[3], MOVEFILE_WRITE_THROUGH)) {
            wprintf(L"move=succeeded\n");
            return 0;
        }
        wprintf(L"move=refused error=%lu\n", GetLastError());
        return 1;
    }
    if (argc != 3 ||
        (wcscmp(argv[1], L"set") != 0 && wcscmp(argv[1], L"check") != 0)) {
        fwprintf(stderr, L"usage: attributes-probe.exe set|check PATH | move PATH DEST | enumerate DIR | copytimes SOURCE DEST | same-metadata PATH PATH\n");
        return 2;
    }
    attributes = GetFileAttributesW(argv[2]);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        fwprintf(stderr, L"GetFileAttributesW failed: %lu\n", GetLastError());
        return 1;
    }
    required = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        required |= FILE_ATTRIBUTE_ARCHIVE;
    if (wcscmp(argv[1], L"set") == 0) {
        attributes |= required;
        if (!SetFileAttributesW(argv[2], attributes)) {
            fwprintf(stderr, L"SetFileAttributesW failed: %lu\n", GetLastError());
            return 1;
        }
        attributes = GetFileAttributesW(argv[2]);
        if (attributes == INVALID_FILE_ATTRIBUTES) return 1;
    }
    wprintf(L"attributes=%08lx hidden=%u system=%u archive=%u\n",
            (unsigned long)attributes,
            (attributes & FILE_ATTRIBUTE_HIDDEN) != 0,
            (attributes & FILE_ATTRIBUTE_SYSTEM) != 0,
            (attributes & FILE_ATTRIBUTE_ARCHIVE) != 0);
    return (attributes & required) == required ? 0 : 1;
}
