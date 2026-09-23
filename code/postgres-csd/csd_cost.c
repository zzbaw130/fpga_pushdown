#include "postgres.h"

#include <math.h>

#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"

#include "csd_pg.h"

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
csd_path_modeled_cost(const Path *path)
{
	if (csd_is_path(path))
	{
		const CustomPath *custom = (const CustomPath *) path;

		return floatVal(list_nth(custom->custom_private, 2));
	}
	if (path != NULL && IsA(path, ProjectionPath))
	{
		const ProjectionPath *project = (const ProjectionPath *) path;

		return csd_path_modeled_cost(project->subpath) +
			Max(1.0, path->rows) * csd_tuple_cost;
	}
	if (path != NULL && IsA(path, SubqueryScanPath))
	{
		const SubqueryScanPath *subquery = (const SubqueryScanPath *) path;

		return csd_path_modeled_cost(subquery->subpath) +
			Max(1.0, path->rows) * csd_tuple_cost;
	}
	pg_unreachable();
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
							  makeFloat(psprintf("%.17g", csd_path_modeled_cost(path))),
							  makeFloat(psprintf("%.17g", csd_host_cache_fraction)));
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
							  makeFloat(psprintf("%.17g", csd_path_modeled_cost(path))),
							  makeFloat(psprintf("%.17g", csd_host_cache_fraction)));
		metadata = lappend(metadata, children);
		metadata = lappend(metadata, makeFloat(psprintf("%.17g", Max(1.0, path->rows))));
		metadata = lappend(metadata, makeFloat(psprintf("%.17g",
			Max(1.0, path->rows) * Max(1, path->pathtarget->width))));
		return lappend(metadata, copyObject(path->pathtarget->exprs));
	}
	pg_unreachable();
}

