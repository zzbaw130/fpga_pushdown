# SF=1 AUTO global-candidate validation

Date: 2026-10-09.  This run uses guest-built PostgreSQL 12, the NVMe-staged
`tpch_s1` cluster, CEMU TCG/VM-Freeze, and `csd_sql_overlap_v2.so`.  KVM and
host-built PostgreSQL binaries are not used in the guest.

## What changed

The old planner retained only the lowest-device-cost CSD state per
`RelOptInfo`.  The new planner retains a Pareto frontier and enumerates every
device-child pair for Join and every surviving device input for upper
operators.  A candidate is dominated only by a state with equivalent
pathkeys, unresolved-SubPlan state, selectivity-risk state, scan count, and
expected rows whose device cost, resource-bound rows, and resource-bound bytes
are all no larger.  This is a complete search over the surviving frontier, not
a query-number-specific placement rule.

Latency and capacity no longer share one cardinality.  Expected
PostgreSQL/ANALYZE rows drive device, spill, and PCIe return cost.  Capacity
admission separately uses physical Scan bounds, structural Join bounds,
propagated Sort bounds, LIMIT bounds, and an eight-times ANALYZE safety factor
for Group/DISTINCT capped by its input bound.  ABI-v8 result-size estimation
includes the result header, per-column cells, and bounded varlena payload.
The grouped bound is conservative but statistical, not a mathematical maximum;
runtime capacity/spill checks remain authoritative and fail explicitly on an
underestimate.

This general output-capacity rule removes the two known partial-offload misses:
Q2 and Q16 now retain their parallel all-HOST plans even when
`csd.host_cache_fraction=0`.  No planner branch checks a TPC-H query number.

## Validation method

Directed AUTO regression passed:

```text
cold=CSD
hot=HOST
mixed=CSD->HOST
oversized-boundary=HOST
subplan-boundary=safe
selectivity-boundary=safe
Q2=HOST
Q16=HOST
```

All 22 AUTO `EXPLAIN` cases passed.  Default placement was 18 complete CSD,
2 mixed (Q17/Q20), and 2 pure HOST (Q2/Q16).  The final rebuilt extension then
passed Q1-Q22 byte-for-byte result comparison: 22/22 `EXACT`.

The performance driver restarts PostgreSQL and drops the guest page cache
before **every** AUTO and HOST sample.  It records two clocks:

- `guest_ms` is the paper/device-model metric.  Guest time is frozen while the
  functional C++ kernel runs, then includes the FPGA/Flash modeled outstanding
  delay and normal PostgreSQL work.
- `wall_ms` includes TCG plus functional C++ implementation overhead and is
  retained only as a prototype diagnostic.  It must not be reported as FPGA or
  NVMe device latency.

Q15 reports the sum of all `Time:` records in its create-view/query/drop-view
script.  Every performance pair also passed exact normalized result comparison.
The performance run preceded a final tightening that added scan count to the
Pareto equivalence key; the final rebuild selected the same 18/2/2 placements
and passed the complete exact regression.  That tightening changes planner
pruning only, not operator execution.

## Results

```text
AUTO guest total: 183,019.742 ms
HOST guest total: 825,114.403 ms
throughput speedup: 4.5083x
per-query geometric-mean speedup: 6.9207x
queries faster than HOST: 20/22

AUTO wall total: 411,124 ms
HOST wall total: 867,529 ms
```

| Query | Placement | AUTO guest ms | HOST guest ms | Speedup |
| ---: | :--- | ---: | ---: | ---: |
| 1 | CSD | 1,952.677 | 85,841.442 | 43.961x |
| 2 | HOST | 20,863.384 | 18,878.329 | 0.905x |
| 3 | CSD | 2,328.680 | 42,629.733 | 18.306x |
| 4 | CSD | 2,254.600 | 22,751.027 | 10.091x |
| 5 | CSD | 3,283.383 | 27,674.362 | 8.429x |
| 6 | CSD | 1,965.277 | 17,621.179 | 8.966x |
| 7 | CSD | 4,027.164 | 35,772.134 | 8.883x |
| 8 | CSD | 6,324.429 | 20,103.195 | 3.179x |
| 9 | CSD | 3,063.612 | 49,762.270 | 16.243x |
| 10 | CSD | 2,394.522 | 28,870.519 | 12.057x |
| 11 | CSD | 3,369.296 | 14,328.082 | 4.253x |
| 12 | CSD | 2,274.897 | 27,992.848 | 12.305x |
| 13 | CSD | 906.592 | 32,624.228 | 35.986x |
| 14 | CSD | 2,067.698 | 14,350.965 | 6.941x |
| 15 | CSD | 15,316.781 | 41,567.424 | 2.714x |
| 16 | HOST | 14,553.573 | 13,720.314 | 0.943x |
| 17 | mixed | 11,727.325 | 77,738.040 | 6.629x |
| 18 | CSD | 3,789.993 | 125,944.818 | 33.231x |
| 19 | CSD | 2,091.191 | 6,695.536 | 3.202x |
| 20 | mixed | 71,840.182 | 73,561.888 | 1.024x |
| 21 | CSD | 5,372.609 | 39,740.594 | 7.397x |
| 22 | CSD | 1,251.877 | 6,945.476 | 5.548x |

Q2 and Q16 execute pure HOST plans; their 9.5% and 5.7% single-sample gaps are
cold-I/O/TCG variance rather than partial-offload overhead.  A separate cold Q2
smoke repeat measured 19,176.669 ms versus 19,218.811 ms.  Q20 is the remaining
near-tie at 1.024x and is the clearest target for later cost calibration.  It is
not a catastrophic selection and its result is exact.

Machine-readable values are in `tpch_sf1_auto_global_search.csv`.  Directed
plans, all 22 final plans, and final normalized/raw exact outputs are archived
in `tpch_sf1_auto_global_search_raw.tar.gz`.
