# SF=1 streaming Scan-to-Aggregate validation

Date: 2026-10-10

## Scope

This milestone lets a sole-consumer
`Scan -> [Filter/Project] -> Aggregate` chain consume decoded FIFO batches
without first building the complete Scan `Batch` in the DAG result vector.
The streaming hash state preserves COUNT, SUM, AVG sum/count and scale,
MIN/MAX, NULL rules, projected aggregate arguments, and multi-column group
keys.

The path is deliberately conservative:

- a non-DISTINCT global Aggregate is eligible because its state size is fixed;
- a non-DISTINCT grouped Aggregate is eligible only when PostgreSQL's
  resource-bound output bytes, transported in node `arg0`, multiplied by an
  additional eightfold hash-state factor fit the operator arena;
- DISTINCT, missing/overflowing bounds and larger grouped states retain the
  existing complete-input radix replay path.

The extra factor covers hash metadata, canonical key copies, group values and
accumulators. Runtime arena accounting remains the final protection and fails
explicitly if a supposedly resident state exceeds capacity.

Final versioned kernel:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_stream_aggregate_v3.so`

## Unit validation

The directed test uses five two-column heap tuples, a 96-byte FIFO that emits
one tuple per batch, and a 2 KiB operator arena. A streaming Project multiplies
the value column by two before a two-group Aggregate computes COUNT, SUM, MIN,
MAX and AVG. The result is exact, five FIFO batches are consumed, two group
states are leased, and both spill read and write counters remain zero.

Optimized and ASan/UBSan builds pass. Existing grouped DISTINCT and low-memory
radix replay tests also pass, demonstrating that uncertain/spill cases remain
on the compatibility path.

## Guest profiles

Both profiles used forced CSD placement, cache fill disabled, profiling
enabled, the normal 4 GiB FDM budget, and the final v3 kernel.

| Counter | TPC-H Q1 | TPC-H Q6 |
| --- | ---: | ---: |
| FIFO batches consumed | 29 | 29 |
| Stream input rows | 5,884,033 | 119,810 |
| Peak decoded FIFO batch | 26,160,338 B | 179,440 B |
| FIFO bytes written/read | 717,853,418 B | 4,793,792 B |
| Query operator-arena peak | 2,528 B | 134 B |
| Spill write/read | 0 B / 0 B | 0 B / 0 B |
| Base Flash read | 1,276,665,856 B | 1,276,665,856 B |
| Guest-visible query time | 1,524.596 ms | 1,514.971 ms |
| VM-Freeze functional C++ time | 13,704.264 ms | 1,305.685 ms |

The final row distinguishes clocks intentionally. Guest-visible time follows
the FPGA/Flash device model. Functional C++ time is host work performed while
the VM is frozen and is not reported as FPGA latency.

## Regression gates

- final v3 forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT` against HOST;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 parameter relocations and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  oversized boundary=HOST, safe SubPlan/selectivity boundaries, Q2=HOST and
  Q16=HOST;
- optimized and ASan/UBSan unit suites: pass.

Raw v3 query outputs, Q1/Q6 profiles, forced planner output and AUTO plans are
archived in `tpch_sf1_stream_aggregate_exact_raw.tar.gz` with SHA-256
`3b376a40233846cc6177360304606cc2ecae57aca9c5d3b62ba75b371ae9cb94`.

## Remaining boundary

This is a memory-resident Aggregate consumer, not yet a streaming spill
writer. Join inputs, DISTINCT/uncertain/oversized Aggregate inputs, and final
blocking outputs still materialize C++ batches. Hash tables are ordinary STL
containers whose modeled bytes are leased from the FDM arena rather than
objects allocated directly inside that range. Direct-to-Flash partition
writers and arena-backed containers remain necessary before claiming a strict
end-to-end SF=32 controller-memory bound.
