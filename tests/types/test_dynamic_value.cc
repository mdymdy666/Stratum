// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// DynamicValue 工厂与自描述输出的测试
//
// 【这套测试的意义】它把 tests/types 里那条 KnownGap 用例（"工厂与 ToString
// 未实现"）变成**已被覆盖的能力**。在实现之前，整个类型系统只是空壳：
// 数据存进去是黑盒字节，无法按字段查询、无法结构化导出、无法可视化。
//
// 覆盖三类（项目测试标准）：
//   · 大数据量 —— 10 万元素的数组
//   · 随机     —— 多种子随机嵌套结构，ToString 后语法必须自洽
//   · 真实场景 —— ① 传感器读数（struct）② 点表（array of struct）③ 二进制负载

// 标准库头文件必须最先包含
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "stratum/types/iserializer.h"
#include "test_framework.h"

using stratum::types::DynamicValue;

namespace {

// 粗略校验输出像不像 JSON（括号配平、引号成对）
bool LooksBalanced(const std::string& s) {
  int brace = 0, bracket = 0, quote = 0;
  bool esc = false;
  for (char c : s) {
    if (esc) { esc = false; continue; }
    if (c == '\\') { esc = true; continue; }
    if (c == '"') { ++quote; continue; }
    if (quote % 2 == 1) continue;   // 字符串内部不计括号
    if (c == '{') ++brace;
    if (c == '}') --brace;
    if (c == '[') ++bracket;
    if (c == ']') --bracket;
  }
  return brace == 0 && bracket == 0 && quote % 2 == 0;
}

}  // namespace

// ===========================================================================
// 一、工厂：11 种 Kind 都能构造，且 kind 与值一致
// ===========================================================================

STRATUM_TEST(DynamicValue, AllElevenFactoriesSetKindAndValue) {
  const auto n = DynamicValue::MakeNull();
  STRATUM_CHECK_EQ(static_cast<int>(n.kind),
                   static_cast<int>(DynamicValue::Kind::kNull));

  const auto b = DynamicValue::MakeBool(true);
  STRATUM_CHECK_EQ(static_cast<int>(b.kind),
                   static_cast<int>(DynamicValue::Kind::kBool));
  STRATUM_CHECK_TRUE(b.bool_value);

  const auto i32 = DynamicValue::MakeInt32(-2147483647);
  STRATUM_CHECK_EQ(static_cast<int>(i32.kind),
                   static_cast<int>(DynamicValue::Kind::kInt32));
  STRATUM_CHECK_EQ(i32.int32_value, -2147483647);

  const auto i64 = DynamicValue::MakeInt64(-9223372036854775807LL);
  STRATUM_CHECK_EQ(i64.int64_value, -9223372036854775807LL);

  const auto u32 = DynamicValue::MakeUInt32(4294967295U);
  STRATUM_CHECK_EQ(u32.uint32_value, 4294967295U);

  const auto u64 = DynamicValue::MakeUInt64(18446744073709551615ULL);
  STRATUM_CHECK_EQ(u64.uint64_value, 18446744073709551615ULL);

  const auto d = DynamicValue::MakeDouble(3.5);
  STRATUM_CHECK_TRUE(d.double_value > 3.49 && d.double_value < 3.51);

  const auto s = DynamicValue::MakeString("abc");
  STRATUM_CHECK_EQ(s.string_value, std::string("abc"));

  const auto by = DynamicValue::MakeBytes(std::string("\x00\x01\xff", 3));
  STRATUM_CHECK_EQ(by.string_value.size(), static_cast<size_t>(3));

  const auto arr = DynamicValue::MakeArray();
  STRATUM_CHECK_TRUE(arr.elements.empty());

  const auto st = DynamicValue::MakeStruct();
  STRATUM_CHECK_TRUE(st.fields.empty());
}

// ===========================================================================
// 二、ToString：标量与转义
// ===========================================================================

