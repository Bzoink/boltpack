# BoltPack Mini

BoltPack mini is a stripped-down variant of BoltPack that targets 16-bit
systems — DOS (8086+), CP/M-86, Windows 3.x (Win16), and OS/2 1.x — where
`sizeof(int) == 2`. It reads and writes the standard `.bolt` format so archives
are fully interchangeable with the full build. Fluff is removed from the
dependency graph entirely. Method 1 (BoltLite) is available on all mini targets and
is the default compression method on the 16-bit frontends; method 2 (boltlz)
remains available only on the 32/64-bit mini builds (16-bit mini builds
decompress boltlz via the shared `boltlz_decomp.c` decoder).
The binary is named `bpmini.exe` (or `bpmini.cmd` on CP/M-86) to
fit within the 8.3 filename limit.

---

## Design Goals

| Goal                         | Detail                                                                       |
| ---------------------------- | ---------------------------------------------------------------------------- |
| 16-bit platform compilation  | Watcom C compact model (`-mc`, 8086), Aztec C 86 (CP/M), MSVC 1.5 (OS/2 1.x) |
| Standard `.bolt` format      | Archives are interchangeable with the full build                             |
| Always display archive stats | `l` and `i` always show sizes and counts, even beyond mini limits            |
| Graceful capability limits   | Unsupported operations print a message and exit — no silent fail             |
| No fluff dependency          | `fluff.c`, `fluff.h` never appear in the mini build graph                    |
| UTF-8 path storage           | Archive format is always UTF-8; display sanitises non-ASCII                  |

---

## Capability Limits

| Capability             | Mini limit                       | Notes                                                                                                       |
| ---------------------- | -------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| Compression methods    | 0 (stored), 1 (BoltLite), 2 (boltlz) | Methods 3 (Fluff) and above return unsupported-method; BoltLZ compression is unavailable on 16-bit mini builds |
| Block size             | 8 KB – 32 KB                     | `-b` option is capped at 32 KB (`BP_MINI_MAX_BLOCK_SIZE`)                                                   |
| Files per archive      | ≤ 65 535                         | Create rejects entry 65 536 and above                                                                       |
| Uncompressed file size | ≤ 4 GB                           | `original_size.hi != 0` triggers error; display still shows the size                                        |
| Practical archive size | ≤ 2 GB                           | `bp_off_t` is `long` on DOS; `fseek`/`ftell` cap at 2 GB                                                    |
| Commands               | `c`, `x`, `l`, `t`, `i`          | `a` (append), `d` (delete), `-F` (Fluff) → unsupported message                                               |

Even when limits are exceeded in an existing archive, the `l` and `i` commands
always enumerate and display all entries and their sizes correctly.

---

## Architecture

### Code Sharing

The mini build lives in `src/core/mini/` — a dedicated directory with stripped
copies of the archiver and compression layer. The shared `bp_types.h` is
referenced in-place (`../bp_types.h`) and never duplicated.

```text
src/core/
    bp_types.h          ← shared by full build + mini (read-only reference)
    archiver.c/.h       ← full build only
    compress.c/.h       ← full build only (includes fluff)
    fluff.c/.h          ← full build only
    mini/
        archiver.c/.h   ← mini archiver (no fluff, capability checks)
        compress.c/.h   ← mini compress (stored + BoltLite + boltlz; boltlz compress on 32/64-bit only)
        boltlz_decomp.c/.h ← shared boltlz decoder for 16-bit (decompress only)

src/frontend/
    dos/                ← 32-bit DOS (DJGPP / DOS extender)
    mini_shared/
        mini_common.h   ← shared declarations (all mini frontends include this)
        mini_common.c   ← print_utf8_safe, bp_io_from_file, error strings,
                          argument parsing helpers, entry display
    dos16/
        main.c          ← 16-bit DOS mini frontend (Watcom)
    cpm86/
        main.c          ← CP/M-86 mini frontend (Aztec C 86 / Turbo C)
    win16/
        main.c          ← Windows 3.x mini frontend (Watcom / Borland / MSVC)
    os2_16/
        main.c          ← OS/2 1.x mini frontend (Microsoft C / Watcom)
```

