BoltPack v{VERSION} - Sharp X68000 (Human68k)
=============================================

A lightweight file archiver for moving files between vintage and modern
computers. Archives made here extract on Linux, Windows, macOS, Atari,
Amiga, DOS and the other BoltPack systems, and the other way round.

FILES
-----

  boltpack.x         Command-line program (68000 assembly for BoltLZ)
  C_BUILD\           Plain C build of boltpack.x (see below)
  README.txt         This file
  RELEASE-NOTES.txt  Release notes
  LICENSE            License terms

COMMANDS
--------

  boltpack c ARCHIVE.BLT FILE1 DIR          Create (folders are recursive)
  boltpack a ARCHIVE.BLT FILE               Add files
  boltpack x ARCHIVE.BLT                    Extract
  boltpack x -o A:\OUT ARCHIVE.BLT          Extract into a folder
  boltpack l ARCHIVE.BLT                    List contents
  boltpack t ARCHIVE.BLT                    Test integrity
  boltpack i ARCHIVE.BLT                    Show archive information
  boltpack d ARCHIVE.BLT FILE               Delete files from the archive

Options:

  -0        Store only (no compression; the default is BoltLZ)
  -b SIZE   Block size: 8k, 16k, 32k, 64k or 256k
  -o PATH   Output folder for extraction
  -f        Overwrite existing files
  -n        Dry run
  -q, -qq   Quiet, silent
  --interchange-profile-1   Limits that every BoltPack reader accepts

Set the time zone so file times are stored correctly, in minutes east of
UTC, for example for Japan:

  SET BP_TZ_OFFSET=540

Without it, file times are recorded as unknown.

ASSEMBLY BUILD
--------------

boltpack.x contains hand-written 68000 assembly for BoltLZ, the default
compression method, and runs on every X68000 model including the X68030. On
other 68000 machines the assembly makes BoltLZ compression and decompression
each about twice as fast as the plain C build; X68000 speed has not been
measured. Other compression methods run at the same speed as in the C build.

C_BUILD\boltpack.x is the plain C build, without assembly. Use it if you
suspect the assembly code; archives are exactly the same.

X68000 NOTES
------------

* File names are stored as UTF-8 and converted to Shift-JIS on extraction.
  Names that Shift-JIS cannot represent are refused.
* FAT attributes are stored and restored. Modification times have
  two-second resolution; folder times are not restored.
* This build has been tested in the run68 Human68k emulator, not yet on a
  real X68000. Please report results.

LICENSE
-------

BoltPack is licensed under the BSD 2-Clause License. See LICENSE.

More documentation and source code: https://github.com/Bzoink/boltpack

Version: {VERSION}
Build date: {BUILD_DATE}
