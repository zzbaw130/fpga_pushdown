#include "postgres.h"

#include <ctype.h>

#include "catalog/pg_type.h"
#include "datatype/timestamp.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "optimizer/clauses.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"

#include "csd_expr.h"
#include "csd_plan.h"

typedef struct CsdExpressionBuilder
{
	StringInfo target;
	StringInfoData constants;
	struct CsdSqlExpressionInstruction *instructions;
	uint32 instruction_count;
	uint32 instruction_capacity;
	int stack_depth;
	int max_stack;
	const CsdExprSchema *input_schema;
	uint32 parameter_count;
} CsdExpressionBuilder;

static void csd_compile_expression(CsdExpressionBuilder *builder, Node *node);

static Oid
csd_expression_output_type(Node *expression)
{
	if (IsA(expression, FuncExpr))
	{
		const char *name = get_func_name(((FuncExpr *) expression)->funcid);

		if (name != NULL && (strcmp(name, "date_part") == 0 ||
			strcmp(name, "extract") == 0))
			return INT4OID;
	}
	return exprType(expression);
}

static uint8
csd_expression_type(Oid type_oid)
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
					 errmsg("PostgreSQL expression type OID %u has no CSD datum type",
							type_oid)));
	}
	pg_unreachable();
}

static int16
csd_numeric_typmod_scale(int32 typmod)
{
	return typmod >= VARHDRSZ ? (int16) ((typmod - VARHDRSZ) & 0xffff) : -1;
}

static void
csd_expression_stack(CsdExpressionBuilder *builder, int delta)
{
	builder->stack_depth += delta;
	if (builder->stack_depth < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("CSD expression compiler produced a stack underflow")));
	builder->max_stack = Max(builder->max_stack, builder->stack_depth);
	if (builder->max_stack > 256)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("CSD expression stack exceeds 256 entries")));
}

static struct CsdSqlExpressionInstruction *
csd_expression_instruction(CsdExpressionBuilder *builder, uint8 opcode)
{
	struct CsdSqlExpressionInstruction *instruction;

	if (builder->instruction_count == PG_UINT16_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("CSD expression exceeds 65535 instructions")));
	if (builder->instruction_count == builder->instruction_capacity)
	{
		builder->instruction_capacity = builder->instruction_capacity == 0 ? 32 :
			builder->instruction_capacity * 2;
		if (builder->instructions == NULL)
			builder->instructions = palloc0(builder->instruction_capacity *
				sizeof(*builder->instructions));
		else
			builder->instructions = repalloc(builder->instructions,
				builder->instruction_capacity * sizeof(*builder->instructions));
	}
	instruction = &builder->instructions[builder->instruction_count++];
	MemSet(instruction, 0, sizeof(*instruction));
	instruction->opcode = opcode;
	return instruction;
}

static void
csd_expression_unary(CsdExpressionBuilder *builder, uint8 opcode,
					 uint8 datum_type, int16 scale)
{
	struct CsdSqlExpressionInstruction *instruction =
		csd_expression_instruction(builder, opcode);

	if (builder->stack_depth < 1)
		csd_expression_stack(builder, -1);
	instruction->datum_type = datum_type;
	instruction->numeric_scale = scale;
}

static void
csd_expression_binary(CsdExpressionBuilder *builder, uint8 opcode,
					  uint8 datum_type, int16 scale)
{
	struct CsdSqlExpressionInstruction *instruction =
		csd_expression_instruction(builder, opcode);

	if (builder->stack_depth < 2)
		csd_expression_stack(builder, -2);
	instruction->datum_type = datum_type;
	instruction->numeric_scale = scale;
	csd_expression_stack(builder, -1);
}

