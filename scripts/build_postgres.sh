#!/usr/bin/env bash
# Build PostgreSQL 12 from the pinned REL_12_STABLE submodule.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="${ROOT}/code/postgres"
BUILD_DIR="${PG_BUILD_DIR:-${ROOT}/build}"
INSTALL_DIR="${PG_INSTALL_DIR:-${BUILD_DIR}/install}"

if [[ ! -x "${SOURCE_DIR}/configure" ]]; then
    echo "PostgreSQL source is missing: ${SOURCE_DIR}" >&2
    echo "Run: git submodule update --init code/postgres" >&2
    exit 1
fi

mkdir -p "${BUILD_DIR}"
pushd "${BUILD_DIR}" >/dev/null
"${SOURCE_DIR}/configure" --prefix="${INSTALL_DIR}" --with-icu --enable-debug
make -j"$(nproc)"
make install
popd >/dev/null

echo "PostgreSQL installed in ${INSTALL_DIR}"
