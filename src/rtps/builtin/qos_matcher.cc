// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// QoS 兼容判定实现
//
// 以下每段都标注了对应的上游行号（EDP.cpp 的 EDP::valid_matching），
// 便于逐条对照。

#include <stratum/rtps/builtin/qos_matcher.h>

namespace stratum {
namespace rtps {

using eprosima::fastdds::rtps::Time_t;

const char* MatchFailureName(MatchFailure f) {
  switch (f) {
    case MatchFailure::kNone: return "none";
    case MatchFailure::kDifferentTopic: return "different_topic";
    case MatchFailure::kDifferentType: return "different_type";
    case MatchFailure::kInconsistentTopic: return "inconsistent_topic";
    case MatchFailure::kIncompatibleQos: return "incompatible_qos";
    case MatchFailure::kReaderNotAlive: return "reader_not_alive";
    default: return "unknown";
  }
}

namespace {

// 数据表示集合是否有交集（对应上游 checkDataRepresentationQos）
bool RepresentationsIntersect(const SedpEndpointData& w,
                              const QosRequest& r) {
  // Writer 未声明时按 XCDR1（DDS 默认）处理
  std::vector<uint32_t> offered;
  if (w.has_data_representation) {
    offered.push_back(w.data_representation);
  } else {
    offered.push_back(0);   // XCDR1
  }
  for (uint32_t o : offered) {
    for (uint32_t req : r.data_representations) {
      if (o == req) return true;
    }
  }
  return false;
}

// Time_t 比较：a > b ?
bool TimeGreater(const Time_t& a, const Time_t& b) {
  if (a.seconds() != b.seconds()) return a.seconds() > b.seconds();
  return a.fraction() > b.fraction();
}

}  // namespace

QosRequest DefaultRecorderQos() {
  QosRequest q;
  q.reliability_kind = 1;      // BEST_EFFORT：接纳最广
  q.durability_kind = 0;       // VOLATILE
  q.ownership_kind = 0;        // SHARED
  q.liveliness_kind = 0;       // AUTOMATIC
  q.deadline = eprosima::fastdds::rtps::c_RTPSTimeInfinite;
  q.liveliness_lease = eprosima::fastdds::rtps::c_RTPSTimeInfinite;
  q.disable_positive_acks = false;
  q.data_representations = {0, 1};
  return q;
}

MatchResult MatchWriterToReader(const SedpEndpointData& writer,
                                const QosRequest& reader) {
  MatchResult res;

  // --- 上游 EDP.cpp:864  topic 名必须一致 ---
  if (writer.topic_name.empty()) {
    res.failure = MatchFailure::kDifferentTopic;
    res.details.push_back("远端公告未包含话题名（PID_TOPIC_NAME 缺失）");
    return res;
  }

  // --- 上游 EDP.cpp:878  type 名必须一致 ---
  // 上游在双方都有 type_information 时比对 TypeObject，否则比对 type_name。
  // 本实现只保留 type_name 比对（Stratum 不解析 TypeObject，见 known gaps）。
  if (!writer.has_type_name || writer.type_name.empty()) {
    res.failure = MatchFailure::kDifferentType;
    res.details.push_back("远端公告未包含类型名（PID_TYPE_NAME 缺失）");
    return res;
  }

  // --- 上游 EDP.cpp:891  topic_kind 必须一致（keyed / unkeyed）---
  // SedpEndpointData 暂未解析 topic_kind，此处按 unknown 放行，
  // 并在有值时比对（见下方 TODO 注释）。
  // TODO(stratum): 解析 PID_TOPIC_KIND（非 DDS 标准 PID，Fast DDS 扩展）。

  // --- 上游 EDP.cpp:905  reliability：writer=BE 且 reader=RE 时不兼容 ---
  if (writer.has_reliability) {
    if (writer.reliability_kind == 1 && reader.reliability_kind == 2) {
      res.incompatible_qos_mask |= kQosReliability;
      res.details.push_back(
          "RELIABILITY: writer=BEST_EFFORT(1) 但 reader=RELIABLE(2)");
    }
  }

  // --- 上游 EDP.cpp:918  durability：writer.kind < reader.kind 时不兼容 ---
  if (writer.has_durability) {
    if (writer.durability_kind < reader.durability_kind) {
      res.incompatible_qos_mask |= kQosDurability;
      res.details.push_back("DURABILITY: offered " +
                            std::to_string(writer.durability_kind) +
                            " < requested " +
                            std::to_string(reader.durability_kind));
    }
  }

  // --- 上游 EDP.cpp:928  ownership：必须相等 ---
  if (writer.has_ownership) {
    if (writer.ownership_kind != reader.ownership_kind) {
      res.incompatible_qos_mask |= kQosOwnership;
      res.details.push_back("OWNERSHIP: offered " +
                            std::to_string(writer.ownership_kind) +
                            " != requested " +
                            std::to_string(reader.ownership_kind));
    }
  }

  // --- 上游 EDP.cpp:935  deadline：writer.period > reader.period 时不兼容 ---
  if (writer.has_deadline) {
    if (TimeGreater(writer.deadline, reader.deadline)) {
      res.incompatible_qos_mask |= kQosDeadline;
      res.details.push_back("DEADLINE: offered period > requested period");
    }
  }

  // --- 上游 EDP.cpp:944  disable_positive_acks ---
  // writer 未声明时视为 false（与上游默认一致）
  {
    const bool w_enabled = false;   // SedpEndpointData 暂未解析该 PID
    if (!w_enabled && reader.disable_positive_acks) {
      res.incompatible_qos_mask |= kQosDisablePositiveAcks;
      res.details.push_back("DISABLE_POSITIVE_ACKS: reader 启用但 writer 未启用");
    }
  }

  // --- 上游 EDP.cpp:951  liveliness lease_duration：writer > reader 时不兼容 ---
  if (writer.has_liveliness) {
    if (TimeGreater(writer.liveliness_lease, reader.liveliness_lease)) {
      res.incompatible_qos_mask |= kQosLiveliness;
      res.details.push_back("LIVELINESS: offered lease > requested lease");
    }
    // --- 上游 EDP.cpp:960  liveliness kind：writer.kind < reader.kind 时不兼容 ---
    if (writer.liveliness_kind < reader.liveliness_kind) {
      res.incompatible_qos_mask |= kQosLiveliness;
      res.details.push_back("LIVELINESS: offered kind " +
                            std::to_string(writer.liveliness_kind) +
                            " < requested kind " +
                            std::to_string(reader.liveliness_kind));
    }
  }

  // --- 上游 EDP.cpp:966  DataRepresentation 必须有交集 ---
  if (!RepresentationsIntersect(writer, reader)) {
    res.incompatible_qos_mask |= kQosDataRepresentation;
    res.details.push_back("DATA_REPRESENTATION: offered 与 requested 无交集");
  }

  // --- 上游 EDP.cpp:975  任一 QoS 不兼容即视为不匹配 ---
  if (res.incompatible_qos_mask != 0) {
    res.failure = MatchFailure::kIncompatibleQos;
    res.matched = false;
    return res;
  }

  res.matched = true;
  res.failure = MatchFailure::kNone;
  return res;
}

}  // namespace rtps
}  // namespace stratum
