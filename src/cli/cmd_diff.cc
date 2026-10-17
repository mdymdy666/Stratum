// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_diff.cc
//
// `stratum diff <会话A> <会话B>` —— 两份会话的**统计对比**。
//
// 设计裁决见 docs/15-session-diff.md（先读那份，本文只写实现说明）。
//
// ---------------------------------------------------------------------------
// 三个裁决的落地
// ---------------------------------------------------------------------------
//
// ① 时间轴对齐：**相对时间**
//    两次录制的绝对时刻必然不同。以各自「最早事件时间」为 origin，
//    比较 event_ts - origin。
//    ★ 局限：不能断言"两次的第 N 秒是同一件事"；时长差一个数量级时
//      分桶结论会被拉伸影响失真 —— 所以输出 duration_warning。
//
// ② 粒度：**topic 级统计，不做事件级逐条**
//    时序数据逐条 diff 只产噪音。比条数/字节/首末偏移/**分桶**。
//
// ③ 语义：**默认不比 payload**
//    payload 是任意字节。--compare-payload {none|hash|sample}，
//    hash 只报变化条数，不报"哪里变了"。
//
// ---------------------------------------------------------------------------
// ★★ 核心约束：不判断「变好还是变坏」
// ---------------------------------------------------------------------------
//
// 条数 1000 → 950 可能是丢数据、业务量少了、采样率调了、业务提前结束。
// **diff 不知道业务预期，所以不能判断。**
//
// 若它说"可能丢数据"，那是猜；而猜错的代价比不猜更高 ——
// 人会去查一个并不存在的问题。（与 verify 不判断业务正确性同一原则。）
//
// 输出里必须有 note 字段写明这句。它不是客套，是契约。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cli_common.h"
#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"

using namespace stratum;
using namespace stratum::cli;

namespace stratum {
namespace cli {

namespace {

// ---------------------------------------------------------------------------
// 复用的小工具
// ---------------------------------------------------------------------------

// 判断是否探针 topic。
//
// 【与 cmd_verify.cc 里的 IsProbeTopic 等价】没有抽到公共头是为了不动机能
// 已经在工作的 verify（改它的风险高于收益）。将来若要统一，两处一起改。
bool IsProbeTopicName(const std::string& name, const std::string& prefix) {
  return !prefix.empty() && name.rfind(prefix, 0) == 0;
}

// payload 哈希：FNV-1a 64 位。
//
// 【为什么不用 std::hash】标准没规定算法，不同 libstdc++ 版本可能不同 ——
// 那会让"同一份数据在不同机器上算出不同哈希"，跨机器对比就失效了。
// FNV-1a 简单、稳定、够用（这里只需要判断"变了没变"）。
uint64_t HashPayload(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ull;
  }
  return h;
}

// ---------------------------------------------------------------------------
// 单个会话的摘要
// ---------------------------------------------------------------------------

struct TopicStat {
  std::string name;
  std::string type;
  uint64_t events = 0;
  uint64_t bytes = 0;
  TimestampNs first_ns = kInvalidTimestamp;
  TimestampNs last_ns = kInvalidTimestamp;
  bool is_probe = false;

  // 相对 origin 的偏移（origin 在算完之后才能填充）
  uint64_t first_offset_ns = 0;
  uint64_t last_offset_ns = 0;

  // 分桶（--bucket > 0 时填充）
  std::vector<uint64_t> buckets;

