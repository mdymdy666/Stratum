#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 24（跨主机 · 复杂场景套件）
#
# 四个子场景，全部在 WSL <-> 独立虚拟机 之间、经 Windows UDP 中继完成：
#   A. 多话题并发       —— 两个话题同时跨主机，验证零串扰
#   B. 大 payload       —— 1200B / 8000B / 60000B
#   C. 随机测试         —— 随机 payload / 条数 / 话题数，多轮
#   D. 实时回访闭环     —— WSL 回放 -> 虚拟机录制 -> 虚拟机回放 -> WSL 接收
#
# 前置：Windows 侧中继在跑
#   python -u tools/udp_relay.py --ports 14900,9360,9361 \
#       --wsl <WSL_IP> --guest 192.168.100.129 --wsl-gw 192.168.100.1 --one-way
#
# 用法：bash tools/fastdds_sub_diag/scenarios/24_cross_host_complex.sh [轮数]
# ---------------------------------------------------------------------------
set -u

ROUNDS="${1:-5}"
DOMAIN="${DOMAIN:-30}"
VM=192.168.100.129
VM_ADV=192.168.100.129        # 虚拟机真实地址（WSL 直连可达）
WSL_ADV=192.168.100.1         # WSL 声明地址（虚拟机发到这里，中继接手）
VM_DIR=/reactor/DDSProject

SPDP_PORT=$((7400 + DOMAIN * 250))
META_PORT=$((9060 + DOMAIN * 10))
USER_PORT=$((9061 + DOMAIN * 10))

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 root@$VM"
mkdir -p "$LOGS"

WSL_IP=$(ip -4 -o addr show eth0 2>/dev/null | awk '{print $4}' | cut -d/ -f1)

FAIL=0
pass() { printf '  %-34s PASS  %s\n' "$1" "$2"; }
fail() { printf '  %-34s FAIL  %s\n' "$1" "$2"; FAIL=$((FAIL + 1)); }

# 在虚拟机里跑订阅端（收所有发现的话题），日志取回 WSL。
# 【重要】调用时不要写成 RJ=(命令替换) 的形式 —— 命令替换会 fork 一个子 shell，
# 在子 shell 里后台启动的 ssh 会随子 shell 退出而被杀（实测表现为"收到 0 条"）。
# 因此这里通过全局变量 VM_READER_PID 回传 PID，调用方直接读该变量。
VM_READER_PID=""
vm_reader() {
    local secs="$1" tag="$2"
    $SSH "cd $VM_DIR && STRATUM_ADVERTISED_ADDRESS=$VM_ADV \
          STRATUM_INITIAL_PEERS=$WSL_ADV \
          timeout $((secs + 12)) ./build/tools/rtps_reader_diag $secs $DOMAIN" \
        > "$LOGS/cx_${tag}_reader.log" 2>&1 &
    VM_READER_PID=$!
}

wsl_replay() {
    local tl="$1" rate="$2"
    STRATUM_ADVERTISED_ADDRESS="$WSL_ADV" STRATUM_INITIAL_PEERS="$VM_ADV" \
        timeout 90 "$TOOLS/stratum_rtps_replay" "$tl" "$DOMAIN" "$rate" 2>&1
}

echo "=== 前置检查 ==="
echo "  WSL=$WSL_IP  虚拟机=$VM  domain=$DOMAIN"
echo "  端口: SPDP=$SPDP_PORT meta=$META_PORT user=$USER_PORT"
$SSH 'test -x /reactor/DDSProject/build/tools/stratum_rtps_record' \
    && echo "  虚拟机工具 ✓" || { echo "  FAIL: 虚拟机工具缺失"; exit 1; }
echo

# ---------------------------------------------------------------------------
echo "=== A. 多话题并发（跨主机）==="
TLA="$TOOLS/cx_multi"
rm -rf "$TLA"
"$TOOLS/make_timeline" "$TLA" 100 0 60 > /dev/null 2>&1
vm_reader 40 multi
    RJ=$VM_READER_PID
sleep 18
wsl_replay "$TLA" 5.0 | grep -aE "已发布样本" | tail -1 | sed 's/^/  WSL: /'
wait $RJ 2>/dev/null
got_a=$(grep -a "用户回调样本" "$LOGS/cx_multi_reader.log" | tail -1 | grep -o "[0-9]*")
topics=$(grep -a "发现话题" "$LOGS/cx_multi_reader.log" | tail -1)
echo "  $topics"
echo "  用户回调样本 = ${got_a:-0}（期望 160）"
if [ "${got_a:-0}" -ge 160 ]; then
    pass "多话题跨主机" "收到 ${got_a}（两个话题合计）"
else
    fail "多话题跨主机" "收到 ${got_a:-0}"
fi
echo

# ---------------------------------------------------------------------------
echo "=== B. 大 payload（跨主机）==="
for spec in "1200:20" "8000:20" "60000:10"; do
    pb="${spec%%:*}"
    cnt="${spec##*:}"
    TLB="$TOOLS/cx_p${pb}"
    rm -rf "$TLB"
    "$TOOLS/make_timeline" "$TLB" "$cnt" "$pb" > /dev/null 2>&1
    vm_reader 35 "p${pb}"
    RJ=$VM_READER_PID
    sleep 18
    wsl_replay "$TLB" 5.0 > /dev/null 2>&1
    wait $RJ 2>/dev/null
    got=$(grep -a "用户回调样本" "$LOGS/cx_p${pb}_reader.log" | tail -1 | grep -o "[0-9]*")
    if [ "${got:-0}" -ge "$cnt" ]; then
        pass "payload=${pb}B x${cnt}" "收到 ${got}"
    else
        fail "payload=${pb}B x${cnt}" "收到 ${got:-0}"
    fi
