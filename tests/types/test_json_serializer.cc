// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// JSON 序列化器 / 工厂 / 降级模式的测试
//
// 【本测试的两个重点】
//
// 1. **降级模式必须"不失败"**
//    iserializer.h 的契约写明：描述符无定义时应返回 supports_deserialization()
//    == false 的透传序列化器，**而不是失败**。因为 v1 允许无类型定义地录制
//    任意 DDS 数据 —— 若工厂在这种情况下失败，录制链路会被未注册类型打断。
//    本测试断言这条契约。
//
// 2. **两处已知有损必须被断言，而不是被掩盖**
//    · kBytes 与"恰好以 hex: 开头且后续为合法十六进制的字符串"无法区分
//    · 数字类型由值范围推断，JSON 不区分 int32/int64/uint32/uint64/double
//    把它们写成断言，是为了让将来改动时**立刻看到影响面**。

// 标准库头文件必须最先包含
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "stratum/types/json_serializer.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::types;

namespace {

std::string RoundTripSerialize(const DynamicValue& v) {
  JsonSerializer s;
  std::string out;
  STRATUM_CHECK_OK(s.Serialize(v, &out));
  return out;
}

DynamicValue RoundTrip(const DynamicValue& v) {
  JsonSerializer s;
  std::string out;
  STRATUM_CHECK_OK(s.Serialize(v, &out));
  DynamicValue back;
  STRATUM_CHECK_OK(s.Deserialize(out, &back));
  return back;
}

}  // namespace

// ===========================================================================
// 一、降级模式（契约）
// ===========================================================================

STRATUM_TEST(JsonSerializer, PassthroughDeclaresNoSupportAndDoesNotFail) {
  PassthroughSerializer ps("unknown/type");
  STRATUM_CHECK_FALSE(ps.supports_serialization());
  STRATUM_CHECK_FALSE(ps.supports_deserialization());
  STRATUM_CHECK_EQ(ps.Name(), std::string("passthrough"));

  // 调用必须返回明确的 NotSupported，并说明"还能怎么用"
  std::string out;
  Status s = ps.Serialize(DynamicValue::MakeInt32(1), &out);
  STRATUM_CHECK_FALSE(s.ok());
  STRATUM_CHECK_TRUE(s.IsNotSupported());
  STRATUM_CHECK_TRUE(s.message().find("json") != std::string::npos);

  DynamicValue v;
  Status d = ps.Deserialize("bytes", &v);
  STRATUM_CHECK_TRUE(d.IsNotSupported());
  // 消息里应提示"仍可按字节回放"—— 让调用方知道退路
  STRATUM_CHECK_TRUE(d.message().find("字节回放") != std::string::npos);

  // 无类型定义时不编造尺寸估计
  STRATUM_CHECK_EQ(ps.EstimateSize(DynamicValue::MakeInt32(1)), static_cast<size_t>(0));
}

STRATUM_TEST(JsonSerializer, FactoryFallsBackInsteadOfFailing) {
  JsonSerializerFactory f;
  STRATUM_CHECK_EQ(f.Name(), std::string("json-factory"));

  // 1) 完全没有定义 -> 透传（**不是失败**）
  TypeDescriptor none;
  none.name = "no/def";
  SerializerPtr s1 = f.Create(none);
  STRATUM_CHECK_TRUE(s1 != nullptr);
  STRATUM_CHECK_FALSE(s1->supports_deserialization());

  // 2) 有定义但格式不是 json -> 同样透传
  TypeDescriptor idl;
  idl.name = "idl/type";
  idl.definition = "struct Foo { long a; };";
  idl.definition_format = "idl";
  SerializerPtr s2 = f.Create(idl);
  STRATUM_CHECK_TRUE(s2 != nullptr);
  STRATUM_CHECK_FALSE(s2->supports_deserialization());

  // 3) json 自描述 -> 真序列化器
  TypeDescriptor js;
  js.name = "json/type";
  js.definition = "{\"a\":\"int32\"}";
  js.definition_format = "json";
  SerializerPtr s3 = f.Create(js);
  STRATUM_CHECK_TRUE(s3 != nullptr);
  STRATUM_CHECK_TRUE(s3->supports_serialization());
  STRATUM_CHECK_TRUE(s3->supports_deserialization());
  STRATUM_CHECK_EQ(s3->Name(), std::string("json"));

  // 便捷函数与工厂一致
  SerializerPtr s4 = CreateSerializer(js);
  STRATUM_CHECK_TRUE(s4->supports_deserialization());
}

