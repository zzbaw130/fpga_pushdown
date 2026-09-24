#include "postgres.h"

#include "access/table.h"
#include "nodes/makefuncs.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"
#include "utils/rel.h"

#include "csd_pg.h"

static List *csd_rewrite_node(PlannerInfo *root, List *metadata,
							  uint32 *rewrite_count);

/* Planner reltargets inside a correlated subplan normally omit the inner
 * correlation key: PostgreSQL consumes it in an index condition and only
 * returns the aggregate argument.  A standalone CSD DAG must carry that key
 * from its scan through each join so the rewritten grouped aggregate can join
 * back to the outer stream. */
static bool
csd_require_inner_var(List *metadata, Var *variable)
{
	const char *name = strVal(list_nth(metadata, 0));
	List *children = (List *) list_nth(metadata, 4);
	ListCell *cell;

	if (strcmp(name, "Scan") == 0)
	{
		List *payload;
		List *targets;

		if ((Index) intVal(list_nth(metadata, 1)) != variable->varno)
			return false;
		payload = (List *) list_nth(metadata, 7);
		targets = (List *) list_nth(payload, 1);
		if (!list_member(targets, variable))
			lfirst(list_nth_cell(payload, 1)) =
				lappend(targets, copyObject(variable));
		return true;
	}
	foreach(cell, children)
	{
		if (csd_require_inner_var((List *) lfirst(cell), variable))
		{
			if (strstr(name, "Join") != NULL)
			{
				List *payload = (List *) list_nth(metadata, 7);
				List *targets = (List *) list_nth(payload, 2);

				if (!list_member(targets, variable))
					lfirst(list_nth_cell(payload, 2)) =
						lappend(targets, copyObject(variable));
			}
			return true;
		}
	}
	return false;
}

static List *
csd_subplan_metadata(PlannerInfo *root, SubPlan *subplan, Plan **subplan_plan)
{
	Plan *plan;
	CustomScan *scan;
	List *metadata = NIL;
	int index;

	if (subplan->plan_id <= 0 || subplan->plan_id > list_length(root->glob->subplans))
		return NIL;
	plan = (Plan *) list_nth(root->glob->subplans, subplan->plan_id - 1);
	if (!IsA(plan, CustomScan))
		return NIL;
	scan = (CustomScan *) plan;
	if (scan->methods != &csd_scan_methods ||
		list_length(scan->custom_private) < 8)
		return NIL;
	for (index = 0; index < 8; ++index)
		metadata = lappend(metadata,
			copyObject(list_nth(scan->custom_private, index)));
	if (subplan_plan != NULL)
		*subplan_plan = plan;
	return metadata;
}

static List *
csd_metadata_clauses(List *metadata, int *payload_index)
{
	const char *name = strVal(list_nth(metadata, 0));
	List *payload = (List *) list_nth(metadata, 7);

	if (strcmp(name, "Scan") == 0)
	{
		*payload_index = 2;
		return (List *) list_nth(payload, 2);
	}
	if (strstr(name, "Join") != NULL)
	{
		*payload_index = 1;
		return (List *) list_nth(payload, 1);
	}
	return NIL;
}

static void
csd_metadata_set_clauses(List *metadata, int payload_index, List *clauses)
{
	List *payload = (List *) list_nth(metadata, 7);

	lfirst(list_nth_cell(payload, payload_index)) = clauses;
}

static bool
csd_metadata_var_not_null(List *metadata, Var *variable)
{
	const char *name = strVal(list_nth(metadata, 0));
	List *children = (List *) list_nth(metadata, 4);
	ListCell *cell;

	if (strcmp(name, "Scan") == 0 &&
		(Index) intVal(list_nth(metadata, 1)) == variable->varno)
	{
		List *payload = (List *) list_nth(metadata, 7);
		Oid relation_oid = (Oid) strtoul(strVal(linitial(payload)), NULL, 10);
		Relation relation = table_open(relation_oid, NoLock);
		bool not_null = variable->varattno > 0 &&
			variable->varattno <= RelationGetDescr(relation)->natts &&
			TupleDescAttr(RelationGetDescr(relation),
				variable->varattno - 1)->attnotnull;

		table_close(relation, NoLock);
		return not_null;
	}
	foreach(cell, children)
		if (csd_metadata_var_not_null((List *) lfirst(cell), variable))
			return true;
	return false;
}

static List *
csd_make_join_metadata(const char *name, JoinType jointype, List *left,
					   List *right, List *clauses)
{
	List *metadata = list_make4(makeString(pstrdup(name)),
		makeInteger((int) jointype), copyObject(list_nth(left, 2)),
		copyObject(list_nth(left, 3)));

	metadata = lappend(metadata, list_make2(left, right));
	metadata = lappend(metadata, copyObject(list_nth(left, 5)));
	metadata = lappend(metadata, copyObject(list_nth(left, 6)));
	metadata = lappend(metadata, list_make3(makeInteger((int) jointype),
		clauses, NIL));
	return metadata;
}

