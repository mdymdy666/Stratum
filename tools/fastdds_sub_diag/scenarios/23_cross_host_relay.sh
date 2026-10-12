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
VM=192.168.100.129
VM_ADV=192.168.100.2      # 虚拟机对外声明：VMware NAT 网关（Windows 侧）
WSL_ADV=172.17.208.1      # WSL 对外声明：WSL NAT 网关（Windows 侧）
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
      timeout 70 ./build/tools/rtps_reader_diag 60 $DOMAIN" \
    > "$LOGS/cross_vm_reader.log" 2>&1 &
VMJOB=$!
sleep 10

echo "=== 4) WSL 侧回放 ==="
STRATUM_ADVERTISED_ADDRESS="$WSL_ADV" STRATUM_DUMP=1 \
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
