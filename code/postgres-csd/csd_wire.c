#include "postgres.h"

#include <math.h>

#include "access/table.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/value.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "storage/bufmgr.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "csd_expr.h"
#include "csd_plan.h"
#include "csd_pg.h"

typedef struct CsdSqlPlanHeader CsdSqlPlanHeader;
typedef struct CsdSqlPlanNode CsdSqlPlanNode;
typedef struct CsdSqlScanSpec CsdSqlScanSpec;
typedef struct CsdSqlColumnSpec CsdSqlColumnSpec;
typedef struct CsdSqlExpressionSpec CsdSqlExpressionSpec;
typedef struct CsdSqlJoinSpec CsdSqlJoinSpec;
typedef struct CsdSqlJoinKey CsdSqlJoinKey;
typedef struct CsdSqlAggregateSpec CsdSqlAggregateSpec;
typedef struct CsdSqlAggregateColumn CsdSqlAggregateColumn;
typedef struct CsdSqlSortSpec CsdSqlSortSpec;
typedef struct CsdSqlSortKey CsdSqlSortKey;

typedef struct CsdWireBuilder
{
	StringInfoData bytes;
	uint32 node_count;
	uint32 next_node;
	uint32 next_input_range;
	uint32 expression_count;
	uint32 parameter_count;
	uint32 pending_expression_count;
} CsdWireBuilder;

typedef struct CsdWireResult
{
	uint32 node_id;
	CsdExprSchema schema;
} CsdWireResult;

typedef struct CsdWireAggregateShape
{
	List *groups;
	List *aggregates;
	List *inputs;
	List *outputs;
	List *targets;
	Node *having;
} CsdWireAggregateShape;

static uint32 csd_wire_count_nodes(List *metadata);
static CsdWireResult csd_wire_emit_node(CsdWireBuilder *builder, List *metadata);
static void csd_wire_validate(const StringInfoData *bytes);

static Node *
csd_wire_expression(Node *node)
{
	return IsA(node, TargetEntry) ? (Node *) ((TargetEntry *) node)->expr : node;
}

static Bitmapset *
csd_wire_scan_required_attrs(List *metadata, Index scan_rti,
							 TupleDesc descriptor)
{
	List *payload = (List *) list_nth(metadata, 7);
	List *targets = (List *) list_nth(payload, 1);
	List *clauses = (List *) list_nth(payload, 2);
	Bitmapset *attrs = NULL;
	ListCell *cell;
	List *sources = list_concat(list_copy(targets), list_copy(clauses));

	foreach(cell, sources)
	{
		Node *expression = csd_wire_expression((Node *) lfirst(cell));
		List *variables = pull_var_clause(expression,
			PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS |
			PVC_RECURSE_PLACEHOLDERS);
		ListCell *variable_cell;

		foreach(variable_cell, variables)
		{
			Var *variable = (Var *) lfirst(variable_cell);

			if (!IsA(variable, Var) || variable->varlevelsup != 0 ||
				variable->varno != scan_rti)
				continue;
			if (variable->varattno < 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("CSD scan does not support PostgreSQL system columns")));
			attrs = bms_add_member(attrs, variable->varattno);
		}
		list_free(variables);
	}
	list_free(sources);
	if (attrs == NULL)
	{
		int attribute;

		for (attribute = 0; attribute < descriptor->natts; ++attribute)
			if (!TupleDescAttr(descriptor, attribute)->attisdropped)
			{
				attrs = bms_add_member(attrs, attribute + 1);
				break;
			}
	}
	return attrs;
}

static bool
csd_wire_collect_aggregates(Node *node, void *context)
{
	List **aggregates = (List **) context;

	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		if (!list_member(*aggregates, node))
			*aggregates = lappend(*aggregates, node);
		return false;
	}
	return expression_tree_walker(node, csd_wire_collect_aggregates, context);
}

static List *
csd_wire_sortgroup_expressions(List *clauses, List *target_list)
{
	List *expressions = NIL;
	ListCell *cell;

	foreach(cell, clauses)
		expressions = lappend(expressions,
			get_sortgroupclause_expr((SortGroupClause *) lfirst(cell), target_list));
	return expressions;
}

static Node *
csd_wire_qual_expression(Node *qual)
{
	List *clauses;
	BoolExpr *combined;

	if (qual == NULL || !IsA(qual, List))
		return qual;
	clauses = (List *) qual;
	if (list_length(clauses) == 1)
		return (Node *) linitial(clauses);
	combined = makeNode(BoolExpr);
	combined->boolop = AND_EXPR;
	combined->args = clauses;
	combined->location = -1;
	return (Node *) combined;
}

