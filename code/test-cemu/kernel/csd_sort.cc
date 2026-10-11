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
        while (runs->size() > 2) {
            std::vector<SpillHandle> merged_runs;
            for (size_t run = 0; run < runs->size(); run += 2) {
                if (run + 1 == runs->size()) {
                    merged_runs.push_back((*runs)[run]);
                    continue;
                }
                SpillHandle handle;
                if (!merge_spill_runs(index, columns, spec, keys,
                                      (*runs)[run], (*runs)[run + 1],
                                      &handle))
                    return false;
                merged_runs.push_back(handle);
            }
            runs->swap(merged_runs);
        }
        output->columns = columns;
        if (runs->empty()) return true;
        if (runs->size() == 1)
            return read_spill_batch(index, (*runs)[0], output);
        return merge_spill_runs_to_batch(index, columns, spec, keys,
                                         (*runs)[0], (*runs)[1], output);
    }

    /* The final external-sort merge is already in output order. Stream its
     * rows directly into the existing DAG/result boundary instead of writing
     * one last Flash run and immediately reading that run back. This follows
     * the merge-and-stream handoff in AOE while retaining the bounded two
     * cursor working set and PostgreSQL's pre-reserved result capacity. */
    bool merge_spill_runs_to_batch(uint32_t index, uint16_t columns,
                                   const CsdSqlSortSpec *spec,
                                   const CsdSqlSortKey *keys,
                                   const SpillHandle &left_handle,
                                   const SpillHandle &right_handle,
                                   Batch *output)
    {
        SpillRowCursor left, right;
        if (!init_spill_cursor(index, left_handle, &left)) {
            finish_spill_cursor(&left);
            return false;
        }
        if (!init_spill_cursor(index, right_handle, &right)) {
            finish_spill_cursor(&left);
            finish_spill_cursor(&right);
            return false;
        }
        uint64_t input_rows = 0;
        if (left.record.column_count != columns ||
            right.record.column_count != columns ||
            !checked_add(left.record.row_count, right.record.row_count,
                         &input_rows)) {
            finish_spill_cursor(&left);
            finish_spill_cursor(&right);
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x242);
        }
        const uint64_t output_rows = spec->top_n == 0 ? input_rows :
            std::min<uint64_t>(spec->top_n, input_rows);
        if (output_rows > std::numeric_limits<size_t>::max()) {
            finish_spill_cursor(&left);
            finish_spill_cursor(&right);
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x243);
        }
        output->columns = columns;
        output->rows.clear();
        output->rows.reserve(static_cast<size_t>(output_rows));
        bool left_has = false, right_has = false;
        while (output->rows.size() < output_rows) {
            if (!ensure_spill_cursor_row(index, &left, &left_has) ||
                !ensure_spill_cursor_row(index, &right, &right_has)) {
                finish_spill_cursor(&left);
                finish_spill_cursor(&right);
                return false;
            }
            if (!left_has && !right_has) {
                finish_spill_cursor(&left);
                finish_spill_cursor(&right);
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x244);
            }
            const bool take_right = right_has &&
                (!left_has || sort_less(spill_cursor_row(right),
                                        spill_cursor_row(left), keys,
                                        spec->key_count));
            output->rows.push_back(take_right ? spill_cursor_row(right) :
                                                spill_cursor_row(left));
            if (take_right) advance_spill_cursor(&right);
            else advance_spill_cursor(&left);
        }
        finish_spill_cursor(&left);
        finish_spill_cursor(&right);

        uint64_t cell_count = 0, serialized_bytes = sizeof(SpillRecordHeader);
        if (!checked_mul(output_rows, columns, &cell_count) ||
            !checked_mul(cell_count, sizeof(CsdSqlResultCell), &cell_count) ||
            !checked_add(serialized_bytes, cell_count, &serialized_bytes))
            serialized_bytes = std::numeric_limits<uint64_t>::max();
        if (serialized_bytes != std::numeric_limits<uint64_t>::max()) {
            for (std::vector<Row>::const_iterator row = output->rows.begin();
                 row != output->rows.end(); ++row)
                for (Row::const_iterator value = row->begin();
                     value != row->end(); ++value)
                    if (!checked_add(serialized_bytes, value->bytes.size(),
                                     &serialized_bytes)) {
                        serialized_bytes =
                            std::numeric_limits<uint64_t>::max();
                        break;
                    }
        }
        {
            uint64_t total = 0;
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.sort_final_stream_calls;
            status_.sort_final_stream_rows = checked_add(
                status_.sort_final_stream_rows, output_rows, &total) ? total :
                std::numeric_limits<uint64_t>::max();
            status_.sort_final_stream_avoided_write_bytes = checked_add(
                status_.sort_final_stream_avoided_write_bytes,
                serialized_bytes, &total) ? total :
                std::numeric_limits<uint64_t>::max();
            status_.sort_final_stream_avoided_read_bytes = checked_add(
                status_.sort_final_stream_avoided_read_bytes,
                serialized_bytes, &total) ? total :
                std::numeric_limits<uint64_t>::max();
            status_.sort_merge_rows = checked_add(status_.sort_merge_rows,
                output_rows, &total) ? total :
                std::numeric_limits<uint64_t>::max();
        }
        return true;
    }

    bool merge_spill_runs(uint32_t index, uint16_t columns,
                          const CsdSqlSortSpec *spec,
                          const CsdSqlSortKey *keys,
                          const SpillHandle &left_handle,
                          const SpillHandle &right_handle,
                          SpillHandle *output_handle)
    {
        SpillRowCursor left, right;
        if (!init_spill_cursor(index, left_handle, &left)) {
            finish_spill_cursor(&left);
            return false;
        }
        if (!init_spill_cursor(index, right_handle, &right)) {
            finish_spill_cursor(&left);
            finish_spill_cursor(&right);
            return false;
        }
        uint64_t input_rows = 0, output_rows = 0, max_payload = 0;
        if (left.record.column_count != columns ||
            right.record.column_count != columns ||
            !checked_add(left.record.row_count, right.record.row_count,
                         &input_rows) ||
            !checked_add(left.record.bytes_length,
                         right.record.bytes_length, &max_payload)) {
            finish_spill_cursor(&left);
            finish_spill_cursor(&right);
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x240);
        }
        output_rows = spec->top_n == 0 ? input_rows :
            std::min<uint64_t>(spec->top_n, input_rows);
        SpillRowWriter writer;
        if (!init_spill_writer(index, columns, output_rows,
                               max_payload, &writer)) {
            finish_spill_cursor(&left);
            finish_spill_cursor(&right);
            return false;
        }
        bool left_has = false, right_has = false;
        uint64_t emitted = 0;
        while (emitted < output_rows) {
            if (!ensure_spill_cursor_row(index, &left, &left_has) ||
                !ensure_spill_cursor_row(index, &right, &right_has)) {
                finish_spill_cursor(&left);
                finish_spill_cursor(&right);
                return false;
            }
            if (!left_has && !right_has) {
                finish_spill_cursor(&left);
                finish_spill_cursor(&right);
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x241);
            }
            const bool take_right = right_has &&
                (!left_has || sort_less(spill_cursor_row(right),
                                        spill_cursor_row(left), keys,
                                        spec->key_count));
            const Row &selected = take_right ? spill_cursor_row(right) :
                                               spill_cursor_row(left);
            if (!append_spill_writer_row(index, &writer, selected)) {
                finish_spill_cursor(&left);
                finish_spill_cursor(&right);
                return false;
            }
            if (take_right) advance_spill_cursor(&right);
            else advance_spill_cursor(&left);
            ++emitted;
        }
        finish_spill_cursor(&left);
        finish_spill_cursor(&right);
        if (!finish_spill_writer(index, &writer, output_handle)) return false;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            status_.sort_merge_rows += emitted;
        }
        return true;
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