// ===========================================================================
// 二、往返（保留类型的情形）
// ===========================================================================

STRATUM_TEST(JsonSerializer, ScalarRoundTripPreservesKind) {
  STRATUM_CHECK_EQ(static_cast<int>(RoundTrip(DynamicValue::MakeNull()).kind),
                   static_cast<int>(DynamicValue::Kind::kNull));
  STRATUM_CHECK_TRUE(RoundTrip(DynamicValue::MakeBool(true)).bool_value);
  STRATUM_CHECK_FALSE(RoundTrip(DynamicValue::MakeBool(false)).bool_value);

  // 小整数 -> int32（范围推断）
  const auto i = RoundTrip(DynamicValue::MakeInt32(-12345));
  STRATUM_CHECK_EQ(static_cast<int>(i.kind),
                   static_cast<int>(DynamicValue::Kind::kInt32));
  STRATUM_CHECK_EQ(i.int32_value, -12345);

  // 超出 int32 的负数 -> int64
  const auto big = RoundTrip(DynamicValue::MakeInt64(-5000000000LL));
  STRATUM_CHECK_EQ(static_cast<int>(big.kind),
                   static_cast<int>(DynamicValue::Kind::kInt64));
  STRATUM_CHECK_EQ(big.int64_value, -5000000000LL);

  // 超出 uint32 的非负数 -> uint64
  const auto u = RoundTrip(DynamicValue::MakeUInt64(10000000000ULL));
  STRATUM_CHECK_EQ(static_cast<int>(u.kind),
                   static_cast<int>(DynamicValue::Kind::kUInt64));
  STRATUM_CHECK_EQ(u.uint64_value, 10000000000ULL);

  // double 往返必须逐位相等
  const double d = 0.1 + 0.2;
  const auto rd = RoundTrip(DynamicValue::MakeDouble(d));
  STRATUM_CHECK_EQ(static_cast<int>(rd.kind),
                   static_cast<int>(DynamicValue::Kind::kDouble));
  STRATUM_CHECK_TRUE(rd.double_value == d);
}

STRATUM_TEST(JsonSerializer, NestedRoundTrip) {
  auto root = DynamicValue::MakeStruct();
  root.fields.emplace_back("id", DynamicValue::MakeInt32(7));
  root.fields.emplace_back("name", DynamicValue::MakeString("sensor"));
  auto arr = DynamicValue::MakeArray();
  arr.elements.push_back(DynamicValue::MakeDouble(1.5));
  arr.elements.push_back(DynamicValue::MakeBool(false));
  root.fields.emplace_back("values", arr);

  const auto back = RoundTrip(root);
  STRATUM_CHECK_EQ(static_cast<int>(back.kind),
                   static_cast<int>(DynamicValue::Kind::kStruct));
  STRATUM_CHECK_EQ(back.fields.size(), static_cast<size_t>(3));
  STRATUM_CHECK_EQ(back.fields[0].first, std::string("id"));
  STRATUM_CHECK_EQ(back.fields[0].second.int32_value, 7);
  STRATUM_CHECK_EQ(back.fields[1].second.string_value, std::string("sensor"));
  STRATUM_CHECK_EQ(back.fields[2].second.elements.size(), static_cast<size_t>(2));
  STRATUM_CHECK_TRUE(back.fields[2].second.elements[0].double_value == 1.5);
  STRATUM_CHECK_FALSE(back.fields[2].second.elements[1].bool_value);
}

STRATUM_TEST(JsonSerializer, StringEscapesSurviveRoundTrip) {
  const std::string tricky = "a\"b\\c\nd\te\u00e9";   // 含引号/反斜杠/换行/制表/非 ASCII
  const auto back = RoundTrip(DynamicValue::MakeString(tricky));
  STRATUM_CHECK_EQ(static_cast<int>(back.kind), static_cast<int>(DynamicValue::Kind::kString));
  STRATUM_CHECK_EQ(back.string_value, tricky);
}

STRATUM_TEST(JsonSerializer, BytesRoundTripViaHex) {
  std::string b;
  for (int i = 0; i < 16; ++i) b.push_back(static_cast<char>(i * 7));
  const auto back = RoundTrip(DynamicValue::MakeBytes(b));
  STRATUM_CHECK_EQ(static_cast<int>(back.kind),
                   static_cast<int>(DynamicValue::Kind::kBytes));
  STRATUM_CHECK_EQ(back.string_value, b);
}

