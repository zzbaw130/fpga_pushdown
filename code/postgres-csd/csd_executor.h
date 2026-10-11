#ifndef CSD_EXECUTOR_H
#define CSD_EXECUTOR_H

#include "postgres.h"

#include "executor/nodeCustom.h"

extern Node *csd_executor_create_state(CustomScan *scan);
extern void csd_executor_begin(CustomScanState *node, EState *estate, int eflags);
extern TupleTableSlot *csd_executor_next(CustomScanState *node);
extern void csd_executor_end(CustomScanState *node);
extern void csd_executor_rescan(CustomScanState *node);

extern char *csd_control_device;
extern char *csd_execute_device;
extern char *csd_fdm_path;
extern char *csd_shared_library;
extern int csd_output_mb;
extern int csd_spill_mb;
extern int csd_spill_stage_mb;
extern int csd_chunk_mb;
extern int csd_fifo_mb;
extern int csd_fdm_capacity_mb;
extern int csd_nvm_nsid;
extern int csd_parallel_workers;
extern int csd_program_runtime;
extern int csd_execute_timeout_ms;
extern int csd_device_work_mem_mb;
extern int csd_cache_wait_ms;
extern bool csd_profile;
extern bool csd_cache_fill;
extern bool csd_cache_overlap;

#endif
