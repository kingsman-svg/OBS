#!/usr/bin/env bash
set -euo pipefail

# 密码从本地 .env 注入，不写入镜像，不输出到日志。
: "${OBS_ROOT_PASSWORD:?OBS_ROOT_PASSWORD is required}"
printf 'root:%s\n' "$OBS_ROOT_PASSWORD" | chpasswd
unset OBS_ROOT_PASSWORD
mkdir -p /run/sshd /etc/ssh/keys
if [[ ! -f /etc/ssh/keys/ssh_host_ed25519_key ]]; then
    ssh-keygen -q -t ed25519 -N '' -f /etc/ssh/keys/ssh_host_ed25519_key
fi
chmod 600 /etc/ssh/keys/ssh_host_ed25519_key
/usr/sbin/sshd -t
exec /usr/sbin/sshd -D -e
