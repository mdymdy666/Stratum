#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 示例 03：大样本自动分片（DATA_FRAG）
#
# 验证什么：超过路径 MTU 的样本能否被自动切片发送、被对端正确重组。
#
# 为什么重要：MTU 1500 的路径上，不分片的 UDP 上限是 1472 字节；
#             超过就要依赖 IP 分片，而 IP 分片在 NAT / 中继路径上会被丢弃。
#             Stratum 用 DATA_FRAG 把切分放到 RTPS 层，每片不超过 MTU。
#
# 流程：生成大 payload 时间线 -> 回放（写出日志会显示探测到的 MTU）-> 订阅端校验
#
# 期望：8000B 与 60000B 两组样本都被完整接收。
#
# 用法：bash examples/03_large_payload/run.sh
# ---------------------------------------------------------------------------
set -u

DOMAIN="${DOMAIN:-72}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu
mkdir -p "$LOGS"

for t in make_timeline stratum_rtps_replay fastdds_sub_diag; do
    [ -x "$TOOLS/$t" ] || { echo "缺少 $TOOLS/$t，请先构建"; exit 1; }
done

echo "=== 示例 03：大样本自动分片（domain=$DOMAIN）==="
FAIL=0

for spec in "1200:20" "8000:20" "60000:8"; do
    PB="${spec%%:*}"; N="${spec##*:}"
    TL="$TOOLS/ex03_${PB}"; rm -rf "$TL"
    "$TOOLS/make_timeline" "$TL" "$N" "$PB" > /dev/null 2>&1

    echo
    echo "--- payload=${PB}B × ${N} ---"
    timeout 70 "$TOOLS/fastdds_sub_diag" 50 "$DOMAIN" hello_world_topic \
        > "$LOGS/ex03_${PB}_sub.log" 2>&1 &
    SUB=$!
    sleep 8
    STRATUM_DUMP=1 timeout 55 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 10.0 \
        > "$LOGS/ex03_${PB}_rep.log" 2>&1
    wait $SUB 2>/dev/null

    mtu=$(grep -a "自动探测路径 MTU" "$LOGS/ex03_${PB}_rep.log" | head -1 | tr -d ' ')
    pub=$(grep -a "已发布样本" "$LOGS/ex03_${PB}_rep.log" | head -1 | tr -d ' ')
    got=$(grep -a "DIAG_SUMMARY" "$LOGS/ex03_${PB}_sub.log" \
          | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)
    [ -n "$mtu" ] && echo "  $mtu"
    echo "  $pub"
    echo "  订阅端收到 = ${got:-0}（期望 >= $N）"
    if [ "${got:-0}" -lt "$N" ]; then
        FAIL=$((FAIL + 1))
        echo "  ❌ FAIL"
    else
        echo "  ✅ PASS"
    fi
done

echo
echo "  说明：单机走 loopback，探测到的 MTU 很大（如 65535），因此不会分片；"
echo "        跨主机时会探测到 1500 并自动切片（见 examples/04_cross_host）。"
echo
if [ "$FAIL" -eq 0 ]; then
    echo "  ✅ PASS —— 大 payload 全部完整接收"
    exit 0
fi
echo "  ❌ $FAIL 组未达标"
exit 1
