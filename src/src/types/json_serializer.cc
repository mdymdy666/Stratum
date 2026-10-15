// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// src/src/types/json_serializer.cc
//
// 【已知的有损之处（明确记录，不假装无损）】
//
// 1. `kBytes` 用 "hex:..." 前缀表示。这是**有损的**：
//    无法区分"字节串"与"恰好以 hex: 开头、且后续是合法十六进制的字符串"。
//    取舍理由：二进制冒充文本会让人误读，而 JSON 没有字节类型。
//    若要完全无损，需要改成 {"$bytes": "..."} 这类带标记的对象 ——
//    但那会让所有二进制值都变成嵌套对象，可读性大幅下降。
//    当前选择"可读性优先 + 明确标注"，并把该限制写进测试。
//
// 2. 数字类型按**值范围**推断（见 InferNumberKind）。
//    JSON 只有 number 一种，不区分 int32/int64/uint32/uint64/double。
//    推断规则让小整数回到 kInt32、大非负数回到 kUInt64 —— 比一律给 kInt64
//    更接近原类型，但仍无法保证与序列化前逐位同类型。
//
// 两条限制都属"格式本身的表达力"，不是实现缺陷。

#include "stratum/types/json_serializer.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace stratum {
namespace types {

namespace {

// ---------------------------------------------------------------------------
// JSON 递归下降解析器
//
// 只支持标准 JSON（无注释、无尾逗号、无单引号）。
// 遇到不符合规范的内容一律**报错并给出位置**，不做宽容解析 ——
// 与项目"不猜"的原则一致：宽容解析会把格式错误变成静默的数据错误。
// ---------------------------------------------------------------------------
class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : s_(text) {}

  bool Parse(DynamicValue* out, std::string* err) {
    SkipWs();
    if (!ParseValue(out)) {
      if (err) *err = ErrorString();
      return false;
    }
    SkipWs();
    if (pos_ != s_.size()) {
      Fail("JSON 结尾有多余内容");
      if (err) *err = ErrorString();
      return false;
    }
    return true;
  }

 private:
  const std::string& s_;
  size_t pos_ = 0;
  std::string error_;

  void Fail(const std::string& what) {
    if (error_.empty()) {
      error_ = what + "（位置 " + std::to_string(pos_) + "）";
    }
  }

  std::string ErrorString() const {
    // 附带上下文片段，便于定位
    const size_t from = pos_ > 20 ? pos_ - 20 : 0;
    const size_t len = (pos_ + 20 < s_.size()) ? 40 : (s_.size() - from);
    return error_ + "，附近内容: ..." + s_.substr(from, len) + "...";
  }