static int64
csd_numeric_constant(Datum value, int16 *scale)
{
	Oid output_function;
	bool variable_length;
	char *text;
	char *cursor;
	char digits[128];
	int digit_count = 0;
	int fractional = 0;
	bool after_decimal = false;
	int64 result;

	getTypeOutputInfo(NUMERICOID, &output_function, &variable_length);
	text = OidOutputFunctionCall(output_function, value);
	for (cursor = text; *cursor != '\0'; ++cursor)
	{
		if (*cursor == '.')
		{
			if (after_decimal)
				goto invalid;
			after_decimal = true;
			continue;
		}
		if ((cursor == text && (*cursor == '-' || *cursor == '+')) ||
			isdigit((unsigned char) *cursor))
		{
			if (digit_count >= (int) sizeof(digits) - 1)
				goto invalid;
			digits[digit_count++] = *cursor;
			if (after_decimal && isdigit((unsigned char) *cursor))
				++fractional;
			continue;
		}
		goto invalid;
	}
	if (fractional > 18 || digit_count == 0)
		goto invalid;
	digits[digit_count] = '\0';
	*scale = fractional;
	if (!scanint8(digits, true, &result))
		goto invalid;
	return result;

invalid:
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("numeric constant %s cannot be represented by CSD fixed point",
					text)));
	pg_unreachable();
}

int16
csd_expression_numeric_scale(Node *expression)
{
	int32 typmod;

	if (expression == NULL)
		return -1;
	if (IsA(expression, TargetEntry))
		expression = (Node *) ((TargetEntry *) expression)->expr;
	if (exprType(expression) != NUMERICOID)
		return 0;
	typmod = exprTypmod(expression);
	if (typmod >= VARHDRSZ)
		return csd_numeric_typmod_scale(typmod);
	if (IsA(expression, Const))
	{
		Const *constant = (Const *) expression;
		int16 scale = 0;

		if (!constant->constisnull)
			(void) csd_numeric_constant(constant->constvalue, &scale);
		return scale;
	}
	if (IsA(expression, OpExpr))
	{
		OpExpr *operation = (OpExpr *) expression;
		const char *name = get_opname(operation->opno);
		int16 left = csd_expression_numeric_scale(linitial(operation->args));
		int16 right = list_length(operation->args) > 1 ?
			csd_expression_numeric_scale(lsecond(operation->args)) : 0;

		if (left < 0 || right < 0)
			return -1;
		if (name != NULL && strcmp(name, "*") == 0)
			return Min(18, left + right);
		if (name != NULL && strcmp(name, "/") == 0)
			return -1;
		return Max(left, right);
	}
	if (IsA(expression, Aggref))
	{
		Aggref *aggregate = (Aggref *) expression;
		int16 scale = aggregate->args == NIL ? 0 :
			csd_expression_numeric_scale(linitial(aggregate->args));
		const char *name = get_func_name(aggregate->aggfnoid);

		if (scale < 0)
			return -1;
		return name != NULL && strcmp(name, "avg") == 0 ?
			Min(18, Max(6, scale)) : scale;
	}
	if (IsA(expression, RelabelType))
		return csd_expression_numeric_scale((Node *) ((RelabelType *) expression)->arg);
	if (IsA(expression, CoerceViaIO))
		return csd_expression_numeric_scale((Node *) ((CoerceViaIO *) expression)->arg);
	if (IsA(expression, CoerceToDomain))
		return csd_expression_numeric_scale((Node *) ((CoerceToDomain *) expression)->arg);
	if (IsA(expression, FuncExpr) && ((FuncExpr *) expression)->args != NIL)
		return csd_expression_numeric_scale(linitial(((FuncExpr *) expression)->args));
	return -1;
}

static void
csd_compile_constant(CsdExpressionBuilder *builder, Oid type_oid, int32 typmod,
					 Datum value, bool is_null)
{
	struct CsdSqlExpressionInstruction *instruction =
		csd_expression_instruction(builder, CSD_SQL_EXPR_PUSH_CONSTANT);
	uint8 datum_type = csd_expression_type(type_oid);

	instruction->datum_type = datum_type;
	if (is_null)
	{
		instruction->flags |= CSD_SQL_EXPR_CONSTANT_NULL;
		if (datum_type == CSD_SQL_TYPE_NUMERIC)
			instruction->numeric_scale = csd_numeric_typmod_scale(typmod);
	}
	else if (datum_type == CSD_SQL_TYPE_INT16)
		instruction->immediate = DatumGetInt16(value);
	else if (datum_type == CSD_SQL_TYPE_INT32)
		instruction->immediate = DatumGetInt32(value);
	else if (datum_type == CSD_SQL_TYPE_DATE)
	{
		if (type_oid == DATEOID)
			instruction->immediate = DatumGetInt32(value);
		else
		{
			Timestamp timestamp = DatumGetTimestamp(value);

			if (timestamp % USECS_PER_DAY != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("CSD date comparator cannot represent a timestamp with time-of-day")));
			instruction->immediate = timestamp / USECS_PER_DAY;
		}
	}
	else if (datum_type == CSD_SQL_TYPE_INT64)
		instruction->immediate = DatumGetInt64(value);
	else if (datum_type == CSD_SQL_TYPE_BOOL)
		instruction->immediate = DatumGetBool(value) ? 1 : 0;
	else if (datum_type == CSD_SQL_TYPE_NUMERIC)
	{
		int16 scale;

		instruction->immediate = csd_numeric_constant(value, &scale);
		instruction->numeric_scale = scale;
	}
	else
	{
		struct varlena *bytes = PG_DETOAST_DATUM_PACKED(value);
		Size length = VARSIZE_ANY_EXHDR(bytes);

		if (length > PG_UINT32_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("CSD byte constant is too large")));
		instruction->bytes_offset = builder->constants.len;
		instruction->bytes_length = length;
		appendBinaryStringInfo(&builder->constants, VARDATA_ANY(bytes), length);
	}
	csd_expression_stack(builder, 1);
}

