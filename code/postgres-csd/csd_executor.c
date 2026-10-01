#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/nvme_ioctl.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "access/table.h"
#include "catalog/pg_type.h"
#include "common/relpath.h"
#include "executor/executor.h"
#include "executor/nodeCustom.h"
#include "executor/nodeSubplan.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/value.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/float.h"
#include "utils/int8.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

#include "cemu_ioctl.h"
#include "csd_plan.h"
#include "csd_executor.h"

typedef struct CsdSqlPlanHeader CsdSqlPlanHeader;
typedef struct CsdSqlPlanNode CsdSqlPlanNode;
typedef struct CsdSqlScanSpec CsdSqlScanSpec;
typedef struct CsdSqlNvmExtent CsdSqlNvmExtent;
typedef struct CsdSqlExpressionSpec CsdSqlExpressionSpec;
typedef struct CsdSqlExpressionInstruction CsdSqlExpressionInstruction;
typedef struct CsdSqlJoinSpec CsdSqlJoinSpec;
typedef struct CsdSqlExecutionStatus CsdSqlExecutionStatus;
typedef struct CsdSqlResultHeader CsdSqlResultHeader;
typedef struct CsdSqlResultCell CsdSqlResultCell;

#define CSD_PROGRAM_TYPE_SHARED_LIB 2
#define CSD_RESULT_NULL UINT8_C(1)
#define CSD_RANGE_ALIGNMENT UINT64_C(8192)

typedef struct CsdNvmeProgramExecuteCmd
{
	uint8 opcode;
	uint8 flags;
	uint16 cid;
	uint32 nsid;
	uint16 pind;
	uint16 rsid;
	uint32 numr;
	uint32 dlen;
	uint32 reserved;
	uint64 prp1;
	uint64 prp2;
	uint64 cparam1;
	uint64 cparam2;
	uint32 group : 8;
	uint32 chunk_nlb : 24;
	uint32 user_runtime;
} CsdNvmeProgramExecuteCmd;

typedef struct CsdExecutorState
{
	CustomScanState custom;
	char *plan_data;
	Size plan_length;
	List *relations;
	char *result_data;
	Size result_length;
	uint32 next_row;
	bool explain_only;
	bool executed;
} CsdExecutorState;

typedef struct CsdTransport
{
	int lock_fd;
	int control_fd;
	int execute_fd;
	int spill_fd;
	char *spill_path;
	int *range_fds;
	long long *range_sizes;
	int range_count;
	struct ioctl_download download;
	struct ioctl_create_mrs mrs;
	bool downloaded;
	bool activated;
	bool mrs_created;
} CsdTransport;

char *csd_control_device = NULL;
char *csd_execute_device = NULL;
char *csd_fdm_path = NULL;
char *csd_shared_library = NULL;
int csd_output_mb = 64;
int csd_spill_mb = 4096;
int csd_spill_stage_mb = 32;
int csd_chunk_mb = 32;
int csd_fifo_mb = 32;
int csd_fdm_capacity_mb = 4096;
int csd_nvm_nsid = 1;
int csd_parallel_workers = 32;
int csd_program_runtime = 0;
int csd_device_work_mem_mb = 0;
bool csd_profile = false;

static uint64
csd_align_range(uint64 bytes)
{
	if (bytes > PG_UINT64_MAX - (CSD_RANGE_ALIGNMENT - 1))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("CSD range size overflow")));
	return (bytes + CSD_RANGE_ALIGNMENT - 1) & ~(CSD_RANGE_ALIGNMENT - 1);
}

static void
csd_account_fdm_range(uint64 *total, uint64 bytes)
{
	uint64 aligned = csd_align_range(Max(bytes, CSD_RANGE_ALIGNMENT));

	if (*total > PG_UINT64_MAX - aligned)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("CSD FDM reservation size overflow")));
	*total += aligned;
}

static void
csd_transport_init(CsdTransport *transport)
{
	MemSet(transport, 0, sizeof(*transport));
	transport->lock_fd = -1;
	transport->control_fd = -1;
	transport->execute_fd = -1;
	transport->spill_fd = -1;
}

static void
csd_transport_cleanup(CsdTransport *transport)
{
	int index;

	if (transport->control_fd >= 0 && transport->mrs_created)
		(void) ioctl(transport->control_fd, IOCTL_CEMU_DELETE_MRS,
					 &transport->mrs);
	if (transport->control_fd >= 0 && transport->activated)
		(void) ioctl(transport->control_fd, IOCTL_CEMU_DEACTIVATE,
					 &transport->download);
	if (transport->control_fd >= 0 && transport->downloaded)
		(void) ioctl(transport->control_fd, IOCTL_CEMU_UNLOAD,
					 &transport->download);
	if (transport->spill_fd >= 0)
		close(transport->spill_fd);
	if (transport->spill_path != NULL)
	{
		(void) unlink(transport->spill_path);
		pfree(transport->spill_path);
	}
	for (index = 0; index < transport->range_count; ++index)
		if (transport->range_fds != NULL && transport->range_fds[index] >= 0)
		{
			int truncate_result = ftruncate(transport->range_fds[index], 0);

			(void) truncate_result;
			close(transport->range_fds[index]);
		}
	/* FDM ranges are query-local bounded memory windows. Leaving high-numbered files
	 * behind after a many-relation query can starve the next, smaller plan even
	 * though it recreates all of its own low-numbered ranges. */
	for (index = 0; index < transport->range_count; ++index)
	{
		char path[MAXPGPATH];

		snprintf(path, sizeof(path), "%s/%d", csd_fdm_path, index);
		(void) unlink(path);
	}
	if (transport->execute_fd >= 0)
		close(transport->execute_fd);
	if (transport->control_fd >= 0)
		close(transport->control_fd);
	if (transport->lock_fd >= 0)
	{
		(void) flock(transport->lock_fd, LOCK_UN);
		close(transport->lock_fd);
	}
	if (transport->download.addr != NULL)
		free(transport->download.addr);
	if (transport->range_fds != NULL)
		pfree(transport->range_fds);
	if (transport->range_sizes != NULL)
		pfree(transport->range_sizes);
	if (transport->mrs.fd != NULL)
		pfree(transport->mrs.fd);
	if (transport->mrs.off != NULL)
		pfree(transport->mrs.off);
	if (transport->mrs.size != NULL)
		pfree(transport->mrs.size);
	csd_transport_init(transport);
}

