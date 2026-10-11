    /* Expression projection: tuple tiles model replicated FPGA data paths. */
    bool execute_project(uint32_t index, const CsdSqlPlanNode &node, Batch &input,
                         Batch *output, bool consume_input)
    {
        const CsdSqlExpressionSpec *spec = NULL;
        const CsdSqlExpressionInstruction *instructions = NULL;
        if (!expression_program(index, node.expression_offset, node.expression_length, &spec, &instructions))
            return false;
        std::vector<uint16_t> direct_columns;
        if (spec->instruction_count == spec->output_count * 2) {
            direct_columns.reserve(spec->output_count);
            for (uint16_t column = 0; column < spec->output_count; ++column) {
                const CsdSqlExpressionInstruction &push = instructions[column * 2];
                const CsdSqlExpressionInstruction &emit = instructions[column * 2 + 1];
                if (push.opcode != CSD_SQL_EXPR_PUSH_COLUMN || emit.opcode != CSD_SQL_EXPR_EMIT ||
                    push.operand >= input.columns) {
                    direct_columns.clear();
                    break;
                }
                direct_columns.push_back(push.operand);
            }
        }
        bool identity = direct_columns.size() == input.columns && spec->output_count == input.columns;
        for (uint16_t column = 0; identity && column < input.columns; ++column)
            identity = direct_columns[column] == column;
        if (identity && consume_input) {
            output->columns = input.columns;
            output->rows.swap(input.rows);
            add_cycles(output->rows.size(), node);
            return true;
        }
        const size_t tiles = std::min<size_t>(parallel_workers_, std::max<size_t>(1, input.rows.size()));
        std::vector<Batch> projected(tiles);
        std::vector<uint8_t> projected_ok(tiles, 0);
        run_parallel(tiles, [&](size_t tile) {
            const size_t begin = input.rows.size() * tile / tiles;
            const size_t end = input.rows.size() * (tile + 1) / tiles;
            projected[tile].columns = spec->output_count;
            projected[tile].rows.reserve(end - begin);
            std::vector<EvalValue> expression_stack;
            for (size_t row_index = begin; row_index < end; ++row_index) {
                Row values;
                if (!direct_columns.empty()) {
                    values.reserve(direct_columns.size());
                    for (std::vector<uint16_t>::const_iterator column = direct_columns.begin();
                         column != direct_columns.end(); ++column) {
                        if (consume_input) values.push_back(std::move(input.rows[row_index][*column]));
                        else values.push_back(input.rows[row_index][*column]);
                    }
                } else if (!evaluate_expression(index, node.expression_offset, node.expression_length,
                                                input.rows[row_index], NULL, &values,
                                                &expression_stack)) return;
                projected[tile].rows.push_back(std::move(values));
            }
            projected_ok[tile] = 1;
        });
        output->columns = spec->output_count;
        size_t output_rows = 0;
        for (size_t tile = 0; tile < tiles; ++tile) output_rows += projected[tile].rows.size();
        output->rows.reserve(output_rows);
        for (size_t tile = 0; tile < tiles; ++tile) {
            if (!projected_ok[tile]) return false;
            output->rows.insert(output->rows.end(),
                std::make_move_iterator(projected[tile].rows.begin()),
                std::make_move_iterator(projected[tile].rows.end()));
        }
        add_cycles(input.rows.size(), node);
        return true;
    }
