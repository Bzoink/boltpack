# BoltPack Architecture

## Design Principles

1. **Strict layer separation** — each layer has a single responsibility and a clean interface
2. **No platform-specific code in the core** — platform details live only in the frontend
3. **ANSI C (C89)** throughout — no compiler extensions in the core or archiver layers

## Layers

```text
┌─────────────────────────────────┐
│         Frontend (CLI)          │  ← argument parsing, console I/O, file system access
├─────────────────────────────────┤
│        Archiver Core            │  ← packing/unpacking, archive format logic
├─────────────────────────────────┤
│      Compression Layer          │  ← BoltLite, boltlz, fluff codec engines
└─────────────────────────────────┘
```

### Layer 1: Compression Layer

**Responsibility:** Compress and decompress a single block of data.

This layer wraps the supported codecs behind a thin abstraction so that the archiver and frontends can select a compression method without depending directly on third-party APIs.

**Interface (conceptual):**

```c
/* Initialize the compression subsystem. Call once at startup. */
int bp_compress_init(void);

/* Compress a block.
 * src/src_len  — input data
 * dst/dst_len  — output buffer (caller allocates), dst_len updated with actual size
 * Returns 0 on success. */
int bp_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                      bp_uint8 *dst, bp_uint32 *dst_len);

/* Decompress a block.
 * src/src_len  — compressed data
 * dst/dst_len  — output buffer (caller allocates with original size), dst_len updated
 * Returns 0 on success. */
int bp_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                        bp_uint8 *dst, bp_uint32 *dst_len);

/* Compute CRC-32 over a data block.
 * Pass crc=0 for the first call; chain the return value for streaming. */
bp_uint32 bp_crc32(bp_uint32 crc, const bp_uint8 *data, bp_uint32 len);
```

The current implementation supports five archive methods: stored raw blocks
(method `0`), BoltLite (method `1`), boltlz (method `2`), Fluff (method `3`), and
zstd (method `4`) on modern builds.
The archiver core decides which codec to invoke for a block and may still store
an individual block raw when a compressed method misses the 99% threshold.

### Error Codes

All archiver core functions return an `int` error code. Zero means success; non-zero means failure. The frontend should check the return value and report the error to the user.

```c
enum {
    BP_OK                    =  0,  /* Success */
    BP_ERR_IO                = -1,  /* Read/write/seek failed */
    BP_ERR_NOMEM             = -2,  /* malloc() returned NULL */
    BP_ERR_MAGIC             = -3,  /* Bad magic bytes (not a .bolt file) */
    BP_ERR_VERSION           = -4,  /* Archive format version is not supported */
    BP_ERR_CHECKSUM          = -5,  /* CRC-32 mismatch on decompressed data */
    BP_ERR_CORRUPT           = -6,  /* Structural corruption (bad block size, impossible offset, etc.) */
    BP_ERR_TRUNCATED         = -7,  /* Unexpected end of file */
    BP_ERR_OVERFLOW          = -8,  /* File or archive exceeds platform limits (e.g., >2 GB on 32-bit I/O) */
    BP_ERR_NAME_TOO_LONG     = -9,  /* File name exceeds 65493 encoded bytes */
    BP_ERR_UNSUPPORTED_METHOD= -10, /* Unknown compression method */
    BP_ERR_COMPRESS          = -11, /* Compression backend call failed */
    BP_ERR_DECOMPRESS        = -12, /* Decompression backend call failed */
    BP_ERR_ABORTED           = -13, /* User cancelled via progress callback */
    BP_ERR_NOT_FOUND         = -14, /* Named entry not found in archive */
    BP_ERR_INVALID_ARG       = -15  /* NULL pointer or invalid parameter */
};
```

The frontend can convert these to human-readable messages via a simple lookup table or switch. The core never prints to `stdout`/`stderr` — all user communication goes through the frontend.

### Layer 2: Archiver Core