static List *
csd_collect_relations(List *metadata, List *relations)
{
	List *children = (List *) list_nth(metadata, 4);
	ListCell *cell;

	foreach(cell, children)
		relations = csd_collect_relations((List *) lfirst(cell), relations);
	if (strcmp(strVal(list_nth(metadata, 0)), "Scan") == 0)
	{
		List *payload = (List *) list_nth(metadata, 7);
		Oid relation_oid = (Oid) strtoul(strVal(list_nth(payload, 0)), NULL, 10);

		relations = lappend_oid(relations, relation_oid);
	}
	return relations;
}

static void
csd_append_extent(StringInfo extents, uint64 logical_offset,
				  uint64 nvm_offset, uint64 length, uint32 flags)
{
	CsdSqlNvmExtent extent;

	if (extents->len >= (int) sizeof(extent))
	{
		CsdSqlNvmExtent *previous = (CsdSqlNvmExtent *)
			(extents->data + extents->len - sizeof(extent));

		if (previous->logical_offset + previous->length == logical_offset &&
			previous->nvm_offset + previous->length == nvm_offset &&
			previous->nsid == (uint32) csd_nvm_nsid)
		{
			previous->length += length;
			previous->flags |= flags;
			return;
		}
	}
	extent.logical_offset = logical_offset;
	extent.nvm_offset = nvm_offset;
	extent.length = length;
	extent.nsid = csd_nvm_nsid;
	extent.flags = flags;
	appendBinaryStringInfo(extents, (const char *) &extent, sizeof(extent));
}

/*
 * Translate a PostgreSQL heap file into physical offsets in the emulated NVM
 * namespace.  PGDATA is mounted directly from /dev/nvme0n1, so FIEMAP's
 * physical offsets use the same byte-address space as CEMU's backend.
 */
static void
csd_append_segment_extents(StringInfo extents, Oid relation_oid,
						   const char *path, uint64 logical_base,
						   uint64 expected_bytes, bool writable_spill)
{
	struct fiemap query;
	struct fiemap *map;
	struct stat stat_buffer;
	Size map_bytes;
	uint64 cursor = 0;
	uint32 index;
	int fd = open(path, (writable_spill ? O_RDWR : O_RDONLY) | PG_BINARY, 0);

	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open CSD heap segment \"%s\": %m", path)));
	if (fstat(fd, &stat_buffer) != 0 || stat_buffer.st_size < 0 ||
		(uint64) stat_buffer.st_size < expected_bytes)
	{
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("CSD heap segment \"%s\" is shorter than expected", path)));
	}
	MemSet(&query, 0, sizeof(query));
	query.fm_start = 0;
	query.fm_length = expected_bytes;
	query.fm_flags = FIEMAP_FLAG_SYNC;
	if (ioctl(fd, FS_IOC_FIEMAP, &query) != 0 || query.fm_mapped_extents == 0)
	{
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not map CSD heap segment \"%s\": %m", path)));
	}
	if (query.fm_mapped_extents > (MaxAllocSize - sizeof(*map)) /
		sizeof(struct fiemap_extent))
	{
		close(fd);
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("relation %u has too many physical extents", relation_oid)));
	}
	map_bytes = sizeof(*map) +
		(Size) query.fm_mapped_extents * sizeof(struct fiemap_extent);
	map = palloc0(map_bytes);
	map->fm_start = 0;
	map->fm_length = expected_bytes;
	map->fm_flags = FIEMAP_FLAG_SYNC;
	map->fm_extent_count = query.fm_mapped_extents;
	if (ioctl(fd, FS_IOC_FIEMAP, map) != 0)
	{
		int saved_errno = errno;

		pfree(map);
		close(fd);
		errno = saved_errno;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read CSD heap extents for \"%s\": %m", path)));
	}
	close(fd);
	for (index = 0; index < map->fm_mapped_extents && cursor < expected_bytes; ++index)
	{
		const struct fiemap_extent *physical = &map->fm_extents[index];
		const uint32 invalid_flags = FIEMAP_EXTENT_UNKNOWN |
			FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_ENCODED |
			FIEMAP_EXTENT_DATA_ENCRYPTED | FIEMAP_EXTENT_NOT_ALIGNED |
			FIEMAP_EXTENT_DATA_INLINE;
		uint64 extent_start = physical->fe_logical;
		uint64 extent_end;
		uint64 copy_start;
		uint64 copy_end;

		if (physical->fe_length > PG_UINT64_MAX - extent_start)
			break;
		extent_end = extent_start + physical->fe_length;
		if (extent_end <= cursor)
			continue;
		if (extent_start > cursor || (physical->fe_flags & invalid_flags) != 0 ||
			(!writable_spill &&
			 (physical->fe_flags & FIEMAP_EXTENT_UNWRITTEN) != 0))
			break;
		copy_start = cursor;
		copy_end = Min(extent_end, expected_bytes);
		if (((logical_base + copy_start) & 511) != 0 ||
			((physical->fe_physical + copy_start - extent_start) & 511) != 0 ||
			((copy_end - copy_start) & 511) != 0)
			break;
		csd_append_extent(extents, logical_base + copy_start,
			physical->fe_physical + copy_start - extent_start,
			copy_end - copy_start, physical->fe_flags);
		cursor = copy_end;
	}
	pfree(map);
	if (cursor != expected_bytes)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("CSD %s contains a hole or unsupported physical extent at byte %llu",
					writable_spill ? "spill file" : "relation",
					(unsigned long long) (logical_base + cursor)),
				 errdetail("Path: %s, relation OID: %u", path, relation_oid),
				 errhint("Ensure PGDATA resides directly on the configured CEMU NVM namespace.")));
}

