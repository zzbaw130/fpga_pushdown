#include "postgres.h"

#include <fcntl.h>
#include <float.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/value.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "storage/bufmgr.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "csd_plan.h"
#include "csd_executor.h"
#include "csd_pg.h"

typedef struct CsdDevicePathState
{
	PlannerInfo *root;
	RelOptInfo *rel;
	List *paths;
} CsdDevicePathState;

static List *csd_device_paths = NIL;

static bool csd_metadata_has_subplan(Node *node);

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

static double
csd_saturated_multiply(double first, double second)
{
	if (first <= 0.0 || second <= 0.0)
		return 0.0;
	if (!isfinite(first) || !isfinite(second) || first > DBL_MAX / second)
		return DBL_MAX;
	return first * second;
}

static double
csd_expression_payload_bound(Node *expression)
{
	Oid type = exprType(expression);
	int32 typmod = exprTypmod(expression);

	switch (type)
	{
		case CHAROID:
			return 1.0;
		case BPCHAROID:
		case VARCHAROID:
			if (typmod >= VARHDRSZ)
				return (double) (typmod - VARHDRSZ);
			return (double) MaxHeapTupleSize;
		case TEXTOID:
			/* CSD lowering rejects external/compressed TOAST values. */
			return (double) MaxHeapTupleSize;
		default:
			return 0.0;
	}
}

static double
csd_target_row_bytes(const PathTarget *target, bool safe)
{
	double bytes = (double) list_length(target->exprs) *
		sizeof(struct CsdSqlResultCell);
	ListCell *cell;

	if (!safe)
		return bytes + Max(1, target->width);
	foreach(cell, target->exprs)
	{
		double payload = csd_expression_payload_bound((Node *) lfirst(cell));

		if (bytes > DBL_MAX - payload)
			return DBL_MAX;
		bytes += payload;
	}
	return Max(bytes, 1.0);
}

static double
csd_result_bytes(double rows, const PathTarget *target, bool safe)
{
	double body = csd_saturated_multiply(Max(rows, 1.0),
		csd_target_row_bytes(target, safe));

	if (body > DBL_MAX - sizeof(struct CsdSqlResultHeader))
		return DBL_MAX;
	return body + sizeof(struct CsdSqlResultHeader);
}

static double
csd_path_safe_rows(const Path *path)
{
	if (csd_is_path(path))
		return floatVal(list_nth(((const CustomPath *) path)->custom_private,
			CSD_META_SAFE_ROWS));
	if (path != NULL && IsA(path, ProjectionPath))
		return csd_path_safe_rows(((const ProjectionPath *) path)->subpath);
	if (path != NULL && IsA(path, SubqueryScanPath))
		return csd_path_safe_rows(((const SubqueryScanPath *) path)->subpath);
	return DBL_MAX;
}

static double
csd_path_safe_bytes(const Path *path)
{
	if (csd_is_path(path))
		return floatVal(list_nth(((const CustomPath *) path)->custom_private,
			CSD_META_SAFE_BYTES));
	if (path != NULL && IsA(path, ProjectionPath))
		return csd_result_bytes(csd_path_safe_rows(path), path->pathtarget, true);
	if (path != NULL && IsA(path, SubqueryScanPath))
		return csd_result_bytes(csd_path_safe_rows(path), path->pathtarget, true);
	return DBL_MAX;
}

