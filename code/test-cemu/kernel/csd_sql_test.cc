#include <assert.h>
#include <cstdio>
#include <cstdlib>
#include <stdint.h>
#include <string.h>

#include <vector>

#include "cemu_def.h"
#include "csd_plan.h"

/* cemu_def.h intentionally replaces assert for shared kernels; tests must fail. */
#undef assert
#define assert(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    std::abort(); \
} } while (false)

extern "C" long long csd_sql(struct cemu_args *args);

namespace {

template <typename T>
uint32_t append(std::vector<unsigned char> *buffer, const T &value)
{
    const uint32_t offset = buffer->size();
    buffer->resize(offset + sizeof(value));
    memcpy(buffer->data() + offset, &value, sizeof(value));
    return offset;
}

uint32_t append_bytes(std::vector<unsigned char> *buffer, const char *value, size_t length)
{
    const uint32_t offset = buffer->size();
    buffer->insert(buffer->end(), value, value + length);
    return offset;
}

template <typename T>
void overwrite(std::vector<unsigned char> *buffer, uint32_t offset, const T &value)
{
    assert(offset + sizeof(value) <= buffer->size());
    memcpy(buffer->data() + offset, &value, sizeof(value));
}

void put_u16(unsigned char *target, uint16_t value)
{
    memcpy(target, &value, sizeof(value));
}

void put_u32(unsigned char *target, uint32_t value)
{
    memcpy(target, &value, sizeof(value));
}

void put_i32(unsigned char *target, int32_t value)
{
    memcpy(target, &value, sizeof(value));
}

void add_left_tuple(unsigned char *page, uint16_t item, uint16_t offset,
                    int32_t key, uint16_t numeric_digit,
                    uint16_t numeric_header = 0x8100U)
{
    const uint16_t tuple_length = 36;
    put_u32(page + 24 + item * 4,
            static_cast<uint32_t>(offset) | (static_cast<uint32_t>(1) << 15) |
            (static_cast<uint32_t>(tuple_length) << 17));
    unsigned char *tuple = page + offset;
    tuple[22] = 24;                 // t_hoff: no null bitmap, MAXALIGN'd.
    put_i32(tuple + 24, key);
    put_u32(tuple + 28, 8U << 2);   // 4-byte varlena header, 8 bytes total.
    put_u16(tuple + 32, numeric_header);
    put_u16(tuple + 34, numeric_digit);
}

void add_right_tuple(unsigned char *page, uint16_t item, uint16_t offset,
                     int32_t key, int32_t value)
{
    const uint16_t tuple_length = 32;
    put_u32(page + 24 + item * 4,
            static_cast<uint32_t>(offset) | (static_cast<uint32_t>(1) << 15) |
            (static_cast<uint32_t>(tuple_length) << 17));
    unsigned char *tuple = page + offset;
    tuple[22] = 24;
    put_i32(tuple + 24, key);
    put_i32(tuple + 28, value);
}

void add_int_text_tuple(unsigned char *page, uint16_t item, uint16_t offset,
                        int32_t key, const char text[8])
{
    const uint16_t tuple_length = 36;
    put_u32(page + 24 + item * 4,
            static_cast<uint32_t>(offset) | (static_cast<uint32_t>(1) << 15) |
            (static_cast<uint32_t>(tuple_length) << 17));
    unsigned char *tuple = page + offset;
    tuple[22] = 24;
    put_i32(tuple + 24, key);
    memcpy(tuple + 28, text, 8);
}

void add_feature_tuple(unsigned char *page, uint16_t item, uint16_t offset,
                       int32_t group1, int32_t group2, int32_t value,
                       int32_t date, const char text[8])
{
    const uint16_t tuple_length = 48;
    put_u32(page + 24 + item * 4,
            static_cast<uint32_t>(offset) | (static_cast<uint32_t>(1) << 15) |
            (static_cast<uint32_t>(tuple_length) << 17));
    unsigned char *tuple = page + offset;
    tuple[22] = 24;
    put_i32(tuple + 24, group1);
    put_i32(tuple + 28, group2);
    put_i32(tuple + 32, value);
    put_i32(tuple + 36, date);
    memcpy(tuple + 40, text, 8);
}

CsdSqlExpressionInstruction expression(uint8_t opcode, uint8_t type = CSD_SQL_TYPE_INVALID,
                                       uint16_t operand = 0, int64_t immediate = 0)
{
    CsdSqlExpressionInstruction instruction = {};
    instruction.opcode = opcode;
    instruction.datum_type = type;
    instruction.operand = operand;
    instruction.immediate = immediate;
    return instruction;
}

CsdSqlPlanNode make_node(uint32_t id, uint16_t opcode)
{
    CsdSqlPlanNode node = {};
    node.node_id = id;
    node.opcode = opcode;
    node.left_input = CSD_SQL_INVALID_NODE;
    node.right_input = CSD_SQL_INVALID_NODE;
    node.aux_input = CSD_SQL_INVALID_NODE;
    node.arg1 = 1;
    return node;
}

struct FakeNvm {
    std::vector<unsigned char> bytes;
    uint32_t reads;
    uint32_t writes;
    uint32_t notifications;
    uint32_t completions;
    uint64_t notified_bytes;
    uint64_t last_generation;
    FakeNvm() : reads(0), writes(0), notifications(0), completions(0), notified_bytes(0),
                last_generation(0) {}
};

long long fake_nvm_read(void *context, unsigned int nsid,
                        unsigned long long byte_offset, void *buffer,
                        unsigned long long length)
{
    FakeNvm *nvm = static_cast<FakeNvm *>(context);
    if (nsid != 1 || byte_offset > nvm->bytes.size() ||
        length > nvm->bytes.size() - byte_offset)
        return -1;
    memcpy(buffer, nvm->bytes.data() + byte_offset, length);
    ++nvm->reads;
    return 0;
}

long long fake_nvm_write(void *context, unsigned int nsid,
                         unsigned long long byte_offset, void *buffer,
                         unsigned long long length)
{
    FakeNvm *nvm = static_cast<FakeNvm *>(context);
    if (nsid != 1 || byte_offset > nvm->bytes.size() ||
        length > nvm->bytes.size() - byte_offset)
        return -1;
    memcpy(nvm->bytes.data() + byte_offset, buffer, length);
    ++nvm->writes;
    return 0;
}

long long fake_cache_notify(void *context, unsigned int nsid,
                            unsigned int relation_oid,
                            unsigned long long physical_offset,
                            unsigned long long logical_offset,
                            unsigned long long length,
                            unsigned long long generation)
{
    FakeNvm *nvm = static_cast<FakeNvm *>(context);
    if (nsid != 1 || relation_oid != 42 ||
        physical_offset > nvm->bytes.size() ||
        length > nvm->bytes.size() - physical_offset ||
        logical_offset > 3 * CSD_SQL_PAGE_BYTES ||
        length > 3 * CSD_SQL_PAGE_BYTES - logical_offset)
        return -1;
    ++nvm->notifications;
    nvm->notified_bytes += length;
    nvm->last_generation = generation;
    return 0;
}

long long fake_cache_complete(void *context, unsigned long long generation)
{
    FakeNvm *nvm = static_cast<FakeNvm *>(context);
    ++nvm->completions;
    nvm->last_generation = generation;
    return 0;
}

void test_chunked_nvm_scan()
{
    FakeNvm nvm;
    nvm.bytes.resize(3 * CSD_SQL_PAGE_BYTES, 0);
    unsigned char *first_page = nvm.bytes.data();
    unsigned char *second_page = nvm.bytes.data() + 2 * CSD_SQL_PAGE_BYTES;
    put_u16(first_page + 12, 24 + 4);
    put_u16(second_page + 12, 24 + 4);
    add_right_tuple(first_page, 0, 128, 1, 10);
    add_right_tuple(second_page, 0, 128, 2, 20);

    std::vector<unsigned char> plan(sizeof(CsdSqlPlanHeader) + sizeof(CsdSqlPlanNode), 0);
    CsdSqlPlanNode node = make_node(0, CSD_SQL_OP_SCAN);
    CsdSqlScanSpec scan = {};
    scan.input_range = 0;
    scan.page_count = 2;
    scan.attr_count = 2;
    scan.output_count = 2;
    scan.flags = CSD_SQL_SCAN_FROZEN_PAGES;
    scan.relation_oid = 42;
    scan.chunk_bytes = 2 * CSD_SQL_PAGE_BYTES;
    scan.relation_bytes = 2 * CSD_SQL_PAGE_BYTES;
    scan.extent_count = 2;
    node.expression_offset = append(&plan, scan);
    CsdSqlColumnSpec key = {};
    key.attnum = 1;
    key.output_column = 0;
    key.attlen = 4;
    key.alignment = 4;
    key.datum_type = CSD_SQL_TYPE_INT32;
    key.flags = CSD_SQL_COLUMN_OUTPUT;
    append(&plan, key);
    CsdSqlColumnSpec value = key;
    value.attnum = 2;
    value.output_column = 1;
    append(&plan, value);
    node.expression_length = sizeof(scan) + 2 * sizeof(CsdSqlColumnSpec);
    scan.extent_offset = plan.size();
    CsdSqlNvmExtent first_extent = {};
    first_extent.logical_offset = 0;
    first_extent.nvm_offset = 0;
    first_extent.length = CSD_SQL_PAGE_BYTES;
    first_extent.nsid = 1;
    append(&plan, first_extent);
    CsdSqlNvmExtent second_extent = first_extent;
    second_extent.logical_offset = CSD_SQL_PAGE_BYTES;
    second_extent.nvm_offset = 2 * CSD_SQL_PAGE_BYTES;
    append(&plan, second_extent);
    overwrite(&plan, node.expression_offset, scan);

    CsdSqlPlanHeader header = {};
    header.magic = CSD_SQL_PLAN_MAGIC;
    header.version = CSD_SQL_PLAN_VERSION;
    header.header_bytes = sizeof(header);
    header.node_count = 1;
    header.node_bytes = sizeof(node);
    header.plan_bytes = plan.size();
    header.root_node = 0;
    header.flags = CSD_SQL_PLAN_STATUS_LAST_RANGE | CSD_SQL_PLAN_FROZEN_SNAPSHOT |
                   CSD_SQL_PLAN_STAGED_RANGE | CSD_SQL_PLAN_CHUNKED_SCAN |
                   CSD_SQL_PLAN_FDM_FIFO;
    header.fifo_range_a = 1;
    header.fifo_range_b = 2;
    /* One two-column row fits, but both rows do not: exercise FIFO splitting. */
    header.fifo_bytes = 96;
    header.fdm_reserved_bytes = 2 * CSD_SQL_PAGE_BYTES + 2 * header.fifo_bytes + 4096;
    header.output_range = 3;
    header.spill_range = CSD_SQL_INVALID_RANGE;
    header.parallel_workers = 4;
    header.plan_id = 0xfeedbeef;
    overwrite(&plan, 0, header);
    overwrite(&plan, sizeof(header), node);

    std::vector<unsigned char> work(2 * CSD_SQL_PAGE_BYTES, 0);
    std::vector<unsigned char> fifo_a(header.fifo_bytes, 0);
    std::vector<unsigned char> fifo_b(header.fifo_bytes, 0);
    std::vector<unsigned char> output(4096, 0);
    std::vector<unsigned char> status(sizeof(CsdSqlExecutionStatus), 0);
    void *ranges[] = {work.data(), fifo_a.data(), fifo_b.data(), output.data(),
                      plan.data(), status.data()};
    long long lengths[] = {static_cast<long long>(work.size()),
                           static_cast<long long>(fifo_a.size()),
                           static_cast<long long>(fifo_b.size()),
                           static_cast<long long>(output.size()),
                           static_cast<long long>(plan.size()),
                           static_cast<long long>(status.size())};
    cemu_args args = {};
    args.numr = 6;
    args.mr_addr = ranges;
    args.mr_len = lengths;
    args.cparam1 = plan.size();
    args.cparam2 = 4;
    args.service_context = &nvm;
    args.nvm_read = fake_nvm_read;
    args.cache_notify = fake_cache_notify;
    args.cache_complete = fake_cache_complete;
    assert(csd_sql(&args) == 2);

    const CsdSqlResultHeader *result =
        reinterpret_cast<const CsdSqlResultHeader *>(output.data());
    const CsdSqlResultCell *cells = reinterpret_cast<const CsdSqlResultCell *>(
        output.data() + result->cells_offset);
    const CsdSqlExecutionStatus *execution =
        reinterpret_cast<const CsdSqlExecutionStatus *>(status.data());
    assert(result->row_count == 2 && result->column_count == 2);
    assert(cells[0].integer == 1 && cells[1].integer == 10);
    assert(cells[2].integer == 2 && cells[3].integer == 20);
    assert(nvm.reads == 2);
    assert(nvm.notifications == 2);
    assert(nvm.completions == 1);
    assert(nvm.notified_bytes == 2 * CSD_SQL_PAGE_BYTES);
    assert(nvm.last_generation == header.plan_id);
    assert(execution->flash_read_bytes == 2 * CSD_SQL_PAGE_BYTES);
    assert(args.flash_read_bytes == execution->flash_read_bytes);
    assert(args.flash_write_bytes == 0);
    assert(args.spill_read_bytes == 0);
    assert(args.spill_write_bytes == 0);
    assert(args.modeled_cycles == execution->modeled_cycles);
    assert(execution->input_chunks == 1);
    assert(execution->fifo_batches == 2);
    assert(execution->fifo_write_bytes > 0);
    assert(execution->fifo_write_bytes == execution->fifo_read_bytes);
    assert(execution->fdm_reserved_bytes == header.fdm_reserved_bytes);
}

void test_streaming_scan_sort_batches(uint32_t top_n)
{
    std::vector<unsigned char> page(CSD_SQL_PAGE_BYTES, 0);
    put_u16(page.data() + 12, 24 + 8 * 4);
    add_int_text_tuple(page.data(), 0, 128, 8, "eight___");
    add_int_text_tuple(page.data(), 1, 192, 1, "one_____");
    add_int_text_tuple(page.data(), 2, 256, 7, "seven___");
    add_int_text_tuple(page.data(), 3, 320, 2, "two_____");
    add_int_text_tuple(page.data(), 4, 384, 6, "six_____");
    add_int_text_tuple(page.data(), 5, 448, 3, "three___");
    add_int_text_tuple(page.data(), 6, 512, 5, "five____");
    add_int_text_tuple(page.data(), 7, 576, 4, "four____");

    std::vector<unsigned char> plan(
        sizeof(CsdSqlPlanHeader) + 2 * sizeof(CsdSqlPlanNode), 0);
    CsdSqlPlanNode scan_node = make_node(0, CSD_SQL_OP_SCAN);
    CsdSqlScanSpec scan = {};
    scan.input_range = 0;
    scan.page_count = 1;
    scan.attr_count = 2;
    scan.output_count = 2;
    scan.flags = CSD_SQL_SCAN_FROZEN_PAGES;
    scan_node.expression_offset = append(&plan, scan);
    CsdSqlColumnSpec integer = {};
    integer.attnum = 1;
    integer.output_column = 0;
    integer.attlen = 4;
    integer.alignment = 4;
    integer.datum_type = CSD_SQL_TYPE_INT32;
    integer.flags = CSD_SQL_COLUMN_OUTPUT;
    append(&plan, integer);
    CsdSqlColumnSpec text = {};
    text.attnum = 2;
    text.output_column = 1;
    text.attlen = 8;
    text.alignment = 1;
    text.datum_type = CSD_SQL_TYPE_BYTES;
    text.flags = CSD_SQL_COLUMN_OUTPUT;
    append(&plan, text);
    scan_node.expression_length = sizeof(scan) +
        2 * sizeof(CsdSqlColumnSpec);

    CsdSqlPlanNode sort_node = make_node(1, CSD_SQL_OP_SORT);
    sort_node.left_input = 0;
    sort_node.flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;
    CsdSqlSortSpec sort = {};
    sort.key_count = 1;
    sort.top_n = top_n;
    sort_node.expression_offset = append(&plan, sort);
    CsdSqlSortKey key = {};
    key.key_column = 0;
    append(&plan, key);
    sort_node.expression_length = sizeof(sort) + sizeof(key);

    CsdSqlPlanHeader header = {};
    header.magic = CSD_SQL_PLAN_MAGIC;
    header.version = CSD_SQL_PLAN_VERSION;
    header.header_bytes = sizeof(header);
    header.node_count = 2;
    header.node_bytes = sizeof(CsdSqlPlanNode);
    header.plan_bytes = plan.size();
    header.root_node = 1;
    header.flags = CSD_SQL_PLAN_STATUS_LAST_RANGE |
                   CSD_SQL_PLAN_FROZEN_SNAPSHOT |
                   CSD_SQL_PLAN_FDM_FIFO | CSD_SQL_PLAN_FDM_ARENA;
    header.output_range = 3;
    header.spill_range = 4;
    header.fifo_range_a = 1;
    header.fifo_range_b = 2;
    /* Header plus one two-column row fits; a second row does not. */
    header.fifo_bytes = 96;
    header.operator_range = 5;
    /* One two-column row is 48 modeled bytes, two exceed the arena. */
    header.operator_bytes = 64;
    header.work_mem_bytes = 1;
    header.parallel_workers = 1;
    header.plan_id = 0x5354524d;
    header.fdm_reserved_bytes = 2 * header.fifo_bytes + 4096 + 8192 +
                                header.operator_bytes;
    overwrite(&plan, 0, header);
    overwrite(&plan, sizeof(header), scan_node);
    overwrite(&plan, sizeof(header) + sizeof(scan_node), sort_node);

    std::vector<unsigned char> fifo_a(header.fifo_bytes, 0);
    std::vector<unsigned char> fifo_b(header.fifo_bytes, 0);
    std::vector<unsigned char> output(4096, 0);
    std::vector<unsigned char> spill(8192, 0);
    std::vector<unsigned char> arena(header.operator_bytes, 0);
    std::vector<unsigned char> status(sizeof(CsdSqlExecutionStatus), 0);
    void *ranges[] = {page.data(), fifo_a.data(), fifo_b.data(), output.data(),
                      spill.data(), arena.data(), status.data()};
    long long lengths[] = {static_cast<long long>(page.size()),
                           static_cast<long long>(fifo_a.size()),
                           static_cast<long long>(fifo_b.size()),
                           static_cast<long long>(output.size()),
                           static_cast<long long>(spill.size()),
                           static_cast<long long>(arena.size()),
                           static_cast<long long>(status.size())};
    cemu_args args = {};
    args.numr = 7;
    args.mr_addr = ranges;
    args.mr_len = lengths;
    args.data_buffer = plan.data();
    args.buffer_len = plan.size();
    const uint32_t expected_rows = top_n == 0 ? 8 : top_n;
    assert(csd_sql(&args) == expected_rows);

    const CsdSqlResultHeader *result =
        reinterpret_cast<const CsdSqlResultHeader *>(output.data());
    const CsdSqlResultCell *cells = reinterpret_cast<const CsdSqlResultCell *>(
        output.data() + result->cells_offset);
    const CsdSqlExecutionStatus *execution =
        reinterpret_cast<const CsdSqlExecutionStatus *>(status.data());
    const unsigned char *payload = output.data() + result->bytes_offset;
    const char *expected_text[] = {"one_____", "two_____", "three___", "four____",
                                   "five____", "six_____", "seven___", "eight___"};
    assert(result->row_count == expected_rows && result->column_count == 2);
    for (uint32_t row = 0; row < result->row_count; ++row) {
        assert(cells[row * 2].integer == static_cast<int64_t>(row + 1));
        assert(cells[row * 2 + 1].bytes_length == 8);
        assert(memcmp(payload + cells[row * 2 + 1].bytes_offset,
                      expected_text[row], 8) == 0);
    }
    assert(execution->status_code == CSD_SQL_STATUS_OK);
    assert(execution->validated_nodes == 2 && execution->executed_nodes == 2);
    assert(execution->stream_input_batches == 8);
    assert(execution->stream_input_rows == 8);
    assert(execution->stream_input_peak_bytes == 48);
    assert(execution->operator_peak_bytes <= header.operator_bytes);
    assert(execution->operator_allocations >= 8);
    assert(execution->operator_spill_count > 0);
    assert(execution->spill_write_bytes > 0);
    assert(execution->spill_read_bytes > 0);
    assert(execution->spill_cursor_peak_bytes > 0);
    assert(execution->spill_cursor_peak_bytes <= spill.size());
    assert(execution->spill_writer_peak_bytes > 0);
    assert(execution->spill_writer_peak_bytes <= spill.size());
    assert(execution->sort_merge_rows == (top_n == 0 ? 24 : 17));
}

void test_heap_pipeline_memory_policy(bool use_fdm_arena)
{
    std::vector<unsigned char> left_page(CSD_SQL_PAGE_BYTES, 0);
    std::vector<unsigned char> right_page(CSD_SQL_PAGE_BYTES, 0);
    put_u16(left_page.data() + 12, 24 + 3 * 4);
    put_u16(right_page.data() + 12, 24 + 2 * 4);
    add_left_tuple(left_page.data(), 0, 128, 1, 10);  // 10.00 -> 1000
    /* NumericShort scale=2, weight=-1: 500 * 10000^-1 = 0.05. */
    add_left_tuple(left_page.data(), 1, 192, 2, 500, 0x817fU); // filtered out
    add_left_tuple(left_page.data(), 2, 256, 3, 20);  // 20.00 -> 2000
    add_right_tuple(right_page.data(), 0, 128, 1, 100);
    add_right_tuple(right_page.data(), 1, 192, 3, 300);

    std::vector<unsigned char> plan(sizeof(CsdSqlPlanHeader) + 7 * sizeof(CsdSqlPlanNode), 0);
    std::vector<CsdSqlPlanNode> nodes;
    nodes.push_back(make_node(0, CSD_SQL_OP_SCAN));
    nodes.push_back(make_node(1, CSD_SQL_OP_FILTER));
    nodes.push_back(make_node(2, CSD_SQL_OP_PROJECT));
    nodes.push_back(make_node(3, CSD_SQL_OP_SCAN));
    nodes.push_back(make_node(4, CSD_SQL_OP_HASH_JOIN));
    nodes.push_back(make_node(5, CSD_SQL_OP_SORT));
    nodes.push_back(make_node(6, CSD_SQL_OP_HASH_AGG));

    CsdSqlScanSpec left_scan = {};
    left_scan.input_range = 0;
    left_scan.page_count = 1;
    left_scan.attr_count = 2;
    left_scan.output_count = 2;
    left_scan.flags = CSD_SQL_SCAN_FROZEN_PAGES;
    nodes[0].expression_offset = append(&plan, left_scan);
    CsdSqlColumnSpec left_key = {};
    left_key.attnum = 1;
    left_key.output_column = 0;
    left_key.attlen = 4;
    left_key.alignment = 4;
    left_key.datum_type = CSD_SQL_TYPE_INT32;
    left_key.flags = CSD_SQL_COLUMN_OUTPUT;
    append(&plan, left_key);
    CsdSqlColumnSpec left_value = {};
    left_value.attnum = 2;
    left_value.output_column = 1;
    left_value.attlen = -1;
    left_value.alignment = 4;
    left_value.datum_type = CSD_SQL_TYPE_NUMERIC;
	/* -1 asks the scan pipeline to derive dscale from each Numeric datum. */
    left_value.numeric_scale = -1;
    left_value.flags = CSD_SQL_COLUMN_OUTPUT | CSD_SQL_COLUMN_NO_EXTERNAL;
    append(&plan, left_value);
    nodes[0].expression_length = sizeof(left_scan) + 2 * sizeof(CsdSqlColumnSpec);

    nodes[1].left_input = 0;
    CsdSqlExpressionSpec filter = {};
    filter.output_count = 1;
    filter.instruction_count = 4;
    filter.max_stack = 2;
    nodes[1].expression_offset = append(&plan, filter);
    CsdSqlExpressionInstruction instruction = {};
    instruction.opcode = CSD_SQL_EXPR_PUSH_COLUMN;
    instruction.operand = 1;
    append(&plan, instruction);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_PUSH_CONSTANT;
    instruction.datum_type = CSD_SQL_TYPE_NUMERIC;
    instruction.numeric_scale = 2;
    instruction.immediate = 800;      // 8.00
    append(&plan, instruction);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_GREATER;
    append(&plan, instruction);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_EMIT;
    append(&plan, instruction);
    nodes[1].expression_length = sizeof(filter) + 4 * sizeof(CsdSqlExpressionInstruction);

    nodes[2].left_input = 1;
    CsdSqlExpressionSpec project = {};
    project.output_count = 2;
    project.instruction_count = 6;
    project.max_stack = 2;
    nodes[2].expression_offset = append(&plan, project);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_PUSH_COLUMN; instruction.operand = 0; append(&plan, instruction);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_EMIT; append(&plan, instruction);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_PUSH_COLUMN; instruction.operand = 1; append(&plan, instruction);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_PUSH_CONSTANT; instruction.datum_type = CSD_SQL_TYPE_NUMERIC;
    instruction.numeric_scale = 2; instruction.immediate = 100; append(&plan, instruction);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_ADD; instruction.datum_type = CSD_SQL_TYPE_NUMERIC;
	instruction.numeric_scale = -1; append(&plan, instruction);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_EMIT; append(&plan, instruction);
    nodes[2].expression_length = sizeof(project) + 6 * sizeof(CsdSqlExpressionInstruction);

    CsdSqlScanSpec right_scan = {};
    right_scan.input_range = 1;
    right_scan.page_count = 1;
    right_scan.attr_count = 2;
    right_scan.output_count = 2;
    right_scan.flags = CSD_SQL_SCAN_FROZEN_PAGES;
    nodes[3].expression_offset = append(&plan, right_scan);
    CsdSqlColumnSpec right_key = left_key;
    append(&plan, right_key);
    CsdSqlColumnSpec right_value = left_key;
    right_value.attnum = 2;
    right_value.output_column = 1;
    append(&plan, right_value);
    nodes[3].expression_length = sizeof(right_scan) + 2 * sizeof(CsdSqlColumnSpec);

    nodes[4].left_input = 2;
    nodes[4].right_input = 3;
    nodes[4].flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;
    CsdSqlJoinSpec join = {};
    join.key_count = 1;
    join.join_kind = CSD_SQL_JOIN_INNER;
    nodes[4].expression_offset = append(&plan, join);
    CsdSqlJoinKey join_key = {};
    join_key.left_key = 0;
    join_key.right_key = 0;
    append(&plan, join_key);
    nodes[4].expression_length = sizeof(join) + sizeof(join_key);

    nodes[5].left_input = 4;
    nodes[5].flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;
    CsdSqlSortSpec sort = {};
    sort.key_count = 1;
    nodes[5].expression_offset = append(&plan, sort);
    CsdSqlSortKey sort_key = {};
    sort_key.key_column = 1;
    append(&plan, sort_key);
    nodes[5].expression_length = sizeof(sort) + sizeof(sort_key);

    nodes[6].left_input = 5;
    nodes[6].flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;
    CsdSqlAggregateSpec aggregate = {};
    aggregate.aggregate_count = 2;
    nodes[6].expression_offset = append(&plan, aggregate);
    CsdSqlAggregateColumn count = {};
    count.input_column = CSD_SQL_NO_COLUMN;
    count.operation = CSD_SQL_AGG_COUNT;
    count.datum_type = CSD_SQL_TYPE_INT64;
    append(&plan, count);
    CsdSqlAggregateColumn sum = {};
    sum.input_column = 1;
    sum.operation = CSD_SQL_AGG_SUM;
    sum.datum_type = CSD_SQL_TYPE_NUMERIC;
	sum.output_scale = -1;
    append(&plan, sum);
    nodes[6].expression_length = sizeof(aggregate) + 2 * sizeof(CsdSqlAggregateColumn);

    FakeNvm spill_nvm;
    spill_nvm.bytes.resize(16384, 0);
    CsdSqlNvmExtent spill_extent = {};
    spill_extent.logical_offset = 0;
    spill_extent.nvm_offset = 0;
    spill_extent.length = spill_nvm.bytes.size();
    spill_extent.nsid = 1;
    const uint32_t spill_extent_offset = append(&plan, spill_extent);

    CsdSqlPlanHeader header = {};
    header.magic = CSD_SQL_PLAN_MAGIC;
    header.version = CSD_SQL_PLAN_VERSION;
    header.header_bytes = sizeof(header);
    header.node_count = nodes.size();
    header.node_bytes = sizeof(CsdSqlPlanNode);
    header.plan_bytes = plan.size();
    header.root_node = 6;
    header.flags = CSD_SQL_PLAN_STATUS_LAST_RANGE | CSD_SQL_PLAN_FROZEN_SNAPSHOT |
                   CSD_SQL_PLAN_STAGED_RANGE | CSD_SQL_PLAN_FLASH_SPILL;
    if (use_fdm_arena)
        header.flags |= CSD_SQL_PLAN_FDM_ARENA;
    header.output_range = 2;
    header.spill_range = 3;
    header.operator_range = 4;
    header.operator_bytes = use_fdm_arena ? 4096 : 0;
    header.plan_id = 0x1234;
    /* ABI v8 must use the FDM arena rather than this obsolete low threshold. */
    header.work_mem_bytes = 64;
    header.parallel_workers = 4;
    header.spill_extent_count = 1;
    header.spill_extent_offset = spill_extent_offset;
    header.spill_bytes = spill_nvm.bytes.size();
    overwrite(&plan, 0, header);
    for (uint32_t index = 0; index < nodes.size(); ++index)
        overwrite(&plan, sizeof(header) + index * sizeof(CsdSqlPlanNode), nodes[index]);

    std::vector<unsigned char> output(4096, 0);
    std::vector<unsigned char> spill(4096, 0);
    std::vector<unsigned char> operator_arena(4096, 0);
    std::vector<unsigned char> staged_plan = plan;
    std::vector<unsigned char> status_buffer(sizeof(CsdSqlExecutionStatus), 0);
    std::vector<void *> ranges;
    std::vector<long long> lengths;
    ranges.push_back(left_page.data());
    lengths.push_back(static_cast<long long>(left_page.size()));
    ranges.push_back(right_page.data());
    lengths.push_back(static_cast<long long>(right_page.size()));
    ranges.push_back(output.data());
    lengths.push_back(static_cast<long long>(output.size()));
    ranges.push_back(spill.data());
    lengths.push_back(static_cast<long long>(spill.size()));
    if (use_fdm_arena) {
        ranges.push_back(operator_arena.data());
        lengths.push_back(static_cast<long long>(operator_arena.size()));
    }
    const uint32_t plan_range = ranges.size();
    ranges.push_back(staged_plan.data());
    lengths.push_back(static_cast<long long>(staged_plan.size()));
    ranges.push_back(status_buffer.data());
    lengths.push_back(static_cast<long long>(status_buffer.size()));
    struct cemu_args args = {};
    args.numr = ranges.size();
    args.mr_addr = ranges.data();
    args.mr_len = lengths.data();
    args.cparam1 = staged_plan.size();
    args.cparam2 = plan_range;
    args.service_context = &spill_nvm;
    args.nvm_read = fake_nvm_read;
    args.nvm_write = fake_nvm_write;

    const long long execution_result = csd_sql(&args);
    if (execution_result != 1)
        std::fprintf(stderr, "execution failed: return=%lld status=%u node=%u\n", execution_result,
                     reinterpret_cast<const CsdSqlExecutionStatus *>(status_buffer.data())->status_code,
                     reinterpret_cast<const CsdSqlExecutionStatus *>(status_buffer.data())->failed_node);
    assert(execution_result == 1);
    const CsdSqlResultHeader *result = reinterpret_cast<const CsdSqlResultHeader *>(output.data());
    assert(result->magic == CSD_SQL_RESULT_MAGIC);
    assert(result->row_count == 1);
    assert(result->column_count == 2);
    const CsdSqlResultCell *cells = reinterpret_cast<const CsdSqlResultCell *>(output.data() + result->cells_offset);
    assert(cells[0].integer == 2);
    assert(cells[1].integer == 3200);
    const CsdSqlExecutionStatus *status =
        reinterpret_cast<const CsdSqlExecutionStatus *>(status_buffer.data());
    assert(status->status_code == CSD_SQL_STATUS_OK);
    assert(args.flash_read_bytes == status->flash_read_bytes);
    assert(args.flash_write_bytes == status->flash_write_bytes);
    assert(args.spill_read_bytes == status->spill_read_bytes);
    assert(args.spill_write_bytes == status->spill_write_bytes);
    assert(status->validated_nodes == 7);
    assert(status->executed_nodes == 7);
    if (use_fdm_arena) {
        assert(status->operator_memory_bytes == operator_arena.size());
        assert(status->operator_peak_bytes > header.work_mem_bytes);
        assert(status->operator_allocations > 0);
        assert(status->operator_spill_count == 0);
        assert(status->spill_write_bytes == 0);
        assert(status->flash_write_bytes == 0);
        assert(spill_nvm.writes == 0);
    } else {
        assert(status->operator_memory_bytes == header.work_mem_bytes);
        /* The oversized build side is radix-partitioned, replayed, and each
         * reconstructed build partition obtains its own bounded lease. */
        assert(status->operator_allocations > 0);
        assert(status->operator_spill_count > 0);
        assert(status->spill_write_bytes > 0);
        assert(status->spill_read_bytes > 0);
        assert(status->flash_write_bytes == status->spill_write_bytes);
        assert(status->flash_read_bytes == status->spill_read_bytes);
        assert(spill_nvm.writes > 0);
        assert(spill_nvm.reads > 0);
        uint32_t spill_magic = 0;
        memcpy(&spill_magic, spill_nvm.bytes.data(), sizeof(spill_magic));
        assert(spill_magic == UINT32_C(0x4353504c));
    }
}

void test_tpch_expression_and_multikey_pipeline()
{
    std::vector<unsigned char> page(CSD_SQL_PAGE_BYTES, 0);
    put_u16(page.data() + 12, 24 + 4 * 4);
    add_feature_tuple(page.data(), 0, 128, 1, 2, 10, -1753, "xxfoo___");
    add_feature_tuple(page.data(), 1, 192, 1, 2, 10, -1753, "US______");
    add_feature_tuple(page.data(), 2, 256, 1, 3, 20, -1753, "none____");
    add_feature_tuple(page.data(), 3, 320, 2, 1, 30, -1753, "CN______");

    std::vector<unsigned char> plan(sizeof(CsdSqlPlanHeader) + 6 * sizeof(CsdSqlPlanNode), 0);
    std::vector<CsdSqlPlanNode> nodes;
    nodes.push_back(make_node(0, CSD_SQL_OP_SCAN));
    nodes.push_back(make_node(1, CSD_SQL_OP_FILTER));
    nodes.push_back(make_node(2, CSD_SQL_OP_PROJECT));
    nodes.push_back(make_node(3, CSD_SQL_OP_HASH_AGG));
    nodes.push_back(make_node(4, CSD_SQL_OP_SORT));
    nodes.push_back(make_node(5, CSD_SQL_OP_LIMIT));
    const uint32_t contains_foo = append_bytes(&plan, "%foo%", 5);
    const uint32_t country_us = append_bytes(&plan, "US", 2);
    const uint32_t country_cn = append_bytes(&plan, "CN", 2);

    CsdSqlScanSpec scan = {};
    scan.input_range = 0;
    scan.page_count = 1;
    scan.attr_count = 5;
    scan.output_count = 5;
    scan.flags = CSD_SQL_SCAN_FROZEN_PAGES;
    nodes[0].expression_offset = append(&plan, scan);
    for (uint16_t column_index = 0; column_index < 5; ++column_index) {
        CsdSqlColumnSpec column = {};
        column.attnum = column_index + 1;
        column.output_column = column_index;
        column.attlen = column_index == 4 ? 8 : 4;
        column.alignment = 4;
        column.datum_type = column_index == 3 ? CSD_SQL_TYPE_DATE :
                            (column_index == 4 ? CSD_SQL_TYPE_BYTES : CSD_SQL_TYPE_INT32);
        column.flags = CSD_SQL_COLUMN_OUTPUT;
        append(&plan, column);
    }
    nodes[0].expression_length = sizeof(scan) + 5 * sizeof(CsdSqlColumnSpec);

    nodes[1].left_input = 0;
    CsdSqlExpressionSpec filter = {};
    filter.output_count = 1;
    filter.instruction_count = 17;
    filter.max_stack = 4;
    nodes[1].expression_offset = append(&plan, filter);
    CsdSqlExpressionInstruction instruction = expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INVALID, 4);
    append(&plan, instruction);
    instruction = expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_BYTES);
    instruction.bytes_offset = contains_foo; instruction.bytes_length = 5; append(&plan, instruction);
    append(&plan, expression(CSD_SQL_EXPR_LIKE));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INVALID, 4));
    instruction = expression(CSD_SQL_EXPR_SUBSTRING, CSD_SQL_TYPE_BYTES, 1);
    instruction.bytes_length = 2; append(&plan, instruction);
    instruction = expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_BYTES);
    instruction.bytes_offset = country_us; instruction.bytes_length = 2; append(&plan, instruction);
    append(&plan, expression(CSD_SQL_EXPR_EQUAL));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INVALID, 4));
    instruction = expression(CSD_SQL_EXPR_SUBSTRING, CSD_SQL_TYPE_BYTES, 1);
    instruction.bytes_length = 2; append(&plan, instruction);
    instruction = expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_BYTES);
    instruction.bytes_offset = country_cn; instruction.bytes_length = 2; append(&plan, instruction);
    append(&plan, expression(CSD_SQL_EXPR_EQUAL));
    append(&plan, expression(CSD_SQL_EXPR_OR));
    append(&plan, expression(CSD_SQL_EXPR_OR));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_BOOL, 0, 0));
    append(&plan, expression(CSD_SQL_EXPR_NOT));
    append(&plan, expression(CSD_SQL_EXPR_AND));
    append(&plan, expression(CSD_SQL_EXPR_EMIT));
    nodes[1].expression_length = sizeof(filter) + filter.instruction_count * sizeof(CsdSqlExpressionInstruction);

    nodes[2].left_input = 1;
    CsdSqlExpressionSpec project = {};
    project.output_count = 6;
    project.instruction_count = 22;
    project.max_stack = 3;
    nodes[2].expression_offset = append(&plan, project);
    append(&plan, expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INVALID, 0));
    append(&plan, expression(CSD_SQL_EXPR_EMIT));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INVALID, 1));
    append(&plan, expression(CSD_SQL_EXPR_EMIT));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INVALID, 3));
    append(&plan, expression(CSD_SQL_EXPR_EXTRACT_YEAR, CSD_SQL_TYPE_INT32));
    append(&plan, expression(CSD_SQL_EXPR_EMIT));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INVALID, 2));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_INT32, 0, 2));
    instruction = expression(CSD_SQL_EXPR_MULTIPLY, CSD_SQL_TYPE_INT64);
    instruction.numeric_scale = 0; append(&plan, instruction);
    append(&plan, expression(CSD_SQL_EXPR_EMIT));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INVALID, 4));
    instruction = expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_BYTES);
    instruction.bytes_offset = contains_foo; instruction.bytes_length = 5; append(&plan, instruction);
    append(&plan, expression(CSD_SQL_EXPR_LIKE));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_INT32, 0, 100));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_INT32, 0, 0));
    append(&plan, expression(CSD_SQL_EXPR_SELECT, CSD_SQL_TYPE_INT32));
    append(&plan, expression(CSD_SQL_EXPR_EMIT));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INVALID, 2));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_INT32, 0, 2));
    instruction = expression(CSD_SQL_EXPR_DIVIDE, CSD_SQL_TYPE_INT64);
    instruction.numeric_scale = 0; append(&plan, instruction);
    append(&plan, expression(CSD_SQL_EXPR_EMIT));
    nodes[2].expression_length = sizeof(project) + project.instruction_count * sizeof(CsdSqlExpressionInstruction);

    nodes[3].left_input = 2;
    nodes[3].flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;
    CsdSqlAggregateSpec aggregate = {};
    aggregate.group_count = 3;
    aggregate.aggregate_count = 5;
    nodes[3].expression_offset = append(&plan, aggregate);
    const uint16_t groups[] = {0, 1, 2};
    for (size_t group = 0; group < 3; ++group) append(&plan, groups[group]);
    CsdSqlAggregateColumn sum = {};
    sum.input_column = 3; sum.operation = CSD_SQL_AGG_SUM; sum.datum_type = CSD_SQL_TYPE_INT64;
    append(&plan, sum);
    CsdSqlAggregateColumn distinct = {};
    distinct.input_column = 3; distinct.operation = CSD_SQL_AGG_COUNT;
    distinct.datum_type = CSD_SQL_TYPE_INT64; distinct.flags = CSD_SQL_AGG_DISTINCT;
    append(&plan, distinct);
    CsdSqlAggregateColumn average = {};
    average.input_column = 3; average.operation = CSD_SQL_AGG_AVG; average.datum_type = CSD_SQL_TYPE_INT64;
    append(&plan, average);
    CsdSqlAggregateColumn minimum = {};
    minimum.input_column = 3; minimum.operation = CSD_SQL_AGG_MIN; minimum.datum_type = CSD_SQL_TYPE_INT64;
    append(&plan, minimum);
    CsdSqlAggregateColumn maximum = {};
    maximum.input_column = 3; maximum.operation = CSD_SQL_AGG_MAX; maximum.datum_type = CSD_SQL_TYPE_INT64;
    append(&plan, maximum);
    nodes[3].expression_length = sizeof(aggregate) + sizeof(groups) + 5 * sizeof(CsdSqlAggregateColumn);

    nodes[4].left_input = 3;
    CsdSqlSortSpec sort = {};
    sort.key_count = 2;
    sort.top_n = 2;
    nodes[4].expression_offset = append(&plan, sort);
    CsdSqlSortKey sort_sum = {};
    sort_sum.key_column = 3; sort_sum.flags = CSD_SQL_SORT_DESC; append(&plan, sort_sum);
    CsdSqlSortKey sort_group = {};
    sort_group.key_column = 0; append(&plan, sort_group);
    nodes[4].expression_length = sizeof(sort) + 2 * sizeof(CsdSqlSortKey);
    nodes[5].left_input = 4;
    nodes[5].arg0 = 2;

    CsdSqlPlanHeader header = {};
    header.magic = CSD_SQL_PLAN_MAGIC;
    header.version = CSD_SQL_PLAN_VERSION;
    header.header_bytes = sizeof(header);
    header.node_count = nodes.size();
    header.node_bytes = sizeof(CsdSqlPlanNode);
    header.plan_bytes = plan.size();
    header.root_node = 5;
    header.flags = CSD_SQL_PLAN_STATUS_LAST_RANGE | CSD_SQL_PLAN_FROZEN_SNAPSHOT |
                   CSD_SQL_PLAN_STAGED_RANGE;
    header.output_range = 1;
    header.spill_range = 2;
    header.parallel_workers = 4;
    /* Two grouped DISTINCT states exceed this budget, while each replay
     * partition fits and must reacquire the bounded aggregate lease. */
    header.work_mem_bytes = 640;
    overwrite(&plan, 0, header);
    for (uint32_t node = 0; node < nodes.size(); ++node)
        overwrite(&plan, sizeof(header) + node * sizeof(CsdSqlPlanNode), nodes[node]);

    std::vector<unsigned char> output(4096, 0), spill(4096, 0), status(sizeof(CsdSqlExecutionStatus), 0);
    void *ranges[] = {page.data(), output.data(), spill.data(), plan.data(), status.data()};
    long long lengths[] = {static_cast<long long>(page.size()), static_cast<long long>(output.size()),
                           static_cast<long long>(spill.size()), static_cast<long long>(plan.size()),
                           static_cast<long long>(status.size())};
    cemu_args args = {};
    args.numr = 5; args.mr_addr = ranges; args.mr_len = lengths;
    args.cparam1 = plan.size(); args.cparam2 = 3;
    const long long execution_result = csd_sql(&args);
    if (execution_result != 2)
        std::fprintf(stderr, "feature execution failed: return=%lld status=%u node=%u\n", execution_result,
                     reinterpret_cast<const CsdSqlExecutionStatus *>(status.data())->status_code,
                     reinterpret_cast<const CsdSqlExecutionStatus *>(status.data())->failed_node);
    assert(execution_result == 2);
    const CsdSqlResultHeader *result = reinterpret_cast<const CsdSqlResultHeader *>(output.data());
    assert(result->row_count == 2 && result->column_count == 8);
    const CsdSqlResultCell *cells = reinterpret_cast<const CsdSqlResultCell *>(output.data() + result->cells_offset);
    assert(cells[0].integer == 2 && cells[1].integer == 1 && cells[2].integer == 1995);
    assert(cells[3].integer == 60 && cells[4].integer == 1 && cells[5].integer == 60);
    assert(cells[6].integer == 60 && cells[7].integer == 60);
    assert(cells[8].integer == 1 && cells[9].integer == 2 && cells[10].integer == 1995);
    assert(cells[11].integer == 40 && cells[12].integer == 1 && cells[13].integer == 20);
    assert(cells[14].integer == 20 && cells[15].integer == 20);
    const CsdSqlExecutionStatus *execution =
        reinterpret_cast<const CsdSqlExecutionStatus *>(status.data());
    assert(execution->spill_write_bytes > 0);
    assert(execution->spill_read_bytes > 0);
    assert(args.flash_read_bytes == execution->flash_read_bytes);
    assert(args.flash_write_bytes == execution->flash_write_bytes);
    assert(args.spill_read_bytes == execution->spill_read_bytes);
    assert(args.spill_write_bytes == execution->spill_write_bytes);
    assert(execution->operator_allocations > 0);
}

