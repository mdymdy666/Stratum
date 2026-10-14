// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// examples/cpp/core_full.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// core 模块 · 全面用法演示（三个真实场景）
//
// 与 core_demo.cpp 相比，本文件演示**实际工程里怎么用**：
//
//   场景 A：产线传感器长时间录制
//           —— 录制 + 实时监控 + 容量控制 + 环形保留 + 优雅停止
//   场景 B：故障复盘
//           —— 按时间段查询 + 定点状态重建 + 区间导出 + 完整性核对
//   场景 C：回归测试数据回放
//           —— 区间裁剪 + 变速 + 话题重映射 + 进度监控 + 中途停止
//
// 覆盖的 API（比 demo 多了这些）：
//   ITimeline::CreateRetentionPoint / ReleaseRetentionPoint   环形保留控制点
//   Query::FindTopic / ExportRange / GetStateAt               精确检索
//   Player::RequestStop / IsPlaying                           协作式停止
//   PlayerConfig::topic_mapping / rate / loop / discovery_wait_ms
//   RecorderConfig::record.retention / overflow_policy / durability
//   Status 的各失败分支（NotFound / InvalidArgument …）
//
// 运行：./core_full [工作目录]（默认 /tmp/core_full_tl）
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "common/memory_backend.h"
#include "stratum/core/recorder.h"
#include "stratum/core/timeline_impl_v2.h"

using namespace stratum;
using stratum::examples::MemoryBackend;
using stratum::examples::NowNs;

namespace {

// 确保目录存在。
//
// 【为什么要这一步】TimelineImplV2::Open 只在**已存在的目录**里建立时间线，
// 不会递归创建父目录。传 "/tmp/base/line_a" 这种多级路径时若 base 不存在，
// 会以 "LSMengine open failed: NotFound: .../LOCK: No such file or directory"
// 失败 —— 错误信息指向 LOCK 文件，很容易误判成权限或存储损坏。
bool EnsureDir(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec && !std::filesystem::exists(dir)) {
    std::printf("  创建目录失败: %s (%s)\n", dir.c_str(), ec.message().c_str());
    return false;
  }
  return true;
}

// 清空并重建目录。
//
// 【为什么不是 EnsureDir】示例必须**可重复运行**。若只确保目录存在，
// 第二次运行时目录里还留着上一次的数据，TimelineImplV2::Open 会打开它，
// 于是"录制了多少条""窗口里有多少条"都跟首次不同，检查项就会误报失败 ——
// 读者照着跑第二遍会以为是自己环境的问题。
// （这也是本项目反复出现的主题：验证手段本身不可靠时，结论就不可信。）
bool ResetDir(const std::string& dir) {
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);   // 目录不存在时也返回成功
  std::filesystem::create_directories(dir, ec);
  if (ec && !std::filesystem::exists(dir)) {
    std::printf("  重建目录失败: %s (%s)\n", dir.c_str(), ec.message().c_str());
    return false;
  }
  return true;
}

int g_failed = 0;

void Check(bool ok, const char* what) {
  std::printf("    [%s] %s\n", ok ? "OK" : "FAIL", what);
  if (!ok) ++g_failed;
}

