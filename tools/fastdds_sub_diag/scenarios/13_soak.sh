#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 13（soak）：长时稳定性 —— 录制器长跑，期间反复触发回放
#
# 为什么这样设计：
#   回放器/诊断程序都是短生命周期进程，观察不到泄漏；只有**长跑进程**才能
#   暴露 RSS / 文件描述符的单调增长。因此让 Stratum 录制器连续跑满整段时间，
#   期间每隔一段时间触发一次"生成时间线 → 回放"，并采样录制器的 RSS 与 fd 数。
#
# 判据：
#   · RSS 与 fd 数**不得单调增长**（允许小幅波动）；
#   · 每次回放后录制端条数**只增不减**（无重置/丢数据）；
#   · 无进程崩溃（退出码、日志中的 FAILED）。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/13_soak.sh [minutes] [domain]
# 产物：build/tools/logs/soak.log（可直接引用）
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
MINUTES="${1:-30}"
DOMAIN="${2:-12}"
INTERVAL="${INTERVAL:-30}"          # 每隔多少秒触发一次回放
BATCH="${BATCH:-100}"               # 每次回放的条数

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"
mkdir -p "$LOGS"
SOAK="$LOGS/soak.log"
: > "$SOAK"

TOTAL_SECS=$((MINUTES * 60))
TL_OUT="$TOOLS/tl_soak_out"
rm -rf "$TL_OUT"

echo "soak: 录制器长跑 ${MINUTES} 分钟（domain=$DOMAIN），每 ${INTERVAL}s 触发一次 ${BATCH} 条回放" | tee -a "$SOAK"

timeout $((TOTAL_SECS + 60)) "$TOOLS/stratum_rtps_record" "$TOTAL_SECS" "$DOMAIN" "$TL_OUT" \
    > "$LOGS/soak_record.log" 2>&1 &
REC_PID=$!
sleep 6

printf '%-6s %-10s %-8s %-10s %s\n' "轮次" "RSS(KB)" "FD数" "录制条数" "回放结果" | tee -a "$SOAK"

ROUND=0
END_TS=$(( $(date +%s) + TOTAL_SECS - INTERVAL ))
while [ "$(date +%s)" -lt "$END_TS" ]; do
    ROUND=$((ROUND + 1))
    TL_IN="$TOOLS/tl_soak_in"
    rm -rf "$TL_IN"
    "$TOOLS/make_timeline" "$TL_IN" "$BATCH" > /dev/null 2>&1

    timeout 60 "$TOOLS/stratum_rtps_replay" "$TL_IN" "$DOMAIN" 0 \
        > "$LOGS/soak_replay_${ROUND}.log" 2>&1
    sent=$(grep -a "已发布样本" "$LOGS/soak_replay_${ROUND}.log" | head -1 | grep -o "[0-9]*" | head -1)

    if ! kill -0 "$REC_PID" 2>/dev/null; then
        echo "录制器已退出（第 $ROUND 轮）—— soak 中止" | tee -a "$SOAK"
        break
    fi
    rss=$(awk '/VmRSS/{print $2}' "/proc/$REC_PID/status" 2>/dev/null)
    fds=$(ls "/proc/$REC_PID/fd" 2>/dev/null | wc -l)
    # 录制端的累计条数（每轮结束时的时间线统计）
    recorded=$(grep -a "hello_world_topic " "$LOGS/soak_record.log" 2>/dev/null | tail -1 | awk '{print $(NF-1)}')
    printf '%-6s %-10s %-8s %-10s %s\n' "$ROUND" "${rss:-?}" "${fds:-?}" \
        "${recorded:-(运行中)}" "发送 ${sent:-?}" | tee -a "$SOAK"
    sleep "$INTERVAL"
done

echo "--- 停止录制器并汇总 ---" | tee -a "$SOAK"
kill "$REC_PID" 2>/dev/null
wait "$REC_PID" 2>/dev/null

echo "录制端最终统计：" | tee -a "$SOAK"
grep -aE "hello_world_topic |序号缺口|合计" "$LOGS/soak_record.log" | tail -5 | tee -a "$SOAK"

echo "soak 完成，日志：$SOAK" | tee -a "$SOAK"
