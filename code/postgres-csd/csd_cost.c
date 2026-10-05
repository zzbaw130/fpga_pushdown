#include "postgres.h"

#include <fcntl.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>

#include "access/table.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "storage/bufmgr.h"
#include "utils/rel.h"

#include "csd_executor.h"
#include "csd_pg.h"

typedef struct CsdDevicePathState
{
	PlannerInfo *root;
	RelOptInfo *rel;
	Path *path;
} CsdDevicePathState;

static List *csd_device_paths = NIL;

static bool
csd_is_path(const Path *path)
{
	return path != NULL && IsA(path, CustomPath) &&
		((const CustomPath *) path)->methods == &csd_path_methods;
}

static bool
csd_is_backed_path(const Path *path)
{
	if (csd_is_path(path))
		return true;
	if (path != NULL && IsA(path, ProjectionPath))
		return csd_is_backed_path(((const ProjectionPath *) path)->subpath);
	if (path != NULL && IsA(path, SubqueryScanPath))
		return csd_is_backed_path(((const SubqueryScanPath *) path)->subpath);
	return false;
}

static double
csd_path_device_cost(const Path *path)
{
	if (csd_is_path(path))
	{
		const CustomPath *custom = (const CustomPath *) path;

		return floatVal(list_nth(custom->custom_private, 2));
	}
	if (path != NULL && IsA(path, ProjectionPath))
	{
		const ProjectionPath *project = (const ProjectionPath *) path;

		return csd_path_device_cost(project->subpath) +
			Max(1.0, path->rows) * csd_tuple_cost;
	}
	if (path != NULL && IsA(path, SubqueryScanPath))
	{
		const SubqueryScanPath *subquery = (const SubqueryScanPath *) path;

		return csd_path_device_cost(subquery->subpath) +
			Max(1.0, path->rows) * csd_tuple_cost;
	}
	pg_unreachable();
}

static double
csd_path_cache_fraction(const Path *path)
{
	if (csd_is_path(path))
		return floatVal(list_nth(((const CustomPath *) path)->custom_private, 3));
	if (path != NULL && IsA(path, ProjectionPath))
		return csd_path_cache_fraction(((const ProjectionPath *) path)->subpath);
	if (path != NULL && IsA(path, SubqueryScanPath))
		return csd_path_cache_fraction(((const SubqueryScanPath *) path)->subpath);
	return 0.0;
}

static int
csd_metadata_scan_count(List *metadata)
{
	List *children = (List *) list_nth(metadata, 4);
	ListCell *cell;
	int count = strcmp(strVal(list_nth(metadata, 0)), "Scan") == 0 ? 1 : 0;

	foreach(cell, children)
		count += csd_metadata_scan_count((List *) lfirst(cell));
	return count;
}

static bool
csd_metadata_has_selectivity_risk(List *metadata)
{
	const char *name = strVal(list_nth(metadata, 0));
	List *children = (List *) list_nth(metadata, 4);
	ListCell *cell;

	if (strcmp(name, "Aggregate") == 0 || strcmp(name, "Distinct") == 0 ||
		strcmp(name, "Limit") == 0)
		return false;
	if (strstr(name, "Semi Join") != NULL ||
		strstr(name, "Anti Join") != NULL)
		return true;
	foreach(cell, children)
		if (csd_metadata_has_selectivity_risk((List *) lfirst(cell)))
			return true;
	return false;
}

static int
csd_path_scan_count(const Path *path)
{
	if (csd_is_path(path))
		return csd_metadata_scan_count(
			((const CustomPath *) path)->custom_private);
	if (path != NULL && IsA(path, ProjectionPath))
		return csd_path_scan_count(((const ProjectionPath *) path)->subpath);
	if (path != NULL && IsA(path, SubqueryScanPath))
		return csd_path_scan_count(((const SubqueryScanPath *) path)->subpath);
	return 0;
}

static bool
csd_path_has_selectivity_risk(const Path *path)
{
	if (csd_is_path(path))
		return csd_metadata_has_selectivity_risk(
			((const CustomPath *) path)->custom_private);
	if (path != NULL && IsA(path, ProjectionPath))
		return csd_path_has_selectivity_risk(
			((const ProjectionPath *) path)->subpath);
	if (path != NULL && IsA(path, SubqueryScanPath))
		return csd_path_has_selectivity_risk(
			((const SubqueryScanPath *) path)->subpath);
	return false;
}

