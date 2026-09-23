#!/usr/bin/env bash
# Run as csdpg.  PostgreSQL was compiled once inside the guest and operates
# directly on the 9p-mounted host PGDATA; no cluster copy is made.
set -euo pipefail

SF="${1:-1}"
[[ "${SF}" =~ ^[1-9][0-9]*$ ]] || { echo "Usage: $0 [scale-factor]" >&2; exit 2; }

PGDATA="${TPCH_SHARED_PGDATA:-/mnt/cemu/data/tpch_s${SF}}"
PG_PREFIX="${PG_GUEST_PREFIX:-/opt/pgsql12}"
PG_BIN="${PG_PREFIX}/bin"
PG_LIB="${PG_PREFIX}/lib"
DB_NAME="tpch_s${SF}"
DB_USER="${TPCH_DB_USER:-dengyao}"
SOCKET_DIR="${TPCH_SHARED_SOCKET:-/tmp/csdpg_tpch_s${SF}}"
PORT="${TPCH_SHARED_PORT:-56432}"

[[ "$(id -un)" != root ]] || { echo "Run this script as csdpg, never root." >&2; exit 1; }
[[ -x "${PG_BIN}/pg_ctl" && -f "${PGDATA}/PG_VERSION" ]] || {
    echo "Missing shared PostgreSQL binary or PGDATA." >&2; exit 1;
}
[[ ! -e "${PGDATA}/postmaster.pid" ]] || {
    echo "${PGDATA} appears active; stop the host instance before direct mounting." >&2; exit 1;
}

mkdir -p "${SOCKET_DIR}"
export LD_LIBRARY_PATH="${PG_LIB}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
OPTIONS="-c port=${PORT} -c listen_addresses='' -c unix_socket_directories=${SOCKET_DIR} -c autovacuum=off -c default_transaction_read_only=on"

cleanup() {
    "${PG_BIN}/pg_ctl" -D "${PGDATA}" -m fast stop >/dev/null 2>&1 || true
    rmdir "${SOCKET_DIR}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

"${PG_BIN}/pg_ctl" -D "${PGDATA}" -o "${OPTIONS}" -w start
"${PG_BIN}/psql" -v ON_ERROR_STOP=1 -U "${DB_USER}" -h "${SOCKET_DIR}" -p "${PORT}" -d "${DB_NAME}" <<'SQL'
SELECT count(*) AS lineitem_rows FROM lineitem;
SELECT count(*) AS orders_in_1995
FROM orders
WHERE o_orderdate >= DATE '1995-01-01' AND o_orderdate < DATE '1996-01-01';
SQL
