#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 08（条数对账）：验收口径 = "Topic 能否正常通信 / 能否回放"
#
#   A. 生成 200 条 → 回放 → 上游 Fast DDS subscriber
#      期望：200/200（首=1 尾=200）—— 证明**回放完整**
#   B. 真实 publisher → Stratum 录制 → 回放 → 上游 subscriber
#      期望：录制 N 条（序号缺口 0）、回放后收到 N-1 或 N 条
#            —— 差 1 条时缺的必是**第一条**（VOLATILE 语义边界，见 design/12）
#
# 为什么需要它：只看"PASS/FAIL"会掩盖"少了哪几条"。条数对账 + 缺口定位
# 才能区分"协议不通"（收 0 条）、"丢中间"（缺口在中间）与
# "匹配边界丢首条"（首=2）这三种完全不同的情况。
#
# 日志与索引文件都写在 build/tools/logs/（不要用 /tmp：会被系统清理，
# 本项目已因此丢过一次证据）。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/08_accounting.sh
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
HW_BIN="${HW_BIN:-/UserData/Fast-DDS/build-fastdds/examples/cpp/hello_world/hello_world}"
DOMAIN="${DOMAIN:-40}"   # 避开 ROS 2 占用的 domain 0（见下方说明）
RATE="${RATE:-5.0}"
COUNT="${COUNT:-200}"

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"
mkdir -p "$LOGS"

analyze() {
    # 去重后统计：收尾补救可能重发首条，重复内容不算缺口。
    grep -o "index: '[0-9]*'" "$1" | grep -o "[0-9]*" | sort -n | uniq > "$1.idx"
    local total
    total=$(wc -l < "$1.idx")
    echo "  收到 = $total"
    awk '
      NR==1 { first=$1; prev=$1; next }
      { if ($1 != prev + 1) print "  缺口: " (prev+1) " .. " ($1-1); prev=$1 }
      END { if (NR>0) print "  首=" first " 尾=" prev }
    ' "$1.idx"
}

echo "########## A. 生成 $COUNT 条 → 回放 → 订阅端（domain $DOMAIN）##########"
# 【时序照抄已验证的手工方案】
#   · 订阅端先起，等 6 秒再回放（4 秒时偶尔来不及完成发现）；
#   · 订阅端跑固定秒数后**自然结束** —— 绝不能用 kill，
#     否则只在正常退出时打印的 DIAG_SUMMARY 拿不到，判据永远是 0
#     （这正是此前多轮"收到 0"的真正原因）。
TLA="$TOOLS/tl_$COUNT"
rm -rf "$TLA"
"$TOOLS/make_timeline" "$TLA" "$COUNT" > "$LOGS/08_a_gen.log" 2>&1
timeout 70 "$TOOLS/fastdds_sub_diag" 40 "$DOMAIN" hello_world_topic \
    > "$LOGS/08_a_sub.log" 2>&1 &
SUB=$!
sleep 6
timeout 90 "$TOOLS/stratum_rtps_replay" "$TLA" "$DOMAIN" "$RATE" > "$LOGS/08_a_rep.log" 2>&1
echo "  回放端: $(grep -a '已发布样本' "$LOGS/08_a_rep.log" | head -1 | tr -d ' ')"
wait "$SUB" 2>/dev/null
got_a=$(grep -a "DIAG_SUMMARY" "$LOGS/08_a_sub.log" | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)
echo "  收到 = ${got_a:-0}（期望 $COUNT）"

echo "########## B. 真实录制 → 回放 → 订阅端（domain $DOMAIN）##########"
TLB="$TOOLS/tl_account"
rm -rf "$TLB"
"$TOOLS/fastdds_pub_diag" --domain="$DOMAIN" --topic=hello_world_topic \
    --count=200 --rate=20 > "$LOGS/08_b_pub.log" 2>&1 &
PUB=$!
sleep 5
timeout 40 "$TOOLS/stratum_rtps_record" 12 "$DOMAIN" "$TLB" > "$LOGS/08_b_rec.log" 2>&1
kill "$PUB" 2>/dev/null; wait "$PUB" 2>/dev/null
grep -a "hello_world_topic " "$LOGS/08_b_rec.log" | head -1 | sed 's/^/  录制: /'
grep -a "序号缺口" "$LOGS/08_b_rec.log" | head -1 | sed 's/^/  /'

sleep 3
timeout 95 "$TOOLS/fastdds_sub_diag" 55 "$DOMAIN" hello_world_topic \
    > "$LOGS/08_b_sub.log" 2>&1 &
SUB2=$!
sleep 6
timeout 90 "$TOOLS/stratum_rtps_replay" "$TLB" "$DOMAIN" "$RATE" > "$LOGS/08_b_rep.log" 2>&1
echo "  回放端: $(grep -a '已发布样本' "$LOGS/08_b_rep.log" | head -1 | tr -d ' ')"
wait "$SUB2" 2>/dev/null
got_b=$(grep -a "DIAG_SUMMARY" "$LOGS/08_b_sub.log" | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)
echo "  订阅端 RECEIVED = ${got_b:-0}"

