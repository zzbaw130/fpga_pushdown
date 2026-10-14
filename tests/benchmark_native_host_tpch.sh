#!/usr/bin/env bash
# Native PostgreSQL baseline for comparison with the CEMU device-time model.
# This is a cross-platform projection, not a same-hardware speedup measurement.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SF=16
QUERIES="$(seq 1 22)"
PGDATA=""
SOCKET_DIR=""
PORT=""
DATABASE=""
DB_USER="${PGUSER:-${USER:-$(id -un 2>/dev/null || true)}}"
OUTPUT_DIR=""
GOLDEN_DIR=""
PG_CTL="${ROOT}/build/install/bin/pg_ctl"
PSQL="${ROOT}/build/install/bin/psql"
VMTOUCH="$(command -v vmtouch || true)"
ALLOW_RUNNING_RESTART=0
PG_OPTIONS="-c autovacuum=off"

usage() {
    cat <<'EOF'
Usage: tests/benchmark_native_host_tpch.sh [options]
  --sf N                 scale factor (default: 16)
  --queries "N ..."      TPC-H query numbers (default: 1 through 22)
  --pgdata PATH          native PostgreSQL cluster (default: data/tpch_s<SF>)
  --socket-dir PATH      native PostgreSQL socket directory
  --port PORT            native PostgreSQL port (default: cluster config)
  --database NAME        database name (default: tpch_s<SF>)
  --user NAME            PostgreSQL role
  --output-dir PATH      raw output and CSV directory
  --golden-dir PATH      existing normalized HOST answers for exact comparison
  --pg-ctl PATH          native pg_ctl
  --psql PATH            native psql
  --vmtouch PATH         scoped page-cache eviction utility
  --pg-options STRING    pg_ctl server options (default: autovacuum=off)
  --allow-running-restart
                        permit restarting a cluster already running at entry
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sf) SF="$2"; shift 2 ;;
        --queries) QUERIES="$2"; shift 2 ;;
        --pgdata) PGDATA="$2"; shift 2 ;;
        --socket-dir) SOCKET_DIR="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --database) DATABASE="$2"; shift 2 ;;
        --user) DB_USER="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --golden-dir) GOLDEN_DIR="$2"; shift 2 ;;
        --pg-ctl) PG_CTL="$2"; shift 2 ;;
        --psql) PSQL="$2"; shift 2 ;;
        --vmtouch) VMTOUCH="$2"; shift 2 ;;
        --pg-options) PG_OPTIONS="$2"; shift 2 ;;
        --allow-running-restart) ALLOW_RUNNING_RESTART=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ "$SF" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid SF" >&2; exit 2; }
[[ -n "$DB_USER" ]] || { echo "Use --user to name the PostgreSQL role" >&2; exit 2; }
PGDATA="${PGDATA:-${ROOT}/data/tpch_s${SF}}"
SOCKET_DIR="${SOCKET_DIR:-${ROOT}/.pgsock_tpch_s${SF}}"
DATABASE="${DATABASE:-tpch_s${SF}}"
OUTPUT_DIR="${OUTPUT_DIR:-${ROOT}/results/tpch_sf${SF}_native_host_cold}"
GOLDEN_DIR="${GOLDEN_DIR:-${ROOT}/results/tpch_sf${SF}_host_golden}"
[[ -x "$PG_CTL" && -x "$PSQL" && -x "$VMTOUCH" ]] || {
    echo "Missing pg_ctl, psql, or vmtouch executable" >&2; exit 1;
}
[[ -d "$PGDATA/base" && -d "$SOCKET_DIR" ]] || {
    echo "Missing native PGDATA or socket directory" >&2; exit 1;
}
for query in $QUERIES; do
    [[ "$query" =~ ^([1-9]|1[0-9]|2[0-2])$ ]] || {
        echo "Invalid query number: $query" >&2; exit 2;
    }
