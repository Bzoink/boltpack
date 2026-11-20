# BoltPack archive format

**Archive format 7** ([specification 2.0](specification-2.0.md)) is written
and read by every CLI: the full CLIs (`src/core/archiver7.c`) and the mini
frontends (16-bit DOS, Win16, OS/2 16-bit, CP/M-86 and Linux mini,
`src/core/mini/archiver.c`, Stored and BoltLite writing, BoltLZ reading,
blocks up to 32 KiB, volumes up to 2 GiB). The GUIs still use format 6.
Every archive is a set of one or more volume files:

- Each volume starts with a 32-byte volume header (magic `BOLT`, version 7,
  64-bit set ID, volume number, creator) and ends with a 32-byte trailer (set
  ID, number, last-volume flag, its own size).
- Entries are an entry header (with its own CRC-32), blocks, and a 16-byte
  entry trailer holding the compressed size and payload checksum, so the
  writer never revisits what it has written.
- The file table and a 48-byte end record are in the last volume. Table
  records locate entries by volume number and offset.
- No structure is split between volumes; a block at the end of a volume may be
  shortened to fill it.
- Volume names: `backup.000`, `backup.001`, ..., last `backup.bolt`. A
  single-volume archive is just `backup.bolt`.

The other frontends still use **archive format 6**, summarised below and
defined by [specification 1.0](specification-1.0.md). The two formats share
the codecs (methods 0-3) and block layout but are not interchangeable.

## Format 6

**Archive format:** 6, little-endian. Writers emit version 6 and readers
reject every other version. The [normative specification](specification-1.0.md)
defines every field and codec representation.

An archive contains a 32-byte header, entry headers and payloads, a file
table, and a 20-byte footer locating that table. The header carries CRC-32
values for itself and the file table. Each table record carries its method,
entry-header size, payload checksum, sizes and metadata; listing does not
need to seek through file data to recover these values.

| Method | Codec                              |
| ------ | ---------------------------------- |
| 0      | Stored                             |
| 1      | BoltLite                               |
| 2      | BoltLZ format 2; default Balanced  |
| 3      | Fluff                              |
| 4      | Retired (was Zstandard)             |

Data files are divided into independent blocks for BoltLZ, Stored and BoltLite.
A block has a 12-byte header: encoded size, original size and CRC-32 of the
encoded bytes. Equal encoded/original sizes mean verbatim data. Compressed
blocks saving less than 1% are stored. File checksums verify reconstructed
bytes across all blocks. Fluff can retain history between blocks as specified
in its codec section.

BoltLZ accepts blocks up to 256 KiB. Both encoder modes use the same decoder;
levels are not recorded in the archive. Block format byte `0x02` identifies
the BoltLZ representation inside method-2 compressed blocks. Empty files and
directories are Stored with no data blocks. Symlink payloads are stored raw.

The table header is 16 bytes and each record has a 43-byte fixed tail after
the name. All names use the platform-independent archive path conventions
in the specification. The archive layer validates safe extraction names,
checksums, declared lengths, and exact decompression sizes.
