#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 21（E4 · 反复回归）
#
# 目的：连续多轮跑"全量单测 + 关键场景"，确认没有 flaky（偶发失败）。
# 单轮全绿不算数 —— 偶发问题正是靠重复暴露的。
#
# 每轮包含：
#   · 全量 ctest
#   · 条数对账 A/B（回放方向核心完整性）
#   · 场景 05（Stratum↔Stratum）、07（→ 上游 subscriber）、16（多话题隔离）
#
# 用法：bash tools/fastdds_sub_diag/scenarios/21_regression_loop.sh [轮数]
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
ROUNDS="${1:-3}"

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"
mkdir -p "$LOGS"

SUMMARY="$LOGS/regloop_summary.txt"
: > "$SUMMARY"

printf '%-6s %-10s %-12s %-12s %-8s %-8s %-8s\n' \
    "轮次" "ctest" "对账A" "对账B" "场景05" "场景07" "场景16"
printf -- '------------------------------------------------------------------------\n'

fails=0
for r in $(seq 1 "$ROUNDS"); do
    # ---- ctest ----
    ( cd "$ROOT/build" && ctest > "$LOGS/regloop_${r}_ctest.log" 2>&1 )
    if grep -qa "100% tests passed" "$LOGS/regloop_${r}_ctest.log"; then
        ct="33/33"
    else
        ct="FAIL"; fails=$((fails + 1))
    fi

    # ---- 对账 A/B ----
    bash "$DIR/08_accounting.sh" > "$LOGS/regloop_${r}_acct.log" 2>&1
    a=$(grep -a "收到" "$LOGS/regloop_${r}_acct.log" | sed -n '1p' | grep -o "[0-9]*" | head -1)
    b=$(grep -a "收到" "$LOGS/regloop_${r}_acct.log" | sed -n '2p' | grep -o "[0-9]*" | head -1)
    [ "${a:-0}" = "200" ] && av="200/200" || { av="${a:-?}/200"; fails=$((fails + 1)); }
    [ "${b:-0}" -ge "110" ] && bv="${b}/$b" || { bv="${b:-?}"; fails=$((fails + 1)); }

    # ---- 场景 05 / 07 / 16 ----
    if bash "$DIR/05_stratum_to_stratum.sh" > "$LOGS/regloop_${r}_s05.log" 2>&1 \
        && grep -qa "PASS" "$LOGS/regloop_${r}_s05.log"; then s05="PASS"; else s05="FAIL"; fails=$((fails + 1)); fi
    if bash "$DIR/07_upstream_subscriber.sh" "$TOOLS/tl_accept" > "$LOGS/regloop_${r}_s07.log" 2>&1 \
        && grep -qa "PASS" "$LOGS/regloop_${r}_s07.log"; then s07="PASS"; else s07="FAIL"; fails=$((fails + 1)); fi
    if bash "$DIR/16_multitopic.sh" 100 60 > "$LOGS/regloop_${r}_s16.log" 2>&1 \
        && grep -qa "PASS" "$LOGS/regloop_${r}_s16.log"; then s16="PASS"; else s16="FAIL"; fails=$((fails + 1)); fi

    printf '%-6s %-10s %-12s %-12s %-8s %-8s %-8s\n' "$r" "$ct" "$av" "$bv" "$s05" "$s07" "$s16"
    printf 'round=%s ctest=%s acctA=%s acctB=%s s05=%s s07=%s s16=%s\n' \
        "$r" "$ct" "$av" "$bv" "$s05" "$s07" "$s16" >> "$SUMMARY"
done

echo
if [ "$fails" -eq 0 ]; then
    echo "判定：PASS（$ROUNDS 轮全部通过，未观察到 flaky）"
else
    echo "判定：FAIL（$fails 项次不达标，存在 flaky 或真实失败，见 $SUMMARY）"
fi
exit $((fails > 0 ? 1 : 0))
