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
SOURCE_ARCHIVE="${SOURCE}.stage.tar.zst"
DEST_ROOT="${NVME_MOUNT}/pgdata"
DEST="${DEST_ROOT}/tpch_s${SF}"
INCOMING="${DEST}.incoming.$$"
FDM_GROUP="${CSD_FDM_GROUP:-${PG_OS_USER:-csdpg}}"

[[ -b "${NVME_DEVICE}" ]] || { echo "Missing NVMe namespace: ${NVME_DEVICE}" >&2; exit 1; }
[[ -f "${SOURCE}/PG_VERSION" ]] || { echo "Missing source PGDATA: ${SOURCE}" >&2; exit 1; }
if [[ ! -f "${SOURCE_ARCHIVE}" && -e "${SOURCE}/postmaster.pid" ]]; then
    echo "Source PGDATA is active or was not shut down cleanly: ${SOURCE}" >&2
    exit 1
fi

mkdir -p "${NVME_MOUNT}"
if mountpoint -q "${NVME_MOUNT}"; then
	mounted_source="$(findmnt -n -o SOURCE --target "${NVME_MOUNT}")"
	mounted_type="$(findmnt -n -o FSTYPE --target "${NVME_MOUNT}")"
	[[ "${mounted_source}" == "${NVME_DEVICE}" && "${mounted_type}" == ext4 ]] || {
		echo "${NVME_MOUNT} must be an ext4 mount of ${NVME_DEVICE}; found ${mounted_source} (${mounted_type})." >&2
		exit 1
	}
else
    if ! blkid "${NVME_DEVICE}" >/dev/null 2>&1; then
        # This namespace is dedicated to PostgreSQL base files plus
        # query-local CSD spill.  The default ext4 5% root reservation would
        # strand several GiB that the non-root csdpg process needs at scale.
        mkfs.ext4 -F -m 0 "${NVME_DEVICE}" >/dev/null
    fi
	# Specify ext4 explicitly. FDMFS accepts arbitrary block devices, so generic
	# filesystem probing can otherwise mount a blank NVM namespace as FDM memory.
	mount -t ext4 "${NVME_DEVICE}" "${NVME_MOUNT}"
fi
# Also normalize namespaces formatted by an older version of this script.
# tune2fs permits changing the reserved-block percentage on a mounted ext4
# filesystem; no data or allocation mapping is rewritten.
tune2fs -m 0 "${NVME_DEVICE}" >/dev/null

# Initialize the standard FDMFS memory-range pool without the unrelated fio
# benchmark files created by cemu-mount.sh.
if [[ -b "${FDM_DEVICE}" ]]; then
    mkdir -p "${FDM_MOUNT}"
    if ! mountpoint -q "${FDM_MOUNT}"; then
        mount -t fdmfs "${FDM_DEVICE}" "${FDM_MOUNT}"
    fi
	# Query execution recreates numeric files for bounded scan windows plus the
	# result, spill-staging, plan and status ranges. The mount therefore belongs
	# to the dedicated PostgreSQL user; benchmark files would reserve unrelated
	# chunks from the finite 4 GiB FDM namespace.
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

USED_ARCHIVE=0
if [[ -f "${SOURCE_ARCHIVE}" ]]; then
    USED_ARCHIVE=1
    command -v zstd >/dev/null || {
        echo "${SOURCE_ARCHIVE} exists, but zstd is not installed in the guest." >&2
        exit 1
    }
    echo "Staging compressed ${SOURCE_ARCHIVE} to ${DEST} ..."
    mkdir -p "${INCOMING}"
    # The archive contains one top-level tpch_s<SF> directory.  Preserve the
    # numeric owner shared by the host and guest csdpg account, but remove that
    # top-level component so the atomic incoming directory remains the target.
    tar --use-compress-program=zstd --extract --file="${SOURCE_ARCHIVE}" \
        --directory="${INCOMING}" --strip-components=1 --same-owner
else
    echo "Staging ${SOURCE} to ${DEST} ..."
    cp -a --reflink=never -- "${SOURCE}" "${INCOMING}"
fi
sync -f "${INCOMING}"

dest_files="$(find "${INCOMING}" -type f | wc -l)"
dest_bytes="$(find "${INCOMING}" -type f -printf '%s\n' | awk '{ total += $1 } END { print total + 0 }')"
if [[ "${USED_ARCHIVE}" == 1 ]]; then
    # zstd and tar checksums/exit status protect the immutable archive.  The
    # live source cluster may have been restarted after that archive was
    # created, so comparing it byte-for-byte here would be both racy and wrong.
    [[ -f "${INCOMING}/PG_VERSION" && ! -e "${INCOMING}/postmaster.pid" ]] || {
        echo "Archived PGDATA is incomplete or was not cleanly stopped." >&2
        exit 1
    }
    # tar creates INCOMING before stripping the archive's top-level component,
    # so that directory itself keeps root ownership even though every archived
    # child preserves the PostgreSQL account's numeric uid/gid.  PostgreSQL
    # requires PGDATA (not only its contents) to belong to the server user.
    chown --reference="${INCOMING}/PG_VERSION" "${INCOMING}"
    chmod --reference="${INCOMING}/base" "${INCOMING}"
else
    source_files="$(find "${SOURCE}" -type f | wc -l)"
    source_bytes="$(find "${SOURCE}" -type f -printf '%s\n' | awk '{ total += $1 } END { print total + 0 }')"
    [[ "${source_files}" == "${dest_files}" && "${source_bytes}" == "${dest_bytes}" ]] || {
        echo "Staged PGDATA verification failed." >&2
        exit 1
    }
fi

mv -- "${INCOMING}" "${DEST}"
trap - EXIT

echo "Staged tpch_s${SF}: files=${dest_files}, logical_bytes=${dest_bytes}"
echo "PGDATA=${DEST}"
