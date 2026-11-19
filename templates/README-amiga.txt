BoltPack v{VERSION} - Amiga
============================

A lightweight file archiver for cross-platform file transfer.

INSTALLATION
------------

1. Copy the unpacked files to your chosen location (for example, SYS:Utilities/BoltPack)
2. Keep each .info file beside its matching program:
  - boltpack.info beside boltpack
  - boltpack-gui.info beside boltpack-gui
3. Set the executable bit if needed (via Protection dialog)

QUICK START
-----------

Shell/CLI:

  boltpack c myarchive.blt file1 file2

Extract:
  boltpack x myarchive.blt

List:
  boltpack l myarchive.blt

COMPRESSION
-----------

-F          Use Fluff (best for big screens)
            boltlz is the default method
-0          No compression

AMIGA NOTES
-----------

File types:
  - Archives created as .blt type
  - Restore original file types on extraction (if available)
  - Some vintage systems may not preserve file types; use archive info

Memory:
  - Requires ~256 KB free RAM during decompression
  - Safe on systems with 512 KB+ RAM
  - On memory-constrained systems, use smaller block sizes

Protection bits:
  - Archive bit is preserved
  - Execute bits are maintained where possible
  - File notes are preserved in comments

WORKBENCH
---------

The package includes Workbench icons for both Amiga executables.

Workbench usage:
  1. Double-click boltpack-gui to open the GadTools GUI
  2. Double-click boltpack to open a Shell window and display CLI usage
  3. For normal command-line work, run boltpack from Shell with arguments

OPTIMIZED VERSIONS
------------------

boltpack and boltpack-gui are the standard build and run on every 68k
Amiga. The Optimized drawer holds the same two programs with hand-written
68000 assembly for BoltLZ, the default compression method:

  Optimized/68000/   any 68k Amiga (A500, A600, A2000, CDTV, ...)
  Optimized/68020/   68020 to 68060 only (A1200, A3000, A4000, accelerators)

To use one, copy boltpack and/or boltpack-gui from the drawer for your CPU
over the standard ones; keep the existing .info icons. Archives are exactly
the same as with the standard build. Measured in an emulator on mixed
files, BoltLZ compression and decompression are each about twice as fast;
the 68020 version adds 10-20% on a 68030. Other compression methods run at
the standard speed. The 68020 version does not run on a 68000.

MULTIVIEW / ICON CLICKING
--------------------------

Archives are not opened directly by double-clicking them. Use boltpack-gui
or the Shell/CLI tool for archive operations.

COMPATIBILITY
--------------

Works on:
  - AmigaOS 1.x, 2.x, 3.x (68k CPU)
  - AROS
  - MorphOS
  - AmigaOne with AmigaOS 4.x

File names:
  - Unicode (UTF-8) supported
  - RISC OS type preservation
  - Long filenames supported

TROUBLESHOOTING
---------------

"Not enough memory":
  Try with smaller files first, or ensure adequate free RAM

File type not preserved:
  Use 'boltpack i' to check original type
  Manually set with Tools menu if needed

See README for full documentation.

---

Version: {VERSION}
Build: {BUILD_DATE}
Platform: Amiga 68k
