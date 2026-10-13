#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 25（跨主机 · 工业级压测 / 随机测试）
#
# 拓扑：WSL 回放（发送端） --1Gbps 虚拟网络+中继--> 虚拟机录制（接收端）
# 接收端用 stratum_rtps_record，因为它能**按 topic 分解统计**条数，
# 便于验证多 topic 场景下的完整性与零串扰。
#
# 子场景：
#   A. 多 topic 大数据量（2 topic 混合，大 payload，数千条）
#   B. 吞吐阶梯（速率 50 / 200 / 1000 条每秒，观察完整率）
#   C. 随机测试（payload / 条数 / topic2 比例 / 速率 全部随机，多轮）
#
# 用法：bash 25_cross_host_stress.sh [随机轮数]
# ---------------------------------------------------------------------------
set -u

ROUNDS="${1:-8}"
DOMAIN="${DOMAIN:-30}"
VM=192.168.100.129
VM_ADV=192.168.100.129
WSL_ADV=192.168.100.1
VM_DIR=/reactor/DDSProject

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=8 root@$VM"
mkdir -p "$LOGS"

FAIL=0
pass() { printf '  %-40s PASS  %s\n' "$1" "$2"; }
fail() { printf '  %-40s FAIL  %s\n' "$1" "$2"; FAIL=$((FAIL + 1)); }

WSL_IP=$(ip -4 -o addr show eth0 2>/dev/null | awk '{print $4}' | cut -d/ -f1)

echo "=== 跨主机工业级压测 ==="
echo "  WSL=$WSL_IP  虚拟机=$VM  domain=$DOMAIN"
$SSH 'test -x /reactor/DDSProject/build/tools/stratum_rtps_record' 2>/dev/null \
    || { echo "  虚拟机不可用或工具缺失"; exit 1; }
echo

# ---------------------------------------------------------------------------
# A. 多 topic 大数据量
# ---------------------------------------------------------------------------
echo "=== A. 多 topic 大数据量（2 topic 混合 + 8KB payload）==="
A_T1=1500; A_T2=900; A_PB=8000
TLA="$TOOLS/st_multi"; rm -rf "$TLA"
"$TOOLS/make_timeline" "$TLA" "$A_T1" "$A_PB" "$A_T2" > /dev/null 2>&1
total_a=$((A_T1 + A_T2))
echo "  计划：topic1=$A_T1 + topic2=$A_T2 = $total_a 条，每条 ${A_PB}B"

$SSH "rm -rf $VM_DIR/build/tl_st_multi" 2>/dev/null
$SSH "cd $VM_DIR && STRATUM_ADVERTISED_ADDRESS=$VM_ADV STRATUM_INITIAL_PEERS=$WSL_ADV \
      timeout 150 ./build/tools/stratum_rtps_record 120 $DOMAIN $VM_DIR/build/tl_st_multi" \
    > "$LOGS/st_multi_rec.log" 2>&1 &
REC=$!
sleep 18
t0=$(date +%s.%N)
STRATUM_ADVERTISED_ADDRESS="$WSL_ADV" STRATUM_INITIAL_PEERS="$VM_ADV" \
    timeout 130 "$TOOLS/stratum_rtps_replay" "$TLA" "$DOMAIN" 0 2>&1 \
    | grep -aE "已发布样本|发送窗口" | tail -2 | sed 's/^/  回放: /'
t1=$(date +%s.%N)
sleep 12
kill $REC 2>/dev/null; wait $REC 2>/dev/null
$SSH "pkill -f 'stratum_rtps_record .*tl_st_multi' 2>/dev/null; sleep 2; true" 2>/dev/null

echo "  虚拟机录制统计:"
$SSH "cd $VM_DIR && timeout 30 ./build/tools/stratum_rtps_replay $VM_DIR/build/tl_st_multi $DOMAIN 0 2>&1 | head -8" 2>&1 \
    | grep -aE "待回放话题|条$|个）" | sed 's/^/    /'

got_a=$($SSH "cd $VM_DIR && timeout 30 ./build/tools/stratum_rtps_replay $VM_DIR/build/tl_st_multi $DOMAIN 0 2>&1 | grep -oE '[0-9]+ 条' | awk '{s+=\$1} END {print s+0}'" 2>/dev/null | tail -1)
elapsed=$(echo "$t1 - $t0" | bc 2>/dev/null || echo "?")
echo "  合计录制 = ${got_a:-0} 条（计划 $total_a），回放耗时 ${elapsed}s"
if [ "${got_a:-0}" -ge $((total_a * 95 / 100)) ]; then
    pass "多 topic 大数据量" "收到 ${got_a}/${total_a}"
else
    fail "多 topic 大数据量" "收到 ${got_a:-0}/${total_a}"
fi
echo

