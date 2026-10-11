#!/usr/bin/env bash
# Run inside the CEMU guest after its 9p share is mounted at /root/CEMU.
set -euo pipefail

[[ "$(id -u)" == 0 ]] || { echo "Run this script as root." >&2; exit 1; }

SOURCE_DIR="${PG_SOURCE_DIR:-/root/CEMU/code/postgres}"
BUILD_DIR="${PG_GUEST_BUILD_DIR:-/root/pg_build}"
# The server must run as an unprivileged OS user, so do not install beneath
# /root (normally mode 0700).  /opt is readable and traversable by csdpg.
INSTALL_DIR="${PG_GUEST_INSTALL_DIR:-/opt/pgsql12}"

if [[ ! -x "${SOURCE_DIR}/configure" ]]; then
    echo "PostgreSQL source was not found at ${SOURCE_DIR}." >&2
    echo "Mount the CEMU 9p share first: mount -t 9p -o trans=virtio cemusrc /root/CEMU" >&2
    exit 1
fi

missing_packages=()
for package in build-essential pkg-config flex bison perl libreadline-dev zlib1g-dev libicu-dev libcap2-bin; do
    dpkg-query -W -f='${db:Status-Status}' "${package}" 2>/dev/null | grep -qx installed ||
        missing_packages+=("${package}")
done
if (( ${#missing_packages[@]} )); then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install --no-install-recommends -y "${missing_packages[@]}"
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"
"${SOURCE_DIR}/configure" --prefix="${INSTALL_DIR}" --with-icu --enable-debug
make -j"$(nproc)"
make install
"$(dirname "${BASH_SOURCE[0]}")/grant_csd_capability.sh" "${INSTALL_DIR}/bin/postgres"
"${INSTALL_DIR}/bin/postgres" --version