static Path *
csd_cheapest_path(RelOptInfo *rel)
{
	Path *best = NULL;
	double best_cost = 0;
	ListCell *cell;

	foreach(cell, rel->pathlist)
	{
		Path *path = (Path *) lfirst(cell);

		if (csd_is_backed_path(path) &&
			(best == NULL || csd_path_modeled_cost(path) < best_cost))
		{
			best = path;
			best_cost = csd_path_modeled_cost(path);
		}
	}
	return best;
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
			  Cost startup_cost, Cost total_cost, double modeled_cost,
			  List *pathkeys, Node *payload)
{
	CustomPath *path = makeNode(CustomPath);
	Cost planner_startup = startup_cost;
	Cost planner_total = total_cost;
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
	path->path.rows = Max(1.0, rel->rows);
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
										makeFloat(psprintf("%.17g", modeled_cost)),
										makeFloat(psprintf("%.17g", csd_host_cache_fraction)));
	path->custom_private = lappend(path->custom_private, child_metadata);
	path->custom_private = lappend(path->custom_private,
		makeFloat(psprintf("%.17g", Max(1.0, rel->rows))));
	path->custom_private = lappend(path->custom_private,
		makeFloat(psprintf("%.17g", Max(1.0, rel->rows) *
			Max(1, rel->reltarget->width))));
	path->custom_private = lappend(path->custom_private, payload);
	path->methods = &csd_path_methods;
	return path;
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
	double modeled;
	CustomPath *path;
	List *payload;

	(void) root;
	if (csd_exe_path == CSD_EXE_HOST || rte->rtekind != RTE_RELATION || rte->inh)
		return;
	if (!csd_expression_list_supported(rel->baserestrictinfo, &reason) ||
		!csd_expression_list_supported(rel->reltarget->exprs, &reason))
	{
		csd_reject_if_forced(reason);
		return;
	}
	rows = Max(1.0, rel->rows);
	bytes = rows * Max(1, rel->reltarget->width);
	/* Cache residency makes the competing host path more attractive. */
	modeled = csd_setup_cost + rel->pages * csd_flash_page_cost +
		rows * csd_tuple_cost + bytes * csd_result_byte_cost +
		rel->pages * csd_host_cache_fraction * seq_page_cost;
	payload = list_make3(makeString(psprintf("%u", rte->relid)),
		copyObject(rel->reltarget->exprs),
		extract_actual_clauses(rel->baserestrictinfo, false));
	path = csd_make_path(rel, "Scan", (int) rti, NIL,
					 csd_setup_cost, modeled, modeled, NIL, (Node *) payload);
	add_path(rel, &path->path);
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
	double modeled;
	CustomPath *path;
	List *payload;

	(void) root;
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
	outer = csd_cheapest_path(outerrel);
	inner = csd_cheapest_path(innerrel);
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
	bytes = rows * Max(1, joinrel->reltarget->width);
	modeled = csd_path_modeled_cost(outer) + csd_path_modeled_cost(inner) +
		(outer->rows + inner->rows) * csd_join_tuple_cost +
		bytes * csd_result_byte_cost;
	payload = list_make3(makeInteger((int) jointype),
		extract_actual_clauses(extra->restrictlist, false),
		copyObject(joinrel->reltarget->exprs));
	path = csd_make_path(joinrel, name, (int) jointype,
					 list_make2(outer, inner), csd_setup_cost, modeled,
					 modeled, NIL, (Node *) payload);
	add_path(joinrel, &path->path);
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
	double stage_cost = 0;
	double modeled;
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
	input = csd_cheapest_path(input_rel);
	if (input == NULL)
		return;
	rows = stage == UPPERREL_FINAL ? Max(1.0, input->rows) :
		Max(1.0, output_rel->rows);
	if (stage == UPPERREL_FINAL)
	{
		final_extra = (FinalPathExtraData *) extra;
		if (final_extra != NULL && final_extra->limit_needed &&
			final_extra->count_est >= 0)
			rows = Min(rows, (double) final_extra->count_est);
	}
	bytes = rows * Max(1, root->upper_targets[stage] != NULL ?
		root->upper_targets[stage]->width : output_rel->reltarget->width);
	name = csd_upper_stage_name(stage);
	if (stage == UPPERREL_FINAL && final_extra != NULL &&
		final_extra->limit_needed)
		name = "Limit";
	switch (stage)
	{
		case UPPERREL_GROUP_AGG:
		case UPPERREL_DISTINCT:
			stage_cost = input->rows * csd_aggregate_tuple_cost;
			break;
		case UPPERREL_ORDERED:
			stage_cost = input->rows * Max(1.0, log(Max(2.0, input->rows)) / log(2.0)) *
				csd_sort_compare_cost;
			break;
		case UPPERREL_FINAL:
			stage_cost = rows * csd_tuple_cost;
			break;
		default:
			return;
	}
	modeled = csd_path_modeled_cost(input) + stage_cost +
		bytes * csd_result_byte_cost;
	payload = list_make4(makeInteger((int) stage),
		copyObject(root->upper_targets[stage] != NULL ?
			root->upper_targets[stage]->exprs : output_rel->reltarget->exprs),
		copyObject(root->parse->groupClause),
		copyObject(root->parse->sortClause));
	payload = lappend(payload, copyObject(root->parse->havingQual));
	if (stage == UPPERREL_FINAL && final_extra != NULL)
	{
		payload = lappend(payload,
			makeString(psprintf(INT64_FORMAT, final_extra->count_est)));
		payload = lappend(payload,
			makeString(psprintf(INT64_FORMAT, final_extra->offset_est)));
	}
	path = csd_make_path(output_rel, name, (int) stage, list_make1(input),
						input->startup_cost, modeled, modeled,
						stage == UPPERREL_ORDERED ? root->query_pathkeys : input->pathkeys,
						(Node *) payload);
	if (root->upper_targets[stage] != NULL)
		path->path.pathtarget = root->upper_targets[stage];
	path->path.rows = Max(1.0, rows);
	add_path(output_rel, &path->path);
	if (csd_exe_path == CSD_EXE_CSD)
		csd_keep_only_paths(output_rel);
}