int
csd_expr_schema_lookup(const CsdExprSchema *schema, Node *expression)
{
	uint16 column;

	if (schema == NULL)
		return -1;
	for (column = 0; column < schema->count; ++column)
		if (equal(schema->columns[column].expression, expression))
			return column;
	return -1;
}

static uint8
csd_operator_opcode(const char *name)
{
	if (strcmp(name, "+") == 0) return CSD_SQL_EXPR_ADD;
	if (strcmp(name, "-") == 0) return CSD_SQL_EXPR_SUBTRACT;
	if (strcmp(name, "*") == 0) return CSD_SQL_EXPR_MULTIPLY;
	if (strcmp(name, "/") == 0) return CSD_SQL_EXPR_DIVIDE;
	if (strcmp(name, "=") == 0) return CSD_SQL_EXPR_EQUAL;
	if (strcmp(name, "<>") == 0) return CSD_SQL_EXPR_NOT_EQUAL;
	if (strcmp(name, "<") == 0) return CSD_SQL_EXPR_LESS;
	if (strcmp(name, "<=") == 0) return CSD_SQL_EXPR_LESS_EQUAL;
	if (strcmp(name, ">") == 0) return CSD_SQL_EXPR_GREATER;
	if (strcmp(name, ">=") == 0) return CSD_SQL_EXPR_GREATER_EQUAL;
	if (strcmp(name, "~~") == 0 || strcmp(name, "!~~") == 0)
		return CSD_SQL_EXPR_LIKE;
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("operator %s has no CSD bytecode opcode", name)));
	pg_unreachable();
}

static void
csd_compile_operator(CsdExpressionBuilder *builder, OpExpr *expression)
{
	const char *name = get_opname(expression->opno);
	ListCell *cell;
	int argument_count = list_length(expression->args);
	uint8 opcode;
	uint8 datum_type;
	int16 scale;

	if (argument_count == 1 && strcmp(name, "-") == 0)
	{
		csd_compile_expression(builder, linitial(expression->args));
		csd_expression_unary(builder, CSD_SQL_EXPR_NEGATE,
			csd_expression_type(expression->opresulttype),
			csd_numeric_typmod_scale(exprTypmod((Node *) expression)));
		return;
	}
	if (argument_count != 2)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CSD operator %s requires one or two operands", name)));
	foreach(cell, expression->args)
		csd_compile_expression(builder, (Node *) lfirst(cell));
	opcode = csd_operator_opcode(name);
	datum_type = expression->opresulttype == BOOLOID ? CSD_SQL_TYPE_BOOL :
		csd_expression_type(expression->opresulttype);
	scale = expression->opresulttype == NUMERICOID ?
		csd_expression_numeric_scale((Node *) expression) : 0;
	csd_expression_binary(builder, opcode, datum_type, scale);
	if (strcmp(name, "!~~") == 0)
		csd_expression_unary(builder, CSD_SQL_EXPR_NOT, CSD_SQL_TYPE_BOOL, 0);
}

