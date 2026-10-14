// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// protocol 模块测试：IDdsBackend / ISource / ISink / BackendConfig / ReceivedSample
//
// ===========================================================================
// 覆盖审计的结论与本文档的目标
// ===========================================================================
//
// 审计发现 protocol 模块已有 5 个测试文件，但：
//   · 随机测试 0 个
//   · 真实场景仅 1 个（不足 3）
// 本文件补齐这三类（大数据量 / 随机 / 真实场景 ×3）。
//
// ===========================================================================
// 为什么全程用测试替身而不接真实网络
// ===========================================================================
//
// protocol 是**抽象层**：它承诺的是"任何后端只要满足这套语义，上层就能无差别使用"。
// 因此这里验证的是**契约本身**：回调线程约束、能力声明、失败语义、
// 就绪状态、守恒计数。这些与具体传输无关，用替身测更快、更稳定、也更精确 ——
// 真实网络的行为由 rtps 模块与场景脚本负责。
//
// 三个真实场景都取自实际用法：
//   ① 接非 DDS 数据源（PLC / 串口）：有界队列 + 背压 + 丢弃守恒
//   ② 向远端转发：连接状态驱动就绪、有语义的超时、注入失败
//   ③ 能力协商与降级：同一套上层代码面对三种能力组合

// 标准库头文件必须最先包含（避免被卷入自定义命名空间）
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "stratum/protocol/idds_backend.h"
#include "test_framework.h"

using namespace stratum;
using namespace stratum::protocol;

namespace {

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

TimestampNs NowNs() {
  return static_cast<TimestampNs>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// 在 BackendConfig::extra 里找键。
// 【注意】extra 是 vector<pair<string,string>>，不是 map —— 这是常见写错点。
const std::string* Find(const BackendConfig& c, const std::string& k) {
  for (const auto& kv : c.extra) {
    if (kv.first == k) return &kv.second;
  }
  return nullptr;
}

uint32_t GetU32(const BackendConfig& c, const std::string& k, uint32_t def) {
  const std::string* v = Find(c, k);
  return v == nullptr ? def : static_cast<uint32_t>(std::atoi(v->c_str()));
}

bool GetBool(const BackendConfig& c, const std::string& k, bool def) {
  const std::string* v = Find(c, k);
  if (v == nullptr) return def;
  return *v == "1" || *v == "true";
}

// ---------------------------------------------------------------------------
// 合成数据源：按配置产出 N 条样本
// ---------------------------------------------------------------------------
class SyntheticSource final : public ISource {
 public:
  struct Options {
    uint32_t count = 100;
    uint32_t interval_us = 0;
    std::string topic = "test/topic";
    std::string type = "TestType";
    size_t payload_bytes = 32;
    bool with_type_definition = true;
    uint32_t sequences_from = 1;   // 序号起点（验证"不提供序号"时置 0）
    bool provide_sequence = true;
  };

  explicit SyntheticSource(Options o) : opt_(std::move(o)) {}

  Status Start(SampleCallback callback) override {
    if (running_.exchange(true)) {
      return Status::AlreadyExists("SyntheticSource 已经启动");
    }
    worker_ = std::thread([this, cb = std::move(callback)]() {
      for (uint32_t i = 0; i < opt_.count && running_.load(); ++i) {
        ReceivedSample s;
        s.topic_name = opt_.topic;
        s.type_name = opt_.type;
        if (opt_.with_type_definition) {
          s.type_definition = "struct " + opt_.type + " { long v; };";
        }
        s.source_timestamp_ns = NowNs();
        s.reception_timestamp_ns = NowNs();
        // 不提供序号的后端必须置 0（而不是填假序号）——否则上层会以为能做缺口检测
        s.sequence = opt_.provide_sequence
                         ? (opt_.sequences_from + i)
                         : 0;
        s.payload.assign(opt_.payload_bytes, static_cast<char>('a' + (i % 26)));
        if (!cb(s)) break;      // 上层要求停止
        if (opt_.interval_us > 0) {
          std::this_thread::sleep_for(std::chrono::microseconds(opt_.interval_us));
        }
      }
      running_.store(false, std::memory_order_release);
    });
    return Status::OK();
  }

  Status Stop() override {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    return Status::OK();
  }

  bool IsRunning() const override { return running_.load(std::memory_order_acquire); }

  std::vector<std::string> DiscoveredTopics() const override { return {opt_.topic}; }

 private:
  Options opt_;
  std::atomic<bool> running_{false};
  std::thread worker_;
};

// ---------------------------------------------------------------------------
// 收集型数据汇
// ---------------------------------------------------------------------------
class CollectingSink final : public ISink {
 public:
  explicit CollectingSink(bool ready = true) : ready_(ready) {}

  Status Open() override {
    open_ = true;
    return Status::OK();
  }
  Status Close() override {
    open_ = false;
    return Status::OK();
  }
  bool IsOpen() const override { return open_; }

  Status Publish(const ReceivedSample& s) override {
    if (!open_) return Status::Internal("sink 未打开");
    std::lock_guard<std::mutex> lk(mu_);
    ++count_;
    bytes_ += s.payload.size();
    if (samples_.size() < 100000) samples_.push_back(s);
    return Status::OK();
  }

  bool ReadyForPublish() const override { return open_ && ready_; }
  Status WaitForAcknowledgments(uint64_t) override { return Status::OK(); }

  uint64_t count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return count_;
  }
  uint64_t bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bytes_;
  }
  std::vector<ReceivedSample> samples() const {
    std::lock_guard<std::mutex> lk(mu_);
    return samples_;
  }
  void SetReady(bool r) { ready_ = r; }

