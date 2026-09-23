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

        std::unordered_map<std::string, std::vector<size_t> > hash_table;
        if (!nested_loop) {
            for (size_t row_index = 0; row_index < right.rows.size(); ++row_index) {
                std::string key;
                if (make_join_key(right.rows[row_index], keys, spec->key_count, false, &key))
                    hash_table[key].push_back(row_index);
            }
        }
        const bool only_left = spec->join_kind == CSD_SQL_JOIN_SEMI || spec->join_kind == CSD_SQL_JOIN_ANTI;
        output->columns = left.columns + (only_left ? 0 : right.columns);
        for (std::vector<Row>::const_iterator left_row = left.rows.begin(); left_row != left.rows.end(); ++left_row) {
            std::vector<size_t> candidates;
            if (nested_loop) {
                candidates.reserve(right.rows.size());
                for (size_t candidate = 0; candidate < right.rows.size(); ++candidate)
                    candidates.push_back(candidate);
            } else {
                std::string key;
                if (make_join_key(*left_row, keys, spec->key_count, true, &key)) {
                    std::unordered_map<std::string, std::vector<size_t> >::const_iterator found = hash_table.find(key);
                    if (found != hash_table.end()) candidates = found->second;
                }
            }
            bool matched = false;
            for (std::vector<size_t>::const_iterator candidate = candidates.begin(); candidate != candidates.end(); ++candidate) {
                if (nested_loop && spec->key_count != 0) {
                    bool equal = true;
                    for (uint16_t key = 0; key < spec->key_count; ++key) {
                        const Value &left_value = (*left_row)[keys[key].left_key];
                        const Value &right_value = right.rows[*candidate][keys[key].right_key];
                        if (left_value.is_null || right_value.is_null || compare_value(left_value, right_value) != 0) {
                            equal = false;
                            break;
                        }
                    }
                    if (!equal) continue;
                }
                Row joined = *left_row;
                joined.insert(joined.end(), right.rows[*candidate].begin(), right.rows[*candidate].end());
                bool residual_match = false;
                if (!join_residual_matches(index, *spec, joined, &residual_match)) return false;
                if (!residual_match) continue;
                matched = true;
                if (spec->join_kind == CSD_SQL_JOIN_SEMI) {
                    output->rows.push_back(*left_row);
                    break;
                }
                if (spec->join_kind != CSD_SQL_JOIN_ANTI) output->rows.push_back(std::move(joined));
            }
            if (spec->join_kind == CSD_SQL_JOIN_ANTI && !matched) output->rows.push_back(*left_row);
            else if (spec->join_kind == CSD_SQL_JOIN_LEFT && !matched) {
                Row joined = *left_row;
                joined.resize(left.columns + right.columns);
                output->rows.push_back(std::move(joined));
            }
        }
        const uint64_t work = nested_loop ? left.rows.size() * right.rows.size() : left.rows.size() + right.rows.size();
        add_cycles(work + output->rows.size(), node);
        return true;
    }
