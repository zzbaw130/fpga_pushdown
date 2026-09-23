#!/usr/bin/env bash
# Run a short read-only smoke test against the cluster staged on CEMU NVMe.
set -euo pipefail

SF="${1:-1}"
[[ "${SF}" =~ ^[1-9][0-9]*$ ]] || { echo "Usage: $0 [scale-factor]" >&2; exit 2; }

PGDATA="${TPCH_NVME_PGDATA:-/mnt/nvme0/pgdata/tpch_s${SF}}"
PG_PREFIX="${PG_GUEST_PREFIX:-/opt/pgsql12}"
PG_BIN="${PG_PREFIX}/bin"
PG_LIB="${PG_PREFIX}/lib"
DB_NAME="tpch_s${SF}"
DB_USER="${TPCH_DB_USER:-dengyao}"
SOCKET_DIR="${TPCH_NVME_SOCKET:-/tmp/csdpg_nvme_tpch_s${SF}}"
PORT="${TPCH_NVME_PORT:-57432}"

[[ "$(id -un)" != root ]] || { echo "Run this script as csdpg, never root." >&2; exit 1; }
[[ -x "${PG_BIN}/pg_ctl" && -f "${PGDATA}/PG_VERSION" ]] || {
    echo "Missing guest PostgreSQL or staged NVMe PGDATA." >&2; exit 1;
}
[[ ! -e "${PGDATA}/postmaster.pid" ]] || {
    echo "${PGDATA} appears active." >&2; exit 1;
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
POSTMASTER_PID="$(head -n 1 "${PGDATA}/postmaster.pid")"
CAP_EFFECTIVE="$(awk '$1 == "CapEff:" { print $2 }' "/proc/${POSTMASTER_PID}/status")"
if (( (16#${CAP_EFFECTIVE} & (1 << 21)) == 0 )); then
    echo "PostgreSQL postmaster is missing effective CAP_SYS_ADMIN." >&2
    exit 1
fi
echo "PostgreSQL CAP_SYS_ADMIN active: pid=${POSTMASTER_PID}, CapEff=${CAP_EFFECTIVE}"
"${PG_BIN}/psql" -v ON_ERROR_STOP=1 -U "${DB_USER}" -h "${SOCKET_DIR}" -p "${PORT}" -d "${DB_NAME}" <<'SQL'
SELECT count(*) AS lineitem_rows FROM lineitem;
SELECT count(*) AS orders_in_1995
FROM orders
WHERE o_orderdate >= DATE '1995-01-01' AND o_orderdate < DATE '1996-01-01';
SQL
