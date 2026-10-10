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

PostgreSQL upper-relation targets are also an ABI boundary. `SORT` preserves
its input row layout while ordering it, so wire lowering emits an
order-preserving `PROJECT` immediately afterward to trim join-internal/resjunk
columns and restore the exact ordered `PathTarget` column order. The CSD result
schema therefore matches the CustomScan tuple descriptor even when a complex
Join child exposes a wider internal layout.

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
multiple FIFO batches. When the sole downstream blocking node is `SORT`, the
FIFO hands each batch directly to the Sort consumer after any fused
Filter/Project stages. Sort moves rows into its current arena-accounted run and
flushes that run to Flash before accepting more rows when the arena is full.
The same consumer protocol drives a non-DISTINCT global Aggregate, or a
grouped Aggregate whose planner resource bound plus eightfold hash-state
overhead fits the arena. Those plans update their banked hash state per FIFO
batch and never materialize a separate complete Scan `Batch`. An eligible
left/probe Scan chain feeding a Hash Join also consumes FIFO batches directly
after the scheduler completes the right/build subtree. A resident build is
leased once and its hash table is reused across all probe batches. When both
sides are sole Scan/Filter/Project chains and the planner's conservative build
capacity exceeds the arena, build FIFO batches instead write directly to radix
Flash partitions. Exact partition bytes can double the initial fan-out before
probe replay when skew is detected. An oversized build from any other subtree
is still classified from its complete `Batch`, while its eligible left Scan
writes each FIFO batch directly to Flash with a global ordinal. Uncertain,
DISTINCT, or oversized grouped Aggregate inputs still use the accumulating
compatibility path, so the full operator pipeline is not yet hard-bounded.
The visibility contract is
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
| Aggregate | multi-key banked hash table with `SUM/COUNT/MIN/MAX/AVG` and `DISTINCT` state; eligible direct Scan chains update state per FIFO batch | Oversized or uncertain grouped state radix-partitions input rows by the complete group key and replays each partition |
| Equi-join | multi-key build/probe hash join with residual filter, modeled after GQE; an eligible left Scan chain probes one resident build hash per FIFO batch | Eligible two-sided Scan chains stream an oversized build and probe directly to Flash; other oversized builds use complete-input classification before probe batches stream, then matching pairs replay one at a time |
| Sort | multi-key bounded run generation followed by stable merge passes | A direct Scan pipeline grows one arena-accounted run row by row, flushes before exceeding the lease, retains only Flash handles, and reads pairs back for stable merge |

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
cross the FDM FIFOs. Sole-consumer `Scan -> [Filter/Project] -> Sort` and
eligible `Scan -> [Filter/Project] -> Aggregate` chains execute as per-batch
consumer pipelines: the Scan result slot remains empty and only the current
FIFO batch plus bounded operator state is live. For an eligible Join, the
scheduler resolves the right/build subtree first, then the left Scan pipeline
either probes that one resident hash table or writes directly to radix
partitions batch by batch. If the right side is itself a sole Scan pipeline and
its conservative capacity exceeds the arena, it is also consumed batch by
batch into build partitions. Other blocking consumers still accumulate FIFO
output before execution. The task layer remains DAG-level concurrency;
direct-to-Flash streaming Aggregate partitions and double-buffering decode
against consumption remain separate steps rather than being conflated with
thread-level parallelism.

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
range. For a direct Scan pipeline, rows are charged to the operator arena as
they arrive; failure to grow the lease closes and sorts the current run before
writing it. Once a run is written, the sort keeps only its offset/length
handle. Each merge pass opens two row cursors. A cursor reads descriptor and
payload blocks on demand through the bounded FDM staging window; the stable
comparison retains only the two current decoded chunks. A bounded row writer
reserves the next record, buffers at most one staging-window-sized descriptor
plus payload block, and writes descriptors and variable payload directly to
their final Flash offsets. It never constructs a merged `Batch`. A final read
reconstructs the operator output. Top-N sorts stop the cursor merge after N
rows and reserve conservatively for the two input payloads, which bounds their
actual Flash traffic without changing the global top-N result. Spill records
and every read are range-checked before deserialization. Status exposes cursor
and writer peak bytes plus total rows emitted by merge passes.

