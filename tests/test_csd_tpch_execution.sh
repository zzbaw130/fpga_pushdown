#!/usr/bin/env bash
# Execute all supplied TPC-H queries through CEMU and PostgreSQL, then compare
# their result text exactly.  Run this inside the CEMU guest against an already
# started guest-built PostgreSQL instance and NVMe-staged PGDATA.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SF=1
SOCKET_DIR="/tmp/csdpg_nvme_tpch_s1"
PORT=57432
DATABASE=""
DB_USER="dengyao"
PSQL="${PSQL:-/opt/pgsql12/bin/psql}"
SHARED_LIBRARY="/workspace/code/CEMU/tests/cemu/build/csd_sql.so"
OUTPUT_DIR=""

usage() {
    cat <<'EOF'
Usage: tests/test_csd_tpch_execution.sh [options]
  --sf N                    database suffix/scale factor (default: 1)
  --socket-dir PATH         running PostgreSQL socket directory
  --port PORT               running PostgreSQL port (default: 57432)
  --database NAME           database name (default: tpch_s<SF>)
  --user NAME               database role (default: dengyao)
  --psql PATH               guest-built psql (default: /opt/pgsql12/bin/psql)
  --shared-library PATH     CEMU-visible csd_sql.so path
  --output-dir PATH         raw/normalized results (default: /tmp)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sf) SF="$2"; shift 2 ;;
        --socket-dir) SOCKET_DIR="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --database) DATABASE="$2"; shift 2 ;;
        --user) DB_USER="$2"; shift 2 ;;
        --psql) PSQL="$2"; shift 2 ;;
        --shared-library) SHARED_LIBRARY="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ "${SF}" =~ ^[1-9][0-9]*$ ]] || { echo "--sf must be positive" >&2; exit 2; }
[[ "${PORT}" =~ ^[1-9][0-9]*$ ]] || { echo "--port must be positive" >&2; exit 2; }
[[ "${SHARED_LIBRARY}" != *"'"* ]] || {
    echo "--shared-library cannot contain a single quote" >&2; exit 2;
}
DATABASE="${DATABASE:-tpch_s${SF}}"
OUTPUT_DIR="${OUTPUT_DIR:-/tmp/csd_tpch_execution_sf${SF}}"
QUERY_DIR="${ROOT}/scripts/gen_data/queries"

[[ -x "${PSQL}" ]] || { echo "Missing guest psql: ${PSQL}" >&2; exit 1; }
# This path is resolved by the CEMU process in the container, not by the guest.
[[ -f "${QUERY_DIR}/22.sql" ]] || { echo "Missing TPC-H queries: ${QUERY_DIR}" >&2; exit 1; }
mkdir -p "${OUTPUT_DIR}"

psql_base=("${PSQL}" -X -q -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}" -p "${PORT}"
           -U "${DB_USER}" -d "${DATABASE}")

normalize() {
    sed -e '/^SET$/d' \
        -e '/^Timing is on\.$/d' \
        -e '/^Time:/d' \
        -e '/^CREATE VIEW$/d' \
        -e '/^DROP VIEW$/d' \
        -e '/^$/d' "$1" >"$2"
}

cleanup_q15_view() {
    "${psql_base[@]}" -c 'SET default_transaction_read_only=off' \
        -c 'DROP VIEW IF EXISTS revenue0' \
        >/dev/null 2>&1 || true
}

failures=0
for query in $(seq 1 22); do
    sql="${QUERY_DIR}/${query}.sql"
    csd_raw="${OUTPUT_DIR}/q${query}.csd.raw"
    host_raw="${OUTPUT_DIR}/q${query}.host.raw"
    csd_result="${OUTPUT_DIR}/q${query}.csd.txt"
    host_result="${OUTPUT_DIR}/q${query}.host.txt"

    [[ "${query}" -ne 15 ]] || cleanup_q15_view
    if ! "${psql_base[@]}" \
        -c 'SET default_transaction_read_only=off' \
        -c "LOAD 'csd_pg'; SET csd.shared_library='${SHARED_LIBRARY}'; SET csd.exe_path='csd';" \
        -f "${sql}" >"${csd_raw}" 2>&1; then
        printf 'Q%02d CSD FAIL\n' "${query}"
        tail -n 12 "${csd_raw}"
        failures=$((failures + 1))
        [[ "${query}" -ne 15 ]] || cleanup_q15_view
        continue
    fi

    [[ "${query}" -ne 15 ]] || cleanup_q15_view
    if ! "${psql_base[@]}" -c 'SET default_transaction_read_only=off' \
        -c "LOAD 'csd_pg'; SET csd.exe_path='host';" \
        -f "${sql}" >"${host_raw}" 2>&1; then
        printf 'Q%02d HOST FAIL\n' "${query}"
        tail -n 12 "${host_raw}"
        failures=$((failures + 1))
        [[ "${query}" -ne 15 ]] || cleanup_q15_view
        continue
    fi
    [[ "${query}" -ne 15 ]] || cleanup_q15_view

    normalize "${csd_raw}" "${csd_result}"
    normalize "${host_raw}" "${host_result}"
    if cmp -s "${csd_result}" "${host_result}"; then
        printf 'Q%02d EXACT\n' "${query}"
    else
        printf 'Q%02d DIFFER\n' "${query}"
        diff -u "${host_result}" "${csd_result}" | head -n 40 || true
        failures=$((failures + 1))
    fi
done

if [[ "${failures}" -ne 0 ]]; then
    echo "${failures} TPC-H execution comparison(s) failed" >&2
    exit 1
fi
echo "All 22 TPC-H queries matched HOST results in forced CSD mode."
