#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 10（压测）：吞吐、速率-完整率阶梯、录制闭环完整率
#
# 【为什么吞吐必须取 Player 的"发送窗口"】
#   外部用 shell 计时会把进程启动（Fast DDS 初始化 + SPDP 发现 ~1-2 秒）
#   与收尾的 ACK 等待（最多 ack_timeout_ms = 5 秒）一起算进去。
#   曾据此得出错误的「420 条/秒」；Player 内部统计发送窗口后实测为
#   **≈8.5 万条/秒**。做性能结论时务必看清计时区间。
#
# 【延迟指标的语义边界】
#   回放的 INFO_TS 承载样本的**源时间戳**（语义正确），而回放是"时间旅行"：
#   从原始生成到回放之间的间隔会被算进 now-source，因此**回放方向测不出
#   传输延迟**。延迟须在录制方向实时发布下测，或改测到达抖动。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/10_bench.sh [count]
# 产物：终端表格 + build/tools/logs/bench_*.log
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
DOMAIN="${DOMAIN:-7}"
COUNT="${1:-500}"
INTERVAL_MS=20   # make_timeline 的样本间隔；rate=1.0 对应 1000/INTERVAL_MS 条/秒

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"
mkdir -p "$LOGS"

TL="$TOOLS/tl_bench_rate"
rm -rf "$TL"
"$TOOLS/make_timeline" "$TL" "$COUNT" > "$LOGS/bench_gen.log" 2>&1

echo "=== 回放方向：速率阶梯（每档 $COUNT 条，样本间隔 ${INTERVAL_MS}ms）==="
printf '%-8s %-14s %-16s %-8s %-8s %s\n' "rate" "目标(条/秒)" "实测窗口(条/秒)" "发送" "收到" "完整率"
printf '%.0s-' $(seq 1 78); echo

run_replay() {
    local rate="$1"
    local target
    target=$(awk "BEGIN{ if ($rate <= 0) print \"尽快\"; else printf \"%.0f\", 1000.0/$INTERVAL_MS*$rate }")
    local diag_secs=60
    timeout $((diag_secs + 20)) "$TOOLS/fastdds_sub_diag" "$diag_secs" "$DOMAIN" hello_world_topic \
        > "$LOGS/bench_r${rate}_diag.log" 2>&1 &
    local diag_pid=$!
    sleep 5
    timeout 150 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" "$rate" > "$LOGS/bench_r${rate}_rep.log" 2>&1
    wait "$diag_pid" 2>/dev/null

    local win sent recv pct
    win=$(grep -a "发送窗口" "$LOGS/bench_r${rate}_rep.log" | head -1 | grep -o "= [0-9]* 条/秒" | grep -o "[0-9]*")
    sent=$(grep -a "已发布样本" "$LOGS/bench_r${rate}_rep.log" | head -1 | grep -o "[0-9]*" | head -1)
    recv=$(grep -a DIAG_SUMMARY "$LOGS/bench_r${rate}_diag.log" | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)
    pct=$(awk "BEGIN{ printf \"%.1f%%\", 100.0*${recv:-0}/${COUNT} }")
    printf '%-8s %-14s %-16s %-8s %-8s %s\n' "$rate" "$target" "${win:-?}" "${sent:-?}" "${recv:-?}" "$pct"
}

run_replay 1.0
run_replay 100
run_replay 500
run_replay 2000
run_replay 0

echo
echo "=== 录制方向：Stratum 回放 → Stratum 录制（闭环完整率）==="
printf '%-8s %-8s %-8s %s\n' "rate" "发送" "录到" "完整率"
printf '%.0s-' $(seq 1 44); echo

run_record() {
    local rate="$1"
    local tl_out="$TOOLS/tl_bench_rec_${rate}"
    rm -rf "$tl_out"
    local rec_secs=45
    timeout $((rec_secs + 20)) "$TOOLS/stratum_rtps_record" "$rec_secs" "$DOMAIN" "$tl_out" \
        > "$LOGS/bench_rec${rate}_rec.log" 2>&1 &
    local rec_pid=$!
    sleep 5
    timeout 150 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" "$rate" > "$LOGS/bench_rec${rate}_rep.log" 2>&1
    wait "$rec_pid" 2>/dev/null

    local sent rec pct
    sent=$(grep -a "已发布样本" "$LOGS/bench_rec${rate}_rep.log" | head -1 | grep -o "[0-9]*" | head -1)
    rec=$(grep -a "hello_world_topic " "$LOGS/bench_rec${rate}_rec.log" | head -1 | awk '{print $(NF-1)}')
    pct=$(awk "BEGIN{ printf \"%.1f%%\", 100.0*${rec:-0}/${sent:-1} }")
    printf '%-8s %-8s %-8s %s\n' "$rate" "${sent:-?}" "${rec:-?}" "$pct"
}

run_record 50
run_record 0

echo
echo "日志目录：$LOGS（bench_*）"
