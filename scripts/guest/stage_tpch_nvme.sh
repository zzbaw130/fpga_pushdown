#!/usr/bin/env bash
# Stage a stopped host-generated TPC-H cluster from the 9p share onto CEMU's
# emulated NVMe namespace. PostgreSQL and CSD execution use only the NVMe copy.
set -euo pipefail

usage() {
    echo "Usage: $0 [scale-factor] [--force]" >&2
    exit 2
}

SF="1"
FORCE=0
for arg in "$@"; do
    if [[ "${arg}" == --force ]]; then
        FORCE=1
    elif [[ "${arg}" =~ ^[1-9][0-9]*$ ]]; then
        SF="${arg}"
    else
        usage
    fi
done

[[ "$(id -u)" == 0 ]] || { echo "Run this script as root." >&2; exit 1; }

SHARED_DATA_ROOT="${TPCH_SHARED_DATA_ROOT:-/root/CEMU/data}"
NVME_DEVICE="${CSD_NVME_DEVICE:-/dev/nvme0n1}"
NVME_MOUNT="${CSD_NVME_MOUNT:-/mnt/nvme0}"
FDM_DEVICE="${CSD_FDM_DEVICE:-/dev/nvme0m2}"
FDM_MOUNT="${CSD_FDM_MOUNT:-/mnt/fdm0}"
SOURCE="${SHARED_DATA_ROOT}/tpch_s${SF}"
DEST_ROOT="${NVME_MOUNT}/pgdata"
DEST="${DEST_ROOT}/tpch_s${SF}"
INCOMING="${DEST}.incoming.$$"
FDM_GROUP="${CSD_FDM_GROUP:-${PG_OS_USER:-csdpg}}"

[[ -b "${NVME_DEVICE}" ]] || { echo "Missing NVMe namespace: ${NVME_DEVICE}" >&2; exit 1; }
[[ -f "${SOURCE}/PG_VERSION" ]] || { echo "Missing source PGDATA: ${SOURCE}" >&2; exit 1; }
[[ ! -e "${SOURCE}/postmaster.pid" ]] || {
    echo "Source PGDATA is active or was not shut down cleanly: ${SOURCE}" >&2
    exit 1
}

mkdir -p "${NVME_MOUNT}"
if ! mountpoint -q "${NVME_MOUNT}"; then
    if ! blkid "${NVME_DEVICE}" >/dev/null 2>&1; then
        mkfs.ext4 -F "${NVME_DEVICE}" >/dev/null
    fi
    mount "${NVME_DEVICE}" "${NVME_MOUNT}"
fi

# Initialize the standard FDMFS memory-range pool without the unrelated fio
# benchmark files created by cemu-mount.sh.
if [[ -b "${FDM_DEVICE}" ]]; then
    mkdir -p "${FDM_MOUNT}"
    if ! mountpoint -q "${FDM_MOUNT}"; then
        mount -t fdmfs "${FDM_DEVICE}" "${FDM_MOUNT}"
    fi
	# Query execution recreates numeric files at the exact size of each input,
	# result, spill, plan and status range.  The mount therefore belongs to the
	# dedicated PostgreSQL user; fixed 32 MiB benchmark files would strand half
	# of the finite FDM before a real TPC-H query starts.
	chown "${FDM_GROUP}:${FDM_GROUP}" "${FDM_MOUNT}"
	find "${FDM_MOUNT}" -maxdepth 1 -type f -name '[0-9]*' -delete
fi

mkdir -p "${DEST_ROOT}"
if [[ -e "${DEST}" ]]; then
    [[ ! -e "${DEST}/postmaster.pid" ]] || {
        echo "Destination PGDATA is active: ${DEST}" >&2
        exit 1
    }
    if [[ "${FORCE}" != 1 ]]; then
        echo "Destination already exists: ${DEST}; use --force to replace it." >&2
        exit 1
    fi
    rm -rf -- "${DEST}"
fi

cleanup() {
    rm -rf -- "${INCOMING}"
}
trap cleanup EXIT

echo "Staging ${SOURCE} to ${DEST} ..."
cp -a --reflink=never -- "${SOURCE}" "${INCOMING}"
sync -f "${INCOMING}"

source_files="$(find "${SOURCE}" -type f | wc -l)"
dest_files="$(find "${INCOMING}" -type f | wc -l)"
source_bytes="$(find "${SOURCE}" -type f -printf '%s\n' | awk '{ total += $1 } END { print total + 0 }')"
dest_bytes="$(find "${INCOMING}" -type f -printf '%s\n' | awk '{ total += $1 } END { print total + 0 }')"
[[ "${source_files}" == "${dest_files}" && "${source_bytes}" == "${dest_bytes}" ]] || {
    echo "Staged PGDATA verification failed." >&2
    exit 1
}

mv -- "${INCOMING}" "${DEST}"
trap - EXIT

echo "Staged tpch_s${SF}: files=${dest_files}, logical_bytes=${dest_bytes}"
echo "PGDATA=${DEST}"
