#include "postgres.h"

#include "catalog/pg_proc.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "utils/lsyscache.h"

#include "csd_pg.h"

typedef struct CsdExpressionContext
{
	const char *reason;
} CsdExpressionContext;

static bool
csd_name_in(const char *name, const char *const *names, int count)
{
	int index;

	if (name == NULL)
		return false;
	for (index = 0; index < count; ++index)
		if (strcmp(name, names[index]) == 0)
			return true;
	return false;
}

static bool
csd_expression_walker(Node *node, void *context)
{
	CsdExpressionContext *state = (CsdExpressionContext *) context;

	if (node == NULL)
		return false;
	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_Param:
		case T_CaseTestExpr:
		case T_CoerceToDomainValue:
		case T_SQLValueFunction:
			return false;

		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
		{
			OpExpr *expression = (OpExpr *) node;
			const char *name = get_opname(expression->opno);
			static const char *const allowed[] = {
				"=", "<>", "<", "<=", ">", ">=", "+", "-", "*", "/",
				"~~", "!~~"
			};

			if (!csd_name_in(name, allowed, lengthof(allowed)) ||
				op_volatile(expression->opno) == PROVOLATILE_VOLATILE)
			{
				state->reason = psprintf("operator %s is not in the bounded CSD expression set",
									 name != NULL ? name : "<unknown>");
				return true;
			}
			return expression_tree_walker(node, csd_expression_walker, context);
		}

		case T_ScalarArrayOpExpr:
		{
			ScalarArrayOpExpr *expression = (ScalarArrayOpExpr *) node;
			const char *name = get_opname(expression->opno);

			if (name == NULL || (strcmp(name, "=") != 0 && strcmp(name, "<>") != 0))
			{
				state->reason = "only IN/NOT IN scalar-array comparisons are supported";
				return true;
			}
			return expression_tree_walker(node, csd_expression_walker, context);
		}

		case T_FuncExpr:
		{
			FuncExpr *expression = (FuncExpr *) node;
			const char *name = get_func_name(expression->funcid);
			static const char *const allowed[] = {
				"substring", "substr", "date_part", "extract",
				"timestamp",
				"numeric", "int2", "int4", "int8", "float4", "float8",
				"text", "bpchar", "btrim"
			};

			if (!csd_name_in(name, allowed, lengthof(allowed)) ||
				func_volatile(expression->funcid) == PROVOLATILE_VOLATILE)
			{
				state->reason = psprintf("function %s has no FPGA-style CSD lowering",
									 name != NULL ? name : "<unknown>");
				return true;
			}
			return expression_tree_walker(node, csd_expression_walker, context);
		}

		case T_Aggref:
		{
			Aggref *aggregate = (Aggref *) node;
			const char *name = get_func_name(aggregate->aggfnoid);
			static const char *const allowed[] = {"count", "sum", "min", "max", "avg"};

			if (!csd_name_in(name, allowed, lengthof(allowed)))
			{
				state->reason = "aggregate is not supported by the CSD aggregate core";
				return true;
			}
			return expression_tree_walker(node, csd_expression_walker, context);
		}

		case T_BoolExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_CaseExpr:
		case T_CaseWhen:
		case T_RelabelType:
		case T_CoerceViaIO:
		case T_CoerceToDomain:
		case T_CollateExpr:
		case T_ArrayExpr:
		case T_FieldSelect:
		case T_PlaceHolderVar:
		case T_TargetEntry:
		case T_SubPlan:
		case T_AlternativeSubPlan:
			return expression_tree_walker(node, csd_expression_walker, context);

		case T_SortGroupClause:
			/* References a target-list entry; it contains no executable Expr. */
			return false;

		default:
			state->reason = psprintf("expression node tag %d has no CSD bytecode opcode",
									 (int) nodeTag(node));
			return true;
	}
}

bool
csd_expression_supported(Node *node, const char **reason)
{
	CsdExpressionContext context = {NULL};
	bool unsupported = csd_expression_walker(node, &context);

	if (reason != NULL)
		*reason = context.reason;
	return !unsupported;
}

bool
csd_expression_list_supported(List *expressions, const char **reason)
{
	ListCell *cell;

	foreach(cell, expressions)
	{
		Node *node = (Node *) lfirst(cell);

		if (IsA(node, RestrictInfo))
			node = (Node *) ((RestrictInfo *) node)->clause;
		if (!csd_expression_supported(node, reason))
			return false;
	}
	return true;
}

const char *
csd_upper_stage_name(UpperRelationKind stage)
{
	switch (stage)
	{
		case UPPERREL_GROUP_AGG:
			return "Aggregate";
		case UPPERREL_DISTINCT:
			return "Distinct";
		case UPPERREL_ORDERED:
			return "Sort";
		case UPPERREL_FINAL:
			return "Final";
		case UPPERREL_PARTIAL_GROUP_AGG:
			return "Partial Aggregate";
		case UPPERREL_SETOP:
			return "SetOp";
		case UPPERREL_WINDOW:
			return "Window";
	}
	return "Unknown";
}
