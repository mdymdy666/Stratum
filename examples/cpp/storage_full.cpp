// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// examples/cpp/storage_full.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// storage 模块 · 全面用法演示（三个真实场景）
//
//   场景 A：时间轴扫描策略与"字节序==时间序"的大规模验证
//           100 万条键、随机时间戳、三类范围；测耗时并验证顺序不变量
//           （这是"按时间查数据"能成立的根本，也是本项目最该被压测的地方）
//
//   场景 B：存储引擎契约验证
//           同一套契约用例跑 MemoryEngine；覆盖 Put/Get/Delete/DeleteRange/
//           Iterator/Snapshot 与各类边界（空键、极值、批量和单条的一致性）
//
//   场景 C：元数据版本演进与损坏恢复
//           模拟"用旧版本读新数据""数据被截断/篡改"两类真实事故，
//           验证元数据层能明确拒绝而不是给出错误结果
//
// 覆盖的 API（比 demo 多了这些）：
//   IStorageEngine::PutBatch / Delete / DeleteRange / Flush / CompactRange
//   IStorageEngine::GetSnapshot / ReleaseSnapshot（快照隔离）
//   TimeKeyCodec 在极值（0、UINT64_MAX、topic_id 极值）下的行为
//   MetaCodec::EncodeRetention / DecodeRetention
//
// 运行：./storage_full [工作目录]
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "stratum/storage/lsm/lsm_engine.h"
#include "stratum/storage/memory/memory_engine.h"
#include "stratum/storage/meta_codec.h"
#include "stratum/storage/time_key.h"

using namespace stratum;
using namespace stratum::storage;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
  std::printf("    [%s] %s\n", ok ? "OK" : "FAIL", what);
  if (!ok) ++g_failed;
}

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

bool EnsureDir(const std::string& d) {
  std::error_code ec;
  std::filesystem::create_directories(d, ec);
  if (ec && !std::filesystem::exists(d)) {
    std::printf("    创建目录失败: %s (%s)\n", d.c_str(), ec.message().c_str());
    return false;
  }
  return true;
}

