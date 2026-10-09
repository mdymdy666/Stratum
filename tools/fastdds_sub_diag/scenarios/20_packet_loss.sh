#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# E1 · 丢包注入：Stratum 可靠传输在丢包下的收敛性
#
# 用 STRATUM_DEBUG_DROP_SEND_PERCENT 让 Stratum **在发送路径上按概率丢包**
# （模拟链路丢包；因为本环境 tc/iptables 不可用，用户态代理又插不进 RTPS 端口协商）。
#
# 考察：RELIABLE + 重传机制能否把丢掉的样本补回来 —— 即"多大丢包率下仍能收敛"。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/20_packet_loss.sh
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
DOMAIN="${DOMAIN:-22}"
COUNT=200

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"
mkdir -p "$LOGS"

TL="$TOOLS/tl_drop"
rm -rf "$TL"
"$TOOLS/make_timeline" "$TL" "$COUNT" > /dev/null 2>&1

printf '%-8s %-10s %-10s %-10s\n' "丢包率" "收到" "完整率" "判定"
printf -- '------------------------------------------------\n'

for pct in 0 1 5 10; do
    timeout 60 "$TOOLS/fastdds_sub_diag" 45 "$DOMAIN" hello_world_topic \
        > "$LOGS/drop_${pct}_sub.log" 2>&1 &
    SUB=$!
    sleep 6
    STRATUM_DEBUG_DROP_SEND_PERCENT="$pct" \
        timeout 60 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 1.0 \
        > "$LOGS/drop_${pct}_rep.log" 2>&1
    wait $SUB 2>/dev/null

    got=$(grep -a DIAG_SUMMARY "$LOGS/drop_${pct}_sub.log" | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)
    got=${got:-0}
    # 允许 1 条首条补救副本
    if [ "$got" -ge "$COUNT" ] && [ "$got" -le $((COUNT + 2)) ]; then
        verdict="PASS（重传已补齐）"
    elif [ "$got" -ge $((COUNT * 95 / 100)) ]; then
        verdict="PARTIAL（少量未补齐）"
    else
        verdict="FAIL"
    fi
    printf '%-8s %-10s %-10s %-10s\n' "${pct}%" "$got" \
        "$(awk "BEGIN{printf \"%.1f%%\", 100.0*$got/$COUNT}")" "$verdict"
done

echo
echo "说明：丢包发生在 Stratum 的**发送路径**（模拟链路丢包），"
echo "      若完整率仍为 100%，说明 RELIABLE + 重传把丢失的样本补回来了。"
