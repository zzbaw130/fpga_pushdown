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

ABI v4 implements raw-heap `SCAN`, expression `FILTER`/`PROJECT`, multi-key
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

`SCAN` reads raw 8-KiB PostgreSQL heap pages from an MRS and performs the
page-header, line-pointer, tuple-header, NULL bitmap, fixed-width datum,
varlena and restricted `numeric` decoding inside CEMU.  It emits an internal
bounded column batch, never a host pointer.  The visibility contract is
intentionally strict: only planner-proven frozen/all-visible pages may use
this path.  External/TOASTed or compressed varlena values and unsupported
numeric encodings fail visibly rather than returning a partial answer.

## FPGA-style functional models

The models preserve FPGA dataflow behaviour while being normal C++ code in
CEMU:

| Operator | Functional model | Blocking / spill rule |
| --- | --- | --- |
| Filter, project | SIMD-like lanes executing bounded expression bytecode | Never blocks; output compacts valid lanes |
| Aggregate | multi-key banked hash table with `SUM/COUNT/MIN/MAX/AVG` and `DISTINCT` state | Current model serializes oversized state; partition/replay remains required for a hard memory bound |
| Equi-join | multi-key build/probe hash join with residual filter, modeled after GQE | Current model records oversized build state; radix partition/replay remains required for a hard memory bound |
| Sort | multi-key bounded run generation followed by stable merge passes | Functional runs are serialized to `spill-to-flash`; the C++ model still retains run objects during replay |

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
| Task | The plan DAG executes all currently-ready, independent nodes together (for example the two scans below a hash join). | A thread-local reentry guard prevents a child operator from recursively creating another worker pool, so the total simulated cores never exceed the configured budget. |

`FILTER` and `PROJECT` also partition a batch into contiguous tuple tiles.
`HASH_JOIN`, hash/group aggregate and external-style sort remain blocking operators:
they use their own operator file and the shared Flash spill allocator is
serialized.  The current task layer is DAG-level concurrency, not yet the
full FIFO/double-buffer, per-batch streaming pipeline shown in Trinity.  That
requires a bounded batch-queue protocol between compatible operators and is
kept as a distinct next step rather than being conflated with thread-level
parallelism.

## Spill-to-flash

Spill extents live in a reserved CSD namespace range, never in PostgreSQL's
relation files.  An extent header contains plan ID, operator ID, partition or
run number, tuple count, checksum, and generation.  The controller allocates
and frees extents only after the whole query completes or aborts.  This gives
join and sort the same semantic safety as PostgreSQL temporary files without
making host page-cache traffic part of the operation.

`SORT` emits bounded runs and merge intermediates to the designated spill
range. The current aggregate/join model also serializes oversized blocking
state there, but it does not yet replay radix partitions. The present C++
functional model therefore validates spill allocation, layout and accounting;
it is not yet a proof of a hard controller-memory bound because scan batches
and sort run objects are retained in host memory. Bounded FIFO streaming plus
Flash deserialization/replay is a separate required milestone before SF32
memory claims. Writes are counted when serialized; `spill_read_bytes` remains
zero until a real replay path reads the extents, avoiding fictional I/O in the
cost calibration.

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

This is not completed query execution.  `PlanCustomPath` now emits a packed,
structurally self-validated ABI-v4 plan with postorder node IDs, dependencies,
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
keys/direction/null ordering, and limits are now serialized as real ABI-v4
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
bad expression, never interpreted as SQL NULL. The `CustomScan` executor still
errors instead of submitting an incomplete plan or silently falling back. The
next stage must patch those executor Params, submit the staged plan through
CEMU Program Execute, and decode result tuples. Per-relation AER cache feedback
follows that transport work.
