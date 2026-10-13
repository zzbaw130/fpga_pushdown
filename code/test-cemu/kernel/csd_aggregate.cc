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

    struct StreamingAggregateGroup {
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

    struct StreamingAggregateState {
        const CsdSqlAggregateSpec *spec;
        const CsdSqlAggregateColumn *aggregates;
        const CsdSqlPlanNode *project_node;
        const CsdSqlExpressionSpec *project_spec;
        std::vector<uint16_t> group_columns;
        std::unordered_map<std::string, size_t> group_map;
        std::vector<StreamingAggregateGroup> groups;
        uint16_t input_columns;
        uint64_t held_bytes;
        uint64_t input_rows;
        bool configured;
        StreamingAggregateState()
            : spec(NULL), aggregates(NULL), project_node(NULL),
              project_spec(NULL), input_columns(0), held_bytes(0),
              input_rows(0), configured(false) {}
    };

    struct StreamingAggregatePartitions {
        uint16_t columns;
        uint8_t radix_shift;
        uint8_t radix_bits;
        uint64_t rows;
        std::vector<uint16_t> group_columns;
        std::vector<SpillHandleChain> chains;
        StreamingAggregatePartitions()
            : columns(0), radix_shift(0), radix_bits(0), rows(0) {}
    };

    /* Partitioned Join-to-Aggregate retains an intact final Flash run, so a
     * grouped DISTINCT can be replayed after partitioning by its complete
     * group key. A global DISTINCT cannot be split this way without a second
     * distinct-key reduction and remains on the compatibility path. */
    bool can_replay_stream_aggregate(const CsdSqlPlanNode &node)
    {
        const CsdSqlAggregateSpec *spec = NULL;
        if (!expression_as(node, sizeof(CsdSqlAggregateSpec), &spec) ||
            (spec->group_count == 0 && spec->aggregate_count == 0))
            return false;
        uint64_t group_bytes = 0, aggregate_bytes = 0, descriptor_bytes = 0;
        if (!checked_mul(spec->group_count, sizeof(uint16_t), &group_bytes) ||
            !checked_mul(spec->aggregate_count,
                         sizeof(CsdSqlAggregateColumn), &aggregate_bytes) ||
            !checked_add(sizeof(CsdSqlAggregateSpec), group_bytes,
                         &descriptor_bytes) ||
            !checked_add(descriptor_bytes, aggregate_bytes,
                         &descriptor_bytes) ||
            node.expression_length < descriptor_bytes)
            return false;
        const unsigned char *group_begin =
            reinterpret_cast<const unsigned char *>(spec + 1);
        const CsdSqlAggregateColumn *aggregates =
            reinterpret_cast<const CsdSqlAggregateColumn *>(
                group_begin + group_bytes);
        for (uint16_t aggregate = 0; aggregate < spec->aggregate_count;
             ++aggregate)
            if ((aggregates[aggregate].flags & CSD_SQL_AGG_DISTINCT) &&
                spec->group_count == 0)
                return false;
        return true;
    }

    bool can_stream_aggregate(const CsdSqlPlanNode &node)
    {
        if (!can_replay_stream_aggregate(node)) return false;
        const CsdSqlAggregateSpec *spec = NULL;
        if (!expression_as(node, sizeof(CsdSqlAggregateSpec), &spec))
            return false;
        uint64_t group_bytes = 0;
        if (!checked_mul(spec->group_count, sizeof(uint16_t), &group_bytes))
            return false;
        const CsdSqlAggregateColumn *aggregates =
            reinterpret_cast<const CsdSqlAggregateColumn *>(
                reinterpret_cast<const unsigned char *>(spec + 1) +
                group_bytes);
        /* A direct Scan has no retained input run if its DISTINCT set exceeds
         * the estimate. Keep grouped DISTINCT on a replayable Flash path. */
        for (uint16_t aggregate = 0; aggregate < spec->aggregate_count;
             ++aggregate)
            if (aggregates[aggregate].flags & CSD_SQL_AGG_DISTINCT)
                return false;
        if (spec->group_count == 0) return true;
        uint64_t protected_estimate = 0;
        return node.arg0 != 0 &&
               checked_mul(node.arg0, UINT64_C(8),
                           &protected_estimate) &&
               protected_estimate <= operator_memory_capacity_;
    }

    bool configure_streaming_aggregate(uint32_t index,
                                       const CsdSqlPlanNode &node,
                                       uint16_t input_columns,
                                       const CsdSqlPlanNode *project_node,
                                       StreamingAggregateState *state)
    {
        if (state->configured)
            return state->input_columns == input_columns ? true :
                fail(CSD_SQL_STATUS_BAD_NODE, index, 0x510);
        const CsdSqlAggregateSpec *spec = NULL;
        if (!expression_as(node, sizeof(CsdSqlAggregateSpec), &spec) ||
            (spec->group_count == 0 && spec->aggregate_count == 0))
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        const CsdSqlExpressionSpec *project_spec = NULL;
        const CsdSqlExpressionInstruction *project_instructions = NULL;
        if (project_node != NULL &&
            !expression_program(project_node->node_id,
                                project_node->expression_offset,
                                project_node->expression_length,
                                &project_spec, &project_instructions))
            return false;
        (void) project_instructions;
        const uint16_t aggregate_input_columns = project_spec == NULL ?
            input_columns : project_spec->output_count;
        uint64_t group_bytes = 0, aggregate_bytes = 0, descriptor_bytes = 0;
        if (!checked_mul(spec->group_count, sizeof(uint16_t), &group_bytes) ||
            !checked_mul(spec->aggregate_count,
                         sizeof(CsdSqlAggregateColumn), &aggregate_bytes) ||
            !checked_add(sizeof(CsdSqlAggregateSpec), group_bytes,
                         &descriptor_bytes) ||
            !checked_add(descriptor_bytes, aggregate_bytes,
                         &descriptor_bytes) ||
            node.expression_length < descriptor_bytes)
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        const unsigned char *group_begin =
            reinterpret_cast<const unsigned char *>(spec + 1);
        state->group_columns.resize(spec->group_count);
        for (uint16_t group = 0; group < spec->group_count; ++group) {
            state->group_columns[group] = read_u16(
                group_begin + group * sizeof(uint16_t));
            if (state->group_columns[group] >= aggregate_input_columns)
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        }
        const CsdSqlAggregateColumn *aggregates =
            reinterpret_cast<const CsdSqlAggregateColumn *>(
                group_begin + group_bytes);
        for (uint16_t aggregate_index = 0;
             aggregate_index < spec->aggregate_count; ++aggregate_index) {
            const CsdSqlAggregateColumn &aggregate = aggregates[aggregate_index];
            if ((aggregate.input_column != CSD_SQL_NO_COLUMN &&
                 aggregate.input_column >= aggregate_input_columns) ||
                !is_supported_type(aggregate.datum_type) ||
                aggregate.operation < CSD_SQL_AGG_COUNT ||
                aggregate.operation > CSD_SQL_AGG_AVG ||
                aggregate.output_scale < -1 || aggregate.output_scale > 18 ||
                ((aggregate.flags & CSD_SQL_AGG_DISTINCT) &&
                 spec->group_count == 0))
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index, 0x511);
        }
        state->spec = spec;
        state->aggregates = aggregates;
        state->project_node = project_node;
        state->project_spec = project_spec;
        state->input_columns = input_columns;
        state->configured = true;
        return true;
    }

    void release_streaming_aggregate_memory(StreamingAggregateState *state)
    {
        if (state->held_bytes != 0) {
            release_operator_memory(state->held_bytes);
            state->held_bytes = 0;
        }
    }

    bool consume_streaming_aggregate_batch(uint32_t index,
                                           const CsdSqlPlanNode &node,
                                           Batch &input,
                                           const CsdSqlPlanNode *project_node,
                                           StreamingAggregateState *state,
                                           bool allow_capacity_fallback = false,
                                           bool *capacity_exceeded = NULL,
                                           uint64_t *required_bytes = NULL)
    {
        if (!configure_streaming_aggregate(index, node, input.columns,
                                            project_node, state))
            return false;
        Row projected_row;
        std::vector<EvalValue> expression_stack;
        if (state->project_spec != NULL)
            projected_row.reserve(state->project_spec->output_count);
        for (std::vector<Row>::const_iterator source = input.rows.begin();
             source != input.rows.end(); ++source) {
            const Row *row = &*source;
            if (state->project_spec != NULL) {
                if (!evaluate_expression(state->project_node->node_id,
                        state->project_node->expression_offset,
                        state->project_node->expression_length, *source, NULL,
                        &projected_row, &expression_stack))
                    return false;
                row = &projected_row;
            }
            const std::string key = state->spec->group_count == 0 ?
                std::string("global") : group_key(*row,
                    state->group_columns.data(), state->spec->group_count);
            size_t state_index = 0;
            std::unordered_map<std::string, size_t>::iterator found =
                state->group_map.find(key);
            if (found == state->group_map.end()) {
                StreamingAggregateGroup group;
                group.key = key;
                for (uint16_t column = 0;
                     column < state->spec->group_count; ++column)
                    group.groups.push_back((*row)[state->group_columns[column]]);
                uint64_t modeled_bytes = 0;
                if (!checked_add(UINT64_C(64), group.key.size(),
                                 &modeled_bytes))
                    return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x512);
                for (Row::const_iterator value = group.groups.begin();
                     value != group.groups.end(); ++value)
                    if (!checked_add(modeled_bytes,
                            sizeof(CsdSqlResultCell) + value->bytes.size(),
                            &modeled_bytes))
                        return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index,
                                    0x513);
                uint64_t accumulators = 0;
                if (!checked_mul(state->spec->aggregate_count, UINT64_C(64),
                                 &accumulators) ||
                    !checked_add(modeled_bytes, accumulators,
                                 &modeled_bytes))
                    return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x514);
                if (modeled_bytes > operator_memory_capacity_ -
                                    state->held_bytes) {
                    if (allow_capacity_fallback) {
                        if (capacity_exceeded != NULL)
                            *capacity_exceeded = true;
                        if (required_bytes != NULL) {
                            uint64_t total = 0;
                            *required_bytes = checked_add(state->held_bytes,
                                modeled_bytes, &total) ? total :
                                std::numeric_limits<uint64_t>::max();
                        }
                        return false;
                    }
                    return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x515);
                }
                if (!wait_acquire_operator_memory(modeled_bytes))
                    return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x515);
                state->held_bytes += modeled_bytes;
                group.count.assign(state->spec->aggregate_count, 0);
                group.sum.assign(state->spec->aggregate_count, 0);
                group.scale.assign(state->spec->aggregate_count, -1);
                group.minimum.assign(state->spec->aggregate_count, Value());
                group.maximum.assign(state->spec->aggregate_count, Value());
                group.has_value.assign(state->spec->aggregate_count, false);
                group.distinct.resize(state->spec->aggregate_count);
                state_index = state->groups.size();
                state->group_map[key] = state_index;
                state->groups.push_back(std::move(group));
            } else {
                state_index = found->second;
            }
            StreamingAggregateGroup &group = state->groups[state_index];
            for (uint16_t aggregate_index = 0;
                 aggregate_index < state->spec->aggregate_count;
                 ++aggregate_index) {
                const CsdSqlAggregateColumn &aggregate =
                    state->aggregates[aggregate_index];
                const Value *value = aggregate.input_column ==
                    CSD_SQL_NO_COLUMN ? NULL :
                    &(*row)[aggregate.input_column];
                if (value != NULL && value->is_null) continue;
                if (aggregate.flags & CSD_SQL_AGG_DISTINCT) {
                    if (value == NULL)
                        return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index,
                                    0x524);
                    const std::string distinct_key = value_key(*value);
                    if (group.distinct[aggregate_index].find(distinct_key) !=
                        group.distinct[aggregate_index].end())
                        continue;
                    uint64_t distinct_bytes = 0;
                    if (!checked_add(sizeof(uint64_t), distinct_key.size(),
                                     &distinct_bytes))
                        return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index,
                                    0x525);
                    if (distinct_bytes > operator_memory_capacity_ -
                                         state->held_bytes) {
                        if (allow_capacity_fallback) {
                            if (capacity_exceeded != NULL)
                                *capacity_exceeded = true;
                            if (required_bytes != NULL) {
                                uint64_t total = 0;
                                *required_bytes = checked_add(
                                    state->held_bytes, distinct_bytes,
                                    &total) ? total :
                                    std::numeric_limits<uint64_t>::max();
                            }
                            return false;
                        }
                        return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
                                    0x526);
                    }
                    if (!wait_acquire_operator_memory(distinct_bytes))
                        return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
                                    0x526);
                    state->held_bytes += distinct_bytes;
                    group.distinct[aggregate_index].insert(distinct_key);
                }
                if (aggregate.operation == CSD_SQL_AGG_COUNT) {
                    ++group.count[aggregate_index];
                    group.has_value[aggregate_index] = true;
                    continue;
                }
                if (value == NULL || value->type == CSD_SQL_TYPE_BYTES)
                    return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x516);
                ++group.count[aggregate_index];
                if (aggregate.operation == CSD_SQL_AGG_SUM ||
                    aggregate.operation == CSD_SQL_AGG_AVG) {
                    const int16_t value_scale =
                        value->type == CSD_SQL_TYPE_NUMERIC ?
                        value->numeric_scale : 0;
                    int16_t target_scale = aggregate.output_scale;
                    if (target_scale < 0) {
                        target_scale = std::max<int16_t>(
                            group.scale[aggregate_index], value_scale);
                        if (group.scale[aggregate_index] >= 0 &&
                            target_scale > group.scale[aggregate_index] &&
                            !multiply_power10(&group.sum[aggregate_index],
                                target_scale - group.scale[aggregate_index]))
                            return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
                                        0x517);
                    }
                    __int128 scaled = 0;
                    if (!scaled_integer(*value, target_scale, &scaled))
                        return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index,
                                    0x518);
                    group.scale[aggregate_index] = target_scale;
                    group.sum[aggregate_index] += scaled;
                }
                if (!group.has_value[aggregate_index] ||
                    compare_value(*value,
                                  group.minimum[aggregate_index]) < 0)
                    group.minimum[aggregate_index] = *value;
                if (!group.has_value[aggregate_index] ||
                    compare_value(*value,
                                  group.maximum[aggregate_index]) > 0)
                    group.maximum[aggregate_index] = *value;
                group.has_value[aggregate_index] = true;
            }
            ++state->input_rows;
        }
        return true;
    }

    bool finish_streaming_aggregate(uint32_t index,
                                    const CsdSqlPlanNode &node,
                                    StreamingAggregateState *state,
                                    Batch *output)
    {
        if (!state->configured)
            return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x519);
        StreamingAggregateGroup empty_global;
        if (state->groups.empty() && state->spec->group_count == 0) {
            empty_global.count.assign(state->spec->aggregate_count, 0);
            empty_global.sum.assign(state->spec->aggregate_count, 0);
            empty_global.scale.assign(state->spec->aggregate_count, -1);
            empty_global.minimum.assign(state->spec->aggregate_count, Value());
            empty_global.maximum.assign(state->spec->aggregate_count, Value());
            empty_global.has_value.assign(state->spec->aggregate_count, false);
            state->groups.push_back(std::move(empty_global));
        }
        output->columns = state->spec->group_count +
                          state->spec->aggregate_count;
        for (std::vector<StreamingAggregateGroup>::const_iterator group =
                 state->groups.begin(); group != state->groups.end(); ++group) {
            Row result = group->groups;
            for (uint16_t aggregate_index = 0;
                 aggregate_index < state->spec->aggregate_count;
                 ++aggregate_index) {
                const CsdSqlAggregateColumn &aggregate =
                    state->aggregates[aggregate_index];
                Value value;
                value.type = aggregate.operation == CSD_SQL_AGG_COUNT ?
                    static_cast<uint8_t>(CSD_SQL_TYPE_INT64) :
                    aggregate.datum_type;
                value.numeric_scale = value.type == CSD_SQL_TYPE_NUMERIC ?
                    (group->scale[aggregate_index] >= 0 ?
                        group->scale[aggregate_index] :
                        (aggregate.output_scale >= 0 ?
                            aggregate.output_scale : 0)) : 0;
                switch (aggregate.operation) {
                case CSD_SQL_AGG_COUNT:
                    value.is_null = false;
                    value.integer = group->count[aggregate_index];
                    break;
                case CSD_SQL_AGG_SUM:
                    if (!group->has_value[aggregate_index]) break;
                    if (!fits_i64(group->sum[aggregate_index]))
                        return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x51a);
                    value.is_null = false;
                    value.integer = static_cast<int64_t>(
                        group->sum[aggregate_index]);
                    break;
                case CSD_SQL_AGG_AVG:
                    if (!group->has_value[aggregate_index]) break;
                    {
                        const int16_t source_scale =
                            group->scale[aggregate_index];
                        int16_t result_scale = aggregate.output_scale >= 0 ?
                            aggregate.output_scale : average_scale(
                                group->sum[aggregate_index],
                                group->count[aggregate_index], source_scale);
                        if (result_scale < 0)
                            return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
                                        0x51b);
                        __int128 quotient = 0;
                        for (;;) {
                            __int128 numerator = group->sum[aggregate_index];
                            __int128 denominator =
                                group->count[aggregate_index];
                            bool scaled = true;
                            if (result_scale >= source_scale)
                                scaled = multiply_power10(&numerator,
                                    result_scale - source_scale);
                            else
                                scaled = multiply_power10(&denominator,
                                    source_scale - result_scale);
                            if (scaled) {
                                quotient = numerator / denominator;
                                __int128 remainder = numerator % denominator;
                                if (remainder < 0) remainder = -remainder;
                                if (remainder * 2 >= denominator)
                                    quotient += numerator < 0 ? -1 : 1;
                                if (fits_i64(quotient)) break;
                            }
                            if (aggregate.output_scale >= 0 ||
                                result_scale <= source_scale)
                                return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
                                            0x51c);
                            --result_scale;
                        }
                        value.is_null = false;
                        value.integer = static_cast<int64_t>(quotient);
                        value.numeric_scale = result_scale;
                    }
                    break;
                case CSD_SQL_AGG_MIN:
                    value = group->minimum[aggregate_index];
                    break;
                case CSD_SQL_AGG_MAX:
                    value = group->maximum[aggregate_index];
                    break;
                default:
                    return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index, 0x51d);
                }
                result.push_back(std::move(value));
            }
            output->rows.push_back(std::move(result));
        }
        if (state->project_node != NULL)
            add_cycles(state->input_rows, *state->project_node);
        add_cycles(state->input_rows * 2 + output->rows.size(), node);
        return true;
    }

    void note_streaming_aggregate_chain_memory(size_t chain_count)
    {
        uint64_t bytes = 0;
        if (!checked_mul(chain_count, sizeof(SpillHandleChain), &bytes))
            bytes = std::numeric_limits<uint64_t>::max();
        std::lock_guard<std::mutex> status_guard(status_mutex_);
        status_.stream_aggregate_chain_peak_bytes = std::max<uint64_t>(
            status_.stream_aggregate_chain_peak_bytes, bytes);
    }

    bool choose_streaming_aggregate_partitions(
        uint32_t index, uint64_t state_estimate, uint8_t radix_shift,
        size_t *partition_count, uint8_t *radix_bits)
    {
        const uint64_t capacity = std::max<uint64_t>(
            1, operator_memory_capacity_);
        const uint64_t required = state_estimate == 0 ? 2 :
            std::max<uint64_t>(2,
                1 + (state_estimate - 1) / capacity);
        if (radix_shift >= 16)
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x51e);
        const size_t maximum = size_t(1) << (16 - radix_shift);
        size_t count = 2;
        while (count < required && count < maximum) count <<= 1;
        if (count < required)
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x51f);
        uint8_t bits = 0;
        for (size_t value = count; value > 1; value >>= 1) ++bits;
        *partition_count = count;
        *radix_bits = bits;
        return true;
    }

    bool configure_streaming_aggregate_partitions(
        uint32_t index, uint16_t columns,
        const std::vector<uint16_t> &group_columns,
        uint64_t state_estimate, uint8_t radix_shift,
        StreamingAggregatePartitions *partitions)
    {
        if (group_columns.empty())
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x520);
        size_t partition_count = 0;
        uint8_t radix_bits = 0;
        if (!choose_streaming_aggregate_partitions(index, state_estimate,
                radix_shift, &partition_count, &radix_bits))
            return false;
        partitions->columns = columns;
        partitions->radix_shift = radix_shift;
        partitions->radix_bits = radix_bits;
        partitions->group_columns = group_columns;
        partitions->chains.resize(partition_count);
        note_streaming_aggregate_chain_memory(partition_count);
        return true;
    }

    bool consume_streaming_aggregate_partition_batch(
        uint32_t index, Batch &batch,
        StreamingAggregatePartitions *partitions)
    {
        if (batch.columns != partitions->columns ||
            partitions->chains.empty())
            return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x521);
        std::vector<std::vector<size_t> > selected(
            partitions->chains.size());
        for (size_t row_index = 0; row_index < batch.rows.size();
             ++row_index) {
            const std::string key = group_key(batch.rows[row_index],
                partitions->group_columns.data(),
                partitions->group_columns.size());
            const size_t partition =
                (std::hash<std::string>()(key) >>
                    partitions->radix_shift) &
                (partitions->chains.size() - 1);
            selected[partition].push_back(row_index);
        }
        for (size_t partition = 0; partition < selected.size(); ++partition) {
            if (selected[partition].empty()) continue;
            SpillHandle handle;
            if (!spill_selected_rows(index, batch, selected[partition],
                                     false, &handle) ||
                !append_spill_chain_handle(index, handle,
                    &partitions->chains[partition], false))
                return false;
        }
        if (!checked_add(partitions->rows, batch.rows.size(),
                         &partitions->rows))
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x522);
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_aggregate_partition_batches;
            uint64_t total = 0;
            status_.stream_aggregate_partition_rows = checked_add(
                status_.stream_aggregate_partition_rows,
                batch.rows.size(), &total) ? total :
                std::numeric_limits<uint64_t>::max();
        }
        return true;
    }

    bool replay_streaming_aggregate_chain(
        uint32_t index, const CsdSqlPlanNode &node,
        const SpillHandleChain &chain, uint16_t columns,
        const std::vector<uint16_t> &group_columns,
        uint8_t radix_shift, Batch *output)
    {
        StreamingAggregateState aggregate_state;
        if (!configure_streaming_aggregate(index, node, columns, NULL,
                                            &aggregate_state))
            return false;
        bool capacity_exceeded = false;
        uint64_t required_bytes = 0;
        const bool streamed = for_each_spill_chain_handle(index, chain,
            [&](const SpillHandle &handle) {
                return stream_spill_batch(index, handle, [&](Batch &chunk) {
                    return consume_streaming_aggregate_batch(index, node,
                        chunk, NULL, &aggregate_state, true,
                        &capacity_exceeded, &required_bytes);
                });
            }, false);
        if (streamed) {
            Batch partial;
            const bool finished = finish_streaming_aggregate(index, node,
                &aggregate_state, &partial);
            release_streaming_aggregate_memory(&aggregate_state);
            if (!finished) return false;
            output->columns = partial.columns;
            output->rows.insert(output->rows.end(),
                std::make_move_iterator(partial.rows.begin()),
                std::make_move_iterator(partial.rows.end()));
            return true;
        }
        const bool oversized_single_group = capacity_exceeded &&
            aggregate_state.groups.empty() &&
            aggregate_state.held_bytes == 0;
        release_streaming_aggregate_memory(&aggregate_state);
        if (failed_ || !capacity_exceeded) return false;
        if (oversized_single_group)
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x523);

        StreamingAggregatePartitions children;
        if (!configure_streaming_aggregate_partitions(index, columns,
                group_columns, required_bytes, radix_shift, &children))
            return false;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            ++status_.stream_aggregate_repartition_passes;
        }
        if (!for_each_spill_chain_handle(index, chain,
                [&](const SpillHandle &handle) {
                    return stream_spill_batch(index, handle,
                        [&](Batch &chunk) {
                            return consume_streaming_aggregate_partition_batch(
                                index, chunk, &children);
                        });
                }, false))
            return false;
        uint64_t repartition_work = 0;
        if (!checked_mul(children.rows, 2, &repartition_work))
            repartition_work = std::numeric_limits<uint64_t>::max();
        add_cycles(repartition_work, node);
        const uint8_t next_shift = static_cast<uint8_t>(
            radix_shift + children.radix_bits);
        for (size_t partition = 0; partition < children.chains.size();
             ++partition) {
            if (children.chains[partition].count == 0) continue;
            if (!replay_streaming_aggregate_chain(index, node,
                    children.chains[partition], columns, group_columns,
                    next_shift, output))
                return false;
        }
        return true;
    }

    bool finish_streaming_aggregate_partitions(
        uint32_t index, const CsdSqlPlanNode &node,
        StreamingAggregatePartitions *partitions, Batch *output)
    {
        output->columns = 0;
        const uint8_t next_shift = static_cast<uint8_t>(
            partitions->radix_shift + partitions->radix_bits);
        for (size_t partition = 0;
             partition < partitions->chains.size(); ++partition) {
            if (partitions->chains[partition].count == 0) continue;
            if (!replay_streaming_aggregate_chain(index, node,
                    partitions->chains[partition], partitions->columns,
                    partitions->group_columns, next_shift, output))
                return false;
        }
        uint64_t partition_work = 0;
        if (!checked_mul(partitions->rows, 2, &partition_work))
            partition_work = std::numeric_limits<uint64_t>::max();
        add_cycles(partition_work, node);
        return true;
    }

    bool execute_scan_to_aggregate(uint32_t scan_index,
                                   const CsdSqlPlanNode &scan_node,
                                   const std::vector<uint32_t> &pipeline,
                                   uint32_t aggregate_index,
                                   const CsdSqlPlanNode &aggregate_node,
                                   const CsdSqlPlanNode *project_node,
                                   Batch *output)
    {
        StreamingAggregateState state;
        Batch discarded;
        std::function<bool(Batch &)> consumer = [&](Batch &batch) {
            return consume_streaming_aggregate_batch(aggregate_index,
                aggregate_node, batch, project_node, &state);
        };
        if (!execute_scan(scan_index, scan_node, &discarded, &pipeline,
                          &consumer)) {
            release_streaming_aggregate_memory(&state);
            return false;
        }
        if (!state.configured &&
            !configure_streaming_aggregate(aggregate_index, aggregate_node,
                                            discarded.columns, project_node,
                                            &state)) {
            release_streaming_aggregate_memory(&state);
            return false;
        }
        const bool succeeded = finish_streaming_aggregate(aggregate_index,
            aggregate_node, &state, output);
        release_streaming_aggregate_memory(&state);
        return succeeded;
    }

    /* Partition complete groups by their canonical key. Every row for one
     * group lands in exactly one record, so DISTINCT and AVG remain local to
     * a replay partition and partial-state combination is unnecessary. */
    bool execute_partitioned_aggregate(uint32_t index, const CsdSqlPlanNode &node,
                                       const Batch &input, Batch *output,
                                       const CsdSqlPlanNode *project_node,
                                       const CsdSqlAggregateSpec &spec,
                                       const std::vector<uint16_t> &group_columns,
                                       uint64_t aggregate_state_bytes,
                                       uint8_t radix_shift)
    {
        const uint64_t capacity = std::max<uint64_t>(1, operator_memory_capacity_);
        const uint64_t required_partitions = aggregate_state_bytes == 0 ? 1 :
            1 + (aggregate_state_bytes - 1) / capacity;
        const size_t max_partitions = size_t(1) << 16;
        size_t partition_count = 2;
        while (partition_count < required_partitions &&
               partition_count < max_partitions)
            partition_count <<= 1;
        if (partition_count < required_partitions)
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x501);
        uint8_t radix_bits = 0;
        for (size_t count = partition_count; count > 1; count >>= 1)
            ++radix_bits;
        if (radix_shift >= sizeof(size_t) * 8 ||
            radix_bits > sizeof(size_t) * 8 - radix_shift)
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x503);

        const CsdSqlExpressionSpec *project_spec = NULL;
        const CsdSqlExpressionInstruction *project_instructions = NULL;
        if (project_node != NULL &&
            !expression_program(project_node->node_id,
                                project_node->expression_offset,
                                project_node->expression_length,
                                &project_spec, &project_instructions))
            return false;
        (void) project_instructions;

        /* Keep only row ordinals during classification. Materialize one
         * partition at a time for serialization instead of duplicating every
         * Value object across a full vector<Batch>. */
        std::vector<std::vector<size_t> > partition_rows(partition_count);
        Row projected_row;
        std::vector<EvalValue> expression_stack;
        if (project_spec != NULL) projected_row.reserve(project_spec->output_count);
        for (size_t row_index = 0; row_index < input.rows.size(); ++row_index) {
            const Row &source = input.rows[row_index];
            const Row *row = &source;
            if (project_spec != NULL) {
                if (!evaluate_expression(project_node->node_id,
                        project_node->expression_offset,
                        project_node->expression_length, source, NULL,
                        &projected_row, &expression_stack))
                    return false;
                row = &projected_row;
            }
            const std::string key = group_key(*row, group_columns.data(),
                                              spec.group_count);
            const size_t partition =
                (std::hash<std::string>()(key) >> radix_shift) &
                (partition_count - 1);
            partition_rows[partition].push_back(row_index);
        }

        std::vector<SpillHandle> handles(partition_count);
        for (size_t partition = 0; partition < partition_count; ++partition) {
            if (!spill_selected_rows(index, input, partition_rows[partition],
                                     false, &handles[partition]))
                return false;
            std::vector<size_t>().swap(partition_rows[partition]);
        }
        std::vector<std::vector<size_t> >().swap(partition_rows);

        output->columns = spec.group_count + spec.aggregate_count;
        for (size_t partition = 0; partition < partition_count; ++partition) {
            Batch restored, partial;
            if (!read_spill_batch(index, handles[partition], &restored) ||
                !execute_aggregate(index, node, restored, &partial,
                                   project_node, true,
                                   static_cast<uint8_t>(radix_shift + radix_bits)))
                return false;
            output->rows.insert(output->rows.end(),
                std::make_move_iterator(partial.rows.begin()),
                std::make_move_iterator(partial.rows.end()));
        }
        uint64_t partition_work = 0;
        if (!checked_mul(input.rows.size(), 2, &partition_work))
            partition_work = std::numeric_limits<uint64_t>::max();
        add_cycles(partition_work, node);
        return true;
    }

    /* Blocking banked hash aggregate with multi-key and DISTINCT state. */
    bool execute_aggregate(uint32_t index, const CsdSqlPlanNode &node, const Batch &input,
                           Batch *output, const CsdSqlPlanNode *project_node = NULL,
                           bool allow_partition = true, uint8_t radix_shift = 0)
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
        const bool can_partition_early = allow_partition && spec->group_count != 0 &&
                                         (node.flags & CSD_SQL_NODE_SPILLABLE);
        std::atomic<uint64_t> incremental_state_bytes(0);
        std::atomic<uint64_t> processed_rows(0);
        std::atomic<uint64_t> projected_state_bytes(0);
        std::atomic<bool> partition_early(false);
        std::atomic<bool> oversized_group(false);
        std::atomic<bool> state_size_overflow(false);

        const auto update_atomic_max = [](std::atomic<uint64_t> *target,
                                          uint64_t value) {
            uint64_t current = target->load(std::memory_order_relaxed);
            while (current < value &&
                   !target->compare_exchange_weak(current, value,
                       std::memory_order_relaxed, std::memory_order_relaxed)) {}
        };
        const auto request_state_bytes = [&](uint64_t bytes) {
            if (!can_partition_early) return true;
            uint64_t current = incremental_state_bytes.load(std::memory_order_relaxed);
            for (;;) {
                uint64_t next = 0;
                if (!checked_add(current, bytes, &next)) {
                    state_size_overflow.store(true, std::memory_order_release);
                    return false;
                }
                if (next > operator_memory_capacity_) {
                    const uint64_t completed = std::max<uint64_t>(1,
                        processed_rows.load(std::memory_order_relaxed));
                    uint64_t projected = std::numeric_limits<uint64_t>::max();
                    uint64_t scaled = 0;
                    if (checked_mul(next, input.rows.size(), &scaled)) {
                        projected = scaled / completed;
                        if (scaled % completed != 0 &&
                            projected != std::numeric_limits<uint64_t>::max())
                            ++projected;
                    }
                    update_atomic_max(&projected_state_bytes,
                                      std::max<uint64_t>(next, projected));
                    partition_early.store(true, std::memory_order_release);
                    return false;
                }
                if (incremental_state_bytes.compare_exchange_weak(current, next,
                        std::memory_order_relaxed, std::memory_order_relaxed))
                    return true;
            }
        };
        const size_t tiles = has_distinct ? 1 :
            std::min<size_t>(parallel_workers_, std::max<size_t>(1, input.rows.size()));
        std::vector<std::unordered_map<std::string, size_t> > local_maps(tiles);
        std::vector<std::vector<AggregateState> > local_states(tiles);
        std::vector<std::vector<uint64_t> > local_state_bytes(tiles);
        std::vector<uint8_t> local_ok(tiles, 0);
        run_parallel(tiles, [&](size_t tile) {
            const size_t begin = input.rows.size() * tile / tiles;
            const size_t end = input.rows.size() * (tile + 1) / tiles;
            std::unordered_map<std::string, size_t> &group_map = local_maps[tile];
            std::vector<AggregateState> &states = local_states[tile];
            std::vector<uint64_t> &state_bytes = local_state_bytes[tile];
            const uint64_t minimum_state_bytes = std::max<uint64_t>(1,
                UINT64_C(64) + static_cast<uint64_t>(spec->aggregate_count) * 64);
            const size_t capacity_groups = static_cast<size_t>(std::min<uint64_t>(
                65536, operator_memory_capacity_ / minimum_state_bytes /
                       std::max<size_t>(1, tiles)));
            group_map.reserve(std::min<size_t>(end - begin,
                std::max<size_t>(1, capacity_groups)));
            Row projected_row;
            std::vector<EvalValue> expression_stack;
            if (project_spec != NULL) projected_row.reserve(project_spec->output_count);
            for (size_t row_index = begin; row_index < end; ++row_index) {
                if (partition_early.load(std::memory_order_acquire) ||
                    oversized_group.load(std::memory_order_acquire) ||
                    state_size_overflow.load(std::memory_order_acquire))
                    return;
                processed_rows.fetch_add(1, std::memory_order_relaxed);
                const Row *row_pointer = &input.rows[row_index];
                if (project_spec != NULL) {
                    if (!evaluate_expression(project_node->node_id, project_node->expression_offset,
                                             project_node->expression_length, input.rows[row_index],
                                             NULL, &projected_row, &expression_stack)) return;
                    row_pointer = &projected_row;
                }
                const Row &row = *row_pointer;
                const std::string key = spec->group_count == 0 ? std::string("global") :
                                        group_key(row, group_columns.data(), spec->group_count);
                size_t state_index = 0;
                std::unordered_map<std::string, size_t>::iterator found = group_map.find(key);
                if (found == group_map.end()) {
                    state_index = states.size();
                    AggregateState state;
                    state.key = key;
                    for (uint16_t group = 0; group < spec->group_count; ++group)
                        state.groups.push_back(row[group_columns[group]]);
                    uint64_t modeled_bytes = 0;
                    if (!checked_add(UINT64_C(64), state.key.size(), &modeled_bytes)) {
                        state_size_overflow.store(true, std::memory_order_release);
                        return;
                    }
                    for (Row::const_iterator group = state.groups.begin();
                         group != state.groups.end(); ++group)
                        if (!checked_add(modeled_bytes,
                                         sizeof(CsdSqlResultCell) + group->bytes.size(),
                                         &modeled_bytes)) {
                            state_size_overflow.store(true, std::memory_order_release);
                            return;
                        }
                    uint64_t accumulator_bytes = 0;
                    if (!checked_mul(spec->aggregate_count, UINT64_C(64),
                                     &accumulator_bytes) ||
                        !checked_add(modeled_bytes, accumulator_bytes,
                                     &modeled_bytes)) {
                        state_size_overflow.store(true, std::memory_order_release);
                        return;
                    }
                    if (can_partition_early && modeled_bytes > operator_memory_capacity_) {
                        oversized_group.store(true, std::memory_order_release);
                        return;
                    }
                    if (!request_state_bytes(modeled_bytes)) return;
                    state.count.assign(spec->aggregate_count, 0);
                    state.sum.assign(spec->aggregate_count, 0);
                    state.scale.assign(spec->aggregate_count, -1);
                    state.minimum.assign(spec->aggregate_count, Value());
                    state.maximum.assign(spec->aggregate_count, Value());
                    state.has_value.assign(spec->aggregate_count, false);
                    state.distinct.resize(spec->aggregate_count);
                    group_map[key] = state_index;
                    states.push_back(std::move(state));
                    state_bytes.push_back(modeled_bytes);
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
                        const std::string distinct_key = value_key(*value);
                        if (state.distinct[aggregate_index].find(distinct_key) !=
                            state.distinct[aggregate_index].end())
                            continue;
                        uint64_t distinct_bytes = 0, next_state_bytes = 0;
                        if (!checked_add(sizeof(uint64_t), distinct_key.size(),
                                         &distinct_bytes) ||
                            !checked_add(state_bytes[state_index], distinct_bytes,
                                         &next_state_bytes)) {
                            state_size_overflow.store(true, std::memory_order_release);
                            return;
                        }
                        if (can_partition_early &&
                            next_state_bytes > operator_memory_capacity_) {
                            oversized_group.store(true, std::memory_order_release);
                            return;
                        }
                        if (!request_state_bytes(distinct_bytes)) return;
                        state.distinct[aggregate_index].insert(distinct_key);
                        state_bytes[state_index] = next_state_bytes;
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
        if (failed_) return false;
        if (state_size_overflow.load(std::memory_order_acquire))
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
        if (oversized_group.load(std::memory_order_acquire))
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x504);
        if (partition_early.load(std::memory_order_acquire)) {
            for (size_t tile = 0; tile < local_maps.size(); ++tile) {
                std::unordered_map<std::string, size_t>().swap(local_maps[tile]);
                std::vector<AggregateState>().swap(local_states[tile]);
                std::vector<uint64_t>().swap(local_state_bytes[tile]);
            }
            std::vector<std::unordered_map<std::string, size_t> >().swap(local_maps);
            std::vector<std::vector<AggregateState> >().swap(local_states);
            std::vector<std::vector<uint64_t> >().swap(local_state_bytes);
            uint64_t partition_estimate = projected_state_bytes.load(
                std::memory_order_relaxed);
            uint64_t over_capacity = 0;
            if (checked_add(operator_memory_capacity_, 1, &over_capacity))
                partition_estimate = std::max(partition_estimate, over_capacity);
            return execute_partitioned_aggregate(index, node, input, output,
                project_node, *spec, group_columns, partition_estimate,
                radix_shift);
        }
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
        /* Merged states are now self-contained. Release per-lane maps,
         * duplicate partial states and accounting vectors before obtaining
         * the arena lease or emitting output. */
        for (size_t tile = 0; tile < local_maps.size(); ++tile) {
            std::unordered_map<std::string, size_t>().swap(local_maps[tile]);
            std::vector<AggregateState>().swap(local_states[tile]);
            std::vector<uint64_t>().swap(local_state_bytes[tile]);
        }
        std::vector<std::unordered_map<std::string, size_t> >().swap(local_maps);
        std::vector<std::vector<AggregateState> >().swap(local_states);
        std::vector<std::vector<uint64_t> >().swap(local_state_bytes);
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

        /* Model the compact FPGA hash-table state rather than charging the
         * entire input batch. Group keys, accumulators and DISTINCT entries
         * stay live until result emission completes. */
        uint64_t aggregate_state_bytes = 0;
        uint64_t largest_aggregate_state_bytes = 0;
        for (std::vector<AggregateState>::const_iterator state = states.begin();
             state != states.end(); ++state) {
            const uint64_t state_begin = aggregate_state_bytes;
            if (!checked_add(aggregate_state_bytes, 64 + state->key.size(),
                             &aggregate_state_bytes))
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
            for (Row::const_iterator group = state->groups.begin();
                 group != state->groups.end(); ++group)
                if (!checked_add(aggregate_state_bytes,
                                 sizeof(CsdSqlResultCell) + group->bytes.size(),
                                 &aggregate_state_bytes))
                    return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
            uint64_t accumulator_bytes = 0;
            if (!checked_mul(spec->aggregate_count, UINT64_C(64),
                             &accumulator_bytes) ||
                !checked_add(aggregate_state_bytes, accumulator_bytes,
                             &aggregate_state_bytes))
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
            for (std::vector<std::unordered_set<std::string> >::const_iterator set =
                     state->distinct.begin(); set != state->distinct.end(); ++set)
                for (std::unordered_set<std::string>::const_iterator value =
                         set->begin(); value != set->end(); ++value)
                    if (!checked_add(aggregate_state_bytes,
                                     sizeof(uint64_t) + value->size(),
                                     &aggregate_state_bytes))
                        return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
            largest_aggregate_state_bytes = std::max<uint64_t>(
                largest_aggregate_state_bytes,
                aggregate_state_bytes - state_begin);
        }
        OperatorMemoryLease aggregate_memory(this, aggregate_state_bytes,
                                              !allow_partition || radix_shift != 0);
        const bool spill_aggregate = !aggregate_memory.acquired();
        if (spill_aggregate && !(node.flags & CSD_SQL_NODE_SPILLABLE))
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
        if (spill_aggregate && allow_partition && spec->group_count != 0) {
            if (largest_aggregate_state_bytes > operator_memory_capacity_)
                return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x504);
            std::unordered_map<std::string, size_t>().swap(group_map);
            std::vector<AggregateState>().swap(states);
            return execute_partitioned_aggregate(index, node, input, output,
                project_node, *spec, group_columns, aggregate_state_bytes,
                radix_shift);
        }
        if (spill_aggregate && !allow_partition)
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x502);

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
        if (spill_aggregate && !spill_batch(index, *output)) return false;
        if (project_node != NULL) add_cycles(input.rows.size(), *project_node);
        add_cycles(input.rows.size() * 2 + output->rows.size(), node);
        return true;
    }
