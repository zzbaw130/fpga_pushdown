# PostgreSQL CSD extension

`csd_pg` is a PGXS extension for the pinned PostgreSQL 12 tree.  It adds
`csd.exe_path = host|csd|auto`, FPGA-style expression eligibility checks,
CSD scan/join/aggregate/sort paths, and an inspectable operator-tree cost
model.

Build and install it into the project-local PostgreSQL installation with:

```bash
make -C code/postgres-csd \
  PG_CONFIG="$PWD/build/install/bin/pg_config"
make -C code/postgres-csd \
  PG_CONFIG="$PWD/build/install/bin/pg_config" install
```

Load the module in each test session before setting its GUCs:

```sql
LOAD 'csd_pg';
SET csd.exe_path = 'csd';
EXPLAIN SELECT ...;
```

The executor serializes a selected path into a structurally validated ABI-v4
header/node table;
every scan contains the real heap page count and physical PostgreSQL column
layout. Filters, projections, join residuals, HAVING, and final arithmetic are
compiled to bounded postfix bytecode, including fixed-point/date constants,
`LIKE`, scalar-array `IN`, boolean/null logic, `CASE`, and substring. Join,
aggregate, and sort keys are bound to input schemas and emitted as real ABI-v4
descriptor arrays; complex aggregate arguments use a streaming pre-projection
and subquery outputs use positional projection. `PARAM_EXEC` inputs are marked
as relocations. They are patched lazily on the first `ExecCustomScan` call,
after the executor tree is fully initialized, so scalar InitPlans can safely
run before the outer Program Execute.

Q2's correlated scalar `MIN` is decorrelated into a grouped aggregate branch
and two-key join. Q16's hashed `NOT IN` becomes a hash anti-join only after both
columns are proven `NOT NULL`; both branches are merged into the outer CSD DAG.
All 22 TPC-H plans therefore lower with zero pending expressions. The executor
stages relation heaps into query-local FDMFS ranges, creates a CEMU MRS, and
submits the original `NVME_IOCTL_IO_CMD` Program Execute command. It reads the
status/result ranges and returns PostgreSQL virtual tuples without a silent
host fallback. Duplicate scans of one relation alias a single staged inode,
which is required by Q17/Q18/Q21 in the 2GB CEMU memory namespace. Numeric
results preserve PostgreSQL dynamic scales for AVG and division, subject to
the fixed-point int64 range.

Run all supplied TPC-H planner cases with `tests/test_csd_planner.sh`.  In
forced mode the test permits a PostgreSQL `Limit` control node above a
`CsdScan`, but rejects host scan, join, aggregate, or sort nodes.

Run `tests/test_csd_tpch_execution.sh` inside CEMU against a running,
NVMe-staged guest cluster to compare all 22 forced-CSD results exactly with the
HOST path. The PostgreSQL executable must retain `CAP_SYS_ADMIN` because the
Linux NVMe passthrough ioctl checks it before reaching CEMU.
