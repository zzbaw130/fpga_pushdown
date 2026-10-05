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

In `auto` mode the planner retains independent HOST and device-resident CSD
states for every relation.  CSD parents can consume only CSD states; HOST may
consume a materialized CSD boundary, so the plan can switch from CSD to HOST
at most once and can never upload a HOST intermediate back to CSD.  Internal
CSD edges accumulate scan/join/aggregate/sort and spill cost.  A materialized
boundary additionally pays `csd.setup_cost` (default 1000) and returned bytes
times `csd.result_byte_cost` (default 0.001).  Boundaries larger than
`csd.output_mb` are not selectable in AUTO, although their internal device
states remain available to a reducing CSD parent.

`csd.host_cache_fraction=-1` samples heap-file page residency with `mincore`;
set a value in `[0,1]` for a deterministic experiment.  Resident pages reduce
HOST sequential-scan cost toward `csd.host_cached_page_cost` (default 0.33).
`csd.spill_page_cost` (default 0.10) charges estimated Flash write/read passes
after output, spill staging, both FIFOs, and scan windows are deducted from
the configured FDM capacity.  An unresolved `SubPlan`, or a path-specific
SEMI/ANTI cardinality risk, cannot cross to HOST and silently lose its
predicate or row estimate.

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
Hash Join build sides radix-partition both inputs, lease one restored build
partition, and stream the matching probe record in staging-bounded chunks;
probe ordinals preserve deterministic output order. Join and
Aggregate partition classification store row ordinals; a two-pass writer emits
each selection directly to Flash without a copied partition Batch. The current
functional executor still stores decoded scan payloads in C++ `Batch` objects.
Oversized grouped Aggregate state now
radix-partitions input by the complete group key and replays each partition,
including grouped DISTINCT and AVG; global Aggregate retains its protection
copy. Grouped state is accounted during construction and switches to radix
spill as soon as it crosses the modeled arena threshold; per-lane partial state
is released immediately after merge. Arena leases enforce the modeled CSD
capacity, but end-to-end physical memory becomes hard-bounded only after
scan-to-operator streaming and a real arena-backed container allocator replace
the remaining C++ heap objects.

Run all supplied TPC-H planner cases with `tests/test_csd_planner.sh`.  In
forced mode the test permits a PostgreSQL `Limit` control node above a
`CsdScan`, but rejects host scan, join, aggregate, or sort nodes.

Run `tests/test_csd_tpch_execution.sh --mode csd|auto` inside CEMU against a
running, NVMe-staged guest cluster to compare all 22 selected results exactly
with the HOST path.  `tests/test_csd_auto_planner.sh` separately asserts cold
and hot cache switching, a legal CSD-to-HOST cut, output-capacity pruning, and
safe SubPlan/SEMI/ANTI boundaries.  The PostgreSQL executable must retain
`CAP_SYS_ADMIN` because the Linux NVMe passthrough ioctl checks it before
reaching CEMU.

Host-cache fill is enabled by default with `csd.cache_fill=on`.  Each CSD scan
publishes relation ranges through a vendor AER/log-page control plane; the
guest NVMe worker resolves those ranges to registered PostgreSQL heap files and
submits readahead in 32-MiB pipeline groups, then waits for the corresponding
folios to become Uptodate without copying them through a temporary buffer.
`csd.cache_overlap=on`
(the default) starts that worker while Program Execute is still outstanding in
CEMU's modeled FPGA/Flash runtime window.  Set it to `off` only for a controlled
non-overlap baseline.  `csd.cache_wait_ms=0` leaves fill asynchronous; a
positive value is an experimental completion barrier.  `csd.profile=on`
reports execute/prefetch timestamps and bytes completed during the overlap
window.  Run `tests/test_csd_cache_overlap.sh` inside the guest for the A/B
timeline assertion.
