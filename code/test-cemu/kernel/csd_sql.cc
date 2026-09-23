#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cemu_def.h"
#include "csd_plan.h"

namespace {

const uint16_t kHeapHasNull = 0x0001;
const uint16_t kLpNormal = 1;
const uint16_t kPageHeaderBytes = 24;
const uint16_t kHeapTupleHeaderBytes = 23;
const uint16_t kResultNull = 1;
const uint32_t kSpillMagic = UINT32_C(0x4353504c); /* CSPL */
const uint32_t kMaxParallelWorkers = 32;
const uint32_t kTupleBindWordBits = 1024;
/* A nested stage consumes the caller's CSD core rather than creating more cores. */
static thread_local bool g_csd_worker_active = false;

struct Value {
    bool is_null;
    uint8_t type;
    int64_t integer;
    int16_t numeric_scale;
    std::string bytes;
    Value() : is_null(true), type(CSD_SQL_TYPE_INVALID), integer(0), numeric_scale(0) {}
};
typedef std::vector<Value> Row;
struct Batch {
    uint16_t columns;
    std::vector<Row> rows;
    Batch() : columns(0) {}
};
struct SpillRecordHeader {
    uint32_t magic;
    uint32_t node_id;
    uint32_t run_id;
    uint32_t row_count;
    uint16_t column_count;
    uint16_t reserved16;
    uint64_t cell_count;
    uint64_t bytes_length;
} __attribute__((packed));

static bool checked_add(uint64_t left, uint64_t right, uint64_t *result)
{
    if (left > std::numeric_limits<uint64_t>::max() - right) return false;
    *result = left + right;
    return true;
}
static bool checked_mul(uint64_t left, uint64_t right, uint64_t *result)
{
    if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) return false;
    *result = left * right;
    return true;
}
static uint16_t read_u16(const unsigned char *source) { uint16_t value; std::memcpy(&value, source, sizeof(value)); return value; }
static int16_t read_i16(const unsigned char *source) { int16_t value; std::memcpy(&value, source, sizeof(value)); return value; }
static uint32_t read_u32(const unsigned char *source) { uint32_t value; std::memcpy(&value, source, sizeof(value)); return value; }
static int32_t read_i32(const unsigned char *source) { int32_t value; std::memcpy(&value, source, sizeof(value)); return value; }
static int64_t read_i64(const unsigned char *source) { int64_t value; std::memcpy(&value, source, sizeof(value)); return value; }
static uint64_t align_up(uint64_t value, uint8_t alignment)
{
    return alignment <= 1 ? value : (value + alignment - 1) & ~(static_cast<uint64_t>(alignment) - 1);
}
static bool is_supported_type(uint8_t type) { return type >= CSD_SQL_TYPE_INT16 && type <= CSD_SQL_TYPE_BYTES; }
static uint64_t batch_bytes(const Batch &batch)
{
    uint64_t total = static_cast<uint64_t>(batch.rows.size()) * batch.columns * sizeof(CsdSqlResultCell);
    for (std::vector<Row>::const_iterator row = batch.rows.begin(); row != batch.rows.end(); ++row)
        for (Row::const_iterator value = row->begin(); value != row->end(); ++value) total += value->bytes.size();
    return total;
}
static std::string value_key(const Value &value)
{
    std::string key;
    key.push_back(static_cast<char>(value.type));
    key.push_back(value.is_null ? '\0' : '\1');
    if (!value.is_null) {
        if (value.type == CSD_SQL_TYPE_BYTES) key.append(value.bytes);
        else {
            int64_t integer = value.integer;
            int16_t scale = value.type == CSD_SQL_TYPE_NUMERIC ? value.numeric_scale : 0;
            while (scale > 0 && integer % 10 == 0) { integer /= 10; --scale; }
            key.append(reinterpret_cast<const char *>(&scale), sizeof(scale));
            key.append(reinterpret_cast<const char *>(&integer), sizeof(integer));
        }
    }
    return key;
}
static int compare_value(const Value &left, const Value &right)
{
    if (left.is_null != right.is_null) return left.is_null ? -1 : 1;
    if (left.is_null) return 0;
    if (left.type == CSD_SQL_TYPE_BYTES || right.type == CSD_SQL_TYPE_BYTES)
        return left.bytes == right.bytes ? 0 : (left.bytes < right.bytes ? -1 : 1);
    if (left.type == CSD_SQL_TYPE_NUMERIC || right.type == CSD_SQL_TYPE_NUMERIC) {
        const int16_t left_scale = left.type == CSD_SQL_TYPE_NUMERIC ? left.numeric_scale : 0;
        const int16_t right_scale = right.type == CSD_SQL_TYPE_NUMERIC ? right.numeric_scale : 0;
        const int16_t scale = std::max(left_scale, right_scale);
        __int128 left_integer = left.integer;
        __int128 right_integer = right.integer;
        for (int16_t power = left_scale; power < scale; ++power) left_integer *= 10;
        for (int16_t power = right_scale; power < scale; ++power) right_integer *= 10;
        return left_integer == right_integer ? 0 : (left_integer < right_integer ? -1 : 1);
    }
    return left.integer == right.integer ? 0 : (left.integer < right.integer ? -1 : 1);
}