All four mini frontends include `../mini_shared/mini_common.h` and link
`mini_common.c`. Only the platform-specific glue differs between them:
file-open/create, directory iteration, mtime handling, and extract-path
resolution. See the per-frontend sections below.

---

## Platform Patches

Two existing files require targeted patches to support 16-bit compilation. All
changes are conditional: the full build sees no difference.

### `src/core/bp_types.h`

On 16-bit Watcom, `sizeof(int) == 2`. `bp_uint32` must map to `unsigned long`
(4 bytes on DOS) or all 32-bit fields in the archive format become wrong. The
`bp_uint64` struct inherits the fix automatically because its members are
`bp_uint32`.

```c
/* 16-bit override: Watcom DOS has int=2, long=4 */
#if defined(SIZEOF_INT) && (SIZEOF_INT < 4)
  typedef unsigned long  bp_uint32;
#else
  typedef unsigned int   bp_uint32;
#endif
```

Two constants are added (active only when `BP_MINI` is defined):

```c
#ifdef BP_MINI
#  define BP_MINI_MAX_ENTRIES    65535U
#  define BP_MINI_MAX_BLOCK_SIZE 32768U
#endif
```

### `src/core/mini/boltlz_decomp.c` / `boltlz_decomp.h`

This adapter validates the mini block limits and invokes the shared BoltLZ
format-2 decoder. On 16-bit targets it includes the decoder-only build of
`src/core/boltlz.c`; no encoder hash tables or encoder entry points are linked.
Other mini builds link the full codec. The decoder checks declared buffers
and requires no workspace or padding. Compression on 32/64-bit mini builds
uses Balanced mode and a 64 KiB heap workspace; 16-bit creation uses Stored
or BoltLite and returns unsupported-method for BoltLZ compression.

### `src/core/mini/compress.h` / `compress.c`

Public API — mirrors the full `compress.h` surface but with a `method`
parameter and mini-specific guards:

```c
int       bp_mini_compress_init(void);
int       bp_mini_compress_block(const bp_uint8 *src, bp_uint32 src_len,
                                 bp_uint8 *dst, bp_uint32 *dst_len,
                                 bp_uint8 method);
int       bp_mini_decompress_block(const bp_uint8 *src, bp_uint32 src_len,
                                   bp_uint8 *dst, bp_uint32 *dst_len,
                                   bp_uint8 method);
bp_uint32 bp_mini_checksum(bp_uint32 adler,
                           const bp_uint8 *data, bp_uint32 len);
bp_uint32 bp_mini_compress_work_size(void);
```

Key behaviour:

- `method > BP_METHOD_BOLTLZ` returns `BP_ERR_UNSUPPORTED` immediately.
- `src_len > BP_MINI_MAX_BLOCK_SIZE` returns `BP_ERR_INVALID_ARG`.
- On 16-bit targets, an 18,432-byte BoltLite scratch buffer (`BOLTLITE_WORK_SIZE`) is
  heap-allocated on the first compress call and cached for the process
  lifetime. Extract, list, test, and info never allocate it; decompression
  needs no scratch. Heap placement keeps it off the 4–8 KB stack. Allocation
  failure returns `BP_ERR_NOMEM`.
- On 16-bit targets, boltlz *compression* returns `BP_ERR_UNSUPPORTED_METHOD`;
  boltlz *decompression* runs via the shared `boltlz_decomp.c` decoder.
- On 32/64-bit targets, boltlz compress/decompress are wired directly to
  `src/core/boltlz.c`; a 64 KiB heap work buffer is used for Balanced compression.

### `src/core/mini/archiver.h` / `archiver.c`

Subset of the full archiver API — no `bp_archive_open_append`,
`bp_archive_delete`, or `bp_archive_copy_from`:

```c
int bp_mini_archive_create(bp_archive_t *ar, bp_io_t *io,
                           bp_uint32 block_size, bp_uint8 method);
int bp_mini_archive_add_entry(bp_archive_t *ar, const char *name,
                              bp_uint8 entry_type, bp_uint8 platform,
                              bp_io_t *file_io,
                              bp_uint64 file_size, bp_uint64 mtime);
int bp_mini_archive_finalize(bp_archive_t *ar);
void bp_mini_archive_close(bp_archive_t *ar);

typedef int (*bp_list_fn)(const bp_entry_t *entry, void *user_data);
int bp_mini_archive_list(bp_io_t *io, bp_list_fn fn, void *user_data);

typedef bp_io_t *(*bp_extract_fn)(const bp_entry_t *e, void *ud);
typedef void    (*bp_extract_done_fn)(const bp_entry_t *e,
                                      bp_io_t *out, void *ud);
int bp_mini_archive_extract_all(bp_io_t *io,
                                bp_extract_fn open_fn,
                                bp_extract_done_fn done_fn,
                                bp_progress_fn progress_fn,
                                void *user_data);

int bp_mini_archive_test(bp_io_t *io, bp_test_fn fn, void *user_data);
int bp_mini_archive_info(bp_io_t *io, bp_archive_info_t *info);
```

Capability checks in `archiver.c`:

- `bp_mini_archive_create`: rejects `block_size > 32KB` or methods above BoltLZ; 16-bit builds reject BoltLZ compression because they have no encoder.
- `bp_mini_archive_add_entry`: rejects `entry_count >= 65535` or
  `file_size.hi != 0`.
- `bp_mini_archive_extract_all`: supports stored and BoltLite entries on all
  mini targets, and BoltLZ entries where the decoder is available; logs and
  skips unsupported methods or entries too large for the target.
- `bp_mini_archive_list` / `bp_mini_archive_info`: no capability filtering —
  all entries are always enumerated and their sizes always displayed.

#### `bp_mini_u64_print`

DOS `printf` has no `%llu`. A small helper prints a `bp_uint64` as decimal
using only `unsigned long` arithmetic:

```c
void bp_mini_u64_print(bp_uint64 v);
```

It handles the full 64-bit range and is used for all size output in list and
info views.

### `src/frontend/dos16/main.c`

Modelled on `src/frontend/dos/main.c` with these differences:

| Aspect       | `dos/main.c` (32-bit DJGPP)        | `dos16/main.c` (16-bit Watcom)                     |
| ------------ | ---------------------------------- | -------------------------------------------------- |
| Includes     | archiver.h, compress.h, fluff.h    | mini/archiver.h, mini/compress.h — no fluff        |
| Seek/tell    | `fseeko`/`ftello` (64-bit offsets) | `fseek`/`ftell` (`long`, 2 GB cap)                 |
| Commands     | c, a, x, l, t, i, d, T             | c, x, l, t, i only                                 |
| `-b` option  | 8 KB – 256 KB                      | 8 KB – 32 KB (capped at `BP_MINI_MAX_BLOCK_SIZE`)  |
| `-F` flag    | selects Fluff codec                | prints "Fluff not supported by mini" + exit 1      |
| Path display | UTF-8 printed raw                  | `print_utf8_safe()` replaces bytes > 0x7E with `?` |
| Memory model | flat 32-bit                        | Watcom compact model (far data heap)               |

The `print_utf8_safe()` function:

```c
static void print_utf8_safe(const char *str, unsigned int len)
{
    unsigned int i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        putchar((c >= 0x20 && c <= 0x7E) ? (int)c : '?');
    }
}
```

### `src/frontend/mini_shared/mini_common.h` / `mini_common.c`

Code shared by all four mini frontends. Each frontend includes the header and
links the `.c` file.

**Provided by mini_common:**

- `bp_io_from_file(bp_io_t *io, FILE *fp)` — wires a `bp_io_t` to a `FILE *`
  using `fseek`/`ftell` (`long` offsets). Correct for all 16-bit targets; none
  have `fseeko`/`ftello`.
- `print_utf8_safe(const char *str, unsigned int len)` — prints bytes 0x20–0x7E
  verbatim, replaces all others with `?`.
- `bp_strerror(int code)` — maps `BP_ERR_*` codes to human-readable strings.
- `parse_block_size(const char *s, bp_uint32 *out)` — parses `8k`, `16k`,
  `32k`; rejects values above `BP_MINI_MAX_BLOCK_SIZE`.
- `print_entry_line(const bp_entry_t *e)` — standard list-format output line
  used by all frontends.
- `print_archive_info(const bp_archive_info_t *info)` — standard info-command
  output used by all frontends.

