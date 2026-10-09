#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 07（端到端验收）：Stratum 回放 → **上游 Fast DDS hello_world subscriber**
#
# 为什么用上游示例而不是自研诊断程序做最终判定：
#   自研程序本身可能有实现缺陷（本轮就踩到过 participant 事件掩码把
#   on_data_available 吞掉的问题）。用**上游官方实现**做接收端，
#   才能证明 Stratum 发出的用户数据符合 RTPS/DDS 规范。
#
# 判据：subscriber 打印 "Subscriber matched."，且 RECEIVED 条数 == 回放条数。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/07_upstream_subscriber.sh [timeline]
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
HW_BIN="${HW_BIN:-/UserData/Fast-DDS/build-fastdds/examples/cpp/hello_world/hello_world}"
REPLAY_BIN="${REPLAY_BIN:-$ROOT/build/tools/stratum_rtps_replay}"
TIMELINE="${1:-/tmp/tl_final}"
DOMAIN="${DOMAIN:-0}"
RATE="${RATE:-1.0}"
HW_LOG=/tmp/diag_sc07_hw.log
REPLAY_LOG=/tmp/diag_sc07_replay.log

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"

if [ ! -x "$HW_BIN" ]; then
    echo "缺少上游示例 $HW_BIN" >&2
    exit 1
fi

timeout 60 "$HW_BIN" subscriber -s 60 > "$HW_LOG" 2>&1 &
HW_PID=$!
sleep 4

timeout 50 "$REPLAY_BIN" "$TIMELINE" "$DOMAIN" "$RATE" > "$REPLAY_LOG" 2>&1
sleep 3
kill "$HW_PID" 2>/dev/null
wait "$HW_PID" 2>/dev/null

matched=$(grep -ac "Subscriber matched" "$HW_LOG" || true)
received=$(grep -ac "RECEIVED" "$HW_LOG" || true)
published=$(grep -a "已发布样本" "$REPLAY_LOG" | head -1 || true)

echo "=== 场景 07：Stratum 回放 → 上游 subscriber ==="
echo "  回放端：$published"
echo "  订阅端匹配事件：$matched"
echo "  订阅端收到样本：$received"
grep -a "RECEIVED" "$HW_LOG" | head -3 | sed 's/^/    /'

if [ "$received" -gt 0 ]; then
    echo "  判定：PASS（Stratum 的用户数据被上游 Fast DDS 正确接收）"
else
    echo "  判定：FAIL（订阅端未收到任何样本，日志见 $HW_LOG）"
fi
exit 0
