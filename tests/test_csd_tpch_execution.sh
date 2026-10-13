#!/usr/bin/env bash
# Execute all supplied TPC-H queries through CEMU and PostgreSQL, then compare
# their result text exactly.  Run this inside the CEMU guest against an already
# started guest-built PostgreSQL instance and NVMe-staged PGDATA.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SF=1
MODE=csd
SOCKET_DIR="/tmp/csdpg_nvme_tpch_s1"
PORT=57432
DATABASE=""
DB_USER="dengyao"
PSQL="${PSQL:-/opt/pgsql12/bin/psql}"
SHARED_LIBRARY="/workspace/code/CEMU/tests/cemu/build/csd_sql.so"
OUTPUT_DIR=""
HOST_GOLDEN_DIR=""
PROFILE="off"
CACHE_FILL="on"
EXECUTE_TIMEOUT_MS=600000
SPILL_MB=""
QUERIES="$(seq 1 22)"

usage() {
    cat <<'EOF'
Usage: tests/test_csd_tpch_execution.sh [options]
  --sf N                    database suffix/scale factor (default: 1)
  --mode csd|auto           execution path compared with HOST (default: csd)
  --socket-dir PATH         running PostgreSQL socket directory
  --port PORT               running PostgreSQL port (default: 57432)
  --database NAME           database name (default: tpch_s<SF>)
  --user NAME               database role (default: dengyao)
  --psql PATH               guest-built psql (default: /opt/pgsql12/bin/psql)
  --shared-library PATH     CEMU-visible csd_sql.so path
  --output-dir PATH         raw/normalized results (default: /tmp)
  --host-golden-dir PATH    compare with pre-generated normalized HOST results
  --profile                  enable per-node CSD diagnostics
  --cache-fill on|off        enable AER cache fill (default: on)
  --execute-timeout-ms N     Program Execute watchdog (default: 600000)
  --spill-mb N               query-local NVMe Flash spill capacity override
  --queries "N ..."           selected queries (default: 1 through 22)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sf) SF="$2"; shift 2 ;;
        --mode) MODE="$2"; shift 2 ;;
        --socket-dir) SOCKET_DIR="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --database) DATABASE="$2"; shift 2 ;;
        --user) DB_USER="$2"; shift 2 ;;
        --psql) PSQL="$2"; shift 2 ;;
        --shared-library) SHARED_LIBRARY="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --host-golden-dir) HOST_GOLDEN_DIR="$2"; shift 2 ;;
        --profile) PROFILE="on"; shift ;;
        --cache-fill) CACHE_FILL="$2"; shift 2 ;;
        --execute-timeout-ms) EXECUTE_TIMEOUT_MS="$2"; shift 2 ;;
        --spill-mb) SPILL_MB="$2"; shift 2 ;;
        --queries) QUERIES="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ "${SF}" =~ ^[1-9][0-9]*$ ]] || { echo "--sf must be positive" >&2; exit 2; }
[[ "${MODE}" == csd || "${MODE}" == auto ]] || {
    echo "--mode must be csd or auto" >&2; exit 2;
}
[[ "${PORT}" =~ ^[1-9][0-9]*$ ]] || { echo "--port must be positive" >&2; exit 2; }
[[ "${CACHE_FILL}" == on || "${CACHE_FILL}" == off ]] || {
    echo "--cache-fill must be on or off" >&2; exit 2;
}
[[ "${EXECUTE_TIMEOUT_MS}" =~ ^[1-9][0-9]*$ && "${EXECUTE_TIMEOUT_MS}" -ge 1000 ]] || {
    echo "--execute-timeout-ms must be at least 1000" >&2; exit 2;
}
[[ -z "${SPILL_MB}" || "${SPILL_MB}" =~ ^[1-9][0-9]*$ ]] || {
    echo "--spill-mb must be positive" >&2; exit 2;
}
for query in ${QUERIES}; do
    [[ "${query}" =~ ^([1-9]|1[0-9]|2[0-2])$ ]] || {
        echo "--queries accepts only query numbers 1 through 22" >&2
        exit 2
    }
done
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
spill_setting=""
[[ -z "${SPILL_MB}" ]] || spill_setting="SET csd.spill_mb=${SPILL_MB};"

normalize() {
    sed -e '/^SET$/d' \
        -e '/^Timing is on\.$/d' \
        -e '/^Time:/d' \
        -e '/NOTICE:.*CSD profile/d' \
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
for query in ${QUERIES}; do
    sql="${QUERY_DIR}/${query}.sql"
    csd_raw="${OUTPUT_DIR}/q${query}.csd.raw"
    host_raw="${OUTPUT_DIR}/q${query}.host.raw"
    csd_result="${OUTPUT_DIR}/q${query}.csd.txt"
    host_result="${OUTPUT_DIR}/q${query}.host.txt"

    [[ "${query}" -ne 15 ]] || cleanup_q15_view
    if ! "${psql_base[@]}" \
        -c 'SET default_transaction_read_only=off' \
        -c "LOAD 'csd_pg'; SET csd.shared_library='${SHARED_LIBRARY}'; SET csd.exe_path='${MODE}'; SET csd.profile='${PROFILE}'; SET csd.cache_fill='${CACHE_FILL}'; SET csd.execute_timeout_ms=${EXECUTE_TIMEOUT_MS}; ${spill_setting}" \
        -f "${sql}" >"${csd_raw}" 2>&1; then
        printf 'Q%02d %s FAIL\n' "${query}" "${MODE^^}"
        tail -n 12 "${csd_raw}"
        failures=$((failures + 1))
        [[ "${query}" -ne 15 ]] || cleanup_q15_view
        continue
    fi

    if [[ -n "${HOST_GOLDEN_DIR}" ]]; then
        golden="${HOST_GOLDEN_DIR}/q${query}.host.txt"
        if [[ ! -f "${golden}" ]]; then
            printf 'Q%02d HOST GOLDEN MISSING\n' "${query}"
            failures=$((failures + 1))
            [[ "${query}" -ne 15 ]] || cleanup_q15_view
            continue
        fi
        cp -- "${golden}" "${host_result}"
    else
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
        normalize "${host_raw}" "${host_result}"
    fi
    [[ "${query}" -ne 15 ]] || cleanup_q15_view

    normalize "${csd_raw}" "${csd_result}"
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
echo "All selected TPC-H queries matched HOST results in ${MODE} mode."