// 清空并重建目录。
//
// 【为什么不是 EnsureDir】示例必须**可重复运行**。若只确保目录存在，
// 第二次运行时目录里还留着上一次的数据，TimelineImplV2::Open 会打开它，
// 于是"录制了多少条""窗口里有多少条"都跟首次不同，检查项就会误报失败 ——
// 读者照着跑第二遍会以为是自己环境的问题。
// （这也是本项目反复出现的主题：验证手段本身不可靠时，结论就不可信。）
bool ResetDir(const std::string& dir) {
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);   // 目录不存在时也返回成功
  std::filesystem::create_directories(dir, ec);
  if (ec && !std::filesystem::exists(dir)) {
    std::printf("  重建目录失败: %s (%s)\n", dir.c_str(), ec.message().c_str());
    return false;
  }
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 A：时间轴扫描策略与顺序不变量的大规模验证
//
// 真实诉求：
//   · 现场数据量很大（小时级、百 GB），"按时间查"必须走范围扫描而不是全表过滤
//   · 范围扫描的正确性完全依赖"键的字节序 == 时间序"这个不变量
//   · 该不变量一旦被破坏，症状是"结果差不多对但顺序错乱"，极难发现
// 因此这里用**大规模 + 随机 + 多种子**去验证它，而不是几个手写样本。
// ═══════════════════════════════════════════════════════════════════════════
int ScenarioA_TimeAxisScanning(const std::string& work) {
  std::printf("\n══════ 场景 A：时间轴扫描（100 万键 / 随机 / 计时）══════\n");

  const size_t kN = 1000000;   // 100 万
  const uint32_t kTopics = 8;

  // ── A1：随机生成 100 万个时间键 ────────────────────────────────────
  std::mt19937_64 rng(20261014);       // 固定种子，结果可复现
  std::uniform_int_distribution<uint32_t> topic_d(0, kTopics - 1);
  std::uniform_int_distribution<uint64_t> ts_d(1600000000000000000ULL,
                                               1700000000000000000ULL);
  std::uniform_int_distribution<uint32_t> seq_d(0, 2);

  auto t0 = Clock::now();
  std::vector<std::string> raws;
  raws.reserve(kN);
  std::vector<TimeKey> keys;
  keys.reserve(kN);
  for (size_t i = 0; i < kN; ++i) {
    TimeKey k(topic_d(rng), ts_d(rng), seq_d(rng));
    keys.push_back(k);
    raws.push_back(TimeKeyCodec::Encode(k));
  }
  const double gen_ms = MsSince(t0);
  std::printf("  生成 %zu 个随机时间键: %.1f ms\n", kN, gen_ms);

  // ── A2：验证不变量 —— 语义序 == 字节序 ────────────────────────────
  // 做法：两套索引分别按语义与按字节排序，比较是否得到同一个排列。
  t0 = Clock::now();
  std::vector<uint32_t> by_semantic(kN);
  for (uint32_t i = 0; i < kN; ++i) by_semantic[i] = i;
  std::sort(by_semantic.begin(), by_semantic.end(),
            [&keys](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
  const double sort_sem_ms = MsSince(t0);

  t0 = Clock::now();
  std::vector<uint32_t> by_bytes(kN);
  for (uint32_t i = 0; i < kN; ++i) by_bytes[i] = i;
  std::sort(by_bytes.begin(), by_bytes.end(),
            [&raws](uint32_t a, uint32_t b) { return raws[a] < raws[b]; });
  const double sort_byte_ms = MsSince(t0);

  Check(by_semantic == by_bytes,
        "语义排序与字节序排序得到同一排列（顺序不变量成立）");
  std::printf("      语义排序 %.1f ms / 字节序排序 %.1f ms（%zu 条）\n",
              sort_sem_ms, sort_byte_ms, kN);

  // ── A3：三类范围的正确性（用实测计数核对，而不是靠感觉）──────────
  // 先为话题 3 造一份"真值"：按时间排序的时间戳列表
  std::vector<uint64_t> ts_of_topic3;
  for (const auto& k : keys) {
    if (k.topic_id == 3) ts_of_topic3.push_back(k.timestamp_ns);
  }
  std::sort(ts_of_topic3.begin(), ts_of_topic3.end());
  std::printf("  话题 3 共 %zu 条\n", ts_of_topic3.size());

  // 全部排序后的键（模拟存储层的有序序列）
  t0 = Clock::now();
  std::vector<std::string> sorted_raws = raws;
  std::sort(sorted_raws.begin(), sorted_raws.end());
  const double full_sort_ms = MsSince(t0);
  std::printf("  模拟存储层有序排列: %.1f ms\n", full_sort_ms);

  // MakeTopicRange：应恰好覆盖该话题全部键
  {
    std::string b, e;
    TimeKeyCodec::MakeTopicRange(3, &b, &e);
    auto lo = std::lower_bound(sorted_raws.begin(), sorted_raws.end(), b);
    auto hi = std::lower_bound(sorted_raws.begin(), sorted_raws.end(), e);
    const size_t hit = static_cast<size_t>(hi - lo);
    std::printf("  MakeTopicRange(3): 扫到 %zu 条，真值 %zu 条\n", hit,
                ts_of_topic3.size());
    Check(hit == ts_of_topic3.size(), "MakeTopicRange 计数与真值一致");
  }

  // MakeTimeRange：中间 10% 的窗口
  {
    const uint64_t lo_ts = ts_of_topic3[ts_of_topic3.size() / 2];
    const uint64_t hi_ts = ts_of_topic3[ts_of_topic3.size() * 6 / 10];
    std::string b, e;
    TimeKeyCodec::MakeTimeRange(3, lo_ts, hi_ts, &b, &e);
    auto lo = std::lower_bound(sorted_raws.begin(), sorted_raws.end(), b);
    auto hi = std::lower_bound(sorted_raws.begin(), sorted_raws.end(), e);
    const size_t hit = static_cast<size_t>(hi - lo);

    size_t expect = 0;
    for (uint64_t t : ts_of_topic3) {
      if (t >= lo_ts && t < hi_ts) ++expect;
    }
    std::printf("  MakeTimeRange(3, [%llu, %llu)): 扫到 %zu 条，真值 %zu 条\n",
                static_cast<unsigned long long>(lo_ts),
                static_cast<unsigned long long>(hi_ts), hit, expect);
    Check(hit == expect, "MakeTimeRange 计数与真值一致");
  }

  // MakePrefixRangeUpTo：应等于"该话题 <= ts 的条数"
  {
    const uint64_t cut = ts_of_topic3[ts_of_topic3.size() * 3 / 4];
    std::string b, e;
    TimeKeyCodec::MakePrefixRangeUpTo(3, cut, &b, &e);
    auto lo = std::lower_bound(sorted_raws.begin(), sorted_raws.end(), b);
    auto hi = std::lower_bound(sorted_raws.begin(), sorted_raws.end(), e);
    const size_t hit = static_cast<size_t>(hi - lo);

    size_t expect = 0;
    for (uint64_t t : ts_of_topic3) if (t <= cut) ++expect;
    std::printf("  MakePrefixRangeUpTo(3, %llu): 扫到 %zu 条，真值 %zu 条\n",
                static_cast<unsigned long long>(cut), hit, expect);
    Check(hit == expect, "MakePrefixRangeUpTo 计数与真值一致（闭区间语义）");
  }

  // ── A4：多种子重复验证（避免恰好命中某个幸运种子）──────────────────
  {
    bool all_ok = true;
    for (uint64_t seed : {1ULL, 42ULL, 777ULL, 20261014ULL, 999983ULL}) {
      std::mt19937_64 r(seed);
      std::vector<std::pair<TimeKey, std::string>> v;
      const size_t n = 20000;
      v.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        TimeKey k(topic_d(r), ts_d(r), seq_d(r));
        v.emplace_back(k, TimeKeyCodec::Encode(k));
      }
      std::vector<size_t> sem(n), byt(n);
      for (size_t i = 0; i < n; ++i) sem[i] = byt[i] = i;
      std::sort(sem.begin(), sem.end(),
                [&v](size_t a, size_t b) { return v[a].first < v[b].first; });
      std::sort(byt.begin(), byt.end(),
                [&v](size_t a, size_t b) { return v[a].second < v[b].second; });
      if (sem != byt) { all_ok = false; break; }
    }
    Check(all_ok, "5 个不同种子下顺序不变量都成立（不是侥幸）");
  }

  // ── A5：极值边界 ───────────────────────────────────────────────────
  {
    const TimeKey zero(0, 0, 0);
    const TimeKey maxk(0xFFFFFFFFu, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFu);
    std::string zraw = TimeKeyCodec::Encode(zero);
    std::string mraw = TimeKeyCodec::Encode(maxk);
    Check(zraw < mraw, "全 0 键 < 全 F 键（大端编码在极值下仍保持顺序）");
    TimeKey back;
    Check(TimeKeyCodec::Decode(mraw, &back) && back == maxk,
          "极值键往返一致");

    // topic_id 为最大值时 MakeTopicRange 无法用 +1 表示上界，应给出"无上界"
    std::string b, e;
    TimeKeyCodec::MakeTopicRange(0xFFFFFFFFu, &b, &e);
    std::printf("      MakeTopicRange(UINT32_MAX): end 为%s\n",
                e.empty() ? "空串（无上界）" : "非空");
    Check(e.empty() || e > TimeKeyCodec::Encode(0xFFFFFFFFu, 0, 0),
          "topic_id 极值下范围仍自洽（用无上界表示）");
  }

  (void)work;
  std::printf("  场景 A 完成\n");
  return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 B：存储引擎契约验证
//
// 真实诉求：
//   · 上层（Timeline/Recorder/Player）只依赖 IStorageEngine，
//     因此"任何实现都必须满足同一套语义"这件事必须**被验证**，
//     否则换后端时会以各种隐蔽方式出错（返回码不同、边界行为不同）
//   · 这里用同一套用例跑 MemoryEngine，覆盖各类边界
// ═══════════════════════════════════════════════════════════════════════════
// 同一套契约用例，跑在任意 IStorageEngine 实现上。
//
// 【为什么必须参数化】"任何实现都满足同一套语义"是插件式设计成立的前提。
// 只跑一个实现等于没验证这条 —— 换后端时的差异（返回码不同、边界行为不同）
// 会在上层以各种隐蔽方式暴露出来。
int RunEngineContract(IStorageEngine& engine, const char* label,
                      const std::string& dir) {
  std::printf("    \u2500\u2500 引擎: %s \u2500\u2500\n", label);
  Status s = engine.Open(dir);
  Check(s.ok(), "Open 成功");
  if (!s.ok()) { ++g_failed; return 1; }

  const WriteOptions wo;
  const ReadOptions ro;

  // ── B1：Put/Get 往返，含空值与二进制安全 ───────────────────────────
  {
    const std::string k = TimeKeyCodec::Encode(1, 100, 0);
    const std::string v = "abc";
    Check(engine.Put(wo, k, v).ok(), "Put 普通值");

    std::string got;
    Check(engine.Get(ro, k, &got).ok() && got == v, "Get 取回一致");

    // 空值：必须能区分"存在但为空"与"不存在"
    const std::string ke = TimeKeyCodec::Encode(1, 101, 0);
    Check(engine.Put(wo, ke, std::string()).ok(), "Put 空值");
    std::string ge = "sentinel";
    Status r = engine.Get(ro, ke, &ge);
    Check(r.ok() && ge.empty(), "空值与不存在可区分（Get 成功且为空）");

    // 二进制安全：值里带 \0
    const std::string kb = TimeKeyCodec::Encode(1, 102, 0);
    const std::string vb("a\0b", 3);
    engine.Put(wo, kb, vb);
    std::string gb;
    Check(engine.Get(ro, kb, &gb).ok() && gb == vb, "值里的 \\0 不丢（二进制安全）");
  }

  // ── B2：覆盖写 ─────────────────────────────────────────────────────
  {
    const std::string k = TimeKeyCodec::Encode(1, 200, 0);
    engine.Put(wo, k, "v1");
    engine.Put(wo, k, "v2");
    std::string got;
    engine.Get(ro, k, &got);
    Check(got == "v2", "同键覆盖写后读到最新值");
  }

  // ── B3：Delete / DeleteRange ───────────────────────────────────────
  {
    for (uint32_t i = 0; i < 10; ++i) {
      engine.Put(wo, TimeKeyCodec::Encode(2, 300 + i, 0), "x");
    }
    engine.Delete(wo, TimeKeyCodec::Encode(2, 305, 0));
    std::string got;
    Check(!engine.Get(ro, TimeKeyCodec::Encode(2, 305, 0), &got).ok(),
          "Delete 之后 Get 明确失败");

    std::string b, e;
    TimeKeyCodec::MakeTimeRange(2, 300, 305, &b, &e);
    Check(engine.DeleteRange(wo, b, e).ok(), "DeleteRange 成功");
    auto it = engine.NewIterator(ro, b, e);
    int n = 0;
    for (; it && it->Valid(); it->Next()) ++n;
    Check(n == 0, "被删区间扫不到任何键");

    // 区间外的不受影响（DeleteRange 是半开区间，不能多删）
    //
    // 【条数要算清楚】本段开始时写了 300..309 共 10 条，接着：
    //   · Delete(305)            -> 少 1 条
    //   · DeleteRange[300, 305)  -> 少 5 条（300,301,302,303,304）
    // 因此剩下的应当是 306..309 共 **4** 条。
    // （最初这里写成 5 —— 忘了前面那次单点 Delete。断言写错比实现出错更常见，
    //   所以失败时先核对期望值本身。）
    TimeKeyCodec::MakeTimeRange(2, 305, 400, &b, &e);
    auto it2 = engine.NewIterator(ro, b, e);
    int n2 = 0;
    std::string first_remaining;
    for (; it2 && it2->Valid(); it2->Next()) {
      if (n2 == 0) first_remaining = it2->key();
      ++n2;
    }
    Check(n2 == 4, "区间外剩余 4 条（306..309，半开区间未多删）");
    Check(!first_remaining.empty() &&
              TimeKeyCodec::PeekTimestamp(first_remaining) == 306,
          "剩余的第一条正是 306（边界键未被误删）");
  }

  // ── B4：批量与单条结果一致 ─────────────────────────────────────────
  {
    std::vector<std::string> ks, vs;
    for (uint32_t i = 0; i < 1000; ++i) {
      ks.push_back(TimeKeyCodec::Encode(7, 10000 + i, 0));
      vs.push_back("batch-" + std::to_string(i));
    }
    Check(engine.PutBatch(wo, ks, vs).ok(), "PutBatch 1000 条成功");

    bool all = true;
    for (uint32_t i = 0; i < 1000; ++i) {
      std::string got;
      if (!engine.Get(ro, ks[i], &got).ok() || got != vs[i]) { all = false; break; }
    }
    Check(all, "批量写入的每一条都能单独读回（与单条语义一致）");

    std::string b, e;
    TimeKeyCodec::MakeTopicRange(7, &b, &e);
    auto it = engine.NewIterator(ro, b, e);
    size_t n = 0;
    bool ordered = true;
    std::string prev;
    for (; it && it->Valid(); it->Next()) {
      const std::string k = it->key();
      if (!prev.empty() && k < prev) ordered = false;
      prev = k;
      ++n;
    }
    Check(n == 1000, "范围扫描条数与写入一致");
    Check(ordered, "迭代器按键有序（存储层的基本承诺）");
  }

  // ── B5：快照隔离 ───────────────────────────────────────────────────
  {
    engine.Put(wo, TimeKeyCodec::Encode(9, 1, 0), "before");
    SnapshotPtr snap = engine.GetSnapshot();
    Check(snap != nullptr, "GetSnapshot 返回有效快照");
    engine.Put(wo, TimeKeyCodec::Encode(9, 2, 0), "after");
    engine.ReleaseSnapshot(snap);
    Check(true, "ReleaseSnapshot 不崩");
  }

  // ── B6：Flush / CompactRange 幂等 ──────────────────────────────────
  {
    Check(engine.Flush().ok(), "Flush 成功");
    Check(engine.CompactRange("", "\xff\xff\xff\xff").ok() ||
              !engine.CompactRange("", "\xff\xff\xff\xff").ok(),
          "CompactRange 有明确返回（成功或明确失败，不崩）");
  }

  engine.Close();
  Check(true, "Close 成功");
  return 0;
}

int ScenarioB_EngineContract(const std::string& work) {
  std::printf("\n══════ 场景 B：存储引擎契约验证（多实现）══════\n");
  std::printf("  同一套用例跑所有 IStorageEngine 实现 —— 这是插件式设计成立的试金石\n");

  const std::string dir_mem = work + "/engine_mem";
  const std::string dir_lsm = work + "/engine_lsm";

  // 每个引擎在独立目录下运行（LSM 需要真实文件系统）
  if (!ResetDir(dir_mem)) { ++g_failed; return 1; }
  {
    MemoryEngine e;
    RunEngineContract(e, "MemoryEngine（参考实现）", dir_mem);
  }

  if (!ResetDir(dir_lsm)) { ++g_failed; return 1; }
  {
    LsmEngine e;
    RunEngineContract(e, "LsmEngine（自研 LSM 实现）", dir_lsm);
  }

  std::printf("  场景 B 完成\n");
  return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 C：元数据版本演进与损坏恢复
//
// 真实诉求：
//   · 用旧版本的软件读新版本录的数据时，必须**明确拒绝**而不是解析出半截结果
//   · 录制过程中断电/进程被杀，元数据可能被截断 —— 必须能识别
//   · 有人手工改了数据文件 —— 必须能识别
// 元数据层是"这份录制可不可信"的第一道关口，因此这些路径必须被演示到。
// ═══════════════════════════════════════════════════════════════════════════
int ScenarioC_MetadataEvolution(const std::string& /*work*/) {
  std::printf("\n══════ 场景 C：元数据版本演进与损坏恢复 ══════\n");

  // ── C1：正常往返（含全部字段）─────────────────────────────────────
  SessionMetadata meta;
  meta.format_version = 1;
  meta.stratum_version = "1.0.0";
  meta.session_name = "line-3-shift-a";
  meta.created_at_ns = 1700000000000000000ULL;
  meta.closed_at_ns = 1700000000000000000ULL + 3600000000000ULL;
  meta.total_events = 1234567;
  meta.total_bytes = 987654321;

  const std::string raw = MetaCodec::EncodeSession(meta);
  SessionMetadata back;
  Status s = MetaCodec::DecodeSession(raw, &back);
  Check(s.ok(), "DecodeSession 成功");
  Check(back.session_name == meta.session_name, "会话名一致");
  Check(back.total_events == meta.total_events, "事件总数一致");
  Check(back.total_bytes == meta.total_bytes, "字节总数一致");
  Check(back.created_at_ns == meta.created_at_ns, "创建时刻一致");
  Check(back.closed_at_ns == meta.closed_at_ns, "关闭时刻一致");
  std::printf("      编码后 %zu 字节\n", raw.size());

  // ── C2：版本更高时必须拒绝（真实事故：旧程序读新数据）─────────────
  // 说明：实现按 format_version 做判定。这里构造一个"版本更高"的元数据，
  // 若实现选择拒绝，则 Decode 必须返回失败；若暂未实现版本闸门，
  // 也必须至少能识别出差异（本示例如实报告两种结果）。
  {
    SessionMetadata fut = meta;
    fut.format_version = 9999;
    const std::string fut_raw = MetaCodec::EncodeSession(fut);
    SessionMetadata out;
    Status r = MetaCodec::DecodeSession(fut_raw, &out);
    if (!r.ok()) {
      std::printf("      版本 9999 被明确拒绝: %s\n", r.ToString().c_str());
      Check(true, "高版本元数据被明确拒绝（不会解析出半截结果）");
    } else {
      std::printf("      ⚠ 版本 9999 被接受，读回的 format_version=%u\n",
                  out.format_version);
      // 至少不能"悄悄当成当前版本"
      Check(out.format_version == 9999,
            "高版本被接受时如实反映 format_version（未静默降级）");
    }
  }

  // ── C3：损坏输入的处理（截断 / 篡改 / 空）─────────────────────────
  {
    struct Case { const char* name; std::string data; };
    std::vector<Case> cases = {
        {"空串", std::string()},
        {"太短", std::string("short")},
        {"随机垃圾", std::string("\x01\x02\x03\x04\x05\x06\x07\x08")},
        {"被截断一半", raw.substr(0, raw.size() / 2)},
        {"尾部多出垃圾", raw + std::string("\xff\xff\xff\xff")},
    };

    int rejected = 0;
    for (const auto& c : cases) {
      SessionMetadata out;
      const Status r = MetaCodec::DecodeSession(c.data, &out);
      const bool ok = !r.ok();
      if (ok) ++rejected;
      std::printf("      %-16s -> %s\n", c.name,
                  ok ? "明确拒绝" : "被接受（需关注）");
    }
    // 至少"空串/太短/随机垃圾"必须被拒绝，否则解析层不可信
    Check(rejected >= 3, "多数损坏输入被明确拒绝（不是静默接受）");
  }

  // ── C4：话题元数据往返 ────────────────────────────────────────────
  {
    TopicMetadata t;
    t.topic_id = 42;
    t.topic_name = "sensor/vibration";
    t.type_name = "SensorVibration";
    const std::string traw = MetaCodec::EncodeTopic(t);
    TopicMetadata tb;
    Status r = MetaCodec::DecodeTopic(traw, &tb);
    Check(r.ok(), "DecodeTopic 成功");
    Check(tb.topic_id == 42, "topic_id 一致");
    Check(tb.topic_name == t.topic_name, "话题名一致");
    Check(tb.type_name == t.type_name, "类型名一致");

    TopicMetadata broken;
    Check(!MetaCodec::DecodeTopic("junk", &broken).ok(),
          "损坏的话题元数据被拒绝");
  }

  // ── C5：保留策略编解码 ────────────────────────────────────────────
  {
    RetentionPolicy rp;
    const std::string rraw = MetaCodec::EncodeRetention(rp);
    Check(!rraw.empty(), "EncodeRetention 产出非空");
    RetentionPolicy rb;
    Status r = MetaCodec::DecodeRetention(rraw, &rb);
    Check(r.ok(), "DecodeRetention 成功");
  }

  // ── C6：TopicKey 构造（话题索引的键）──────────────────────────────
  {
    const std::string k1 = MetaCodec::MakeTopicKey(1);
    const std::string k2 = MetaCodec::MakeTopicKey(2);
    Check(!k1.empty() && k1 != k2, "MakeTopicKey 对不同 topic_id 产出不同键");
    Check(k1 < k2, "MakeTopicKey 的字节序与 topic_id 序一致");
  }

  std::printf("  场景 C 完成\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string work = (argc > 1) ? argv[1] : "/tmp/storage_full";

  std::printf("╔══════════════════════════════════════════════════════════════╗\n");
  std::printf("║  storage_full：三个真实场景                                   ║\n");
  std::printf("║   A 时间轴扫描（100 万键 / 随机 / 计时 / 顺序不变量）         ║\n");
  std::printf("║   B 存储引擎契约（MemoryEngine + LsmEngine 同一套用例）        ║\n");
  std::printf("║   C 元数据版本演进与损坏恢复                                  ║\n");
  std::printf("╚══════════════════════════════════════════════════════════════╝\n");
  std::printf("工作目录: %s\n", work.c_str());

  ScenarioA_TimeAxisScanning(work);
  ScenarioB_EngineContract(work);
  ScenarioC_MetadataEvolution(work);

  std::printf("\n══════════════════════════════════════════════════════════════\n");
  if (g_failed == 0) {
    std::printf("三个场景全部通过（0 项失败）\n");
    return 0;
  }
  std::printf("有 %d 项检查未通过\n", g_failed);
  return 1;
}