static double
csd_combined_cache_fraction(const Path *first, const Path *second)
{
	double first_bytes = Max(1.0, first->rows) * Max(1, first->pathtarget->width);
	double second_bytes = Max(1.0, second->rows) * Max(1, second->pathtarget->width);

	return (csd_path_cache_fraction(first) * first_bytes +
			csd_path_cache_fraction(second) * second_bytes) /
		(first_bytes + second_bytes);
}

static bool
csd_segment_cache_residency(const char *path, uint64 bytes,
							uint64 *resident_pages, uint64 *total_pages)
{
	long page_size = sysconf(_SC_PAGESIZE);
	uint64 pages;
	unsigned char *residency;
	void *mapping;
	int fd;
	uint64 index;

	if (bytes == 0)
		return true;
	if (page_size <= 0)
		page_size = 4096;
	pages = (bytes + (uint64) page_size - 1) / (uint64) page_size;
	if (pages > MaxAllocSize)
		return false;
	fd = open(path, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
		return false;
	mapping = mmap(NULL, (size_t) bytes, PROT_NONE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED)
	{
		close(fd);
		return false;
	}
	residency = palloc((Size) pages);
	if (mincore(mapping, (size_t) bytes, residency) != 0)
	{
		pfree(residency);
		munmap(mapping, (size_t) bytes);
		close(fd);
		return false;
	}
	for (index = 0; index < pages; ++index)
		if ((residency[index] & 1) != 0)
			++*resident_pages;
	*total_pages += pages;
	pfree(residency);
	munmap(mapping, (size_t) bytes);
	close(fd);
	return true;
}

static double
csd_relation_cache_fraction(Oid relation_oid)
{
	Relation relation;
	char *relative;
	char *base;
	uint64 relation_bytes;
	uint64 logical_base = 0;
	uint64 resident_pages = 0;
	uint64 total_pages = 0;
	int segment = 0;
	bool complete = true;

	if (csd_host_cache_fraction >= 0.0)
		return csd_host_cache_fraction;
	if (csd_exe_path != CSD_EXE_AUTO)
		return 0.0;
	relation = table_open(relation_oid, NoLock);
	relative = relpathbackend(relation->rd_node, relation->rd_backend,
							  MAIN_FORKNUM);
	base = psprintf("%s/%s", DataDir, relative);
	relation_bytes = (uint64) RelationGetNumberOfBlocks(relation) * BLCKSZ;
	while (logical_base < relation_bytes)
	{
		char *path = segment == 0 ? pstrdup(base) :
			psprintf("%s.%d", base, segment);
		uint64 segment_bytes = Min(relation_bytes - logical_base,
			(uint64) RELSEG_SIZE * BLCKSZ);

		if (!csd_segment_cache_residency(path, segment_bytes,
				&resident_pages, &total_pages))
			complete = false;
		pfree(path);
		if (!complete)
			break;
		logical_base += segment_bytes;
		++segment;
	}
	pfree(base);
	pfree(relative);
	table_close(relation, NoLock);
	if (!complete || total_pages == 0)
		return 0.0;
	return (double) resident_pages / (double) total_pages;
}

static int
csd_path_total_cost_compare(const void *first, const void *second)
{
	const Path *first_path = (const Path *) lfirst(*(ListCell **) first);
	const Path *second_path = (const Path *) lfirst(*(ListCell **) second);

	if (first_path->total_cost < second_path->total_cost)
		return -1;
	if (first_path->total_cost > second_path->total_cost)
		return 1;
	if (first_path->startup_cost < second_path->startup_cost)
		return -1;
	if (first_path->startup_cost > second_path->startup_cost)
		return 1;
	return 0;
}

static void
csd_adjust_host_cache_costs(RelOptInfo *rel, double cache_fraction)
{
	double saving;
	ListCell *cell;

	if (csd_exe_path != CSD_EXE_AUTO || cache_fraction <= 0.0)
		return;
	saving = rel->pages * cache_fraction *
		Max(0.0, seq_page_cost - csd_host_cached_page_cost);
	foreach(cell, rel->pathlist)
	{
		Path *path = (Path *) lfirst(cell);

		if (path->pathtype == T_SeqScan)
			path->total_cost = Max(path->startup_cost,
				path->total_cost - saving);
	}
	foreach(cell, rel->partial_pathlist)
	{
		Path *path = (Path *) lfirst(cell);

		if (path->pathtype == T_SeqScan)
			path->total_cost = Max(path->startup_cost,
				path->total_cost - saving);
	}
	rel->pathlist = list_qsort(rel->pathlist, csd_path_total_cost_compare);
	rel->partial_pathlist = list_qsort(rel->partial_pathlist,
		csd_path_total_cost_compare);
}

static uint64
csd_operator_memory_capacity(int scan_count)
{
	uint64 capacity_mb = (uint64) Max(csd_fdm_capacity_mb, 1);
	uint64 reserved_mb = (uint64) Max(csd_output_mb, 1) +
		(uint64) Max(csd_spill_stage_mb, 1) +
		(uint64) Max(csd_fifo_mb, 1) * 2 +
		(uint64) Max(csd_chunk_mb, 1) * Max(scan_count, 1);
	uint64 available_mb = capacity_mb > reserved_mb ?
		capacity_mb - reserved_mb : 1;

	if (csd_device_work_mem_mb > 0)
		available_mb = Min(available_mb, (uint64) csd_device_work_mem_mb);
	return available_mb << 20;
}

static double
csd_spill_cost(double io_bytes, double workset_bytes, uint64 memory_bytes,
			   int passes)
{
	if (workset_bytes <= (double) memory_bytes || io_bytes <= 0.0)
		return 0.0;
	return ceil(io_bytes / BLCKSZ) * Max(passes, 1) * 2.0 *
		csd_spill_page_cost;
}

static double
csd_boundary_cost(double device_cost, double result_bytes)
{
	double cost = csd_setup_cost + device_cost +
		result_bytes * csd_result_byte_cost;
	double output_capacity = (double) Max(csd_output_mb, 1) * 1024.0 * 1024.0;

	if (csd_exe_path == CSD_EXE_AUTO && result_bytes > output_capacity)
		cost += disable_cost;
	return cost;
}

void
csd_cost_reset_paths(void)
{
	csd_device_paths = NIL;
}

static void
csd_register_device_path(PlannerInfo *root, RelOptInfo *rel, Path *path)
{
	ListCell *cell;

	foreach(cell, csd_device_paths)
	{
		CsdDevicePathState *state = (CsdDevicePathState *) lfirst(cell);

		if (state->root == root && state->rel == rel)
		{
			if (csd_path_device_cost(path) <
				csd_path_device_cost(state->path))
				state->path = path;
			return;
		}
	}
	{
		CsdDevicePathState *state = palloc(sizeof(*state));

		state->root = root;
		state->rel = rel;
		state->path = path;
		csd_device_paths = lappend(csd_device_paths, state);
	}
}

static Path *
csd_registered_device_path(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell *cell;

	foreach(cell, csd_device_paths)
	{
		CsdDevicePathState *state = (CsdDevicePathState *) lfirst(cell);

		if (state->root == root && state->rel == rel)
			return state->path;
	}
	return NULL;
}

static List *
csd_path_metadata(const Path *path)
{
	if (csd_is_path(path))
		return copyObject(((const CustomPath *) path)->custom_private);
	if (path != NULL && IsA(path, ProjectionPath))
	{
		const ProjectionPath *project = (const ProjectionPath *) path;
		List *metadata;
		List *children = list_make1(csd_path_metadata(project->subpath));

		metadata = list_make4(makeString(pstrdup("Project")), makeInteger(0),
							  makeFloat(psprintf("%.17g", csd_path_device_cost(path))),
							  makeFloat(psprintf("%.17g",
								csd_path_cache_fraction(project->subpath))));
		metadata = lappend(metadata, children);
		metadata = lappend(metadata, makeFloat(psprintf("%.17g", Max(1.0, path->rows))));
		metadata = lappend(metadata, makeFloat(psprintf("%.17g",
			Max(1.0, path->rows) * Max(1, path->pathtarget->width))));
		return lappend(metadata, copyObject(path->pathtarget->exprs));
	}
	if (path != NULL && IsA(path, SubqueryScanPath))
	{
		const SubqueryScanPath *subquery = (const SubqueryScanPath *) path;
		List *metadata;
		List *children = list_make1(csd_path_metadata(subquery->subpath));

		metadata = list_make4(makeString(pstrdup("Subquery")), makeInteger(0),
							  makeFloat(psprintf("%.17g", csd_path_device_cost(path))),
							  makeFloat(psprintf("%.17g",
								csd_path_cache_fraction(subquery->subpath))));
		metadata = lappend(metadata, children);
		metadata = lappend(metadata, makeFloat(psprintf("%.17g", Max(1.0, path->rows))));
		metadata = lappend(metadata, makeFloat(psprintf("%.17g",
			Max(1.0, path->rows) * Max(1, path->pathtarget->width))));
		return lappend(metadata, list_make2(makeInteger(path->parent->relid),
			copyObject(path->pathtarget->exprs)));
	}
	pg_unreachable();
}

static Path *
csd_cheapest_path(PlannerInfo *root, RelOptInfo *rel)
{
	Path *best = csd_registered_device_path(root, rel);
	double best_cost = best != NULL ? csd_path_device_cost(best) : 0;
	ListCell *cell;

	foreach(cell, rel->pathlist)
	{
		Path *path = (Path *) lfirst(cell);

		if (csd_is_backed_path(path) &&
			(best == NULL || csd_path_device_cost(path) < best_cost))
		{
			best = path;
			best_cost = csd_path_device_cost(path);
		}
	}
	return best;
}

static double
csd_output_rows(RelOptInfo *rel)
{
	Path *best = NULL;
	ListCell *cell;

	foreach(cell, rel->pathlist)
	{
		Path *path = (Path *) lfirst(cell);

		if (best == NULL || path->total_cost < best->total_cost)
			best = path;
	}
	return best != NULL ? Max(1.0, best->rows) : Max(1.0, rel->rows);
}

static void
csd_keep_only_paths(RelOptInfo *rel)
{
	List *paths = NIL;
	ListCell *cell;

	foreach(cell, rel->pathlist)
	{
		Path *path = (Path *) lfirst(cell);

		if (csd_is_path(path))
			paths = lappend(paths, path);
	}
	if (paths != NIL)
		rel->pathlist = paths;
}

static CustomPath *
csd_make_path(RelOptInfo *rel, const char *name, int detail, List *children,
			  Cost boundary_cost, double device_cost, double cache_fraction,
			  double estimated_rows, double result_bytes, List *pathkeys,
			  Node *payload)
{
	CustomPath *path = makeNode(CustomPath);
	Cost planner_startup = boundary_cost;
	Cost planner_total = boundary_cost;
	List *child_metadata = NIL;
	ListCell *cell;

	if (csd_exe_path == CSD_EXE_CSD)
	{
		planner_startup = 0;
		planner_total = 0.000001 * (1.0 + list_length(children));
	}
	path->path.pathtype = T_CustomScan;
	path->path.parent = rel;
	path->path.pathtarget = rel->reltarget;
	path->path.param_info = NULL;
	path->path.parallel_aware = false;
	path->path.parallel_safe = false;
	path->path.parallel_workers = 0;
	path->path.rows = Max(1.0, estimated_rows);
	path->path.startup_cost = planner_startup;
	path->path.total_cost = planner_total;
	path->path.pathkeys = pathkeys;
	path->flags = 0;
	/*
	 * The selected CSD path is submitted as one command.  Keep the complete
	 * operator tree as copyable metadata rather than PostgreSQL child Plans;
	 * otherwise setrefs tries to execute each intermediate CSD tuple stream as
	 * a host-side subplan and demands host target-list Vars for internal keys.
	 */
	foreach(cell, children)
	{
		Path *child = (Path *) lfirst(cell);

		Assert(csd_is_backed_path(child));
		child_metadata = lappend(child_metadata, csd_path_metadata(child));
	}
	path->custom_paths = NIL;
	path->custom_private = list_make4(makeString(pstrdup(name)),
										makeInteger(detail),
										makeFloat(psprintf("%.17g", device_cost)),
										makeFloat(psprintf("%.17g", cache_fraction)));
	path->custom_private = lappend(path->custom_private, child_metadata);
	path->custom_private = lappend(path->custom_private,
		makeFloat(psprintf("%.17g", Max(1.0, estimated_rows))));
	path->custom_private = lappend(path->custom_private,
		makeFloat(psprintf("%.17g", Max(1.0, result_bytes))));
	path->custom_private = lappend(path->custom_private, payload);
	path->methods = &csd_path_methods;
	return path;
}

static bool
csd_metadata_has_subplan(Node *node)
{
	ListCell *cell;

	if (node == NULL)
		return false;
	if (IsA(node, List))
	{
		foreach(cell, (List *) node)
			if (csd_metadata_has_subplan((Node *) lfirst(cell)))
				return true;
		return false;
	}
	if (IsA(node, Integer) || IsA(node, Float) || IsA(node, String) ||
		IsA(node, BitString) || IsA(node, Null))
		return false;
	return contain_subplans(node);
}

static void
csd_publish_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *device_path)
{
	CustomPath *boundary_path = palloc(sizeof(*boundary_path));
	bool has_subplan;
	bool has_selectivity_risk;

	csd_register_device_path(root, rel, &device_path->path);
	/*
	 * The boundary has the same immutable planner metadata as the registered
	 * device state; only its planner-visible cost may be penalized below.
	 */
	memcpy(boundary_path, device_path, sizeof(*boundary_path));
	/*
	 * SubPlan clauses are rewritten only when the complete query remains on
	 * the device.  At a CSD-to-HOST boundary the executor cannot silently
	 * drop such a clause: doing so would return a superset of the rows.  Keep
	 * the device state for a later CSD parent, but make this terminal boundary
	 * unavailable to AUTO.  The same rule applies below a SEMI/ANTI join whose
	 * row estimate has been conservatively raised: PostgreSQL stores one row
	 * estimate per join relation, so a HOST parent would otherwise discard the
	 * path-specific estimate and recreate a catastrophic nested-loop cut.
	 * Aggregate, DISTINCT, or LIMIT clears that cardinality risk.  Walking each
	 * metadata item separately avoids passing String/Float bookkeeping nodes to
	 * expression_tree_walker.
	 */
	has_subplan = csd_metadata_has_subplan(
		(Node *) boundary_path->custom_private);
	has_selectivity_risk = csd_path_has_selectivity_risk(
		&boundary_path->path);
	if (csd_exe_path == CSD_EXE_AUTO &&
		(has_subplan || has_selectivity_risk))
	{
		boundary_path->path.startup_cost += disable_cost;
		boundary_path->path.total_cost += disable_cost;
	}
	add_path(rel, &boundary_path->path);
}

