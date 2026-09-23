    /* Expression projection: tuple tiles model replicated FPGA data paths. */
    bool execute_project(uint32_t index, const CsdSqlPlanNode &node, const Batch &input, Batch *output)
    {
        const CsdSqlExpressionSpec *spec = NULL;
        const CsdSqlExpressionInstruction *instructions = NULL;
        if (!expression_program(index, node.expression_offset, node.expression_length, &spec, &instructions))
            return false;
        (void) instructions;
        const size_t tiles = std::min<size_t>(parallel_workers_, std::max<size_t>(1, input.rows.size()));
        std::vector<Batch> projected(tiles);
        std::vector<uint8_t> projected_ok(tiles, 0);
        run_parallel(tiles, [&](size_t tile) {
            const size_t begin = input.rows.size() * tile / tiles;
            const size_t end = input.rows.size() * (tile + 1) / tiles;
            projected[tile].columns = spec->output_count;
            for (size_t row_index = begin; row_index < end; ++row_index) {
                std::vector<Value> values;
                if (!evaluate_expression(index, node.expression_offset, node.expression_length,
                                         input.rows[row_index], NULL, &values)) return;
                projected[tile].rows.push_back(Row(values.begin(), values.end()));
            }
            projected_ok[tile] = 1;
        });
        output->columns = spec->output_count;
        for (size_t tile = 0; tile < tiles; ++tile) {
            if (!projected_ok[tile]) return false;
            output->rows.insert(output->rows.end(), projected[tile].rows.begin(), projected[tile].rows.end());
        }
        add_cycles(input.rows.size(), node);
        return true;
    }