static uint64
csd_collect_relation_extents(Oid relation_oid, StringInfo extents)
{
	Relation relation = table_open(relation_oid, NoLock);
	char *relative = relpathbackend(relation->rd_node, relation->rd_backend,
								 MAIN_FORKNUM);
	char *base = psprintf("%s/%s", DataDir, relative);
	uint64 relation_bytes = (uint64) RelationGetNumberOfBlocks(relation) * BLCKSZ;
	uint64 logical_base = 0;
	int segment = 0;

	while (logical_base < relation_bytes)
	{
		char *path = segment == 0 ? pstrdup(base) : psprintf("%s.%d", base, segment);
		uint64 segment_bytes = Min(relation_bytes - logical_base,
			(uint64) RELSEG_SIZE * BLCKSZ);

		csd_append_segment_extents(extents, relation_oid, path, logical_base,
			segment_bytes, false);
		logical_base += segment_bytes;
		++segment;
		pfree(path);
	}
	pfree(base);
	pfree(relative);
	table_close(relation, NoLock);
	return relation_bytes;
}

/*
 * Reserve filesystem-owned blocks on the PGDATA NVMe namespace, then expose
 * their physical extents to the CSD.  The CSD reads and writes these blocks
 * through the internal NVM service; the guest never treats their contents as
 * a PostgreSQL file.  Keeping the file open pins the allocation for the whole
 * Program Execute operation, and cleanup unlinks it afterwards.
 */
static void
csd_create_flash_spill(CsdTransport *transport, uint64 spill_bytes,
					   uint64 plan_id, StringInfo extents)
{
	int allocation_error;

	transport->spill_path = psprintf("%s/pg_csd_spill_%d_%llu.tmp", DataDir,
		MyProcPid, (unsigned long long) plan_id);
	if (unlink(transport->spill_path) != 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not recycle CSD Flash spill file \"%s\": %m",
					transport->spill_path)));
	transport->spill_fd = open(transport->spill_path,
		O_CREAT | O_EXCL | O_RDWR | PG_BINARY, 0600);
	if (transport->spill_fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create CSD Flash spill file \"%s\": %m",
					transport->spill_path)));
	allocation_error = posix_fallocate(transport->spill_fd, 0,
		(off_t) spill_bytes);
	if (allocation_error != 0)
	{
		errno = allocation_error;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not reserve %llu bytes of CSD Flash spill space: %m",
					(unsigned long long) spill_bytes),
				 errhint("Reduce csd.spill_mb or free space on the PGDATA NVMe namespace.")));
	}
	csd_append_segment_extents(extents, InvalidOid, transport->spill_path,
		0, spill_bytes, true);
}

static int
csd_try_create_range(uint32 range, uint64 bytes, int *allocation_error)
{
	char path[MAXPGPATH];
	uint64 allocated = csd_align_range(Max(bytes, CSD_RANGE_ALIGNMENT));
	int fd;

	snprintf(path, sizeof(path), "%s/%u", csd_fdm_path, range);
	if (unlink(path) != 0 && errno != ENOENT)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not recycle CSD range \"%s\": %m", path),
				 errhint("Make the FDMFS mount directory writable by the PostgreSQL OS user.")));
	fd = open(path, O_CREAT | O_EXCL | O_RDWR | PG_BINARY, 0600);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create CSD range \"%s\": %m", path)));
	{
		int allocate_error = posix_fallocate(fd, 0, (off_t) allocated);

		if (allocate_error != 0)
		{
			int truncate_result = ftruncate(fd, 0);

			(void) truncate_result;
			close(fd);
			(void) unlink(path);
			*allocation_error = allocate_error;
			return -1;
		}
	}
	*allocation_error = 0;
	return fd;
}

static int
csd_create_range(uint32 range, uint64 bytes)
{
	char path[MAXPGPATH];
	int allocation_error;
	int fd = csd_try_create_range(range, bytes, &allocation_error);

	if (fd >= 0)
		return fd;
	snprintf(path, sizeof(path), "%s/%u", csd_fdm_path, range);
	errno = allocation_error;
	ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not allocate %llu bytes for CSD range \"%s\": %m",
					(unsigned long long) csd_align_range(bytes), path)));
	pg_unreachable();
}

static int64
csd_numeric_to_scaled(Datum datum, int16 *scale)
{
	Oid output_function;
	bool variable_length;
	char *text;
	char *cursor;
	char digits[128];
	int digit_count = 0;
	int fractional = 0;
	bool after_decimal = false;
	int64 value;

	getTypeOutputInfo(NUMERICOID, &output_function, &variable_length);
	text = OidOutputFunctionCall(output_function, datum);
	for (cursor = text; *cursor != '\0'; ++cursor)
	{
		if (*cursor == '.')
		{
			after_decimal = true;
			continue;
		}
		if ((*cursor == '-' || *cursor == '+') && cursor == text)
			digits[digit_count++] = *cursor;
		else if (*cursor >= '0' && *cursor <= '9')
		{
			digits[digit_count++] = *cursor;
			if (after_decimal)
				++fractional;
		}
		else
			goto invalid;
		if (digit_count >= (int) sizeof(digits) - 1)
			goto invalid;
	}
	if (*scale < 0)
		*scale = fractional;
	if (*scale > 36)
		goto invalid;
	while (fractional < *scale)
	{
		digits[digit_count++] = '0';
		++fractional;
		if (digit_count >= (int) sizeof(digits) - 1)
			goto invalid;
	}
	while (fractional > *scale)
	{
		if (digit_count == 0 || digits[digit_count - 1] != '0')
			goto invalid;
		--digit_count;
		--fractional;
	}
	digits[digit_count] = '\0';
	if (!scanint8(digits, true, &value))
		goto invalid;
	pfree(text);
	return value;

invalid:
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("CSD parameter %s cannot be represented at scale %d", text, *scale)));
	pg_unreachable();
}

