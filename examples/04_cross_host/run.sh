#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 示例 04：跨主机收发（NAT + UDP 中继）
#
# 验证什么：当两端都在 NAT 之后、互相不可直达时，
#           借助宿主机上的 UDP 中继仍能完成双向发现与数据传输。
#
# 拓扑（本项目实测环境）：
#   WSL (172.17.x.x)  ──直连可达──▶  虚拟机 (192.168.100.129)
#        ▲                                    │
#        └──── Windows 单向/双向 UDP 中继 ◀────┘
#              （绑定 192.168.100.1）
#
# 前置（缺一不可）：
#   1. 虚拟机上已部署 Stratum（/reactor/DDSProject）且已构建
#   2. Windows 侧中继在跑：
#        python tools/start_cross_host.py            # 一键准备（推荐）
#      或手工：
#        python tools/udp_relay.py --ports 14900,9350,9351 \
#            --wsl <WSL_IP> --guest 192.168.100.129 --bind 192.168.100.1
#
# 用法：bash examples/04_cross_host/run.sh
# ---------------------------------------------------------------------------
set -u

DOMAIN="${DOMAIN:-73}"
VM="${VM:-192.168.100.129}"
WSL_ADV="${WSL_ADV:-192.168.100.1}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu
mkdir -p "$LOGS"

SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 root@$VM"
WSL_IP=$(hostname -I | awk '{print $1}')

SPDP=$((7400 + DOMAIN * 250))
META=$((9050 + DOMAIN * 10))
USER=$((9051 + DOMAIN * 10))

echo "=== 示例 04：跨主机收发（domain=$DOMAIN）==="
echo "  WSL=$WSL_IP  虚拟机=$VM"
echo "  端口：SPDP=$SPDP metatraffic=$META user=$USER"

echo
echo "--- 前置检查 ---"
if ! timeout 5 bash -c "echo > /dev/tcp/$VM/22" 2>/dev/null; then
    echo "  ❌ 虚拟机 SSH 不可达。请先在 Windows 侧执行："
    echo "     F:\\vmware\\vmrun.exe start \"<vmx路径>\" gui"
    exit 1
fi
echo "  虚拟机可达 ✓"
if ! $SSH 'test -x /reactor/DDSProject/build/tools/rtps_reader_diag' 2>/dev/null; then
    echo "  ❌ 虚拟机上缺少 Stratum 工具（/reactor/DDSProject）"
    exit 1
fi
echo "  虚拟机侧工具就绪 ✓"
echo "  （中继是否在跑无法从 WSL 直接探测，由结果反推）"

echo
echo "--- 1) WSL 回放 -> 虚拟机接收 ---"
TL="$TOOLS/ex04_tl"; rm -rf "$TL"
"$TOOLS/make_timeline" "$TL" 50 8000 > /dev/null 2>&1
$SSH "cd /reactor/DDSProject && STRATUM_ADVERTISED_ADDRESS=$VM \
      STRATUM_INITIAL_PEERS=$WSL_ADV \
      timeout 75 ./build/tools/rtps_reader_diag 60 $DOMAIN" \
    > "$LOGS/ex04_fwd_sub.log" 2>&1 &
RJ=$!
sleep 18
STRATUM_ADVERTISED_ADDRESS="$WSL_ADV" STRATUM_INITIAL_PEERS="$VM" \
    timeout 60 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 10.0 \
    > "$LOGS/ex04_fwd_rep.log" 2>&1
wait $RJ 2>/dev/null
fwd=$(grep -a "用户回调样本" "$LOGS/ex04_fwd_sub.log" | tail -1 | grep -o "[0-9]*" | head -1)
echo "  虚拟机收到 = ${fwd:-0}（期望 50）"

echo
echo "--- 2) 虚拟机回放 -> WSL 接收 ---"
$SSH "cd /reactor/DDSProject && rm -rf build/ex04_tl && \
      ./build/tools/make_timeline build/ex04_tl 50" > /dev/null 2>&1
STRATUM_ADVERTISED_ADDRESS="$WSL_ADV" STRATUM_INITIAL_PEERS="$VM" \
    timeout 80 "$TOOLS/rtps_reader_diag" 65 "$DOMAIN" > "$LOGS/ex04_rev_sub.log" 2>&1 &
RJ=$!
sleep 16
$SSH "cd /reactor/DDSProject && STRATUM_ADVERTISED_ADDRESS=$VM \
      STRATUM_INITIAL_PEERS=$WSL_ADV \
      timeout 50 ./build/tools/stratum_rtps_replay build/ex04_tl $DOMAIN 10.0" \
    > "$LOGS/ex04_rev_rep.log" 2>&1
wait $RJ 2>/dev/null
rev=$(grep -a "用户回调样本" "$LOGS/ex04_rev_sub.log" | tail -1 | grep -o "[0-9]*" | head -1)
echo "  WSL 收到 = ${rev:-0}（期望 50）"

echo
echo "=========================================="
if [ "${fwd:-0}" -ge 50 ] && [ "${rev:-0}" -ge 50 ]; then
    echo "  ✅ PASS —— 跨主机双向收发正常"
    exit 0
fi
echo "  ❌ FAIL（正向 ${fwd:-0}，反向 ${rev:-0}）"
echo "  排查顺序："
echo "    1. Windows 侧中继是否在跑？（python tools/start_cross_host.py）"
echo "    2. 中继端口是否算全？（漏 SPDP/metatraffic 会表现为发现不了或收不到数据）"
echo "    3. 两侧的 STRATUM_ADVERTISED_ADDRESS / STRATUM_INITIAL_PEERS 是否对称？"
exit 1
