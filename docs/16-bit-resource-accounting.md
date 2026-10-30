# 16-bit resource accounting

This report records the Open Watcom DOS16, Win16, and OS/2-16 mini builds on
2026-10-29. `make mini-all` rebuilds the three targets and emits a WLINK map
beside each executable. The DOS16 ABI was also inspected in DOSBox 0.74-3.
The compiler and linker were Open Watcom 2.0 beta (2026-10-14); flags are the
`-0` 8086, mini, and target model flags in the Makefile. These are measurements
of this configuration, not of other compiler versions or memory managers.

## Linker measurements

| Target | Memory model | EXE bytes | Linked code bytes | Largest code segment | DGROUP bytes | Reserved stack | Static DGROUP excluding stack |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| DOS16 | compact (`-mc`) | 58,280 | 52,929 | 52,922 | 13,152 | 8,192 | 4,960 |
| Win16 | large (`-ml`) | 84,416 | 73,200 | 35,752 | 14,016 | 8,192 | 5,824 |
| OS/2-16 | compact (`-mc`) | 68,244 | 60,846 | 60,839 | 9,120 | 4,096 | 5,024 |

Code/data/group figures are sums from the WLINK map segment table. EXE size
includes executable headers and relocation data. Win16 splits code among
segments; its largest code segment is below 64 KiB. `STACK` is the linker
reservation, not a measurement of worst-case call-chain consumption. DOS16 has
a sampled workload measurement below; worst-case call-chain use and
target-specific CRT startup consumption remain open.

## Heap requests and limits

The mini ABI uses a 32 KiB maximum block. On DOS16, Watcom compact model has a
16-bit `size_t`; the DOSBox fixture measured `sizeof(bp_mini_entry_t) == 54`.
The create writer retains one entry record per table row, so one `realloc`
request cannot exceed 65,535 bytes. The safe maximum is therefore
`floor(65,535 / 54) == 1,213` entries, or 65,502 requested bytes. The format
limit remains 65,535 entries for targets that can hold it. The writer now
returns `BP_ERR_OVERFLOW` at the target allocation bound instead of allowing
the byte count to wrap and writing past an undersized allocation.

The DOSBox test creates 1,213 directory entries, confirms the 1,214th is
refused, then successfully finalizes the archive. The full Linux reader tests
the finalized 1,213-entry archive. The same DOSBox fixture fills the projected
name index to 128 entries and 4,016 of its 4,096 key bytes, checks both limits
refuse overflow without corrupting the index, and fills the deferred-directory
queue to 256 nodes before checking the next node is rejected. `make unit-test-dos16`
runs these checks alongside the existing 20 DOS16 mini unit checks. The DOS16,
Win16, and OS/2-16 builds compile assertions that `size_t` is 16-bit and the
entry record is 54 bytes, so the byte bound is the same across these Watcom
models. Win16 and OS/2-16 runtimes are not available here.

Largest fixed buffer requests with a 32 KiB block are:

| Operation | Input buffer | Output buffer | BoltLite work buffer | Concurrent requested buffers |
| --- | ---: | ---: | ---: | ---: |
| Create/add nonempty entry | 32,768 | 36,880 (BoltLite worst-case bound) | 18,432 (lazy, retained for process lifetime) | 88,080 |
| Extract/test | 34,883 (compressed-block bound) | 32,768 | 0 | 67,651 |

These totals exclude the archive entry table, retained entry names, directory
queue and path strings, projected-name index (4,736 bytes), frontend path
buffers, CRT state, and allocator metadata. Entry names and directory queue
nodes are allocated separately and can increase total memory substantially.
All allocations are checked; allocation failure is reported as
`BP_ERR_NOMEM`. The DOSBox regressions verify table, index, and queue bounds.
A separate instrumented DOSBox executable forces 26 allocation failures.
Nine cover the entry table, entry name, input block, output block, BoltLite
work area, entry name after BoltLite has retained its workspace, projected-name
index, directory queue node, and directory queue path. Six sweep allocation
failure through archive-test table preflight, metadata grouping, entry-name
loading, and decompression buffers. Six sweep the same read-side operations
through extraction; failures after an output is opened verify that its
completion callback closes it and receives `BP_ERR_NOMEM`. Every case checks
that temporary allocations are released, with the expected cached BoltLite
workspace retained for the process lifetime. Four more fail each allocation
in the shared 16-bit directory-walker buffer helper and the mini name
round-trip buffer, and one fails the projected-index scratch allocation; they
verify `BP_ERR_NOMEM`,
and check that partial allocations are released. These cases cover core and
shared-mini allocations used by DOS16, Win16, and OS/2-16.

