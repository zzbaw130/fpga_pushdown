    bool execute_materialize(uint32_t index, const CsdSqlPlanNode &node, Batch &input,
                             Batch *output, bool consume_input)
    {
        const size_t input_rows = input.rows.size();
        if (!spill_if_needed(index, node, input)) return false;
        if (consume_input) {
            output->columns = input.columns;
            output->rows.swap(input.rows);
        } else *output = input;
        add_cycles(input_rows, node);
        return true;
    }

    bool execute_limit(uint32_t index, const CsdSqlPlanNode &node, Batch &input,
                       Batch *output, bool consume_input)
    {
        if (consume_input) {
            output->columns = input.columns;
            output->rows.swap(input.rows);
        } else *output = input;
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

    bool write_flash_spill(uint32_t index, uint64_t logical_offset,
                           const unsigned char *source, uint64_t length)
    {
        uint64_t extent_bytes = 0;
        if (args_->nvm_write == NULL ||
            !checked_mul(header_->spill_extent_count, sizeof(CsdSqlNvmExtent), &extent_bytes) ||
            !blob_range(header_->spill_extent_offset, extent_bytes) ||
            logical_offset > header_->spill_bytes || length > header_->spill_bytes - logical_offset)
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x201);
        const CsdSqlNvmExtent *extents = reinterpret_cast<const CsdSqlNvmExtent *>(
            static_cast<const char *>(plan_data_) + header_->spill_extent_offset);
        uint64_t copied = 0;
        for (uint32_t extent_index = 0;
             extent_index < header_->spill_extent_count && copied < length;
             ++extent_index) {
            const CsdSqlNvmExtent &extent = extents[extent_index];
            uint64_t extent_end = 0;
            const uint64_t current = logical_offset + copied;
            if (!checked_add(extent.logical_offset, extent.length, &extent_end))
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x202);
            if (current >= extent_end) continue;
            if (current < extent.logical_offset)
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x203);
            const uint64_t within_extent = current - extent.logical_offset;
            const uint64_t transfer = std::min<uint64_t>(length - copied,
                extent.length - within_extent);
            if (transfer == 0 ||
                args_->nvm_write(args_->service_context, extent.nsid,
                                 extent.nvm_offset + within_extent,
                                 const_cast<unsigned char *>(source + copied), transfer) != 0)
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x204);
            copied += transfer;
        }
        if (copied != length)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x205);
        {
            uint64_t total = 0;
            std::lock_guard<std::mutex> guard(status_mutex_);
            if (checked_add(status_.flash_write_bytes, length, &total))
                status_.flash_write_bytes = total;
            else status_.flash_write_bytes = std::numeric_limits<uint64_t>::max();
        }
        return true;
    }

    bool append_spill_bytes(uint32_t index, unsigned char *stage, uint64_t capacity,
                            uint64_t record_offset, uint64_t *flushed,
                            uint64_t *staged, const void *source, uint64_t length)
    {
        const unsigned char *bytes = static_cast<const unsigned char *>(source);
        while (length > 0) {
            const uint64_t copy = std::min<uint64_t>(length, capacity - *staged);
            std::memcpy(stage + *staged, bytes, copy);
            *staged += copy;
            bytes += copy;
            length -= copy;
            if (*staged == capacity) {
                if (!write_flash_spill(index, record_offset + *flushed, stage, *staged))
                    return false;
                *flushed += *staged;
                *staged = 0;
            }
        }
        return true;
    }

    /* Flash allocation is serialized: all CSD lanes share one append cursor. */
    bool spill_batch(uint32_t index, const Batch &batch)
    {
        std::lock_guard<std::mutex> spill_guard(spill_mutex_);
        if (header_->spill_range == CSD_SQL_INVALID_RANGE ||
            header_->spill_range >= static_cast<uint32_t>(args_->numr) ||
            args_->mr_addr[header_->spill_range] == NULL ||
            args_->mr_len[header_->spill_range] <= 0)
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
        uint64_t cells_bytes = 0, total_bytes = sizeof(SpillRecordHeader), payload_bytes = 0;
        if (batch.rows.size() > std::numeric_limits<uint32_t>::max() ||
            !checked_mul(batch.rows.size(), batch.columns, &cells_bytes) ||
            !checked_mul(cells_bytes, sizeof(CsdSqlResultCell), &cells_bytes) ||
            !checked_add(total_bytes, cells_bytes, &total_bytes))
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
        for (std::vector<Row>::const_iterator row = batch.rows.begin(); row != batch.rows.end(); ++row)
            for (Row::const_iterator value = row->begin(); value != row->end(); ++value)
                if (!checked_add(payload_bytes, value->bytes.size(), &payload_bytes))
                    return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
        if (!checked_add(total_bytes, payload_bytes, &total_bytes))
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);

        SpillRecordHeader record = {};
        record.magic = kSpillMagic;
        record.node_id = index;
        record.run_id = spill_run_id_++;
        record.row_count = batch.rows.size();
        record.column_count = batch.columns;
        record.cell_count = static_cast<uint64_t>(batch.rows.size()) * batch.columns;
        record.bytes_length = payload_bytes;

        if (header_->flags & CSD_SQL_PLAN_FLASH_SPILL) {
            if (spill_offset_ > header_->spill_bytes ||
                total_bytes > header_->spill_bytes - spill_offset_)
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x206);
            unsigned char *stage = static_cast<unsigned char *>(
                args_->mr_addr[header_->spill_range]);
            const uint64_t capacity = static_cast<uint64_t>(
                args_->mr_len[header_->spill_range]);
            const uint64_t record_offset = spill_offset_;
            uint64_t flushed = 0, staged = 0, payload_offset = 0;
            if (!append_spill_bytes(index, stage, capacity, record_offset,
                                    &flushed, &staged, &record, sizeof(record)))
                return false;
            for (std::vector<Row>::const_iterator row = batch.rows.begin();
                 row != batch.rows.end(); ++row)
                for (Row::const_iterator value = row->begin(); value != row->end(); ++value) {
                    CsdSqlResultCell cell = {};
                    cell.integer = value->integer;
                    cell.bytes_offset = payload_offset;
                    cell.bytes_length = value->bytes.size();
                    cell.datum_type = value->type;
                    cell.flags = value->is_null ? kResultNull : 0;
                    cell.numeric_scale = value->numeric_scale;
                    if (!append_spill_bytes(index, stage, capacity, record_offset,
                                            &flushed, &staged, &cell, sizeof(cell)))
                        return false;
                    payload_offset += value->bytes.size();
                }
            for (std::vector<Row>::const_iterator row = batch.rows.begin();
                 row != batch.rows.end(); ++row)
                for (Row::const_iterator value = row->begin(); value != row->end(); ++value)
                    if (!value->bytes.empty() &&
                        !append_spill_bytes(index, stage, capacity, record_offset,
                            &flushed, &staged, value->bytes.data(), value->bytes.size()))
                        return false;
            if (staged != 0 &&
                !write_flash_spill(index, record_offset + flushed, stage, staged))
                return false;
        } else {
            /* ABI test/compatibility path: the complete run resides in FDM. */
            if (spill_offset_ > static_cast<uint64_t>(args_->mr_len[header_->spill_range]) ||
                total_bytes > static_cast<uint64_t>(args_->mr_len[header_->spill_range]) - spill_offset_)
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
            unsigned char *target = static_cast<unsigned char *>(
                args_->mr_addr[header_->spill_range]) + spill_offset_;
            std::memcpy(target, &record, sizeof(record));
            CsdSqlResultCell *cells = reinterpret_cast<CsdSqlResultCell *>(
                target + sizeof(record));
            unsigned char *payload = target + sizeof(record) + cells_bytes;
            uint64_t payload_offset = 0, cell_index = 0;
            for (std::vector<Row>::const_iterator row = batch.rows.begin();
                 row != batch.rows.end(); ++row)
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
                        std::memcpy(payload + payload_offset, value->bytes.data(),
                                    value->bytes.size());
                        payload_offset += value->bytes.size();
                    }
                }
        }
        spill_offset_ += total_bytes;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            status_.spill_write_bytes += total_bytes;
        }
        return true;
    }
