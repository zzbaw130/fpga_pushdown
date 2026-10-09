# SF=1 cursor-based external Sort merge validation

Date: 2026-10-09

## Scope

This milestone removes complete-run reconstruction from external Sort merge
passes. Each pairwise merge now uses:

- two on-demand spill row cursors;
- descriptor and payload blocks fetched through the reusable FDM spill staging
  range;
- stable left-before-right selection when sort keys compare equal;
- one bounded writer that places result descriptors and variable payload at
  their final offsets in the next Flash spill record;
- conservative record-space reservation for Top-N, while accounting only bytes
  actually transferred.

The merge never constructs either complete input `Batch` or a merged output
`Batch`. The final Sort result is still decoded into a `Batch` for the existing
DAG/output interface.

Append-only status diagnostics are:

- `spill_cursor_peak_bytes`;
- `spill_writer_peak_bytes`;
- `sort_merge_rows`.

## Unit validation

The streaming Scan-to-Sort test now uses eight `(int32, fixed 8-byte text)`
rows, a 96-byte FIFO and a 64-byte operator arena. It runs both a complete sort
and Top-N=3 across three merge levels. The Top-N case deliberately leaves
reserved-but-unwritten gaps in intermediate records and then consumes those
handles in a later merge. The test verifies stable order, exact text payload
offsets, bounded cursor/writer peaks, expected merge row counts, and spill
replay. Optimized and ASan/UBSan builds pass.

## Guest Flash-spill validation

Versioned kernel:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_cursor_merge_v3.so`

The v2 suffix recorded a concurrency correction: cursor handle-range validation
and header fetch now hold the same spill allocator mutex, so a concurrent DAG
branch cannot update the global append offset during validation. The final v3
also turns impossible incomplete-writer/empty-cursor states into explicit ABI
errors rather than returning an unset status.

Both directed tests used forced CSD placement, `csd.device_work_mem_mb=1`,
`csd.spill_stage_mb=1`, cache fill disabled, and profiling enabled. The 1 MiB
staging range forces each run to cross multiple descriptor/payload cursor
blocks.

### Fixed-width rows

```sql
SELECT l_orderkey, l_linenumber
FROM lineitem
WHERE l_orderkey < 30000
ORDER BY l_orderkey, l_linenumber;
```

The 30,209-row CSD result matched HOST exactly.

| Counter | Value |
| --- | ---: |
| Initial runs / merge records | 2 / 1 |
| Spill write/read | 2,416,828 B / 2,416,828 B |
| Operator peak | 1,048,560 B |
| Cursor peak | 524,240 B |
| Writer peak | 1,048,560 B |
| Rows emitted by merge | 30,209 |

### Variable-length payload

```sql
SELECT l_orderkey, l_linenumber, l_comment
FROM lineitem
WHERE l_orderkey < 30000
ORDER BY l_orderkey, l_linenumber;
```

The 30,209-row CSD result, including every `l_comment`, matched HOST exactly.

| Counter | Value |
| --- | ---: |
| Initial runs / merge records | 3 / 2 |
| Spill write/read | 7,323,116 B / 7,323,116 B |
| Operator peak | 1,048,545 B |
| Cursor peak | 757,278 B |
| Writer peak | 1,048,566 B |
| Rows emitted by merge passes | 54,460 |

For both tests, base relation reads are 1,276,665,856 B after subtracting spill
replay from total Flash reads. Cursor/writer peaks are independent of complete
run size and remain bounded by the 1 MiB staging configuration.

## Regression gates

- forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT` against HOST;
- forced-CSD planner coverage: 22/22 `PASS`, zero pending non-scalar
  expressions, 3 parameter relocations, and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut, oversized
  boundary=HOST, safe SubPlan/selectivity boundaries, Q2=HOST, Q16=HOST;
- optimized and ASan/UBSan unit suites: pass.

Raw exact-query, planner, AUTO, directed output and profile files are archived
in `tpch_sf1_cursor_merge_exact_raw.tar.gz` with SHA-256
`69558a36457c64125444fb45465d6aaabb7804e6f4dd0f075015fe2f6de7e138`.

## Remaining boundary

Sort run generation and merge intermediates are now bounded by the FIFO,
operator lease, two cursor chunks and one writer chunk. The final Sort output
still materializes for the current DAG interface. Scan inputs feeding Join and
Aggregate also still accumulate complete C++ batches, and hash/cursor objects
use STL storage rather than an allocator backed directly by the FDM arena.
Those are the remaining requirements before claiming a strict end-to-end
physical FDM-only memory bound at SF=32.