void test_join_canonicalization_and_residual()
{
    std::vector<unsigned char> left_page(CSD_SQL_PAGE_BYTES, 0), right_page(CSD_SQL_PAGE_BYTES, 0);
    put_u16(left_page.data() + 12, 24 + 3 * 4);
    put_u16(right_page.data() + 12, 24 + 2 * 4);
    add_right_tuple(left_page.data(), 0, 128, 1, 10);
    add_right_tuple(left_page.data(), 1, 192, 2, 20);
    add_right_tuple(left_page.data(), 2, 256, 3, 30);
    add_right_tuple(right_page.data(), 0, 128, 1, 10);
    add_right_tuple(right_page.data(), 1, 192, 2, 99);

    std::vector<unsigned char> plan(sizeof(CsdSqlPlanHeader) + 7 * sizeof(CsdSqlPlanNode), 0);
    std::vector<CsdSqlPlanNode> nodes;
    nodes.push_back(make_node(0, CSD_SQL_OP_INDEX_SCAN));
    nodes.push_back(make_node(1, CSD_SQL_OP_SCAN));
    nodes.push_back(make_node(2, CSD_SQL_OP_MERGE_JOIN));
    nodes.push_back(make_node(3, CSD_SQL_OP_SUBPLAN));
    nodes.push_back(make_node(4, CSD_SQL_OP_NL_JOIN));
    nodes.push_back(make_node(5, CSD_SQL_OP_HASH_JOIN));
    nodes.push_back(make_node(6, CSD_SQL_OP_HASH_JOIN));
    for (size_t node = 2; node < nodes.size(); ++node)
        nodes[node].flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;

    CsdSqlExpressionSpec residual = {};
    residual.output_count = 1; residual.instruction_count = 4; residual.max_stack = 2;
    const uint32_t residual_offset = append(&plan, residual);
    append(&plan, expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INVALID, 1));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INVALID, 3));
    append(&plan, expression(CSD_SQL_EXPR_NOT_EQUAL));
    append(&plan, expression(CSD_SQL_EXPR_EMIT));
    const uint32_t residual_length = sizeof(residual) + 4 * sizeof(CsdSqlExpressionInstruction);

    for (uint32_t scan_index = 0; scan_index < 2; ++scan_index) {
        CsdSqlScanSpec scan = {};
        scan.input_range = scan_index;
        scan.page_count = 1;
        scan.attr_count = 2;
        scan.output_count = 2;
        scan.flags = CSD_SQL_SCAN_FROZEN_PAGES;
        nodes[scan_index].expression_offset = append(&plan, scan);
        for (uint16_t column_index = 0; column_index < 2; ++column_index) {
            CsdSqlColumnSpec column = {};
            column.attnum = column_index + 1;
            column.output_column = column_index;
            column.attlen = 4;
            column.alignment = 4;
            column.datum_type = CSD_SQL_TYPE_INT32;
            column.flags = CSD_SQL_COLUMN_OUTPUT;
            append(&plan, column);
        }
        nodes[scan_index].expression_length = sizeof(scan) + 2 * sizeof(CsdSqlColumnSpec);
    }
    nodes[2].left_input = 0; nodes[2].right_input = 1;
    CsdSqlJoinSpec merge = {};
    merge.key_count = 1; merge.join_kind = CSD_SQL_JOIN_INNER;
    nodes[2].expression_offset = append(&plan, merge);
    CsdSqlJoinKey key = {};
    key.left_key = 0; key.right_key = 0; append(&plan, key);
    nodes[2].expression_length = sizeof(merge) + sizeof(key);
    nodes[3].left_input = 2;
    nodes[4].left_input = 0; nodes[4].right_input = 1;
    CsdSqlJoinSpec nested = {};
    nested.key_count = 1; nested.join_kind = CSD_SQL_JOIN_ANTI;
    nested.residual_offset = residual_offset; nested.residual_length = residual_length;
    nodes[4].expression_offset = append(&plan, nested);
    append(&plan, key);
    nodes[4].expression_length = sizeof(nested) + sizeof(key);
    nodes[5].left_input = 0; nodes[5].right_input = 1;
    CsdSqlJoinSpec left_join = {};
    left_join.key_count = 1; left_join.join_kind = CSD_SQL_JOIN_LEFT;
    left_join.residual_offset = residual_offset; left_join.residual_length = residual_length;
    nodes[5].expression_offset = append(&plan, left_join);
    append(&plan, key);
    nodes[5].expression_length = sizeof(left_join) + sizeof(key);
    nodes[6].left_input = 0; nodes[6].right_input = 1;
    CsdSqlJoinSpec semi_join = {};
    semi_join.key_count = 1; semi_join.join_kind = CSD_SQL_JOIN_SEMI;
    nodes[6].expression_offset = append(&plan, semi_join);
    append(&plan, key);
    nodes[6].expression_length = sizeof(semi_join) + sizeof(key);

    CsdSqlPlanHeader header = {};
    header.magic = CSD_SQL_PLAN_MAGIC; header.version = CSD_SQL_PLAN_VERSION;
    header.header_bytes = sizeof(header); header.node_count = nodes.size();
    header.node_bytes = sizeof(CsdSqlPlanNode); header.plan_bytes = plan.size();
    header.root_node = 4;
    header.flags = CSD_SQL_PLAN_STATUS_LAST_RANGE | CSD_SQL_PLAN_FROZEN_SNAPSHOT |
                   CSD_SQL_PLAN_STAGED_RANGE;
    header.output_range = 2; header.spill_range = 3; header.parallel_workers = 1;
    /* Force merge/hash joins through partition replay while retaining the
     * nested-loop compatibility spill and the residual predicate checks. */
    header.work_mem_bytes = 64;
    overwrite(&plan, 0, header);
    for (uint32_t node = 0; node < nodes.size(); ++node)
        overwrite(&plan, sizeof(header) + node * sizeof(CsdSqlPlanNode), nodes[node]);
    std::vector<unsigned char> output(4096, 0), spill(16384, 0), status(sizeof(CsdSqlExecutionStatus), 0);
    void *ranges[] = {left_page.data(), right_page.data(), output.data(), spill.data(), plan.data(), status.data()};
    long long lengths[] = {static_cast<long long>(left_page.size()), static_cast<long long>(right_page.size()),
                           static_cast<long long>(output.size()), static_cast<long long>(spill.size()),
                           static_cast<long long>(plan.size()), static_cast<long long>(status.size())};
    cemu_args args = {};
    args.numr = 6; args.mr_addr = ranges; args.mr_len = lengths;
    args.cparam1 = plan.size(); args.cparam2 = 4;
    assert(csd_sql(&args) == 2);
    const CsdSqlResultHeader *result = reinterpret_cast<const CsdSqlResultHeader *>(output.data());
    const CsdSqlResultCell *cells = reinterpret_cast<const CsdSqlResultCell *>(output.data() + result->cells_offset);
    assert(result->row_count == 2 && result->column_count == 2);
    assert(cells[0].integer == 1 && cells[2].integer == 3);
    const CsdSqlExecutionStatus *execution = reinterpret_cast<const CsdSqlExecutionStatus *>(status.data());
    assert(execution->canonicalized_nodes == 3);
    assert(execution->software_fallback_nodes == 1);
    assert(execution->executed_nodes == 7);
}

