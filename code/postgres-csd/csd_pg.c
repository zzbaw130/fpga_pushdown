#include "postgres.h"

#include "fmgr.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "executor/nodeCustom.h"
#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "utils/builtins.h"
#include "utils/guc.h"

#include "csd_pg.h"
#include "csd_executor.h"

PG_MODULE_MAGIC;

void _PG_init(void);
void _PG_fini(void);

int csd_exe_path = CSD_EXE_HOST;
double csd_setup_cost = 1.0;
double csd_flash_page_cost = 0.05;
double csd_tuple_cost = 0.001;
double csd_join_tuple_cost = 0.002;
double csd_aggregate_tuple_cost = 0.0015;
double csd_sort_compare_cost = 0.0005;
double csd_result_byte_cost = 0.0000001;
double csd_host_cache_fraction = 0.0;

static set_rel_pathlist_hook_type previous_rel_hook = NULL;
static set_join_pathlist_hook_type previous_join_hook = NULL;
static create_upper_paths_hook_type previous_upper_hook = NULL;

static Plan *csd_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
								  CustomPath *best_path, List *tlist,
								  List *clauses, List *custom_plans);
static Node *csd_create_scan_state(CustomScan *scan);
static void csd_begin_scan(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *csd_exec_scan(CustomScanState *node);
static void csd_end_scan(CustomScanState *node);
static void csd_rescan(CustomScanState *node);
static void csd_explain_scan(CustomScanState *node, List *ancestors,
								ExplainState *es);
static int csd_metadata_operator_count(List *metadata);

const CustomPathMethods csd_path_methods = {
	.CustomName = "CsdPath",
	.PlanCustomPath = csd_plan_custom_path
};

const CustomScanMethods csd_scan_methods = {
	.CustomName = "CsdScan",
	.CreateCustomScanState = csd_create_scan_state
};

static const CustomExecMethods csd_exec_methods = {
	.CustomName = "CsdScan",
	.BeginCustomScan = csd_begin_scan,
	.ExecCustomScan = csd_exec_scan,
	.EndCustomScan = csd_end_scan,
	.ReScanCustomScan = csd_rescan,
	.ExplainCustomScan = csd_explain_scan
};

static void
csd_rel_hook(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	if (previous_rel_hook != NULL)
		previous_rel_hook(root, rel, rti, rte);
	csd_set_rel_pathlist(root, rel, rti, rte);
}

static void
csd_join_hook(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel,
			  RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra)
{
	if (previous_join_hook != NULL)
		previous_join_hook(root, joinrel, outerrel, innerrel, jointype, extra);
	csd_set_join_pathlist(root, joinrel, outerrel, innerrel, jointype, extra);
}

static void
csd_upper_hook(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel,
			   RelOptInfo *output_rel, void *extra)
{
	if (previous_upper_hook != NULL)
		previous_upper_hook(root, stage, input_rel, output_rel, extra);
	csd_create_upper_paths(root, stage, input_rel, output_rel, extra);
}

void
_PG_init(void)
{
	static const struct config_enum_entry path_options[] = {
		{"host", CSD_EXE_HOST, false},
		{"csd", CSD_EXE_CSD, false},
		{"auto", CSD_EXE_AUTO, false},
		{NULL, 0, false}
	};

	DefineCustomEnumVariable("csd.exe_path",
						 "Select host, forced CSD, or cost-based execution paths.",
						 NULL, &csd_exe_path, CSD_EXE_HOST, path_options,
						 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("csd.setup_cost", "CSD command launch cost.", NULL,
						 &csd_setup_cost, 1.0, 0.0, 1.0e12,
						 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("csd.flash_page_cost", "CSD Flash page-read cost.", NULL,
						 &csd_flash_page_cost, 0.05, 0.0, 1.0e12,
						 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("csd.tuple_cost", "Streaming CSD tuple cost.", NULL,
						 &csd_tuple_cost, 0.001, 0.0, 1.0e12,
						 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("csd.join_tuple_cost", "CSD hash build/probe tuple cost.", NULL,
						 &csd_join_tuple_cost, 0.002, 0.0, 1.0e12,
						 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("csd.aggregate_tuple_cost", "CSD aggregate tuple cost.", NULL,
						 &csd_aggregate_tuple_cost, 0.0015, 0.0, 1.0e12,
						 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("csd.sort_compare_cost", "CSD sort comparator cost.", NULL,
						 &csd_sort_compare_cost, 0.0005, 0.0, 1.0e12,
						 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("csd.result_byte_cost", "CSD result-return byte cost.", NULL,
						 &csd_result_byte_cost, 0.0000001, 0.0, 1.0e12,
						 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("csd.host_cache_fraction",
						 "Observed fraction of relation pages resident in host cache.", NULL,
						 &csd_host_cache_fraction, 0.0, 0.0, 1.0,
						 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("csd.control_device", "CEMU control device.", NULL,
						   &csd_control_device, "/dev/nvme0c3", PGC_USERSET, 0,
						   NULL, NULL, NULL);
	DefineCustomStringVariable("csd.execute_device", "CEMU NVMe execute device.", NULL,
						   &csd_execute_device, "/dev/ng0n3", PGC_USERSET, 0,
						   NULL, NULL, NULL);
	DefineCustomStringVariable("csd.fdm_path", "FDMFS range directory.", NULL,
						   &csd_fdm_path, "/mnt/fdm0", PGC_USERSET, 0,
						   NULL, NULL, NULL);
	DefineCustomStringVariable("csd.shared_library", "Host-visible CSD SQL shared library.", NULL,
						   &csd_shared_library,
						   "/workspace/code/CEMU/tests/cemu/build/csd_sql.so",
						   PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("csd.output_mb", "FDM space reserved for returned rows.", NULL,
						&csd_output_mb, 64, 1, 1024, PGC_USERSET, 0,
						NULL, NULL, NULL);
	DefineCustomIntVariable("csd.spill_mb", "NVMe Flash capacity reserved for query spill runs.", NULL,
						&csd_spill_mb, 4096, 1, INT_MAX, PGC_USERSET, 0,
						NULL, NULL, NULL);
	DefineCustomIntVariable("csd.spill_stage_mb", "Reusable FDM staging-window size for Flash spill I/O.", NULL,
						&csd_spill_stage_mb, 32, 1, 1024, PGC_USERSET, 0,
						NULL, NULL, NULL);
	DefineCustomIntVariable("csd.chunk_mb", "Per-scan reusable FDM input-buffer size.", NULL,
						&csd_chunk_mb, 32, 1, 1024, PGC_USERSET, 0,
						NULL, NULL, NULL);
	DefineCustomIntVariable("csd.fifo_mb", "Size of each alternating FDM tuple FIFO.", NULL,
						&csd_fifo_mb, 32, 1, 1024, PGC_USERSET, 0,
						NULL, NULL, NULL);
	DefineCustomIntVariable("csd.fdm_capacity_mb", "Configured CEMU FDM capacity.", NULL,
						&csd_fdm_capacity_mb, 4096, 64, INT_MAX, PGC_USERSET, 0,
						NULL, NULL, NULL);
	DefineCustomIntVariable("csd.nvm_nsid", "CEMU NVM namespace containing PGDATA.", NULL,
						&csd_nvm_nsid, 1, 1, PG_UINT16_MAX, PGC_USERSET, 0,
						NULL, NULL, NULL);
	DefineCustomIntVariable("csd.parallel_workers", "CSD software FPGA worker lanes.", NULL,
						&csd_parallel_workers, 32, 0, 32, PGC_USERSET, 0,
						NULL, NULL, NULL);
	DefineCustomIntVariable("csd.program_runtime", "CEMU modeled Program Execute runtime.", NULL,
						&csd_program_runtime, 0, 0, INT_MAX, PGC_USERSET, 0,
						NULL, NULL, NULL);
	DefineCustomIntVariable("csd.device_work_mem_mb",
						"Functional-model memory threshold before spill.", NULL,
						&csd_device_work_mem_mb, 512, 0, INT_MAX, PGC_USERSET, 0,
						NULL, NULL, NULL);
	DefineCustomBoolVariable("csd.profile", "Report CSD functional-model time by operator.", NULL,
						 &csd_profile, false, PGC_USERSET, 0,
						 NULL, NULL, NULL);

	RegisterCustomScanMethods(&csd_scan_methods);
	previous_rel_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = csd_rel_hook;
	previous_join_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = csd_join_hook;
	previous_upper_hook = create_upper_paths_hook;
	create_upper_paths_hook = csd_upper_hook;
}

void
_PG_fini(void)
{
	if (set_rel_pathlist_hook == csd_rel_hook)
		set_rel_pathlist_hook = previous_rel_hook;
	if (set_join_pathlist_hook == csd_join_hook)
		set_join_pathlist_hook = previous_join_hook;
	if (create_upper_paths_hook == csd_upper_hook)
		create_upper_paths_hook = previous_upper_hook;
}

static Plan *
csd_plan_custom_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
					 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *scan = makeNode(CustomScan);
	Const *wire_plan;
	bytea *wire_bytes;
	uint32 wire_nodes = 0;
	uint32 wire_expressions = 0;
	uint32 wire_parameters = 0;
	uint32 pending_expressions = 0;
	uint32 subplan_rewrites = 0;
	List *lowering_metadata;

	(void) clauses;
	scan->scan.plan.targetlist = tlist;
	scan->scan.plan.qual = NIL;
	scan->scan.scanrelid = rel->reloptkind == RELOPT_BASEREL ? rel->relid : 0;
	scan->flags = best_path->flags;
	/* custom_paths is intentionally empty: the metadata is one CSD command. */
	Assert(custom_plans == NIL);
	scan->custom_plans = NIL;
	/* Expressions will be encoded into the ABI wire plan before execution. */
	scan->custom_exprs = NIL;
	lowering_metadata = csd_rewrite_subplans(root, best_path->custom_private,
		&subplan_rewrites);
	scan->custom_private = copyObject(lowering_metadata);
	wire_bytes = csd_lower_wire_plan(lowering_metadata, &wire_nodes,
		&wire_expressions, &wire_parameters, &pending_expressions);
	wire_plan = makeConst(BYTEAOID, -1, InvalidOid, -1,
					  PointerGetDatum(wire_bytes), false, false);
	scan->custom_private = lappend(scan->custom_private, wire_plan);
	scan->custom_private = lappend(scan->custom_private,
		makeInteger((int) wire_nodes));
	scan->custom_private = lappend(scan->custom_private,
		makeInteger((int) wire_expressions));
	scan->custom_private = lappend(scan->custom_private,
		makeInteger((int) wire_parameters));
	scan->custom_private = lappend(scan->custom_private,
		makeInteger((int) pending_expressions));
	scan->custom_private = lappend(scan->custom_private,
		makeInteger((int) subplan_rewrites));
	/*
	 * A base-relation CustomScan reads the relation's physical row type, so
	 * setrefs must process its quals as ordinary scan expressions.  Supplying
	 * custom_scan_tlist here would instead make setrefs look for every qual Var
	 * in the projected output (for example, a WHERE-only column), which fails
	 * with "variable not found in subplan target list".
	 *
	 * Join and upper-relation scans have no scanrelid and therefore expose a
	 * virtual tuple described by their custom scan target list.
	 */
	scan->custom_scan_tlist = scan->scan.scanrelid > 0 ? NIL : tlist;
	scan->methods = &csd_scan_methods;
	return &scan->scan.plan;
}

static Node *
csd_create_scan_state(CustomScan *scan)
{
	CustomScanState *state = (CustomScanState *) csd_executor_create_state(scan);

	state->methods = &csd_exec_methods;
	return (Node *) state;
}

static void
csd_begin_scan(CustomScanState *node, EState *estate, int eflags)
{
	csd_executor_begin(node, estate, eflags);
}

static TupleTableSlot *
csd_exec_scan(CustomScanState *node)
{
	return csd_executor_next(node);
}

static void
csd_end_scan(CustomScanState *node)
{
	csd_executor_end(node);
}

static void
csd_rescan(CustomScanState *node)
{
	csd_executor_rescan(node);
}

static void
csd_explain_scan(CustomScanState *node, List *ancestors, ExplainState *es)
{
	CustomScan *scan = (CustomScan *) node->ss.ps.plan;
	const char *name = strVal(list_nth(scan->custom_private, 0));
	double modeled = floatVal(list_nth(scan->custom_private, 2));
	double cache_fraction = floatVal(list_nth(scan->custom_private, 3));
	Const *wire_plan = castNode(Const, list_nth(scan->custom_private, 8));
	bytea *wire_bytes = DatumGetByteaPP(wire_plan->constvalue);

	(void) ancestors;
	ExplainPropertyText("CSD Operator", name, es);
	ExplainPropertyInteger("CSD Operator Count", NULL,
						   csd_metadata_operator_count(scan->custom_private), es);
	ExplainPropertyFloat("CSD Modeled Cost", NULL, modeled, 3, es);
	ExplainPropertyFloat("Host Cache Fraction", NULL, cache_fraction, 3, es);
	ExplainPropertyInteger("CSD Wire Nodes", NULL,
						   intVal(list_nth(scan->custom_private, 9)), es);
	ExplainPropertyInteger("CSD Wire Bytes", NULL, VARSIZE_ANY_EXHDR(wire_bytes), es);
	ExplainPropertyInteger("CSD Expression Programs", NULL,
						   intVal(list_nth(scan->custom_private, 10)), es);
	ExplainPropertyInteger("CSD Parameter Relocations", NULL,
						   intVal(list_nth(scan->custom_private, 11)), es);
	ExplainPropertyInteger("CSD Pending Expressions", NULL,
						   intVal(list_nth(scan->custom_private, 12)), es);
	ExplainPropertyInteger("CSD SubPlan Rewrites", NULL,
						   intVal(list_nth(scan->custom_private, 13)), es);
	ExplainPropertyText("CSD Wire Status",
		"ABI v7 FDM-FIFO/chunked-scan/Flash-spill operator descriptors", es);
	ExplainPropertyText("CSD Transport", "NVMe Program Execute", es);
}

static int
csd_metadata_operator_count(List *metadata)
{
	List *children = (List *) list_nth(metadata, 4);
	ListCell *cell;
	int count = 1;

	foreach(cell, children)
		count += csd_metadata_operator_count((List *) lfirst(cell));
	return count;
}

PG_FUNCTION_INFO_V1(csd_engine_capabilities);

Datum
csd_engine_capabilities(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(
		"abi=4; scan; expression-vm; multikey-join; multikey-aggregate; "
		"multikey-sort; spill-layout; planner-paths; wire-lowering=operators; "
		"subplan-rewrite; transport=nvme-program-execute"));
}
