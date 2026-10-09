#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 06：抓"诊断程序自己发出的 SPDP"，看它公告的 metatraffic 端口到底是多少
#
# 为什么需要：
#   Stratum 从 SPDP 学到的 peer 是 `172.17.211.143:7438`，而诊断程序是用
#   `--meta-port=9410` 启动的。必须区分两种可能：
#     A. Fast DDS 没有按我们指定的 locator 公告（设置未生效）；
#     B. Stratum 的 SpdpParser 解析错了（报文里其实是 9410）。
#   做法：让 capture_udp 占住诊断程序的 initial peer 端口（Stratum 不启动），
#   诊断程序会把 SPDP 单播过来；然后用 analyze_capture.py --dump-pids
#   直接读报文里的 PID 0x0031/0x0032 locator 值。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/06_spdp_selfcheck.sh
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
CAP_BIN="${CAP_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/capture_udp}"
DIAG_BIN="${DIAG_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/fastdds_sub_diag}"
DOMAIN="${DOMAIN:-3}"
META_PORT="${META_PORT:-9410}"
USER_PORT="${USER_PORT:-9411}"
PEER_PORT="${PEER_PORT:-9090}"
CAP_OUT=/tmp/diag_sc06_capture.bin
CAP_LOG=/tmp/diag_sc06_capture.log
SUB_LOG=/tmp/diag_sc06_sub.log

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"

"$CAP_BIN" 18 "$DOMAIN" "$PEER_PORT" "$CAP_OUT" 9 > "$CAP_LOG" 2>&1 &
CAP_PID=$!
sleep 2

timeout 40 "$DIAG_BIN" 10 "$DOMAIN" hello_world_topic \
    --meta-port="$META_PORT" --user-port="$USER_PORT" --peer="127.0.0.1:$PEER_PORT" \
    > "$SUB_LOG" 2>&1

wait "$CAP_PID" 2>/dev/null

echo "=== 抓到的报文（前 6 条）==="
grep -a "^\[rx" "$CAP_LOG" | head -6
echo
echo "=== SPDP 参数表（locator 已解成 ip:port）==="
python3 "$ROOT/tools/fastdds_sub_diag/analyze_capture.py" "$CAP_OUT" --dump-pids 2>&1 | tail -40
echo
echo "=== 诊断端启动参数确认 ==="
grep -aE "初始对端|reader 就绪|domain=" "$SUB_LOG" | head -5
echo "=== 场景 06 完成 ==="
exit 0
