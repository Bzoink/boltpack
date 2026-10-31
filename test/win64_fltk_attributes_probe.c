#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    DWORD attributes;
    DWORD required;

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
        fwprintf(stderr, L"usage: attributes-probe.exe set|check PATH | move PATH DEST\n");
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
