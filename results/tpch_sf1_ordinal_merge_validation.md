# SF=1 bounded Join ordinal-merge validation

Date: 2026-10-10

## Scope

Partitioned Join previously restored every result into an in-memory
`OrderedJoinRow` vector, stable-sorted the complete vector by hidden probe
ordinal, and then moved the rows into the output `Batch`. This duplicated the
complete result and made reorder memory proportional to output cardinality.

The replacement writes every tagged Join output chunk directly as a sorted
Flash run. A binary-carry merge keeps at most one run handle per level, so
resident reorder metadata is `O(log(run count))`. When a level is occupied,
its older run and the incoming newer run are merged immediately with the
existing bounded two-cursor/one-writer path. Final levels are merged from
oldest to newest, then the winning run is decoded in staging-bounded chunks;
the hidden ordinal is removed as each row moves into the final output.

NULL-key LEFT/ANTI rows use the same path and are buffered only up to the
spill staging capacity. A NULL-extended cell may retain the invalid datum-type
tag only while its NULL flag is set, matching the existing result ABI. Spill
replay releases the shared staging lock after a chunk is fully decoded and
before invoking a downstream spill-writing consumer, then reacquires it for
the next read.

The final versioned kernel is:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_ordinal_merge_v1.so`

## Directed unit validation

The low-memory two-sided streaming LEFT Join produces three logical result
runs. Binary carry performs two merges over five cumulative rows and preserves
the physical probe order `1, 5, 2`, including the final NULL-extended row.
The test asserts zero ordinal runs on the resident-build fast path, and on the
partitioned path asserts:

- 3 leaf ordinal runs;
- 2 merge records and 5 merge rows;
- 12 total data spill records: 7 partition records, 3 leaf runs and 2 merges;
- nonzero, logarithmically bounded resident run-handle metadata.

Optimized and ASan/UBSan suites pass.

## CEMU Q3 at a 32 MiB operator arena

Forced-CSD Q3 used cache fill off, profiling on and the final ordinal-merge
kernel. Its result is byte-for-byte identical to the archived HOST result.

| Counter | Flash chain v1 | Ordinal merge v1 |
| --- | ---: | ---: |
| Operator capacity | 33,554,432 B | 33,554,432 B |
| Operator peak | 8,832,780 B | 8,832,780 B |
| Partition data/chain records | 256 / 256 | 256 / 256 |
| Ordinal leaf runs / merges | 0 / 0 | 32 / 31 |
| Ordinal merge rows | 0 | 736,065 |
| Resident ordinal handle peak | full `OrderedJoinRow` vector | 150 B |
| Spill cursor / writer peak | 0 / 0 | 16,777,200 B / 17,665,560 B |
| Spill write/read | 60,190,616 B / 60,190,616 B | 166,186,244 B / 166,186,244 B |
| Base relation Flash read | 1,519,804,416 B | 1,519,804,416 B |
| Guest-visible query time | 2,020.243 ms | 2,218.529 ms |

Binary levels 0 through 5 require six logical entries. With a
24-byte `SpillHandle` and one occupied byte per entry, the measured descriptor
payload is `6 * 25 = 150` bytes. The implementation writes 32 leaf runs and 31
merge records, increasing both spill read and write by exactly 105,995,628
bytes. At the configured 1,000 MiB/s read and write bandwidths, the additional
device-model delay is about 202.17 ms. The measured guest-visible increase is
198.286 ms, a 1.92% difference. This comparison is a model-accounting check,
not a claim about TCG host wall time.

## Regression gates

- final kernel forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT`;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 parameter relocations and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  output-capacity/SubPlan/selectivity guards, Q2=HOST and Q16=HOST;
- optimized and ASan/UBSan unit suites: pass.

Raw query outputs, Q3 profile, planner output and AUTO plans are archived in
`tpch_sf1_ordinal_merge_validation_raw.tar.gz` with SHA-256
`881662128094edca4e98f190eac2d32e2957997e9f6b3e416bf227a39de7f978`.

## Remaining boundary

Historical note: the subsequent fan-out streaming stage removes the temporary
per-probe-chunk `partition_output`; see
`tpch_sf1_fanout_stream_validation.md`. The paragraph below records the
boundary at the time of this ordinal-merge milestone.

Ordinal reorder metadata and its row copies are now bounded, but the final
visible Join output is still reconstructed as one `Batch` because the current
DAG scheduler exposes materialized node results. A single probe chunk with
extreme one-to-many fan-out can also create a large temporary
`partition_output` before it becomes a run. The hash table, cursor chunks and
run-level vectors still use ordinary C++ allocators; the 150-byte metric is
logical descriptor payload, not allocator overhead. Merge records are
append-only and obsolete runs are reclaimed only when query-local scratch is
destroyed. Strict SF=32 physical-memory bounding therefore still requires a
streaming Join output consumer/result writer, fan-out-bounded probe emission,
arena-backed containers and reclaimable scratch extents.
