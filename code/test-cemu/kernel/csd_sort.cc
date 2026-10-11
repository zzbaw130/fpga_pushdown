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

    bool execute_sort(uint32_t index, const CsdSqlPlanNode &node, Batch &input,
                      Batch *output, bool consume_input)
    {
        const CsdSqlSortSpec *spec = NULL;
        uint64_t keys_bytes = 0;
        if (!expression_as(node, sizeof(CsdSqlSortSpec), &spec) || spec->key_count == 0 ||
            !checked_mul(spec->key_count, sizeof(CsdSqlSortKey), &keys_bytes) ||
            node.expression_length < sizeof(CsdSqlSortSpec) + keys_bytes)
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        const CsdSqlSortKey *keys = reinterpret_cast<const CsdSqlSortKey *>(spec + 1);
        for (uint16_t key = 0; key < spec->key_count; ++key)
            if (keys[key].key_column >= input.columns) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
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
            while (runs.size() > 1) {
                std::vector<SpillHandle> merged_runs;
                for (size_t run = 0; run < runs.size(); run += 2) {
                    if (run + 1 == runs.size()) {
                        merged_runs.push_back(runs[run]);
                        continue;
                    }
                    Batch left, right, merged;
                    if (!read_spill_batch(index, runs[run], &left) ||
                        !read_spill_batch(index, runs[run + 1], &right))
                        return false;
                    merged.columns = input.columns;
                    const size_t merged_limit = spec->top_n == 0 ?
                        left.rows.size() + right.rows.size() :
                        std::min<size_t>(spec->top_n,
                            left.rows.size() + right.rows.size());
                    merged.rows.reserve(merged_limit);
                    size_t left_index = 0, right_index = 0;
                    while (merged.rows.size() < merged_limit &&
                           left_index < left.rows.size() && right_index < right.rows.size()) {
                        if (sort_less(right.rows[right_index], left.rows[left_index], keys, spec->key_count))
                            merged.rows.push_back(std::move(right.rows[right_index++]));
                        else merged.rows.push_back(std::move(left.rows[left_index++]));
                    }
                    while (merged.rows.size() < merged_limit && left_index < left.rows.size())
                        merged.rows.push_back(std::move(left.rows[left_index++]));
                    while (merged.rows.size() < merged_limit && right_index < right.rows.size())
                        merged.rows.push_back(std::move(right.rows[right_index++]));
                    SpillHandle handle;
                    if (!spill_batch(index, merged, &handle)) return false;
                    merged_runs.push_back(handle);
                }
                runs.swap(merged_runs);
            }
            if (!runs.empty() && !read_spill_batch(index, runs[0], output))
                return false;
        }
        if (spec->top_n != 0 && output->rows.size() > spec->top_n) output->rows.resize(spec->top_n);
        const uint64_t logarithm = input_rows <= 1 ? 1 :
            static_cast<uint64_t>(std::ceil(std::log(static_cast<double>(input_rows)) / std::log(2.0)));
        add_cycles(input_rows * logarithm, node);
        return true;
    }
