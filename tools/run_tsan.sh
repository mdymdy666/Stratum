#!/usr/bin/env bash
#
# tools/run_tsan.sh —— 用 ThreadSanitizer 验证并发正确性
#
# 【为什么需要这个脚本】
#
# 2026-10-15 修复了一个真实的并发 use-after-free：`TimelineImplV2` 的
# `if (!opened_)` 检查在 `lock_guard` **之前**执行，而 `Close()` 会在持锁
# 状态下释放 `db_` —— 于是存在"读到 opened_ 为 true，随后使用已释放 db_"
# 的窗口。该 bug 只在**并发关闭**时出现，普通构建侥幸通过。
#
# 结论：**并发正确性不能靠"跑一次没崩"来验证，必须上 TSan。**
#
# 【WSL 下的一个坑（本脚本已处理）】
#
# 直接跑 TSan 会立刻失败：
#     FATAL: ThreadSanitizer: unexpected memory mapping 0x...
# 原因是 WSL 内核的 ASLR 随机位宽（vm.mmap_rnd_bits，常为 32）与 TSan
# 的 shadow memory 布局假设冲突。**不需要 sudo 改内核参数**，
# 用 `setarch -R`（禁用本进程的 ASLR）即可绕过。
#
# 【用法】
#     bash tools/run_tsan.sh                     # 默认跑并发相关测试
#     bash tools/run_tsan.sh test_xxx [test_yyy] # 指定测试
#
# 构建目录固定为 build-tsan，**不污染主 build**（主 build 用于常规 ctest）。

set -u

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ" || exit 1

BUILD_DIR="build-tsan"
DEFAULT_TARGETS=(test_concurrent_register test_pruned_ranges test_topic_type_definition)

if [ "$#" -gt 0 ]; then
  TARGETS=("$@")
else
  TARGETS=("${DEFAULT_TARGETS[@]}")
fi

echo "=== 1) 配置 $BUILD_DIR ==="
cmake -S . -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1 -fno-omit-frame-pointer" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" > /dev/null || {
  echo "  配置失败"; exit 1
}
echo "  ✅ 完成"

echo
echo "=== 2) 构建: ${TARGETS[*]} ==="
# TSan 需要重编全部依赖，第一次会比较慢
cmake --build "$BUILD_DIR" --target "${TARGETS[@]}" -j "$(nproc)" > /tmp/_tsan_build.log 2>&1 || {
  echo "  构建失败："
  grep -E "error:" /tmp/_tsan_build.log | head -15 | sed 's/^/    /'
  rm -f /tmp/_tsan_build.log
  exit 1
}
rm -f /tmp/_tsan_build.log
echo "  ✅ 完成"

echo
echo "=== 3) 运行（关键：setarch -R 禁用 ASLR）==="
echo "  说明：WSL 的 ASLR 位宽与 TSan shadow memory 冲突，"
echo "        直接跑会报 'unexpected memory mapping'；setarch -R 可绕过。"
export TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 history_size=7"

FAILED=0
RACES=0
for t in "${TARGETS[@]}"; do
  BIN="$BUILD_DIR/tests/$t"
  if [ ! -x "$BIN" ]; then
    echo "  [跳过] $t 未构建出可执行文件"
    continue
  fi
  LOG="/tmp/_tsan_${t}.log"
  setarch "$(uname -m)" -R "$BIN" > "$LOG" 2>&1
  RC=$?
  # 注意：不要写 `grep -c ... || echo 0`。
  # grep -c 总会输出一个数字，但计数为 0 时退出码是 1，
  # 于是 `|| echo 0` 会再追加一行，变量变成 "0\n0"，
  # 后续 `[ "$N" -gt 0 ]` 就会报 "integer expression expected"。
  N_RACE=$(grep -c "WARNING: ThreadSanitizer" "$LOG" 2>/dev/null)
  N_FATAL=$(grep -c "FATAL: ThreadSanitizer" "$LOG" 2>/dev/null)
  N_RACE=${N_RACE:-0}
  N_FATAL=${N_FATAL:-0}

  if [ "$N_FATAL" -gt 0 ]; then
    echo "  [环境] $t: TSan 无法运行（$N_FATAL 条 FATAL）"
    grep "FATAL" "$LOG" | head -2 | sed 's/^/      /'
    FAILED=$((FAILED + 1))
  elif [ "$N_RACE" -gt 0 ]; then
    echo "  ✗ $t: 发现 $N_RACE 条竞争告警（退出码 $RC）"
    grep -A12 "WARNING: ThreadSanitizer" "$LOG" | head -30 | sed 's/^/      /'
    RACES=$((RACES + N_RACE))
  elif [ "$RC" -ne 0 ]; then
    echo "  ✗ $t: 退出码 $RC（无竞争告警）"
    tail -6 "$LOG" | sed 's/^/      /'
    FAILED=$((FAILED + 1))
  else
    echo "  ✅ $t: 通过，零竞争告警"
  fi
  rm -f "$LOG"
done

echo
echo "=== 结论 ==="
echo "  竞争告警: $RACES 条"
echo "  运行失败: $FAILED 个"
if [ "$RACES" -gt 0 ]; then
  echo "  -> 有竞争需要修复。TSan 的告警里会同时给出两个访问点的调用栈，"
  echo "     按它指出的位置去核对锁的范围即可。"
  exit 2
elif [ "$FAILED" -gt 0 ]; then
  echo "  -> 有测试无法运行，先确认是否为环境问题（见上面的 FATAL 行）。"
  exit 1
fi
echo "  -> 未发现数据竞争。"
exit 0