// ===========================================================================
// 三、已知有损：写成断言，让将来改动立刻看到影响
// ===========================================================================

STRATUM_TEST(JsonSerializer, KnownLoss_BytesPrefixCollidesWithOrdinaryString) {
  // 【已知有损 1】"hex:0a0b" 这样的普通字符串会被解析成 kBytes。
  // 取舍：二进制冒充文本会让人误读，而 JSON 没有字节类型。
  // 若要让两者可区分，需要改成 {"$bytes": ...} 这类带标记的对象 ——
  // 但会让所有二进制值变成嵌套对象，可读性大幅下降。
  const auto s = RoundTrip(DynamicValue::MakeString("hex:0a0b"));
  STRATUM_CHECK_EQ(static_cast<int>(s.kind),
                   static_cast<int>(DynamicValue::Kind::kBytes));
  STRATUM_CHECK_EQ(s.string_value, std::string("\x0a\x0b", 2));

  // 但不满足十六进制形式（奇数长度/非 hex 字符）时仍是字符串 —— 冲突面有限
  const auto s2 = RoundTrip(DynamicValue::MakeString("hex:0a0"));
  STRATUM_CHECK_EQ(static_cast<int>(s2.kind), static_cast<int>(DynamicValue::Kind::kString));
  const auto s3 = RoundTrip(DynamicValue::MakeString("hex:zz"));
  STRATUM_CHECK_EQ(static_cast<int>(s3.kind), static_cast<int>(DynamicValue::Kind::kString));
  const auto s4 = RoundTrip(DynamicValue::MakeString("hex:"));
  STRATUM_CHECK_EQ(static_cast<int>(s4.kind), static_cast<int>(DynamicValue::Kind::kString));
}

STRATUM_TEST(JsonSerializer, KnownLoss_NumberKindIsInferredNotPreserved) {
  // 【已知有损 2】JSON 只有 number，类型靠值范围推断：
  //   uint32 小值 -> 回到 int32（而不是 uint32）
  //   int64 小值  -> 回到 int32
  // 这不是实现缺陷，是 JSON 格式本身的表达力限制。
  const auto from_u32 = RoundTrip(DynamicValue::MakeUInt32(100));
  STRATUM_CHECK_EQ(static_cast<int>(from_u32.kind),
                   static_cast<int>(DynamicValue::Kind::kInt32));   // 变成 int32

  const auto from_i64 = RoundTrip(DynamicValue::MakeInt64(42));
  STRATUM_CHECK_EQ(static_cast<int>(from_i64.kind),
                   static_cast<int>(DynamicValue::Kind::kInt32));   // 变成 int32

  // 但**数值**必须无损
  STRATUM_CHECK_EQ(from_u32.int32_value, 100);
  STRATUM_CHECK_EQ(from_i64.int32_value, 42);
}

// ===========================================================================
// 四、错误处理：畸形输入必须被拒绝并给出位置
// ===========================================================================

STRATUM_TEST(JsonSerializer, MalformedInputIsRejectedWithPosition) {
  JsonSerializer s;
  const char* bad[] = {
      "{",                          // 未闭合对象
      "[1, 2",                      // 未闭合数组
      "{\"a\": }",                  // 缺值
      "{\"a\" 1}",                  // 缺冒号
      "{a: 1}",                     // 键未加引号
      "\"unterminated",             // 字符串未闭合
      "nul",                        // 非法字面量
      "tru",                        // 非法字面量
      "1 2",                        // 结尾多余内容
      "[1,]",                       // 尾逗号
      "{\"a\": \"\\q\"}",           // 未知转义
      "{\"a\": \"\\u12\"}",         // \u 位数不足
      "\"ctrl\x01char\"",           // 未转义控制字符
  };
  for (const char* text : bad) {
    DynamicValue v;
    Status st = s.Deserialize(text, &v);
    STRATUM_CHECK_FALSE(st.ok());
    STRATUM_CHECK_TRUE(st.IsCorruption());
    // 消息里必须带位置，否则大文件里无法定位
    STRATUM_CHECK_TRUE(st.message().find("位置") != std::string::npos);
  }
}

