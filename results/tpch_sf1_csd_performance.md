# TPC-H SF=1 forced-CSD performance

> Historical prototype-wall-time result.  These measurements include the
> functional C++ kernel running under TCG/VM-Freeze and therefore must not be
> presented as FPGA/Flash device latency.  Use
> `tpch_sf1_auto_global_search_validation.md` and its `guest_ms` column for the
> current modeled-device timing method.

Configuration: CEMU TCG/VM-Freeze, guest-built PostgreSQL 12, forced
`csd.exe_path=csd`, `csd.parallel_workers=32`, and `csd.program_runtime=0`.
Elapsed time is measured outside the VM because VM-Freeze pauses the guest
clock.  Before every CSD and HOST sample PostgreSQL is restarted and Linux page
cache is dropped; restart time is excluded.  This clears both PostgreSQL
`shared_buffers` and the guest page cache.  CSD and HOST result text is compared
exactly after removing psql timing/control lines.

- All 22 queries matched exactly.
- All 22 queries were faster on CSD in this cold-start run.
- Total CSD time: 210,090 ms.
- Total HOST time: 951,977 ms.
- Aggregate throughput speedup (`sum(HOST) / sum(CSD)`): 4.531x.
- Geometric-mean per-query speedup: 4.249x.
- Minimum speedup: 1.476x (Q22).
- Maximum speedup: 15.880x (Q1).

The per-query measurements are in `tpch_sf1_csd_performance.csv`.  The combined
latency/speedup chart is available as `tpch_sf1_host_vs_csd.png` and
`tpch_sf1_host_vs_csd.pdf`; regenerate both with
`scripts/plot_tpch_performance.py` (requires Matplotlib and NumPy).

Reproduce the measurements from the host with
`tests/benchmark_csd_tpch.exp`; its environment variables allow the SSH,
PostgreSQL, query, shared-library, and output paths to be overridden.
