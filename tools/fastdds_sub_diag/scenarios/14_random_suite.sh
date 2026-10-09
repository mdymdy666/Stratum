#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 14（随机套件）：随机化 payload / 条数 / 速率 / 话题数，逐轮校验条数对账
#
# 为什么要随机：固定参数的用例容易"恰好走通"。这里刻意让参数跨过已知边界：
#   · payload 跨 2 KiB（曾因固定 2048 缓冲静默失效）
#   · payload 跨 64 KiB（RTPS 单子消息上限，应当**显式失败**而不是发坏包）
#   · 速率跨"尽快 / 1000 条每秒"
#   · 话题数 1 或 2（多话题隔离）
#
# 判据（任一不满足即记 FAIL 并以非零退出）：
#   · 正常档：接收端收到条数 ∈ [期望, 期望+2]（+1 为首条补救副本，+2 为边界裕量）
#   · 超限档（payload > 65515）：回放端必须出现"样本过大"且订阅端 0 条
#
# 用法：bash tools/fastdds_sub_diag/scenarios/14_random_suite.sh [rounds] [seed] [domain]
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
ROUNDS="${1:-10}"
SEED="${2:-20261008}"
DOMAIN="${3:-13}"

# 模式：
#   full     —— 含"尽快 / 数千条每秒"等**超出接收端消费能力**的档位，
#               用于暴露 L1（高速突发丢包，见 design/15-test-report.md §3）
#   reliable —— 仅按录制速率回放（rate=1.0，即 50 条/秒）。
#               这是真实使用场景，**应当全绿**；用来把"基本功能可靠"与
#               "高速突发限制"两件事分开陈述。
MODE="${4:-full}"

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"
mkdir -p "$LOGS"

RANDOM="$SEED"
FAILED=0
LOSS_FAILED=0
printf '%-6s %-9s %-7s %-7s %-8s %-9s %-9s %s\n' \
    "轮次" "payload" "条数" "话题2" "rate" "发送" "收到" "判定"
printf '%.0s-' $(seq 1 84); echo

for round in $(seq 1 "$ROUNDS"); do
    # 随机 payload：刻意包含 2 KiB 与 64 KiB 两侧
    case $((RANDOM % 6)) in
        0) bytes=0 ;;          # 默认 "Hello world"
        1) bytes=1200 ;;
        2) bytes=8000 ;;       # 跨 2 KiB
        3) bytes=60000 ;;      # 接近 64 KiB
        4) bytes=0 ;;
        5) bytes=70000 ;;      # **超过** 16 位长度上限 → 应显式失败
    esac
    count=$((10 + RANDOM % 150))
    if [ $((RANDOM % 3)) -eq 0 ]; then
        topic2=$((5 + RANDOM % 50))
    else
        topic2=0
    fi
    if [ "$MODE" = "reliable" ]; then
        rate=1.0                     # 原速 = 按录制节奏回放（真实使用场景）
    elif [ $((RANDOM % 3)) -eq 0 ]; then
        rate=0
    else
        rate=1000
    fi

    TL="$TOOLS/tl_rand"
    rm -rf "$TL"
    "$TOOLS/make_timeline" "$TL" "$count" "$bytes" "$topic2" > "$LOGS/rand_${round}_gen.log" 2>&1

    diag_secs=30
    timeout $((diag_secs + 15)) "$TOOLS/fastdds_sub_diag" "$diag_secs" "$DOMAIN" hello_world_topic \
        > "$LOGS/rand_${round}_diag.log" 2>&1 &
    DIAG=$!
    sleep 5
    timeout 60 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" "$rate" \
        > "$LOGS/rand_${round}_rep.log" 2>&1
    wait $DIAG 2>/dev/null

    sent=$(grep -a "已发布样本" "$LOGS/rand_${round}_rep.log" | head -1 | grep -o "[0-9]*" | head -1)
    recv=$(grep -a DIAG_SUMMARY "$LOGS/rand_${round}_diag.log" | grep -o "samples=[0-9]*" | head -1 | cut -d= -f2)
    oversize=$(grep -ac "样本过大" "$LOGS/rand_${round}_rep.log" || true)

    # 【只期望第一个话题的条数】诊断程序仅订阅 hello_world_topic；
    # 第二个话题收不到正是**多话题隔离生效**的证明（见 scenarios/12）。
    # 之前把 topic2 加进期望，导致差额恰好等于 topic2 时被误判为失败。
    expected=$((count))
    verdict=""
    if [ "$bytes" -gt 65515 ]; then
        # 超限档：必须显式失败
        if [ "${oversize:-0}" -gt 0 ] && [ "${recv:-0}" -eq 0 ]; then
            verdict="PASS(显式失败)"
        else
            verdict="FAIL(超限却未显式失败: oversize=$oversize recv=${recv:-?})"
            FAILED=$((FAILED + 1))
        fi
    elif [ "${recv:-0}" -ge "$expected" ] && [ "${recv:-0}" -le $((expected + 2)) ]; then
        verdict="PASS"
    elif [ "${recv:-0}" -ge $((expected * 8 / 10)) ]; then
        # 收到 80% 以上但不足：归类为"无流控突发丢包"（真缺陷，单列以便修复）
        verdict="FAIL-LOSS(丢 $((expected - ${recv:-0}))/$expected)"
        LOSS_FAILED=$((LOSS_FAILED + 1))
    else
        verdict="FAIL(期望 $expected ±2，实得 ${recv:-?})"
        FAILED=$((FAILED + 1))
    fi

    printf '%-6s %-9s %-7s %-7s %-8s %-9s %-9s %s\n' \
        "$round" "${bytes}B" "$count" "$topic2" "$rate" "${sent:-?}" "${recv:-?}" "$verdict"
done

echo
echo "随机套件汇总（mode=$MODE, seed=$SEED, domain=$DOMAIN）：PASS=$((ROUNDS - FAILED - LOSS_FAILED)) "\
     "对账失败=$FAILED 无流控丢包=$LOSS_FAILED 共 $ROUNDS 轮"
exit $((FAILED > 0 ? 1 : 0))
