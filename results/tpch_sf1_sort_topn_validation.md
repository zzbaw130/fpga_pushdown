# SF=1 SQL LIMIT/OFFSET to external Sort Top-N validation

Date: 2026-10-11

## Scope

This milestone connects PostgreSQL's SQL-level LIMIT estimate to the existing
bounded external-Sort Top-N implementation. At `UPPERREL_ORDERED`, PostgreSQL's
`limit_tuples` value is transported in the Sort descriptor, so every generated
run and every merge level retains at most `LIMIT + OFFSET` rows. The subsequent
Limit node discards OFFSET rows and returns at most LIMIT rows.

The plan ABI remains v8: the existing `CsdSqlPlanNode::reserved` field carries
the non-negative LIMIT offset. A query without a finite positive LIMIT keeps
`top_n=0` and follows the unchanged full-sort path. The planner cost remains
conservative because it still prices the full sort and spill traffic; this
change can therefore miss a beneficial CSD choice, but cannot make AUTO choose
CSD by assuming an unmeasured Top-N saving. Unknown or out-of-range
LIMIT/OFFSET values are not embedded in a CSD Limit node; AUTO retains a HOST
alternative and forced CSD reports the unsupported boundary instead of using
an estimate as SQL semantics.

Final versioned kernel:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_sort_topn_v1.so`

## Directed low-memory validation

The directed queries sort the same 30,209 variable-length rows with forced CSD
placement, `csd.device_work_mem_mb=1`, `csd.spill_stage_mb=1`, cache fill
disabled, and profiling enabled:

```sql
SELECT l_orderkey, l_linenumber, l_comment
FROM lineitem
WHERE l_orderkey < 30000
ORDER BY l_orderkey, l_linenumber
LIMIT 100 [OFFSET 50];
```

The unlimited query, `LIMIT 100`, `LIMIT 100 OFFSET 50`, OFFSET-only (30,159
rows), and `LIMIT 0` all matched HOST byte-for-byte. OFFSET initially exposed
an older correctness gap:
the CSD Limit node truncated to count but did not skip offset. The final version
encodes and applies offset before count, and the unit suite independently covers
`offset=1, count=1`.

| Counter | Unlimited baseline | LIMIT 100 | LIMIT 100 OFFSET 50 |
| --- | ---: | ---: | ---: |
| Input rows | 30,209 | 30,209 | 30,209 |
| Returned rows | 30,209 | 100 | 100 |
| Final-stream rows | 30,209 | 100 | 150 |
| Sort merge rows | 54,460 | 200 | 300 |
| Base-table Flash read | 1,276,665,856 B | 1,276,665,856 B | 1,276,665,856 B |
| Spill write | 4,710,141 B | 34,595 B | 51,808 B |
| Spill replay read | 4,710,141 B | 34,595 B | 51,808 B |
| Total spill I/O | 9,420,282 B | 69,190 B | 103,616 B |
| Spill-I/O reduction | baseline | 99.2655% (136.15x) | 98.9001% (90.92x) |
| Cursor peak | 757,278 B | 17,332 B | 25,924 B |
| Writer peak | 1,048,566 B | 8,666 B | 12,962 B |

At the configured 1,000 MiB/s Flash read and write bandwidths, the measured
byte reduction removes 8.918 ms for LIMIT 100 and 8.885 ms for the OFFSET
variant from the dependent spill component of the current device-time model.
These are modeled Flash-delay reductions, not TCG functional C++ wall time.

## Regression gates

- forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT` against HOST;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 executor parameter relocations, and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  oversized boundary=HOST, safe SubPlan/selectivity boundaries, Q2=HOST, and
  Q16=HOST;
- optimized host, guest optimized, and ASan/UBSan kernel unit suites: pass.

Directed outputs and profiles, the 22-query results, forced planner outputs,
and AUTO plans are archived in `tpch_sf1_sort_topn_raw.tar.gz` with SHA-256
`e4cdd50e20861205b43eda547342f676981aa8811d536c246559c05448ca77df`.

## Remaining boundary

The Top-N limit now reaches the device implementation and bounds spill traffic,
but the planner's expected device cost deliberately remains the full-sort cost.
Future calibration may price bounded run/merge work only after the estimator
can distinguish in-memory and spilled Top-N shapes without optimistic placement.
The final retained rows still enter the planner-checked result boundary rather
than a general Sort-to-operator streaming consumer.
