#ifndef CSD_SQL_PLAN_H
#define CSD_SQL_PLAN_H

#include <stdint.h>

/*
 * Stable little-endian wire ABI between PostgreSQL and the CEMU program.
 * PostgreSQL executor nodes are deliberately not sent across this boundary.
 * Version 8 adds a query-level FDM operator arena whose free capacity controls
 * blocking-operator spill. Version 7 added two capacity-accounted FDM FIFO
 * buffers between scan and downstream operators. Version 6 added bounded FDM spill staging and writable
 * NVMe spill extents.
 * Version 5 added bounded FDM chunk buffers and NVMe physical extents to the
 * scan ABI. Version 4 added the TPC-H expression bytecode, multi-column blocking
 * operators, fixed-point scale metadata, and explicit canonicalization
 * counters. PostgreSQL executor structs never cross this boundary.
 */
#define CSD_SQL_PLAN_MAGIC       UINT32_C(0x43534450) /* "CSDP" */
#define CSD_SQL_PLAN_VERSION     8
#define CSD_SQL_RESULT_MAGIC     UINT32_C(0x43534452) /* "CSDR" */
#define CSD_SQL_INVALID_NODE     UINT32_C(0xffffffff)
#define CSD_SQL_INVALID_RANGE    UINT32_C(0xffffffff)
#define CSD_SQL_NO_COLUMN        UINT16_C(0xffff)
#define CSD_SQL_PAGE_BYTES       8192U
#define CSD_SQL_PROFILE_SLOTS    14U

enum CsdSqlOpcode {
    CSD_SQL_OP_SCAN = 1,
    CSD_SQL_OP_INDEX_SCAN,
    CSD_SQL_OP_FILTER,
    CSD_SQL_OP_PROJECT,
    CSD_SQL_OP_HASH_JOIN,
    CSD_SQL_OP_MERGE_JOIN,
    CSD_SQL_OP_NL_JOIN,
    CSD_SQL_OP_HASH_AGG,
    CSD_SQL_OP_GROUP_AGG,
    CSD_SQL_OP_SORT,
    CSD_SQL_OP_MATERIALIZE,
    CSD_SQL_OP_LIMIT,
    CSD_SQL_OP_SUBPLAN,
};

enum CsdSqlJoinKind {
    CSD_SQL_JOIN_INNER = 1,
    CSD_SQL_JOIN_LEFT,
    CSD_SQL_JOIN_SEMI,
    CSD_SQL_JOIN_ANTI,
};

enum CsdSqlPlanFlags {
    /* The final CEMU memory range is a CsdSqlExecutionStatus. */
    CSD_SQL_PLAN_STATUS_LAST_RANGE = UINT32_C(1) << 0,
    /* The scanner may inspect tuple visibility only under this contract. */
    CSD_SQL_PLAN_FROZEN_SNAPSHOT = UINT32_C(1) << 1,
    /*
     * The plan bytes reside in an MRS entry.  Program Execute cparam1 is the
     * exact byte count and cparam2 is the zero-based MRS entry index.  This
     * avoids the Linux passthrough overlay, where CEMU's dlen shares bytes
     * with the standard NVMe metadata fields.
     */
    CSD_SQL_PLAN_STAGED_RANGE = UINT32_C(1) << 2,
    /* Emit functional-model operator timing to the CEMU process log. */
    CSD_SQL_PLAN_PROFILE = UINT32_C(1) << 3,
    /* SCAN ranges are bounded FDM windows populated from NVMe extents. */
    CSD_SQL_PLAN_CHUNKED_SCAN = UINT32_C(1) << 4,
    /* spill_range is a bounded staging window; spill bytes live on Flash. */
    CSD_SQL_PLAN_FLASH_SPILL = UINT32_C(1) << 5,
    /* fifo_range_a/b are alternating, fixed-capacity FDM tuple buffers. */
    CSD_SQL_PLAN_FDM_FIFO = UINT32_C(1) << 6,
    /* operator_range owns the remaining query-level FDM working capacity. */
    CSD_SQL_PLAN_FDM_ARENA = UINT32_C(1) << 7,
};

