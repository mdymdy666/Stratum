#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 23（跨主机 · UDP 中继版）
#
# 适用拓扑（本项目实际环境）：
#   虚拟机在 VMware NAT（192.168.100.129），WSL 在 WSL NAT（172.17.x.x），
#   宿主机走 Wi-Fi 导致 VMware 桥接不可用 → 由 Windows 上的 UDP 中继转发。
#
# 前置条件（缺一不可）：
#   1. WSL 已改回 NAT：python tools/wsl_netmode.py nat && wsl --shutdown
#   2. Windows 上中继已启动：
#        python E:\CodexWorkspace\LearnFastDDS\tools\udp_relay.py \
#            --wsl <本脚本打印的 WSL IP> --guest 192.168.100.129
#   3. 虚拟机上 Stratum 已编译（/reactor/DDSProject/build/tools/rtps_reader_diag 存在）
#
# 用法：bash tools/fastdds_sub_diag/scenarios/23_cross_host_relay.sh [条数]
# ---------------------------------------------------------------------------
set -u

N="${1:-200}"
DOMAIN="${DOMAIN:-30}"
# RTPS 端口（随 domain 偏移，中继必须同时转发 SPDP 端口，否则永远发现不了彼此）：
#   SPDP        = 7400 + d*250  -> domain 30 = 14900
#   metatraffic = 9050 + d*10   -> domain 30 = 9350
#   user data   = 9051 + d*10   -> domain 30 = 9351
SPDP_PORT=$((7400 + DOMAIN * 250))
META_PORT=$((9050 + DOMAIN * 10))
USER_PORT=$((9051 + DOMAIN * 10))
VM=192.168.100.129
# 地址关系（实测确认）：
#   · 192.168.100.129 = 虚拟机自身
#   · 192.168.100.1   = 宿主机在 VMnet8 上的接口（与虚拟机同网段，直连可达）
#   · 192.168.100.2   = VMware NAT 的"内部网关"，**不是**宿主机的地址（绑不上）
#   · 172.17.211.143  = WSL
#
# 因此：
#   WSL -> 虚拟机   **直连可达**（实测 0% 丢包），不需要中继；
#   虚拟机 -> WSL   **不可达**，需要中继：中继绑 192.168.100.1，转给 WSL。
#
# 于是两侧声明：
#   WSL 侧 advertised_address = 192.168.100.1（虚拟机发到这里 -> 中继 -> WSL）
#   虚拟机侧 advertised_address = 192.168.100.129（真实地址，WSL 直连可达）
#   两侧 initial_peers 分别指向对方的"声明地址"。
WSL_ADV=192.168.100.1
VM_ADV=192.168.100.129
VM_DIR=/reactor/DDSProject

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 root@$VM"
mkdir -p "$LOGS"

WSL_IP=$(ip -4 -o addr show eth0 2>/dev/null | awk '{print $4}' | cut -d/ -f1)
echo "=== 拓扑 ==="
echo "  WSL   : $WSL_IP   （对外声明 $WSL_ADV）"
echo "  虚拟机: $VM       （对外声明 $VM_ADV）"
echo "  domain: $DOMAIN   条数: $N"
echo "  端口: SPDP=$SPDP_PORT meta=$META_PORT user=$USER_PORT（中继必须转发这三个）"
echo

echo "=== 1) 前置检查 ==="
if [ -z "$WSL_IP" ]; then echo "  FAIL: 取不到 WSL 地址"; exit 1; fi
if $SSH 'test -x /reactor/DDSProject/build/tools/rtps_reader_diag'; then
    echo "  虚拟机侧工具 ✓"
else
    echo "  FAIL: 虚拟机侧工具缺失，请先在那边构建"; exit 1
fi
echo

echo "=== 2) 生成时间线 ==="
TL="$TOOLS/tl_cross_relay"
rm -rf "$TL"
"$TOOLS/make_timeline" "$TL" "$N" > /dev/null 2>&1
echo "  已生成 $N 条"
echo

echo "=== 3) 虚拟机侧启动订阅端（后台，60 秒）==="
$SSH "cd $VM_DIR && STRATUM_ADVERTISED_ADDRESS=$VM_ADV \
      STRATUM_INITIAL_PEERS=$WSL_ADV \
      timeout 70 ./build/tools/rtps_reader_diag 60 $DOMAIN" \
    > "$LOGS/cross_vm_reader.log" 2>&1 &
VMJOB=$!
sleep 10

echo "=== 4) WSL 侧回放 ==="
STRATUM_ADVERTISED_ADDRESS="$WSL_ADV" STRATUM_INITIAL_PEERS="$VM_ADV" STRATUM_DUMP=1 \
    timeout 70 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 2.0 2>&1 \
    | grep -aE "已发布样本|发送窗口|has_user|ACKNACK 判定|发现话题" | tail -8
sleep 12
wait $VMJOB 2>/dev/null

echo
echo "=== 5) 结果 ==="
got=$(grep -a "用户回调样本" "$LOGS/cross_vm_reader.log" | tail -1)
echo "  虚拟机订阅端: ${got:-（无输出）}"
grep -a "发现话题" "$LOGS/cross_vm_reader.log" | tail -2 | sed 's/^/  /'
echo
if echo "${got:-}" | grep -qE "用户回调样本 = [1-9]"; then
    echo "判定：PASS —— **跨主机数据收发成功**（虚拟机收到了 WSL 回放的样本）"
else
    echo "判定：FAIL —— 未收到数据，请检查：中继是否在跑、两侧 advertised_address 是否正确"
    echo "  虚拟机侧日志尾部:"
    tail -5 "$LOGS/cross_vm_reader.log" | sed 's/^/    /'
fi