static void
csd_patch_expression(char *plan, uint32 plan_bytes, uint32 offset, uint32 length,
					 EState *estate)
{
	CsdSqlExpressionSpec *spec;
	CsdSqlExpressionInstruction *instructions;
	uint32 needed;
	uint16 index;

	if (length == 0)
		return;
	if (offset > plan_bytes || length > plan_bytes - offset ||
		length < sizeof(CsdSqlExpressionSpec))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid CSD expression relocation range")));
	spec = (CsdSqlExpressionSpec *) (plan + offset);
	needed = sizeof(*spec) + spec->instruction_count * sizeof(*instructions);
	if (needed > length)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("truncated CSD expression program")));
	instructions = (CsdSqlExpressionInstruction *) (spec + 1);
	for (index = 0; index < spec->instruction_count; ++index)
	{
		CsdSqlExpressionInstruction *instruction = &instructions[index];
		ParamExecData *parameter;

		if (!(instruction->flags & CSD_SQL_EXPR_PARAMETER_PLACEHOLDER))
			continue;
		if (instruction->operand >= list_length(estate->es_plannedstmt->paramExecTypes))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("CSD parameter ID %u is outside executor storage",
							instruction->operand)));
		parameter = &estate->es_param_exec_vals[instruction->operand];
		if (parameter->execPlan != NULL)
			ExecSetParamPlan(parameter->execPlan, GetPerTupleExprContext(estate));
		instruction->flags &= ~(CSD_SQL_EXPR_PARAMETER_PLACEHOLDER |
			CSD_SQL_EXPR_CONSTANT_NULL);
		if (parameter->isnull)
		{
			instruction->flags |= CSD_SQL_EXPR_CONSTANT_NULL;
			continue;
		}
		switch (instruction->datum_type)
		{
			case CSD_SQL_TYPE_INT16:
				instruction->immediate = DatumGetInt16(parameter->value);
				break;
			case CSD_SQL_TYPE_INT32:
			case CSD_SQL_TYPE_DATE:
				instruction->immediate = DatumGetInt32(parameter->value);
				break;
			case CSD_SQL_TYPE_INT64:
				instruction->immediate = DatumGetInt64(parameter->value);
				break;
			case CSD_SQL_TYPE_BOOL:
				instruction->immediate = DatumGetBool(parameter->value) ? 1 : 0;
				break;
			case CSD_SQL_TYPE_NUMERIC:
			{
				int16 scale = instruction->numeric_scale;

				instruction->immediate = csd_numeric_to_scaled(parameter->value,
					&scale);
				instruction->numeric_scale = scale;
				break;
			}
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("CSD runtime byte parameters are not yet supported")));
		}
	}
}

static void
csd_patch_plan(char **plan_pointer, Size *plan_length_pointer, EState *estate,
			   List *relations)
{
	char *plan = *plan_pointer;
	Size plan_length = *plan_length_pointer;
	CsdSqlPlanHeader *header = (CsdSqlPlanHeader *) plan;
	CsdSqlPlanNode *nodes;
	ListCell *relation_cell = list_head(relations);
	uint32 index;
	uint64 chunk_bytes = (uint64) csd_chunk_mb * 1024 * 1024;

	if (plan_length < sizeof(*header) || header->plan_bytes != plan_length ||
		header->node_count == 0 ||
		header->header_bytes + (uint64) header->node_count * sizeof(*nodes) > plan_length)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid staged CSD plan")));
	if (chunk_bytes == 0 || chunk_bytes > PG_UINT32_MAX ||
		(chunk_bytes % BLCKSZ) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("csd.chunk_mb must describe a nonzero, page-aligned buffer no larger than 4 GB")));
	header->flags |= CSD_SQL_PLAN_STATUS_LAST_RANGE |
		CSD_SQL_PLAN_FROZEN_SNAPSHOT | CSD_SQL_PLAN_STAGED_RANGE |
		CSD_SQL_PLAN_CHUNKED_SCAN;
	header->parallel_workers = csd_parallel_workers;
	header->plan_id = ((uint64) MyProcPid << 32) ^ (uint64) GetCurrentTimestamp();
	if (csd_device_work_mem_mb > 0)
		header->work_mem_bytes = (uint64) csd_device_work_mem_mb * 1024 * 1024;
	nodes = (CsdSqlPlanNode *) (plan + header->header_bytes);
	for (index = 0; index < header->node_count; ++index)
	{
		CsdSqlPlanNode *node = &nodes[index];

		if (node->opcode == CSD_SQL_OP_SCAN || node->opcode == CSD_SQL_OP_INDEX_SCAN)
		{
			CsdSqlScanSpec *scan;
			StringInfoData extents;
			Oid relation_oid;
			uint32 expression_offset = node->expression_offset;
			uint64 relation_bytes;
			uint32 extent_count;
			Size old_length;

			if (node->expression_offset > plan_length ||
				node->expression_length < sizeof(*scan) ||
				node->expression_length > plan_length - node->expression_offset)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid CSD scan descriptor")));
			scan = (CsdSqlScanSpec *) (plan + node->expression_offset);
			if (relation_cell == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("CSD plan has more scan nodes than relation metadata")));
			relation_oid = lfirst_oid(relation_cell);
			relation_cell = lnext(relation_cell);
			if (scan->relation_oid != relation_oid)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("CSD scan relation %u does not match executor relation %u",
								scan->relation_oid, relation_oid)));
			initStringInfo(&extents);
			relation_bytes = csd_collect_relation_extents(relation_oid, &extents);
			if (extents.len % sizeof(CsdSqlNvmExtent) != 0 ||
				(uint64) extents.len / sizeof(CsdSqlNvmExtent) > PG_UINT32_MAX ||
				plan_length > PG_UINT32_MAX - (uint32) extents.len)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("CSD physical extent plan exceeds the wire ABI limit")));
			extent_count = extents.len / sizeof(CsdSqlNvmExtent);
			old_length = plan_length;
			if (extents.len > 0)
			{
				plan_length += extents.len;
				plan = repalloc(plan, plan_length);
				memcpy(plan + old_length, extents.data, extents.len);
			}
			pfree(extents.data);
			header = (CsdSqlPlanHeader *) plan;
			header->plan_bytes = plan_length;
			nodes = (CsdSqlPlanNode *) (plan + header->header_bytes);
			node = &nodes[index];
			scan = (CsdSqlScanSpec *) (plan + expression_offset);
			scan->flags |= CSD_SQL_SCAN_FROZEN_PAGES;
			scan->extent_offset = old_length;
			scan->extent_count = extent_count;
			scan->chunk_bytes = chunk_bytes;
			scan->relation_bytes = relation_bytes;
			if (relation_bytes != (uint64) scan->page_count * BLCKSZ)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("relation %u changed size while building the CSD plan",
								relation_oid)));
		}
		if (node->opcode == CSD_SQL_OP_FILTER || node->opcode == CSD_SQL_OP_PROJECT)
			csd_patch_expression(plan, plan_length, node->expression_offset,
				node->expression_length, estate);
		else if (node->opcode == CSD_SQL_OP_HASH_JOIN ||
				 node->opcode == CSD_SQL_OP_MERGE_JOIN ||
				 node->opcode == CSD_SQL_OP_NL_JOIN)
		{
			CsdSqlJoinSpec *join = (CsdSqlJoinSpec *) (plan + node->expression_offset);

			if (node->expression_length >= sizeof(*join) && join->residual_length > 0)
				csd_patch_expression(plan, plan_length, join->residual_offset,
					join->residual_length, estate);
		}
	}
	if (relation_cell != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("CSD relation metadata contains entries without scan nodes")));
	*plan_pointer = plan;
	*plan_length_pointer = plan_length;
}

