    static std::string group_key(const Row &row, const uint16_t *columns, uint16_t count)
    {
        std::string key;
        for (uint16_t index = 0; index < count; ++index) {
            const std::string component = value_key(row[columns[index]]);
            const uint32_t length = component.size();
            key.append(reinterpret_cast<const char *>(&length), sizeof(length));
            key.append(component);
        }
        return key;
    }

    /* Blocking banked hash aggregate with multi-key and DISTINCT state. */
    bool execute_aggregate(uint32_t index, const CsdSqlPlanNode &node, const Batch &input, Batch *output)
    {
        const CsdSqlAggregateSpec *spec = NULL;
        if (!expression_as(node, sizeof(CsdSqlAggregateSpec), &spec) ||
            (spec->group_count == 0 && spec->aggregate_count == 0))
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        uint64_t group_bytes = 0, aggregate_bytes = 0, descriptor_bytes = 0;
        if (!checked_mul(spec->group_count, sizeof(uint16_t), &group_bytes) ||
            !checked_mul(spec->aggregate_count, sizeof(CsdSqlAggregateColumn), &aggregate_bytes) ||
            !checked_add(sizeof(CsdSqlAggregateSpec), group_bytes, &descriptor_bytes) ||
            !checked_add(descriptor_bytes, aggregate_bytes, &descriptor_bytes) ||
            node.expression_length < descriptor_bytes)
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        const unsigned char *group_bytes_begin = reinterpret_cast<const unsigned char *>(spec + 1);
        std::vector<uint16_t> group_columns(spec->group_count);
        for (uint16_t group = 0; group < spec->group_count; ++group)
            group_columns[group] = read_u16(group_bytes_begin + group * sizeof(uint16_t));
        const CsdSqlAggregateColumn *aggregates = reinterpret_cast<const CsdSqlAggregateColumn *>(
            group_bytes_begin + group_bytes);
        for (uint16_t group = 0; group < spec->group_count; ++group)
            if (group_columns[group] >= input.columns) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        for (uint16_t aggregate_index = 0; aggregate_index < spec->aggregate_count; ++aggregate_index) {
            const CsdSqlAggregateColumn &aggregate = aggregates[aggregate_index];
            if ((aggregate.input_column != CSD_SQL_NO_COLUMN && aggregate.input_column >= input.columns) ||
                !is_supported_type(aggregate.datum_type) || aggregate.operation < CSD_SQL_AGG_COUNT ||
                aggregate.operation > CSD_SQL_AGG_AVG || aggregate.output_scale < 0 || aggregate.output_scale > 18)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        }
        struct AggregateState {
            Row groups;
            std::vector<int64_t> count;
            std::vector<__int128> sum;
            std::vector<Value> minimum;
            std::vector<Value> maximum;
            std::vector<bool> has_value;
            std::vector<std::unordered_set<std::string> > distinct;
        };
        std::unordered_map<std::string, size_t> group_map;
        std::vector<AggregateState> states;
        for (std::vector<Row>::const_iterator row = input.rows.begin(); row != input.rows.end(); ++row) {
            const std::string key = spec->group_count == 0 ? std::string("global") :
                                    group_key(*row, group_columns.data(), spec->group_count);
            size_t state_index = 0;
            std::unordered_map<std::string, size_t>::iterator found = group_map.find(key);
            if (found == group_map.end()) {
                state_index = states.size();
                group_map[key] = state_index;
                AggregateState state;
                for (uint16_t group = 0; group < spec->group_count; ++group)
                    state.groups.push_back((*row)[group_columns[group]]);
                state.count.assign(spec->aggregate_count, 0);
                state.sum.assign(spec->aggregate_count, 0);
                state.minimum.assign(spec->aggregate_count, Value());
                state.maximum.assign(spec->aggregate_count, Value());
                state.has_value.assign(spec->aggregate_count, false);
                state.distinct.resize(spec->aggregate_count);
                states.push_back(state);
            } else state_index = found->second;
            AggregateState &state = states[state_index];
            for (uint16_t aggregate_index = 0; aggregate_index < spec->aggregate_count; ++aggregate_index) {
                const CsdSqlAggregateColumn &aggregate = aggregates[aggregate_index];
                const Value *value = aggregate.input_column == CSD_SQL_NO_COLUMN ? NULL : &(*row)[aggregate.input_column];
                const bool usable = value == NULL || !value->is_null;
                if (!usable) continue;
                if (aggregate.flags & CSD_SQL_AGG_DISTINCT) {
                    if (value == NULL) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                    if (!state.distinct[aggregate_index].insert(value_key(*value)).second) continue;
                }
                if (aggregate.operation == CSD_SQL_AGG_COUNT) {
                    ++state.count[aggregate_index];
                    state.has_value[aggregate_index] = true;
                    continue;
                }
                if (value == NULL || value->type == CSD_SQL_TYPE_BYTES)
                    return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
                ++state.count[aggregate_index];
                __int128 scaled = 0;
                if (!scaled_integer(*value, aggregate.output_scale, &scaled))
                    return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                state.sum[aggregate_index] += scaled;
                if (!state.has_value[aggregate_index] || compare_value(*value, state.minimum[aggregate_index]) < 0)
                    state.minimum[aggregate_index] = *value;
                if (!state.has_value[aggregate_index] || compare_value(*value, state.maximum[aggregate_index]) > 0)
                    state.maximum[aggregate_index] = *value;
                state.has_value[aggregate_index] = true;
            }
        }
        if (spec->group_count == 0 && states.empty()) {
            AggregateState state;
            state.count.assign(spec->aggregate_count, 0);
            state.sum.assign(spec->aggregate_count, 0);
            state.minimum.assign(spec->aggregate_count, Value());
            state.maximum.assign(spec->aggregate_count, Value());
            state.has_value.assign(spec->aggregate_count, false);
            state.distinct.resize(spec->aggregate_count);
            states.push_back(state);
        }
        output->columns = spec->group_count + spec->aggregate_count;
        for (std::vector<AggregateState>::const_iterator state = states.begin(); state != states.end(); ++state) {
            Row result = state->groups;
            for (uint16_t aggregate_index = 0; aggregate_index < spec->aggregate_count; ++aggregate_index) {
                const CsdSqlAggregateColumn &aggregate = aggregates[aggregate_index];
                Value value;
                value.type = aggregate.operation == CSD_SQL_AGG_COUNT ?
                    static_cast<uint8_t>(CSD_SQL_TYPE_INT64) : aggregate.datum_type;
                value.numeric_scale = value.type == CSD_SQL_TYPE_NUMERIC ? aggregate.output_scale : 0;
                switch (aggregate.operation) {
                case CSD_SQL_AGG_COUNT:
                    value.is_null = false;
                    value.integer = state->count[aggregate_index];
                    break;
                case CSD_SQL_AGG_SUM:
                    if (!state->has_value[aggregate_index]) break;
                    if (!fits_i64(state->sum[aggregate_index])) return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
                    value.is_null = false;
                    value.integer = static_cast<int64_t>(state->sum[aggregate_index]);
                    break;
                case CSD_SQL_AGG_AVG:
                    if (!state->has_value[aggregate_index]) break;
                    value.is_null = false;
                    value.integer = static_cast<int64_t>(state->sum[aggregate_index] / state->count[aggregate_index]);
                    break;
                case CSD_SQL_AGG_MIN:
                    value = state->minimum[aggregate_index];
                    break;
                case CSD_SQL_AGG_MAX:
                    value = state->maximum[aggregate_index];
                    break;
                default: return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                }
                result.push_back(value);
            }
            output->rows.push_back(std::move(result));
        }
        if (!spill_if_needed(index, node, *output)) return false;
        add_cycles(input.rows.size() * 2 + output->rows.size(), node);
        return true;
    }
