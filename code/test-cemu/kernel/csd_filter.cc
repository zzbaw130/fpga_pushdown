    /* Stateless expression stage: one worker consumes one contiguous tuple tile. */
    bool execute_filter(uint32_t index, const CsdSqlPlanNode &node, const Batch &input, Batch *output)
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
            for (size_t row_index = begin; row_index < end; ++row_index) {
                std::vector<Value> values;
                if (!evaluate_expression(index, node.expression_offset, node.expression_length,
                                         input.rows[row_index], NULL, &values)) return;
                if (values[0].type != CSD_SQL_TYPE_BOOL) return;
                if (!values[0].is_null && values[0].integer != 0)
                    selected[tile].rows.push_back(input.rows[row_index]);
            }
            selected_ok[tile] = 1;
        });
        output->columns = input.columns;
        for (size_t tile = 0; tile < tiles; ++tile) {
            if (!selected_ok[tile]) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
            output->rows.insert(output->rows.end(), selected[tile].rows.begin(), selected[tile].rows.end());
        }
        add_cycles(input.rows.size(), node);
        return true;
    }
