#include "postgres.h"

#include <math.h>

#include "access/table.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/value.h"
#include "storage/bufmgr.h"
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
typedef struct CsdSqlAggregateSpec CsdSqlAggregateSpec;
typedef struct CsdSqlSortSpec CsdSqlSortSpec;

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

static uint32 csd_wire_count_nodes(List *metadata);
static uint32 csd_wire_emit_node(CsdWireBuilder *builder, List *metadata);
static void csd_wire_validate(const StringInfoData *bytes);

static uint8
csd_wire_datum_type(Oid type_oid)
{
	switch (type_oid)
	{
		case INT2OID: return CSD_SQL_TYPE_INT16;
		case INT4OID: return CSD_SQL_TYPE_INT32;
		case INT8OID: return CSD_SQL_TYPE_INT64;
		case DATEOID: return CSD_SQL_TYPE_DATE;
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
			 errmsg("CSD metadata operator %s has no ABI v4 opcode", name)));
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
		List *clauses = (List *) list_nth(payload, 2);
		List *lowerable = csd_scan_lowerable_clauses(clauses, NULL);

		if (lowerable != NIL)
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
		scan.attr_count = descriptor->natts;
		for (attribute = 0; attribute < descriptor->natts; ++attribute)
			if (!TupleDescAttr(descriptor, attribute)->attisdropped)
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
			if (pg_attribute->atttypid == NUMERICOID && pg_attribute->atttypmod >= VARHDRSZ)
				column.numeric_scale = (pg_attribute->atttypmod - VARHDRSZ) & 0xffff;
			if (!pg_attribute->attisdropped)
			{
				column.output_column = output_column++;
				column.flags = CSD_SQL_COLUMN_OUTPUT;
				if (pg_attribute->attlen == -1)
					column.flags |= CSD_SQL_COLUMN_NO_EXTERNAL;
			}
			else
				column.output_column = CSD_SQL_NO_COLUMN;
			csd_wire_append(&builder->bytes, &column, sizeof(column));
		}
		table_close(relation, NoLock);
	}
	else if (node->opcode == CSD_SQL_OP_PROJECT)
	{
		CsdSqlExpressionSpec expression = {0};

		csd_wire_append(&builder->bytes, &expression, sizeof(expression));
	}
	else if (node->opcode == CSD_SQL_OP_HASH_JOIN)
	{
		CsdSqlJoinSpec join = {0};

		if (strstr(name, "Left") != NULL || strstr(name, "Right") != NULL)
			join.join_kind = CSD_SQL_JOIN_LEFT;
		else if (strstr(name, "Semi") != NULL)
			join.join_kind = CSD_SQL_JOIN_SEMI;
		else if (strstr(name, "Anti") != NULL)
			join.join_kind = CSD_SQL_JOIN_ANTI;
		else
			join.join_kind = CSD_SQL_JOIN_INNER;
		csd_wire_append(&builder->bytes, &join, sizeof(join));
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

static uint32
csd_wire_emit_node(CsdWireBuilder *builder, List *metadata)
{
	const char *name = strVal(list_nth(metadata, 0));
	int detail = intVal(list_nth(metadata, 1));
	List *children = (List *) list_nth(metadata, 4);
	uint32 inputs[3] = {
		CSD_SQL_INVALID_NODE, CSD_SQL_INVALID_NODE, CSD_SQL_INVALID_NODE
	};
	ListCell *cell;
	CsdSqlPlanNode node = {0};
	uint32 child_index = 0;
	uint32 node_id;

	foreach(cell, children)
	{
		if (child_index >= lengthof(inputs))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("CSD ABI nodes support at most three inputs")));
		inputs[child_index++] = csd_wire_emit_node(builder,
			(List *) lfirst(cell));
	}
	node_id = builder->next_node++;
	node.node_id = node_id;
	node.opcode = csd_wire_opcode(name);
	node.left_input = inputs[0];
	node.right_input = inputs[1];
	node.aux_input = inputs[2];
	node.estimated_rows = csd_wire_estimate(metadata, 5);
	node.estimated_bytes = csd_wire_estimate(metadata, 6);
	node.arg1 = 1;
	if (node.opcode == CSD_SQL_OP_HASH_JOIN ||
		node.opcode == CSD_SQL_OP_HASH_AGG || node.opcode == CSD_SQL_OP_SORT)
		node.flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;
	if (node.opcode == CSD_SQL_OP_SORT)
		node.flags |= CSD_SQL_NODE_REQUIRES_ORDER;
	if (strcmp(name, "Right Hash Join") == 0)
	{
		node.left_input = inputs[1];
		node.right_input = inputs[0];
	}
	csd_wire_append_payload(builder, &node, metadata, name, detail);
	memcpy(builder->bytes.data + sizeof(CsdSqlPlanHeader) +
		   node_id * sizeof(CsdSqlPlanNode), &node, sizeof(node));
	if (node.opcode == CSD_SQL_OP_SCAN)
	{
		List *payload = (List *) list_nth(metadata, 7);
		List *clauses = (List *) list_nth(payload, 2);
		List *lowerable = csd_scan_lowerable_clauses(clauses,
			&builder->pending_expression_count);

		if (lowerable != NIL)
		{
			Oid relation_oid = (Oid) strtoul(strVal(list_nth(payload, 0)), NULL, 10);
			Relation relation = table_open(relation_oid, NoLock);
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
			csd_append_scan_expression(&builder->bytes, lowerable, (Index) detail,
				RelationGetDescr(relation), &expression_offset,
				&expression_length, &builder->parameter_count);
			filter.expression_offset = expression_offset;
			filter.expression_length = expression_length;
			table_close(relation, NoLock);
			++builder->expression_count;
			memcpy(builder->bytes.data + sizeof(CsdSqlPlanHeader) +
				   filter_id * sizeof(CsdSqlPlanNode), &filter, sizeof(filter));
			return filter_id;
		}
	}
	return node_id;
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
		else if (node.opcode == CSD_SQL_OP_FILTER)
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
	}
	return;

invalid:
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("generated CSD ABI v4 wire plan failed structural validation")));
}

bytea *
csd_lower_wire_plan(List *metadata, uint32 *node_count,
					uint32 *expression_count, uint32 *parameter_count,
					uint32 *pending_expression_count)
{
	CsdWireBuilder builder;
	CsdSqlPlanHeader header = {0};
	uint32 root_node;
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
	root_node = csd_wire_emit_node(&builder, metadata);

	header.magic = CSD_SQL_PLAN_MAGIC;
	header.version = CSD_SQL_PLAN_VERSION;
	header.header_bytes = sizeof(header);
	header.node_count = builder.node_count;
	header.node_bytes = sizeof(CsdSqlPlanNode);
	header.plan_bytes = builder.bytes.len;
	header.root_node = root_node;
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
	pfree(builder.bytes.data);
	return result;
}
