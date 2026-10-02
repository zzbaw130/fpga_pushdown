# CSD SQL offload design

This document fixes the boundary between PostgreSQL, the CEMU CSD controller,
and a CSD SQL program before the implementation is expanded beyond the current
stand-alone CEMU examples.  It deliberately uses a compact, versioned binary
plan instead of sending PostgreSQL internal structs across NVMe.

## Plan ABI and pipeline contract

The host serializes a `CsdSqlPlanHeader` followed by fixed-size operator records
and optional constant data.  Every record has the same input/output contract:
an input vector stream, an output vector stream, a status record, and a
spill-handle list.  The CEMU program is loaded through its existing shared
library program interface.  The production transport stages the exact plan
blob in a CSD memory-range-set (MRS) entry, then carries its byte length and
MRS index in Program Execute `cparam1` and `cparam2`.
`CSD_SQL_PLAN_STAGED_RANGE` makes this mode explicit.  This is still an NVMe
command submission, but it avoids the CEMU/Linux passthrough overlay: CEMU's
custom `dlen` shares bytes with standard Linux passthrough metadata fields and
cannot safely carry arbitrary plan data.  A plan is rejected when its ABI
version, total byte length, or operator count is invalid.

The ABI names `SCAN`, `INDEX_SCAN`, `FILTER`, `PROJECT`,
`HASH_AGG`, `GROUP_AGG`, `HASH_JOIN`, `MERGE_JOIN`, `NL_JOIN`, `SORT`,
`MATERIALIZE`, `LIMIT`, and `SUBPLAN`.  Join kinds are `INNER`, `LEFT`,
`SEMI`, and `ANTI`.  The plan executor splits at blocking
operators (`HASH_AGG`, `HASH_JOIN`, `SORT`, and `MATERIALIZE`) and keeps all
other compatible operators in one streaming pipeline.  Thus a planner and a
scheduler can use one interface even though the implementations differ.

ABI v8 implements bounded, extent-driven raw-heap `SCAN`, alternating FDM tuple
FIFOs, a query-level FDM operator arena, NVMe-backed spill staging, expression
`FILTER`/`PROJECT`, multi-key
hash/group aggregate, inner/left/semi/anti joins with residual predicates,
multi-key external-style `SORT`/top-N, `MATERIALIZE`, `LIMIT`, and subplan DAG
barriers. `INDEX_SCAN` is explicitly canonicalized to full `SCAN+FILTER`, and
equality `MERGE_JOIN` is canonicalized to `HASH_JOIN`; both are counted in the
execution status. A general `NL_JOIN` is functionally available for correlated
or non-equality cases but is counted as `software_fallback_nodes`, because an
unbounded nested loop is not an honest FPGA streaming primitive. PostgreSQL
should prefer hash semi/anti joins or a scalar-aggregate plus keyed join and
use the fallback only when no bounded transform exists. The complete operator
inventory derived from the 22 supplied query plans is
documented in [TPC-H operator coverage](tpch_operator_coverage.md).

`FILTER`, `PROJECT`, and join residuals share a bounded postfix expression
program. It has no branches, jumps, indirect memory access, or user-defined
calls. Its fixed operators cover TPC-H arithmetic, comparisons, SQL
three-valued `AND`/`OR`/`NOT`, `IS NULL`, `LIKE`, `CASE` as a mux, date-year
extraction, and substring. `BETWEEN` and `IN` lower to comparison/boolean
trees; date plus constant interval is folded by PostgreSQL. Decimal values are
fixed-point `int64` values carrying an explicit scale through scan,
expressions, aggregates, spills, and result cells. Overflow and division by
zero fail the plan rather than wrapping silently.

`SCAN` reads raw 8-KiB PostgreSQL heap pages through a bounded FDM work window
and performs the
page-header, line-pointer, tuple-header, NULL bitmap, fixed-width datum,
varlena and restricted `numeric` decoding inside CEMU. It emits an internal
column batch, never a guest pointer. Each decoded scan window crosses one of two
alternating, fixed-capacity FDM tuple FIFOs; an oversized window is split into
multiple FIFO batches. The current executor then appends decoded FIFO output to
the downstream C++ `Batch`, so this transport boundary is bounded but the full
operator pipeline is not yet hard-bounded. The visibility contract is
intentionally strict: only planner-proven frozen/all-visible pages may use
this path.  External/TOASTed or compressed varlena values and unsupported
numeric encodings fail visibly rather than returning a partial answer.