  // payload 比较（--compare-payload != none 时填充）
  uint64_t payload_scanned = 0;
  uint64_t payload_changed = 0;
  std::vector<uint64_t> payload_hashes;   // 用于与另一侧比
};

struct SessionSummary {
  std::string path;
  bool ok = false;
  std::string error;
  uint64_t events = 0;      // 所有 topic 的事件总数
  uint64_t bytes = 0;
  size_t topic_count = 0;
  size_t probe_topic_count = 0;
  uint64_t probe_events = 0;
  size_t dds_topic_count = 0;
  uint64_t dds_events = 0;
  // 对齐基准：所有 topic 里最早的 first_ns
  TimestampNs origin_ns = kInvalidTimestamp;
  TimestampNs last_ns = kInvalidTimestamp;
  uint64_t span_ns = 0;
  std::vector<TopicStat> topics;
};

// 打开会话并收集摘要。**只读，不创建任何东西**。
// ref_span_ns：分桶用的**统一参考跨度**（两侧 span 的较大者）。
// 【为什么需要它】若各用各自的 span，两个会话都会被归一化到 [0,1] ——
// "B 只在前一半时间有数据"这类差异会被完全抹掉（详见 docs/15 §三的修订）。
bool OpenSession(const std::string& path, const std::string& probe_prefix,
                 bool want_buckets, uint32_t buckets, bool want_payload,
                 size_t payload_sample, uint64_t ref_span_ns,
                 SessionSummary* out, std::string* err) {
  TimelineImplV2 timeline;
  Status s = timeline.Open(path);
  if (!s.ok()) {
    if (err) {
      *err = "无法打开会话 " + path + "：" + s.ToString() +
             "（确认它是会话目录；用 `stratum session list <父目录>` 查看）";
    }
    return false;
  }

  out->path = path;
  Query q(&timeline);
  std::vector<TopicSummary> sums;
  s = q.ListTopics(&sums);
  if (!s.ok()) {
    timeline.Close();
    if (err) *err = "列出话题失败：" + s.ToString();
    return false;
  }

  // 第一遍：填统计，算 origin
  for (const auto& t : sums) {
    TopicStat ts;
    ts.name = t.topic_name;
    ts.type = t.type_name;
    ts.events = t.message_count;
    ts.bytes = t.byte_count;
    ts.first_ns = t.first_ns;
    ts.last_ns = t.last_ns;
    ts.is_probe = IsProbeTopicName(t.topic_name, probe_prefix);

    out->events += t.message_count;
    out->bytes += t.byte_count;
    if (ts.is_probe) {
      ++out->probe_topic_count;
      out->probe_events += t.message_count;
    } else {
      ++out->dds_topic_count;
      out->dds_events += t.message_count;
    }
    if (t.first_ns != kInvalidTimestamp &&
        (out->origin_ns == kInvalidTimestamp || t.first_ns < out->origin_ns)) {
      out->origin_ns = t.first_ns;
    }
    if (t.last_ns > out->last_ns) out->last_ns = t.last_ns;
    out->topics.push_back(std::move(ts));
  }
  out->topic_count = out->topics.size();
  // 跨度：末条 - 最早起点
  if (out->origin_ns != kInvalidTimestamp && out->last_ns >= out->origin_ns) {
    out->span_ns = out->last_ns - out->origin_ns;
  }
  // 按名字排序，保证输出稳定（unordered 容器顺序会变）
  std::sort(out->topics.begin(), out->topics.end(),
            [](const TopicStat& a, const TopicStat& b) { return a.name < b.name; });

  // 第二遍：填相对偏移（需要 origin）
  for (auto& t : out->topics) {
    if (t.first_ns != kInvalidTimestamp && out->origin_ns != kInvalidTimestamp &&
        t.first_ns >= out->origin_ns) {
      t.first_offset_ns = t.first_ns - out->origin_ns;
    }
    if (t.last_ns != kInvalidTimestamp && out->origin_ns != kInvalidTimestamp &&
        t.last_ns >= out->origin_ns) {
      t.last_offset_ns = t.last_ns - out->origin_ns;
    }
  }

  // 第三遍：分桶 + payload（需要遍历事件，代价高，按需做）
  if (want_buckets || want_payload) {
    for (auto& t : out->topics) {
      if (t.events == 0) continue;
      if (t.first_ns == kInvalidTimestamp) continue;   // 无有效起点

      TopicMetadata tmc;
      if (!q.FindTopic(t.name, &tmc).ok()) continue;

      // ★ 时间范围必须用真实 first/last。
      // 我曾用 begin_ns = 0 以为"从头"，但 kInvalidTimestamp == 0 —— 那是"无效"，会崩。
      TimeRange range;
      range.begin_ns = t.first_ns;
      range.end_ns = (t.last_ns == kInvalidTimestamp) ? 0 : (t.last_ns + 1);

      EventIteratorPtr it;
      Status qs = timeline.QueryRange(tmc.topic_id, range, &it);
      if (!qs.ok() || !it) continue;

      if (want_buckets && buckets > 0) t.buckets.assign(buckets, 0);

      uint64_t seen = 0;
      for (; it->Valid(); it->Next()) {
        const Event& e = it->event();
        if (want_buckets && buckets > 0) {
          // 落到哪个桶：相对 origin 的位置按**统一参考跨度**等分。
          // 用 ref_span_ns 而不是 out->span_ns —— 否则两个会话各自归一化，
          // 时长差异会被抹掉。
          uint32_t b = 0;
          if (ref_span_ns > 0 && e.source_timestamp_ns >= out->origin_ns) {
            const uint64_t rel = e.source_timestamp_ns - out->origin_ns;
            // 用 128 位避免 rel * buckets 溢出
            const unsigned __int128 idx =
                (static_cast<unsigned __int128>(rel) * buckets) / ref_span_ns;
            b = (idx >= buckets) ? (buckets - 1) : static_cast<uint32_t>(idx);
          }
          ++t.buckets[b];
        }
        if (want_payload && (payload_sample == 0 || seen < payload_sample)) {
          t.payload_hashes.push_back(HashPayload(e.payload));
          ++t.payload_scanned;
        }
        ++seen;
      }
    }
  }

  timeline.Close();
  out->ok = true;
  return true;
}

// ---------------------------------------------------------------------------
// 对比结果
// ---------------------------------------------------------------------------

struct TopicDiff {
  std::string name;
  bool is_probe = false;
  uint64_t left_events = 0;
  uint64_t right_events = 0;
  int64_t delta = 0;           // right - left
  double delta_pct = 0.0;
  uint64_t left_first_offset_ns = 0;
  uint64_t right_first_offset_ns = 0;
  uint64_t left_last_offset_ns = 0;
  uint64_t right_last_offset_ns = 0;
  // 分桶
  uint32_t bucket_n = 0;
  std::vector<uint64_t> left_buckets;
  std::vector<uint64_t> right_buckets;
  double max_bucket_delta_pct = 0.0;   // 单桶相对差的最大值
  int32_t max_bucket_index = -1;
  // payload
  uint64_t payload_compared = 0;
  uint64_t payload_changed = 0;
};

struct DiffResult {
  SessionSummary left;
  SessionSummary right;
  std::vector<TopicDiff> added;      // 只在右
  std::vector<TopicDiff> removed;    // 只在左
  std::vector<TopicDiff> changed;
  std::vector<std::string> same;
  bool duration_warning = false;
  double duration_ratio = 1.0;
  bool any_difference = false;
};

// 相对差百分比（left 为 0 时特殊处理，避免除零）
double PctDelta(uint64_t left, uint64_t right) {
  if (left == 0) {
    return (right == 0) ? 0.0 : 100.0;   // 从无到有视为 +100%
  }
  return (static_cast<double>(right) - static_cast<double>(left)) * 100.0 /
         static_cast<double>(left);
}

DiffResult CompareSessions(SessionSummary left, SessionSummary right,
                           bool compare_payload) {
  DiffResult r;
  r.left = std::move(left);
  r.right = std::move(right);

  // 时长比：把"相对对齐的局限"变成一个可检查的信号
  if (r.left.span_ns > 0 && r.right.span_ns > 0) {
    const double a = static_cast<double>(r.left.span_ns);
    const double b = static_cast<double>(r.right.span_ns);
    r.duration_ratio = (a > b) ? (a / b) : (b / a);
    // 相差一个数量级就警告：此时分桶对比会被拉伸/压扁影响
    r.duration_warning = (r.duration_ratio >= 10.0);
  } else if (r.left.span_ns != r.right.span_ns) {
    // 一侧为 0：也算不可比
    r.duration_warning = true;
  }

  // 建索引
  std::map<std::string, const TopicStat*> lm, rm;
  for (const auto& t : r.left.topics) lm[t.name] = &t;
  for (const auto& t : r.right.topics) rm[t.name] = &t;

  auto FillCommon = [&](TopicDiff& d, const TopicStat& a, const TopicStat& b) {
    d.name = a.name;
    d.is_probe = a.is_probe;
    d.left_events = a.events;
    d.right_events = b.events;
    d.delta = static_cast<int64_t>(b.events) - static_cast<int64_t>(a.events);
    d.delta_pct = PctDelta(a.events, b.events);
    d.left_first_offset_ns = a.first_offset_ns;
    d.right_first_offset_ns = b.first_offset_ns;
    d.left_last_offset_ns = a.last_offset_ns;
    d.right_last_offset_ns = b.last_offset_ns;

    // 分桶：两侧桶数必须一致才比（bucket 参数相同，正常是一致的）
    if (!a.buckets.empty() && a.buckets.size() == b.buckets.size()) {
      d.bucket_n = static_cast<uint32_t>(a.buckets.size());
      d.left_buckets = a.buckets;
      d.right_buckets = b.buckets;
      // 找偏差最大的桶（这就是"给线索"而不是"报数字"）
      for (size_t i = 0; i < a.buckets.size(); ++i) {
        const double pct = std::fabs(PctDelta(a.buckets[i], b.buckets[i]));
        if (pct > d.max_bucket_delta_pct) {
          d.max_bucket_delta_pct = pct;
          d.max_bucket_index = static_cast<int32_t>(i);
        }
      }
    }

    // payload：只比条数与"变了没变"，不比内容
    if (compare_payload) {
      const size_t n = std::min(a.payload_hashes.size(), b.payload_hashes.size());
      d.payload_compared = n;
      for (size_t i = 0; i < n; ++i) {
        if (a.payload_hashes[i] != b.payload_hashes[i]) ++d.payload_changed;
      }
    }
  };

  // removed：只在左
  for (const auto& t : r.left.topics) {
    if (rm.find(t.name) == rm.end()) {
      TopicDiff d;
      d.name = t.name;
      d.is_probe = t.is_probe;
      d.left_events = t.events;
      d.right_events = 0;
      d.delta = -static_cast<int64_t>(t.events);
      d.delta_pct = -100.0;
      d.left_first_offset_ns = t.first_offset_ns;
      d.left_last_offset_ns = t.last_offset_ns;
      r.removed.push_back(d);
    }
  }
  // added：只在右
  for (const auto& t : r.right.topics) {
    if (lm.find(t.name) == lm.end()) {
      TopicDiff d;
      d.name = t.name;
      d.is_probe = t.is_probe;
      d.left_events = 0;
      d.right_events = t.events;
      d.delta = static_cast<int64_t>(t.events);
      d.delta_pct = 100.0;
      d.right_first_offset_ns = t.first_offset_ns;
      d.right_last_offset_ns = t.last_offset_ns;
      r.added.push_back(d);
    }
  }
  // common
  for (const auto& t : r.left.topics) {
    auto it = rm.find(t.name);
    if (it == rm.end()) continue;
    const TopicStat& b = *it->second;
    TopicDiff d;
    FillCommon(d, t, b);
    // "有变化"的判据：条数不同，或分桶偏差 > 0.5%，或 payload 有变化
    const bool bucket_changed = d.max_bucket_delta_pct > 0.5;
    const bool payload_changed = d.payload_changed > 0;
    if (d.delta != 0 || bucket_changed || payload_changed) {
      r.changed.push_back(d);
    } else {
      r.same.push_back(t.name);
    }
  }

  r.any_difference = !r.added.empty() || !r.removed.empty() || !r.changed.empty();
  return r;
}

// ---------------------------------------------------------------------------
// 输出
// ---------------------------------------------------------------------------

void PrintJson(const DiffResult& r, const std::string& payload_mode,
               uint32_t bucket_n, const std::string& probe_prefix) {
  std::printf("{\"schema_version\":1,");

  auto PrintSide = [](const char* key, const SessionSummary& s) {
    std::printf("\"%s\":{\"session\":\"%s\",\"events\":%llu,\"bytes\":%llu,"
                "\"topics\":%zu,\"probe_topics\":%zu,\"dds_topics\":%zu,"
                "\"span_ns\":%llu},",
                key, JsonEscape(s.path).c_str(),
                static_cast<unsigned long long>(s.events),
                static_cast<unsigned long long>(s.bytes), s.topic_count,
                s.probe_topic_count, s.dds_topic_count,
                static_cast<unsigned long long>(s.span_ns));
  };
  PrintSide("left", r.left);
  PrintSide("right", r.right);

  std::printf("\"alignment\":{\"mode\":\"relative\",\"left_origin_ns\":%llu,"
              "\"right_origin_ns\":%llu,\"duration_ratio\":%.3f,"
              "\"duration_warning\":%s},",
              static_cast<unsigned long long>(r.left.origin_ns),
              static_cast<unsigned long long>(r.right.origin_ns),
              r.duration_ratio, r.duration_warning ? "true" : "false");

  auto PrintList = [](const std::vector<TopicDiff>& v, bool with_buckets) {
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) std::printf(",");
      const auto& d = v[i];
      std::printf("{\"topic\":\"%s\",\"is_probe\":%s,\"left_events\":%llu,"
                  "\"right_events\":%llu,\"delta\":%lld,\"delta_pct\":%.2f",
                  JsonEscape(d.name).c_str(), d.is_probe ? "true" : "false",
                  static_cast<unsigned long long>(d.left_events),
                  static_cast<unsigned long long>(d.right_events),
                  static_cast<long long>(d.delta), d.delta_pct);
      if (with_buckets && d.bucket_n > 0) {
        std::printf(",\"buckets\":{\"n\":%u,\"max_delta_pct\":%.2f,"
                    "\"max_delta_index\":%d,\"left\":[",
                    d.bucket_n, d.max_bucket_delta_pct, d.max_bucket_index);
        for (size_t k = 0; k < d.left_buckets.size(); ++k) {
          if (k) std::printf(",");
          std::printf("%llu", static_cast<unsigned long long>(d.left_buckets[k]));
        }
        std::printf("],\"right\":[");
        for (size_t k = 0; k < d.right_buckets.size(); ++k) {
          if (k) std::printf(",");
          std::printf("%llu", static_cast<unsigned long long>(d.right_buckets[k]));
        }
        std::printf("]}");
      }
      if (d.payload_compared > 0) {
        std::printf(",\"payload\":{\"compared\":%llu,\"changed\":%llu}",
                    static_cast<unsigned long long>(d.payload_compared),
                    static_cast<unsigned long long>(d.payload_changed));
      }
      std::printf("}");
    }
  };

