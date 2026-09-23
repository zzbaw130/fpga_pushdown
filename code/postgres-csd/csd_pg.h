#ifndef CSD_PG_H
#define CSD_PG_H

#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/pathnodes.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"

typedef enum CsdExePath
{
	CSD_EXE_HOST = 0,
	CSD_EXE_CSD,
	CSD_EXE_AUTO
} CsdExePath;

extern int csd_exe_path;
extern double csd_setup_cost;
extern double csd_flash_page_cost;
extern double csd_tuple_cost;
extern double csd_join_tuple_cost;
extern double csd_aggregate_tuple_cost;
extern double csd_sort_compare_cost;
extern double csd_result_byte_cost;
extern double csd_host_cache_fraction;

extern const CustomPathMethods csd_path_methods;
extern const CustomScanMethods csd_scan_methods;

extern void csd_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
								Index rti, RangeTblEntry *rte);
extern void csd_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
								 RelOptInfo *outerrel, RelOptInfo *innerrel,
								 JoinType jointype, JoinPathExtraData *extra);
extern void csd_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
								  RelOptInfo *input_rel, RelOptInfo *output_rel,
								  void *extra);
extern bool csd_expression_supported(Node *node, const char **reason);
extern bool csd_expression_list_supported(List *expressions, const char **reason);
extern const char *csd_upper_stage_name(UpperRelationKind stage);
extern bytea *csd_lower_wire_plan(List *metadata, uint32 *node_count);

#endif