static bool
csd_remove_correlated_param(List *metadata, int param_id,
							OpExpr **correlation, Node **inner_expression)
{
	List *children = (List *) list_nth(metadata, 4);
	ListCell *cell;
	int payload_index = -1;
	List *clauses = csd_metadata_clauses(metadata, &payload_index);

	foreach(cell, clauses)
	{
		Node *clause = (Node *) lfirst(cell);

		if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
		{
			OpExpr *operation = (OpExpr *) clause;
			Node *first = (Node *) linitial(operation->args);
			Node *second = (Node *) lsecond(operation->args);
			Param *parameter = NULL;
			Node *inner = NULL;

			if (IsA(first, Param) && ((Param *) first)->paramid == param_id)
			{
				parameter = (Param *) first;
				inner = second;
			}
			else if (IsA(second, Param) && ((Param *) second)->paramid == param_id)
			{
				parameter = (Param *) second;
				inner = first;
			}
			if (parameter != NULL)
			{
				*correlation = copyObject(operation);
				*inner_expression = copyObject(inner);
				csd_metadata_set_clauses(metadata, payload_index,
					list_delete_ptr(clauses, clause));
				return true;
			}
		}
	}
	foreach(cell, children)
		if (csd_remove_correlated_param((List *) lfirst(cell), param_id,
			correlation, inner_expression))
			return true;
	return false;
}

static bool
csd_replace_param_argument(OpExpr *operation, int param_id, Node *replacement)
{
	ListCell *cell;

	foreach(cell, operation->args)
	{
		Node *argument = (Node *) lfirst(cell);

		if (IsA(argument, Param) && ((Param *) argument)->paramid == param_id)
		{
			lfirst(cell) = copyObject(replacement);
			return true;
		}
	}
	return false;
}

static bool
csd_replace_subplan_argument(OpExpr *operation, SubPlan *subplan,
							 Node *replacement)
{
	ListCell *cell;

	foreach(cell, operation->args)
		if (lfirst(cell) == subplan ||
			(IsA(lfirst(cell), SubPlan) &&
			 ((SubPlan *) lfirst(cell))->plan_id == subplan->plan_id))
		{
			lfirst(cell) = copyObject(replacement);
			return true;
		}
	return false;
}

static List *
csd_rewrite_not_in(PlannerInfo *root, List *metadata, Node *clause,
				   SubPlan *subplan, uint32 *rewrite_count)
{
	Plan *plan = NULL;
	List *inner_metadata;
	OpExpr *test;
	Node *outer_expression = NULL;
	Node *inner_expression;
	OpExpr *join_clause;
	ListCell *cell;
	int payload_index = -1;
	List *clauses;

	if (subplan->subLinkType != ANY_SUBLINK || subplan->parParam != NIL ||
		!IsA(subplan->testexpr, OpExpr))
		return metadata;
	inner_metadata = csd_subplan_metadata(root, subplan, &plan);
	if (inner_metadata == NIL || plan->targetlist == NIL)
		return metadata;
	test = (OpExpr *) subplan->testexpr;
	if (list_length(test->args) != 2)
		return metadata;
	foreach(cell, test->args)
	{
		Node *argument = (Node *) lfirst(cell);

		if (!IsA(argument, Param))
			outer_expression = argument;
	}
	inner_expression = (Node *) ((TargetEntry *) linitial(plan->targetlist))->expr;
	if (outer_expression == NULL || !IsA(outer_expression, Var) ||
		!IsA(inner_expression, Var) ||
		!csd_metadata_var_not_null(metadata, (Var *) outer_expression) ||
		!csd_metadata_var_not_null(inner_metadata, (Var *) inner_expression))
		return metadata;
	join_clause = copyObject(test);
	if (!csd_replace_param_argument(join_clause,
		linitial_int(subplan->paramIds), inner_expression))
		return metadata;
	clauses = csd_metadata_clauses(metadata, &payload_index);
	csd_metadata_set_clauses(metadata, payload_index,
		list_delete_ptr(clauses, clause));
	inner_metadata = csd_rewrite_node(root, inner_metadata, rewrite_count);
	++*rewrite_count;
	return csd_make_join_metadata("Hash Anti Join", JOIN_ANTI, metadata,
		inner_metadata, list_make1(join_clause));
}

