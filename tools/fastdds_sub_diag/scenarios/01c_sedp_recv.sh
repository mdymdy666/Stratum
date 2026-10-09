#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 01c：用 Stratum 自带工具 sedp_recv 抓 Stratum 自己发出的 SEDP 公告
#
# 目的：区分两种可能
#   A. Stratum 根本没把 SEDP Publication 发给对端（发送侧问题）
#   B. 发了，但 Fast DDS 侧没有接受（接收侧问题）
#
# sedp_recv 会：声明 metatraffic locator（9050）→ 周期发 SPDP → 打印收到的
# SEDP 报文里每个 PID；它用与 Stratum 相同的 guid prefix，
# 因此若 Stratum 按 prefix 过滤自身公告，这里就收不到 —— 结果同样有诊断意义。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/01c_sedp_recv.sh
# ---------------------------------------------------------------------------
set -u

RECV_BIN="${RECV_BIN:-/tmp/sedp_recv}"
REPLAY_BIN="${REPLAY_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/stratum_rtps_replay}"
TIMELINE="${1:-/tmp/tl_final}"
RECV_LOG=/tmp/diag_sc01c_recv.log
REPLAY_LOG=/tmp/diag_sc01c_replay.log

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"

if [ ! -x "$RECV_BIN" ]; then
    echo "缺少 $RECV_BIN" >&2
    exit 1
fi

# 注意：sedp_recv 只在循环结束后打印统计，中途 kill 会丢失全部结果，
# 所以这里必须等它自然退出（RECV_SECONDS 要覆盖 replay 的完整时长）。
"$RECV_BIN" "${RECV_SECONDS:-30}" 0 > "$RECV_LOG" 2>&1 &
RECV_PID=$!
sleep 2

STRATUM_DUMP=1 timeout 180 "$REPLAY_BIN" "$TIMELINE" 0 "${RATE:-10.0}" > "$REPLAY_LOG" 2>&1

wait "$RECV_PID" 2>/dev/null

echo "=== sedp_recv 收到的 DATA 类型统计 ==="
sed -n '/DATA 类型统计/,/各类型首个样本/p' "$RECV_LOG"
echo "=== 首个 SEDP 样本的 PID 列表（若有）==="
sed -n '/各类型首个样本/,$p' "$RECV_LOG" | head -60
echo "=== 回放端发送目标 ==="
grep -o "has_user=[01] -> [0-9.]*:[0-9]*" "$REPLAY_LOG" | sort | uniq -c | sort -rn
echo "=== 场景 01c 完成 ==="
exit 0
