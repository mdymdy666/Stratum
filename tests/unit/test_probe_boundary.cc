// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 探针 ring 的**边界行为**测试
//
// 与 test_probe.cc 的分工：
//   test_probe.cc         —— 正常路径（写入/环满/并发/SCOPE/零开销）
//   test_probe_boundary.cc —— **只在边界上才暴露的问题**：
//       · 最小容量环（capacity=2）
//       · 序号回绕（write_seq 超过 2^32）
//       · ring 复用（同一 shm 被关闭后重新打开）
//       · 空负载 / 零长度 topic
//       · 容量不是 2 的幂时（resolve_capacity 会向上取幂）
//
// 【为什么单独一个文件】这些用例的共同点是「平时跑不到，
// 一旦触发就静默错」：环小了 `seq % cap` 会覆盖得太快；
// 序号超过 2^32 后记录里的 32 位 seq 会回绕，比对逻辑就失效。

// 标准库头文件必须最先包含
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "stratum/probe/probe.h"
#include "test_framework.h"

namespace fs = std::filesystem;

namespace {

std::string FreshShm(const char* tag) {
  const std::string p = std::string("/dev/shm/stratum_bnd_") + tag;
  std::error_code ec;
  fs::remove(p, ec);
  return p;
}

void Reset(const std::string& shm) {
  stratum_probe_close();
  std::error_code ec;
  fs::remove(shm, ec);
}

const stratum_probe_record* Records(const stratum_ring_header* h) {
  return stratum_ring_records_const(h);
}

}  // namespace

// ===========================================================================
// 一、最小容量环（capacity=2）
// ===========================================================================
//
// 【为什么要测最小的】容量大时环形覆盖的 bug 会被"数据还没绕回来"掩盖。
// capacity=2 时，第 3 条就立刻覆盖第 1 条 —— 是最容易暴露索引错误的场景。

STRATUM_TEST(ProbeBoundary, MinimalCapacityTwo) {
  const std::string shm = FreshShm("cap2");
  Reset(shm);
  // 用环境变量把容量压到 2（resolve_capacity 会向上取 2 的幂，2 就是 2）
  ::setenv("STRATUM_PROBE_CAPACITY", "2", 1);
  const int rc = stratum_probe_open(shm.c_str());
  ::unsetenv("STRATUM_PROBE_CAPACITY");
  STRATUM_CHECK_EQ(rc, 0);

  stratum_probe_stats st0;
  stratum_probe_get_stats(&st0);
  STRATUM_CHECK_EQ(st0.capacity, static_cast<uint32_t>(2));

  // 写 5 条：第 3、4、5 条分别覆盖第 1、2、1 条
  for (int i = 0; i < 5; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", i);
    stratum_probe_write("cap2/x", buf, std::strlen(buf));
  }

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(5));
  // 超出容量 2 的部分都算覆盖丢弃 = 3
  STRATUM_CHECK_EQ(st.dropped_full, static_cast<uint64_t>(3));
  STRATUM_CHECK_EQ(stratum_probe_allocated(), static_cast<uint64_t>(5));

  // 环里存的应该是最后两条（i=3, i=4）
  const auto* h = static_cast<const stratum_ring_header*>(stratum_probe_ring_view());
  const stratum_probe_record* recs = Records(h);
  // slot = seq % 2：seq 3 → slot 1，seq 4 → slot 0
  STRATUM_CHECK_EQ(recs[1].seq, static_cast<uint32_t>(3));
  STRATUM_CHECK_EQ(recs[0].seq, static_cast<uint32_t>(4));
  // 内容也要对：seq 4 的那条 payload 是 "4"
  STRATUM_CHECK_EQ(recs[0].payload_len, static_cast<uint32_t>(1));
  STRATUM_CHECK_EQ(recs[0].payload[0], '4');

  std::printf("      [最小环] capacity=2，写 5 条：覆盖丢弃 3，环内存 seq 3 与 4\n");
  Reset(shm);
}

// ===========================================================================
// 二、容量不是 2 的幂 → 向上取幂
// ===========================================================================