static double
csd_path_expected_bytes(const Path *path)
{
	if (csd_is_path(path))
		return floatVal(list_nth(((const CustomPath *) path)->custom_private,
			CSD_META_EXPECTED_BYTES));
	if (path != NULL && (IsA(path, ProjectionPath) ||
		IsA(path, SubqueryScanPath)))
		return csd_result_bytes(Max(1.0, path->rows), path->pathtarget, false);
	return DBL_MAX;
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
csd_boundary_cost(double device_cost, double result_bytes,
				  double safe_result_bytes)
{
	double cost = csd_setup_cost + device_cost +
		result_bytes * csd_result_byte_cost;
	double output_capacity = (double) Max(csd_output_mb, 1) * 1024.0 * 1024.0;

	if (csd_exe_path == CSD_EXE_AUTO && safe_result_bytes > output_capacity)
		cost += disable_cost;
	return cost;
}

void
csd_cost_reset_paths(void)
{
	csd_device_paths = NIL;
}

static CsdDevicePathState *
csd_device_path_state(PlannerInfo *root, RelOptInfo *rel, bool create)
{
	ListCell *cell;

	foreach(cell, csd_device_paths)
	{
		CsdDevicePathState *state = (CsdDevicePathState *) lfirst(cell);

		if (state->root == root && state->rel == rel)
			return state;
	}
	if (!create)
		return NULL;
	{
		CsdDevicePathState *state = palloc(sizeof(*state));

		state->root = root;
		state->rel = rel;
		state->paths = NIL;
		csd_device_paths = lappend(csd_device_paths, state);
		return state;
	}
}

static bool
csd_register_device_path(PlannerInfo *root, RelOptInfo *rel, Path *path)
{
	CsdDevicePathState *state = csd_device_path_state(root, rel, true);
	List *survivors = NIL;
	ListCell *cell;
	bool new_has_subplan;
	bool new_has_risk;

	Assert(csd_is_path(path));
	new_has_subplan = csd_metadata_has_subplan(
		(Node *) ((CustomPath *) path)->custom_private);
	new_has_risk = csd_path_has_selectivity_risk(path);
	foreach(cell, state->paths)
	{
		Path *existing = (Path *) lfirst(cell);
		const CustomPath *existing_custom = (const CustomPath *) existing;
		const CustomPath *new_custom = (const CustomPath *) path;
		bool same_properties;
		bool existing_dominates;
		bool new_dominates;

		if (equal(existing_custom->custom_private,
			new_custom->custom_private) &&
			equal(existing->pathkeys, path->pathkeys))
			return false;
		same_properties = equal(existing->pathkeys, path->pathkeys) &&
			csd_metadata_has_subplan(
				(Node *) existing_custom->custom_private) == new_has_subplan &&
			csd_path_has_selectivity_risk(existing) == new_has_risk &&
			csd_path_scan_count(existing) == csd_path_scan_count(path) &&
			fabs(existing->rows - path->rows) <=
				1.0e-9 * Max(Max(existing->rows, path->rows), 1.0);
		existing_dominates = same_properties &&
			csd_path_device_cost(existing) <= csd_path_device_cost(path) &&
			csd_path_safe_rows(existing) <= csd_path_safe_rows(path) &&
			csd_path_safe_bytes(existing) <= csd_path_safe_bytes(path);
		if (existing_dominates)
			return false;
		new_dominates = same_properties &&
			csd_path_device_cost(path) <= csd_path_device_cost(existing) &&
			csd_path_safe_rows(path) <= csd_path_safe_rows(existing) &&
			csd_path_safe_bytes(path) <= csd_path_safe_bytes(existing);
		if (!new_dominates)
			survivors = lappend(survivors, existing);
	}
	state->paths = lappend(survivors, path);
	return true;
}

static List *
csd_device_candidate_paths(PlannerInfo *root, RelOptInfo *rel)
{
	CsdDevicePathState *state = csd_device_path_state(root, rel, false);
	List *paths = state != NULL ? list_copy(state->paths) : NIL;
	ListCell *cell;

	/* PostgreSQL may wrap a surviving boundary in Projection/Subquery paths.
	 * Direct CustomPaths are boundary clones of registry entries and would
	 * otherwise duplicate the same device state. */
	foreach(cell, rel->pathlist)
	{
		Path *path = (Path *) lfirst(cell);

		if (!csd_is_path(path) && csd_is_backed_path(path) &&
			!list_member_ptr(paths, path))
			paths = lappend(paths, path);
	}
	return paths;
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
			csd_result_bytes(Max(1.0, path->rows), path->pathtarget,
				false))));
		metadata = lappend(metadata, copyObject(path->pathtarget->exprs));
		metadata = lappend(metadata, makeFloat(psprintf("%.17g",
			csd_path_safe_rows(project->subpath))));
		return lappend(metadata, makeFloat(psprintf("%.17g",
			csd_path_safe_bytes(path))));
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
			csd_result_bytes(Max(1.0, path->rows), path->pathtarget,
				false))));
		metadata = lappend(metadata, list_make2(makeInteger(path->parent->relid),
			copyObject(path->pathtarget->exprs)));
		metadata = lappend(metadata, makeFloat(psprintf("%.17g",
			csd_path_safe_rows(subquery->subpath))));
		return lappend(metadata, makeFloat(psprintf("%.17g",
			csd_path_safe_bytes(path))));
	}
	pg_unreachable();
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
			  double estimated_rows, double result_bytes, double safe_rows,
			  double safe_result_bytes, List *pathkeys, Node *payload)
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
	path->custom_private = lappend(path->custom_private,
		makeFloat(psprintf("%.17g", Max(1.0, safe_rows))));
	path->custom_private = lappend(path->custom_private,
		makeFloat(psprintf("%.17g", Max(1.0, safe_result_bytes))));
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

	if (!csd_register_device_path(root, rel, &device_path->path))
		return;
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
	double safe_rows;
	double safe_bytes;
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
	/* rel->tuples comes from ANALYZE/pg_class.  The physical page bound makes
	 * capacity admission safe even when those statistics are stale. */
	safe_rows = Max(Max(1.0, rel->tuples),
		(double) rel->pages * MaxHeapTuplesPerPage);
	bytes = csd_result_bytes(rows, rel->reltarget, false);
	safe_bytes = csd_result_bytes(safe_rows, rel->reltarget, true);
	device_cost = rel->pages * csd_flash_page_cost +
		Max(1.0, rel->tuples) * csd_tuple_cost;
	boundary_cost = csd_boundary_cost(device_cost, bytes, safe_bytes);
	payload = list_make3(makeString(psprintf("%u", rte->relid)),
		copyObject(rel->reltarget->exprs),
		extract_actual_clauses(rel->baserestrictinfo, false));
	path = csd_make_path(rel, "Scan", (int) rti, NIL,
					 boundary_cost, device_cost, cache_fraction,
					 rows, bytes, safe_rows, safe_bytes, NIL,
					 (Node *) payload);
	csd_publish_path(root, rel, path);
	if (csd_exe_path == CSD_EXE_CSD)
		csd_keep_only_paths(rel);
}