static List *
csd_rewrite_correlated_scalar(PlannerInfo *root, List *metadata, Node *clause,
							  SubPlan *subplan, uint32 *rewrite_count)
{
	List *inner_metadata;
	Plan *plan = NULL;
	OpExpr *correlation = NULL;
	Node *inner_correlation = NULL;
	Node *outer_correlation;
	Node *outer_value;
	List *aggregate_payload;
	List *aggregate_targets;
	Node *aggregate_result;
	OpExpr *value_clause;
	List *clauses;
	int payload_index = -1;
	int param_id;

	if (subplan->subLinkType != EXPR_SUBLINK ||
		list_length(subplan->parParam) != 1 ||
		list_length(subplan->args) != 1 || !IsA(clause, OpExpr))
		return metadata;
	inner_metadata = csd_subplan_metadata(root, subplan, &plan);
	if (inner_metadata == NIL || strcmp(strVal(linitial(inner_metadata)),
		"Aggregate") != 0)
		return metadata;
	param_id = linitial_int(subplan->parParam);
	if (!csd_remove_correlated_param(inner_metadata, param_id, &correlation,
		&inner_correlation))
		return metadata;
	outer_correlation = (Node *) linitial(subplan->args);
	if (!csd_replace_param_argument(correlation, param_id, outer_correlation))
		return metadata;
	outer_value = IsA(linitial(((OpExpr *) clause)->args), SubPlan) ?
		(Node *) lsecond(((OpExpr *) clause)->args) :
		(Node *) linitial(((OpExpr *) clause)->args);
	if (IsA(outer_correlation, Var))
		(void) csd_require_inner_var(metadata, (Var *) outer_correlation);
	if (IsA(outer_value, Var))
		(void) csd_require_inner_var(metadata, (Var *) outer_value);
	aggregate_payload = (List *) list_nth(inner_metadata, 7);
	aggregate_targets = (List *) list_nth(aggregate_payload, 1);
	if (aggregate_targets == NIL)
		return metadata;
	aggregate_result = (Node *) linitial(aggregate_targets);
	lfirst(list_nth_cell(aggregate_payload, 1)) =
		lappend(aggregate_targets, copyObject(inner_correlation));
	if (IsA(inner_correlation, Var))
	{
		List *children = (List *) list_nth(inner_metadata, 4);

		if (children == NIL || !csd_require_inner_var(
			(List *) linitial(children), (Var *) inner_correlation))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("CSD correlated aggregate could not expose its inner key")));
	}
	value_clause = copyObject((OpExpr *) clause);
	if (!csd_replace_subplan_argument(value_clause, subplan, aggregate_result))
		return metadata;
	clauses = csd_metadata_clauses(metadata, &payload_index);
	csd_metadata_set_clauses(metadata, payload_index,
		list_delete_ptr(clauses, clause));
	inner_metadata = csd_rewrite_node(root, inner_metadata, rewrite_count);
	++*rewrite_count;
	return csd_make_join_metadata("Hash Join", JOIN_INNER, metadata,
		inner_metadata, list_make2(correlation, value_clause));
}

static List *
csd_rewrite_node(PlannerInfo *root, List *metadata, uint32 *rewrite_count)
{
	List *children = (List *) list_nth(metadata, 4);
	ListCell *cell;
	int payload_index = -1;
	List *clauses;

	foreach(cell, children)
		lfirst(cell) = csd_rewrite_node(root, (List *) lfirst(cell),
			rewrite_count);
	clauses = csd_metadata_clauses(metadata, &payload_index);
	foreach(cell, clauses)
	{
		Node *clause = (Node *) lfirst(cell);

		if (IsA(clause, BoolExpr) &&
			((BoolExpr *) clause)->boolop == NOT_EXPR &&
			list_length(((BoolExpr *) clause)->args) == 1 &&
			IsA(linitial(((BoolExpr *) clause)->args), SubPlan))
			return csd_rewrite_not_in(root, metadata, clause,
				(SubPlan *) linitial(((BoolExpr *) clause)->args), rewrite_count);
		if (IsA(clause, OpExpr) && list_length(((OpExpr *) clause)->args) == 2)
		{
			Node *first = (Node *) linitial(((OpExpr *) clause)->args);
			Node *second = (Node *) lsecond(((OpExpr *) clause)->args);
			SubPlan *subplan = IsA(first, SubPlan) ? (SubPlan *) first :
				(IsA(second, SubPlan) ? (SubPlan *) second : NULL);

			if (subplan != NULL)
				return csd_rewrite_correlated_scalar(root, metadata, clause,
					subplan, rewrite_count);
		}
	}
	return metadata;
}

List *
csd_rewrite_subplans(PlannerInfo *root, List *metadata, uint32 *rewrite_count)
{
	List *copy = copyObject(metadata);

	if (rewrite_count != NULL)
		*rewrite_count = 0;
	return csd_rewrite_node(root, copy, rewrite_count);
}
