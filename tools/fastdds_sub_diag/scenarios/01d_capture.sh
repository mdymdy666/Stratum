#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 01d（决定性）：用原始抓包确认 Stratum 是否发出 SEDP Publication
#
# 做法：capture_udp 以一个最小 RTPS 参与者身份接收（不做任何语义解析），
#       把 Stratum 发来的报文原样落盘；再用 analyze_capture.py 统计
#       各 writer entity 的 DATA 到达数量。
#
# 判据：
#   · 若出现 SEDP_PUB_W  → Stratum 确实在发 Publication，问题在 Fast DDS 接收/解析侧
#   · 若只有 USER_W      → Stratum 从未把 Publication 发到这个 peer（发送侧问题）
#
# 用法：bash tools/fastdds_sub_diag/scenarios/01d_capture.sh
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
CAP_BIN="${CAP_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/capture_udp}"
REPLAY_BIN="${REPLAY_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/stratum_rtps_replay}"
TIMELINE="${1:-/tmp/tl_final}"
CAP_PORT="${CAP_PORT:-9090}"
CAP_SECONDS="${CAP_SECONDS:-35}"
CAP_OUT=/tmp/diag_sc01d_capture.bin
CAP_LOG=/tmp/diag_sc01d_capture.log
REPLAY_LOG=/tmp/diag_sc01d_replay.log

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"

"$CAP_BIN" "$CAP_SECONDS" 0 "$CAP_PORT" "$CAP_OUT" 9 > "$CAP_LOG" 2>&1 &
CAP_PID=$!
sleep 3

STRATUM_DUMP=1 timeout 180 "$REPLAY_BIN" "$TIMELINE" 0 "${RATE:-10.0}" > "$REPLAY_LOG" 2>&1

wait "$CAP_PID" 2>/dev/null

echo "=== 抓包器前 12 条报文 ==="
grep -E "^\[rx" "$CAP_LOG" | head -12
echo
echo "=== 抓包统计 ==="
python3 "$ROOT/tools/fastdds_sub_diag/analyze_capture.py" "$CAP_OUT" \
    --dump-writer 000003c2 --out /tmp/diag_sc01d_sedp_pub.bin
echo
echo "=== Stratum 发送目标（前 6 条去重）==="
grep -o "has_user=[01] -> [0-9.]*:[0-9]*" "$REPLAY_LOG" | sort | uniq -c | sort -rn | head -6
echo "=== 场景 01d 完成 ==="
exit 0
