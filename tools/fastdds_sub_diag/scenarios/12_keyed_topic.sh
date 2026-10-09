#!/usr/bin/env bash
# A4 验证：上游 keyed 示例 publisher → Stratum Reader（应匹配并提取 instance_key）
set -u

D=/UserData/CodexWorkSpace/Stratum
TOOLS="$D/build/tools"
LOGS="$TOOLS/logs"
TI=/UserData/Fast-DDS/build-fastdds/examples/cpp/topic_instances/topic_instances
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu
mkdir -p "$LOGS"

echo "=== 上游 keyed 示例：4 个实例（颜色）× 5 样本 = 20 条 ==="
timeout 40 "$TI" publisher -d 9 -i 4 -n Square -s 5 > "$LOGS/a4_pub.log" 2>&1 &
PUB=$!
sleep 5
timeout 30 "$TOOLS/rtps_reader_diag" 20 9 > "$LOGS/a4_rd.log" 2>&1
wait $PUB 2>/dev/null

echo "  上游是否真正发送: $(grep -ac "SENT\|Sent\|sent" "$LOGS/a4_pub.log") 行"
grep -a "Sent\|SENT" "$LOGS/a4_pub.log" | head -3 | sed 's/^/    /'
echo "  Stratum 打印的样本行数: $(grep -ac '数据 #' "$LOGS/a4_rd.log")"
echo "  key_len 分布:"
grep -ao "key_len=[0-9]*" "$LOGS/a4_rd.log" | sort | uniq -c | sed 's/^/    /'
echo "  不同 key 数量: $(grep -ao "key=[0-9a-f]\{32\}" "$LOGS/a4_rd.log" | sort -u | wc -l)（期望 4）"
echo "  样例:"
grep -a "数据 #" "$LOGS/a4_rd.log" | head -4 | sed 's/^/    /'
echo "  统计末行:"
grep -a "pkt=" "$LOGS/a4_rd.log" | tail -1 | sed 's/^/    /'
