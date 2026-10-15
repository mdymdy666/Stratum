// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/src/types/dynamic_value.cc
//
// DynamicValue 的构造工厂与自描述文本输出。
//
// 【为什么需要这个文件】iserializer.h 早就声明了这些符号，但一直没有实现
// （tests/types 里的 KnownGap 用例记录了这点：nm 扫遍静态库无符号）。
// 结果是整个"类型系统"只是空壳：数据存进去是黑盒字节，
// 无法按字段查询、无法结构化导出、无法可视化。
//
// 本文件填上基础层。ToString 刻意输出 **JSON 风格**文本 ——
// 因为它既便于人读，又能直接被 CLI 当结构化输出使用（stdout 是数据出口）。
//
// 关于 kBytes：它输出 "hex:..." 前缀而不是当字符串处理。
// 二进制内容冒充文本会让人误读，**明确标注比看起来干净更重要**。

#include "stratum/types/iserializer.h"

#include <cstdio>
#include <sstream>

namespace stratum {
namespace types {

namespace {

void AppendIndent(std::string* out, int indent) {
  out->append(static_cast<size_t>(indent) * 2, ' ');
}

// JSON 字符串转义（与 CLI 的 JsonEscape 语义一致，但此处独立实现，
// 避免 types 层反向依赖 CLI 层）
void AppendJsonString(std::string* out, const std::string& s) {
  out->push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out->append("\\\""); break;
      case '\\': out->append("\\\\"); break;
      case '\n': out->append("\\n"); break;
      case '\r': out->append("\\r"); break;
      case '\t': out->append("\\t"); break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out->append(buf);
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

void AppendHex(std::string* out, const std::string& bytes) {
  static const char* kDigits = "0123456789abcdef";
  out->append("hex:");
  for (unsigned char c : bytes) {
    out->push_back(kDigits[c >> 4]);
    out->push_back(kDigits[c & 0x0F]);
  }
}

// double 用 17 位有效数字 —— IEEE754 往返安全（否则 ToString 后再解析会失真）
std::string FormatDouble(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// 工厂
// ---------------------------------------------------------------------------

DynamicValue DynamicValue::MakeNull() {
  DynamicValue v;
  v.kind = Kind::kNull;
  return v;
}

DynamicValue DynamicValue::MakeBool(bool b) {
  DynamicValue v;
  v.kind = Kind::kBool;
  v.bool_value = b;
  return v;
}

DynamicValue DynamicValue::MakeInt32(int32_t i) {
  DynamicValue v;
  v.kind = Kind::kInt32;
  v.int32_value = i;
  return v;
}

DynamicValue DynamicValue::MakeInt64(int64_t i) {
  DynamicValue v;
  v.kind = Kind::kInt64;
  v.int64_value = i;
  return v;
}

DynamicValue DynamicValue::MakeUInt32(uint32_t i) {
  DynamicValue v;
  v.kind = Kind::kUInt32;
  v.uint32_value = i;
  return v;
}

DynamicValue DynamicValue::MakeUInt64(uint64_t i) {
  DynamicValue v;
  v.kind = Kind::kUInt64;
  v.uint64_value = i;
  return v;
}

DynamicValue DynamicValue::MakeDouble(double d) {
  DynamicValue v;
  v.kind = Kind::kDouble;
  v.double_value = d;
  return v;
}

DynamicValue DynamicValue::MakeString(std::string s) {
  DynamicValue v;
  v.kind = Kind::kString;
  v.string_value = std::move(s);
  return v;
}

DynamicValue DynamicValue::MakeBytes(std::string b) {
  DynamicValue v;
  v.kind = Kind::kBytes;
  v.string_value = std::move(b);
  return v;
}

DynamicValue DynamicValue::MakeArray() {
  DynamicValue v;
  v.kind = Kind::kArray;
  return v;
}

DynamicValue DynamicValue::MakeStruct() {
  DynamicValue v;
  v.kind = Kind::kStruct;
  return v;
}

// ---------------------------------------------------------------------------
// ToString（JSON 风格，带缩进）
//
// 输出目标有两个：人读、以及被 CLI 当结构化数据直接用。
// 因此语法严格贴 JSON（对象键一定加引号、字符串转义、二进制加 hex: 前缀）。
// ---------------------------------------------------------------------------

std::string DynamicValue::ToString(int indent) const {
  std::string out;
  switch (kind) {
    case Kind::kNull:
      out = "null";
      break;
    case Kind::kBool:
      out = bool_value ? "true" : "false";
      break;
    case Kind::kInt32:
      out = std::to_string(int32_value);
      break;
    case Kind::kInt64:
      out = std::to_string(int64_value);
      break;
    case Kind::kUInt32:
      out = std::to_string(uint32_value);
      break;
    case Kind::kUInt64:
      out = std::to_string(uint64_value);
      break;
    case Kind::kDouble:
      out = FormatDouble(double_value);
      break;
    case Kind::kString:
      AppendJsonString(&out, string_value);
      break;
    case Kind::kBytes:
      // 二进制不当文本处理：加 hex: 前缀，避免被误读
      out = "\"";
      AppendHex(&out, string_value);
      out += "\"";
      break;
    case Kind::kArray: {
      if (elements.empty()) { out = "[]"; break; }
      out = "[\n";
      for (size_t i = 0; i < elements.size(); ++i) {
        AppendIndent(&out, indent + 1);
        out += elements[i].ToString(indent + 1);
        if (i + 1 < elements.size()) out += ",";
        out += "\n";
      }
      AppendIndent(&out, indent);
      out += "]";
      break;
    }
    case Kind::kStruct: {
      if (fields.empty()) { out = "{}"; break; }
      out = "{\n";
      for (size_t i = 0; i < fields.size(); ++i) {
        AppendIndent(&out, indent + 1);
        AppendJsonString(&out, fields[i].first);
        out += ": ";
        out += fields[i].second.ToString(indent + 1);
        if (i + 1 < fields.size()) out += ",";
        out += "\n";
      }
      AppendIndent(&out, indent);
      out += "}";
      break;
    }
  }
  return out;
}

}  // namespace types
}  // namespace stratum
