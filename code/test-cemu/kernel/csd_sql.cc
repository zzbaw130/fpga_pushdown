#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <mutex>
#include <pthread.h>
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
const uint32_t kFifoMagic = UINT32_C(0x43464946); /* CFIF */
const uint32_t kMaxParallelWorkers = 32;
const uint32_t kTupleBindWordBits = 1024;

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
struct SpillHandle {
    uint64_t offset;
    uint64_t length;
    uint32_t row_count;
    uint16_t column_count;
    uint16_t reserved16;
    SpillHandle() : offset(0), length(0), row_count(0), column_count(0), reserved16(0) {}
};
struct SpillRowCursor {
    SpillHandle handle;
    SpillRecordHeader record;
    uint64_t cells_bytes;
    uint64_t row_cells_bytes;
    uint64_t payload_start;
    uint64_t next_row;
    uint64_t validated_payload_offset;
    uint64_t bytes_read;
    uint64_t chunk_capacity;
    uint64_t descriptor_rows;
    uint64_t descriptor_position;
    size_t chunk_position;
    std::vector<CsdSqlResultCell> descriptors;
    Batch chunk;
    bool initialized;
    bool accounted;
    SpillRowCursor()
        : cells_bytes(0), row_cells_bytes(0), payload_start(0), next_row(0),
          validated_payload_offset(0), bytes_read(0), chunk_capacity(0),
          descriptor_rows(0), descriptor_position(0), chunk_position(0),
          initialized(false), accounted(false)
    {
        std::memset(&record, 0, sizeof(record));
    }
};
struct SpillRowWriter {
    uint32_t node_id;
    uint32_t run_id;
    uint32_t row_count;
    uint32_t written_rows;
    uint16_t columns;
    uint64_t offset;
    uint64_t reserved_length;
    uint64_t cells_bytes;
    uint64_t payload_bytes;
    uint64_t chunk_capacity;
    std::vector<CsdSqlResultCell> cells;
    std::vector<unsigned char> payload;
    bool initialized;
    SpillRowWriter()
        : node_id(0), run_id(0), row_count(0), written_rows(0), columns(0),
          offset(0), reserved_length(0), cells_bytes(0), payload_bytes(0),
          chunk_capacity(0), initialized(false) {}
};
/* A direct Scan->[Filter/Project] build pipeline may finish before its probe
 * Scan starts.  Keep only Flash record handles and exact per-partition byte
 * totals across those two DAG steps; table rows never enter results[]. */
struct StreamedJoinBuildState {
    const CsdSqlJoinSpec *spec;
    const CsdSqlJoinKey *keys;
    uint16_t columns;
    size_t partition_count;
    bool scalar_key;
    uint64_t rows;
    uint64_t bytes;
    std::vector<std::vector<SpillHandle> > handles;
    std::vector<uint64_t> partition_bytes;
    bool configured;
    bool complete;
    bool consumed;
    StreamedJoinBuildState()
        : spec(NULL), keys(NULL), columns(0), partition_count(0),
          scalar_key(false), rows(0), bytes(0), configured(false),
          complete(false), consumed(false) {}
};
struct FifoBatchHeader {
    uint32_t magic;
    uint32_t generation;
    uint32_t row_count;
    uint16_t column_count;
    uint16_t reserved16;
    uint64_t total_bytes;
    uint64_t cells_offset;
    uint64_t bytes_offset;
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
          spill_run_id_(0), fifo_generation_(0), operator_memory_capacity_(0),
          operator_memory_used_(0), worker_budget_key_valid_(false), failed_(false)
    {
        worker_budget_key_valid_ = pthread_key_create(&worker_budget_key_, NULL) == 0;
        std::memset(&status_, 0, sizeof(status_));
        status_.magic = CSD_SQL_PLAN_MAGIC;
        status_.version = CSD_SQL_PLAN_VERSION;
        status_.failed_node = CSD_SQL_INVALID_NODE;
    }

    ~Engine()
    {
        if (worker_budget_key_valid_) pthread_key_delete(worker_budget_key_);
    }

    uint64_t cache_generation() const
    {
        return header_ != NULL ? header_->plan_id : 0;
    }