 private:
  bool open_ = false;
  bool ready_ = true;
  mutable std::mutex mu_;
  uint64_t count_ = 0;
  uint64_t bytes_ = 0;
  std::vector<ReceivedSample> samples_;
};

// ---------------------------------------------------------------------------
// 可配置后端
// ---------------------------------------------------------------------------
class ConfigurableBackend final : public IDdsBackend {
 public:
  Capabilities caps;
  SyntheticSource::Options source_options;

  std::string Name() const override { return "configurable"; }
  std::string Version() const override { return "1.0.0"; }

  Status CreateSource(const BackendConfig& cfg, SourcePtr* out) override {
    if (!provide_source_) return Status::NotSupported("本实例未启用数据源");
    SyntheticSource::Options o = source_options;
    o.count = GetU32(cfg, "count", o.count);
    o.interval_us = GetU32(cfg, "interval_us", o.interval_us);
    o.payload_bytes = GetU32(cfg, "payload_bytes", static_cast<uint32_t>(o.payload_bytes));
    o.provide_sequence = GetBool(cfg, "provide_sequence", o.provide_sequence);
    if (const std::string* v = Find(cfg, "topic")) o.topic = *v;
    if (const std::string* v = Find(cfg, "type_name")) o.type = *v;
    *out = SourcePtr(new SyntheticSource(o));
    return Status::OK();
  }

  Status CreateSink(const BackendConfig& cfg, SinkPtr* out) override {
    auto sp = std::make_shared<CollectingSink>(GetBool(cfg, "ready", true));
    last_sink_ = sp;
    *out = SinkPtr(new SharedSink(sp));
    return Status::OK();
  }

  IDdsBackend::Capabilities GetCapabilities() const override { return caps; }

  std::shared_ptr<CollectingSink> last_sink() const { return last_sink_; }
  void DisableSource() { provide_source_ = false; }

 private:
  class SharedSink final : public ISink {
   public:
    explicit SharedSink(std::shared_ptr<CollectingSink> p) : p_(std::move(p)) {}
    Status Open() override { return p_->Open(); }
    Status Close() override { return p_->Close(); }
    bool IsOpen() const override { return p_->IsOpen(); }
    Status Publish(const ReceivedSample& s) override { return p_->Publish(s); }
    bool ReadyForPublish() const override { return p_->ReadyForPublish(); }
    Status WaitForAcknowledgments(uint64_t t) override {
      return p_->WaitForAcknowledgments(t);
    }

   private:
    std::shared_ptr<CollectingSink> p_;
  };

  bool provide_source_ = true;
  std::shared_ptr<CollectingSink> last_sink_;
};

// ---------------------------------------------------------------------------
// 有界队列 + 三种溢出策略（场景 1 用）
// ---------------------------------------------------------------------------
class BoundedQueue {
 public:
  enum class Policy { kDropNewest, kDropOldest, kBlock };

  BoundedQueue(size_t cap, Policy p) : cap_(cap), policy_(p) {}

  // 返回 true = 已入队；false = 新元素被拒
  //
  // 【两种"丢失"必须分开计】这是一个真实的概念混淆：
  //   kDropNewest / kBlock 超时 -> 丢掉的是**刚来的**新元素（rejected）
  //   kDropOldest              -> 丢掉的是**队首旧元素**来腾位置（evicted），
  //                               而那条旧元素**曾经被计入 enqueued**
  // 若把两者合并成一个 dropped_，则 "enqueued + dropped == 生产数" 在
  // kDropOldest 下会多算（实测 actual 9455 vs expect 4752，约两倍）。
  // 正确的不变量是： enqueued + rejected == 生产数（任何策略都成立）。
  bool Push(const ReceivedSample& s) {
    std::unique_lock<std::mutex> lk(mu_);
    if (q_.size() >= cap_) {
      switch (policy_) {
        case Policy::kDropNewest:
          ++rejected_;
          return false;
        case Policy::kDropOldest:
          q_.pop_front();
          ++evicted_;
          break;
        case Policy::kBlock:
          // 【等待时间要短】kBlock 的语义是"队列满就等"。
          // 若调用方**没有消费者**（本测试的循环就是这样），队列永远满，
          // 每次 Push 都会等满这个超时 —— 50ms × 5000 条 × 3 种子 ≈ 750 秒，
          // 足以把整个测试套拖成分钟级（这正是它一度表现为"卡住"的原因）。
          // 2ms 足以表达"阻塞等待"的语义，又不会拖慢测试。
          not_full_.wait_for(lk, std::chrono::milliseconds(2),
                             [this] { return q_.size() < cap_; });
          if (q_.size() >= cap_) {
            ++rejected_;
            return false;
          }
          break;
      }
    }
    q_.push_back(s);
    ++enqueued_;
    return true;
  }

