# SF=1 spill-aware device-time validation

Date: 2026-10-09 (Asia/Shanghai)

## Scope

This milestone makes the automatic CEMU Program Execute delay account for
Flash writes and separates base-table reads from dependency-ordered spill
replay.  The CSD kernel now returns five model counters through the append-only
`cemu_args` ABI: FPGA cycles, total Flash reads, total Flash writes, spill
replay reads, and logical spill writes.

With `csd.program_runtime=0`, CEMU uses:

```text
base_read_bytes = flash_read_bytes - spill_read_bytes
T_stream = max(modeled_cycles / fpga_clock,
               base_read_bytes / flash_read_bandwidth)
T_device = T_stream
         + spill_read_bytes / flash_read_bandwidth
         + flash_write_bytes / flash_write_bandwidth
```

The image was rebuilt and restarted under TCG/VM-Freeze, SF=1 was restaged on
the emulated NVMe namespace, PostgreSQL 12.22 remained the guest-built
`/opt/pgsql12` installation, and the versioned CEMU-visible kernel was:

```text
/workspace/code/CEMU/tests/cemu/build/csd_sql_spill_timing_v1.so
```

The configured model parameters were 250 MHz FPGA clock, 1000 MiB/s Flash read
bandwidth, and 1000 MiB/s Flash write bandwidth.

## Forced Join replay

The following query was executed in forced-CSD mode with AER cache fill off:

```sql
SELECT count(*)
FROM lineitem l
JOIN orders o ON l.l_orderkey = o.o_orderkey
WHERE o.o_orderkey < 300000;
```

The normal arena and the 1 MiB forced-spill arena both returned `299808`.

| Counter | Normal arena | 1 MiB arena |
| --- | ---: | ---: |
| Total Flash reads | 1,490,436,096 B | 1,731,984,820 B |
| Base-table reads | 1,490,436,096 B | 1,490,436,096 B |
| Flash writes | 0 B | 241,548,724 B |
| Spill replay reads | 0 B | 241,548,724 B |
| Logical spill writes | 0 B | 241,548,724 B |
| Operator peak | 1,499,980 B | 750,240 B |
| Spill records | 0 | 4 |

At 1000 MiB/s, each 241,548,724-byte spill direction costs 230.358814 ms, so
the forced replay must add 460.717628 ms after the common streaming phase.

PostgreSQL `EXPLAIN ANALYZE` execution time uses the guest clock and therefore
excludes the C++ functional work performed while CEMU freezes the VM.  Two
paired runs produced:

| Run | Normal arena | 1 MiB arena | Difference |
| --- | ---: | ---: | ---: |
| 1 | 1667.371 ms | 2124.693 ms | 457.322 ms |
| 2 | 1672.798 ms | 2124.228 ms | 451.430 ms |
| Mean | 1670.0845 ms | 2124.4605 ms | 454.3760 ms |

The observed mean increment is 6.3416 ms below the 460.7176 ms byte-model
increment, an absolute relative difference of 1.38%.  This validates that
spill writes and replay reads are no longer hidden behind the base-scan
`max()` term.  The small residual includes executor/scheduler measurement
variation; these two samples are a directed calibration check, not a complete
performance distribution.

## Correctness and regression

The first full regression exposed a pre-existing Q2 boundary mismatch: the CSD
Sort preserved an internal Join row layout while the PostgreSQL CustomScan
descriptor advertised the final ordered target list.  The third returned cell
was therefore numeric but was decoded as `n_name` (`BPCHAR`).  Wire lowering now
adds an order-preserving Project after Sort to trim and reorder the physical
row into the exact upper-relation target layout.  This is a general Sort
boundary rule and contains no query-number special case.

Validation after the fix:

- forced-CSD TPC-H Q1-Q22: 22/22 byte-for-byte `EXACT` against HOST;
- forced-CSD planner lowering: 22/22 passed, zero pending expressions, three
  parameter relocations, and two relational SubPlan rewrites;
- AUTO directed regression: cold=CSD, hot=HOST, legal CSD-to-HOST cut,
  oversized boundary=HOST, safe SubPlan/selectivity boundaries, Q2/Q16=HOST;
- optimized C++ kernel unit suite passed;
- AddressSanitizer and UndefinedBehaviorSanitizer suite passed with leak
  detection disabled as in prior ptrace-based runs;
- PostgreSQL extension compiled successfully both inside the guest and against
  the host PostgreSQL 12 installation;
- the complete CEMU Docker image, Linux guest, uBPF runtime, and QEMU/CEMU
  controller compiled successfully before the end-to-end run.

## Boundary

This result validates SF=1 accounting and the scheduler-visible device window.
It does not claim that the TCG C++ wall time is FPGA time, nor does it replace
SF=32 memory-bound validation.  Spill phases are conservatively serialized
after the base scan/FPGA streaming maximum until per-operator overlap is
calibrated with a more detailed pipeline model.
