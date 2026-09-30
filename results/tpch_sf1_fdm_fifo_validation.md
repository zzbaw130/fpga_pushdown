# SF=1 ABI v7 FDM-FIFO validation

Date: 2026-09-30

The PostgreSQL extension was compiled inside the CEMU guest with the guest's
`/opt/pgsql12/bin/pg_config`. The guest PostgreSQL instance used the SF=1
cluster staged on the NVMe namespace, and every supplied TPC-H query was run
once with `csd.exe_path = 'csd'` and once with `csd.exe_path = 'host'`.

## Result correctness

`tests/test_csd_tpch_execution.sh` reported `EXACT` for Q1 through Q22. The
second complete run used the final no-C++-TLS kernel build; all 22 normalized
result files matched the HOST path byte for byte.

`tests/test_csd_planner.sh` also passed Q1 through Q22 in forced CSD mode with
zero pending non-scalar expressions, three executor parameter relocations, and
the expected two relational SubPlan rewrites.

## FDM FIFO and Flash profile

Q8 exercises eight raw-heap scans, multiple joins, aggregation, projection,
and the oversized blocking-state spill path. Its ABI-v7 profile reported:

| Counter | Value |
| --- | ---: |
| Scan calls | 8 |
| Input rows | 6,619,871 |
| Flash read bytes | 1,555,210,240 |
| Input chunks | 53 |
| FIFO batches | 42 |
| FIFO write bytes | 633,988,333 |
| FIFO read bytes | 633,988,333 |
| FDM reserved bytes | 436,232,192 |
| Flash/spill write bytes | 600,121,536 |
| Spill read bytes | 0 |

The equal FIFO write/read counters show that each decoded scan batch crossed a
real alternating FDMFS range before entering the downstream operator DAG. The
reservation includes all scan windows, both 32 MiB FIFOs, output, spill staging,
plan, and status ranges, and remains below the configured 4 GiB FDM capacity.
The 600,121,536-byte spill allocation is on the NVMe namespace and is therefore
not charged to the FDM budget.

## Boundary of this milestone

The scan-to-operator transport is now capacity checked and backed by FDMFS, but
the current DAG executor decodes FIFO records into a C++ `Batch`. Join hash
tables, aggregate state, and sort run objects are also still retained in C++
memory after their spill records are written. `spill_read_bytes = 0` makes that
remaining work observable: the next memory-bounding milestone is partitioned
Flash replay for Batch, Join/Aggregate, and Sort state.

The expression scratch stack and worker-lane budget no longer use C++
`thread_local` storage. A query-complete kernel was verified absent from the
CEMU process maps, so rebuilding and loading the same `.so` in later development
does not retain a stale ABI through a long-lived CEMU worker thread.
