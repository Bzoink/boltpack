BoltPack v{VERSION} - RISC OS
==============================

A portable file archiver for cross-system transfers.

INSTALLATION
------------

1. Copy the boltpack application to your chosen location
   (e.g., Utilities folder, or create an app directory)

2. Ensure correct file type is set (0xFF8 for absolute executable)
   The binary should already have this set, but verify if needed:
   - Use *Type or FileType utility if binaries show as Data type

QUICK START
-----------

RISC OS Command Line (via ShellCLI or Filer):

  Run the boltpack tool:
  *boltpack c myarchive blt file1 file2

Extract:
  *boltpack x myarchive blt

List:
  *boltpack l myarchive blt

Show info:
  *boltpack i myarchive blt

RISC OS NAMING CONVENTION
--------------------------

Note: On RISC OS, file extensions use forward slashes:
  myarchive/blt        (equivalent to myarchive.blt on Unix)
  file/txt

The archiver accepts both notations:
  boltpack c "myarchive blt" file1 file2
  OR
  boltpack c myarchive/blt file1 file2

COMPRESSION
-----------

-F              Fluff (best compression)
-M 1            miniLZO (fastest)
-0              Stored (no compression)

RISC OS FEATURES
----------------

File types:
  - Preserves RISC OS file types on extraction
  - Archives created as type 0xFFC (ZIP-like)
  - Can be opened with archive managers supporting ZIP

Protection bits:
  - Preserves RISC OS protection bits (R/W/D/X)
  - Restores execute bits automatically

Long file names:
  - Supports full UTF-8 encoding
  - RISC OS automatically converts display where needed

CREATING ARCHIVES
-----------------

Easiest way in Filer:
  1. Select files/folders
  2. Open a ShellCLI window
  3. Type: *boltpack c archive blt <selected files>

Or use drag-drop (if your desktop supports it):
  *boltpack c archive blt {drag files here}

EXTRACTING ARCHIVES
-------------------

From ShellCLI:
  *boltpack x archive blt

This creates a "archive" folder with contents.

MEMORY REQUIREMENTS
-------------------

Memory needed depends on compression method:

- Stored (no compression):        ~8 KB (minimal)
- miniLZO (default):              ~64 KB free memory
- Fluff (best compression):       ~256 KB free memory during decompression

Block sizes scale from 8 KB (memory-constrained systems) to 256 KB (modern).

Suitable for RISC OS 3.1+ on machines with 2 MB+ RAM (miniLZO) or 4 MB+ RAM (fluff).

TROUBLESHOOTING
---------------

"File type 0xFF8 not found":
  The binary should already have correct type. If not, set with:
  *Type boltpack 0xFF8

"Permission denied / Protected":
  RISC OS may prevent execution of network files.
  Copy to local drive: *Copy archive <destination>

"Cannot run as application":
  Ensures file type is 0xFF8 (see above)
  Or use from ShellCLI with: *boltpack ...

COMPRESSION COMPARISONS
-----------------------

Fluff (-F):      29% ratio, best for archive storage
miniLZO (-M 1):  47% ratio, fastest decompression
Stored (-0):     100% size, instant (for pre-compressed)

Example:
  *boltpack c -F bigfile blt largefile.iso    (best storage)

See generic README for full documentation.

---

Version: {VERSION}
Build: {BUILD_DATE}
Platform: RISC OS / ARM
