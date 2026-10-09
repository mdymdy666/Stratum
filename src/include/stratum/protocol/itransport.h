// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// 传输插件接口
//
// Stratum 的传输层要同时服务两件事：
//   1. 记录模式：接收 DDS 数据（被动）
//   2. 回放模式：发送录制数据（主动）
//
// 因此接口必须同时支持收发，且不假设对端是谁。

#ifndef STRATUM_PROTOCOL_ITRANSPORT_H_
#define STRATUM_PROTOCOL_ITRANSPORT_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "stratum/core/status.h"

namespace stratum {
namespace protocol {

// ---------------------------------------------------------------------------
// 端点地址
//
// 刻意做成"字符串 + 类型标识"的通用形式，不假设是 IP/端口或共享内存路径。
// 具体解释交给传输实现。
// ---------------------------------------------------------------------------
struct Endpoint {
  // 传输类型（如 "shm"、"udp"、"unix"）
  std::string transport;

  // 传输相关的地址串。约定：
  //   shm  : 段名或路径
  //   udp  : "ip:port"
  //   unix : socket 路径
  std::string address;

  std::string ToString() const { return transport + "://" + address; }
};

// ---------------------------------------------------------------------------
// 收到的数据报
// ---------------------------------------------------------------------------
struct Datagram {
  std::string payload;      // 原始字节
  Endpoint sender;          // 来源（尽力填充）
  uint64_t receive_time_ns = 0;
};

// 收到数据时的回调。返回 false 表示要求停止接收。
using ReceiveCallback = std::function<bool(const Datagram&)>;

// ---------------------------------------------------------------------------
// 传输配置
// ---------------------------------------------------------------------------
struct TransportConfig {
  // 本端监听地址（服务端语义）；为空表示仅作客户端
  Endpoint local;

  // 对端地址（客户端语义）
  Endpoint remote;

  // 接收缓冲上限（字节）。超过后按策略处理。
  uint64_t receive_buffer_bytes = 16ULL * 1024 * 1024;

  // 发送超时（毫秒）
  uint32_t send_timeout_ms = 1000;

  // 传输特有的键值配置
  std::vector<std::pair<std::string, std::string>> extra;
};

// ---------------------------------------------------------------------------
// 传输接口
//
// 线程安全：
//   - Send 可被多线程并发调用
//   - StartReceiving 只能调用一次，直到 StopReceiving
// ---------------------------------------------------------------------------
class ITransport {
 public:
  virtual ~ITransport() = default;

  // --- 生命周期 -----------------------------------------------------------

  virtual Status Open(const TransportConfig& config) = 0;
  virtual Status Close() = 0;
  virtual bool IsOpen() const = 0;

  // --- 发送 ---------------------------------------------------------------

  // 发送一个数据报。返回 OK 表示已交给传输层（不保证对端收到）。
  virtual Status Send(const Endpoint& to, const std::string& payload) = 0;

  // --- 接收 ---------------------------------------------------------------

  // 开始接收。回调在**传输自己的线程**上被调用。
  // 回调中不得执行阻塞操作（否则会拖慢接收）。
  virtual Status StartReceiving(ReceiveCallback callback) = 0;

  virtual Status StopReceiving() = 0;

  // --- 能力与诊断 ---------------------------------------------------------

  // 该传输是否支持一对多发送
  virtual bool supports_multicast() const = 0;

  // 该传输是否为可靠传输（可靠=底层保证不丢）
  virtual bool is_reliable() const = 0;

  // 最大传输单元（字节）。超过需要分片。
  virtual size_t max_datagram_size() const = 0;

  virtual std::string Name() const = 0;
};

using TransportPtr = std::unique_ptr<ITransport>;

// ---------------------------------------------------------------------------
// 工厂
// ---------------------------------------------------------------------------
class ITransportFactory {
 public:
  virtual ~ITransportFactory() = default;

  virtual std::string Name() const = 0;
  virtual TransportPtr Create() = 0;
};

}  // namespace protocol
}  // namespace stratum

#endif  // STRATUM_PROTOCOL_ITRANSPORT_H_
