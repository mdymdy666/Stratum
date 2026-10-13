#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 示例 02：录制 -> 回放 -> 校验（Stratum 的核心用途）
#
# 验证什么：把 DDS 域中流动的数据录下来，再从录制产物回放出去，
#           并检查**完整性**（条数 + 序号缺口）与**时序保真**。
#
# 流程：
#   fastdds_pub_diag（发布 N 条）
#       -> stratum_rtps_record（录制到磁盘）
#       -> stratum_rtps_replay（按原速回放）
#       -> fastdds_sub_diag（接收并统计）
#
# 期望：录制条数 > 0 且序号缺口为 0；回放后订阅端收到的条数 ≈ 录制条数。
#
# 用法：bash examples/02_record_replay/run.sh
# ---------------------------------------------------------------------------
set -u

DOMAIN="${DOMAIN:-71}"
COUNT="${COUNT:-120}"
RATE="${RATE:-30}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu
mkdir -p "$LOGS"

for t in stratum_rtps_record stratum_rtps_replay fastdds_pub_diag fastdds_sub_diag; do
    [ -x "$TOOLS/$t" ] || { echo "缺少 $TOOLS/$t，请先构建"; exit 1; }
done

TL="$TOOLS/ex02_rec"; rm -rf "$TL"

echo "=== 示例 02：录制 -> 回放（domain=$DOMAIN, $COUNT 条 @ ${RATE}/s）==="

echo "  [1/3] 发布并录制 ..."
"$TOOLS/fastdds_pub_diag" --domain="$DOMAIN" --topic=hello_world_topic \
    --count="$COUNT" --rate="$RATE" > "$LOGS/ex02_pub.log" 2>&1 &
PUB=$!
sleep 4
timeout 90 "$TOOLS/stratum_rtps_record" 60 "$DOMAIN" "$TL" \
    > "$LOGS/ex02_rec.log" 2>&1
kill "$PUB" 2>/dev/null; wait "$PUB" 2>/dev/null

rec=$(grep -a "hello_world_topic " "$LOGS/ex02_rec.log" | head -1)
gap=$(grep -a "序号缺口" "$LOGS/ex02_rec.log" | head -1)
echo "      $rec"
echo "      ${gap:-（无缺口信息）}"
recn=$(echo "$rec" | grep -o "[0-9]*" | head -1)

if [ "${recn:-0}" -eq 0 ]; then
    echo
    echo "  ❌ FAIL —— 没录到任何数据，详见 $LOGS/ex02_rec.log"
    exit 1
fi

echo "  [2/3] 回放 ..."
sleep 2
timeout 95 "$TOOLS/fastdds_sub_diag" 75 "$DOMAIN" hello_world_topic \
    > "$LOGS/ex02_sub.log" 2>&1 &
SUB=$!
sleep 8
timeout 80 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 10.0 \
    > "$LOGS/ex02_rep.log" 2>&1
wait $SUB 2>/dev/null

pub=$(grep -a "已发布样本" "$LOGS/ex02_rep.log" | head -1 | tr -d ' ')
got=$(grep -a "DIAG_SUMMARY" "$LOGS/ex02_sub.log" \
      | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)

echo "  [3/3] 校验"
echo "      录制 = ${recn:-0} 条"
echo "      $pub"
echo "      回放后订阅端收到 = ${got:-0} 条"

echo
echo "  提示：录制产物在 $TL（可用 stratum_rtps_replay 反复回放）"
if [ "${got:-0}" -ge $((recn * 90 / 100)) ]; then
    echo "  ✅ PASS —— 录制与回放闭环正常（收到 >= 90% 录制条数）"
    exit 0
fi
echo "  ❌ FAIL —— 回放后收到 ${got:-0} 条，低于录制条数 ${recn} 的 90%"
exit 1