done
echo

# ---------------------------------------------------------------------------
echo "=== C. 随机测试（跨主机，${ROUNDS} 轮）==="
for r in $(seq 1 "$ROUNDS"); do
    payload=$(( (RANDOM % 4) * 2000 ))
    count=$(( 20 + RANDOM % 80 ))
    t2=$(( RANDOM % 40 ))
    TLC="$TOOLS/cx_rand"
    rm -rf "$TLC"
    "$TOOLS/make_timeline" "$TLC" "$count" "$payload" "$t2" > /dev/null 2>&1
    vm_reader 35 "rand${r}"
    RJ=$VM_READER_PID
    sleep 18
    wsl_replay "$TLC" 5.0 > /dev/null 2>&1
    wait $RJ 2>/dev/null
    got=$(grep -a "用户回调样本" "$LOGS/cx_rand${r}_reader.log" | tail -1 | grep -o "[0-9]*")
    printf '  第 %s 轮  payload=%-6s count=%-4s topic2=%-4s 收到=%s\n' \
        "$r" "${payload}B" "$count" "$t2" "${got:-?}"
    if [ "${got:-0}" -lt "$count" ]; then
        fail "随机第 ${r} 轮" "收到 ${got:-0} < $count"
    fi
done
[ "$FAIL" -eq 0 ] && pass "随机测试全部轮次" "全部达标"
echo

# ---------------------------------------------------------------------------
echo "=== D. 实时回访闭环（WSL 回放 -> 虚拟机录制 -> 虚拟机回放 -> WSL 接收）==="
TLD_IN="$TOOLS/cx_loop_in"
rm -rf "$TLD_IN"
"$TOOLS/make_timeline" "$TLD_IN" 150 0 0
$SSH "rm -rf $VM_DIR/build/tl_cx_rec" 2>/dev/null
rm -rf "$TOOLS/cx_loop_rec" "$TOOLS/cx_loop_rec_vm" 2>/dev/null > /dev/null 2>&1

echo "--- D1: 虚拟机录制 WSL 回放出来的数据 ---"
$SSH "cd $VM_DIR && STRATUM_ADVERTISED_ADDRESS=$VM_ADV \
      STRATUM_INITIAL_PEERS=$WSL_ADV \
      timeout 75 ./build/tools/stratum_rtps_record 60 $DOMAIN $VM_DIR/build/tl_cx_rec" \
    > "$LOGS/cx_rec_vm.log" 2>&1 &
RECJOB=$!
sleep 18
wsl_replay "$TLD_IN" 5.0 | grep -aE "已发布样本" | tail -1 | sed 's/^/  D1 WSL 回放: /'
sleep 10
kill $RECJOB 2>/dev/null
$SSH "pkill -f 'stratum_rtps_record .*tl_cx_rec' 2>/dev/null; sleep 2; true" 2>/dev/null
wait $RECJOB 2>/dev/null

# 用虚拟机自己的 replay 检查录到了多少（只看开头的"待回放话题"统计）
recstat=$($SSH "cd $VM_DIR && timeout 25 ./build/tools/stratum_rtps_replay \
        $VM_DIR/build/tl_cx_rec $DOMAIN 5.0 2>&1 | head -4" 2>&1)
echo "$recstat" | sed 's/^/  D1 录制结果: /'
recn=$(echo "$recstat" | grep -o "[0-9]* 条" | head -1 | grep -o "[0-9]*")
echo "  D1 录到 ${recn:-0} 条"

echo "--- D2: 虚拟机回放录到的数据 -> WSL 接收 ---"
STRATUM_ADVERTISED_ADDRESS="$WSL_ADV" STRATUM_INITIAL_PEERS="$VM_ADV" \
    timeout 80 "$TOOLS/rtps_reader_diag" 60 "$DOMAIN" > "$LOGS/cx_loop_wsl.log" 2>&1 &
RJW=$!
sleep 12
$SSH "cd $VM_DIR && STRATUM_ADVERTISED_ADDRESS=$VM_ADV \
      STRATUM_INITIAL_PEERS=$WSL_ADV \
      timeout 50 ./build/tools/stratum_rtps_replay $VM_DIR/build/tl_cx_rec $DOMAIN 5.0" \
    2>&1 | grep -aE "已发布样本" | tail -1 | sed 's/^/  D2 虚拟机回放: /'
wait $RJW 2>/dev/null
wslgot=$(grep -a "用户回调样本" "$LOGS/cx_loop_wsl.log" | tail -1 | grep -o "[0-9]*")
echo "  D2 WSL 接收: 用户回调样本 = ${wslgot:-?}"

if [ "${recn:-0}" -gt 50 ] && [ "${wslgot:-0}" -gt 50 ]; then
    pass "实时回访闭环" "录制 ${recn} 条 / 回放到 WSL ${wslgot} 条"
else
    fail "实时回访闭环" "录制 ${recn:-0} / WSL 收到 ${wslgot:-0}"
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "判定：PASS（跨主机复杂场景全部通过）"
else
    echo "判定：$FAIL 项不达标（见上）"
fi
exit $((FAIL > 0 ? 1 : 0))
