/*
 * help_text.h -- shared in-app documentation for BoltPack GUI frontends
 *
 * This header is intentionally C89-compatible so it can be included
 * from C, C++, and Objective-C frontends alike.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#ifndef BP_HELP_TEXT_H
#define BP_HELP_TEXT_H

#define BP_STRINGIFY2(x) #x
#define BP_STRINGIFY(x) BP_STRINGIFY2(x)
#define BP_APP_NAME      "BoltPack"
#define BP_APP_VERSION   BP_STRINGIFY(BP_VERSION_MAJOR) "." BP_STRINGIFY(BP_VERSION_MINOR)
#define BP_COPYRIGHT     "Copyright (C) 2026 Tony Mattsson"
#define BP_LICENSE       "Released under the GNU General Public License v2"
#define BP_DESCRIPTION   "A lightweight file archiver for vintage and modern systems"

static const char bp_help_text[] =
    "BoltPack - Quick Guide\n"
    "======================\n"
    "\n"
    "BoltPack is a lightweight file archiver designed for maximum\n"
    "portability across vintage and modern systems.\n"
    "\n"
    "Opening an Archive\n"
    "-------------------\n"
    "Use Archive > Open to select a .bolt or .blt file.\n"
    "The main window will display the archive contents\n"
    "including file names, sizes, compression ratios,\n"
    "and timestamps.\n"
    "\n"
    "You can also drag and drop a .bolt file onto the\n"
    "main window to open it.\n"
    "\n"
    "Extracting Files\n"
    "-----------------\n"
    "With an archive open, use Archive > Extract Files.\n"
    "Select a destination folder and the files will be\n"
    "extracted. A progress window shows the current file\n"
    "and overall progress.\n"
    "\n"
    "Creating an Archive\n"
    "--------------------\n"
    "Use Archive > Create New to select files and folders\n"
    "to archive. Choose a destination and file name for\n"
    "the new archive. A progress window shows the packing\n"
    "progress.\n"
    "\n"
    "You can also drag and drop files or folders onto the\n"
    "main window to create a new archive from them.\n"
    "\n"
    "Drag and Drop\n"
    "--------------\n"
    "Drop .bolt/.blt files to open them.\n"
    "Drop other files or folders to create a new archive.\n"
    "\n"
    "Compression Methods\n"
    "--------------------\n"
    "BoltPack supports four compression methods:\n"
    "  Stored  - No compression\n"
    "  LZSS    - Small-memory LZ77 for 16-bit systems\n"
    "  miniLZO - Fast general-purpose (default)\n"
    "  fluff   - Best compression ratio\n"
    "\n"
    "Keyboard Shortcuts\n"
    "-------------------\n"
    "  Ctrl+O / Cmd+O  - Open archive\n"
    "  Ctrl+N / Cmd+N  - Create new archive\n"
    "  Ctrl+Q / Cmd+Q  - Quit\n";

#endif /* BP_HELP_TEXT_H */
