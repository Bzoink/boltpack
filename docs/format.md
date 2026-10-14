# BoltPack archive format

**Archive format:** 5, little-endian. Writers emit version 5 and readers
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
| 4      | Zstandard on enabled modern builds |

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

The table header is 16 bytes and each record has a 41-byte fixed tail after
the name. All names use the platform-independent archive path conventions
in the specification. The archive layer validates safe extraction names,
checksums, declared lengths, and exact decompression sizes.