class Engine {
public:
    explicit Engine(struct cemu_args *args)
        : args_(args), plan_data_(NULL), plan_length_(0), plan_from_range_(false), header_(NULL),
          nodes_(NULL), plan_bytes_(0), work_mem_bytes_(0), parallel_workers_(1), spill_offset_(0),
          spill_run_id_(0), failed_(false)
    {
        std::memset(&status_, 0, sizeof(status_));
        status_.magic = CSD_SQL_PLAN_MAGIC;
        status_.version = CSD_SQL_PLAN_VERSION;
        status_.failed_node = CSD_SQL_INVALID_NODE;
    }

    long long run()
    {
        if (!validate_plan()) return error_return();
        std::vector<Batch> results(header_->node_count);
        std::vector<uint8_t> complete(header_->node_count, 0);
        uint32_t completed = 0;
        while (completed < header_->node_count) {
            std::vector<uint32_t> ready;
            for (uint32_t index = 0; index < header_->node_count; ++index)
                if (!complete[index] && node_ready(nodes_[index], complete)) ready.push_back(index);
            if (ready.empty()) {
                fail(CSD_SQL_STATUS_BAD_NODE, CSD_SQL_INVALID_NODE);
                return error_return();
            }
            std::vector<uint8_t> success(ready.size(), 0);
            run_parallel(ready.size(), [&](size_t slot) {
                success[slot] = execute_node(ready[slot], &results[ready[slot]], results) ? 1 : 0;
            });
            for (size_t slot = 0; slot < ready.size(); ++slot) {
                if (!success[slot]) return error_return();
                complete[ready[slot]] = 1;
                ++completed;
                std::lock_guard<std::mutex> guard(status_mutex_);
                ++status_.executed_nodes;
            }
        }
        const Batch &root = results[header_->root_node];
        if (!write_result(root)) return error_return();
        {
            std::lock_guard<std::mutex> guard(status_mutex_);
            status_.status_code = CSD_SQL_STATUS_OK;
            status_.output_rows = root.rows.size();
        }
        write_status();
        return static_cast<long long>(root.rows.size());
    }

private:
    bool validate_plan()
    {
        if (!select_plan_source() || plan_length_ < sizeof(CsdSqlPlanHeader))
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE);
        header_ = static_cast<const CsdSqlPlanHeader *>(plan_data_);
        status_.plan_id = header_->plan_id;
        if (header_->magic != CSD_SQL_PLAN_MAGIC || header_->version != CSD_SQL_PLAN_VERSION ||
            header_->header_bytes != sizeof(CsdSqlPlanHeader) || header_->node_bytes != sizeof(CsdSqlPlanNode) ||
            header_->node_count == 0 || header_->root_node >= header_->node_count ||
            header_->plan_bytes > plan_length_ ||
            (plan_from_range_ && !(header_->flags & CSD_SQL_PLAN_STAGED_RANGE)))
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE);
        uint64_t node_bytes = 0, node_end = 0;
        if (!checked_mul(header_->node_count, sizeof(CsdSqlPlanNode), &node_bytes) ||
            !checked_add(header_->header_bytes, node_bytes, &node_end) || header_->plan_bytes < node_end)
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE);
        if (header_->output_range == CSD_SQL_INVALID_RANGE || args_->mr_addr == NULL || args_->mr_len == NULL ||
            header_->output_range >= static_cast<uint32_t>(args_->numr))
            return fail(CSD_SQL_STATUS_NO_INPUT_RANGE, CSD_SQL_INVALID_NODE);
        if ((header_->flags & CSD_SQL_PLAN_STATUS_LAST_RANGE) && args_->numr == 0)
            return fail(CSD_SQL_STATUS_NO_STATUS_RANGE, CSD_SQL_INVALID_NODE);
        plan_bytes_ = header_->plan_bytes;
        work_mem_bytes_ = header_->work_mem_bytes == 0 ? UINT64_C(1) << 20 : header_->work_mem_bytes;
        parallel_workers_ = header_->parallel_workers == 0 ? 1 :
            std::min<uint32_t>(header_->parallel_workers, kMaxParallelWorkers);
        nodes_ = reinterpret_cast<const CsdSqlPlanNode *>(static_cast<const char *>(plan_data_) + header_->header_bytes);
        for (uint32_t index = 0; index < header_->node_count; ++index) {
            const CsdSqlPlanNode &node = nodes_[index];
            if (node.node_id != index || !is_valid_opcode(node.opcode) ||
                !is_valid_input(node.left_input, index) || !is_valid_input(node.right_input, index) ||
                !is_valid_input(node.aux_input, index)) return fail(CSD_SQL_STATUS_BAD_NODE, index);
            if (!blob_range(node.expression_offset, node.expression_length))
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
            ++status_.validated_nodes;
            status_.estimated_bytes += node.estimated_bytes;
        }
        return true;
    }

    bool select_plan_source()
    {
        if (args_ == NULL) return false;
        if (args_->data_buffer != NULL && args_->buffer_len > 0) {
            plan_data_ = args_->data_buffer;
            plan_length_ = static_cast<uint64_t>(args_->buffer_len);
            plan_from_range_ = false;
            return true;
        }
        if (args_->cparam1 < static_cast<long long>(sizeof(CsdSqlPlanHeader)) || args_->cparam2 < 0 ||
            args_->numr <= 0 || args_->mr_addr == NULL || args_->mr_len == NULL || args_->cparam2 >= args_->numr)
            return false;
        const int plan_range = static_cast<int>(args_->cparam2);
        if (args_->mr_addr[plan_range] == NULL || args_->mr_len[plan_range] < 0 ||
            args_->cparam1 > args_->mr_len[plan_range]) return false;
        plan_data_ = args_->mr_addr[plan_range];
        plan_length_ = static_cast<uint64_t>(args_->cparam1);
        plan_from_range_ = true;
        return true;
    }

    bool node_ready(const CsdSqlPlanNode &node, const std::vector<uint8_t> &complete) const
    {
        return input_ready(node.left_input, complete) && input_ready(node.right_input, complete) &&
               input_ready(node.aux_input, complete);
    }
    bool input_ready(uint32_t input, const std::vector<uint8_t> &complete) const
    {
        return input == CSD_SQL_INVALID_NODE || complete[input] != 0;
    }
    template <typename Function>
    void run_parallel(size_t task_count, Function function)
    {
        const size_t workers = std::min<size_t>(parallel_workers_, task_count);
        if (workers <= 1 || g_csd_worker_active) {
            for (size_t task = 0; task < task_count; ++task) function(task);
            return;
        }
        std::atomic<size_t> next_task(0);
        std::vector<std::thread> threads;
        for (size_t worker = 0; worker < workers; ++worker)
            threads.push_back(std::thread([&]() {
                g_csd_worker_active = true;
                for (;;) {
                    const size_t task = next_task.fetch_add(1);
                    if (task >= task_count) break;
                    function(task);
                }
                g_csd_worker_active = false;
            }));
        for (std::vector<std::thread>::iterator thread = threads.begin(); thread != threads.end(); ++thread)
            thread->join();
    }

    bool execute_node(uint32_t index, Batch *output, const std::vector<Batch> &results)
    {
        const CsdSqlPlanNode &node = nodes_[index];
        if ((node.opcode == CSD_SQL_OP_FILTER || node.opcode == CSD_SQL_OP_PROJECT ||
             node.opcode == CSD_SQL_OP_HASH_AGG || node.opcode == CSD_SQL_OP_GROUP_AGG ||
             node.opcode == CSD_SQL_OP_SORT || node.opcode == CSD_SQL_OP_MATERIALIZE ||
             node.opcode == CSD_SQL_OP_LIMIT || node.opcode == CSD_SQL_OP_SUBPLAN) &&
            node.left_input == CSD_SQL_INVALID_NODE)
            return fail(CSD_SQL_STATUS_BAD_NODE, index);
        if ((node.opcode == CSD_SQL_OP_HASH_JOIN || node.opcode == CSD_SQL_OP_MERGE_JOIN ||
             node.opcode == CSD_SQL_OP_NL_JOIN) &&
            (node.left_input == CSD_SQL_INVALID_NODE || node.right_input == CSD_SQL_INVALID_NODE))
            return fail(CSD_SQL_STATUS_BAD_NODE, index);
        switch (node.opcode) {
        case CSD_SQL_OP_SCAN: return execute_scan(index, node, output);
        case CSD_SQL_OP_INDEX_SCAN:
            add_canonicalized_node();
            return execute_scan(index, node, output);
        case CSD_SQL_OP_FILTER: return execute_filter(index, node, input_batch(node.left_input, results), output);
        case CSD_SQL_OP_PROJECT: return execute_project(index, node, input_batch(node.left_input, results), output);
        case CSD_SQL_OP_HASH_AGG:
        case CSD_SQL_OP_GROUP_AGG: return execute_aggregate(index, node, input_batch(node.left_input, results), output);
        case CSD_SQL_OP_HASH_JOIN:
            return execute_join(index, node, input_batch(node.left_input, results), input_batch(node.right_input, results), output, false);
        case CSD_SQL_OP_MERGE_JOIN:
            add_canonicalized_node();
            return execute_join(index, node, input_batch(node.left_input, results), input_batch(node.right_input, results), output, false);
        case CSD_SQL_OP_NL_JOIN:
            add_software_fallback_node();
            return execute_join(index, node, input_batch(node.left_input, results), input_batch(node.right_input, results), output, true);
        case CSD_SQL_OP_SORT: return execute_sort(index, node, input_batch(node.left_input, results), output);
        case CSD_SQL_OP_MATERIALIZE: return execute_materialize(index, node, input_batch(node.left_input, results), output);
        case CSD_SQL_OP_SUBPLAN:
            add_canonicalized_node();
            return execute_materialize(index, node, input_batch(node.left_input, results), output);
        case CSD_SQL_OP_LIMIT: return execute_limit(index, node, input_batch(node.left_input, results), output);
        default: return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
        }
    }
    const Batch &input_batch(uint32_t input, const std::vector<Batch> &results) const { return results[input]; }