static void
csd_reject_if_forced(const char *reason)
{
	if (csd_exe_path == CSD_EXE_CSD)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("query cannot be fully lowered to the CSD engine"),
				 errdetail("%s", reason != NULL ? reason : "unsupported planner node"),
				 errhint("Use SET csd.exe_path = 'host' or rewrite the expression.")));
}

void
csd_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					 RangeTblEntry *rte)
{
	const char *reason = NULL;
	double rows;
	double bytes;
	double cache_fraction;
	double device_cost;
	double boundary_cost;
	CustomPath *path;
	List *payload;

	if (csd_exe_path == CSD_EXE_HOST || rte->rtekind != RTE_RELATION || rte->inh)
		return;
	cache_fraction = csd_relation_cache_fraction(rte->relid);
	csd_adjust_host_cache_costs(rel, cache_fraction);
	if (!csd_expression_list_supported(rel->baserestrictinfo, &reason) ||
		!csd_expression_list_supported(rel->reltarget->exprs, &reason))
	{
		csd_reject_if_forced(reason);
		return;
	}
	rows = Max(1.0, rel->rows);
	bytes = rows * Max(1, rel->reltarget->width);
	device_cost = rel->pages * csd_flash_page_cost +
		Max(1.0, rel->tuples) * csd_tuple_cost;
	boundary_cost = csd_boundary_cost(device_cost, bytes);
	payload = list_make3(makeString(psprintf("%u", rte->relid)),
		copyObject(rel->reltarget->exprs),
		extract_actual_clauses(rel->baserestrictinfo, false));
	path = csd_make_path(rel, "Scan", (int) rti, NIL,
					 boundary_cost, device_cost, cache_fraction,
					 rows, bytes, NIL, (Node *) payload);
	csd_publish_path(root, rel, path);
	if (csd_exe_path == CSD_EXE_CSD)
		csd_keep_only_paths(rel);
}

