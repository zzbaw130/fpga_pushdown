    /*
     * Trinity-style database page decoder.  A decode task is one (page,
     * tuple-lane) pair.  The shared bounded worker pool therefore exposes
     * page-level parallelism across different pages and tuple-level parallelism
     * inside a page without nested software thread pools.
     */
    bool execute_scan(uint32_t index, const CsdSqlPlanNode &node, Batch *output,
                      const std::vector<uint32_t> *pipeline = NULL)
    {
        const CsdSqlScanSpec *spec = NULL;
        uint64_t descriptors_bytes = 0;
        if (!expression_as(node, sizeof(CsdSqlScanSpec), &spec) ||
            !checked_mul(spec->attr_count, sizeof(CsdSqlColumnSpec), &descriptors_bytes) ||
            node.expression_length < sizeof(CsdSqlScanSpec) + descriptors_bytes)
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        const CsdSqlColumnSpec *columns = reinterpret_cast<const CsdSqlColumnSpec *>(spec + 1);
		if (!(header_->flags & CSD_SQL_PLAN_FROZEN_SNAPSHOT))
			return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x101);
		if (!(spec->flags & CSD_SQL_SCAN_FROZEN_PAGES))
			return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x102);
		if (spec->attr_count == 0)
			return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x103);
		if (spec->output_count == 0)
			return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x104);
		if (spec->input_range >= static_cast<uint32_t>(args_->numr))
			return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x105);
		if (args_->mr_addr[spec->input_range] == NULL)
			return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x106);
		if (args_->mr_len[spec->input_range] < 0)
			return fail(CSD_SQL_STATUS_UNSUPPORTED, index, 0x107);
        for (uint16_t attribute = 0; attribute < spec->attr_count; ++attribute) {
            const CsdSqlColumnSpec &column = columns[attribute];
            if (column.attnum != attribute + 1 || !is_supported_type(column.datum_type) || column.attlen == 0 ||
                (column.alignment != 1 && column.alignment != 2 && column.alignment != 4 && column.alignment != 8) ||
                ((column.flags & CSD_SQL_COLUMN_OUTPUT) && column.output_column >= spec->output_count))
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        }
        uint64_t first_byte = 0, pages_bytes = 0;
        if (!checked_mul(spec->first_page, CSD_SQL_PAGE_BYTES, &first_byte) ||
            !checked_mul(spec->page_count, CSD_SQL_PAGE_BYTES, &pages_bytes))
            return fail(CSD_SQL_STATUS_BAD_PAGE, index);
        const bool chunked = (header_->flags & CSD_SQL_PLAN_CHUNKED_SCAN) != 0;
        if (chunked) {
            uint64_t extent_bytes = 0;
            if (args_->nvm_read == NULL)
                return fail(CSD_SQL_STATUS_NO_INPUT_RANGE, index, 0x108);
            if (spec->chunk_bytes < CSD_SQL_PAGE_BYTES ||
                (spec->chunk_bytes % CSD_SQL_PAGE_BYTES) != 0 ||
                spec->chunk_bytes > static_cast<uint64_t>(args_->mr_len[spec->input_range]) ||
                first_byte > spec->relation_bytes || pages_bytes > spec->relation_bytes - first_byte)
                return fail(CSD_SQL_STATUS_BAD_PAGE, index, 0x109);
            if (!checked_mul(spec->extent_count, sizeof(CsdSqlNvmExtent), &extent_bytes) ||
                !blob_range(spec->extent_offset, extent_bytes) ||
                (pages_bytes != 0 && spec->extent_count == 0))
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x10a);
        } else if (first_byte > static_cast<uint64_t>(args_->mr_len[spec->input_range]) ||
                   pages_bytes > static_cast<uint64_t>(args_->mr_len[spec->input_range]) - first_byte) {
            return fail(CSD_SQL_STATUS_BAD_PAGE, index);
        }
        unsigned char *input = static_cast<unsigned char *>(args_->mr_addr[spec->input_range]);
        uint16_t output_columns = spec->output_count;
        if (pipeline != NULL)
            for (std::vector<uint32_t>::const_iterator stage = pipeline->begin();
                 stage != pipeline->end(); ++stage) {
                if (nodes_[*stage].opcode == CSD_SQL_OP_PROJECT) {
                    const CsdSqlExpressionSpec *project_spec = NULL;
                    const CsdSqlExpressionInstruction *project_instructions = NULL;
                    if (!expression_program(*stage, nodes_[*stage].expression_offset,
                                            nodes_[*stage].expression_length,
                                            &project_spec, &project_instructions))
                        return false;
                    (void) project_instructions;
                    output_columns = project_spec->output_count;
                }
            }
        output->columns = output_columns;
        const uint32_t pages_per_window = chunked ?
            spec->chunk_bytes / CSD_SQL_PAGE_BYTES : std::max<uint32_t>(1, spec->page_count);
        uint64_t input_rows = 0;
        for (uint32_t page_offset = 0; page_offset < spec->page_count;) {
            const uint32_t window_pages = std::min<uint32_t>(pages_per_window,
                spec->page_count - page_offset);
            uint64_t window_offset = first_byte +
                static_cast<uint64_t>(page_offset) * CSD_SQL_PAGE_BYTES;
            const unsigned char *window = input + (chunked ? 0 : window_offset);
            if (chunked && !load_scan_window(index, *spec, window_offset,
                                             static_cast<uint64_t>(window_pages) * CSD_SQL_PAGE_BYTES,
                                             input))
                return false;
            Batch window_output;
            window_output.columns = output_columns;
            if (!decode_scan_window(index, *spec, columns, window, window_pages,
                                    output_columns, pipeline, &input_rows, &window_output))
                return false;
            if (header_->flags & CSD_SQL_PLAN_FDM_FIFO) {
                if (!fifo_forward_batch(index, window_output, output)) return false;
            } else {
                output->rows.insert(output->rows.end(),
                    std::make_move_iterator(window_output.rows.begin()),
                    std::make_move_iterator(window_output.rows.end()));
            }
            page_offset += window_pages;
        }
        add_input_rows(input_rows);
        add_cycles(static_cast<uint64_t>(spec->page_count) * 64 + input_rows, node);
        if (pipeline != NULL)
            for (std::vector<uint32_t>::const_iterator stage = pipeline->begin();
                 stage != pipeline->end(); ++stage)
                add_cycles(input_rows, nodes_[*stage]);
        return true;
    }

    bool load_scan_window(uint32_t index, const CsdSqlScanSpec &spec,
                          uint64_t logical_offset, uint64_t length,
                          unsigned char *target)
    {
        const CsdSqlNvmExtent *extents = reinterpret_cast<const CsdSqlNvmExtent *>(
            static_cast<const char *>(plan_data_) + spec.extent_offset);
        uint64_t copied = 0;
        for (uint32_t extent_index = 0; extent_index < spec.extent_count && copied < length;
             ++extent_index) {
            const CsdSqlNvmExtent &extent = extents[extent_index];
            uint64_t extent_end = 0;
            const uint64_t current = logical_offset + copied;
            if (!checked_add(extent.logical_offset, extent.length, &extent_end))
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x10b);
            if (current >= extent_end) continue;
            if (current < extent.logical_offset)
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x10c);
            const uint64_t within_extent = current - extent.logical_offset;
            const uint64_t transfer = std::min<uint64_t>(length - copied,
                extent.length - within_extent);
            const uint64_t physical_offset = extent.nvm_offset + within_extent;
            if (transfer == 0 ||
                args_->nvm_read(args_->service_context, extent.nsid,
                                physical_offset, target + copied, transfer) != 0)
                return fail(CSD_SQL_STATUS_NO_INPUT_RANGE, index, 0x10d);
            if (args_->cache_notify != NULL)
                (void) args_->cache_notify(args_->service_context, extent.nsid,
                                           spec.relation_oid, physical_offset,
                                           current, transfer,
                                           header_->plan_id);
            copied += transfer;
        }
        if (copied != length)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x10e);
        {
            uint64_t total = 0;
            std::lock_guard<std::mutex> guard(status_mutex_);
            if (checked_add(status_.flash_read_bytes, length, &total))
                status_.flash_read_bytes = total;
            else status_.flash_read_bytes = std::numeric_limits<uint64_t>::max();
            ++status_.input_chunks;
        }
        return true;
    }

    bool decode_scan_window(uint32_t index, const CsdSqlScanSpec &spec,
                            const CsdSqlColumnSpec *columns,
                            const unsigned char *input, uint32_t page_count,
                            uint16_t output_columns,
                            const std::vector<uint32_t> *pipeline,
                            uint64_t *input_rows, Batch *output)
    {
        struct DecodeTask {
            uint32_t first_page;
            uint32_t end_page;
            uint16_t lane;
            uint16_t lanes;
            size_t reserve_rows;
        };
        std::vector<uint16_t> item_counts(page_count, 0);
        const uint32_t tuple_bits = std::min<uint32_t>(kTupleBindWordBits,
            std::max<uint32_t>(32, static_cast<uint32_t>(spec.attr_count) * 64));
        const uint32_t bind_lanes = std::max<uint32_t>(1, kTupleBindWordBits / tuple_bits);
        for (uint32_t page_number = 0; page_number < page_count; ++page_number) {
            const unsigned char *page = input + static_cast<uint64_t>(page_number) * CSD_SQL_PAGE_BYTES;
            const uint16_t lower = read_u16(page + 12);
            if (lower < kPageHeaderBytes || lower > CSD_SQL_PAGE_BYTES ||
                ((lower - kPageHeaderBytes) % 4) != 0)
                return fail(CSD_SQL_STATUS_BAD_PAGE, index);
            item_counts[page_number] = (lower - kPageHeaderBytes) / 4;
        }
        std::vector<DecodeTask> tasks;
        if (page_count > 0) {
            const uint16_t lanes = static_cast<uint16_t>(std::max<uint32_t>(1,
                std::min<uint32_t>(parallel_workers_, bind_lanes)));
            const uint32_t page_tiles = std::min<uint32_t>(page_count,
                std::max<uint32_t>(1, parallel_workers_ * 2));
            tasks.reserve(static_cast<size_t>(page_tiles) * lanes);
            for (uint32_t tile = 0; tile < page_tiles; ++tile) {
                const uint32_t first_page = page_count * tile / page_tiles;
                const uint32_t end_page = page_count * (tile + 1) / page_tiles;
                for (uint16_t lane = 0; lane < lanes; ++lane) {
                    size_t reserve_rows = 0;
                    for (uint32_t page_number = first_page; page_number < end_page; ++page_number) {
                        const uint16_t item_count = item_counts[page_number];
                        if (item_count > lane)
                            reserve_rows += (item_count - lane + lanes - 1) / lanes;
                    }
                    DecodeTask task = {first_page, end_page, lane, lanes, reserve_rows};
                    tasks.push_back(task);
                }
            }
        }
        std::vector<Batch> decoded(tasks.size());
        std::vector<uint8_t> decoded_ok(tasks.size(), 0);
        std::vector<uint64_t> scanned_rows(tasks.size(), 0);
        run_parallel(tasks.size(), [&](size_t task_index) {
            const DecodeTask &task = tasks[task_index];
            decoded[task_index].columns = output_columns;
            decoded[task_index].rows.reserve(task.reserve_rows);
            for (uint32_t page_number = task.first_page; page_number < task.end_page; ++page_number) {
                const unsigned char *page = input + static_cast<uint64_t>(page_number) * CSD_SQL_PAGE_BYTES;
                if (!scan_page_lane(index, spec, columns, page, item_counts[page_number],
                                    task.lane, task.lanes, output_columns, pipeline,
                                    &scanned_rows[task_index], &decoded[task_index]))
                    return;
            }
            decoded_ok[task_index] = 1;
        });
        size_t output_rows = 0;
        uint64_t scanned_total = 0;
        for (size_t task_index = 0; task_index < tasks.size(); ++task_index)
        {
            output_rows += decoded[task_index].rows.size();
            scanned_total += scanned_rows[task_index];
        }
        output->rows.reserve(output->rows.size() + output_rows);
        for (size_t task_index = 0; task_index < tasks.size(); ++task_index) {
            if (!decoded_ok[task_index]) return false;
            output->rows.insert(output->rows.end(),
                std::make_move_iterator(decoded[task_index].rows.begin()),
                std::make_move_iterator(decoded[task_index].rows.end()));
        }
        *input_rows += scanned_total;
        return true;
    }

    bool scan_page_lane(uint32_t index, const CsdSqlScanSpec &spec, const CsdSqlColumnSpec *columns,
                        const unsigned char *page, uint16_t item_count, uint16_t lane, uint16_t lanes,
                        uint16_t output_columns, const std::vector<uint32_t> *pipeline,
                        uint64_t *scanned_rows, Batch *output)
    {
        output->columns = output_columns;
        output->rows.reserve((item_count + lanes - 1 - lane) / lanes);
        const uint16_t lower = read_u16(page + 12);
        Row row;
        row.reserve(spec.output_count);
        std::vector<Value> stage_values;
        std::vector<EvalValue> expression_stack;
        for (uint16_t item = lane; item < item_count; item = static_cast<uint16_t>(item + lanes)) {
            const uint32_t item_id = read_u32(page + kPageHeaderBytes + item * 4);
            const uint16_t offset = item_id & 0x7fff;
            const uint16_t flags = (item_id >> 15) & 0x3;
            const uint16_t length = item_id >> 17;
            if (flags != kLpNormal) {
                if ((spec.flags & CSD_SQL_SCAN_STRICT_PAGE_FORMAT) && (flags == 2 || flags == 3))
                    return fail(CSD_SQL_STATUS_BAD_PAGE, index);
                continue;
            }
            if (offset < lower || length < kHeapTupleHeaderBytes || offset > CSD_SQL_PAGE_BYTES ||
                length > CSD_SQL_PAGE_BYTES - offset)
                return fail(CSD_SQL_STATUS_BAD_PAGE, index);
            if (!decode_tuple(index, columns, spec.attr_count, spec.output_count, page + offset, length, &row))
                return false;
            ++*scanned_rows;
            bool selected = true;
            if (pipeline != NULL)
                for (std::vector<uint32_t>::const_iterator stage = pipeline->begin();
                     stage != pipeline->end(); ++stage) {
                    const CsdSqlPlanNode &stage_node = nodes_[*stage];
                    if (!evaluate_expression(*stage, stage_node.expression_offset,
                                             stage_node.expression_length, row, NULL,
                                             &stage_values, &expression_stack))
                        return false;
                    if (stage_node.opcode == CSD_SQL_OP_FILTER) {
                        if (stage_values.size() != 1 || stage_values[0].type != CSD_SQL_TYPE_BOOL)
                            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, *stage);
                        if (stage_values[0].is_null || stage_values[0].integer == 0) {
                            selected = false;
                            break;
                        }
                    } else row.swap(stage_values);
                }
            if (!selected) continue;
            output->rows.push_back(std::move(row));
        }
        return true;
    }

    bool decode_tuple(uint32_t index, const CsdSqlColumnSpec *columns, uint16_t attr_count,
                      uint16_t output_count, const unsigned char *tuple, uint16_t tuple_length, Row *row)
    {
        const uint16_t infomask = read_u16(tuple + 20);
        const uint8_t data_offset = tuple[22];
        if (data_offset < kHeapTupleHeaderBytes || data_offset > tuple_length)
            return fail(CSD_SQL_STATUS_BAD_PAGE, index);
        row->assign(output_count, Value());
        uint64_t cursor = data_offset;
        const bool has_nulls = (infomask & kHeapHasNull) != 0;
        if (has_nulls && kHeapTupleHeaderBytes + (attr_count + 7) / 8 > data_offset)
            return fail(CSD_SQL_STATUS_BAD_PAGE, index);
        for (uint16_t attribute = 0; attribute < attr_count; ++attribute) {
            const CsdSqlColumnSpec &column = columns[attribute];
            const bool materialize = (column.flags & CSD_SQL_COLUMN_OUTPUT) != 0;
            const bool is_null = has_nulls &&
                (tuple[kHeapTupleHeaderBytes + attribute / 8] & (1U << (attribute % 8))) == 0;
            Value value;
            value.type = column.datum_type;
            value.numeric_scale = column.datum_type == CSD_SQL_TYPE_NUMERIC ? column.numeric_scale : 0;
            value.is_null = is_null;
            if (!is_null) {
				/* PostgreSQL's att_align_pointer deliberately leaves a packed
				 * varlena unaligned when the current byte is already a varlena
				 * header.  Aligning every -1 attribute skips that header in real
				 * TPC-H tuples containing adjacent Numeric/text columns. */
				if (column.attlen != -1 || cursor >= tuple_length || tuple[cursor] == 0)
					cursor = align_up(cursor, column.alignment);
                if (cursor >= tuple_length) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
                uint64_t consumed = 0;
                if (!decode_datum(index, column, tuple + cursor, tuple_length - cursor,
                                  materialize, &value, &consumed))
                    return false;
                if (consumed > tuple_length - cursor)
                    return fail(CSD_SQL_STATUS_BAD_PAGE, index);
                cursor += consumed;
                if (materialize && (column.flags & CSD_SQL_COLUMN_TRIM_SPACE) &&
					value.type == CSD_SQL_TYPE_BYTES)
					while (!value.bytes.empty() && value.bytes[value.bytes.size() - 1] == ' ')
						value.bytes.resize(value.bytes.size() - 1);
            }
            if (materialize) (*row)[column.output_column] = std::move(value);
        }
        return true;
    }

    bool decode_datum(uint32_t index, const CsdSqlColumnSpec &column, const unsigned char *source,
                      uint64_t available, bool materialize, Value *value, uint64_t *consumed)
    {
        if (column.attlen > 0) {
            if (available < static_cast<uint64_t>(column.attlen)) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
            *consumed = static_cast<uint64_t>(column.attlen);
            if ((column.datum_type == CSD_SQL_TYPE_INT16 && column.attlen != 2) ||
                ((column.datum_type == CSD_SQL_TYPE_INT32 || column.datum_type == CSD_SQL_TYPE_DATE) &&
                 column.attlen != 4) ||
                (column.datum_type == CSD_SQL_TYPE_INT64 && column.attlen != 8) ||
                (column.datum_type == CSD_SQL_TYPE_BOOL && column.attlen != 1) ||
                (column.datum_type != CSD_SQL_TYPE_INT16 && column.datum_type != CSD_SQL_TYPE_INT32 &&
                 column.datum_type != CSD_SQL_TYPE_DATE && column.datum_type != CSD_SQL_TYPE_INT64 &&
                 column.datum_type != CSD_SQL_TYPE_BOOL && column.datum_type != CSD_SQL_TYPE_BYTES))
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
            if (!materialize) return true;
            switch (column.datum_type) {
            case CSD_SQL_TYPE_INT16: value->integer = read_i16(source); return true;
            case CSD_SQL_TYPE_INT32:
            case CSD_SQL_TYPE_DATE: value->integer = read_i32(source); return true;
            case CSD_SQL_TYPE_INT64: value->integer = read_i64(source); return true;
            case CSD_SQL_TYPE_BOOL: value->integer = source[0] == 0 ? 0 : 1; return true;
            case CSD_SQL_TYPE_BYTES: value->bytes.assign(reinterpret_cast<const char *>(source), column.attlen); return true;
			default: return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
				(static_cast<uint32_t>(column.attnum) << 16) | 1);
            }
        }
        if (column.attlen != -1 || available == 0) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        uint64_t header_bytes = 0, length = 0;
        if (source[0] & 0x01U) {
			if (source[0] == 0x01U) return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
				(static_cast<uint32_t>(column.attnum) << 16) | 2);
            header_bytes = 1;
            length = source[0] >> 1;
        } else {
            if (available < 4) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
            const uint32_t header = read_u32(source);
			if (header & 0x03U) return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
				(static_cast<uint32_t>(column.attnum) << 16) | 3);
            header_bytes = 4;
            length = header >> 2;
        }
        if (length < header_bytes || length > available) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
        *consumed = length;
        if (!materialize && (column.datum_type == CSD_SQL_TYPE_BYTES ||
                             column.datum_type == CSD_SQL_TYPE_NUMERIC))
            return true;
        if (column.datum_type == CSD_SQL_TYPE_BYTES) {
            value->bytes.assign(reinterpret_cast<const char *>(source + header_bytes), length - header_bytes);
            return true;
        }
        if (column.datum_type == CSD_SQL_TYPE_NUMERIC)
			return decode_numeric(index, column.attnum, source + header_bytes,
				length - header_bytes, column.numeric_scale, value);
		return fail(CSD_SQL_STATUS_UNSUPPORTED, index,
			(static_cast<uint32_t>(column.attnum) << 16) | 4);
    }

	bool decode_numeric(uint32_t index, uint16_t attnum, const unsigned char *source,
						uint64_t length, int32_t target_scale, Value *value)
    {
		const uint32_t detail = static_cast<uint32_t>(attnum) << 16;
		if (length < 2 || target_scale < -1 || target_scale > 18)
			return fail(CSD_SQL_STATUS_UNSUPPORTED, index, detail | 5);
        const uint16_t header = read_u16(source);
        const uint16_t flag = header & 0xc000U;
        bool negative = false;
        int16_t weight = 0;
		int16_t source_scale = 0;
        uint64_t header_bytes = 0;
        if (flag == 0x8000U) {
            negative = (header & 0x2000U) != 0;
			source_scale = static_cast<int16_t>((header & 0x1f80U) >> 7);
			/* NumericShort stores weight as a signed 7-bit field.  Do not
			 * sign-extend the unrelated sign/dscale bits in the same header. */
			weight = static_cast<int16_t>(header & 0x003fU);
			if (header & 0x0040U)
				weight = static_cast<int16_t>(weight | static_cast<int16_t>(~0x003f));
            header_bytes = 2;
        } else if (flag == 0x0000U || flag == 0x4000U) {
            if (length < 4) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
            negative = flag == 0x4000U;
			source_scale = static_cast<int16_t>(header & 0x3fffU);
            weight = read_i16(source + 2);
            header_bytes = 4;
		} else return fail(CSD_SQL_STATUS_UNSUPPORTED, index, detail | 6);
		if (source_scale > 18)
			return fail(CSD_SQL_STATUS_UNSUPPORTED, index, detail | 11);
		if (target_scale < 0)
			target_scale = source_scale;
		value->numeric_scale = static_cast<int16_t>(target_scale);
        if ((length - header_bytes) % 2) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
        __int128 scaled = 0;
        const uint64_t digits = (length - header_bytes) / 2;
        for (uint64_t digit_index = 0; digit_index < digits; ++digit_index) {
            const uint16_t digit = read_u16(source + header_bytes + digit_index * 2);
            if (digit >= 10000) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
            const int64_t exponent = (static_cast<int64_t>(weight) - static_cast<int64_t>(digit_index)) * 4 + target_scale;
            __int128 part = digit;
            if (exponent >= 0) {
				if (exponent > 18) return fail(CSD_SQL_STATUS_UNSUPPORTED, index, detail | 7);
                for (int64_t power = 0; power < exponent; ++power) part *= 10;
            } else {
                const int64_t divisor_power = -exponent;
				if (divisor_power > 4) return fail(CSD_SQL_STATUS_UNSUPPORTED, index, detail | 8);
                int64_t divisor = 1;
                for (int64_t power = 0; power < divisor_power; ++power) divisor *= 10;
				if (digit % divisor)
					return fail(CSD_SQL_STATUS_UNSUPPORTED, index, detail | 9);
                part /= divisor;
            }
            scaled += part;
        }
        if (negative) scaled = -scaled;
        if (scaled > std::numeric_limits<int64_t>::max() || scaled < std::numeric_limits<int64_t>::min())
			return fail(CSD_SQL_STATUS_UNSUPPORTED, index, detail | 10);
        value->integer = static_cast<int64_t>(scaled);
        return true;
    }
