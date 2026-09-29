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

    bool join_residual_matches(uint32_t index, const CsdSqlJoinSpec &spec, const Row &joined, bool *matches)
    {
        if (spec.residual_length == 0) { *matches = true; return true; }
        std::vector<Value> values;
        if (!evaluate_expression(index, spec.residual_offset, spec.residual_length, joined, NULL, &values) ||
            values.size() != 1 || values[0].type != CSD_SQL_TYPE_BOOL)
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        *matches = !values[0].is_null && values[0].integer != 0;
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
            spec->join_kind < CSD_SQL_JOIN_INNER || spec->join_kind > CSD_SQL_JOIN_ANTI ||
            (!nested_loop && spec->key_count == 0) ||
            (spec->residual_length != 0 && !blob_range(spec->residual_offset, spec->residual_length)))
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        const CsdSqlJoinKey *keys = reinterpret_cast<const CsdSqlJoinKey *>(spec + 1);
        for (uint16_t key = 0; key < spec->key_count; ++key)
            if (keys[key].left_key >= left.columns || keys[key].right_key >= right.columns)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        if (!spill_if_needed(index, node, right)) return false;

        bool scalar_key = !nested_loop && spec->key_count == 1 &&
            (left.rows.empty() || left.rows[0][keys[0].left_key].type != CSD_SQL_TYPE_BYTES) &&
            (right.rows.empty() || right.rows[0][keys[0].right_key].type != CSD_SQL_TYPE_BYTES);
        std::unordered_map<std::string, std::vector<size_t> > hash_table;
        std::unordered_map<ScalarJoinKey, std::vector<size_t>, ScalarJoinKeyHash> scalar_hash_table;
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
                    if (make_join_key(right.rows[row_index], keys, spec->key_count, false, &key))
                        hash_table[key].push_back(row_index);
                }
            }
        }
        const bool only_left = spec->join_kind == CSD_SQL_JOIN_SEMI || spec->join_kind == CSD_SQL_JOIN_ANTI;
        output->columns = left.columns + (only_left ? 0 : right.columns);
        const size_t tiles = std::min<size_t>(parallel_workers_, std::max<size_t>(1, left.rows.size()));
        std::vector<Batch> joined(tiles);
        std::vector<uint8_t> joined_ok(tiles, 0);
        const std::vector<size_t> no_candidates;
        run_parallel(tiles, [&](size_t tile) {
            const size_t begin = left.rows.size() * tile / tiles;
            const size_t end = left.rows.size() * (tile + 1) / tiles;
            joined[tile].columns = output->columns;
            joined[tile].rows.reserve(end - begin);
            std::vector<size_t> nested_candidates;
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
                        std::unordered_map<ScalarJoinKey, std::vector<size_t>, ScalarJoinKeyHash>::const_iterator found =
                            scalar_hash_table.find(key);
                        if (found != scalar_hash_table.end()) candidates = &found->second;
                    }
                }
                else {
                    std::string key;
                    if (make_join_key(left_row, keys, spec->key_count, true, &key)) {
                        std::unordered_map<std::string, std::vector<size_t> >::const_iterator found = hash_table.find(key);
                        if (found != hash_table.end()) candidates = &found->second;
                    }
                }
                bool matched = false;
                for (std::vector<size_t>::const_iterator candidate = candidates->begin(); candidate != candidates->end(); ++candidate) {
                    if (nested_loop && spec->key_count != 0) {
                        bool equal = true;
                        for (uint16_t key = 0; key < spec->key_count; ++key) {
                            const Value &left_value = left_row[keys[key].left_key];
                            const Value &right_value = right.rows[*candidate][keys[key].right_key];
                            if (left_value.is_null || right_value.is_null || compare_value(left_value, right_value) != 0) {
                                equal = false;
                                break;
                            }
                        }
                        if (!equal) continue;
                    }
                    Row joined_row = left_row;
                    joined_row.insert(joined_row.end(), right.rows[*candidate].begin(), right.rows[*candidate].end());
                    bool residual_match = false;
                    if (!join_residual_matches(index, *spec, joined_row, &residual_match)) return;
                    if (!residual_match) continue;
                    matched = true;
                    if (spec->join_kind == CSD_SQL_JOIN_SEMI) {
                        joined[tile].rows.push_back(left_row);
                        break;
                    }
                    if (spec->join_kind != CSD_SQL_JOIN_ANTI)
                        joined[tile].rows.push_back(std::move(joined_row));
                }
                if (spec->join_kind == CSD_SQL_JOIN_ANTI && !matched)
                    joined[tile].rows.push_back(left_row);
                else if (spec->join_kind == CSD_SQL_JOIN_LEFT && !matched) {
                    Row joined_row = left_row;
                    joined_row.resize(left.columns + right.columns);
                    joined[tile].rows.push_back(std::move(joined_row));
                }
            }
            joined_ok[tile] = 1;
        });
        size_t output_rows = 0;
        for (size_t tile = 0; tile < tiles; ++tile) output_rows += joined[tile].rows.size();
        output->rows.reserve(output_rows);
        for (size_t tile = 0; tile < tiles; ++tile) {
            if (!joined_ok[tile]) return false;
            output->rows.insert(output->rows.end(),
                std::make_move_iterator(joined[tile].rows.begin()),
                std::make_move_iterator(joined[tile].rows.end()));
        }
        const uint64_t work = nested_loop ? left.rows.size() * right.rows.size() : left.rows.size() + right.rows.size();
        add_cycles(work + output->rows.size(), node);
        return true;
    }