STRATUM_TEST(DynamicValue, ToStringScalars) {
  STRATUM_CHECK_EQ(DynamicValue::MakeNull().ToString(), std::string("null"));
  STRATUM_CHECK_EQ(DynamicValue::MakeBool(true).ToString(), std::string("true"));
  STRATUM_CHECK_EQ(DynamicValue::MakeBool(false).ToString(), std::string("false"));
  STRATUM_CHECK_EQ(DynamicValue::MakeInt32(-7).ToString(), std::string("-7"));
  STRATUM_CHECK_EQ(DynamicValue::MakeInt64(123456789012345LL).ToString(),
                   std::string("123456789012345"));
  STRATUM_CHECK_EQ(DynamicValue::MakeUInt64(42).ToString(), std::string("42"));
  STRATUM_CHECK_EQ(DynamicValue::MakeString("hi").ToString(), std::string("\"hi\""));
}

STRATUM_TEST(DynamicValue, ToStringEscapesStrings) {
  // 引号、反斜杠、换行、制表都必须转义，否则输出不是合法 JSON
  const auto v = DynamicValue::MakeString("a\"b\\c\nd\te");
  const std::string out = v.ToString();
  STRATUM_CHECK_TRUE(out.find("\\\"") != std::string::npos);
  STRATUM_CHECK_TRUE(out.find("\\\\") != std::string::npos);
  STRATUM_CHECK_TRUE(out.find("\\n") != std::string::npos);
  STRATUM_CHECK_TRUE(out.find("\\t") != std::string::npos);
  // 不能出现裸换行（那会破坏单行 JSON）
  STRATUM_CHECK_TRUE(out.find('\n') == std::string::npos);
  STRATUM_CHECK_TRUE(LooksBalanced(out));
}

STRATUM_TEST(DynamicValue, BytesAreMarkedHexNotFakedAsText) {
  // 【设计裁决】二进制内容不当文本处理，加 hex: 前缀。
  // 冒充字符串会让人误读 —— 明确标注比看起来干净更重要。
  const auto v = DynamicValue::MakeBytes(std::string("\x00\x0a\xff", 3));
  const std::string out = v.ToString();
  STRATUM_CHECK_EQ(out, std::string("\"hex:000aff\""));
}

STRATUM_TEST(DynamicValue, DoubleUsesRoundTripPrecision) {
  // 17 位有效数字是 IEEE754 往返安全的最低要求；
  // 若用 %.6g，ToString 后再解析会失真。
  const double tricky = 0.1 + 0.2;   // 0.30000000000000004
  const std::string out = DynamicValue::MakeDouble(tricky).ToString();
  const double back = std::stod(out);
  STRATUM_CHECK_TRUE(back == tricky);
}

// ===========================================================================
// 三、ToString：嵌套结构
// ===========================================================================

STRATUM_TEST(DynamicValue, NestedArrayAndStructWithIndent) {
  auto root = DynamicValue::MakeStruct();
  root.fields.emplace_back("id", DynamicValue::MakeInt64(7));
  root.fields.emplace_back("name", DynamicValue::MakeString("sensor"));

  auto arr = DynamicValue::MakeArray();
  arr.elements.push_back(DynamicValue::MakeDouble(1.5));
  arr.elements.push_back(DynamicValue::MakeDouble(2.5));
  root.fields.emplace_back("values", arr);

  auto nested = DynamicValue::MakeStruct();
  nested.fields.emplace_back("ok", DynamicValue::MakeBool(true));
  root.fields.emplace_back("meta", nested);

  const std::string out = root.ToString();
  STRATUM_CHECK_TRUE(out.find("\"id\": 7") != std::string::npos);
  STRATUM_CHECK_TRUE(out.find("\"name\": \"sensor\"") != std::string::npos);
  STRATUM_CHECK_TRUE(out.find("1.5") != std::string::npos);
  STRATUM_CHECK_TRUE(out.find("\"ok\": true") != std::string::npos);
  STRATUM_CHECK_TRUE(LooksBalanced(out));

  // 缩进必须真的生效（顶层字段前有 2 空格）
  STRATUM_CHECK_TRUE(out.find("\n  \"id\"") != std::string::npos);
}