**Not provided (each frontend implements its own):**

| Responsibility         | DOS 16                 | CP/M-86               | Win16            | OS/2 1.x         |
| ---------------------- | ---------------------- | --------------------- | ---------------- | ---------------- |
| File open for read     | `fopen`                | `fopen`               | `fopen`          | `fopen`          |
| Output path resolution | `mkdir` chain          | flatten to 8.3        | `mkdir` chain    | `mkdir` chain    |
| Directory iteration    | `findfirst`/`findnext` | none (filenames only) | `_dos_findfirst` | `DosFindFirst`   |
| mtime restore          | `_dos_setftime`        | no-op                 | `_dos_setftime`  | `DosSetFileInfo` |

### `src/frontend/cpm86/main.c`

CP/M-86 has a flat filesystem — no subdirectories at any version, including
CP/M Plus 3.0. Files are addressed by drive letter (`A:`) and user number
(0–15) only.

**Commands supported:** `c`, `x`, `l`, `t`, `i`

**Filesystem constraints:**

- **Create (`c`):** accepts individual filenames on the command line only — no
  directory recursion is possible (no `findfirst`/directory API). The archive
  path stored for each file is the basename only (no directory prefix).
- **Extract (`x`):** strips all directory components from the stored path,
  truncates the name part to 8 characters and the extension to 3, converts to
  uppercase, and writes the result to the current drive/user area. Warns on
  name collision. `BP_TYPE_DIR` and `BP_TYPE_SYMLINK` entries are silently
  skipped.
- **mtime:** CP/M-86 has no standard file timestamp API; mtime is not set on
  extraction and not read on creation (stored as zero).
- **Executable format:** `.CMD` (CP/M-86 segmented command format), not `.COM`.
  Total size exceeds 64 KB due to buffer requirements, so `.COM` format is not
  usable. Aztec C 86 and suitable Turbo C ports can produce `.CMD` files.

**Compiler flags (Aztec C 86):**

```text
CC       = cc86
CFLAGS   = -DSIZEOF_INT=2 -DSIZEOF_LONG=4 -DBP_MINI -DBP_OS_CPM86
```

### `src/frontend/win16/main.c`

Targets Windows 3.x (3.0 and 3.1) in standard or enhanced mode. Uses a
standard `main()` entry point with `stdio` — no `WinMain` or GUI required.
Runs in a DOS box under Windows.

**Commands supported:** `c`, `x`, `l`, `t`, `i`

The frontend is structurally identical to `dos16/main.c`. Differences:

- `_dos_findfirst`/`_dos_findnext` (or `FindFirstFile` where available) for
  directory iteration on create.
- `_dos_setftime` for mtime restore on extract.
- `mkdir` is available; hierarchical extraction works as on DOS 16.
- Compiler: Watcom C 16-bit (`wcc -bt=windows`), Borland C 3.x, or MSVC 1.5.

**Compiler flags (Watcom):**

```makefile
WIN16_CC     = wcc
WIN16_CFLAGS = -bt=windows -mc -2 -d0 -za \
               -DSIZEOF_INT=2 -DSIZEOF_LONG=4 \
                \
               -DBP_MINI -DBP_OS_WIN16
WIN16_EXE    = build/win16/bpmini.exe
```

Linked with `wlink system windows`.

### `src/frontend/os2_16/main.c`

Targets OS/2 1.x (1.0–1.3), which runs on 286 16-bit protected mode. OS/2 1.x
has a full hierarchical filesystem, long filename support on HPFS, and a
complete POSIX-like C runtime.

**Commands supported:** `c`, `x`, `l`, `t`, `i`

- `DosFindFirst`/`DosFindNext` for directory iteration on create.
- `DosSetFileInfo` for mtime restore on extract.
- Full path creation (`mkdir` or `DosCreateDir`) on extract; hierarchical
  paths work correctly.
- Executable format: OS/2 NE `.EXE` (same NE container format as Win16,
  distinguished by the OS/2 target flag in the NE header).
- Compiler: Microsoft C 5.1/6.0 with OS/2 SDK, IBM C/2, or Watcom C 16-bit.

**Compiler flags (Watcom):**

