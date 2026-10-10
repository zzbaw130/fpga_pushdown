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
     * Selection mode avoids constructing a copied partition Batch. An ordinal
     * base lets independently consumed FIFO batches share one global probe
     * order during partition replay. */
    bool spill_batch_selection(uint32_t index, const Batch &batch,
                               const std::vector<size_t> *selection,
                               bool append_ordinal, uint64_t ordinal_base,
                               SpillHandle *handle)
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
            uint64_t ordinal = 0;
            if (source_row >= batch.rows.size() ||
                batch.rows[source_row].size() != batch.columns ||
                (append_ordinal &&
                 (!checked_add(ordinal_base, source_row, &ordinal) ||
                  ordinal > static_cast<uint64_t>(
                      std::numeric_limits<int64_t>::max()))))
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
                    uint64_t ordinal_value = 0;
                    if (!checked_add(ordinal_base, source_row,
                                     &ordinal_value) ||
                        ordinal_value > static_cast<uint64_t>(
                            std::numeric_limits<int64_t>::max()))
                        return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x208);
                    CsdSqlResultCell ordinal = {};
                    ordinal.integer = static_cast<int64_t>(ordinal_value);
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
                    uint64_t ordinal_value = 0;
                    if (!checked_add(ordinal_base, source_row,
                                     &ordinal_value) ||
                        ordinal_value > static_cast<uint64_t>(
                            std::numeric_limits<int64_t>::max()))
                        return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x208);
                    CsdSqlResultCell ordinal = {};
                    ordinal.integer = static_cast<int64_t>(ordinal_value);
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
        return spill_batch_selection(index, batch, NULL, false, 0, handle);
    }

    bool spill_selected_rows(uint32_t index, const Batch &batch,
                             const std::vector<size_t> &selection,
                             bool append_ordinal, SpillHandle *handle,
                             uint64_t ordinal_base = 0)
    {
        return spill_batch_selection(index, batch, &selection,
                                     append_ordinal, ordinal_base, handle);
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

    bool spill_cursor_read(uint32_t index, SpillRowCursor *cursor,
                           uint64_t logical_offset, void *target,
                           uint64_t length)
    {
        std::lock_guard<std::mutex> spill_guard(spill_mutex_);
        if (!read_spill_bytes(index, logical_offset, target, length))
            return false;
        uint64_t total = 0;
        cursor->bytes_read = checked_add(cursor->bytes_read, length, &total) ?
            total : std::numeric_limits<uint64_t>::max();
        return true;
    }

    bool init_spill_cursor(uint32_t index, const SpillHandle &handle,
                           SpillRowCursor *cursor)
    {
        cursor->handle = handle;
        cursor->initialized = true;
        {
            std::lock_guard<std::mutex> spill_guard(spill_mutex_);
            if (handle.length < sizeof(SpillRecordHeader) ||
                handle.offset > spill_offset_ ||
                handle.length > spill_offset_ - handle.offset)
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x220);
            if (!read_spill_bytes(index, handle.offset,
                                  &cursor->record, sizeof(cursor->record)))
                return false;
            cursor->bytes_read = sizeof(cursor->record);
        }
        uint64_t expected_cells = 0, expected_length = sizeof(cursor->record);
        if (cursor->record.magic != kSpillMagic ||
            cursor->record.node_id != index ||
            cursor->record.row_count != handle.row_count ||
            cursor->record.column_count != handle.column_count ||
            (cursor->record.row_count != 0 &&
             cursor->record.column_count == 0) ||
            !checked_mul(cursor->record.row_count,
                         cursor->record.column_count, &expected_cells) ||
            cursor->record.cell_count != expected_cells ||
            !checked_mul(expected_cells, sizeof(CsdSqlResultCell),
                         &cursor->cells_bytes) ||
            !checked_add(expected_length, cursor->cells_bytes,
                         &expected_length) ||
            !checked_add(expected_length, cursor->record.bytes_length,
                         &expected_length) ||
            expected_length != handle.length ||
            !checked_mul(cursor->record.column_count,
                         sizeof(CsdSqlResultCell),
                         &cursor->row_cells_bytes))
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x221);
        const uint64_t stage_capacity = static_cast<uint64_t>(
            args_->mr_len[header_->spill_range]);
        if (cursor->record.row_count != 0 &&
            (cursor->row_cells_bytes == 0 ||
             cursor->row_cells_bytes > stage_capacity))
            return fail(CSD_SQL_STATUS_FIFO_TOO_SMALL, index, 0x322);
        cursor->payload_start = handle.offset + sizeof(cursor->record) +
                                cursor->cells_bytes;
        const uint64_t quarter_stage = std::max<uint64_t>(
            1, stage_capacity / 4);
        cursor->chunk_capacity = std::min<uint64_t>(stage_capacity,
            std::max<uint64_t>(cursor->row_cells_bytes, quarter_stage));
        cursor->chunk.columns = cursor->record.column_count;
        return true;
    }

    bool load_spill_cursor_descriptors(uint32_t index,
                                       SpillRowCursor *cursor)
    {
        if (cursor->descriptor_position < cursor->descriptor_rows)
            return true;
        cursor->descriptors.clear();
        cursor->descriptor_rows = 0;
        cursor->descriptor_position = 0;
        if (cursor->next_row >= cursor->record.row_count) return true;
        const uint64_t remaining = cursor->record.row_count - cursor->next_row;
        const uint64_t rows = std::min<uint64_t>(remaining,
            std::max<uint64_t>(1,
                cursor->chunk_capacity / cursor->row_cells_bytes));
        uint64_t cell_count = 0, bytes = 0, row_offset = 0;
        if (!checked_mul(rows, cursor->record.column_count, &cell_count) ||
            cell_count > std::numeric_limits<size_t>::max() ||
            !checked_mul(cell_count, sizeof(CsdSqlResultCell), &bytes) ||
            !checked_mul(cursor->next_row, cursor->row_cells_bytes,
                         &row_offset))
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x223);
        cursor->descriptors.resize(static_cast<size_t>(cell_count));
        if (!spill_cursor_read(index, cursor,
                cursor->handle.offset + sizeof(SpillRecordHeader) + row_offset,
                cursor->descriptors.data(), bytes))
            return false;
        for (size_t cell_index = 0;
             cell_index < cursor->descriptors.size(); ++cell_index) {
            const CsdSqlResultCell &cell = cursor->descriptors[cell_index];
            uint64_t payload_end = 0;
            if (!is_supported_type(cell.datum_type) ||
                cell.bytes_offset != cursor->validated_payload_offset ||
                !checked_add(cell.bytes_offset, cell.bytes_length,
                             &payload_end) ||
                payload_end > cursor->record.bytes_length)
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x224);
            cursor->validated_payload_offset = payload_end;
        }
        cursor->descriptor_rows = rows;
        return true;
    }

    bool load_spill_cursor_chunk(uint32_t index, SpillRowCursor *cursor)
    {
        if (!load_spill_cursor_descriptors(index, cursor)) return false;
        if (cursor->next_row >= cursor->record.row_count) return true;
        const uint64_t available = cursor->descriptor_rows -
                                   cursor->descriptor_position;
        const size_t first_cell = static_cast<size_t>(
            cursor->descriptor_position * cursor->record.column_count);
        const uint64_t payload_begin =
            cursor->descriptors[first_cell].bytes_offset;
        uint64_t chunk_rows = 0, payload_end = payload_begin;
        while (chunk_rows < available) {
            const size_t last_cell = static_cast<size_t>(
                (cursor->descriptor_position + chunk_rows + 1) *
                    cursor->record.column_count - 1);
            uint64_t candidate_end = 0;
            if (!checked_add(cursor->descriptors[last_cell].bytes_offset,
                             cursor->descriptors[last_cell].bytes_length,
                             &candidate_end))
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x225);
            const uint64_t candidate_bytes = candidate_end - payload_begin;
            if (chunk_rows != 0 && candidate_bytes > cursor->chunk_capacity)
                break;
            if (chunk_rows == 0 && candidate_bytes >
                static_cast<uint64_t>(args_->mr_len[header_->spill_range]))
                return fail(CSD_SQL_STATUS_FIFO_TOO_SMALL, index, 0x326);
            payload_end = candidate_end;
            ++chunk_rows;
        }
        if (chunk_rows == 0)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x227);
        const uint64_t payload_bytes = payload_end - payload_begin;
        if (payload_bytes > std::numeric_limits<size_t>::max())
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x228);
        std::vector<unsigned char> payload(
            static_cast<size_t>(payload_bytes));
        if (payload_bytes != 0 &&
            !spill_cursor_read(index, cursor,
                cursor->payload_start + payload_begin,
                payload.data(), payload_bytes))
            return false;

        cursor->chunk.rows.clear();
        cursor->chunk.rows.resize(static_cast<size_t>(chunk_rows));
        for (size_t row = 0; row < cursor->chunk.rows.size(); ++row) {
            cursor->chunk.rows[row].resize(cursor->record.column_count);
            for (uint16_t column = 0;
                 column < cursor->record.column_count; ++column) {
                const CsdSqlResultCell &cell = cursor->descriptors[
                    static_cast<size_t>((cursor->descriptor_position + row) *
                        cursor->record.column_count + column)];
                Value &value = cursor->chunk.rows[row][column];
                value.is_null = (cell.flags & kResultNull) != 0;
                value.type = cell.datum_type;
                value.integer = cell.integer;
                value.numeric_scale = cell.numeric_scale;
                if (cell.bytes_length != 0) {
                    const uint64_t within = cell.bytes_offset - payload_begin;
                    if (within > payload_bytes ||
                        cell.bytes_length > payload_bytes - within)
                        return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x229);
                    value.bytes.assign(reinterpret_cast<const char *>(
                        payload.data() + within), cell.bytes_length);
                }
            }
        }
        uint64_t peak = cursor->descriptors.size() *
                        sizeof(CsdSqlResultCell);
        if (!checked_add(peak, payload_bytes, &peak) ||
            !checked_add(peak, batch_bytes(cursor->chunk), &peak))
            peak = std::numeric_limits<uint64_t>::max();
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            status_.spill_cursor_peak_bytes = std::max<uint64_t>(
                status_.spill_cursor_peak_bytes, peak);
        }
        cursor->descriptor_position += chunk_rows;
        cursor->next_row += chunk_rows;
        cursor->chunk_position = 0;
        if (cursor->descriptor_position == cursor->descriptor_rows) {
            std::vector<CsdSqlResultCell>().swap(cursor->descriptors);
            cursor->descriptor_rows = 0;
            cursor->descriptor_position = 0;
        }
        if (cursor->next_row == cursor->record.row_count &&
            cursor->validated_payload_offset != cursor->record.bytes_length)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x22a);
        return true;
    }

    bool ensure_spill_cursor_row(uint32_t index, SpillRowCursor *cursor,
                                 bool *has_row)
    {
        if (cursor->chunk_position < cursor->chunk.rows.size()) {
            *has_row = true;
            return true;
        }
        std::vector<Row>().swap(cursor->chunk.rows);
        cursor->chunk_position = 0;
        if (cursor->next_row >= cursor->record.row_count) {
            *has_row = false;
            return true;
        }
        if (!load_spill_cursor_chunk(index, cursor)) return false;
        *has_row = !cursor->chunk.rows.empty();
        if (!*has_row && cursor->next_row != cursor->record.row_count)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x23a);
        return true;
    }

    const Row &spill_cursor_row(const SpillRowCursor &cursor) const
    {
        return cursor.chunk.rows[cursor.chunk_position];
    }

    void advance_spill_cursor(SpillRowCursor *cursor)
    {
        ++cursor->chunk_position;
    }

    void finish_spill_cursor(SpillRowCursor *cursor)
    {
        if (!cursor->initialized || cursor->accounted) return;
        {
            uint64_t total = 0;
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            if (checked_add(status_.spill_read_bytes, cursor->bytes_read,
                            &total))
                status_.spill_read_bytes = total;
            else
                status_.spill_read_bytes =
                    std::numeric_limits<uint64_t>::max();
        }
        cursor->accounted = true;
        std::vector<CsdSqlResultCell>().swap(cursor->descriptors);
        std::vector<Row>().swap(cursor->chunk.rows);
    }

    bool write_spill_storage_unlocked(uint32_t index, uint64_t logical_offset,
                                      const void *source, uint64_t length)
    {
        if (header_->spill_range == CSD_SQL_INVALID_RANGE ||
            header_->spill_range >= static_cast<uint32_t>(args_->numr) ||
            args_->mr_addr[header_->spill_range] == NULL ||
            args_->mr_len[header_->spill_range] <= 0)
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x22b);
        const unsigned char *bytes = static_cast<const unsigned char *>(source);
        if (!(header_->flags & CSD_SQL_PLAN_FLASH_SPILL)) {
            const uint64_t capacity = static_cast<uint64_t>(
                args_->mr_len[header_->spill_range]);
            if (logical_offset > capacity || length > capacity - logical_offset)
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x22c);
            std::memcpy(static_cast<unsigned char *>(
                args_->mr_addr[header_->spill_range]) + logical_offset,
                bytes, length);
            return true;
        }
        unsigned char *stage = static_cast<unsigned char *>(
            args_->mr_addr[header_->spill_range]);
        const uint64_t capacity = static_cast<uint64_t>(
            args_->mr_len[header_->spill_range]);
        uint64_t written = 0;
        while (written < length) {
            const uint64_t transfer = std::min<uint64_t>(
                capacity, length - written);
            std::memcpy(stage, bytes + written, transfer);
            if (!write_flash_spill(index, logical_offset + written,
                                   stage, transfer))
                return false;
            written += transfer;
        }
        return true;
    }

    bool init_spill_writer(uint32_t index, uint16_t columns,
                           uint64_t row_count, uint64_t max_payload_bytes,
                           SpillRowWriter *writer)
    {
        if (columns == 0 || row_count > std::numeric_limits<uint32_t>::max() ||
            max_payload_bytes > std::numeric_limits<uint32_t>::max())
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x22d);
        uint64_t cell_count = 0, reserved = sizeof(SpillRecordHeader);
        if (!checked_mul(row_count, columns, &cell_count) ||
            !checked_mul(cell_count, sizeof(CsdSqlResultCell),
                         &writer->cells_bytes) ||
            !checked_add(reserved, writer->cells_bytes, &reserved) ||
            !checked_add(reserved, max_payload_bytes, &reserved))
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x22e);
        if (header_->spill_range == CSD_SQL_INVALID_RANGE ||
            header_->spill_range >= static_cast<uint32_t>(args_->numr) ||
            args_->mr_addr[header_->spill_range] == NULL ||
            args_->mr_len[header_->spill_range] <= 0)
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x22f);
        const uint64_t storage_capacity =
            (header_->flags & CSD_SQL_PLAN_FLASH_SPILL) ?
                header_->spill_bytes : static_cast<uint64_t>(
                    args_->mr_len[header_->spill_range]);
        {
            std::lock_guard<std::mutex> spill_guard(spill_mutex_);
            if (spill_offset_ > storage_capacity ||
                reserved > storage_capacity - spill_offset_)
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x230);
            writer->offset = spill_offset_;
            spill_offset_ += reserved;
            writer->run_id = spill_run_id_++;
        }
        writer->node_id = index;
        writer->row_count = static_cast<uint32_t>(row_count);
        writer->columns = columns;
        writer->reserved_length = reserved;
        writer->chunk_capacity = static_cast<uint64_t>(
            args_->mr_len[header_->spill_range]);
        writer->initialized = true;
        return true;
    }

    bool flush_spill_writer(uint32_t index, SpillRowWriter *writer)
    {
        if (writer->cells.empty()) return true;
        const uint64_t buffered_rows = writer->cells.size() / writer->columns;
        const uint64_t first_row = writer->written_rows - buffered_rows;
        const uint64_t buffered_payload_start = writer->payload_bytes -
                                                writer->payload.size();
        uint64_t descriptor_offset = 0, row_bytes = 0, payload_offset = 0;
        if (!checked_mul(first_row, writer->columns, &descriptor_offset) ||
            !checked_mul(descriptor_offset, sizeof(CsdSqlResultCell),
                         &descriptor_offset) ||
            !checked_add(writer->offset + sizeof(SpillRecordHeader),
                         descriptor_offset, &descriptor_offset) ||
            !checked_mul(writer->cells.size(), sizeof(CsdSqlResultCell),
                         &row_bytes) ||
            !checked_add(writer->offset + sizeof(SpillRecordHeader),
                         writer->cells_bytes, &payload_offset) ||
            !checked_add(payload_offset, buffered_payload_start,
                         &payload_offset))
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x231);
        std::lock_guard<std::mutex> spill_guard(spill_mutex_);
        if (!write_spill_storage_unlocked(index, descriptor_offset,
                                          writer->cells.data(), row_bytes) ||
            (!writer->payload.empty() &&
             !write_spill_storage_unlocked(index, payload_offset,
                                           writer->payload.data(),
                                           writer->payload.size())))
            return false;
        writer->cells.clear();
        writer->payload.clear();
        return true;
    }

    bool append_spill_writer_row(uint32_t index, SpillRowWriter *writer,
                                 const Row &row)
    {
        if (!writer->initialized || row.size() != writer->columns ||
            writer->written_rows >= writer->row_count)
            return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x232);
        uint64_t row_payload = 0, row_cells_bytes = 0, row_bytes = 0;
        if (!checked_mul(writer->columns, sizeof(CsdSqlResultCell),
                         &row_cells_bytes))
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x233);
        for (Row::const_iterator value = row.begin(); value != row.end(); ++value)
            if (value->bytes.size() > std::numeric_limits<uint16_t>::max() ||
                !checked_add(row_payload, value->bytes.size(), &row_payload))
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x234);
        if (!checked_add(row_cells_bytes, row_payload, &row_bytes) ||
            row_bytes > writer->chunk_capacity)
            return fail(CSD_SQL_STATUS_FIFO_TOO_SMALL, index, 0x335);
        uint64_t buffered_bytes = 0;
        if (!checked_mul(writer->cells.size(), sizeof(CsdSqlResultCell),
                         &buffered_bytes) ||
            !checked_add(buffered_bytes, writer->payload.size(),
                         &buffered_bytes))
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x236);
        if (!writer->cells.empty() &&
            row_bytes > writer->chunk_capacity - buffered_bytes &&
            !flush_spill_writer(index, writer))
            return false;
        uint64_t payload_offset = writer->payload_bytes;
        for (Row::const_iterator value = row.begin(); value != row.end(); ++value) {
            if (payload_offset > std::numeric_limits<uint32_t>::max())
                return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x237);
            CsdSqlResultCell cell = {};
            cell.integer = value->integer;
            cell.bytes_offset = static_cast<uint32_t>(payload_offset);
            cell.bytes_length = value->bytes.size();
            cell.datum_type = value->type;
            cell.flags = value->is_null ? kResultNull : 0;
            cell.numeric_scale = value->numeric_scale;
            writer->cells.push_back(cell);
            writer->payload.insert(writer->payload.end(),
                value->bytes.begin(), value->bytes.end());
            payload_offset += value->bytes.size();
        }
        writer->payload_bytes = payload_offset;
        ++writer->written_rows;
        uint64_t peak = writer->cells.size() * sizeof(CsdSqlResultCell);
        if (!checked_add(peak, writer->payload.size(), &peak))
            peak = std::numeric_limits<uint64_t>::max();
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            status_.spill_writer_peak_bytes = std::max<uint64_t>(
                status_.spill_writer_peak_bytes, peak);
        }
        return true;
    }

    bool finish_spill_writer(uint32_t index, SpillRowWriter *writer,
                             SpillHandle *handle)
    {
        if (!writer->initialized || writer->written_rows != writer->row_count)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x23b);
        if (!flush_spill_writer(index, writer))
            return false;
        SpillRecordHeader record = {};
        record.magic = kSpillMagic;
        record.node_id = writer->node_id;
        record.run_id = writer->run_id;
        record.row_count = writer->row_count;
        record.column_count = writer->columns;
        record.cell_count = static_cast<uint64_t>(writer->row_count) *
                            writer->columns;
        record.bytes_length = writer->payload_bytes;
        uint64_t actual_length = sizeof(record);
        if (!checked_add(actual_length, writer->cells_bytes, &actual_length) ||
            !checked_add(actual_length, writer->payload_bytes, &actual_length))
            return fail(CSD_SQL_STATUS_SPILL_TOO_SMALL, index, 0x238);
        {
            std::lock_guard<std::mutex> spill_guard(spill_mutex_);
            if (!write_spill_storage_unlocked(index, writer->offset,
                                              &record, sizeof(record)))
                return false;
        }
        handle->offset = writer->offset;
        handle->length = actual_length;
        handle->row_count = writer->row_count;
        handle->column_count = writer->columns;
        {
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            status_.spill_write_bytes += actual_length;
            ++status_.operator_spill_count;
        }
        return true;
    }

    /* Decode a spill record in bounded row chunks. Cell descriptors and byte
     * payload retain the existing on-Flash layout; the consumer never sees a
     * partially decoded row. */
    template <typename Consumer>
    bool stream_spill_batch(uint32_t index, const SpillHandle &handle,
                            Consumer consumer)
    {
        std::lock_guard<std::mutex> spill_guard(spill_mutex_);
        if (handle.length < sizeof(SpillRecordHeader) ||
            handle.offset > spill_offset_ ||
            handle.length > spill_offset_ - handle.offset)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x214);
        SpillRecordHeader record = {};
        if (!read_spill_bytes(index, handle.offset, &record, sizeof(record)))
            return false;
        uint64_t expected_cells = 0, cells_bytes = 0;
        uint64_t expected_length = sizeof(record);
        if (record.magic != kSpillMagic || record.node_id != index ||
            record.row_count != handle.row_count ||
            record.column_count != handle.column_count ||
            (record.row_count != 0 && record.column_count == 0) ||
            !checked_mul(record.row_count, record.column_count, &expected_cells) ||
            record.cell_count != expected_cells ||
            !checked_mul(record.cell_count, sizeof(CsdSqlResultCell), &cells_bytes) ||
            !checked_add(expected_length, cells_bytes, &expected_length) ||
            !checked_add(expected_length, record.bytes_length, &expected_length) ||
            expected_length != handle.length)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x215);

        const uint64_t stage_capacity = static_cast<uint64_t>(
            args_->mr_len[header_->spill_range]);
        uint64_t row_cells_bytes = 0;
        if (!checked_mul(record.column_count, sizeof(CsdSqlResultCell),
                         &row_cells_bytes) ||
            (record.row_count != 0 && row_cells_bytes > stage_capacity))
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x216);
        const uint64_t rows_per_descriptor_block = record.row_count == 0 ? 1 :
            std::max<uint64_t>(1, stage_capacity / row_cells_bytes);
        const uint64_t payload_start = handle.offset + sizeof(record) + cells_bytes;
        uint64_t first_row = 0, expected_payload_offset = 0;
        while (first_row < record.row_count) {
            const uint64_t block_rows = std::min<uint64_t>(
                rows_per_descriptor_block, record.row_count - first_row);
            uint64_t block_cells = 0;
            if (!checked_mul(block_rows, record.column_count, &block_cells) ||
                block_cells > std::numeric_limits<size_t>::max())
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x217);
            std::vector<CsdSqlResultCell> cells(
                static_cast<size_t>(block_cells));
            if (!read_spill_bytes(index,
                    handle.offset + sizeof(record) +
                        first_row * row_cells_bytes,
                    cells.data(), block_cells * sizeof(CsdSqlResultCell)))
                return false;
            for (size_t cell_index = 0; cell_index < cells.size(); ++cell_index) {
                uint64_t payload_end = 0;
                if (!is_supported_type(cells[cell_index].datum_type) ||
                    cells[cell_index].bytes_offset != expected_payload_offset ||
                    !checked_add(cells[cell_index].bytes_offset,
                                 cells[cell_index].bytes_length, &payload_end) ||
                    payload_end > record.bytes_length)
                    return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x218);
                expected_payload_offset = payload_end;
            }

            uint64_t block_row = 0;
            while (block_row < block_rows) {
                const size_t first_cell = static_cast<size_t>(
                    block_row * record.column_count);
                const uint64_t chunk_payload_begin =
                    cells[first_cell].bytes_offset;
                uint64_t end_row = block_row;
                uint64_t chunk_payload_end = chunk_payload_begin;
                while (end_row < block_rows) {
                    const size_t last_cell = static_cast<size_t>(
                        (end_row + 1) * record.column_count - 1);
                    uint64_t candidate_end = 0;
                    if (!checked_add(cells[last_cell].bytes_offset,
                                     cells[last_cell].bytes_length,
                                     &candidate_end))
                        return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x219);
                    if (end_row != block_row &&
                        candidate_end - chunk_payload_begin > stage_capacity)
                        break;
                    if (candidate_end - chunk_payload_begin > stage_capacity)
                        return fail(CSD_SQL_STATUS_FIFO_TOO_SMALL, index, 0x31a);
                    chunk_payload_end = candidate_end;
                    ++end_row;
                }
                const uint64_t chunk_rows = end_row - block_row;
                const uint64_t payload_bytes =
                    chunk_payload_end - chunk_payload_begin;
                if (payload_bytes > std::numeric_limits<size_t>::max())
                    return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x21a);
                std::vector<unsigned char> payload(
                    static_cast<size_t>(payload_bytes));
                if (payload_bytes != 0 &&
                    !read_spill_bytes(index,
                        payload_start + chunk_payload_begin,
                        payload.data(), payload_bytes))
                    return false;

                Batch chunk;
                chunk.columns = record.column_count;
                chunk.rows.resize(static_cast<size_t>(chunk_rows));
                for (size_t row = 0; row < chunk.rows.size(); ++row) {
                    chunk.rows[row].resize(record.column_count);
                    for (uint16_t column = 0; column < record.column_count;
                         ++column) {
                        const CsdSqlResultCell &cell = cells[
                            static_cast<size_t>((block_row + row) *
                                record.column_count + column)];
                        Value &value = chunk.rows[row][column];
                        value.is_null = (cell.flags & kResultNull) != 0;
                        value.type = cell.datum_type;
                        value.integer = cell.integer;
                        value.numeric_scale = cell.numeric_scale;
                        if (cell.bytes_length != 0) {
                            const uint64_t within =
                                cell.bytes_offset - chunk_payload_begin;
                            value.bytes.assign(reinterpret_cast<const char *>(
                                payload.data() + within), cell.bytes_length);
                        }
                    }
                }
                std::vector<unsigned char>().swap(payload);
                if (!consumer(chunk)) return false;
                block_row = end_row;
            }
            first_row += block_rows;
        }
        if (expected_payload_offset != record.bytes_length)
            return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x21b);
        {
            uint64_t total = 0;
            std::lock_guard<std::mutex> status_guard(status_mutex_);
            if (checked_add(status_.spill_read_bytes, handle.length, &total))
                status_.spill_read_bytes = total;
            else status_.spill_read_bytes = std::numeric_limits<uint64_t>::max();
        }
        return true;
    }
