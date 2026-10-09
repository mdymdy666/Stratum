#!/usr/bin/env bash
# 大 payload 回放验证（覆盖"分片/边界"这一未验证维度）
#   1500B 以下：单包；8KB：IP 分片；60KB：接近 64KB 边界
set -u

D=/UserData/CodexWorkSpace/Stratum
TOOLS="$D/build/tools"
LOGS="$TOOLS/logs"
HW=/UserData/Fast-DDS/build-fastdds/examples/cpp/hello_world/hello_world
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu
mkdir -p "$LOGS"

run_case() {
    local bytes="$1" count="$2" tag="$3"
    local tl="$TOOLS/tl_${tag}"
    rm -rf "$tl"
    echo "########## payload=${bytes}B × ${count} 条 ##########"
    "$TOOLS/make_timeline" "$tl" "$count" "$bytes" 2>&1 | sed 's/^/  /'

    timeout 90 "$HW" subscriber -s 0 > "$LOGS/pay_${tag}_sub.log" 2>&1 &
    local sub=$!
    sleep 4
    timeout 90 "$TOOLS/stratum_rtps_replay" "$tl" 0 20.0 > "$LOGS/pay_${tag}_rep.log" 2>&1
    sleep 5
    kill "$sub" 2>/dev/null; wait "$sub" 2>/dev/null

    local recv
    recv=$(grep -ac RECEIVED "$LOGS/pay_${tag}_sub.log")
    echo "  回放端: $(grep -a '已发布样本' "$LOGS/pay_${tag}_rep.log" | head -1 | tr -d ' ')"
    echo "  订阅端 RECEIVED = $recv / $count"
    # 校验收到的内容长度（payload 一致性）
    local len
    len=$(grep -a "Message: '" "$LOGS/pay_${tag}_sub.log" | head -1 | awk '{print length($0)}')
    echo "  首条日志行长度 = ${len:-0}（payload=${bytes}B 时应明显放大）"
    echo
}

run_case 1200 20 small
run_case 8000 20 frag
run_case 60000 10 large
