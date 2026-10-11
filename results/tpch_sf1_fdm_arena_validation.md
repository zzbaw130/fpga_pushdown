# SF=1 ABI v8 FDM operator-arena validation

Date: 2026-10-01 (Asia/Shanghai)

## Scope

This validation covers the ABI-v8 query-level FDM operator arena and the
FDMFS free-space coalescing fix. PostgreSQL and the extension were compiled
inside the CEMU guest. PGDATA was staged once from 9P onto the emulated NVMe
namespace; query execution did not read table data from 9P.

## FDMFS reuse regression

`tests/test_fdmfs_reuse.sh` filled the 4-GiB FDM namespace with three unequal
ranges, released them in allocation order, and then allocated one full-size
range. The rebuilt guest kernel reported:

```text
FDMFS reuse PASS: coalesced 4294967296 bytes after fragmented release.
```

This pattern failed with the old one-sided free-list merge and reproduces the
cross-query fragmentation seen when Q8 was followed by Q1.

## Q8 then Q1 profile

Q8 was executed first so that Q1 subsequently had to request a larger arena
from the ranges released by Q8.

| Metric | Q8 | Q1 |
| --- | ---: | ---: |
| FDM reserved | 4,294,967,296 | 4,294,967,296 |
| Operator arena | 3,858,735,104 (3.594 GiB) | 4,093,624,320 (3.812 GiB) |
| Operator peak | 600,121,500 (572.320 MiB) | 2,528 |
| Operator allocations | 9 | 2 |
| Operator spills | 0 | 0 |
| Spill write bytes | 0 | 0 |
| Flash write bytes | 0 | 0 |

Q8 therefore keeps its 572-MiB hash-join build state within the available FDM
budget instead of spilling merely because the former fixed 512-MiB threshold
was exceeded. Q1's larger arena allocation succeeding immediately afterward
confirms that query cleanup restored a contiguous FDM pool.

## TPC-H correctness and planning

- The forced-CSD planner suite passed all 22 TPC-H queries under ABI v8, with
  zero pending non-scalar expressions, three executor parameter relocations,
  and two relational SubPlan rewrites.
- The final sequential execution comparison reported exact HOST matches for
  all Q1-Q22 using the kernel that accounts for Join, Aggregate, and Sort
  state. The guest retained the smoke-test setting
  `default_transaction_read_only=on`; the execution, planner, and benchmark
  drivers now make Q15's transaction mode explicit, so its temporary view is
  independent of postmaster defaults.
- The host-side CSD kernel unit test, PostgreSQL extension build, shell syntax
  checks, and Docker CEMU image/kernel build all passed.

## Current modeling boundary

The operator arena is a real FDMFS MRS range and its allocator controls whether
blocking operators spill. The functional C++ model still stores tuple payloads
in `Batch` containers in guest-process memory. Arena-backed STL allocation and
true hash/sort partition replay remain necessary before claiming a physically
hard-bounded FPGA implementation; this milestone establishes the correct FDM
capacity and spill decision policy.