void
csd_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
					  RelOptInfo *outerrel, RelOptInfo *innerrel,
					  JoinType jointype, JoinPathExtraData *extra)
{
	List *outer_paths;
	List *inner_paths;
	ListCell *outer_cell;
	ListCell *inner_cell;
	const char *reason = NULL;
	const char *name;
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
	outer_paths = csd_device_candidate_paths(root, outerrel);
	inner_paths = csd_device_candidate_paths(root, innerrel);
	if (outer_paths == NIL || inner_paths == NIL)
		return;
	switch (jointype)
	{
		case JOIN_LEFT: name = "Left Hash Join"; break;
		case JOIN_RIGHT: name = "Right Hash Join"; break;
		case JOIN_SEMI: name = "Hash Semi Join"; break;
		case JOIN_ANTI: name = "Hash Anti Join"; break;
		default: name = "Hash Join"; break;
	}
	payload = list_make3(makeInteger((int) jointype),
		extract_actual_clauses(extra->restrictlist, false),
		copyObject(joinrel->reltarget->exprs));
	/* Enumerate every distinct device-resident child pair.  PostgreSQL still
	 * prices and prunes materialized HOST boundaries, while this private state
	 * set preserves CSD alternatives for larger join trees. */
	foreach(outer_cell, outer_paths)
	{
		Path *outer = (Path *) lfirst(outer_cell);

		foreach(inner_cell, inner_paths)
		{
			Path *inner = (Path *) lfirst(inner_cell);
			double rows = Max(1.0, joinrel->rows);
			double safe_outer_rows = csd_path_safe_rows(outer);
			double safe_inner_rows = csd_path_safe_rows(inner);
			double safe_rows;
			double bytes;
			double safe_bytes;
			double input_bytes;
			double expected_build_bytes;
			double cache_fraction;
			double device_cost;
			double boundary_cost;
			double spill_cost;
			uint64 memory_bytes;
			int scan_count;
			CustomPath *path;

			/* A unique inner, SEMI, or ANTI join emits at most one row per
			 * outer row.  Other joins use the structural Cartesian upper bound. */
			if (jointype == JOIN_SEMI || jointype == JOIN_ANTI ||
				(jointype != JOIN_RIGHT && extra->inner_unique))
				safe_rows = safe_outer_rows;
			else
				safe_rows = csd_saturated_multiply(safe_outer_rows,
					safe_inner_rows);
			/* Keep the expected PostgreSQL estimate for normal costing.  The
			 * preserved side is used only where a known SEMI/ANTI estimation
			 * hazard would otherwise create a tiny HOST boundary. */
			if (jointype == JOIN_LEFT || jointype == JOIN_SEMI ||
				jointype == JOIN_ANTI ||
				(jointype != JOIN_RIGHT && extra->inner_unique))
				rows = Max(rows, Max(1.0, outer->rows));
			else if (jointype == JOIN_RIGHT)
				rows = Max(rows, Max(1.0, inner->rows));
			if (csd_path_has_selectivity_risk(outer))
				rows = Max(rows, Max(1.0, outer->rows));
			if (csd_path_has_selectivity_risk(inner))
				rows = Max(rows, Max(1.0, inner->rows));
			bytes = csd_result_bytes(rows, joinrel->reltarget, false);
			safe_bytes = csd_result_bytes(safe_rows,
				joinrel->reltarget, true);
			input_bytes = csd_path_expected_bytes(outer) +
				csd_path_expected_bytes(inner);
			expected_build_bytes = csd_path_expected_bytes(inner);
			scan_count = csd_path_scan_count(outer) +
				csd_path_scan_count(inner);
			memory_bytes = csd_operator_memory_capacity(scan_count);
			spill_cost = csd_spill_cost(input_bytes, expected_build_bytes,
				memory_bytes, 1);
			device_cost = csd_path_device_cost(outer) +
				csd_path_device_cost(inner) +
				(outer->rows + inner->rows) * csd_join_tuple_cost +
				spill_cost;
			boundary_cost = csd_boundary_cost(device_cost, bytes,
				safe_bytes);
			cache_fraction = csd_combined_cache_fraction(outer, inner);
			path = csd_make_path(joinrel, name, (int) jointype,
				list_make2(outer, inner), boundary_cost, device_cost,
				cache_fraction, rows, bytes, safe_rows, safe_bytes, NIL,
				(Node *) payload);
			csd_publish_path(root, joinrel, path);
		}
	}
	if (csd_exe_path == CSD_EXE_CSD)
		csd_keep_only_paths(joinrel);
}