static void
csd_compile_scalar_array(CsdExpressionBuilder *builder,
						ScalarArrayOpExpr *expression)
{
	Node *left = (Node *) linitial(expression->args);
	Node *right = (Node *) lsecond(expression->args);
	const char *name = get_opname(expression->opno);
	uint8 comparison = csd_operator_opcode(name);
	uint8 combine = expression->useOr ? CSD_SQL_EXPR_OR : CSD_SQL_EXPR_AND;
	int elements = 0;

	if (IsA(right, ArrayExpr))
	{
		ListCell *cell;

		foreach(cell, ((ArrayExpr *) right)->elements)
		{
			csd_compile_expression(builder, left);
			csd_compile_expression(builder, (Node *) lfirst(cell));
			csd_expression_binary(builder, comparison, CSD_SQL_TYPE_BOOL, 0);
			if (elements++ > 0)
				csd_expression_binary(builder, combine, CSD_SQL_TYPE_BOOL, 0);
		}
	}
	else if (IsA(right, Const) && !((Const *) right)->constisnull)
	{
		Const *array_constant = (Const *) right;
		ArrayType *array = DatumGetArrayTypeP(array_constant->constvalue);
		Oid element_type = ARR_ELEMTYPE(array);
		int16 element_length;
		bool element_by_value;
		char element_alignment;
		Datum *values;
		bool *nulls;
		int value_count;
		int index;

		get_typlenbyvalalign(element_type, &element_length, &element_by_value,
			&element_alignment);
		deconstruct_array(array, element_type, element_length, element_by_value,
			element_alignment, &values, &nulls, &value_count);
		for (index = 0; index < value_count; ++index)
		{
			csd_compile_expression(builder, left);
			csd_compile_constant(builder, element_type, -1, values[index], nulls[index]);
			csd_expression_binary(builder, comparison, CSD_SQL_TYPE_BOOL, 0);
			if (elements++ > 0)
				csd_expression_binary(builder, combine, CSD_SQL_TYPE_BOOL, 0);
		}
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CSD scalar-array comparison requires a constant array")));
	if (elements == 0)
		csd_compile_constant(builder, BOOLOID, -1,
			BoolGetDatum(!expression->useOr), false);
}

static void
csd_compile_case_from(CsdExpressionBuilder *builder, List *arguments,
					  Node *default_result)
{
	CaseWhen *when;

	if (arguments == NIL)
	{
		csd_compile_expression(builder, default_result);
		return;
	}
	when = castNode(CaseWhen, linitial(arguments));
	csd_compile_expression(builder, (Node *) when->expr);
	csd_compile_expression(builder, (Node *) when->result);
	csd_compile_case_from(builder, list_delete_first(list_copy(arguments)),
		default_result);
	csd_expression_instruction(builder, CSD_SQL_EXPR_SELECT)->datum_type =
		csd_expression_type(exprType((Node *) when->result));
	csd_expression_stack(builder, -2);
}

static void
csd_compile_function(CsdExpressionBuilder *builder, FuncExpr *expression)
{
	const char *name = get_func_name(expression->funcid);
	static const char *const casts[] = {
		"timestamp", "numeric", "int2", "int4", "int8", "float4", "float8",
		"text", "bpchar"
	};
	int index;

	for (index = 0; index < lengthof(casts); ++index)
		if (strcmp(name, casts[index]) == 0)
		{
			if (list_length(expression->args) != 1)
				break;
			csd_compile_expression(builder, linitial(expression->args));
			return;
		}
	if (strcmp(name, "date_part") == 0 || strcmp(name, "extract") == 0)
	{
		if (list_length(expression->args) != 2)
			goto unsupported;
		csd_compile_expression(builder, lsecond(expression->args));
		csd_expression_unary(builder, CSD_SQL_EXPR_EXTRACT_YEAR,
			CSD_SQL_TYPE_INT32, 0);
		return;
	}
	if (strcmp(name, "substring") == 0 || strcmp(name, "substr") == 0)
	{
		Const *start;
		Const *length;
		struct CsdSqlExpressionInstruction *instruction;

		if (list_length(expression->args) != 3 ||
			!IsA(list_nth(expression->args, 1), Const) ||
			!IsA(list_nth(expression->args, 2), Const))
			goto unsupported;
		start = castNode(Const, list_nth(expression->args, 1));
		length = castNode(Const, list_nth(expression->args, 2));
		if (start->constisnull || length->constisnull ||
			start->consttype != INT4OID || length->consttype != INT4OID)
			goto unsupported;
		csd_compile_expression(builder, linitial(expression->args));
		instruction = csd_expression_instruction(builder, CSD_SQL_EXPR_SUBSTRING);
		instruction->datum_type = CSD_SQL_TYPE_BYTES;
		instruction->operand = DatumGetInt32(start->constvalue);
		instruction->bytes_length = DatumGetInt32(length->constvalue);
		return;
	}

unsupported:
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("function %s has no CSD bytecode lowering", name)));
}

