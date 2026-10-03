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
        OperatorMemoryLease memory(this, batch_bytes(batch));
        if (memory.acquired()) return true;
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

    bool read_flash_spill(uint32_t index, uint64_t logical_offset,
                          unsigned char *target, uint64_t length)
    {
        uint64_t extent_bytes = 0;
        if (args_->nvm_read == NULL ||
            !checked_mul(header_->spill_extent_count, sizeof(CsdSqlNvmExtent), &extent_bytes) ||
            !blob_range(header_->spill_extent_offset, extent_bytes) ||
            logical_offset > header_->spill_bytes || length > header_->spill_bytes - logical_offset)
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x207);
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
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x208);
            if (current >= extent_end) continue;
            if (current < extent.logical_offset)
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x209);
            const uint64_t within_extent = current - extent.logical_offset;
            const uint64_t transfer = std::min<uint64_t>(length - copied,
                extent.length - within_extent);
            if (transfer == 0 ||
                args_->nvm_read(args_->service_context, extent.nsid,
                                extent.nvm_offset + within_extent,
                                target + copied, transfer) != 0)
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x20a);
            copied += transfer;
        }
        if (copied != length)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x20b);
        {
            uint64_t total = 0;
            std::lock_guard<std::mutex> guard(status_mutex_);
            if (checked_add(status_.flash_read_bytes, length, &total))
                status_.flash_read_bytes = total;
            else status_.flash_read_bytes = std::numeric_limits<uint64_t>::max();
        }
        return true;
    }

    bool read_spill_bytes(uint32_t index, uint64_t logical_offset,
                          void *target, uint64_t length)
    {
        if (header_->spill_range == CSD_SQL_INVALID_RANGE ||
            header_->spill_range >= static_cast<uint32_t>(args_->numr) ||
            args_->mr_addr[header_->spill_range] == NULL ||
            args_->mr_len[header_->spill_range] <= 0)
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x20c);
        unsigned char *destination = static_cast<unsigned char *>(target);
        if (!(header_->flags & CSD_SQL_PLAN_FLASH_SPILL)) {
            const uint64_t capacity = static_cast<uint64_t>(
                args_->mr_len[header_->spill_range]);
            if (logical_offset > capacity || length > capacity - logical_offset)
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x20d);
            std::memcpy(destination, static_cast<const unsigned char *>(
                args_->mr_addr[header_->spill_range]) + logical_offset, length);
            return true;
        }
        unsigned char *stage = static_cast<unsigned char *>(
            args_->mr_addr[header_->spill_range]);
        const uint64_t capacity = static_cast<uint64_t>(
            args_->mr_len[header_->spill_range]);
        uint64_t copied = 0;
        while (copied < length) {
            const uint64_t transfer = std::min<uint64_t>(capacity, length - copied);
            if (!read_flash_spill(index, logical_offset + copied, stage, transfer))
                return false;
            std::memcpy(destination + copied, stage, transfer);
            copied += transfer;
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

    /* Serialize either a complete Batch or a row-ordinal selection directly.
     * Selection mode avoids constructing a copied partition Batch. */
    bool spill_batch_selection(uint32_t index, const Batch &batch,
                               const std::vector<size_t> *selection,
                               bool append_ordinal, SpillHandle *handle)
    {
        std::lock_guard<std::mutex> spill_guard(spill_mutex_);
        if (header_->spill_range == CSD_SQL_INVALID_RANGE ||
            header_->spill_range >= static_cast<uint32_t>(args_->numr) ||
            args_->mr_addr[header_->spill_range] == NULL ||
            args_->mr_len[header_->spill_range] <= 0)
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
        if (append_ordinal && batch.columns == std::numeric_limits<uint16_t>::max())
            return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x207);
        const size_t row_count = selection == NULL ? batch.rows.size() : selection->size();
        const uint16_t column_count = batch.columns + (append_ordinal ? 1 : 0);
        uint64_t cells_bytes = 0, total_bytes = sizeof(SpillRecordHeader), payload_bytes = 0;
        if (row_count > std::numeric_limits<uint32_t>::max() ||
            !checked_mul(row_count, column_count, &cells_bytes) ||
            !checked_mul(cells_bytes, sizeof(CsdSqlResultCell), &cells_bytes) ||
            !checked_add(total_bytes, cells_bytes, &total_bytes))
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
        for (size_t logical_row = 0; logical_row < row_count; ++logical_row) {
            const size_t source_row = selection == NULL ? logical_row : (*selection)[logical_row];
            if (source_row >= batch.rows.size() ||
                batch.rows[source_row].size() != batch.columns ||
                (append_ordinal &&
                 source_row > static_cast<size_t>(std::numeric_limits<int64_t>::max())))
                return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x208);
            for (Row::const_iterator value = batch.rows[source_row].begin();
                 value != batch.rows[source_row].end(); ++value)
                if (!checked_add(payload_bytes, value->bytes.size(), &payload_bytes))
                    return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);
        }
        if (!checked_add(total_bytes, payload_bytes, &total_bytes))
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index);

        SpillRecordHeader record = {};
        record.magic = kSpillMagic;
        record.node_id = index;
        record.run_id = spill_run_id_++;
        record.row_count = row_count;
        record.column_count = column_count;
        record.cell_count = static_cast<uint64_t>(row_count) * column_count;
        record.bytes_length = payload_bytes;

        const uint64_t record_offset = spill_offset_;
        if (header_->flags & CSD_SQL_PLAN_FLASH_SPILL) {
            if (spill_offset_ > header_->spill_bytes ||
                total_bytes > header_->spill_bytes - spill_offset_)
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x206);
            unsigned char *stage = static_cast<unsigned char *>(
                args_->mr_addr[header_->spill_range]);
            const uint64_t capacity = static_cast<uint64_t>(
                args_->mr_len[header_->spill_range]);
            uint64_t flushed = 0, staged = 0, payload_offset = 0;
            if (!append_spill_bytes(index, stage, capacity, record_offset,
                                    &flushed, &staged, &record, sizeof(record)))
                return false;
            for (size_t logical_row = 0; logical_row < row_count; ++logical_row) {
                const size_t source_row = selection == NULL ? logical_row :
                                          (*selection)[logical_row];
                for (Row::const_iterator value = batch.rows[source_row].begin();
                     value != batch.rows[source_row].end(); ++value) {
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
                if (append_ordinal) {
                    CsdSqlResultCell ordinal = {};
                    ordinal.integer = static_cast<int64_t>(source_row);
                    ordinal.bytes_offset = payload_offset;
                    ordinal.datum_type = CSD_SQL_TYPE_INT64;
                    if (!append_spill_bytes(index, stage, capacity, record_offset,
                                            &flushed, &staged, &ordinal,
                                            sizeof(ordinal)))
                        return false;
                }
            }
            for (size_t logical_row = 0; logical_row < row_count; ++logical_row) {
                const size_t source_row = selection == NULL ? logical_row :
                                          (*selection)[logical_row];
                for (Row::const_iterator value = batch.rows[source_row].begin();
                     value != batch.rows[source_row].end(); ++value)
                    if (!value->bytes.empty() &&
                        !append_spill_bytes(index, stage, capacity, record_offset,
                            &flushed, &staged, value->bytes.data(), value->bytes.size()))
                        return false;
            }
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
            for (size_t logical_row = 0; logical_row < row_count; ++logical_row) {
                const size_t source_row = selection == NULL ? logical_row :
                                          (*selection)[logical_row];
                for (Row::const_iterator value = batch.rows[source_row].begin();
                     value != batch.rows[source_row].end(); ++value) {
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
                if (append_ordinal) {
                    CsdSqlResultCell ordinal = {};
                    ordinal.integer = static_cast<int64_t>(source_row);
                    ordinal.bytes_offset = payload_offset;
                    ordinal.datum_type = CSD_SQL_TYPE_INT64;
                    cells[cell_index++] = ordinal;
                }
            }
        }
        spill_offset_ += total_bytes;
        if (handle != NULL) {
            handle->offset = record_offset;
            handle->length = total_bytes;
            handle->row_count = record.row_count;
            handle->column_count = record.column_count;
        }
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            status_.spill_write_bytes += total_bytes;
            ++status_.operator_spill_count;
        }
        return true;
    }

    /* Flash allocation is serialized: all CSD lanes share one append cursor. */
    bool spill_batch(uint32_t index, const Batch &batch, SpillHandle *handle = NULL)
    {
        return spill_batch_selection(index, batch, NULL, false, handle);
    }

    bool spill_selected_rows(uint32_t index, const Batch &batch,
                             const std::vector<size_t> &selection,
                             bool append_ordinal, SpillHandle *handle)
    {
        return spill_batch_selection(index, batch, &selection,
                                     append_ordinal, handle);
    }

    bool read_spill_batch(uint32_t index, const SpillHandle &handle, Batch *output)
    {
        std::lock_guard<std::mutex> spill_guard(spill_mutex_);
        if (handle.length < sizeof(SpillRecordHeader) ||
            handle.offset > spill_offset_ || handle.length > spill_offset_ - handle.offset)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x20e);
        SpillRecordHeader record = {};
        if (!read_spill_bytes(index, handle.offset, &record, sizeof(record)))
            return false;
        uint64_t expected_cells = 0, cells_bytes = 0, expected_length = sizeof(record);
        if (record.magic != kSpillMagic || record.node_id != index ||
            record.row_count != handle.row_count ||
            record.column_count != handle.column_count ||
            !checked_mul(record.row_count, record.column_count, &expected_cells) ||
            record.cell_count != expected_cells ||
            !checked_mul(record.cell_count, sizeof(CsdSqlResultCell), &cells_bytes) ||
            !checked_add(expected_length, cells_bytes, &expected_length) ||
            !checked_add(expected_length, record.bytes_length, &expected_length) ||
            expected_length != handle.length)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x20f);

        Batch restored;
        restored.columns = record.column_count;
        restored.rows.resize(record.row_count);
        for (size_t row = 0; row < restored.rows.size(); ++row)
            restored.rows[row].resize(record.column_count);

        const uint64_t stage_capacity = static_cast<uint64_t>(
            args_->mr_len[header_->spill_range]);
        const uint64_t cells_per_chunk = std::max<uint64_t>(
            1, stage_capacity / sizeof(CsdSqlResultCell));
        std::vector<CsdSqlResultCell> cells(static_cast<size_t>(
            std::min<uint64_t>(record.cell_count, cells_per_chunk)));
        uint64_t cell_index = 0, expected_payload_offset = 0;
        while (cell_index < record.cell_count) {
            const uint64_t count = std::min<uint64_t>(cells.size(),
                record.cell_count - cell_index);
            if (!read_spill_bytes(index,
                    handle.offset + sizeof(record) + cell_index * sizeof(CsdSqlResultCell),
                    cells.data(), count * sizeof(CsdSqlResultCell)))
                return false;
            for (uint64_t slot = 0; slot < count; ++slot) {
                const CsdSqlResultCell &cell = cells[slot];
                uint64_t payload_end = 0;
                if (!is_supported_type(cell.datum_type) ||
                    cell.bytes_offset != expected_payload_offset ||
                    !checked_add(cell.bytes_offset, cell.bytes_length, &payload_end) ||
                    payload_end > record.bytes_length)
                    return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x210);
                const uint64_t absolute = cell_index + slot;
                Value &value = restored.rows[absolute / record.column_count]
                    [absolute % record.column_count];
                value.is_null = (cell.flags & kResultNull) != 0;
                value.type = cell.datum_type;
                value.integer = cell.integer;
                value.numeric_scale = cell.numeric_scale;
                value.bytes.resize(cell.bytes_length);
                expected_payload_offset = payload_end;
            }
            cell_index += count;
        }
        if (expected_payload_offset != record.bytes_length)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x211);

        uint64_t payload_offset = 0;
        const uint64_t payload_start = handle.offset + sizeof(record) + cells_bytes;
        if (header_->flags & CSD_SQL_PLAN_FLASH_SPILL) {
            unsigned char *stage = static_cast<unsigned char *>(
                args_->mr_addr[header_->spill_range]);
            size_t payload_row = 0, payload_column = 0;
            uint64_t value_offset = 0;
            while (payload_offset < record.bytes_length) {
                const uint64_t transfer = std::min<uint64_t>(stage_capacity,
                    record.bytes_length - payload_offset);
                if (!read_flash_spill(index, payload_start + payload_offset,
                                      stage, transfer))
                    return false;
                uint64_t chunk_offset = 0;
                while (chunk_offset < transfer) {
                    while (payload_row < restored.rows.size() &&
                           value_offset == restored.rows[payload_row][payload_column].bytes.size()) {
                        value_offset = 0;
                        if (++payload_column == restored.columns) {
                            payload_column = 0;
                            ++payload_row;
                        }
                    }
                    if (payload_row == restored.rows.size())
                        return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x212);
                    Value &value = restored.rows[payload_row][payload_column];
                    const uint64_t copy = std::min<uint64_t>(
                        transfer - chunk_offset, value.bytes.size() - value_offset);
                    std::memcpy(&value.bytes[static_cast<size_t>(value_offset)],
                                stage + chunk_offset, copy);
                    value_offset += copy;
                    chunk_offset += copy;
                }
                payload_offset += transfer;
            }
        } else {
            const unsigned char *payload = static_cast<const unsigned char *>(
                args_->mr_addr[header_->spill_range]) + payload_start;
            for (size_t row = 0; row < restored.rows.size(); ++row)
                for (size_t column = 0; column < restored.rows[row].size(); ++column) {
                    Value &value = restored.rows[row][column];
                    if (!value.bytes.empty()) {
                        std::memcpy(&value.bytes[0], payload + payload_offset,
                                    value.bytes.size());
                        payload_offset += value.bytes.size();
                    }
                }
        }
        {
            uint64_t total = 0;
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            if (checked_add(status_.spill_read_bytes, handle.length, &total))
                status_.spill_read_bytes = total;
            else status_.spill_read_bytes = std::numeric_limits<uint64_t>::max();
        }
        *output = std::move(restored);
        return true;
    }