**Responsibility:** Read and write `.bolt` archive files. Manages the archive header, file table, and per-file compression/decompression.

This layer works with **abstract I/O callbacks** so it does not call `fopen`/`fread` directly. The frontend provides I/O functions, making it possible to adapt the archiver to non-POSIX file systems (e.g., classic Mac OS, Amiga, DOS).

**Interface (conceptual):**

```c
/* I/O callbacks provided by the frontend */
typedef struct {
    /* Read up to 'count' bytes into 'buf'. Return bytes read, or -1 on error. */
    long     (*read)(void *handle, void *buf, bp_uint32 count);
    /* Write 'count' bytes from 'buf'. Return bytes written, or -1 on error. */
    long     (*write)(void *handle, const void *buf, bp_uint32 count);
    /* Seek to 'offset' from origin. Return 0 on success. */
    int      (*seek)(void *handle, bp_off_t offset, int origin);
    /* Return current position, or -1 on error. */
    bp_off_t (*tell)(void *handle);
    void    *handle;  /* opaque context (e.g. FILE *, Amiga BPTR) */
} bp_io_t;

/* Progress information passed to the frontend during archive/extract */
typedef struct {
    bp_uint32     file_index;       /* Current file number (0-based) */
    bp_uint32     file_count;       /* Total number of files */
    const char   *file_name;        /* Name of the current file */
    bp_uint64     file_orig_size;   /* Original size of current file */
    bp_uint64     file_bytes_done;  /* Bytes processed so far for current file */
    bp_uint64     file_comp_size;   /* Compressed bytes written so far for current file */
    bp_uint64     archive_orig_total;  /* Total original bytes across all files so far */
    bp_uint64     archive_comp_total;  /* Total compressed bytes across all files so far */
    bp_uint64     archive_orig_done;   /* Original bytes of fully-completed entries */
} bp_progress_t;

/* Progress callback. Called by the archiver core after each block.
 * The frontend uses this to display status. Return 0 to continue, non-zero to abort. */
typedef int (*bp_progress_fn)(const bp_progress_t *progress, void *user_data);

/* Create a new archive (block_size: one of BP_BLOCK_*, method: BP_METHOD_*). */
int bp_archive_create(bp_archive_t *ar, bp_io_t *io,
                      bp_uint32 block_size, bp_uint8 method);

/* Add an entry (file, directory, symlink, resource fork, metadata). */
int bp_archive_add_entry(bp_archive_t *ar, const char *name,
                         bp_uint8 entry_type, bp_uint8 platform,
                         bp_io_t *file_io, bp_uint64 file_size,
                         bp_uint64 mtime);

/* Finalize: write file table, footer, patch header. */
int bp_archive_finalize(bp_archive_t *ar);

/* Open an existing archive for in-place append. */
int bp_archive_open_append(bp_archive_t *ar, bp_io_t *io,
                           bp_uint32 block_size, bp_uint8 method);

/* Copy all entries from src_io into ar (raw-copy, no decompress).
 * Call between bp_archive_create and bp_archive_finalize. */
int bp_archive_copy_from(bp_archive_t *ar, bp_io_t *src_io);

/* Free internal resources. */
void bp_archive_close(bp_archive_t *ar);

/* List contents — calls fn for each entry. */
int bp_archive_list(bp_io_t *io, bp_list_fn fn, void *user_data);

/* Extract all entries — open_fn provides output bp_io_t per entry. */
int bp_archive_extract_all(bp_io_t *io,
                           bp_extract_fn open_fn,
                           bp_extract_done_fn done_fn,
                           bp_progress_fn progress_fn,
                           void *user_data);

/* Test archive integrity. */
int bp_archive_test(bp_io_t *io, bp_test_fn fn, void *user_data);

/* Test archive integrity with progress reporting.
 * progress_fn may be NULL. Return non-zero from progress_fn to abort. */
int bp_archive_test_progress(bp_io_t *io, bp_test_fn fn, void *user_data,
                             bp_progress_fn progress_fn,
                             void *progress_data);

/* Read summary info from archive header and file table. */
int bp_archive_info(bp_io_t *io, bp_archive_info_t *info);

/* Delete entries — reads from src_io, writes new archive to dst_io. */
int bp_archive_delete(bp_io_t *src_io, bp_io_t *dst_io,
                      const char **del_names, bp_uint32 del_count,
                      bp_uint32 *out_deleted);
```

