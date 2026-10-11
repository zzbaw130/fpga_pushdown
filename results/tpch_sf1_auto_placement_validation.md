# SF=1 AUTO operator-placement validation

Date: 2026-10-05.  This result uses the guest-built PostgreSQL 12 extension,
the NVMe-staged `tpch_s1` cluster, CEMU TCG/VM-Freeze, and
`csd_sql_overlap_v2.so`.  It does not use KVM or host-built PostgreSQL code in
the guest.

## Placement algorithm

AUTO keeps two dynamic-programming states for every PostgreSQL relation:

- the normal HOST path selected by PostgreSQL; and
- the cheapest device-resident CSD path, retained in a private planner
  registry even when PostgreSQL prunes its materialized boundary path.

A CSD parent may consume only CSD-resident children.  A HOST parent may consume
either a HOST child or a materialized CSD boundary.  Consequently the only
transitions are `CSD -> CSD`, `CSD -> HOST`, and `HOST -> HOST`; after the first
HOST operator, `HOST -> CSD` is impossible.  This is the requested one-way cut
and also prunes the search space.

The modeled CSD device cost is accumulated without charging PCIe return at
every internal edge:

```text
scan   = flash_pages * flash_page_cost + input_tuples * tuple_cost
join   = child_device_costs + input_tuples * join_tuple_cost + spill_cost
agg    = child_device_cost + input_tuples * aggregate_tuple_cost + spill_cost
sort   = child_device_cost + n * log2(n) * sort_compare_cost + spill_cost
cut    = setup_cost + device_cost + returned_bytes * result_byte_cost
```

The spill term uses the FDM capacity left after output, spill staging, both
FIFOs, and one chunk window per scan have been reserved.  Sort charges the
estimated merge passes; Join/Aggregate charge Flash write/read passes when the
work set exceeds this capacity.  A cut larger than `csd.output_mb` is disabled,
but its device state remains available to a later reducing CSD operator.

`csd.host_cache_fraction=-1` samples each heap segment with Linux `mincore`.
The observed resident fraction reduces HOST sequential-scan cost from
`seq_page_cost` toward `csd.host_cached_page_cost=0.33`.  Values in `[0,1]`
remain available as deterministic overrides.  Cache benefit is charged once:
it lowers HOST cost and is not also added to CSD cost.

The calibrated defaults used here are `setup_cost=1000`,
`result_byte_cost=0.001`, `flash_page_cost=0.05`, `tuple_cost=0.001`,
`join_tuple_cost=0.002`, `aggregate_tuple_cost=0.0015`,
`sort_compare_cost=0.0005`, and `spill_page_cost=0.10`.

Two correctness guards are part of placement rather than executor fallback:

1. A CSD state containing an unresolved PostgreSQL `SubPlan` cannot cross to
   HOST, because CSD lowering would otherwise skip its predicate.  The state
   may still be consumed by a higher CSD node.
2. SEMI/ANTI estimates use the preserved-side cardinality conservatively and
   propagate that path-specific estimate through later CSD joins.  Such a
   state cannot cross to HOST until Aggregate, DISTINCT, or LIMIT removes the
   uncertainty.  This prevents Q21's estimated one-row boundary from feeding
   a HOST nested loop when the actual boundary contains hundreds of thousands
   of rows.

## Planner validation

`tests/test_csd_auto_planner.sh` passed all directed checks:

```text
cold=CSD
hot=HOST
mixed=CSD->HOST
oversized-boundary=HOST
subplan-boundary=safe
selectivity-boundary=safe
```

All 22 supplied TPC-H `EXPLAIN` cases also passed in AUTO mode.  With the
default cost parameters and the cache state of this run, 15 queries selected a
complete CSD path and 7 selected a one-way mixed path; none selected pure HOST.
The directed hot-cache test demonstrates that AUTO can still select pure HOST
when its estimated cost is lower.

## Execution validation and performance

Every AUTO result matched the corresponding HOST result byte-for-byte:

```text
Q01-Q22: 22/22 EXACT
AUTO total:  90,872.251 ms
HOST total: 502,799.632 ms
throughput speedup: 5.5330x
per-query geometric-mean speedup: 5.1641x
queries faster than HOST: 18/22
```

| Query | Placement | AUTO ms | HOST ms | Speedup |
| ---: | :--- | ---: | ---: | ---: |
| 1 | CSD | 1,839.326 | 68,384.294 | 37.179x |
| 2 | mixed | 12,539.731 | 8,968.494 | 0.715x |
| 3 | CSD | 2,152.159 | 15,592.778 | 7.245x |
| 4 | CSD | 2,079.101 | 9,101.140 | 4.377x |
| 5 | CSD | 2,376.076 | 8,937.415 | 3.761x |
| 6 | CSD | 1,826.484 | 12,401.871 | 6.790x |
| 7 | CSD | 2,333.766 | 11,299.830 | 4.842x |
| 8 | CSD | 2,528.703 | 6,321.702 | 2.500x |
| 9 | CSD | 2,668.958 | 25,407.689 | 9.520x |
| 10 | CSD | 2,193.742 | 12,572.737 | 5.731x |
| 11 | CSD | 958.570 | 5,309.703 | 5.539x |
| 12 | CSD | 2,101.176 | 16,132.870 | 7.678x |
| 13 | CSD | 610.412 | 21,012.612 | 34.424x |
| 14 | CSD | 1,881.824 | 4,954.103 | 2.633x |
| 15 | mixed | 3,683.598 | 26,969.128 | 7.321x |
| 16 | mixed | 13,266.659 | 8,788.305 | 0.662x |
| 17 | mixed | 3,437.761 | 66,066.784 | 19.218x |
| 18 | CSD | 3,714.318 | 88,909.031 | 23.937x |
| 19 | mixed | 1,928.045 | 1,858.118 | 0.964x |
| 20 | mixed | 5,835.410 | 61,072.210 | 10.466x |
| 21 | mixed | 20,023.062 | 19,045.464 | 0.951x |
| 22 | CSD | 893.370 | 3,693.354 | 4.134x |

Q2 and Q16 remain cost-model misses: partial offload changes PostgreSQL join
order and disables some HOST parallelism, effects not fully represented by the
current PostgreSQL 12 cardinality/cost estimates.  Q19 and Q21 are within 5%
of HOST in this single run.  These are calibration limitations, not result
correctness failures, and no query-number-specific rule is used.  Before the
general SEMI/ANTI guard, Q21 selected a one-row CSD boundary that actually
returned 337,680 rows and took about 100.8 seconds; the final rule reduces it
to 20.0 seconds.

Raw plans, exact-comparison outputs, and the Q21 `EXPLAIN ANALYZE` diagnostic
are archived in `results/tpch_sf1_auto_placement_raw.tar.gz`.  Guest `psql`
timing excludes the host wall time spent while TCG freezes the VM; as elsewhere
in this project, these numbers represent CEMU's modeled device delay plus guest
execution, not the C++ functional kernel's host wall time or physical-FPGA
measurements.
