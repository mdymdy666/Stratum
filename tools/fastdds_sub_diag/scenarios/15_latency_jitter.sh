#!/usr/bin/env bash
# B1b 验证：① 录制侧实时延迟（实时发布 → Stratum Reader）
#            ② 到达抖动（回放 → 诊断程序，原速与尽快两档）
set -u

D=/UserData/CodexWorkSpace/Stratum
TOOLS="$D/build/tools"
LOGS="$TOOLS/logs"
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu
mkdir -p "$LOGS"

echo "########## ① 录制侧实时延迟（实时发布 200 条 @100 条/秒）##########"
timeout 40 "$TOOLS/rtps_reader_diag" 25 14 > "$LOGS/b1b_rec.log" 2>&1 &
RD=$!
sleep 6
timeout 30 "$TOOLS/fastdds_pub_diag" --domain=14 --count=200 --rate=100 \
    --durability=volatile --reliability=reliable > "$LOGS/b1b_pub.log" 2>&1
wait $RD 2>/dev/null
echo "  上游发送: $(grep -a PUB_DONE "$LOGS/b1b_pub.log" | head -1)"
echo "  Stratum 收到: $(grep -a '用户回调样本' "$LOGS/b1b_rec.log" | head -1)"
grep -a "READER_LATENCY_MS" "$LOGS/b1b_rec.log" | sed 's/^/  /'
grep -a "READER_JITTER_MS" "$LOGS/b1b_rec.log" | sed 's/^/  /'

echo
echo "########## ② 到达抖动（回放 200 条，原速 rate=1.0 = 50 条/秒）##########"
TL="$TOOLS/tl_b1b"
rm -rf "$TL"
"$TOOLS/make_timeline" "$TL" 200 > /dev/null 2>&1
timeout 40 "$TOOLS/fastdds_sub_diag" 30 14 hello_world_topic > "$LOGS/b1b_d1.log" 2>&1 &
D1=$!
sleep 5
timeout 40 "$TOOLS/stratum_rtps_replay" "$TL" 14 1.0 > "$LOGS/b1b_r1.log" 2>&1
wait $D1 2>/dev/null
echo "  回放: $(grep -a '已发布样本' "$LOGS/b1b_r1.log" | head -1 | tr -d ' ')"
grep -a "DIAG_SUMMARY" "$LOGS/b1b_d1.log" | grep -o "samples=[0-9]*" | sed 's/^/  收到 /'
grep -a "DIAG_JITTER_MS" "$LOGS/b1b_d1.log" | sed 's/^/  /'

echo
echo "########## ③ 到达抖动（回放 200 条，尽快 rate=0）##########"
timeout 40 "$TOOLS/fastdds_sub_diag" 30 14 hello_world_topic > "$LOGS/b1b_d2.log" 2>&1 &
D2=$!
sleep 5
timeout 40 "$TOOLS/stratum_rtps_replay" "$TL" 14 0 > "$LOGS/b1b_r2.log" 2>&1
wait $D2 2>/dev/null
echo "  回放: $(grep -a '已发布样本' "$LOGS/b1b_r2.log" | head -1 | tr -d ' ')"
grep -a "DIAG_SUMMARY" "$LOGS/b1b_d2.log" | grep -o "samples=[0-9]*" | sed 's/^/  收到 /'
grep -a "DIAG_JITTER_MS" "$LOGS/b1b_d2.log" | sed 's/^/  /'