### Progress Reporting

The archiver core calls the progress callback **after each compressed block**, giving the frontend fine-grained updates even for large files. The frontend decides how to present this information.

**What the frontend can compute from `bp_progress_t`:**

- **File x of y**: `file_index + 1` of `file_count`
- **% of current file**: `file_bytes_done * 100 / file_orig_size`
- **Current file compression ratio**: `file_comp_size` vs `file_bytes_done`
- **Overall archive compression ratio**: `archive_comp_total` vs `archive_orig_total`

**Example CLI output during creation (default):**

```text
   creating: MyApp/
   creating: MyApp/Resources/
compressing: MyApp/Resources/sounds.aiff  (1.8:1)
compressing: MyApp/Resources/icon.png  (stored)
compressing: MyApp/main  (2.4:1)
17 files, 3 directories, 1,204,736 bytes, ratio 2.1:1, 0.38s, 3.02 MB/s
```

**Example CLI output during extraction (default):**

```text
   creating: MyApp/
   creating: MyApp/Resources/
 extracting: MyApp/Resources/sounds.aiff
 extracting: MyApp/Resources/icon.png
 extracting: MyApp/main
17 files, 3 directories, 1,204,736 bytes, 0.12s, 9.57 MB/s
```

**With `-q` (quiet):**

```text
 34% [6/17]
```

A single updating line showing overall percentage and file count. The summary line is still printed at the end.

**With `-qq` (silent):** No output at all unless an error occurs.

The callback returns an `int` so the frontend can signal **abort** (e.g., if the user presses Ctrl-C and the frontend catches it). Return `0` to continue, non-zero to stop.

**Summary line:** After the operation completes (unless `-qq`), the frontend prints a summary with total size, compression ratio (create only), elapsed time, and throughput in the most readable unit:

```text
17 files, 3 directories, 1,204,736 bytes, ratio 2.1:1, 0.38s, 3.02 MB/s
```

Unit selection: bytes/s below 1024, KB/s below 1048576, MB/s above. The frontend records the wall-clock start time (platform-specific: `clock()`, `gettimeofday()`, etc.) and computes throughput from `archive_orig_total` (uncompressed bytes processed) divided by elapsed seconds. On systems where sub-second timing is unavailable, the frontend prints seconds only (no fractional part) and omits throughput if elapsed rounds to zero.

The progress callback is optional. Passing `NULL` disables it — useful for non-interactive use or very constrained systems where the callback overhead is unwanted.

### Layer 3: Frontend (CLI)

**Responsibility:** Parse command-line arguments, interact with the file system, recurse directories, report progress and errors to the user.

This is the **only layer** that uses platform-specific headers (`<stdio.h>` for file I/O, `<dirent.h>` or platform equivalents for directory listing, etc.).

The CLI frontend implements the `bp_io_t` callbacks using standard C file functions and passes them to the archiver core.

**Recursive by default:** When a directory is given as an argument, the frontend recurses into it automatically. No `-r` flag needed. This is the common case — archiving a folder of files should just work.

**Commands:**

| Command                                   | Description                                                                      |
| ----------------------------------------- | -------------------------------------------------------------------------------- |
| `boltpack c archive.bolt file1 dir1 ...`  | Create archive (directories are recursed automatically)                          |
| `boltpack x archive.bolt [file ...]`      | Extract (recreates directory structure)                                          |
| `boltpack l archive.bolt`                 | List archive contents                                                            |
| `boltpack t archive.bolt`                 | Test archive integrity                                                           |
| `boltpack a archive.bolt file1 dir1 ...`  | Append files to an existing archive                                              |
| `boltpack i archive.bolt`                 | Show archive information (format version, entry count, sizes, compression ratio) |
| `boltpack d archive.bolt file1 dir1/ ...` | Delete entries from archive                                                      |

