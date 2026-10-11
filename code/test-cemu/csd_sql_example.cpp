/*
 * Minimal real-CEMU validation for the CSD SQL shared library.  It writes one
 * synthetic PostgreSQL heap page to FDMFS, stages a versioned plan in a CSD
 * memory range, passes its descriptor through Program Execute, and checks the
 * result and spill ranges afterwards.
 */
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <vector>

#include "cemu_ioctl.h"
#include "kernel/csd_plan.h"
#include "util.h"

template <typename T>
static uint32_t append(std::vector<unsigned char> *buffer, const T &value)
{
    const uint32_t offset = buffer->size();
    buffer->resize(offset + sizeof(value));
    memcpy(buffer->data() + offset, &value, sizeof(value));
    return offset;
}

template <typename T>
static void overwrite(std::vector<unsigned char> *buffer, uint32_t offset, const T &value)
{
    assert(offset + sizeof(value) <= buffer->size());
    memcpy(buffer->data() + offset, &value, sizeof(value));
}

static void put_u16(unsigned char *target, uint16_t value)
{
    memcpy(target, &value, sizeof(value));
}

static void put_u32(unsigned char *target, uint32_t value)
{
    memcpy(target, &value, sizeof(value));
}

static void put_i32(unsigned char *target, int32_t value)
{
    memcpy(target, &value, sizeof(value));
}

static void add_tuple(unsigned char *page, uint16_t item, uint16_t offset,
                      int32_t key, uint16_t numeric_digit)
{
    const uint16_t tuple_length = 36;
    put_u32(page + 24 + item * 4,
            static_cast<uint32_t>(offset) | (static_cast<uint32_t>(1) << 15) |
            (static_cast<uint32_t>(tuple_length) << 17));
    unsigned char *tuple = page + offset;
    tuple[22] = 24;                 // t_hoff
    put_i32(tuple + 24, key);
    put_u32(tuple + 28, 8U << 2);   // varlena length = 8
    put_u16(tuple + 32, 0x8100U);   // NumericShort(scale=2, weight=0)
    put_u16(tuple + 34, numeric_digit);
}

static CsdSqlPlanNode make_node(uint32_t id, uint16_t opcode)
{
    CsdSqlPlanNode node = {};
    node.node_id = id;
    node.opcode = opcode;
    node.left_input = CSD_SQL_INVALID_NODE;
    node.right_input = CSD_SQL_INVALID_NODE;
    node.aux_input = CSD_SQL_INVALID_NODE;
    node.arg1 = 1;
    return node;
}

