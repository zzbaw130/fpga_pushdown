# SF=1 fan-out-bounded Join output validation

Date: 2026-10-11

## Scope

Partitioned Join replay previously decoded a bounded probe chunk but appended
every match from that chunk to a complete `partition_output` `Batch`. A highly
duplicated key could therefore turn one small probe chunk into an arbitrarily
large temporary result before the ordinal-run writer gained control.

The replacement builds the restored build-partition hash table once, probes
rows sequentially, and invokes a row consumer immediately for each INNER,
LEFT, SEMI or ANTI result. The consumer validates the hidden global probe
ordinal and moves the row into a staging-capacity-bounded pending buffer. A
full buffer is written directly as an ordinal Flash run. Buffers are also
flushed at each backwards-chain handle boundary because handles are traversed
newest first and their ordinal ranges are not globally monotonic. Rows within
each run remain ordered, and the existing binary-carry Flash merge restores
the complete probe order.

This removes the full per-chunk `partition_output` object. It does not remove
the final visible Join `Batch`, which is still required by the current DAG
node-result interface.

The final versioned kernel is:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_fanout_stream_v1.so`

## Directed unit validation

The low-memory streaming LEFT Join test exercises partition spill, NULL
extension and ordinal recovery. It asserts that the resident-build fast path
does not use the new buffering counters, while the partitioned path emits:

- 3 output buffers containing 3 result rows;
- a 100-byte maximum pending output buffer;
- 3 ordinal leaf runs, 2 merge records and 5 cumulative merge rows;
- the exact physical result order `1, 5, 2`, including the NULL-extended row.

The optimized suite and ASan/UBSan suite both pass.

## CEMU Q3 at a 32 MiB operator arena

Forced-CSD Q3 used cache fill off, profiling on and the final fan-out-stream
kernel. Its result is byte-for-byte identical to the archived HOST result.

| Counter | Ordinal merge v1 | Fan-out stream v1 |
| --- | ---: | ---: |
| Operator capacity | 33,554,432 B | 33,554,432 B |
| Operator peak | 8,832,780 B | 8,832,780 B |
| Partition data/chain records | 256 / 256 | 256 / 256 |
| Ordinal leaf runs / merges | 32 / 31 | 32 / 31 |
| Ordinal merge rows | 736,065 | 736,065 |
| Resident ordinal handle peak | 150 B | 150 B |
| Streamed output buffers / rows | not reported | 32 / 147,213 |
| Streamed output buffer peak | unbounded `partition_output` | 598,680 B |
| Spill cursor / writer peak | 16,777,200 B / 17,665,560 B | 16,777,200 B / 17,665,560 B |
| Spill write/read | 166,186,244 B / 166,186,244 B | 166,186,244 B / 166,186,244 B |
| Base relation Flash read | 1,519,804,416 B | 1,519,804,416 B |
| Guest-visible query time | 2,218.529 ms | 2,224.722 ms |

The partitioned Join produced 147,213 rows in 32 handle-bounded buffers. Its
largest pending buffer was 598,680 bytes, well below the 32 MiB reusable spill
staging range. The change adds no Flash record and changes neither base reads
nor spill bytes. Query time increased by 6.193 ms (0.279%), which is normal
run-to-run variation because the modeled Flash traffic and FPGA cycles are
unchanged. This is not a TCG host-wall-time performance claim.

## Regression gates

- final kernel forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT`;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 parameter relocations and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  output-capacity/SubPlan/selectivity guards, Q2=HOST and Q16=HOST;
- optimized and ASan/UBSan unit suites: pass.

Raw query outputs, Q3 profile, planner output and AUTO plans are archived in
`tpch_sf1_fanout_stream_validation_raw.tar.gz` with SHA-256
`750d61f6231bf67a4e2c675958b706b8e2646bb0be4c03d06d8de3cd9780e772`.

## Remaining boundary

Partition replay no longer accumulates output proportional to one probe
chunk's join fan-out. The final visible Join output is still reconstructed as
one `Batch`, however, because downstream DAG operators consume materialized
node results. The restored hash table, pending rows, cursor chunks and run
level vectors also use ordinary C++ allocators rather than an allocator carved
from the FDM arena. Merge and adaptive-repartition records remain append-only
until query-local scratch teardown. Strict SF=32 physical-memory bounding
still requires a streaming downstream/final-result interface, arena-backed
containers, streaming Aggregate partition writers and reclaimable scratch
extents.