**Options:**

- **`-b SIZE`** (`c`, `a`): Set block size: `8k`–`256k`. Overrides auto-detection.
- **`-0`** (`c`, `a`): Store only — skip compression.
- **`-F`** (`c`, `a`): Use Fluff compression. When no `-b` is given, block size is automatically set to 256 KB to match Fluff's window size.
- **`-1`** (`c`, `a`): Use BoltLite compression.
- **`-2`** (`c`, `a`): Use boltlz compression. `-l 1` selects level 1; `-l 2` selects level 2; omitting `-l` selects Balanced.
- **`-l N` / `--level N`** (`c`, `a`): Select an exact codec level: BoltLZ 1–2, Fluff 1–9, or zstd 1–9. Fluff defaults to 8 and zstd to 3.
- **`--light`, `--standard`, `--best`** (`c`, `a`): Select quality presets. They map to BoltLZ 1/2/2, Fluff 1/8/9, and zstd 1/3/9 respectively. Standard matches each codec's normal default.
- **`-q`** (all): Quiet — show only progress.
- **`-qq`** (all): Silent — suppress all output.
- **`-o PATH`** (`x`): Output directory for extraction.
- **`-f`** (`x`): Force overwrite existing files.
- **`-n`** (`c`, `x`): Dry run — don't write anything.

**Usage examples:**

```sh
boltpack c backup.bolt MyApp                Pack entire MyApp folder recursively
boltpack c docs.bolt readme.txt manual/     Mix files and directories
boltpack c -b 8k small.bolt data/           Force 8 KB blocks (low-memory system)
boltpack c -0 images.bolt photos/           Store without compression (already-compressed data)
boltpack c -F sources.bolt src/             Use Fluff LZ77 + FSE/tANS compression
boltpack a backup.bolt extra_file.txt       Append a file to an existing archive
boltpack x backup.bolt                      Extract everything, recreating directories
boltpack x backup.bolt MyApp/config.txt     Extract a single file
boltpack l backup.bolt                      List all files with sizes and types
boltpack t backup.bolt                      Verify archive structure and checksums
boltpack i backup.bolt                      Show format version, entry count, compression ratio
boltpack d backup.bolt MyApp/config.txt     Remove an entry from the archive
```

**Directory recursion is handled entirely in the frontend.** The archiver core has no concept of directories — it just receives entries one at a time. The frontend walks the directory tree (using `opendir`/`readdir` on Unix, `FindFirstFile`/`FindNextFile` on Windows, etc.) and feeds entries to the archiver in this order: **directory entries first, then files within.** This is a depth-first walk — for each directory encountered, emit an `entry_type=4` entry for the directory itself, then recurse into its contents. This guarantees directories exist in the archive before their children.

On extraction, the frontend processes entries in archive order. Directory entries (`entry_type=4`) create the directory and apply metadata. File entries create parent directories implicitly if somehow missing (defensive), then write the file.

### Block Size Selection

The frontend selects the block size at archive creation time and passes it to the archiver core. The archiver core itself has no knowledge of available memory — block size is an input, not a decision.

**Priority order:**

1. **User override** — if the user passes `-b SIZE`, use that. No questions asked.
2. **Auto-detect** — if no `-b` flag, the frontend queries available memory via a platform-specific API and picks the largest block size that fits comfortably.

Auto-detection APIs per platform:

| Platform      | API                                       |
| ------------- | ----------------------------------------- |
| Linux         | `sysinfo()` or read `/proc/meminfo`       |
| macOS / BSDs  | `sysctl()` with `HW_PHYSMEM`              |
| AmigaOS       | `AvailMem(MEMF_LARGEST)`                  |
| DOS (DJGPP)   | `_go32_dpmi_remaining_physical_memory()`  |
| DOS (Turbo C) | `farcoreleft()`                           |
| Windows       | `GlobalMemoryStatus()`                    |
| RISC OS       | `OS_ReadDynamicArea`                      |
| Atari TOS     | `Malloc(-1)` (returns largest free block) |
| BeOS / Haiku  | `get_system_info()`                       |

The frontend maps available memory to block size using the recommended table (see format spec). If auto-detection fails or is not implemented for a platform, default to **32 KB** — safe for any system with 256 KB free.

### Compression Decisions

The archiver core makes per-block decisions about whether to compress or store.
It keeps compressed data only when it saves at least 1% of the input block:

1. Compress the block via `bp_compress_block()`
2. If `compressed_size` is at least `original_size - floor(original_size / 100)`, discard the compressed output and store the block as-is (method 0)
3. Otherwise, write the compressed block (method 1)

This is automatic and per-block — a single file can have a mix of compressed and stored blocks (e.g., a mostly-text file with an embedded JPEG section).

If the user passes `-0` (store-only), the archiver skips `bp_compress_block()` entirely for every block and writes all data as method 0. This is useful for archives of already-compressed data (JPEGs, MP3s, ZIPs) where attempting boltlz on every block wastes CPU time.

### Test Command (`boltpack t`)

`boltpack t archive.bolt` performs a full integrity check without writing any files to disk. It verifies:

1. **Archive header** — magic bytes (`BOLT`), format version exactly 5
2. **Each file entry** — decompress all blocks, compute CRC-32 of the decompressed data, compare to stored `checksum`
3. **File table** — magic bytes (`TBLT`), `entry_count` matches header `file_count`
4. **Archive footer** — magic bytes (`TLOB`), `table_offset` points to valid file table

**Output:**

```text
testing: apps/MyApp (12345 bytes) OK
testing: apps/MyApp [resource fork] OK
testing: docs/readme.txt (4096 bytes) OK
testing: docs/notes.txt (1024 bytes) FAILED — checksum mismatch

3 files OK, 1 file FAILED
```

Exit code: `0` if all files pass, non-zero if any check fails. The frontend reports each failure as it occurs so the user doesn't have to wait for the entire archive.

### Appending Files (`boltpack a`)

`boltpack a archive.bolt files...` appends new entries to an existing archive. The procedure:

1. Read the footer to find the file table offset
2. Seek to that offset (where the old file table begins)
3. Write new file entries starting at that position, overwriting the old file table
4. Write a new file table covering all entries (old + new)
5. Write a new footer
6. Update the header's `file_count` and `archive_size`

This requires one read (footer), one seek, and then sequential writes. The cost is minimal — no existing entry data is moved or rewritten.

The `boltpack d` command rebuilds the archive without the specified entries, raw-copying all retained entry data without decompression. A directory name ending with `/` removes all entries under that directory.

## Memory Budget

BoltPack uses block-based compression so that memory usage is bounded and predictable, regardless of how large the files being archived are.

Per-archive memory during BoltLZ compression:

- One input block and an output buffer of `n + floor(n / 63) + 16` bytes.
- A reusable 64 KiB heap workspace that covers both encoder modes.
- No large compressor table on the stack.

The standalone Fast encoder requires 32 KiB workspace and Balanced 64 KiB.
At blocks up to 64 KiB, the implementation uses packed 16-bit entries;
public allocation sizes cover larger blocks as well. Decoding needs no
separate workspace. Both modes can reference the entire independent block,
up to 256 KiB. Larger blocks often improve ratio, but the benefit depends
on the data and the codec being compared. Block headers contain sizes and
CRC-32 and occupy 12 bytes.

The file table is read into memory for listing and extraction. For archives with huge numbers of files on low-memory systems, the archiver could fall back to sequential scanning (reading entry headers one by one), but the table approach is preferred.