static void
csd_write_range(int fd, const void *data, Size length)
{
	char *aligned;
	uint64 write_length = csd_align_range(length);
	ssize_t written;

	if (posix_memalign((void **) &aligned, CSD_RANGE_ALIGNMENT, write_length) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not allocate aligned CSD staging buffer")));
	MemSet(aligned, 0, write_length);
	memcpy(aligned, data, length);
	written = pwrite(fd, aligned, write_length, 0);
	free(aligned);
	if (written != (ssize_t) write_length)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write staged CSD range: %m")));
}

static void
csd_read_range(int fd, void *target, Size length, const char *description)
{
	char *aligned;
	uint64 read_length = csd_align_range(length);
	ssize_t bytes;

	if (posix_memalign((void **) &aligned, CSD_RANGE_ALIGNMENT, read_length) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not allocate aligned CSD read buffer")));
	bytes = pread(fd, aligned, read_length, 0);
	if (bytes != (ssize_t) read_length)
	{
		int saved_errno = errno;

		free(aligned);
		errno = saved_errno;
		ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not read %s: %m", description)));
	}
	memcpy(target, aligned, length);
	free(aligned);
}

static void
csd_run_transport(CsdExecutorState *state, List *relations)
{
	char *plan = state->plan_data;
	Size plan_length = state->plan_length;
	CsdSqlPlanHeader *header = (CsdSqlPlanHeader *) plan;
	CsdTransport transport;
	uint32 relation_count = list_length(relations);
	uint32 fifo_range_a = relation_count;
	uint32 fifo_range_b = fifo_range_a + 1;
	uint32 output_range = fifo_range_b + 1;
	uint32 spill_range = output_range + 1;
	uint32 operator_range = spill_range + 1;
	uint32 plan_range = operator_range + 1;
	uint32 status_range = plan_range + 1;
	uint64 output_bytes = (uint64) csd_output_mb * 1024 * 1024;
	uint64 spill_bytes = (uint64) csd_spill_mb * 1024 * 1024;
	uint64 spill_stage_bytes = (uint64) csd_spill_stage_mb * 1024 * 1024;
	uint64 chunk_bytes = (uint64) csd_chunk_mb * 1024 * 1024;
	uint64 fifo_bytes = (uint64) csd_fifo_mb * 1024 * 1024;
	uint64 fdm_capacity_bytes = (uint64) csd_fdm_capacity_mb * 1024 * 1024;
	uint64 operator_limit_bytes = (uint64) csd_device_work_mem_mb * 1024 * 1024;
	uint64 operator_bytes = 0;
	uint64 reserved_fdm_bytes = 0;
	char *lock_path = pstrdup("/tmp/csd_executor.lock");
	char program_payload[PATH_MAX + 32];
	CsdNvmeProgramExecuteCmd *execute_command;
	struct nvme_passthru_cmd command;
	CsdSqlExecutionStatus status;
	CsdSqlResultHeader result;
	uint64 total_result_bytes;
	uint32 index = 0;

	csd_transport_init(&transport);
	PG_TRY();
	{
		StringInfoData spill_extents;
		Size old_plan_length;

		transport.lock_fd = open(lock_path, O_CREAT | O_RDWR | PG_BINARY, 0600);
		if (transport.lock_fd < 0 || flock(transport.lock_fd, LOCK_EX) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not lock CSD FDM workspace: %m")));
		/* Pin a query-local file allocation on the NVMe namespace and append its
		 * physical layout to the already patched execution plan.  This capacity
		 * is Flash, not FDM, so it is deliberately absent from the memory budget. */
		initStringInfo(&spill_extents);
		csd_create_flash_spill(&transport, spill_bytes, header->plan_id,
			&spill_extents);
		if (spill_extents.len == 0 ||
			spill_extents.len % sizeof(CsdSqlNvmExtent) != 0 ||
			(uint64) spill_extents.len / sizeof(CsdSqlNvmExtent) > PG_UINT32_MAX ||
			plan_length > PG_UINT32_MAX - (uint32) spill_extents.len)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("CSD Flash spill extent plan exceeds the wire ABI limit")));
		old_plan_length = plan_length;
		plan_length += spill_extents.len;
		plan = repalloc(plan, plan_length);
		memcpy(plan + old_plan_length, spill_extents.data, spill_extents.len);
		pfree(spill_extents.data);
		state->plan_data = plan;
		state->plan_length = plan_length;
		header = (CsdSqlPlanHeader *) plan;
		header->plan_bytes = plan_length;
		header->flags |= CSD_SQL_PLAN_FLASH_SPILL;
		header->spill_extent_offset = old_plan_length;
		header->spill_extent_count =
			(plan_length - old_plan_length) / sizeof(CsdSqlNvmExtent);
		header->spill_bytes = spill_bytes;

		transport.range_count = status_range + 1;
		transport.range_fds = palloc(sizeof(int) * transport.range_count);
		transport.range_sizes = palloc0(sizeof(long long) * transport.range_count);
		for (index = 0; index < (uint32) transport.range_count; ++index)
			transport.range_fds[index] = -1;
		for (index = 0; index < relation_count; ++index)
			csd_account_fdm_range(&reserved_fdm_bytes, chunk_bytes);
		csd_account_fdm_range(&reserved_fdm_bytes, fifo_bytes);
		csd_account_fdm_range(&reserved_fdm_bytes, fifo_bytes);
		csd_account_fdm_range(&reserved_fdm_bytes, output_bytes);
		csd_account_fdm_range(&reserved_fdm_bytes, spill_stage_bytes);
		csd_account_fdm_range(&reserved_fdm_bytes, plan_length);
		csd_account_fdm_range(&reserved_fdm_bytes, CSD_RANGE_ALIGNMENT);
		if (reserved_fdm_bytes >= fdm_capacity_bytes)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("CSD fixed ranges require %llu bytes of FDM but csd.fdm_capacity_mb allows %llu bytes",
							(unsigned long long) reserved_fdm_bytes,
							(unsigned long long) fdm_capacity_bytes),
					 errhint("Reduce csd.chunk_mb, csd.fifo_mb, csd.output_mb, or csd.spill_stage_mb.")));
		operator_bytes = (fdm_capacity_bytes - reserved_fdm_bytes) &
			~(CSD_RANGE_ALIGNMENT - 1);
		if (operator_limit_bytes != 0)
			operator_bytes = Min(operator_bytes,
				operator_limit_bytes & ~(CSD_RANGE_ALIGNMENT - 1));
		/* CEMU's nvme_memory_range length is uint32 even though the ioctl
		 * wrapper accepts a long long.  Keep every individual MRS entry
		 * representable; larger FDM pools can be partitioned in a later ABI. */
		operator_bytes = Min(operator_bytes,
			((uint64) PG_UINT32_MAX) & ~(CSD_RANGE_ALIGNMENT - 1));
		if (operator_bytes < CSD_RANGE_ALIGNMENT)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("CSD query has no page-aligned FDM capacity for blocking operators"),
					 errhint("Increase csd.fdm_capacity_mb or raise/remove the csd.device_work_mem_mb cap.")));
		csd_account_fdm_range(&reserved_fdm_bytes, operator_bytes);
		/* Each logical scan owns one reusable bounded work window.  Relation
		 * bytes arrive from NVM extents inside Program Execute, so no heap is
		 * copied wholesale into FDMFS here. */
		for (index = 0; index < relation_count; ++index)
		{
			transport.range_sizes[index] = chunk_bytes;
			transport.range_fds[index] = csd_create_range(index, chunk_bytes);
		}
		transport.range_sizes[fifo_range_a] = fifo_bytes;
		transport.range_fds[fifo_range_a] = csd_create_range(fifo_range_a,
			fifo_bytes);
		transport.range_sizes[fifo_range_b] = fifo_bytes;
		transport.range_fds[fifo_range_b] = csd_create_range(fifo_range_b,
			fifo_bytes);
		transport.range_sizes[output_range] = output_bytes;
		transport.range_fds[output_range] = csd_create_range(output_range, output_bytes);
		transport.range_fds[spill_range] = csd_create_range(spill_range,
			spill_stage_bytes);
		transport.range_sizes[spill_range] = spill_stage_bytes;
		transport.range_sizes[operator_range] = operator_bytes;
		transport.range_fds[operator_range] = csd_create_range(operator_range,
			operator_bytes);
		transport.range_sizes[plan_range] = csd_align_range(plan_length);
		transport.range_fds[plan_range] = csd_create_range(plan_range, plan_length);
		transport.range_sizes[status_range] = CSD_RANGE_ALIGNMENT;
		transport.range_fds[status_range] = csd_create_range(status_range,
			CSD_RANGE_ALIGNMENT);

		header->output_range = output_range;
		header->spill_range = spill_range;
		header->fifo_range_a = fifo_range_a;
		header->fifo_range_b = fifo_range_b;
		header->fifo_bytes = fifo_bytes;
		header->operator_range = operator_range;
		header->operator_bytes = operator_bytes;
		header->work_mem_bytes = operator_bytes;
		header->fdm_reserved_bytes = reserved_fdm_bytes;
		header->flags |= CSD_SQL_PLAN_FDM_FIFO | CSD_SQL_PLAN_FDM_ARENA;
		if (csd_profile)
		{
			header->flags |= CSD_SQL_PLAN_PROFILE;
			ereport(NOTICE,
					(errmsg("CSD profile enabled for plan %llu (flags=%u)",
							(unsigned long long) header->plan_id, header->flags)));
		}
		csd_write_range(transport.range_fds[plan_range], plan, plan_length);

		transport.control_fd = open(csd_control_device, O_RDWR | PG_BINARY, 0);
		transport.execute_fd = open(csd_execute_device, O_RDWR | PG_BINARY, 0);
		if (transport.control_fd < 0 || transport.execute_fd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open CEMU control/execute device: %m")));

		MemSet(&transport.download, 0, sizeof(transport.download));
		transport.download.name = "csd_sql";
		if (strlen(csd_shared_library) + sizeof("csd_sql") + 1 > sizeof(program_payload))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("CSD shared-library path is too long")));
		strcpy(program_payload, csd_shared_library);
		strcpy(program_payload + strlen(csd_shared_library) + 1, "csd_sql");
		if (posix_memalign(&transport.download.addr, 4096, 4096) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not allocate CEMU program descriptor")));
		MemSet(transport.download.addr, 0, 4096);
		memcpy(transport.download.addr, program_payload,
			strlen(csd_shared_library) + sizeof("csd_sql") + 1);
		transport.download.size = strlen(csd_shared_library) + sizeof("csd_sql") + 1;
		transport.download.ptype = CSD_PROGRAM_TYPE_SHARED_LIB;
		if (ioctl(transport.control_fd, IOCTL_CEMU_DOWNLOAD,
				  &transport.download) <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_IO_ERROR),
					 errmsg("CEMU could not download csd_sql: %m")));
		transport.downloaded = true;
		if (ioctl(transport.control_fd, IOCTL_CEMU_ACTIVATE,
				  &transport.download) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_IO_ERROR),
					 errmsg("CEMU could not activate csd_sql: %m")));
		transport.activated = true;

		MemSet(&transport.mrs, 0, sizeof(transport.mrs));
		transport.mrs.nr_fd = transport.range_count;
		transport.mrs.fd = palloc(sizeof(int) * transport.range_count);
		transport.mrs.off = palloc0(sizeof(long long) * transport.range_count);
		transport.mrs.size = palloc(sizeof(long long) * transport.range_count);
		for (index = 0; index < (uint32) transport.range_count; ++index)
		{
			transport.mrs.fd[index] = transport.range_fds[index];
			transport.mrs.size[index] = transport.range_sizes[index];
		}
		if (ioctl(transport.control_fd, IOCTL_CEMU_CREATE_MRS,
				  &transport.mrs) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_IO_ERROR),
					 errmsg("CEMU could not create SQL memory ranges: %m")));
		transport.mrs_created = true;

		MemSet(&command, 0, sizeof(command));
		execute_command = (CsdNvmeProgramExecuteCmd *) &command;
		execute_command->opcode = 0x01;
		execute_command->nsid = 3;
		execute_command->pind = transport.download.pind;
		execute_command->rsid = transport.mrs.rsid;
		execute_command->cparam1 = plan_length;
		execute_command->cparam2 = plan_range;
		execute_command->user_runtime = csd_program_runtime;
		if (ioctl(transport.execute_fd, NVME_IOCTL_IO_CMD, &command) < 0)
			ereport(ERROR,
					(errcode(ERRCODE_IO_ERROR),
					 errmsg("CEMU Program Execute failed: %m"),
					 errhint("The PostgreSQL executable needs CAP_SYS_ADMIN in the CEMU guest.")));

		csd_read_range(transport.range_fds[status_range], &status, sizeof(status),
			"CSD execution status");
		if (status.magic != CSD_SQL_PLAN_MAGIC || status.version != CSD_SQL_PLAN_VERSION ||
			status.status_code != CSD_SQL_STATUS_OK)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("CSD SQL execution failed with status %u at node %u (detail %u)",
							status.status_code, status.failed_node, status.reserved32)));
		if (csd_profile)
		{
			uint32 opcode;

			ereport(NOTICE,
					(errmsg("CSD profile status scan_calls=%llu scan_ms=%.3f flash_read_bytes=%llu flash_write_bytes=%llu input_chunks=%llu spill_write_bytes=%llu spill_read_bytes=%llu fifo_batches=%llu fifo_write_bytes=%llu fifo_read_bytes=%llu fdm_reserved_bytes=%llu operator_memory_bytes=%llu operator_peak_bytes=%llu operator_allocations=%llu operator_spill_count=%llu",
							(unsigned long long) status.operator_calls[CSD_SQL_OP_SCAN],
							(double) status.operator_ns[CSD_SQL_OP_SCAN] / 1000000.0,
							(unsigned long long) status.flash_read_bytes,
							(unsigned long long) status.flash_write_bytes,
							(unsigned long long) status.input_chunks,
							(unsigned long long) status.spill_write_bytes,
							(unsigned long long) status.spill_read_bytes,
							(unsigned long long) status.fifo_batches,
							(unsigned long long) status.fifo_write_bytes,
							(unsigned long long) status.fifo_read_bytes,
							(unsigned long long) status.fdm_reserved_bytes,
							(unsigned long long) status.operator_memory_bytes,
							(unsigned long long) status.operator_peak_bytes,
							(unsigned long long) status.operator_allocations,
							(unsigned long long) status.operator_spill_count)));

			for (opcode = CSD_SQL_OP_SCAN; opcode < CSD_SQL_PROFILE_SLOTS; ++opcode)
				if (status.operator_calls[opcode] != 0)
					ereport(NOTICE,
							(errmsg("CSD profile opcode=%u calls=%llu rows=%llu elapsed_ms=%.3f",
									opcode,
									(unsigned long long) status.operator_calls[opcode],
									(unsigned long long) status.operator_rows[opcode],
									(double) status.operator_ns[opcode] / 1000000.0)));
		}
		csd_read_range(transport.range_fds[output_range], &result, sizeof(result),
			"CSD result header");
		if (result.magic != CSD_SQL_RESULT_MAGIC || result.version != CSD_SQL_PLAN_VERSION ||
			result.root_node != header->root_node ||
			result.cells_offset < sizeof(result) || result.cells_offset > output_bytes ||
			result.cell_count > (output_bytes - result.cells_offset) / sizeof(CsdSqlResultCell) ||
			result.bytes_offset < result.cells_offset +
				result.cell_count * sizeof(CsdSqlResultCell) ||
			result.bytes_offset > output_bytes ||
			result.bytes_length > output_bytes - result.bytes_offset)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("CSD returned an invalid result descriptor")));
		total_result_bytes = result.bytes_offset + result.bytes_length;
		if (total_result_bytes > MaxAllocSize)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("CSD result exceeds PostgreSQL allocation limit")));
		state->result_data = palloc(total_result_bytes);
		state->result_length = total_result_bytes;
		csd_read_range(transport.range_fds[output_range], state->result_data,
			total_result_bytes, "CSD result rows");
	}
	PG_CATCH();
	{
		csd_transport_cleanup(&transport);
		pfree(lock_path);
		PG_RE_THROW();
	}
	PG_END_TRY();
	csd_transport_cleanup(&transport);
	pfree(lock_path);
}