static CsdWireAggregateShape
csd_wire_aggregate_shape(List *metadata)
{
	List *payload = (List *) list_nth(metadata, 7);
	const char *name = strVal(list_nth(metadata, 0));
	List *target_list = (List *) list_nth(payload, 5);
	List *group_clauses = strcmp(name, "Distinct") == 0 ?
		(List *) list_nth(payload, 6) : (List *) list_nth(payload, 2);
	CsdWireAggregateShape shape;
	ListCell *cell;

	MemSet(&shape, 0, sizeof(shape));
	shape.groups = csd_wire_sortgroup_expressions(group_clauses, target_list);
	shape.targets = (List *) list_nth(payload, 1);
	shape.having = strcmp(name, "Aggregate") == 0 ?
		csd_wire_qual_expression((Node *) list_nth(payload, 4)) : NULL;
	/*
	 * PostgreSQL can remove functionally dependent SELECT columns from its
	 * physical Group Key (for example customer columns determined by the
	 * primary key).  The standalone CSD aggregate has no host executor to
	 * restore them, so include every non-aggregate output as an equivalent
	 * hardware group key.
	 */
	foreach(cell, shape.targets)
	{
		Node *expression = csd_wire_expression((Node *) lfirst(cell));

		if (!contain_agg_clause(expression) &&
			!list_member(shape.groups, expression))
			shape.groups = lappend(shape.groups, expression);
	}
	foreach(cell, shape.targets)
		csd_wire_collect_aggregates((Node *) lfirst(cell), &shape.aggregates);
	csd_wire_collect_aggregates(shape.having, &shape.aggregates);
	shape.inputs = list_copy(shape.groups);
	foreach(cell, shape.aggregates)
	{
		Aggref *aggregate = (Aggref *) lfirst(cell);

		if (aggregate->aggfilter != NULL || list_length(aggregate->args) > 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("CSD aggregate supports one argument and no FILTER clause")));
		if (aggregate->args != NIL)
			shape.inputs = lappend(shape.inputs,
				csd_wire_expression((Node *) linitial(aggregate->args)));
	}
	shape.outputs = list_concat(list_copy(shape.groups),
		list_copy(shape.aggregates));
	return shape;
}

static uint16
csd_wire_join_kind(const char *name)
{
	if (strstr(name, "Left") != NULL || strstr(name, "Right") != NULL)
		return CSD_SQL_JOIN_LEFT;
	if (strstr(name, "Semi") != NULL)
		return CSD_SQL_JOIN_SEMI;
	if (strstr(name, "Anti") != NULL)
		return CSD_SQL_JOIN_ANTI;
	return CSD_SQL_JOIN_INNER;
}

static CsdExprSchema
csd_wire_concat_schema(const CsdExprSchema *left, const CsdExprSchema *right)
{
	CsdExprSchema result;

	if ((uint32) left->count + right->count > PG_UINT16_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("CSD intermediate schema exceeds 65535 columns")));
	result.count = left->count + right->count;
	result.columns = palloc0(result.count * sizeof(*result.columns));
	memcpy(result.columns, left->columns, left->count * sizeof(*result.columns));
	memcpy(result.columns + left->count, right->columns,
		right->count * sizeof(*result.columns));
	return result;
}

static CsdExprSchema
csd_wire_scan_schema(List *metadata, Index scan_rti)
{
	List *payload = (List *) list_nth(metadata, 7);
	Oid relation_oid = (Oid) strtoul(strVal(list_nth(payload, 0)), NULL, 10);
	Relation relation = table_open(relation_oid, NoLock);
	CsdExprSchema schema;
	Bitmapset *required_attrs = csd_wire_scan_required_attrs(metadata, scan_rti,
		RelationGetDescr(relation));

	csd_expr_schema_from_scan(&schema, scan_rti, RelationGetDescr(relation),
		required_attrs);
	bms_free(required_attrs);
	table_close(relation, NoLock);
	return schema;
}

static uint8
csd_wire_datum_type(Oid type_oid)
{
	switch (type_oid)
	{
		case INT2OID: return CSD_SQL_TYPE_INT16;
		case INT4OID: return CSD_SQL_TYPE_INT32;
		case INT8OID: return CSD_SQL_TYPE_INT64;
		case DATEOID: return CSD_SQL_TYPE_DATE;
		case TIMESTAMPOID: return CSD_SQL_TYPE_DATE;
		case BOOLOID: return CSD_SQL_TYPE_BOOL;
		case NUMERICOID: return CSD_SQL_TYPE_NUMERIC;
		case CHAROID:
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
			return CSD_SQL_TYPE_BYTES;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PostgreSQL type OID %u has no CSD scan decoder", type_oid)));
	}
	pg_unreachable();
}

static uint8
csd_wire_alignment(char alignment)
{
	switch (alignment)
	{
		case 'c': return 1;
		case 's': return 2;
		case 'i': return 4;
		case 'd': return 8;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("unexpected PostgreSQL attribute alignment %c", alignment)));
	}
	pg_unreachable();
}

static void
csd_wire_append(StringInfo target, const void *source, Size bytes)
{
	appendBinaryStringInfo(target, (const char *) source, bytes);
}

static uint64
csd_wire_estimate(List *metadata, int index)
{
	double value = floatVal(list_nth(metadata, index));

	if (value <= 0)
		return 0;
	if (value >= (double) PG_UINT64_MAX)
		return PG_UINT64_MAX;
	return (uint64) ceil(value);
}

static uint16
csd_wire_opcode(const char *name)
{
	if (strcmp(name, "Scan") == 0)
		return CSD_SQL_OP_SCAN;
	if (strcmp(name, "Project") == 0)
		return CSD_SQL_OP_PROJECT;
	if (strstr(name, "Join") != NULL)
		return CSD_SQL_OP_HASH_JOIN;
	if (strcmp(name, "Aggregate") == 0 || strcmp(name, "Distinct") == 0)
		return CSD_SQL_OP_HASH_AGG;
	if (strcmp(name, "Sort") == 0)
		return CSD_SQL_OP_SORT;
	if (strcmp(name, "Limit") == 0)
		return CSD_SQL_OP_LIMIT;
	if (strcmp(name, "Subquery") == 0)
		return CSD_SQL_OP_SUBPLAN;
	if (strcmp(name, "Final") == 0)
		return CSD_SQL_OP_MATERIALIZE;
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CSD metadata operator %s has no ABI v8 opcode", name)));
	pg_unreachable();
}