void test_visibility_contract_is_required()
{
    CsdSqlPlanHeader header = {};
    header.magic = CSD_SQL_PLAN_MAGIC;
    header.version = CSD_SQL_PLAN_VERSION;
    header.header_bytes = sizeof(header);
    header.node_count = 1;
    header.node_bytes = sizeof(CsdSqlPlanNode);
    header.root_node = 0;
    header.output_range = 1;
    header.spill_range = CSD_SQL_INVALID_RANGE;
    header.plan_bytes = sizeof(header) + sizeof(CsdSqlPlanNode) + sizeof(CsdSqlScanSpec) + sizeof(CsdSqlColumnSpec);
    CsdSqlPlanNode node = make_node(0, CSD_SQL_OP_SCAN);
    node.expression_offset = sizeof(header) + sizeof(node);
    node.expression_length = sizeof(CsdSqlScanSpec) + sizeof(CsdSqlColumnSpec);
    CsdSqlScanSpec scan = {};
    scan.input_range = 0;
    scan.page_count = 1;
    scan.attr_count = 1;
    scan.output_count = 1;
    scan.flags = CSD_SQL_SCAN_FROZEN_PAGES;
    CsdSqlColumnSpec column = {};
    column.attnum = 1;
    column.output_column = 0;
    column.attlen = 4;
    column.alignment = 4;
    column.datum_type = CSD_SQL_TYPE_INT32;
    column.flags = CSD_SQL_COLUMN_OUTPUT;
    std::vector<unsigned char> plan;
    append(&plan, header);
    append(&plan, node);
    append(&plan, scan);
    append(&plan, column);
    std::vector<unsigned char> page(CSD_SQL_PAGE_BYTES, 0);
    std::vector<unsigned char> output(1024, 0);
    void *ranges[] = {page.data(), output.data()};
    long long lengths[] = {static_cast<long long>(page.size()), static_cast<long long>(output.size())};
    struct cemu_args args = {};
    args.numr = 2;
    args.mr_addr = ranges;
    args.mr_len = lengths;
    args.data_buffer = plan.data();
    args.buffer_len = plan.size();
    const long long result = csd_sql(&args);
    assert(static_cast<uint32_t>(static_cast<uint64_t>(result) >> 32) == CSD_SQL_STATUS_UNSUPPORTED);
}