void
csd_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
					  RelOptInfo *outerrel, RelOptInfo *innerrel,
					  JoinType jointype, JoinPathExtraData *extra)
{
	Path *outer;
	Path *inner;
	const char *reason = NULL;
	const char *name;
	double rows;
	double bytes;
	double input_bytes;
	double cache_fraction;
	double device_cost;
	double boundary_cost;
	double spill_cost;
	uint64 memory_bytes;
	int scan_count;
	CustomPath *path;
	List *payload;

	if (csd_exe_path == CSD_EXE_HOST)
		return;
	if (jointype == JOIN_UNIQUE_OUTER || jointype == JOIN_UNIQUE_INNER)
		return;
	if (jointype != JOIN_INNER && jointype != JOIN_LEFT && jointype != JOIN_RIGHT &&
		jointype != JOIN_SEMI && jointype != JOIN_ANTI)
	{
		csd_reject_if_forced("FULL joins have no current FPGA lowering");
		return;
	}
	if (!csd_expression_list_supported(extra->restrictlist, &reason) ||
		!csd_expression_list_supported(joinrel->reltarget->exprs, &reason))
	{
		csd_reject_if_forced(reason);
		return;
	}
	outer = csd_cheapest_path(root, outerrel);
	inner = csd_cheapest_path(root, innerrel);
	if (outer == NULL || inner == NULL)
		return;
	switch (jointype)
	{
		case JOIN_LEFT: name = "Left Hash Join"; break;
		case JOIN_RIGHT: name = "Right Hash Join"; break;
		case JOIN_SEMI: name = "Hash Semi Join"; break;
		case JOIN_ANTI: name = "Hash Anti Join"; break;
		default: name = "Hash Join"; break;
	}
	rows = Max(1.0, joinrel->rows);
	/*
	 * PostgreSQL can drive SEMI/ANTI selectivity down to one row for deeply
	 * correlated EXISTS trees.  That is unsafe at a materialization boundary
	 * when many rows actually reach later host-side joins.  Use the preserved
	 * side for outer/semi/anti joins and for inner-unique joins as a conservative
	 * transfer estimate.  A later CSD aggregate can still reduce the registered
	 * device state before it crosses the boundary.
	 */
	if (jointype == JOIN_LEFT || jointype == JOIN_SEMI ||
		jointype == JOIN_ANTI || extra->inner_unique)
		rows = Max(rows, Max(1.0, outer->rows));
	else if (jointype == JOIN_RIGHT)
		rows = Max(rows, Max(1.0, inner->rows));
	if (csd_path_has_selectivity_risk(outer))
		rows = Max(rows, Max(1.0, outer->rows));
	if (csd_path_has_selectivity_risk(inner))
		rows = Max(rows, Max(1.0, inner->rows));
	bytes = rows * Max(1, joinrel->reltarget->width);
	input_bytes = Max(1.0, outer->rows) * Max(1, outer->pathtarget->width) +
		Max(1.0, inner->rows) * Max(1, inner->pathtarget->width);
	scan_count = csd_path_scan_count(outer) + csd_path_scan_count(inner);
	memory_bytes = csd_operator_memory_capacity(scan_count);
	spill_cost = csd_spill_cost(input_bytes,
		Max(1.0, inner->rows) * Max(1, inner->pathtarget->width),
		memory_bytes, 1);
	device_cost = csd_path_device_cost(outer) + csd_path_device_cost(inner) +
		(outer->rows + inner->rows) * csd_join_tuple_cost + spill_cost;
	boundary_cost = csd_boundary_cost(device_cost, bytes);
	cache_fraction = csd_combined_cache_fraction(outer, inner);
	payload = list_make3(makeInteger((int) jointype),
		extract_actual_clauses(extra->restrictlist, false),
		copyObject(joinrel->reltarget->exprs));
	path = csd_make_path(joinrel, name, (int) jointype,
					 list_make2(outer, inner), boundary_cost, device_cost,
					 cache_fraction, rows, bytes, NIL, (Node *) payload);
	csd_publish_path(root, joinrel, path);
	if (csd_exe_path == CSD_EXE_CSD)
		csd_keep_only_paths(joinrel);
}

