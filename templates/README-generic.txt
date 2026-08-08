BoltPack v{VERSION}
====================

A lightweight file archiver for portability across vintage and modern computer systems.

ABOUT
-----

BoltPack is designed to store and transfer data and programs across different
platforms — from an 8088 PC running DOS to a modern x86-64 Linux box, including
AmigaOS, Atari TOS, Classic Mac OS, RISC OS, Haiku, Plan 9, and many more.

Key features:
  * Three compression methods: Stored, miniLZO (fast), Fluff (best compression)
  * Cross-platform compatibility: strict ANSI C89 implementation
  * Small binaries: ~50 KB standalone executable
  * 64-bit file support and block-based compression
  * Unicode file names (stored as UTF-8, displayed in native encoding)

QUICK START
-----------

Create an archive:
  boltpack c myarchive.blt file1 file2 folder/

Create with compression:
  boltpack c -F myarchive.blt file1 file2    (best ratio)
  boltpack c -M 1 myarchive.blt file1        (fast decompression)
  boltpack c -0 myarchive.blt file1          (no compression)

Extract:
  boltpack x myarchive.blt

List contents:
  boltpack l myarchive.blt

Show archive info:
  boltpack i myarchive.blt

COMMANDS
--------

boltpack c [options] <archive> <files...>     Create archive
boltpack x [options] <archive>                 Extract archive
boltpack l [options] <archive>                 List contents
boltpack i <archive>                           Show archive info

OPTIONS
-------

-F, --fluff           Use Fluff codec (best compression, 29%)
-M 1                  Use miniLZO (fast, 47%)
-0, --stored          No compression (stored)
-r, --recursive       Include directories recursively
-q, --quiet           Suppress progress messages

COMPRESSION METHODS
-------------------

Stored (0):     No compression, instant speed
miniLZO (1):    Fast (730 MB/s decompress), 47% ratio, good compatibility
Fluff (2):      Best ratio (29%), modern systems

Default: miniLZO for maximum speed

LICENSE
-------

BoltPack is licensed under the GNU General Public License v2 (GPLv2).
See the included LICENSE file for full details.

DOCUMENTATION
--------------

For complete documentation, usage examples, and platform-specific notes:
  * Online: https://github.com/your-repo/boltpack
  * Local: See RELEASE-NOTES.txt and accompanying documentation

PLATFORMS
---------

BoltPack has been built and tested on 30+ platforms including:

  Modern:      Linux, Windows, macOS, FreeBSD, NetBSD, OpenBSD
  Vintage:     Amiga, Atari TOS, RISC OS, DOS, OS/2, Classic Mac OS,
               Haiku, Plan 9, Solaris, HP-UX, AIX, IRIX

Support matrix and build details are available in RELEASE-NOTES.txt.

TROUBLESHOOTING
---------------

Q: Archive is corrupted or won't extract
A: Ensure the .bolt file is intact (check file size). Try listing first:
   boltpack l archive.blt

Q: Decompression is slow
A: If using Fluff (-F), this is normal on slow systems. Use miniLZO:
   boltpack c -M 1 archive.blt <files>

Q: File names appear garbled
A: BoltPack stores all paths as UTF-8. If your system uses a different
   character set (e.g., Shift-JIS on Japanese systems), the extraction
   will convert automatically. Check your system's locale settings.

Q: Can't execute the binary
A: Verify file permissions (on Unix-like systems):
   chmod +x boltpack
   On Amiga/RISC OS, may require setting file type/protection bits.

FEEDBACK & SUPPORT
------------------

For bug reports, feature requests, and platform support:
  https://github.com/your-repo/boltpack/issues

Source code is available at:
  https://github.com/your-repo/boltpack

---

Version: {VERSION}
Build date: {BUILD_DATE}