    void execution_model(uint64_t *modeled_cycles, uint64_t *flash_read_bytes,
                         uint64_t *flash_write_bytes,
                         uint64_t *spill_read_bytes,
                         uint64_t *spill_write_bytes)
    {
        std::lock_guard<std::mutex> guard(status_mutex_);
        *modeled_cycles = status_.modeled_cycles;
        *flash_read_bytes = status_.flash_read_bytes;
        *flash_write_bytes = status_.flash_write_bytes;
        *spill_read_bytes = status_.spill_read_bytes;
        *spill_write_bytes = status_.spill_write_bytes;
    }

    long long run()
    {
        if (!validate_plan()) return error_return();
        streamed_join_builds_.clear();
        streamed_join_builds_.resize(header_->node_count);
        std::vector<Batch> results(header_->node_count);
        std::vector<uint8_t> complete(header_->node_count, 0);
        std::vector<uint32_t> consumers(header_->node_count, 0);
        std::vector<uint32_t> sole_consumer(header_->node_count, CSD_SQL_INVALID_NODE);
        std::vector<uint8_t> precomputed(header_->node_count, 0);
        for (uint32_t index = 0; index < header_->node_count; ++index) {
            const CsdSqlPlanNode &node = nodes_[index];
            add_consumer(node.left_input, index, consumers, sole_consumer);
            add_consumer(node.right_input, index, consumers, sole_consumer);
            add_consumer(node.aux_input, index, consumers, sole_consumer);
        }
        uint32_t completed = 0;
        while (completed < header_->node_count) {
            std::vector<uint32_t> ready;
            for (uint32_t index = 0; index < header_->node_count; ++index) {
                if (complete[index] || !node_ready(nodes_[index], complete))
                    continue;
                if (nodes_[index].opcode == CSD_SQL_OP_SCAN) {
                    const std::vector<uint32_t> pipeline = stateless_pipeline(
                        index, consumers, sole_consumer);
                    const uint32_t join_index = streaming_join_target(index,
                        pipeline, consumers, sole_consumer);
                    /* A resident-build Join consumes its sole probe Scan
                     * directly. Resolve the build subtree first; nested Join
                     * trees apply the same rule recursively. */
                    if (join_index != CSD_SQL_INVALID_NODE &&
                        !complete[nodes_[join_index].right_input])
                        continue;
                }
                ready.push_back(index);
            }
            if (ready.empty()) {
                fail(CSD_SQL_STATUS_BAD_NODE, CSD_SQL_INVALID_NODE);
                return error_return();
            }
            std::vector<uint8_t> success(ready.size(), 0);
            run_ready_nodes(ready, [&](size_t slot) {
                success[slot] = execute_node(ready[slot], &results[ready[slot]], results,
                                             complete, consumers, sole_consumer,
                                             precomputed) ? 1 : 0;
            });
            for (size_t slot = 0; slot < ready.size(); ++slot) {
                if (!success[slot]) return error_return();
                const uint32_t node_index = ready[slot];
                const CsdSqlPlanNode &node = nodes_[node_index];
                complete[node_index] = 1;
                ++completed;
                std::lock_guard<std::mutex> guard(status_mutex_);
                ++status_.executed_nodes;
                release_input(node.left_input, consumers, results);
                release_input(node.right_input, consumers, results);
                release_input(node.aux_input, consumers, results);
            }
        }
        const Batch &root = results[header_->root_node];
        if (!write_result(root)) return error_return();
        {
            std::lock_guard<std::mutex> guard(status_mutex_);
            status_.status_code = CSD_SQL_STATUS_OK;
            status_.output_rows = root.rows.size();
        }
        if (header_->flags & CSD_SQL_PLAN_PROFILE) write_profile();
        write_status();
        return static_cast<long long>(root.rows.size());
    }

private:
    bool validate_plan()
    {
        if (!select_plan_source())
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE, 0x101);
        if (plan_length_ < sizeof(CsdSqlPlanHeader))
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE, 0x102);
        header_ = static_cast<const CsdSqlPlanHeader *>(plan_data_);
        status_.plan_id = header_->plan_id;
        if (header_->magic != CSD_SQL_PLAN_MAGIC)
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE, 0x103);
        if (header_->version != CSD_SQL_PLAN_VERSION)
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE, 0x104);
        if (header_->header_bytes != sizeof(CsdSqlPlanHeader))
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE, 0x105);
        if (header_->node_bytes != sizeof(CsdSqlPlanNode))
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE, 0x106);
        if (header_->node_count == 0 || header_->root_node >= header_->node_count)
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE, 0x107);
        if (header_->plan_bytes > plan_length_)
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE, 0x108);
        if (plan_from_range_ && !(header_->flags & CSD_SQL_PLAN_STAGED_RANGE))
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE, 0x109);
        uint64_t node_bytes = 0, node_end = 0;
        if (!checked_mul(header_->node_count, sizeof(CsdSqlPlanNode), &node_bytes) ||
            !checked_add(header_->header_bytes, node_bytes, &node_end) || header_->plan_bytes < node_end)
            return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE, 0x10a);
        if (header_->output_range == CSD_SQL_INVALID_RANGE || args_->mr_addr == NULL || args_->mr_len == NULL ||
            header_->output_range >= static_cast<uint32_t>(args_->numr))
            return fail(CSD_SQL_STATUS_NO_INPUT_RANGE, CSD_SQL_INVALID_NODE);
        if ((header_->flags & CSD_SQL_PLAN_STATUS_LAST_RANGE) && args_->numr == 0)
            return fail(CSD_SQL_STATUS_NO_STATUS_RANGE, CSD_SQL_INVALID_NODE);
        plan_bytes_ = header_->plan_bytes;
        work_mem_bytes_ = header_->work_mem_bytes == 0 ? UINT64_C(1) << 20 : header_->work_mem_bytes;
        parallel_workers_ = header_->parallel_workers == 0 ? 1 :
            std::min<uint32_t>(header_->parallel_workers, kMaxParallelWorkers);
        if (!worker_budget_key_valid_) parallel_workers_ = 1;
        status_.fdm_reserved_bytes = header_->fdm_reserved_bytes;
        if (header_->flags & CSD_SQL_PLAN_FDM_FIFO) {
            if (header_->fifo_range_a == CSD_SQL_INVALID_RANGE ||
                header_->fifo_range_b == CSD_SQL_INVALID_RANGE ||
                header_->fifo_range_a >= static_cast<uint32_t>(args_->numr) ||
                header_->fifo_range_b >= static_cast<uint32_t>(args_->numr) ||
                header_->fifo_range_a == header_->fifo_range_b ||
                header_->fifo_bytes < sizeof(FifoBatchHeader) ||
                args_->mr_addr[header_->fifo_range_a] == NULL ||
                args_->mr_addr[header_->fifo_range_b] == NULL ||
                args_->mr_len[header_->fifo_range_a] < 0 ||
                args_->mr_len[header_->fifo_range_b] < 0 ||
                header_->fifo_bytes > static_cast<uint64_t>(args_->mr_len[header_->fifo_range_a]) ||
                header_->fifo_bytes > static_cast<uint64_t>(args_->mr_len[header_->fifo_range_b]))
                return fail(CSD_SQL_STATUS_FIFO_TOO_SMALL, CSD_SQL_INVALID_NODE, 0x301);
        }
        if (header_->flags & CSD_SQL_PLAN_FDM_ARENA) {
            if (header_->operator_range == CSD_SQL_INVALID_RANGE ||
                header_->operator_range >= static_cast<uint32_t>(args_->numr) ||
                header_->operator_bytes == 0 ||
                args_->mr_addr[header_->operator_range] == NULL ||
                args_->mr_len[header_->operator_range] < 0 ||
                header_->operator_bytes > static_cast<uint64_t>(
                    args_->mr_len[header_->operator_range]))
                return fail(CSD_SQL_STATUS_BAD_ABI, CSD_SQL_INVALID_NODE, 0x309);
            operator_memory_capacity_ = header_->operator_bytes;
        } else {
            /* Stand-alone ABI tests retain the pre-v8 work-memory contract. */
            operator_memory_capacity_ = work_mem_bytes_;
        }
        status_.operator_memory_bytes = operator_memory_capacity_;
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
    static void release_input(uint32_t input, std::vector<uint32_t> &consumers,
                              std::vector<Batch> &results)
    {
        if (input == CSD_SQL_INVALID_NODE) return;
        if (consumers[input] > 0 && --consumers[input] == 0) {
            std::vector<Row> released;
            results[input].rows.swap(released);
            results[input].columns = 0;
        }
    }
    static void add_consumer(uint32_t input, uint32_t consumer,
                             std::vector<uint32_t> &consumers,
                             std::vector<uint32_t> &sole_consumer)
    {
        if (input == CSD_SQL_INVALID_NODE) return;
        if (consumers[input] == 0) sole_consumer[input] = consumer;
        else sole_consumer[input] = CSD_SQL_INVALID_NODE;
        ++consumers[input];
    }
    template <typename Function>
    void run_parallel(size_t task_count, Function function)
    {
        const size_t lane_budget = worker_budget();
        const size_t workers = std::min<size_t>(lane_budget, task_count);
        if (workers <= 1) {
            for (size_t task = 0; task < task_count; ++task) function(task);
            return;
        }
        std::atomic<size_t> next_task(0);
        std::vector<std::thread> threads;
        for (size_t worker = 0; worker < workers; ++worker)
            threads.push_back(std::thread([&]() {
                set_worker_budget(1);
                for (;;) {
                    const size_t task = next_task.fetch_add(1);
                    if (task >= task_count) break;
                    function(task);
                }
                clear_worker_budget();
            }));
        for (std::vector<std::thread>::iterator thread = threads.begin(); thread != threads.end(); ++thread)
            thread->join();
    }

    /*
     * Task-level parallelism and intra-operator lanes share one resource
     * budget.  Allocate lanes in proportion to the square root of estimated
     * bytes.  A purely byte-proportional split starves a smaller scan whose
     * predicate may be much denser (TPC-H Q19's part input), while an even
     * split underfeeds lineitem.  Square-root weighting models replicated FPGA
     * pipelines without either extreme.
     */
    template <typename Function>
    void run_ready_nodes(const std::vector<uint32_t> &ready, Function function)
    {
        if (ready.size() <= 1) {
            if (!ready.empty()) function(0);
            return;
        }
        if (parallel_workers_ <= 1 || ready.size() > parallel_workers_) {
            run_parallel(ready.size(), function);
            return;
        }
        std::vector<uint32_t> budgets(ready.size(), 1);
        std::vector<double> weights(ready.size(), 1.0);
        for (size_t slot = 0; slot < ready.size(); ++slot) {
            const CsdSqlPlanNode &node = nodes_[ready[slot]];
            const uint64_t work = std::max<uint64_t>(UINT64_C(1),
                std::max<uint64_t>(node.estimated_bytes, node.estimated_rows));
            weights[slot] = std::sqrt(static_cast<double>(work));
        }
        for (uint32_t lane = static_cast<uint32_t>(ready.size());
             lane < parallel_workers_; ++lane) {
            size_t selected = 0;
            double selected_score = weights[0] / (budgets[0] + 1);
            for (size_t slot = 1; slot < ready.size(); ++slot) {
                const double score = weights[slot] / (budgets[slot] + 1);
                if (score > selected_score) {
                    selected = slot;
                    selected_score = score;
                }
            }
            ++budgets[selected];
        }
        std::vector<std::thread> threads;
        threads.reserve(ready.size());
        for (size_t slot = 0; slot < ready.size(); ++slot)
            threads.push_back(std::thread([&, slot]() {
                set_worker_budget(budgets[slot]);
                function(slot);
                clear_worker_budget();
            }));
        for (std::vector<std::thread>::iterator thread = threads.begin();
             thread != threads.end(); ++thread)
            thread->join();
    }

    size_t worker_budget() const
    {
        if (!worker_budget_key_valid_) return 1;
        const uintptr_t encoded = reinterpret_cast<uintptr_t>(
            pthread_getspecific(worker_budget_key_));
        return encoded == 0 ? parallel_workers_ : encoded - 1;
    }

    void set_worker_budget(uint32_t budget)
    {
        if (worker_budget_key_valid_)
            pthread_setspecific(worker_budget_key_, reinterpret_cast<void *>(
                static_cast<uintptr_t>(budget) + 1));
    }

    void clear_worker_budget()
    {
        if (worker_budget_key_valid_) pthread_setspecific(worker_budget_key_, NULL);
    }

    bool try_acquire_operator_memory(uint64_t bytes)
    {
        std::lock_guard<std::mutex> memory_guard(operator_memory_mutex_);
        if (bytes > operator_memory_capacity_ - operator_memory_used_) return false;
        operator_memory_used_ += bytes;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.operator_allocations;
            status_.operator_peak_bytes = std::max<uint64_t>(
                status_.operator_peak_bytes, operator_memory_used_);
        }
        return true;
    }

    bool try_grow_operator_memory(uint64_t bytes)
    {
        std::lock_guard<std::mutex> memory_guard(operator_memory_mutex_);
        if (bytes > operator_memory_capacity_ - operator_memory_used_) return false;
        operator_memory_used_ += bytes;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            status_.operator_peak_bytes = std::max<uint64_t>(
                status_.operator_peak_bytes, operator_memory_used_);
        }
        return true;
    }

    bool wait_acquire_operator_memory(uint64_t bytes)
    {
        std::unique_lock<std::mutex> memory_guard(operator_memory_mutex_);
        if (bytes > operator_memory_capacity_) return false;
        operator_memory_available_.wait(memory_guard, [this, bytes]() {
            return bytes <= operator_memory_capacity_ - operator_memory_used_;
        });
        operator_memory_used_ += bytes;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.operator_allocations;
            status_.operator_peak_bytes = std::max<uint64_t>(
                status_.operator_peak_bytes, operator_memory_used_);
        }
        return true;
    }

    void release_operator_memory(uint64_t bytes)
    {
        {
            std::lock_guard<std::mutex> memory_guard(operator_memory_mutex_);
            if (bytes <= operator_memory_used_) operator_memory_used_ -= bytes;
            else operator_memory_used_ = 0;
        }
        operator_memory_available_.notify_all();
    }

    class OperatorMemoryLease {
    public:
        OperatorMemoryLease(Engine *engine, uint64_t bytes, bool wait = false)
            : engine_(engine), bytes_(bytes), acquired_(
                wait ? engine_->wait_acquire_operator_memory(bytes) :
                       engine_->try_acquire_operator_memory(bytes)) {}
        ~OperatorMemoryLease()
        {
            if (acquired_) engine_->release_operator_memory(bytes_);
        }
        bool acquired() const { return acquired_; }
    private:
        Engine *engine_;
        uint64_t bytes_;
        bool acquired_;
    };

    bool execute_node(uint32_t index, Batch *output, std::vector<Batch> &results,
                      const std::vector<uint8_t> &complete,
                      const std::vector<uint32_t> &consumers,
                      const std::vector<uint32_t> &sole_consumer,
                      std::vector<uint8_t> &precomputed)
    {
        const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        const bool succeeded = execute_node_impl(index, output, results,
                                                 complete, consumers,
                                                 sole_consumer, precomputed);
        const uint64_t elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
        const uint16_t opcode = nodes_[index].opcode;
        if (opcode < CSD_SQL_PROFILE_SLOTS) {
            std::lock_guard<std::mutex> guard(status_mutex_);
            ++status_.operator_calls[opcode];
            status_.operator_ns[opcode] += elapsed_ns;
            if (succeeded) status_.operator_rows[opcode] += output->rows.size();
            if (header_->flags & CSD_SQL_PLAN_PROFILE)
                std::fprintf(stderr,
                    "CSD_NODE plan=%llu node=%u opcode=%u rows=%llu elapsed_ms=%.3f\n",
                    static_cast<unsigned long long>(header_->plan_id), index, opcode,
                    static_cast<unsigned long long>(succeeded ? output->rows.size() : 0),
                    static_cast<double>(elapsed_ns) / 1000000.0);
        }
        return succeeded;
    }

    bool execute_node_impl(uint32_t index, Batch *output, std::vector<Batch> &results,
                           const std::vector<uint8_t> &complete,
                           const std::vector<uint32_t> &consumers,
                           const std::vector<uint32_t> &sole_consumer,
                           std::vector<uint8_t> &precomputed)
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
        if (precomputed[index]) {
            Batch &input = mutable_input_batch(node.left_input, results);
            output->columns = input.columns;
            output->rows.swap(input.rows);
            return true;
        }
        switch (node.opcode) {
        case CSD_SQL_OP_SCAN: {
            const std::vector<uint32_t> pipeline = stateless_pipeline(index, consumers,
                                                                      sole_consumer);
            const uint32_t tail = pipeline.empty() ? index : pipeline.back();
            const uint32_t next = consumers[tail] == 1 ? sole_consumer[tail] :
                                  CSD_SQL_INVALID_NODE;
            const bool stream_sort = next != CSD_SQL_INVALID_NODE &&
                                     nodes_[next].opcode == CSD_SQL_OP_SORT;
            uint32_t aggregate_index = CSD_SQL_INVALID_NODE;
            uint32_t aggregate_project = CSD_SQL_INVALID_NODE;
            if (!stream_sort && next != CSD_SQL_INVALID_NODE &&
                (nodes_[next].opcode == CSD_SQL_OP_HASH_AGG ||
                 nodes_[next].opcode == CSD_SQL_OP_GROUP_AGG)) {
                aggregate_index = next;
            } else if (!stream_sort && next != CSD_SQL_INVALID_NODE &&
                       nodes_[next].opcode == CSD_SQL_OP_PROJECT &&
                       consumers[next] == 1 &&
                       sole_consumer[next] != CSD_SQL_INVALID_NODE &&
                       (nodes_[sole_consumer[next]].opcode ==
                            CSD_SQL_OP_HASH_AGG ||
                        nodes_[sole_consumer[next]].opcode ==
                            CSD_SQL_OP_GROUP_AGG)) {
                aggregate_project = next;
                aggregate_index = sole_consumer[next];
            }
            const bool stream_aggregate =
                aggregate_index != CSD_SQL_INVALID_NODE &&
                can_stream_aggregate(nodes_[aggregate_index]);
            const uint32_t join_index = streaming_join_target(index, pipeline,
                consumers, sole_consumer);
            const uint32_t build_join_index = streaming_join_build_target(
                index, pipeline, consumers, sole_consumer);
            /* Actual build bytes, rather than a planner estimate, select a
             * resident hash or a Flash-partitioned build. A spillable
             * oversized build still consumes its probe Scan by FIFO batch. */
            const bool stream_join = join_index != CSD_SQL_INVALID_NODE &&
                complete[nodes_[join_index].right_input] &&
                (batch_bytes(input_batch(nodes_[join_index].right_input,
                                         results)) <=
                     operator_memory_capacity_ ||
                 (nodes_[join_index].flags & CSD_SQL_NODE_SPILLABLE));
            bool succeeded = false;
            if (build_join_index != CSD_SQL_INVALID_NODE)
                succeeded = execute_scan_to_join_build(index, node, pipeline,
                    build_join_index, nodes_[build_join_index], output);
            else if (stream_sort)
                succeeded = execute_scan_to_sort(index, node, pipeline, next,
                                                  nodes_[next], output);
            else if (stream_aggregate)
                succeeded = execute_scan_to_aggregate(index, node, pipeline,
                    aggregate_index, nodes_[aggregate_index],
                    aggregate_project == CSD_SQL_INVALID_NODE ? NULL :
                        &nodes_[aggregate_project], output);
            else if (stream_join)
                succeeded = execute_scan_to_join(index, node, pipeline,
                    join_index, nodes_[join_index],
                    input_batch(nodes_[join_index].right_input, results),
                    output);
            else
                succeeded = execute_scan(index, node, output, &pipeline);
            if (succeeded) {
                for (std::vector<uint32_t>::const_iterator stage = pipeline.begin();
                     stage != pipeline.end(); ++stage)
                    precomputed[*stage] = 1;
                if (stream_sort) precomputed[next] = 1;
                if (stream_aggregate) {
                    if (aggregate_project != CSD_SQL_INVALID_NODE)
                        precomputed[aggregate_project] = 1;
                    precomputed[aggregate_index] = 1;
                }
                if (stream_join) precomputed[join_index] = 1;
            }
            return succeeded;
        }
        case CSD_SQL_OP_INDEX_SCAN:
            add_canonicalized_node();
            return execute_scan(index, node, output);
        case CSD_SQL_OP_FILTER:
            return execute_filter(index, node, mutable_input_batch(node.left_input, results), output,
                                  consumers[node.left_input] == 1);
        case CSD_SQL_OP_PROJECT:
            if (consumers[index] == 1 && sole_consumer[index] != CSD_SQL_INVALID_NODE &&
                (nodes_[sole_consumer[index]].opcode == CSD_SQL_OP_HASH_AGG ||
                 nodes_[sole_consumer[index]].opcode == CSD_SQL_OP_GROUP_AGG)) {
                const uint32_t aggregate_index = sole_consumer[index];
                const bool succeeded = execute_aggregate(aggregate_index, nodes_[aggregate_index],
                    input_batch(node.left_input, results), output, &node);
                if (succeeded) precomputed[aggregate_index] = 1;
                return succeeded;
            }
            return execute_project(index, node, mutable_input_batch(node.left_input, results), output,
                                   consumers[node.left_input] == 1);
        case CSD_SQL_OP_HASH_AGG:
        case CSD_SQL_OP_GROUP_AGG:
            return execute_aggregate(index, node, input_batch(node.left_input, results), output);
        case CSD_SQL_OP_HASH_JOIN:
            return execute_join(index, node, input_batch(node.left_input, results), input_batch(node.right_input, results), output, false);
        case CSD_SQL_OP_MERGE_JOIN:
            add_canonicalized_node();
            return execute_join(index, node, input_batch(node.left_input, results), input_batch(node.right_input, results), output, false);
        case CSD_SQL_OP_NL_JOIN:
            add_software_fallback_node();
            return execute_join(index, node, input_batch(node.left_input, results), input_batch(node.right_input, results), output, true);
        case CSD_SQL_OP_SORT:
            return execute_sort(index, node, mutable_input_batch(node.left_input, results), output,
                                consumers[node.left_input] == 1);
        case CSD_SQL_OP_MATERIALIZE:
            return execute_materialize(index, node, mutable_input_batch(node.left_input, results), output,
                                       consumers[node.left_input] == 1);
        case CSD_SQL_OP_SUBPLAN:
            add_canonicalized_node();
            return execute_materialize(index, node, mutable_input_batch(node.left_input, results), output,
                                       consumers[node.left_input] == 1);
        case CSD_SQL_OP_LIMIT:
            return execute_limit(index, node, mutable_input_batch(node.left_input, results), output,
                                 consumers[node.left_input] == 1);
        default: return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
        }
    }
    const Batch &input_batch(uint32_t input, const std::vector<Batch> &results) const { return results[input]; }
    Batch &mutable_input_batch(uint32_t input, std::vector<Batch> &results) const { return results[input]; }
    std::vector<uint32_t> stateless_pipeline(uint32_t input,
                                             const std::vector<uint32_t> &consumers,
                                             const std::vector<uint32_t> &sole_consumer) const
    {
        std::vector<uint32_t> pipeline;
        uint32_t current = input;
        while (consumers[current] == 1 && sole_consumer[current] != CSD_SQL_INVALID_NODE) {
            const uint32_t next = sole_consumer[current];
            const uint16_t opcode = nodes_[next].opcode;
            if (opcode != CSD_SQL_OP_FILTER && opcode != CSD_SQL_OP_PROJECT) break;
            if (opcode == CSD_SQL_OP_PROJECT && consumers[next] == 1 &&
                sole_consumer[next] != CSD_SQL_INVALID_NODE &&
                (nodes_[sole_consumer[next]].opcode == CSD_SQL_OP_HASH_AGG ||
                 nodes_[sole_consumer[next]].opcode == CSD_SQL_OP_GROUP_AGG))
                break;
            pipeline.push_back(next);
            current = next;
        }
        return pipeline;
    }

    uint32_t streaming_join_target(
        uint32_t input, const std::vector<uint32_t> &pipeline,
        const std::vector<uint32_t> &consumers,
        const std::vector<uint32_t> &sole_consumer) const
    {
        const uint32_t tail = pipeline.empty() ? input : pipeline.back();
        if (consumers[tail] != 1 ||
            sole_consumer[tail] == CSD_SQL_INVALID_NODE)
            return CSD_SQL_INVALID_NODE;
        const uint32_t join_index = sole_consumer[tail];
        const CsdSqlPlanNode &join = nodes_[join_index];
        if ((join.opcode != CSD_SQL_OP_HASH_JOIN &&
             join.opcode != CSD_SQL_OP_MERGE_JOIN) ||
            join.left_input != tail ||
            join.right_input == CSD_SQL_INVALID_NODE)
            return CSD_SQL_INVALID_NODE;
        return join_index;
    }

    uint32_t streaming_join_build_target(
        uint32_t input, const std::vector<uint32_t> &pipeline,
        const std::vector<uint32_t> &consumers,
        const std::vector<uint32_t> &sole_consumer) const
    {
        const uint32_t tail = pipeline.empty() ? input : pipeline.back();
        if (consumers[tail] != 1 ||
            sole_consumer[tail] == CSD_SQL_INVALID_NODE)
            return CSD_SQL_INVALID_NODE;
        const uint32_t join_index = sole_consumer[tail];
        const CsdSqlPlanNode &join = nodes_[join_index];
        if ((join.opcode != CSD_SQL_OP_HASH_JOIN &&
             join.opcode != CSD_SQL_OP_MERGE_JOIN) ||
            join.right_input != tail ||
            join.left_input == CSD_SQL_INVALID_NODE ||
            !has_streaming_scan_source(join.left_input, consumers,
                                       sole_consumer) ||
            !(join.flags & CSD_SQL_NODE_SPILLABLE) ||
            join.arg0 <= operator_memory_capacity_)
            return CSD_SQL_INVALID_NODE;
        return join_index;
    }

    bool has_streaming_scan_source(
        uint32_t tail, const std::vector<uint32_t> &consumers,
        const std::vector<uint32_t> &sole_consumer) const
    {
        uint32_t current = tail;
        while (current != CSD_SQL_INVALID_NODE &&
               (nodes_[current].opcode == CSD_SQL_OP_FILTER ||
                nodes_[current].opcode == CSD_SQL_OP_PROJECT)) {
            const uint32_t input = nodes_[current].left_input;
            if (input == CSD_SQL_INVALID_NODE || consumers[input] != 1 ||
                sole_consumer[input] != current)
                return false;
            current = input;
        }
        return current != CSD_SQL_INVALID_NODE &&
               nodes_[current].opcode == CSD_SQL_OP_SCAN;
    }

