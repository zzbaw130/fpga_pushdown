#!/usr/bin/env bash
# Run inside a CEMU guest on an otherwise empty FDMFS mount.  The allocation
# pattern fills the device with unequal ranges, releases them in allocation
# order, and then asks for one full-size range.  This reproduces the allocator
# fragmentation that used to make a later PostgreSQL query fail despite having
# enough aggregate free FDM.
set -euo pipefail

FDM_DIR="${CSD_FDM_DIR:-/mnt/fdm0}"
FDM_DEVICE="${CSD_FDM_DEVICE:-/dev/nvme0m2}"
PREFIX=".csd-fdm-reuse-$$"

[[ "$(id -u)" -eq 0 ]] || { echo "Run as root." >&2; exit 1; }
mountpoint -q "${FDM_DIR}" || { echo "${FDM_DIR} is not mounted." >&2; exit 1; }
[[ -b "${FDM_DEVICE}" ]] || { echo "Missing FDM device ${FDM_DEVICE}." >&2; exit 1; }
if find "${FDM_DIR}" -mindepth 1 -maxdepth 1 -print -quit | grep -q .; then
	echo "${FDM_DIR} must be empty so the test can account for the full pool." >&2
	exit 1
fi

capacity="$(blockdev --getsize64 "${FDM_DEVICE}")"
first=$((64 * 1024 * 1024))
second=$((96 * 1024 * 1024))
third=$((capacity - first - second))
(( third > 0 && capacity % 4096 == 0 )) || { echo "Invalid FDM capacity ${capacity}." >&2; exit 1; }

paths=("${FDM_DIR}/${PREFIX}-0" "${FDM_DIR}/${PREFIX}-1" "${FDM_DIR}/${PREFIX}-2")
full="${FDM_DIR}/${PREFIX}-full"
cleanup() {
	for path in "${paths[@]}" "${full}"; do
		[[ ! -e "${path}" ]] || unlink "${path}"
	done
}
trap cleanup EXIT

for path in "${paths[@]}"; do
	: >"${path}"
done
fallocate -l "${first}" "${paths[0]}"
fallocate -l "${second}" "${paths[1]}"
fallocate -l "${third}" "${paths[2]}"

# Allocation-order release requires both backward and forward coalescing.
for path in "${paths[@]}"; do
	unlink "${path}"
done

: >"${full}"
fallocate -l "${capacity}" "${full}"
actual="$(stat -c %s "${full}")"
[[ "${actual}" -eq "${capacity}" ]]
echo "FDMFS reuse PASS: coalesced ${capacity} bytes after fragmented release."
