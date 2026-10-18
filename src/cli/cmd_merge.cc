// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/cli/cmd_merge.cc
//
// `stratum session merge <会话1> <会话2> [...] --to <目标>`
// —— 把多份会话放到**同一条时间轴**上，产出一个新会话 + 一份合并报告。
//
// 设计裁决见 docs/16-session-merge.md（先读那份，本文只写实现说明）。
//
// ---------------------------------------------------------------------------
// 三个裁决的落地
// ---------------------------------------------------------------------------
//
// ① 时间轴：**source_timestamp_ns + 显式 --offset，按绝对值合并**
//    source 是发布端时钟，同一条广播在 A、B 两份录制里是**同一个数值** ——
//    这是唯一能让"同一条消息落在同一时刻"的轴。
//    reception 是各录制器的本地钟（差一个网络延迟），只在 source 无效（==0）时兜底。
//    不做重定基，不自动校正时钟偏差（只观测，见 clock_skew）。
//
// ② topic：**同名合成一个，名字原样保留**
//    来源信息进报告，不进名字。加前缀会让 `_probe.` 探针分类、diff 的 topic 配对、
//    现有查询脚本同时失效（见 docs/16 §三）。
//
// ③ 去重：**(topic, 合并后 source_ts, payload_hash)，仅跨源**
//    seq 是录制器本地分配的，不能用于跨会话去重；只看 payload 哈希会误删
//    真实重复发布（心跳）。同源内的相同记录**不**去重。
//
// ---------------------------------------------------------------------------
// ★★ 核心约束：合并不判断数据对错
// ---------------------------------------------------------------------------
//
// 不修正时间戳、不补齐空洞、不删除"看起来不对"的记录、不判断哪个源可信。
// 冲突与时钟偏差**只报不改**。输出里的 note 字段是契约的一部分。
//
// 合并的错误模式不是报错，而是安静地给出一个自洽的假时间轴 —— 这条护栏就是防它。

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cli_common.h"
#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"

namespace fs = std::filesystem;

using namespace stratum;

namespace stratum {
namespace cli {

namespace {

// 时钟偏差告警阈值：同一份内容在两个源里相差超过 1ms，就值得人看一眼。
// 【为什么是 1ms】单机内 source 时间戳是发布端打的，同一条消息应当**逐位相同**；
// 任何非零差都可能来自时钟不同源。但 payload 相同也可能是真实的重复发布，
// 所以这只是"观测 + 提示"，不是判定（见 docs/16 §二）。
constexpr int64_t kSkewWarnNs = 1000000;

// 时钟偏差观测的样本上限（防止大会话把内存吃光；统计量对样本数不敏感）
constexpr size_t kMaxSkewSamples = 20000;

// 每批写入条数。太大则单次 WriteBatch 过重，太小则元数据提交次数多。
constexpr size_t kAppendChunk = 4096;

// payload 哈希：FNV-1a 64。
//
// 与 cmd_diff.cc 用同一算法（那边是为了"跨机器稳定"）。合并这里同理：
// 去重键要能被任何人重算出来，不能用 std::hash（标准未规定算法）。
uint64_t HashPayload(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ull;
  }
  return h;
}

// ---------------------------------------------------------------------------
// 参数与源描述
// ---------------------------------------------------------------------------

struct SourceSpec {
  std::string path;
  std::string label;
  int64_t user_offset_ns = 0;
};

struct SourceTopic {
  std::string name;
  std::string type;
  uint64_t events = 0;
  uint64_t bytes = 0;
  TimestampNs first_ns = kInvalidTimestamp;
  TimestampNs last_ns = kInvalidTimestamp;
};

struct SourceSummary {
  std::string path;
  std::string label;
  uint64_t events = 0;
  uint64_t bytes = 0;
  size_t topic_count = 0;
  // 源自己的起点（source 时间戳；没有有效时间戳时为 0）
  TimestampNs origin_ns = kInvalidTimestamp;
  TimestampNs last_ns = kInvalidTimestamp;
  int64_t user_offset_ns = 0;
  int64_t applied_offset_ns = 0;

  // 合并过程中的计数
  uint64_t contributed = 0;   // 实际写进目标的条数
  uint64_t duplicates = 0;    // 被判为跨源重复而丢弃的条数
  uint64_t conflicts = 0;     // 同名同刻不同内容（保留，只计数）
  uint64_t fallback = 0;      // source 无效 → 用 reception 的条数
  uint64_t clamped = 0;       // source + offset <= 0 被钳到 1ns 的条数

  QualityStats quality;
  std::vector<SourceTopic> topics;
};

// 一条待归并事件（从源读出来、还没写进目标）
struct PendingEvent {
  TimestampNs ts = kInvalidTimestamp;           // 合并后的时间戳
  TimestampNs reception_ts = kInvalidTimestamp; // 原样保留（不随 offset 平移）
  uint32_t src = 0;
  uint32_t seq = 0;
  uint64_t ord = 0;
  std::string payload;
  std::string instance_key;
};

struct TopicReport {
  std::string name;
  std::string type;
  size_t sources = 0;
  uint64_t events = 0;
  uint64_t duplicates = 0;
  uint64_t conflicts = 0;
  TimestampNs first_ns = kInvalidTimestamp;
  TimestampNs last_ns = kInvalidTimestamp;
};

struct MergeReport {
  std::string target;
  std::string dedupe_mode = "content";
  std::vector<SourceSummary> sources;
  std::vector<TopicReport> topics;

  uint64_t merged_events = 0;
  uint64_t merged_bytes = 0;
  TimestampNs merged_first_ns = kInvalidTimestamp;
  TimestampNs merged_last_ns = kInvalidTimestamp;

  uint64_t duplicates_removed = 0;
  uint64_t conflicts = 0;
  uint64_t fallback_to_reception = 0;
  uint64_t clamped_timestamps = 0;
  uint64_t type_conflicts = 0;

