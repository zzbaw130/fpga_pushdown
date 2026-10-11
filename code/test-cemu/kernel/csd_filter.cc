    /* Stateless expression stage: one worker consumes one contiguous tuple tile. */
    bool execute_filter(uint32_t index, const CsdSqlPlanNode &node, Batch &input,
                        Batch *output, bool consume_input)
    {
        const CsdSqlExpressionSpec *spec = NULL;
        const CsdSqlExpressionInstruction *instructions = NULL;
        if (!expression_program(index, node.expression_offset, node.expression_length, &spec, &instructions) ||
            spec->output_count != 1)
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        (void) instructions;
        const size_t tiles = std::min<size_t>(parallel_workers_, std::max<size_t>(1, input.rows.size()));
        std::vector<Batch> selected(tiles);
        std::vector<uint8_t> selected_ok(tiles, 0);
        run_parallel(tiles, [&](size_t tile) {
            const size_t begin = input.rows.size() * tile / tiles;
            const size_t end = input.rows.size() * (tile + 1) / tiles;
            selected[tile].columns = input.columns;
            selected[tile].rows.reserve(end - begin);
            std::vector<Value> values;
            std::vector<EvalValue> expression_stack;
            values.reserve(spec->output_count);
            for (size_t row_index = begin; row_index < end; ++row_index) {
                if (!evaluate_expression(index, node.expression_offset, node.expression_length,
                                         input.rows[row_index], NULL, &values,
                                         &expression_stack)) return;
                if (values[0].type != CSD_SQL_TYPE_BOOL) return;
                if (!values[0].is_null && values[0].integer != 0) {
                    if (consume_input) selected[tile].rows.push_back(std::move(input.rows[row_index]));
                    else selected[tile].rows.push_back(input.rows[row_index]);
                }
            }
            selected_ok[tile] = 1;
        });
        output->columns = input.columns;
        size_t output_rows = 0;
        for (size_t tile = 0; tile < tiles; ++tile) output_rows += selected[tile].rows.size();
        output->rows.reserve(output_rows);
        for (size_t tile = 0; tile < tiles; ++tile) {
            if (!selected_ok[tile]) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
            output->rows.insert(output->rows.end(),
                std::make_move_iterator(selected[tile].rows.begin()),
                std::make_move_iterator(selected[tile].rows.end()));
        }
        add_cycles(input.rows.size(), node);
        return true;
    }
