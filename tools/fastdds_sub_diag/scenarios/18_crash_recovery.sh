#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 场景 18（E2 · 进程中断与恢复）
#
# 考三件事：
#   ① **崩溃一致性**：录制进程被 `kill -9`（不给清理机会）后，
#      已落盘的时间线**仍可读、未损坏**；
#   ② **可恢复性**：重启录制后能否继续工作（哪怕不能续录，也不该崩溃）；
#   ③ **数据安全性**：录制器启动时会 `rm -rf <dir>` —— 这一点必须实测确认，
#      因为它意味着"对已有时间线重启录制 = 清空历史"。
#
# 用法：bash tools/fastdds_sub_diag/scenarios/18_crash_recovery.sh
# ---------------------------------------------------------------------------
set -u

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
TOOLS="$ROOT/build/tools"
LOGS="$TOOLS/logs"
DOMAIN="${DOMAIN:-20}"

export LD_LIBRARY_PATH="/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu"
mkdir -p "$LOGS"

TL="$TOOLS/tl_crash"
TL_IN="$TOOLS/tl_crash_in"
FAIL=0
rm -rf "$TL" "$TL_IN"
"$TOOLS/make_timeline" "$TL_IN" 300 > /dev/null 2>&1

echo "=== 阶段 1：录制若干数据后强杀录制器 ==="
# 【踩过的坑】此前写成 `timeout 60 record ... &` 再 `kill -9 $!` ——
#  `$!` 是 **timeout 的 PID**，真正的录制器是它的子进程，根本没被杀掉，
#  于是后续"打不开时间线"其实是 **LOCK 被占用**，而不是数据损坏。
#  正确做法：直接后台启动，`kill -9` 打在录制器本身。
"$TOOLS/stratum_rtps_record" 60 "$DOMAIN" "$TL" > "$LOGS/cr_rec1.log" 2>&1 &
REC=$!
sleep 6
timeout 30 "$TOOLS/stratum_rtps_replay" "$TL_IN" "$DOMAIN" 1.0 > "$LOGS/cr_rep1.log" 2>&1
sleep 3
# 确认它真的在跑，再强杀；并确认它真的退出了
if kill -0 "$REC" 2>/dev/null; then
    kill -9 "$REC" 2>/dev/null
fi
for _ in $(seq 1 50); do
    kill -0 "$REC" 2>/dev/null || break
    sleep 0.2
done
if kill -0 "$REC" 2>/dev/null; then
    echo "  FAIL: 录制器未被杀死（PID $REC）"
    kill -9 "$REC" 2>/dev/null
    FAIL=$((FAIL + 1))
else
    echo "  录制器进程已确认退出（强杀，无 flush 机会）"
fi
echo "  录制器已被 kill -9；时间线目录文件数：$(ls "$TL" 2>/dev/null | wc -l)"

echo
echo "=== 阶段 2：强杀后**时间线内容**是否完好（崩溃一致性）==="
# 【判据修正】此前用"回放条数"判定，但那要求有订阅端在场；
# 而本脚本没有起订阅端，于是稳定得到 0 条 —— 那是预期的，不是数据丢失。
# 正确的判据是 **时间线自身是否还能列出数据**：
#   `stratum_rtps_replay` 在开头会打印"待回放话题（N 个）：<话题> M 条"。
timeout 40 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 1.0 > "$LOGS/cr_rep2.log" 2>&1
rc=$?
line=$(grep -a "hello_world_topic" "$LOGS/cr_rep2.log" | head -1)
cnt=$(echo "$line" | grep -o "[0-9]* 条" | grep -o "[0-9]*")
echo "  回放退出码=$rc；时间线内容：${line:-（无）}"
if [ "${cnt:-0}" -gt 0 ]; then
    echo "  → **强杀后数据完好**（时间线里仍有 $cnt 条）PASS"
else
    echo "  → FAIL：强杀后时间线里没有数据"
    FAIL=$((FAIL + 1))
fi
# 同时说明：无对端时 Player 会明确报错，这是"不静默"的正确行为
if grep -qa "尚无已发现的对端 Reader" "$LOGS/cr_rep2.log"; then
    echo "  （附注：本脚本未起订阅端，Player 明确报错'尚无已发现的对端 Reader' —— 属正确行为）"
fi

echo
echo "=== 阶段 3：重启录制器（同一目录）是否清空已有数据 ==="
before_cnt="$cnt"
"$TOOLS/stratum_rtps_record" 8 "$DOMAIN" "$TL" > "$LOGS/cr_rec2.log" 2>&1 &
REC2=$!
sleep 10
kill "$REC2" 2>/dev/null
for _ in $(seq 1 50); do kill -0 "$REC2" 2>/dev/null || break; sleep 0.2; done
kill -9 "$REC2" 2>/dev/null; wait "$REC2" 2>/dev/null

timeout 40 "$TOOLS/stratum_rtps_replay" "$TL" "$DOMAIN" 1.0 > "$LOGS/cr_rep3.log" 2>&1
line3=$(grep -a "hello_world_topic" "$LOGS/cr_rep3.log" | head -1)
after_cnt=$(echo "$line3" | grep -o "[0-9]* 条" | grep -o "[0-9]*")
echo "  重启前 $before_cnt 条 → 重启后 ${after_cnt:-0} 条"
if [ "${after_cnt:-0}" -ge "${before_cnt:-1}" ]; then
    echo "  → 旧数据保留，录制器可继续使用 PASS"
else
    echo "  → **发现**：重启录制器清空了已有时间线（$before_cnt → ${after_cnt:-0}）"
    echo "     录制器启动时会执行 rm -rf <dir>，属**数据安全性问题**，应记入遗留清单"
    FAIL=$((FAIL + 1))
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "判定：PASS（崩溃一致性与恢复行为均符合预期）"
else
    echo "判定：$FAIL 项需关注（见上）"
fi
exit $((FAIL > 0 ? 1 : 0))