  // 时钟偏差观测（只报不改）
  std::vector<int64_t> skew_deltas;
  int64_t skew_min_ns = 0;
  int64_t skew_max_ns = 0;
  int64_t skew_median_ns = 0;
  bool skew_warning = false;

  std::vector<std::string> warnings;

  bool HasWarnings() const { return !warnings.empty(); }
};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::string BaseName(const std::string& path) {
  const std::string name = fs::path(path).filename().string();
  return name.empty() ? path : name;
}

// 目录是否"看起来像会话"（LSM 的 CURRENT / MANIFEST-*）。
//
// 【为什么必须先判】TimelineImplV2::Open() 对不存在的路径会**创建**会话
// （录制时正需要如此）。合并是写命令，但**源**必须是已有会话 ——
// 否则 `session merge /typo/path --to ...` 会先在一个拼错的目录里建出空会话，
// 然后"成功"合并出空结果。这与 diff 当初踩的坑是同一个（docs/15 §实现注记）。
bool LooksLikeSession(const fs::path& p) {
  std::error_code ec;
  if (!fs::is_directory(p, ec)) return false;
  if (fs::exists(p / "CURRENT", ec)) return true;
  fs::directory_iterator it(p, ec);
  if (ec) return false;
  for (const auto& e : it) {
    const std::string n = e.path().filename().string();
    if (n.rfind("MANIFEST-", 0) == 0) return true;
  }
  return false;
}

bool IsDirectoryNonEmpty(const std::string& path) {
  std::error_code ec;
  if (!fs::exists(path, ec)) return false;
  if (!fs::is_directory(path, ec)) return true;   // 是文件也算"已存在"
  return !fs::is_empty(path, ec);
}

// 解析带符号的时长："-100ms" / "+1.5s" / "0"
bool ParseSignedDuration(const std::string& s, int64_t* out) {
  if (s.empty() || out == nullptr) return false;
  bool negative = false;
  std::string v = s;
  if (v[0] == '+' || v[0] == '-') {
    negative = (v[0] == '-');
    v = v.substr(1);
  }
  if (v.empty()) return false;
  uint64_t magnitude = 0;
  if (!ParseDurationNs(v, &magnitude)) return false;
  // 防止 int64 溢出（离 292 年还有很远的量级）
  const uint64_t kMax = 9000000000000000000ull;
  if (magnitude > kMax) return false;
  *out = negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
  return true;
}

// 去重键：(topic_id, 合并后时间戳, payload 哈希)。
// 用二进制串而不是拼接字符串，避免 topic 名里的分隔符造成键碰撞。
std::string DedupKey(uint32_t topic_id, TimestampNs ts, uint64_t payload_hash) {
  std::string k(20, '\0');
  std::memcpy(&k[0], &topic_id, sizeof(topic_id));
  std::memcpy(&k[4], &ts, sizeof(ts));
  std::memcpy(&k[12], &payload_hash, sizeof(payload_hash));
  return k;
}

struct TsSrc {
  TimestampNs ts = 0;
  uint32_t src = 0;
};

void Appendf(std::string* out, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

void Appendf(std::string* out, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  if (static_cast<size_t>(n) < sizeof(buf)) {
    out->append(buf, static_cast<size_t>(n));
  } else {
    std::string big(static_cast<size_t>(n) + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(&big[0], big.size(), fmt, ap);
    va_end(ap);
    out->append(big.data(), static_cast<size_t>(n));
  }
}

// ---------------------------------------------------------------------------
// 第一遍：扫描源（只读）
//
// 只拿话题清单与统计，不读事件。目的是在写目标**之前**确认：
//   · 每个源都能打开（否则不应当先建出一个半成品目标）
//   · 所有源的话题名/类型已知（目标的话题注册顺序可确定）
// ---------------------------------------------------------------------------
bool ScanSource(const SourceSpec& spec, SourceSummary* out, std::string* err) {
  TimelineImplV2 timeline;
  Status s = timeline.Open(spec.path);
  if (!s.ok()) {
    if (err) {
      *err = "无法打开源会话 " + spec.path + "：" + s.ToString() +
             "（确认它是会话目录；用 `stratum session list <父目录>` 查看）";
    }
    return false;
  }

  out->path = spec.path;
  out->label = spec.label;
  out->user_offset_ns = spec.user_offset_ns;
  out->applied_offset_ns = spec.user_offset_ns;

  Query q(&timeline);
  std::vector<TopicSummary> sums;
  s = q.ListTopics(&sums);
  if (!s.ok()) {
    if (err) *err = "列出话题失败（" + spec.path + "）：" + s.ToString();
    timeline.Close();
    return false;
  }

  for (const auto& t : sums) {
    SourceTopic st;
    st.name = t.topic_name;
    st.type = t.type_name;
    st.events = t.message_count;
    st.bytes = t.byte_count;
    st.first_ns = t.first_ns;
    st.last_ns = t.last_ns;

    out->events += t.message_count;
    out->bytes += t.byte_count;
    if (t.first_ns != kInvalidTimestamp &&
        (out->origin_ns == kInvalidTimestamp || t.first_ns < out->origin_ns)) {
      out->origin_ns = t.first_ns;
    }
    if (t.last_ns > out->last_ns) out->last_ns = t.last_ns;
    out->topics.push_back(std::move(st));
  }
  out->topic_count = out->topics.size();
  std::sort(out->topics.begin(), out->topics.end(),
            [](const SourceTopic& a, const SourceTopic& b) { return a.name < b.name; });

  out->quality = timeline.GetQualityStats();
  timeline.Close();
  return true;
}

// ---------------------------------------------------------------------------
// 第二遍：读取源事件到内存（按话题缓冲）
//
// 【为什么必须缓冲】绝对时间轴要求同一话题的事件**按时间**写进目标。
// 若按"源 1 全部、源 2 全部"的顺序追加，第二条源的早期事件会落在第一条源的
// 晚期事件之后 → 合并会话的 timestamp_regressions 暴涨，verify 会报假故障。
// 代价是峰值内存 ≈ 所有源 payload 总量（已写进 docs/16 §七）。
// ---------------------------------------------------------------------------
bool ReadSourceEvents(const std::string& path, uint32_t src_index,
                      const SourceSummary& summary,
                      std::map<std::string, std::vector<PendingEvent>>* buffers,
                      uint64_t* ord_counter, SourceSummary* stats,
                      std::string* err) {
  TimelineImplV2 timeline;
  Status s = timeline.Open(path);
  if (!s.ok()) {
    if (err) *err = "重新打开源会话失败 " + path + "：" + s.ToString();
    return false;
  }

  Query q(&timeline);
  for (const auto& st : summary.topics) {
    if (st.events == 0) {
      // 零事件话题也要出现在合并产物里（否则 `diff 源 合并` 会报"消失 topic"）
      (*buffers)[st.name];
      continue;
    }

    TopicMetadata tm;
    if (!q.FindTopic(st.name, &tm).ok()) {
      if (err) *err = "源 " + path + " 的话题 " + st.name + " 元数据读取失败";
      timeline.Close();
      return false;
    }

    // ★ 时间范围必须用话题真实的 first/last。
    // kInvalidTimestamp == 0 是"无效"，不是"最早"：直接拿 0 当 begin 会
    // 把 begin_ns 传给范围查询，而 end_ns == 0 在 TimeRange 语义里表示"无上界"。
    // 这里 treat "first 有效" 才设置 begin，避免把无效值当成合法时间。
    TimeRange range;
    range.begin_ns = (st.first_ns == kInvalidTimestamp) ? 0 : st.first_ns;
    range.end_ns = (st.last_ns == kInvalidTimestamp) ? 0 : (st.last_ns + 1);

    EventIteratorPtr it;
    Status qs = timeline.QueryRange(tm.topic_id, range, &it);
    if (!qs.ok() || !it) {
      if (err) *err = "读取源 " + path + " 话题 " + st.name + " 失败：" + qs.ToString();
      timeline.Close();
      return false;
    }

    std::vector<PendingEvent>& out = (*buffers)[st.name];
    for (; it->Valid(); it->Next()) {
      const Event& e = it->event();
      PendingEvent p;
      p.reception_ts = e.reception_timestamp_ns;
      p.seq = e.seq;
      p.src = src_index;
      p.ord = ++(*ord_counter);
      p.payload = e.payload;
      p.instance_key = e.instance_key;

      TimestampNs base = e.source_timestamp_ns;
      if (base == kInvalidTimestamp) {
        // 源数据缺 source 时间戳：退化为 reception（并计数，报告里可见）
        base = e.reception_timestamp_ns;
        ++stats->fallback;
      }
      // 合并后时间戳 = 基准 + 该源偏移。<= 0 会被钳到 1ns（0 == 无效）。
      const int64_t shifted = static_cast<int64_t>(base) + summary.applied_offset_ns;
      if (shifted <= 0) {
        p.ts = 1;
        ++stats->clamped;
      } else {
        p.ts = static_cast<TimestampNs>(shifted);
      }
      out.push_back(std::move(p));
    }
    Status is = it->status();
    if (!is.ok()) {
      if (err) *err = "读取源 " + path + " 话题 " + st.name + " 不完整：" + is.ToString();
      timeline.Close();
      return false;
    }
  }

  timeline.Close();
  return true;
}

// ---------------------------------------------------------------------------
// 写目标：按话题注册 + 归并 + 去重 + 追加
// ---------------------------------------------------------------------------
bool WriteTarget(const std::string& target,
                 std::map<std::string, std::vector<PendingEvent>>* buffers,
                 const std::map<std::string, std::string>& topic_types,
                 const std::map<std::string, std::set<uint32_t>>& topic_sources,
                 MergeReport* report, std::string* err) {
  TimelineImplV2 timeline;
  Status s = timeline.Open(target);
  if (!s.ok()) {
    if (err) {
      *err = "无法创建目标会话 " + target + "：" + s.ToString() +
             "（确认父目录存在且可写）";
    }
    return false;
  }

  bool ok = true;
  for (auto& kv : *buffers) {
    const std::string& topic_name = kv.first;
    std::vector<PendingEvent>& vec = kv.second;

    TopicMetadata meta;
    meta.topic_name = topic_name;
    auto tit = topic_types.find(topic_name);
    if (tit != topic_types.end()) meta.type_name = tit->second;
    meta.active = true;

    uint32_t topic_id = 0;
    s = timeline.RegisterTopic(meta, &topic_id);
    if (!s.ok()) {
      if (err) *err = "注册目标话题 " + topic_name + " 失败：" + s.ToString();
      ok = false;
      break;
    }

    TopicReport tr;
    tr.name = topic_name;
    tr.type = meta.type_name;
    auto sit = topic_sources.find(topic_name);
    tr.sources = (sit == topic_sources.end()) ? 0 : sit->second.size();

    // 按 (时间, 源序号, seq, 读入序) 排序 —— 时间优先，保证追加不产生时间戳倒退；
    // 同刻的跨源记录顺序由源序号固定，使"保留哪一条"可复现。
    std::sort(vec.begin(), vec.end(),
              [](const PendingEvent& a, const PendingEvent& b) {
                if (a.ts != b.ts) return a.ts < b.ts;
                if (a.src != b.src) return a.src < b.src;
                if (a.seq != b.seq) return a.seq < b.seq;
                return a.ord < b.ord;
              });

    // --- 时钟偏差观测（在去重之前做：需要看到"同内容不同时刻"的全部记录）---
    // 同一话题、同一 payload 哈希、来自**不同**源，时间差就是观测到的偏差。
    {
      std::map<uint64_t, std::vector<TsSrc>> groups;
      for (const auto& e : vec) {
        groups[HashPayload(e.payload)].push_back(TsSrc{e.ts, e.src});
      }
      for (const auto& g : groups) {
        const std::vector<TsSrc>& v = g.second;
        for (size_t i = 1; i < v.size(); ++i) {
          if (v[i].src != v[i - 1].src) {
            report->skew_deltas.push_back(static_cast<int64_t>(v[i].ts) -
                                          static_cast<int64_t>(v[i - 1].ts));
          } else if (i >= 2 && v[i].src != v[i - 2].src) {
            report->skew_deltas.push_back(static_cast<int64_t>(v[i].ts) -
                                          static_cast<int64_t>(v[i - 2].ts));
          }
          if (report->skew_deltas.size() >= kMaxSkewSamples) break;
        }
        if (report->skew_deltas.size() >= kMaxSkewSamples) break;
      }
    }

    // --- 去重 + 冲突 + 组装 ---
    std::unordered_map<std::string, uint32_t> dedup;   // key → 首个写出它的源
    std::unordered_map<std::string, std::pair<uint32_t, uint64_t>> ts_first;
    std::vector<Event> out;
    out.reserve(vec.size());
    std::map<uint32_t, uint64_t> kept_by_src;   // 本话题各源"保留"的条数
    uint64_t topic_bytes = 0;
    uint32_t seq_out = 1;

    for (auto& e : vec) {
      const uint64_t ph = HashPayload(e.payload);

      if (report->dedupe_mode == "content") {
        const std::string key = DedupKey(topic_id, e.ts, ph);
        auto it = dedup.find(key);
        if (it != dedup.end() && it->second != e.src) {
          // 跨源完全相同：这条广播在物理上只有一条（docs/16 §四）
          ++tr.duplicates;
          ++report->sources[e.src].duplicates;
          continue;
        }
        if (it == dedup.end()) dedup.emplace(key, e.src);
      }

      // 冲突：同名、同刻、不同内容（跨源）。两条都保留，只计数。
      {
        std::string tk(8, '\0');
        std::memcpy(&tk[0], &e.ts, sizeof(e.ts));
        auto it = ts_first.find(tk);
        if (it == ts_first.end()) {
          ts_first.emplace(tk, std::make_pair(e.src, ph));
        } else if (it->second.first != e.src && it->second.second != ph) {
          ++tr.conflicts;
          ++report->sources[e.src].conflicts;
        }
      }

      Event ev;
      ev.topic_id = topic_id;
      ev.seq = seq_out++;
      // seq_in_ts 由存储层分配（StratumDB::AppendBatch 内部 NextSeqInTs）
      ev.seq_in_ts = 0;
      ev.source_timestamp_ns = e.ts;
      ev.reception_timestamp_ns = e.reception_ts;   // 不平移：它是录制器本地钟
      topic_bytes += e.payload.size();
      ++kept_by_src[e.src];
      ev.payload = std::move(e.payload);
      ev.instance_key = std::move(e.instance_key);
      out.push_back(std::move(ev));
    }

    if (!out.empty()) {
      tr.first_ns = out.front().source_timestamp_ns;
      tr.last_ns = out.back().source_timestamp_ns;
      for (size_t off = 0; off < out.size(); off += kAppendChunk) {
        const size_t n = std::min(kAppendChunk, out.size() - off);
        std::vector<Event> batch(out.begin() + static_cast<long>(off),
                                 out.begin() + static_cast<long>(off + n));
        s = timeline.AppendBatch(batch);
        if (!s.ok()) {
          if (err) {
            *err = "写入目标失败（话题 " + topic_name + "）：" + s.ToString() +
                   "；目标会话可能不完整，建议删除后重试";
          }
          ok = false;
          break;
        }
      }
    }
    if (!ok) break;

    // 只有整批写成功才计入"贡献"（写失败时宁可少报，不能虚报）
    for (const auto& kv : kept_by_src) {
      report->sources[kv.first].contributed += kv.second;
    }

    tr.events = out.size();
    report->merged_events += tr.events;
    report->merged_bytes += topic_bytes;
    if (tr.events > 0) {
      if (report->merged_first_ns == kInvalidTimestamp ||
          tr.first_ns < report->merged_first_ns) {
        report->merged_first_ns = tr.first_ns;
      }
      if (tr.last_ns > report->merged_last_ns) report->merged_last_ns = tr.last_ns;
    }
    report->topics.push_back(std::move(tr));
  }

  if (ok) {
    Status fs_ = timeline.Flush();
    if (!fs_.ok()) {
      if (err) *err = "刷新目标会话失败：" + fs_.ToString();
      ok = false;
    }
  }
  timeline.Close();
  return ok;
}

// ---------------------------------------------------------------------------
// 报告输出
// ---------------------------------------------------------------------------

void FinalizeSkew(MergeReport* r) {
  if (r->skew_deltas.empty()) {
    r->skew_min_ns = r->skew_max_ns = r->skew_median_ns = 0;
    r->skew_warning = false;
    return;
  }
  std::vector<int64_t> v = r->skew_deltas;
  std::sort(v.begin(), v.end());
  r->skew_min_ns = v.front();
  r->skew_max_ns = v.back();
  r->skew_median_ns = v[v.size() / 2];
  r->skew_warning = (std::llabs(r->skew_median_ns) > kSkewWarnNs) ||
                    ((r->skew_max_ns - r->skew_min_ns) > kSkewWarnNs);
}

std::string BuildJson(const MergeReport& r) {
  std::string o;
  Appendf(&o, "{\"schema_version\":1,");
  Appendf(&o, "\"target\":\"%s\",", JsonEscape(r.target).c_str());

  Appendf(&o, "\"merged\":{\"events\":%llu,\"topics\":%zu,\"bytes\":%llu,"
              "\"first_ns\":%llu,\"last_ns\":%llu,\"span_ns\":%llu},",
          static_cast<unsigned long long>(r.merged_events), r.topics.size(),
          static_cast<unsigned long long>(r.merged_bytes),
          static_cast<unsigned long long>(r.merged_first_ns),
          static_cast<unsigned long long>(r.merged_last_ns),
          static_cast<unsigned long long>(
              (r.merged_last_ns >= r.merged_first_ns && r.merged_first_ns != kInvalidTimestamp)
                  ? (r.merged_last_ns - r.merged_first_ns)
                  : 0));

  Appendf(&o, "\"alignment\":{\"mode\":\"absolute\",\"clock\":\"source_timestamp_ns\","
              "\"fallback_to_reception\":%llu,\"clamped_timestamps\":%llu,\"offsets\":[",
          static_cast<unsigned long long>(r.fallback_to_reception),
          static_cast<unsigned long long>(r.clamped_timestamps));
  for (size_t i = 0; i < r.sources.size(); ++i) {
    if (i) Appendf(&o, ",");
    Appendf(&o, "{\"session\":\"%s\",\"offset_ns\":%lld}",
            JsonEscape(r.sources[i].path).c_str(),
            static_cast<long long>(r.sources[i].applied_offset_ns));
  }
  Appendf(&o, "],\"skew\":{\"samples\":%zu,\"min_ns\":%lld,\"max_ns\":%lld,"
              "\"median_ns\":%lld,\"warning\":%s}},",
          r.skew_deltas.size(), static_cast<long long>(r.skew_min_ns),
          static_cast<long long>(r.skew_max_ns),
          static_cast<long long>(r.skew_median_ns),
          r.skew_warning ? "true" : "false");

  Appendf(&o, "\"sources\":[");
  for (size_t i = 0; i < r.sources.size(); ++i) {
    const SourceSummary& s = r.sources[i];
    if (i) Appendf(&o, ",");
    Appendf(&o, "{\"session\":\"%s\",\"label\":\"%s\",\"events\":%llu,"
                "\"topics\":%zu,\"bytes\":%llu,\"origin_ns\":%llu,"
                "\"last_ns\":%llu,\"offset_ns\":%lld,"
                "\"contributed\":%llu,\"duplicates\":%llu,\"conflicts\":%llu,"
                "\"fallback_to_reception\":%llu,\"clamped_timestamps\":%llu,"
                "\"quality\":{\"dropped\":%llu,\"sequence_gaps\":%llu,"
                "\"missing_messages\":%llu,\"timestamp_regressions\":%llu,"
                "\"interruptions\":%llu}}",
            JsonEscape(s.path).c_str(), JsonEscape(s.label).c_str(),
            static_cast<unsigned long long>(s.events), s.topic_count,
            static_cast<unsigned long long>(s.bytes),
            static_cast<unsigned long long>(s.origin_ns),
            static_cast<unsigned long long>(s.last_ns),
            static_cast<long long>(s.applied_offset_ns),
            static_cast<unsigned long long>(s.contributed),
            static_cast<unsigned long long>(s.duplicates),
            static_cast<unsigned long long>(s.conflicts),
            static_cast<unsigned long long>(s.fallback),
            static_cast<unsigned long long>(s.clamped),
            static_cast<unsigned long long>(s.quality.dropped_messages),
            static_cast<unsigned long long>(s.quality.sequence_gaps),
            static_cast<unsigned long long>(s.quality.missing_messages),
            static_cast<unsigned long long>(s.quality.timestamp_regressions),
            static_cast<unsigned long long>(s.quality.interruptions));
  }
  Appendf(&o, "],");

  Appendf(&o, "\"topics\":[");
  for (size_t i = 0; i < r.topics.size(); ++i) {
    const TopicReport& t = r.topics[i];
    if (i) Appendf(&o, ",");
    Appendf(&o, "{\"topic\":\"%s\",\"type\":\"%s\",\"sources\":%zu,"
                "\"events\":%llu,\"duplicates\":%llu,\"conflicts\":%llu,"
                "\"first_ns\":%llu,\"last_ns\":%llu}",
            JsonEscape(t.name).c_str(), JsonEscape(t.type).c_str(), t.sources,
            static_cast<unsigned long long>(t.events),
            static_cast<unsigned long long>(t.duplicates),
            static_cast<unsigned long long>(t.conflicts),
            static_cast<unsigned long long>(t.first_ns),
            static_cast<unsigned long long>(t.last_ns));
  }
  Appendf(&o, "],");

  Appendf(&o, "\"dedupe\":{\"mode\":\"%s\",\"scope\":\"cross_source\","
              "\"key\":\"(topic,source_timestamp,payload_hash)\",\"hash\":\"fnv1a64\","
              "\"duplicates_removed\":%llu,\"conflicts\":%llu},",
          r.dedupe_mode.c_str(),
          static_cast<unsigned long long>(r.duplicates_removed),
          static_cast<unsigned long long>(r.conflicts));

  Appendf(&o, "\"warnings\":[");
  for (size_t i = 0; i < r.warnings.size(); ++i) {
    if (i) Appendf(&o, ",");
    Appendf(&o, "\"%s\"", JsonEscape(r.warnings[i]).c_str());
  }
  Appendf(&o, "],");

  // ★ 这条是契约的一部分，不是客套（docs/16 §五）
  Appendf(&o, "\"note\":\"合并不判断数据对错，只把多份放在同一时间轴上 —— "
              "不修正时间戳、不补齐空洞、不删除源数据（仅跨源完全相同的重复记录"
              "会合并为一条）；冲突与时钟偏差只报不改，是否可信请结合上下文判断\"}\n");
  return o;
}

std::string HumanSpan(uint64_t ns) {
  char buf[48];
  if (ns >= 1000000000ull) {
    std::snprintf(buf, sizeof(buf), "%.2fs", static_cast<double>(ns) / 1e9);
  } else if (ns >= 1000000ull) {
    std::snprintf(buf, sizeof(buf), "%.2fms", static_cast<double>(ns) / 1e6);
  } else {
    std::snprintf(buf, sizeof(buf), "%lluns", static_cast<unsigned long long>(ns));
  }
  return buf;
}

std::string HumanOffset(int64_t ns) {
  char buf[48];
  const double ms = static_cast<double>(ns) / 1e6;
  std::snprintf(buf, sizeof(buf), "%+.2fms", ms);
  return buf;
}

void PrintTable(const MergeReport& r, const GlobalOptions& g) {
  (void)g;
  const uint64_t span =
      (r.merged_first_ns != kInvalidTimestamp && r.merged_last_ns >= r.merged_first_ns)
          ? (r.merged_last_ns - r.merged_first_ns)
          : 0;

  std::printf("会话合并\n");
  std::printf("  目标: %s\n", r.target.c_str());
  std::printf("        %llu 条 / %zu topic / 跨度 %s\n",
              static_cast<unsigned long long>(r.merged_events), r.topics.size(),
              HumanSpan(span).c_str());
  std::printf("  方式: 绝对时间轴（source_timestamp_ns）；跨源去重（%s）\n",
              r.dedupe_mode.c_str());

  std::printf("\n来源（%zu）:\n", r.sources.size());
  for (const auto& s : r.sources) {
    std::printf("  %s  %s\n", s.label.c_str(), s.path.c_str());
    std::printf("         %llu 条 / %zu topic   偏移 %s\n",
                static_cast<unsigned long long>(s.events), s.topic_count,
                HumanOffset(s.applied_offset_ns).c_str());
    if (s.events == 0) {
      std::printf("         （空会话：没有贡献任何事件）\n");
    } else {
      std::printf("         写入 %llu 条，去重 %llu 条",
                  static_cast<unsigned long long>(s.contributed),
                  static_cast<unsigned long long>(s.duplicates));
      if (s.conflicts > 0) {
        std::printf("，冲突 %llu 处", static_cast<unsigned long long>(s.conflicts));
      }
      if (s.fallback > 0) {
        std::printf("，无 source 时间戳 %llu 条（已用 reception）",
                    static_cast<unsigned long long>(s.fallback));
      }
      if (s.clamped > 0) {
        std::printf("，时间戳被钳制 %llu 条", static_cast<unsigned long long>(s.clamped));
      }
      std::printf("\n");
    }
    if (s.quality.HasIntegrityIssue()) {
      std::printf("         源自身质量问题: 丢弃 %llu / 缺口 %llu / 时间戳倒退 %llu / 中断 %llu\n",
                  static_cast<unsigned long long>(s.quality.dropped_messages),
                  static_cast<unsigned long long>(s.quality.sequence_gaps),
                  static_cast<unsigned long long>(s.quality.timestamp_regressions),
                  static_cast<unsigned long long>(s.quality.interruptions));
    }
  }

  if (!r.topics.empty()) {
    std::printf("\n话题（%zu）:\n", r.topics.size());
    for (const auto& t : r.topics) {
      std::printf("  %-32s %zu 个源   %llu 条", t.name.c_str(), t.sources,
                  static_cast<unsigned long long>(t.events));
      if (t.duplicates > 0) {
        std::printf("（去重 %llu）", static_cast<unsigned long long>(t.duplicates));
      }
      if (t.conflicts > 0) {
        std::printf("（冲突 %llu）", static_cast<unsigned long long>(t.conflicts));
      }
      if (t.events > 0) {
        std::printf("  %s ~ %s", FormatTimestampNs(t.first_ns).c_str(),
                    FormatTimestampNs(t.last_ns).c_str());
      }
      std::printf("\n");
    }
  }

  if (r.skew_deltas.empty()) {
    std::printf("\n时钟偏差观测: 没有「同内容跨源」样本（无法观测，不代表没有偏差）\n");
  } else {
    std::printf("\n时钟偏差观测: %zu 对同内容记录，中位偏差 %s（min %s / max %s）%s\n",
                r.skew_deltas.size(),
                HumanOffset(r.skew_median_ns).c_str(),
                HumanOffset(r.skew_min_ns).c_str(),
                HumanOffset(r.skew_max_ns).c_str(),
                r.skew_warning ? "  ** 可疑 **" : "");
  }
  std::printf("冲突: %llu 处（同名同刻不同内容，两条均保留）\n",
              static_cast<unsigned long long>(r.conflicts));
  if (r.type_conflicts > 0) {
    std::printf("同名不同类型: %llu 个话题（已取第一个非空类型，仅提示）\n",
                static_cast<unsigned long long>(r.type_conflicts));
  }

  std::printf("\n说明: 合并不判断数据对错，只把多份放在同一时间轴上。\n");
  std::printf("      不修正时间戳、不补齐空洞；冲突与时钟偏差只报不改，\n");
  std::printf("      是否可信请结合上下文判断。\n");
}

// ---------------------------------------------------------------------------
// --offset 参数：匹配源（先按路径精确匹配，再按标签）
// ---------------------------------------------------------------------------
bool ApplyOffset(const std::string& spec, std::vector<SourceSpec>* sources,
                 std::string* err) {
  const size_t eq = spec.rfind('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 >= spec.size()) {
    if (err) *err = "--offset 需要 <会话>=<±时长>，例如 --offset /data/nodeB=-100ms";
    return false;
  }
  const std::string who = spec.substr(0, eq);
  const std::string dur = spec.substr(eq + 1);
  int64_t offset = 0;
  if (!ParseSignedDuration(dur, &offset)) {
    if (err) *err = "--offset 的时长非法: " + dur + "（支持 ms/s/m/h/d/w，可带 +/-）";
    return false;
  }
  for (auto& s : *sources) {
    if (s.path == who) {
      s.user_offset_ns = offset;
      return true;
    }
  }
  for (auto& s : *sources) {
    if (s.label == who) {
      s.user_offset_ns = offset;
      return true;
    }
  }
  if (err) *err = "--offset 指名的会话不在源列表里: " + who +
                  "（可用完整路径或源标签）";
  return false;
}

std::string Canonical(const std::string& path) {
  std::error_code ec;
  const fs::path p = fs::weakly_canonical(fs::path(path), ec);
  return ec ? fs::path(path).lexically_normal().string() : p.string();
}

// 本命令选项多（目标策略/去重/偏移/报告），给一个**子命令级** help。
// 其他子命令没有 `--help`（选项少，未知选项的报错里已给出可用列表）；
// 合并的选项组合直接影响结论，值得让人先读一遍再跑。
void PrintMergeHelp() {
  std::printf(
      "用法: stratum [--format table|json] session merge <会话1> [<会话2> ...] --to <目标>\n"
      "\n"
      "把多份会话放到同一条时间轴上。设计裁决见 docs/16-session-merge.md\n"
      "\n"
      "选项:\n"
      "  --to <路径>              目标会话（必填）。不存在则新建；已存在且非空 -> 3 EXISTS\n"
      "  --overwrite              目标非空时先清空（危险操作，必须显式）\n"
      "  --dedupe content|none    跨源去重策略（默认 content）；none = 全部保留\n"
      "  --offset <会话>=<+-时长>  给某个源加固定偏移，用于时钟偏差校正（可重复）\n"
      "                           <会话> 可用完整路径或源标签（目录名）；最小 1ms\n"
      "  --report <文件>          把合并报告（JSON）另存到文件\n"
      "  --help, -h               本说明\n"
      "\n"
      "退出码: 0 成功 / 5 合并完成但有冲突或可疑时钟偏差 / 8 源不存在 /\n"
      "        3 目标已存在 / 2 用法错误 / 6 环境不可写 / 1 写入失败\n"
      "\n"
      "说明: 合并不判断数据对错，只把多份放在同一时间轴上；\n"
      "      不修正时间戳、不补齐空洞，冲突与时钟偏差只报不改。\n");
}

}  // namespace

// ---------------------------------------------------------------------------
// 命令入口
// ---------------------------------------------------------------------------
int CmdMerge(const std::vector<std::string>& args, GlobalOptions g) {
  std::vector<std::string> positional;
  std::vector<std::string> offset_specs;
  std::string to;
  std::string report_path;
  std::string dedupe_mode = "content";
  bool overwrite = false;

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--help" || a == "-h") {
      PrintMergeHelp();
      return kOk;
    }
    auto NextValue = [&](const char* opt) -> const std::string* {
      if (i + 1 >= args.size()) {
        Fail(kUsage, std::string(opt) + " 缺少参数值", "", "");
        return nullptr;
      }
      return &args[++i];
    };

    if (a == "--to") {
      const std::string* v = NextValue("--to");
      if (v == nullptr) return kUsage;
      to = *v;
      continue;
    }
    if (a == "--report") {
      const std::string* v = NextValue("--report");
      if (v == nullptr) return kUsage;
      report_path = *v;
      continue;
    }
    if (a == "--dedupe") {
      const std::string* v = NextValue("--dedupe");
      if (v == nullptr) return kUsage;
      dedupe_mode = *v;
      if (dedupe_mode != "content" && dedupe_mode != "none") {
        return Fail(kUsage, "--dedupe 取值非法: " + dedupe_mode,
                    "只支持 content / none",
                    "content = 跨源完全相同只留一条（默认）；none = 全部保留");
      }
      continue;
    }
    if (a == "--offset") {
      const std::string* v = NextValue("--offset");
      if (v == nullptr) return kUsage;
      offset_specs.push_back(*v);
      continue;
    }
    if (a == "--overwrite") { overwrite = true; continue; }
    if (!a.empty() && a[0] == '-') {
      return Fail(kUsage, "session merge 不支持该选项: " + a,
                  "支持 --to / --dedupe / --offset / --overwrite / --report",
                  "完整说明见 docs/16-session-merge.md");
    }
    positional.push_back(a);
  }