The DOS16, Win16, and OS/2-16 source-directory walkers use the shared helper
for their path buffers. Each recursion level now allocates only the pattern
and child path capacities required by its current path and the target's
`find_t.name` field, instead of retaining three 260-byte buffers. The helper
frees successful partial allocations and reports `BP_ERR_NOMEM`. The DOS16
test CLI now injects failure into each differently sized buffer and verifies
that all three cases report out-of-memory, return failure, and leave zero
tracked allocations. The full `make unit-test-dos16` target passes. Win16 and
OS/2-16 cross-builds pass; their runtime behavior and target-specific heap
exhaustion remain unqualified.

The DOS16 CLI also has a test-only build that fails each of the three path
buffer allocations during archive create. The destination is seeded with
sentinel bytes; after every injected failure DOSBox confirms the CLI reports
out-of-memory, tracks no leaked allocations, and leaves those bytes unchanged.
The DOS16/Linux emulator fixture also replaces an existing archive with a new
archive, verifies the new contents with the Linux reader, and checks that no
stage or backup name remains after success. This checks publication behavior
on the DOSBox host mount, not crash recovery or DOS filesystem attributes.

Watcom compact and large models use far data pointers for dynamic allocations;
the code calls the standard `malloc`/`realloc`/`free` API. The exact usable heap
and allocator bookkeeping depend on the selected DOS, Windows, or OS/2 runtime
and memory manager. The report now records requested and `_msize()` usable
payload sizes for successful allocations in the DOS16 instrumented build; it
does not measure allocator headers or guarantee available memory. The
projected-name collision check now uses a checked
520-byte heap scratch block instead of two 260-byte stack arrays; a DOSBox
allocation-failure test confirms `BP_ERR_NOMEM` without a leak. This trades
some heap headroom for stack headroom on that path.

The DOS16 runtime probe tried Watcom `_memavl()` and heap walks. The reported
near-heap free space stayed around 58 KiB while the application allocated its
far buffers, and `_fheapwalk()` did not reach a valid end status in this DOSBox
runtime. The test-only probe also makes an impossible DOS `INT 21h/AH=48h`
allocation request and records the largest free block returned by DOS on the
expected out-of-memory result. A separate allocation wrapper calls Watcom
`_msize()` after each successful `malloc`, `calloc`, or `realloc`, without
allocating tracking records. In the 2026-10-30 DOSBox corpus, the largest
requested block was 36,880 bytes and `_msize()` reported 36,884 usable bytes;
the largest observed per-block difference was 5 bytes. This reports payload
capacity rounding for individual blocks, not allocator headers or a guaranteed
heap limit. The wrapper tracks
usable sizes across alloc/free/realloc calls without a per-allocation table;
the maximum measured per-command live-usable high-water is now 163,286 bytes
in the `WRITMAX` writer workload; the earlier 35-frame rejected-tree run was
103,166 bytes. This covers only allocations routed through the
instrumented project wrappers and excludes allocator metadata and runtime
allocations. Cumulative requested and usable byte columns sum calls (including
every `realloc` request) for a command and are not concurrent memory totals.
The 2026-10-30 rerun of
`make CC=gcc dos16-interoperability-test` and
`make CC=gcc dos16-stack-interoperability-test` passed the DOS16 allocation,
resource-bound, stack, replacement, and Linux-transfer checks; its per-command
measurements are retained in `build/dos16-stack/resource-low-water.txt`. The
instrumented depth-bound run reports a 360,144-byte DOS-manager largest-block
low-water; use this as the latest observed value, not as a general capacity
guarantee. This measures the DOS manager's largest available block in this
DOSBox setup, not guaranteed Watcom `malloc` capacity. Allocator headers,
fragmentation behavior, uninstrumented runtime allocations, and Win16/OS/2-16
runtime heap behavior remain unqualified.