static uint32
csd_wire_count_nodes(List *metadata)
{
	List *children = (List *) list_nth(metadata, 4);
	ListCell *cell;
	uint32 count = 1;
	const char *name = strVal(list_nth(metadata, 0));

	if (strcmp(name, "Scan") == 0)
	{
		List *payload = (List *) list_nth(metadata, 7);
		List *targets = (List *) list_nth(payload, 1);
		List *clauses = (List *) list_nth(payload, 2);
		List *lowerable = csd_scan_lowerable_clauses(clauses, NULL);

		/* COUNT(*) can legitimately give its scan an empty reltarget. */
		if (targets != NIL)
			++count;
		if (lowerable != NIL)
			++count;
	}
	else if (strstr(name, "Join") != NULL)
	{
		List *payload = (List *) list_nth(metadata, 7);
		List *targets = (List *) list_nth(payload, 2);

		/* A join core emits concatenated inputs; trim/reorder when requested. */
		if (targets != NIL)
			++count;
	}
	else if (strcmp(name, "Aggregate") == 0 || strcmp(name, "Distinct") == 0)
	{
		CsdWireAggregateShape shape = csd_wire_aggregate_shape(metadata);

		if (shape.inputs != NIL)
			++count;
		if (shape.having != NULL)
			++count;
		if (!equal(shape.targets, shape.outputs))
			++count;
	}

	foreach(cell, children)
	{
		uint32 child_count = csd_wire_count_nodes((List *) lfirst(cell));

		if (child_count > PG_UINT32_MAX - count)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("CSD operator tree is too large")));
		count += child_count;
	}
	return count;
}

static void
csd_wire_append_payload(CsdWireBuilder *builder, CsdSqlPlanNode *node,
						List *metadata, const char *name, int detail)
{
	node->expression_offset = builder->bytes.len;
	if (node->opcode == CSD_SQL_OP_SCAN)
	{
		CsdSqlScanSpec scan = {0};
		List *payload = (List *) list_nth(metadata, 7);
		Oid relation_oid = (Oid) strtoul(strVal(list_nth(payload, 0)), NULL, 10);
		Relation relation = table_open(relation_oid, NoLock);
		TupleDesc descriptor = RelationGetDescr(relation);
		Bitmapset *required_attrs = csd_wire_scan_required_attrs(metadata,
			(Index) detail, descriptor);
		uint16 output_column = 0;
		int attribute;

		if (descriptor->natts <= 0 || descriptor->natts > PG_UINT16_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("relation %s has too many attributes for the CSD ABI",
							RelationGetRelationName(relation))));
		scan.input_range = builder->next_input_range++;
		scan.first_page = 0;
		scan.page_count = RelationGetNumberOfBlocks(relation);
		scan.relation_oid = relation_oid;
		scan.relation_bytes = (uint64) scan.page_count * BLCKSZ;
		scan.attr_count = descriptor->natts;
		for (attribute = 0; attribute < descriptor->natts; ++attribute)
			if (!TupleDescAttr(descriptor, attribute)->attisdropped &&
				(bms_is_member(0, required_attrs) ||
				 bms_is_member(attribute + 1, required_attrs)))
				++output_column;
		scan.output_count = output_column;
		scan.flags = CSD_SQL_SCAN_STRICT_PAGE_FORMAT;
		csd_wire_append(&builder->bytes, &scan, sizeof(scan));
		output_column = 0;
		for (attribute = 0; attribute < descriptor->natts; ++attribute)
		{
			Form_pg_attribute pg_attribute = TupleDescAttr(descriptor, attribute);
			CsdSqlColumnSpec column = {0};

			column.attnum = attribute + 1;
			column.attlen = pg_attribute->attlen;
			column.alignment = csd_wire_alignment(pg_attribute->attalign);
			column.datum_type = csd_wire_datum_type(pg_attribute->atttypid);
			if (pg_attribute->atttypid == NUMERICOID)
				column.numeric_scale = pg_attribute->atttypmod >= VARHDRSZ ?
					(pg_attribute->atttypmod - VARHDRSZ) & 0xffff : -1;
			if (!pg_attribute->attisdropped &&
				(bms_is_member(0, required_attrs) ||
				 bms_is_member(attribute + 1, required_attrs)))
			{
				column.output_column = output_column++;
				column.flags = CSD_SQL_COLUMN_OUTPUT;
				if (pg_attribute->attlen == -1)
					column.flags |= CSD_SQL_COLUMN_NO_EXTERNAL;
				if (pg_attribute->atttypid == BPCHAROID)
					column.flags |= CSD_SQL_COLUMN_TRIM_SPACE;
			}
			else
				column.output_column = CSD_SQL_NO_COLUMN;
			csd_wire_append(&builder->bytes, &column, sizeof(column));
		}
		bms_free(required_attrs);
		table_close(relation, NoLock);
	}
	else if (node->opcode == CSD_SQL_OP_HASH_AGG)
	{
		CsdSqlAggregateSpec aggregate = {0};

		csd_wire_append(&builder->bytes, &aggregate, sizeof(aggregate));
	}
	else if (node->opcode == CSD_SQL_OP_SORT)
	{
		CsdSqlSortSpec sort = {0};

		csd_wire_append(&builder->bytes, &sort, sizeof(sort));
	}
	else
		node->expression_offset = 0;
	node->expression_length = node->expression_offset == 0 ? 0 :
		builder->bytes.len - node->expression_offset;
}

