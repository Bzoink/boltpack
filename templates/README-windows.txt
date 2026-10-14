BoltPack v{VERSION} - Windows
==============================

A lightweight file archiver for archiving and transferring files.

INSTALLATION
------------

1. Extract boltpack.exe from this archive
2. (Optional) Add to System PATH for command-line use:
   - Right-click Computer > Properties > Advanced System Settings
   - Environment Variables > System variables > Path > Edit
   - Add folder where boltpack.exe is located

QUICK START
-----------

Command Prompt or PowerShell:

  boltpack c myarchive.blt file1.txt file2.doc

Extract:
  boltpack x myarchive.blt

List files:
  boltpack l myarchive.blt

GET INFO:
  boltpack i myarchive.blt

COMPRESSION
-----------

Default: Fast (boltlz, 47% ratio)

Options:
  -F, --fluff         Best compression (29% ratio)
  -2, --boltlz        Fast decompression (47% ratio)
  -1, --lzss          Small-memory compression
  -Z, --zstd          Best general-purpose ratio
  -0, --stored        No compression

Example:
  boltpack c -F backup.blt folder\documents\*

FEATURES
--------

- Small executable: ~{BINARY_SIZE}
- No installation needed (portable)
- Supports large files (64-bit)
- Unicode file names (UTF-8)
- Five compression methods

WINDOWS INTEGRATION
-------------------

Total Commander Plugin:
  An optional WCX plugin (boltpack.wcx / boltpack.wcx64) provides
  integration with Total Commander. See plugin documentation.

Explorer Context Menu:
  Windows shell integration not included. Use command line or
  third-party archiver frontend.

PYTHON BINDING:
  For integration with Python scripts, see documentation.

TROUBLESHOOTING
---------------

"Is not recognized as an internal or external command":
  Add the folder to System PATH (see Installation section above)
  OR run with full path: C:\path\to\boltpack.exe ...

Archive is corrupted:
  Verify file integrity (Windows -> Properties)
  Re-download from official source

Can't extract: Permission denied
  Ensure destination folder is writable
  Run Command Prompt as Administrator if extracting system-protected folders

PROPERTIES
----------

- Platform: Windows {ARCH} (x86-64 or x86)
- No system dependencies (standalone executable)
- Works on Windows XP, Vista, 7, 8, 10, 11
- All system code pages supported for file names

See generic README for full documentation.

---

Version: {VERSION}
Build: {BUILD_DATE}
