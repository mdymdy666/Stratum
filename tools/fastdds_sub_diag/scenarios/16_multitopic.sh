#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 16（多话题并发）：验证同 domain 下两个话题**互不串扰**
#
# 为什么要专门的脚本：多话题此前只在临时实验里验证过（续 27），
# 而它依赖"每个话题独立的 writer entityId"这一关键修复
# （RTPS 中一个 DataWriter 只能对应一个 topic），属于必须长期守护的能力。
#
# 做法：
#   · 时间线同时造两个话题：hello_world_topic（index 1..N）
#     与 hello_world_topic_2（index 1001..，故意错开区间以便识别串扰）；
#   · 两个诊断程序实例（不同 participant-id）分别订阅两个话题；
#   · 断言：各自样本数正确，且 **index 全部落在自己的区间内**（串扰 0 条）。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/16_multitopic.sh [count1] [count2]
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
DOMAIN="${DOMAIN:-16}"
N1="${1:-100}"
N2="${2:-60}"

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"
mkdir -p "$LOGS"

TL="$TOOLS/tl_multitopic"
rm -rf "$TL"
"$TOOLS/make_timeline" "$TL" "$N1" 0 "$N2" | sed 's/^/  /'

timeout 50 "$TOOLS/fastdds_sub_diag" 40 "$DOMAIN" hello_world_topic --participant-id=30 \
    > "$LOGS/mt_t1.log" 2>&1 &
D1=$!
timeout 50 "$TOOLS/fastdds_sub_diag" 40 "$DOMAIN" hello_world_topic_2 --participant-id=40 \
    > "$LOGS/mt_t2.log" 2>&1 &
D2=$!
sleep 6
timeout 60 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 20.0 > "$LOGS/mt_rep.log" 2>&1
wait $D1 2>/dev/null
wait $D2 2>/dev/null

check() {
    local log="$1" label="$2" lo="$3" hi="$4"
    local samples outside
    samples=$(grep -a DIAG_SUMMARY "$log" | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)
    grep -ao "index=[0-9]*" "$log" | grep -o "[0-9]*" | sort -n | uniq > "$log.idx"
    outside=$(awk -v lo="$lo" -v hi="$hi" '$1<lo || $1>hi' "$log.idx" | wc -l)
    local expect=$((hi - lo + 1))
    printf '  %-20s samples=%-5s 期望=%s  区间外(串扰)=%s\n' "$label" "${samples:-?}" "$expect" "$outside"
    if [ "${samples:-0}" -lt "$expect" ] || [ "${outside:-1}" -ne 0 ]; then
        echo "    → FAIL"
        return 1
    fi
    echo "    → PASS"
    return 0
}

echo
echo "=== 回放端 ==="
grep -a "已发布样本" "$LOGS/mt_rep.log" | head -1 | sed 's/^/  /'
echo "=== 话题隔离检查 ==="
rc=0
# 注意：诊断程序只打印前 5 条 + 每 10 条一条，所以"区间外"是强断言，
# 样本总数才是完整性判据。
check "$LOGS/mt_t1.log" "hello_world_topic" 1 "$N1" || rc=1
check "$LOGS/mt_t2.log" "hello_world_topic_2" 1001 $((1000 + N2)) || rc=1

echo
if [ "$rc" -eq 0 ]; then
    echo "判定：PASS（两个话题各自完整、零串扰）"
else
    echo "判定：FAIL（见上）"
fi
exit "$rc"