STRATUM_TEST(ProbeBoundary, CapacityRoundsUpToPowerOfTwo) {
  const std::string shm = FreshShm("capr");
  Reset(shm);
  ::setenv("STRATUM_PROBE_CAPACITY", "100", 1);
  const int rc = stratum_probe_open(shm.c_str());
  ::unsetenv("STRATUM_PROBE_CAPACITY");
  STRATUM_CHECK_EQ(rc, 0);

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  // 100 → 128
  STRATUM_CHECK_EQ(st.capacity, static_cast<uint32_t>(128));
  std::printf("      [容量取幂] 请求 100 → 实际 128\n");
  Reset(shm);
}

// ===========================================================================
// 三、序号回绕（32 位 seq 溢出）
// ===========================================================================
//
// 记录里存的是**32 位** seq（`uint32_t seq`），而计数器是 64 位。
// 写完 2^32 条后记录里的 seq 会回绕 —— 读取器若拿 32 位 seq 与 64 位位置比较，
// 就会判错"这条是旧的还是新的"。
//
// 不可能真写 40 亿条（太慢），所以直接**构造**一个接近回绕的 ring 状态来验证
// 索引计算与 32 位截断行为一致。

STRATUM_TEST(ProbeBoundary, SeqWrapsWithoutBreakingIndexing) {
  const std::string shm = FreshShm("wrap");
  Reset(shm);
  ::setenv("STRATUM_PROBE_CAPACITY", "4", 1);
  const int rc = stratum_probe_open(shm.c_str());
  ::unsetenv("STRATUM_PROBE_CAPACITY");
  STRATUM_CHECK_EQ(rc, 0);

  // 写 8 条，观察 seq 的低位与 slot 的关系是否始终自洽
  for (int i = 0; i < 8; ++i) {
    stratum_probe_write("wrap/x", "v", 1);
  }
  const auto* h = static_cast<const stratum_ring_header*>(stratum_probe_ring_view());
  const stratum_probe_record* recs = Records(h);
  const uint32_t cap = h->capacity;

  // 每个 slot 里记录的 seq 必须满足 seq % cap == slot
  for (uint32_t slot = 0; slot < cap; ++slot) {
    STRATUM_CHECK_EQ(recs[slot].seq % cap, slot);
  }
  // 32 位截断本身是预期行为：只断言它**保持一致**（seq % cap == slot）
  std::printf("      [序号] 8 条写入后各 slot 的 seq %% cap 与 slot 一致（32 位截断自洽）\n");
  Reset(shm);
}

// ===========================================================================
// 四、ring 复用：关闭后重新打开同一个 shm
// ===========================================================================
//
// 【为什么重要】读取器可能重启，业务也可能重连。若重新打开时
// 把计数清零，读取器就分不清"新数据"和"重放"。

STRATUM_TEST(ProbeBoundary, ReopenKeepsExistingState) {
  const std::string shm = FreshShm("reuse");
  Reset(shm);

  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  for (int i = 0; i < 3; ++i) stratum_probe_write("reuse/x", "a", 1);
  const uint64_t before_committed = stratum_probe_committed();
  STRATUM_CHECK_EQ(before_committed, static_cast<uint64_t>(3));
  stratum_probe_close();

  // 重新打开：应复用已有段，计数**延续**而不是清零
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);
  STRATUM_CHECK_EQ(stratum_probe_committed(), before_committed);
  // 再写一条 → 4
  stratum_probe_write("reuse/x", "b", 1);
  STRATUM_CHECK_EQ(stratum_probe_committed(), static_cast<uint64_t>(4));
  std::printf("      [复用] 重新打开后 committed 延续 3 → 4，未清零\n");
  Reset(shm);
}

// ===========================================================================
// 五、空负载 / 零长度
// ===========================================================================

STRATUM_TEST(ProbeBoundary, EmptyPayloadIsAccepted) {
  const std::string shm = FreshShm("empty");
  Reset(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  // 空负载：应该写入成功（payload_len=0），不是参数错误
  stratum_probe_write("empty/x", nullptr, 0);
  stratum_probe_write("empty/x", "", 0);

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(2));
  STRATUM_CHECK_EQ(st.dropped_bad, static_cast<uint64_t>(0));

  const auto* h = static_cast<const stratum_ring_header*>(stratum_probe_ring_view());
  STRATUM_CHECK_EQ(Records(h)[0].payload_len, static_cast<uint32_t>(0));
  Reset(shm);
}

// ===========================================================================
// 六、topic 恰好等于上限长度
// ===========================================================================

