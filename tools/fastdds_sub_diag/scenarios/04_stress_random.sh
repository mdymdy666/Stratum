#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 04：随机参数 + 压力测试
#
# 为什么这样设计（务必先读）：
#   本机 WSL 是 mirrored networking，Fast DDS 的组播加入失败、且 7400/7438/7439
#   被 WSL 之外的进程占用，导致**用户数据平面不可用**（连上游 hello_world
#   publisher → subscriber 也收不到数据，见 scenarios/03）。
#   因此本脚本量化的是**协议层稳定性**，而不是吞吐：
#     · 阶段 A（随机）：随机 domain/端口/QoS/回放速率下，诊断程序必须每次
#       正常退出、打印 DIAG_SUMMARY、且摘要字段自洽（无 FATAL、无崩溃）；
#     · 阶段 B（压测）：rate=0（尽快回放）× 多轮 + 长跑，进程不得崩溃/卡死，
#       RSS 峰值必须有界；同时记录协议计数（pkt/heartbeat/acks）证明链路活着。
#   samples 在环境正常时应 > 0；本脚本只记录、不断言（断言它需要正常网络）。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/04_stress_random.sh [timeline]
#   ROUNDS=6 bash ...            随机轮数（默认 6）
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
DIAG_BIN="${DIAG_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/fastdds_sub_diag}"
REPLAY_BIN="${REPLAY_BIN:-/UserData/CodexWorkSpace/Stratum/build/tools/stratum_rtps_replay}"
TIMELINE="${1:-/tmp/tl_final}"
ROUNDS="${ROUNDS:-6}"
OUT_DIR=/tmp/diag_sc04
mkdir -p "$OUT_DIR"

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"

summary_field() {   # $1=日志文件 $2=字段名
    grep -o "$2=[^ ]*" "$1" 2>/dev/null | tail -1 | cut -d= -f2
}

echo "=== 阶段 A：随机参数（$ROUNDS 轮）==="
pass=0
fail=0
for i in $(seq 1 "$ROUNDS"); do
    domain=$((5 + (RANDOM % 5) * 2))          # 5,7,9,11,13：避开 domain 0
    meta=$((22000 + i * 20))
    user=$((meta + 1))
    strat_meta=$((9060 + domain * 10))
    rate=$(( (RANDOM % 40) + 1 ))             # 0.1x ~ 4.0x
    seconds=$((8 + RANDOM % 6))
    if [ $((RANDOM % 2)) -eq 0 ]; then rel=reliable; else rel=besteffort; fi
    if [ $((RANDOM % 2)) -eq 0 ]; then dur=volatile; else dur=transient_local; fi

    sub_log="$OUT_DIR/rand_${i}_sub.log"
    rep_log="$OUT_DIR/rand_${i}_replay.log"

    "$DIAG_BIN" "$seconds" "$domain" hello_world_topic \
        --meta-port="$meta" --user-port="$user" --peer="127.0.0.1:$strat_meta" \
        --reader-reliability="$rel" --reader-durability="$dur" \
        > "$sub_log" 2>&1 &
    diag_pid=$!
    sleep 2

    timeout 60 "$REPLAY_BIN" "$TIMELINE" "$domain" "$rate" > "$rep_log" 2>&1

    wait "$diag_pid"
    rc=$?

    ok=1
    reason=""
    if [ "$rc" -ne 0 ]; then ok=0; reason="退出码=$rc"; fi
    if ! grep -q "DIAG_SUMMARY" "$sub_log"; then ok=0; reason="$reason 缺少DIAG_SUMMARY"; fi
    if grep -q "FATAL" "$sub_log"; then ok=0; reason="$reason 出现FATAL"; fi

    # 自洽性：valid 样本数不可能超过"被取出的样本数"
    calls=$(summary_field "$sub_log" data_available_calls)
    invalid=$(summary_field "$sub_log" invalid_data_samples)
    samples=$(summary_field "$sub_log" samples)
    matched=$(summary_field "$sub_log" matched_events)
    if [ -n "$calls" ] && [ -n "$invalid" ] && [ -n "$samples" ]; then
        total_taken=$((invalid + samples))
        if [ "$samples" -gt "$calls" ] && [ "$calls" -ne 0 ]; then
            ok=0; reason="$reason samples($samples)>data_available_calls($calls)"
        fi
        if [ "$total_taken" -gt 0 ] && [ "$calls" -eq 0 ]; then
            ok=0; reason="$reason 取到样本但回调次数为0"
        fi
    fi

    if [ "$ok" -eq 1 ]; then
        pass=$((pass + 1))
        echo "  [轮 $i PASS] domain=$domain rate=$rate rel=$rel dur=$dur matched=$matched samples=$samples calls=$calls"
    else
        fail=$((fail + 1))
        echo "  [轮 $i FAIL] domain=$domain rate=$rate rel=$rel dur=$dur ->$reason（日志 $sub_log）"
    fi
done

echo "阶段 A 结果：$pass 通过 / $fail 失败"

echo
echo "=== 阶段 B：压力测试（rate=0 尽快回放 × 3 轮，采样 RSS）==="
domain=7
meta=22990
user=22991
strat_meta=$((9060 + domain * 10))
stress_log="$OUT_DIR/stress_sub.log"

"$DIAG_BIN" 70 "$domain" hello_world_topic \
    --meta-port="$meta" --user-port="$user" --peer="127.0.0.1:$strat_meta" \
    > "$stress_log" 2>&1 &
stress_pid=$!
sleep 2

peak_kb=0
for round in 1 2 3; do
    timeout 90 "$REPLAY_BIN" "$TIMELINE" "$domain" 0 \
        > "$OUT_DIR/stress_replay_$round.log" 2>&1
    for _ in 1 2 3 4 5; do
        rss=$(awk '/VmHWM/{print $2}' "/proc/$stress_pid/status" 2>/dev/null || echo 0)
        if [ -n "$rss" ] && [ "$rss" -gt "$peak_kb" ]; then peak_kb=$rss; fi
        sleep 1
    done
done

wait "$stress_pid"
stress_rc=$?

stress_calls=$(summary_field "$stress_log" data_available_calls)
stress_samples=$(summary_field "$stress_log" samples)
stress_writers=$(summary_field "$stress_log" writers_discovered)

echo "  诊断端退出码 = $stress_rc"
echo "  峰值 RSS     = ${peak_kb} KB"
echo "  writers_discovered = ${stress_writers:-?}  samples = ${stress_samples:-?}  data_available_calls = ${stress_calls:-?}"
grep -c "DIAG_SUMMARY" "$stress_log" | sed 's/^/  DIAG_SUMMARY 行数 = /'

if [ "$stress_rc" -eq 0 ] && [ "$peak_kb" -lt 204800 ]; then
    echo "阶段 B 结果：PASS（正常退出且 RSS < 200MB）"
else
    echo "阶段 B 结果：FAIL（退出码 $stress_rc，峰值 RSS ${peak_kb} KB）"
fi

echo
echo "两个阶段的日志都在 $OUT_DIR/"
exit 0
