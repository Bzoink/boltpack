BoltPack v{VERSION} for Sinclair QL
===================================

BoltPack is a portable file archiver for moving files between vintage and
modern systems.

This package contains the Sinclair QL / SMSQ-E command line build.

FILES
-----

boltpack           Main executable (68000 assembly for BoltLZ)
C_BUILD/           Plain C build of boltpack (see ASSEMBLY BUILD)
README.txt         This file
LICENSE            License information
RELEASE-NOTES.txt  Release notes

QUICK START
-----------

Create an archive:
  boltpack c archive.blt win1_docs_letter_txt

Create with Fluff compression:
  boltpack c -F archive.blt win1_docs_

Extract an archive:
  boltpack x archive.blt

Extract to another directory:
  boltpack x -o ram1_tmp_ archive.blt

List contents:
  boltpack l archive.blt

Show archive information:
  boltpack i archive.blt

Test an archive:
  boltpack t archive.blt

Delete entries from an archive:
  boltpack d archive.blt docs/letter.txt

COMMANDS
--------

boltpack c [options] archive.blt files...   Create archive
boltpack a [options] archive.blt files...   Add files to archive
boltpack x [options] archive.blt            Extract archive
boltpack l archive.blt                      List contents
boltpack t archive.blt                      Test archive
boltpack i archive.blt                      Show archive info
boltpack d archive.blt names...             Delete entries

OPTIONS
-------

-b SIZE   Block size: 8k, 16k, 32k, 64k, 256k
-0        Store files without compression
-F        Use Fluff compression
-o PATH   Output path for extraction
-f        Overwrite existing files on extract
-n        Dry run
-q        Quiet output
-qq       Silent output

ASSEMBLY BUILD
--------------

boltpack contains hand-written 68000 assembly for BoltLZ, the default
compression method. It runs on every QL and QL-compatible (68008, Gold Card,
Super Gold Card, Q68, QPC2). Measured in an emulator at original QL speed on
mixed files: BoltLZ compression about 7 KB/s instead of about 1.3 KB/s,
decompression about 49 KB/s instead of about 14 KB/s with the plain C build.
Other compression methods run at the same speed as in the C build.

C_BUILD/boltpack is the plain C build, without assembly. Use it if you
suspect the assembly code; archives are exactly the same.

QL NOTES
--------

- QDOS device prefixes such as win1_ and flp1_ are stripped from archive names.
- QDOS directories end with an underscore, for example win1_docs_.
- Archive paths always use forward slashes internally.
- File names are stored as UTF-8 in the archive and converted to local Latin-1.

COMPRESSION
-----------

Default method: boltlz

-0        No compression
-F        Fluff compression

For low-memory systems, use smaller block sizes such as -b 8k or -b 16k.

SUPPORT
-------

Project home:
  https://github.com/your-repo/boltpack

Version: {VERSION}
Build date: {BUILD_DATE}
Architecture: {ARCH}
