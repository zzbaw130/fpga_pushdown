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

/*
 * Copyable planner metadata shared by costing, lowering, and execution.
 * The first eight fields are the original ABI-v8 planner representation;
 * safe rows/bytes are planner-side resource bounds and are ignored by the
 * wire encoder.
 */
typedef enum CsdMetadataIndex
{
	CSD_META_NAME = 0,
	CSD_META_DETAIL,
	CSD_META_DEVICE_COST,
	CSD_META_CACHE_FRACTION,
	CSD_META_CHILDREN,
	CSD_META_EXPECTED_ROWS,
	CSD_META_EXPECTED_BYTES,
	CSD_META_PAYLOAD,
	CSD_META_SAFE_ROWS,
	CSD_META_SAFE_BYTES,
	CSD_META_FIELD_COUNT
} CsdMetadataIndex;

#define CSD_SCAN_WIRE_PLAN_INDEX CSD_META_FIELD_COUNT
#define CSD_SCAN_WIRE_NODES_INDEX (CSD_SCAN_WIRE_PLAN_INDEX + 1)
#define CSD_SCAN_WIRE_EXPRESSIONS_INDEX (CSD_SCAN_WIRE_PLAN_INDEX + 2)
#define CSD_SCAN_WIRE_PARAMETERS_INDEX (CSD_SCAN_WIRE_PLAN_INDEX + 3)
#define CSD_SCAN_PENDING_EXPRESSIONS_INDEX (CSD_SCAN_WIRE_PLAN_INDEX + 4)
#define CSD_SCAN_SUBPLAN_REWRITES_INDEX (CSD_SCAN_WIRE_PLAN_INDEX + 5)

extern int csd_exe_path;
extern double csd_setup_cost;
extern double csd_flash_page_cost;
extern double csd_tuple_cost;
extern double csd_join_tuple_cost;
extern double csd_aggregate_tuple_cost;
extern double csd_sort_compare_cost;
extern double csd_result_byte_cost;
extern double csd_host_cache_fraction;
extern double csd_host_cached_page_cost;
extern double csd_spill_page_cost;
extern double csd_stats_safety_factor;

extern const CustomPathMethods csd_path_methods;
extern const CustomScanMethods csd_scan_methods;

extern void csd_cost_reset_paths(void);
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
extern List *csd_rewrite_subplans(PlannerInfo *root, List *metadata,
								  uint32 *rewrite_count);
extern bytea *csd_lower_wire_plan(List *metadata, uint32 *node_count,
								  uint32 *expression_count,
								  uint32 *parameter_count,
								  uint32 *pending_expression_count);

#endif
