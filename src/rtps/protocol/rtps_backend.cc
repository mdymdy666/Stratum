// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// RtpsBackend / RtpsSource / RtpsSink 实现

#include <stratum/protocol/rtps/rtps_backend.h>

#include <chrono>

namespace stratum {
namespace protocol {
namespace rtps {

namespace {

// 从 BackendConfig 解析端口等后端特有配置
void ApplyExtra(const BackendConfig& config, uint16_t* meta_port,
                uint16_t* user_port, uint32_t* announce_ms) {
  for (const auto& kv : config.extra) {
    if (kv.first == "metatraffic_port") {
      *meta_port = static_cast<uint16_t>(std::stoi(kv.second));
    } else if (kv.first == "user_port") {
      *user_port = static_cast<uint16_t>(std::stoi(kv.second));
    } else if (kv.first == "announce_interval_ms") {
      *announce_ms = static_cast<uint32_t>(std::stoi(kv.second));
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// RtpsSource
// ---------------------------------------------------------------------------

RtpsSource::RtpsSource(const BackendConfig& config) {
  reader_config_.domain_id = config.domain_id;
  reader_config_.participant_name =
      config.participant_name.empty() ? "stratum_recorder" : config.participant_name;
  for (const std::string& t : config.topics) {
    reader_config_.topic_filter.insert(t);
  }
  ApplyExtra(config, &reader_config_.metatraffic_port, &reader_config_.user_port,
             &reader_config_.announce_interval_ms);
}

RtpsSource::~RtpsSource() { Stop(); }

Status RtpsSource::Start(SampleCallback callback) {
  Status s = reader_.Open(reader_config_);
  if (!s.ok()) return s;

  // 适配：RtpsReader 的 UserData → ISource 的 ReceivedSample
  return reader_.Start([callback](const UserData& ud) -> bool {
    ReceivedSample rs;
    rs.topic_name = ud.topic_name;
    rs.type_name = ud.type_name;
    rs.source_timestamp_ns = ud.source_timestamp_ns;
    rs.reception_timestamp_ns = ud.reception_timestamp_ns;
    rs.sequence = ud.sequence;
    rs.payload = ud.payload;
    rs.instance_key = ud.instance_key;
    // 返回 false 表示要求后端停止推送（Recorder 停止时使用）
    return callback(rs);
  });
}

Status RtpsSource::Stop() { return reader_.Stop(); }

bool RtpsSource::IsRunning() const { return reader_.IsRunning(); }

std::vector<std::string> RtpsSource::DiscoveredTopics() const {
  return reader_.DiscoveredTopics();
}

// ---------------------------------------------------------------------------
// RtpsSink
// ---------------------------------------------------------------------------

RtpsSink::RtpsSink(const BackendConfig& config) {
  writer_config_.domain_id = config.domain_id;
  writer_config_.participant_name =
      config.participant_name.empty() ? "stratum_player" : config.participant_name;
  ApplyExtra(config, &writer_config_.metatraffic_port, &writer_config_.user_port,
             &writer_config_.announce_interval_ms);
}

RtpsSink::~RtpsSink() { Close(); }

Status RtpsSink::Open() {
  if (open_) return Status::AlreadyExists("RtpsSink 已打开");
  // 【调试用丢包注入】用环境变量开启，便于测试"网络丢包时可靠传输是否收敛"。
  // 走环境变量而非公开配置项：它只服务于异常路径测试，不该进生产接口。
  if (const char* dv = std::getenv("STRATUM_DEBUG_DROP_SEND_PERCENT")) {
    writer_config_.debug_drop_send_percent =
        static_cast<uint32_t>(std::atoi(dv));
  }
  Status s = writer_.Open(writer_config_);
  if (!s.ok()) return s;
  s = writer_.Start();
  if (!s.ok()) return s;
  open_ = true;
  return Status::OK();
}

Status RtpsSink::Close() {
  if (!open_) return Status::OK();
  Status s = writer_.Stop();
  open_ = false;
  return s;
}

bool RtpsSink::IsOpen() const { return open_; }

Status RtpsSink::Publish(const ReceivedSample& sample) {
  if (!open_) return Status::InvalidArgument("RtpsSink 未打开");
  return writer_.Publish(sample.topic_name, sample.type_name, sample.payload,
                         sample.source_timestamp_ns);
}

Status RtpsSink::WaitForAcknowledgments(uint64_t timeout_ms) {
  if (!open_) return Status::InvalidArgument("RtpsSink 未打开");
  return writer_.WaitForAcknowledgments(timeout_ms);
}

// ---------------------------------------------------------------------------
// RtpsBackend
// ---------------------------------------------------------------------------

Status RtpsBackend::CreateSource(const BackendConfig& config, SourcePtr* out) {
  if (out == nullptr) return Status::InvalidArgument("out 为空");
  out->reset(new RtpsSource(config));
  return Status::OK();
}

Status RtpsBackend::CreateSink(const BackendConfig& config, SinkPtr* out) {
  if (out == nullptr) return Status::InvalidArgument("out 为空");
  out->reset(new RtpsSink(config));
  return Status::OK();
}

}  // namespace rtps
}  // namespace protocol
}  // namespace stratum