static void
csd_wire_append_join_payload(CsdWireBuilder *builder, CsdSqlPlanNode *node,
							 List *metadata, const char *name,
							 const CsdExprSchema *left,
							 const CsdExprSchema *right)
{
	List *payload = (List *) list_nth(metadata, 7);
	List *clauses = (List *) list_nth(payload, 1);
	List *residuals = NIL;
	CsdSqlJoinKey *keys;
	uint16 key_count = 0;
	uint16 key_capacity = Max(1, list_length(clauses));
	ListCell *cell;
	CsdSqlJoinSpec join = {0};
	uint32 residual_offset = 0;
	uint32 residual_length = 0;
	CsdExprSchema joined_schema;

	keys = palloc0(key_capacity * sizeof(*keys));
	foreach(cell, clauses)
	{
		Node *clause = (Node *) lfirst(cell);
		bool is_key = false;

		if (contain_subplans(clause))
		{
			++builder->pending_expression_count;
			continue;
		}

		if (IsA(clause, OpExpr))
		{
			OpExpr *operation = (OpExpr *) clause;
			const char *operator_name = get_opname(operation->opno);

			if (operator_name != NULL && strcmp(operator_name, "=") == 0 &&
				list_length(operation->args) == 2)
			{
				Node *first = (Node *) linitial(operation->args);
				Node *second = (Node *) lsecond(operation->args);
				int left_key = csd_expr_schema_lookup(left, first);
				int right_key = csd_expr_schema_lookup(right, second);

				if (left_key < 0 || right_key < 0)
				{
					left_key = csd_expr_schema_lookup(left, second);
					right_key = csd_expr_schema_lookup(right, first);
				}
				if (left_key >= 0 && right_key >= 0)
				{
					keys[key_count].left_key = left_key;
					keys[key_count].right_key = right_key;
					++key_count;
					is_key = true;
				}
			}
		}
		if (!is_key)
			residuals = lappend(residuals, clause);
	}
	join.key_count = key_count;
	join.join_kind = csd_wire_join_kind(name);
	node->expression_offset = builder->bytes.len;
	csd_wire_append(&builder->bytes, &join, sizeof(join));
	if (key_count > 0)
		csd_wire_append(&builder->bytes, keys, key_count * sizeof(*keys));
	node->expression_length = builder->bytes.len - node->expression_offset;
	if (key_count == 0)
	{
		node->opcode = CSD_SQL_OP_NL_JOIN;
		node->flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;
	}
	if (residuals != NIL)
	{
		BoolExpr *combined = makeNode(BoolExpr);
		List *program;

		combined->boolop = AND_EXPR;
		combined->args = residuals;
		combined->location = -1;
		program = list_make1(combined);
		joined_schema = csd_wire_concat_schema(left, right);
		csd_append_expression_program(&builder->bytes, program, &joined_schema,
			&residual_offset, &residual_length, &builder->parameter_count);
		csd_expr_schema_release(&joined_schema);
		++builder->expression_count;
		join.residual_offset = residual_offset;
		join.residual_length = residual_length;
		memcpy(builder->bytes.data + node->expression_offset, &join, sizeof(join));
	}
	pfree(keys);
}

static void
csd_wire_store_node(CsdWireBuilder *builder, const CsdSqlPlanNode *node)
{
	memcpy(builder->bytes.data + sizeof(CsdSqlPlanHeader) +
		node->node_id * sizeof(*node), node, sizeof(*node));
}

static CsdWireResult
csd_wire_emit_expression_node(CsdWireBuilder *builder, uint16 opcode,
							 CsdWireResult input, List *expressions,
							 List *metadata)
{
	CsdSqlPlanNode node = {0};
	CsdWireResult result;
	uint32 expression_offset;
	uint32 expression_length;

	MemSet(&result, 0, sizeof(result));
	node.node_id = builder->next_node++;
	node.opcode = opcode;
	node.left_input = input.node_id;
	node.right_input = CSD_SQL_INVALID_NODE;
	node.aux_input = CSD_SQL_INVALID_NODE;
	node.estimated_rows = csd_wire_estimate(metadata, 5);
	node.estimated_bytes = csd_wire_estimate(metadata, 6);
	node.arg1 = 1;
	csd_append_expression_program(&builder->bytes, expressions, &input.schema,
		&expression_offset, &expression_length,
		&builder->parameter_count);
	node.expression_offset = expression_offset;
	node.expression_length = expression_length;
	++builder->expression_count;
	csd_wire_store_node(builder, &node);
	result.node_id = node.node_id;
	if (opcode == CSD_SQL_OP_PROJECT)
	{
		csd_expr_schema_from_expressions(&result.schema, expressions);
		csd_expr_schema_release(&input.schema);
	}
	else
	{
		result.schema = input.schema;
		MemSet(&input.schema, 0, sizeof(input.schema));
	}
	return result;
}

