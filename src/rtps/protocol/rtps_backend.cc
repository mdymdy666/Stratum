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

// 从环境变量 STRATUM_INITIAL_PEERS 解析"单播 SPDP 对端列表"。
//
// 为什么需要（跨主机发现的必要条件）：SPDP 默认只发组播 239.255.0.1，
// 而组播跨不过 NAT 边界 —— 实测本机 WSL 为 nat 模式时，与局域网内虚拟机的
// 组播互不可达，双方永远发现不了彼此。这正是 Fast DDS initialPeersList
// 要解决的问题；这里用环境变量传入，避免改动工具/公共接口。
// 格式：STRATUM_INITIAL_PEERS="192.168.100.129,10.0.0.5"
// 从环境变量 STRATUM_ADVERTISED_ADDRESS 读取"对外声明的地址"。
//
// 为什么需要（跨主机场景）：当本端位于 NAT 之后时，**本机探测到的地址
// 对端并不一定可达** —— 例如 WSL 在 NAT 里探测到 172.17.211.143，而局域网内的
// 虚拟机根本到不了这个地址。此时必须显式声明一个"对端可达的地址"
// （典型做法是声明宿主机的 NAT 网关地址，再由宿主机上的 UDP 中继转发）。
// 见 tools/udp_relay.py 与 AI_COLLAB.md 续 50/51。
void ApplyAdvertisedAddressFromEnv(std::string* out) {
  const char* v = std::getenv("STRATUM_ADVERTISED_ADDRESS");
  if (v == nullptr) return;
  std::string s(v);
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  if (!s.empty()) *out = s;
}

void ParseInitialPeersFromEnv(std::vector<std::string>* out) {
  const char* v = std::getenv("STRATUM_INITIAL_PEERS");
  if (v == nullptr) return;
  const std::string s(v);
  size_t pos = 0;
  while (pos <= s.size()) {
    const size_t comma = s.find(',', pos);
    std::string item = s.substr(
        pos, comma == std::string::npos ? std::string::npos : comma - pos);
    while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
      item.erase(item.begin());
    }
    while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
      item.pop_back();
    }
    if (!item.empty()) out->push_back(item);
    if (comma == std::string::npos) break;
    pos = comma + 1;
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
  ParseInitialPeersFromEnv(&reader_config_.initial_peers);
  ApplyAdvertisedAddressFromEnv(&reader_config_.advertised_address);
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

  ParseInitialPeersFromEnv(&writer_config_.initial_peers);
  ApplyAdvertisedAddressFromEnv(&writer_config_.advertised_address);
  if (const char* mv = std::getenv("STRATUM_MAX_UDP_PAYLOAD")) {
    writer_config_.max_udp_payload = static_cast<uint32_t>(std::atoi(mv));
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
