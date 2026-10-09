#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 02：诊断订阅端 ← Stratum 发布端（直接持有 RtpsSink，带协议层统计）
#
# 与场景 01 的差别：01 用回放工具（只有"发布了 N 条"），本场景用
# tools/rtps_sink_diag2.cc，它逐秒打印 pkt/spdp/sedp_sub/peers/pubs/acks，
# 因此能回答一个关键问题：
#   Fast DDS 到底有没有回 SEDP Subscription（sedp_sub 计数）？
# 注意 sedp_sub 的语义：它是「收到的 Subscription 条数」。对端只公告一次，
# 因此修复 ACKNACK 精确记账后它应停在 1 —— 此前持续增长是因为我们把整段
# SN 说成缺失、导致对端反复重传同一条公告。判据是 sedp_sub > 0，
# 不是「持续增长」。
# 若 sedp_sub > 0 而诊断端 samples 仍为 0，说明数据发送目标（user locator）
# 有问题；若 sedp_sub == 0，说明对端根本没回 Subscription。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/02_direct_sink.sh
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIAG_BIN="${DIAG_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/fastdds_sub_diag}"
SINK_BIN="${SINK_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/rtps_sink_diag2}"
SUB_LOG=/tmp/diag_sc02_sub.log
SINK_LOG=/tmp/diag_sc02_sink.log

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"

"$DIAG_BIN" 45 0 hello_world_topic > "$SUB_LOG" 2>&1 &
DIAG_PID=$!
sleep 4

STRATUM_DUMP=1 timeout 120 "$SINK_BIN" 0 15 10 > "$SINK_LOG" 2>&1

wait "$DIAG_PID" 2>/dev/null

echo "=== Stratum 收到的子消息统计（[rx] 行）==="
grep -o "\[rx\] [A-Za-z]*\( writer=0x[0-9a-f]*\)\?" "$SINK_LOG" | sort | uniq -c | sort -rn | head -15
echo "=== 收到 HEARTBEAT（sm=0x07）总数 ==="
grep -c "\[rx\] sm=0x07" "$SINK_LOG" || true
echo "=== 是否收到 SEDP Subscription 的 DATA（writer=0x000004c2）==="
grep -c "\[rx\] DATA writer=0x000004c2" "$SINK_LOG" || true

echo "=== Stratum 发布端统计（$SINK_LOG）==="
grep -E "pkt=|发布后|发布 [0-9]" "$SINK_LOG" | tail -30
echo
echo "=== 诊断端：匹配 / 数据 / 汇总 ==="
grep -E "\[diag\]\[EDP\].*hello_world_topic|\[diag\]\[MATCH\]|\[diag\]\[DATA\]|DIAG_SUMMARY" "$SUB_LOG" | head -25
echo "=== 场景 02 完成 ==="
exit 0