static uint8
csd_wire_aggregate_operation(Aggref *aggregate)
{
	const char *name = get_func_name(aggregate->aggfnoid);

	if (strcmp(name, "count") == 0) return CSD_SQL_AGG_COUNT;
	if (strcmp(name, "sum") == 0) return CSD_SQL_AGG_SUM;
	if (strcmp(name, "min") == 0) return CSD_SQL_AGG_MIN;
	if (strcmp(name, "max") == 0) return CSD_SQL_AGG_MAX;
	if (strcmp(name, "avg") == 0) return CSD_SQL_AGG_AVG;
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("aggregate %s has no CSD descriptor", name)));
	pg_unreachable();
}

static CsdWireResult
csd_wire_emit_aggregate(CsdWireBuilder *builder, List *metadata,
						 CsdWireResult input)
{
	CsdWireAggregateShape shape = csd_wire_aggregate_shape(metadata);
	CsdWireResult current = input;
	CsdWireResult result;
	CsdSqlPlanNode node = {0};
	CsdSqlAggregateSpec spec = {0};
	ListCell *cell;
	uint16 input_column;

	if (shape.inputs != NIL)
		current = csd_wire_emit_expression_node(builder, CSD_SQL_OP_PROJECT,
			current, shape.inputs, metadata);
	node.node_id = builder->next_node++;
	node.opcode = CSD_SQL_OP_HASH_AGG;
	node.flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;
	node.left_input = current.node_id;
	node.right_input = CSD_SQL_INVALID_NODE;
	node.aux_input = CSD_SQL_INVALID_NODE;
	node.estimated_rows = csd_wire_estimate(metadata, 5);
	node.estimated_bytes = csd_wire_estimate(metadata, 6);
	node.arg1 = 1;
	node.expression_offset = builder->bytes.len;
	spec.group_count = list_length(shape.groups);
	spec.aggregate_count = list_length(shape.aggregates);
	csd_wire_append(&builder->bytes, &spec, sizeof(spec));
	for (input_column = 0; input_column < spec.group_count; ++input_column)
		csd_wire_append(&builder->bytes, &input_column, sizeof(input_column));
	input_column = spec.group_count;
	foreach(cell, shape.aggregates)
	{
		Aggref *aggregate = (Aggref *) lfirst(cell);
		CsdSqlAggregateColumn column = {0};
		Node *argument = aggregate->args == NIL ? NULL :
			csd_wire_expression((Node *) linitial(aggregate->args));

		column.input_column = argument == NULL ? CSD_SQL_NO_COLUMN : input_column++;
		column.operation = csd_wire_aggregate_operation(aggregate);
		column.datum_type = csd_wire_datum_type(aggregate->aggtype);
		column.output_scale = aggregate->aggtype == NUMERICOID ?
			csd_expression_numeric_scale((Node *) aggregate) : 0;
		if (aggregate->aggdistinct != NIL)
			column.flags |= CSD_SQL_AGG_DISTINCT;
		csd_wire_append(&builder->bytes, &column, sizeof(column));
	}
	node.expression_length = builder->bytes.len - node.expression_offset;
	csd_wire_store_node(builder, &node);
	csd_expr_schema_release(&current.schema);
	result.node_id = node.node_id;
	csd_expr_schema_from_expressions(&result.schema, shape.outputs);
	if (shape.having != NULL)
		result = csd_wire_emit_expression_node(builder, CSD_SQL_OP_FILTER,
			result, list_make1(shape.having), metadata);
	if (!equal(shape.targets, shape.outputs))
		result = csd_wire_emit_expression_node(builder, CSD_SQL_OP_PROJECT,
			result, shape.targets, metadata);
	return result;
}

static void
csd_wire_append_sort_payload(CsdWireBuilder *builder, CsdSqlPlanNode *node,
							 List *metadata, const CsdExprSchema *schema)
{
	List *payload = (List *) list_nth(metadata, 7);
	List *clauses = (List *) list_nth(payload, 3);
	List *target_list = (List *) list_nth(payload, 5);
	CsdSqlSortSpec sort = {0};
	ListCell *cell;

	if (clauses == NIL || list_length(clauses) > PG_UINT16_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CSD Sort requires between 1 and 65535 keys")));
	sort.key_count = list_length(clauses);
	node->expression_offset = builder->bytes.len;
	csd_wire_append(&builder->bytes, &sort, sizeof(sort));
	foreach(cell, clauses)
	{
		SortGroupClause *clause = (SortGroupClause *) lfirst(cell);
		Node *expression = get_sortgroupclause_expr(clause, target_list);
		const char *operator_name = get_opname(clause->sortop);
		int key_column = csd_expr_schema_lookup(schema, expression);
		CsdSqlSortKey key = {0};

		if (key_column < 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("CSD sort key is absent from its input schema")));
		key.key_column = key_column;
		if (operator_name != NULL && strcmp(operator_name, ">") == 0)
			key.flags |= CSD_SQL_SORT_DESC;
		else if (operator_name == NULL || strcmp(operator_name, "<") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("CSD sort operator is not a standard ascending/descending comparator")));
		if (clause->nulls_first)
			key.flags |= CSD_SQL_SORT_NULLS_FIRST;
		csd_wire_append(&builder->bytes, &key, sizeof(key));
	}
	node->expression_length = builder->bytes.len - node->expression_offset;
}

