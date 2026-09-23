#!/usr/bin/env bash
# Compatibility entry point. TPC-H validation uses the PGDATA copy staged on
# CEMU's NVMe namespace, not the 9p-mounted source cluster.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${script_dir}/run_nvme_tpch.sh" "$@"
