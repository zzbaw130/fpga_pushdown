#!/usr/bin/env bash
# Prove that completed host-cache reads occur while CSD Program Execute is
# still outstanding. Run as root inside the CEMU guest.
set -euo pipefail

[[ "$(id -u)" == 0 ]] || { echo "Run as root inside the CEMU guest." >&2; exit 1; }

PGDATA="${CSD_PGDATA:-/mnt/nvme0/pgdata/tpch_s1}"
PG_PREFIX="${CSD_PG_PREFIX:-/opt/pgsql12}"
PG_OS_USER="${CSD_PG_OS_USER:-csdpg}"
DB_USER="${CSD_DB_USER:-dengyao}"
DATABASE="${CSD_DATABASE:-tpch_s1}"
SOCKET_DIR="${CSD_SOCKET_DIR:-/tmp/csdpg_nvme_tpch_s1}"
PORT="${CSD_PORT:-57432}"
CSD_DEVICE="${CSD_CONTROL_DEVICE:-/dev/nvme0c3}"
CSD_LIBRARY="${CSD_SHARED_LIBRARY:-/workspace/code/CEMU/tests/cemu/build/csd_sql_overlap_v1.so}"
CSD_LIBRARY_CHECK="${CSD_LIBRARY}"
CACHE_WAIT_MS="${CSD_CACHE_WAIT_MS:-180000}"
EXPECTED_ROWS="${CSD_EXPECTED_LINEITEM_ROWS:-6001215}"
OUTPUT_DIR="${CSD_OVERLAP_OUTPUT_DIR:-/tmp/csd_cache_overlap}"
STATS_HELPER="${CSD_CACHE_STATS_HELPER:-/root/CEMU/scripts/csd_cache_stats.py}"
PG_CTL="${PG_PREFIX}/bin/pg_ctl"
PSQL="${PG_PREFIX}/bin/psql"
CSV="${OUTPUT_DIR}/overlap.csv"

[[ -f "${PGDATA}/PG_VERSION" ]] || { echo "Missing PGDATA: ${PGDATA}" >&2; exit 1; }
[[ -x "${PG_CTL}" && -x "${PSQL}" ]] || { echo "Missing guest PostgreSQL." >&2; exit 1; }
[[ -c "${CSD_DEVICE}" ]] || { echo "Missing CEMU control device: ${CSD_DEVICE}" >&2; exit 1; }
if [[ ! -f "${CSD_LIBRARY_CHECK}" && "${CSD_LIBRARY}" == /workspace/* ]]; then
    # Program Execute resolves /workspace in the CEMU container, while the
    # same 9P repository is mounted at /root/CEMU inside the guest.
    CSD_LIBRARY_CHECK="/root/CEMU/${CSD_LIBRARY#/workspace/}"
fi
[[ -f "${CSD_LIBRARY_CHECK}" ]] || {
    echo "Missing CSD kernel: ${CSD_LIBRARY} (guest path: ${CSD_LIBRARY_CHECK})" >&2
    exit 1
}
[[ -x "${STATS_HELPER}" ]] || { echo "Missing stats helper: ${STATS_HELPER}" >&2; exit 1; }

mkdir -p "${OUTPUT_DIR}"
printf 'cache_overlap,query_ms,execute_ms,prefetch_start_ms,prefetched_bytes,overlap_prefetched_bytes,events,final_events,errors\n' >"${CSV}"

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

trap 'server_running || start_server || true' EXIT

psql_base=("${PSQL}" -X -v ON_ERROR_STOP=1 -h "${SOCKET_DIR}"
           -p "${PORT}" -U "${DB_USER}" -d "${DATABASE}")

run_mode() {
    local mode="$1"
    local stats_file="${OUTPUT_DIR}/${mode}.json"
    local stderr_file="${OUTPUT_DIR}/${mode}.stderr"
    local start_ns end_ns count query_ms values

    cold_start_server
    start_ns="$(date +%s%N)"
    count="$("${psql_base[@]}" -Atq \
        -c "SET default_transaction_read_only=off; LOAD 'csd_pg'; SET csd.shared_library='${CSD_LIBRARY}'; SET csd.exe_path='csd'; SET csd.cache_fill=on; SET csd.cache_overlap=${mode}; SET csd.cache_wait_ms=${CACHE_WAIT_MS}; SELECT count(*) FROM lineitem;" \
        2>"${stderr_file}")"
    end_ns="$(date +%s%N)"
    [[ "${count}" == "${EXPECTED_ROWS}" ]] || {
        echo "Unexpected CSD count in ${mode} mode: ${count}" >&2
        exit 1
    }
    query_ms="$(( (end_ns - start_ns) / 1000000 ))"
    "${STATS_HELPER}" --device "${CSD_DEVICE}" >"${stats_file}"
    values="$(python3 - "${stats_file}" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as source:
    stats = json.load(source)
assert stats["active_generation"] == stats["completed_generation"] != 0, stats
assert stats["final_events"] >= 1, stats
assert stats["execute_complete_ns"] > stats["execute_start_ns"] > 0, stats
assert stats["prefetched_bytes"] > 0, stats
assert stats["errors"] == 0, stats
execute_ms = (stats["execute_complete_ns"] - stats["execute_start_ns"]) // 1_000_000
prefetch_start_ms = (stats["first_prefetch_ns"] - stats["execute_start_ns"]) // 1_000_000
print(execute_ms, prefetch_start_ms,
      stats["prefetched_bytes"], stats["overlap_prefetched_bytes"],
      stats["events"], stats["final_events"], stats["errors"])
PY
)"
    read -r execute_ms prefetch_start_ms prefetched overlap events finals errors <<<"${values}"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "${mode}" "${query_ms}" "${execute_ms}" "${prefetch_start_ms}" \
        "${prefetched}" "${overlap}" \
        "${events}" "${finals}" "${errors}" >>"${CSV}"
    printf 'cache_overlap=%s query_ms=%s execute_ms=%s prefetch_start_ms=%s prefetched_bytes=%s overlap_prefetched_bytes=%s\n' \
        "${mode}" "${query_ms}" "${execute_ms}" "${prefetch_start_ms}" \
        "${prefetched}" "${overlap}"
}

run_mode off
run_mode on

python3 - "${CSV}" <<'PY'
import csv, sys
with open(sys.argv[1], newline="", encoding="utf-8") as source:
    rows = {row["cache_overlap"]: row for row in csv.DictReader(source)}
off = int(rows["off"]["overlap_prefetched_bytes"])
on = int(rows["on"]["overlap_prefetched_bytes"])
off_execute = int(rows["off"]["execute_ms"])
on_execute = int(rows["on"]["execute_ms"])
off_prefetch = int(rows["off"]["prefetch_start_ms"])
on_prefetch = int(rows["on"]["prefetch_start_ms"])
if on <= 0 or on <= off:
    raise SystemExit(f"no measurable cache/compute overlap improvement: off={off}, on={on}")
if on_prefetch >= on_execute:
    raise SystemExit(f"overlap mode did not start cache fill during execute: start={on_prefetch}ms execute={on_execute}ms")
if off_prefetch < off_execute:
    raise SystemExit(f"non-overlap mode unexpectedly started cache fill during execute: start={off_prefetch}ms execute={off_execute}ms")
print(f"PASS overlap_prefetched_bytes off={off} on={on}")
PY

echo "Raw results: ${OUTPUT_DIR}"
