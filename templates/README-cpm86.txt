BoltPack mini - CP/M-86 (BoltPack v{VERSION})
=============================================

A small file archiver for CP/M-86 on 8086-class machines. Archives
made here extract on Linux, Windows, macOS, DOS and the other BoltPack
systems, and the other way round.

FILES
-----

  BPMINI.CMD       Command-line program (8086 assembly BoltLite decoder)
  C_BUILD\         Plain C build of BPMINI.CMD (see below)
  README.TXT       This file
  RELNOTES.TXT     Release notes
  LICENSE          License terms

Copy BPMINI.CMD to a CP/M-86 disk. The files are plain text with CRLF line
endings, and the names are 8.3.

COMMANDS
--------

  BPMINI c <archive> <file>...   create an archive (files only, no folders)
  BPMINI x <archive>             extract (flat, 8.3 names)
  BPMINI l <archive>             list contents
  BPMINI t <archive>             test integrity
  BPMINI i <archive>             show information

Options (create only):

  -b8k, -b16k, -b32k   block size (default 16 KiB)
  -m0                  store, no compression
  -m1                  BoltLite compression (default)

Extraction understands stored, BoltLite and BoltLZ blocks, so archives made
with the other BoltPack programs extract here if they use those methods (not
Fluff). Make an archive elsewhere for a CP/M-86 machine with
--interchange-profile-1 if you are unsure.

CP/M-86 NOTES
-------------

* CP/M-86 has no folders. Only flat 8.3 file names are accepted: overlong
  names, unsupported characters, names that differ only in case and names that
  would overwrite an existing file are refused. Extraction creates only exact
  upper-case 8.3 names; archives with folders are extracted partially and the
  rest is reported.
* CP/M-86 has no file time API, so modification times are neither read nor
  restored.
* The default block size is 16 KiB so that the encoder's buffers fit one far
  heap segment; 32 KiB blocks need a runtime that can allocate several.
* This build was tested in the emu2-cpm86 emulator against the Linux program
  (archives exchanged both ways, and the assembly and C builds compared). It
  has not yet been run on real hardware. Please report results.

LICENSE
-------

BoltPack is licensed under the BSD 2-Clause License. See LICENSE.
