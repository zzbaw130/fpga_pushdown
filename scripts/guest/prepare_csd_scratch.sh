#!/usr/bin/env bash
# Provision only the FDMFS files used as CSD program ranges.  Unlike
# femu-scripts/cemu-mount.sh this does not format an NVMe namespace or run a
# data-writing benchmark, so it is safe to invoke on an already configured VM.
set -euo pipefail

SCRATCH_DEVICE="${CSD_SCRATCH_DEVICE:-/dev/nvme0m2}"
SCRATCH_DIR="${CSD_SCRATCH_DIR:-/mnt/fdm0}"
PG_OS_USER="${PG_OS_USER:-csdpg}"
RANGE_COUNT="${CSD_SCRATCH_RANGE_COUNT:-8}"
RANGE_SIZE="${CSD_SCRATCH_RANGE_SIZE:-4M}"

[[ "$(id -u)" -eq 0 ]] || { echo "Run as root." >&2; exit 1; }
[[ -b "${SCRATCH_DEVICE}" ]] || { echo "Missing FDM device: ${SCRATCH_DEVICE}" >&2; exit 1; }
[[ "${RANGE_COUNT}" =~ ^[1-9][0-9]*$ ]] || { echo "Invalid range count." >&2; exit 2; }

install -d -m 0755 "${SCRATCH_DIR}"
if ! mountpoint -q "${SCRATCH_DIR}"; then
    mount -t fdmfs "${SCRATCH_DEVICE}" "${SCRATCH_DIR}"
fi

for ((range = 0; range < RANGE_COUNT; ++range)); do
    range_path="${SCRATCH_DIR}/${range}"
    if [[ ! -e "${range_path}" ]]; then
        install -o "${PG_OS_USER}" -g "${PG_OS_USER}" -m 0660 /dev/null "${range_path}"
        fallocate -l "${RANGE_SIZE}" "${range_path}"
    fi
done

echo "Prepared ${RANGE_COUNT} CSD ranges in ${SCRATCH_DIR}."
