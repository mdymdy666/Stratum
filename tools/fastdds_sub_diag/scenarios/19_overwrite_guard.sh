#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 19（O19 断言）：录制器不得静默清空已有时间线
#
# 背景：`stratum_rtps_record` 此前**无条件** `rm -rf <dir>`，导致"重启录制器"
# 会静默毁掉历史数据（实测 301 条 → 0 条）。修复后：
#   · 目录非空且无 `--overwrite` ⇒ **明确拒绝**（退出码 3），数据保持不动；
#   · 带 `--overwrite`          ⇒ 允许清空。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/19_overwrite_guard.sh
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
DOMAIN="${DOMAIN:-21}"

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"
mkdir -p "$LOGS"

TL="$TOOLS/tl_guard"
TL_IN="$TOOLS/tl_guard_in"
FAIL=0
rm -rf "$TL" "$TL_IN"

# 造一个"非空"时间线：先正常录一小段
"$TOOLS/make_timeline" "$TL_IN" 120 > /dev/null 2>&1
"$TOOLS/stratum_rtps_record" 20 "$DOMAIN" "$TL" > "$LOGS/guard_rec0.log" 2>&1 &
REC=$!
sleep 5
timeout 25 "$TOOLS/stratum_rtps_replay" "$TL_IN" "$DOMAIN" 1.0 > /dev/null 2>&1
sleep 6
kill "$REC" 2>/dev/null
for _ in $(seq 1 50); do kill -0 "$REC" 2>/dev/null || break; sleep 0.2; done
kill -9 "$REC" 2>/dev/null; wait "$REC" 2>/dev/null

before=$(grep -a "hello_world_topic" <(timeout 30 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 1.0 2>&1) | head -1 | grep -o "[0-9]* 条" | grep -o "[0-9]*")
echo "=== 前置：时间线里已有 ${before:-0} 条 ==="

echo
echo "=== 用例 1：非空目录 + 无 --overwrite ⇒ 必须拒绝 ==="
"$TOOLS/stratum_rtps_record" 8 "$DOMAIN" "$TL" > "$LOGS/guard_rec1.log" 2>&1
rc1=$?
after1=$(grep -a "hello_world_topic" <(timeout 30 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 1.0 2>&1) | head -1 | grep -o "[0-9]* 条" | grep -o "[0-9]*")
echo "  退出码=$rc1（期望 3）；拒绝提示：$(grep -a REFUSED "$LOGS/guard_rec1.log" | head -1 | cut -c1-60)"
echo "  时间线数据：${before:-0} → ${after1:-0}"
if [ "$rc1" -eq 3 ] && [ "${after1:-0}" -ge "${before:-1}" ]; then
    echo "  → PASS（明确拒绝，且旧数据完好）"
else
    echo "  → FAIL"
    FAIL=$((FAIL + 1))
fi

echo
echo "=== 用例 2：带 --overwrite ⇒ 允许清空 ==="
"$TOOLS/stratum_rtps_record" 6 "$DOMAIN" "$TL" --overwrite > "$LOGS/guard_rec2.log" 2>&1 &
REC2=$!
sleep 8
kill "$REC2" 2>/dev/null
for _ in $(seq 1 50); do kill -0 "$REC2" 2>/dev/null || break; sleep 0.2; done
kill -9 "$REC2" 2>/dev/null; wait "$REC2" 2>/dev/null
echo "  覆盖提示：$(grep -a "已按 --overwrite 清空" "$LOGS/guard_rec2.log" | head -1 | cut -c1-50)"
if grep -qa "已按 --overwrite 清空" "$LOGS/guard_rec2.log"; then
    echo "  → PASS（显式覆盖被允许）"
else
    echo "  → FAIL（--overwrite 未生效）"
    FAIL=$((FAIL + 1))
fi

echo
echo "=== 用例 3：--help 应说明默认行为 ==="
if "$TOOLS/stratum_rtps_record" --help 2>&1 | grep -qa "拒绝"; then
    echo "  → PASS（帮助里说明了默认拒绝）"
else
    echo "  → FAIL（帮助未说明）"
    FAIL=$((FAIL + 1))
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "判定：PASS（O19 已修复：不会静默清空已有时间线）"
else
    echo "判定：FAIL（$FAIL 项）"
fi
exit $((FAIL > 0 ? 1 : 0))
