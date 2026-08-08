BoltPack v{VERSION} UQLX/sqlux QL container
===========================================

This directory is a UQLX/sqlux QDOS datastore prepared for Sinclair QL
emulators and transfer workflows.

FILES
-----

boltpack             Main executable
readme_txt           This file
license_txt          License information
release_notes_txt    Release notes

USE ON HOST
-----------

1. Extract the tar.gz archive on the host system.
2. Keep the directory marker file named .-UQLX- in place.
3. Point UQLX/sqlux or another compatible QDOS datastore tool at the
   extracted directory.

USE ON QL
---------

Run commands such as:

  boltpack l ram1_test_blt
  boltpack x ram1_test_blt
  boltpack c archive_blt win1_docs_letter_txt

QL NOTES
--------

- The executable has been copied through qcp so its dataspace is preserved.
- QDOS device prefixes such as win1_ are stripped from archive names.
- QDOS directories end with an underscore.
- Archive paths always use forward slashes internally.

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

Project home:
  https://github.com/your-repo/boltpack

Version: {VERSION}
Build date: {BUILD_DATE}
Architecture: {ARCH}
