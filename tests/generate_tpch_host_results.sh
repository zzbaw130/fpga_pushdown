#!/usr/bin/env bash
# Generate normalized PostgreSQL HOST reference outputs without involving CEMU.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SF=1
SOCKET_DIR="${ROOT}/.pgsock_tpch_s1"
PORT=55433
DATABASE=""
DB_USER="$(id -un)"
PSQL="${PSQL:-${ROOT}/build/install/bin/psql}"
OUTPUT_DIR=""

usage() {
    cat <<'EOF'
Usage: tests/generate_tpch_host_results.sh [options]
  --sf N                 database suffix/scale factor
  --socket-dir PATH      PostgreSQL socket directory
  --port PORT            PostgreSQL port
  --database NAME        database name (default: tpch_s<SF>)
  --user NAME            PostgreSQL role
  --psql PATH            psql binary
  --output-dir PATH      result directory
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
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ "${SF}" =~ ^[1-9][0-9]*$ ]] || { echo "--sf must be positive" >&2; exit 2; }
[[ "${PORT}" =~ ^[1-9][0-9]*$ ]] || { echo "--port must be positive" >&2; exit 2; }
DATABASE="${DATABASE:-tpch_s${SF}}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT}/results/tpch_sf${SF}_host_golden}"
QUERY_DIR="${ROOT}/scripts/gen_data/queries"
mkdir -p "${OUTPUT_DIR}"

psql_base=("${PSQL}" -X -q -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}" -p "${PORT}"
           -U "${DB_USER}" -d "${DATABASE}")

cleanup_q15_view() {
    "${psql_base[@]}" -c 'SET default_transaction_read_only=off' \
        -c 'DROP VIEW IF EXISTS revenue0' >/dev/null 2>&1 || true
}

normalize() {
    sed -e '/^SET$/d' \
        -e '/^Timing is on\.$/d' \
        -e '/^Time:/d' \
        -e '/^CREATE VIEW$/d' \
        -e '/^DROP VIEW$/d' \
        -e '/^$/d' "$1" >"$2"
}

for query in $(seq 1 22); do
    raw="${OUTPUT_DIR}/q${query}.host.raw"
    result="${OUTPUT_DIR}/q${query}.host.txt"
    [[ "${query}" -ne 15 ]] || cleanup_q15_view
    "${psql_base[@]}" -c 'SET default_transaction_read_only=off' \
        -f "${QUERY_DIR}/${query}.sql" >"${raw}" 2>&1
    [[ "${query}" -ne 15 ]] || cleanup_q15_view
    normalize "${raw}" "${result}"
    printf 'Q%02d HOST OK\n' "${query}"
done