# ---------------------------------------------------------------------------
# B. 吞吐阶梯
# ---------------------------------------------------------------------------
echo "=== B. 吞吐阶梯（2KB payload，同一批数据不同速率）==="
for rate in 50 200 1000; do
    TLB="$TOOLS/st_rate"; rm -rf "$TLB"
    "$TOOLS/make_timeline" "$TLB" 400 2000 > /dev/null 2>&1
    $SSH "rm -rf $VM_DIR/build/tl_st_rate" 2>/dev/null
    $SSH "cd $VM_DIR && STRATUM_ADVERTISED_ADDRESS=$VM_ADV STRATUM_INITIAL_PEERS=$WSL_ADV \
          timeout 120 ./build/tools/stratum_rtps_record 90 $DOMAIN $VM_DIR/build/tl_st_rate" \
        > "$LOGS/st_rate${rate}_rec.log" 2>&1 &
    REC=$!
    sleep 16
    STRATUM_ADVERTISED_ADDRESS="$WSL_ADV" STRATUM_INITIAL_PEERS="$VM_ADV" \
        timeout 100 "$TOOLS/stratum_rtps_replay" "$TLB" "$DOMAIN" "$rate" > "$LOGS/st_rate${rate}_rep.log" 2>&1
    sleep 10
    kill $REC 2>/dev/null; wait $REC 2>/dev/null
    $SSH "pkill -f 'stratum_rtps_record .*tl_st_rate' 2>/dev/null; sleep 2; true" 2>/dev/null
    n=$($SSH "cd $VM_DIR && timeout 25 ./build/tools/stratum_rtps_replay $VM_DIR/build/tl_st_rate $DOMAIN 0 2>&1 | grep -oE '[0-9]+ 条' | awk '{s+=\$1} END {print s+0}'" 2>/dev/null | tail -1)
    echo "  速率 ${rate}/s: 收到 ${n:-0} / 400"
    if [ "${n:-0}" -ge 380 ]; then
        pass "吞吐 ${rate}/s" "收到 ${n}/400"
    else
        fail "吞吐 ${rate}/s" "收到 ${n:-0}/400"
    fi
done
echo

# ---------------------------------------------------------------------------
# C. 随机测试
# ---------------------------------------------------------------------------
echo "=== C. 随机测试（${ROUNDS} 轮）==="
for r in $(seq 1 "$ROUNDS"); do
    pb=$(( (RANDOM % 5) * 2000 ))          # 0 / 2K / 4K / 6K / 8K
    cnt=$(( 100 + RANDOM % 400 ))          # 100~499
    t2=$(( RANDOM % (cnt / 2 + 1) ))       # topic2 随机
    rate=$(( 50 + RANDOM % 950 ))
    TLC="$TOOLS/st_rand"; rm -rf "$TLC"
    "$TOOLS/make_timeline" "$TLC" "$cnt" "$pb" "$t2" > /dev/null 2>&1
    want=$((cnt + t2))

    $SSH "rm -rf $VM_DIR/build/tl_st_rand" 2>/dev/null
    $SSH "cd $VM_DIR && STRATUM_ADVERTISED_ADDRESS=$VM_ADV STRATUM_INITIAL_PEERS=$WSL_ADV \
          timeout 140 ./build/tools/stratum_rtps_record 110 $DOMAIN $VM_DIR/build/tl_st_rand" \
        > "$LOGS/st_rand${r}_rec.log" 2>&1 &
    REC=$!
    sleep 16
    STRATUM_ADVERTISED_ADDRESS="$WSL_ADV" STRATUM_INITIAL_PEERS="$VM_ADV" \
        timeout 120 "$TOOLS/stratum_rtps_replay" "$TLC" "$DOMAIN" "$rate" > "$LOGS/st_rand${r}_rep.log" 2>&1
    sleep 10
    kill $REC 2>/dev/null; wait $REC 2>/dev/null
    $SSH "pkill -f 'stratum_rtps_record .*tl_st_rand' 2>/dev/null; sleep 2; true" 2>/dev/null
    n=$($SSH "cd $VM_DIR && timeout 25 ./build/tools/stratum_rtps_replay $VM_DIR/build/tl_st_rand $DOMAIN 0 2>&1 | grep -oE '[0-9]+ 条' | awk '{s+=\$1} END {print s+0}'" 2>/dev/null | tail -1)
    printf '  第 %2s 轮  payload=%-6s count=%-4s topic2=%-4s rate=%-5s -> 收到 %-5s / %s  %s\n' \
        "$r" "${pb}B" "$cnt" "$t2" "$rate" "${n:-0}" "$want" \
        "$( [ "${n:-0}" -ge $((want * 95 / 100)) ] && echo '✅' || echo '❌')"
    if [ "${n:-0}" -lt $((want * 95 / 100)) ]; then
        fail "随机第 ${r} 轮" "收到 ${n:-0}/$want"
    fi
done
echo

echo "=========================================="
if [ "$FAIL" -eq 0 ]; then
    echo "判定：PASS —— 跨主机工业级压测全部通过"
else
    echo "判定：$FAIL 项不达标"
fi
exit $((FAIL > 0 ? 1 : 0))