  if (positional.empty()) {
    return Fail(kUsage, "session merge 需要至少一个源会话",
                "没有给出任何源会话路径",
                "例如: stratum session merge /data/a /data/b --to /data/merged");
  }
  if (to.empty()) {
    return Fail(kUsage, "session merge 需要 --to <目标会话>",
                "缺少目标会话路径",
                "例如: stratum session merge /data/a --to /data/merged");
  }

  // --- 源的预检（查询类副作用纪律的延伸：不存在就不要动目标）---
  for (const auto& p : positional) {
    // 必须是**已有会话**：普通目录不算（否则 Open 会在里面建空会话）
    if (!SessionExists(p) || !LooksLikeSession(fs::path(p))) {
      return Fail(kNotFound, "源会话不存在: " + p,
                  "该路径不是一个会话目录（merge 不会创建它）",
                  "用 `stratum session list <父目录>` 查看可用会话");
    }
  }

  const std::string target_canon = Canonical(to);
  for (const auto& p : positional) {
    if (Canonical(p) == target_canon) {
      return Fail(kUsage, "目标会话不能是源之一: " + p,
                  "合并会写目标；写进源会破坏源数据",
                  "换一个 --to 路径");
    }
  }
  // 目标若是某个源的祖先目录，--overwrite 会连源一起删掉 —— 必须拦下。
  for (const auto& p : positional) {
    const std::string src_canon = Canonical(p);
    if (src_canon.size() > target_canon.size() &&
        src_canon.compare(0, target_canon.size(), target_canon) == 0 &&
        src_canon[target_canon.size()] == '/') {
      return Fail(kUsage, "目标会话不能是源会话的父目录: " + to,
                  "源 " + p + " 位于目标目录内，--overwrite 会连同源一起删除",
                  "换一个 --to 路径（不要指向源所在目录）");
    }
  }

