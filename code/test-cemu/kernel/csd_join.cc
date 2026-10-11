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

    void note_join_chain_memory(size_t chain_count)
    {
        uint64_t bytes = 0;
        if (!checked_mul(chain_count, sizeof(SpillHandleChain), &bytes))
            bytes = std::numeric_limits<uint64_t>::max();
        std::lock_guard<std::mutex> status_guard(status_mutex_);
        status_.stream_join_chain_peak_bytes = std::max<uint64_t>(
            status_.stream_join_chain_peak_bytes, bytes);
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
        state->chains.resize(partition_count);
        state->partition_bytes.assign(partition_count, 0);
        note_join_chain_memory(partition_count);
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
            if (!append_spill_chain_handle(index, handle,
                                           &state->chains[partition]))
                return false;
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
            std::vector<SpillHandleChain> next_chains(next_count);
            std::vector<uint64_t> next_bytes(next_count, 0);
            note_join_chain_memory(next_count);
            for (size_t partition = 0; partition < state->partition_count;
                 ++partition) {
                if (!for_each_spill_chain_handle(index,
                        state->chains[partition], [&](const SpillHandle &handle) {
                    Batch batch;
                    if (!read_spill_batch(index, handle, &batch) ||
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
                        if (!append_spill_chain_handle(index, next_handle,
                                &next_chains[next_partition]))
                            return false;
                        if (!checked_add(next_bytes[next_partition],
                                selected_bytes[next_partition],
                                &next_bytes[next_partition]))
                            return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
                                        0x42b);
                    }
                    return true;
                })) return false;
            }
            state->partition_count = next_count;
            state->chains.swap(next_chains);
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

    /* Partition replay emits each joined row immediately. This is the
     * functional equivalent of a bounded FPGA output FIFO: a high-fanout
     * probe cannot first accumulate a complete partition_output Batch. */
    template <typename Consumer>
    bool stream_join_probe_rows(uint32_t index, const CsdSqlJoinSpec &spec,
                                const CsdSqlJoinKey *keys,
                                const Batch &left, const Batch &right,
                                const JoinHashState &state,
                                bool tagged_left, Consumer consumer)
    {
        if (tagged_left && left.columns == 0)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x43a);
        const uint16_t logical_left_columns = left.columns -
                                              (tagged_left ? 1 : 0);
        const std::vector<size_t> no_candidates;
        std::vector<EvalValue> expression_stack;
        for (size_t left_index = 0; left_index < left.rows.size();
             ++left_index) {
            const Row &left_row = left.rows[left_index];
            const std::vector<size_t> *candidates = &no_candidates;
            if (state.scalar_key) {
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
                Row joined_row = logical_left;
                joined_row.insert(joined_row.end(),
                    right.rows[*candidate].begin(),
                    right.rows[*candidate].end());
                bool residual_match = false;
                if (!join_residual_matches(index, spec, joined_row,
                                           &residual_match,
                                           &expression_stack))
                    return false;
                if (!residual_match) continue;
                matched = true;
                if (spec.join_kind == CSD_SQL_JOIN_SEMI) {
                    if (tagged_left) logical_left.push_back(*ordinal);
                    if (!consumer(std::move(logical_left))) return false;
                    break;
                }
                if (spec.join_kind != CSD_SQL_JOIN_ANTI) {
                    if (tagged_left) joined_row.push_back(*ordinal);
                    if (!consumer(std::move(joined_row))) return false;
                }
            }
            if (spec.join_kind == CSD_SQL_JOIN_ANTI && !matched) {
                if (tagged_left) logical_left.push_back(*ordinal);
                if (!consumer(std::move(logical_left))) return false;
            } else if (spec.join_kind == CSD_SQL_JOIN_LEFT && !matched) {
                logical_left.resize(logical_left_columns + right.columns);
                if (tagged_left) logical_left.push_back(*ordinal);
                if (!consumer(std::move(logical_left))) return false;
            }
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

    struct JoinOrdinalRunState {
        CsdSqlSortSpec sort_spec;
        CsdSqlSortKey sort_key;
        std::vector<SpillHandle> levels;
        std::vector<uint8_t> occupied;
        uint16_t visible_columns;
        uint64_t rows;
        uint64_t merge_rows;
        bool configured;
        JoinOrdinalRunState()
            : visible_columns(0), rows(0), merge_rows(0), configured(false)
        {
            std::memset(&sort_spec, 0, sizeof(sort_spec));
            std::memset(&sort_key, 0, sizeof(sort_key));
            sort_spec.key_count = 1;
        }
    };

    void note_join_ordinal_memory(const JoinOrdinalRunState &state)
    {
        uint64_t handles = 0, occupied = 0, bytes = 0;
        if (!checked_mul(state.levels.size(), sizeof(SpillHandle), &handles) ||
            !checked_mul(state.occupied.size(), sizeof(uint8_t), &occupied) ||
            !checked_add(handles, occupied, &bytes))
            bytes = std::numeric_limits<uint64_t>::max();
        std::lock_guard<std::mutex> status_guard(status_mutex_);
        status_.stream_join_ordinal_peak_bytes = std::max<uint64_t>(
            status_.stream_join_ordinal_peak_bytes, bytes);
    }

    bool configure_join_ordinal_runs(uint32_t index, uint16_t visible_columns,
                                     JoinOrdinalRunState *state)
    {
        if (state->configured)
            return state->visible_columns == visible_columns ? true :
                fail(CSD_SQL_STATUS_BAD_NODE, index, 0x430);
        if (visible_columns == std::numeric_limits<uint16_t>::max())
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x431);
        state->visible_columns = visible_columns;
        state->sort_key.key_column = visible_columns;
        state->configured = true;
        return true;
    }

    bool merge_join_ordinal_handles(uint32_t index,
                                    JoinOrdinalRunState *state,
                                    const SpillHandle &older,
                                    const SpillHandle &newer,
                                    SpillHandle *merged)
    {
        uint64_t rows = 0;
        if (!checked_add(older.row_count, newer.row_count, &rows) ||
            !merge_spill_runs(index, state->visible_columns + 1,
                              &state->sort_spec, &state->sort_key,
                              older, newer, merged))
            return false;
        if (!checked_add(state->merge_rows, rows, &state->merge_rows))
            state->merge_rows = std::numeric_limits<uint64_t>::max();
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_ordinal_merge_records;
            uint64_t total = 0;
            if (checked_add(status_.stream_join_ordinal_merge_rows, rows,
                            &total))
                status_.stream_join_ordinal_merge_rows = total;
            else
                status_.stream_join_ordinal_merge_rows =
                    std::numeric_limits<uint64_t>::max();
        }
        return true;
    }

    bool append_join_ordinal_run(uint32_t index, Batch *tagged,
                                 JoinOrdinalRunState *state)
    {
        if (tagged->rows.empty()) return true;
        if (!state->configured ||
            tagged->columns != state->visible_columns + 1)
            return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x432);
        int64_t previous_ordinal = -1;
        for (std::vector<Row>::const_iterator row = tagged->rows.begin();
             row != tagged->rows.end(); ++row)
            if (row->size() != tagged->columns || row->back().is_null ||
                row->back().type != CSD_SQL_TYPE_INT64 ||
                row->back().integer < 0 ||
                row->back().integer < previous_ordinal)
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x433);
            else
                previous_ordinal = row->back().integer;

        SpillHandle carry;
        const uint64_t rows = tagged->rows.size();
        if (!spill_batch(index, *tagged, &carry)) return false;
        if (!checked_add(state->rows, rows, &state->rows))
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x434);
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_ordinal_runs;
        }
        size_t level = 0;
        while (true) {
            if (level == state->levels.size()) {
                state->levels.resize(level + 1);
                state->occupied.resize(level + 1, 0);
                note_join_ordinal_memory(*state);
            }
            if (!state->occupied[level]) {
                state->levels[level] = carry;
                state->occupied[level] = 1;
                return true;
            }
            SpillHandle merged;
            if (!merge_join_ordinal_handles(index, state,
                    state->levels[level], carry, &merged))
                return false;
            state->occupied[level] = 0;
            carry = merged;
            ++level;
        }
    }

    bool buffer_join_ordinal_row(uint32_t index, Row *row, Batch *pending,
                                 uint64_t *pending_bytes,
                                 JoinOrdinalRunState *state)
    {
        uint64_t row_bytes = 0;
        if (!join_row_bytes(*row, pending->columns, &row_bytes) ||
            header_->spill_range == CSD_SQL_INVALID_RANGE ||
            header_->spill_range >= static_cast<uint32_t>(args_->numr) ||
            args_->mr_len[header_->spill_range] <= 0)
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x438);
        const uint64_t capacity = static_cast<uint64_t>(
            args_->mr_len[header_->spill_range]);
        if (row_bytes > capacity)
            return fail(CSD_SQL_STATUS_FIFO_TOO_SMALL, index, 0x439);
        if (!pending->rows.empty() && row_bytes > capacity - *pending_bytes) {
            if (!flush_join_ordinal_rows(index, pending, pending_bytes,
                                         state))
                return false;
        }
        pending->rows.push_back(std::move(*row));
        *pending_bytes += row_bytes;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            status_.stream_join_output_peak_bytes = std::max<uint64_t>(
                status_.stream_join_output_peak_bytes, *pending_bytes);
        }
        return true;
    }

    bool flush_join_ordinal_rows(uint32_t index, Batch *pending,
                                 uint64_t *pending_bytes,
                                 JoinOrdinalRunState *state)
    {
        if (pending->rows.empty()) return true;
        const uint64_t rows = pending->rows.size();
        if (!append_join_ordinal_run(index, pending, state)) return false;
        std::vector<Row>().swap(pending->rows);
        *pending_bytes = 0;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_output_buffers;
            uint64_t total = 0;
            if (checked_add(status_.stream_join_output_rows, rows, &total))
                status_.stream_join_output_rows = total;
            else
                status_.stream_join_output_rows =
                    std::numeric_limits<uint64_t>::max();
        }
        return true;
    }

    bool final_join_ordinal_handle(uint32_t index,
                                   JoinOrdinalRunState *state,
                                   SpillHandle *final_handle,
                                   bool *have_final)
    {
        *have_final = false;
        for (size_t level = state->levels.size(); level > 0; --level) {
            const size_t slot = level - 1;
            if (!state->occupied[slot]) continue;
            if (!*have_final) {
                *final_handle = state->levels[slot];
                *have_final = true;
                continue;
            }
            SpillHandle merged;
            if (!merge_join_ordinal_handles(index, state, *final_handle,
                                             state->levels[slot], &merged))
                return false;
            *final_handle = merged;
        }
        return true;
    }

    bool stream_join_ordinal_handle(
        uint32_t index, const JoinOrdinalRunState &state,
        const SpillHandle &handle, Batch *output,
        const std::function<bool(Batch &)> *consumer = NULL)
    {
        output->columns = state.visible_columns;
        if (consumer == NULL) output->rows.reserve(handle.row_count);
        uint64_t emitted_rows = 0;
        if (!stream_spill_batch(index, handle,
                [&](Batch &chunk) {
                    if (chunk.columns != state.visible_columns + 1)
                        return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x435);
                    for (std::vector<Row>::iterator row =
                             chunk.rows.begin(); row != chunk.rows.end();
                         ++row) {
                        if (row->size() != chunk.columns ||
                            row->back().is_null ||
                            row->back().type != CSD_SQL_TYPE_INT64 ||
                            row->back().integer < 0)
                            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x436);
                        row->pop_back();
                    }
                    chunk.columns = state.visible_columns;
                    const uint64_t rows = chunk.rows.size();
                    if (!checked_add(emitted_rows, rows, &emitted_rows))
                        return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x43b);
                    if (consumer != NULL) {
                        const uint64_t bytes = batch_bytes(chunk);
                        {
                            std::lock_guard<std::mutex> status_guard(
                                status_mutex_);
                            ++status_.stream_join_downstream_batches;
                            uint64_t total = 0;
                            if (checked_add(
                                    status_.stream_join_downstream_rows,
                                    rows, &total))
                                status_.stream_join_downstream_rows = total;
                            else
                                status_.stream_join_downstream_rows =
                                    std::numeric_limits<uint64_t>::max();
                            status_.stream_join_downstream_peak_bytes =
                                std::max<uint64_t>(
                                    status_.stream_join_downstream_peak_bytes,
                                    bytes);
                        }
                        if (!(*consumer)(chunk)) return false;
                    } else {
                        output->rows.insert(output->rows.end(),
                            std::make_move_iterator(chunk.rows.begin()),
                            std::make_move_iterator(chunk.rows.end()));
                    }
                    return true;
                }))
            return false;
        return emitted_rows == state.rows ? true :
            fail(CSD_SQL_STATUS_BAD_ABI, index, 0x437);
    }

    bool finish_join_ordinal_runs(uint32_t index,
                                  JoinOrdinalRunState *state,
                                  Batch *output)
    {
        output->columns = state->visible_columns;
        SpillHandle final_handle;
        bool have_final = false;
        if (!final_join_ordinal_handle(index, state, &final_handle,
                                       &have_final))
            return false;
        if (!have_final) return true;
        return stream_join_ordinal_handle(index, *state, final_handle,
                                          output);
    }

    /* A partitioned Join already owns a stable Flash run. Feed that run to a
     * compatible downstream Aggregate one decoded chunk at a time instead of
     * reconstructing the complete visible Join Batch. If the runtime hash
     * state exceeds the shared arena, replay the still-intact run into
     * bounded Aggregate radix chains and aggregate one partition at a time. */
    bool apply_join_downstream_pipeline(
        Batch &input, const std::vector<uint32_t> &pipeline, Batch *output)
    {
        Batch current;
        current.columns = input.columns;
        current.rows.swap(input.rows);
        for (std::vector<uint32_t>::const_iterator stage = pipeline.begin();
             stage != pipeline.end(); ++stage) {
            Batch next;
            if (nodes_[*stage].opcode == CSD_SQL_OP_FILTER) {
                if (!execute_filter(*stage, nodes_[*stage], current, &next,
                                    true))
                    return false;
            } else if (nodes_[*stage].opcode == CSD_SQL_OP_PROJECT) {
                if (!execute_project(*stage, nodes_[*stage], current, &next,
                                     true))
                    return false;
            } else {
                return fail(CSD_SQL_STATUS_BAD_NODE, *stage, 0x43c);
            }
            current.columns = next.columns;
            current.rows.swap(next.rows);
        }
        output->columns = current.columns;
        output->rows.swap(current.rows);
        return true;
    }

    bool finish_join_ordinal_to_aggregate(
        uint32_t join_index, JoinOrdinalRunState *runs,
        uint32_t aggregate_index, const CsdSqlPlanNode &aggregate_node,
        const std::vector<uint32_t> &pipeline, Batch *output)
    {
        SpillHandle final_handle;
        bool have_final = false;
        if (!final_join_ordinal_handle(join_index, runs, &final_handle,
                                       &have_final))
            return false;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_downstream_calls;
        }

        bool capacity_exceeded = false;
        uint64_t required_bytes = 0;
        uint64_t processed_rows = 0;
        uint16_t aggregate_input_columns = 0;
        std::vector<uint16_t> group_columns;
        {
            StreamingAggregateState aggregate_state;
            Batch discarded;
            discarded.columns = runs->visible_columns;
            bool streamed = true;
            if (have_final) {
                std::function<bool(Batch &)> consumer = [&](Batch &chunk) {
                    Batch prepared;
                    if (!apply_join_downstream_pipeline(chunk, pipeline,
                                                        &prepared))
                        return false;
                    return consume_streaming_aggregate_batch(
                        aggregate_index, aggregate_node, prepared, NULL,
                        &aggregate_state, true, &capacity_exceeded,
                        &required_bytes);
                };
                streamed = stream_join_ordinal_handle(join_index, *runs,
                    final_handle, &discarded, &consumer);
            }
            if (streamed && !aggregate_state.configured) {
                Batch prepared;
                if (!apply_join_downstream_pipeline(discarded, pipeline,
                                                    &prepared))
                    streamed = false;
                if (streamed)
                streamed = configure_streaming_aggregate(aggregate_index,
                    aggregate_node, prepared.columns, NULL,
                    &aggregate_state);
            }
            if (streamed)
                streamed = finish_streaming_aggregate(aggregate_index,
                    aggregate_node, &aggregate_state, output);
            if (capacity_exceeded && aggregate_state.configured) {
                processed_rows = aggregate_state.input_rows;
                aggregate_input_columns = aggregate_state.input_columns;
                group_columns = aggregate_state.group_columns;
            }
            release_streaming_aggregate_memory(&aggregate_state);
            if (streamed) return true;
        }
        if (failed_ || !capacity_exceeded) return false;

        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_join_downstream_fallbacks;
            ++status_.stream_aggregate_partitioned_calls;
        }
        uint64_t state_estimate = required_bytes;
        if (processed_rows != 0 && runs->rows > processed_rows) {
            uint64_t scaled = 0;
            if (checked_mul(required_bytes, runs->rows, &scaled)) {
                state_estimate = scaled / processed_rows;
                if (scaled % processed_rows != 0 &&
                    state_estimate != std::numeric_limits<uint64_t>::max())
                    ++state_estimate;
            } else {
                state_estimate = std::numeric_limits<uint64_t>::max();
            }
        }
        StreamingAggregatePartitions partitions;
        if (!configure_streaming_aggregate_partitions(aggregate_index,
                aggregate_input_columns, group_columns, state_estimate, 0,
                &partitions))
            return false;
        Batch discarded;
        discarded.columns = runs->visible_columns;
        std::function<bool(Batch &)> partition_consumer = [&](Batch &chunk) {
            Batch prepared;
            if (!apply_join_downstream_pipeline(chunk, pipeline, &prepared))
                return false;
            return consume_streaming_aggregate_partition_batch(
                aggregate_index, prepared, &partitions);
        };
        if (have_final && !stream_join_ordinal_handle(join_index, *runs,
                final_handle, &discarded, &partition_consumer))
            return false;
        return finish_streaming_aggregate_partitions(aggregate_index,
            aggregate_node, &partitions, output);
    }

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
        std::vector<SpillHandleChain> right_chains;
        std::vector<SpillHandleChain> left_chains;
        JoinOrdinalRunState ordinal_runs;
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

        state->right_chains.resize(partition_count);
        state->left_chains.resize(partition_count);
        note_join_chain_memory(partition_count * 2);
        for (size_t partition = 0; partition < partition_count;
             ++partition) {
            if (!right_partition_rows[partition].empty()) {
                SpillHandle handle;
                if (!spill_selected_rows(index, right,
                                         right_partition_rows[partition], false,
                                         &handle))
                    return false;
                if (!append_spill_chain_handle(index, handle,
                        &state->right_chains[partition]))
                    return false;
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
        if (!configure_join_ordinal_runs(index, output->columns,
                                         &state->ordinal_runs))
            return false;
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
        Batch unmatched;
        unmatched.columns = state->ordinal_runs.visible_columns + 1;
        uint64_t unmatched_bytes = 0;
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
            Row unmatched_row = left.rows[row_index];
            if (state->spec->join_kind == CSD_SQL_JOIN_LEFT)
                unmatched_row.resize(left.columns + state->right_columns);
            Value ordinal;
            ordinal.is_null = false;
            ordinal.type = CSD_SQL_TYPE_INT64;
            ordinal.integer = static_cast<int64_t>(state->input_rows +
                                                   row_index);
            unmatched_row.push_back(std::move(ordinal));
            if (!buffer_join_ordinal_row(index, &unmatched_row, &unmatched,
                                         &unmatched_bytes,
                                         &state->ordinal_runs))
                return false;
        }
        if (!flush_join_ordinal_rows(index, &unmatched, &unmatched_bytes,
                                     &state->ordinal_runs))
            return false;
        for (size_t partition = 0; partition < state->partition_count;
             ++partition) {
            if (partition_rows[partition].empty()) continue;
            SpillHandle handle;
            if (!spill_selected_rows(index, left,
                                     partition_rows[partition], true,
                                     &handle, state->input_rows))
                return false;
            if (!append_spill_chain_handle(index, handle,
                    &state->left_chains[partition]))
                return false;
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
            build->chains.size() != build->partition_count)
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
        state->right_chains.swap(build->chains);
        state->left_chains.resize(state->partition_count);
        note_join_chain_memory(state->partition_count * 2);
        const bool only_left = state->spec->join_kind == CSD_SQL_JOIN_SEMI ||
                               state->spec->join_kind == CSD_SQL_JOIN_ANTI;
        output->columns = left.columns +
                          (only_left ? 0 : state->right_columns);
        if (!configure_join_ordinal_runs(index, output->columns,
                                         &state->ordinal_runs))
            return false;
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
        StreamingPartitionedJoinState *state, Batch *output,
        uint32_t aggregate_index = CSD_SQL_INVALID_NODE,
        const CsdSqlPlanNode *aggregate_node = NULL,
        const std::vector<uint32_t> *aggregate_pipeline = NULL)
    {
        for (size_t partition = 0; partition < state->partition_count;
             ++partition) {
            Batch partition_right;
            partition_right.columns = state->right_columns;
            if (!for_each_spill_chain_handle(index,
                    state->right_chains[partition],
                    [&](const SpillHandle &build_handle) {
                Batch build_chunk;
                if (!read_spill_batch(index, build_handle, &build_chunk) ||
                    build_chunk.columns != state->right_columns)
                    return false;
                partition_right.rows.insert(partition_right.rows.end(),
                    std::make_move_iterator(build_chunk.rows.begin()),
                    std::make_move_iterator(build_chunk.rows.end()));
                return true;
            })) return false;
            OperatorMemoryLease partition_memory(this,
                batch_bytes(partition_right), true);
            if (!partition_memory.acquired())
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x41c);
            JoinHashState partition_hash;
            bool hash_built = false;
            if (!for_each_spill_chain_handle(index,
                    state->left_chains[partition],
                    [&](const SpillHandle &handle) {
                Batch pending;
                pending.columns = state->ordinal_runs.visible_columns + 1;
                uint64_t pending_bytes = 0;
                if (!stream_spill_batch(index, handle,
                        [&](const Batch &partition_left) {
                            if (!hash_built) {
                                build_join_hash(partition_left,
                                    partition_right, state->keys,
                                    state->spec->key_count, false,
                                    &partition_hash);
                                hash_built = true;
                            }
                            return stream_join_probe_rows(index, *state->spec,
                                state->keys, partition_left, partition_right,
                                partition_hash, true, [&](Row &&row) {
                                    if (row.empty() || row.back().is_null ||
                                        row.back().type !=
                                            CSD_SQL_TYPE_INT64 ||
                                        row.back().integer < 0 ||
                                        static_cast<uint64_t>(
                                            row.back().integer) >=
                                            state->input_rows)
                                        return fail(CSD_SQL_STATUS_BAD_ABI,
                                                    index, 0x41d);
                                    return buffer_join_ordinal_row(index, &row,
                                        &pending, &pending_bytes,
                                        &state->ordinal_runs);
                                });
                        }))
                    return false;
                return flush_join_ordinal_rows(index, &pending,
                    &pending_bytes, &state->ordinal_runs);
            })) return false;
        }
        if (aggregate_node != NULL) {
            if (aggregate_index == CSD_SQL_INVALID_NODE ||
                aggregate_pipeline == NULL ||
                !finish_join_ordinal_to_aggregate(index,
                    &state->ordinal_runs, aggregate_index, *aggregate_node,
                    *aggregate_pipeline, output))
                return false;
        } else if (!finish_join_ordinal_runs(index, &state->ordinal_runs,
                                             output)) {
            return false;
        }

        uint64_t input_rows = 0, partition_work = 0, modeled_work = 0;
        if (!checked_add(state->input_rows, state->right_rows,
                         &input_rows) ||
            !checked_mul(input_rows, 3, &partition_work) ||
            !checked_add(partition_work, state->ordinal_runs.rows,
                         &modeled_work) ||
            !checked_add(modeled_work, state->ordinal_runs.merge_rows,
                         &modeled_work))
            modeled_work = std::numeric_limits<uint64_t>::max();
        add_cycles(modeled_work, node);
        if (node.opcode == CSD_SQL_OP_MERGE_JOIN)
            add_canonicalized_node();
        return true;
    }

    bool execute_scan_to_partitioned_join(
        uint32_t scan_index, const CsdSqlPlanNode &scan_node,
        const std::vector<uint32_t> &pipeline, uint32_t join_index,
        const CsdSqlPlanNode &join_node, const Batch &right, Batch *output,
        uint32_t aggregate_index = CSD_SQL_INVALID_NODE,
        const CsdSqlPlanNode *aggregate_node = NULL,
        const std::vector<uint32_t> *aggregate_pipeline = NULL)
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
            &state, output, aggregate_index, aggregate_node,
            aggregate_pipeline);
    }

    bool execute_scan_to_streamed_build_join(
        uint32_t scan_index, const CsdSqlPlanNode &scan_node,
        const std::vector<uint32_t> &pipeline, uint32_t join_index,
        const CsdSqlPlanNode &join_node, Batch *output,
        uint32_t aggregate_index = CSD_SQL_INVALID_NODE,
        const CsdSqlPlanNode *aggregate_node = NULL,
        const std::vector<uint32_t> *aggregate_pipeline = NULL)
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
            &state, output, aggregate_index, aggregate_node,
            aggregate_pipeline);
    }

    bool execute_scan_to_join(uint32_t scan_index,
                              const CsdSqlPlanNode &scan_node,
                              const std::vector<uint32_t> &pipeline,
                              uint32_t join_index,
                              const CsdSqlPlanNode &join_node,
                              const Batch &right,
                              Batch *output,
                              uint32_t aggregate_index =
                                  CSD_SQL_INVALID_NODE,
                              const CsdSqlPlanNode *aggregate_node = NULL,
                              const std::vector<uint32_t> *aggregate_pipeline =
                                  NULL)
    {
        if (join_index < streamed_join_builds_.size() &&
            streamed_join_builds_[join_index].complete &&
            !streamed_join_builds_[join_index].consumed)
            return execute_scan_to_streamed_build_join(
                scan_index, scan_node, pipeline, join_index, join_node,
                output, aggregate_index, aggregate_node,
                aggregate_pipeline);
        if (batch_bytes(right) > operator_memory_capacity_)
            return execute_scan_to_partitioned_join(
                scan_index, scan_node, pipeline, join_index, join_node,
                right, output, aggregate_index, aggregate_node,
                aggregate_pipeline);
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

        const bool only_left = spec.join_kind == CSD_SQL_JOIN_SEMI ||
                               spec.join_kind == CSD_SQL_JOIN_ANTI;
        output->columns = left.columns + (only_left ? 0 : right.columns);
        JoinOrdinalRunState ordinal_runs;
        if (!configure_join_ordinal_runs(index, output->columns,
                                         &ordinal_runs))
            return false;
        Batch unmatched;
        unmatched.columns = output->columns + 1;
        uint64_t unmatched_bytes = 0;
        std::vector<std::vector<size_t> > left_partition_rows(partition_count);
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
            Row unmatched_row = left.rows[row_index];
            if (spec.join_kind == CSD_SQL_JOIN_LEFT)
                unmatched_row.resize(left.columns + right.columns);
            Value ordinal;
            ordinal.is_null = false;
            ordinal.type = CSD_SQL_TYPE_INT64;
            ordinal.integer = static_cast<int64_t>(row_index);
            unmatched_row.push_back(std::move(ordinal));
            if (!buffer_join_ordinal_row(index, &unmatched_row, &unmatched,
                                         &unmatched_bytes, &ordinal_runs))
                return false;
        }
        if (!flush_join_ordinal_rows(index, &unmatched, &unmatched_bytes,
                                     &ordinal_runs))
            return false;

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
            JoinHashState partition_hash;
            bool hash_built = false;
            Batch pending;
            pending.columns = ordinal_runs.visible_columns + 1;
            uint64_t pending_bytes = 0;
            if (!stream_spill_batch(index, handles[partition].left,
                    [&](const Batch &partition_left) {
                        if (!hash_built) {
                            build_join_hash(partition_left, partition_right,
                                            keys, spec.key_count, false,
                                            &partition_hash);
                            hash_built = true;
                        }
                        return stream_join_probe_rows(index, spec, keys,
                            partition_left, partition_right, partition_hash,
                            true, [&](Row &&row) {
                                if (row.empty() || row.back().is_null ||
                                    row.back().type != CSD_SQL_TYPE_INT64 ||
                                    row.back().integer < 0 ||
                                    static_cast<uint64_t>(
                                        row.back().integer) >=
                                        left.rows.size())
                                    return fail(CSD_SQL_STATUS_BAD_ABI, index,
                                                0x405);
                                return buffer_join_ordinal_row(index, &row,
                                    &pending, &pending_bytes, &ordinal_runs);
                            });
                    }))
                return false;
            if (!flush_join_ordinal_rows(index, &pending, &pending_bytes,
                                         &ordinal_runs))
                return false;
        }
        if (!finish_join_ordinal_runs(index, &ordinal_runs, output))
            return false;

        uint64_t input_rows = 0, partition_work = 0, modeled_work = 0;
        if (!checked_add(left.rows.size(), right.rows.size(), &input_rows) ||
            !checked_mul(input_rows, 3, &partition_work) ||
            !checked_add(partition_work, output->rows.size(), &modeled_work) ||
            !checked_add(modeled_work, ordinal_runs.merge_rows,
                         &modeled_work))
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
