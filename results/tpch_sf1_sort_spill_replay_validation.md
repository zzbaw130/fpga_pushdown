# SF=1 external Sort Flash-replay validation

Date: 2026-10-02 (Asia/Shanghai)

## Scope

This milestone replaces the former Sort protection-copy behavior with real
external-run replay. When the FDM operator arena cannot hold the input, Sort
now writes bounded sorted runs to the query-local NVMe spill file, retains only
validated offset/length handles, reads pairs back through the reusable FDM
staging window, performs stable merge passes, and finally reads the result run.
Top-N is applied to every run and merge result.

PostgreSQL and the kernel shared library were compiled inside the TCG CEMU
guest. PGDATA remained on the emulated NVMe namespace at
`/mnt/nvme0/pgdata/tpch_s1`.

## Forced replay case

The operator arena was capped at 8 MiB and this query was forced through CSD:

```sql
SELECT l_orderkey, l_quantity
FROM lineitem
WHERE l_orderkey < 300000
ORDER BY l_orderkey, l_quantity
LIMIT 10;
```

The CEMU status record reported:

| Metric | Bytes / count |
| --- | ---: |
| Base-table Flash reads | 1,276,665,856 |
| Sort spill writes | 23,984,748 |
| Sort spill reads | 23,984,748 |
| Total Flash reads | 1,300,650,604 |
| Spill records | 3 |

The total Flash-read count is exactly the base relation size plus the replayed
spill bytes. The forced-CSD result matched the HOST result byte for byte.

## Regression coverage

- The normal 4-GiB FDM configuration completed Q1 through Q22 with every CSD
  result reported `EXACT` against HOST.
- The standalone kernel test forces a multi-run Sort, asserts nonzero backend
  reads, and requires `flash_read_bytes == spill_read_bytes` for that plan.
- The host unit suite passed under AddressSanitizer and UndefinedBehaviorSanitizer.
- The kernel unit suite also passed after compilation inside the CEMU guest.

## Remaining boundary

At this Sort milestone, Hash Join and Aggregate still wrote protection copies
when an arena lease failed. Hash Join replay was implemented in the following
milestone; Aggregate and fully streamed scan/partition buffers remain. This
result therefore establishes genuine Sort I/O and replay semantics, but is not
the final SF=32 hard-memory-bound claim.