enum CsdSqlNodeFlags {
    CSD_SQL_NODE_BLOCKING = UINT16_C(1) << 0,
    CSD_SQL_NODE_SPILLABLE = UINT16_C(1) << 1,
    CSD_SQL_NODE_REQUIRES_ORDER = UINT16_C(1) << 2,
};

enum CsdSqlScanFlags {
    /* The PostgreSQL planner proved the scanned pages safe for CSD visibility. */
    CSD_SQL_SCAN_FROZEN_PAGES = UINT32_C(1) << 0,
    /* Fail rather than producing a partial answer when a non-normal tuple appears. */
    CSD_SQL_SCAN_STRICT_PAGE_FORMAT = UINT32_C(1) << 1,
};

enum CsdSqlColumnFlags {
    /* Value is emitted by SCAN.  Other physical attributes are decoded only to skip them. */
    CSD_SQL_COLUMN_OUTPUT = UINT16_C(1) << 0,
    /* Data in a varlena datum must not be external/TOASTed. */
    CSD_SQL_COLUMN_NO_EXTERNAL = UINT16_C(1) << 1,
    /* PostgreSQL bpchar comparison ignores physical blank padding. */
    CSD_SQL_COLUMN_TRIM_SPACE = UINT16_C(1) << 2,
};

enum CsdSqlDatumType {
    CSD_SQL_TYPE_INVALID = 0,
    CSD_SQL_TYPE_INT16,
    CSD_SQL_TYPE_INT32,
    CSD_SQL_TYPE_INT64,
    CSD_SQL_TYPE_DATE,
    CSD_SQL_TYPE_BOOL,
    CSD_SQL_TYPE_NUMERIC,
    CSD_SQL_TYPE_BYTES,
};

/* Bounded postfix bytecode shared by FILTER, PROJECT and join residuals. */
enum CsdSqlExpressionOpcode {
    CSD_SQL_EXPR_PUSH_COLUMN = 1,
    CSD_SQL_EXPR_PUSH_AUX_COLUMN,
    CSD_SQL_EXPR_PUSH_CONSTANT,
    CSD_SQL_EXPR_ADD,
    CSD_SQL_EXPR_SUBTRACT,
    CSD_SQL_EXPR_MULTIPLY,
    CSD_SQL_EXPR_DIVIDE,
    CSD_SQL_EXPR_NEGATE,
    CSD_SQL_EXPR_EQUAL,
    CSD_SQL_EXPR_NOT_EQUAL,
    CSD_SQL_EXPR_LESS,
    CSD_SQL_EXPR_LESS_EQUAL,
    CSD_SQL_EXPR_GREATER,
    CSD_SQL_EXPR_GREATER_EQUAL,
    CSD_SQL_EXPR_AND,
    CSD_SQL_EXPR_OR,
    CSD_SQL_EXPR_NOT,
    CSD_SQL_EXPR_IS_NULL,
    CSD_SQL_EXPR_IS_NOT_NULL,
    CSD_SQL_EXPR_LIKE,
    CSD_SQL_EXPR_SELECT,
    CSD_SQL_EXPR_EXTRACT_YEAR,
    CSD_SQL_EXPR_SUBSTRING,
    CSD_SQL_EXPR_EMIT,
};

enum CsdSqlExpressionFlags {
    CSD_SQL_EXPR_CASE_INSENSITIVE = UINT16_C(1) << 0,
};

enum CsdSqlExpressionInstructionFlags {
    CSD_SQL_EXPR_CONSTANT_NULL = UINT16_C(1) << 0,
    /* Host executor must replace this PUSH_CONSTANT before Program Execute. */
    CSD_SQL_EXPR_PARAMETER_PLACEHOLDER = UINT16_C(1) << 1,
};

enum CsdSqlAggregateOp {
    CSD_SQL_AGG_COUNT = 1,
    CSD_SQL_AGG_SUM,
    CSD_SQL_AGG_MIN,
    CSD_SQL_AGG_MAX,
    CSD_SQL_AGG_AVG,
};

enum CsdSqlAggregateFlags {
    CSD_SQL_AGG_DISTINCT = UINT16_C(1) << 0,
};

enum CsdSqlSortFlags {
    CSD_SQL_SORT_DESC = UINT16_C(1) << 0,
    CSD_SQL_SORT_NULLS_FIRST = UINT16_C(1) << 1,
};

