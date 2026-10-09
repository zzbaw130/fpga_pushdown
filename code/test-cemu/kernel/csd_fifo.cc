    /*
     * Encode decoded tuples into alternating FDM ranges before forwarding
     * them to the next software operator.  The payload format is deliberately
     * identical to result cells: fixed descriptors followed by compact byte
     * data.  A batch is split when it would exceed one FIFO range.
     *
     * A bounded consumer may take ownership of each decoded FIFO batch.  This
     * lets a blocking operator build bounded state without first materializing
     * the complete Scan output in the DAG result vector.
     */
    void note_stream_input(const Batch &batch)
    {
        const uint64_t bytes = batch_bytes(batch);
        std::lock_guard<std::mutex> status_guard(status_mutex_);
        ++status_.stream_input_batches;
        status_.stream_input_rows += batch.rows.size();
        status_.stream_input_peak_bytes = std::max<uint64_t>(
            status_.stream_input_peak_bytes, bytes);
    }

    bool fifo_forward_batch(uint32_t index, Batch &input, Batch *output,
                            const std::function<bool(Batch &)> *consumer = NULL)
    {
        output->columns = input.columns;
        if (input.rows.empty()) return true;
        if (!(header_->flags & CSD_SQL_PLAN_FDM_FIFO)) {
            if (consumer != NULL) {
                note_stream_input(input);
                return (*consumer)(input);
            }
            output->rows.insert(output->rows.end(),
                std::make_move_iterator(input.rows.begin()),
                std::make_move_iterator(input.rows.end()));
            return true;
        }

        size_t first_row = 0;
        while (first_row < input.rows.size()) {
            std::unique_lock<std::mutex> fifo_guard(fifo_mutex_);
            const uint32_t range = (fifo_generation_ & 1U) == 0 ?
                header_->fifo_range_a : header_->fifo_range_b;
            unsigned char *buffer = static_cast<unsigned char *>(args_->mr_addr[range]);
            const uint64_t capacity = std::min<uint64_t>(header_->fifo_bytes,
                static_cast<uint64_t>(args_->mr_len[range]));
            uint64_t payload_bytes = 0;
            size_t end_row = first_row;
            for (; end_row < input.rows.size(); ++end_row) {
                const Row &row = input.rows[end_row];
                if (row.size() != input.columns)
                    return fail(CSD_SQL_STATUS_BAD_NODE, index, 0x302);
                uint64_t row_payload = 0;
                for (Row::const_iterator value = row.begin(); value != row.end(); ++value) {
                    if (value->bytes.size() > std::numeric_limits<uint16_t>::max() ||
                        !checked_add(row_payload, value->bytes.size(), &row_payload))
                        return fail(CSD_SQL_STATUS_FIFO_TOO_SMALL, index, 0x303);
                }
                uint64_t row_cells = 0, candidate_cells = 0, candidate_total = 0;
                if (!checked_mul(input.columns, sizeof(CsdSqlResultCell), &row_cells) ||
                    !checked_mul(end_row - first_row + 1, row_cells, &candidate_cells) ||
                    !checked_add(sizeof(FifoBatchHeader), candidate_cells, &candidate_total) ||
                    !checked_add(candidate_total, payload_bytes, &candidate_total) ||
                    !checked_add(candidate_total, row_payload, &candidate_total))
                    return fail(CSD_SQL_STATUS_FIFO_TOO_SMALL, index, 0x304);
                if (candidate_total > capacity) break;
                payload_bytes += row_payload;
            }
            if (end_row == first_row)
                return fail(CSD_SQL_STATUS_FIFO_TOO_SMALL, index, 0x305);

            const uint64_t row_count = end_row - first_row;
            uint64_t cell_count = 0, cells_bytes = 0, bytes_offset = 0, total_bytes = 0;
            if (!checked_mul(row_count, input.columns, &cell_count) ||
                !checked_mul(cell_count, sizeof(CsdSqlResultCell), &cells_bytes) ||
                !checked_add(sizeof(FifoBatchHeader), cells_bytes, &bytes_offset) ||
                !checked_add(bytes_offset, payload_bytes, &total_bytes) ||
                row_count > std::numeric_limits<uint32_t>::max())
                return fail(CSD_SQL_STATUS_FIFO_TOO_SMALL, index, 0x306);

            FifoBatchHeader fifo = {};
            fifo.magic = kFifoMagic;
            fifo.generation = fifo_generation_;
            fifo.row_count = static_cast<uint32_t>(row_count);
            fifo.column_count = input.columns;
            fifo.total_bytes = total_bytes;
            fifo.cells_offset = sizeof(FifoBatchHeader);
            fifo.bytes_offset = bytes_offset;
            fifo.bytes_length = payload_bytes;
            std::memcpy(buffer, &fifo, sizeof(fifo));
            CsdSqlResultCell *cells = reinterpret_cast<CsdSqlResultCell *>(
                buffer + fifo.cells_offset);
            unsigned char *payload = buffer + fifo.bytes_offset;
            uint64_t cell_index = 0, payload_offset = 0;
            for (size_t row_index = first_row; row_index < end_row; ++row_index)
                for (Row::const_iterator value = input.rows[row_index].begin();
                     value != input.rows[row_index].end(); ++value) {
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
            std::atomic_thread_fence(std::memory_order_release);

            const FifoBatchHeader *ready = reinterpret_cast<const FifoBatchHeader *>(buffer);
            if (ready->magic != kFifoMagic || ready->generation != fifo_generation_ ||
                ready->column_count != input.columns || ready->row_count != row_count ||
                ready->cells_offset != sizeof(FifoBatchHeader) ||
                ready->bytes_offset != bytes_offset || ready->bytes_length != payload_bytes ||
                ready->total_bytes != total_bytes || total_bytes > capacity)
                return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x307);
            std::atomic_thread_fence(std::memory_order_acquire);
            const CsdSqlResultCell *read_cells = reinterpret_cast<const CsdSqlResultCell *>(
                buffer + ready->cells_offset);
            const unsigned char *read_payload = buffer + ready->bytes_offset;
            Batch forwarded;
            forwarded.columns = ready->column_count;
            forwarded.rows.reserve(row_count);
            for (uint32_t row_index = 0; row_index < ready->row_count; ++row_index) {
                Row row;
                row.reserve(ready->column_count);
                for (uint16_t column = 0; column < ready->column_count; ++column) {
                    const CsdSqlResultCell &cell = read_cells[
                        static_cast<uint64_t>(row_index) * ready->column_count + column];
                    if (cell.bytes_offset > ready->bytes_length ||
                        cell.bytes_length > ready->bytes_length - cell.bytes_offset ||
                        !is_supported_type(cell.datum_type))
                        return fail(CSD_SQL_STATUS_BAD_ABI, index, 0x308);
                    Value value;
                    value.integer = cell.integer;
                    value.type = cell.datum_type;
                    value.is_null = (cell.flags & kResultNull) != 0;
                    value.numeric_scale = cell.numeric_scale;
                    if (cell.bytes_length != 0)
                        value.bytes.assign(reinterpret_cast<const char *>(
                            read_payload + cell.bytes_offset), cell.bytes_length);
                    row.push_back(std::move(value));
                }
                forwarded.rows.push_back(std::move(row));
            }
            {
                std::lock_guard<std::mutex> status_guard(status_mutex_);
                ++status_.fifo_batches;
                status_.fifo_write_bytes += total_bytes;
                status_.fifo_read_bytes += total_bytes;
            }
            ++fifo_generation_;
            /* The reconstructed batch owns its bytes.  Release the shared FDM
             * FIFO before a blocking consumer waits for arena capacity, or an
             * independent branch holding that capacity could be prevented
             * from advancing its own FIFO and releasing its lease. */
            fifo_guard.unlock();
            if (consumer != NULL) {
                note_stream_input(forwarded);
                if (!(*consumer)(forwarded)) return false;
            } else {
                output->rows.insert(output->rows.end(),
                    std::make_move_iterator(forwarded.rows.begin()),
                    std::make_move_iterator(forwarded.rows.end()));
            }
            first_row = end_row;
        }
        return true;
    }