static CsdWireResult
csd_wire_emit_node(CsdWireBuilder *builder, List *metadata)
{
	const char *name = strVal(list_nth(metadata, 0));
	int detail = intVal(list_nth(metadata, 1));
	List *children = (List *) list_nth(metadata, 4);
	CsdWireResult child_results[3];
	ListCell *cell;
	CsdSqlPlanNode node = {0};
	CsdWireResult result;
	uint32 child_index = 0;
	uint32 node_id;

	MemSet(child_results, 0, sizeof(child_results));
	MemSet(&result, 0, sizeof(result));
	result.node_id = CSD_SQL_INVALID_NODE;

	foreach(cell, children)
	{
		if (child_index >= lengthof(child_results))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("CSD ABI nodes support at most three inputs")));
		child_results[child_index++] = csd_wire_emit_node(builder,
			(List *) lfirst(cell));
	}
	if (strcmp(name, "Aggregate") == 0 || strcmp(name, "Distinct") == 0)
	{
		if (child_index != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("CSD Aggregate metadata must have one input")));
		return csd_wire_emit_aggregate(builder, metadata, child_results[0]);
	}
	node_id = builder->next_node++;
	node.node_id = node_id;
	node.opcode = csd_wire_opcode(name);
	node.left_input = child_index > 0 ? child_results[0].node_id :
		CSD_SQL_INVALID_NODE;
	node.right_input = child_index > 1 ? child_results[1].node_id :
		CSD_SQL_INVALID_NODE;
	node.aux_input = child_index > 2 ? child_results[2].node_id :
		CSD_SQL_INVALID_NODE;
	node.estimated_rows = csd_wire_estimate(metadata, 5);
	node.estimated_bytes = csd_wire_estimate(metadata, 6);
	node.arg1 = 1;
	if (node.opcode == CSD_SQL_OP_HASH_JOIN ||
		node.opcode == CSD_SQL_OP_HASH_AGG || node.opcode == CSD_SQL_OP_SORT)
		node.flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;
	if (node.opcode == CSD_SQL_OP_SORT)
		node.flags |= CSD_SQL_NODE_REQUIRES_ORDER;
	if (node.opcode == CSD_SQL_OP_LIMIT)
	{
		List *payload = (List *) list_nth(metadata, 7);
		int count_est = intVal(list_nth(payload, 9));

		node.arg0 = count_est < 0 ? PG_UINT64_MAX : (uint64) count_est;
	}
	if (strcmp(name, "Right Hash Join") == 0)
	{
		node.left_input = child_results[1].node_id;
		node.right_input = child_results[0].node_id;
	}
	if (strcmp(name, "Subquery") == 0)
	{
		List *payload = (List *) list_nth(metadata, 7);
		Index subquery_rti = intVal(list_nth(payload, 0));
		List *expressions = (List *) list_nth(payload, 1);
		List *inputs = NIL;
		ListCell *expression_cell;
		uint32 expression_offset;
		uint32 expression_length;

		if (child_index != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("CSD Subquery metadata must have one input")));
		foreach(expression_cell, expressions)
		{
			Node *expression = csd_wire_expression((Node *) lfirst(expression_cell));
			Var *variable;

			if (!IsA(expression, Var))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("CSD Subquery output must be a positional Var")));
			variable = (Var *) expression;
			if (variable->varno != subquery_rti || variable->varattno <= 0 ||
				variable->varattno > child_results[0].schema.count)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("CSD Subquery output column is outside its child schema")));
			inputs = lappend(inputs, child_results[0].schema.columns[
				variable->varattno - 1].expression);
		}
		node.opcode = CSD_SQL_OP_PROJECT;
		csd_append_expression_program(&builder->bytes, inputs,
			&child_results[0].schema, &expression_offset, &expression_length,
			&builder->parameter_count);
		node.expression_offset = expression_offset;
		node.expression_length = expression_length;
		++builder->expression_count;
		csd_expr_schema_from_expressions(&result.schema, expressions);
		csd_expr_schema_release(&child_results[0].schema);
	}
	else if (node.opcode == CSD_SQL_OP_PROJECT)
	{
		List *expressions = (List *) list_nth(metadata, 7);
		uint32 expression_offset;
		uint32 expression_length;

		if (child_index != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("CSD Project metadata must have one input")));
		csd_append_expression_program(&builder->bytes, expressions,
			&child_results[0].schema, &expression_offset, &expression_length,
			&builder->parameter_count);
		node.expression_offset = expression_offset;
		node.expression_length = expression_length;
		++builder->expression_count;
		csd_expr_schema_from_expressions(&result.schema, expressions);
		csd_expr_schema_release(&child_results[0].schema);
	}
	else if (strstr(name, "Join") != NULL)
	{
		const CsdExprSchema *left_schema;
		const CsdExprSchema *right_schema;

		if (child_index != 2)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("CSD Join metadata must have two inputs")));
		left_schema = strcmp(name, "Right Hash Join") == 0 ?
			&child_results[1].schema : &child_results[0].schema;
		right_schema = strcmp(name, "Right Hash Join") == 0 ?
			&child_results[0].schema : &child_results[1].schema;
		csd_wire_append_join_payload(builder, &node, metadata, name,
			left_schema, right_schema);
	}
	else if (node.opcode == CSD_SQL_OP_SORT)
	{
		if (child_index != 1)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("CSD Sort metadata must have one input")));
		csd_wire_append_sort_payload(builder, &node, metadata,
			&child_results[0].schema);
	}
	else
		csd_wire_append_payload(builder, &node, metadata, name, detail);
	csd_wire_store_node(builder, &node);
	if (node.opcode == CSD_SQL_OP_SCAN)
	{
		List *payload = (List *) list_nth(metadata, 7);
		List *targets = (List *) list_nth(payload, 1);
		List *clauses = (List *) list_nth(payload, 2);
		List *lowerable = csd_scan_lowerable_clauses(clauses,
			&builder->pending_expression_count);
		result.node_id = node_id;
		result.schema = csd_wire_scan_schema(metadata, (Index) detail);

		if (lowerable != NIL)
		{
			CsdSqlPlanNode filter = {0};
			uint32 filter_id = builder->next_node++;
			uint32 expression_offset;
			uint32 expression_length;

			filter.node_id = filter_id;
			filter.opcode = CSD_SQL_OP_FILTER;
			filter.left_input = node_id;
			filter.right_input = CSD_SQL_INVALID_NODE;
			filter.aux_input = CSD_SQL_INVALID_NODE;
			filter.estimated_rows = node.estimated_rows;
			filter.estimated_bytes = node.estimated_bytes;
			filter.arg1 = 1;
			csd_append_scan_expression(&builder->bytes, lowerable, &result.schema,
				&expression_offset,
				&expression_length, &builder->parameter_count);
			filter.expression_offset = expression_offset;
			filter.expression_length = expression_length;
			++builder->expression_count;
			memcpy(builder->bytes.data + sizeof(CsdSqlPlanHeader) +
				   filter_id * sizeof(CsdSqlPlanNode), &filter, sizeof(filter));
			result.node_id = filter_id;
		}
		return targets == NIL ? result :
			csd_wire_emit_expression_node(builder, CSD_SQL_OP_PROJECT,
				result, targets, metadata);
	}
	else if (strstr(name, "Join") != NULL)
	{
		List *payload = (List *) list_nth(metadata, 7);
		List *targets = (List *) list_nth(payload, 2);

		if (child_index != 2)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("CSD Join metadata must have two inputs")));
		if (strstr(name, "Semi") != NULL || strstr(name, "Anti") != NULL)
		{
			result.schema = child_results[0].schema;
			MemSet(&child_results[0].schema, 0, sizeof(child_results[0].schema));
		}
		else if (strcmp(name, "Right Hash Join") == 0)
			result.schema = csd_wire_concat_schema(&child_results[1].schema,
				&child_results[0].schema);
		else
			result.schema = csd_wire_concat_schema(&child_results[0].schema,
				&child_results[1].schema);
		csd_expr_schema_release(&child_results[0].schema);
		csd_expr_schema_release(&child_results[1].schema);
		result.node_id = node_id;
		return targets == NIL ? result :
			csd_wire_emit_expression_node(builder, CSD_SQL_OP_PROJECT,
				result, targets, metadata);
	}
	else if (node.opcode != CSD_SQL_OP_PROJECT && child_index == 1)
	{
		result.schema = child_results[0].schema;
		MemSet(&child_results[0].schema, 0, sizeof(child_results[0].schema));
	}
	result.node_id = node_id;
	return result;
}