enum CsdSqlStatusCode {
    CSD_SQL_STATUS_OK = 0,
    CSD_SQL_STATUS_BAD_ABI = 1,
    CSD_SQL_STATUS_BAD_NODE = 2,
    CSD_SQL_STATUS_BAD_EXPRESSION = 3,
    CSD_SQL_STATUS_NO_STATUS_RANGE = 4,
    CSD_SQL_STATUS_UNSUPPORTED = 5,
    CSD_SQL_STATUS_BAD_PAGE = 6,
    CSD_SQL_STATUS_OUTPUT_TOO_SMALL = 7,
    CSD_SQL_STATUS_SPILL_TOO_SMALL = 8,
    CSD_SQL_STATUS_NO_INPUT_RANGE = 9,
    CSD_SQL_STATUS_FIFO_TOO_SMALL = 10,
};

/*
 * The plan payload consists of this header, fixed-size nodes and descriptors.
 * It can be delivered as legacy command data, or placed in a CSD MRS entry
 * when CSD_SQL_PLAN_STAGED_RANGE is set.  In the latter (production) mode,
 * Program Execute cparam1 is plan_bytes and cparam2 selects the MRS entry;
 * only that descriptor travels in the NVMe command.
 */
struct CsdSqlPlanHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t node_count;
    uint32_t node_bytes;
    uint32_t plan_bytes;
    uint32_t root_node;
    uint32_t flags;
    uint32_t output_range;
    uint32_t spill_range;
    /* Zero is deterministic single-worker mode; otherwise capped by CSD. */
    uint32_t parallel_workers;
    uint64_t plan_id;
    uint64_t work_mem_bytes;
    uint32_t fifo_range_a;
    uint32_t fifo_range_b;
    uint64_t fifo_bytes;
    uint32_t operator_range;
    uint32_t reserved_operator32;
    uint64_t operator_bytes;
    uint64_t fdm_reserved_bytes;
    uint32_t spill_extent_count;
    uint32_t spill_extent_offset;
    uint64_t spill_bytes;
} __attribute__((packed));

/* Inputs must refer to an earlier node ID, or CSD_SQL_INVALID_NODE. */
struct CsdSqlPlanNode {
    uint32_t node_id;
    uint16_t opcode;
    uint16_t flags;
    uint32_t left_input;
    uint32_t right_input;
    uint32_t aux_input;
    uint32_t expression_offset;
    uint32_t expression_length;
    uint32_t output_schema_id;
    uint64_t estimated_rows;
    uint64_t estimated_bytes;
    uint64_t arg0; /* LIMIT count; Aggregate conservative output bytes */
    uint64_t arg1;
    uint32_t reserved;
} __attribute__((packed));

/* SCAN expression: CsdSqlScanSpec immediately followed by attr_count columns. */
struct CsdSqlScanSpec {
    /* Bounded FDM work-buffer range, not a complete relation image. */
    uint32_t input_range;
    uint32_t first_page;
    uint32_t page_count;
    uint16_t attr_count;
    uint16_t output_count;
    uint32_t flags;
    uint32_t relation_oid;
    uint32_t extent_count;
    uint32_t extent_offset; /* absolute offset in the complete plan blob */
    uint32_t chunk_bytes;
    uint64_t relation_bytes;
} __attribute__((packed));

/* A logical relation is the ordered concatenation of all of its heap files. */
struct CsdSqlNvmExtent {
    uint64_t logical_offset;
    uint64_t nvm_offset;
    uint64_t length;
    uint32_t nsid;
    uint32_t flags;
} __attribute__((packed));

/*
 * Physical attributes must appear in PostgreSQL attnum order.  attlen is the
 * PostgreSQL typlen (-1 for varlena); alignment is one of 1, 2, 4 or 8.
 * Numeric is normalized to a scaled int64 using numeric_scale.
 */
struct CsdSqlColumnSpec {
    uint16_t attnum;
    uint16_t output_column;
    int16_t attlen;
    uint8_t alignment;
    uint8_t datum_type;
    int32_t numeric_scale;
    uint16_t flags;
    uint16_t reserved;
} __attribute__((packed));

/* Followed by instruction_count CsdSqlExpressionInstruction records. */
struct CsdSqlExpressionSpec {
    uint16_t output_count;
    uint16_t instruction_count;
    uint16_t max_stack;
    uint16_t flags;
} __attribute__((packed));

