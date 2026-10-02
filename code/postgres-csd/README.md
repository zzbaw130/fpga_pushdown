# PostgreSQL CSD extension

`csd_pg` is a PGXS extension for the pinned PostgreSQL 12 tree.  It adds
`csd.exe_path = host|csd|auto`, FPGA-style expression eligibility checks,
CSD scan/join/aggregate/sort paths, and an inspectable operator-tree cost
model.

Build and install it into the project-local PostgreSQL installation with:

```bash
make -C code/postgres-csd \
  PG_CONFIG="$PWD/build/install/bin/pg_config"
make -C code/postgres-csd \
  PG_CONFIG="$PWD/build/install/bin/pg_config" install
```

Load the module in each test session before setting its GUCs:

```sql
LOAD 'csd_pg';
SET csd.exe_path = 'csd';
EXPLAIN SELECT ...;
```

The executor serializes a selected path into a structurally validated ABI-v8
header/node table;
every scan contains the real heap page count and physical PostgreSQL column
layout. Filters, projections, join residuals, HAVING, and final arithmetic are
compiled to bounded postfix bytecode, including fixed-point/date constants,
`LIKE`, scalar-array `IN`, boolean/null logic, `CASE`, and substring. Join,
aggregate, and sort keys are bound to input schemas and emitted as real ABI-v8
descriptor arrays; complex aggregate arguments use a streaming pre-projection
and subquery outputs use positional projection. `PARAM_EXEC` inputs are marked
as relocations. They are patched lazily on the first `ExecCustomScan` call,
after the executor tree is fully initialized, so scalar InitPlans can safely
run before the outer Program Execute.

Q2's correlated scalar `MIN` is decorrelated into a grouped aggregate branch
and two-key join. Q16's hashed `NOT IN` becomes a hash anti-join only after both
columns are proven `NOT NULL`; both branches are merged into the outer CSD DAG.
All 22 TPC-H plans therefore lower with zero pending expressions. The executor
uses FIEMAP to append the heap files' physical NVM extents to the plan. Each
logical scan gets a fixed reusable FDMFS window (`csd.chunk_mb`, 32 MiB by
default); while Program Execute is active, CEMU refills that window directly
from the NVM backend, decodes it, and reuses it for the next chunk. No relation
is copied wholesale into FDM. Decoded scan windows are serialized through two
alternating FDMFS tuple FIFOs (`csd.fifo_mb`, 32 MiB each by default), splitting
a window into multiple FIFO batches when necessary. Output, both FIFOs, a
bounded spill staging window, plan, status, and scan windows are deducted from
`csd.fdm_capacity_mb` (4 GiB by default) before MRS creation. ABI v8 creates one
real FDMFS operator-arena range from all remaining bytes. A zero
`csd.device_work_mem_mb` selects that automatic capacity; a positive value caps
it. Blocking Join/Sort state obtains query-scoped arena leases and spills only
when a lease cannot be satisfied. Spill bytes pass through
`csd.spill_stage_mb` (32 MiB) into query-local physical NVMe extents
(`csd.spill_mb`, 4 GiB); those extents do not consume FDM capacity and are
unlinked after execution. The executor submits the original
`NVME_IOCTL_IO_CMD`, reads the
status/result ranges, and returns PostgreSQL virtual tuples without a silent
host fallback. Numeric
results preserve PostgreSQL dynamic scales for AVG and division, subject to
the fixed-point int64 range.

The ABI-v8 FIFO and operator arena are real, capacity-accounted FDM ranges.
External Sort writes bounded runs to Flash, retains only handles, and reads run
pairs back through the spill staging range for stable merge passes. Oversized
Hash Join build sides radix-partition both inputs and replay one partition pair
at a time; probe ordinals preserve deterministic output order. The current
functional executor still stores decoded tuple payloads and partition-formation
buffers in C++ `Batch` objects. Oversized grouped Aggregate state now
radix-partitions input by the complete group key and replays each partition,
including grouped DISTINCT and AVG; global Aggregate retains its protection
copy. Arena leases enforce the modeled CSD capacity, but end-to-end physical
memory becomes hard-bounded only after scan/partition streaming and incremental
arena-backed state replace the initial full C++ state build.

Run all supplied TPC-H planner cases with `tests/test_csd_planner.sh`.  In
forced mode the test permits a PostgreSQL `Limit` control node above a
`CsdScan`, but rejects host scan, join, aggregate, or sort nodes.

Run `tests/test_csd_tpch_execution.sh` inside CEMU against a running,
NVMe-staged guest cluster to compare all 22 forced-CSD results exactly with the
HOST path. The PostgreSQL executable must retain `CAP_SYS_ADMIN` because the
Linux NVMe passthrough ioctl checks it before reaching CEMU.