static void
csd_wire_validate(const StringInfoData *bytes)
{
	CsdSqlPlanHeader header;
	uint64 fixed_bytes;
	uint32 index;

	if (bytes->len < (int) sizeof(header))
		goto invalid;
	memcpy(&header, bytes->data, sizeof(header));
	fixed_bytes = sizeof(header) +
		(uint64) header.node_count * sizeof(CsdSqlPlanNode);
	if (header.magic != CSD_SQL_PLAN_MAGIC ||
		header.version != CSD_SQL_PLAN_VERSION ||
		header.header_bytes != sizeof(header) ||
		header.node_bytes != sizeof(CsdSqlPlanNode) ||
		header.node_count == 0 || header.root_node >= header.node_count ||
		header.plan_bytes != (uint32) bytes->len || fixed_bytes > header.plan_bytes)
		goto invalid;
	for (index = 0; index < header.node_count; ++index)
	{
		CsdSqlPlanNode node;

		memcpy(&node, bytes->data + sizeof(header) +
			   index * sizeof(node), sizeof(node));
		if (node.node_id != index ||
			node.opcode < CSD_SQL_OP_SCAN || node.opcode > CSD_SQL_OP_SUBPLAN ||
			(node.left_input != CSD_SQL_INVALID_NODE && node.left_input >= index) ||
			(node.right_input != CSD_SQL_INVALID_NODE && node.right_input >= index) ||
			(node.aux_input != CSD_SQL_INVALID_NODE && node.aux_input >= index) ||
			node.expression_offset > header.plan_bytes ||
			node.expression_length > header.plan_bytes - node.expression_offset)
			goto invalid;
		if (node.opcode == CSD_SQL_OP_SCAN)
		{
			CsdSqlScanSpec scan;
			uint64 scan_bytes;

			if (node.expression_length < sizeof(scan))
				goto invalid;
			memcpy(&scan, bytes->data + node.expression_offset, sizeof(scan));
			scan_bytes = sizeof(scan) +
				(uint64) scan.attr_count * sizeof(CsdSqlColumnSpec);
			if (scan.attr_count == 0 || scan.output_count == 0 ||
				scan_bytes > node.expression_length)
				goto invalid;
		}
		else if (node.opcode == CSD_SQL_OP_FILTER ||
				 node.opcode == CSD_SQL_OP_PROJECT)
		{
			CsdSqlExpressionSpec expression;
			uint64 expression_bytes;

			if (node.expression_length < sizeof(expression))
				goto invalid;
			memcpy(&expression, bytes->data + node.expression_offset,
				   sizeof(expression));
			expression_bytes = sizeof(expression) +
				(uint64) expression.instruction_count *
				sizeof(struct CsdSqlExpressionInstruction);
			if (expression.output_count == 0 || expression.instruction_count == 0 ||
				expression.max_stack == 0 || expression.max_stack > 256 ||
				expression_bytes > node.expression_length)
				goto invalid;
		}
		else if (node.opcode == CSD_SQL_OP_HASH_AGG)
		{
			CsdSqlAggregateSpec aggregate;
			uint64 descriptor_bytes;

			if (node.expression_length < sizeof(aggregate))
				goto invalid;
			memcpy(&aggregate, bytes->data + node.expression_offset,
				   sizeof(aggregate));
			descriptor_bytes = sizeof(aggregate) +
				(uint64) aggregate.group_count * sizeof(uint16) +
				(uint64) aggregate.aggregate_count *
				sizeof(CsdSqlAggregateColumn);
			if ((aggregate.group_count == 0 && aggregate.aggregate_count == 0) ||
				descriptor_bytes > node.expression_length)
				goto invalid;
		}
		else if (node.opcode == CSD_SQL_OP_SORT)
		{
			CsdSqlSortSpec sort;
			uint64 descriptor_bytes;

			if (node.expression_length < sizeof(sort))
				goto invalid;
			memcpy(&sort, bytes->data + node.expression_offset, sizeof(sort));
			descriptor_bytes = sizeof(sort) +
				(uint64) sort.key_count * sizeof(CsdSqlSortKey);
			if (sort.key_count == 0 || descriptor_bytes > node.expression_length)
				goto invalid;
		}
		else if (node.opcode == CSD_SQL_OP_HASH_JOIN ||
				 node.opcode == CSD_SQL_OP_NL_JOIN ||
				 node.opcode == CSD_SQL_OP_MERGE_JOIN)
		{
			CsdSqlJoinSpec join;
			uint64 descriptor_bytes;

			if (node.expression_length < sizeof(join))
				goto invalid;
			memcpy(&join, bytes->data + node.expression_offset, sizeof(join));
			descriptor_bytes = sizeof(join) +
				(uint64) join.key_count * sizeof(CsdSqlJoinKey);
			if ((node.opcode != CSD_SQL_OP_NL_JOIN && join.key_count == 0) ||
				descriptor_bytes > node.expression_length ||
				join.join_kind < CSD_SQL_JOIN_INNER ||
				join.join_kind > CSD_SQL_JOIN_ANTI)
				goto invalid;
		}
	}
	return;

invalid:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("generated CSD ABI v8 wire plan failed structural validation")));
}