static std::vector<unsigned char> build_plan()
{
    std::vector<unsigned char> plan(sizeof(CsdSqlPlanHeader) + 3 * sizeof(CsdSqlPlanNode), 0);
    std::vector<CsdSqlPlanNode> nodes;
    nodes.push_back(make_node(0, CSD_SQL_OP_SCAN));
    nodes.push_back(make_node(1, CSD_SQL_OP_FILTER));
    nodes.push_back(make_node(2, CSD_SQL_OP_SORT));

    CsdSqlScanSpec scan = {};
    scan.input_range = 0;
    scan.page_count = 1;
    scan.attr_count = 2;
    scan.output_count = 2;
    scan.flags = CSD_SQL_SCAN_FROZEN_PAGES;
    nodes[0].expression_offset = append(&plan, scan);
    CsdSqlColumnSpec key = {};
    key.attnum = 1;
    key.output_column = 0;
    key.attlen = 4;
    key.alignment = 4;
    key.datum_type = CSD_SQL_TYPE_INT32;
    key.flags = CSD_SQL_COLUMN_OUTPUT;
    append(&plan, key);
    CsdSqlColumnSpec value = {};
    value.attnum = 2;
    value.output_column = 1;
    value.attlen = -1;
    value.alignment = 4;
    value.datum_type = CSD_SQL_TYPE_NUMERIC;
    value.numeric_scale = 2;
    value.flags = CSD_SQL_COLUMN_OUTPUT | CSD_SQL_COLUMN_NO_EXTERNAL;
    append(&plan, value);
    nodes[0].expression_length = sizeof(scan) + 2 * sizeof(CsdSqlColumnSpec);

    nodes[1].left_input = 0;
    CsdSqlExpressionSpec filter = {};
    filter.output_count = 1;
    filter.instruction_count = 4;
    filter.max_stack = 2;
    nodes[1].expression_offset = append(&plan, filter);
    CsdSqlExpressionInstruction instruction = {};
    instruction.opcode = CSD_SQL_EXPR_PUSH_COLUMN;
    instruction.operand = 1;
    append(&plan, instruction);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_PUSH_CONSTANT;
    instruction.datum_type = CSD_SQL_TYPE_NUMERIC;
    instruction.numeric_scale = 2;
    instruction.immediate = 800; // 8.00
    append(&plan, instruction);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_GREATER;
    append(&plan, instruction);
    instruction = CsdSqlExpressionInstruction();
    instruction.opcode = CSD_SQL_EXPR_EMIT;
    append(&plan, instruction);
    nodes[1].expression_length = sizeof(filter) + 4 * sizeof(CsdSqlExpressionInstruction);

    nodes[2].left_input = 1;
    nodes[2].flags = CSD_SQL_NODE_BLOCKING | CSD_SQL_NODE_SPILLABLE;
    CsdSqlSortSpec sort = {};
    sort.key_count = 1;
    nodes[2].expression_offset = append(&plan, sort);
    CsdSqlSortKey sort_key = {};
    sort_key.key_column = 1;
    append(&plan, sort_key);
    nodes[2].expression_length = sizeof(sort) + sizeof(sort_key);

    CsdSqlPlanHeader header = {};
    header.magic = CSD_SQL_PLAN_MAGIC;
    header.version = CSD_SQL_PLAN_VERSION;
    header.header_bytes = sizeof(header);
    header.node_count = nodes.size();
    header.node_bytes = sizeof(CsdSqlPlanNode);
    header.plan_bytes = plan.size();
    header.root_node = 2;
    header.flags = CSD_SQL_PLAN_STATUS_LAST_RANGE | CSD_SQL_PLAN_FROZEN_SNAPSHOT |
                   CSD_SQL_PLAN_STAGED_RANGE;
    header.output_range = 1;
    header.spill_range = 2;
    header.plan_id = 0xc5d2026;
    /* Force external run generation even as the result-cell ABI evolves. */
    header.work_mem_bytes = 63;
    header.parallel_workers = 4;
    overwrite(&plan, 0, header);
    for (uint32_t index = 0; index < nodes.size(); ++index)
        overwrite(&plan, sizeof(header) + index * sizeof(CsdSqlPlanNode), nodes[index]);
    return plan;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--cleanup-mrs") == 0) {
        struct ioctl_create_mrs stale = {};
        stale.rsid = static_cast<int32_t>(strtol(argv[2], NULL, 10));
        const int control_fd = open("/dev/nvme0c3", O_RDWR);
        if (control_fd < 0 || stale.rsid <= 0) {
            perror("open CEMU control device");
            return 1;
        }
        if (ioctl(control_fd, IOCTL_CEMU_DELETE_MRS, &stale) != 0) {
            perror("IOCTL_CEMU_DELETE_MRS");
            close(control_fd);
            return 1;
        }
        close(control_fd);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--cleanup") == 0) {
        struct ioctl_download stale = {};
        stale.name = "csd_sql";
        stale.pind = static_cast<int32_t>(strtol(argv[2], NULL, 10));
        const int control_fd = open("/dev/nvme0c3", O_RDWR);
        if (control_fd < 0 || stale.pind <= 0) {
            perror("open CEMU control device");
            return 1;
        }
        /* Deactivation is harmless when a previous failed run never activated. */
        (void) ioctl(control_fd, IOCTL_CEMU_DEACTIVATE, &stale);
        if (ioctl(control_fd, IOCTL_CEMU_UNLOAD, &stale) != 0) {
            perror("IOCTL_CEMU_UNLOAD");
            close(control_fd);
            return 1;
        }
        close(control_fd);
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--cleanup <program-index> | --cleanup-mrs <range-set-id>]\n", argv[0]);
        return 2;
    }
    std::vector<unsigned char> page(CSD_SQL_PAGE_BYTES, 0);
    put_u16(page.data() + 12, 24 + 3 * 4);
    add_tuple(page.data(), 0, 128, 3, 20);
    add_tuple(page.data(), 1, 192, 2, 5);
    add_tuple(page.data(), 2, 256, 1, 10);
    std::vector<unsigned char> plan = build_plan();
    unsigned char *io_buffer = static_cast<unsigned char *>(aligned_alloc(4096, CSD_SQL_PAGE_BYTES));
    if (io_buffer == NULL) {
        perror("aligned_alloc");
        return 1;
    }

    struct ioctl_download download = {};
    download.name = "csd_sql";
    const char *shared_library = getenv("CSD_SQL_SHARED_LIBRARY");
    if (shared_library == NULL)
        shared_library = "/workspace/code/CEMU/tests/cemu/build/csd_sql.so";
    prep_shared_library(shared_library, "csd_sql", &download);
    const int control_fd = open("/dev/nvme0c3", O_RDWR);
    const int execute_fd = open("/dev/ng0n3", O_RDWR);
    if (control_fd < 0 || execute_fd < 0) {
        perror("open CEMU device");
        return 1;
    }
    if (ioctl(control_fd, IOCTL_CEMU_DOWNLOAD, &download) <= 0) {
        perror("IOCTL_CEMU_DOWNLOAD");
        return 1;
    }
    printf("CSD staged-plan program index: %d\n", download.pind);
    if (ioctl(control_fd, IOCTL_CEMU_ACTIVATE, &download) != 0) {
        perror("IOCTL_CEMU_ACTIVATE");
        return 1;
    }

    struct ioctl_create_mrs mrs = {};
    /* input, result, Flash spill, staged plan, execution status */
    prep_mrs(&mrs, 5);
    for (int range = 0; range < mrs.nr_fd; ++range) {
        char path[64] = {};
        snprintf(path, sizeof(path), "/mnt/fdm0/%d", range);
        mrs.fd[range] = open(path, O_RDWR);
        mrs.size[range] = CSD_SQL_PAGE_BYTES;
        if (mrs.fd[range] < 0) {
            perror(path);
            return 1;
        }
    }
    memcpy(io_buffer, page.data(), page.size());
    if (pwrite(mrs.fd[0], io_buffer, CSD_SQL_PAGE_BYTES, 0) != CSD_SQL_PAGE_BYTES) {
        perror("pwrite FDM range");
        return 1;
    }
    memset(io_buffer, 0, CSD_SQL_PAGE_BYTES);
    for (int range = 1; range < mrs.nr_fd; ++range) {
        if (pwrite(mrs.fd[range], io_buffer, CSD_SQL_PAGE_BYTES, 0) != CSD_SQL_PAGE_BYTES) {
            perror("clear FDM range");
            return 1;
        }
    }
    /* FDMFS requires page-aligned I/O; cparam1 still carries the exact plan length. */
    memset(io_buffer, 0, CSD_SQL_PAGE_BYTES);
    memcpy(io_buffer, plan.data(), plan.size());
    if (pwrite(mrs.fd[3], io_buffer, CSD_SQL_PAGE_BYTES, 0) != CSD_SQL_PAGE_BYTES) {
        perror("pwrite staged CSD plan");
        return 1;
    }
    if (ioctl(control_fd, IOCTL_CEMU_CREATE_MRS, &mrs) != 0) {
        perror("IOCTL_CEMU_CREATE_MRS");
        return 1;
    }

    struct nvme_passthru_cmd command = {};
    prep_nvme_passthru_program_execute(&command, plan.size(), 3, download.pind, mrs.rsid,
                                       0, 0, 0, 0, NULL, 0);
    if (ioctl(execute_fd, NVME_IOCTL_IO_CMD, &command) < 0) {
        perror("NVME_IOCTL_IO_CMD");
        struct ioctl_create_mrs remove = {};
        remove.rsid = mrs.rsid;
        (void) ioctl(control_fd, IOCTL_CEMU_DELETE_MRS, &remove);
        (void) ioctl(control_fd, IOCTL_CEMU_DEACTIVATE, &download);
        (void) ioctl(control_fd, IOCTL_CEMU_UNLOAD, &download);
        return 1;
    }

    assert(pread(mrs.fd[1], io_buffer, CSD_SQL_PAGE_BYTES, 0) == CSD_SQL_PAGE_BYTES);
    CsdSqlResultHeader result = {};
    CsdSqlResultCell cells[4] = {};
    memcpy(&result, io_buffer, sizeof(result));
    memcpy(cells, io_buffer + result.cells_offset, sizeof(cells));
    assert(pread(mrs.fd[4], io_buffer, CSD_SQL_PAGE_BYTES, 0) == CSD_SQL_PAGE_BYTES);
    CsdSqlExecutionStatus status = {};
    memcpy(&status, io_buffer, sizeof(status));
    if (status.status_code != CSD_SQL_STATUS_OK || result.magic != CSD_SQL_RESULT_MAGIC) {
        fprintf(stderr, "CSD execution failed: status_magic=%#x status=%u failed_node=%u transport=%#x result_magic=%#x\n",
                status.magic, status.status_code, status.failed_node, status.reserved32, result.magic);
        struct ioctl_create_mrs remove = {};
        remove.rsid = mrs.rsid;
        (void) ioctl(control_fd, IOCTL_CEMU_DELETE_MRS, &remove);
        (void) ioctl(control_fd, IOCTL_CEMU_DEACTIVATE, &download);
        (void) ioctl(control_fd, IOCTL_CEMU_UNLOAD, &download);
        return 1;
    }
    assert(result.row_count == 2);
    assert(result.column_count == 2);
    assert(cells[0].integer == 1 && cells[1].integer == 1000);
    assert(cells[2].integer == 3 && cells[3].integer == 2000);
    assert(status.status_code == CSD_SQL_STATUS_OK);
    assert(status.spill_write_bytes > 0);
    printf("CEMU CSD SQL validation passed: rows=%u spill=%llu modeled_cycles=%llu\n",
           result.row_count, static_cast<unsigned long long>(status.spill_write_bytes),
           static_cast<unsigned long long>(status.modeled_cycles));

    struct ioctl_create_mrs remove = {};
    remove.rsid = mrs.rsid;
    assert(ioctl(control_fd, IOCTL_CEMU_DELETE_MRS, &remove) == 0);
    assert(ioctl(control_fd, IOCTL_CEMU_DEACTIVATE, &download) == 0);
    assert(ioctl(control_fd, IOCTL_CEMU_UNLOAD, &download) == 0);
    for (int range = 0; range < mrs.nr_fd; ++range)
        close(mrs.fd[range]);
    free(mrs.fd);
    free(mrs.off);
    free(mrs.size);
    free(download.addr);
    free(io_buffer);
    close(execute_fd);
    close(control_fd);
    return 0;
}
