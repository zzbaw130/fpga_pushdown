# SF=1 grouped Aggregate Flash-replay validation

Date: 2026-10-03 (Asia/Shanghai)

## Scope

Oversized grouped Aggregate state now partitions source rows by the canonical
complete group key, serializes the partitions to query-local NVMe Flash, and
replays them through the bounded FDM staging window. Because every row for one
group remains in one partition, SUM, COUNT, MIN, MAX, AVG and grouped DISTINCT
retain their normal semantics without a second partial-state combination pass.

If multiple groups collide in one oversized partition, replay recursively uses
later hash bits. Each reconstructed partition waits for and obtains an arena
lease. A single group whose state exceeds the arena is rejected explicitly.
During the initial build, every new group and grouped `DISTINCT` entry is
accounted before insertion. Crossing the arena threshold aborts the partial
build immediately, projects the complete state size from rows processed, and
selects the first radix fan-out without ever constructing the full state.
Per-lane partial maps and duplicate states are released as soon as their merge
finishes.

## Forced replay case

The operator arena was capped at 1 MiB and this query was forced through CSD:

```sql
SELECT l_orderkey, count(*)
FROM lineitem
WHERE l_orderkey < 300000
GROUP BY l_orderkey;
```

The CEMU status record reported:

| Metric | Bytes / count |
| --- | ---: |
| Base-table Flash reads | 1,276,665,856 |
| Aggregate partition writes | 5,996,736 |
| Aggregate partition reads | 5,996,736 |
| Total Flash reads | 1,282,662,592 |
| Operator arena | 1,048,576 |
| Peak replay state lease | 796,876 |
| Successful replay leases | 16 |
| Spill records | 16 |
| Output groups | 74,999 |

After sorting both outputs by `l_orderkey`, all 74,999 CSD rows matched HOST
byte for byte.

## Regression coverage

- The host unit test forces a fused Project plus three-key Aggregate through
  recursive replay at a 640-byte budget. It covers SUM, COUNT(DISTINCT), AVG,
  MIN and MAX and requires real spill reads and replay-time leases.
- The final shared library and unit suite compiled and passed inside the TCG
  CEMU guest.
- With the normal 4-GiB FDM configuration, Q1 through Q22 all reported `EXACT`
  against HOST using the final Aggregate replay library.
- The incremental-threshold version retained the same forced-replay metrics:
  16 records, 5,996,736 bytes written/read and a 796,876-byte peak lease. This
  confirms that the projected fan-out selected the same one-pass partitioning.
- The direct row-selection writer retained those metrics and all 74,999 exact
  rows after removing the final per-partition `Batch` copy.

## Remaining boundary

The initial grouped-state build is now threshold-bounded by the modeled arena,
and partition classification keeps only source-row ordinals. The two-pass
selection writer serializes those rows directly, so no partition `Batch`
remains. However, the complete decoded scan input is still a C++ `Batch`, while
the hash state uses ordinary C++ containers rather than an allocator backed by
the FDM arena itself. A streaming scan consumer and arena allocator are still
required for a physical SF=32 memory bound. Global aggregates, particularly a
large global DISTINCT set, retain the protection-copy compatibility path.
