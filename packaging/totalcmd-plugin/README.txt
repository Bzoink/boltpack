BoltPack v1.2 - Total Commander Plugin
=======================================

BoltPack is a lightweight file archiver for archiving and transferring
files across different platforms and operating systems.

INSTALLATION
------------

Automatic (recommended):
  Double-click boltpack.wcx (or boltpack.wcx64) in Total Commander.
  Total Commander will prompt to install the plugin automatically.

Manual:
  1. Copy boltpack.wcx (32-bit) or boltpack.wcx64 (64-bit) to:
       C:\Program Files\totalcmd\plugins\wcx\boltpack\
  2. In Total Commander: Configuration > Options > Plugins > Packer plugins
  3. Click Add, select the .wcx / .wcx64 file
  4. Associate with extensions: bolt, blt

Configuration tool (boltpack-config.exe):
  Run boltpack-config.exe to set default compression method and level.

SUPPORTED EXTENSIONS
--------------------

  .bolt   Full-featured BoltPack archive
  .blt    Short alias for .bolt archives

COMPRESSION METHODS
-------------------

  Stored     No compression (fastest)
  LZSS       Compact LZ77; best for very low-memory targets
  boltlz    Fast general-purpose compression (default)
  Fluff      Best compression ratio (LZ77 + Huffman)

FEATURES
--------

  - 32-bit and 64-bit plugin variants included
  - Supports large files (>4 GB)
  - Unicode file names (UTF-8 internally)
  - Archive creation, extraction, and listing
  - Configurable compression level (1-9 for Fluff)

LICENSE
-------

See LICENSE file for terms.