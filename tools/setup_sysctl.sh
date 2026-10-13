#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 调大 UDP 接收缓冲上限（跨主机大吞吐场景的必要部署步骤）
#
# 为什么必须做：net.core.rmem_max 默认只有 212992 字节（208KB）。
# Stratum 请求 8MB 接收缓冲，但普通 SO_RCVBUF 会被内核**静默截断**到该上限，
# 高突发下直接丢包。实测多 topic 2400 条 x 8KB：
#   rmem_max=208KB -> 只送达 87%
#   rmem_max=64MB  -> 送达 100%（含 1 条首条补救副本）
#
# 用法：
#   bash tools/setup_sysctl.sh            # 只对当前系统生效（重启失效）
#   bash tools/setup_sysctl.sh --persist  # 同时写入 /etc/sysctl.d/ 持久化
#
# 注意：容器/WSL 内可能没有写 /proc/sys 的权限（报 permission denied），
# 此时请在**宿主机或接收端所在的实际系统**上执行。
# ---------------------------------------------------------------------------
set -u

VALUE="${RMEM_MAX:-67108864}"
PERSIST=0
[ "${1:-}" = "--persist" ] && PERSIST=1

echo "目标 net.core.rmem_max = $VALUE"

if sysctl -w "net.core.rmem_max=$VALUE" 2>/dev/null; then
    echo "  net.core.rmem_max 已设置"
else
    echo "  警告：无法写入 net.core.rmem_max（权限不足）"
    echo "        请在接收端所在的实际系统上执行本脚本"
fi
# rmem_default 在部分内核/WSL 上是只读键，失败不影响主目标
sysctl -w net.core.rmem_default=8388608 2>/dev/null || true

if [ "$PERSIST" -eq 1 ]; then
    if [ -w /etc/sysctl.d ]; then
        cat > /etc/sysctl.d/99-stratum-rmem.conf <<EOF2
# Stratum：跨主机大吞吐需要更大的 UDP 接收缓冲（见 tools/setup_sysctl.sh）
net.core.rmem_max = $VALUE
net.core.rmem_default = 8388608
EOF2
        echo "  已持久化到 /etc/sysctl.d/99-stratum-rmem.conf"
    else
        echo "  警告：/etc/sysctl.d 不可写，无法持久化"
    fi
fi

echo
echo "当前值："
sysctl net.core.rmem_max 2>/dev/null | sed 's/^/  /'
