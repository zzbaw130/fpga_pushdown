# SF=1 Join-to-Aggregate partition-stream validation

Date: 2026-10-11

## Scope

This milestone removes the previous materialized fallback from an eligible
partitioned `Join -> [Filter/Project] -> non-DISTINCT Aggregate` pipeline. The
normal path still decodes the Join's final ordinal-sorted Flash run in bounded
chunks and updates one Aggregate hash state. If that state exceeds the
operator arena, the engine now releases the partial state, rereads the intact
Join run, and writes each bounded input chunk directly into radix-partitioned
Flash handle chains. It then aggregates one partition at a time. A partition
that still exceeds the arena is repartitioned using higher hash bits.

Rows are assigned by the complete canonical group key, so every row for one
group remains in one replay partition and no partial-state merge is required.
The path deliberately excludes DISTINCT Aggregate state; oversized individual
group state and exhausted radix bits fail explicitly.

Final versioned kernel:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_stream_agg_partition_v1.so`

## Directed unit validation

The low-memory test uses two Scan inputs, a partitioned LEFT Join, three
result groups, 96-byte FIFOs, a 192-byte operator arena, and a 16 KiB spill
range. The first streaming Aggregate attempt exceeds the arena. The test
requires the partition writer and at least one recursive repartition pass,
then verifies three exact `(group key, count=1)` rows. It also asserts that
Aggregate Flash-chain record write/read counters are nonzero and that peak
operator use never exceeds the 192-byte arena.

Both the optimized host build, ASan/UBSan build, and the guest optimized unit
suite pass.

## Guest low-memory profile

TPC-H Q12 was executed with forced CSD placement,
`csd.device_work_mem_mb=1`, `csd.spill_stage_mb=1`, cache fill disabled, and
profiling enabled. Its result was compared independently with forced HOST and
was byte-for-byte exact.

| Counter | Q12 |
| --- | ---: |
| Join input rows | 1,531,235 |
| Join output rows consumed by downstream pipeline | 31,235 |
| Downstream Aggregate batches | 3 |
| Downstream peak decoded batch | 968,984 B |
| Operator arena limit | 1,048,576 B |
| Operator arena peak | 142,715 B |
| Join ordinal leaf runs / merge records | 12,481 / 12,480 |
| Spill write / replay read | 138,358,591 B / 138,358,591 B |
| Aggregate repartition fallback | 0 |

Q12 has only two groups, so the 1 MiB Aggregate state correctly stays
resident; the runtime fallback is not triggered unnecessarily. The profile
demonstrates that 31,235 Join rows are passed through bounded batches instead
of reconstructing a complete visible Join `Batch`. The directed unit test
exercises the capacity-overflow and recursive Aggregate repartition branch.

## Regression gates

- forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT` against HOST;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 executor parameter relocations, and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  oversized boundary=HOST, safe SubPlan/selectivity boundaries, Q2=HOST, and
  Q16=HOST;
- optimized, guest optimized, and ASan/UBSan unit suites: pass.

Raw results, Q12 low-memory profiles, forced planner outputs, and AUTO plans
are archived in `tpch_sf1_stream_aggregate_partition_raw.tar.gz` with SHA-256
`ecec6d1e50806db7546dea39257a8cb9e4662556bec7d2e38d8c9dbc58daec6d`.

## Remaining boundary

This milestone bounds the Aggregate fallback fed by a partitioned Join final
run, but it does not make every plan shape physically bounded. Join-to-Join,
DISTINCT/unsupported Aggregate consumers, final returned blocking outputs,
and some non-Scan build subtrees can still materialize complete C++ batches.
Hash state, selection ordinals, and chain descriptor vectors still use STL
allocators while their modeled payload is capacity-accounted. Old query-local
Flash scratch records are not reclaimed during recursive repartition.