  bool Pop(ReceivedSample* out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.empty()) return false;
    *out = q_.front();
    q_.pop_front();
    not_full_.notify_one();
    return true;
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
  }
  uint64_t enqueued() const {
    std::lock_guard<std::mutex> lk(mu_);
    return enqueued_;
  }
  // 因队列满而被拒绝的新元素数
  uint64_t rejected() const {
    std::lock_guard<std::mutex> lk(mu_);
    return rejected_;
  }
  // 为腾位置而被淘汰的旧元素数（这些元素曾被计入 enqueued）
  uint64_t evicted() const {
    std::lock_guard<std::mutex> lk(mu_);
    return evicted_;
  }
  // 总丢失 = 被拒 + 被汰
  uint64_t lost() const {
    std::lock_guard<std::mutex> lk(mu_);
    return rejected_ + evicted_;
  }
  size_t capacity() const { return cap_; }

 private:
  mutable std::mutex mu_;
  std::condition_variable not_full_;
  std::deque<ReceivedSample> q_;
  size_t cap_;
  Policy policy_;
  uint64_t enqueued_ = 0;
  uint64_t rejected_ = 0;
  uint64_t evicted_ = 0;
};

}  // namespace

// ===========================================================================
// 一、ReceivedSample：字段语义
// ===========================================================================

STRATUM_TEST(Protocol, ReceivedSampleFieldSemantics) {
  ReceivedSample s;
  // 默认值必须能表达"未设置"，而不是假装有值
  STRATUM_CHECK_EQ(s.source_timestamp_ns, kInvalidTimestamp);
  STRATUM_CHECK_EQ(s.reception_timestamp_ns, kInvalidTimestamp);
  STRATUM_CHECK_EQ(s.sequence, static_cast<uint32_t>(0));
  STRATUM_CHECK_FALSE(s.is_dispose);

  s.topic_name = "a/b";
  s.type_name = "T";
  s.sequence = 7;
  // 两种时间戳分工不同：源时间戳定回放时序，接收时间戳诊断本端延迟
  s.source_timestamp_ns = 1000000000ULL;
  s.reception_timestamp_ns = 1000500000ULL;
  s.payload = std::string("\x00\x01", 2);

  STRATUM_CHECK_EQ(s.reception_timestamp_ns - s.source_timestamp_ns,
                   static_cast<TimestampNs>(500000));
  // payload 是原始字节，必须二进制安全
  STRATUM_CHECK_EQ(s.payload.size(), static_cast<size_t>(2));
  STRATUM_CHECK_TRUE(s.payload == std::string("\x00\x01", 2));

  // keyed 话题的实例标识：非 keyed 时为空
  STRATUM_CHECK_EQ(s.instance_key, std::string());
  s.instance_key = "key-1";
  STRATUM_CHECK_EQ(s.instance_key, std::string("key-1"));
}

// ===========================================================================
// 二、BackendConfig：extra 的用法约定
// ===========================================================================

STRATUM_TEST(Protocol, BackendConfigExtraIsVectorNotMap) {
  BackendConfig c;
  STRATUM_CHECK_EQ(c.domain_id, static_cast<uint32_t>(0));
  STRATUM_CHECK_EQ(c.participant_name, std::string("stratum"));
  STRATUM_CHECK_TRUE(c.topics.empty());   // 空 = 自动发现全部
  STRATUM_CHECK_TRUE(c.extra.empty());

  // extra 是 vector<pair>：同名键可以出现多次（不是 map 的覆盖语义）
  c.extra.push_back({"k", "1"});
  c.extra.push_back({"k", "2"});
  STRATUM_CHECK_EQ(c.extra.size(), static_cast<size_t>(2));
  STRATUM_CHECK_EQ(*Find(c, "k"), std::string("1"));   // 取第一个
  STRATUM_CHECK_EQ(GetU32(c, "k", 99), static_cast<uint32_t>(1));
  STRATUM_CHECK_EQ(GetU32(c, "missing", 99), static_cast<uint32_t>(99));

  // 话题列表：支持精确名与前缀通配（以 * 结尾）
  c.topics.push_back("sensor/*");
  c.topics.push_back("exact/topic");
  STRATUM_CHECK_EQ(c.topics.size(), static_cast<size_t>(2));
  STRATUM_CHECK_EQ(c.topics[0].back(), '*');
}

