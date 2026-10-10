# SF=1 one-sided streaming Scan-to-Join validation

Date: 2026-10-10

## Scope

This milestone lets an eligible
`Scan -> [Filter/Project] -> Hash/Merge Join` probe branch consume decoded
FIFO batches without first constructing the complete left/probe `Batch` in
the DAG result vector. The scheduler completes the right/build subtree first,
leases its exact serialized batch size from the query operator arena, builds
one read-only multi-key hash table, and reuses it for every probe batch.
INNER, LEFT, SEMI and ANTI semantics and the residual expression are evaluated
by the same probe implementation as the complete-input path. Equality Merge
Join remains explicitly canonicalized to the hash implementation.

The path is deliberately one-sided. It is selected only when the right/build
batch actually fits the operator arena. If it does not fit, the probe Scan is
executed normally and the existing radix/Grace implementation partitions both
complete inputs to Flash. The final Join output is still materialized.

Final versioned kernel:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_stream_join_v2.so`

## Unit validation

The directed test uses a three-row probe heap page, a two-row build heap page,
and a 96-byte FIFO, which emits one probe tuple per batch.

- With a 128-byte arena, the 80-byte build is leased once, three probe batches
  are consumed, two joined rows are exact, and spill read/write stay zero.
- With a 64-byte arena, all streaming-Join counters stay zero and the same
  result is produced through radix/Grace spill and replay with nonzero Flash
  read/write counters.

Optimized and ASan/UBSan builds pass. Existing join-kind, residual, multi-key,
low-memory replay and complete TPC-H tests run in the same suites.

## Guest profiles

Both profiles used forced CSD placement, the normal 4 GiB FDM budget,
profiling enabled, and the final v2 kernel.

| Counter | TPC-H Q3 | TPC-H Q5 |
| --- | ---: | ---: |
| Scan calls / input chunks | 3 / 47 | 6 / 50 |
| FIFO batches overall | 37 | 40 |
| FIFO bytes written/read | 251,659,996 B | 495,646,297 B |
| Streaming Join invocations | 1 | 2 |
| Probe batches consumed | 1 | 2 |
| Probe rows consumed | 29,949 | 10,001 |
| Peak decoded probe batch | 598,980 B | 400,000 B |
| Resident build bytes | 58,973,200 B | 23,279,081 B |
| Query operator-arena peak | 192,086,040 B | 480,097,200 B |
| Spill write/read | 0 B / 0 B | 0 B / 0 B |
| Base Flash read | 1,519,804,416 B | 1,521,639,424 B |
| Guest-visible query time | 1,821.995 ms | 2,848.480 ms |

`stream_join_*` counts only the new direct probe consumers. The larger
query-level arena peak includes other blocking operators in each plan. The
guest-visible time follows the FPGA/Flash device model; functional C++ work
executed during VM-Freeze is not FPGA latency. Because the Join probe is called
inside the Scan consumer, its functional wall time is currently included in
the enclosing Scan profile bucket; the dedicated streaming counters are the
authoritative evidence for this path.

## Regression gates

- final v2 forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT` against HOST;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 parameter relocations and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  oversized boundary=HOST, safe SubPlan/selectivity boundaries, Q2=HOST and
  Q16=HOST;
- optimized and ASan/UBSan unit suites: pass.

Raw v2 query outputs, Q3/Q5 profiles, forced planner output and AUTO plans are
archived in `tpch_sf1_stream_join_validation_raw.tar.gz` with SHA-256
`a8b22c61121bce30ee7a07c88e84ef73ef9469b36aac75dbeedc81e077e02619`.

## Remaining boundary

This removes the complete probe-side Scan batch only when a resident build
fits. The build `Batch`, hash table and final Join output remain ordinary C++
containers; the arena lease models their capacity but does not allocate those
objects from FDM. Oversized builds still require both complete inputs before
the existing radix writer runs. Direct per-batch partition writers for both
sides, an arena-backed hash table, and bounded output consumption are still
required before claiming a strict end-to-end SF=32 controller-memory bound.
