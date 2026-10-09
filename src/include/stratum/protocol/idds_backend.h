// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// DDS 后端插件接口
//
// ===========================================================================
// 为什么需要这一层
// ===========================================================================
//
// Recorder（录制器）需要"从某处拿到 DDS 数据"，Player（回放器）需要
// "把数据发到某处"。如果把这段逻辑直接写死到 Stratum 自研的 RTPS 实现里，
// 会带来两个问题：
//
//   1. **无法测试**：任何录制/回放的测试都要启动真实网络协议栈
//   2. **无法替换**：想接入 Fast DDS 或做模拟数据源，必须改 Recorder
//
// 因此把"DDS 实现"抽象成本接口（插件），Recorder/Player 只依赖它。
//
// ===========================================================================
// 分层
// ===========================================================================
//
//   Recorder ──uses──▶ ISource ──created by──▶ IBackend
//   Player   ──uses──▶ ISink   ──created by──▶ IBackend
//                          ▲
//                          │ 实现（均为插件）
//              ┌───────────┼────────────┬──────────────┐
//         自研 RTPS     模拟数据源    Fast DDS 适配   （未来）
//
// 这一层的存在使得"录制 → 回放 → 校验"的端到端测试可以在**纯内存、
// 零网络**的条件下完成。这对正确性验证的价值极大。

#ifndef STRATUM_PROTOCOL_IDDS_BACKEND_H_
#define STRATUM_PROTOCOL_IDDS_BACKEND_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "stratum/core/status.h"
#include "stratum/core/types.h"

namespace stratum {
namespace protocol {

// ---------------------------------------------------------------------------
// 后端配置
// ---------------------------------------------------------------------------
struct BackendConfig {
  // DDS 域号
  uint32_t domain_id = 0;

  // 参与者名称（便于运维识别，避免"未知订阅者"）
  std::string participant_name = "stratum";

  // 要关注的话题。空表示"全部"（自动发现模式）。
  std::vector<std::string> topics;

  // 后端特有配置
  std::vector<std::pair<std::string, std::string>> extra;
};

// ---------------------------------------------------------------------------
// 收到的样本
//
// 注意：这里刻意保留"原始字节"而不是解析后的结构。
// Stratum 的核心契约是"记录原始字节"，解析是可选的上层能力。
// ---------------------------------------------------------------------------
struct ReceivedSample {
  std::string topic_name;
  std::string type_name;

  // 类型定义（可能为空，见 docs/03-open-questions.md 的 Q11）
  std::string type_definition;

  // 两种时间戳都必须保留（见 docs/06-data-trust.md）
  TimestampNs source_timestamp_ns = kInvalidTimestamp;
  TimestampNs reception_timestamp_ns = kInvalidTimestamp;

  // 同一话题内的序号（用于缺口检测；后端不提供时置 0）
  uint32_t sequence = 0;

  // 原始序列化负载
  std::string payload;

  // keyed 话题的实例标识
  std::string instance_key;

  bool is_dispose = false;
};

// 收到样本时的回调。返回 false 表示要求后端停止推送。
using SampleCallback = std::function<bool(const ReceivedSample&)>;

// ---------------------------------------------------------------------------
// 数据源：录制器从这里取数据
// ---------------------------------------------------------------------------
class ISource {
 public:
  virtual ~ISource() = default;

  // 启动接收。回调在**后端自己的线程**上被调用。
  // 实现必须保证：回调中不得阻塞（否则拖慢接收）。
  virtual Status Start(SampleCallback callback) = 0;

  virtual Status Stop() = 0;

  virtual bool IsRunning() const = 0;

  // 当前已发现的话题（用于自检与质量报告）
  virtual std::vector<std::string> DiscoveredTopics() const = 0;
};

using SourcePtr = std::unique_ptr<ISource>;

// ---------------------------------------------------------------------------
// 数据汇：回放器往这里发数据
// ---------------------------------------------------------------------------
class ISink {
 public:
  virtual ~ISink() = default;

  virtual Status Open() = 0;
  virtual Status Close() = 0;
  virtual bool IsOpen() const = 0;

  // 发布一条样本。
  // 返回 OK 表示已交给后端（不保证对端收到）。
  virtual Status Publish(const ReceivedSample& sample) = 0;


  // 尚未被对端确认的样本（可选能力，默认空）。
  //
  // 用途：VOLATILE durability 下 reader 不会索要匹配完成之前发出的数据，
  // 回放的首条会永久丢失，且重发**相同 SN** 无效。上层据此用**新 SN**
  // 重新发布同一份内容，从而保证内容完整（见 Player 的收尾补救）。
  virtual std::vector<ReceivedSample> UnackedSamples() const { return {}; }

  // 数据汇是否已准备好接受发布（可选能力）。
  //
  // 【为什么需要】DDS 的 VOLATILE durability 语义下，reader 在匹配建立
  // **之前**发出的数据它不会索要 —— 最早的那条会被永久忽略（实测回放
  // 200 条时订阅端稳定收到 199 条，缺的恒为第一条，且重发也无效）。
  // 因此回放必须等到"对端 reader 已就绪"再发第一条。
  // 默认 true：内存/模拟后端没有发现过程。
  virtual bool ReadyForPublish() const { return true; }

  // 等待所有已发布数据被确认（可选能力）。
  // 不支持的后端应返回 kNotSupported。
  virtual Status WaitForAcknowledgments(uint64_t timeout_ms) = 0;
};

using SinkPtr = std::unique_ptr<ISink>;

// ---------------------------------------------------------------------------
// DDS 后端（插件）
// ---------------------------------------------------------------------------
class IDdsBackend {
 public:
  virtual ~IDdsBackend() = default;

  virtual std::string Name() const = 0;
  virtual std::string Version() const = 0;

  // 创建数据源（录制用）
  virtual Status CreateSource(const BackendConfig& config,
                              SourcePtr* out) = 0;

  // 创建数据汇（回放用）
  virtual Status CreateSink(const BackendConfig& config, SinkPtr* out) = 0;

  // 后端能力
  struct Capabilities {
    bool auto_discovery = false;   // 支持"录制全部话题"
    bool provides_sequence = false;  // 提供序号（用于缺口检测）
    bool provides_type_def = false;  // 提供类型定义
    bool supports_multicast = false;
  };
  virtual Capabilities GetCapabilities() const = 0;
};

using DdsBackendPtr = std::unique_ptr<IDdsBackend>;

// 工厂（用于插件注册）
class IDdsBackendFactory {
 public:
  virtual ~IDdsBackendFactory() = default;
  virtual std::string Name() const = 0;
  virtual DdsBackendPtr Create() = 0;
};

}  // namespace protocol
}  // namespace stratum

#endif  // STRATUM_PROTOCOL_IDDS_BACKEND_H_