```makefile
OS216_CC     = wcc
OS216_CFLAGS = -bt=os2 -mc -2 -d0 -za \
               -DSIZEOF_INT=2 -DSIZEOF_LONG=4 \
                \
               -DBP_MINI -DBP_OS_OS216
OS216_EXE    = build/os2_16/bpmini.exe
```

Linked with `wlink system os2`.

---

## Build

### Makefile Target

```makefile
DOS16_CC     = wcc
DOS16_CFLAGS = -bt=dos -mc -0 -d0 -za \
               -DSIZEOF_INT=2 -DSIZEOF_LONG=4 \
                \
               -DBP_MINI
DOS16_OBJDIR = build/dos16
DOS16_EXE    = $(DOS16_OBJDIR)/bpmini.exe
```

Objects compiled:

- `$(DOS16_OBJDIR)/archiver.obj` — from `src/core/mini/archiver.c`
- `$(DOS16_OBJDIR)/compress.obj` — from `src/core/mini/compress.c`
- `$(DOS16_OBJDIR)/boltlz_decomp.obj` — from `src/core/mini/boltlz_decomp.c`
- `$(DOS16_OBJDIR)/main.obj` — from `src/frontend/dos16/main.c`

Linked with `wlink system dos`.

The other targets follow the same object pattern with their own `CC`, `CFLAGS`,
output directory, and wlink system keyword:

| Target   | `wlink system` | Output EXE/CMD            | Compiler     |
| -------- | -------------- | ------------------------- | ------------ |
| `dos16`  | `dos`          | `build/dos16/bpmini.exe`  | Watcom `wcc` |
| `cpm86`  | n/a (Aztec C)  | `build/cpm86/bpmini.cmd`  | Aztec C 86   |
| `win16`  | `windows`      | `build/win16/bpmini.exe`  | Watcom `wcc` |
| `os2_16` | `os2`          | `build/os2_16/bpmini.exe` | Watcom `wcc` |

All four share the same mini core objects; only `main.obj` and the linked
system differ.

### Memory Model Rationale

The `-mc` (compact) model is required. During create with BoltLite the following
allocations must coexist:

| Buffer                          | Size   |
| ------------------------------- | ------ |
| BoltLite work buffer (lazy, create) | ~18 KB |
| Input block buffer              | 32 KB  |
| Output block buffer             | ~37 KB |
| **Create peak**                 | ~87 KB |

Extraction is lighter: the BoltLite work buffer is never allocated, leaving one
~34 KB compressed-input buffer plus one 32 KB output buffer (allocated once
per run and reused across entries) — a peak of about 66 KB.

This exceeds the 64 KB near-data segment of the small model (`-ms`). Compact
model places all data in a far heap and resolves this. Under Watcom compact
model, standard `malloc()` returns far pointers — no `_fmalloc` is needed,
keeping the code portable.

---

## Files Modified

| File                       | Change                                           |
| -------------------------- | ------------------------------------------------ |
| `src/core/bp_types.h`      | 16-bit `bp_uint32` override; `BP_MINI` constants |
| `src/core/mini/compress.c` | boltlz decompress via shared decoder on 16-bit   |
| `Makefile`                 | `dos16`, `cpm86`, `win16`, `os2_16` targets      |
| `docs/portability.md`      | Mini platform rows and capability notes          |

## Files Created

| File                                     | Purpose                                       |
| ---------------------------------------- | --------------------------------------------- |
| `src/core/mini/compress.h`               | Mini compress API (stored + BoltLite + boltlz)    |
| `src/core/mini/compress.c`               | Mini compression wrapper — no fluff           |
| `src/core/mini/boltlz_decomp.h`          | Shared BoltLZ decoder API (16-bit)            |
| `src/core/mini/boltlz_decomp.c`          | Shared BoltLZ decoder (16-bit)                |
| `src/core/mini/archiver.h`               | Mini archiver API (subset of full)            |
| `src/core/mini/archiver.c`               | Archiver with per-operation capability checks |
| `src/frontend/mini_shared/mini_common.h` | Shared declarations for all mini frontends    |
| `src/frontend/mini_shared/mini_common.c` | print_utf8_safe, bp_io_from_file, helpers     |
| `src/frontend/dos16/main.c`              | 16-bit DOS CLI frontend (Watcom)              |
| `src/frontend/cpm86/main.c`              | CP/M-86 CLI frontend (Aztec C 86)             |
| `src/frontend/win16/main.c`              | Windows 3.x CLI frontend (Watcom / Borland)   |
| `src/frontend/os2_16/main.c`             | OS/2 1.x CLI frontend (Watcom / Microsoft C)  |

