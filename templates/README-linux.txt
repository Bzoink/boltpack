BoltPack v{VERSION} - Linux
=============================

A lightweight file archiver for portability across vintage and modern systems.

INSTALLATION
------------

1. Make the binary executable:
   chmod +x boltpack

2. (Optional) Install to system PATH:
   sudo cp boltpack /usr/local/bin/

USAGE
-----

Create an archive:
  ./boltpack c myarchive.blt file1 file2 folder/

Extract:
  ./boltpack x myarchive.blt

List:
  ./boltpack l myarchive.blt

Show info:
  ./boltpack i myarchive.blt

COMPRESSION OPTIONS
-------------------

-F, --fluff    Use Fluff codec (best ratio, 29%)
-2, --boltlz   Use boltlz (fast, 47%)
-1, --lzss     Use LZSS (small-memory)
-Z, --zstd     Use zstd (best general-purpose ratio)
-0             No compression (stored)

Example:
  ./boltpack c -F myarchive.blt largefile.iso  (best compression)

COMMAND-LINE OPTIONS
--------------------

-r, --recursive    Include directories recursively
-q, --quiet        Suppress progress messages
-h, --help         Show help

ADVANCED USAGE
--------------

Compress entire directory:
  ./boltpack c -r -F backup.blt /home/user/documents/

Extract to specific location:
  mkdir extracted
  cd extracted
  ../boltpack x ../archive.blt

PROPERTIES
----------

- Platform: Linux x{ARCH}
- Binary size: ~{BINARY_SIZE}
- Supports files > 4 GB
- Stores file permissions, ownership (when extracting as same user)
- Full Unicode path support (UTF-8)

TROUBLESHOOTING
---------------

Permission denied:
  chmod +x ./boltpack

Out of memory on large files:
  BoltPack uses ~256 KB per decompression block. No issue on modern Linux.

See README for full documentation.

---

Version: {VERSION}
Build: {BUILD_DATE}