#include "csd_fifo.cc"
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
    bool fail(uint16_t code, uint32_t node, uint32_t detail = 0)
    {
        { std::lock_guard<std::mutex> guard(status_mutex_);
          if (!failed_) { status_.status_code = code; status_.failed_node = node;
			  status_.reserved32 = detail; failed_ = true; } }
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
    void write_profile()
    {
        CsdSqlExecutionStatus snapshot;
        {
            std::lock_guard<std::mutex> guard(status_mutex_);
            snapshot = status_;
        }
        for (uint32_t opcode = CSD_SQL_OP_SCAN; opcode < CSD_SQL_PROFILE_SLOTS; ++opcode)
            if (snapshot.operator_calls[opcode] != 0)
                std::fprintf(stderr,
                    "CSD_PROFILE plan=%llu opcode=%u calls=%llu rows=%llu elapsed_ms=%.3f\n",
                    static_cast<unsigned long long>(snapshot.plan_id), opcode,
                    static_cast<unsigned long long>(snapshot.operator_calls[opcode]),
                    static_cast<unsigned long long>(snapshot.operator_rows[opcode]),
                    static_cast<double>(snapshot.operator_ns[opcode]) / 1000000.0);
        std::fflush(stderr);
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
    uint32_t fifo_generation_;
    uint64_t operator_memory_capacity_;
    uint64_t operator_memory_used_;
    std::vector<StreamedJoinBuildState> streamed_join_builds_;
    pthread_key_t worker_budget_key_;
    bool worker_budget_key_valid_;
    CsdSqlExecutionStatus status_;
    bool failed_;
    std::mutex status_mutex_;
    std::mutex spill_mutex_;
    std::mutex fifo_mutex_;
    std::mutex operator_memory_mutex_;
    std::condition_variable operator_memory_available_;
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
    const long long result = engine.run();
    if (args != NULL) {
        uint64_t modeled_cycles = 0;
        uint64_t flash_read_bytes = 0;
        uint64_t flash_write_bytes = 0;
        uint64_t spill_read_bytes = 0;
        uint64_t spill_write_bytes = 0;
        engine.execution_model(&modeled_cycles, &flash_read_bytes,
                               &flash_write_bytes, &spill_read_bytes,
                               &spill_write_bytes);
        args->modeled_cycles = modeled_cycles;
        args->flash_read_bytes = flash_read_bytes;
        args->flash_write_bytes = flash_write_bytes;
        args->spill_read_bytes = spill_read_bytes;
        args->spill_write_bytes = spill_write_bytes;
    }
    if (args != NULL && args->cache_complete != NULL &&
        engine.cache_generation() != 0)
        (void) args->cache_complete(args->service_context,
                                    engine.cache_generation());
    return result;
}