STRATUM_TEST(JsonSerializer, NullOutputPointerIsInvalidArgument) {
  JsonSerializer s;
  Status a = s.Serialize(DynamicValue::MakeInt32(1), nullptr);
  STRATUM_CHECK_TRUE(a.code() == StatusCode::kInvalidArgument);
  Status b = s.Deserialize("1", nullptr);
  STRATUM_CHECK_TRUE(b.code() == StatusCode::kInvalidArgument);
}

// ===========================================================================
// 五、大数据量
// ===========================================================================

STRATUM_TEST(JsonSerializer, LargeStructureRoundTrip) {
  auto root = DynamicValue::MakeArray();
  const size_t kN = 20000;
  root.elements.reserve(kN);
  for (size_t i = 0; i < kN; ++i) {
    auto row = DynamicValue::MakeStruct();
    row.fields.emplace_back("i", DynamicValue::MakeInt32(static_cast<int32_t>(i)));
    row.fields.emplace_back("v", DynamicValue::MakeDouble(static_cast<double>(i) / 3.0));
    root.elements.push_back(row);
  }
  JsonSerializer s;
  std::string bytes;
  STRATUM_CHECK_OK(s.Serialize(root, &bytes));

  DynamicValue back;
  STRATUM_CHECK_OK(s.Deserialize(bytes, &back));
  STRATUM_CHECK_EQ(back.elements.size(), kN);
  STRATUM_CHECK_EQ(back.elements[0].fields[0].second.int32_value, 0);
  STRATUM_CHECK_EQ(back.elements[kN - 1].fields[0].second.int32_value,
                   static_cast<int32_t>(kN - 1));
  std::printf("      [规模] %zu 行结构：JSON %zu 字节，往返成功\n", kN, bytes.size());
  STRATUM_CHECK_EQ(s.EstimateSize(root), bytes.size());
}

// ===========================================================================
// 六、随机（多种子）
// ===========================================================================

namespace {

DynamicValue RandomValue(std::mt19937_64* rng, int depth) {
  std::uniform_int_distribution<int> kind_d(0, depth > 0 ? 7 : 5);
  switch (kind_d(*rng)) {
    case 0: return DynamicValue::MakeBool((*rng)() % 2 == 0);
    case 1: return DynamicValue::MakeInt32(static_cast<int32_t>((*rng)() % 100000) - 50000);
    case 2: return DynamicValue::MakeUInt64((*rng)() % 10000000000ULL);
    case 3: return DynamicValue::MakeDouble(static_cast<double>((*rng)() % 100000) / 13.0);
    case 4: {
      std::string s;
      const int n = static_cast<int>((*rng)() % 10);
      for (int i = 0; i < n; ++i) s.push_back(static_cast<char>('a' + ((*rng)() % 26)));
      return DynamicValue::MakeString(s);
    }
    case 5: return DynamicValue::MakeBytes(std::string((*rng)() % 8, '\x05'));
    case 6: {
      auto a = DynamicValue::MakeArray();
      const int n = static_cast<int>((*rng)() % 4);
      for (int i = 0; i < n; ++i) a.elements.push_back(RandomValue(rng, depth - 1));
      return a;
    }
    default: {
      auto st = DynamicValue::MakeStruct();
      const int n = static_cast<int>((*rng)() % 4);
      for (int i = 0; i < n; ++i) {
        st.fields.emplace_back("k" + std::to_string(i), RandomValue(rng, depth - 1));
      }
      return st;
    }
  }
}

}  // namespace

STRATUM_TEST(JsonSerializer, RandomStructuresRoundTripMultipleSeeds) {
  JsonSerializer s;
  for (uint64_t seed : {5ULL, 55ULL, 555ULL, 20261015ULL}) {
    std::mt19937_64 rng(seed);
    const int n = static_cast<int>(rng() % 40) + 1;
    for (int i = 0; i < n; ++i) {
      const DynamicValue v = RandomValue(&rng, 3);
      std::string bytes;
      STRATUM_CHECK_OK(s.Serialize(v, &bytes));
      DynamicValue back;
      Status st = s.Deserialize(bytes, &back);
      STRATUM_CHECK_OK(st);   // 任意随机结构的输出都必须能被解析回来
      // 再序列化一次必须得到**完全相同**的文本（幂等）
      std::string bytes2;
      STRATUM_CHECK_OK(s.Serialize(back, &bytes2));
      STRATUM_CHECK_EQ(bytes2, bytes);
    }
  }
}

