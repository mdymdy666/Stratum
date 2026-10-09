// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// QoS 兼容判定（DDS Requested/Offered 模型）
//
// ===========================================================================
// 移植说明
// ===========================================================================
//
// 判定规则**逐条移植**自 Fast DDS：
//   src/cpp/rtps/builtin/discovery/endpoint/EDP.cpp:856 `EDP::valid_matching`
//
// 该函数本体依赖 WriterProxyData / ReaderProxyData 两个各约 2000 行的类，
// 整体移植会再次触发依赖链发散（attributes → transport → flowcontrol，
// 实测补齐 19 个文件仍不收敛）。故按项目既有做法**提取判定规则本身**，
// 输入改为 Stratum 的轻量结构（SedpEndpointData + QosRequest）。
//
// 规则顺序与上游一致，便于逐条对照复核。
//
// ===========================================================================
// DDS 的 Requested/Offered 语义
// ===========================================================================
//
// Stratum 作为录制器，扮演 **Reader（requested）**；
// 远端发布者扮演 **Writer（offered）**。
// 通用原则：**offered 必须 >= requested**（提供方不能比请求方更弱）。
// 注意 OWNERSHIP 是**必须相等**的例外。

#ifndef STRATUM_RTPS_BUILTIN_QOS_MATCHER_H_
#define STRATUM_RTPS_BUILTIN_QOS_MATCHER_H_

#include <cstdint>
#include <string>
#include <vector>

#include <stratum/rtps/builtin/sedp_parser.h>
#include <stratum/rtps/common/Time_t.hpp>

namespace stratum {
namespace rtps {

// 不匹配的原因（对应上游 MatchingFailureMask）
enum class MatchFailure : uint8_t {
  kNone = 0,
  kDifferentTopic,       // 话题名不同
  kDifferentType,        // 类型名不同
  kInconsistentTopic,    // topic_kind 不同（keyed/unkeyed）
  kIncompatibleQos,      // QoS 不兼容
  kReaderNotAlive,       // 对端 Reader 不活跃（本实现中恒为活跃）
};

const char* MatchFailureName(MatchFailure f);

// DDS 标准 QoS 策略 ID（对应上游 PolicyMask 的位）
enum QosPolicyBit : uint32_t {
  kQosReliability = 1u << 0,
  kQosDurability = 1u << 1,
  kQosOwnership = 1u << 2,
  kQosDeadline = 1u << 3,
  kQosLiveliness = 1u << 4,
  kQosDataRepresentation = 1u << 5,
  kQosDisablePositiveAcks = 1u << 6,
};

// Stratum 作为 Reader 请求的 QoS
struct QosRequest {
  // 默认取**最宽松**取值：录制器应尽可能接纳各种发布者。
  // 若要严格捕获可靠流，可将 reliability_kind 设为 RELIABLE(2)，
  // 但那样会拒绝 BEST_EFFORT 的发布者（见匹配规则第 4 条）。
  uint32_t reliability_kind = 1;      // 1=BEST_EFFORT, 2=RELIABLE
  uint32_t durability_kind = 0;       // 0=VOLATILE, 1=TRANSIENT_LOCAL, ...
  uint32_t ownership_kind = 0;        // 0=SHARED, 1=EXCLUSIVE（必须与 Writer 相等）
  uint32_t liveliness_kind = 0;       // 0=AUTOMATIC, 1=MANUAL_BY_PARTICIPANT, 2=MANUAL_BY_TOPIC

  // 无限 = 最宽松（不施加额外时序要求）
  eprosima::fastdds::rtps::Time_t deadline =
      eprosima::fastdds::rtps::c_RTPSTimeInfinite;
  eprosima::fastdds::rtps::Time_t liveliness_lease =
      eprosima::fastdds::rtps::c_RTPSTimeInfinite;

  bool disable_positive_acks = false;

  // 请求的数据表示（XCDR1=0 / XCDR2=1）；与 Writer 的集合须有交集
  std::vector<uint32_t> data_representations{0, 1};
};

// 匹配结果
struct MatchResult {
  bool matched = false;
  MatchFailure failure = MatchFailure::kNone;
  uint32_t incompatible_qos_mask = 0;   // QosPolicyBit 的按位或
  std::vector<std::string> details;     // 人类可读的不兼容说明

  bool has_qos(uint32_t bit) const {
    return (incompatible_qos_mask & bit) != 0;
  }
};

// 判定：远端 Writer（offered）能否与 Stratum Reader（requested）匹配
//
// writer —— 由 SedpParser 从 SEDP Publication 公告解析出的端点信息
// reader —— Stratum 请求的 QoS
MatchResult MatchWriterToReader(const SedpEndpointData& writer,
                                const QosRequest& reader);

// 便捷：Stratum 录制器的默认（最宽松）请求
QosRequest DefaultRecorderQos();

}  // namespace rtps
}  // namespace stratum

#endif  // STRATUM_RTPS_BUILTIN_QOS_MATCHER_H_
