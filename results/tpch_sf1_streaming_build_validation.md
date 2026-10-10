# SF=1 two-sided streaming Join partition validation

Date: 2026-10-10

## Scope

This milestone removes the complete build `Batch` for an eligible oversized
Hash Join. Eligibility is deliberately structural: both inputs must be sole
consumer `Scan -> [Filter/Project]` chains, the node must be spillable, and
PostgreSQL's conservative build-side capacity (wire node `arg0`) must exceed
the operator arena. Other Join trees retain the established complete-input
path.

The scheduler runs the build Scan first. Every build FIFO batch is classified
by the complete canonical key and its nonempty selections are written directly
to query-local Flash partitions. Only handles and exact partition byte totals
survive until the probe Scan starts. The planner bound selects the initial
power-of-two fan-out; actual byte totals remain authoritative. If a build
partition exceeds the arena, its records are replayed into twice as many
partitions until every partition fits or the explicit 65,536-partition/skew
limit is reached. Probe batches then use the previous global-ordinal replay
path, preserving INNER, LEFT, SEMI and ANTI semantics and Scan/FIFO order.

The final versioned kernel is:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_stream_build_v2.so`

## Directed validation

The low-memory unit case uses an 80-byte build, a 64-byte arena, two build
FIFO batches and three probe FIFO batches. The conservative bound initially
selects four partitions. Build keys 1 and 5 collide under that mask but split
under an eight-way mask, so the test proves that one runtime repartition pass
occurs. A LEFT join returns all three expected rows in the original probe
order, including one NULL-extended unmatched row. It asserts:

- `stream_join_streamed_build_calls=1`;
- `stream_join_build_batches=2`, `stream_join_build_rows=2`;
- `stream_join_build_repartition_passes=1`;
- five total streamed input batches and nonzero Flash spill/replay;
- operator peak no larger than the 64-byte modeled arena.

The optimized and ASan/UBSan suites pass. A Q7 regression found and fixed an
over-broad first version: a direct build Scan was streamed even though its
probe came from another Join. The final admission check walks the complete
probe-side Filter/Project chain back to a sole source Scan. Final Q7 is exact
and reports `stream_join_streamed_build_calls=0`.

## CEMU Q3 at a 32 MiB operator arena

Forced-CSD Q3 used cache fill off, profiling on, and the final v2 kernel. Its
result is byte-for-byte identical to HOST.

| Counter | Probe-to-Flash milestone | Two-sided streaming build |
| --- | ---: | ---: |
| Operator capacity | 33,554,432 B | 33,554,432 B |
| Operator peak | 29,494,560 B | 8,832,780 B |
| Streamed build calls | 0 | 1 |
| Build FIFO batches / rows | 0 / 0 | 7 / 737,165 |
| Probe FIFO batches / rows | 30 / 3,231,383 | 30 / 3,231,383 |
| All streamed batches / rows | 30 / 3,231,383 | 37 / 3,968,548 |
| Build repartition passes | n/a | 0 |
| Spill records | 4 | 256 |
| Spill write/read | 60,171,304 B / 60,171,304 B | 60,180,376 B / 60,180,376 B |
| Base relation Flash read | 1,519,804,416 B | 1,519,804,416 B |
| Guest-visible query time | 1,973.067 ms | 1,939.764 ms |

The arena peak falls by 20,661,780 bytes (70.06%) because the 29.49 MiB
complete build is no longer retained. Spill payload changes by only 9,072
bytes, but the current per-batch/per-partition record format increases record
metadata from 4 to 256 handles. The small timing difference is not presented
as a performance claim; these are independent TCG samples. Total final Flash
read is 1,579,984,792 bytes, and subtracting replay read leaves the same base
relation read as the earlier milestone.

## Regression gates

- final v2 forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT` against HOST;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 parameter relocations and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  oversized boundary=HOST, safe SubPlan/selectivity boundaries, Q2=HOST and
  Q16=HOST;
- optimized and ASan/UBSan unit suites: pass.

Raw outputs, Q3/Q7 profiles, planner output and AUTO plans are archived in
`tpch_sf1_streaming_build_validation_raw.tar.gz` with SHA-256
`32d310595fd6095d077704f01c767198d4c96debac008eb4209cf67ca7608bbd`.

## Remaining boundary

Historical note: the subsequent bounded spill-chain milestone replaces these
per-record resident handle vectors with Flash-linked records and one fixed
descriptor per radix partition. See
`tpch_sf1_spill_chain_validation.md`. The output/reorder and arena-backed
container limitations below remain current.

Build and probe payloads are now FIFO-bounded for the eligible two-sided Scan
shape, but their `SpillHandle` vectors still grow with every nonempty
batch/partition selection. Repartitioning appends new Flash records and does
not reclaim obsolete scratch extents within the query. Each restored build
partition, the final Join output and the ordinal reorder vector are ordinary
C++ containers; final output is still reconstructed as one `Batch`. Join
subtrees whose input is not a direct Scan pipeline retain the complete-input
path. Consequently this milestone materially reduces Q3 resident memory but
does not claim a strict SF=32 end-to-end physical-memory bound. The next work
is bounded handle chaining/compaction and bounded output/ordinal consumption.
