    bool execute_materialize(uint32_t index, const CsdSqlPlanNode &node, const Batch &input, Batch *output)
    {
        *output = input;
        if (!spill_if_needed(index, node, input)) return false;
        add_cycles(input.rows.size(), node);
        return true;
    }

    bool execute_limit(uint32_t index, const CsdSqlPlanNode &node, const Batch &input, Batch *output)
    {
        *output = input;
        if (node.arg0 < output->rows.size()) output->rows.resize(node.arg0);
        add_cycles(output->rows.size(), node);
        (void) index;
        return true;
    }

    bool spill_if_needed(uint32_t index, const CsdSqlPlanNode &node, const Batch &batch)
    {
        if (batch_bytes(batch) <= work_mem_bytes_) return true;
        if (!(node.flags & CSD_SQL_NODE_SPILLABLE)) return fail(CSD_SQL_STATUS_UNSUPPORTED, index);
        return spill_batch(index, batch);
    }

    /* Flash spill allocation is intentionally serialized: all CSD cores share one extent allocator. */
    bool spill_batch(uint32_t index, const Batch &batch)
    {
        std::lock_guard<std::mutex> spill_guard(spill_mutex_);
        if (header_->spill_range == CSD_SQL_INVALID_RANGE || header_->spill_range >= static_cast<uint32_t>(args_->numr) ||
            args_->mr_addr[header_->spill_range] == NULL || args_->mr_len[header_->spill_range] < 0)
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
        uint64_t cells_bytes = 0, total_bytes = sizeof(SpillRecordHeader), payload_bytes = 0;
        if (!checked_mul(batch.rows.size(), batch.columns, &cells_bytes) ||
            !checked_mul(cells_bytes, sizeof(CsdSqlResultCell), &cells_bytes) ||
            !checked_add(total_bytes, cells_bytes, &total_bytes)) return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
        for (std::vector<Row>::const_iterator row = batch.rows.begin(); row != batch.rows.end(); ++row)
            for (Row::const_iterator value = row->begin(); value != row->end(); ++value)
                if (!checked_add(payload_bytes, value->bytes.size(), &payload_bytes)) return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
        if (!checked_add(total_bytes, payload_bytes, &total_bytes) ||
            spill_offset_ > static_cast<uint64_t>(args_->mr_len[header_->spill_range]) ||
            total_bytes > static_cast<uint64_t>(args_->mr_len[header_->spill_range]) - spill_offset_)
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
        unsigned char *target = static_cast<unsigned char *>(args_->mr_addr[header_->spill_range]) + spill_offset_;
        SpillRecordHeader record = {};
        record.magic = kSpillMagic;
        record.node_id = index;
        record.run_id = spill_run_id_++;
        record.row_count = batch.rows.size();
        record.column_count = batch.columns;
        record.cell_count = static_cast<uint64_t>(batch.rows.size()) * batch.columns;
        record.bytes_length = payload_bytes;
        std::memcpy(target, &record, sizeof(record));
        CsdSqlResultCell *cells = reinterpret_cast<CsdSqlResultCell *>(target + sizeof(record));
        unsigned char *payload = target + sizeof(record) + cells_bytes;
        uint64_t payload_offset = 0, cell_index = 0;
        for (std::vector<Row>::const_iterator row = batch.rows.begin(); row != batch.rows.end(); ++row)
            for (Row::const_iterator value = row->begin(); value != row->end(); ++value) {
                CsdSqlResultCell cell = {};
                cell.integer = value->integer;
                cell.bytes_offset = payload_offset;
                cell.bytes_length = value->bytes.size();
                cell.datum_type = value->type;
                cell.flags = value->is_null ? kResultNull : 0;
                cell.numeric_scale = value->numeric_scale;
                cells[cell_index++] = cell;
                if (!value->bytes.empty()) {
                    std::memcpy(payload + payload_offset, value->bytes.data(), value->bytes.size());
                    payload_offset += value->bytes.size();
                }
            }
        spill_offset_ += total_bytes;
        { std::lock_guard<std::mutex> status_guard(status_mutex_);
          status_.spill_write_bytes += total_bytes; }
        return true;
    }
