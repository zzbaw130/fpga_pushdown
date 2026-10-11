# SF=1 staging-bounded adaptive k-way Sort validation

Date: 2026-10-12

## Scope

This milestone adopts the earlier AOE design's 8--128-way external merge in
the current NASCENT2-style Sort. The previous implementation merged Flash runs
pairwise, so a large run set repeatedly wrote and reread intermediate records.
The new implementation selects a fan-in from 3 through 128 according to the
actual spill-staging capacity and the exact maximum serialized row size carried
by each in-memory Sort-run descriptor. It retains the binary implementation as
a compatibility fallback when a row cannot safely fit at a higher fan-in.

For fan-in `k`, each cursor receives `stage / (8*k)` bytes. The implementation
conservatively reserves four times that amount for each cursor's descriptors,
payload and decoded row chunk, totaling at most half the staging range. An
intermediate merge reserves the other half for its bounded writer. A final
merge has no writer and streams directly into the separately admitted result
boundary. The heap tie-breaks equal keys by original run order, preserving the
stable ordering of contiguous run groups. SQL Top-N truncation remains active
at every run and merge level.

Final versioned kernel:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_sort_kway_v1.so`

New append-only profile counters are:

- `sort_kway_merge_calls`;
- `sort_kway_input_runs`;
- `sort_kway_max_fan_in`;
- `sort_kway_staging_reserved_peak_bytes`.

## Three-run variable-length validation

The first directed query sorts 30,209 rows including variable-length
`l_comment`, with forced CSD placement, a 1 MiB operator arena, a 1 MiB spill
staging range, cache fill disabled, and profiling enabled. Unlimited,
`LIMIT 100`, and `LIMIT 100 OFFSET 50` outputs all matched HOST byte-for-byte.

```sql
SELECT l_orderkey, l_linenumber, l_comment
FROM lineitem
WHERE l_orderkey < 30000
ORDER BY l_orderkey, l_linenumber
[LIMIT 100 [OFFSET 50]];
```

| Counter | Binary unlimited | 3-way unlimited | Binary LIMIT 100 | 3-way LIMIT 100 | Binary OFFSET | 3-way OFFSET |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Returned/final-stream rows | 30,209 | 30,209 | 100 | 100 | 100/150 | 100/150 |
| Spill records | 4 | 3 | 4 | 3 | 4 | 3 |
| Sort merge rows | 54,460 | 30,209 | 200 | 100 | 300 | 150 |
| Spill write | 4,710,141 B | 2,613,047 B | 34,595 B | 25,893 B | 51,808 B | 38,810 B |
| Spill replay read | 4,710,141 B | 2,613,047 B | 34,595 B | 25,893 B | 51,808 B | 38,810 B |
| Total spill I/O reduction | baseline | 44.5230% | baseline | 25.1539% | baseline | 25.0888% |
| Cursor peak | 757,278 B | 127,692 B | 17,332 B | 17,332 B | 25,924 B | 25,924 B |
| Writer peak | 1,048,566 B | 0 B | 8,666 B | 0 B | 12,962 B | 0 B |
| k-way calls / max fan-in | 0 / 0 | 1 / 3 | 0 / 0 | 1 / 3 | 0 / 0 | 1 / 3 |
| Conservative staging reservation | 0 B | 524,280 B | 0 B | 524,280 B | 0 B | 524,280 B |

The unlimited query removes 4,194,188 B of dependent spill traffic. At the
configured 1,000 MiB/s Flash read and write bandwidths, this is a 4.000 ms
reduction in the current device-time model.

## 229-run multi-level Top-N validation

The second query scans all 6,001,215 `lineitem` rows and returns the first 100
ordered keys. A 1 MiB arena creates 229 initial runs. The new implementation
merges them as 128-way and 101-way intermediate groups, then streams the two
remaining runs through the binary final cursor path. Old binary, new adaptive,
and HOST results have the same SHA-256
`cb0c853d5b3e7d6b5f3b37d66486fdb741116e4ac921594ef4b1f9f7e826c23a`.

| Counter | Binary merge | Adaptive k-way | Difference |
| --- | ---: | ---: | ---: |
| Initial runs | 229 | 229 | 0 |
| Total spill records | 456 | 231 | -225 |
| Sort merge rows | 22,800 | 300 | -22,500 |
| Spill write | 1,840,416 B | 932,316 B | -49.3421% |
| Spill replay read | 1,840,416 B | 251,316 B | -86.3446% |
| Total spill I/O | 3,680,832 B | 1,183,632 B | -67.8434% |
| k-way calls / input runs / max fan-in | 0 / 0 / 0 | 2 / 229 / 128 | enabled |
| Conservative staging reservation peak | 0 B | 1,048,576 B | exactly 1 MiB |

Top-N lets a merge stop after 100 rows, so k-way cursors need not read every
input record completely; this explains why replay-read reduction is larger
than write reduction. The removed 2,497,200 B contributes 2.382 ms less
dependent Flash delay at 1,000 MiB/s. This number is derived from measured
Flash bytes and is not TCG functional wall time.

## Unit and regression gates

- an 8-run unit case performs one 8-way final merge for both unbounded and
  Top-N execution;
- a 130-run unit case performs a 128-way intermediate merge, exercises the
  bounded writer, reaches exactly the 256 KiB staging limit, and verifies
  stable ordering for duplicate keys spread across separate runs;
- optimized host, guest optimized, and ASan/UBSan suites pass;
- forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT` against HOST;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 executor parameter relocations, and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  oversized boundary=HOST, safe SubPlan/selectivity boundaries, Q2=HOST, and
  Q16=HOST.

Directed old/new/HOST outputs and profiles, complete 22-query outputs, forced
planner plans, and AUTO plans are archived in
`tpch_sf1_sort_kway_raw.tar.gz` with SHA-256
`12e13208a9a947b90989fb8006f780b6ec93d24e6df96ae294a399c83c6ddfed`.

## Remaining boundary

The functional model uses a C++ priority queue to express the stable k-way
selection; an FPGA implementation would use a comparator/tournament tree.
The current cycle model still charges the conservative full Sort complexity,
so only measured Flash-byte reductions change modeled device time. Cursor and
heap containers remain ordinary STL allocations governed by a conservative FDM
reservation rather than objects physically allocated inside the FDM range.
Final retained rows still enter the planner-checked complete result `Batch`.
