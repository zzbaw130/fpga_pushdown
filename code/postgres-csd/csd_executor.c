#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
int csd_spill_mb = 256;
int csd_parallel_workers = 4;
int csd_program_runtime = 0;
int csd_device_work_mem_mb = 8192;

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
csd_transport_init(CsdTransport *transport)
{
	MemSet(transport, 0, sizeof(*transport));
	transport->lock_fd = -1;
	transport->control_fd = -1;
	transport->execute_fd = -1;
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
	for (index = 0; index < transport->range_count; ++index)
		if (transport->range_fds != NULL && transport->range_fds[index] >= 0)
		{
			int truncate_result = ftruncate(transport->range_fds[index], 0);

			(void) truncate_result;
			close(transport->range_fds[index]);
		}
	/* FDM ranges are query-local Flash scratch.  Leaving high-numbered files
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

static uint64
csd_relation_bytes(Oid relation_oid)
{
	Relation relation = table_open(relation_oid, AccessShareLock);
	uint64 bytes = (uint64) RelationGetNumberOfBlocks(relation) * BLCKSZ;

	table_close(relation, NoLock);
	return bytes;
}

static void
csd_copy_relation(Oid relation_oid, int output_fd, uint64 expected_bytes)
{
	Relation relation = table_open(relation_oid, NoLock);
	char *relative = relpathbackend(relation->rd_node, relation->rd_backend,
								 MAIN_FORKNUM);
	char *base = psprintf("%s/%s", DataDir, relative);
	uint64 copied = 0;
	int segment = 0;

	while (copied < expected_bytes)
	{
		char *path = segment == 0 ? pstrdup(base) : psprintf("%s.%d", base, segment);
		uint64 segment_bytes = Min(expected_bytes - copied,
			(uint64) RELSEG_SIZE * BLCKSZ);
		/* FDMFS hands the source iterator directly to the NVMe backend. */
		int input_fd = open(path, O_RDONLY | O_DIRECT | PG_BINARY, 0);
		off_t input_offset = 0;
		off_t output_offset = (off_t) copied;

		if (input_fd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open CSD heap segment \"%s\": %m", path)));
		while (segment_bytes > 0)
		{
			size_t request = (size_t) Min(segment_bytes, UINT64_C(1) << 30);
			ssize_t bytes = copy_file_range(input_fd, &input_offset, output_fd,
										&output_offset, request, 0);

			if (bytes < 0)
			{
				int saved_errno = errno;
				close(input_fd);
				errno = saved_errno;
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stage relation %u into CSD memory: %m",
								relation_oid)));
			}
			if (bytes == 0)
			{
				close(input_fd);
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("short copy while staging relation %u", relation_oid)));
			}
			segment_bytes -= bytes;
			copied += bytes;
		}
		close(input_fd);
		pfree(path);
		++segment;
	}
	pfree(base);
	pfree(relative);
	table_close(relation, NoLock);
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

/* Treat csd.spill_mb as the preferred reservation.  FDMFS has no useful
 * statvfs capacity reporting, so shrink an unused spill reservation when the
 * relation ranges already occupy most of the memory namespace.  If a plan
 * really spills beyond the granted range the kernel reports SPILL_TOO_SMALL. */
