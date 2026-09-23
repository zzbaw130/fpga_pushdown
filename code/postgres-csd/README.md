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

The current executor intentionally rejects execution.  Planner selection,
operator-tree metadata, and cost comparison are active.  A selected path is
already serialized into a structurally validated ABI-v4 header/node table;
every scan contains the real heap page count and physical PostgreSQL column
layout. Filters, projections, join residuals, HAVING, and final arithmetic are
compiled to bounded postfix bytecode, including fixed-point/date constants,
`LIKE`, scalar-array `IN`, boolean/null logic, `CASE`, and substring. Join,
aggregate, and sort keys are bound to input schemas and emitted as real ABI-v4
descriptor arrays; complex aggregate arguments use a streaming pre-projection
and subquery outputs use positional projection. `PARAM_EXEC` inputs are marked
as relocations and CEMU rejects them until the host executor patches the value.

Result decoding and NVMe submission remain integration stages. Q2's correlated
scalar `MIN` SubPlan and Q16's hashed `NOT IN SubPlan` are explicitly counted
as pending relational rewrites; neither is emitted as an incorrect scalar
predicate. There is no silent host fallback when a selected `CustomScan`
reaches the executor.

Run all supplied TPC-H planner cases with `tests/test_csd_planner.sh`.  In
forced mode the test permits a PostgreSQL `Limit` control node above a
`CsdScan`, but rejects host scan, join, aggregate, or sort nodes.
