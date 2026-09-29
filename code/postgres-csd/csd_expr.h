#ifndef CSD_EXPR_H
#define CSD_EXPR_H

#include "postgres.h"

#include "access/tupdesc.h"
#include "lib/stringinfo.h"

typedef struct CsdExprColumn
{
	Node *expression;
	Oid type_oid;
	int32 typmod;
} CsdExprColumn;

typedef struct CsdExprSchema
{
	uint16 count;
	CsdExprColumn *columns;
} CsdExprSchema;

extern void csd_expr_schema_from_scan(CsdExprSchema *schema, Index scan_rti,
									  TupleDesc descriptor,
									  const Bitmapset *required_attrs);
extern void csd_expr_schema_from_expressions(CsdExprSchema *schema,
										 List *expressions);
extern int csd_expr_schema_lookup(const CsdExprSchema *schema, Node *expression);
extern void csd_expr_schema_release(CsdExprSchema *schema);
extern int16 csd_expression_numeric_scale(Node *expression);
extern void csd_append_expression_program(StringInfo target, List *expressions,
										  const CsdExprSchema *input_schema,
										  uint32 *offset, uint32 *length,
										  uint32 *parameter_count);
extern void csd_append_scan_expression(StringInfo target, List *clauses,
									   const CsdExprSchema *scan_schema,
									   uint32 *offset, uint32 *length,
									   uint32 *parameter_count);
extern List *csd_scan_lowerable_clauses(List *clauses, uint32 *pending_count);

#endif