An oversized equi-join uses power-of-two radix partitions. Both build and probe
rows are serialized to Flash, only partition handles survive the write phase,
and each build partition is read back under an arena lease. The matching probe
record is decoded in row-complete chunks bounded by the reusable spill staging
range, then immediately passed to the hash probe. A hidden probe ordinal is
removed after replay and used to restore the original probe order; residual
predicates see only the logical columns. NULL join keys retain the correct
INNER/LEFT/SEMI/ANTI semantics. A single build row larger than the arena, or an
unresolvable skew partition, is rejected explicitly instead of silently
claiming a bound. Nested loops and transient contention retain the older
protection-copy compatibility path. Replayed build partitions that fit the
arena wait on its condition variable when an independent DAG branch currently
holds capacity, so parallel blocking operators serialize at the memory
boundary instead of failing spuriously. Initial build/probe classification
stores row ordinals rather than copied `Row` objects. A two-pass selection
writer emits cell descriptors and variable payload directly from those source
ordinals, so no partition `Batch` is materialized. Probe ordinals are appended
by the writer without modifying or copying the source row.

When a Join's sole left input is a direct Scan/Filter/Project chain and its
already-completed right batch fits the arena, the scheduler instead takes the
one-sided streaming path. It leases the exact build batch bytes once, creates
one scalar or canonical multi-key hash table, and feeds every decoded left FIFO
batch through the common probe/residual/join-kind logic. The lease is released
only after the last probe batch. If the actual build bytes exceed the arena,
the left Scan still does not materialize. For a build subtree that is not an
eligible Scan chain, the complete build selects a power-of-two fan-out and is
written to Flash. Each probe FIFO batch classifies row
ordinals, writes nonempty selections as separate partition records, and
advances a global ordinal base. Replay consumes every record belonging to one
build partition before advancing; hidden ordinals restore Scan/FIFO order and
LEFT/ANTI NULL-key rows enter the same reorder stream. This decision uses
runtime bytes rather than a planner estimate.

If both sides are sole Scan/Filter/Project chains, PostgreSQL additionally
places the conservative build-side capacity in Join `arg0`. A value larger
than the arena makes the scheduler consume the build Scan first and serialize
each FIFO selection directly to Flash, retaining only partition handles and
exact byte totals. The bound selects an initial power-of-two fan-out; after the
Scan, any partition above the arena is replayed under a doubled radix mask.
This repeats up to 65,536 partitions, after which irreducible skew is rejected.
The build optimization is admitted only when the probe tail can be walked back
through sole Filter/Project nodes to a Scan; a Join-produced probe therefore
keeps its complete build result for the normal executor.

Streamed build and probe records do not retain one controller-memory handle
per nonempty batch selection. Each data record is followed by a packed
40-byte backwards-link record in query-local Flash. A partition retains only
the 16-byte tail offset and record count needed for bounded traversal. Reverse
traversal is semantically safe because build insertion order is unobservable
and probe records carry global ordinals before the final stable reorder.
Adaptive repartition writes a new chain; its obsolete data and link records
remain charged to the current query's scratch capacity until query teardown.

Partition replay also avoids a complete controller-memory ordinal reorder
vector. Every tagged output chunk becomes a sorted Flash run. Binary-carry
merge retains at most one run handle per level and uses the same two bounded
row cursors plus one bounded writer as external Sort. Final levels merge from
oldest to newest; the winning run is decoded by staging-sized chunks and the
hidden ordinal is removed while rows move into the visible result. NULL-key
LEFT/ANTI rows are staged through this path as well. Once a replay chunk is
fully decoded, the shared spill-stage lock is released before invoking a
downstream spill-writing consumer and reacquired before the next read.

Grouped Aggregate uses the same handle/replay infrastructure. All rows for a
group are assigned to one radix partition, so `DISTINCT` sets and AVG's
sum/count pair never require cross-partition reconciliation. Hash collisions
are resolved recursively with later hash bits. Every replayed state must fit
and wait for an arena lease; a single group larger than the arena is rejected.
New groups and grouped `DISTINCT` entries are charged atomically during the
initial build. If the modeled state crosses the arena threshold, the partial
table is discarded immediately and the complete size is projected from build
progress to choose the radix fan-out. Per-lane partial maps are freed directly
after merge. For a direct sole-consumer Scan branch, non-DISTINCT global
Aggregate always consumes FIFO batches directly because its state size is
fixed. A non-DISTINCT grouped Aggregate uses the same path only when PostgreSQL
places its conservative output-capacity estimate in `arg0` and eight times
that value fits the operator arena. The extra factor covers hash buckets, key
copies and accumulator state. A zero, overflowing, or oversized bound retains
the existing complete-input/radix path; runtime arena accounting remains the
final guard. Large global DISTINCT state retains the protection-copy path.

