# SF=1 Scan-to-Sort streaming validation

Date: 2026-10-09

## Scope

This milestone replaces the complete decoded Scan intermediate for a
sole-consumer `Scan -> [Filter/Project] -> Sort` chain. Each decoded window is
split by the existing alternating FDM FIFO and handed to a Sort batch consumer.
The consumer moves rows into one current run, charges every row against the
query-level operator arena, and closes the run before an arena growth would
fail. Closed runs are stably sorted, written to the query-local Flash spill
extent, and represented only by handles.

The append-only execution status adds:

- `stream_input_batches`
- `stream_input_rows`
- `stream_input_peak_bytes`

These counters distinguish actual FIFO-to-operator batch consumption from the
older path that appends every decoded row to a Scan result `Batch`.

## Local functional validation

The C++ unit test constructs a four-row PostgreSQL heap page, uses 96-byte FDM
FIFOs so exactly one two-column row fits in a transfer, and limits the operator
arena to 64 bytes so only one modeled 40-byte row fits in a run. The result is
the stable key order `1,2,3,4`, with:

- 4 streamed input batches and 4 streamed rows;
- 40-byte peak input batch;
- operator peak no larger than 64 bytes;
- multiple run writes and replay reads;
- both optimized and ASan/UBSan builds passing.

## Guest PostgreSQL validation

Versioned kernel:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_stream_sort_v2.so`

The v2 suffix records a concurrency correction: a reconstructed FIFO batch
releases the shared FIFO mutex before invoking a potentially blocking Sort
consumer. This prevents an arena waiter from blocking another independent DAG
branch that must advance its FIFO before releasing an arena lease.

Directed query:

```sql
SELECT l_orderkey, l_linenumber
FROM lineitem
WHERE l_orderkey < 30000
ORDER BY l_orderkey, l_linenumber;
```

Settings included forced CSD placement, `csd.device_work_mem_mb=1`, cache fill
disabled, and profiling enabled. The CSD output matched HOST exactly for 30,209
rows. Reported counters were:

| Counter | Value |
| --- | ---: |
| FIFO batches | 1 |
| FIFO write/read | 1,208,408 B / 1,208,408 B |
| Stream batches | 1 |
| Stream rows | 30,209 |
| Stream peak batch | 1,208,360 B |
| Operator capacity | 1,048,576 B |
| Operator peak | 1,048,560 B |
| Operator allocations | 2 |
| Spill records | 3 |
| Spill write/read | 2,416,828 B / 2,416,828 B |

The three spill records are two initial runs plus their merged run. The input
batch may be larger than the operator arena because it is independently bounded
by the FIFO; the consumer never retains it after moving its rows into runs.

## Regression gates

- forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT` against HOST;
- forced-CSD planner coverage: 22/22 `PASS`, zero pending non-scalar
  expressions, 3 parameter relocations, and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut, oversized
  boundary=HOST, safe SubPlan/selectivity boundaries, Q2=HOST, Q16=HOST;
- optimized unit suite: pass;
- ASan/UBSan unit suite: pass.

The raw exact-query, planner, AUTO and profile outputs are archived in
`tpch_sf1_streaming_sort_exact_raw.tar.gz` with SHA-256
`aefe9c169edd7d44131be03b4da8f18c5cf1578bca7b069a2053740cbb61da68`.

## Remaining memory boundary

This result does not establish an end-to-end physical controller-memory bound.
Scan inputs feeding Join or Aggregate still accumulate complete C++ batches.
Sort run generation is arena-accounted, but a merge pass still reconstructs
both input runs and the merged output in ordinary C++ heap objects even though
its Flash transfers pass through bounded FDM staging. The next memory milestone
is cursor-based stable merge, followed by streaming consumers for Aggregate and
Join and arena-backed containers.
