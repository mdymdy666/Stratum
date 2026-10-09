// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 序列化插件接口
//
// Stratum 记录的是"原始字节"。但要能做内容分析与离线解析，
// 就需要把字节还原成结构化数据 —— 这正是本接口的职责。
//
// 设计取舍（见 docs/03-open-questions.md 的 Q11）：
//   v1 允许"无类型定义"的降级模式：只按字节回放，不解析内容。
//   因此本接口的所有方法都必须能优雅处理"类型未知"的情况。

#ifndef STRATUM_TYPES_ISERIALIZER_H_
#define STRATUM_TYPES_ISERIALIZER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "stratum/core/status.h"

namespace stratum {
namespace types {

// ---------------------------------------------------------------------------
// 类型描述
// ---------------------------------------------------------------------------
struct TypeDescriptor {
  std::string name;               // 类型名
  std::string definition;         // 类型定义原文（可为空）
  std::string definition_format;  // "idl" / "json" / "typeobject" / ""

  bool HasDefinition() const { return !definition.empty(); }
};

// ---------------------------------------------------------------------------
// 动态值
//
// 刻意设计成"树"而非"变体数组"，便于自描述与调试输出。
// 不追求表达任意 IDL 特性，只覆盖实际用到的主力场景。
// ---------------------------------------------------------------------------
struct DynamicValue {
  enum class Kind : uint8_t {
    kNull = 0,
    kBool,
    kInt32,
    kInt64,
    kUInt32,
    kUInt64,
    kDouble,
    kString,
    kBytes,
    kArray,
    kStruct,
  };

  Kind kind = Kind::kNull;

  bool bool_value = false;
  int32_t int32_value = 0;
  int64_t int64_value = 0;
  uint32_t uint32_value = 0;
  uint64_t uint64_value = 0;
  double double_value = 0.0;
  std::string string_value;  // kString 与 kBytes 共用

  std::vector<DynamicValue> elements;                        // kArray
  std::vector<std::pair<std::string, DynamicValue>> fields;  // kStruct

  static DynamicValue MakeString(std::string s);
  static DynamicValue MakeInt64(int64_t v);
  static DynamicValue MakeDouble(double v);
  static DynamicValue MakeBytes(std::string b);
  static DynamicValue MakeArray();
  static DynamicValue MakeStruct();

  std::string ToString(int indent = 0) const;
};

// ---------------------------------------------------------------------------
// 序列化器
//
// 生命周期：一个实例可复用于同一种类型。
// 线程安全：Serialize/Deserialize 必须可并发调用。
// ---------------------------------------------------------------------------
class ISerializer {
 public:
  virtual ~ISerializer() = default;

  virtual bool supports_deserialization() const = 0;
  virtual bool supports_serialization() const = 0;
  virtual std::string Name() const = 0;

  virtual Status Serialize(const DynamicValue& value, std::string* out) = 0;
  virtual Status Deserialize(const std::string& bytes, DynamicValue* out) = 0;
  virtual size_t EstimateSize(const DynamicValue& value) const = 0;
};

using SerializerPtr = std::shared_ptr<ISerializer>;

// ---------------------------------------------------------------------------
// 工厂
// ---------------------------------------------------------------------------
class ISerializerFactory {
 public:
  virtual ~ISerializerFactory() = default;

  virtual std::string Name() const = 0;

  // 为指定类型创建序列化器。
  // descriptor 可能不含定义（降级模式）——此时应返回一个
  // supports_deserialization() == false 的"透传"序列化器，而不是失败。
  virtual SerializerPtr Create(const TypeDescriptor& descriptor) = 0;
};

}  // namespace types
}  // namespace stratum

#endif  // STRATUM_TYPES_ISERIALIZER_H_
