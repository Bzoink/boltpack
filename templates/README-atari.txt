BoltPack v{VERSION} - Atari ST/STE/TT/Falcon
=============================================

A lightweight file archiver for moving files between vintage and modern
computers. Archives made here extract on Linux, Windows, macOS, Amiga, DOS
and the other BoltPack systems, and the other way round.

FILES
-----

  BOLTPACK.PRG   GEM program (windows, menus, file selector)
  BOLTPACK.TOS   Command-line program (for a shell, or from the desktop
                 with "TOS takes parameters")
  README.TXT     This file
  RELNOTES.TXT   Release notes
  LICENSE        License terms

Requirements: TOS 1.04 or later (tested in Hatari with TOS 1.06, 1.62 and
2.05). 512 KB RAM works; 1 MB or more is recommended for larger archives.

GEM PROGRAM
-----------

Double-click BOLTPACK.PRG.

  Archive > Open...          Open an archive and list its contents
  Archive > Extract...       Unpack the open archive into a folder
  Archive > Validate         Check every file in the open archive
  Archive > Information      Method, block size and number of files
  Compression > Create New   Choose files or a folder for a new archive;
                             the method ticked in the same menu is used

New archives are written to a temporary file (BPxxxxxx) next to the archive
and renamed only when complete, so an existing archive is never damaged by a
failed or cancelled Create. If Create fails, the alert names any unfinished
temporary file that was kept; it can be deleted.

COMMAND LINE
------------

  boltpack.tos c ARCHIVE.BLT FILE1 FOLDER   Create (folders are recursive)
  boltpack.tos a ARCHIVE.BLT FILE           Add files
  boltpack.tos x ARCHIVE.BLT                Extract
  boltpack.tos x -o D:\OUT ARCHIVE.BLT      Extract into a folder
  boltpack.tos l ARCHIVE.BLT                List contents
  boltpack.tos t ARCHIVE.BLT                Test integrity
  boltpack.tos i ARCHIVE.BLT                Show archive information
  boltpack.tos d ARCHIVE.BLT FILE           Delete files from the archive

Options:

  -0        Store only (no compression)
  -1        BoltLite (smallest memory use)
  -F        Fluff (best ratio, slow on a 68000)
            Without -0, -1 or -F, boltlz is used (fast).
  -b SIZE   Block size: 8k, 16k, 32k, 64k or 256k. Chosen from free RAM
            when not given.
  -o PATH   Output folder for extraction
  -f        Overwrite existing files
  -n        Dry run
  -q, -qq   Quiet, silent
  --interchange-profile-1   Limits that every BoltPack reader accepts

Approximate speed on an 8 MHz 68000: boltlz 27 KB/s, BoltLite and Fluff
about 1 KB/s. Use boltlz on an ST unless size matters most.

ATARI NOTES
-----------

* File names must fit 8.3. Atari ST accented letters are kept. Names with
  spaces, control characters or : / \ ? * cannot be created on TOS; when
  making an archive elsewhere for an Atari, use --target-profile=atari-st.
* TOS attributes (read-only, hidden, system, archive) are stored and
  restored. Modification times have two-second resolution.
* File names are stored as UTF-8 and converted to the Atari ST character
  set on extraction.

LICENSE
-------

BoltPack is licensed under the BSD 2-Clause License. See LICENSE.

More documentation and source code: https://github.com/Bzoink/boltpack

Version: {VERSION}
Build date: {BUILD_DATE}