  // --- 目标已存在？---
  if (IsDirectoryNonEmpty(to)) {
    if (!overwrite) {
      return Fail(kExists, "目标已存在且非空，拒绝写入: " + to,
                  "目标里已有数据，合并会往里追加一个来源不明的时间线",
                  "换一个 --to；确实要重建时显式加 --overwrite（会先清空目标）");
    }
    std::error_code ec;
    fs::remove_all(to, ec);
    if (ec) {
      return Fail(kEnv, "清空目标失败: " + to, ec.message(), "");
    }
  }

  // --- 源描述与标签（同名目录用 #2 区分，保证报告与 --offset 无歧义）---
  std::vector<SourceSpec> specs;
  std::map<std::string, int> label_used;
  for (const auto& p : positional) {
    SourceSpec s;
    s.path = p;
    std::string base = BaseName(p);
    const int n = ++label_used[base];
    if (n > 1) base += "#" + std::to_string(n);
    s.label = base;
    specs.push_back(std::move(s));
  }
  for (const auto& spec : offset_specs) {
    std::string err;
    if (!ApplyOffset(spec, &specs, &err)) {
      return Fail(kUsage, "无法解析 --offset", err,
                  "例如: --offset /data/nodeB=-100ms（也可用源标签 nodeB）");
    }
  }

