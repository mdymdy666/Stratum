#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 03（对照组）：诊断订阅端 ← **真实 Fast DDS publisher**
#
# 为什么必须有这个对照：
#   此前所有"对端收不到"的结论都建立在"我方诊断程序正常"这一假设上。
#   本场景用上游 hello_world 示例的 publisher（纯 Fast DDS，无我方代码）
#   发数据，若诊断端能收到，则证明：
#     · 诊断程序本身工作正常（不是它在丢数据）
#     · Fast DDS 的 user 平面通路与端口选择正确
#   之后 Stratum 仍收不到，问题就一定在 Stratum 发出的报文/目标上。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/03_fastdds_publisher.sh
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIAG_BIN="${DIAG_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/fastdds_sub_diag}"
PUB_BIN="${PUB_BIN:-/UserData/Fast-DDS/build-fastdds/examples/cpp/hello_world/hello_world}"
SUB_LOG=/tmp/diag_sc03_sub.log
PUB_LOG=/tmp/diag_sc03_pub.log

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"

if [ ! -x "$PUB_BIN" ]; then
    echo "缺少 $PUB_BIN" >&2
    exit 1
fi

"$DIAG_BIN" 30 0 hello_world_topic > "$SUB_LOG" 2>&1 &
DIAG_PID=$!
sleep 4

timeout 40 "$PUB_BIN" publisher -s 10 > "$PUB_LOG" 2>&1
wait "$DIAG_PID" 2>/dev/null

echo "=== Fast DDS publisher 输出（$PUB_LOG）==="
grep -aE "Message|matched|Published|sent" "$PUB_LOG" | head -12
echo
echo "=== 诊断端：匹配 / 数据到达 / 汇总 ==="
grep -aE "\[diag\]\[MATCH\]|\[diag\]\[DATA\]|DIAG_SUMMARY" "$SUB_LOG" | head -20
echo "=== 场景 03 完成 ==="
exit 0
