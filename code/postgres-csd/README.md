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
layout.  Base-relation filters are compiled to bounded postfix bytecode,
including fixed-point/date constants, `LIKE`, scalar-array `IN`, boolean/null
logic, and substring.  `PARAM_EXEC` inputs are marked as relocations and CEMU
rejects them until the host executor patches the value.

Join/group/sort descriptors, result decoding, and NVMe submission remain the
next integration stages.  Q16's hashed `NOT IN SubPlan` is explicitly counted
as pending until it is normalized to a null-safe anti-join; it is not emitted
as an incorrect scalar predicate.  There is no silent host fallback when a
selected `CustomScan` reaches the executor.

Run all supplied TPC-H planner cases with `tests/test_csd_planner.sh`.  In
forced mode the test permits a PostgreSQL `Limit` control node above a
`CsdScan`, but rejects host scan, join, aggregate, or sort nodes.
