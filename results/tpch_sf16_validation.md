# TPC-H SF=16 CSD validation

Date: 2026-10-14

This is a correctness and memory-boundary validation, not a HOST-versus-CSD
performance comparison.  CEMU used TCG plus VM-Freeze, the original in-memory
NVM backend, a 48 GiB namespace, a 4 GiB FDM namespace, and a 16 GiB guest.
The tested kernel was the versioned
`/workspace/code/CEMU/tests/cemu/build/csd_sql_sf16_v9.so`.

## Dataset and method

- PostgreSQL cluster logical file bytes: 33,846,563,123; files: 1,375.
- `lineitem` rows: 95,988,640.
- Staging archive: `data/tpch_s16.stage.tar.zst`, 9,115,519,315 bytes,
  SHA-256 `fea2eb2d5ea4055306e8c35a8a6dcca1ab4be90f0180db4fcd5cd801a3148c94`.
- Forced CSD, cache fill disabled, 13,312 MiB query-local Flash spill,
  `csd.execute_timeout_ms=3600000`, profiling enabled.
- CSD output was compared byte-for-byte with pre-generated PostgreSQL HOST
  golden output.  Q15 view creation/removal was handled explicitly.

The final CSD execution artifacts, HOST goldens, and forced-planner output are
archived in `tpch_sf16_validation_raw.tar.gz`.  It expands to
`tpch_sf16_csd_full_v9/`, `tpch_sf16_host_golden/`, and
`tpch_sf16_planner_v9_forced/`.  The canonical concatenation of normalized
Q1--Q22 output has SHA-256
`7f55833fe3e5b5c4e6fe06f72348df5e8cde258f5b3b902586c2f0568d161e03`
for both CSD and HOST.  The archive is a deterministic GNU tar stream of those
three directories (`--sort=name --mtime=@0 --owner=0 --group=0
--numeric-owner`, then `gzip -n`) and has SHA-256
`4ae3b446c59d37268f9a35ccf25dc58f70b7fe031e60705e49b7d541f3adb42d`.

## Results

- Result comparison: Q1--Q22 all `EXACT` (22/22).
- Forced-CSD planner: Q1--Q22 all pass; zero pending non-scalar expressions,
  three executor parameter relocations, and two relational SubPlan rewrites.
- 25 CSD plans executed.  Every plan reported
  `spill_failed_required_bytes=0`.
- Aggregate Flash accounting: 626,102,308,143 B read and 234,905,285,205 B
  written.  Of the reads, 235,884,629,295 B were spill replay; all writes were
  query-local spill writes.  Base-relation reads were therefore
  390,217,678,848 B.
- Maximum spill high-water mark: 9,137,763,384 B in Q21, below the
  13,958,643,712 B capacity.  Q9, the motivating oversized probe case, peaked
  at 7,830,014,467 B after bounded probe-wave replay.
- Maximum operator-arena peak: 3,936,000,000 B in Q18, below the available
  FDM-derived operator budget.

Per-query values, including all multi-plan queries, are recorded in
`tpch_sf16_validation.csv`.  Its `guest_ms` column is PostgreSQL-observed time
inside the guest: VM-Freeze hides functional C++ wall time, while the CEMU
scheduler's FPGA/Flash model remains pending after unfreeze.  The total is
850,480.919 ms, but this sequential correctness run did not apply the
cold-start sampling protocol and must not be used as a speedup result.

## Implemented scale fixes

- CEMU scheduler runtime accumulation is 64-bit, so modeled device delays no
  longer wrap at 4.29 seconds.
- Query-local spill uses coalescing reusable extents; bounded writers return
  unused reservations, and consumed Sort, ordinal-merge, and Flash-chain
  inputs release their extents.
- Oversized streamed Join probes replay in bounded waves while retaining build
  partitions.  Global hidden ordinals preserve input order across waves.
- Partitioned Join output can stream into downstream Join probe/build paths;
  grouped DISTINCT replay accounts distinct-state memory.
- Constant PostgreSQL `LIMIT + OFFSET` is recovered exactly when PG12 leaves
  `limit_tuples` unset, and fixed-point multiplication removes only lossless
  trailing decimal zeros before declaring int64 overflow.

## Honest boundary

This run proves correctness on a dataset larger than the 4 GiB FDM and proves
that the instrumented operator arena and query-local spill allocator stay
within their configured capacities.  It does **not** yet prove that all
functional-model host memory is physically bounded by FDM.  Some final visible
or unsupported downstream shapes still materialize ordinary C++ STL `Batch`
objects: Q17 and Q18 reported conservative resident peaks of 22,194,284,144 B
and 15,550,897,984 B respectively.  These allocations explain why the
pure-memory CEMU process needs much more host RAM than the modeled 4 GiB FDM.
The remaining final-result and Aggregate shapes must be converted to bounded
streaming consumers before claiming strict end-to-end FDM-bounded execution.

The SF=1 AUTO calibration test deliberately asserts `hot=HOST`.  At SF=16 its
cached `count(lineitem)` still chooses CSD because FPGA aggregation cost is
lower than HOST CPU processing cost even after scan I/O is discounted.  This
scale-dependent choice is not a correctness failure; the SF=16 milestone uses
the full forced-CSD planner coverage above.  The fixed hot-path assertion
should continue to be run against its calibrated SF=1 dataset.