bytea *
csd_lower_wire_plan(List *metadata, uint32 *node_count,
					uint32 *expression_count, uint32 *parameter_count,
					uint32 *pending_expression_count)
{
	CsdWireBuilder builder;
	CsdSqlPlanHeader header = {0};
	CsdWireResult root;
	Size fixed_bytes;
	bytea *result;

	MemSet(&builder, 0, sizeof(builder));
	initStringInfo(&builder.bytes);
	builder.node_count = csd_wire_count_nodes(metadata);
	if (builder.node_count > (MaxAllocSize - sizeof(header)) /
		sizeof(CsdSqlPlanNode))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("CSD wire plan node table exceeds PostgreSQL allocation limit")));
	fixed_bytes = sizeof(header) + builder.node_count * sizeof(CsdSqlPlanNode);
	enlargeStringInfo(&builder.bytes, fixed_bytes);
	MemSet(builder.bytes.data, 0, fixed_bytes);
	builder.bytes.len = fixed_bytes;
	builder.bytes.data[builder.bytes.len] = '\0';
	root = csd_wire_emit_node(&builder, metadata);

	header.magic = CSD_SQL_PLAN_MAGIC;
	header.version = CSD_SQL_PLAN_VERSION;
	header.header_bytes = sizeof(header);
	header.node_count = builder.node_count;
	header.node_bytes = sizeof(CsdSqlPlanNode);
	header.plan_bytes = builder.bytes.len;
	header.root_node = root.node_id;
	header.flags = CSD_SQL_PLAN_STATUS_LAST_RANGE;
	header.output_range = builder.next_input_range;
	header.spill_range = builder.next_input_range + 1;
	header.work_mem_bytes = (uint64) work_mem * 1024;
	memcpy(builder.bytes.data, &header, sizeof(header));
	csd_wire_validate(&builder.bytes);

	result = (bytea *) palloc(VARHDRSZ + builder.bytes.len);
	SET_VARSIZE(result, VARHDRSZ + builder.bytes.len);
	memcpy(VARDATA(result), builder.bytes.data, builder.bytes.len);
	if (node_count != NULL)
		*node_count = builder.node_count;
	if (expression_count != NULL)
		*expression_count = builder.expression_count;
	if (parameter_count != NULL)
		*parameter_count = builder.parameter_count;
	if (pending_expression_count != NULL)
		*pending_expression_count = builder.pending_expression_count;
	csd_expr_schema_release(&root.schema);
	pfree(builder.bytes.data);
	return result;
}
