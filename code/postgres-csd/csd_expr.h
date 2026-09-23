#ifndef CSD_EXPR_H
#define CSD_EXPR_H

#include "postgres.h"

#include "access/tupdesc.h"
#include "lib/stringinfo.h"

extern void csd_append_scan_expression(StringInfo target, List *clauses,
									   Index scan_rti, TupleDesc descriptor,
									   uint32 *offset, uint32 *length,
									   uint32 *parameter_count);
extern List *csd_scan_lowerable_clauses(List *clauses, uint32 *pending_count);

#endif