static Datum
csd_decode_numeric(int64 integer, int16 scale, int32 typmod)
{
	char digits[64];
	char text[96];
	bool negative = integer < 0;
	uint64 magnitude = negative ? (uint64) (-(integer + 1)) + 1 : (uint64) integer;
	int digits_length;

	snprintf(digits, sizeof(digits), "%llu", (unsigned long long) magnitude);
	digits_length = strlen(digits);
	if (scale <= 0)
		snprintf(text, sizeof(text), "%s%s", negative ? "-" : "", digits);
	else if (digits_length > scale)
		snprintf(text, sizeof(text), "%s%.*s.%s", negative ? "-" : "",
			digits_length - scale, digits, digits + digits_length - scale);
	else
		snprintf(text, sizeof(text), "%s0.%0*llu", negative ? "-" : "", scale,
			(unsigned long long) magnitude);
	return DirectFunctionCall3(numeric_in, CStringGetDatum(text),
		ObjectIdGetDatum(InvalidOid), Int32GetDatum(typmod));
}

static Datum
csd_decode_cell(const CsdSqlResultCell *cell, const CsdSqlResultHeader *result,
				const char *result_data, Form_pg_attribute attribute)
{
	if (cell->datum_type == CSD_SQL_TYPE_BYTES)
	{
		Oid input_function;
		Oid type_io_parameter;
		char *text;

		if ((uint64) cell->bytes_offset + cell->bytes_length > result->bytes_length)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("CSD byte result is outside its payload")));
		text = pnstrdup(result_data + result->bytes_offset + cell->bytes_offset,
			cell->bytes_length);
		getTypeInputInfo(attribute->atttypid, &input_function, &type_io_parameter);
		return OidInputFunctionCall(input_function, text, type_io_parameter,
			attribute->atttypmod);
	}
	switch (attribute->atttypid)
	{
		case INT2OID: return Int16GetDatum((int16) cell->integer);
		case INT4OID: return Int32GetDatum((int32) cell->integer);
		case INT8OID: return Int64GetDatum(cell->integer);
		case FLOAT4OID:
			return Float4GetDatum((float4) (cell->datum_type == CSD_SQL_TYPE_NUMERIC ?
				((double) cell->integer / pow(10.0, cell->numeric_scale)) :
				(double) cell->integer));
		case FLOAT8OID:
			return Float8GetDatum(cell->datum_type == CSD_SQL_TYPE_NUMERIC ?
				((double) cell->integer / pow(10.0, cell->numeric_scale)) :
				(double) cell->integer);
		case DATEOID: return DateADTGetDatum((DateADT) cell->integer);
		case TIMESTAMPOID: return TimestampGetDatum((Timestamp) cell->integer);
		case BOOLOID: return BoolGetDatum(cell->integer != 0);
		case NUMERICOID:
			return csd_decode_numeric(cell->integer, cell->numeric_scale,
				attribute->atttypmod);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot decode CSD result into PostgreSQL type %u",
							attribute->atttypid)));
	}
	pg_unreachable();
}

