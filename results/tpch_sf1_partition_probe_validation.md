# SF=1 streaming Join probe-to-Flash validation

Date: 2026-10-10

## Scope

This milestone extends the one-sided Join consumer to builds that exceed the
operator arena. The already-materialized right/build `Batch` is classified
with the same exact radix fan-out and skew checks as the established
Grace-style path. Its partitions are written once. The sole left/probe
`Scan -> [Filter/Project]` branch then consumes each FIFO batch directly:
rows are classified by the complete canonical join key and each nonempty
selection is serialized to its Flash partition without appending the batch to
the DAG result vector.

Every spilled probe row carries a global, cross-batch ordinal. Partition replay
removes that hidden cell and stable-sorts the output by the ordinal, preserving
the original parallel Scan/FIFO order for INNER, LEFT, SEMI and ANTI joins.
NULL-key LEFT/ANTI rows are retained with the same ordinal. Each build
partition is restored under an arena lease and every corresponding probe
record is decoded in staging-bounded row chunks.

The scope is intentionally asymmetric: the complete build `Batch` and final
Join output are still materialized. This milestone eliminates accumulated
probe row payloads; its small vector of per-batch/per-partition handles still
grows with the number of emitted records. It does not claim a fully streaming
two-sided Grace join or a strict constant-memory proof.

Final versioned kernel:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_partition_probe_v2.so`

## Directed unit validation

The test uses a three-row probe Scan, a two-row build Scan, a 96-byte FIFO,
and two arena settings.

- At 128 B, the 80-byte build is resident, three probe batches reuse one hash,
  two INNER rows are exact, and no spill occurs.
- At 64 B, the build is radix-partitioned and all three probe batches are
  written directly to their partitions. A LEFT join returns three exact rows,
  including a NULL-extended unmatched row. Two parallel tuple lanes emit keys
  in `1,3,2` order; replay restores that exact global FIFO order.

The low-memory test asserts `stream_join_partitioned_calls=1`, nonzero spill
write/read, and operator peak no larger than the 64-byte arena. Optimized and
ASan/UBSan suites pass.

## CEMU Q3 capacity comparison

Both runs used forced CSD placement, cache fill disabled, profiling enabled,
and the final v2 kernel. The 32 MiB run was separately compared with HOST and
was byte-for-byte exact.

| Counter | Default 4 GiB FDM | 32 MiB operator arena |
| --- | ---: | ---: |
| Operator capacity | 4,026,515,456 B | 33,554,432 B |
| Operator peak | 192,086,040 B | 29,494,560 B |
| Streaming Join invocations | 1 | 2 |
| Partitioned streaming Join invocations | 0 | 1 |
| Probe batches consumed | 1 | 30 |
| Probe rows consumed | 29,949 | 3,231,383 |
| Peak probe FIFO batch | 598,980 B | 7,036,920 B |
| Build bytes across streaming joins | 58,973,200 B | 67,805,980 B |
| Spill records | 0 | 4 |
| Spill write/read | 0 B / 0 B | 60,171,304 B / 60,171,304 B |
| Base relation Flash read | 1,519,804,416 B | 1,519,804,416 B |
| Guest-visible query time | 1,820.451 ms | 1,973.067 ms |

The total low-memory `flash_read_bytes` is 1,579,975,720 B; subtracting the
60,171,304 B replay read gives the unchanged base-relation read above. The
additional modeled time is therefore associated with ordered spill write and
replay, not a larger base-table scan. Functional C++ work during VM-Freeze is
not reported as FPGA latency.

## Regression gates

- final v2 forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT` against HOST;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 parameter relocations and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  oversized boundary=HOST, safe SubPlan/selectivity boundaries, Q2=HOST and
  Q16=HOST;
- optimized and ASan/UBSan unit suites: pass.

Raw v2 query outputs, the 32 MiB exact comparison, both Q3 profiles, forced
planner output and AUTO plans are archived in
`tpch_sf1_partition_probe_validation_raw.tar.gz` with SHA-256
`460c833204a395dc15ff2a965d2d017331f3559f7b0222472eaeb04759739485`.

## Remaining boundary

The build subtree must still finish as a complete C++ `Batch` before exact
fan-out and skew inspection. The output rows and the ordinal reorder vector
also accumulate, and the partition-handle vectors grow with the count of
nonempty batch selections. Hash tables and row vectors use ordinary STL
allocation while their modeled build partitions are protected by FDM arena
leases. A fully bounded SF=32 path still requires a streaming/adaptive build
partition writer, bounded handle management, bounded output consumption or
external ordinal merge, and arena-backed containers.