  MergeReport report;
  report.target = to;
  report.dedupe_mode = dedupe_mode;

  // --- 第一遍：扫描所有源 ---
  for (const auto& spec : specs) {
    SourceSummary s;
    std::string err;
    if (!ScanSource(spec, &s, &err)) {
      return Fail(kEnv, "源会话无法读取: " + spec.path, err,
                  "先确认该会话能打开（`stratum session info <会话>`）");
    }
    s.applied_offset_ns = spec.user_offset_ns;
    report.sources.push_back(std::move(s));
  }

  // --- 第二遍：读事件、写目标 ---
  std::map<std::string, std::vector<PendingEvent>> buffers;
  std::map<std::string, std::string> topic_types;
  std::map<std::string, std::set<uint32_t>> topic_sources;
  uint64_t ord = 0;

  for (size_t i = 0; i < specs.size(); ++i) {
    std::string err;
    if (!ReadSourceEvents(specs[i].path, static_cast<uint32_t>(i),
                          report.sources[i], &buffers, &ord,
                          &report.sources[i], &err)) {
      return Fail(kEnv, "读取源会话事件失败: " + specs[i].path, err,
                  "源会话可能在读取过程中被改动；确认没有进程正在写它");
    }
    for (const auto& t : report.sources[i].topics) {
      topic_sources[t.name].insert(static_cast<uint32_t>(i));
      auto it = topic_types.find(t.name);
      if (it == topic_types.end()) {
        if (!t.type.empty()) topic_types[t.name] = t.type;
      } else if (!t.type.empty() && it->second != t.type) {
        // 同名不同类型：取第一个非空，只报不改（docs/16 §三）
        ++report.type_conflicts;
      }
    }
  }