// ===========================================================================
// 七、真实场景
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1：传感器读数的完整链路（类型定义 -> 编解码）
// 这是"采集"最典型的形态：值要能被下游直接读懂。
// ---------------------------------------------------------------------------
STRATUM_TEST(JsonSerializerReal, SensorReadingEndToEnd) {
  TypeDescriptor d;
  d.name = "sensor/Reading";
  d.definition_format = "json";
  d.definition = "{\"temperature_c\":\"double\",\"valid\":\"bool\"}";
  SerializerPtr ser = CreateSerializer(d);
  STRATUM_CHECK_TRUE(ser->supports_serialization());

  auto reading = DynamicValue::MakeStruct();
  reading.fields.emplace_back("temperature_c", DynamicValue::MakeDouble(23.5));
  reading.fields.emplace_back("valid", DynamicValue::MakeBool(true));

  std::string bytes;
  STRATUM_CHECK_OK(ser->Serialize(reading, &bytes));
  DynamicValue back;
  STRATUM_CHECK_OK(ser->Deserialize(bytes, &back));
  STRATUM_CHECK_EQ(back.fields.size(), static_cast<size_t>(2));
  STRATUM_CHECK_TRUE(back.fields[0].second.double_value == 23.5);
  STRATUM_CHECK_TRUE(back.fields[1].second.bool_value);
  std::printf("      [场景1] 传感器读数编解码：%zu 字节，字段往返正确\n", bytes.size());
}

// ---------------------------------------------------------------------------
// 场景 2：无类型定义的真实数据（降级模式）
// 这是 v1 的常态：录到未知类型时不能中断，必须能继续录、按字节回放。
// ---------------------------------------------------------------------------
STRATUM_TEST(JsonSerializerReal, UnknownTypeStillRecords) {
  // 现场遇到一个没注册的类型
  TypeDescriptor unknown;
  unknown.name = "vendor/Proprietary";
  unknown.definition_format = "";      // 没有定义
  SerializerPtr ser = CreateSerializer(unknown);

  // 关键：工厂**没有失败**，录制链路可以继续
  STRATUM_CHECK_TRUE(ser != nullptr);
  STRATUM_CHECK_FALSE(ser->supports_serialization());
  STRATUM_CHECK_FALSE(ser->supports_deserialization());

  // 尝试解析会得到明确的可操作提示，而不是含糊失败
  DynamicValue v;
  Status st = ser->Deserialize("raw-bytes", &v);
  STRATUM_CHECK_TRUE(st.IsNotSupported());
  STRATUM_CHECK_TRUE(st.message().find("降级模式") != std::string::npos ||
                     st.message().find("字节回放") != std::string::npos);
  std::printf("      [场景2] 未知类型：工厂未失败，录制链路可继续（降级模式）\n");
}

// ---------------------------------------------------------------------------
// 场景 3：配置下发 —— 嵌套结构 + 数组，验证深层往返
// ---------------------------------------------------------------------------
STRATUM_TEST(JsonSerializerReal, NestedConfigPayload) {
  auto cfg = DynamicValue::MakeStruct();
  cfg.fields.emplace_back("name", DynamicValue::MakeString("line3"));
  cfg.fields.emplace_back("enabled", DynamicValue::MakeBool(true));
  auto limits = DynamicValue::MakeStruct();
  limits.fields.emplace_back("min", DynamicValue::MakeDouble(-1.25));
  limits.fields.emplace_back("max", DynamicValue::MakeDouble(998.75));
  cfg.fields.emplace_back("limits", limits);
  auto peers = DynamicValue::MakeArray();
  peers.elements.push_back(DynamicValue::MakeString("192.168.100.1"));
  peers.elements.push_back(DynamicValue::MakeString("192.168.100.129"));
  cfg.fields.emplace_back("peers", peers);

  const auto back = RoundTrip(cfg);
  STRATUM_CHECK_EQ(back.fields.size(), static_cast<size_t>(4));
  const auto& blimits = back.fields[2].second;
  STRATUM_CHECK_EQ(blimits.fields.size(), static_cast<size_t>(2));
  STRATUM_CHECK_TRUE(blimits.fields[0].second.double_value == -1.25);
  const auto& bpeers = back.fields[3].second;
  STRATUM_CHECK_EQ(bpeers.elements.size(), static_cast<size_t>(2));
  STRATUM_CHECK_EQ(bpeers.elements[1].string_value, std::string("192.168.100.129"));
  std::printf("      [场景3] 嵌套配置（含 struct+array）往返完全一致\n");
}

STRATUM_TEST_MAIN()
