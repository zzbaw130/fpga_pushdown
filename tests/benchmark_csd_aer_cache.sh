#!/usr/bin/env bash
# Compare a cold HOST scan after the same forced CSD scan with AER cache
# backfill disabled and enabled.  Run as root inside the CEMU guest.
set -euo pipefail

ROUNDS="${1:-3}"
[[ "${ROUNDS}" =~ ^[1-9][0-9]*$ ]] || {
    echo "Usage: $0 [positive-round-count]" >&2
    exit 2
}
[[ "$(id -u)" == 0 ]] || { echo "Run as root inside the CEMU guest." >&2; exit 1; }

PGDATA="${CSD_PGDATA:-/mnt/nvme0/pgdata/tpch_s1}"
PG_PREFIX="${CSD_PG_PREFIX:-/opt/pgsql12}"
PG_OS_USER="${CSD_PG_OS_USER:-csdpg}"
DB_USER="${CSD_DB_USER:-dengyao}"
DATABASE="${CSD_DATABASE:-tpch_s1}"
SOCKET_DIR="${CSD_SOCKET_DIR:-/tmp/csdpg_nvme_tpch_s1}"
PORT="${CSD_PORT:-57432}"
CSD_DEVICE="${CSD_CONTROL_DEVICE:-/dev/nvme0c3}"
CSD_LIBRARY="${CSD_SHARED_LIBRARY:-/workspace/code/CEMU/tests/cemu/build/csd_sql_aer_v1.so}"
CACHE_WAIT_MS="${CSD_CACHE_WAIT_MS:-180000}"
EXPECTED_ROWS="${CSD_EXPECTED_LINEITEM_ROWS:-6001215}"
OUTPUT_DIR="${CSD_AER_OUTPUT_DIR:-/tmp/csd_aer_cache}"
STATS_HELPER="${CSD_CACHE_STATS_HELPER:-/root/CEMU/scripts/csd_cache_stats.py}"
PG_CTL="${PG_PREFIX}/bin/pg_ctl"
PSQL="${PG_PREFIX}/bin/psql"
CSV="${OUTPUT_DIR}/host_scan_ms.csv"

[[ -f "${PGDATA}/PG_VERSION" ]] || { echo "Missing PGDATA: ${PGDATA}" >&2; exit 1; }
[[ -x "${PG_CTL}" && -x "${PSQL}" ]] || { echo "Missing guest PostgreSQL." >&2; exit 1; }
[[ -c "${CSD_DEVICE}" ]] || { echo "Missing CEMU control device: ${CSD_DEVICE}" >&2; exit 1; }
[[ -x "${STATS_HELPER}" ]] || { echo "Missing stats helper: ${STATS_HELPER}" >&2; exit 1; }

mkdir -p "${OUTPUT_DIR}"
printf 'round,cache_fill,host_execution_ms\n' >"${CSV}"

server_running() {
    runuser -u "${PG_OS_USER}" -- "${PG_CTL}" -D "${PGDATA}" status >/dev/null 2>&1
}

stop_server() {
    if server_running; then
        (cd /tmp && runuser -u "${PG_OS_USER}" -- \
            "${PG_CTL}" -D "${PGDATA}" -m fast -w stop >/dev/null)
    fi
}

start_server() {
    install -d -o "${PG_OS_USER}" -g "${PG_OS_USER}" "${SOCKET_DIR}"
    local options
    options="-c port=${PORT} -c listen_addresses=''"
    options+=" -c unix_socket_directories=${SOCKET_DIR}"
    options+=" -c autovacuum=off -c default_transaction_read_only=on"
    (cd /tmp && runuser -u "${PG_OS_USER}" -- \
        "${PG_CTL}" -D "${PGDATA}" -l "${SOCKET_DIR}/postgres.log" \
        -o "${options}" -w start >/dev/null)
}

cold_start_server() {
    stop_server
    sync
    echo 3 >/proc/sys/vm/drop_caches
    start_server
}

ensure_server_on_exit() {
    if ! server_running; then
        start_server || true
    fi
}
trap ensure_server_on_exit EXIT

psql_base=("${PSQL}" -X -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}"
           -p "${PORT}" -U "${DB_USER}" -d "${DATABASE}")

run_sample() {
    local round="$1"
    local cache_fill="$2"
    local wait_ms=0
    local sample_dir="${OUTPUT_DIR}/round_${round}_${cache_fill}"
    local count host_ms

    [[ "${cache_fill}" == on ]] && wait_ms="${CACHE_WAIT_MS}"
    mkdir -p "${sample_dir}"
    cold_start_server

    count="$("${psql_base[@]}" -Atq \
        -c "SET default_transaction_read_only=off; LOAD 'csd_pg'; SET csd.shared_library='${CSD_LIBRARY}'; SET csd.exe_path='csd'; SET csd.cache_fill=${cache_fill}; SET csd.cache_wait_ms=${wait_ms}; SELECT count(*) FROM lineitem;" \
        2>"${sample_dir}/csd.stderr")"
    [[ "${count}" == "${EXPECTED_ROWS}" ]] || {
        echo "Unexpected CSD count in round ${round}/${cache_fill}: ${count}" >&2
        exit 1
    }

    "${psql_base[@]}" \
        -c "LOAD 'csd_pg'; SET csd.exe_path='host'; EXPLAIN (ANALYZE, BUFFERS, TIMING OFF, SUMMARY ON) SELECT count(*) FROM lineitem;" \
        >"${sample_dir}/host_explain.txt"
    host_ms="$(awk '/Execution Time:/ { print $3 }' "${sample_dir}/host_explain.txt")"
    [[ "${host_ms}" =~ ^[0-9]+([.][0-9]+)?$ ]] || {
        echo "Could not parse HOST execution time in round ${round}/${cache_fill}." >&2
        exit 1
    }
    printf '%s,%s,%s\n' "${round}" "${cache_fill}" "${host_ms}" >>"${CSV}"
    if [[ "${cache_fill}" == on ]]; then
        "${STATS_HELPER}" --device "${CSD_DEVICE}" >"${sample_dir}/cache_stats.json"
    fi
    printf 'round=%s cache_fill=%s host_ms=%s\n' "${round}" "${cache_fill}" "${host_ms}"
}

for round in $(seq 1 "${ROUNDS}"); do
    run_sample "${round}" off
    run_sample "${round}" on
done

awk -F, '
    NR == 1 { next }
    $2 == "off" { off += $3; off_n++ }
    $2 == "on"  { on += $3; on_n++ }
    END {
        if (!off_n || !on_n) exit 1;
        off_mean = off / off_n;
        on_mean = on / on_n;
        printf "mean_off_ms=%.3f mean_on_ms=%.3f speedup=%.4fx reduction=%.2f%%\n", \
               off_mean, on_mean, off_mean / on_mean, (1 - on_mean / off_mean) * 100;
    }
' "${CSV}"
echo "Raw results: ${OUTPUT_DIR}"
