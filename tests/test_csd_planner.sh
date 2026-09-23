#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SF=1
MODE=csd
SOCKET_DIR=""
PORT=""
OUTPUT_DIR=""
SKIP_BUILD=0

usage() {
    cat <<'EOF'
Usage: tests/test_csd_planner.sh [options]
  --sf N               TPC-H scale factor/database suffix (default: 1)
  --mode host|csd|auto csd.exe_path value (default: csd)
  --socket-dir PATH    PostgreSQL Unix socket directory
  --port PORT          PostgreSQL port (default: 55432 + SF)
  --output-dir PATH    EXPLAIN output directory
  --skip-build         Do not rebuild/install csd_pg
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sf) SF="$2"; shift 2 ;;
        --mode) MODE="$2"; shift 2 ;;
        --socket-dir) SOCKET_DIR="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "${MODE}" in
    host|csd|auto) ;;
    *) echo "Invalid --mode: ${MODE}" >&2; exit 2 ;;
esac
[[ "${SF}" =~ ^[0-9]+$ ]] || { echo "--sf must be an integer" >&2; exit 2; }

PG_CONFIG="${PG_CONFIG:-${ROOT}/build/install/bin/pg_config}"
PSQL="$(dirname "${PG_CONFIG}")/psql"
SOCKET_DIR="${SOCKET_DIR:-${ROOT}/.pgsock_tpch_s${SF}}"
PORT="${PORT:-$((55432 + SF))}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT}/results/csd_planner_sf${SF}/${MODE}}"
DATABASE="tpch_s${SF}"

[[ -x "${PG_CONFIG}" ]] || { echo "Missing pg_config: ${PG_CONFIG}" >&2; exit 1; }
[[ -x "${PSQL}" ]] || { echo "Missing psql: ${PSQL}" >&2; exit 1; }

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
    make -C "${ROOT}/code/postgres-csd" clean
    make -C "${ROOT}/code/postgres-csd" PG_CONFIG="${PG_CONFIG}"
    make -C "${ROOT}/code/postgres-csd" PG_CONFIG="${PG_CONFIG}" install
fi

mkdir -p "${OUTPUT_DIR}"
failures=0
pending_expressions=0
parameter_relocations=0

for query in $(seq 1 22); do
    sql="${ROOT}/scripts/gen_data/queries/${query}.explain.sql"
    output="${OUTPUT_DIR}/q${query}.txt"

    if ! "${PSQL}" -X --single-transaction -v ON_ERROR_STOP=1 \
        -h "${SOCKET_DIR}" -p "${PORT}" -d "${DATABASE}" \
        -c "LOAD 'csd_pg'; SET csd.exe_path='${MODE}';" \
        -f "${sql}" >"${output}" 2>&1; then
        printf 'Q%02d FAIL (psql)\n' "${query}"
        tail -n 6 "${output}"
        failures=$((failures + 1))
        continue
    fi

    if [[ "${MODE}" == csd ]]; then
        if ! grep -q 'Custom Scan (CsdScan)' "${output}"; then
            printf 'Q%02d FAIL (no CsdScan)\n' "${query}"
            failures=$((failures + 1))
            continue
        fi
        csd_scans="$(grep -c 'Custom Scan (CsdScan)' "${output}")"
        wire_plans="$(grep -c 'CSD Wire Status: ABI v4 scan/expression/join/aggregate/sort descriptors' "${output}" || true)"
        if [[ "${wire_plans}" -ne "${csd_scans}" ]]; then
            printf 'Q%02d FAIL (missing ABI v4 wire plan: scans=%s wire=%s)\n' \
                "${query}" "${csd_scans}" "${wire_plans}"
            failures=$((failures + 1))
            continue
        fi
        host_nodes="$(grep '(cost=' "${output}" | \
            grep -Ev 'Custom Scan \(CsdScan\)|^[[:space:]]*Limit ' || true)"
        if [[ -n "${host_nodes}" ]]; then
            printf 'Q%02d FAIL (host compute node remains)\n%s\n' \
                "${query}" "${host_nodes}"
            failures=$((failures + 1))
            continue
        fi
        while IFS= read -r pending; do
            pending_expressions=$((pending_expressions + pending))
        done < <(sed -n 's/.*CSD Pending Expressions: \([0-9][0-9]*\).*/\1/p' "${output}")
        while IFS= read -r relocations; do
            parameter_relocations=$((parameter_relocations + relocations))
        done < <(sed -n 's/.*CSD Parameter Relocations: \([0-9][0-9]*\).*/\1/p' "${output}")
    elif [[ "${MODE}" == host ]] && grep -q 'Custom Scan (CsdScan)' "${output}"; then
        printf 'Q%02d FAIL (CsdScan present in host mode)\n' "${query}"
        failures=$((failures + 1))
        continue
    fi

    printf 'Q%02d PASS\n' "${query}"
done

if [[ "${failures}" -ne 0 ]]; then
    echo "${failures} planner test(s) failed" >&2
    exit 1
fi

echo "All 22 TPC-H planner tests passed in ${MODE} mode."
if [[ "${MODE}" == csd ]]; then
    echo "Pending non-scalar expressions: ${pending_expressions}"
    echo "Executor parameter relocations: ${parameter_relocations}"
fi
