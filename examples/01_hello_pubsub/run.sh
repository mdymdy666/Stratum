#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 示例 01：最小发布订阅（Stratum 回放 -> Fast DDS 订阅端）
#
# 验证什么：Stratum 作为 DataWriter 能否被标准 DDS 实现（Fast DDS）正常发现、
#           匹配并接收数据。这是"能不能互通"的最小闭环。
#
# 期望：Fast DDS 订阅端收到 32 条（+1 条首条补救副本，30 条以上即通过）。
#
# 用法：bash examples/01_hello_pubsub/run.sh
# ---------------------------------------------------------------------------
set -u

DOMAIN="${DOMAIN:-70}"
COUNT="${COUNT:-32}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu
mkdir -p "$LOGS"

for t in make_timeline stratum_rtps_replay fastdds_sub_diag; do
    [ -x "$TOOLS/$t" ] || {
        echo "缺少 $TOOLS/$t —— 请先执行："
        echo "  cmake -S . -B build -G Ninja && cmake --build build"
        echo "  bash tools/fastdds_sub_diag/build_tools.sh"
        exit 1
    }
done

echo "=== 示例 01：最小发布订阅（domain=$DOMAIN）==="
TL="$TOOLS/ex01_tl"; rm -rf "$TL"
"$TOOLS/make_timeline" "$TL" "$COUNT" > /dev/null 2>&1
echo "  已生成时间线：$COUNT 条（$TL）"

echo "  启动 Fast DDS 订阅端 ..."
timeout 70 "$TOOLS/fastdds_sub_diag" 45 "$DOMAIN" hello_world_topic \
    > "$LOGS/ex01_sub.log" 2>&1 &
SUB=$!
sleep 8

echo "  开始回放 ..."
timeout 60 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 10.0 \
    > "$LOGS/ex01_rep.log" 2>&1
wait $SUB 2>/dev/null

pub=$(grep -a "已发布样本" "$LOGS/ex01_rep.log" | head -1 | tr -d ' ')
got=$(grep -a "DIAG_SUMMARY" "$LOGS/ex01_sub.log" \
      | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)
echo
echo "  $pub"
echo "  订阅端收到 samples = ${got:-0}（期望 >= $COUNT）"
echo
if [ "${got:-0}" -ge "$COUNT" ]; then
    echo "  ✅ PASS —— Stratum 与 Fast DDS 互通正常"
    exit 0
fi
echo "  ❌ FAIL —— 未收到预期数据，详见 $LOGS/ex01_sub.log"
exit 1
