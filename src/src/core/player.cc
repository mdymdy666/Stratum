// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// Player 实现：按时间轴回放
//
// ===========================================================================
// 速率控制的两个要点
// ===========================================================================
//
//  1. **不累积漂移**：用"目标时刻 - 当前时刻"计算睡眠时间，而不是
//     "每条都 sleep(interval)"。后者会因系统调度误差逐条累积，
//     回放越长偏差越大。
//
//  2. **不早于目标时刻发送**：若某条处理耗时超过间隔，不补偿性"加速"
//     （那会破坏时序保真），而是记入统计并继续。

#include "stratum/core/recorder.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace stratum {

Player::Player(ITimeline* timeline, protocol::IDdsBackend* backend)
    : timeline_(timeline), backend_(backend) {}

Player::~Player() = default;

void Player::RequestStop() {
  stop_requested_.store(true, std::memory_order_release);
}

PlaybackProgress Player::GetProgress() const {
  std::lock_guard<std::mutex> lock(progress_mutex_);
  return progress_;
}

Status Player::Play(const TimeRange& range, const PlayerConfig& config) {
  if (playing_.load()) {
    return Status::AlreadyExists("player already running");
  }
  if (timeline_ == nullptr) {
    return Status::InvalidArgument("timeline is null");
  }
  if (backend_ == nullptr) {
    return Status::InvalidArgument("backend is null");
  }

  playing_.store(true, std::memory_order_release);
  stop_requested_.store(false);

  // 打开数据汇
  // 【缺陷修复】这里曾经是 `protocol::BackendConfig bcfg;`（默认对象），
  // 调用方设置的 domain_id 被静默丢弃 —— 回放**永远发生在 domain 0**，
  // 而任何在其它域上等待的订阅端都收不到数据（实测：声称 domain=3 的
  // 回放进程实际绑定 domain 0 的 7400/9060）。这类"配置在中间层丢掉"
  // 的缺陷不会产生任何错误日志，只会让互操作长期对不上。
  protocol::BackendConfig bcfg = config.backend;
  protocol::SinkPtr sink;
  Status s = backend_->CreateSink(bcfg, &sink);
  if (!s.ok()) {
    playing_.store(false, std::memory_order_release);
    return s;
  }
  s = sink->Open();
  if (!s.ok()) {
    playing_.store(false, std::memory_order_release);
    return s;
  }

  // 等待发现完成（见 PlayerConfig::discovery_wait_ms 的说明）。
  // 支持中途停止：分片睡眠并检查 stop_requested_。
  if (config.discovery_wait_ms > 0) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(config.discovery_wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (stop_requested_.load(std::memory_order_acquire)) break;
      // 提前就绪就立刻开始，不必等满：对端 reader 在匹配建立前收不到
      // 第一条（VOLATILE 语义），等它就绪比固定等待更可靠。
      if (sink->ReadyForPublish()) {
        // 就绪后仍要给对端一个稳定期：VOLATILE durability 下 reader 不会
        // 索要"matched_writer_add 完成之前"发出的数据，首条发太早会永久
        // 丢失（实测回放 200 条稳定差第 1 条，且重发相同 SN 也无效）。
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  // 话题名映射表
  auto map_topic = [&config](const std::string& name) -> std::string {
    for (const auto& kv : config.topic_mapping) {
      if (kv.first == name) {
        return kv.second;
      }
    }
    if (config.allow_original_topic_names) {
      return name;
    }
    // 默认加后缀，避免与实时数据流冲突（对应 EC-18 的防呆设计）
    return name + "_replay";
  };

  // 预扫描：统计总数与时间范围
  {
    std::vector<TopicMetadata> topics;
    if (timeline_->ListTopics(&topics).ok()) {
      uint64_t total = 0;
      TimeRange effective = range;
      for (const auto& t : topics) {
        uint64_t n = 0;
        if (timeline_->CountRange(t.topic_id, range, &n).ok()) {
          total += n;
        }
        if (effective.begin_ns == kInvalidTimestamp ||
            (t.first_seen_ns != kInvalidTimestamp &&
             t.first_seen_ns < effective.begin_ns)) {
          effective.begin_ns = t.first_seen_ns;
        }
        if (t.last_seen_ns > effective.end_ns) {
          effective.end_ns = t.last_seen_ns;
        }
      }
      std::lock_guard<std::mutex> lock(progress_mutex_);
      progress_ = PlaybackProgress();
      progress_.begin_ns = effective.begin_ns;
      progress_.end_ns = effective.end_ns;
      progress_.cursor_ns = effective.begin_ns;
      progress_.total = total;
    }
  }

  const double rate = config.rate;
  const bool as_fast_as_possible = (rate <= 0.0);

  do {  // loop
    // 逐话题回放。为保持全局时序，这里采用"多路归并"的简化版本：
    // 收集全部事件后按时间戳排序再发送。
    //
    // 取舍：这需要把整个范围读进内存。对超大范围应改为真正的多路归并
    // 迭代器（记录为优化方向）；当前实现优先保证时序正确。
    struct Item {
      TimestampNs ts;
      protocol::ReceivedSample sample;
    };
    std::vector<Item> items;

    std::vector<TopicMetadata> topics;
    STRATUM_RETURN_IF_ERROR(timeline_->ListTopics(&topics));

    for (const auto& t : topics) {
      EventIteratorPtr it;
      Status qs = timeline_->QueryRange(t.topic_id, range, &it);
      if (!qs.ok()) {
        continue;
      }
      for (; it->Valid(); it->Next()) {
        Item item;
        item.ts = it->event().source_timestamp_ns;
        item.sample.topic_name = map_topic(t.topic_name);
        item.sample.type_name = t.type_name;
        item.sample.type_definition = t.type_definition;
        item.sample.source_timestamp_ns = it->event().source_timestamp_ns;
        item.sample.reception_timestamp_ns = it->event().reception_timestamp_ns;
        item.sample.sequence = it->event().seq;
        item.sample.payload = it->event().payload;
        item.sample.instance_key = it->event().instance_key;
        items.push_back(std::move(item));
      }
    }

    // 按源时间戳排序（保证回放时序与录制一致）
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.ts < b.ts; });

    TimestampNs first_ts = items.empty() ? 0 : items.front().ts;
    play_start_ = std::chrono::steady_clock::now();  // 本轮基准
    uint64_t published = 0;

    // 回放首条的副本，用于收尾补救（见下方说明）。
    // 只留 1 条：实测 VOLATILE 边界恒定只影响"匹配完成瞬间"的那一条，
    // 多留只会多制造重复内容。
    std::vector<protocol::ReceivedSample> head_samples;

    for (const auto& item : items) {
      if (stop_requested_.load(std::memory_order_acquire)) {
        break;
      }

      // --- 速率控制：基于目标时刻，不累积漂移 ---
      if (!as_fast_as_possible && item.ts > first_ts) {
        const uint64_t offset_ns = item.ts - first_ts;
        const uint64_t target_ns =
            static_cast<uint64_t>(static_cast<double>(offset_ns) / rate);
        // 与"从本轮起点起实际已耗时"比较，只睡差值。
        // 每步都以起点为基准重算，因此调度误差不会累积。
        const int64_t actual_elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - play_start_)
                .count();
        const int64_t remain =
            static_cast<int64_t>(target_ns) - actual_elapsed_ns;
        if (remain > 0) {
          std::this_thread::sleep_for(std::chrono::nanoseconds(remain));
        }
      }

      // 【不得忽略返回值】发布失败（例如样本超过 RTPS 单子消息上限、
      // 报文构造失败）必须可见，否则会表现为"回放端显示已发布 N 条、
      // 订阅端一条都收不到"这种最难查的静默失败。
      const Status pub_status = sink->Publish(item.sample);
      if (!pub_status.ok()) {
        if (publish_failed_.fetch_add(1, std::memory_order_relaxed) == 0) {
          std::fprintf(stderr, "[player] 发布失败（后续同类失败只计数）: %s\n",
                       pub_status.ToString().c_str());
          std::fflush(stderr);
        }
        continue;
      }
      if (head_samples.empty()) {
        head_samples.push_back(item.sample);
      }
      if (config.publish_callback) {
        config.publish_callback(item.sample);
      }
      ++published;

      {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.cursor_ns = item.ts;
        progress_.published = published;
      }
    }

    // 【发送窗口统计】—— 外部用 shell 计时会把进程启动（Fast DDS 初始化 +
    // SPDP 发现，约 1~2 秒）与收尾的 ACK 等待（最多 ack_timeout_ms = 5 秒）
    // 一起算进去，导致吞吐被严重低估（实测曾据此得出错误的「420 条/秒」）。
    {
      const int64_t window_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - play_start_)
              .count();
      const double secs = static_cast<double>(window_ns) / 1e9;
      std::fprintf(stderr,
                   "[player] 发送窗口: %llu 条 / %.4f 秒 = %.0f 条/秒（仅发送循环）\n",
                   static_cast<unsigned long long>(published), secs,
                   secs > 0.0 ? static_cast<double>(published) / secs : 0.0);
      std::fflush(stderr);
    }

    if (config.wait_for_ack) {
      sink->WaitForAcknowledgments(config.ack_timeout_ms);

      // 【收尾补救：把首条用**新 SN** 重新发布】
      //
      // 为什么不能靠 ACKNACK 判断：`base_sn` 只表示"小于它的 SN 我都收到了"，
      // 而 VOLATILE reader 对"匹配完成前发出的数据"是**不想要**（并非收到）。
      // 实测依赖 acked 标志会既救不回被丢弃的首条、又反复重发其它样本
      // （对账日志出现大量重复与倒序缺口）。
      // 直接用新 SN 重发首条：新 SN 落在 reader 的 changesFromRL 之后，
      // 因此一定被接收；代价是最多多出 1 条重复内容。
      if (config.republish_unacked && !config.loop) {
        for (const protocol::ReceivedSample& s : head_samples) {
          if (stop_requested_.load(std::memory_order_acquire)) {
            break;
          }
          if (sink->Publish(s).ok()) {
            republished_.fetch_add(1, std::memory_order_relaxed);
          }
        }
        sink->WaitForAcknowledgments(config.ack_timeout_ms);
      }
    }   // if (config.wait_for_ack)

    {
      std::lock_guard<std::mutex> lock(progress_mutex_);
      progress_.published = published;
      if (!config.loop) {
        progress_.finished = true;
      }
    }

    if (stop_requested_.load(std::memory_order_acquire)) {
      break;
    }
  } while (config.loop);

  sink->Close();
  playing_.store(false, std::memory_order_release);
  return Status::OK();
}

}  // namespace stratum