void
csd_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					   RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	Path *input;
	FinalPathExtraData *final_extra = NULL;
	const char *reason = NULL;
	const char *name;
	double rows;
	double bytes;
	double input_bytes;
	double stage_cost = 0;
	double spill_cost = 0;
	double device_cost;
	double boundary_cost;
	double cache_fraction;
	uint64 memory_bytes;
	int scan_count;
	int result_width;
	CustomPath *path;
	List *payload;

	if (csd_exe_path == CSD_EXE_HOST || input_rel == NULL || output_rel == NULL)
		return;
	if (stage == UPPERREL_PARTIAL_GROUP_AGG)
		return;
	if (stage == UPPERREL_SETOP || stage == UPPERREL_WINDOW)
	{
		csd_reject_if_forced("set operations and window functions are not TPC-H CSD operators");
		return;
	}
	if (!csd_expression_list_supported(output_rel->reltarget->exprs, &reason))
	{
		csd_reject_if_forced(reason);
		return;
	}
	input = csd_cheapest_path(root, input_rel);
	if (input == NULL)
		return;
	rows = stage == UPPERREL_FINAL ? Max(1.0, input->rows) :
		csd_output_rows(output_rel);
	if (stage == UPPERREL_FINAL)
	{
		final_extra = (FinalPathExtraData *) extra;
		if (final_extra != NULL && final_extra->limit_needed &&
			final_extra->count_est >= 0)
			rows = Min(rows, (double) final_extra->count_est);
	}
	result_width = Max(1, root->upper_targets[stage] != NULL ?
		root->upper_targets[stage]->width : output_rel->reltarget->width);
	bytes = rows * result_width;
	input_bytes = Max(1.0, input->rows) * Max(1, input->pathtarget->width);
	scan_count = csd_path_scan_count(input);
	memory_bytes = csd_operator_memory_capacity(scan_count);
	name = csd_upper_stage_name(stage);
	if (stage == UPPERREL_FINAL && final_extra != NULL &&
		final_extra->limit_needed)
		name = "Limit";
	switch (stage)
	{
		case UPPERREL_GROUP_AGG:
		case UPPERREL_DISTINCT:
			stage_cost = input->rows * csd_aggregate_tuple_cost;
			spill_cost = csd_spill_cost(input_bytes,
				rows * (result_width + 32.0), memory_bytes, 1);
			break;
		case UPPERREL_ORDERED:
			stage_cost = input->rows * Max(1.0, log(Max(2.0, input->rows)) / log(2.0)) *
				csd_sort_compare_cost;
			if (input_bytes > (double) memory_bytes)
			{
				double ratio = input_bytes / (double) memory_bytes;
				int passes = 1 + (int) ceil(log(Max(1.0, ratio)) / log(2.0));

				spill_cost = csd_spill_cost(input_bytes, input_bytes,
					memory_bytes, passes);
			}
			break;
		case UPPERREL_FINAL:
			stage_cost = rows * csd_tuple_cost;
			break;
		default:
			return;
	}
	device_cost = csd_path_device_cost(input) + stage_cost + spill_cost;
	boundary_cost = csd_boundary_cost(device_cost, bytes);
	cache_fraction = csd_path_cache_fraction(input);
	payload = list_make4(makeInteger((int) stage),
		copyObject(root->upper_targets[stage] != NULL ?
			root->upper_targets[stage]->exprs : output_rel->reltarget->exprs),
		copyObject(root->parse->groupClause),
		copyObject(root->parse->sortClause));
	payload = lappend(payload, copyObject(root->parse->havingQual));
	/* Sort/group clauses identify entries in the parse target list. */
	payload = lappend(payload, copyObject(root->parse->targetList));
	payload = lappend(payload, copyObject(root->parse->distinctClause));
	payload = lappend(payload, copyObject(root->parse->limitCount));
	payload = lappend(payload, copyObject(root->parse->limitOffset));
	payload = lappend(payload, makeInteger(final_extra != NULL ?
		final_extra->count_est : -1));
	payload = lappend(payload, makeInteger(final_extra != NULL ?
		final_extra->offset_est : 0));
	path = csd_make_path(output_rel, name, (int) stage, list_make1(input),
						boundary_cost, device_cost, cache_fraction,
						rows, bytes,
						stage == UPPERREL_ORDERED ? root->query_pathkeys : input->pathkeys,
						(Node *) payload);
	if (root->upper_targets[stage] != NULL)
		path->path.pathtarget = root->upper_targets[stage];
	csd_publish_path(root, output_rel, path);
	if (csd_exe_path == CSD_EXE_CSD)
		csd_keep_only_paths(output_rel);
}
