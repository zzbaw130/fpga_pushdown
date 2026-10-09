    /* NASCENT2-style bounded run generation followed by stable merge passes. */
    static bool sort_less(const Row &left, const Row &right, const CsdSqlSortKey *keys, uint16_t key_count)
    {
        for (uint16_t key_index = 0; key_index < key_count; ++key_index) {
            const CsdSqlSortKey &key = keys[key_index];
            const Value &left_key = left[key.key_column];
            const Value &right_key = right[key.key_column];
            if (left_key.is_null != right_key.is_null) {
                const bool nulls_first = (key.flags & CSD_SQL_SORT_NULLS_FIRST) != 0;
                return left_key.is_null ? nulls_first : !nulls_first;
            }
            const int comparison = compare_value(left_key, right_key);
            if (comparison != 0) return (key.flags & CSD_SQL_SORT_DESC) ? comparison > 0 : comparison < 0;
        }
        return false;
    }

    bool sort_program(uint32_t index, const CsdSqlPlanNode &node,
                      uint16_t columns, const CsdSqlSortSpec **spec,
                      const CsdSqlSortKey **keys)
    {
        uint64_t keys_bytes = 0;
        if (!expression_as(node, sizeof(CsdSqlSortSpec), spec) ||
            (*spec)->key_count == 0 ||
            !checked_mul((*spec)->key_count, sizeof(CsdSqlSortKey), &keys_bytes) ||
            node.expression_length < sizeof(CsdSqlSortSpec) + keys_bytes)
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        *keys = reinterpret_cast<const CsdSqlSortKey *>((*spec) + 1);
        for (uint16_t key = 0; key < (*spec)->key_count; ++key)
            if ((*keys)[key].key_column >= columns)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        return true;
    }

    bool merge_sort_runs(uint32_t index, uint16_t columns,
                         const CsdSqlSortSpec *spec,
                         const CsdSqlSortKey *keys,
                         std::vector<SpillHandle> *runs, Batch *output)
    {
        while (runs->size() > 1) {
            std::vector<SpillHandle> merged_runs;
            for (size_t run = 0; run < runs->size(); run += 2) {
                if (run + 1 == runs->size()) {
                    merged_runs.push_back((*runs)[run]);
                    continue;
                }
                Batch left, right, merged;
                if (!read_spill_batch(index, (*runs)[run], &left) ||
                    !read_spill_batch(index, (*runs)[run + 1], &right))
                    return false;
                merged.columns = columns;
                const size_t merged_limit = spec->top_n == 0 ?
                    left.rows.size() + right.rows.size() :
                    std::min<size_t>(spec->top_n,
                        left.rows.size() + right.rows.size());
                merged.rows.reserve(merged_limit);
                size_t left_index = 0, right_index = 0;
                while (merged.rows.size() < merged_limit &&
                       left_index < left.rows.size() &&
                       right_index < right.rows.size()) {
                    if (sort_less(right.rows[right_index], left.rows[left_index],
                                  keys, spec->key_count))
                        merged.rows.push_back(std::move(right.rows[right_index++]));
                    else
                        merged.rows.push_back(std::move(left.rows[left_index++]));
                }
                while (merged.rows.size() < merged_limit &&
                       left_index < left.rows.size())
                    merged.rows.push_back(std::move(left.rows[left_index++]));
                while (merged.rows.size() < merged_limit &&
                       right_index < right.rows.size())
                    merged.rows.push_back(std::move(right.rows[right_index++]));
                SpillHandle handle;
                if (!spill_batch(index, merged, &handle)) return false;
                merged_runs.push_back(handle);
            }
            runs->swap(merged_runs);
        }
        output->columns = columns;
        return runs->empty() || read_spill_batch(index, (*runs)[0], output);
    }

    struct StreamingSortState {
        Batch run;
        std::vector<SpillHandle> runs;
        const CsdSqlSortSpec *spec;
        const CsdSqlSortKey *keys;
        uint64_t held_bytes;
        uint64_t input_rows;
        bool configured;
        StreamingSortState()
            : spec(NULL), keys(NULL), held_bytes(0), input_rows(0),
              configured(false) {}
    };

    bool configure_streaming_sort(uint32_t index, const CsdSqlPlanNode &node,
                                  uint16_t columns, StreamingSortState *state)
    {
        if (state->configured)
            return state->run.columns == columns ? true :
                fail(CSD_SQL_STATUS_BAD_NODE, index, 0x400);
        if (!sort_program(index, node, columns, &state->spec, &state->keys))
            return false;
        state->run.columns = columns;
        state->configured = true;
        return true;
    }

    bool streaming_sort_row_bytes(uint32_t index, const Row &row,
                                  uint16_t columns, uint64_t *bytes)
    {
        if (row.size() != columns ||
            !checked_mul(columns, sizeof(CsdSqlResultCell), bytes))
            return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x401);
        for (Row::const_iterator value = row.begin(); value != row.end(); ++value)
            if (!checked_add(*bytes, value->bytes.size(), bytes))
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x402);
        return true;
    }

    void release_streaming_sort_memory(StreamingSortState *state)
    {
        if (state->held_bytes != 0) {
            release_operator_memory(state->held_bytes);
            state->held_bytes = 0;
        }
    }

    bool flush_streaming_sort_run(uint32_t index, const CsdSqlPlanNode &node,
                                  StreamingSortState *state)
    {
        if (state->run.rows.empty()) return true;
        if (!(node.flags & CSD_SQL_NODE_SPILLABLE))
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x403);
        std::stable_sort(state->run.rows.begin(), state->run.rows.end(),
            [state](const Row &left, const Row &right) {
                return sort_less(left, right, state->keys,
                                 state->spec->key_count);
            });
        if (state->spec->top_n != 0 &&
            state->run.rows.size() > state->spec->top_n)
            state->run.rows.resize(state->spec->top_n);
        SpillHandle handle;
        const bool succeeded = spill_batch(index, state->run, &handle);
        release_streaming_sort_memory(state);
        std::vector<Row> released;
        state->run.rows.swap(released);
        if (!succeeded) return false;
        state->runs.push_back(handle);
        return true;
    }

    bool consume_streaming_sort_batch(uint32_t index,
                                      const CsdSqlPlanNode &node,
                                      Batch &input,
                                      StreamingSortState *state)
    {
        if (!configure_streaming_sort(index, node, input.columns, state))
            return false;
        for (std::vector<Row>::iterator row = input.rows.begin();
             row != input.rows.end(); ++row) {
            uint64_t row_bytes = 0;
            if (!streaming_sort_row_bytes(index, *row, input.columns, &row_bytes))
                return false;
            if (row_bytes > operator_memory_capacity_) {
                release_streaming_sort_memory(state);
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x404);
            }
            if (state->held_bytes == 0) {
                if (!wait_acquire_operator_memory(row_bytes))
                    return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x405);
            } else if (!try_grow_operator_memory(row_bytes)) {
                if (!flush_streaming_sort_run(index, node, state)) return false;
                if (!wait_acquire_operator_memory(row_bytes))
                    return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x406);
            }
            state->held_bytes += row_bytes;
            state->run.rows.push_back(std::move(*row));
            ++state->input_rows;
        }
        return true;
    }

    bool finish_streaming_sort(uint32_t index, const CsdSqlPlanNode &node,
                               StreamingSortState *state, Batch *output)
    {
        if (state->runs.empty()) {
            std::stable_sort(state->run.rows.begin(), state->run.rows.end(),
                [state](const Row &left, const Row &right) {
                    return sort_less(left, right, state->keys,
                                     state->spec->key_count);
                });
            if (state->spec->top_n != 0 &&
                state->run.rows.size() > state->spec->top_n)
                state->run.rows.resize(state->spec->top_n);
            output->columns = state->run.columns;
            output->rows.swap(state->run.rows);
            release_streaming_sort_memory(state);
        } else {
            if (!flush_streaming_sort_run(index, node, state)) return false;
            if (!merge_sort_runs(index, state->run.columns, state->spec,
                                 state->keys, &state->runs, output))
                return false;
        }
        const uint64_t logarithm = state->input_rows <= 1 ? 1 :
            static_cast<uint64_t>(std::ceil(std::log(
                static_cast<double>(state->input_rows)) / std::log(2.0)));
        add_cycles(state->input_rows * logarithm, node);
        return true;
    }

    bool execute_scan_to_sort(uint32_t scan_index,
                              const CsdSqlPlanNode &scan_node,
                              const std::vector<uint32_t> &pipeline,
                              uint32_t sort_index,
                              const CsdSqlPlanNode &sort_node,
                              Batch *output)
    {
        StreamingSortState state;
        Batch discarded;
        std::function<bool(Batch &)> consumer =
            [&](Batch &batch) {
                return consume_streaming_sort_batch(sort_index, sort_node,
                                                    batch, &state);
            };
        if (!execute_scan(scan_index, scan_node, &discarded, &pipeline,
                          &consumer)) {
            release_streaming_sort_memory(&state);
            return false;
        }
        if (!state.configured &&
            !configure_streaming_sort(sort_index, sort_node,
                                      discarded.columns, &state))
            return false;
        return finish_streaming_sort(sort_index, sort_node, &state, output);
    }

    bool execute_sort(uint32_t index, const CsdSqlPlanNode &node, Batch &input,
                      Batch *output, bool consume_input)
    {
        const CsdSqlSortSpec *spec = NULL;
        const CsdSqlSortKey *keys = NULL;
        if (!sort_program(index, node, input.columns, &spec, &keys)) return false;
        output->columns = input.columns;
        const size_t input_rows = input.rows.size();
        const uint64_t input_bytes = batch_bytes(input);
        OperatorMemoryLease sort_memory(this, input_bytes);
        if (sort_memory.acquired()) {
            if (consume_input) output->rows.swap(input.rows);
            else output->rows = input.rows;
            std::stable_sort(output->rows.begin(), output->rows.end(), [spec, keys](const Row &left, const Row &right) {
                return sort_less(left, right, keys, spec->key_count);
            });
        } else {
            if (!(node.flags & CSD_SQL_NODE_SPILLABLE)) return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
            const uint64_t average_row_bytes = input.rows.empty() ? 1 : std::max<uint64_t>(1, input_bytes / input.rows.size());
            const size_t run_rows = std::max<size_t>(1,
                operator_memory_capacity_ / average_row_bytes);
            std::vector<SpillHandle> runs;
            for (size_t start = 0; start < input.rows.size(); start += run_rows) {
                const size_t end = std::min(input.rows.size(), start + run_rows);
                Batch run;
                run.columns = input.columns;
                run.rows.assign(input.rows.begin() + start, input.rows.begin() + end);
                std::stable_sort(run.rows.begin(), run.rows.end(), [spec, keys](const Row &left, const Row &right) {
                    return sort_less(left, right, keys, spec->key_count);
                });
                if (spec->top_n != 0 && run.rows.size() > spec->top_n)
                    run.rows.resize(spec->top_n);
                SpillHandle handle;
                if (!spill_batch(index, run, &handle)) return false;
                runs.push_back(handle);
            }
            if (consume_input) {
                std::vector<Row> released;
                input.rows.swap(released);
            }
            if (!merge_sort_runs(index, input.columns, spec, keys, &runs, output))
                return false;
        }
        if (spec->top_n != 0 && output->rows.size() > spec->top_n) output->rows.resize(spec->top_n);
        const uint64_t logarithm = input_rows <= 1 ? 1 :
            static_cast<uint64_t>(std::ceil(std::log(static_cast<double>(input_rows)) / std::log(2.0)));
        add_cycles(input_rows * logarithm, node);
        return true;
    }
