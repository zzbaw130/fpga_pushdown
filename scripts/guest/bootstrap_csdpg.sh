#!/usr/bin/env bash
# Run once as root in the guest. The account UID/GID intentionally matches the
# source PGDATA so its ownership remains valid after staging it onto CEMU NVMe.
set -euo pipefail

DATA_ROOT="${CSD_DATA_ROOT:-/root/CEMU/data}"
SHARE_ROOT="${CSD_SHARE_ROOT:-/root/CEMU}"
USER_SHARE_ROOT="${CSD_USER_SHARE_ROOT:-/mnt/cemu}"
PG_OS_USER="${PG_OS_USER:-csdpg}"
DEFAULT_PASSWORD="${CSDPG_PASSWORD:-csdpg}"

if [[ ! -d "${DATA_ROOT}" ]]; then
    echo "Missing shared data directory: ${DATA_ROOT}" >&2
    exit 1
fi

# The stock image mounts 9p under /root, which ordinary users cannot traverse.
# Bind it to a neutral mountpoint and persist that bind in the qcow2 overlay.
mkdir -p "${USER_SHARE_ROOT}"
if ! mountpoint -q "${USER_SHARE_ROOT}"; then
    mount --bind "${SHARE_ROOT}" "${USER_SHARE_ROOT}"
fi
fstab_line="${SHARE_ROOT} ${USER_SHARE_ROOT} none bind 0 0"
grep -qxF "${fstab_line}" /etc/fstab || echo "${fstab_line}" >> /etc/fstab

owner_uid="$(stat -c '%u' "${DATA_ROOT}")"
owner_gid="$(stat -c '%g' "${DATA_ROOT}")"

if ! getent group "${owner_gid}" >/dev/null; then
    groupadd -g "${owner_gid}" "${PG_OS_USER}"
fi
if id -u "${PG_OS_USER}" >/dev/null 2>&1; then
    [[ "$(id -u "${PG_OS_USER}")" == "${owner_uid}" ]] || {
        echo "${PG_OS_USER} exists with a different UID" >&2
        exit 1
    }
else
    useradd -m -u "${owner_uid}" -g "${owner_gid}" -s /bin/bash "${PG_OS_USER}"
    echo "${PG_OS_USER}:${DEFAULT_PASSWORD}" | chpasswd
fi

echo "Provisioned ${PG_OS_USER} (uid=${owner_uid}, gid=${owner_gid})."
echo "Shared files are available to ${PG_OS_USER} at ${USER_SHARE_ROOT}."
echo "Change its password after first login: passwd ${PG_OS_USER}"

# Grant csdpg permission to open the CEMU control and execute nodes after udev
# creates them on every boot. Linux still requires CAP_SYS_ADMIN for unknown
# NVMe passthrough opcodes; the PostgreSQL executor must use the forthcoming
# constrained Program Execute allowlist rather than a broad process capability.
install -d -m 0755 /usr/local/sbin /etc/systemd/system
cat >/usr/local/sbin/csd-grant-device-access <<EOF
#!/usr/bin/env bash
set -euo pipefail
group_name='${PG_OS_USER}'
shopt -s nullglob
for device in /dev/nvme*c3 /dev/ng*n3; do
    chgrp "\${group_name}" "\${device}"
    chmod 0660 "\${device}"
done
EOF
chmod 0755 /usr/local/sbin/csd-grant-device-access
cat >/etc/systemd/system/csd-device-access.service <<'EOF'
[Unit]
Description=Grant CEMU program-device access to csdpg
After=systemd-udev-settle.service
Wants=systemd-udev-settle.service

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/csd-grant-device-access

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable csd-device-access.service
/usr/local/sbin/csd-grant-device-access