static int
csd_create_spill_range(uint32 range, uint64 requested_bytes,
					   uint64 *allocated_bytes)
{
	const uint64 minimum_bytes = UINT64_C(1) << 20;
	uint64 attempt = csd_align_range(Max(requested_bytes, minimum_bytes));
	int allocation_error = 0;

	for (;;)
	{
		int fd = csd_try_create_range(range, attempt, &allocation_error);

		if (fd >= 0)
		{
			*allocated_bytes = attempt;
			return fd;
		}
		if ((allocation_error != ENOSPC && allocation_error != ENOMEM) ||
			attempt <= minimum_bytes)
			break;
		attempt = Max(minimum_bytes, csd_align_range(attempt / 2));
	}
	errno = allocation_error;
	ereport(ERROR,
			(errcode_for_file_access(),
			 errmsg("could not reserve CSD spill range (requested %llu bytes, minimum %llu bytes): %m",
					(unsigned long long) requested_bytes,
					(unsigned long long) minimum_bytes)));
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
csd_patch_plan(char *plan, Size plan_length, EState *estate)
{
	CsdSqlPlanHeader *header = (CsdSqlPlanHeader *) plan;
	CsdSqlPlanNode *nodes;
	uint32 index;

	if (plan_length < sizeof(*header) || header->plan_bytes != plan_length ||
		header->node_count == 0 ||
		header->header_bytes + (uint64) header->node_count * sizeof(*nodes) > plan_length)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid staged CSD plan")));
	header->flags |= CSD_SQL_PLAN_STATUS_LAST_RANGE |
		CSD_SQL_PLAN_FROZEN_SNAPSHOT | CSD_SQL_PLAN_STAGED_RANGE;
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

			if (node->expression_offset > plan_length ||
				node->expression_length < sizeof(*scan) ||
				node->expression_length > plan_length - node->expression_offset)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("invalid CSD scan descriptor")));
			scan = (CsdSqlScanSpec *) (plan + node->expression_offset);
			scan->flags |= CSD_SQL_SCAN_FROZEN_PAGES;
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
csd_run_transport(CsdExecutorState *state, char *plan, Size plan_length,
				  List *relations)
{
	CsdSqlPlanHeader *header = (CsdSqlPlanHeader *) plan;
	CsdTransport transport;
	ListCell *cell;
	uint32 relation_count = list_length(relations);
	uint32 output_range = relation_count;
	uint32 spill_range = output_range + 1;
	uint32 plan_range = spill_range + 1;
	uint32 status_range = plan_range + 1;
	uint64 output_bytes = (uint64) csd_output_mb * 1024 * 1024;
	uint64 spill_bytes = (uint64) csd_spill_mb * 1024 * 1024;
	uint64 allocated_spill_bytes;
	char *lock_path = pstrdup("/tmp/csd_executor.lock");
	char program_payload[PATH_MAX + 32];
	CsdNvmeProgramExecuteCmd *execute_command;
	struct nvme_passthru_cmd command;
	CsdSqlExecutionStatus status;
	CsdSqlResultHeader result;
	Oid *relation_oids;
	uint64 *relation_bytes;
	uint32 *allocation_order;
	uint64 total_result_bytes;
	uint32 index = 0;
	uint32 order_index;

	csd_transport_init(&transport);
	PG_TRY();
	{
		transport.lock_fd = open(lock_path, O_CREAT | O_RDWR | PG_BINARY, 0600);
		if (transport.lock_fd < 0 || flock(transport.lock_fd, LOCK_EX) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not lock CSD FDM workspace: %m")));
		transport.range_count = status_range + 1;
		transport.range_fds = palloc(sizeof(int) * transport.range_count);
		transport.range_sizes = palloc0(sizeof(long long) * transport.range_count);
		for (index = 0; index < (uint32) transport.range_count; ++index)
			transport.range_fds[index] = -1;
		relation_oids = palloc(sizeof(Oid) * relation_count);
		relation_bytes = palloc(sizeof(uint64) * relation_count);
		allocation_order = palloc(sizeof(uint32) * relation_count);
		index = 0;
		foreach(cell, relations)
		{
			relation_oids[index] = lfirst_oid(cell);
			relation_bytes[index] = csd_relation_bytes(relation_oids[index]);
			transport.range_sizes[index] = relation_bytes[index];
			allocation_order[index] = index;
			++index;
		}
		/* FDMFS allocates each range as a large contiguous extent.  Preserve
		 * logical range IDs, but reserve/copy the largest heaps first to avoid
		 * fragmentation when lineitem follows smaller join inputs. */
		for (index = 0; index < relation_count; ++index)
			for (order_index = index + 1; order_index < relation_count; ++order_index)
				if (relation_bytes[allocation_order[order_index]] >
					relation_bytes[allocation_order[index]])
				{
					uint32 swap = allocation_order[index];

					allocation_order[index] = allocation_order[order_index];
					allocation_order[order_index] = swap;
				}
		for (order_index = 0; order_index < relation_count; ++order_index)
		{
			uint32 duplicate_index = relation_count;
			uint32 candidate;

			index = allocation_order[order_index];
			/* A plan may scan the same heap several times (TPC-H Q17/Q18/Q21).
			 * Keep distinct logical MRS IDs for the wire plan, but alias them to
			 * one staged FDM inode instead of consuming the namespace repeatedly. */
			for (candidate = 0; candidate < relation_count; ++candidate)
				if (candidate != index && relation_oids[candidate] == relation_oids[index] &&
					transport.range_fds[candidate] >= 0)
				{
					duplicate_index = candidate;
					break;
				}
			if (duplicate_index < relation_count)
			{
				transport.range_fds[index] = dup(transport.range_fds[duplicate_index]);
				if (transport.range_fds[index] < 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not alias duplicate CSD relation range: %m")));
				continue;
			}
			transport.range_fds[index] = csd_create_range(index,
				relation_bytes[index]);
			csd_copy_relation(relation_oids[index], transport.range_fds[index],
				relation_bytes[index]);
		}
		pfree(allocation_order);
		pfree(relation_bytes);
		pfree(relation_oids);
		transport.range_sizes[output_range] = output_bytes;
		transport.range_fds[output_range] = csd_create_range(output_range, output_bytes);
		transport.range_fds[spill_range] = csd_create_spill_range(spill_range,
			spill_bytes, &allocated_spill_bytes);
		transport.range_sizes[spill_range] = allocated_spill_bytes;
		transport.range_sizes[plan_range] = csd_align_range(plan_length);
		transport.range_fds[plan_range] = csd_create_range(plan_range, plan_length);
		transport.range_sizes[status_range] = CSD_RANGE_ALIGNMENT;
		transport.range_fds[status_range] = csd_create_range(status_range,
			CSD_RANGE_ALIGNMENT);

		header->output_range = output_range;
		header->spill_range = spill_range;
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
		csd_patch_plan(state->plan_data, state->plan_length,
			node->ss.ps.state);
		csd_run_transport(state, state->plan_data, state->plan_length,
			state->relations);
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