PostgreSQL obtains the main-fork heap files' physical NVM extents with FIEMAP
and appends them to the plan. During one Program Execute, the CEMU service
callback copies the next logical chunk directly from namespace 1 into the
scan's FDM range. The default window is 32 MiB and is reused until the relation
is exhausted, so a relation larger than FDM never has to be resident in full.
The configured FDM namespace is 4 GiB. Query-local scan windows, two tuple FIFOs
(`csd.fifo_mb`, 32 MiB each by default), result, a bounded spill staging window,
plan, and status reservations are deducted first; all remaining page-aligned
bytes become a real FDMFS operator-arena MRS range. `csd.device_work_mem_mb=0`
uses that full remainder and a positive value caps it. Blocking operators take
query-scoped leases and spill only when the arena cannot satisfy a lease.
`csd.spill_mb` reserves query-local ext4
blocks on the PGDATA NVMe namespace (4 GiB by default), while
`csd.spill_stage_mb` controls only the reusable FDM transfer window (32 MiB by
default). Flash spill capacity is therefore not charged to the FDM budget.

## FPGA-style functional models

The models preserve FPGA dataflow behaviour while being normal C++ code in
CEMU:

| Operator | Functional model | Blocking / spill rule |
| --- | --- | --- |
| Filter, project | SIMD-like lanes executing bounded expression bytecode | Never blocks; output compacts valid lanes |
| Aggregate | multi-key banked hash table with `SUM/COUNT/MIN/MAX/AVG` and `DISTINCT` state | Arena capacity is checked; partition/replay remains required for a physical hard bound |
| Equi-join | multi-key build/probe hash join with residual filter, modeled after GQE | An intrinsically oversized build side radix-partitions both inputs to Flash and replays one matching pair at a time |
| Sort | multi-key bounded run generation followed by stable merge passes | Input takes an arena lease; failed allocation writes runs to Flash, retains only handles, and reads pairs back for stable merge |