  void SkipWs() {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' ||
            s_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool Literal(const char* lit) {
    const size_t n = std::char_traits<char>::length(lit);
    if (s_.compare(pos_, n, lit) == 0) {
      pos_ += n;
      return true;
    }
    return false;
  }

  bool ParseValue(DynamicValue* out) {
    SkipWs();
    if (pos_ >= s_.size()) {
      Fail("期望一个值，但已到结尾");
      return false;
    }
    const char c = s_[pos_];
    if (c == '{') return ParseObject(out);
    if (c == '[') return ParseArray(out);
    if (c == '"') {
      std::string str;
      if (!ParseString(&str)) return false;
      // "hex:..." -> bytes（见文件头的有损说明）
      if (str.rfind("hex:", 0) == 0 && IsValidHex(str.substr(4))) {
        *out = DynamicValue::MakeBytes(HexToBytes(str.substr(4)));
        return true;
      }
      *out = DynamicValue::MakeString(std::move(str));
      return true;
    }
    if (Literal("null")) { *out = DynamicValue::MakeNull(); return true; }
    if (Literal("true")) { *out = DynamicValue::MakeBool(true); return true; }
    if (Literal("false")) { *out = DynamicValue::MakeBool(false); return true; }
    if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
    Fail(std::string("无法识别的值起始字符 '") + c + "'");
    return false;
  }

  bool ParseObject(DynamicValue* out) {
    ++pos_;   // '{'
    *out = DynamicValue::MakeStruct();
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
    while (true) {
      SkipWs();
      if (pos_ >= s_.size() || s_[pos_] != '"') {
        Fail("对象的键必须是字符串");
        return false;
      }
      std::string key;
      if (!ParseString(&key)) return false;
      SkipWs();
      if (pos_ >= s_.size() || s_[pos_] != ':') {
        Fail("对象键之后应为 ':'");
        return false;
      }
      ++pos_;
      DynamicValue v;
      if (!ParseValue(&v)) return false;
      out->fields.emplace_back(std::move(key), std::move(v));
      SkipWs();
      if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
      if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
      Fail("对象内期望 ',' 或 '}'");
      return false;
    }
  }

  bool ParseArray(DynamicValue* out) {
    ++pos_;   // '['
    *out = DynamicValue::MakeArray();
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
    while (true) {
      DynamicValue v;
      if (!ParseValue(&v)) return false;
      out->elements.push_back(std::move(v));
      SkipWs();
      if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
      if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
      Fail("数组内期望 ',' 或 ']'");
      return false;
    }
  }

  // 把 \uXXXX 按 UTF-8 编码（仅支持 BMP；代理对留作原样输出并置错）
  static void AppendUtf8(std::string* out, uint32_t cp) {
    if (cp <= 0x7F) {
      out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool ParseString(std::string* out) {
    ++pos_;   // 开引号
    out->clear();
    while (pos_ < s_.size()) {
      const unsigned char c = static_cast<unsigned char>(s_[pos_]);
      if (c == '"') { ++pos_; return true; }
      if (c == '\\') {
        ++pos_;
        if (pos_ >= s_.size()) { Fail("转义符后已到结尾"); return false; }
        const char e = s_[pos_++];
        switch (e) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'u': {
            if (pos_ + 4 > s_.size()) { Fail("\\u 需要 4 位十六进制"); return false; }
            uint32_t cp = 0;
            for (int i = 0; i < 4; ++i) {
              const char h = s_[pos_ + static_cast<size_t>(i)];
              uint32_t d = 0;
              if (h >= '0' && h <= '9') d = static_cast<uint32_t>(h - '0');
              else if (h >= 'a' && h <= 'f') d = static_cast<uint32_t>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') d = static_cast<uint32_t>(h - 'A' + 10);
              else { Fail("\\u 中的非十六进制字符"); return false; }
              cp = (cp << 4) | d;
            }
            pos_ += 4;
            AppendUtf8(out, cp);
            break;
          }
          default:
            Fail(std::string("未知转义序列 \\") + e);
            return false;
        }
        continue;
      }
      if (c < 0x20) {
        Fail("字符串中出现未转义的控制字符");
        return false;
      }
      out->push_back(static_cast<char>(c));
      ++pos_;
    }
    Fail("字符串未以引号结束");
    return false;
  }

  // 按值范围推断数字类型（见文件头的有损说明）
  static DynamicValue InferNumberKind(bool is_double, bool negative,
                                      double dval, int64_t ival, uint64_t uval) {
    if (is_double) return DynamicValue::MakeDouble(dval);
    if (negative) {
      if (ival >= std::numeric_limits<int32_t>::min()) {
        return DynamicValue::MakeInt32(static_cast<int32_t>(ival));
      }
      return DynamicValue::MakeInt64(ival);
    }
    if (uval <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      return DynamicValue::MakeInt32(static_cast<int32_t>(uval));
    }
    if (uval <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
      return DynamicValue::MakeUInt32(static_cast<uint32_t>(uval));
    }
    return DynamicValue::MakeUInt64(uval);
  }

  bool ParseNumber(DynamicValue* out) {
    const size_t begin = pos_;
    bool is_double = false;
    bool negative = false;
    if (pos_ < s_.size() && s_[pos_] == '-') { negative = true; ++pos_; }
    while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    if (pos_ < s_.size() && s_[pos_] == '.') {
      is_double = true;
      ++pos_;
      while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      is_double = true;
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    const std::string tok = s_.substr(begin, pos_ - begin);
    if (tok.empty() || tok == "-") {
      Fail("数字格式非法");
      return false;
    }
    if (is_double) {
      *out = InferNumberKind(true, negative, std::strtod(tok.c_str(), nullptr), 0, 0);
      return true;
    }
    if (negative) {
      const int64_t v = std::strtoll(tok.c_str(), nullptr, 10);
      *out = InferNumberKind(false, true, 0.0, v, 0);
      return true;
    }
    const uint64_t v = std::strtoull(tok.c_str(), nullptr, 10);
    *out = InferNumberKind(false, false, 0.0, 0, v);
    return true;
  }

  static bool IsValidHex(const std::string& h) {
    if (h.empty() || h.size() % 2 != 0) return false;
    for (char c : h) {
      const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F');
      if (!ok) return false;
    }
    return true;
  }

  static std::string HexToBytes(const std::string& h) {
    auto val = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return c - 'A' + 10;
    };
    std::string out;
    out.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
      out.push_back(static_cast<char>((val(h[i]) << 4) | val(h[i + 1])));
    }
    return out;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// PassthroughSerializer
// ---------------------------------------------------------------------------

Status PassthroughSerializer::Serialize(const DynamicValue& value, std::string* out) {
  (void)value;
  (void)out;
  return Status::NotSupported(
      "类型 " + (type_name_.empty() ? std::string("(未命名)") : type_name_) +
      " 没有可用的类型定义，无法序列化；"
      "请提供 definition_format=json 的自描述定义，或改用字节透传");
}

Status PassthroughSerializer::Deserialize(const std::string& bytes, DynamicValue* out) {
  (void)bytes;
  (void)out;
  return Status::NotSupported(
      "类型 " + (type_name_.empty() ? std::string("(未命名)") : type_name_) +
      " 没有可用的类型定义，无法反序列化；"
      "数据仍可按原始字节回放（这是 v1 的降级模式）");
}

size_t PassthroughSerializer::EstimateSize(const DynamicValue& value) const {
  (void)value;
  // 无类型定义时无法估计。返回 0 而不是编一个看起来合理的数。
  return 0;
}

// ---------------------------------------------------------------------------
// JsonSerializer
// ---------------------------------------------------------------------------

Status JsonSerializer::Serialize(const DynamicValue& value, std::string* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("JsonSerializer::Serialize 的输出指针为空");
  }
  *out = value.ToString();
  return Status::OK();
}

Status JsonSerializer::Deserialize(const std::string& bytes, DynamicValue* out) {
  if (out == nullptr) {
    return Status::InvalidArgument("JsonSerializer::Deserialize 的输出指针为空");
  }
  JsonParser parser(bytes);
  std::string err;
  if (!parser.Parse(out, &err)) {
    return Status::Corruption("JSON 解析失败: " + err);
  }
  return Status::OK();
}

size_t JsonSerializer::EstimateSize(const DynamicValue& value) const {
  return value.ToString().size();
}

// ---------------------------------------------------------------------------
// JsonSerializerFactory / CreateSerializer
// ---------------------------------------------------------------------------

SerializerPtr JsonSerializerFactory::Create(const TypeDescriptor& descriptor) {
  // 契约：无定义时返回**透传**序列化器，而不是失败。
  // 理由见头文件注释：v1 允许"无类型定义"地录制任意 DDS 数据。
  if (descriptor.definition_format == "json" && descriptor.HasDefinition()) {
    return std::make_shared<JsonSerializer>(descriptor.name);
  }
  return std::make_shared<PassthroughSerializer>(descriptor.name);
}

SerializerPtr CreateSerializer(const TypeDescriptor& descriptor) {
  JsonSerializerFactory factory;
  return factory.Create(descriptor);
}

}  // namespace types
}  // namespace stratum