static void
csd_compile_expression(CsdExpressionBuilder *builder, Node *node)
{
	int bound_column;

	if (node == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("CSD expression cannot lower a NULL node")));
	bound_column = csd_expr_schema_lookup(builder->input_schema, node);
	if (bound_column >= 0)
	{
		CsdExprColumn *column = &builder->input_schema->columns[bound_column];
		struct CsdSqlExpressionInstruction *instruction =
			csd_expression_instruction(builder, CSD_SQL_EXPR_PUSH_COLUMN);

		instruction->operand = bound_column;
		instruction->datum_type = csd_expression_type(column->type_oid);
		instruction->numeric_scale = column->type_oid == NUMERICOID ?
			csd_numeric_typmod_scale(column->typmod) : 0;
		csd_expression_stack(builder, 1);
	}
	else if (IsA(node, Var))
	{
		List *schema_expressions = NIL;
		uint16 column;

		for (column = 0; column < builder->input_schema->count; ++column)
			schema_expressions = lappend(schema_expressions,
				builder->input_schema->columns[column].expression);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CSD expression references a column absent from its input schema"),
				 errdetail("Unbound expression: %s; input schema: %s",
					nodeToString(node), nodeToString(schema_expressions))));
	}
	else if (IsA(node, Const))
	{
		Const *constant = (Const *) node;

		csd_compile_constant(builder, constant->consttype, constant->consttypmod,
			constant->constvalue, constant->constisnull);
	}
	else if (IsA(node, Param))
	{
		Param *parameter = (Param *) node;
		struct CsdSqlExpressionInstruction *instruction;

		if (parameter->paramid > PG_UINT16_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("CSD parameter ID exceeds ABI operand width")));
		instruction = csd_expression_instruction(builder,
			CSD_SQL_EXPR_PUSH_CONSTANT);
		instruction->datum_type = csd_expression_type(parameter->paramtype);
		instruction->numeric_scale = parameter->paramtype == NUMERICOID ?
			csd_numeric_typmod_scale(parameter->paramtypmod) : 0;
		instruction->operand = parameter->paramid;
		instruction->flags = CSD_SQL_EXPR_CONSTANT_NULL |
			CSD_SQL_EXPR_PARAMETER_PLACEHOLDER;
		++builder->parameter_count;
		csd_expression_stack(builder, 1);
	}
	else if (IsA(node, OpExpr))
		csd_compile_operator(builder, (OpExpr *) node);
	else if (IsA(node, ScalarArrayOpExpr))
		csd_compile_scalar_array(builder, (ScalarArrayOpExpr *) node);
	else if (IsA(node, BoolExpr))
	{
		BoolExpr *boolean = (BoolExpr *) node;
		ListCell *cell;
		int compiled = 0;

		foreach(cell, boolean->args)
		{
			csd_compile_expression(builder, (Node *) lfirst(cell));
			if (compiled++ > 0)
				csd_expression_binary(builder,
					boolean->boolop == AND_EXPR ? CSD_SQL_EXPR_AND : CSD_SQL_EXPR_OR,
					CSD_SQL_TYPE_BOOL, 0);
		}
		if (boolean->boolop == NOT_EXPR)
			csd_expression_unary(builder, CSD_SQL_EXPR_NOT, CSD_SQL_TYPE_BOOL, 0);
	}
	else if (IsA(node, NullTest))
	{
		NullTest *test = (NullTest *) node;

		csd_compile_expression(builder, (Node *) test->arg);
		csd_expression_unary(builder,
			test->nulltesttype == IS_NULL ? CSD_SQL_EXPR_IS_NULL :
			CSD_SQL_EXPR_IS_NOT_NULL, CSD_SQL_TYPE_BOOL, 0);
	}
	else if (IsA(node, CaseExpr))
	{
		CaseExpr *case_expression = (CaseExpr *) node;

		if (case_expression->arg != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("simple CASE requires preprocessing before CSD lowering")));
		csd_compile_case_from(builder, case_expression->args,
			(Node *) case_expression->defresult);
	}
	else if (IsA(node, FuncExpr))
		csd_compile_function(builder, (FuncExpr *) node);
	else if (IsA(node, RelabelType))
		csd_compile_expression(builder, (Node *) ((RelabelType *) node)->arg);
	else if (IsA(node, CoerceViaIO))
		csd_compile_expression(builder, (Node *) ((CoerceViaIO *) node)->arg);
	else if (IsA(node, CoerceToDomain))
		csd_compile_expression(builder, (Node *) ((CoerceToDomain *) node)->arg);
	else if (IsA(node, CollateExpr))
		csd_compile_expression(builder, (Node *) ((CollateExpr *) node)->arg);
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("expression node tag %d has no CSD bytecode lowering",
						(int) nodeTag(node)),
				 errdetail("Unbound expression: %s", nodeToString(node))));
}

