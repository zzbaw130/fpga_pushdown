    struct ScalarJoinKey {
        int64_t integer;
        int16_t scale;
        uint8_t type;

        bool operator==(const ScalarJoinKey &other) const
        {
            return integer == other.integer && scale == other.scale && type == other.type;
        }
    };

    struct ScalarJoinKeyHash {
        size_t operator()(const ScalarJoinKey &key) const
        {
            uint64_t mixed = static_cast<uint64_t>(key.integer);
            mixed ^= mixed >> 33;
            mixed *= UINT64_C(0xff51afd7ed558ccd);
            mixed ^= mixed >> 33;
            mixed ^= static_cast<uint64_t>(static_cast<uint16_t>(key.scale)) << 8;
            mixed ^= key.type;
            return static_cast<size_t>(mixed);
        }
    };

    static bool make_scalar_join_key(const Row &row, const CsdSqlJoinKey &key,
                                     bool left_side, ScalarJoinKey *result)
    {
        const Value &value = row[left_side ? key.left_key : key.right_key];
        if (value.is_null || value.type == CSD_SQL_TYPE_BYTES) return false;
        result->type = value.type;
        result->integer = value.integer;
        result->scale = value.type == CSD_SQL_TYPE_NUMERIC ? value.numeric_scale : 0;
        while (result->scale > 0 && result->integer % 10 == 0) {
            result->integer /= 10;
            --result->scale;
        }
        return true;
    }

    static bool make_join_key(const Row &row, const CsdSqlJoinKey *keys, uint16_t count,
                              bool left_side, std::string *key)
    {
        key->clear();
        for (uint16_t index = 0; index < count; ++index) {
            const uint16_t column = left_side ? keys[index].left_key : keys[index].right_key;
            const Value &value = row[column];
            if (value.is_null) return false;
            const std::string component = value_key(value);
            const uint32_t length = component.size();
            key->append(reinterpret_cast<const char *>(&length), sizeof(length));
            key->append(component);
        }
        return true;
    }

    static bool join_uses_scalar_key(const Batch &left, const Batch &right,
                                     const CsdSqlJoinKey *keys, uint16_t key_count,
                                     bool nested_loop)
    {
        return !nested_loop && key_count == 1 &&
            (left.rows.empty() || left.rows[0][keys[0].left_key].type != CSD_SQL_TYPE_BYTES) &&
            (right.rows.empty() || right.rows[0][keys[0].right_key].type != CSD_SQL_TYPE_BYTES);
    }

    static bool join_partition_index(const Row &row, const CsdSqlJoinKey *keys,
                                     uint16_t key_count, bool left_side,
                                     bool scalar_key, size_t partition_count,
                                     size_t *partition)
    {
        if (scalar_key) {
            ScalarJoinKey key;
            if (!make_scalar_join_key(row, keys[0], left_side, &key)) return false;
            *partition = ScalarJoinKeyHash()(key) & (partition_count - 1);
            return true;
        }
        std::string key;
        if (!make_join_key(row, keys, key_count, left_side, &key)) return false;
        *partition = std::hash<std::string>()(key) & (partition_count - 1);
        return true;
    }

    static bool join_row_bytes(const Row &row, uint16_t columns, uint64_t *bytes)
    {
        if (!checked_mul(columns, sizeof(CsdSqlResultCell), bytes)) return false;
        for (Row::const_iterator value = row.begin(); value != row.end(); ++value)
            if (!checked_add(*bytes, value->bytes.size(), bytes)) return false;
        return true;
    }

    bool classify_join_build_partitions(
        uint32_t index, const CsdSqlJoinSpec &spec,
        const CsdSqlJoinKey *keys, const Batch &left, const Batch &right,
        size_t *partition_count, bool *scalar_key,
        std::vector<std::vector<size_t> > *partition_rows)
    {
        const uint64_t capacity = std::max<uint64_t>(
            1, operator_memory_capacity_);
        const uint64_t right_bytes = batch_bytes(right);
        *scalar_key = join_uses_scalar_key(left, right, keys,
                                           spec.key_count, false);
        std::vector<uint64_t> right_row_bytes(right.rows.size());
        for (size_t row_index = 0; row_index < right.rows.size();
             ++row_index) {
            if (!join_row_bytes(right.rows[row_index], right.columns,
                                &right_row_bytes[row_index]) ||
                right_row_bytes[row_index] > capacity)
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x406);
        }
        const size_t max_partitions = size_t(1) << 16;
        *partition_count = 2;
        const uint64_t required_partitions = right_bytes == 0 ? 1 :
            1 + (right_bytes - 1) / capacity;
        while (*partition_count < required_partitions &&
               *partition_count < max_partitions)
            *partition_count <<= 1;
        if (*partition_count < required_partitions)
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x402);

        for (;;) {
            partition_rows->assign(*partition_count,
                                   std::vector<size_t>());
            std::vector<uint64_t> partition_bytes(*partition_count, 0);
            for (size_t row_index = 0; row_index < right.rows.size();
                 ++row_index) {
                size_t partition = 0;
                if (!join_partition_index(right.rows[row_index], keys,
                                          spec.key_count, false, *scalar_key,
                                          *partition_count, &partition))
                    continue;
                (*partition_rows)[partition].push_back(row_index);
                if (!checked_add(partition_bytes[partition],
                                 right_row_bytes[row_index],
                                 &partition_bytes[partition]))
                    return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x406);
            }
            uint64_t largest_partition = 0;
            for (size_t partition = 0; partition < *partition_count;
                 ++partition)
                largest_partition = std::max<uint64_t>(
                    largest_partition, partition_bytes[partition]);
            if (largest_partition <= capacity) break;
            if (*partition_count == max_partitions)
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x403);
            *partition_count <<= 1;
        }
        return true;
    }

    bool join_build_uses_scalar_key(const Batch &right,
                                    const CsdSqlJoinKey *keys,
                                    uint16_t key_count) const
    {
        return key_count == 1 &&
            (right.rows.empty() ||
             right.rows[0][keys[0].right_key].type != CSD_SQL_TYPE_BYTES);
    }

    bool configure_streamed_join_build(uint32_t index,
                                       const CsdSqlPlanNode &node,
                                       const Batch &right,
                                       StreamedJoinBuildState *state)
    {
        if (state->configured)
            return state->columns == right.columns ||
                fail(CSD_SQL_STATUS_BAD_NODE, index, 0x420);
        const CsdSqlJoinSpec *spec = NULL;
        uint64_t key_bytes = 0;
        if (!expression_as(node, sizeof(CsdSqlJoinSpec), &spec) ||
            !checked_mul(spec->key_count, sizeof(CsdSqlJoinKey),
                         &key_bytes) ||
            node.expression_length < sizeof(CsdSqlJoinSpec) + key_bytes ||
            spec->join_kind < CSD_SQL_JOIN_INNER ||
            spec->join_kind > CSD_SQL_JOIN_ANTI || spec->key_count == 0 ||
            (spec->residual_length != 0 &&
             !blob_range(spec->residual_offset, spec->residual_length)))
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index, 0x421);
        const CsdSqlJoinKey *keys =
            reinterpret_cast<const CsdSqlJoinKey *>(spec + 1);
        for (uint16_t key = 0; key < spec->key_count; ++key)
            if (keys[key].right_key >= right.columns)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index, 0x422);

        const uint64_t capacity = std::max<uint64_t>(
            1, operator_memory_capacity_);
        const uint64_t safe_bytes = std::max<uint64_t>(node.arg0, 1);
        const uint64_t required = 1 + (safe_bytes - 1) / capacity;
        const size_t max_partitions = size_t(1) << 16;
        size_t partition_count = 2;
        while (partition_count < required && partition_count < max_partitions)
            partition_count <<= 1;
        if (partition_count < required)
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x423);
        state->spec = spec;
        state->keys = keys;
        state->columns = right.columns;
        state->partition_count = partition_count;
        state->scalar_key = join_build_uses_scalar_key(
            right, keys, spec->key_count);
        state->handles.resize(partition_count);
        state->partition_bytes.assign(partition_count, 0);
        state->configured = true;
        return true;
    }

    bool consume_streamed_join_build_batch(uint32_t index,
                                           const CsdSqlPlanNode &node,
                                           Batch &right,
                                           StreamedJoinBuildState *state)
    {
        if (!configure_streamed_join_build(index, node, right, state))
            return false;
        uint64_t next_rows = 0, next_bytes = 0;
        const uint64_t batch_size = batch_bytes(right);
        if (!checked_add(state->rows, right.rows.size(), &next_rows) ||
            !checked_add(state->bytes, batch_size, &next_bytes))
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x424);
        std::vector<std::vector<size_t> > selected(state->partition_count);
        std::vector<uint64_t> selected_bytes(state->partition_count, 0);
        for (size_t row_index = 0; row_index < right.rows.size(); ++row_index) {
            uint64_t bytes = 0;
            if (!join_row_bytes(right.rows[row_index], right.columns, &bytes) ||
                bytes > operator_memory_capacity_)
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x425);
            size_t partition = 0;
            if (!join_partition_index(right.rows[row_index], state->keys,
                                      state->spec->key_count, false,
                                      state->scalar_key,
                                      state->partition_count, &partition))
                continue;
            selected[partition].push_back(row_index);
            if (!checked_add(selected_bytes[partition], bytes,
                             &selected_bytes[partition]))
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x426);
        }
        for (size_t partition = 0; partition < state->partition_count;
             ++partition) {
            if (selected[partition].empty()) continue;
            SpillHandle handle;
            if (!spill_selected_rows(index, right, selected[partition], false,
                                     &handle))
                return false;
            state->handles[partition].push_back(handle);
            if (!checked_add(state->partition_bytes[partition],
                             selected_bytes[partition],
                             &state->partition_bytes[partition]))
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x427);
        }
        state->rows = next_rows;
        state->bytes = next_bytes;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_build_batches;
            status_.stream_join_build_rows += right.rows.size();
        }
        return true;
    }

    /* The planner bound chooses the first fan-out.  Actual byte totals remain
     * authoritative: a skewed partition is streamed through a higher radix
     * mask before probe replay, without reconstructing the complete build. */
    bool rebucket_streamed_join_build(uint32_t index,
                                      StreamedJoinBuildState *state)
    {
        const uint64_t capacity = std::max<uint64_t>(
            1, operator_memory_capacity_);
        const size_t max_partitions = size_t(1) << 16;
        for (;;) {
            uint64_t largest = 0;
            for (size_t partition = 0;
                 partition < state->partition_bytes.size(); ++partition)
                largest = std::max<uint64_t>(largest,
                    state->partition_bytes[partition]);
            if (largest <= capacity) return true;
            if (state->partition_count == max_partitions)
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x428);
            const size_t next_count = state->partition_count << 1;
            {
                std::lock_guard<std::mutex> status_guard(status_mutex_);
                ++status_.stream_join_build_repartition_passes;
            }
            std::vector<std::vector<SpillHandle> > next_handles(next_count);
            std::vector<uint64_t> next_bytes(next_count, 0);
            for (size_t partition = 0; partition < state->partition_count;
                 ++partition) {
                for (std::vector<SpillHandle>::const_iterator handle =
                         state->handles[partition].begin();
                     handle != state->handles[partition].end(); ++handle) {
                    Batch batch;
                    if (!read_spill_batch(index, *handle, &batch) ||
                        batch.columns != state->columns)
                        return false;
                    std::vector<std::vector<size_t> > selected(next_count);
                    std::vector<uint64_t> selected_bytes(next_count, 0);
                    for (size_t row_index = 0; row_index < batch.rows.size();
                         ++row_index) {
                        size_t next_partition = 0;
                        uint64_t bytes = 0;
                        if (!join_row_bytes(batch.rows[row_index], batch.columns,
                                            &bytes) ||
                            !join_partition_index(batch.rows[row_index],
                                state->keys, state->spec->key_count, false,
                                state->scalar_key, next_count,
                                &next_partition))
                            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x429);
                        selected[next_partition].push_back(row_index);
                        if (!checked_add(selected_bytes[next_partition], bytes,
                                         &selected_bytes[next_partition]))
                            return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
                                        0x42a);
                    }
                    for (size_t next_partition = 0;
                         next_partition < next_count; ++next_partition) {
                        if (selected[next_partition].empty()) continue;
                        SpillHandle next_handle;
                        if (!spill_selected_rows(index, batch,
                                selected[next_partition], false,
                                &next_handle))
                            return false;
                        next_handles[next_partition].push_back(next_handle);
                        if (!checked_add(next_bytes[next_partition],
                                selected_bytes[next_partition],
                                &next_bytes[next_partition]))
                            return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
                                        0x42b);
                    }
                }
            }
            state->partition_count = next_count;
            state->handles.swap(next_handles);
            state->partition_bytes.swap(next_bytes);
        }
    }

    bool execute_scan_to_join_build(uint32_t scan_index,
                                    const CsdSqlPlanNode &scan_node,
                                    const std::vector<uint32_t> &pipeline,
                                    uint32_t join_index,
                                    const CsdSqlPlanNode &join_node,
                                    Batch *output)
    {
        StreamedJoinBuildState &state = streamed_join_builds_[join_index];
        std::function<bool(Batch &)> consumer = [&](Batch &batch) {
            return consume_streamed_join_build_batch(join_index, join_node,
                                                     batch, &state);
        };
        if (!execute_scan(scan_index, scan_node, output, &pipeline, &consumer))
            return false;
        if (!state.configured &&
            !configure_streamed_join_build(join_index, join_node, *output,
                                           &state))
            return false;
        if (!rebucket_streamed_join_build(join_index, &state))
            return false;
        state.complete = true;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_streamed_build_calls;
        }
        return true;
    }

    bool join_residual_matches(uint32_t index, const CsdSqlJoinSpec &spec, const Row &joined,
                               bool *matches, std::vector<EvalValue> *expression_stack)
    {
        if (spec.residual_length == 0) { *matches = true; return true; }
        std::vector<Value> values;
        if (!evaluate_expression(index, spec.residual_offset, spec.residual_length, joined, NULL,
                                 &values, expression_stack) ||
            values.size() != 1 || values[0].type != CSD_SQL_TYPE_BOOL)
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        *matches = !values[0].is_null && values[0].integer != 0;
        return true;
    }

    struct JoinHashState {
        bool scalar_key;
        std::unordered_map<std::string, std::vector<size_t> > hash_table;
        std::unordered_map<ScalarJoinKey, std::vector<size_t>,
                           ScalarJoinKeyHash> scalar_hash_table;
        JoinHashState() : scalar_key(false) {}
    };

    void build_join_hash(const Batch &left, const Batch &right,
                         const CsdSqlJoinKey *keys, uint16_t key_count,
                         bool nested_loop, JoinHashState *state)
    {
        state->scalar_key = join_uses_scalar_key(left, right, keys,
                                                 key_count, nested_loop);
        if (nested_loop) return;
        if (state->scalar_key) {
            state->scalar_hash_table.reserve(right.rows.size());
            for (size_t row_index = 0; row_index < right.rows.size();
                 ++row_index) {
                ScalarJoinKey key;
                if (make_scalar_join_key(right.rows[row_index], keys[0],
                                         false, &key))
                    state->scalar_hash_table[key].push_back(row_index);
            }
        } else {
            state->hash_table.reserve(right.rows.size());
            for (size_t row_index = 0; row_index < right.rows.size();
                 ++row_index) {
                std::string key;
                if (make_join_key(right.rows[row_index], keys, key_count,
                                  false, &key))
                    state->hash_table[key].push_back(row_index);
            }
        }
    }

    bool execute_join_probe(uint32_t index, const CsdSqlJoinSpec &spec,
                            const CsdSqlJoinKey *keys, const Batch &left,
                            const Batch &right, const JoinHashState &state,
                            Batch *output, bool nested_loop, bool tagged_left)
    {
        if (tagged_left && left.columns == 0)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x401);
        const uint16_t logical_left_columns = left.columns -
                                              (tagged_left ? 1 : 0);
        const bool only_left = spec.join_kind == CSD_SQL_JOIN_SEMI ||
                               spec.join_kind == CSD_SQL_JOIN_ANTI;
        output->columns = logical_left_columns + (only_left ? 0 : right.columns) +
                          (tagged_left ? 1 : 0);
        const size_t tiles = std::min<size_t>(parallel_workers_,
            std::max<size_t>(1, left.rows.size()));
        std::vector<Batch> joined(tiles);
        std::vector<uint8_t> joined_ok(tiles, 0);
        const std::vector<size_t> no_candidates;
        run_parallel(tiles, [&](size_t tile) {
            const size_t begin = left.rows.size() * tile / tiles;
            const size_t end = left.rows.size() * (tile + 1) / tiles;
            joined[tile].columns = output->columns;
            joined[tile].rows.reserve(end - begin);
            std::vector<size_t> nested_candidates;
            std::vector<EvalValue> expression_stack;
            if (nested_loop) {
                nested_candidates.reserve(right.rows.size());
                for (size_t candidate = 0; candidate < right.rows.size();
                     ++candidate)
                    nested_candidates.push_back(candidate);
            }
            for (size_t left_index = begin; left_index < end; ++left_index) {
                const Row &left_row = left.rows[left_index];
                const std::vector<size_t> *candidates = &no_candidates;
                if (nested_loop) {
                    candidates = &nested_candidates;
                } else if (state.scalar_key) {
                    ScalarJoinKey key;
                    if (make_scalar_join_key(left_row, keys[0], true, &key)) {
                        std::unordered_map<ScalarJoinKey, std::vector<size_t>,
                            ScalarJoinKeyHash>::const_iterator found =
                            state.scalar_hash_table.find(key);
                        if (found != state.scalar_hash_table.end())
                            candidates = &found->second;
                    }
                } else {
                    std::string key;
                    if (make_join_key(left_row, keys, spec.key_count, true,
                                      &key)) {
                        std::unordered_map<std::string,
                            std::vector<size_t> >::const_iterator found =
                            state.hash_table.find(key);
                        if (found != state.hash_table.end())
                            candidates = &found->second;
                    }
                }
                const Value *ordinal = tagged_left ? &left_row.back() : NULL;
                Row logical_left(left_row.begin(),
                                 left_row.begin() + logical_left_columns);
                bool matched = false;
                for (std::vector<size_t>::const_iterator candidate =
                         candidates->begin(); candidate != candidates->end();
                     ++candidate) {
                    if (nested_loop && spec.key_count != 0) {
                        bool equal = true;
                        for (uint16_t key = 0; key < spec.key_count; ++key) {
                            const Value &left_value =
                                left_row[keys[key].left_key];
                            const Value &right_value =
                                right.rows[*candidate][keys[key].right_key];
                            if (left_value.is_null || right_value.is_null ||
                                compare_value(left_value, right_value) != 0) {
                                equal = false;
                                break;
                            }
                        }
                        if (!equal) continue;
                    }
                    Row joined_row = logical_left;
                    joined_row.insert(joined_row.end(),
                        right.rows[*candidate].begin(),
                        right.rows[*candidate].end());
                    bool residual_match = false;
                    if (!join_residual_matches(index, spec, joined_row,
                                               &residual_match,
                                               &expression_stack))
                        return;
                    if (!residual_match) continue;
                    matched = true;
                    if (spec.join_kind == CSD_SQL_JOIN_SEMI) {
                        if (tagged_left) logical_left.push_back(*ordinal);
                        joined[tile].rows.push_back(std::move(logical_left));
                        break;
                    }
                    if (spec.join_kind != CSD_SQL_JOIN_ANTI) {
                        if (tagged_left) joined_row.push_back(*ordinal);
                        joined[tile].rows.push_back(std::move(joined_row));
                    }
                }
                if (spec.join_kind == CSD_SQL_JOIN_ANTI && !matched) {
                    if (tagged_left) logical_left.push_back(*ordinal);
                    joined[tile].rows.push_back(std::move(logical_left));
                } else if (spec.join_kind == CSD_SQL_JOIN_LEFT && !matched) {
                    logical_left.resize(logical_left_columns + right.columns);
                    if (tagged_left) logical_left.push_back(*ordinal);
                    joined[tile].rows.push_back(std::move(logical_left));
                }
            }
            joined_ok[tile] = 1;
        });
        size_t output_rows = 0;
        for (size_t tile = 0; tile < tiles; ++tile)
            output_rows += joined[tile].rows.size();
        output->rows.reserve(output->rows.size() + output_rows);
        for (size_t tile = 0; tile < tiles; ++tile) {
            if (!joined_ok[tile]) return false;
            output->rows.insert(output->rows.end(),
                std::make_move_iterator(joined[tile].rows.begin()),
                std::make_move_iterator(joined[tile].rows.end()));
        }
        return true;
    }

    /* Build/probe one in-memory partition. A tagged left row has a private
     * ordinal in its final column; residual expressions never see that tag. */
    bool execute_join_core(uint32_t index, const CsdSqlJoinSpec &spec,
                           const CsdSqlJoinKey *keys, const Batch &left,
                           const Batch &right, Batch *output, bool nested_loop,
                           bool tagged_left)
    {
        JoinHashState state;
        build_join_hash(left, right, keys, spec.key_count, nested_loop,
                        &state);
        return execute_join_probe(index, spec, keys, left, right, state,
                                  output, nested_loop, tagged_left);
    }

    struct StreamingJoinState {
        const CsdSqlJoinSpec *spec;
        const CsdSqlJoinKey *keys;
        const Batch *right;
        JoinHashState hash;
        uint64_t held_bytes;
        uint64_t input_rows;
        bool configured;
        StreamingJoinState()
            : spec(NULL), keys(NULL), right(NULL), held_bytes(0),
              input_rows(0), configured(false) {}
    };

    bool configure_streaming_join(uint32_t index,
                                  const CsdSqlPlanNode &node,
                                  const Batch &left,
                                  const Batch &right,
                                  Batch *output,
                                  StreamingJoinState *state)
    {
        if (state->configured) {
            const bool only_left =
                state->spec->join_kind == CSD_SQL_JOIN_SEMI ||
                state->spec->join_kind == CSD_SQL_JOIN_ANTI;
            const uint16_t expected_columns =
                left.columns + (only_left ? 0 : right.columns);
            if (state->right == &right &&
                output->columns == expected_columns)
                return true;
            return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x410);
        }
        const CsdSqlJoinSpec *spec = NULL;
        uint64_t key_bytes = 0;
        if (!expression_as(node, sizeof(CsdSqlJoinSpec), &spec) ||
            !checked_mul(spec->key_count, sizeof(CsdSqlJoinKey),
                         &key_bytes) ||
            node.expression_length < sizeof(CsdSqlJoinSpec) + key_bytes ||
            spec->join_kind < CSD_SQL_JOIN_INNER ||
            spec->join_kind > CSD_SQL_JOIN_ANTI ||
            spec->key_count == 0 ||
            (spec->residual_length != 0 &&
             !blob_range(spec->residual_offset, spec->residual_length)))
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index, 0x411);
        const CsdSqlJoinKey *keys =
            reinterpret_cast<const CsdSqlJoinKey *>(spec + 1);
        for (uint16_t key = 0; key < spec->key_count; ++key)
            if (keys[key].left_key >= left.columns ||
                keys[key].right_key >= right.columns)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index, 0x412);
        const uint64_t right_bytes = batch_bytes(right);
        if (right_bytes > operator_memory_capacity_ ||
            !wait_acquire_operator_memory(right_bytes))
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x413);
        state->held_bytes = right_bytes;
        state->spec = spec;
        state->keys = keys;
        state->right = &right;
        build_join_hash(left, right, keys, spec->key_count, false,
                        &state->hash);
        const bool only_left = spec->join_kind == CSD_SQL_JOIN_SEMI ||
                               spec->join_kind == CSD_SQL_JOIN_ANTI;
        output->columns = left.columns + (only_left ? 0 : right.columns);
        state->configured = true;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_calls;
            status_.stream_join_build_bytes += right_bytes;
        }
        return true;
    }

    void release_streaming_join_memory(StreamingJoinState *state)
    {
        if (state->held_bytes != 0) {
            release_operator_memory(state->held_bytes);
            state->held_bytes = 0;
        }
    }

    bool consume_streaming_join_batch(uint32_t index,
                                      const CsdSqlPlanNode &node,
                                      Batch &left,
                                      const Batch &right,
                                      Batch *output,
                                      StreamingJoinState *state)
    {
        if (!configure_streaming_join(index, node, left, right, output,
                                      state))
            return false;
        if (!execute_join_probe(index, *state->spec, state->keys, left,
                                right, state->hash, output, false, false))
            return false;
        state->input_rows += left.rows.size();
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_batches;
            status_.stream_join_rows += left.rows.size();
        }
        return true;
    }

    struct OrderedJoinRow {
        uint64_t ordinal;
        Row row;
    };

    struct StreamingPartitionedJoinState {
        const CsdSqlJoinSpec *spec;
        const CsdSqlJoinKey *keys;
        const Batch *right;
        uint16_t right_columns;
        uint64_t right_rows;
        uint64_t right_bytes;
        size_t partition_count;
        bool scalar_key;
        uint64_t input_rows;
        std::vector<std::vector<SpillHandle> > right_handles;
        std::vector<std::vector<SpillHandle> > left_handles;
        std::vector<OrderedJoinRow> ordered;
        bool configured;
        StreamingPartitionedJoinState()
            : spec(NULL), keys(NULL), right(NULL), right_columns(0),
              right_rows(0), right_bytes(0), partition_count(0),
              scalar_key(false), input_rows(0), configured(false) {}
    };

    /* The complete build supplies exact fan-out/skew information. Probe rows
     * arrive later as bounded FIFO batches and become independent Flash
     * records carrying global ordinals. */
    bool configure_streaming_partitioned_join(
        uint32_t index, const CsdSqlPlanNode &node, const Batch &left,
        const Batch &right, Batch *output,
        StreamingPartitionedJoinState *state)
    {
        if (state->configured) {
            const bool only_left =
                state->spec->join_kind == CSD_SQL_JOIN_SEMI ||
                state->spec->join_kind == CSD_SQL_JOIN_ANTI;
            const uint16_t expected_columns =
                left.columns + (only_left ? 0 : right.columns);
            if (state->right == &right &&
                output->columns == expected_columns)
                return true;
            return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x414);
        }
        const CsdSqlJoinSpec *spec = NULL;
        uint64_t key_bytes = 0;
        if (!expression_as(node, sizeof(CsdSqlJoinSpec), &spec) ||
            !checked_mul(spec->key_count, sizeof(CsdSqlJoinKey),
                         &key_bytes) ||
            node.expression_length < sizeof(CsdSqlJoinSpec) + key_bytes ||
            spec->join_kind < CSD_SQL_JOIN_INNER ||
            spec->join_kind > CSD_SQL_JOIN_ANTI ||
            spec->key_count == 0 ||
            (spec->residual_length != 0 &&
             !blob_range(spec->residual_offset, spec->residual_length)))
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index, 0x415);
        const CsdSqlJoinKey *keys =
            reinterpret_cast<const CsdSqlJoinKey *>(spec + 1);
        for (uint16_t key = 0; key < spec->key_count; ++key)
            if (keys[key].left_key >= left.columns ||
                keys[key].right_key >= right.columns)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index, 0x416);

        const uint64_t right_bytes = batch_bytes(right);
        std::vector<std::vector<size_t> > right_partition_rows;
        size_t partition_count = 0;
        if (!classify_join_build_partitions(index, *spec, keys, left, right,
                                             &partition_count,
                                             &state->scalar_key,
                                             &right_partition_rows))
            return false;

        state->right_handles.resize(partition_count);
        state->left_handles.resize(partition_count);
        for (size_t partition = 0; partition < partition_count;
             ++partition) {
            if (!right_partition_rows[partition].empty()) {
                SpillHandle handle;
                if (!spill_selected_rows(index, right,
                                         right_partition_rows[partition], false,
                                         &handle))
                    return false;
                state->right_handles[partition].push_back(handle);
            }
            std::vector<size_t>().swap(right_partition_rows[partition]);
        }
        state->spec = spec;
        state->keys = keys;
        state->right = &right;
        state->right_columns = right.columns;
        state->right_rows = right.rows.size();
        state->right_bytes = right_bytes;
        state->partition_count = partition_count;
        const bool only_left = spec->join_kind == CSD_SQL_JOIN_SEMI ||
                               spec->join_kind == CSD_SQL_JOIN_ANTI;
        output->columns = left.columns + (only_left ? 0 : right.columns);
        state->configured = true;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_calls;
            ++status_.stream_join_partitioned_calls;
            status_.stream_join_build_bytes += right_bytes;
        }
        return true;
    }

    bool consume_streaming_partitioned_join_batch(
        uint32_t index, const CsdSqlPlanNode &node, Batch &left,
        const Batch &right, Batch *output,
        StreamingPartitionedJoinState *state)
    {
        if (!configure_streaming_partitioned_join(index, node, left, right,
                                                  output, state))
            return false;
        return consume_streaming_partitioned_join_rows(index, left, state);
    }

    bool consume_streaming_partitioned_join_rows(
        uint32_t index, Batch &left, StreamingPartitionedJoinState *state)
    {
        uint64_t end_ordinal = 0;
        if (!checked_add(state->input_rows, left.rows.size(), &end_ordinal) ||
            end_ordinal > static_cast<uint64_t>(
                std::numeric_limits<int64_t>::max()))
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x41b);
        std::vector<std::vector<size_t> > partition_rows(
            state->partition_count);
        for (size_t row_index = 0; row_index < left.rows.size();
             ++row_index) {
            size_t partition = 0;
            if (join_partition_index(left.rows[row_index], state->keys,
                                     state->spec->key_count, true,
                                     state->scalar_key,
                                     state->partition_count, &partition)) {
                partition_rows[partition].push_back(row_index);
                continue;
            }
            if (state->spec->join_kind != CSD_SQL_JOIN_LEFT &&
                state->spec->join_kind != CSD_SQL_JOIN_ANTI)
                continue;
            OrderedJoinRow unmatched;
            unmatched.ordinal = state->input_rows + row_index;
            unmatched.row = left.rows[row_index];
            if (state->spec->join_kind == CSD_SQL_JOIN_LEFT)
                unmatched.row.resize(left.columns + state->right_columns);
            state->ordered.push_back(std::move(unmatched));
        }
        for (size_t partition = 0; partition < state->partition_count;
             ++partition) {
            if (partition_rows[partition].empty()) continue;
            SpillHandle handle;
            if (!spill_selected_rows(index, left,
                                     partition_rows[partition], true,
                                     &handle, state->input_rows))
                return false;
            state->left_handles[partition].push_back(handle);
        }
        state->input_rows = end_ordinal;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_batches;
            status_.stream_join_rows += left.rows.size();
        }
        return true;
    }

    bool configure_streaming_partitioned_join_from_build(
        uint32_t index, const CsdSqlPlanNode &node, const Batch &left,
        StreamedJoinBuildState *build, Batch *output,
        StreamingPartitionedJoinState *state)
    {
        if (state->configured) return true;
        if (!build->configured || !build->complete || build->consumed ||
            build->spec == NULL || build->keys == NULL ||
            build->handles.size() != build->partition_count)
            return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x42c);
        for (uint16_t key = 0; key < build->spec->key_count; ++key)
            if (build->keys[key].left_key >= left.columns ||
                build->keys[key].right_key >= build->columns)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index, 0x42d);
        state->spec = build->spec;
        state->keys = build->keys;
        state->right = NULL;
        state->right_columns = build->columns;
        state->right_rows = build->rows;
        state->right_bytes = build->bytes;
        state->partition_count = build->partition_count;
        state->scalar_key = build->scalar_key;
        state->right_handles.swap(build->handles);
        state->left_handles.resize(state->partition_count);
        const bool only_left = state->spec->join_kind == CSD_SQL_JOIN_SEMI ||
                               state->spec->join_kind == CSD_SQL_JOIN_ANTI;
        output->columns = left.columns +
                          (only_left ? 0 : state->right_columns);
        state->configured = true;
        build->consumed = true;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_calls;
            ++status_.stream_join_partitioned_calls;
            status_.stream_join_build_bytes += build->bytes;
        }
        (void) node;
        return true;
    }

    bool consume_streaming_partitioned_join_build_batch(
        uint32_t index, const CsdSqlPlanNode &node, Batch &left,
        StreamedJoinBuildState *build, Batch *output,
        StreamingPartitionedJoinState *state)
    {
        if (!configure_streaming_partitioned_join_from_build(
                index, node, left, build, output, state))
            return false;
        return consume_streaming_partitioned_join_rows(index, left, state);
    }

    bool finish_streaming_partitioned_join(
        uint32_t index, const CsdSqlPlanNode &node,
        StreamingPartitionedJoinState *state, Batch *output)
    {
        for (size_t partition = 0; partition < state->partition_count;
             ++partition) {
            Batch partition_right;
            partition_right.columns = state->right_columns;
            for (std::vector<SpillHandle>::const_iterator build_handle =
                     state->right_handles[partition].begin();
                 build_handle != state->right_handles[partition].end();
                 ++build_handle) {
                Batch build_chunk;
                if (!read_spill_batch(index, *build_handle, &build_chunk) ||
                    build_chunk.columns != state->right_columns)
                    return false;
                partition_right.rows.insert(partition_right.rows.end(),
                    std::make_move_iterator(build_chunk.rows.begin()),
                    std::make_move_iterator(build_chunk.rows.end()));
            }
            OperatorMemoryLease partition_memory(this,
                batch_bytes(partition_right), true);
            if (!partition_memory.acquired())
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x41c);
            for (std::vector<SpillHandle>::const_iterator handle =
                     state->left_handles[partition].begin();
                 handle != state->left_handles[partition].end(); ++handle) {
                if (!stream_spill_batch(index, *handle,
                        [&](const Batch &partition_left) {
                            Batch partition_output;
                            if (!execute_join_core(index, *state->spec,
                                    state->keys, partition_left,
                                    partition_right, &partition_output,
                                    false, true))
                                return false;
                            for (std::vector<Row>::iterator row =
                                     partition_output.rows.begin();
                                 row != partition_output.rows.end(); ++row) {
                                if (row->empty() || row->back().is_null ||
                                    row->back().type != CSD_SQL_TYPE_INT64 ||
                                    row->back().integer < 0 ||
                                    static_cast<uint64_t>(
                                        row->back().integer) >=
                                        state->input_rows)
                                    return fail(CSD_SQL_STATUS_BAD_ABI, index,
                                                0x41d);
                                OrderedJoinRow restored;
                                restored.ordinal = static_cast<uint64_t>(
                                    row->back().integer);
                                row->pop_back();
                                restored.row = std::move(*row);
                                state->ordered.push_back(std::move(restored));
                            }
                            return true;
                        }))
                    return false;
            }
        }
        std::stable_sort(state->ordered.begin(), state->ordered.end(),
            [](const OrderedJoinRow &first, const OrderedJoinRow &second) {
                return first.ordinal < second.ordinal;
            });
        output->rows.reserve(state->ordered.size());
        for (std::vector<OrderedJoinRow>::iterator row =
                 state->ordered.begin(); row != state->ordered.end(); ++row)
            output->rows.push_back(std::move(row->row));

        uint64_t input_rows = 0, partition_work = 0, modeled_work = 0;
        if (!checked_add(state->input_rows, state->right_rows,
                         &input_rows) ||
            !checked_mul(input_rows, 3, &partition_work) ||
            !checked_add(partition_work, output->rows.size(), &modeled_work))
            modeled_work = std::numeric_limits<uint64_t>::max();
        add_cycles(modeled_work, node);
        if (node.opcode == CSD_SQL_OP_MERGE_JOIN)
            add_canonicalized_node();
        return true;
    }

    bool execute_scan_to_partitioned_join(
        uint32_t scan_index, const CsdSqlPlanNode &scan_node,
        const std::vector<uint32_t> &pipeline, uint32_t join_index,
        const CsdSqlPlanNode &join_node, const Batch &right, Batch *output)
    {
        StreamingPartitionedJoinState state;
        Batch discarded;
        std::function<bool(Batch &)> consumer = [&](Batch &batch) {
            return consume_streaming_partitioned_join_batch(
                join_index, join_node, batch, right, output, &state);
        };
        if (!execute_scan(scan_index, scan_node, &discarded, &pipeline,
                          &consumer))
            return false;
        if (!state.configured &&
            !configure_streaming_partitioned_join(join_index, join_node,
                                                   discarded, right, output,
                                                   &state))
            return false;
        return finish_streaming_partitioned_join(join_index, join_node,
                                                  &state, output);
    }

    bool execute_scan_to_streamed_build_join(
        uint32_t scan_index, const CsdSqlPlanNode &scan_node,
        const std::vector<uint32_t> &pipeline, uint32_t join_index,
        const CsdSqlPlanNode &join_node, Batch *output)
    {
        StreamedJoinBuildState &build = streamed_join_builds_[join_index];
        StreamingPartitionedJoinState state;
        Batch discarded;
        std::function<bool(Batch &)> consumer = [&](Batch &batch) {
            return consume_streaming_partitioned_join_build_batch(
                join_index, join_node, batch, &build, output, &state);
        };
        if (!execute_scan(scan_index, scan_node, &discarded, &pipeline,
                          &consumer))
            return false;
        if (!state.configured &&
            !configure_streaming_partitioned_join_from_build(
                join_index, join_node, discarded, &build, output, &state))
            return false;
        return finish_streaming_partitioned_join(join_index, join_node,
                                                  &state, output);
    }

    bool execute_scan_to_join(uint32_t scan_index,
                              const CsdSqlPlanNode &scan_node,
                              const std::vector<uint32_t> &pipeline,
                              uint32_t join_index,
                              const CsdSqlPlanNode &join_node,
                              const Batch &right,
                              Batch *output)
    {
        if (join_index < streamed_join_builds_.size() &&
            streamed_join_builds_[join_index].complete &&
            !streamed_join_builds_[join_index].consumed)
            return execute_scan_to_streamed_build_join(
                scan_index, scan_node, pipeline, join_index, join_node,
                output);
        if (batch_bytes(right) > operator_memory_capacity_)
            return execute_scan_to_partitioned_join(
                scan_index, scan_node, pipeline, join_index, join_node,
                right, output);
        StreamingJoinState state;
        Batch discarded;
        std::function<bool(Batch &)> consumer = [&](Batch &batch) {
            return consume_streaming_join_batch(join_index, join_node, batch,
                                                right, output, &state);
        };
        if (!execute_scan(scan_index, scan_node, &discarded, &pipeline,
                          &consumer)) {
            release_streaming_join_memory(&state);
            return false;
        }
        if (!state.configured &&
            !configure_streaming_join(join_index, join_node, discarded,
                                      right, output, &state)) {
            release_streaming_join_memory(&state);
            return false;
        }
        uint64_t input_rows = 0, with_output = 0;
        if (!checked_add(state.input_rows, right.rows.size(), &input_rows) ||
            !checked_add(input_rows, output->rows.size(), &with_output))
            with_output = std::numeric_limits<uint64_t>::max();
        add_cycles(with_output, join_node);
        if (join_node.opcode == CSD_SQL_OP_MERGE_JOIN)
            add_canonicalized_node();
        release_streaming_join_memory(&state);
        return true;
    }

    /* GQE-style radix partitioning: both sides are serialized to Flash, then
     * one matching partition pair at a time is rebuilt and probed. */
    bool execute_partitioned_join(uint32_t index, const CsdSqlPlanNode &node,
                                  const CsdSqlJoinSpec &spec,
                                  const CsdSqlJoinKey *keys, const Batch &left,
                                  const Batch &right, Batch *output)
    {
        /* Classify with row ordinals and serialized byte counters. This keeps
         * adaptive radix sizing independent of full Row copies. */
        std::vector<std::vector<size_t> > right_partition_rows;
        size_t partition_count = 0;
        bool scalar_key = false;
        if (!classify_join_build_partitions(index, spec, keys, left, right,
                                             &partition_count, &scalar_key,
                                             &right_partition_rows))
            return false;

        struct JoinPartitionHandles {
            SpillHandle left;
            SpillHandle right;
        };
        std::vector<JoinPartitionHandles> handles(partition_count);

        /* Serialize build partitions directly from source-row ordinals. */
        for (size_t partition = 0; partition < partition_count; ++partition) {
            if (!spill_selected_rows(index, right,
                                     right_partition_rows[partition], false,
                                     &handles[partition].right))
                return false;
            std::vector<size_t>().swap(right_partition_rows[partition]);
        }
        std::vector<std::vector<size_t> >().swap(right_partition_rows);

        std::vector<std::vector<size_t> > left_partition_rows(partition_count);
        std::vector<OrderedJoinRow> ordered;
        for (size_t row_index = 0; row_index < left.rows.size(); ++row_index) {
            size_t partition = 0;
            if (join_partition_index(left.rows[row_index], keys, spec.key_count,
                                     true, scalar_key, partition_count, &partition)) {
                left_partition_rows[partition].push_back(row_index);
                continue;
            }
            if (spec.join_kind != CSD_SQL_JOIN_LEFT &&
                spec.join_kind != CSD_SQL_JOIN_ANTI)
                continue;
            OrderedJoinRow unmatched;
            unmatched.ordinal = row_index;
            unmatched.row = left.rows[row_index];
            if (spec.join_kind == CSD_SQL_JOIN_LEFT)
                unmatched.row.resize(left.columns + right.columns);
            ordered.push_back(std::move(unmatched));
        }

        /* The writer appends a private probe ordinal without copying/tagging
         * the source Row. Replay uses it to restore PostgreSQL probe order. */
        for (size_t partition = 0; partition < partition_count; ++partition) {
            if (!spill_selected_rows(index, left,
                                     left_partition_rows[partition], true,
                                     &handles[partition].left))
                return false;
            std::vector<size_t>().swap(left_partition_rows[partition]);
        }
        std::vector<std::vector<size_t> >().swap(left_partition_rows);

        for (size_t partition = 0; partition < partition_count; ++partition) {
            Batch partition_right;
            if (!read_spill_batch(index, handles[partition].right,
                                  &partition_right))
                return false;
            OperatorMemoryLease partition_memory(this,
                batch_bytes(partition_right), true);
            if (!partition_memory.acquired())
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x404);
            if (!stream_spill_batch(index, handles[partition].left,
                    [&](const Batch &partition_left) {
                        Batch partition_output;
                        if (!execute_join_core(index, spec, keys, partition_left,
                                               partition_right, &partition_output,
                                               false, true))
                            return false;
                        for (std::vector<Row>::iterator row =
                                 partition_output.rows.begin();
                             row != partition_output.rows.end(); ++row) {
                            if (row->empty() || row->back().is_null ||
                                row->back().type != CSD_SQL_TYPE_INT64 ||
                                row->back().integer < 0 ||
                                static_cast<uint64_t>(row->back().integer) >=
                                    left.rows.size())
                                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x405);
                            OrderedJoinRow restored;
                            restored.ordinal = static_cast<uint64_t>(
                                row->back().integer);
                            row->pop_back();
                            restored.row = std::move(*row);
                            ordered.push_back(std::move(restored));
                        }
                        return true;
                    }))
                return false;
        }
        std::stable_sort(ordered.begin(), ordered.end(),
            [](const OrderedJoinRow &first, const OrderedJoinRow &second) {
                return first.ordinal < second.ordinal;
            });
        const bool only_left = spec.join_kind == CSD_SQL_JOIN_SEMI ||
                               spec.join_kind == CSD_SQL_JOIN_ANTI;
        output->columns = left.columns + (only_left ? 0 : right.columns);
        output->rows.reserve(ordered.size());
        for (std::vector<OrderedJoinRow>::iterator row = ordered.begin();
             row != ordered.end(); ++row)
            output->rows.push_back(std::move(row->row));

        uint64_t input_rows = 0, partition_work = 0, modeled_work = 0;
        if (!checked_add(left.rows.size(), right.rows.size(), &input_rows) ||
            !checked_mul(input_rows, 3, &partition_work) ||
            !checked_add(partition_work, output->rows.size(), &modeled_work))
            modeled_work = std::numeric_limits<uint64_t>::max();
        add_cycles(modeled_work, node);
        return true;
    }

    /* Multi-key build/probe join. Residual predicates execute before join-kind semantics. */
    bool execute_join(uint32_t index, const CsdSqlPlanNode &node, const Batch &left,
                      const Batch &right, Batch *output, bool nested_loop)
    {
        const CsdSqlJoinSpec *spec = NULL;
        uint64_t key_bytes = 0;
        if (!expression_as(node, sizeof(CsdSqlJoinSpec), &spec) ||
            !checked_mul(spec->key_count, sizeof(CsdSqlJoinKey), &key_bytes) ||
            node.expression_length < sizeof(CsdSqlJoinSpec) + key_bytes ||
            spec->join_kind < CSD_SQL_JOIN_INNER ||
            spec->join_kind > CSD_SQL_JOIN_ANTI ||
            (!nested_loop && spec->key_count == 0) ||
            (spec->residual_length != 0 &&
             !blob_range(spec->residual_offset, spec->residual_length)))
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        const CsdSqlJoinKey *keys = reinterpret_cast<const CsdSqlJoinKey *>(spec + 1);
        for (uint16_t key = 0; key < spec->key_count; ++key)
            if (keys[key].left_key >= left.columns ||
                keys[key].right_key >= right.columns)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);

        const uint64_t right_bytes = batch_bytes(right);
        OperatorMemoryLease build_memory(this, right_bytes);
        if (!build_memory.acquired()) {
            if (!(node.flags & CSD_SQL_NODE_SPILLABLE))
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
            if (!nested_loop && right_bytes > operator_memory_capacity_)
                return execute_partitioned_join(index, node, *spec, keys,
                                                left, right, output);
            /* Compatibility path for nested loops and transient arena
             * contention: persist a protection copy, then execute normally. */
            if (!spill_batch(index, right)) return false;
        }

        if (!execute_join_core(index, *spec, keys, left, right, output,
                               nested_loop, false))
            return false;
        uint64_t work = 0;
        if (nested_loop) {
            if (!checked_mul(left.rows.size(), right.rows.size(), &work))
                work = std::numeric_limits<uint64_t>::max();
        } else if (!checked_add(left.rows.size(), right.rows.size(), &work))
            work = std::numeric_limits<uint64_t>::max();
        uint64_t with_output = 0;
        if (!checked_add(work, output->rows.size(), &with_output))
            with_output = std::numeric_limits<uint64_t>::max();
        add_cycles(with_output, node);
        return true;
    }
