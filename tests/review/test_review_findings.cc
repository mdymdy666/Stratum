// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 审查发现的验证测试
//
// ===========================================================================
// 数据量标准（见 design/07-testing-standards.md）
// ===========================================================================
//
// 本文件遵循"测试默认大数据量"的要求：
//   - 存储引擎测试 ≥ 50,000 条 或 强制 ≥ 2 次 compaction
//   - 持久化测试必须包含"重启后再写"
//   - 队列/缓冲测试必须超过配置上限
//
// **为什么**：第 1 轮审查发现 9 个 Critical/High，其中 7 个是
// "小数据量 + 单次打开"的甜区测不出来的。小数据量让代码只走最顺的路径，
// 绕过了分层、合并、溢出、恢复等所有容易出错的分支。

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl.h"
#include "stratum/storage/lsm/lsm_engine.h"
#include "stratum/storage/memory/memory_engine.h"
#include "test_framework.h"
#include "test_isolation.h"

using namespace stratum;
using namespace stratum::storage;

namespace {

struct DirStats {
  int sst_count = 0;
  uint64_t sst_bytes = 0;
};

DirStats ScanDir(const std::string& dir) {
  DirStats stats;
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) {
    return stats;
  }
  while (struct dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".sst") == 0) {
      struct stat st {};
      if (::stat((dir + "/" + name).c_str(), &st) == 0) {
        stats.sst_count += 1;
        stats.sst_bytes += static_cast<uint64_t>(st.st_size);
      }
    }
  }
  ::closedir(d);
  return stats;
}

