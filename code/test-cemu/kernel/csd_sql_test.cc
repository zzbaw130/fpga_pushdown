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
                    int32_t key, uint16_t numeric_digit)
{
    const uint16_t tuple_length = 36;
    put_u32(page + 24 + item * 4,
            static_cast<uint32_t>(offset) | (static_cast<uint32_t>(1) << 15) |
            (static_cast<uint32_t>(tuple_length) << 17));
    unsigned char *tuple = page + offset;
    tuple[22] = 24;                 // t_hoff: no null bitmap, MAXALIGN'd.
    put_i32(tuple + 24, key);
    put_u32(tuple + 28, 8U << 2);   // 4-byte varlena header, 8 bytes total.
    put_u16(tuple + 32, 0x8100U);   // NumericShort: scale 2, weight 0.
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

void test_heap_pipeline_with_spill()
{
    std::vector<unsigned char> left_page(CSD_SQL_PAGE_BYTES, 0);
    std::vector<unsigned char> right_page(CSD_SQL_PAGE_BYTES, 0);
    put_u16(left_page.data() + 12, 24 + 3 * 4);
    put_u16(right_page.data() + 12, 24 + 2 * 4);
    add_left_tuple(left_page.data(), 0, 128, 1, 10);  // 10.00 -> 1000
    add_left_tuple(left_page.data(), 1, 192, 2, 5);   // filtered out
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
    left_value.numeric_scale = 2;
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
    instruction.numeric_scale = 2; append(&plan, instruction);
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
    sum.output_scale = 2;
    append(&plan, sum);
    nodes[6].expression_length = sizeof(aggregate) + 2 * sizeof(CsdSqlAggregateColumn);

    CsdSqlPlanHeader header = {};
    header.magic = CSD_SQL_PLAN_MAGIC;
    header.version = CSD_SQL_PLAN_VERSION;
    header.header_bytes = sizeof(header);
    header.node_count = nodes.size();
    header.node_bytes = sizeof(CsdSqlPlanNode);
    header.plan_bytes = plan.size();
    header.root_node = 6;
    header.flags = CSD_SQL_PLAN_STATUS_LAST_RANGE | CSD_SQL_PLAN_FROZEN_SNAPSHOT |
                   CSD_SQL_PLAN_STAGED_RANGE;
    header.output_range = 2;
    header.spill_range = 3;
    header.plan_id = 0x1234;
    header.work_mem_bytes = 64; // forces the two-row sort to create Flash runs
    header.parallel_workers = 4;
    overwrite(&plan, 0, header);
    for (uint32_t index = 0; index < nodes.size(); ++index)
        overwrite(&plan, sizeof(header) + index * sizeof(CsdSqlPlanNode), nodes[index]);

    std::vector<unsigned char> output(4096, 0);
    std::vector<unsigned char> spill(4096, 0);
    std::vector<unsigned char> staged_plan = plan;
    std::vector<unsigned char> status_buffer(sizeof(CsdSqlExecutionStatus), 0);
    void *ranges[] = {left_page.data(), right_page.data(), output.data(), spill.data(),
                      staged_plan.data(), status_buffer.data()};
    long long lengths[] = {
        static_cast<long long>(left_page.size()), static_cast<long long>(right_page.size()),
        static_cast<long long>(output.size()), static_cast<long long>(spill.size()),
        static_cast<long long>(staged_plan.size()),
        static_cast<long long>(status_buffer.size())};
    struct cemu_args args = {};
    args.numr = 6;
    args.mr_addr = ranges;
    args.mr_len = lengths;
    args.cparam1 = staged_plan.size();
    args.cparam2 = 4;

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
    assert(status->validated_nodes == 7);
    assert(status->executed_nodes == 7);
    assert(status->spill_write_bytes > 0);
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
    header.work_mem_bytes = 1U << 20;
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
    header.output_range = 2; header.spill_range = 3; header.parallel_workers = 4;
    header.work_mem_bytes = 1U << 20;
    overwrite(&plan, 0, header);
    for (uint32_t node = 0; node < nodes.size(); ++node)
        overwrite(&plan, sizeof(header) + node * sizeof(CsdSqlPlanNode), nodes[node]);
    std::vector<unsigned char> output(4096, 0), spill(4096, 0), status(sizeof(CsdSqlExecutionStatus), 0);
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

} // namespace

int main()
{
    test_heap_pipeline_with_spill();
    test_tpch_expression_and_multikey_pipeline();
    test_join_canonicalization_and_residual();
    test_visibility_contract_is_required();
    return 0;
}
