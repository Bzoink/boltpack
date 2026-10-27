# Archive Interoperability Matrix

**Reviewed:** 2026-10-27  
**Format:** 6

This matrix distinguishes code presence from validation evidence. “Cross-built”
does not mean that extraction has been run on the target OS. A capability not
listed as implemented remains unqualified; do not infer it from the core format.

| Frontend/build | Codecs and size limits | Names and destination safety | Time and attributes | Evidence and remaining work |
| --- | --- | --- | --- | --- |
| Full core + Linux CLI | Stored, BoltLite, BoltLZ, Fluff, zstd when compiled; 64-bit format sizes; configurable blocks | UTF-8 canonical relative paths; exact duplicate names rejected; extraction uses directory-relative operations and sibling staging | POSIX mode and time flags; foreign metadata/resource forks are reported as partial | Host unit and integration suites; Linux path-race tests. Unicode normalization is not needed on common Linux filesystems but other destinations need their own collision rules. |
| Mini core + Linux mini | Stored, BoltLite, and BoltLZ when the build supports it; 32 KiB maximum block; 65,535 physical entries; 2,147,483,647-byte serialized archive limit | Shared UTF-8 and canonical path checks; Linux output uses directory-relative operations | POSIX mode/time restoration; unsupported foreign attributes are reported | Host archive-format tests. 16-bit DOSBox suite covers mini compression, not all metadata or filesystem behavior. |
| Full Windows CLI | Stored, BoltLite, BoltLZ, Fluff, optional zstd; full-format sizes | Strict ACP projection, case-insensitive collision checks, reserved-name and trailing-dot/space refusal; staged output | Portable FAT attributes for files/directories and native Windows attributes; deferred directory restoration | Win32/Win64 cross-builds; runtime tests still required. Reparse-point and concurrent path mutation need target verification. |
| Native Win32 GUI | Stored, BoltLite, BoltLZ, and Fluff in the GUI build | Strict UTF-8-to-ACP conversion, Windows component checks, case-insensitive projected-name collision refusal, and path-based reparse checks; Unicode normalization and parent-swap races are not covered | Stages regular files before commit; restores known times and portable FAT flags, defers directory metadata; POSIX modes, symlinks, and resource forks report partial | Win32 and Win64 cross-builds pass. No Windows runtime or reparse-point tests are available here. |
| Full Atari CLI | Stored, BoltLite, BoltLZ, Fluff; target-specific memory and path limits | Strict base GEMDOS ASCII 8.3 projection; 1,024-entry/32-KiB collision index; checked path components and staged files | TOS attributes plus portable FAT flags; directory attributes/times deferred; timezone uses `BP_TZ_OFFSET` | Cross-build succeeds; no Atari emulator or hardware round trip. Path-based parent substitution remains possible. |
| Full OS/2 CLI | Stored, BoltLite, BoltLZ, Fluff; 32-bit Watcom LX executable cross-build | Destination codepage and case-map preflight; FAT targets require 8.3; bounded collision index; sibling staging | Native OS/2 attributes plus portable FAT; directory metadata deferred; explicit fixed timezone offset | Watcom cross-build succeeds; no OS/2 runtime round trip. Final verified-file copy can be interrupted, and path APIs leave a parent substitution race. HPFS `.TYPE` EAs are not transferred. |
| DOS16, Win16, OS/2-16 mini | Stored/BoltLite on 16-bit; 32 KiB blocks; mini entry and archive limits | Target codepage conversion is used; target-specific projected collision behavior is not fully qualified | Portable FAT envelope and deferred directory restoration; DOS-style times require configured offset | DOS16 mini unit program passes in DOSBox; DOS16, Win16, OS/2-16 cross-build. Win16 uses Watcom large-code model. Win16/OS2-16 runtime metadata tests remain. |
| Non-Linux POSIX wrappers using `linux/main.c` | Same compiled core as their build target | Shared lexical checks; target build does not use Linux `openat` branch, so symlink race protection is weaker | POSIX mode/time fields; foreign metadata is reported unsupported | Individual target runtime tests and builds remain. Do not treat Linux host evidence as target evidence. |
| macOS CLI and Cocoa GUI | Full core codecs supported by each build | Source-specific path handling; no common destination collision preflight or staged regular-file commit | macOS CLI captures resource forks, Unix mode, and Finder info; GUI coverage differs | macOS target runtime validation and safe extraction work remain. |
| Haiku, Amiga-family, RISC OS, Plan 9, QL, X68000, and classic Mac frontends | Frontend-specific codec/build limits | Name conversion and collision behavior vary by adapter; not qualified as a common profile | Native metadata exists in selected adapters; cross-platform fallback coverage varies | Per-target audit, build evidence, and extraction round trips remain to be recorded. |
| Linux FLTK GUI | Full core codecs enabled by the GUI build | Canonical names, directory-relative `openat` traversal with `O_NOFOLLOW`, sibling staging, atomic publication; flattened extraction reserves distinct names | Restores file mode/time and defers directory mode/time until children finish; unsupported metadata/resource forks return partial | `make linux-gui` builds on Linux; interactive extraction and filesystem behavior still need runtime coverage. |
| Windows FLTK GUI | Stored, BoltLite, BoltLZ, Fluff, and zstd in the GUI build | Strict UTF-8-to-ACP projection for archive names and chosen destination root; chooser-selected archive opens through UTF-16; Windows component checks; case-insensitive path and file/directory-prefix collision preflight capped at 65,535 selected entries and path nodes; checks selected root and traversed directories for reparse points | Sibling staging publishes only after checksum and close succeed; restores representable file and deferred directory times plus portable FAT flags; POSIX modes, symlinks, resource forks, and unsupported metadata report partial | `make win32-fltk-gui` and `make win64-fltk-gui` pass. No Windows runtime test; parent-directory swap races remain, and names or root paths outside the active ACP are refused. |
| Haiku, Atari, Amiga, and macOS desktop GUIs | Use the shared format-6 core with frontend-specific codec builds | Name conversion and collision behavior vary by adapter; no common extraction preflight | Native metadata and timestamp coverage varies | Per-target safe extraction, metadata, and runtime audits remain. |
| Total Commander WCX | Shared format-6 core with Stored, BoltLite, BoltLZ, Fluff, and zstd when compiled | Plugin extraction has not yet been qualified for strict name projection and destination collision preflight | Plugin extraction lifecycle and portable/native metadata restore remain under review | Win32/Win64 builds exist; plugin-host runtime verification and safe overwrite behavior remain. |

## Core evidence

- Full and mini cores emit and accept format 6 only.
- Both cores validate counted UTF-8 names, canonical relative path components,
  primary-name uniqueness, inline/table agreement, and supplementary-group
  framing before extraction or archive editing.
- Portable metadata envelopes are bounded to 256 bytes. Duplicate or orphaned
  portable records and duplicate resource forks are rejected.
- The mini writer and reader enforce the serialized-size limit, including file
  table and footer overhead. Host tests exercise write overflow, finalization
  overhead, and oversized header rejection.
- On 2026-10-27, `make CC=gcc test` passed: 175 archive-format checks and 121
  integration checks, with all other host suites green. `make linux-gui` also
  built successfully. A Linux CLI fixture with a valid portable FAT record
  returned partial while restoring the directory's Unix mode and epoch time.
- GUI interaction was not runtime-tested: Xvfb starts but this environment
  cannot open its display. Atari, OS/2, and Windows evidence remains
  cross-build-only; target runtime checks are still outstanding.

This matrix is intentionally not a support promise for every OS listed in
`frontends.md`. Completion requires either the listed capability to be
implemented and validated or a deliberate, user-visible refusal/reporting
policy for that adapter.