STRATUM_TEST(DynamicValue, EmptyContainersAreCompactForm) {
  STRATUM_CHECK_EQ(DynamicValue::MakeArray().ToString(), std::string("[]"));
  STRATUM_CHECK_EQ(DynamicValue::MakeStruct().ToString(), std::string("{}"));
}

// ===========================================================================
// 四、大数据量
// ===========================================================================

STRATUM_TEST(DynamicValue, LargeArrayHundredThousand) {
  auto arr = DynamicValue::MakeArray();
  const size_t kN = 100000;
  arr.elements.reserve(kN);
  for (size_t i = 0; i < kN; ++i) {
    arr.elements.push_back(DynamicValue::MakeInt64(static_cast<int64_t>(i)));
  }
  const std::string out = arr.ToString();
  STRATUM_CHECK_TRUE(LooksBalanced(out));
  // 首尾必须正确（中间不逐项校验，避免测试自身成为瓶颈）
  STRATUM_CHECK_TRUE(out.find("\n  0,\n") != std::string::npos);
  STRATUM_CHECK_TRUE(out.rfind(std::to_string(kN - 1)) != std::string::npos);
  std::printf("      [规模] %zu 元素数组：ToString 产出 %zu 字节\n", kN, out.size());
}

// ===========================================================================
// 五、随机（多种子）
// ===========================================================================

namespace {

DynamicValue RandomValue(std::mt19937_64* rng, int depth) {
  std::uniform_int_distribution<int> kind_d(0, depth > 0 ? 8 : 6);
  switch (kind_d(*rng)) {
    case 0: return DynamicValue::MakeNull();
    case 1: return DynamicValue::MakeBool((*rng)() % 2 == 0);
    case 2: return DynamicValue::MakeInt32(static_cast<int32_t>((*rng)() % 100000));
    case 3: return DynamicValue::MakeUInt64((*rng)());
    case 4: return DynamicValue::MakeDouble(static_cast<double>((*rng)() % 10000) / 7.0);
    case 5: {
      std::string s;
      const int n = static_cast<int>((*rng)() % 12);
      for (int i = 0; i < n; ++i) {
        s.push_back(static_cast<char>('a' + ((*rng)() % 26)));
      }
      return DynamicValue::MakeString(s);
    }
    case 6: return DynamicValue::MakeBytes(std::string((*rng)() % 8, '\x01'));
    case 7: {
      auto a = DynamicValue::MakeArray();
      const int n = static_cast<int>((*rng)() % 5);
      for (int i = 0; i < n; ++i) a.elements.push_back(RandomValue(rng, depth - 1));
      return a;
    }
    default: {
      auto s = DynamicValue::MakeStruct();
      const int n = static_cast<int>((*rng)() % 5);
      for (int i = 0; i < n; ++i) {
        s.fields.emplace_back("f" + std::to_string(i), RandomValue(rng, depth - 1));
      }
      return s;
    }
  }
}

}  // namespace

STRATUM_TEST(DynamicValue, RandomStructuresMultipleSeeds) {
  for (uint64_t seed : {1ULL, 42ULL, 777ULL, 20261015ULL}) {
    std::mt19937_64 rng(seed);
    const int n = static_cast<int>(rng() % 60) + 1;
    for (int i = 0; i < n; ++i) {
      const DynamicValue v = RandomValue(&rng, 3);
      const std::string out = v.ToString();
      // 任意随机结构生成的输出都必须是语法自洽的
      STRATUM_CHECK_TRUE(LooksBalanced(out));
      STRATUM_CHECK_TRUE(!out.empty());
    }
  }
}