done
if [[ -n "$PORT" ]]; then
    [[ "$PORT" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid port" >&2; exit 2; }
fi
mkdir -p "$OUTPUT_DIR"

started_here=0
cleanup() {
    if (( started_here )); then
        "$PG_CTL" -D "$PGDATA" -m fast -w stop >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT
if "$PG_CTL" -D "$PGDATA" status >/dev/null 2>&1; then
    if (( ! ALLOW_RUNNING_RESTART )); then
        echo "Refusing to restart an already-running PostgreSQL cluster" >&2
        echo "Pass --allow-running-restart if it is dedicated to this benchmark" >&2
        exit 2
    fi
else
    "$PG_CTL" -D "$PGDATA" -l "$OUTPUT_DIR/postgres.log" \
        -o "$PG_OPTIONS" -w start
    started_here=1
fi

psql_base=("$PSQL" -X -q -v ON_ERROR_STOP=1 -h "$SOCKET_DIR")
[[ -z "$PORT" ]] || psql_base+=(-p "$PORT")
psql_base+=(-U "$DB_USER" -d "$DATABASE")
cleanup_q15() {
    "${psql_base[@]}" -c 'SET default_transaction_read_only=off' \
        -c 'DROP VIEW IF EXISTS revenue0' >/dev/null 2>&1 || true
}
normalize() {
    sed -e '/^SET$/d' -e '/^Timing is on\.$/d' -e '/^Time:/d' \
        -e '/^CREATE VIEW$/d' -e '/^DROP VIEW$/d' -e '/^$/d' "$1" >"$2"
}

{
    printf 'sf=%s pgdata=%s database=%s socket=%s port=%s\n' \
        "$SF" "$PGDATA" "$DATABASE" "$SOCKET_DIR" "${PORT:-cluster-default}"
    printf 'pg_ctl=%s psql=%s vmtouch=%s\n' "$PG_CTL" "$PSQL" "$VMTOUCH"
    printf 'pg_options=%s\n' "$PG_OPTIONS"
    printf 'cache_policy=restart_postgres_then_sync_and_vmtouch_evict_pgdata_base\n'
    printf 'timing=native_psql_query_Time_excludes_restart_and_cache_eviction\n'
    printf 'comparison=cross-platform_native_CPU_vs_CEMU_FPGA_Flash_model_not_real_CSD_hardware\n'
    printf 'queries=%s\n' "$QUERIES"
} >"$OUTPUT_DIR/settings.txt"
printf 'query,rc,native_ms,exact\n' >"$OUTPUT_DIR/timings.csv"

failures=0
for query in $QUERIES; do
    [[ "$query" -ne 15 ]] || cleanup_q15
    "$PG_CTL" -D "$PGDATA" -m fast -o "$PG_OPTIONS" -w restart \
        >"$OUTPUT_DIR/q${query}.restart.log" 2>&1
    # Scope cache eviction to this cluster; never clear the machine-wide cache.
    sync
    "$VMTOUCH" -q -e "$PGDATA/base" \
        >"$OUTPUT_DIR/q${query}.evict.log" 2>&1
    "$VMTOUCH" "$PGDATA/base" \
        >"$OUTPUT_DIR/q${query}.cache-before.log" 2>&1
    raw="$OUTPUT_DIR/q${query}.host.raw"
    normalized="$OUTPUT_DIR/q${query}.host.txt"
    rc=0
    "${psql_base[@]}" -c 'SET default_transaction_read_only=off' \
        -f "$ROOT/scripts/gen_data/queries/$query.sql" >"$raw" 2>&1 || rc=$?
    [[ "$query" -ne 15 ]] || cleanup_q15
    normalize "$raw" "$normalized"
    exact=0
    if [[ -f "$GOLDEN_DIR/q${query}.host.txt" ]]; then
        cmp -s "$normalized" "$GOLDEN_DIR/q${query}.host.txt" || exact=1
    else
        exact=1
    fi
    native_ms="$(awk '/^Time:/ {sum += $2; count++} END {if (count) printf "%.3f", sum; else print "0"}' "$raw")"
    printf '%s,%s,%s,%s\n' "$query" "$rc" "$native_ms" "$exact" \
        >>"$OUTPUT_DIR/timings.csv"
    printf 'Q%02d native=%s ms exact=%s rc=%s\n' \
        "$query" "$native_ms" "$exact" "$rc"
    if (( rc != 0 || exact != 0 )); then failures=$((failures + 1)); fi
done
(( failures == 0 ))
