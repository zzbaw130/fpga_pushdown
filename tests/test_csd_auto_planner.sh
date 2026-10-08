#!/usr/bin/env bash
# Validate the AUTO CSD-candidate frontier and its one-way CSD->HOST cut.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SF="${CSD_AUTO_SF:-1}"
PG_CONFIG="${PG_CONFIG:-${ROOT}/build/install/bin/pg_config}"
PSQL="$(dirname "${PG_CONFIG}")/psql"
SOCKET_DIR="${CSD_AUTO_SOCKET_DIR:-${ROOT}/.pgsock_tpch_s${SF}}"
PORT="${CSD_AUTO_PORT:-$((55432 + SF))}"
DATABASE="${CSD_AUTO_DATABASE:-tpch_s${SF}}"
OUTPUT_DIR="${CSD_AUTO_OUTPUT_DIR:-/tmp/csd_auto_planner}"
SKIP_BUILD="${CSD_AUTO_SKIP_BUILD:-0}"

[[ -x "${PG_CONFIG}" && -x "${PSQL}" ]] || {
    echo "Missing PostgreSQL installation below ${PG_CONFIG}." >&2
    exit 1
}

if [[ "${SKIP_BUILD}" != 1 ]]; then
    make -C "${ROOT}/code/postgres-csd" clean
    make -C "${ROOT}/code/postgres-csd" PG_CONFIG="${PG_CONFIG}"
    make -C "${ROOT}/code/postgres-csd" PG_CONFIG="${PG_CONFIG}" install
fi

mkdir -p "${OUTPUT_DIR}"
psql_base=("${PSQL}" -X -Atq -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}"
           -p "${PORT}" -d "${DATABASE}")

run_explain() {
    local name="$1"
    local settings="$2"
    local query="$3"

    "${psql_base[@]}" -c "LOAD 'csd_pg'; SET csd.exe_path='auto'; ${settings} EXPLAIN ${query};" \
        >"${OUTPUT_DIR}/${name}.txt"
}

common="SET csd.flash_page_cost=0.6; SET csd.tuple_cost=0;"
common+=" SET csd.aggregate_tuple_cost=0; SET csd.result_byte_cost=0;"
run_explain cold "${common} SET csd.host_cache_fraction=0;" \
    "SELECT count(*) FROM lineitem"
run_explain hot "${common} SET csd.host_cache_fraction=1;" \
    "SELECT count(*) FROM lineitem"

grep -q '^Custom Scan (CsdScan)' "${OUTPUT_DIR}/cold.txt" || {
    echo "Cold-cache AUTO did not choose the calibrated CSD path." >&2
    exit 1
}
if grep -q 'Custom Scan (CsdScan)' "${OUTPUT_DIR}/hot.txt"; then
    echo "Hot-cache AUTO did not switch back to HOST." >&2
    exit 1
fi

mixed_settings="SET csd.host_cache_fraction=0; SET csd.flash_page_cost=0.01;"
mixed_settings+=" SET csd.tuple_cost=0.0001; SET csd.aggregate_tuple_cost=0.0001;"
mixed_settings+=" SET csd.sort_compare_cost=100; SET csd.result_byte_cost=0.0000015;"
run_explain mixed "${mixed_settings}" \
    "SELECT l_returnflag, l_linestatus, sum(l_extendedprice) AS revenue FROM lineitem GROUP BY l_returnflag, l_linestatus ORDER BY revenue"
grep -q '^Sort ' "${OUTPUT_DIR}/mixed.txt" || {
    echo "Mixed AUTO plan does not retain the HOST Sort above the cut." >&2
    exit 1
}
grep -q 'Custom Scan (CsdScan)' "${OUTPUT_DIR}/mixed.txt" || {
    echo "Mixed AUTO plan has no CSD Aggregate/Scan subtree." >&2
    exit 1
}

# A CSD subtree can remain available as an internal DP state even when its
# materialized result cannot fit in the FDM output range.  It must not be
# selected as a HOST boundary until a later CSD operator reduces the result.
capacity_settings="SET csd.host_cache_fraction=0; SET csd.output_mb=1;"
capacity_settings+=" SET csd.setup_cost=0; SET csd.flash_page_cost=0;"
capacity_settings+=" SET csd.tuple_cost=0; SET csd.result_byte_cost=0;"
run_explain output_capacity "${capacity_settings}" "SELECT * FROM lineitem"
if grep -q 'Custom Scan (CsdScan)' "${OUTPUT_DIR}/output_capacity.txt"; then
    echo "AUTO selected a CSD->HOST boundary larger than csd.output_mb." >&2
    exit 1
fi

"${psql_base[@]}" \
    -c "LOAD 'csd_pg'; SET csd.exe_path='auto'; SET csd.host_cache_fraction=0; SET csd.output_mb=1024;" \
    -f "${ROOT}/scripts/gen_data/queries/2.explain.sql" \
    >"${OUTPUT_DIR}/subplan_boundary.txt"
grep -q 'Custom Scan (CsdScan)' "${OUTPUT_DIR}/subplan_boundary.txt" || {
    echo "SubPlan boundary test did not retain any safe CSD child." >&2
    exit 1
}
if grep -Eq 'CSD Pending Expressions: [1-9]' \
    "${OUTPUT_DIR}/subplan_boundary.txt"; then
    echo "AUTO exposed a CSD boundary with an unevaluated SubPlan predicate." >&2
    exit 1
fi

"${psql_base[@]}" \
    -c "LOAD 'csd_pg'; SET csd.exe_path='auto'; SET csd.host_cache_fraction=1;" \
    -f "${ROOT}/scripts/gen_data/queries/21.explain.sql" \
    >"${OUTPUT_DIR}/selectivity_boundary.txt"
if grep -Eq 'CSD Operator: .* (Semi|Anti) Join' \
    "${OUTPUT_DIR}/selectivity_boundary.txt"; then
    echo "AUTO cut below a SEMI/ANTI join with a path-specific row estimate." >&2
    exit 1
fi

# Q2 and Q16 previously selected a cheap-looking Scan boundary whose actual
# result disabled PostgreSQL's faster parallel HOST plan.  Even with an
# explicitly cold HOST cache, the safe output-capacity bound must now retain
# the all-HOST alternative instead of admitting that partial-offload cut.
for query in 2 16; do
    output="${OUTPUT_DIR}/q${query}_cold_default.txt"

    "${psql_base[@]}" \
        -c "LOAD 'csd_pg'; SET csd.exe_path='auto'; SET csd.host_cache_fraction=0;" \
        -f "${ROOT}/scripts/gen_data/queries/${query}.explain.sql" >"${output}"
    if grep -q 'Custom Scan (CsdScan)' "${output}"; then
        echo "Cold-cache AUTO still partially offloads TPC-H Q${query}." >&2
        exit 1
    fi
done

echo "PASS cold=CSD hot=HOST mixed=CSD->HOST oversized-boundary=HOST subplan-boundary=safe selectivity-boundary=safe q2=HOST q16=HOST"
echo "Raw plans: ${OUTPUT_DIR}"