Node *
csd_executor_create_state(CustomScan *scan)
{
	CsdExecutorState *state = palloc0(sizeof(*state));

	(void) scan;
	NodeSetTag(&state->custom, T_CustomScanState);
	return (Node *) state;
}

void
csd_executor_begin(CustomScanState *node, EState *estate, int eflags)
{
	CsdExecutorState *state = (CsdExecutorState *) node;
	CustomScan *scan = (CustomScan *) node->ss.ps.plan;
	Const *wire_plan;
	bytea *wire_bytes;
	Size plan_length;

	state->explain_only = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;
	if (state->explain_only)
		return;
	wire_plan = castNode(Const, list_nth(scan->custom_private, 8));
	wire_bytes = DatumGetByteaPP(wire_plan->constvalue);
	plan_length = VARSIZE_ANY_EXHDR(wire_bytes);
	state->plan_data = palloc(plan_length);
	state->plan_length = plan_length;
	memcpy(state->plan_data, VARDATA_ANY(wire_bytes), plan_length);
	state->relations = csd_collect_relations(scan->custom_private, NIL);
	if (list_length(state->relations) !=
		((CsdSqlPlanHeader *) state->plan_data)->output_range)
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("CSD scan metadata and wire ranges disagree")));
	(void) estate;
}

TupleTableSlot *
csd_executor_next(CustomScanState *node)
{
	CsdExecutorState *state = (CsdExecutorState *) node;
	TupleTableSlot *slot = node->ss.ps.ps_ResultTupleSlot;
	CsdSqlResultHeader *result;
	CsdSqlResultCell *cells;
	TupleDesc descriptor = slot->tts_tupleDescriptor;
	uint64 first_cell;
	int column;

	ExecClearTuple(slot);
	if (state->explain_only)
		return slot;
	/* InitPlan parameters are not safe to evaluate from BeginCustomScan while
	 * ExecInitNode is still constructing the plan tree.  Patch parameters and
	 * enter CEMU only after the first ExecProcNode call, when every subplan has
	 * a valid executor dispatch function. */
	if (!state->executed)
	{
		csd_patch_plan(&state->plan_data, &state->plan_length,
			node->ss.ps.state, state->relations);
		csd_run_transport(state, state->relations);
		pfree(state->plan_data);
		state->plan_data = NULL;
		list_free(state->relations);
		state->relations = NIL;
		state->executed = true;
	}
	if (state->result_data == NULL)
		return slot;
	result = (CsdSqlResultHeader *) state->result_data;
	if (state->next_row >= result->row_count)
		return slot;
	if (result->column_count != descriptor->natts ||
		result->cell_count != (uint64) result->row_count * result->column_count ||
		result->cells_offset > state->result_length ||
		result->cell_count > (state->result_length - result->cells_offset) / sizeof(*cells))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("CSD result has %u columns but PostgreSQL expects %d",
						result->column_count, descriptor->natts)));
	cells = (CsdSqlResultCell *) (state->result_data + result->cells_offset);
	first_cell = (uint64) state->next_row * result->column_count;
	for (column = 0; column < descriptor->natts; ++column)
	{
		CsdSqlResultCell *cell = &cells[first_cell + column];

		slot->tts_isnull[column] = (cell->flags & CSD_RESULT_NULL) != 0;
		if (!slot->tts_isnull[column])
			slot->tts_values[column] = csd_decode_cell(cell, result,
				state->result_data, TupleDescAttr(descriptor, column));
	}
	++state->next_row;
	ExecStoreVirtualTuple(slot);
	return slot;
}

void
csd_executor_end(CustomScanState *node)
{
	CsdExecutorState *state = (CsdExecutorState *) node;

	if (state->plan_data != NULL)
	{
		pfree(state->plan_data);
		state->plan_data = NULL;
	}
	if (state->relations != NIL)
	{
		list_free(state->relations);
		state->relations = NIL;
	}
	if (state->result_data != NULL)
	{
		pfree(state->result_data);
		state->result_data = NULL;
	}
}

void
csd_executor_rescan(CustomScanState *node)
{
	CsdExecutorState *state = (CsdExecutorState *) node;

	state->next_row = 0;
}