The 2026-10-30 resource fixture now also extracts an archive with 126 empty
child directories and one leaf file, exercising deferred directory metadata
queue allocation through the DOS16 frontend at the 128-name projected-index
bound. DOSBox restored all 126 directory names and the leaf bytes. The
instrumented `QUEUEOUT.TXT` sample records 1,282 successful allocation calls,
a 73,264-byte tracked live-usable high-water, and a 429,808-byte DOS-manager
largest-block low-water. Its lowest sampled stack availability is 6,759 bytes.
This fixture reports partial when the host-mounted
DOS filesystem does not restore the source directory times; the test accepts
that documented attribute outcome only after checking the complete tree and
payload. It provides a workload measurement near the 128-name projected-index
limit, not a maximum-memory proof for every legal archive.

The same run now integrity-tests `LIMIT.BLT`, the 1,213-entry directory-only
archive generated by the DOS16 size-bound unit fixture. The check passes and
exercises maximum-entry traversal. `LIMTEST.TXT` records 3,640 successful
allocation calls, a 24,520-byte tracked live-usable high-water, a largest
request of 256 bytes, and a 495,392-byte DOS-manager largest-block low-water;
the lowest sampled stack availability is 7,462 bytes. This command streams the
archive records and does not allocate the writer table.

The instrumented DOS16 run now builds `WRITMAX.BLT` with 1,212 directory rows
and one 32 KiB BoltLite-compressed data entry, reaching the 1,213-entry table
bound while the input/output blocks and lazy BoltLite workspace are live.
Linux integrity-tests the result. `WRITMAX.TXT` records a 65,502-byte largest
request and a 163,286-byte tracked live-usable peak; 18,432 bytes remain live at
exit for BoltLite's cached work area. The command makes 2,436 successful
allocation calls, sums 284,026 requested and 285,250 usable bytes, and reports
no invalid `_msize()` measurements. This combines the maximum writer table,
retained names, and the largest supported create codec buffers in one observed
workload. It does not include allocator headers, DOS runtime allocations, or
prove a general minimum-memory guarantee; the DOS manager's free-block low-water
from the other workloads remains a separate measurement.

The test-only DOS16 CLI samples `stackavail()` at mini-core, shared-frontend,
and CLI function entries. An earlier 2 KiB-stack corpus observed 172 bytes
available before moving the projected-name scratch strings to the heap, then
627 bytes afterward. With the DOS16 linker stack reservation set to 8 KiB, the
accepted source-tree fixture reaches 34 nested directory frames (the `DEEP`
root plus 33 single-component descendants) and observes 3,706 bytes available
at `src/core/mini/compress.c:42`. DOS16 now caps recursive source traversal at
34 directory frames. The same fixture with 35 directory frames is refused
before entering the next recursive frame; its staged archive is removed and no
final archive is published. The rejected case observes 3,700 bytes at the
compression probe while recording the refusal. This is a workload-derived
supported bound with at least 3,700 bytes sampled remaining in the deepest
tested call path, not a static proof of worst-case stack consumption. The
runner records each command's minimum and source line. Short-lived stack use
between probes, other command paths, compiler/runtime variations, and
Win16/OS/2-16 are not covered. The instrumented probe itself consumes some
stack, and the DOS block value is not a guaranteed C-heap capacity. Watcom
allocator headers and Win16/OS/2-16 runtime measurements remain open.

## Reproduction

```sh
make mini-all
make unit-test-dos16
make CC=gcc dos16-interoperability-test
make CC=gcc dos16-stack-interoperability-test
```

The maps are `build/dos16/bpmini.map`, `build/win16/bpmini.map`, and
`build/os2-16/bpmini.map`. The DOSBox test writes its assertion to
`build/unit-test-dos16/RESOURCE.TXT` and its finalized archive fixture to
`build/unit-test-dos16/LIMIT.BLT`. The allocation-failure runner writes its
result to `build/unit-test-dos16/ALLOCFL.TXT`.
The instrumented runner records per-command stack, DOS-manager largest-block,
and Watcom allocation usable-block observations in
`build/dos16-stack/resource-low-water.txt`, including stack probe source lines.
The maximum writer archive and its run record are retained as
`build/dos16-stack/WRITMAX.BLT` and
`build/dos16-stack/writer-resource-run.json`.
The latest report has a 3,700-byte sampled minimum stack, a 360,144-byte
DOS-manager largest-block minimum, a 163,286-byte instrumented live-usable
allocation high-water, and a largest observed allocation rounding difference
of 5 bytes. The high-water includes the maximum writer table plus BoltLite
buffers. The report does not measure allocator headers, guarantee heap
capacity, certify all workloads, or qualify Win16 or OS/2-16 runtimes.