void test_group_only_distinct_aggregate()
{
    std::vector<unsigned char> page(CSD_SQL_PAGE_BYTES, 0);
    put_u16(page.data() + 12, 24 + 3 * 4);
    add_right_tuple(page.data(), 0, 128, 1, 10);
    add_right_tuple(page.data(), 1, 192, 1, 20);
    add_right_tuple(page.data(), 2, 256, 2, 30);

    std::vector<unsigned char> plan(sizeof(CsdSqlPlanHeader) +
                                    2 * sizeof(CsdSqlPlanNode), 0);
    CsdSqlPlanNode scan_node = make_node(0, CSD_SQL_OP_SCAN);
    CsdSqlScanSpec scan = {};
    scan.input_range = 0;
    scan.page_count = 1;
    scan.attr_count = 2;
    scan.output_count = 2;
    scan.flags = CSD_SQL_SCAN_FROZEN_PAGES;
    scan_node.expression_offset = append(&plan, scan);
    for (uint16_t attribute = 0; attribute < 2; ++attribute) {
        CsdSqlColumnSpec column = {};
        column.attnum = attribute + 1;
        column.output_column = attribute;
        column.attlen = 4;
        column.alignment = 4;
        column.datum_type = CSD_SQL_TYPE_INT32;
        column.flags = CSD_SQL_COLUMN_OUTPUT;
        append(&plan, column);
    }
    scan_node.expression_length = sizeof(scan) + 2 * sizeof(CsdSqlColumnSpec);

    CsdSqlPlanNode aggregate_node = make_node(1, CSD_SQL_OP_HASH_AGG);
    aggregate_node.left_input = 0;
    aggregate_node.flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;
    CsdSqlAggregateSpec aggregate = {};
    aggregate.group_count = 1;
    aggregate_node.expression_offset = append(&plan, aggregate);
    const uint16_t group_column = 0;
    append(&plan, group_column);
    aggregate_node.expression_length = sizeof(aggregate) + sizeof(group_column);

    CsdSqlPlanHeader header = {};
    header.magic = CSD_SQL_PLAN_MAGIC;
    header.version = CSD_SQL_PLAN_VERSION;
    header.header_bytes = sizeof(header);
    header.node_count = 2;
    header.node_bytes = sizeof(CsdSqlPlanNode);
    header.plan_bytes = plan.size();
    header.root_node = 1;
    header.flags = CSD_SQL_PLAN_FROZEN_SNAPSHOT;
    header.output_range = 1;
    header.spill_range = 2;
    header.work_mem_bytes = 4096;
    overwrite(&plan, 0, header);
    overwrite(&plan, sizeof(header), scan_node);
    overwrite(&plan, sizeof(header) + sizeof(scan_node), aggregate_node);

    std::vector<unsigned char> output(1024, 0), spill(1024, 0);
    void *ranges[] = {page.data(), output.data(), spill.data()};
    long long lengths[] = {static_cast<long long>(page.size()),
                           static_cast<long long>(output.size()),
                           static_cast<long long>(spill.size())};
    cemu_args args = {};
    args.numr = 3;
    args.mr_addr = ranges;
    args.mr_len = lengths;
    args.data_buffer = plan.data();
    args.buffer_len = plan.size();
    assert(csd_sql(&args) == 2);
    const CsdSqlResultHeader *result =
        reinterpret_cast<const CsdSqlResultHeader *>(output.data());
    assert(result->row_count == 2 && result->column_count == 1);
}