  std::printf("\"topics\":{\"added\":[");
  PrintList(r.added, false);
  std::printf("],\"removed\":[");
  PrintList(r.removed, false);
  std::printf("],\"changed\":[");
  PrintList(r.changed, true);
  std::printf("],\"same\":[");
  for (size_t i = 0; i < r.same.size(); ++i) {
    if (i) std::printf(",");
    std::printf("\"%s\"", JsonEscape(r.same[i]).c_str());
  }
  std::printf("]},");

  std::printf("\"payload_compare\":{\"mode\":\"%s\"},", payload_mode.c_str());
  std::printf("\"bucket\":%u,", bucket_n);
  std::printf("\"probe_prefix\":\"%s\",", JsonEscape(probe_prefix).c_str());
  std::printf("\"business_layer\":{\"left\":{\"dds_events\":%llu,\"dds_topics\":%zu,"
              "\"probe_events\":%llu,\"probe_topics\":%zu},"
              "\"right\":{\"dds_events\":%llu,\"dds_topics\":%zu,"
              "\"probe_events\":%llu,\"probe_topics\":%zu}},",
              static_cast<unsigned long long>(r.left.dds_events), r.left.dds_topic_count,
              static_cast<unsigned long long>(r.left.probe_events), r.left.probe_topic_count,
              static_cast<unsigned long long>(r.right.dds_events), r.right.dds_topic_count,
              static_cast<unsigned long long>(r.right.probe_events), r.right.probe_topic_count);