## Platform-Specific Frontend Concerns

### Core Principle: Extract What You Understand, Ignore What You Don't

The **archiver core** iterates over entries and hands each one to the **frontend** via a callback. The frontend decides per-entry what to do based on `entry_type` and `platform`. The core never skips entries — that decision belongs to the frontend.

### Extraction Behavior Matrix

| Archive created on           | Mac OS 9        | Amiga        | Linux               | DOS             |
| :--------------------------- | :-------------- | :----------- | :------------------ | :-------------- |
| **Mac** (data + rsrc + meta) | Full restore    | Data only    | Data, skip perms    | Data            |
| **Amiga** (data + meta)      | Data, skip meta | Full restore | Data, skip meta     | Data            |
| **Linux** (data + meta)      | Data, skip meta | Data         | Data — full restore | Data, skip meta |
| **DOS** (data + meta)        | Data, skip meta | Data         | Data, skip meta     | Full restore    |
| **RISC OS** (data + meta)    | Data, skip meta | Data         | Data, skip meta     | Data            |

**Rules for every frontend:**

1. **`entry_type=0` (data)** — ALWAYS extract. This is the file itself.
2. **`entry_type=1` (resource fork)** — Extract only if the frontend knows how to attach a resource fork (Mac frontend). All others **skip** by default.
3. **`entry_type=2` (metadata)** — Apply if `platform` matches the host or the host's family. Otherwise **skip**. Family matching means: a FreeBSD frontend can apply Linux metadata (both are Unix family), an AROS frontend can apply AmigaOS metadata, etc.
4. **`entry_type=3` (symlink)** — Create a symbolic link if the OS supports it (Unix, AmigaOS 2+, Haiku). Otherwise **skip** with a message.
5. **`entry_type=4` (directory)** — ALWAYS create. Apply metadata (permissions, timestamps) if `platform` matches or is compatible. On extraction, directories are created before their contents because directory entries appear first in the archive (see below).

"Skip" means: the archiver core still reads past the entry (to maintain stream position), but the frontend provides no output target, so nothing is written to disk.

### What "Skip" Looks Like to the User

When listing (`boltpack l`), all entries are shown regardless of platform, with a type indicator:

```text
  Size   Compressed  Type  Name
     0           0   DIR   apps/
 12345        8901   DATA  apps/MyApp
 67890       45678   RSRC  apps/MyApp
    18          18   META  apps/MyApp
     0           0   DIR   docs/
  4096        3200   DATA  docs/readme.txt
```

When extracting, skipped entries produce an informational message (not an error):

```text
   creating: apps/
  extracting: apps/MyApp (12345 bytes)
    skipping: apps/MyApp [resource fork - not supported on this platform]
    skipping: apps/MyApp [Mac metadata - not supported on this platform]
   creating: docs/
  extracting: docs/readme.txt (4096 bytes)
```

This way the user always knows what was in the archive and what was actually restored.

### Creating Archives: What Each Frontend Stores

| Frontend        | Entries created per file                                |
| :-------------- | :------------------------------------------------------ |
| **Linux/Unix**  | data + Unix perms                                       |
| **Mac OS X**    | data + resource fork (if present) + perms + Finder info |
| **Mac OS 9**    | data + resource fork (if non-empty) + Finder info       |
| **Amiga**       | data + protection bits & comment                        |
| **DOS/Win/OS2** | data + file attributes                                  |
| **RISC OS**     | data + filetype & load/exec addr                        |
| **Atari TOS**   | data + TOS attributes                                   |
| **BeOS/Haiku**  | data + perms + MIME type                                |

### Platform Frontend Details

#### Classic Mac OS (resource forks)

The Mac frontend must:

- Read both the **data fork** and **resource fork** of each file (via `FSpOpenDF`/`FSpOpenRF` or FSSpec APIs)
- Store them as separate archive entries with `entry_type=0` and `entry_type=1`
- Read Finder metadata (Type, Creator, flags) via `FSpGetFInfo` and store as `entry_type=2`
- On extraction: recreate both forks and restore Finder metadata via `FSpSetFInfo`
- On extraction of a non-Mac archive: just write the data fork (file works, just no Mac-specific attributes)

The **archiver core** is completely unaware of resource forks. It just stores entries.

#### Amiga (file comments, protection bits)

AmigaOS files have protection bits (HSPARWED) and optional file comments. The Amiga frontend stores these as `entry_type=2` metadata entries and restores them on extraction using `SetProtection()` and `SetComment()`.

When extracting a non-Amiga archive, the Amiga frontend just writes the data. Files get default protection bits — they work fine.

#### DOS (file attributes)

DOS files have attribute bits (Read-only, Hidden, System, Archive). The DOS frontend stores these as `entry_type=2` metadata and restores via `_dos_setfileattr()` or equivalent.

When extracting a non-DOS archive, the DOS frontend just writes the data with default attributes.

#### Linux / modern Unix (permissions)

The Unix frontend must:

- Read file permissions via `stat()` and store the low 12 bits of `mode_t` as `entry_type=2` metadata
- On extraction: apply permissions via `chmod()` — at minimum, restore the execute bit
- Extracts `entry_type=0` data entries and applies `entry_type=2` Unix metadata
- Skips resource forks and non-Unix metadata

**This is essential.** Without the execute bit, extracted binaries and shell scripts will not run.

#### RISC OS (filetype)

The RISC OS frontend must:

- Read the file's load address, exec address, and attributes via `OS_File 5`
- Store as `entry_type=2` metadata
- On extraction: restore via `OS_File 1` or `OS_File 18`

RISC OS identifies files by a 12-bit filetype encoded in the load address, not by extension. Without restoring this, the Filer won't recognise the file and executables won't run. As critical as resource forks on Mac.

#### Atari TOS / MiNT

Pure TOS: programs are identified by extension (.PRG, .TOS, .APP, .TTP, .GTP). File attributes are DOS-style. Simple case.

MiNT (Unix-like layer on Atari): has Unix permissions. Execute bit matters. The frontend stores both TOS attributes and MiNT permissions if running under MiNT.

#### BeOS / Haiku

Unix-like underneath. The frontend stores Unix permissions (execute bit) plus the MIME type string. On extraction, restores permissions via `chmod()` and sets the MIME type via BFS attribute APIs.

#### Windows / OS/2

Same as DOS: store RHSA attribute bits. Programs run based on extension, not permissions.

### File Name Encoding

The archive stores all file names as **UTF-8**. The archiver core passes raw UTF-8 bytes to the frontend — it never interprets or converts them. Each frontend handles conversion to and from the native encoding.

#### Archiving (native → UTF-8)

Each frontend converts native file names to UTF-8 when adding entries. A small static lookup table per codepage handles this:

- **ASCII names** (0x00–0x7F) are already valid UTF-8 — no conversion needed
- **Non-ASCII** bytes are looked up in a codepage-to-Unicode table (e.g., CP437 `0x82` → U+00E9 `é` → UTF-8 `0xC3 0xA9`)
- **MacRoman**, **Latin-1**, **Atari ST charset** each need one 128-entry table (128 bytes of ROM)

The conversion table per codepage is ~128–256 bytes. A frontend only needs its own codepage table.

#### Extracting (UTF-8 → native)

The frontend converts UTF-8 names to the native encoding using a best-effort, lossy approach:

1. **Direct mapping** — if the Unicode codepoint exists in the target encoding, use it (`é` → CP437 `0x82`, MacRoman `0x8E`, Latin-1 `0xE9`)
2. **Transliteration** — if no direct mapping, simplify (`ä` → `a`, `ñ` → `n`, `ø` → `o`). A small static table covers common accented Latin characters
3. **Replacement** — unmappable characters (CJK, Cyrillic, Arabic on Western codepage) become `_`
4. **Truncation** — trim to the target filesystem limit (8.3 on DOS/TOS, 31 bytes on Mac OS, 107 on Amiga, 255 on modern systems)
5. **Collision resolution** — if two different UTF-8 names map to the same native name, append `~1`, `~2`, etc.
6. **Report changes** — when a name is modified, print a message: `"résumé.pdf" → "RESUME~1.PDF"`

The implementation should stay small. The reverse table (Unicode → codepage) can be derived from the forward table at build time, or a small hand-written table covering the 128 non-ASCII entries suffices. The transliteration table is ~50 entries for common accented characters. Total data per codepage: **under 512 bytes**.

#### Roundtrip on the same system

Archiving on DOS and extracting on DOS is lossless: CP437 → UTF-8 → CP437 maps back to the original bytes. This holds for any system where the same codepage is used at both ends.

#### 8.3 filename generation

DOS and Atari TOS are limited to 8.3 uppercase names. The frontend:

- Uppercases the (already converted) name
- Strips characters illegal in 8.3 (spaces, most punctuation)
- Splits at the last `.` into base (max 8) and extension (max 3)
- Truncates each part
- Resolves collisions with `~N` suffix (base becomes max 6 + `~N`)

#### What the archiver core does

Nothing. The core stores and retrieves raw UTF-8 bytes. All encoding logic lives in the frontend. This keeps the core simple and encoding-agnostic.

## Directory Structure

```text
boltpack/
├── build/              Pre-built binaries per platform (one subdirectory per target)
├── dev/                Docker images and vendor setup scripts for cross-compilation
├── docs/               Documentation
├── lib/
│   └── zstd/             Third-party: zstd source
├── magic/              file(1) magic definition for .bolt files
├── src/
│   ├── core/
│   │   ├── bp_types.h      Portable type definitions, constants, bp_io_t, bp_progress_t
│   │   ├── compress.c      Compression layer (wraps boltlz)
│   │   ├── compress.h
│   │   ├── archiver.c      Archiver core (format read/write)
│   │   ├── archiver.h
│   │   ├── fluff.c         Native LZ77 + FSE/tANS codec (primary)
│   │   ├── fluff.h
│   │   ├── fluff_json.c    Fluff JSON debugging
│   │   ├── fluff_json.h
│   │   ├── fluff_trace.c   Fluff bitstream trace
│   │   ├── fluff_trace.h
│   │   ├── bp_codepage.c   Codepage conversion tables (CP437, MacRoman, Latin-1, etc.)
│   │   └── bp_codepage.h
│   └── frontend/
│       ├── linux/main.c     Linux (and Hurd, FreeBSD, NetBSD, OpenBSD, etc.)
│       ├── amiga/main.c     AmigaOS 2.x/3.x 68k
│       ├── amigaos4/main.c  AmigaOS 4.x PowerPC
│       ├── aros/main.c      AROS
│       ├── atari/main.c     Atari TOS / MiNT
│       ├── dos/main.c       MS-DOS (DJGPP / Turbo C)
│       ├── haiku/main.c     BeOS / Haiku
│       ├── macosx/main.c    Mac OS X / macOS
│       ├── macos9/          Classic Mac OS (CodeWarrior project)
│       ├── morphos/main.c   MorphOS
│       ├── os2/main.c       OS/2 / ArcaOS
│       ├── plan9/main.c     Plan 9
│       ├── ql/main.c        Sinclair QL
│       ├── riscos/main.c    RISC OS
│       ├── win/main.c       Windows (32-bit and 64-bit)
│       ├── win_wcx/         Total Commander plugin (WCX interface)
│       ├── x68k/main.c      Sharp X68000
│       └── ...              (other platforms: solaris, aix, irix, hpux, etc.)

├── Makefile            Build for all targets (FRONTEND=linux default)
└── build.sh            Helper build script
```
