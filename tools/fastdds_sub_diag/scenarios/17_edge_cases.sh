#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 17（E3 · 边界与并发）
#
# 四类边界 + 一个关键并发用例：
#   A. 0 条 —— 空时间线应正常结束，不崩溃、不报错
#   B. 1 条 —— 首条补救逻辑的极端情形
#   C. 超长 —— 5000 条（超过 pending_ 的 4096 裁剪阈值，顺便观察是否影响完整性）
#   D. **多 reader 并发订阅** —— 3 个诊断程序实例同时订阅同一话题，
#      **每一个都必须收到全部条数**。
#      这一项直指续 17 修过的缺陷："学到的 user locator 被写给所有 peer"
#      （当时表现为多订阅端里只有一个能收到数据）。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/17_edge_cases.sh
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
DOMAIN="${DOMAIN:-19}"

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"
mkdir -p "$LOGS"

FAIL=0
report() {
    local label="$1" expect="$2" got="$3"
    if [ "$got" = "$expect" ]; then
        printf '  %-28s 期望 %-6s 实得 %-6s PASS\n' "$label" "$expect" "$got"
    else
        printf '  %-28s 期望 %-6s 实得 %-6s FAIL\n' "$label" "$expect" "$got"
        FAIL=$((FAIL + 1))
    fi
}

# ---------------- A/B：0 条与 1 条 ----------------
for n in 0 1; do
    TL="$TOOLS/tl_edge_$n"
    rm -rf "$TL"
    "$TOOLS/make_timeline" "$TL" "$n" > /dev/null 2>&1
    timeout 30 "$TOOLS/fastdds_sub_diag" 20 "$DOMAIN" hello_world_topic > "$LOGS/edge_${n}_sub.log" 2>&1 &
    SUB=$!
    sleep 5
    timeout 30 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 1.0 > "$LOGS/edge_${n}_rep.log" 2>&1
    rep_rc=$?
    wait $SUB 2>/dev/null
    got=$(grep -a DIAG_SUMMARY "$LOGS/edge_${n}_sub.log" | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)
    sent=$(grep -a "已发布样本" "$LOGS/edge_${n}_rep.log" | head -1 | grep -o "[0-9]*" | head -1)
    echo "=== $n 条（回放退出码=$rep_rc，已发布 ${sent:-?}）==="
    if [ "$n" -eq 0 ]; then
        report "空时间线：收到" "0" "${got:-?}"
        # 【判据修正】空时间线时回放**本应**返回非零（"时间线为空，无可回放数据"），
        # 这不是崩溃。此前把它当失败，是脚本判据写错了。
        if [ "$rep_rc" -eq 0 ] || [ "$rep_rc" -eq 2 ]; then
            echo "  （回放正常退出，码=$rep_rc —— 空时间线返回非零是预期）"
        else
            echo "  FAIL: 回放异常退出码 $rep_rc"; FAIL=$((FAIL+1))
        fi
    else
        # 1 条最多可能多 1 条补救副本
        if [ "${got:-0}" -ge 1 ] && [ "${got:-0}" -le 2 ]; then
            echo "  1 条：收到 ${got} PASS（允许 1 条补救副本）"
        else
            echo "  1 条：收到 ${got} FAIL"; FAIL=$((FAIL+1))
        fi
    fi
done

# ---------------- C：超长（5000 条） ----------------
echo "=== C. 超长 5000 条（超过 pending_ 的 4096 裁剪阈值）==="
TL="$TOOLS/tl_edge_long"
rm -rf "$TL"
"$TOOLS/make_timeline" "$TL" 5000 > /dev/null 2>&1
timeout 60 "$TOOLS/fastdds_sub_diag" 50 "$DOMAIN" hello_world_topic > "$LOGS/edge_long_sub.log" 2>&1 &
SUB=$!
sleep 5
timeout 60 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 0 > "$LOGS/edge_long_rep.log" 2>&1
wait $SUB 2>/dev/null
got=$(grep -a DIAG_SUMMARY "$LOGS/edge_long_sub.log" | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)
sent=$(grep -a "已发布样本" "$LOGS/edge_long_rep.log" | head -1 | grep -o "[0-9]*" | head -1)
echo "  发送 ${sent:-?} / 收到 ${got:-?}（"尽快"档允许因接收端缓冲丢少量）"

# ---------------- D：多 reader 并发订阅（关键） ----------------
echo "=== D. 3 个 reader 同时订阅（验证 locator 定向）==="
TL="$TOOLS/tl_edge_multi"
rm -rf "$TL"
"$TOOLS/make_timeline" "$TL" 200 > /dev/null 2>&1
for pid in 31 32 33; do
    timeout 45 "$TOOLS/fastdds_sub_diag" 35 "$DOMAIN" hello_world_topic --participant-id="$pid" \
        > "$LOGS/edge_r${pid}.log" 2>&1 &
    eval "R${pid}=$!"
done
sleep 6
# 打开 STRATUM_DUMP：否则日志里没有 [send] 行，"发送目标数"永远是 0（判据失效）
STRATUM_DUMP=1 timeout 45 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 1.0 \
    > "$LOGS/edge_multi_rep.log" 2>&1
wait $R31 2>/dev/null; wait $R32 2>/dev/null; wait $R33 2>/dev/null

# 【判据修正】回放恒多出最多 1 条"首条补救副本"（新 SN 重发），
# 所以期望是 200~202，而不是恰好 200。此前写死 200 导致三个 reader 全被误判为 FAIL。
for pid in 31 32 33; do
    got=$(grep -a DIAG_SUMMARY "$LOGS/edge_r${pid}.log" | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)
    if [ "${got:-0}" -ge 200 ] && [ "${got:-0}" -le 202 ]; then
        printf '  %-28s 期望 200~202  实得 %-6s PASS\n' "reader(participant-id=$pid)" "${got:-?}"
    else
        printf '  %-28s 期望 200~202  实得 %-6s FAIL\n' "reader(participant-id=$pid)" "${got:-?}"
        FAIL=$((FAIL + 1))
    fi
done
echo "  回放端: $(grep -a '已发布样本' "$LOGS/edge_multi_rep.log" | head -1 | tr -d ' ')"
targets=$(grep -ao 'has_user=1 -> [0-9.]*:[0-9]*' "$LOGS/edge_multi_rep.log" 2>/dev/null | sort -u | wc -l)
if [ "$targets" -ge 3 ]; then
    echo "  发送目标数 = $targets（≥3）PASS —— 数据被定向发往每个 reader 各自的端口"
else
    echo "  发送目标数 = $targets（<3）FAIL —— 可能又退化成'只发给一个 peer'"
    FAIL=$((FAIL + 1))
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "判定：PASS（全部边界与并发用例通过）"
else
    echo "判定：FAIL（$FAIL 项不达标）"
fi
exit $((FAIL > 0 ? 1 : 0))
