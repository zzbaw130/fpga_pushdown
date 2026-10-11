/* Replay a PostgreSQL-lowered plan against the first page of each heap input. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "kernel/cemu_def.h"
#include "kernel/csd_plan.h"

extern "C" long long csd_sql(struct cemu_args *args);

static std::vector<unsigned char>
read_file(const char *path, size_t limit = 0, size_t offset = 0)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::perror(path);
        std::exit(1);
    }
	input.seekg(0, std::ios::end);
	size_t file_length = static_cast<size_t>(input.tellg());
	if (offset > file_length) {
		std::fprintf(stderr, "offset past EOF: %s\n", path);
		std::exit(1);
	}
	size_t length = file_length - offset;
	input.seekg(offset, std::ios::beg);
    if (limit != 0 && length > limit)
        length = limit;
    std::vector<unsigned char> bytes(length);
    input.read(reinterpret_cast<char *>(bytes.data()), length);
    if (static_cast<size_t>(input.gcount()) != length) {
        std::fprintf(stderr, "short read: %s\n", path);
        std::exit(1);
    }
    return bytes;
}

int
main(int argc, char **argv)
{
	if (argc < 2 || argc > 5) {
		std::fprintf(stderr, "usage: %s PLAN [HEAP [FIRST_PAGE [PAGE_COUNT]]]\n", argv[0]);
        return 2;
    }
    std::vector<unsigned char> plan_range = read_file(argv[1]);
	if (plan_range.size() < sizeof(CsdSqlPlanHeader)) {
		std::fprintf(stderr, "invalid plan\n");
        return 1;
    }
    CsdSqlPlanHeader *header = reinterpret_cast<CsdSqlPlanHeader *>(plan_range.data());
    plan_range.resize(header->plan_bytes);
    CsdSqlPlanNode *nodes = reinterpret_cast<CsdSqlPlanNode *>(
        plan_range.data() + header->header_bytes);
	std::printf("plan nodes=%u root=%u output=%u spill=%u bytes=%u\n",
		header->node_count, header->root_node, header->output_range,
		header->spill_range, header->plan_bytes);
    for (uint32_t index = 0; index < header->node_count; ++index) {
		std::printf("node=%u opcode=%u left=%u right=%u expression=%u+%u\n",
			index, nodes[index].opcode, nodes[index].left_input,
			nodes[index].right_input, nodes[index].expression_offset,
			nodes[index].expression_length);
        if (nodes[index].opcode == CSD_SQL_OP_SCAN ||
            nodes[index].opcode == CSD_SQL_OP_INDEX_SCAN) {
            CsdSqlScanSpec *scan = reinterpret_cast<CsdSqlScanSpec *>(
                plan_range.data() + nodes[index].expression_offset);
			const CsdSqlColumnSpec *columns = reinterpret_cast<const CsdSqlColumnSpec *>(scan + 1);
			std::printf("scan node=%u attrs=%u outputs=%u input_range=%u pages=%u\n",
				index, scan->attr_count, scan->output_count, scan->input_range, scan->page_count);
			for (uint16_t attribute = 0; attribute < scan->attr_count; ++attribute)
				std::printf("  attnum=%u type=%u len=%d scale=%d flags=%u output=%u\n",
					columns[attribute].attnum, columns[attribute].datum_type,
					columns[attribute].attlen, columns[attribute].numeric_scale,
					columns[attribute].flags, columns[attribute].output_column);
			if (argc >= 3) {
				scan->first_page = 0;
				scan->page_count = argc >= 5 ? std::strtoul(argv[4], NULL, 10) : 1;
			}
		} else if (nodes[index].opcode == CSD_SQL_OP_HASH_JOIN ||
				   nodes[index].opcode == CSD_SQL_OP_MERGE_JOIN ||
				   nodes[index].opcode == CSD_SQL_OP_NL_JOIN) {
			const CsdSqlJoinSpec *join = reinterpret_cast<const CsdSqlJoinSpec *>(
				plan_range.data() + nodes[index].expression_offset);
			const CsdSqlJoinKey *keys = reinterpret_cast<const CsdSqlJoinKey *>(join + 1);
			std::printf("  join kind=%u keys=%u residual=%u+%u\n", join->join_kind,
				join->key_count, join->residual_offset, join->residual_length);
			for (uint16_t key = 0; key < join->key_count; ++key)
				std::printf("    key left=%u right=%u\n", keys[key].left_key, keys[key].right_key);
		} else if (nodes[index].opcode == CSD_SQL_OP_HASH_AGG ||
				   nodes[index].opcode == CSD_SQL_OP_GROUP_AGG) {
			const CsdSqlAggregateSpec *aggregate = reinterpret_cast<const CsdSqlAggregateSpec *>(
				plan_range.data() + nodes[index].expression_offset);
			std::printf("  aggregate groups=%u values=%u\n", aggregate->group_count,
				aggregate->aggregate_count);
        }
    }
	if (argc == 2) return 0;
	const uint32_t first_page = argc >= 4 ? std::strtoul(argv[3], NULL, 10) : 0;
	const uint32_t page_count = argc >= 5 ? std::strtoul(argv[4], NULL, 10) : 1;
	std::vector<unsigned char> input = read_file(argv[2],
		static_cast<size_t>(page_count) * CSD_SQL_PAGE_BYTES,
		static_cast<size_t>(first_page) * CSD_SQL_PAGE_BYTES);
	if (input.size() != static_cast<size_t>(page_count) * CSD_SQL_PAGE_BYTES) {
		std::fprintf(stderr, "invalid heap page\n");
		return 1;
	}
    std::vector<unsigned char> output(1024 * 1024, 0);
    std::vector<unsigned char> spill(1024 * 1024, 0);
    std::vector<unsigned char> status_bytes(sizeof(CsdSqlExecutionStatus), 0);
    void *ranges[] = {input.data(), output.data(), spill.data(),
                      plan_range.data(), status_bytes.data()};
    long long lengths[] = {
        static_cast<long long>(input.size()), static_cast<long long>(output.size()),
        static_cast<long long>(spill.size()), static_cast<long long>(plan_range.size()),
        static_cast<long long>(status_bytes.size())};
    struct cemu_args args = {};
    args.numr = 5;
    args.mr_addr = ranges;
    args.mr_len = lengths;
    args.cparam1 = plan_range.size();
    args.cparam2 = 3;
    long long returned = csd_sql(&args);
    const CsdSqlExecutionStatus *status =
        reinterpret_cast<const CsdSqlExecutionStatus *>(status_bytes.data());
	std::printf("pages=%u..%u return=%lld status=%u node=%u detail=%u validated=%u executed=%u\n",
				first_page, first_page + page_count - 1,
                returned, status->status_code, status->failed_node,
                status->reserved32, status->validated_nodes, status->executed_nodes);
    return status->status_code == CSD_SQL_STATUS_OK ? 0 : 1;
}
