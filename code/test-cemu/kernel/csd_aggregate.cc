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

	/* Match PostgreSQL numeric division's minimum of 16 significant decimal
	 * digits.  Numeric stores four decimal digits per base-10000 weight, so the
	 * display scale is 16 - qweight * 4 (subject to the input dscale floor). */
	static int16_t average_scale(__int128 sum, int64_t count, int16_t input_scale)
	{
		if (sum == 0 || count <= 0)
			return std::max<int16_t>(16, input_scale);
		__int128 magnitude = sum < 0 ? -sum : sum;
		__int128 denominator = count;
		if (!multiply_power10(&denominator, input_scale)) return -1;
		int qweight = 0;
		if (magnitude >= denominator) {
			while (denominator <= magnitude / 10000) {
				denominator *= 10000;
				++qweight;
			}
		} else {
			qweight = -1;
			while (magnitude <= denominator / 10000) {
				magnitude *= 10000;
				--qweight;
			}
		}
		const int scale = std::max<int>(input_scale, 16 - qweight * 4);
		return scale <= 36 ? static_cast<int16_t>(scale) : -1;
	}

    /* Blocking banked hash aggregate with multi-key and DISTINCT state. */
    bool execute_aggregate(uint32_t index, const CsdSqlPlanNode &node, const Batch &input,
                           Batch *output, const CsdSqlPlanNode *project_node = NULL)
    {
        const CsdSqlAggregateSpec *spec = NULL;
        if (!expression_as(node, sizeof(CsdSqlAggregateSpec), &spec) ||
            (spec->group_count == 0 && spec->aggregate_count == 0))
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        const CsdSqlExpressionSpec *project_spec = NULL;
        const CsdSqlExpressionInstruction *project_instructions = NULL;
        if (project_node != NULL &&
            !expression_program(project_node->node_id, project_node->expression_offset,
                                project_node->expression_length, &project_spec,
                                &project_instructions))
            return false;
        (void) project_instructions;
        const uint16_t aggregate_input_columns = project_spec == NULL ? input.columns :
                                                  project_spec->output_count;
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
            if (group_columns[group] >= aggregate_input_columns)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        for (uint16_t aggregate_index = 0; aggregate_index < spec->aggregate_count; ++aggregate_index) {
            const CsdSqlAggregateColumn &aggregate = aggregates[aggregate_index];
            if ((aggregate.input_column != CSD_SQL_NO_COLUMN &&
                 aggregate.input_column >= aggregate_input_columns) ||
                !is_supported_type(aggregate.datum_type) || aggregate.operation < CSD_SQL_AGG_COUNT ||
                aggregate.operation > CSD_SQL_AGG_AVG || aggregate.output_scale < -1 || aggregate.output_scale > 18)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        }
        struct AggregateState {
            std::string key;
            Row groups;
            std::vector<int64_t> count;
            std::vector<__int128> sum;
			std::vector<int16_t> scale;
            std::vector<Value> minimum;
            std::vector<Value> maximum;
            std::vector<bool> has_value;
            std::vector<std::unordered_set<std::string> > distinct;
        };
        bool has_distinct = false;
        for (uint16_t aggregate_index = 0; aggregate_index < spec->aggregate_count; ++aggregate_index)
            has_distinct = has_distinct || (aggregates[aggregate_index].flags & CSD_SQL_AGG_DISTINCT) != 0;
        const size_t tiles = has_distinct ? 1 :
            std::min<size_t>(parallel_workers_, std::max<size_t>(1, input.rows.size()));
        std::vector<std::unordered_map<std::string, size_t> > local_maps(tiles);
        std::vector<std::vector<AggregateState> > local_states(tiles);
        std::vector<uint8_t> local_ok(tiles, 0);
        run_parallel(tiles, [&](size_t tile) {
            const size_t begin = input.rows.size() * tile / tiles;
            const size_t end = input.rows.size() * (tile + 1) / tiles;
            std::unordered_map<std::string, size_t> &group_map = local_maps[tile];
            std::vector<AggregateState> &states = local_states[tile];
            group_map.reserve(std::min<size_t>(end - begin, 65536));
            Row projected_row;
            if (project_spec != NULL) projected_row.reserve(project_spec->output_count);
            for (size_t row_index = begin; row_index < end; ++row_index) {
                const Row *row_pointer = &input.rows[row_index];
                if (project_spec != NULL) {
                    if (!evaluate_expression(project_node->node_id, project_node->expression_offset,
                                             project_node->expression_length, input.rows[row_index],
                                             NULL, &projected_row)) return;
                    row_pointer = &projected_row;
                }
                const Row &row = *row_pointer;
                const std::string key = spec->group_count == 0 ? std::string("global") :
                                        group_key(row, group_columns.data(), spec->group_count);
                size_t state_index = 0;
                std::unordered_map<std::string, size_t>::iterator found = group_map.find(key);
                if (found == group_map.end()) {
                    state_index = states.size();
                    group_map[key] = state_index;
                    AggregateState state;
                    state.key = key;
                    for (uint16_t group = 0; group < spec->group_count; ++group)
                        state.groups.push_back(row[group_columns[group]]);
                    state.count.assign(spec->aggregate_count, 0);
                    state.sum.assign(spec->aggregate_count, 0);
                    state.scale.assign(spec->aggregate_count, -1);
                    state.minimum.assign(spec->aggregate_count, Value());
                    state.maximum.assign(spec->aggregate_count, Value());
                    state.has_value.assign(spec->aggregate_count, false);
                    state.distinct.resize(spec->aggregate_count);
                    states.push_back(std::move(state));
                } else state_index = found->second;
                AggregateState &state = states[state_index];
                for (uint16_t aggregate_index = 0; aggregate_index < spec->aggregate_count; ++aggregate_index) {
                    const CsdSqlAggregateColumn &aggregate = aggregates[aggregate_index];
                    const Value *value = aggregate.input_column == CSD_SQL_NO_COLUMN ? NULL :
                                         &row[aggregate.input_column];
                    const bool usable = value == NULL || !value->is_null;
                    if (!usable) continue;
                    if (aggregate.flags & CSD_SQL_AGG_DISTINCT) {
                        if (value == NULL) { fail(CSD_SQL_STATUS_BAD_EXPRESSION, index); return; }
                        if (!state.distinct[aggregate_index].insert(value_key(*value)).second) continue;
                    }
                    if (aggregate.operation == CSD_SQL_AGG_COUNT) {
                        ++state.count[aggregate_index];
                        state.has_value[aggregate_index] = true;
                        continue;
                    }
                    if (value == NULL || value->type == CSD_SQL_TYPE_BYTES) {
                        fail(CSD_SQL_STATUS_UNSUPPORTED, index);
                        return;
                    }
                    ++state.count[aggregate_index];
                    if (aggregate.operation == CSD_SQL_AGG_SUM || aggregate.operation == CSD_SQL_AGG_AVG) {
                        const int16_t value_scale = value->type == CSD_SQL_TYPE_NUMERIC ? value->numeric_scale : 0;
                        int16_t target_scale = aggregate.output_scale;
                        if (target_scale < 0) {
                            target_scale = std::max<int16_t>(state.scale[aggregate_index], value_scale);
                            if (state.scale[aggregate_index] >= 0 && target_scale > state.scale[aggregate_index] &&
                                !multiply_power10(&state.sum[aggregate_index],
                                                  target_scale - state.scale[aggregate_index])) {
                                fail(CSD_SQL_STATUS_UNSUPPORTED, index);
                                return;
                            }
                        }
                        __int128 scaled = 0;
                        if (!scaled_integer(*value, target_scale, &scaled)) {
                            fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                            return;
                        }
                        state.scale[aggregate_index] = target_scale;
                        state.sum[aggregate_index] += scaled;
                    }
                    if (!state.has_value[aggregate_index] ||
                        compare_value(*value, state.minimum[aggregate_index]) < 0)
                        state.minimum[aggregate_index] = *value;
                    if (!state.has_value[aggregate_index] ||
                        compare_value(*value, state.maximum[aggregate_index]) > 0)
                        state.maximum[aggregate_index] = *value;
                    state.has_value[aggregate_index] = true;
                }
            }
            local_ok[tile] = 1;
        });
        for (size_t tile = 0; tile < tiles; ++tile)
            if (!local_ok[tile]) return false;
        std::unordered_map<std::string, size_t> group_map;
        std::vector<AggregateState> states;
        for (size_t tile = 0; tile < tiles; ++tile) {
            for (typename std::vector<AggregateState>::iterator source = local_states[tile].begin();
                 source != local_states[tile].end(); ++source) {
                std::unordered_map<std::string, size_t>::iterator found = group_map.find(source->key);
                if (found == group_map.end()) {
                    group_map[source->key] = states.size();
                    states.push_back(std::move(*source));
                    continue;
                }
                AggregateState &target = states[found->second];
                for (uint16_t aggregate_index = 0; aggregate_index < spec->aggregate_count; ++aggregate_index) {
                    const CsdSqlAggregateColumn &aggregate = aggregates[aggregate_index];
                    target.count[aggregate_index] += source->count[aggregate_index];
                    if (aggregate.operation == CSD_SQL_AGG_SUM || aggregate.operation == CSD_SQL_AGG_AVG) {
                        const int16_t target_scale = std::max(target.scale[aggregate_index],
                                                              source->scale[aggregate_index]);
                        if (target.scale[aggregate_index] >= 0 && target.scale[aggregate_index] < target_scale &&
                            !multiply_power10(&target.sum[aggregate_index],
                                              target_scale - target.scale[aggregate_index]))
                            return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
                        if (source->scale[aggregate_index] >= 0 && source->scale[aggregate_index] < target_scale &&
                            !multiply_power10(&source->sum[aggregate_index],
                                              target_scale - source->scale[aggregate_index]))
                            return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
                        target.scale[aggregate_index] = target_scale;
                        target.sum[aggregate_index] += source->sum[aggregate_index];
                    }
                    if (source->has_value[aggregate_index] &&
                        (!target.has_value[aggregate_index] ||
                         compare_value(source->minimum[aggregate_index], target.minimum[aggregate_index]) < 0))
                        target.minimum[aggregate_index] = source->minimum[aggregate_index];
                    if (source->has_value[aggregate_index] &&
                        (!target.has_value[aggregate_index] ||
                         compare_value(source->maximum[aggregate_index], target.maximum[aggregate_index]) > 0))
                        target.maximum[aggregate_index] = source->maximum[aggregate_index];
                    target.has_value[aggregate_index] = target.has_value[aggregate_index] ||
                                                        source->has_value[aggregate_index];
                }
            }
        }
        if (spec->group_count == 0 && states.empty()) {
            AggregateState state;
            state.count.assign(spec->aggregate_count, 0);
            state.sum.assign(spec->aggregate_count, 0);
			state.scale.assign(spec->aggregate_count, -1);
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
				value.numeric_scale = value.type == CSD_SQL_TYPE_NUMERIC ?
					(state->scale[aggregate_index] >= 0 ? state->scale[aggregate_index] :
					 (aggregate.output_scale >= 0 ? aggregate.output_scale : 0)) : 0;
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
					{
						const int16_t source_scale = state->scale[aggregate_index];
						int16_t result_scale = aggregate.output_scale >= 0 ?
							aggregate.output_scale : average_scale(state->sum[aggregate_index],
								state->count[aggregate_index], source_scale);
						if (result_scale < 0)
							return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
						__int128 quotient = 0;
						for (;;) {
							__int128 numerator = state->sum[aggregate_index];
							__int128 denominator = state->count[aggregate_index];
							bool scaled = true;
							if (result_scale >= source_scale)
								scaled = multiply_power10(&numerator, result_scale - source_scale);
							else
								scaled = multiply_power10(&denominator, source_scale - result_scale);
							if (scaled) {
								quotient = numerator / denominator;
								__int128 remainder = numerator % denominator;
								if (remainder < 0) remainder = -remainder;
								if (remainder * 2 >= denominator)
									quotient += numerator < 0 ? -1 : 1;
								if (fits_i64(quotient)) break;
							}
							/* Explicit typmods are contracts.  Only a dynamic numeric AVG may
							 * trade fractional digits for the fixed-point wire's range. */
							if (aggregate.output_scale >= 0 || result_scale <= source_scale)
								return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
							--result_scale;
						}
					value.is_null = false;
						value.integer = static_cast<int64_t>(quotient);
						value.numeric_scale = result_scale;
                    break;
					}
                case CSD_SQL_AGG_MIN:
                    value = state->minimum[aggregate_index];
                    break;
                case CSD_SQL_AGG_MAX:
                    value = state->maximum[aggregate_index];
                    break;
                default: return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
                }
                result.push_back(std::move(value));
            }
            output->rows.push_back(std::move(result));
        }
        if (!spill_if_needed(index, node, *output)) return false;
        if (project_node != NULL) add_cycles(input.rows.size(), *project_node);
        add_cycles(input.rows.size() * 2 + output->rows.size(), node);
        return true;
    }