// 大数据量写入辅助：小 flush 阈值 → 必然触发多次 compaction
void WriteBulk(LsmEngine* engine, const std::string& prefix, int count,
               size_t payload_size, char filler) {
  WriteOptions wo;
  for (int i = 0; i < count; ++i) {
    char key[64];
    std::snprintf(key, sizeof(key), "%s-%08d", prefix.c_str(), i);
    engine->Put(wo, key, std::string(payload_size, filler));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// 【C1 验证】重启后写入同一 key，新值必须在 compaction 后仍然胜出
//
// 这是修复前**必然失败**的用例：
//   修复前 last_sequence_ 重启后回绕为 0，新值的 sequence 小于旧值，
//   被判定为"旧版本"而丢弃 → Get 返回旧值。
// ---------------------------------------------------------------------------
STRATUM_TEST(Regression, C1_SequenceWatermarkSurvivesRestart) {
  const std::string dir = std::string("/tmp/stratum_reg_c1_") + UniqueSuffix();
  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }

  // --- 第一次运行：写入并触发 compaction，然后关闭 ---
  {
    auto engine = std::unique_ptr<LsmEngine>(new LsmEngine());
    STRATUM_CHECK_OK(engine->Open(dir));
    engine->SetMemTableFlushThreshold(16 * 1024);

    WriteOptions wo;
    STRATUM_CHECK_OK(engine->Put(wo, "target", "OLD_VALUE"));

    // 大数据量：确保 target 落盘并经过 compaction
    WriteBulk(engine.get(), "filler", 30000, 64, 'x');
    STRATUM_CHECK_OK(engine->CompactRange("", ""));

    std::string v;
    ReadOptions ro;
    STRATUM_CHECK_OK(engine->Get(ro, "target", &v));
    STRATUM_CHECK_EQ(v, std::string("OLD_VALUE"));

    STRATUM_CHECK_OK(engine->Close());
  }

  // --- 第二次运行：重启后写同一 key ---
  {
    auto engine = std::unique_ptr<LsmEngine>(new LsmEngine());
    STRATUM_CHECK_OK(engine->Open(dir));

    const uint64_t seq_after_reopen = engine->CurrentSequence();
    ctx.param_desc = "重启后水位=" + std::to_string(seq_after_reopen);
    // 水位必须大于 0（修复前恒为 0）
    STRATUM_CHECK_TRUE(seq_after_reopen > 0);

    WriteOptions wo;
    STRATUM_CHECK_OK(engine->Put(wo, "target", "NEW_VALUE"));

    std::string v;
    ReadOptions ro;
    STRATUM_CHECK_OK(engine->Get(ro, "target", &v));
    STRATUM_CHECK_EQ(v, std::string("NEW_VALUE"));

    // 【关键】触发 compaction，检查新值是否被旧值覆盖
    STRATUM_CHECK_OK(engine->Flush());
    STRATUM_CHECK_OK(engine->CompactRange("", ""));

    v.clear();
    STRATUM_CHECK_OK(engine->Get(ro, "target", &v));
    ctx.param_desc += " compaction后=" + v;
    STRATUM_CHECK_EQ(v, std::string("NEW_VALUE"));

    STRATUM_CHECK_OK(engine->Close());
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
}

// ---------------------------------------------------------------------------
// 【C1 验证 2】重启后删除，compaction 后数据不得"复活"
// ---------------------------------------------------------------------------
STRATUM_TEST(Regression, C1_DeletedDataDoesNotResurrect) {
  const std::string dir = std::string("/tmp/stratum_reg_c1b_") + UniqueSuffix();
  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }

  {
    auto engine = std::unique_ptr<LsmEngine>(new LsmEngine());
    STRATUM_CHECK_OK(engine->Open(dir));
    engine->SetMemTableFlushThreshold(16 * 1024);

    WriteOptions wo;
    STRATUM_CHECK_OK(engine->Put(wo, "victim", "payload"));
    WriteBulk(engine.get(), "filler", 30000, 64, 'x');
    STRATUM_CHECK_OK(engine->CompactRange("", ""));
    STRATUM_CHECK_OK(engine->Close());
  }

  {
    auto engine = std::unique_ptr<LsmEngine>(new LsmEngine());
    STRATUM_CHECK_OK(engine->Open(dir));

    WriteOptions wo;
    STRATUM_CHECK_OK(engine->Delete(wo, "victim"));

    std::string v;
    ReadOptions ro;
    STRATUM_CHECK_TRUE(engine->Get(ro, "victim", &v).IsNotFound());

    // 【关键】compaction 后不得复活
    STRATUM_CHECK_OK(engine->Flush());
    STRATUM_CHECK_OK(engine->CompactRange("", ""));

    const Status s = engine->Get(ro, "victim", &v);
    ctx.param_desc = "compaction后=" + s.ToString() + " value=" + v;
    STRATUM_CHECK_TRUE(s.IsNotFound());

    STRATUM_CHECK_OK(engine->Close());
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
}

// ---------------------------------------------------------------------------
// 【C2 验证】多次 compaction 后，磁盘文件数必须有界
//
// 修复前：每次 compaction 都留下旧文件，文件数单调增长。
// ---------------------------------------------------------------------------
STRATUM_TEST(Regression, C2_CompactionReleasesDiskSpace) {
  const std::string dir = std::string("/tmp/stratum_reg_c2_") + UniqueSuffix();
  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }

  constexpr int kTotal = 50000;
  constexpr size_t kPayload = 64;

  {
    auto engine = std::unique_ptr<LsmEngine>(new LsmEngine());
    STRATUM_CHECK_OK(engine->Open(dir));
    engine->SetMemTableFlushThreshold(16 * 1024);

    WriteBulk(engine.get(), "bulk", kTotal, kPayload, 'z');

    const DirStats s = ScanDir(dir);
    const uint64_t data_bytes = static_cast<uint64_t>(kTotal) * kPayload;
    ctx.param_desc = "写入 " + std::to_string(kTotal) + " 条后: 文件数=" +
                     std::to_string(s.sst_count) + " 字节=" +
                     std::to_string(s.sst_bytes) + " 有效数据=" +
                     std::to_string(data_bytes);

    // 文件数必须有界（L0 阈值 4 + L1，允许少量余量）
    STRATUM_CHECK_LE(s.sst_count, 8);
    // 磁盘占用不应远超有效数据（允许 compaction 期间的临时冗余）
    STRATUM_CHECK_LE(s.sst_bytes, data_bytes * 3 + 1024 * 1024);

    STRATUM_CHECK_OK(engine->Close());
  }

  // 重启后再写，文件数仍应有界（修复前会持续增长）
  {
    auto engine = std::unique_ptr<LsmEngine>(new LsmEngine());
    STRATUM_CHECK_OK(engine->Open(dir));
    engine->SetMemTableFlushThreshold(16 * 1024);

    WriteBulk(engine.get(), "bulk2", 20000, kPayload, 'w');
    STRATUM_CHECK_OK(engine->Close());

    const DirStats s = ScanDir(dir);
    ctx.param_desc = "重启再写后: 文件数=" + std::to_string(s.sst_count);
    STRATUM_CHECK_LE(s.sst_count, 10);
  }

  if (system(("rm -rf " + dir).c_str()) != 0) {
    // 忽略
  }
}

// ---------------------------------------------------------------------------
// 【C4 验证】保留策略：max_bytes 路径（已知未实现，记录实际行为）
// ---------------------------------------------------------------------------
STRATUM_TEST(Regression, C4_RetentionByDurationEvicts) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  TimelineImpl timeline(std::move(engine));
  STRATUM_CHECK_OK(timeline.Open("mem://reg-c4"));

  TopicMetadata meta;
  meta.topic_name = "retention/bulk";
  uint32_t topic = 0;
  STRATUM_CHECK_OK(timeline.RegisterTopic(meta, &topic));

  // 大数据量：10000 条，跨 1000 秒
  constexpr uint64_t kBaseTs = 1700000000000000000ULL;
  constexpr int kCount = 10000;
  constexpr int kBatch = 1000;
  for (int base = 0; base < kCount; base += kBatch) {
    std::vector<Event> events;
    events.reserve(kBatch);
    for (int i = 0; i < kBatch; ++i) {
      Event e;
      e.topic_id = topic;
      e.seq = static_cast<uint32_t>(base + i);
      e.source_timestamp_ns =
          kBaseTs + static_cast<uint64_t>(base + i) * 100000000ULL;  // 每 0.1s
      e.payload.assign(128, 'r');
      events.push_back(std::move(e));
    }
    STRATUM_CHECK_OK(timeline.AppendBatch(events));
  }

  uint64_t before = 0;
  STRATUM_CHECK_OK(timeline.CountRange(topic, TimeRange(), &before));
  STRATUM_CHECK_EQ(before, static_cast<uint64_t>(kCount));

  // 只保留最近 10 秒
  RetentionPolicy policy;
  policy.retain_duration_ns = 10000000000ULL;
  STRATUM_CHECK_OK(timeline.SetRetentionPolicy(policy));

  const Status rs = timeline.RunRetention();
  uint64_t after = 0;
  STRATUM_CHECK_OK(timeline.CountRange(topic, TimeRange(), &after));
  ctx.param_desc = "淘汰前=" + std::to_string(before) +
                   " 淘汰后=" + std::to_string(after) + " 返回=" + rs.ToString();

  // 按时间淘汰应显著减少数据
  STRATUM_CHECK_TRUE(after < before);
}

