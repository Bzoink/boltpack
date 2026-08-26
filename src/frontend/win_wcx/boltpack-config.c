/*
 * boltpack-config.c -- Configuration utility for BoltPack WCX plugin
 *
 * Allows users to configure the compression method used by the WCX plugin
 * via a simple dialog box or command-line interface. Stores settings in:
 *
 *   HKEY_CURRENT_USER\Software\BoltPack\CompressionMethod (DWORD)
 *
 * Build as a Win32 GUI executable (MinGW):
 *   i686-w64-mingw32-gcc -DWIN32_LEAN_AND_MEAN -o boltpack-config.exe \
 *     src/frontend/win_wcx/boltpack-config.c -luser32 -ladvapi32 -lshell32
 *
 * Usage (command-line):
 *   boltpack-config.exe [--get | --set 0|1|2|3|4]
 *
 * Or run without arguments to open the GUI dialog.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Released under the BSD 2-Clause License
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Registry helpers
 * ========================================================================== */

static int get_compression_method(void)
{
    HKEY hKey;
    DWORD method = 2;  /* default: fastlz */
    DWORD size = sizeof(DWORD);
    DWORD regType;
    LONG ret;

    ret = RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\BoltPack", 0,
                        KEY_READ, &hKey);
    if (ret != ERROR_SUCCESS) {
        return 2;
    }

    ret = RegQueryValueExA(hKey, "CompressionMethod", NULL, &regType,
                           (LPBYTE)&method, &size);

    RegCloseKey(hKey);

    if (ret != ERROR_SUCCESS) {
        return 2;
    }

    if (method > 4) {
        return 2;
    }

    return (int)method;
}

static int set_compression_method(int method)
{
    HKEY hKey;
    DWORD dwDisposition;
    DWORD dwMethod = (DWORD)method;
    LONG ret;

    ret = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\BoltPack", 0, NULL,
                          REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL,
                          &hKey, &dwDisposition);
    if (ret != ERROR_SUCCESS) {
        return -1;
    }

    ret = RegSetValueExA(hKey, "CompressionMethod", 0, REG_DWORD,
                         (const BYTE *)&dwMethod, sizeof(dwMethod));

    RegCloseKey(hKey);

    return (ret == ERROR_SUCCESS) ? 0 : -1;
}

static const char *method_name(int method)
{
    switch (method) {
    case 0: return "Stored (no compression)";
    case 1: return "LZSS (small-memory LZ77)";
    case 2: return "fastlz (fast, good ratio)";
    case 3: return "Fluff (LZ77+FSE/tANS, best ratio)";
    case 4: return "zstd (Zstandard, best ratio; modern 64-bit builds)";
    default: return "Unknown";
    }
}

/* ==========================================================================
 * Dialog box
 * ========================================================================== */

#define IDC_STORED   100
#define IDC_LZSS     101
#define IDC_FASTLZ  102
#define IDC_FLUFF    103
#define IDC_ZSTD     104
#define IDC_OK       IDOK
#define IDC_CANCEL   IDCANCEL

static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam)
{
    int method;

    switch (msg) {
    case WM_INITDIALOG:
        method = get_compression_method();
        switch (method) {
        case 0:
            SendDlgItemMessageA(hwnd, IDC_STORED, BM_SETCHECK, BST_CHECKED, 0);
            break;
        case 1:
            SendDlgItemMessageA(hwnd, IDC_LZSS, BM_SETCHECK, BST_CHECKED, 0);
            break;
        case 3:
            SendDlgItemMessageA(hwnd, IDC_FLUFF, BM_SETCHECK, BST_CHECKED, 0);
            break;
        case 4:
            SendDlgItemMessageA(hwnd, IDC_ZSTD, BM_SETCHECK, BST_CHECKED, 0);
            break;
        case 2:
        default:
            SendDlgItemMessageA(hwnd, IDC_FASTLZ, BM_SETCHECK, BST_CHECKED, 0);
            break;
        }
        return TRUE;

    case WM_COMMAND:
        if (wParam == IDC_OK) {
            if (SendDlgItemMessageA(hwnd, IDC_STORED, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                method = 0;
            } else if (SendDlgItemMessageA(hwnd, IDC_LZSS, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                method = 1;
            } else if (SendDlgItemMessageA(hwnd, IDC_FLUFF, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                method = 3;
            } else if (SendDlgItemMessageA(hwnd, IDC_ZSTD, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                method = 4;
            } else {
                method = 2; /* fastlz default */
            }

            if (set_compression_method(method) == 0) {
                MessageBoxA(hwnd, "Configuration saved successfully.", "BoltPack Config",
                            MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxA(hwnd, "Failed to save configuration to registry.", "Error",
                            MB_OK | MB_ICONERROR);
            }

            EndDialog(hwnd, IDOK);
            return TRUE;
        } else if (wParam == IDC_CANCEL) {
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;
    }

    return FALSE;
}

/* ==========================================================================
 * Main
 * ========================================================================== */

int main(int argc, char *argv[])
{
    int method;

    if (argc > 1) {
        if (strcmp(argv[1], "--get") == 0) {
            method = get_compression_method();
            printf("Current compression method: %d (%s)\n", method,
                   method_name(method));
            return 0;
        } else if (strcmp(argv[1], "--set") == 0 && argc > 2) {
            method = atoi(argv[2]);
            if (method < 0 || method > 4) {
                fprintf(stderr, "Error: compression method must be 0, 1, 2, 3, or 4\n");
                return 1;
            }
            if (set_compression_method(method) == 0) {
                printf("Compression method set to %d (%s)\n", method,
                       method_name(method));
                return 0;
            } else {
                fprintf(stderr, "Error: failed to set compression method\n");
                return 1;
            }
        } else {
            fprintf(stderr, "Usage:\n");
            fprintf(stderr, "  boltpack-config.exe              (open GUI)\n");
            fprintf(stderr, "  boltpack-config.exe --get       (print current setting)\n");
            fprintf(stderr, "  boltpack-config.exe --set 0|1|2|3|4 (set method)\n");
            fprintf(stderr, "\nCompression methods:\n");
            fprintf(stderr, "  0 = Stored (no compression)\n");
            fprintf(stderr, "  1 = LZSS (small-memory LZ77)\n");
            fprintf(stderr, "  2 = fastlz (default, fast)\n");
            fprintf(stderr, "  3 = Fluff (best compression ratio)\n");
            fprintf(stderr, "  4 = zstd (best ratio; modern 64-bit builds only)\n");
            return 1;
        }
    }

    /* No arguments: open dialog box */
    DialogBoxA(GetModuleHandleA(NULL),
               MAKEINTRESOURCEA(1000), NULL, DialogProc);

    return 0;
}

/* ==========================================================================
 * Resource template for dialog box (Windows RC format)
 *
 * To compile with the RC file, use:
 *   windres.exe boltpack-config.rc -o boltpack-config-res.o
 *   i686-w64-mingw32-gcc -o boltpack-config.exe \
 *       boltpack-config.c boltpack-config-res.o -luser32 -ladvapi32
 *
 * Alternatively, define IDD_COMPRESSION_CONFIG = 1000 in boltpack-config.rc
 * ========================================================================== */

/* Dialog template is in boltpack-config.rc */