The choices follow the streaming filter/project and hash GQE direction in the
[Xilinx Vitis Database Library](https://github.com/Xilinx/Vitis_Libraries/tree/master/database).
Sort must instead be external because its working set is unbounded; its run
generation and merge stages are modeled independently so a near-storage sort
pipeline can overlap Flash I/O and comparison lanes.

## Three-level parallel execution

`CsdSqlPlanHeader::parallel_workers` is the simulated number of replicated
PPU/query-core resources.  Zero selects deterministic single-worker mode;
the implementation caps a request at 32 workers.  This is a functional C++
model running inside CEMU's VM-Freeze callback, not a claim that host CPU
threads are FPGA hardware.

| Trinity level | CSD implementation | Resource rule |
| --- | --- | --- |
| Page | `SCAN` turns every input page into decode tasks | Different page tasks are scheduled over the shared worker budget. |
| Tuple | Each page is split into dynamic tuple lanes. Lane count is derived from its physical attribute count under a 1024-bit binding word, then capped by `parallel_workers`. | A task is `(page, tuple-lane)`; its local output is merged after workers join. |
| Task | The plan DAG executes all currently-ready, independent nodes together (for example the two scans below a hash join). | A query-owned POSIX thread key carries each task's lane budget into child operators, so the total simulated cores never exceed the configured budget and worker state does not depend on C++ TLS. |

`FILTER` and `PROJECT` also partition a batch into contiguous tuple tiles.
`HASH_JOIN`, hash/group aggregate and external-style sort remain blocking operators:
they use their own operator file and the shared Flash spill allocator is
serialized. Scan input is now chunked and reusable and its decoded windows
cross the FDM FIFOs, but each scan still accumulates the FIFO output `Batch`
before a blocking consumer. The current task
layer is DAG-level concurrency, not yet the full FIFO/double-buffer,
per-batch streaming pipeline shown in Trinity. That
requires a bounded batch-queue protocol between compatible operators and is
kept as a distinct next step rather than being conflated with thread-level
parallelism.

CEMU currently retains a shared-library mapping after its unload control
command even when the CSD kernel itself has no TLS segment. Normal repeated
queries safely reuse the loaded implementation, but an in-place rebuild of the
same `.so` pathname can continue executing the old image. Restart CEMU or use a
new versioned `csd.shared_library` pathname after rebuilding the kernel.

## Spill-to-flash

PostgreSQL creates a query-local, preallocated file in PGDATA and obtains its
physical layout with FIEMAP. The open file pins those blocks while CEMU writes
them directly through the internal NVM service. The file is unlinked on both
success and error and is never used as a relation or read through the host
page cache. Spill records contain operator/run identity, tuple and column
counts, cell descriptors, and byte payloads.

`SORT` emits bounded runs and merge intermediates to the designated spill
range. Once a run is written, the sort keeps only its offset/length handle;
merge passes read two records back through the same bounded FDM staging window,
perform a stable merge, and write the next run. A final read reconstructs the
operator output. Top-N sorts truncate every run and merge result to N rows,
which bounds their Flash traffic without changing the global top-N result.
Spill records and every read are range-checked before deserialization.

An oversized equi-join uses power-of-two radix partitions. Both build and probe
rows are serialized to Flash, only partition handles survive the write phase,
and each matching pair is read back before a bounded build/probe pass. A hidden
probe ordinal is removed after replay and used to restore the original probe
order; residual predicates see only the logical columns. NULL join keys retain
the correct INNER/LEFT/SEMI/ANTI semantics. A single build row larger than the
arena, or an unresolvable skew partition, is rejected explicitly instead of
silently claiming a bound. Nested loops and transient contention retain the
older protection-copy compatibility path. Replayed partitions that fit the
arena wait on its condition variable when an independent DAG branch currently
holds capacity, so parallel blocking operators serialize at the memory
boundary instead of failing spuriously.

The implementation is still not a proof of a hard controller-memory bound:
scans accumulate decoded `Batch` objects, radix partition formation currently
duplicates rows in C++ before they are serialized, and Aggregate retains its
state after a protection copy. The spill range itself is only a reusable FDM
staging window; serialized records reside in real NVM scratch extents.
`flash_write_bytes` and `spill_write_bytes` count backend writes, while Sort
and Hash Join replay increment both `flash_read_bytes` and
`spill_read_bytes` with the bytes actually fetched.

## Timing model

The present functional timing model is deliberately simple and inspectable:
each node charges a fixed 64-cycle launch plus `rows × arg1`, where `arg1` is
the node initiation interval (default 1).  It is accumulated in
`CsdSqlExecutionStatus::modeled_cycles`; CEMU's Program Execute runtime
parameter can then impose a query-level delay for calibration.  The intended
calibrated model for each streaming stage is

```
T = T_launch + ceil(rows / lanes) * II / f_clk + bytes_in / BW_in
    + bytes_out / BW_out + T_spill
```

For hash aggregate and join, `T_spill` includes a partition pass when the
state budget is exceeded.  For sort it is the sum of run generation and each
merge pass.  Each term is supplied in CEMU nanoseconds; no CPU wall-clock time
is treated as an FPGA result.  CEMU's existing per-program `runtime` override
and `runtime_scale` remain supported: an explicit runtime takes precedence,
and a scale is useful only for calibration experiments.  Initial parameters
are intentionally configuration values, not claims of a particular FPGA's
performance: clock rate, lane count, II, SRAM budget, Flash bandwidth, and
per-command launch latency are recorded with every experiment.

## Cache return with NVMe AER

An Asynchronous Event Request (AER) is a notification completion, not a DMA
path for arbitrary table data.  The CSD therefore emits a vendor-specific AER
notice that identifies a completed input chunk by namespace ID, LBA range,
and generation.  The host CSD agent consumes the notice, reads the matching
vendor log-page/ring entry, verifies the generation, and issues ordinary NVMe
reads or readahead for the range.  Those reads populate the host page cache in
parallel with CSD computation.  The controller never writes directly into
host cache pages.

This is consistent with the NVMe event model in the
[NVMe Base Specification](https://nvmexpress.org/wp-content/uploads/NVM-Express-1_4c-2021.06.28-Ratified.pdf).
The log entry must be idempotent because an AER may be coalesced or delayed.
The PostgreSQL relation/page generation is included so a truncate, rewrite,
or drop invalidates stale prefetch work.

## PostgreSQL placement

The planned GUC is `csd.exe_path` with values `host`, `csd`, and `auto` (documented
command spelling: `SET csd.exe_path = 'auto'`).  `host` suppresses CSD paths;
`csd` requires a CSD alternative for every selected candidate and otherwise
returns an explicit planning error; `auto` adds both paths to normal PostgreSQL
dynamic-programming path selection.

The out-of-tree PostgreSQL 12 extension in `code/postgres-csd` now installs
planner hooks for base relations, joins, and upper relations.  It creates
`CustomPath` alternatives whose modeled cost includes Flash pages, streamed
tuples, hash build/probe, aggregate state, sort comparisons, command setup,
and result bytes.  `host` suppresses these paths; `csd` prunes host compute
alternatives and rejects unsupported expressions; `auto` leaves both choices
to PostgreSQL's dynamic-programming search.  A configurable
`csd.host_cache_fraction` is already a cost-model input, but is still a manual
value until the AER feedback control plane supplies per-relation observations.

The chosen CSD subtree is represented as one `CustomScan` plus a recursive,
copy-safe operator metadata tree.  Projection and unflattened subquery paths
are retained as metadata rather than executable PostgreSQL child plans, so
internal join/group keys do not cross back into host tuple pipelines.  The 22
supplied TPC-H `EXPLAIN` files pass forced-CSD planning; scalar InitPlans are
separate CSD commands and a top-level PostgreSQL `Limit` may remain as control
orchestration, while host scan/join/aggregate/sort nodes are rejected by the
planner test.

`PlanCustomPath` emits a packed,
structurally self-validated ABI-v8 plan with postorder node IDs, dependencies,
blocking/spill flags, estimates, work-mem budget, logical MRS slots, real heap
page counts, and one physical column descriptor per PostgreSQL attribute.
The scan descriptor intentionally does not assert the frozen-page flag: that
must come from a real visibility/all-visible proof, not from forced mode.

Base-relation restrictions, projections, join residuals, HAVING clauses, and
post-aggregate arithmetic are lowered to the same bounded postfix VM used by
CEMU. This covers fixed-point/date constants, timestamp-at-midnight folding,
arithmetic comparisons, SQL boolean/null logic, `LIKE`, constant-array `IN`,
`CASE`, and substring. Numeric expressions with unknown typmods get an
explicit fixed-point scale inferred from their columns, constants, and
arithmetic operators rather than silently collapsing to integer scale.

Join keys, aggregate group/argument descriptors, aggregate `DISTINCT`, sort
keys/direction/null ordering, and limits are now serialized as real ABI-v8
payloads. Complex aggregate inputs are projected before the FPGA-style
aggregate core; HAVING and final expressions form later streaming stages.
PostgreSQL columns removed from a physical Group Key through functional
dependency are added to the CSD key so the standalone result retains them.

Q2's correlated scalar `MIN` SubPlan is decorrelated into a grouped aggregate
branch and a two-key join. Q16's hashed `NOT IN` SubPlan is converted to an
anti-join only after both comparison columns are proven `NOT NULL`; otherwise
the rewrite is refused so SQL's UNKNOWN result cannot be lost. Both subplans
are merged into the same postorder CSD DAG with newly allocated input ranges.
The forced-CSD TPC-H suite therefore has zero pending non-scalar expressions.

PostgreSQL `PARAM_EXEC` values used by independent scalar InitPlans become
flagged constant relocations: an unpatched relocation is rejected by CEMU as a
bad expression, never interpreted as SQL NULL. The `CustomScan` executor
patches these relocations lazily, maps heap files to physical NVM extents,
creates bounded FDM scan windows and an MRS, submits `NVME_IOCTL_IO_CMD`, checks
the status range, and decodes result cells into PostgreSQL tuples. SF=1 execution
of all 22 supplied queries matches HOST results exactly. Per-relation AER cache
feedback remains the next transport-control milestone.