struct CsdSqlExpressionInstruction {
    uint8_t opcode;
    uint8_t datum_type;
    uint16_t operand;
    int16_t numeric_scale;
    uint16_t flags;
    uint32_t bytes_offset; /* relative to the complete plan blob */
    uint32_t bytes_length;
    int64_t immediate;
} __attribute__((packed));

/* Followed by group_count uint16 columns, then aggregate_count descriptors. */
struct CsdSqlAggregateSpec {
    uint16_t group_count;
    uint16_t aggregate_count;
    uint16_t flags;
    uint16_t reserved;
} __attribute__((packed));

struct CsdSqlAggregateColumn {
    uint16_t input_column;
    uint8_t operation;
    uint8_t datum_type;
    int16_t output_scale;
    uint16_t flags;
} __attribute__((packed));

/* Followed by key_count CsdSqlJoinKey descriptors. */
struct CsdSqlJoinSpec {
    uint16_t key_count;
    uint16_t join_kind;
    uint32_t residual_offset; /* optional expression over the joined row */
    uint32_t residual_length;
    uint32_t flags;
} __attribute__((packed));

struct CsdSqlJoinKey {
    uint16_t left_key;
    uint16_t right_key;
} __attribute__((packed));

/* Followed by key_count CsdSqlSortKey descriptors. */
struct CsdSqlSortSpec {
    uint16_t key_count;
    uint16_t reserved;
    uint64_t top_n; /* zero means all rows */
} __attribute__((packed));

struct CsdSqlSortKey {
    uint16_t key_column;
    uint16_t flags;
} __attribute__((packed));

/* Wire result in output_range: header, cell_count fixed cells, byte payload. */
struct CsdSqlResultHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t status_code;
    uint32_t root_node;
    uint32_t row_count;
    uint16_t column_count;
    uint16_t reserved16;
    uint64_t cell_count;
    uint64_t cells_offset;
    uint64_t bytes_offset;
    uint64_t bytes_length;
} __attribute__((packed));

struct CsdSqlResultCell {
    int64_t integer;
    uint32_t bytes_offset; /* relative to CsdSqlResultHeader::bytes_offset */
    uint16_t bytes_length;
    uint8_t datum_type;
    uint8_t flags; /* bit 0: NULL */
    int16_t numeric_scale;
    uint16_t reserved16;
} __attribute__((packed));

struct CsdSqlExecutionStatus {
    uint32_t magic;
    uint16_t version;
    uint16_t status_code;
    uint32_t validated_nodes;
    uint32_t executed_nodes;
    uint32_t failed_node;
    uint32_t reserved32;
    uint64_t plan_id;
    uint64_t estimated_bytes;
    uint64_t input_rows;
    uint64_t output_rows;
    uint64_t flash_read_bytes;
    uint64_t flash_write_bytes;
    uint64_t input_chunks;
    uint64_t spill_write_bytes;
    uint64_t spill_read_bytes;
    uint64_t fifo_batches;
    uint64_t fifo_write_bytes;
    uint64_t fifo_read_bytes;
    uint64_t fdm_reserved_bytes;
    uint64_t operator_memory_bytes;
    uint64_t operator_peak_bytes;
    uint64_t operator_allocations;
    uint64_t operator_spill_count;
    uint64_t modeled_cycles;
    uint32_t canonicalized_nodes;
    uint32_t software_fallback_nodes;
    /* Functional-model wall time and cardinality, indexed by CsdSqlOpcode. */
    uint64_t operator_calls[CSD_SQL_PROFILE_SLOTS];
    uint64_t operator_rows[CSD_SQL_PROFILE_SLOTS];
    uint64_t operator_ns[CSD_SQL_PROFILE_SLOTS];
    /* Append-only diagnostics for bounded Scan batch consumers. */
    uint64_t stream_input_batches;
    uint64_t stream_input_rows;
    uint64_t stream_input_peak_bytes;
    /* Append-only diagnostics for cursor-based external Sort merge. */
    uint64_t spill_cursor_peak_bytes;
    uint64_t spill_writer_peak_bytes;
    uint64_t sort_merge_rows;
} __attribute__((packed));

#endif /* CSD_SQL_PLAN_H */
