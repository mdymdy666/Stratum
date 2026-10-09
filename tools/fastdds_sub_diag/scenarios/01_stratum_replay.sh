#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 01（核心）：Fast DDS 订阅诊断程序 ← Stratum 回放
#
# 目的：拿到 Fast DDS 拒绝 Stratum SEDP Publication 的**确切原因**。
#
# 做法（对应 design/11-rtps-interop-handoff.md 第 1 节）：
#   1. 先起 fastdds_sub_diag（Log::SetVerbosity(Info) + on_data_writer_discovery
#      + on_subscription_matched）
#   2. 再用 tools/stratum_rtps_replay 回放 /tmp/tl_final（32 条 hello_world_topic）
#   3. 收集两侧输出：Fast DDS 的 RTPS_EDP 日志 + 本程序的事件/汇总
#
# 用法：bash tools/fastdds_sub_diag/scenarios/01_stratum_replay.sh [时间线目录]
# 产物：/tmp/diag_sc01_sub.log（诊断端）、/tmp/diag_sc01_replay.log（回放端）
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
DIAG_BIN="${DIAG_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/fastdds_sub_diag}"
REPLAY_BIN="${REPLAY_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/stratum_rtps_replay}"
TIMELINE="${1:-/tmp/tl_final}"
DIAG_SECONDS="${DIAG_SECONDS:-25}"
RATE="${RATE:-10.0}"
# domain 必须可配：domain 0 的 Fast DDS 端口（7400/7410/7411/7438/7439）
# 在本机被 WSL 之外的进程占用（/proc/net/udp 能看到绑定但找不到属主，
# 典型的 mirrored networking），会导致 user 数据平面不可用。
DOMAIN="${DOMAIN:-3}"
SUB_LOG=/tmp/diag_sc01_sub.log
REPLAY_LOG=/tmp/diag_sc01_replay.log

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"

if [ ! -x "$DIAG_BIN" ]; then
    echo "缺少诊断程序 $DIAG_BIN（先 bash tools/fastdds_sub_diag/build.sh）" >&2
    exit 1
fi
if [ ! -x "$REPLAY_BIN" ]; then
    echo "缺少回放程序 $REPLAY_BIN" >&2
    exit 1
fi
if [ ! -d "$TIMELINE" ]; then
    echo "时间线目录不存在：$TIMELINE" >&2
    exit 1
fi

echo "=== 场景 01：诊断程序 ← Stratum 回放（domain=$DOMAIN timeline=$TIMELINE rate=$RATE）==="

"$DIAG_BIN" "$DIAG_SECONDS" "$DOMAIN" hello_world_topic ${DIAG_ARGS:-} > "$SUB_LOG" 2>&1 &
DIAG_PID=$!
sleep 3

if [ "${STRATUM_DUMP:-0}" = "1" ]; then
    export STRATUM_DUMP=1
fi
timeout 180 "$REPLAY_BIN" "$TIMELINE" "$DOMAIN" "$RATE" > "$REPLAY_LOG" 2>&1
REPLAY_RC=$?

# 等诊断程序自然退出：它只在循环结束后打印 DIAG_SUMMARY（机器可解析的汇总）。
# DIAG_SECONDS 必须大于 "3 + 回放时长 + 余量"。
wait "$DIAG_PID" 2>/dev/null

echo "--- 回放端（$REPLAY_LOG，rc=$REPLAY_RC）---"
grep -E "已发布样本|待回放话题|hello_world_topic|Play 返回值|结果：" "$REPLAY_LOG" | head -20

echo "--- 诊断端关键证据（$SUB_LOG）---"
echo "[stratum 参与者发现]"
grep -E "\[diag\]\[PDP\]" "$SUB_LOG" | head -10
echo "[出现 hello_world_topic 的 SEDP 解析结果]"
grep -E "\[diag\]\[EDP\].*hello_world_topic" "$SUB_LOG" | head -10
echo "[Fast DDS 匹配/拒绝理由（RTPS_EDP / INCOMPATIBLE / fallback）]"
grep -E "INCOMPATIBLE|fallback mechanism|RTPS_EDP|EDP\]" "$SUB_LOG" | grep -v "^\[diag\]\[EDP\]" | head -20
echo "[订阅匹配]"
grep -E "\[diag\]\[MATCH\]" "$SUB_LOG" | head -10
echo "[汇总]"
grep -E "DIAG_SUMMARY" "$SUB_LOG"

echo "=== 场景 01 完成 ==="
exit 0
