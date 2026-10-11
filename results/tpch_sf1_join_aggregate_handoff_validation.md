# SF=1 partitioned Join-to-Aggregate handoff validation

Date: 2026-10-11

## Scope

Partitioned Join already finishes with a stable, ordinal-sorted Flash run. The
previous executor decoded that complete run into a visible Join `Batch`, then
executed its sole downstream Filter/Project chain and Aggregate. The new path
recognizes

```text
partitioned Join -> [Filter/Project]* -> non-DISTINCT Aggregate
```

and replays the final run in spill-staging-bounded chunks. Each chunk loses its
hidden ordinal, passes through the stateless chain, and updates one streaming
Aggregate state. The complete visible Join `Batch` is not constructed.

The admission rule is semantic rather than estimate-only: all intermediate
nodes must be sole consumers and the Aggregate must use the supported
non-DISTINCT state. Runtime arena accounting remains authoritative. If grouped
state outgrows the arena, the partial state is released and the still-intact
final Join run is replayed through the existing materialized/radix compatibility
path. A capacity miss therefore cannot silently return a partial aggregate.

Final versioned kernel:

`/workspace/code/CEMU/tests/cemu/build/csd_sql_join_agg_stream_v2.so`

## Directed unit validation

The low-memory two-sided Scan/LEFT-Join test now has two Aggregate variants:

- a global `COUNT(*)` consumes the final ordinal run directly and returns 3;
- a deliberately undersized grouped `COUNT(*)` begins direct consumption,
  exceeds the arena, increments the fallback counter, rereads the run and
  returns the exact three groups `(1,1)`, `(2,1)` and `(5,1)`.

Resident and partitioned Join-only variants assert that the new downstream
counters remain zero. Optimized and ASan/UBSan suites both pass.

## CEMU 32 MiB measurements

Forced-CSD Q12, Q14 and Q19 used cache fill off and a 32 MiB operator arena.
The baseline is the preceding fan-out-stream kernel; the handoff column is the
new kernel. Both versions are byte-for-byte exact against archived HOST output.

| Query | Join rows handed downstream | Replay batches | Largest downstream chunk | Fallbacks | Baseline / handoff guest time |
| --- | ---: | ---: | ---: | ---: | ---: |
| Q12 | 31,235 | 1 | 3,510,755 B | 0 | 2,088.929 / 2,004.960 ms |
| Q14 | 77,062 | 1 | 10,835,462 B | 0 | 1,814.643 / 1,760.806 ms |
| Q19 | 127 | 1 | 24,738 B | 0 | 1,608.625 / 1,639.274 ms |

Flash traffic is exactly unchanged across the two kernels:

| Query | Total Flash read | Flash/spill write | Spill replay read |
| --- | ---: | ---: | ---: |
| Q12 | 1,634,434,731 B | 143,998,635 B | 143,998,635 B |
| Q14 | 1,427,215,328 B | 116,986,848 B | 116,986,848 B |
| Q19 | 1,323,337,078 B | 13,108,598 B | 13,108,598 B |

The handoff consumes the final run read that the old materialization already
required; it adds no spill record or replay. The small guest-time differences
are run-to-run variation and are not a physical-FPGA speedup claim.

## Regression gates

- clean-restored forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT`;
- forced-CSD planner: 22/22 `PASS`, zero pending non-scalar expressions,
  3 parameter relocations and 2 relational SubPlan rewrites;
- AUTO directed planner: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  output-capacity/SubPlan/selectivity guards, Q2=HOST and Q16=HOST;
- optimized and ASan/UBSan unit suites: pass.

The PostgreSQL transport also clears the complete status range before execute.
Because status fields are append-only within ABI v8, an older compatible kernel
that writes only the known prefix now leaves newly appended counters at zero
instead of exposing stale FDM bytes.

Raw full-regression/planner output is archived in
`tpch_sf1_join_aggregate_handoff_validation_raw.tar.gz` (SHA-256
`a294c7517f8ee080aba913126ed3ec572f850b22f2b0b4be8a8321710e004672`).
New-path directed profiles are in
`tpch_sf1_join_aggregate_handoff_probe_raw.tar.gz` (SHA-256
`5beda81163615312135ca69e936d8f92234939f21152ca7ec4a940b1fb865082`).
The clean old-kernel comparison is in
`tpch_sf1_join_aggregate_handoff_baseline_raw.tar.gz` (SHA-256
`1f773d85941895d002864dd1e272c84d5f8b3e1563d438bac8d152d883d8c87e`).

## Watchdog and remaining boundary

`csd.execute_timeout_ms` now controls only the host NVMe ioctl watchdog and
defaults to 600,000 ms for TCG functional execution. It does not enter
`modeled_cycles`, `csd.program_runtime` or the FPGA/Flash timing formula.
`EINTR`/`ETIMEDOUT` now reports a watchdog-specific hint instead of the
misleading CAP_SYS_ADMIN hint.

An intentionally extreme Q21 run with a 32 MiB arena exceeded this watchdog.
Linux issued abort and later controller reset while the C++ VM-Freeze worker
continued consuming host CPU. That discarded run was not used as correctness
or performance evidence; CEMU was restarted, SF=1 was restaged and all final
gates above ran in the clean environment. Until CEMU supports cooperative
kernel cancellation, a timed-out Program Execute requires this clean restart
before trustworthy measurements.

This milestone removes the full final Join `Batch` only when a compatible
Aggregate is the sole downstream blocking consumer. Join output feeding
another Join, DISTINCT/unsupported Aggregate state, final result serialization,
and the compatibility fallback can still materialize complete batches. A true
streaming Aggregate partition writer and Join-to-Join/final-result consumers
remain necessary for a strict SF=32 end-to-end controller-memory bound.
