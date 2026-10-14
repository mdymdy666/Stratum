// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// examples/cpp/types_demo.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// types 模块 · 简单用法演示
//
// types 是 Stratum 的"数据模型"：所有模块交换的都是这些结构。
// 它们看起来朴素，但每个字段都有明确职责，误用会在很后面才暴露症状
// （典型：漏填 reception_timestamp_ns，事后无法判断录制延迟）。
//
// 本示例逐个演示：
//   ① Event          一条记录的全部字段（含**两种时间戳**的用途）
//   ② TimeRange      时间区间的半开语义
//   ③ TopicMetadata  话题元数据（必须自描述）
//   ④ SessionMetadata 会话元数据 + QualityStats
//   ⑤ QualityStats   质量统计（"这份录制可不可信"的唯一凭据）
//   ⑥ RetentionPolicy / SplitPolicy / RecordConfig  三类配置
//   ⑦ DynamicValue   自描述的动态值树 + ToString
//
// 运行：./types_demo
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>
#include <string>
#include <vector>

#include "stratum/core/itimeline.h"
#include "stratum/core/types.h"
#include "stratum/types/iserializer.h"

using namespace stratum;
using namespace stratum::types;

namespace {

// DynamicValue 的递归打印。
//
// 【为什么要自己写】iserializer.h 声明了 DynamicValue::ToString(int) const，
// 但当前代码库没有它的实现（链接时 undefined reference）。这与上面那几个
// MakeXxx 工厂是同一情况：接口已定、实现未落。示例自带一个等价实现，
// 既演示了 DynamicValue 的树形结构，也不依赖尚未落地的符号。
std::string DumpValue(const DynamicValue& v, int indent = 0) {
  const std::string pad(static_cast<size_t>(indent) * 2, ' ');
  switch (v.kind) {
    case DynamicValue::Kind::kNull:   return "null";
    case DynamicValue::Kind::kBool:   return v.bool_value ? "true" : "false";
    case DynamicValue::Kind::kInt32:  return std::to_string(v.int32_value);
    case DynamicValue::Kind::kInt64:  return std::to_string(v.int64_value);
    case DynamicValue::Kind::kUInt32: return std::to_string(v.uint32_value);
    case DynamicValue::Kind::kUInt64: return std::to_string(v.uint64_value);
    case DynamicValue::Kind::kDouble: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%g", v.double_value);
      return buf;
    }
    case DynamicValue::Kind::kString: return "\"" + v.string_value + "\"";
    case DynamicValue::Kind::kBytes:  return "<" + std::to_string(v.string_value.size()) + " bytes>";
    case DynamicValue::Kind::kArray: {
      std::string s = "[\n";
      for (const auto& e : v.elements) {
        s += pad + "  " + DumpValue(e, indent + 1) + ",\n";
      }
      return s + pad + "]";
    }
    case DynamicValue::Kind::kStruct: {
      std::string s = "{\n";
      for (const auto& kv : v.fields) {
        s += pad + "  " + kv.first + " = " + DumpValue(kv.second, indent + 1) + "\n";
      }
      return s + pad + "}";
    }
  }
  return "?";
}

// ---------------------------------------------------------------------------
// DynamicValue 的构造助手
//
// 【为什么要这几个助手】iserializer.h 里声明了 DynamicValue::MakeInt64 /
// MakeString / MakeDouble / MakeBytes / MakeArray / MakeStruct 这些静态工厂，
// 但**当前代码库里没有它们的实现**（用 nm 在各静态库中都找不到符号），
// 因此直接调用会链接失败（undefined reference）。
//
// 这属于"接口已定、实现未落"的中间状态。示例在这里用直接构造代替，
// 既能演示 DynamicValue 的真实数据结构，也不会因为未实现的工厂而无法编译。
// 等工厂实现后，把 MkXxx 换成 MakeXxx 即可，语义完全一致。
// ---------------------------------------------------------------------------
DynamicValue MkStruct() {
  DynamicValue d;
  d.kind = DynamicValue::Kind::kStruct;
  return d;
}
DynamicValue MkArray() {
  DynamicValue d;
  d.kind = DynamicValue::Kind::kArray;
  return d;
}
DynamicValue MkInt64(int64_t v) {
  DynamicValue d;
  d.kind = DynamicValue::Kind::kInt64;
  d.int64_value = v;
  return d;
}
DynamicValue MkDouble(double v) {
  DynamicValue d;
  d.kind = DynamicValue::Kind::kDouble;
  d.double_value = v;
  return d;
}
DynamicValue MkString(std::string s) {
  DynamicValue d;
  d.kind = DynamicValue::Kind::kString;
  d.string_value = std::move(s);
  return d;
}
DynamicValue MkBytes(std::string b) {
  DynamicValue d;
  d.kind = DynamicValue::Kind::kBytes;
  d.string_value = std::move(b);
  return d;
}

int g_failed = 0;

void Check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "OK" : "FAIL", what);
  if (!ok) ++g_failed;
}

}  // namespace

