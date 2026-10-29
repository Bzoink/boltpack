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
and memory manager. This report records requested bytes, not allocator block
overhead or guaranteed available memory. Measured usable-heap/allocator
overhead remains open. The projected-name collision check now uses a checked
520-byte heap scratch block instead of two 260-byte stack arrays; a DOSBox
allocation-failure test confirms `BP_ERR_NOMEM` without a leak. This trades
some heap headroom for stack headroom on that path.

The DOS16 runtime probe tried Watcom `_memavl()` and heap walks. The reported
near-heap free space stayed around 58 KiB while the application allocated its
far buffers, and `_fheapwalk()` did not reach a valid end status in this DOSBox
runtime. The test-only probe now also makes an impossible DOS `INT 21h/AH=48h`
allocation request and records the largest free block returned by DOS on the
expected out-of-memory result. Across the interoperability fixture, the DOS
manager reported a low-water largest block of 361,184 bytes. This measures the
DOS manager's largest available block in this DOSBox setup, not guaranteed
Watcom `malloc` capacity or allocator overhead; those remain unqualified.

The test-only DOS16 CLI samples `stackavail()` at mini-core, shared-frontend,
and CLI function entries. An earlier 2 KiB-stack corpus observed 172 bytes
available before moving the projected-name scratch strings to the heap, then
627 bytes afterward. With the DOS16 linker stack reservation set to 8 KiB, the
expanded 33-level source-tree transfer corpus passes and observes 3,770 bytes
available at `src/core/mini/compress.c:42`. The same run samples the DOS
manager's largest free block and reports a 361,184-byte low-water value. The
runner records the stack probe source file and line. These are workload
observations, not worst-case bounds: short-lived stack use between probes,
other command paths, DOS environments, and Win16/OS/2-16 are not covered. The
instrumented probe itself consumes some stack, and the DOS block value is not a
guaranteed C-heap capacity.
Worst-case DOS16 stack use and the Win16/OS/2-16 runtime measurements remain
open.

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
The instrumented runner records per-command stack and heap low-water
observations in `build/dos16-stack/resource-low-water.txt`, including
available bytes and the probe's source file and line.
