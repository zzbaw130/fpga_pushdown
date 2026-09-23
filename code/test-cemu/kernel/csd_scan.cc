    /*
     * Trinity-style database page decoder.  A decode task is one (page,
     * tuple-lane) pair.  The shared bounded worker pool therefore exposes
     * page-level parallelism across different pages and tuple-level parallelism
     * inside a page without nested software thread pools.
     */
    bool execute_scan(uint32_t index, const CsdSqlPlanNode &node, Batch *output)
    {
        const CsdSqlScanSpec *spec = NULL;
        uint64_t descriptors_bytes = 0;
        if (!expression_as(node, sizeof(CsdSqlScanSpec), &spec) ||
            !checked_mul(spec->attr_count, sizeof(CsdSqlColumnSpec), &descriptors_bytes) ||
            node.expression_length < sizeof(CsdSqlScanSpec) + descriptors_bytes)
            return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        const CsdSqlColumnSpec *columns = reinterpret_cast<const CsdSqlColumnSpec *>(spec + 1);
        if (!(header_->flags & CSD_SQL_PLAN_FROZEN_SNAPSHOT) ||
            !(spec->flags & CSD_SQL_SCAN_FROZEN_PAGES) || spec->attr_count == 0 ||
            spec->output_count == 0 || spec->input_range >= static_cast<uint32_t>(args_->numr) ||
            args_->mr_addr[spec->input_range] == NULL || args_->mr_len[spec->input_range] < 0)
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
        for (uint16_t attribute = 0; attribute < spec->attr_count; ++attribute) {
            const CsdSqlColumnSpec &column = columns[attribute];
            if (column.attnum != attribute + 1 || !is_supported_type(column.datum_type) || column.attlen == 0 ||
                (column.alignment != 1 && column.alignment != 2 && column.alignment != 4 && column.alignment != 8) ||
                ((column.flags & CSD_SQL_COLUMN_OUTPUT) && column.output_column >= spec->output_count))
                return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        }
        uint64_t first_byte = 0, pages_bytes = 0;
        if (!checked_mul(spec->first_page, CSD_SQL_PAGE_BYTES, &first_byte) ||
            !checked_mul(spec->page_count, CSD_SQL_PAGE_BYTES, &pages_bytes) ||
            first_byte > static_cast<uint64_t>(args_->mr_len[spec->input_range]) ||
            pages_bytes > static_cast<uint64_t>(args_->mr_len[spec->input_range]) - first_byte)
            return fail(CSD_SQL_STATUS_BAD_PAGE, index);
        const unsigned char *input = static_cast<const unsigned char *>(args_->mr_addr[spec->input_range]);
        struct DecodeTask { uint32_t page; uint16_t lane; uint16_t lanes; uint16_t item_count; };
        std::vector<DecodeTask> tasks;
        const uint32_t tuple_bits = std::min<uint32_t>(kTupleBindWordBits,
            std::max<uint32_t>(32, static_cast<uint32_t>(spec->attr_count) * 64));
        const uint32_t bind_lanes = std::max<uint32_t>(1, kTupleBindWordBits / tuple_bits);
        for (uint32_t page_number = 0; page_number < spec->page_count; ++page_number) {
            const unsigned char *page = input + first_byte + static_cast<uint64_t>(page_number) * CSD_SQL_PAGE_BYTES;
            const uint16_t lower = read_u16(page + 12);
            if (lower < kPageHeaderBytes || lower > CSD_SQL_PAGE_BYTES ||
                ((lower - kPageHeaderBytes) % 4) != 0)
                return fail(CSD_SQL_STATUS_BAD_PAGE, index);
            const uint16_t item_count = (lower - kPageHeaderBytes) / 4;
            const uint16_t lanes = static_cast<uint16_t>(std::min<uint32_t>(
                std::min<uint32_t>(parallel_workers_, bind_lanes), std::max<uint16_t>(1, item_count)));
            for (uint16_t lane = 0; lane < lanes; ++lane) {
                DecodeTask task = {page_number, lane, lanes, item_count};
                tasks.push_back(task);
            }
        }
        std::vector<Batch> decoded(tasks.size());
        std::vector<uint8_t> decoded_ok(tasks.size(), 0);
        run_parallel(tasks.size(), [&](size_t task_index) {
            const DecodeTask &task = tasks[task_index];
            const unsigned char *page = input + first_byte + static_cast<uint64_t>(task.page) * CSD_SQL_PAGE_BYTES;
            decoded_ok[task_index] = scan_page_lane(index, *spec, columns, page, task.item_count,
                                                    task.lane, task.lanes, &decoded[task_index]) ? 1 : 0;
        });
        output->columns = spec->output_count;
        for (size_t task_index = 0; task_index < tasks.size(); ++task_index) {
            if (!decoded_ok[task_index]) return false;
            output->rows.insert(output->rows.end(), decoded[task_index].rows.begin(), decoded[task_index].rows.end());
        }
        add_input_rows(output->rows.size());
        add_cycles(static_cast<uint64_t>(spec->page_count) * 64 + output->rows.size(), node);
        return true;
    }

    bool scan_page_lane(uint32_t index, const CsdSqlScanSpec &spec, const CsdSqlColumnSpec *columns,
                        const unsigned char *page, uint16_t item_count, uint16_t lane, uint16_t lanes, Batch *output)
    {
        output->columns = spec.output_count;
        const uint16_t lower = read_u16(page + 12);
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
            Row row;
            if (!decode_tuple(index, columns, spec.attr_count, spec.output_count, page + offset, length, &row))
                return false;
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
            const bool is_null = has_nulls &&
                (tuple[kHeapTupleHeaderBytes + attribute / 8] & (1U << (attribute % 8))) == 0;
            Value value;
            value.type = column.datum_type;
            value.numeric_scale = column.datum_type == CSD_SQL_TYPE_NUMERIC ? column.numeric_scale : 0;
            value.is_null = is_null;
            if (!is_null) {
                cursor = align_up(cursor, column.alignment);
                if (cursor >= tuple_length) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
                uint64_t consumed = 0;
                if (!decode_datum(index, column, tuple + cursor, tuple_length - cursor, &value, &consumed))
                    return false;
                if (consumed > tuple_length - cursor)
                    return fail(CSD_SQL_STATUS_BAD_PAGE, index);
                cursor += consumed;
            }
            if (column.flags & CSD_SQL_COLUMN_OUTPUT) (*row)[column.output_column] = value;
        }
        return true;
    }

    bool decode_datum(uint32_t index, const CsdSqlColumnSpec &column, const unsigned char *source,
                      uint64_t available, Value *value, uint64_t *consumed)
    {
        if (column.attlen > 0) {
            if (available < static_cast<uint64_t>(column.attlen)) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
            *consumed = static_cast<uint64_t>(column.attlen);
            switch (column.datum_type) {
            case CSD_SQL_TYPE_INT16: if (column.attlen != 2) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index); value->integer = read_i16(source); return true;
            case CSD_SQL_TYPE_INT32:
            case CSD_SQL_TYPE_DATE: if (column.attlen != 4) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index); value->integer = read_i32(source); return true;
            case CSD_SQL_TYPE_INT64: if (column.attlen != 8) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index); value->integer = read_i64(source); return true;
            case CSD_SQL_TYPE_BOOL: if (column.attlen != 1) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index); value->integer = source[0] == 0 ? 0 : 1; return true;
            case CSD_SQL_TYPE_BYTES: value->bytes.assign(reinterpret_cast<const char *>(source), column.attlen); return true;
            default: return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
            }
        }
        if (column.attlen != -1 || available == 0) return fail(CSD_SQL_STATUS_BAD_EXPRESSION, index);
        uint64_t header_bytes = 0, length = 0;
        if (source[0] & 0x01U) {
            if (source[0] == 0x01U) return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
            header_bytes = 1;
            length = source[0] >> 1;
        } else {
            if (available < 4) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
            const uint32_t header = read_u32(source);
            if (header & 0x03U) return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
            header_bytes = 4;
            length = header >> 2;
        }
        if (length < header_bytes || length > available) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
        *consumed = length;
        if (column.datum_type == CSD_SQL_TYPE_BYTES) {
            value->bytes.assign(reinterpret_cast<const char *>(source + header_bytes), length - header_bytes);
            return true;
        }
        if (column.datum_type == CSD_SQL_TYPE_NUMERIC)
            return decode_numeric(index, source + header_bytes, length - header_bytes, column.numeric_scale, value);
        return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
    }

    bool decode_numeric(uint32_t index, const unsigned char *source, uint64_t length, int32_t target_scale, Value *value)
    {
        if (length < 2 || target_scale < 0 || target_scale > 18) return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
        const uint16_t header = read_u16(source);
        const uint16_t flag = header & 0xc000U;
        bool negative = false;
        int16_t weight = 0;
        uint64_t header_bytes = 0;
        if (flag == 0x8000U) {
            negative = (header & 0x2000U) != 0;
            weight = static_cast<int16_t>((header & 0x0040U) ? (header | static_cast<uint16_t>(~0x003fU)) : (header & 0x003fU));
            header_bytes = 2;
        } else if (flag == 0x0000U || flag == 0x4000U) {
            if (length < 4) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
            negative = flag == 0x4000U;
            weight = read_i16(source + 2);
            header_bytes = 4;
        } else return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
        if ((length - header_bytes) % 2) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
        __int128 scaled = 0;
        const uint64_t digits = (length - header_bytes) / 2;
        for (uint64_t digit_index = 0; digit_index < digits; ++digit_index) {
            const uint16_t digit = read_u16(source + header_bytes + digit_index * 2);
            if (digit >= 10000) return fail(CSD_SQL_STATUS_BAD_PAGE, index);
            const int64_t exponent = (static_cast<int64_t>(weight) - static_cast<int64_t>(digit_index)) * 4 + target_scale;
            __int128 part = digit;
            if (exponent >= 0) {
                if (exponent > 18) return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
                for (int64_t power = 0; power < exponent; ++power) part *= 10;
            } else {
                const int64_t divisor_power = -exponent;
                if (divisor_power > 4) return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
                int64_t divisor = 1;
                for (int64_t power = 0; power < divisor_power; ++power) divisor *= 10;
                if (digit % divisor) return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
                part /= divisor;
            }
            scaled += part;
        }
        if (negative) scaled = -scaled;
        if (scaled > std::numeric_limits<int64_t>::max() || scaled < std::numeric_limits<int64_t>::min())
            return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
        value->integer = static_cast<int64_t>(scaled);
        return true;
    }