void test_unpatched_parameter_is_rejected()
{
    std::vector<unsigned char> page(CSD_SQL_PAGE_BYTES, 0);
    put_u16(page.data() + 12, 24 + 4);
    add_right_tuple(page.data(), 0, 128, 7, 70);
    std::vector<unsigned char> plan(sizeof(CsdSqlPlanHeader) + 2 * sizeof(CsdSqlPlanNode), 0);
    CsdSqlPlanNode scan_node = make_node(0, CSD_SQL_OP_SCAN);
    CsdSqlScanSpec scan = {};
    scan.input_range = 0;
    scan.page_count = 1;
    scan.attr_count = 2;
    scan.output_count = 2;
    scan.flags = CSD_SQL_SCAN_FROZEN_PAGES;
    scan_node.expression_offset = append(&plan, scan);
    for (uint16_t attribute = 0; attribute < 2; ++attribute) {
        CsdSqlColumnSpec column = {};
        column.attnum = attribute + 1;
        column.output_column = attribute;
        column.attlen = 4;
        column.alignment = 4;
        column.datum_type = CSD_SQL_TYPE_INT32;
        column.flags = CSD_SQL_COLUMN_OUTPUT;
        append(&plan, column);
    }
    scan_node.expression_length = sizeof(scan) + 2 * sizeof(CsdSqlColumnSpec);

    CsdSqlPlanNode filter_node = make_node(1, CSD_SQL_OP_FILTER);
    filter_node.left_input = 0;
    CsdSqlExpressionSpec filter = {};
    filter.output_count = 1;
    filter.instruction_count = 4;
    filter.max_stack = 2;
    filter_node.expression_offset = append(&plan, filter);
    append(&plan, expression(CSD_SQL_EXPR_PUSH_COLUMN, CSD_SQL_TYPE_INT32, 0));
    CsdSqlExpressionInstruction parameter =
        expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_INT32, 1);
    parameter.flags = CSD_SQL_EXPR_CONSTANT_NULL | CSD_SQL_EXPR_PARAMETER_PLACEHOLDER;
    append(&plan, parameter);
    append(&plan, expression(CSD_SQL_EXPR_EQUAL, CSD_SQL_TYPE_BOOL));
    append(&plan, expression(CSD_SQL_EXPR_EMIT, CSD_SQL_TYPE_BOOL));
    filter_node.expression_length = sizeof(filter) +
        4 * sizeof(CsdSqlExpressionInstruction);

    CsdSqlPlanHeader header = {};
    header.magic = CSD_SQL_PLAN_MAGIC;
    header.version = CSD_SQL_PLAN_VERSION;
    header.header_bytes = sizeof(header);
    header.node_count = 2;
    header.node_bytes = sizeof(CsdSqlPlanNode);
    header.plan_bytes = plan.size();
    header.root_node = 1;
    header.flags = CSD_SQL_PLAN_FROZEN_SNAPSHOT;
    header.output_range = 1;
    header.spill_range = CSD_SQL_INVALID_RANGE;
    overwrite(&plan, 0, header);
    overwrite(&plan, sizeof(header), scan_node);
    overwrite(&plan, sizeof(header) + sizeof(scan_node), filter_node);

    std::vector<unsigned char> output(1024, 0);
    void *ranges[] = {page.data(), output.data()};
    long long lengths[] = {static_cast<long long>(page.size()),
                           static_cast<long long>(output.size())};
    cemu_args args = {};
    args.numr = 2;
    args.mr_addr = ranges;
    args.mr_len = lengths;
    args.data_buffer = plan.data();
    args.buffer_len = plan.size();
    const long long result = csd_sql(&args);
    assert(static_cast<uint32_t>(static_cast<uint64_t>(result) >> 32) ==
           CSD_SQL_STATUS_BAD_EXPRESSION);
}

