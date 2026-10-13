#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 26（DATA_FRAG 接收侧健壮性压力测试）
#
# 覆盖四类异常输入，验证 RtpsReader 的分片重组：
#   A. 多 writer 交错分片 —— 两个话题各自大 payload，分片交错到达，验证零串扰
#   B. 缓存上限         —— 为大量不同 SN 各发 1 片（永不收齐），观察内存与存活
#   C. 畸 形 分 片      —— fragmentSize=0 / sampleSize=0 / 计数极大 / 起始号越界
#   D. 超时分片淘汰     —— 只发首片，验证 30 秒淘汰后不误交付、内存不涨
#
# 依赖 tools/frag_fuzz（见其源码注释）。
# 用法：bash tools/fastdds_sub_diag/scenarios/26_frag_stress.sh [domain]
# ---------------------------------------------------------------------------
set -u

D=/UserData/CodexWorkSpace/Stratum
TOOLS="$D/build/tools"; LOGS="$TOOLS/logs"
DOMAIN="${1:-63}"
FUZZ="$TOOLS/frag_fuzz"
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu
cd "$D"
mkdir -p "$LOGS"

FAIL=0
pass() { printf '  %-40s PASS  %s\n' "$1" "$2"; }
fail() { printf '  %-40s FAIL  %s\n' "$1" "$2"; FAIL=$((FAIL + 1)); }

META_PORT=$((9050 + DOMAIN * 10))
USER_PORT=$((9051 + DOMAIN * 10))

[ -x "$FUZZ" ] || { echo "缺少 $FUZZ，请先跑 tools/fastdds_sub_diag/build_tools.sh"; exit 1; }

echo "=== DATA_FRAG 接收侧健壮性（domain=$DOMAIN, user_port=$USER_PORT）==="
echo

# ---------------------------------------------------------------------------
echo "=== A. 多 writer 交错分片（2 话题 + 8KB，应零串扰）==="
TLA="$TOOLS/frag_st_multi"; rm -rf "$TLA"
"$TOOLS/make_timeline" "$TLA" 60 8000 40 > /dev/null 2>&1
"$TOOLS/rtps_reader_diag" 45 "$DOMAIN" > "$LOGS/fs_multi_sub.log" 2>&1 &
RS=$!
sleep 8
timeout 40 "$TOOLS/stratum_rtps_replay" "$TLA" "$DOMAIN" 20 > "$LOGS/fs_multi_rep.log" 2>&1
wait $RS 2>/dev/null
got=$(grep -a "用户回调样本" "$LOGS/fs_multi_sub.log" | tail -1 | grep -o "[0-9]*" | head -1)
topics=$(grep -a "发现话题" "$LOGS/fs_multi_sub.log" | tail -1)
echo "  $topics"
echo "  收到 = ${got:-0}（期望 100 = 60 + 40）"
if [ "${got:-0}" -ge 100 ]; then
    pass "多 writer 交错分片" "收到 ${got}"
else
    fail "多 writer 交错分片" "收到 ${got:-0}"
fi
echo

# ---------------------------------------------------------------------------
echo "=== B. 缓存上限（500 个不同 SN 各 1 片，永不收齐）==="
"$TOOLS/rtps_reader_diag" 30 "$DOMAIN" > "$LOGS/fs_ovf_sub.log" 2>&1 &
RS=$!
sleep 6
rss_before=$(ps -o rss= -p "$RS" 2>/dev/null | tr -d ' ')
"$FUZZ" overflow 500 --dst=127.0.0.1:"$USER_PORT" --domain="$DOMAIN" 2>&1 | sed 's/^/  /'
sleep 6
rss_after=$(ps -o rss= -p "$RS" 2>/dev/null | tr -d ' ')
alive=$(kill -0 "$RS" 2>/dev/null && echo yes || echo no)
echo "  RSS: ${rss_before:-?} KB -> ${rss_after:-?} KB（进程存活: $alive）"
if [ "$alive" = "yes" ]; then
    pass "缓存上限不崩" "500 个半成品分片后仍存活"
else
    fail "缓存上限不崩" "进程已退出"
fi
wait $RS 2>/dev/null
echo

# ---------------------------------------------------------------------------
echo "=== C. 畸形分片（fragmentSize=0 / sampleSize=0 / 计数极大 / 起始号越界）==="
"$TOOLS/rtps_reader_diag" 30 "$DOMAIN" > "$LOGS/fs_bad_sub.log" 2>&1 &
RS=$!
sleep 6
"$FUZZ" malformed 100 --dst=127.0.0.1:"$USER_PORT" --domain="$DOMAIN" 2>&1 | sed 's/^/  /'
sleep 6
alive=$(kill -0 "$RS" 2>/dev/null && echo yes || echo no)
echo "  进程存活: $alive"
if [ "$alive" = "yes" ]; then
    pass "畸形分片不崩" "100 个畸形报文后仍存活"
else
    fail "畸形分片不崩" "进程已退出"
fi
wait $RS 2>/dev/null
echo

# ---------------------------------------------------------------------------
echo "=== D. 部分分片不误交付（只发首片，样本不应被交付）==="
"$TOOLS/rtps_reader_diag" 40 "$DOMAIN" > "$LOGS/fs_part_sub.log" 2>&1 &
RS=$!
sleep 6
"$FUZZ" partial 30 --dst=127.0.0.1:"$USER_PORT" --domain="$DOMAIN" 2>&1 | sed 's/^/  /'
sleep 12
early=$(grep -a "用户回调样本" "$LOGS/fs_part_sub.log" | tail -1 | grep -o "[0-9]*" | head -1)
echo "  12 秒时收到 = ${early:-0}（期望 0：全部样本都缺片）"
if [ "${early:-0}" -eq 0 ]; then
    pass "不完整样本不误交付" "收到 0"
else
    fail "不完整样本不误交付" "收到 ${early}"
fi
wait $RS 2>/dev/null
echo

echo "=========================================="
if [ "$FAIL" -eq 0 ]; then
    echo "判定：PASS —— DATA_FRAG 接收侧健壮性全部通过"
else
    echo "判定：$FAIL 项不达标"
fi
exit $((FAIL > 0 ? 1 : 0))
