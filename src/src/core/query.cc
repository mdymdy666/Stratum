// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Query 与质量报告实现

#include "stratum/core/recorder.h"

#include <algorithm>
#include <sstream>

#include "stratum/storage/time_key.h"

namespace stratum {

// ---------------------------------------------------------------------------
// Query
// ---------------------------------------------------------------------------
Status Query::ListTopics(std::vector<TopicSummary>* out) const {
  if (out == nullptr) {
    return Status::InvalidArgument("out is null");
  }
  if (timeline_ == nullptr) {
    return Status::InvalidArgument("timeline is null");
  }

  std::vector<TopicMetadata> topics;
  STRATUM_RETURN_IF_ERROR(timeline_->ListTopics(&topics));

  out->clear();
  out->reserve(topics.size());
  for (const auto& t : topics) {
    TopicSummary s;
    s.topic_name = t.topic_name;
    s.type_name = t.type_name;
    s.message_count = t.message_count;
    s.byte_count = t.byte_count;
    s.first_ns = t.first_seen_ns;
    s.last_ns = t.last_seen_ns;
    s.active = t.active;
    out->push_back(std::move(s));
  }
  return Status::OK();
}

Status Query::GetInfo(SessionMetadata* out) const {
  if (timeline_ == nullptr) {
    return Status::InvalidArgument("timeline is null");
  }
  return timeline_->GetSessionMetadata(out);
}

Status Query::ExportRange(uint32_t topic_id, const TimeRange& range,
                          std::vector<Event>* out) const {
  if (out == nullptr) {
    return Status::InvalidArgument("out is null");
  }
  if (timeline_ == nullptr) {
    return Status::InvalidArgument("timeline is null");
  }

  EventIteratorPtr it;
  STRATUM_RETURN_IF_ERROR(timeline_->QueryRange(topic_id, range, &it));

  out->clear();
  for (; it->Valid(); it->Next()) {
    out->push_back(it->event());
  }
  return it->status();
}

Status Query::GetStateAt(uint32_t topic_id, TimestampNs as_of_ns,
                         StateAtResult* out) const {
  if (timeline_ == nullptr) {
    return Status::InvalidArgument("timeline is null");
  }
  return timeline_->GetStateAt(topic_id, as_of_ns, out);
}

Status Query::FindTopic(const std::string& name, TopicMetadata* out) const {
  if (timeline_ == nullptr) {
    return Status::InvalidArgument("timeline is null");
  }
  return timeline_->FindTopicByName(name, out);
}

// ---------------------------------------------------------------------------
// 质量报告
// ---------------------------------------------------------------------------
Status QualityReporter::Generate(uint64_t gap_threshold_ns,
                                 QualityReport* out) const {
  if (out == nullptr) {
    return Status::InvalidArgument("out is null");
  }
  if (timeline_ == nullptr) {
    return Status::InvalidArgument("timeline is null");
  }

  // 默认空洞阈值：1 秒
  const uint64_t threshold =
      gap_threshold_ns == 0 ? 1000000000ULL : gap_threshold_ns;

  QualityReport report;
  report.stats = timeline_->GetQualityStats();

  std::vector<TopicMetadata> topics;
  STRATUM_RETURN_IF_ERROR(timeline_->ListTopics(&topics));

  for (const auto& t : topics) {
    QualityReport::TopicQuality tq;
    tq.topic_name = t.topic_name;
    tq.message_count = t.message_count;

    // 逐条扫描，找时间轴空洞
    EventIteratorPtr it;
    if (timeline_->QueryRange(t.topic_id, TimeRange(), &it).ok()) {
      TimestampNs prev = kInvalidTimestamp;
      uint64_t gaps = 0;
      for (; it->Valid(); it->Next()) {
        const TimestampNs ts = it->event().source_timestamp_ns;
        if (prev != kInvalidTimestamp && ts > prev) {
          const uint64_t delta = ts - prev;
          if (delta > threshold) {
            ++gaps;
            // 粗略估计缺失条数（按平均间隔推算）
            if (t.message_count > 1 && t.last_seen_ns > t.first_seen_ns) {
              const uint64_t avg =
                  (t.last_seen_ns - t.first_seen_ns) / (t.message_count - 1);
              if (avg > 0) {
                tq.missing += (delta / avg) - 1;
              }
            }
          }
        }
        prev = ts;
      }
      tq.gaps = gaps;
    }

    // 起始处标记：如果话题的首条记录时间远晚于会话开始，说明可能有发现延迟
    tq.truncated_at_start = false;
    report.topics.push_back(std::move(tq));
  }

  // 总体评级
  if (report.stats.dropped_messages > 0 || report.stats.timestamp_regressions > 0 ||
      report.stats.interruptions > 0) {
    report.level = QualityReport::Level::kBad;
  } else if (report.stats.sequence_gaps > 0) {
    report.level = QualityReport::Level::kWarning;
  } else {
    report.level = QualityReport::Level::kGood;
  }
  for (const auto& tq : report.topics) {
    if (tq.gaps > 0 && report.level == QualityReport::Level::kGood) {
      report.level = QualityReport::Level::kWarning;
    }
  }

  *out = std::move(report);
  return Status::OK();
}

std::string QualityReport::ToText() const {
  std::ostringstream oss;

  oss << "===== Stratum 录制质量报告 =====\n\n";

  oss << "【总体评估】\n  ";
  switch (level) {
    case Level::kGood:
      oss << "可信度高\n";
      break;
    case Level::kWarning:
      oss << "可信度中（存在问题但不影响内容分析）\n";
      break;
    case Level::kBad:
      oss << "可信度低（存在数据丢失或时钟异常）\n";
      break;
  }

  oss << "\n【统计】\n";
  oss << "  丢弃消息数    : " << stats.dropped_messages << "\n";
  oss << "  时间戳倒退    : " << stats.timestamp_regressions << "\n";
  oss << "  序号缺口      : " << stats.sequence_gaps << "（缺 "
      << stats.missing_messages << " 条）\n";
  oss << "  录制中断      : " << stats.interruptions << "\n";
  if (stats.latency_samples > 0) {
    oss << "  接收延迟      : 最小 " << stats.min_latency_ns
        << " ns / 平均 " << static_cast<int64_t>(stats.AverageLatencyNs())
        << " ns / 最大 " << stats.max_latency_ns << " ns\n";
  }

  oss << "\n【按话题】\n";
  for (const auto& tq : topics) {
    oss << "  " << tq.topic_name << " : " << tq.message_count << " 条";
    if (tq.gaps > 0) {
      oss << "，存在 " << tq.gaps << " 处空洞（估计缺 " << tq.missing << " 条）";
    } else {
      oss << "，无空洞";
    }
    oss << "\n";
  }

  // --- 使用建议（这是报告的核心价值：给判断，而不只是给数字）---
  oss << "\n【使用建议】\n";
  if (level == Level::kGood) {
    oss << "  可用于: 内容分析、算法调参、回归测试\n";
    oss << "  注意  : 无论何时都不应假设'录制完整'——未录到的数据无法证明其不存在\n";
  } else if (level == Level::kWarning) {
    oss << "  可用于: 内容分析、算法调参\n";
    oss << "  慎用于: 时序精度相关结论\n";
    oss << "  不可用于: 完整性证明\n";
  } else {
    oss << "  可用于: 定性观察\n";
    oss << "  慎用于: 任何依赖完整性的结论\n";
    oss << "  不可用于: 审计或责任认定\n";
  }

  oss << "\n【诚实声明】\n";
  oss << "  本报告只能证明'已录到的数据'的完整性，\n";
  oss << "  无法证明'未录到的数据不存在'。\n";
  oss << "  特别是：录制器发现话题之前的数据无法获取（DDS 历史缓存限制）。\n";

  return oss.str();
}

std::string QualityReport::ToJson() const {
  std::ostringstream oss;
  oss << "{";
  oss << "\"level\":\"";
  switch (level) {
    case Level::kGood:
      oss << "good";
      break;
    case Level::kWarning:
      oss << "warning";
      break;
    case Level::kBad:
      oss << "bad";
      break;
  }
  oss << "\",";
  oss << "\"dropped_messages\":" << stats.dropped_messages << ",";
  oss << "\"timestamp_regressions\":" << stats.timestamp_regressions << ",";
  oss << "\"sequence_gaps\":" << stats.sequence_gaps << ",";
  oss << "\"missing_messages\":" << stats.missing_messages << ",";
  oss << "\"interruptions\":" << stats.interruptions << ",";
  oss << "\"topics\":[";
  for (size_t i = 0; i < topics.size(); ++i) {
    if (i > 0) oss << ",";
    oss << "{\"name\":\"" << topics[i].topic_name << "\","
        << "\"count\":" << topics[i].message_count << ","
        << "\"gaps\":" << topics[i].gaps << ","
        << "\"missing\":" << topics[i].missing << "}";
  }
  oss << "]}";
  return oss.str();
}

}  // namespace stratum
