# SF=1 ABI v6 Flash-spill validation

Date: 2026-09-30

Environment:

- CEMU under TCG/VM-Freeze (no KVM)
- PostgreSQL 12.22 compiled inside the guest
- PGDATA on `/dev/nvme0n1` mounted as ext4
- FDMFS on `/dev/nvme0m2`, 4 GiB total
- `csd.chunk_mb = 32`
- `csd.spill_stage_mb = 32`
- `csd.spill_mb = 4096`
- forced execution with `csd.exe_path = 'csd'`

`tests/test_csd_tpch_execution.sh` reported `EXACT` for Q1 through Q22 and
finished with:

```text
All 22 TPC-H queries matched HOST results in forced CSD mode.
```

Q8 exercises the oversized multi-join spill path. Its ABI v6 profile reported:

```text
flags=63
flash_read_bytes=1555210240
input_chunks=53
flash_write_bytes=600121536
spill_write_bytes=600121536
spill_read_bytes=0
```

The 600,121,536 spill bytes were transferred through a reusable 32 MiB FDM
staging window into query-local NVMe physical extents. Both the numeric FDM
ranges and `pg_csd_spill_*.tmp` file were absent after execution, confirming
success-path cleanup. `spill_read_bytes` remains zero because radix/run replay
is a later milestone; the functional engine still retains blocking C++ state
after serializing it.