  // ★ 这条是契约的一部分，不是客套
  std::printf("\"note\":\"这是统计对比，不是逐条 diff；diff 不判断变好还是变坏 —— "
              "条数变化可能来自丢数据，也可能来自业务量变化，请结合上下文判断\"}\n");
}

void PrintText(const DiffResult& r, const std::string& payload_mode,
               uint32_t bucket_n) {
  auto SpanHuman = [](uint64_t ns) -> std::string {
    if (ns >= 1000000000ull) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.2fs",
                    static_cast<double>(ns) / 1e9);
      return buf;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2fms", static_cast<double>(ns) / 1e6);
    return buf;
  };

  std::printf("会话对比\n");
  std::printf("  左: %s\n", r.left.path.c_str());
  std::printf("      %llu 条 / %zu topic / 跨度 %s\n",
              static_cast<unsigned long long>(r.left.events), r.left.topic_count,
              SpanHuman(r.left.span_ns).c_str());
  std::printf("  右: %s\n", r.right.path.c_str());
  std::printf("      %llu 条 / %zu topic / 跨度 %s\n",
              static_cast<unsigned long long>(r.right.events), r.right.topic_count,
              SpanHuman(r.right.span_ns).c_str());
  std::printf("  对齐: 相对时间（两侧 origin 不同，按各自起点算偏移）\n");

  if (r.duration_warning) {
    std::printf("\n  ** 警告：两侧时长相差 %.1f 倍 **\n", r.duration_ratio);
    std::printf("     分桶对比的结论可能被拉伸/压扁影响，请谨慎解读。\n");
  }

  if (!r.added.empty()) {
    std::printf("\n新增 topic（%zu）:\n", r.added.size());
    for (const auto& d : r.added) {
      std::printf("  %-32s %llu 条\n", d.name.c_str(),
                  static_cast<unsigned long long>(d.right_events));
    }
  }
  if (!r.removed.empty()) {
    std::printf("\n消失 topic（%zu）:\n", r.removed.size());
    for (const auto& d : r.removed) {
      std::printf("  %-32s %llu 条（左侧有，右侧无）\n", d.name.c_str(),
                  static_cast<unsigned long long>(d.left_events));
    }
  }
  if (!r.changed.empty()) {
    std::printf("\n变化 topic（%zu）:\n", r.changed.size());
    for (const auto& d : r.changed) {
      std::printf("  %s%s\n", d.name.c_str(), d.is_probe ? "  [探针]" : "");
      std::printf("      %llu → %llu（%+.1f%%）\n",
                  static_cast<unsigned long long>(d.left_events),
                  static_cast<unsigned long long>(d.right_events), d.delta_pct);
      if (d.left_first_offset_ns != d.right_first_offset_ns) {
        const int64_t df = static_cast<int64_t>(d.right_first_offset_ns) -
                           static_cast<int64_t>(d.left_first_offset_ns);
        std::printf("      首条偏移差 %+.2fms\n", static_cast<double>(df) / 1e6);
      }
      if (d.bucket_n > 0 && d.max_bucket_index >= 0) {
        std::printf("      分桶最大偏差 %.1f%%（第 %d/%u 桶）\n",
                    d.max_bucket_delta_pct, d.max_bucket_index + 1, d.bucket_n);
      }
      if (d.payload_compared > 0) {
        std::printf("      负载：比了 %llu 条，其中 %llu 条不同\n",
                    static_cast<unsigned long long>(d.payload_compared),
                    static_cast<unsigned long long>(d.payload_changed));
      }
    }
  }
  if (!r.same.empty()) {
    std::printf("\n未变 topic: %zu 个\n", r.same.size());
  }
  if (r.added.empty() && r.removed.empty() && r.changed.empty()) {
    std::printf("\n  两份会话在对比范围内没有差异。\n");
  }

  std::printf("\n业务层:\n");
  std::printf("  左: 探针 %llu 条 / %zu topic；业务 %llu 条 / %zu topic\n",
              static_cast<unsigned long long>(r.left.probe_events),
              r.left.probe_topic_count,
              static_cast<unsigned long long>(r.left.dds_events),
              r.left.dds_topic_count);
  std::printf("  右: 探针 %llu 条 / %zu topic；业务 %llu 条 / %zu topic\n",
              static_cast<unsigned long long>(r.right.probe_events),
              r.right.probe_topic_count,
              static_cast<unsigned long long>(r.right.dds_events),
              r.right.dds_topic_count);

  std::printf("\n说明: 这是统计对比，不是逐条 diff；diff 不判断变好还是变坏。\n");
  std::printf("      条数变化可能来自丢数据，也可能来自业务量变化，请结合上下文判断。\n");
  std::printf("      参数: bucket=%u, payload=%s\n", bucket_n, payload_mode.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// 命令入口
// ---------------------------------------------------------------------------

int CmdDiff(const std::vector<std::string>& args, GlobalOptions g) {
  std::vector<std::string> positional;
  uint32_t bucket_n = 20;
  std::string payload_mode = "none";
  std::string probe_prefix = "_probe.";
  bool only_changed = false;
  size_t limit = 0;
  bool want_json = (g.format == Format::kJson);

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--bucket") {
      if (i + 1 >= args.size()) {
        return Fail(kUsage, "--bucket 缺少参数值", "", "0 = 跳过分桶（快速模式），默认 20");
      }
      bucket_n = static_cast<uint32_t>(std::strtoul(args[++i].c_str(), nullptr, 10));
      continue;
    }
    if (a == "--compare-payload") {
      if (i + 1 >= args.size()) {
        return Fail(kUsage, "--compare-payload 缺少参数值", "",
                    "none（默认）| hash | sample");
      }
      payload_mode = args[++i];
      if (payload_mode != "none" && payload_mode != "hash" && payload_mode != "sample") {
        return Fail(kUsage, "--compare-payload 取值非法: " + payload_mode,
                    "只支持 none / hash / sample",
                    "none = 不比；hash = 比哈希只报变化条数；sample = 只比前 100 条");
      }
      continue;
    }
    if (a == "--probe-prefix") {
      if (i + 1 >= args.size()) {
        return Fail(kUsage, "--probe-prefix 缺少参数值", "",
                    "默认 _probe.（与 stratum probe 一致）");
      }
      probe_prefix = args[++i];
      continue;
    }
    if (a == "--only-changed") { only_changed = true; continue; }
    if (a == "--limit") {
      if (i + 1 >= args.size()) {
        return Fail(kUsage, "--limit 缺少参数值", "", "整数，0 = 不限");
      }
      limit = static_cast<size_t>(std::strtoull(args[++i].c_str(), nullptr, 10));
      continue;
    }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "diff 不支持该选项: " + a,
                  "支持 --bucket / --compare-payload / --probe-prefix / "
                  "--only-changed / --limit",
                  "完整说明见 docs/15-session-diff.md");
    }
    positional.push_back(a);
  }

  if (positional.size() < 2) {
    return Fail(kUsage, "diff 需要两个会话路径",
                positional.empty() ? "两个位置参数都缺失"
                                   : "只给了 1 个（" + positional[0] + "）",
                "例如: stratum diff /data/s1 /data/s2");
  }
  if (positional.size() > 2) {
    return Fail(kUsage, "diff 只接受两个会话路径，多了: " + positional[2], "", "");
  }

  const bool want_payload = (payload_mode != "none");
  const size_t payload_sample = (payload_mode == "sample") ? 100 : 0;   // 0 = 不限

  // -------------------------------------------------------------------------
  // 【必须先预检】TimelineImplV2::Open 会**创建目录**。
  // diff 是纯查询命令，对一个拼错的路径静默创建一个空会话、
  // 然后报告"无差异" —— 这是最坏的结果：用户以为两份数据一样。
  // 项目纪律：查询类命令不得有创建副作用。
  // -------------------------------------------------------------------------
  for (const auto& p : positional) {
    if (!SessionExists(p)) {
      return Fail(kNotFound, "会话不存在: " + p,
                  "该路径不是一个会话目录（diff 不会创建它）",
                  "用 `stratum session list <父目录>` 查看可用会话");
    }
  }

  // 打开两侧。
  //
  // 两遍：第一遍只拿 span（不算桶），因为分桶需要**两侧共同的参考跨度**；
  // 算出 ref_span 后再打开一遍算桶。
  // 代价是多一次 ListTopics（很轻，不遍历事件），换来的是分桶可比。
  SessionSummary probe_l, probe_r;
  std::string err;
  if (!OpenSession(positional[0], probe_prefix, false, 0, false, 0, 0,
                   &probe_l, &err)) {
    return Fail(kEnv, "左侧会话无法对比", err, "先确认该会话能打开");
  }
  if (!OpenSession(positional[1], probe_prefix, false, 0, false, 0, 0,
                   &probe_r, &err)) {
    return Fail(kEnv, "右侧会话无法对比", err, "先确认该会话能打开");
  }
  const uint64_t ref_span =
      (probe_l.span_ns > probe_r.span_ns) ? probe_l.span_ns : probe_r.span_ns;

  SessionSummary left, right;
  if (!OpenSession(positional[0], probe_prefix, bucket_n > 0, bucket_n,
                   want_payload, payload_sample, ref_span, &left, &err)) {
    return Fail(kEnv, "左侧会话无法对比", err, "先确认该会话能打开");
  }
  if (!OpenSession(positional[1], probe_prefix, bucket_n > 0, bucket_n,
                   want_payload, payload_sample, ref_span, &right, &err)) {
    return Fail(kEnv, "右侧会话无法对比", err, "先确认该会话能打开");
  }

  DiffResult r = CompareSessions(std::move(left), std::move(right), want_payload);

  // --limit：只影响输出条数，不影响判据
  auto Truncate = [limit](std::vector<TopicDiff>& v) {
    if (limit > 0 && v.size() > limit) v.resize(limit);
  };
  Truncate(r.added);
  Truncate(r.removed);
  Truncate(r.changed);

  // 时长差异过大时给 stderr 警告（局限变成可检查信号）
  if (r.duration_warning && !g.quiet) {
    std::fprintf(stderr,
                 "警告: 两侧时长相差 %.1f 倍，分桶对比的结论可能被拉伸影响失真\n",
                 r.duration_ratio);
  }

  if (want_json) {
    PrintJson(r, payload_mode, bucket_n, probe_prefix);
  } else {
    PrintText(r, payload_mode, bucket_n);
  }

  // --only-changed 只影响显示，退出码仍按真实差异
  (void)only_changed;

  // 有差异 → PARTIAL(5)，不是错误。
  // 这样脚本能区分"完全一致"与"有变化"与"工具出错"。
  return r.any_difference ? kPartial : kOk;
}

}  // namespace cli
}  // namespace stratum