#include "csd_scan.cc"
#include "csd_expression.cc"
#include "csd_filter.cc"
#include "csd_project.cc"
#include "csd_aggregate.cc"
#include "csd_join.cc"
#include "csd_sort.cc"
#include "csd_control.cc"

    bool write_result(const Batch &batch)
    {
        if (args_->mr_addr[header_->output_range] == NULL || args_->mr_len[header_->output_range] < 0)
            return fail(CSD_SQL_STATUS_NO_INPUT_RANGE, CSD_SQL_INVALID_NODE);
        uint64_t cells_count = 0, cells_bytes = 0, total_bytes = sizeof(CsdSqlResultHeader), payload_bytes = 0;
        if (!checked_mul(batch.rows.size(), batch.columns, &cells_count) ||
            !checked_mul(cells_count, sizeof(CsdSqlResultCell), &cells_bytes) ||
            !checked_add(total_bytes, cells_bytes, &total_bytes)) return fail(CSD_SQL_STATUS_OUTPUT_TOO_SMALL, CSD_SQL_INVALID_NODE);
        for (std::vector<Row>::const_iterator row = batch.rows.begin(); row != batch.rows.end(); ++row) {
            if (row->size() != batch.columns) return fail(CSD_SQL_STATUS_BAD_NODE, CSD_SQL_INVALID_NODE);
            for (Row::const_iterator value = row->begin(); value != row->end(); ++value)
                if (!checked_add(payload_bytes, value->bytes.size(), &payload_bytes)) return fail(CSD_SQL_STATUS_OUTPUT_TOO_SMALL, CSD_SQL_INVALID_NODE);
        }
        if (!checked_add(total_bytes, payload_bytes, &total_bytes) || total_bytes > static_cast<uint64_t>(args_->mr_len[header_->output_range]))
            return fail(CSD_SQL_STATUS_OUTPUT_TOO_SMALL, CSD_SQL_INVALID_NODE);
        unsigned char *target = static_cast<unsigned char *>(args_->mr_addr[header_->output_range]);
        CsdSqlResultHeader result = {};
        result.magic = CSD_SQL_RESULT_MAGIC;
        result.version = CSD_SQL_PLAN_VERSION;
        result.root_node = header_->root_node;
        result.row_count = batch.rows.size();
        result.column_count = batch.columns;
        result.cell_count = cells_count;
        result.cells_offset = sizeof(result);
        result.bytes_offset = sizeof(result) + cells_bytes;
        result.bytes_length = payload_bytes;
        std::memcpy(target, &result, sizeof(result));
        CsdSqlResultCell *cells = reinterpret_cast<CsdSqlResultCell *>(target + result.cells_offset);
        unsigned char *payload = target + result.bytes_offset;
        uint64_t payload_offset = 0, cell_index = 0;
        for (std::vector<Row>::const_iterator row = batch.rows.begin(); row != batch.rows.end(); ++row)
            for (Row::const_iterator value = row->begin(); value != row->end(); ++value) {
                CsdSqlResultCell cell = {};
                cell.integer = value->integer;
                cell.bytes_offset = payload_offset;
                cell.bytes_length = value->bytes.size();
                cell.datum_type = value->type;
                cell.flags = value->is_null ? kResultNull : 0;
                cell.numeric_scale = value->numeric_scale;
                cells[cell_index++] = cell;
                if (!value->bytes.empty()) {
                    std::memcpy(payload + payload_offset, value->bytes.data(), value->bytes.size());
                    payload_offset += value->bytes.size();
                }
            }
        return true;
    }
    bool is_valid_opcode(uint16_t opcode) const { return opcode >= CSD_SQL_OP_SCAN && opcode <= CSD_SQL_OP_SUBPLAN; }
    bool is_valid_input(uint32_t input, uint32_t current_node) const { return input == CSD_SQL_INVALID_NODE || input < current_node; }
    bool blob_range(uint64_t offset, uint64_t length) const { return offset <= plan_bytes_ && length <= plan_bytes_ - offset; }
    template <typename T>
    bool expression_as(const CsdSqlPlanNode &node, size_t bytes, const T **result) const
    {
        if (node.expression_length < bytes || !blob_range(node.expression_offset, bytes)) return false;
        *result = reinterpret_cast<const T *>(static_cast<const char *>(plan_data_) + node.expression_offset);
        return true;
    }
    bool fail(uint16_t code, uint32_t node)
    {
        { std::lock_guard<std::mutex> guard(status_mutex_);
          if (!failed_) { status_.status_code = code; status_.failed_node = node; failed_ = true; } }
        write_status();
        return false;
    }
    void write_status()
    {
        if (args_ == NULL || args_->numr == 0 || args_->mr_addr == NULL || args_->mr_len == NULL ||
            (header_ != NULL && !(header_->flags & CSD_SQL_PLAN_STATUS_LAST_RANGE))) return;
        const int range = args_->numr - 1;
        if (args_->mr_addr[range] != NULL && args_->mr_len[range] >= static_cast<long long>(sizeof(status_))) {
            CsdSqlExecutionStatus snapshot;
            { std::lock_guard<std::mutex> guard(status_mutex_); snapshot = status_; }
            std::memcpy(args_->mr_addr[range], &snapshot, sizeof(snapshot));
        }
    }
    void add_input_rows(uint64_t rows) { std::lock_guard<std::mutex> guard(status_mutex_); status_.input_rows += rows; }
    void add_canonicalized_node() { std::lock_guard<std::mutex> guard(status_mutex_); ++status_.canonicalized_nodes; }
    void add_software_fallback_node() { std::lock_guard<std::mutex> guard(status_mutex_); ++status_.software_fallback_nodes; }
    void add_cycles(uint64_t rows, const CsdSqlPlanNode &node)
    {
        const uint64_t ii = node.arg1 == 0 ? 1 : node.arg1;
        uint64_t cycles = 0, with_launch = 0, modeled = 0;
        std::lock_guard<std::mutex> guard(status_mutex_);
        if (checked_mul(rows, ii, &cycles) && checked_add(cycles, 64, &with_launch) && checked_add(status_.modeled_cycles, with_launch, &modeled)) status_.modeled_cycles = modeled;
        else status_.modeled_cycles = std::numeric_limits<uint64_t>::max();
    }
    long long error_return()
    {
        write_status();
        std::lock_guard<std::mutex> guard(status_mutex_);
        return static_cast<long long>((static_cast<uint64_t>(status_.status_code) << 32) | status_.failed_node);
    }

    struct cemu_args *args_;
    const void *plan_data_;
    uint64_t plan_length_;
    bool plan_from_range_;
    const CsdSqlPlanHeader *header_;
    const CsdSqlPlanNode *nodes_;
    uint64_t plan_bytes_;
    uint64_t work_mem_bytes_;
    uint32_t parallel_workers_;
    uint64_t spill_offset_;
    uint32_t spill_run_id_;
    CsdSqlExecutionStatus status_;
    bool failed_;
    std::mutex status_mutex_;
    std::mutex spill_mutex_;
};

} // namespace

extern "C" long long csd_sql(struct cemu_args *args)
{
    if (args != NULL && args->numr > 0 && args->mr_addr != NULL && args->mr_len != NULL) {
        const int status_range = args->numr - 1;
        if (args->mr_addr[status_range] != NULL && args->mr_len[status_range] >= static_cast<long long>(sizeof(CsdSqlExecutionStatus))) {
            CsdSqlExecutionStatus entered = {};
            entered.magic = CSD_SQL_PLAN_MAGIC;
            entered.version = CSD_SQL_PLAN_VERSION;
            entered.reserved32 = args->data_buffer != NULL ? UINT32_C(0x44415441) : (args->cparam1 > 0 ? UINT32_C(0x4d52504c) : UINT32_C(0x4e554c4c));
            std::memcpy(args->mr_addr[status_range], &entered, sizeof(entered));
        }
    }
    Engine engine(args);
    return engine.run();
}
