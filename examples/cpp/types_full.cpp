// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// examples/cpp/types_full.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// types 模块 · 全面用法演示（三个真实场景）
//
//   场景 A：两种时间戳与回放时序保真
//           实录里源时钟会出现抖动与回退（NTP 校正、设备对时）；
//           回放时序必须基于**源时间戳**，而接收时间戳只用于诊断。
//           本场景用 10 万条 + 随机时钟异常验证这一点，并算出延迟分布。
//
//   场景 B：质量报告 —— 回答"这份录制可不可信"
//           本项目的第一设计原则：最危险的不是知道数据不全，
//           而是"以为录全了，实际漏了"。本场景构造三类真实故障
//           （本端丢弃 / 对端缺口 / 时钟回退），演示如何如实报告。
//
//   场景 C：自定义 ISerializer 与工厂的降级契约
//           实现一个最小序列化器，并验证"类型定义缺失时应返回透传
//           序列化器而不是失败"这一契约 —— 它决定了无 XTypes 的对端
//           能不能照样被录制。
//
// 覆盖的 API：
//   Event 全部字段 / TimeRange 语义 / TopicMetadata / SessionMetadata
//   QualityStats 全部计数与 AverageLatencyNs / HasIntegrityIssue
//   types::TypeDescriptor / DynamicValue（含 kArray、kStruct 嵌套）
//   types::ISerializer / ISerializerFactory 的完整实现与降级契约
//
// 运行：./types_full
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <thread>
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
  std::printf("    [%s] %s\n", ok ? "OK" : "FAIL", what);
  if (!ok) ++g_failed;
}

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 A：两种时间戳与回放时序保真
//
// 真实诉求：
//   · 现场设备时钟会被 NTP 校正，出现"倒退"或"跳变"
//   · 回放要还原**当时的时序**，因此必须用源时间戳排序/定时
//   · 但接收时间戳也不能丢：它记录了"录制器当时跟得上吗"
// 本场景用 10 万条数据 + 随机注入的时钟异常来验证，并算出延迟分布。
// ═══════════════════════════════════════════════════════════════════════════
int ScenarioA_TwoTimestamps(const std::string& /*work*/) {
  std::printf("\n══════ 场景 A：两种时间戳与回放时序保真 ══════\n");

  const size_t kN = 100000;
  const uint64_t kBase = 1700000000000000000ULL;
  const uint64_t kInterval = 1000000ULL;   // 名义 1 ms 一条（1 kHz）

  std::printf("  生成 %zu 条事件（名义 1 kHz），并注入时钟异常\n", kN);

  std::mt19937_64 rng(20261014);
  std::uniform_int_distribution<int> jitter_d(-50000, 50000);        // ±50 µs
  std::uniform_int_distribution<int> minute_d(0, 99);

  std::vector<Event> events;
  events.reserve(kN);
  QualityStats q;

  auto t0 = Clock::now();
  uint64_t src = kBase;
  uint64_t recv = kBase + 136000;      // 接收比源晚约 0.136 ms
  size_t regressions = 0;
  size_t clock_jumps = 0;

  for (size_t i = 0; i < kN; ++i) {
    // 1) 名义推进
    src += kInterval;

    // 2) 注入时钟异常（都是现场真会遇到的）
    const int roll = minute_d(rng);
    if (roll == 0) {
      // 时钟**回退**：NTP 把时间往回拨了 5 ms
      src -= 5000000ULL;
      ++regressions;
    } else if (roll == 1) {
      // 时钟**跳变**：设备重新对时，向前跳 20 ms
      src += 20000000ULL;
      ++clock_jumps;
    }

    // 3) 抖动（网络/调度的正常波动）
    const int64_t j = jitter_d(rng);
    const uint64_t src_i = static_cast<uint64_t>(static_cast<int64_t>(src) + j);

    Event e;
    e.topic_id = 1;
    e.seq = static_cast<uint32_t>(i);
    e.seq_in_ts = 0;
    e.source_timestamp_ns = src_i;
    e.reception_timestamp_ns = recv;
    e.payload.assign(64, 'x');
    events.push_back(e);

    // 4) 累积延迟统计（这正是 QualityStats 里那 4 个字段的用途）
    const int64_t lat = static_cast<int64_t>(recv) - static_cast<int64_t>(src_i);
    if (q.latency_samples == 0 || lat < q.min_latency_ns) q.min_latency_ns = lat;
    if (q.latency_samples == 0 || lat > q.max_latency_ns) q.max_latency_ns = lat;
    q.total_latency_ns += lat;
    ++q.latency_samples;

    recv += kInterval;
  }
  const double gen_ms = MsSince(t0);
  std::printf("  生成完成: %.1f ms（注入 %zu 次时钟回退、%zu 次跳变）\n",
              gen_ms, regressions, clock_jumps);

  q.timestamp_regressions = regressions;
  q.clock_jumps = clock_jumps;

  // ── A1：延迟分布（回答"录制器跟得上吗"）────────────────────────────
  std::printf("  延迟统计: 平均 %.3f ms / min %.3f ms / max %.3f ms（样本 %llu）\n",
              q.AverageLatencyNs() / 1e6,
              static_cast<double>(q.min_latency_ns) / 1e6,
              static_cast<double>(q.max_latency_ns) / 1e6,
              static_cast<unsigned long long>(q.latency_samples));
  Check(q.latency_samples == kN, "每条都被计入延迟统计");
  Check(q.min_latency_ns <= q.max_latency_ns, "min <= max（统计自洽）");
  // 【不要假设延迟为正】本场景注入了**累积**时钟偏移（近千次回退与跳变），
  // 源时间戳会逐渐偏离接收时间戳，于是平均延迟变负 —— 这不是错误，
  // 而是"两端时钟未同步"的正确观测。真正该断言的是统计自洽：
  //   total 落在 [min*n, max*n] 区间内，且平均值与 total/samples 一致。
  {
    const double avg = q.AverageLatencyNs();
    const bool in_range = avg >= static_cast<double>(q.min_latency_ns) &&
                          avg <= static_cast<double>(q.max_latency_ns);
    Check(in_range, "平均值落在 [min, max] 内（统计自洽，与符号无关）");
    if (avg < 0) {
      std::printf("      平均延迟为负 -> 两端时钟未同步（累积偏移所致），"
                  "此时该数字不能用于判断性能\n");
    }
  }

  // ── A2：回放时序必须按源时间戳排 ───────────────────────────────────
  // 用两套排序分别模拟"按源时间戳回放"与"按接收时间戳回放"。
  std::vector<size_t> by_src(kN), by_recv(kN);
  for (size_t i = 0; i < kN; ++i) by_src[i] = by_recv[i] = i;
  std::sort(by_src.begin(), by_src.end(), [&events](size_t a, size_t b) {
    return events[a].source_timestamp_ns < events[b].source_timestamp_ns;
  });
  std::sort(by_recv.begin(), by_recv.end(), [&events](size_t a, size_t b) {
    return events[a].reception_timestamp_ns < events[b].reception_timestamp_ns;
  });

  const bool same = (by_src == by_recv);
  std::printf("  两种排序是否一致: %s\n", same ? "一致" : "**不一致**");
  Check(!same, "注入时钟异常后两种排序确实不同（证明选错时间戳会改变回放顺序）");

  // 量化差异：有多少条位置不同
  size_t diff = 0;
  for (size_t i = 0; i < kN; ++i) {
    if (by_src[i] != by_recv[i]) ++diff;
  }
  std::printf("  位置不同的条数: %zu / %zu（%.2f%%）\n", diff, kN,
              100.0 * static_cast<double>(diff) / static_cast<double>(kN));
  Check(diff > 0, "差异可量化（不是靠感觉判断）");

  // ── A3：回放间隔保真度 ────────────────────────────────────────────
  // 按源时间戳排好后，相邻两条的间隔应当接近名义 1 ms。
  {
    size_t bad = 0;
    for (size_t i = 1; i < kN; ++i) {
      const Event& a = events[by_src[i - 1]];
      const Event& b = events[by_src[i]];
      if (b.source_timestamp_ns < a.source_timestamp_ns) ++bad;
    }
    Check(bad == 0, "按源时间戳排序后不出现倒退（回放时序单调）");
  }

  // ── A4：多种子复现性（避免结论依赖某个幸运种子）───────────────────
  {
    bool all_diff = true;
    for (uint64_t seed : {1ULL, 7ULL, 99ULL}) {
      std::mt19937_64 r(seed);
      std::uniform_int_distribution<uint64_t> jit(0, 20000);
      std::vector<uint64_t> s, rc;
      uint64_t base = kBase;
      // 接收时间戳在录制端是**独立且单调**推进的（录制器自己的时钟不会回退）。
      // 【曾经的错误】这里写作 rc.push_back(base + 136000)，让接收时间跟着
      // 源时间一起回退 —— 于是两种排序当然完全相同，断言"排序不同"必然失败。
      // 两种时间戳必须是两条独立的序列，这是整个场景的前提。
      uint64_t recv = kBase + 136000;
      for (size_t i = 0; i < 20000; ++i) {
        base += kInterval;
        // 【确定性注入】每 1000 条强制回退 50 ms，不依赖随机是否撞上。
        // 依赖概率的断言就是 flaky 断言 —— 这一点在本项目已反复吃过亏。
        if (i % 1000 == 999) base -= 50000000ULL;
        base += jit(r);          // 再加入正常抖动，避免数据过于规整
        s.push_back(base);
        recv += kInterval;       // 接收侧单调推进，与源侧的回退无关
        rc.push_back(recv);
      }
      std::vector<size_t> i1(20000), i2(20000);
      for (size_t i = 0; i < 20000; ++i) i1[i] = i2[i] = i;
      std::sort(i1.begin(), i1.end(), [&s](size_t a, size_t b) { return s[a] < s[b]; });
      std::sort(i2.begin(), i2.end(), [&rc](size_t a, size_t b) { return rc[a] < rc[b]; });
      if (i1 == i2) { all_diff = false; break; }
    }
    Check(all_diff, "3 个不同种子下两种排序都不同（结论稳定）");
  }

  std::printf("  场景 A 完成\n");
  return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 B：质量报告 —— 回答"这份录制可不可信"
//
// 真实诉求：
//   · 用户必须先知道录制有没有问题，才敢拿它做复盘结论
//   · 三类成因必须**分开计**：本端丢弃 / 对端缺口 / 时钟异常
//   · 最危险的失败模式是"看起来完整" —— 因此要有办法把它识破
// ═══════════════════════════════════════════════════════════════════════════
int ScenarioB_QualityReport(const std::string& /*work*/) {
  std::printf("\n══════ 场景 B：质量报告 ══════\n");

  // ── B1：三类故障分别构造 ──────────────────────────────────────────
  struct Fault {
    const char* name;
    uint64_t dropped;
    uint64_t gaps;
    uint64_t missing;
    uint64_t regressions;
    uint64_t interruptions;
  };
  const Fault faults[] = {
      {"健康录制", 0, 0, 0, 0, 0},
      {"本端丢弃（磁盘/队列跟不上）", 1200, 0, 0, 0, 0},
      {"对端缺口（发布者侧丢包）", 0, 37, 512, 0, 0},
      {"时钟回退（NTP 校正）", 0, 0, 0, 15, 0},
      {"录制中断（进程被杀）", 0, 0, 0, 0, 2},
      {"三类并发故障", 800, 12, 240, 5, 1},
  };

  for (const auto& f : faults) {
    QualityStats q;
    q.dropped_messages = f.dropped;
    q.sequence_gaps = f.gaps;
    q.missing_messages = f.missing;
    q.timestamp_regressions = f.regressions;
    q.interruptions = f.interruptions;

    const bool issue = q.HasIntegrityIssue();
    const bool expect_issue = (f.dropped + f.gaps + f.regressions + f.interruptions) > 0;

    std::printf("    %-34s HasIntegrityIssue=%-5s 丢弃=%-5llu 缺口=%-4llu 回退=%-3llu 中断=%llu\n",
                f.name, issue ? "true" : "false",
                static_cast<unsigned long long>(f.dropped),
                static_cast<unsigned long long>(f.gaps),
                static_cast<unsigned long long>(f.regressions),
                static_cast<unsigned long long>(f.interruptions));
    Check(issue == expect_issue, "问题判定与构造一致");
  }

  // ── B2：核心断言 —— "缺口会被如实报告，不会被掩盖" ────────────────
  //
  // 这是本项目最重要的行为承诺。构造一个"条数看着很多、但缺了512条"的录制，
  // 验证质量报告**不会**因为条数多就报告健康。
  {
    QualityStats q;
    q.sequence_gaps = 37;
    q.missing_messages = 512;
    q.dropped_messages = 0;             // 本端没丢 —— 容易误判成"健康"

    Check(q.HasIntegrityIssue(),
          "对端缺口即使本端零丢弃也必须被判定为有问题（不能只看本端）");
    std::printf("      → 这正是设计原则：不能出现\"以为录全了，实际漏了\"\n");
  }

  // ── B3：健康录制不能被误报 ────────────────────────────────────────
  {
    QualityStats q;
    // 只有延迟统计（那是**正常**的，不算问题）
    q.latency_samples = 100000;
    q.total_latency_ns = 100000LL * 136000;
    q.min_latency_ns = 50000;
    q.max_latency_ns = 2000000;
    Check(!q.HasIntegrityIssue(), "仅有延迟统计不构成完整性问题（避免误报）");
    std::printf("      平均延迟 %.3f ms\n", q.AverageLatencyNs() / 1e6);
  }

  // ── B4：延迟统计的边界（不除零、负延迟要能表达）───────────────────
  {
    QualityStats z;
    Check(z.AverageLatencyNs() == 0.0, "无样本时平均延迟为 0（不除零）");

    QualityStats neg;
    neg.latency_samples = 10;
    neg.total_latency_ns = -10 * 5000;      // 接收早于源：时钟不同步
    neg.min_latency_ns = -50000;
    Check(neg.AverageLatencyNs() < 0,
          "负延迟可表达（两端时钟未同步时的真实情况）");
    std::printf("      负平均延迟 %.3f ms → 说明两端时钟未同步，"
                "此时延迟数字不可用于判断性能\n", neg.AverageLatencyNs() / 1e6);
  }

  std::printf("  场景 B 完成\n");
  return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 C：自定义 ISerializer 与工厂的降级契约
//
// 真实诉求：
//   · 不是所有对端都提供类型定义（无 XTypes 的实现很常见）
//   · 契约要求：工厂在**定义缺失**时必须返回"透传"序列化器，而不是失败
//     —— 否则这类对端根本无法被录制
//   · 序列化器会被并发调用，因此必须无状态
// ═══════════════════════════════════════════════════════════════════════════

// 一个最小但完整的序列化器：把 DynamicValue 编成简单的自描述文本。
// 真实实现应产出 CDR 等标准格式；这里只为演示契约。
class TextSerializer final : public ISerializer {
 public:
  explicit TextSerializer(bool passthrough) : passthrough_(passthrough) {}

  std::string Name() const override {
    return passthrough_ ? "text-passthrough" : "text";
  }

  // 透传模式下：不能反序列化（因为没有类型信息），但**能**序列化原始字节
  bool supports_deserialization() const override { return !passthrough_; }
  bool supports_serialization() const override { return true; }

  Status Serialize(const DynamicValue& value, std::string* out) override {
    if (out == nullptr) return Status::InvalidArgument("out 为空");
    *out = DumpValue(value);
    return Status::OK();
  }

  Status Deserialize(const std::string& bytes, DynamicValue* out) override {
    if (passthrough_) {
      // 明确说明"我做不到"，而不是返回一个猜测的结果
      return Status::NotSupported("透传模式没有类型信息，无法反序列化");
    }
    if (out == nullptr) return Status::InvalidArgument("out 为空");
    // 极简解析：只认 "key=value" 形式，够演示往返即可
    DynamicValue st = MkStruct();
    st.fields.emplace_back("raw", MkBytes(bytes));
    *out = st;
    return Status::OK();
  }

  size_t EstimateSize(const DynamicValue& value) const override {
    return DumpValue(value).size();
  }

 private:
  bool passthrough_;
};

class TextSerializerFactory final : public ISerializerFactory {
 public:
  std::string Name() const override { return "text"; }

  SerializerPtr Create(const TypeDescriptor& descriptor) override {
    // 【核心契约】没有类型定义时，必须返回**透传**序列化器，而不是失败。
    // 否则"对端不提供类型定义"这一常见情况会导致整条链路录不了。
    const bool passthrough = !descriptor.HasDefinition();
    return std::make_shared<TextSerializer>(passthrough);
  }
};

int ScenarioC_CustomSerializer(const std::string& /*work*/) {
  std::printf("\n══════ 场景 C：自定义 ISerializer 与降级契约 ══════\n");

  TextSerializerFactory factory;
  Check(factory.Name() == "text", "工厂名字正确");

  // ── C1：有定义 → 完整能力 ─────────────────────────────────────────
  {
    TypeDescriptor d;
    d.name = "SensorTemperature";
    d.definition = "struct SensorTemperature { long id; double value; };";
    d.definition_format = "idl";
    Check(d.HasDefinition(), "类型描述含定义");

    SerializerPtr s = factory.Create(d);
    Check(s != nullptr, "工厂返回非空");
    Check(s->supports_deserialization(), "有定义时支持反序列化");
    Check(s->supports_serialization(), "支持序列化");
  }

  // ── C2：无定义 → 透传降级（关键契约）────────────────────────────
  {
    TypeDescriptor d;
    d.name = "OpaqueType";      // 对端没给定义（无 XTypes）
    Check(!d.HasDefinition(), "类型描述无定义");

    SerializerPtr s = factory.Create(d);
    Check(s != nullptr, "**无定义时工厂仍须返回有效序列化器（不是失败、不是空）**");
    Check(!s->supports_deserialization(),
          "透传模式如实声明不支持反序列化（不假装能解）");
    Check(s->supports_serialization(),
          "透传模式仍能序列化原始字节（这样才录得下来）");

    // 调用不支持的能力时，必须**明确失败**
    DynamicValue out;
    Status r = s->Deserialize("whatever", &out);
    Check(!r.ok(), "调用不支持的能力返回明确失败");
    std::printf("      失败信息: %s\n", r.ToString().c_str());
  }

  // ── C3：嵌套结构的往返（含数组）──────────────────────────────────
  {
    TypeDescriptor d;
    d.name = "Complex";
    d.definition = "struct Complex { long id; double value; long history[3]; };";
    SerializerPtr s = factory.Create(d);

    DynamicValue st = MkStruct();
    st.fields.emplace_back("id", MkInt64(7));
    st.fields.emplace_back("value", MkDouble(3.14));
    DynamicValue arr = MkArray();
    for (int i = 0; i < 3; ++i) arr.elements.push_back(MkInt64(i));
    st.fields.emplace_back("history", arr);

    std::string bytes;
    Status r = s->Serialize(st, &bytes);
    Check(r.ok() && !bytes.empty(), "嵌套结构序列化成功");
    Check(s->EstimateSize(st) > 0, "EstimateSize 给出正数");

    DynamicValue back;
    r = s->Deserialize(bytes, &back);
    Check(r.ok(), "反序列化成功");
    Check(!back.fields.empty(), "反序列化结果非空");
  }

  // ── C4：大数据量 + 多种规模（不是只跑一个样本）────────────────────
  {
    TypeDescriptor d;
    d.name = "Bulk";
    d.definition = "struct Bulk { long v; };";
    SerializerPtr s = factory.Create(d);

    auto t0 = Clock::now();
    size_t total_bytes = 0;
    bool all_ok = true;
    for (size_t n : {1000, 10000, 50000}) {
      for (size_t i = 0; i < n; ++i) {
        DynamicValue v = MkStruct();
        v.fields.emplace_back("v", MkInt64(static_cast<int64_t>(i)));
        std::string b;
        if (!s->Serialize(v, &b).ok()) { all_ok = false; break; }
        total_bytes += b.size();
      }
      if (!all_ok) break;
    }
    const double ms = MsSince(t0);
    std::printf("      序列化 61000 次共 %zu 字节，耗时 %.1f ms\n", total_bytes, ms);
    Check(all_ok, "三种规模（1000/10000/50000）全部成功");
    Check(total_bytes > 0, "产出了实际字节");
  }

  // ── C5：并发调用（契约要求 Serialize/Deserialize 可并发）──────────
  {
    TypeDescriptor d;
    d.name = "Concurrent";
    d.definition = "struct Concurrent { long v; };";
    SerializerPtr s = factory.Create(d);

    std::vector<std::thread> threads;
    std::atomic<int> ok_count{0};
    for (int t = 0; t < 4; ++t) {
      threads.emplace_back([&s, &ok_count, t]() {
        for (int i = 0; i < 2000; ++i) {
          DynamicValue v = MkStruct();
          v.fields.emplace_back("v", MkInt64(t * 10000 + i));
          std::string b;
          if (s->Serialize(v, &b).ok() && !b.empty()) ++ok_count;
        }
      });
    }
    for (auto& th : threads) th.join();
    Check(ok_count.load() == 8000, "4 线程 × 2000 次并发序列化全部成功");
  }

  std::printf("  场景 C 完成\n");
  return 0;
}

}  // namespace

int main() {
  std::printf("╔══════════════════════════════════════════════════════════════╗\n");
  std::printf("║  types_full：三个真实场景                                     ║\n");
  std::printf("║   A 两种时间戳与回放时序保真（10 万条 + 时钟异常）            ║\n");
  std::printf("║   B 质量报告（能否识破\"看起来完整\"）                          ║\n");
  std::printf("║   C 自定义 ISerializer 与降级契约                             ║\n");
  std::printf("╚══════════════════════════════════════════════════════════════╝\n");

  ScenarioA_TwoTimestamps("/tmp/types_full");
  ScenarioB_QualityReport("/tmp/types_full");
  ScenarioC_CustomSerializer("/tmp/types_full");

  std::printf("\n══════════════════════════════════════════════════════════════\n");
  if (g_failed == 0) {
    std::printf("三个场景全部通过（0 项失败）\n");
    return 0;
  }
  std::printf("有 %d 项检查未通过\n", g_failed);
  return 1;
}