void
csd_append_scan_expression(StringInfo target, List *clauses, Index scan_rti,
						   TupleDesc descriptor, uint32 *offset, uint32 *length,
						   uint32 *parameter_count)
{
	CsdExpressionBuilder builder;
	struct CsdSqlExpressionSpec spec = {0};
	ListCell *cell;
	uint32 compiled = 0;
	uint32 instruction_offset;
	uint32 constants_offset;
	uint32 index;
	CsdExprSchema scan_schema;

	MemSet(&builder, 0, sizeof(builder));
	builder.target = target;
	csd_expr_schema_from_scan(&scan_schema, scan_rti, descriptor);
	builder.input_schema = &scan_schema;
	initStringInfo(&builder.constants);
	foreach(cell, clauses)
	{
		csd_compile_expression(&builder, (Node *) lfirst(cell));
		if (compiled++ > 0)
			csd_expression_binary(&builder, CSD_SQL_EXPR_AND,
				CSD_SQL_TYPE_BOOL, 0);
	}
	if (compiled == 0)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("cannot emit an empty CSD filter expression")));
	csd_expression_instruction(&builder, CSD_SQL_EXPR_EMIT)->datum_type =
		CSD_SQL_TYPE_BOOL;
	csd_expression_stack(&builder, -1);
	if (builder.stack_depth != 0 || builder.max_stack == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("CSD expression compiler left an invalid stack depth")));

	*offset = target->len;
	spec.output_count = 1;
	spec.instruction_count = builder.instruction_count;
	spec.max_stack = builder.max_stack;
	appendBinaryStringInfo(target, (const char *) &spec, sizeof(spec));
	instruction_offset = target->len;
	constants_offset = instruction_offset + builder.instruction_count *
		sizeof(*builder.instructions);
	for (index = 0; index < builder.instruction_count; ++index)
		if (builder.instructions[index].bytes_length != 0 &&
			builder.instructions[index].opcode == CSD_SQL_EXPR_PUSH_CONSTANT)
			builder.instructions[index].bytes_offset += constants_offset;
	appendBinaryStringInfo(target, (const char *) builder.instructions,
		builder.instruction_count * sizeof(*builder.instructions));
	appendBinaryStringInfo(target, builder.constants.data, builder.constants.len);
	*length = target->len - *offset;
	if (parameter_count != NULL)
		*parameter_count += builder.parameter_count;
	pfree(builder.instructions);
	pfree(builder.constants.data);
	csd_expr_schema_release(&scan_schema);
}