// ===========================================================================
// 六、真实场景
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1：传感器读数 —— 这是"采集"最典型的形态
//
// 关键：输出必须能直接喂给下游（JSON），且字段名可读。
// 在没有这套实现之前，这样的数据只能以"24 字节"的形式存在。
// ---------------------------------------------------------------------------
STRATUM_TEST(DynamicValueReal, SensorReadingStruct) {
  auto reading = DynamicValue::MakeStruct();
  reading.fields.emplace_back("temperature_c", DynamicValue::MakeDouble(23.5));
  reading.fields.emplace_back("humidity_pct", DynamicValue::MakeDouble(61.25));
  reading.fields.emplace_back("ts_ns", DynamicValue::MakeUInt64(1792044082460601271ULL));
  reading.fields.emplace_back("valid", DynamicValue::MakeBool(true));

  const std::string json = reading.ToString();
  std::printf("      [场景1] 传感器读数 → %s\n",
              json.substr(0, 90).c_str());
  STRATUM_CHECK_TRUE(json.find("\"temperature_c\": 23.5") != std::string::npos);
  STRATUM_CHECK_TRUE(json.find("\"humidity_pct\": 61.25") != std::string::npos);
  STRATUM_CHECK_TRUE(json.find("\"valid\": true") != std::string::npos);
  STRATUM_CHECK_TRUE(LooksBalanced(json));
}

// ---------------------------------------------------------------------------
// 场景 2：点表（array of struct）—— PLC / 工业采集的典型形态
// ---------------------------------------------------------------------------
STRATUM_TEST(DynamicValueReal, PointTableArrayOfStructs) {
  auto table = DynamicValue::MakeArray();
  const char* names[] = {"motor_speed", "spindle_load", "coolant_temp"};
  const double values[] = {1450.0, 78.5, 42.25};
  for (int i = 0; i < 3; ++i) {
    auto row = DynamicValue::MakeStruct();
    row.fields.emplace_back("name", DynamicValue::MakeString(names[i]));
    row.fields.emplace_back("value", DynamicValue::MakeDouble(values[i]));
    row.fields.emplace_back("quality", DynamicValue::MakeInt32(192));   // GOOD
    table.elements.push_back(row);
  }
  const std::string json = table.ToString();
  STRATUM_CHECK_TRUE(json.find("\"motor_speed\"") != std::string::npos);
  STRATUM_CHECK_TRUE(json.find("1450") != std::string::npos);
  STRATUM_CHECK_TRUE(LooksBalanced(json));
  std::printf("      [场景2] 点表 3 行 → %zu 字节 JSON\n", json.size());
}

// ---------------------------------------------------------------------------
// 场景 3：二进制负载 —— 必须能被明确标识，不能冒充文本
//
// 真实情况：很多 DDS 负载是自定义二进制格式。若把它当字符串输出，
// 下游会得到乱码且无从判断。hex: 前缀让它一眼可辨。
// ---------------------------------------------------------------------------
STRATUM_TEST(DynamicValueReal, BinaryPayloadIsUnambiguous) {
  std::string cdr;
  cdr.push_back('\x00');
  cdr.push_back('\x01');          // CDR_LE encapsulation
  cdr.append("\x00\x00", 2);
  for (int i = 0; i < 8; ++i) cdr.push_back(static_cast<char>(i * 3));

  auto wrapper = DynamicValue::MakeStruct();
  wrapper.fields.emplace_back("payload", DynamicValue::MakeBytes(cdr));
  wrapper.fields.emplace_back("encoding", DynamicValue::MakeString("cdr_le"));

  const std::string json = wrapper.ToString();
  STRATUM_CHECK_TRUE(json.find("\"hex:") != std::string::npos);
  STRATUM_CHECK_TRUE(json.find("\"encoding\": \"cdr_le\"") != std::string::npos);
  STRATUM_CHECK_TRUE(LooksBalanced(json));
  std::printf("      [场景3] 二进制负载 12 字节 → hex 前缀标注，未冒充文本\n");
}

STRATUM_TEST_MAIN()
