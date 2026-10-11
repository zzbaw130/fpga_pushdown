# SF=1 external Sort final merge-and-stream validation

Date: 2026-10-11

## Scope

This milestone adopts the earlier AOE design's final
merge-and-stream principle in the current NASCENT2-style external Sort. The
previous implementation merged the last two ordered Flash runs into another
Flash record and immediately decoded that record into the DAG result. The new
path keeps the same two bounded row cursors but emits their stable merge
directly into the already-reserved Sort output boundary. It therefore removes
one complete intermediate write and one immediate reread without changing run
generation, comparison, Top-N truncation, or result-capacity admission.

Final versioned kernel:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_sort_final_stream_v1.so`

New append-only profile counters are:

- `sort_final_stream_calls`;
- `sort_final_stream_rows`;
- `sort_final_stream_avoided_write_bytes`;
- `sort_final_stream_avoided_read_bytes`.

## Directed variable-length validation

The following query was executed with forced CSD placement,
`csd.device_work_mem_mb=1`, `csd.spill_stage_mb=1`, cache fill disabled, and
profiling enabled:

```sql
SELECT l_orderkey, l_linenumber, l_comment
FROM lineitem
WHERE l_orderkey < 30000
ORDER BY l_orderkey, l_linenumber;
```

All 30,209 rows, including every variable-length `l_comment`, matched HOST
byte-for-byte for both the previous cursor-merge kernel and the final-stream
kernel.

| Counter | Previous final Flash run | Direct final stream | Difference |
| --- | ---: | ---: | ---: |
| Base-table Flash read | 1,276,665,856 B | 1,276,665,856 B | 0 B |
| Spill write | 7,323,116 B | 4,710,141 B | -2,612,975 B |
| Spill replay read | 7,323,116 B | 4,710,141 B | -2,612,975 B |
| Total spill I/O | 14,646,232 B | 9,420,282 B | -5,225,950 B (-35.68%) |
| Spill records | 5 | 4 | -1 |
| Final-stream rows | 0 | 30,209 | +30,209 |
| Avoided final write/read | 0 B / 0 B | 2,612,975 B / 2,612,975 B | exact |
| Cursor peak | 757,278 B | 757,278 B | 0 B |
| Writer peak | 1,048,566 B | 1,048,566 B | 0 B |
| Sort merge rows | 54,460 | 54,460 | 0 |

With the configured 1,000 MiB/s Flash read and write bandwidths, the current
device-time formula removes 4.984 ms of dependent spill delay for this query.
This is a modeled Flash-delay reduction derived from measured bytes, not the
TCG functional C++ wall time.

A SQL `LIMIT 100` variant also matched HOST exactly. PostgreSQL currently keeps
that LIMIT as a separate plan node for this query, so the profile honestly
shows 30,209 final-stream rows rather than claiming SQL-level early Top-N. The
kernel unit suite separately exercises `CsdSqlSortSpec::top_n=3` across three
merge levels and verifies the same direct final merge.

## Regression gates

- forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT` against HOST;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 executor parameter relocations, and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  oversized boundary=HOST, safe SubPlan/selectivity boundaries, Q2=HOST, and
  Q16=HOST;
- optimized, guest optimized, and ASan/UBSan unit suites: pass.

Raw new/old/HOST directed outputs and profiles, the 22-query results, forced
planner outputs, and AUTO plans are archived in
`tpch_sf1_sort_final_stream_raw.tar.gz` with SHA-256
`0eada4dccf14df271f388be328ec9dd02a5f353bb12e2345aab35b6a878452c6`.

## Remaining boundary

The final merge now avoids a redundant Flash record, but its rows still enter
the existing complete DAG/result `Batch`. This is safe under the separately
reserved and planner-checked `csd.output_mb` boundary, but it is not yet a
general Sort-to-operator batch consumer. Earlier merge levels remain pairwise;
an adaptive k-way merge requires subdividing and accounting the shared FDM
staging window across all simultaneous cursors before it can be claimed as a
bounded implementation.