// ---------------------------------------------------------------------------
// 【H1 验证】反向迭代定位（已知缺陷，记录实际行为）
//
// 这是修复前**会失败**的用例，用于证明缺陷真实存在。
// ---------------------------------------------------------------------------
STRATUM_TEST(Regression, H1_SeekToLastLocatesLastElement) {
  auto engine = StorageEnginePtr(new MemoryEngine());
  STRATUM_CHECK_OK(engine->Open("mem://reg-h1"));

  WriteOptions wo;
  // 大数据量确保经过 map 的多种路径
  constexpr int kCount = 20000;
  for (int i = 0; i < kCount; ++i) {
    char key[32];
    std::snprintf(key, sizeof(key), "k-%08d", i);
    STRATUM_CHECK_OK(engine->Put(wo, key, "v"));
  }

  ReadOptions ro;
  auto it = engine->NewIterator(ro, "", "");
  it->SeekToLast();

  STRATUM_CHECK_TRUE(it->Valid());
  char expected[32];
  std::snprintf(expected, sizeof(expected), "k-%08d", kCount - 1);
  ctx.param_desc = std::string("SeekToLast -> ") +
                   (it->Valid() ? it->key() : std::string("<invalid>")) +
                   " 期望=" + expected;
  STRATUM_CHECK_EQ(it->key(), std::string(expected));

  engine->Close();
}

STRATUM_TEST_MAIN()