void test_postgres_numeric_division_scale()
{
    std::vector<unsigned char> page(CSD_SQL_PAGE_BYTES, 0);
    put_u16(page.data() + 12, 24 + 4);
    add_right_tuple(page.data(), 0, 128, 1, 1);
    std::vector<unsigned char> plan(sizeof(CsdSqlPlanHeader) + 2 * sizeof(CsdSqlPlanNode), 0);
    CsdSqlPlanNode scan_node = make_node(0, CSD_SQL_OP_SCAN);
    CsdSqlScanSpec scan = {};
    scan.input_range = 0;
    scan.page_count = 1;
    scan.attr_count = 2;
    scan.output_count = 1;
    scan.flags = CSD_SQL_SCAN_FROZEN_PAGES;
    scan_node.expression_offset = append(&plan, scan);
    for (uint16_t attribute = 0; attribute < 2; ++attribute) {
        CsdSqlColumnSpec column = {};
        column.attnum = attribute + 1;
        column.output_column = 0;
        column.attlen = 4;
        column.alignment = 4;
        column.datum_type = CSD_SQL_TYPE_INT32;
        column.flags = attribute == 0 ? CSD_SQL_COLUMN_OUTPUT : 0;
        append(&plan, column);
    }
    scan_node.expression_length = sizeof(scan) + 2 * sizeof(CsdSqlColumnSpec);

    CsdSqlPlanNode project_node = make_node(1, CSD_SQL_OP_PROJECT);
    project_node.left_input = 0;
    CsdSqlExpressionSpec project = {};
    project.output_count = 2;
    project.instruction_count = 8;
    project.max_stack = 2;
    project_node.expression_offset = append(&plan, project);
    append(&plan, expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_INT64, 0, 4312626));
    append(&plan, expression(CSD_SQL_EXPR_PUSH_CONSTANT, CSD_SQL_TYPE_INT64, 0, 100000000));
    CsdSqlExpressionInstruction divide = expression(CSD_SQL_EXPR_DIVIDE, CSD_SQL_TYPE_NUMERIC);
    divide.numeric_scale = -1;
    append(&plan, divide);
    append(&plan, expression(CSD_SQL_EXPR_EMIT));
    CsdSqlExpressionInstruction left = expression(CSD_SQL_EXPR_PUSH_CONSTANT,
        CSD_SQL_TYPE_NUMERIC, 0, 246331539);
    left.numeric_scale = 2;
    append(&plan, left);
    CsdSqlExpressionInstruction right = expression(CSD_SQL_EXPR_PUSH_CONSTANT,
        CSD_SQL_TYPE_NUMERIC, 0, 70);
    right.numeric_scale = 1;
    append(&plan, right);
    append(&plan, divide);
    append(&plan, expression(CSD_SQL_EXPR_EMIT));
    project_node.expression_length = sizeof(project) +
        8 * sizeof(CsdSqlExpressionInstruction);

    CsdSqlPlanHeader header = {};
    header.magic = CSD_SQL_PLAN_MAGIC;
    header.version = CSD_SQL_PLAN_VERSION;
    header.header_bytes = sizeof(header);
    header.node_count = 2;
    header.node_bytes = sizeof(CsdSqlPlanNode);
    header.plan_bytes = plan.size();
    header.root_node = 1;
    header.flags = CSD_SQL_PLAN_FROZEN_SNAPSHOT;
    header.output_range = 1;
    header.spill_range = CSD_SQL_INVALID_RANGE;
    overwrite(&plan, 0, header);
    overwrite(&plan, sizeof(header), scan_node);
    overwrite(&plan, sizeof(header) + sizeof(scan_node), project_node);

    std::vector<unsigned char> output(1024, 0);
    void *ranges[] = {page.data(), output.data()};
    long long lengths[] = {static_cast<long long>(page.size()),
                           static_cast<long long>(output.size())};
    cemu_args args = {};
    args.numr = 2;
    args.mr_addr = ranges;
    args.mr_len = lengths;
    args.data_buffer = plan.data();
    args.buffer_len = plan.size();
    assert(csd_sql(&args) == 1);
    const CsdSqlResultHeader *result =
        reinterpret_cast<const CsdSqlResultHeader *>(output.data());
    const CsdSqlResultCell *cells = reinterpret_cast<const CsdSqlResultCell *>(
        output.data() + result->cells_offset);
    assert(result->row_count == 1 && result->column_count == 2);
    assert(cells[0].integer == INT64_C(4312626000000000000));
    assert(cells[0].numeric_scale == 20);
    assert(cells[1].integer == INT64_C(351902198571428571));
    assert(cells[1].numeric_scale == 12);
}

} // namespace

int main()
{
    test_chunked_nvm_scan();
    test_streaming_scan_sort_batches(0);
    test_streaming_scan_sort_batches(3);
    test_heap_pipeline_memory_policy(false);
    test_heap_pipeline_memory_policy(true);
    test_tpch_expression_and_multikey_pipeline();
    test_join_canonicalization_and_residual();
    test_group_only_distinct_aggregate();
    test_visibility_contract_is_required();
    test_unpatched_parameter_is_rejected();
	test_postgres_numeric_division_scale();
    return 0;
}