The implementation is still not a proof of a hard controller-memory bound.
Direct Scan-to-Sort plans no longer accumulate a complete decoded Scan
`Batch`, and Sort merge memory is bounded by two cursor chunks plus one writer
chunk. Eligible direct Scan-to-Aggregate plans likewise keep only the current
FIFO batch and the modeled hash state. Eligible Join probe chains keep the
current FIFO batch plus one resident build, but their final Join output still
materializes. With an oversized build, the probe Scan is batch-bounded and
writes directly to Flash. An eligible two-sided Scan shape also streams the
build without a complete build `Batch`; other build subtrees still materialize
for exact fan-out/skew classification. Streamed Join handle payload is bounded
by the radix partition count because per-record metadata is Flash-linked;
partitioned Join ordinal reorder payload is bounded logarithmically by a
binary-carry Flash merge instead of a complete `OrderedJoinRow` vector;
adaptive repartitioning still appends replacement records without reclaiming
old query scratch extents.
Ineligible Aggregate inputs still
accumulate before their radix writers run. The final Sort/Aggregate output is
also reconstructed as a C++ `Batch`. Aggregate and Hash Join no
longer copy partition rows: their selection writer serializes source ordinals
directly through the bounded FDM staging range, and Join probe replay is
chunked by that same limit. The aggregate threshold bounds its modeled state,
but the table and cursor chunks still use ordinary C++ containers rather than
allocators carved from the FDM arena. Streaming Aggregate partition writers,
streaming Join/final-result consumers, fan-out-bounded probe emission and
arena-backed containers are needed for a strict physical FDM-only bound.
Serialized spill records reside in real NVM scratch extents.
`flash_write_bytes` and `spill_write_bytes` count backend writes, while Sort
and Hash Join replay increment both `flash_read_bytes` and
`spill_read_bytes` with the bytes actually fetched.

## Timing model

The functional timing model is deliberately simple and inspectable:
each node charges a fixed 64-cycle launch plus `rows × arg1`, where `arg1` is
the node initiation interval (default 1).  It is accumulated in
`CsdSqlExecutionStatus::modeled_cycles`.  The kernel separately exports total
Flash reads, total Flash writes, spill replay reads, and logical spill writes
through the append-only `cemu_args` ABI.  CEMU derives base relation reads as
`flash_read_bytes - spill_read_bytes`.  Unless PostgreSQL sets an explicit
Program Execute runtime, CEMU automatically keeps the request outstanding for

```
max(ceil(modeled_cycles / fpga_clock),
    ceil(base_read_bytes / flash_read_bandwidth))
+ ceil(spill_read_bytes / flash_read_bandwidth)
+ ceil(flash_write_bytes / flash_write_bandwidth)
```

Base scans feed the FPGA stream and may overlap its compute cycles.  Spill
writes and replay reads are dependency-ordered blocking phases and therefore
are not hidden behind the base-stream maximum.  The formula is deliberately
conservative until per-operator overlap is calibrated.  It also saturates on
counter overflow and never counts functional C++ wall time.  The defaults are
250 MHz and symmetric 1000 MiB/s read/write bandwidth in
`config_example.json`; experiments must record or calibrate both bandwidths.
The intended calibrated model for each streaming stage is

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
notice and appends a bounded vendor log entry identifying a completed input
chunk by namespace, physical offset, relation OID, logical relation offset,
length, and plan generation.  The guest Linux NVMe driver consumes the notice,
verifies the generation, resolves the range to registered PostgreSQL heap
segment file descriptors, and issues inode readahead.  Linux limits one
`POSIX_FADV_WILLNEED` call to the backing device's readahead window, so the
worker submits all such windows in a 32-MiB pipeline group before waiting for
their folios to become Uptodate.  It does not copy relation bytes through a
throwaway kernel buffer.  This populates the heap inode page cache in parallel
with CEMU's modeled device execution window; the controller never writes
directly into host cache pages.  A final zero-length event closes the
generation only after all preceding range entries have been processed.

`csd.cache_fill` enables this best-effort path; `csd.cache_overlap` controls
whether the driver starts reads while Program Execute is outstanding or
defers them until completion for an experimental baseline.  A positive
`csd.cache_wait_ms` is a reproducibility barrier, while the normal zero value
leaves fill asynchronous.  The stats ioctl exposes command and read-completion
timestamps plus bytes completed inside the overlap window.