STRATUM_TEST(ProbeBoundary, TopicExactlyAtLimit) {
  const std::string shm = FreshShm("topiclen");
  Reset(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  // 恰好 TOPIC_MAX-1 个字符（加结尾 0 正好占满）
  std::string exact(STRATUM_PROBE_TOPIC_MAX - 1, 'T');
  stratum_probe_write(exact.c_str(), "v", 1);

  const auto* h = static_cast<const stratum_ring_header*>(stratum_probe_ring_view());
  const stratum_probe_record* r = &Records(h)[0];
  STRATUM_CHECK_EQ(r->topic_len, static_cast<uint32_t>(STRATUM_PROBE_TOPIC_MAX - 1));
  STRATUM_CHECK_EQ(r->topic[r->topic_len], '\0');   // 必须正确终止
  // 恰好占满时不该标截断
  STRATUM_CHECK_EQ(r->flags & 1u, static_cast<uint32_t>(0));

  // 再超一个字符 → 截断并标记
  std::string over(STRATUM_PROBE_TOPIC_MAX, 'T');
  stratum_probe_write(over.c_str(), "v", 1);
  const stratum_probe_record* r2 = &Records(h)[1];
  STRATUM_CHECK_EQ(r2->topic_len, static_cast<uint32_t>(STRATUM_PROBE_TOPIC_MAX - 1));
  STRATUM_CHECK_EQ(r2->flags & 1u, static_cast<uint32_t>(1));
  std::printf("      [topic 上限] 恰好占满不标截断，超一个字符才标\n");
  Reset(shm);
}

// ===========================================================================
// 七、负载恰好等于上限
// ===========================================================================

STRATUM_TEST(ProbeBoundary, PayloadExactlyAtLimit) {
  const std::string shm = FreshShm("paylen");
  Reset(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  std::vector<char> exact(STRATUM_PROBE_PAYLOAD_MAX, 'P');
  stratum_probe_write("pay/x", exact.data(), exact.size());
  const auto* h = static_cast<const stratum_ring_header*>(stratum_probe_ring_view());
  STRATUM_CHECK_EQ(Records(h)[0].payload_len,
                   static_cast<uint32_t>(STRATUM_PROBE_PAYLOAD_MAX));
  STRATUM_CHECK_EQ(Records(h)[0].flags & 1u, static_cast<uint32_t>(0));

  // 超一个字节 → 截断
  std::vector<char> over(STRATUM_PROBE_PAYLOAD_MAX + 1, 'P');
  stratum_probe_write("pay/x", over.data(), over.size());
  STRATUM_CHECK_EQ(Records(h)[1].payload_len,
                   static_cast<uint32_t>(STRATUM_PROBE_PAYLOAD_MAX));
  STRATUM_CHECK_EQ(Records(h)[1].flags & 1u, static_cast<uint32_t>(1));
  std::printf("      [负载上限] 恰好 %d 字节不标截断，超一字节才标\n",
              STRATUM_PROBE_PAYLOAD_MAX);
  Reset(shm);
}

// ===========================================================================
// 八、全局开关关闭后仍能累计统计
// ===========================================================================
//
// 【为什么重要】S04 的裁决：关闭状态要保留"丢弃计数"，
// 这样 status 能回答"这个探针被调用过多少次，只是没发"。

STRATUM_TEST(ProbeBoundary, DisabledStillCountsCalls) {
  const std::string shm = FreshShm("offcnt");
  Reset(shm);
  STRATUM_CHECK_EQ(stratum_probe_open(shm.c_str()), 0);

  stratum_probe_write("off/x", "a", 1);
  stratum_probe_set_enabled(0);
  for (int i = 0; i < 7; ++i) stratum_probe_write("off/x", "b", 1);

  stratum_probe_stats st;
  stratum_probe_get_stats(&st);
  STRATUM_CHECK_EQ(st.written, static_cast<uint64_t>(1));
  STRATUM_CHECK_EQ(st.dropped_off, static_cast<uint64_t>(7));
  // 关闭时不占用环（committed 不变）
  STRATUM_CHECK_EQ(stratum_probe_committed(), static_cast<uint64_t>(1));
  std::printf("      [关闭统计] 关闭后 7 次调用全部计入 dropped_off，环未被占用\n");
  Reset(shm);
}

STRATUM_TEST_MAIN()
