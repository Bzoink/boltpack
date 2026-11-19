BoltPack v{VERSION}
====================

A lightweight file archiver for moving files and programs between vintage
and modern computers.

ABOUT
-----

BoltPack stores and transfers data and programs across platforms, from an
8088 PC running DOS to a modern x86-64 Linux machine, including AmigaOS,
Atari TOS, Classic Mac OS, RISC OS, Haiku, Plan 9 and many more.

  * Compression methods: Stored, BoltLite, boltlz and Fluff (the methods
    available depend on the build)
  * Portable C89 core; the same archive format on every system
  * File names stored as UTF-8 and converted to the local character set
  * Native file information (permissions, attributes, file types) kept
    where the system has it

QUICK START
-----------

  boltpack c myarchive.bolt file1 file2 folder    Create (recursive)
  boltpack x myarchive.bolt                       Extract
  boltpack x -o outdir myarchive.bolt             Extract into a folder
  boltpack l myarchive.bolt                       List contents
  boltpack t myarchive.bolt                       Test integrity
  boltpack i myarchive.bolt                       Show archive information

Run boltpack without arguments for the full list of commands and options
in this build.

COMMON OPTIONS
--------------

  -0        Store only (no compression)
  -1        BoltLite (smallest memory use)
  -2        boltlz (fast)
  -F        Fluff (best ratio)
  -b SIZE   Block size: 8k, 16k, 32k, 64k or 256k
  -o PATH   Output folder for extraction
  -f        Overwrite existing files
  -n        Dry run
  -q, -qq   Quiet, silent

64-bit builds use Fluff by default; 32-bit and vintage builds use boltlz.
Use --interchange-profile-1 for archives that every BoltPack reader,
including the smallest ones, must accept.

TROUBLESHOOTING
---------------

Archive will not extract: run "boltpack t archive.bolt" to find the damaged
file, and check that the archive was transferred in binary mode.

Extraction is slow on an old machine: the archive probably uses Fluff.
Create it again with -2 (boltlz).

File names look wrong: names are stored as UTF-8. Characters the local
system cannot represent are replaced on extraction.

LICENSE
-------

BoltPack is licensed under the BSD 2-Clause License. See LICENSE.

Documentation, source code and bug reports:
  https://github.com/Bzoink/boltpack

Version: {VERSION}
Build date: {BUILD_DATE}
