#!/usr/bin/env bash
# Grant selected guest executables the capability required by Linux's generic
# NVMe passthrough permission check. This is intentionally broad for the CEMU
# research prototype and must not be copied to a production deployment.
set -euo pipefail

[[ "$(id -u)" == 0 ]] || { echo "Run this script as root." >&2; exit 1; }
command -v setcap >/dev/null 2>&1 || {
    echo "setcap is missing; install the libcap2-bin package." >&2
    exit 1
}

if (( $# == 0 )); then
    set -- "${PG_GUEST_INSTALL_DIR:-/opt/pgsql12}/bin/postgres"
fi

for binary in "$@"; do
    [[ "${binary}" == /* && -f "${binary}" && -x "${binary}" ]] || {
        echo "Capability target must be an absolute executable path: ${binary}" >&2
        exit 1
    }
    setcap cap_sys_admin=ep "${binary}"
    capability="$(getcap "${binary}")"
    [[ "${capability}" == *cap_sys_admin=ep* ]] || {
        echo "Failed to grant CAP_SYS_ADMIN: ${binary}" >&2
        exit 1
    }
    echo "Granted CAP_SYS_ADMIN: ${capability}"
done
