#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 01b：端口对照（Stratum 的发送目标 vs 诊断程序实际监听的 UDP 端口）
#
# 为什么需要它：
#   场景 01 显示 Fast DDS 既没有触发 on_data_writer_discovery，也没有任何
#   RTPS_EDP 日志。必须先回答一个更基本的问题：
#     Stratum 到底有没有把 SEDP Publication 发到我们这个 participant 的
#     metatraffic 端口？
#   本脚本用 ss 列出诊断程序真实监听的端口，并与 STRATUM_DUMP=1 打印的
#   [send] 目标做对照。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/01b_port_compare.sh
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIAG_BIN="${DIAG_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/fastdds_sub_diag}"
REPLAY_BIN="${REPLAY_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/stratum_rtps_replay}"
TIMELINE="${1:-/tmp/tl_final}"
SUB_LOG=/tmp/diag_sc01b_sub.log
REPLAY_LOG=/tmp/diag_sc01b_replay.log
SS_LOG=/tmp/diag_sc01b_ss.txt

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"

"$DIAG_BIN" 60 0 hello_world_topic > "$SUB_LOG" 2>&1 &
DIAG_PID=$!
sleep 4

echo "=== 诊断程序监听的 UDP 端口 ==="
ss -ulpn 2>/dev/null | grep -E "fastdds_sub_diag" | tee "$SS_LOG" || echo "(ss 需要权限或未匹配到)"

STRATUM_DUMP=1 timeout 180 "$REPLAY_BIN" "$TIMELINE" 0 "${RATE:-10.0}" > "$REPLAY_LOG" 2>&1
sleep 2

kill "$DIAG_PID" 2>/dev/null
wait "$DIAG_PID" 2>/dev/null

echo "=== Stratum 发送目标统计（[send] 行）==="
grep -o "has_user=[01] -> [0-9.]*:[0-9]*" "$REPLAY_LOG" | sort | uniq -c | sort -rn || echo "(没有 [send] 行)"

echo "=== Stratum 是否发出过 SEDP Publication（pubs 计数来自回放端的统计）==="
grep -E "回放结果|已发布样本|Play 返回值" "$REPLAY_LOG" | head

echo "=== 诊断端：是否发现 Stratum 参与者 ==="
grep -E "\[diag\]\[PDP\].*stratum" "$SUB_LOG" | head

echo "=== 诊断端：是否有任何 hello_world_topic 的 SEDP 结果 ==="
grep -E "\[diag\]\[EDP\].*hello_world_topic" "$SUB_LOG" | head || echo "(无)"

echo "=== 诊断端：Fast DDS 自身日志行数 ==="
grep -cE "^\x1b\[|Warning\]|Error\]" "$SUB_LOG" || true

echo "=== 场景 01b 完成 ==="
exit 0