// 把纳秒时间戳转成"相对起点"的秒，便于打印
double Rel(TimestampNs base, TimestampNs t) {
  if (t <= base) return 0.0;
  return static_cast<double>(t - base) / 1e9;
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 A：产线传感器长时间录制
//
// 真实诉求：
//   · 传感器 500 Hz，要连续录，不能拖慢业务
//   · 磁盘有限，必须能控制容量、淘汰旧数据
//   · 进程要能被 SIGTERM 优雅停掉（此处用 Stop() 模拟）
//   · 运维需要随时知道"录了多少、丢了多少、队列积压多少"
// ═══════════════════════════════════════════════════════════════════════════
int ScenarioA_ProductionRecording(const std::string& base_dir) {
  std::printf("\n══════ 场景 A：产线传感器长时间录制 ══════\n");
  const std::string dir = base_dir + "/line_a";

  if (!ResetDir(dir)) { ++g_failed; return 1; }
  TimelineImplV2 timeline;
  Status s = timeline.Open(dir);
  if (!s.ok()) {
    std::printf("  Open 失败: %s\n", s.ToString().c_str());
    ++g_failed;                       // 【必须计数】否则会误报"全部通过"
    return 1;
  }

  MemoryBackend backend;
  Recorder recorder(&timeline, &backend);

  RecorderConfig rcfg;
  rcfg.close_timeline_on_stop = false;

  // ── 容量控制：磁盘有限，必须给出明确策略 ────────────────────────────
  // overflow_policy 决定"缓冲满时怎么办"：
  //   kDropNewest（默认）—— 丢新数据，保证业务不被拖慢
  //   kDropOldest        —— 丢旧数据，保证"最近的数据"完整
  //   kBlock             —— 阻塞等待，保证不丢但可能影响业务
  // 产线场景通常选 kDropNewest，并把 dropped 计数接到告警上。
  rcfg.record.overflow_policy = RecordConfig::OverflowPolicy::kDropNewest;
  rcfg.record.max_buffer_bytes = 8ULL * 1024 * 1024;

  // 落盘持久性：kRelaxed 异步最快、kStrict 每条同步最慢、kBalanced 折中
  rcfg.record.durability = RecordConfig::DurabilityLevel::kBalanced;

  // 只录关心的前缀（以 '*' 结尾为前缀通配）；留空 = 全部发现到的话题
  rcfg.record.topics = {"line/*"};

  rcfg.backend.extra.push_back({"count", "2000"});
  rcfg.backend.extra.push_back({"interval_us", "500"});   // ≈2 kHz 合成速率
  rcfg.backend.extra.push_back({"topic", "line/temperature"});
  rcfg.backend.extra.push_back({"payload_bytes", "64"});

  s = recorder.Start(rcfg);
  if (!s.ok()) {
    std::printf("  Start 失败: %s\n", s.ToString().c_str());
    timeline.Close();
    return 1;
  }

  // ── 运维监控：周期采样统计，模拟"录 2 秒，每秒看一眼" ──────────────
  std::printf("  录制中，每 500ms 采样一次统计：\n");
  for (int i = 0; i < 6; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const Recorder::Stats st = recorder.GetStats();
    std::printf("    t=%4dms received=%-6llu recorded=%-6llu dropped=%-4llu queue=%llu\n",
                (i + 1) * 500,
                static_cast<unsigned long long>(st.received),
                static_cast<unsigned long long>(st.recorded),
                static_cast<unsigned long long>(st.dropped),
                static_cast<unsigned long long>(st.queue_depth));
    // 真实系统在这里判断：queue_depth 持续高位 → 磁盘跟不上 → 告警
    if (st.queue_depth > 10000) {
      std::printf("    ⚠ 队列积压，应触发告警\n");
    }
  }

  // ── 优雅停止：先停数据源，再等落盘完成 ─────────────────────────────
  s = recorder.Stop();
  Check(s.ok(), "Recorder::Stop 成功返回");

  const Recorder::Stats fin = recorder.GetStats();
  std::printf("  最终: received=%llu recorded=%llu dropped=%llu bytes=%llu\n",
              static_cast<unsigned long long>(fin.received),
              static_cast<unsigned long long>(fin.recorded),
              static_cast<unsigned long long>(fin.dropped),
              static_cast<unsigned long long>(fin.bytes_recorded));
  Check(fin.recorded > 0, "至少录到了一条数据");
  Check(fin.recorded <= fin.received, "recorded 不应超过 received");

  // ── 环形保留：标记一个保留点，之后可以回收更早的数据 ────────────────
  // 真实用途："只保留最近 24 小时" —— 定期创建保留点并释放旧的。
  uint64_t point_id = 0;
  s = timeline.CreateRetentionPoint(NowNs(), &point_id);
  if (s.ok()) {
    Check(true, "CreateRetentionPoint 成功");
    Status r = timeline.ReleaseRetentionPoint(point_id);
    Check(r.ok(), "ReleaseRetentionPoint 成功");
  } else {
    // 不是所有实现都支持保留点；不支持时应能优雅降级而不是崩
    std::printf("    [SKIP] 本实现不支持保留点: %s\n", s.ToString().c_str());
  }

  // ── 关闭前用 Query 自查一次，确认落盘内容可用 ───────────────────────
  Query q(&timeline);
  std::vector<TopicSummary> topics;
  q.ListTopics(&topics);
  Check(!topics.empty(), "关闭前能列出话题");

  timeline.Close();
  std::printf("  场景 A 完成\n");
  return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 B：故障复盘
//
// 真实诉求：
//   · 现场设备在某个时刻异常，事后要回答"当时数据是什么"
//   · 要能按时间段把数据导出来交给分析同学
//   · 要能核对"这段录制完不完整"（有没有缺口）
// ═══════════════════════════════════════════════════════════════════════════
int ScenarioB_IncidentReview(const std::string& base_dir) {
  std::printf("\n══════ 场景 B：故障复盘 ══════\n");
  const std::string dir = base_dir + "/incident_b";

  if (!ResetDir(dir)) { ++g_failed; return 1; }
  TimelineImplV2 timeline;
  if (!timeline.Open(dir).ok()) {
    std::printf("  Open 失败: %s\n", dir.c_str());
    ++g_failed;
    return 1;
  }

  // 先造一段"现场数据"
  {
    MemoryBackend backend;
    Recorder recorder(&timeline, &backend);
    RecorderConfig rcfg;
    rcfg.close_timeline_on_stop = false;
    rcfg.backend.extra.push_back({"count", "400"});
    rcfg.backend.extra.push_back({"interval_us", "1500"});
    rcfg.backend.extra.push_back({"topic", "device/vibration"});
    rcfg.backend.extra.push_back({"payload_bytes", "96"});
    if (!recorder.Start(rcfg).ok()) {
      std::printf("  Start 失败\n");
      timeline.Close();
      return 1;
    }
    for (int i = 0; i < 200; ++i) {
      if (recorder.GetStats().received >= 400) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    recorder.Stop();
    std::printf("  已录制 %llu 条现场数据\n",
                static_cast<unsigned long long>(recorder.GetStats().recorded));
  }

  Query query(&timeline);

  // ── 1) 先摸清有什么 ────────────────────────────────────────────────
  std::vector<TopicSummary> topics;
  Status s = query.ListTopics(&topics);
  Check(s.ok() && !topics.empty(), "ListTopics 拿到话题");
  if (topics.empty()) {
    timeline.Close();
    return 1;
  }

  const TopicSummary& t0 = topics.front();
  const TimestampNs begin = t0.first_ns;
  const TimestampNs end = t0.last_ns;
  const TimestampNs span = (end > begin) ? (end - begin) : 1;
  std::printf("  话题 %s：%llu 条，跨度 %.3f s\n", t0.topic_name.c_str(),
              static_cast<unsigned long long>(t0.message_count), Rel(begin, end));

  // ── 2) 按名字精确定位（录制目录里名字稳定，编号不稳定）────────────
  TopicMetadata tmeta;
  s = query.FindTopic(t0.topic_name, &tmeta);
  Check(s.ok(), "FindTopic 成功");
  if (!s.ok()) {
    timeline.Close();
    return 1;
  }

  // 找一个不存在的话题，验证失败分支是"明确失败"而不是崩溃
  TopicMetadata missing;
  Status nf = query.FindTopic("no/such/topic", &missing);
  Check(!nf.ok(), "FindTopic 对不存在的话题明确失败");
  std::printf("    失败信息: %s\n", nf.ToString().c_str());

  // ── 3) 按时间段导出：模拟"取异常前后 30% 的窗口" ────────────────────
  TimeRange window;
  window.begin_ns = begin + span * 3 / 10;
  window.end_ns = begin + span * 6 / 10;

  std::vector<Event> events;
  s = query.ExportRange(tmeta.topic_id, window, &events);
  Check(s.ok(), "ExportRange 成功");
  std::printf("    窗口 [%.3f, %.3f] s 导出 %zu 条\n",
              Rel(begin, window.begin_ns), Rel(begin, window.end_ns), events.size());
  Check(!events.empty(), "导出窗口非空");

  // 空区间应当返回空而不是错误（边界语义）
  TimeRange empty_range;
  empty_range.begin_ns = end + 1000000000ULL;
  empty_range.end_ns = end + 2000000000ULL;
  std::vector<Event> none;
  Status es = query.ExportRange(tmeta.topic_id, empty_range, &none);
  Check(es.ok() && none.empty(), "超出范围的区间返回空且不报错");

  // ── 4) 定点状态重建：回答"事故时刻的状态是什么" ────────────────────
  // 这是 DDS 本身没有的能力（DDS 只有"现在"），也是本项目的核心价值。
  const TimestampNs incident_at = begin + span / 2;
  StateAtResult state;
  s = query.GetStateAt(tmeta.topic_id, incident_at, &state);
  Check(s.ok(), "GetStateAt 成功");
  if (s.ok()) {
    const char* kind_name = "?";
    switch (state.kind) {
      case StateAtResult::Kind::kFound: kind_name = "kFound"; break;
      case StateAtResult::Kind::kNotFound: kind_name = "kNotFound"; break;
      case StateAtResult::Kind::kDisposed: kind_name = "kDisposed"; break;
      case StateAtResult::Kind::kOutOfRange: kind_name = "kOutOfRange"; break;
    }
    std::printf("    事故时刻 %.3f s 的状态: %s（命中时刻 %.3f s）\n",
                Rel(begin, incident_at), kind_name, Rel(begin, state.as_of_ns));
    Check(state.kind != StateAtResult::Kind::kOutOfRange,
          "事故时刻落在录制范围内");
  }

  // ── 5) 完整性核对：条数是否对得上 ──────────────────────────────────
  uint64_t counted = 0;
  s = timeline.CountRange(tmeta.topic_id, TimeRange{}, &counted);
  if (s.ok()) {
    std::printf("    全量条数 = %llu，摘要条数 = %llu\n",
                static_cast<unsigned long long>(counted),
                static_cast<unsigned long long>(t0.message_count));
    Check(counted == t0.message_count, "全量计数与摘要一致（无遗漏）");
  } else {
    std::printf("    [SKIP] CountRange 不可用: %s\n", s.ToString().c_str());
  }

  timeline.Close();
  std::printf("  场景 B 完成\n");
  return 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// 场景 C：回归测试数据回放
//
// 真实诉求：
//   · CI 里用真实录制的数据回放，替代造假数据
//   · 只回放关心的片段、可以加速、可以改名避免污染实时流
//   · 要能中途取消（CI 超时）
//   · 要能拿到"发了多少"作为断言依据
// ═══════════════════════════════════════════════════════════════════════════
int ScenarioC_RegressionReplay(const std::string& base_dir) {
  std::printf("\n══════ 场景 C：回归测试数据回放 ══════\n");
  const std::string dir = base_dir + "/regression_c";

  if (!ResetDir(dir)) { ++g_failed; return 1; }
  TimelineImplV2 timeline;
  if (!timeline.Open(dir).ok()) {
    std::printf("  Open 失败: %s\n", dir.c_str());
    ++g_failed;
    return 1;
  }

  // 造数据（两个话题，模拟"多路信号"）
  {
    MemoryBackend backend;
    Recorder recorder(&timeline, &backend);
    RecorderConfig rcfg;
    rcfg.close_timeline_on_stop = false;
    rcfg.backend.extra.push_back({"count", "300"});
    rcfg.backend.extra.push_back({"interval_us", "1000"});
    rcfg.backend.extra.push_back({"topic", "regression/input"});
    rcfg.backend.extra.push_back({"payload_bytes", "128"});
    recorder.Start(rcfg);
    for (int i = 0; i < 150; ++i) {
      if (recorder.GetStats().received >= 300) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    recorder.Stop();
  }

  Query query(&timeline);
  std::vector<TopicSummary> topics;
  query.ListTopics(&topics);
  if (topics.empty()) {
    std::printf("  无数据\n");
    timeline.Close();
    return 1;
  }
  const TopicSummary& t0 = topics.front();
  const TimestampNs span = (t0.last_ns > t0.first_ns) ? (t0.last_ns - t0.first_ns) : 1;

  MemoryBackend backend;
  Player player(&timeline, &backend);

  PlayerConfig pcfg;
  pcfg.rate = 0.0;              // CI 里通常尽快跑完；要复现时序才用 1.0
  pcfg.discovery_wait_ms = 0;
  pcfg.wait_for_ack = false;
  pcfg.republish_unacked = false;

  // 话题重映射：回放时改名，避免与仍在运行的实时发布者冲突
  pcfg.topic_mapping = {{t0.topic_name, t0.topic_name + "_replay"}};

  uint64_t observed = 0;
  uint64_t payload_seen = 0;
  pcfg.publish_callback = [&observed, &payload_seen](
                              const protocol::ReceivedSample& sample) {
    ++observed;
    payload_seen += sample.payload.size();
  };

  // ── 区间裁剪：只回放中间 50%（CI 里用于缩短耗时）──────────────────
  TimeRange range;
  range.begin_ns = t0.first_ns + span / 4;
  range.end_ns = t0.first_ns + span * 3 / 4;
  std::printf("  回放区间 %.3f .. %.3f s（原始跨度 %.3f s）\n",
              Rel(t0.first_ns, range.begin_ns), Rel(t0.first_ns, range.end_ns),
              Rel(t0.first_ns, t0.last_ns));

  // ── 协作式停止：起一个线程在 200ms 后请求停止，验证可取消 ───────────
  std::thread canceller([&player]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (player.IsPlaying()) {
      std::printf("    [canceller] 请求停止（模拟 CI 超时取消）\n");
      player.RequestStop();
    }
  });

  Status s = player.Play(range, pcfg);
  canceller.join();

  Check(s.ok(), "Play 正常返回（取消不算失败）");
  const PlaybackProgress p = player.GetProgress();
  std::printf("    回调收到 %llu 条 / %llu 字节；进度 %.1f%%\n",
              static_cast<unsigned long long>(observed),
              static_cast<unsigned long long>(payload_seen), p.Percent());
  Check(observed > 0, "至少回放出一条（CI 的断言依据）");

  // ── 校验重映射确实生效（在 sink 侧看话题名）──────────────────────
  if (auto sink = backend.last_sink()) {
    auto got = sink->samples();
    bool renamed = !got.empty() && got.front().topic_name == t0.topic_name + "_replay";
    Check(renamed, "topic_mapping 生效（sink 收到的是改名后的话题）");
  }

  timeline.Close();
  std::printf("  场景 C 完成\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string base = (argc > 1) ? argv[1] : "/tmp/core_full_tl";

  std::printf("╔══════════════════════════════════════════════════════════════╗\n");
  std::printf("║  core_full：三个真实场景（产线录制 / 故障复盘 / 回归回放）   ║\n");
  std::printf("╚══════════════════════════════════════════════════════════════╝\n");
  std::printf("工作目录: %s\n", base.c_str());

  ScenarioA_ProductionRecording(base);
  ScenarioB_IncidentReview(base);
  ScenarioC_RegressionReplay(base);

  std::printf("\n══════════════════════════════════════════════════════════════\n");
  if (g_failed == 0) {
    std::printf("三个场景全部通过（0 项失败）\n");
    return 0;
  }
  std::printf("有 %d 项检查未通过\n", g_failed);
  return 1;
}
