// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 元数据编解码
//
// ===========================================================================
// 为什么需要它
// ===========================================================================
//
// Session 元数据（话题列表、质量统计、时间范围）必须持久化，且必须"自描述"
// —— 脱离原程序也能读懂（FR-R-11）。这需要一个稳定的二进制格式。
//
// 设计取舍：
//   - 不使用 JSON/Protobuf：为避免外部依赖（项目原则：零依赖）
//   - 采用简单的 TLV（Tag-Length-Value）风格，但因为是固定 schema，
//     实际用"顺序写字段 + 版本号"即可，无需通用 TLV 解析
//   - 关键：**读取方遇到更高版本必须拒绝**（FR-S-2），而不是尝试解析
//
// 格式约定：
//   - 所有整数小端（元数据不参与范围扫描，无需保持字节序可比）
//   - 字符串：4 字节长度 + 内容
//   - 读时严格校验剩余长度，防止越界

#ifndef STRATUM_STORAGE_META_CODEC_H_
#define STRATUM_STORAGE_META_CODEC_H_

#include <string>

#include "stratum/core/status.h"
#include "stratum/core/types.h"

namespace stratum {
namespace storage {

// 元数据键命名空间
// 注意：事件键以 TimeKey magic(0x53545231, 首字节 'S') 开头，
// 因此元数据键用其他首字节即可避免冲突。
constexpr const char* kMetaSessionKey = "META/session";
constexpr const char* kMetaTopicPrefix = "META/topic/";
constexpr const char* kMetaRetentionKey = "META/retention";

class MetaCodec {
 public:
  // --- Session 元数据 ------------------------------------------------------

  static std::string EncodeSession(const SessionMetadata& meta);
  static Status DecodeSession(const std::string& raw, SessionMetadata* out);

  // --- 话题元数据 ----------------------------------------------------------

  static std::string EncodeTopic(const TopicMetadata& meta);
  static Status DecodeTopic(const std::string& raw, TopicMetadata* out);

  // --- 保留策略 ------------------------------------------------------------

  static std::string EncodeRetention(const RetentionPolicy& policy);
  static Status DecodeRetention(const std::string& raw, RetentionPolicy* out);

  // --- 工具 ----------------------------------------------------------------

  // 构造话题的元数据键
  static std::string MakeTopicKey(uint32_t topic_id);
};

}  // namespace storage
}  // namespace stratum

#endif  // STRATUM_STORAGE_META_CODEC_H_
