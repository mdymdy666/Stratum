#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 22（跨主机测试）
#
# 两端各自运行本脚本，互为对端。用法：
#
#   A 端（发布）: bash 22_cross_host.sh pub  <本端domain> <对端IP> [条数]
#   B 端（订阅）: bash 22_cross_host.sh sub  <本端domain> <对端IP> [超时秒]
#
# 关键点（跨主机与单机的差异）：
#   1. **组播跨不过 NAT**：WSL 默认 nat 模式时，SPDP 的组播 239.255.0.1
#      到不了另一端，必须用 STRATUM_INITIAL_PEERS 显式指定单播对端；
#   2. **对端必须能反向访问本端**：RTPS 是双向协议（SPDP/SEDP/ACKNACK 都要回），
#      若本端在 NAT 后（如 WSL 的 172.17.x.x），对端发不回来，发现就无法完成。
#      解决：把本端暴露在对方可达的地址上（WSL 用 mirrored 网络模式，
#      或在 Windows 上用 netsh portproxy 转发 9060/9061 并设置 advertised_address）。
# ---------------------------------------------------------------------------
set -u

ROLE="${1:?用法: $0 pub|sub <domain> <peer_ip> [n]}"
DOMAIN="${2:?缺少 domain}"
PEER="${3:?缺少对端 IP}"
N="${4:-200}"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-/opt/ros/jazzy/lib}"
export STRATUM_INITIAL_PEERS="$PEER"
mkdir -p "$LOGS"

echo "角色=$ROLE domain=$DOMAIN 对端=$PEER 单播 SPDP 对端列表=$STRATUM_INITIAL_PEERS"

if [ "$ROLE" = "pub" ]; then
    TL="$TOOLS/tl_cross"
    rm -rf "$TL"
    "$TOOLS/make_timeline" "$TL" "$N" > /dev/null 2>&1
    echo "已生成 $N 条时间线，开始回放（等 8 秒让对方先发现）..."
    sleep 8
    STRATUM_DUMP=1 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 5.0 2>&1 \
        | grep -aE "已发布样本|发送窗口|ACKNACK 判定|has_user" | tail -8
else
    timeout 60 "$TOOLS/rtps_reader_diag" "${N}" "$DOMAIN" 2>&1 \
        | grep -aE "用户回调样本|发现话题|READER_LATENCY" | tail -6
fi