void
csd_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
					   RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	List *input_paths;
	ListCell *input_cell;
	FinalPathExtraData *final_extra = NULL;
	const char *reason = NULL;
	const char *name;
	double output_rows;
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
	input_paths = csd_device_candidate_paths(root, input_rel);
	if (input_paths == NIL)
		return;
	if (stage == UPPERREL_FINAL)
		final_extra = (FinalPathExtraData *) extra;
	output_rows = stage == UPPERREL_FINAL ? 0.0 :
		csd_output_rows(output_rel);
	name = csd_upper_stage_name(stage);
	if (stage == UPPERREL_FINAL && final_extra != NULL &&
		final_extra->limit_needed)
		name = "Limit";
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
	foreach(input_cell, input_paths)
	{
		Path *input = (Path *) lfirst(input_cell);
		PathTarget *result_target = root->upper_targets[stage] != NULL ?
			root->upper_targets[stage] : output_rel->reltarget;
		double rows = stage == UPPERREL_FINAL ? Max(1.0, input->rows) :
			output_rows;
		double safe_input_rows = csd_path_safe_rows(input);
		double safe_rows;
		double bytes;
		double safe_bytes;
		double input_bytes = csd_path_expected_bytes(input);
		double stage_cost = 0.0;
		double spill_cost = 0.0;
		double device_cost;
		double boundary_cost;
		double cache_fraction;
		uint64 memory_bytes;
		int scan_count;
		CustomPath *path;

		if (stage == UPPERREL_FINAL && final_extra != NULL &&
			final_extra->limit_needed && final_extra->count_est >= 0)
			rows = Min(rows, (double) final_extra->count_est);
		switch (stage)
		{
			case UPPERREL_GROUP_AGG:
				if (root->parse->groupClause == NIL)
					safe_rows = 1.0;
				else
					safe_rows = Min(safe_input_rows,
						Max(rows, rows * csd_stats_safety_factor));
				stage_cost = input->rows * csd_aggregate_tuple_cost;
				break;
			case UPPERREL_DISTINCT:
				safe_rows = Min(safe_input_rows,
					Max(rows, rows * csd_stats_safety_factor));
				stage_cost = input->rows * csd_aggregate_tuple_cost;
				break;
			case UPPERREL_ORDERED:
				safe_rows = safe_input_rows;
				stage_cost = input->rows * Max(1.0,
					log(Max(2.0, input->rows)) / log(2.0)) *
					csd_sort_compare_cost;
				break;
			case UPPERREL_FINAL:
				safe_rows = safe_input_rows;
				if (final_extra != NULL && final_extra->limit_needed &&
					final_extra->count_est >= 0)
					safe_rows = Min(safe_rows,
						(double) final_extra->count_est);
				stage_cost = rows * csd_tuple_cost;
				break;
			default:
				return;
		}
		bytes = csd_result_bytes(rows, result_target, false);
		safe_bytes = csd_result_bytes(safe_rows, result_target, true);
		scan_count = csd_path_scan_count(input);
		memory_bytes = csd_operator_memory_capacity(scan_count);
		if (stage == UPPERREL_GROUP_AGG || stage == UPPERREL_DISTINCT)
			spill_cost = csd_spill_cost(input_bytes, bytes,
				memory_bytes, 1);
		else if (stage == UPPERREL_ORDERED &&
			input_bytes > (double) memory_bytes)
		{
			double ratio = input_bytes / (double) memory_bytes;
			int passes = 1 + (int) ceil(log(Max(1.0, ratio)) /
				log(2.0));

			spill_cost = csd_spill_cost(input_bytes, input_bytes,
				memory_bytes, passes);
		}
		device_cost = csd_path_device_cost(input) + stage_cost + spill_cost;
		boundary_cost = csd_boundary_cost(device_cost, bytes, safe_bytes);
		cache_fraction = csd_path_cache_fraction(input);
		path = csd_make_path(output_rel, name, (int) stage,
			list_make1(input), boundary_cost, device_cost, cache_fraction,
			rows, bytes, safe_rows, safe_bytes,
			stage == UPPERREL_ORDERED ? root->query_pathkeys :
				input->pathkeys, (Node *) payload);
		if (root->upper_targets[stage] != NULL)
			path->path.pathtarget = root->upper_targets[stage];
		csd_publish_path(root, output_rel, path);
	}
	if (csd_exe_path == CSD_EXE_CSD)
		csd_keep_only_paths(output_rel);
}