This is consistent with the NVMe event model in the
[NVMe Base Specification](https://nvmexpress.org/wp-content/uploads/NVM-Express-1_4c-2021.06.28-Ratified.pdf).
The log entry must be idempotent because an AER may be coalesced or delayed.
The PostgreSQL relation/page generation is included so a truncate, rewrite,
or drop invalidates stale prefetch work.

## PostgreSQL placement

The GUC `csd.exe_path` accepts `host`, `csd`, and `auto` (for example,
`SET csd.exe_path = 'auto'`).  `host` suppresses CSD paths; `csd` requires a CSD
alternative for every selected candidate and otherwise returns an explicit
planning error; `auto` compares HOST and CSD states with a one-way placement
dynamic program.

The out-of-tree PostgreSQL 12 extension in `code/postgres-csd` now installs
planner hooks for base relations, joins, and upper relations.  It creates
`CustomPath` alternatives whose modeled device-state cost includes Flash
pages, streamed tuples, hash build/probe, aggregate state, sort comparisons,
and spill passes.  PCIe result return and command setup are charged only when a
device state is materialized as a CSD-to-HOST boundary, not at every internal
CSD edge.

AUTO keeps a private Pareto frontier of device-resident paths for every
`RelOptInfo`, in addition to PostgreSQL's ordinary HOST path tournament.  Join
planning evaluates the cross product of the two device frontiers and each upper
operator evaluates every surviving input state.  Two candidates are comparable
for pruning only when pathkeys, unresolved-SubPlan state, selectivity-risk
state, scan-window count, and expected rows agree; one then dominates only if
its device cost, resource-bound rows, and resource-bound bytes are all no
larger.  This retains alternatives needed by later operators without the
unbounded duplicate explosion seen in naive enumeration.

A CSD parent can use only device-resident children; a HOST parent may consume a
CSD boundary.  The valid transitions are therefore `CSD -> CSD`,
`CSD -> HOST`, and `HOST -> HOST`.  `HOST -> CSD` is impossible, so once a low
operator is placed on HOST every ancestor remains on HOST.  Keeping the private
device states also prevents PostgreSQL from prematurely pruning a CSD scan/join
that a later CSD Aggregate can reduce before returning data.

The cost formulas are:

```text
scan = pages * flash_page_cost + input_tuples * tuple_cost
join = child_device_costs + input_tuples * join_tuple_cost + spill_cost
agg  = child_device_cost + input_tuples * aggregate_tuple_cost + spill_cost
sort = child_device_cost + n * log2(n) * sort_compare_cost + spill_cost
cut  = setup_cost + device_cost + output_bytes * result_byte_cost
```

The spill estimator uses FDM capacity after the output range, spill staging,
two FIFOs, and one scan chunk per relation have been reserved.  Latency and
capacity use separate cardinalities.  PostgreSQL/ANALYZE expected rows drive
normal device, spill, and PCIe costs.  Capacity admission uses physical Scan
page bounds, structural Join bounds, propagated Sort bounds, LIMIT clamps, and
an ANALYZE estimate multiplied by `csd.stats_safety_factor` (default 8) for
Group/DISTINCT, capped by the input bound.  The serialized result bound includes
ABI header/cell overhead and bounded varlena payload.  A boundary larger than
`csd.output_mb` receives `disable_cost`, although its device state can still
feed a later CSD reducer.  The grouped statistical bound is intentionally
conservative but not a formal maximum; runtime checks/spill remain authoritative
and fail explicitly on an underestimate.

`csd.host_cache_fraction=-1` samples every heap segment with Linux `mincore`;
a value in `[0,1]` is a deterministic override.  Resident pages reduce HOST
sequential-scan cost from `seq_page_cost` toward
`csd.host_cached_page_cost`.  The benefit is counted once and reflects the
host page cache populated by the implemented AER worker.

An unresolved `SubPlan` cannot cross a CSD-to-HOST boundary because its
predicate is only valid after whole-DAG CSD rewriting.  SEMI/ANTI joins use a
conservative preserved-side row count; this path-specific estimate propagates
through subsequent CSD joins, and the boundary remains disabled until
Aggregate, DISTINCT, or LIMIT removes the cardinality risk.  This prevents a
severely underestimated boundary from creating a HOST nested-loop explosion.

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
return, compute/cache overlap, automatic cache-residency sampling, and one-way
AUTO placement are implemented.  The 2026-10-09 global-candidate run selected
18 complete CSD, 2 mixed, and 2 pure HOST plans and produced 22/22 exact
results.  With PostgreSQL restarted and the guest page cache dropped before
every sample, modeled guest time was 183.020 s versus 825.114 s for HOST
(4.508x throughput speedup; 6.921x per-query geometric mean).  See
`results/tpch_sf1_auto_global_search_validation.md` for the dual-clock method,
per-query values, and remaining Q20 near-tie.
