// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 示例 02：查询时间线（Query API 全覆盖）
//
// 展示 Stratum 的**只读查询能力** —— 这是它区别于普通录制工具的核心：
// 录下来的数据不只能整体回放，还能按时间范围、按话题、按时刻检索。
//
// 覆盖的 API：
//   Query::ListTopics    列出话题摘要（条数/字节数/时间跨度）
//   Query::GetInfo       会话总体信息（格式版本/创建时间/总量）
//   Query::FindTopic     按名字找话题
//   Query::ExportRange   区间导出（把某话题某时间段的事件取出来）
//   Query::GetStateAt    定点查询（"某时刻的状态是什么"）
//
// 编译：见 examples/cpp/CMakeLists.txt
// 运行：./02_query_timeline <时间线目录>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"

using namespace stratum;

namespace {

// 时间戳是人不可读的纳秒计数，展示时转成秒
double ToSec(TimestampNs ns) {
  return static_cast<double>(ns) / 1e9;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("用法: %s <时间线目录>\n", argv[0]);
    return 2;
  }
  const std::string dir = argv[1];

  TimelineImplV2 timeline;
  Status s = timeline.Open(dir);
  if (!s.ok()) {
    std::printf("FAILED: timeline.Open(%s): %s\n", dir.c_str(),
                s.ToString().c_str());
    return 1;
  }
  Query query(&timeline);

  // -------------------------------------------------------------------------
  // 1) 会话总体信息
  //
  // SessionMetadata 是"这份录制是什么"的自描述信息；
  // 设计上要求**脱离原程序也能读懂**（见 06-data-trust.md）。
  // -------------------------------------------------------------------------
  SessionMetadata meta;
  s = query.GetInfo(&meta);
  if (!s.ok()) {
    std::printf("FAILED: query.GetInfo: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("=== 会话信息 ===\n");
  std::printf("  格式版本      : %u\n", meta.format_version);
  std::printf("  Stratum 版本  : %s\n", meta.stratum_version.c_str());
  std::printf("  会话名        : %s\n", meta.session_name.c_str());
  std::printf("  创建时刻      : %.3f s（单调时钟）\n", ToSec(meta.created_at_ns));
  std::printf("  关闭时刻      : %.3f s\n", ToSec(meta.closed_at_ns));
  std::printf("  事件总数      : %llu\n",
              static_cast<unsigned long long>(meta.total_events));
  std::printf("  总字节数      : %llu\n",
              static_cast<unsigned long long>(meta.total_bytes));

  // -------------------------------------------------------------------------
  // 2) 话题摘要
  // -------------------------------------------------------------------------
  std::vector<TopicSummary> topics;
  s = query.ListTopics(&topics);
  if (!s.ok()) {
    std::printf("FAILED: query.ListTopics: %s\n", s.ToString().c_str());
    return 1;
  }
  std::printf("\n=== 话题（%zu 个）===\n", topics.size());
  for (const auto& t : topics) {
    std::printf("  [%s] %s\n", t.active ? "active" : "closed", t.topic_name.c_str());
    std::printf("      type   = %s\n", t.type_name.c_str());
    std::printf("      count  = %llu   bytes = %llu\n",
                static_cast<unsigned long long>(t.message_count),
                static_cast<unsigned long long>(t.byte_count));
    std::printf("      时间跨度 = %.3f s .. %.3f s\n", ToSec(t.first_ns),
                ToSec(t.last_ns));
  }
  if (topics.empty()) {
    std::printf("（无话题）\n");
    timeline.Close();
    return 0;
  }

  // -------------------------------------------------------------------------
  // 3) 按名字查找（录制目录里话题名是稳定的，但编号不是）
  // -------------------------------------------------------------------------
  const std::string want = topics.front().topic_name;
  TopicMetadata tmeta;
  s = query.FindTopic(want, &tmeta);
  std::printf("\n=== FindTopic(\"%s\") ===\n", want.c_str());
  if (s.ok()) {
    std::printf("  topic_id = %u  type_name = %s\n", tmeta.topic_id,
                tmeta.type_name.c_str());
  } else {
    std::printf("  未找到: %s\n", s.ToString().c_str());
  }

  // -------------------------------------------------------------------------
  // 4) 区间导出
  //
  // TimeRange 语义：begin_ns 起，end_ns 止（0 = 无上界）。
  // 这里示例取"第一个话题的前 1 秒"，实际用 [first_ns, last_ns] 即全量。
  // -------------------------------------------------------------------------
  const uint32_t topic_id = tmeta.topic_id;
  TimeRange range;
  range.begin_ns = topics.front().first_ns;
  range.end_ns = topics.front().first_ns + 1000000000ULL;   // +1 秒

  std::vector<Event> events;
  s = query.ExportRange(topic_id, range, &events);
  std::printf("\n=== ExportRange(topic_id=%u, [%.3f, %.3f] s) ===\n", topic_id,
              ToSec(range.begin_ns), ToSec(range.end_ns));
  if (s.ok()) {
    std::printf("  取到 %zu 条事件\n", events.size());
  } else {
    std::printf("  查询失败: %s\n", s.ToString().c_str());
  }

  // -------------------------------------------------------------------------
  // 5) 定点查询
  //
  // "某一时刻这个话题的状态是什么" —— DDS 本身没有这个语义，
  // 这正是 Stratum 补上的那一层。
  // -------------------------------------------------------------------------
  StateAtResult state;
  const TimestampNs as_of = (topics.front().first_ns + topics.front().last_ns) / 2;
  s = query.GetStateAt(topic_id, as_of, &state);
  std::printf("\n=== GetStateAt(topic_id=%u, t=%.3f s) ===\n", topic_id,
              ToSec(as_of));
  if (s.ok()) {
    std::printf("  查询成功\n");
  } else {
    std::printf("  查询失败: %s\n", s.ToString().c_str());
  }

  timeline.Close();
  std::printf("\n完成。\n");
  return 0;
}