// ===========================================================================
// 三、ISource / ISink 契约
// ===========================================================================

STRATUM_TEST(Protocol, SourceLifecycleAndCallbackContract) {
  ConfigurableBackend b;
  b.source_options.count = 50;
  b.source_options.payload_bytes = 16;

  BackendConfig cfg;
  SourcePtr src;
  STRATUM_CHECK_TRUE(b.CreateSource(cfg, &src).ok());
  STRATUM_CHECK_TRUE(src != nullptr);
  STRATUM_CHECK_FALSE(src->IsRunning());

  std::atomic<uint64_t> got{0};
  std::atomic<uint64_t> ordered_violations{0};
  uint32_t last_seq = 0;
  std::atomic<bool> callback_on_other_thread{false};
  const auto main_tid = std::this_thread::get_id();

  Status s = src->Start([&](const ReceivedSample& smp) {
    // 【契约】回调在后端自己的线程上，而不是调用者线程
    if (std::this_thread::get_id() != main_tid) {
      callback_on_other_thread.store(true, std::memory_order_relaxed);
    }
    // 序号必须单调（上层据此做缺口检测）
    if (smp.sequence != 0 && smp.sequence <= last_seq) {
      ordered_violations.fetch_add(1, std::memory_order_relaxed);
    }
    last_seq = smp.sequence;
    got.fetch_add(1, std::memory_order_relaxed);
    return true;
  });
  STRATUM_CHECK_TRUE(s.ok());
  STRATUM_CHECK_TRUE(src->IsRunning());

  // 重复 Start 必须明确失败，而不是启动第二个线程
  Status again = src->Start([](const ReceivedSample&) { return true; });
  STRATUM_CHECK_FALSE(again.ok());
  STRATUM_CHECK_TRUE(again.ToString().find("AlreadyExists") != std::string::npos);

  for (int i = 0; i < 200 && got.load() < 50; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  STRATUM_CHECK_TRUE(src->Stop().ok());
  STRATUM_CHECK_FALSE(src->IsRunning());

  STRATUM_CHECK_EQ(got.load(), static_cast<uint64_t>(50));
  STRATUM_CHECK_EQ(ordered_violations.load(), static_cast<uint64_t>(0));
  STRATUM_CHECK_TRUE(callback_on_other_thread.load());

  auto topics = src->DiscoveredTopics();
  STRATUM_CHECK_EQ(topics.size(), static_cast<size_t>(1));
}

STRATUM_TEST(Protocol, SourceCallbackCanStopEarly) {
  // 回调返回 false 表示"上层要求停止"，后端应尽快退出
  ConfigurableBackend b;
  b.source_options.count = 100000;   // 远多于我们想收的
  BackendConfig cfg;
  SourcePtr src;
  b.CreateSource(cfg, &src);

  std::atomic<uint64_t> got{0};
  src->Start([&](const ReceivedSample&) {
    const uint64_t n = got.fetch_add(1, std::memory_order_relaxed) + 1;
    return n < 10;      // 收到第 10 条就要求停止
  });

  for (int i = 0; i < 200 && got.load() < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  src->Stop();
  // 必须真的停下来，而不是把 10 万条都推完
  STRATUM_CHECK_TRUE(got.load() <= 11);
  STRATUM_CHECK_TRUE(got.load() >= 10);
}

STRATUM_TEST(Protocol, SinkNotOpenRejectsPublish) {
  ConfigurableBackend b;
  BackendConfig cfg;
  SinkPtr sink;
  b.CreateSink(cfg, &sink);
  STRATUM_CHECK_FALSE(sink->IsOpen());

  ReceivedSample s;
  s.topic_name = "t";
  s.payload = "x";
  // 未打开就发布必须明确失败（静默丢是本项目最大的坑源）
  const Status r = sink->Publish(s);
  STRATUM_CHECK_FALSE(r.ok());
  STRATUM_CHECK_TRUE(r.ToString().find("Internal") != std::string::npos);

  STRATUM_CHECK_TRUE(sink->Open().ok());
  STRATUM_CHECK_TRUE(sink->IsOpen());
  STRATUM_CHECK_TRUE(sink->Publish(s).ok());
  STRATUM_CHECK_TRUE(sink->Close().ok());
  STRATUM_CHECK_FALSE(sink->IsOpen());
}

STRATUM_TEST(Protocol, SinkReadyForPublishIsHonest) {
  ConfigurableBackend b;
  BackendConfig cfg;
  cfg.extra.push_back({"ready", "0"});     // 未就绪
  SinkPtr sink;
  b.CreateSink(cfg, &sink);
  sink->Open();

  // 就绪状态必须如实反映，而不是恒为 true ——
  // 它存在的意义就是把"显示已发布、对端收不到"的静默失败显式化
  STRATUM_CHECK_FALSE(sink->ReadyForPublish());
  if (auto sp = b.last_sink()) {
    sp->SetReady(true);
    STRATUM_CHECK_TRUE(sink->ReadyForPublish());
  }
  sink->Close();
}

STRATUM_TEST(Protocol, BackendWithoutSourceFailsExplicitly) {
  ConfigurableBackend b;
  b.DisableSource();
  SourcePtr src;
  const Status r = b.CreateSource(BackendConfig{}, &src);
  STRATUM_CHECK_FALSE(r.ok());
  STRATUM_CHECK_TRUE(r.ToString().find("NotSupported") != std::string::npos);
}

STRATUM_TEST(Protocol, CapabilitiesAreAdvertised) {
  ConfigurableBackend b;
  b.caps.auto_discovery = true;
  b.caps.provides_sequence = true;
  b.caps.provides_type_def = false;
  b.caps.supports_multicast = false;
  const auto c = b.GetCapabilities();
  STRATUM_CHECK_TRUE(c.auto_discovery);
  STRATUM_CHECK_TRUE(c.provides_sequence);
  STRATUM_CHECK_FALSE(c.provides_type_def);
  STRATUM_CHECK_FALSE(c.supports_multicast);
  STRATUM_CHECK_EQ(b.Name(), std::string("configurable"));
  STRATUM_CHECK_EQ(b.Version(), std::string("1.0.0"));
}

// ===========================================================================
// 四、大数据量
// ===========================================================================

STRATUM_TEST(Protocol, LargeVolumeBulkDelivery) {
  const uint32_t kN = 1000000;      // 100 万条

  ConfigurableBackend b;
  b.source_options.count = kN;
  b.source_options.payload_bytes = 64;
  b.source_options.interval_us = 0;   // 不限速

  BackendConfig cfg;
  cfg.extra.push_back({"count", std::to_string(kN)});
  cfg.extra.push_back({"payload_bytes", "64"});

  SourcePtr src;
  STRATUM_CHECK_TRUE(b.CreateSource(cfg, &src).ok());

  std::atomic<uint64_t> got{0};
  std::atomic<uint64_t> bytes{0};
  auto t0 = Clock::now();
  src->Start([&](const ReceivedSample& s) {
    got.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(s.payload.size(), std::memory_order_relaxed);
    return true;
  });

  // 轮询直到收完（给出上限，避免测试挂死）
  for (int i = 0; i < 3000 && got.load() < kN; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const double ms = MsSince(t0);
  src->Stop();

  STRATUM_CHECK_EQ(got.load(), static_cast<uint64_t>(kN));
  STRATUM_CHECK_EQ(bytes.load(), static_cast<uint64_t>(kN) * 64);
  const double mb = static_cast<double>(bytes.load()) / (1024.0 * 1024.0);
  std::cout << "      [规模] " << kN << " 条 / " << mb << " MB 经回调交付，耗时 "
            << ms << " ms（" << (static_cast<double>(kN) / (ms / 1000.0) / 1e6)
            << " M 条/秒）" << std::endl;
}

STRATUM_TEST(Protocol, LargePayloadDelivery) {
  // 大 payload：单条 1 MB × 200 条 = 200 MB
  ConfigurableBackend b;
  BackendConfig cfg;
  cfg.extra.push_back({"count", "200"});
  cfg.extra.push_back({"payload_bytes", std::to_string(1024 * 1024)});

  SourcePtr src;
  b.CreateSource(cfg, &src);

  std::atomic<uint64_t> got{0};
  std::atomic<uint64_t> bytes{0};
  auto t0 = Clock::now();
  src->Start([&](const ReceivedSample& s) {
    got.fetch_add(1, std::memory_order_relaxed);
    bytes.fetch_add(s.payload.size(), std::memory_order_relaxed);
    return true;
  });
  for (int i = 0; i < 2000 && got.load() < 200; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const double ms = MsSince(t0);
  src->Stop();

  STRATUM_CHECK_EQ(got.load(), static_cast<uint64_t>(200));
  STRATUM_CHECK_EQ(bytes.load(), static_cast<uint64_t>(200) * 1024 * 1024);
  std::cout << "      [规模] 200 条 × 1MB = "
            << (static_cast<double>(bytes.load()) / (1024.0 * 1024.0))
            << " MB，耗时 " << ms << " ms" << std::endl;
}

// ===========================================================================
// 五、随机测试（多种子）
// ===========================================================================

STRATUM_TEST(Protocol, RandomSampleFieldsMultipleSeeds) {
  // 随机生成样本字段组合，验证：
  //   · 字段赋值/取回不串味（尤其 kString 与 kBytes 共用的 string_value 这类）
  //   · payload 二进制安全（随机含 \0）
  // 依赖概率的断言是 flaky 断言，因此对每个种子都做**确定性**校验。
  for (uint64_t seed : {1ULL, 42ULL, 777ULL, 20261014ULL}) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<size_t> len_d(0, 512);
    std::uniform_int_distribution<uint32_t> seq_d(0, 1000000);
    std::uniform_int_distribution<int> byte_d(0, 255);
    std::uniform_int_distribution<int> roll(0, 3);

    for (int i = 0; i < 5000; ++i) {
      ReceivedSample s;
      const size_t n = len_d(rng);
      s.payload.resize(n);
      for (size_t k = 0; k < n; ++k) {
        s.payload[k] = static_cast<char>(byte_d(rng));
      }
      s.sequence = seq_d(rng);
      s.topic_name = "t" + std::to_string(roll(rng));
      s.type_name = "T" + std::to_string(roll(rng));
      s.is_dispose = (roll(rng) == 0);
      if (roll(rng) == 0) s.instance_key = "k" + std::to_string(seq_d(rng));

      // 拷贝后内容必须逐字节相同（二进制安全）
      ReceivedSample copy = s;
      STRATUM_CHECK_EQ(copy.payload.size(), n);
      STRATUM_CHECK_TRUE(copy.payload == s.payload);
      STRATUM_CHECK_EQ(copy.sequence, s.sequence);
      STRATUM_CHECK_EQ(copy.is_dispose, s.is_dispose);
      STRATUM_CHECK_EQ(copy.instance_key, s.instance_key);
    }
  }
}

STRATUM_TEST(Protocol, RandomOverflowPolicyConservesCount) {
  // 随机容量与随机丢弃策略，验证**守恒**：
  //     入队数 + 被拒数 == 生产数      （任何策略、任何时序都成立）
  //     队列长度 == 入队数 - 被汰数
  // 不要写成"入队 + 丢失 == 生产"—— 那在 kDropOldest 下会多算，
  // 因为被淘汰的旧元素曾经入过队。
  for (uint64_t seed : {7ULL, 99ULL, 20261014ULL}) {
    for (auto policy : {BoundedQueue::Policy::kDropNewest,
                        BoundedQueue::Policy::kDropOldest,
                        BoundedQueue::Policy::kBlock}) {
      std::mt19937_64 rng(seed);
      std::uniform_int_distribution<size_t> cap_d(1, 64);
      std::uniform_int_distribution<uint32_t> n_d(100, 5000);

      const size_t cap = cap_d(rng);
      // 【kBlock 必须用小规模】没有消费者时它会几乎每次都等满超时，
      // 这是策略定义（阻塞等待）而非缺陷；但用它跑几千条没有意义，
      // 只会让测试变慢。其余两种策略是 O(1) 丢弃，规模可以大。
      const bool is_block = (policy == BoundedQueue::Policy::kBlock);
      const uint32_t total = is_block ? (n_d(rng) % 300) + 1 : n_d(rng);
      BoundedQueue q(cap, policy);

      for (uint32_t i = 0; i < total; ++i) {
        ReceivedSample s;
        s.topic_name = "t";
        s.sequence = i + 1;
        s.payload.assign(16, 'x');
        q.Push(s);
      }

      STRATUM_CHECK_EQ(q.enqueued() + q.rejected(), static_cast<uint64_t>(total));
      STRATUM_CHECK_EQ(q.Size(), q.enqueued() - q.evicted());
      STRATUM_CHECK_TRUE(q.Size() <= cap);
      STRATUM_CHECK_EQ(q.lost(), q.rejected() + q.evicted());
    }
  }
}

// ===========================================================================
// 六、真实场景
// ===========================================================================

// ---------------------------------------------------------------------------
// 场景 1：接非 DDS 数据源（PLC / 串口）
//
// 现实：外部设备速率不稳定，本端消费可能跟不上。必须有**有界队列**，
// 队列满时的策略要由业务决定，并且**丢弃必须可见地计数**（不能悄悄丢）。
// ---------------------------------------------------------------------------
STRATUM_TEST(Protocol, RealScenario_NonDdsSourceWithBackpressure) {
  const size_t kCap = 64;          // 故意很小
  const uint32_t kTotal = 20000;
  const size_t kConsumeCostUs = 50;   // 消费者每条耗时（模拟真实处理成本）

  BoundedQueue q(kCap, BoundedQueue::Policy::kDropNewest);
  std::atomic<bool> producing{true};

  // 生产者（外部设备）：不限速
  std::thread producer([&]() {
    for (uint32_t i = 0; i < kTotal; ++i) {
      ReceivedSample s;
      s.topic_name = "plc/register";
      s.type_name = "PlcRegister";
      s.sequence = i + 1;
      s.source_timestamp_ns = NowNs();
      s.reception_timestamp_ns = s.source_timestamp_ns;
      s.payload.assign(32, 'p');
      q.Push(s);
    }
    producing.store(false);
  });

  // 消费者（Stratum 采样回调侧）：有真实处理成本
  std::atomic<uint64_t> delivered{0};
  std::thread consumer([&]() {
    ReceivedSample s;
    while (producing.load() || q.Size() > 0) {
      if (q.Pop(&s)) {
        delivered.fetch_add(1, std::memory_order_relaxed);
        if (kConsumeCostUs) {
          std::this_thread::sleep_for(std::chrono::microseconds(kConsumeCostUs));
        }
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
      }
    }
  });

  producer.join();
  consumer.join();

  const uint64_t enq = q.enqueued();
  const uint64_t rej = q.rejected();
  const uint64_t evi = q.evicted();

  std::cout << "      [场景1] 生产 " << kTotal << " / 队列容量 " << kCap
            << " -> 入队 " << enq << "、被拒 " << rej << "、被汰 " << evi
            << "、消费 " << delivered.load() << std::endl;

  // 【守恒不变量】任何时序下都成立 —— 这才是该断言的东西
  STRATUM_CHECK_EQ(enq + rej, static_cast<uint64_t>(kTotal));
  // 被淘汰的元素从未被消费
  STRATUM_CHECK_EQ(delivered.load(), enq - evi);
  STRATUM_CHECK_TRUE(q.lost() > 0);
}

// ---------------------------------------------------------------------------
// 场景 2：向远端转发
//
// 现实：连接会断，断了就不该继续发（发了也是丢），要如实告诉上层；
// 发送会超时，超时必须返回**有语义的失败**，而不是 OK。
// ---------------------------------------------------------------------------
namespace {

class FlakyRemoteSink final : public ISink {
 public:
  FlakyRemoteSink(uint32_t fail_first, uint32_t flap_every)
      : fail_first_(fail_first), flap_every_(flap_every) {}

  Status Open() override {
    open_ = true;
    connected_ = true;
    return Status::OK();
  }
  Status Close() override {
    open_ = false;
    return Status::OK();
  }
  bool IsOpen() const override { return open_; }

  Status Publish(const ReceivedSample& s) override {
    if (!open_) return Status::Internal("未打开");
    ++attempts_;
    if (attempts_ <= fail_first_) {
      ++fail_;
      return Status::IOError("远端尚未就绪");
    }
    if (flap_every_ > 0 && (attempts_ % flap_every_) == 0) {
      ++fail_;
      connected_ = false;
      return Status::IOError("连接抖动");
    }
    connected_ = true;
    ++ok_;
    if (unacked_.size() < 1000) unacked_.push_back(s);
    return Status::OK();
  }

  // 就绪由真实连接状态决定，不是恒 true
  bool ReadyForPublish() const override { return open_ && connected_; }

  // 有真实语义的确认等待：超时返回 Timeout，不能返回 OK
  Status WaitForAcknowledgments(uint64_t timeout_ms) override {
    if (!open_) return Status::Internal("未打开");
    std::this_thread::sleep_for(std::chrono::milliseconds(
        std::min<uint64_t>(timeout_ms, 50)));
    if (fail_ > 0) return Status::Timeout("仍有未确认样本");
    return Status::OK();
  }

  std::vector<ReceivedSample> UnackedSamples() const override { return unacked_; }

  uint64_t ok() const { return ok_; }
  uint64_t fail() const { return fail_; }

 private:
  uint32_t fail_first_;
  uint32_t flap_every_;
  bool open_ = false;
  bool connected_ = false;
  uint64_t attempts_ = 0;
  uint64_t ok_ = 0;
  uint64_t fail_ = 0;
  std::vector<ReceivedSample> unacked_;
};

}  // namespace

STRATUM_TEST(Protocol, RealScenario_RemoteForwardingWithRetry) {
  FlakyRemoteSink sink(3, 7);       // 前 3 次失败，之后每 7 次抖一次
  STRATUM_CHECK_TRUE(sink.Open().ok());
  STRATUM_CHECK_TRUE(sink.ReadyForPublish());

  uint64_t published = 0;
  for (uint32_t i = 1; i <= 100; ++i) {
    ReceivedSample s;
    s.topic_name = "remote/topic";
    s.sequence = i;
    s.payload.assign(128, 'r');
    const Status r = sink.Publish(s);
    if (r.ok()) ++published;
  }

  // 错误路径必须真实可达（否则"测试失败处理"这件事根本没被验证）
  STRATUM_CHECK_TRUE(sink.fail() > 0);
  STRATUM_CHECK_TRUE(sink.ok() > 0);
  STRATUM_CHECK_EQ(sink.ok(), published);

  // 超时必须返回 Timeout，而不是谎报成功
  const Status w = sink.WaitForAcknowledgments(50);
  STRATUM_CHECK_FALSE(w.ok());
  STRATUM_CHECK_TRUE(w.ToString().find("Timeout") != std::string::npos);

  // 未确认样本可供上层用新 SN 重发
  STRATUM_CHECK_TRUE(!sink.UnackedSamples().empty());

  std::cout << "      [场景2] 100 条转发：成功 " << sink.ok() << "、失败 "
            << sink.fail() << "；超时语义为 Timeout" << std::endl;
}

// ---------------------------------------------------------------------------
// 场景 3：能力协商与优雅降级
//
// 现实：同一份上层代码要面对不同后端。没有序号的后端**不能**做缺口检测 ——
// 上层必须知道并告知用户，而不是给出一份"看起来很完整"的质量报告。
// ---------------------------------------------------------------------------
namespace {

struct RecordingPlan {
  bool can_record_all = false;
  bool can_detect_gaps = false;
  bool can_export_types = false;
  std::vector<std::string> warnings;
};

RecordingPlan PlanFrom(const IDdsBackend::Capabilities& c) {
  RecordingPlan p;
  p.can_record_all = c.auto_discovery;
  p.can_detect_gaps = c.provides_sequence;
  p.can_export_types = c.provides_type_def;
  if (!p.can_record_all) p.warnings.push_back("必须显式给出话题");
  if (!p.can_detect_gaps) p.warnings.push_back("无法检测缺口");
  if (!p.can_export_types) p.warnings.push_back("回放端需自带类型");
  return p;
}

}  // namespace

STRATUM_TEST(Protocol, RealScenario_CapabilityNegotiation) {
  struct Case {
    const char* name;
    bool disc, seq, type;
    int expect_warnings;
  };
  const Case cases[] = {
      {"能力完备", true, true, true, 0},
      {"有发现缺序号", true, false, true, 1},
      {"最小能力", false, false, false, 3},
      {"仅有序号", false, true, false, 2},
  };

  for (const auto& c : cases) {
    ConfigurableBackend b;
    b.caps.auto_discovery = c.disc;
    b.caps.provides_sequence = c.seq;
    b.caps.provides_type_def = c.type;

    const auto caps = b.GetCapabilities();
    const RecordingPlan plan = PlanFrom(caps);

    STRATUM_CHECK_EQ(plan.can_record_all, c.disc);
    STRATUM_CHECK_EQ(plan.can_detect_gaps, c.seq);
    STRATUM_CHECK_EQ(plan.can_export_types, c.type);
    STRATUM_CHECK_EQ(static_cast<int>(plan.warnings.size()), c.expect_warnings);

    // 【关键】"没有序号"必须导致上层**不做**缺口检测 ——
    // 否则会输出一份看起来完整、实际无法保证的质量报告。
    if (!caps.provides_sequence) {
      STRATUM_CHECK_FALSE(plan.can_detect_gaps);
      bool warned = false;
      for (const auto& w : plan.warnings) {
        if (w.find("缺口") != std::string::npos) warned = true;
      }
      STRATUM_CHECK_TRUE(warned);
    }

    // 不支持自动发现时，必须显式给话题才能录制
    BackendConfig cfg;
    cfg.extra.push_back({"count", "10"});
    if (!plan.can_record_all) cfg.topics.push_back("explicit/topic");
    SourcePtr src;
    STRATUM_CHECK_TRUE(b.CreateSource(cfg, &src).ok());

    // 不提供序号的后端，其样本 sequence 必须为 0（不能填假序号）
    std::atomic<uint32_t> first_seq{999};
    std::atomic<uint64_t> got{0};
    b.source_options.provide_sequence = c.seq;
    b.source_options.count = 5;
    SourcePtr src2;
    b.CreateSource(cfg, &src2);
    src2->Start([&](const ReceivedSample& s) {
      if (got.fetch_add(1) == 0) first_seq.store(s.sequence);
      return true;
    });
    for (int i = 0; i < 200 && got.load() < 5; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    src2->Stop();

    if (c.seq) {
      STRATUM_CHECK_TRUE(first_seq.load() > 0);
    } else {
      STRATUM_CHECK_EQ(first_seq.load(), static_cast<uint32_t>(0));
    }
  }

  std::cout << "      [场景3] 4 种能力组合的录制计划与告警均符合预期" << std::endl;
}

STRATUM_TEST_MAIN()
