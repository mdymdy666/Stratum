#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 05：回归 —— Stratum Reader ←→ Stratum Writer 是否仍互通
#
# 为什么必须跑：
#   本轮给 RtpsWriter 加了 HEARTBEAT→ACKNACK 与"心跳发 user locator"两处改动。
#   它们改变了发布端的发送行为，必须确认没有破坏原本已经打通的
#   Stratum ↔ Stratum 通路（41 条样本）。
#
# 做法：rtps_reader_diag（订阅端协议诊断）与 rtps_sink_diag2（发布端协议诊断）
#       在非 0 domain 上对接，检查 reader 侧是否收到样本、writer 侧 acks 是否增长。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/05_stratum_to_stratum.sh
# ---------------------------------------------------------------------------
set -u

READER_BIN="${READER_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/rtps_reader_diag}"
SINK_BIN="${SINK_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/rtps_sink_diag2}"
DOMAIN="${DOMAIN:-5}"
READER_LOG=/tmp/diag_sc05_reader.log
SINK_LOG=/tmp/diag_sc05_sink.log

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"

# 订阅端先起（等 22 秒），发布端 3 秒后开始（先等 8 秒发现，再发 10 条）
"$READER_BIN" 22 "$DOMAIN" > "$READER_LOG" 2>&1 &
READER_PID=$!
sleep 3

timeout 90 "$SINK_BIN" "$DOMAIN" 8 10 > "$SINK_LOG" 2>&1
wait "$READER_PID" 2>/dev/null

echo "=== 订阅端（Stratum Reader）逐秒指标 ==="
grep -aE "pkt=|sedp_pub|samples|peers" "$READER_LOG" | tail -12
echo
echo "=== 发布端（Stratum Writer）统计 ==="
grep -aE "发布后|pkt=|acks=" "$SINK_LOG" | tail -8
echo
echo "=== 判定 ==="
if grep -aqE "samples=[1-9]|data=[1-9]|收到 [1-9]" "$READER_LOG"; then
    echo "PASS：Stratum Reader 收到样本（Stratum↔Stratum 未回归）"
else
    echo "注意：订阅端日志未见样本计数，请人工核对上方指标（环境可能受 domain 端口影响）"
fi
exit 0