---

## Verification

1. `make dos16` (Watcom cross-toolchain or Docker container) — produces
   `build/dos16/bpmini.exe`, zero errors under `-za`.
2. `make cpm86` / `make win16` / `make os2_16` — each produces its binary with
   zero errors.
3. `wcc -bt=dos -mc -0 -za ... src/core/mini/boltlz_decomp.c` in isolation —
   must compile without errors.
4. `make` on Linux — full build unaffected.
5. `python3 test/check_boltpack.py` — all existing integration tests pass.
6. DOSBox or real DOS hardware:
   - `bpmini c test.bolt file.txt` — create archive
   - `bpmini l test.bolt` — list entries, sizes display correctly
   - `bpmini x test.bolt` — extract, compare bytes
   - `bpmini t test.bolt` — test integrity
7. CP/M-86 (real hardware or emulator such as DOSBOX-X with CP/M-86 ROM):
   - `bpmini c test.bolt file1.txt file2.txt` — files stored with basename
     paths only
   - `bpmini x test.bolt` — flat extraction to current drive/user area
   - `bpmini l test.bolt` — directory paths from a multi-level archive display
     correctly (even though extraction would flatten them)
   - Extract archive containing `src/core/foo.c` and `src/core/bar.c` → both
     extract to `FOO.C` and `BAR.C`; duplicate basename → warning printed
8. Win16 (Windows 3.1 in DOSBox-X or VirtualBox):
   - Same command set as DOS 16 test; confirm hierarchical extraction creates
     subdirectories correctly.
9. OS/2 1.x (VirtualBox or real hardware):
   - Same command set; confirm full path extraction and mtime restoration.
10. Cross-method test: create a fluff archive on Linux (`boltpack c -F
out.bolt file.txt`), then `bpmini x out.bolt` on any mini target — must
    print `"Method 3 not supported by mini"` and exit 1.
11. Large-archive test: `bpmini l` on an archive with > 65 535 entries must
    display all of them; `bpmini c` must reject at entry 65 536 with a clear
    message.

---

## Further Considerations

### boltlz indices on 16-bit Watcom

Under Watcom compact model with 16-bit `int`, boltlz decompression uses
`unsigned int` array indices and `unsigned long` (32-bit) public sizes. The
32 KB block ceiling keeps valid indices within the 16-bit range, while input
and far-distance arithmetic is checked before subtraction or addition can
wrap.

### `bp_mini_u64_print` on 32-bit Hosts

The same `bp_mini_u64_print` function ships in the mini archiver and runs on
all mini targets, including 32-bit DOS extenders and Linux tier-3 test builds.
The implementation must handle the full 64-bit range correctly — `bp_uint64.hi`
can legitimately be non-zero when listing large archives created on 64-bit
systems.

### Future `BP_MINI_NO_BOLTLZ` Flag

If a future target needs stored-only mode (true real-mode small model with no
room for the boltlz decode paths), a `BP_MINI_NO_BOLTLZ` compile flag can
strip boltlz from `src/core/mini/compress.c` cleanly. The mini layer already
isolates boltlz behind `bp_mini_compress_block`, so the removal is a
localised change.

### CP/M-86 Filename Collisions on Extract

When a multi-level archive is extracted to CP/M-86, multiple entries can map to
the same 8.3 basename (e.g. `src/core/main.c` and `src/frontend/main.c` both
become `MAIN.C`). The CP/M-86 frontend must detect this and print a warning
naming the collision before skipping the duplicate. Overwriting silently would
corrupt data with no way to recover on a flat filesystem.

### OS/2 1.x Long Filenames

HPFS on OS/2 supports filenames longer than 8.3. The archive format stores the
full UTF-8 name. The OS/2 1.x frontend should attempt to create the full name
and fall back to a truncated 8.3 name (with a warning) only when the target
volume is FAT.
