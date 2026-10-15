// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/include/stratum/types/json_serializer.h
//
// 序列化器的具体实现：
//   · PassthroughSerializer —— 降级模式的"透传"实现
//   · JsonSerializer        —— 自描述（definition_format = "json"）的编解码
//   · JsonSerializerFactory —— 按 TypeDescriptor 选择实现
//
// 【为什么需要 Passthrough】iserializer.h 的接口契约写得很明确：
//
//   > descriptor 可能不含定义（降级模式）——此时应返回一个
//   > supports_deserialization() == false 的"透传"序列化器，**而不是失败**。
//
// 这条约束很重要：Stratum 的 v1 允许"无类型定义"地录制任意 DDS 数据。
// 若工厂在这种情况下直接失败，整个录制链路会在遇到未注册类型时中断 ——
// 而"能录下来"是第一位的，"能读懂"是第二位的。
//
// 【已知的有损之处】见 json_serializer.cc 顶部说明。

#ifndef STRATUM_TYPES_JSON_SERIALIZER_H_
#define STRATUM_TYPES_JSON_SERIALIZER_H_

#include <string>

#include "stratum/types/iserializer.h"

namespace stratum {
namespace types {

// ---------------------------------------------------------------------------
// 降级模式：透传序列化器
//
// 明确声明"不支持序列化与反序列化"，让调用方可以据此选择降级行为
// （例如只按字节回放），而不是拿到一个含糊的成功或失败。
// ---------------------------------------------------------------------------
class PassthroughSerializer final : public ISerializer {
 public:
  explicit PassthroughSerializer(std::string type_name)
      : type_name_(std::move(type_name)) {}

  bool supports_deserialization() const override { return false; }
  bool supports_serialization() const override { return false; }
  std::string Name() const override { return "passthrough"; }

  Status Serialize(const DynamicValue& value, std::string* out) override;
  Status Deserialize(const std::string& bytes, DynamicValue* out) override;

  // 无类型定义时无法估计尺寸。返回 0 而不是编一个数。
  size_t EstimateSize(const DynamicValue& value) const override;

  const std::string& type_name() const { return type_name_; }

 private:
  std::string type_name_;
};

// ---------------------------------------------------------------------------
// JSON 自描述序列化器
//
// 序列化：DynamicValue -> JSON 文本（复用 DynamicValue::ToString）
// 反序列化：JSON 文本 -> DynamicValue（内置递归下降解析器）
//
// 该实现**不需要类型定义**：JSON 是自描述的，结构信息在文本里。
// 因此 definition_format == "json" 时 supports_serialization/deserialization 均为 true。
// ---------------------------------------------------------------------------
class JsonSerializer final : public ISerializer {
 public:
  explicit JsonSerializer(std::string type_name = {})
      : type_name_(std::move(type_name)) {}

  bool supports_deserialization() const override { return true; }
  bool supports_serialization() const override { return true; }
  std::string Name() const override { return "json"; }

  Status Serialize(const DynamicValue& value, std::string* out) override;
  Status Deserialize(const std::string& bytes, DynamicValue* out) override;
  size_t EstimateSize(const DynamicValue& value) const override;

 private:
  std::string type_name_;
};

// ---------------------------------------------------------------------------
// 工厂：按 TypeDescriptor 选择实现
//
// 规则（与接口契约一致）：
//   definition_format == "json"  -> JsonSerializer
//   其它（含空定义）             -> PassthroughSerializer（**不失败**）
// ---------------------------------------------------------------------------
class JsonSerializerFactory final : public ISerializerFactory {
 public:
  std::string Name() const override { return "json-factory"; }
  SerializerPtr Create(const TypeDescriptor& descriptor) override;
};

// 便捷函数：按描述符创建序列化器（等价于 JsonSerializerFactory().Create(d)）。
// 将来接入更多格式时，这里会成为统一的分发点。
SerializerPtr CreateSerializer(const TypeDescriptor& descriptor);

}  // namespace types
}  // namespace stratum

#endif  // STRATUM_TYPES_JSON_SERIALIZER_H_
