#!/usr/bin/env bash
# Compatibility entry point. CSD validation must use shared PGDATA directly;
# copying a cluster into the guest changes the path being measured and is slow.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${script_dir}/run_shared_tpch.sh" "$@"