  std::string err;
  if (!WriteTarget(to, &buffers, topic_types, topic_sources, &report, &err)) {
    return Fail(kError, "合并写入失败", err,
                "目标会话可能不完整；删除目标后重试");
  }

  // --- 汇总 ---
  for (const auto& s : report.sources) {
    report.duplicates_removed += s.duplicates;
    report.conflicts += s.conflicts;
    report.fallback_to_reception += s.fallback;
    report.clamped_timestamps += s.clamped;
  }
  FinalizeSkew(&report);

  if (report.conflicts > 0) {
    report.warnings.push_back(
        "conflicts: 存在同名同刻不同内容的跨源记录（两条均保留），"
        "这通常意味着两个节点上的同名 topic 是不同数据流");
  }
  if (report.skew_warning) {
    report.warnings.push_back(
        "clock_skew_suspected: 同内容记录在不同源里相差超过 1ms，"
        "可能是时钟不同源；确认后可用 --offset 校正（工具不会自动校正）");
  }
  if (report.fallback_to_reception > 0) {
    report.warnings.push_back(
        "fallback_to_reception: 有记录缺少 source 时间戳，已用 reception 兜底，"
        "这些记录的对齐精度取决于本机时钟");
  }
  if (report.clamped_timestamps > 0) {
    report.warnings.push_back(
        "clamped_timestamps: --offset 过大导致时间戳 <= 0，已被钳到 1ns");
  }
  if (report.type_conflicts > 0) {
    report.warnings.push_back(
        "type_conflicts: 同名 topic 在不同源里类型不同，已取第一个非空类型");
  }

  if (report.skew_warning && !g.quiet) {
    std::fprintf(stderr,
                 "警告: 同内容记录跨源时间差中位数 %.2fms（超过 1ms），"
                 "可能是时钟不同源；合并结果的时间关系需谨慎解读\n",
                 static_cast<double>(report.skew_median_ns) / 1e6);
  }

  const std::string json = BuildJson(report);
  if (g.format == Format::kJson) {
    std::fputs(json.c_str(), stdout);
  } else {
    PrintTable(report, g);
  }

  if (!report_path.empty()) {
    std::FILE* f = std::fopen(report_path.c_str(), "wb");
    if (f == nullptr) {
      return Fail(kEnv, "无法写合并报告: " + report_path,
                  std::strerror(errno), "换一个可写路径，或去掉 --report");
    }
    const size_t n = std::fwrite(json.data(), 1, json.size(), f);
    const int closed = std::fclose(f);
    if (n != json.size() || closed != 0) {
      return Fail(kEnv, "写合并报告不完整: " + report_path, "", "");
    }
  }

  // 合并完成但有需要人看的东西 → PARTIAL(5)，与 diff 的"有差异"同约定。
  return report.HasWarnings() ? kPartial : kOk;
}

}  // namespace cli
}  // namespace stratum
