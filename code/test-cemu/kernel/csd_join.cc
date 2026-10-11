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

    /* Build/probe one in-memory partition. A tagged left row has a private
     * ordinal in its final column; residual expressions never see that tag. */
    bool execute_join_core(uint32_t index, const CsdSqlJoinSpec &spec,
                           const CsdSqlJoinKey *keys, const Batch &left,
                           const Batch &right, Batch *output, bool nested_loop,
                           bool tagged_left)
    {
        const bool scalar_key = join_uses_scalar_key(left, right, keys,
                                                      spec.key_count, nested_loop);
        std::unordered_map<std::string, std::vector<size_t> > hash_table;
        std::unordered_map<ScalarJoinKey, std::vector<size_t>, ScalarJoinKeyHash>
            scalar_hash_table;
        if (!nested_loop) {
            if (scalar_key) {
                scalar_hash_table.reserve(right.rows.size());
                for (size_t row_index = 0; row_index < right.rows.size(); ++row_index) {
                    ScalarJoinKey key;
                    if (make_scalar_join_key(right.rows[row_index], keys[0], false, &key))
                        scalar_hash_table[key].push_back(row_index);
                }
            } else {
                hash_table.reserve(right.rows.size());
                for (size_t row_index = 0; row_index < right.rows.size(); ++row_index) {
                    std::string key;
                    if (make_join_key(right.rows[row_index], keys, spec.key_count, false, &key))
                        hash_table[key].push_back(row_index);
                }
            }
        }
        if (tagged_left && left.columns == 0)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x401);
        const uint16_t logical_left_columns = left.columns - (tagged_left ? 1 : 0);
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
                for (size_t candidate = 0; candidate < right.rows.size(); ++candidate)
                    nested_candidates.push_back(candidate);
            }
            for (size_t left_index = begin; left_index < end; ++left_index) {
                const Row &left_row = left.rows[left_index];
                const std::vector<size_t> *candidates = &no_candidates;
                if (nested_loop) candidates = &nested_candidates;
                else if (scalar_key) {
                    ScalarJoinKey key;
                    if (make_scalar_join_key(left_row, keys[0], true, &key)) {
                        std::unordered_map<ScalarJoinKey, std::vector<size_t>,
                            ScalarJoinKeyHash>::const_iterator found =
                            scalar_hash_table.find(key);
                        if (found != scalar_hash_table.end()) candidates = &found->second;
                    }
                } else {
                    std::string key;
                    if (make_join_key(left_row, keys, spec.key_count, true, &key)) {
                        std::unordered_map<std::string, std::vector<size_t> >::const_iterator
                            found = hash_table.find(key);
                        if (found != hash_table.end()) candidates = &found->second;
                    }
                }
                const Value *ordinal = tagged_left ? &left_row.back() : NULL;
                Row logical_left(left_row.begin(),
                                 left_row.begin() + logical_left_columns);
                bool matched = false;
                for (std::vector<size_t>::const_iterator candidate = candidates->begin();
                     candidate != candidates->end(); ++candidate) {
                    if (nested_loop && spec.key_count != 0) {
                        bool equal = true;
                        for (uint16_t key = 0; key < spec.key_count; ++key) {
                            const Value &left_value = left_row[keys[key].left_key];
                            const Value &right_value = right.rows[*candidate][keys[key].right_key];
                            if (left_value.is_null || right_value.is_null ||
                                compare_value(left_value, right_value) != 0) {
                                equal = false;
                                break;
                            }
                        }
                        if (!equal) continue;
                    }
                    Row joined_row = logical_left;
                    joined_row.insert(joined_row.end(), right.rows[*candidate].begin(),
                                      right.rows[*candidate].end());
                    bool residual_match = false;
                    if (!join_residual_matches(index, spec, joined_row, &residual_match,
                                               &expression_stack)) return;
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
        output->rows.reserve(output_rows);
        for (size_t tile = 0; tile < tiles; ++tile) {
            if (!joined_ok[tile]) return false;
            output->rows.insert(output->rows.end(),
                std::make_move_iterator(joined[tile].rows.begin()),
                std::make_move_iterator(joined[tile].rows.end()));
        }
        return true;
    }

    /* GQE-style radix partitioning: both sides are serialized to Flash, then
     * one matching partition pair at a time is rebuilt and probed. */
    bool execute_partitioned_join(uint32_t index, const CsdSqlPlanNode &node,
                                  const CsdSqlJoinSpec &spec,
                                  const CsdSqlJoinKey *keys, const Batch &left,
                                  const Batch &right, Batch *output)
    {
        const uint64_t capacity = std::max<uint64_t>(1, operator_memory_capacity_);
        const uint64_t right_bytes = batch_bytes(right);
        const bool scalar_key = join_uses_scalar_key(left, right, keys,
                                                      spec.key_count, false);
        std::vector<uint64_t> right_row_bytes(right.rows.size());
        for (size_t row_index = 0; row_index < right.rows.size(); ++row_index) {
            if (!join_row_bytes(right.rows[row_index], right.columns,
                                &right_row_bytes[row_index]) ||
                right_row_bytes[row_index] > capacity)
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x406);
        }
        const size_t max_partitions = size_t(1) << 16;
        size_t partition_count = 2;
        const uint64_t required_partitions = right_bytes == 0 ? 1 :
            1 + (right_bytes - 1) / capacity;
        while (partition_count < required_partitions &&
               partition_count < max_partitions)
            partition_count <<= 1;
        if (partition_count < required_partitions)
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x402);

        /* Classify with row ordinals and serialized byte counters. This keeps
         * adaptive radix sizing independent of full Row copies. */
        std::vector<std::vector<size_t> > right_partition_rows;
        for (;;) {
            right_partition_rows.assign(partition_count, std::vector<size_t>());
            std::vector<uint64_t> partition_bytes(partition_count, 0);
            for (size_t row_index = 0; row_index < right.rows.size(); ++row_index) {
                size_t partition = 0;
                if (!join_partition_index(right.rows[row_index], keys,
                                          spec.key_count, false, scalar_key,
                                          partition_count, &partition))
                    continue;
                right_partition_rows[partition].push_back(row_index);
                if (!checked_add(partition_bytes[partition],
                                 right_row_bytes[row_index],
                                 &partition_bytes[partition]))
                    return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x406);
            }
            uint64_t largest_partition = 0;
            for (size_t partition = 0; partition < partition_count; ++partition)
                largest_partition = std::max<uint64_t>(largest_partition,
                                                       partition_bytes[partition]);
            if (largest_partition <= capacity) break;
            if (partition_count == max_partitions)
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x403);
            partition_count <<= 1;
        }

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
        std::vector<uint64_t>().swap(right_row_bytes);

        std::vector<std::vector<size_t> > left_partition_rows(partition_count);
        struct OrderedJoinRow {
            uint64_t ordinal;
            Row row;
        };
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