void
csd_append_expression_program(StringInfo target, List *expressions,
							  const CsdExprSchema *input_schema,
							  uint32 *offset, uint32 *length,
							  uint32 *parameter_count)
{
	CsdExpressionBuilder builder;
	struct CsdSqlExpressionSpec spec = {0};
	ListCell *cell;
	uint32 instruction_offset;
	uint32 constants_offset;
	uint32 index;

	if (expressions == NIL || list_length(expressions) > PG_UINT16_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("CSD expression program has an invalid output count")));
	MemSet(&builder, 0, sizeof(builder));
	builder.target = target;
	builder.input_schema = input_schema;
	initStringInfo(&builder.constants);
	foreach(cell, expressions)
	{
		Node *expression = (Node *) lfirst(cell);

		if (IsA(expression, TargetEntry))
			expression = (Node *) ((TargetEntry *) expression)->expr;
		csd_compile_expression(&builder, expression);
		csd_expression_instruction(&builder, CSD_SQL_EXPR_EMIT)->datum_type =
			csd_expression_type(csd_expression_output_type(expression));
		csd_expression_stack(&builder, -1);
	}
	if (builder.stack_depth != 0 || builder.max_stack == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("CSD expression compiler left an invalid stack depth")));
	*offset = target->len;
	spec.output_count = list_length(expressions);
	spec.instruction_count = builder.instruction_count;
	spec.max_stack = builder.max_stack;
	appendBinaryStringInfo(target, (const char *) &spec, sizeof(spec));
	instruction_offset = target->len;
	constants_offset = instruction_offset + builder.instruction_count *
		sizeof(*builder.instructions);
	for (index = 0; index < builder.instruction_count; ++index)
		if (builder.instructions[index].bytes_length != 0 &&
			builder.instructions[index].opcode == CSD_SQL_EXPR_PUSH_CONSTANT)
			builder.instructions[index].bytes_offset += constants_offset;
	appendBinaryStringInfo(target, (const char *) builder.instructions,
		builder.instruction_count * sizeof(*builder.instructions));
	appendBinaryStringInfo(target, builder.constants.data, builder.constants.len);
	*length = target->len - *offset;
	if (parameter_count != NULL)
		*parameter_count += builder.parameter_count;
	pfree(builder.instructions);
	pfree(builder.constants.data);
}

void
csd_expr_schema_from_scan(CsdExprSchema *schema, Index scan_rti,
						  TupleDesc descriptor)
{
	uint16 count = 0;
	int attribute;

	MemSet(schema, 0, sizeof(*schema));
	for (attribute = 0; attribute < descriptor->natts; ++attribute)
		if (!TupleDescAttr(descriptor, attribute)->attisdropped)
			++count;
	schema->count = count;
	schema->columns = palloc0(count * sizeof(*schema->columns));
	count = 0;
	for (attribute = 0; attribute < descriptor->natts; ++attribute)
	{
		Form_pg_attribute pg_attribute = TupleDescAttr(descriptor, attribute);

		if (pg_attribute->attisdropped)
			continue;
		schema->columns[count].expression = (Node *) makeVar(scan_rti,
			attribute + 1, pg_attribute->atttypid, pg_attribute->atttypmod,
			pg_attribute->attcollation, 0);
		schema->columns[count].type_oid = pg_attribute->atttypid;
		schema->columns[count].typmod = pg_attribute->atttypmod;
		++count;
	}
}

void
csd_expr_schema_from_expressions(CsdExprSchema *schema, List *expressions)
{
	ListCell *cell;
	uint16 count = 0;

	if (list_length(expressions) > PG_UINT16_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("CSD schema exceeds 65535 columns")));
	MemSet(schema, 0, sizeof(*schema));
	schema->count = list_length(expressions);
	schema->columns = palloc0(schema->count * sizeof(*schema->columns));
	foreach(cell, expressions)
	{
		Node *expression = (Node *) lfirst(cell);

		if (IsA(expression, TargetEntry))
			expression = (Node *) ((TargetEntry *) expression)->expr;
		schema->columns[count].expression = expression;
		schema->columns[count].type_oid = csd_expression_output_type(expression);
		schema->columns[count].typmod = exprTypmod(expression);
		if (schema->columns[count].type_oid == NUMERICOID &&
			schema->columns[count].typmod < VARHDRSZ)
		{
			int16 scale = csd_expression_numeric_scale(expression);

			if (scale >= 0)
				schema->columns[count].typmod = VARHDRSZ + scale;
		}
		++count;
	}
}

void
csd_expr_schema_release(CsdExprSchema *schema)
{
	if (schema->columns != NULL)
		pfree(schema->columns);
	MemSet(schema, 0, sizeof(*schema));
}

List *
csd_scan_lowerable_clauses(List *clauses, uint32 *pending_count)
{
	List *lowerable = NIL;
	ListCell *cell;

	foreach(cell, clauses)
	{
		Node *clause = (Node *) lfirst(cell);

		/* A hashed SubPlan must become a CSD semi/anti join, not scalar bytecode. */
		if (contain_subplans(clause))
		{
			if (pending_count != NULL)
				++*pending_count;
			continue;
		}
		lowerable = lappend(lowerable, clause);
	}
	return lowerable;
}