int main() {
  std::printf("=== types_demo：Stratum 的数据模型 ===\n\n");

  // ═══════════════════════════════════════════════════════════════════════
  // ① Event：一条记录
  //
  // 两种时间戳都要保留，这不是冗余：
  //   source_timestamp_ns    —— 数据**发布者**的时钟，回放时序以它为准
  //   reception_timestamp_ns —— **录制器**收到的本地时刻，用于诊断录制延迟
  // 二者的差值就是"这条数据在路上花了多久"，是排查现场问题的第一手信息。
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("[1] Event\n");
  {
    Event e;
    e.topic_id = 3;
    e.seq = 100;                 // 同一话题内单调递增（Recorder 分配）
    e.seq_in_ts = 0;             // 同一时间戳内的去重序号
    e.source_timestamp_ns = 1700000000000000000ULL;
    e.reception_timestamp_ns = 1700000000000500000ULL;   // 晚了 0.5 ms
    e.payload = std::string("\x01\x02\x03\x04", 4);      // 原始字节，存储层不解释
    e.instance_key = "";         // 非 keyed 话题留空

    Check(e.topic_id == 3 && e.seq == 100, "基本字段可设置");
    Check(e.reception_timestamp_ns > e.source_timestamp_ns,
          "两种时间戳都在（可用于诊断端到端延迟）");
    const double latency_ms =
        static_cast<double>(e.reception_timestamp_ns - e.source_timestamp_ns) / 1e6;
    std::printf("      本条端到端延迟 = %.3f ms\n", latency_ms);
    Check(e.payload.size() == 4, "payload 是原始字节，允许含 \\0");
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ② TimeRange：半开语义
  //
  // [begin_ns, end_ns)，end_ns == 0 表示"无上界"。
  // 半开区间的好处是相邻区间可以直接拼： [a,b) + [b,c) == [a,c)
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[2] TimeRange\n");
  {
    TimeRange r;
    Check(r.begin_ns == kInvalidTimestamp && r.end_ns == kInvalidTimestamp,
          "默认构造是 {0,0}，表示全范围");
    Check(!r.Empty(), "{0,0} 不算空区间（表示无界）");

    TimeRange a{100, 200};
    TimeRange b{200, 300};
    Check(!a.Empty(), "[100,200) 非空");
    Check(a.end_ns == b.begin_ns, "相邻区间首尾相接（半开的直接好处）");

    TimeRange bad{300, 100};
    Check(bad.Empty(), "begin >= end 被判为空区间");
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ③ TopicMetadata：必须自描述
  //
  // 设计原则：录制产物**脱离原程序也能读懂**。
  // 所以类型名与类型定义是显式落盘的，而不是靠"加载时再问发布者"。
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[3] TopicMetadata\n");
  {
    TopicMetadata t;
    t.topic_id = 1;                       // 会话内稳定编号，键里存它（省空间）
    t.topic_name = "sensor/temperature";
    t.type_name = "SensorTemperature";
    Check(t.topic_id == 1 && !t.topic_name.empty(), "话题元数据可设置");
    std::printf("      topic_id=%u name=%s type=%s\n", t.topic_id,
                t.topic_name.c_str(), t.type_name.c_str());
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ④ SessionMetadata：会话元数据
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[4] SessionMetadata\n");
  {
    SessionMetadata s;
    s.session_name = "line-3";
    s.stratum_version = "1.0.0";
    s.created_at_ns = 1700000000000000000ULL;
    s.total_events = 1234;
    s.total_bytes = 567890;
    Check(s.format_version == 1, "默认格式版本为 1（读取方遇到更高版本必须拒绝）");
    Check(s.topics.empty(), "话题列表初始为空");
    s.topics.push_back(TopicMetadata{});
    Check(s.topics.size() == 1, "可以登记话题");
    std::printf("      格式版本=%u 事件=%llu 字节=%llu\n", s.format_version,
                static_cast<unsigned long long>(s.total_events),
                static_cast<unsigned long long>(s.total_bytes));
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ⑤ QualityStats：数据可信度的唯一凭据
  //
  // 设计原则："最危险的不是知道数据不全，而是以为录全了实际漏了"。
  // 因此每一类缺失都必须**单独计数**，并且有 HasProblems() 供上层一眼判断。
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[5] QualityStats\n");
  {
    QualityStats q;
    // 判据方法名是 HasIntegrityIssue（四类问题：丢弃/时间戳倒退/序号缺口/中断）
    Check(!q.HasIntegrityIssue(), "全新统计没有完整性问题");

    q.dropped_messages = 5;                 // 录制器**本端**丢的
    q.dropped_bytes = 640;
    q.first_drop_ns = 1700000000000000000ULL;
    q.last_drop_ns = 1700000000001000000ULL;
    q.timestamp_regressions = 2;            // 源时间戳倒退（发布者时钟问题）
    q.clock_jumps = 1;                      // 时钟跳变
    q.sequence_gaps = 1;                    // 序号缺口（**对端**发布者侧丢失）
    q.missing_messages = 3;
    q.interruptions = 1;                    // 录制中断
    Check(q.HasIntegrityIssue(), "有任何一类问题时 HasIntegrityIssue 为真");

    std::printf("      丢弃=%llu 时间戳倒退=%llu 时钟跳变=%llu\n",
                static_cast<unsigned long long>(q.dropped_messages),
                static_cast<unsigned long long>(q.timestamp_regressions),
                static_cast<unsigned long long>(q.clock_jumps));
    std::printf("      序号缺口=%llu（缺 %llu 条）中断=%llu\n",
                static_cast<unsigned long long>(q.sequence_gaps),
                static_cast<unsigned long long>(q.missing_messages),
                static_cast<unsigned long long>(q.interruptions));
    std::printf("      注意：丢弃是本端成因、缺口是对端成因，必须分开计\n");

    // 延迟统计：源时间戳与接收时间戳之差，回答"录制器跟得上吗"
    q.latency_samples = 100;
    q.total_latency_ns = 100 * 136000;      // 平均 0.136 ms
    q.min_latency_ns = 80000;
    q.max_latency_ns = 429000;
    std::printf("      平均延迟 = %.3f ms（min %.3f / max %.3f ms）\n",
                q.AverageLatencyNs() / 1e6,
                static_cast<double>(q.min_latency_ns) / 1e6,
                static_cast<double>(q.max_latency_ns) / 1e6);
    Check(q.AverageLatencyNs() > 0, "AverageLatencyNs 由 total/samples 得出");

    QualityStats empty;
    Check(empty.AverageLatencyNs() == 0.0,
          "没有样本时 AverageLatencyNs 返回 0（不除零）");
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ⑥ 三类配置：保留 / 分片 / 录制
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[6] RetentionPolicy / SplitPolicy / RecordConfig\n");
  {
    RetentionPolicy rp;
    Check(rp.retain_duration_ns == 0 && rp.max_bytes == 0,
          "保留策略默认不限（0 表示不限制）");
    rp.retain_duration_ns = 24ULL * 3600 * 1000000000ULL;   // 保留 24 小时
    Check(rp.check_interval_ns == 1000000000ULL, "检查间隔默认 1 秒");

    SplitPolicy sp;
    Check(sp.max_layer_bytes == 256ULL * 1024 * 1024, "分片阈值默认 256MB");

    RecordConfig rc;
    Check(rc.topics.empty(), "话题列表为空 = 自动发现全部");
    Check(rc.overflow_policy == RecordConfig::OverflowPolicy::kDropNewest,
          "缓冲溢出默认丢最新（保证业务不被拖慢）");
    Check(rc.durability == RecordConfig::DurabilityLevel::kBalanced,
          "落盘默认折中（批量同步）");
    rc.topics.push_back("sensor/*");        // 支持以 * 结尾的前缀通配
    std::printf("      通配示例: %s\n", rc.topics.front().c_str());
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ⑦ DynamicValue：自描述的动态值树
  //
  // 它是"树"而不是"变体数组"，便于自描述与调试输出。
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[7] DynamicValue\n");
  {
    DynamicValue st = MkStruct();
    st.fields.emplace_back("id", MkInt64(42));
    st.fields.emplace_back("name", MkString("sensor-7"));
    st.fields.emplace_back("value", MkDouble(23.5));

    DynamicValue arr = MkArray();
    arr.elements.push_back(MkInt64(1));
    arr.elements.push_back(MkInt64(2));
    st.fields.emplace_back("history", arr);

    Check(st.kind == DynamicValue::Kind::kStruct, "结构体构造正确");
    Check(st.fields.size() == 4, "字段数正确");
    Check(arr.kind == DynamicValue::Kind::kArray && arr.elements.size() == 2,
          "数组构造正确");

    const std::string s = DumpValue(st);
    Check(!s.empty(), "ToString 产出非空");
    std::printf("------ ToString 输出 ------\n%s\n---------------------------\n",
                s.c_str());
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ⑧ TypeDescriptor：类型描述与"降级模式"
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[8] TypeDescriptor\n");
  {
    TypeDescriptor d;
    d.name = "SensorTemperature";
    Check(!d.HasDefinition(), "没有定义时 HasDefinition 为假（降级模式）");
    d.definition = "struct SensorTemperature { long id; double value; };";
    d.definition_format = "idl";
    Check(d.HasDefinition(), "有定义时为真");
    std::printf("      name=%s format=%s\n", d.name.c_str(),
                d.definition_format.c_str());
    std::printf("      契约：工厂在无定义时应返回**透传**序列化器，而不是失败\n");
  }

  std::printf("\n");
  if (g_failed == 0) {
    std::printf("全部通过。下一步看 types_full.cpp —— 三个真实场景。\n");
    return 0;
  }
  std::printf("有 %d 项未通过\n", g_failed);
  return 1;
}
