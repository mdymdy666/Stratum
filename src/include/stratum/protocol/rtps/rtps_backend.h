// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// RtpsBackend —— 把自研 RTPS 引擎接入 Stratum 的 IDdsBackend 插件接口
//
// ===========================================================================
// 这一层做什么
// ===========================================================================
//
// 它是**适配器**，不含协议逻辑：
//
//   Recorder ──▶ ISource ──▶ RtpsSource ──▶ RtpsReader（订阅端引擎）
//   Player   ──▶ ISink   ──▶ RtpsSink   ──▶ RtpsWriter（发布端引擎）
//
// 存在的意义（见 idds_backend.h 的说明）：
//   - Recorder/Player 不需要知道 RTPS 的存在
//   - 测试可以用 MockBackend 在零网络条件下验证上层
//   - 未来可替换为 Fast DDS 适配实现

#ifndef STRATUM_PROTOCOL_RTPS_RTPS_BACKEND_H_
#define STRATUM_PROTOCOL_RTPS_RTPS_BACKEND_H_

#include <memory>
#include <string>

#include "stratum/protocol/idds_backend.h"
#include "stratum/protocol/rtps/rtps_reader.h"
#include "stratum/protocol/rtps/rtps_writer.h"

namespace stratum {
namespace protocol {
namespace rtps {

// 录制数据源：把 RtpsReader 的用户数据回调适配成 ISource 的 SampleCallback
class RtpsSource : public ISource {
 public:
  explicit RtpsSource(const BackendConfig& config);
  ~RtpsSource() override;

  RtpsSource(const RtpsSource&) = delete;
  RtpsSource& operator=(const RtpsSource&) = delete;

  Status Start(SampleCallback callback) override;
  Status Stop() override;
  bool IsRunning() const override;
  std::vector<std::string> DiscoveredTopics() const override;

  RtpsReader::Stats GetReaderStats() const { return reader_.GetStats(); }
  const std::string& last_error() const { return reader_.last_error(); }

 private:
  RtpsReader reader_;
  RtpsReaderConfig reader_config_;
};

// 回放数据汇：把 ISink 的 Publish 适配到 RtpsWriter
class RtpsSink : public ISink {
 public:
  explicit RtpsSink(const BackendConfig& config);
  ~RtpsSink() override;

  RtpsSink(const RtpsSink&) = delete;
  RtpsSink& operator=(const RtpsSink&) = delete;

  Status Open() override;
  Status Close() override;
  bool IsOpen() const override;

  Status Publish(const ReceivedSample& sample) override;
  Status WaitForAcknowledgments(uint64_t timeout_ms) override;

  bool ReadyForPublish() const override { return writer_.HasMatchedReader(); }

  std::vector<ReceivedSample> UnackedSamples() const override {
    return writer_.UnackedSamples();
  }

  RtpsWriter::Stats GetWriterStats() const { return writer_.GetStats(); }
  const std::string& last_error() const { return writer_.last_error(); }

 private:
  RtpsWriter writer_;
  RtpsWriterConfig writer_config_;
  bool open_ = false;
};

// RTPS 后端
class RtpsBackend : public IDdsBackend {
 public:
  RtpsBackend() = default;
  ~RtpsBackend() override = default;

  std::string Name() const override { return "rtps"; }
  std::string Version() const override { return "stratum-rtps/0.2"; }

  Status CreateSource(const BackendConfig& config, SourcePtr* out) override;
  Status CreateSink(const BackendConfig& config, SinkPtr* out) override;

  Capabilities GetCapabilities() const override {
    Capabilities c;
    c.auto_discovery = true;      // 支持"录制全部发现到的话题"
    c.provides_sequence = true;   // 提供 writerSN 作为序号
    c.provides_type_def = false;  // 尚未解析 TypeObject
    c.supports_multicast = true;
    return c;
  }
};

// 工厂（供插件注册使用）
class RtpsBackendFactory : public IDdsBackendFactory {
 public:
  std::string Name() const override { return "rtps"; }
  DdsBackendPtr Create() override {
    return DdsBackendPtr(new RtpsBackend());
  }
};

}  // namespace rtps
}  // namespace protocol
}  // namespace stratum

#endif  // STRATUM_PROTOCOL_RTPS_RTPS_BACKEND_H_
