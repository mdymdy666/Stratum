// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// examples/cpp/storage_demo.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// storage 模块 · 简单用法演示
//
// 存储层是 Stratum 的"地层"：时间轴上的数据以**有序键**落盘，
// 因此"按时间范围扫描"就是一次顺序读，而不是全表过滤。
//
// 这个有序性依赖一个关键设计：**键的字节序 == 时间序**。
// 本示例先验证这个不变量，再演示三类 API：
//
//   ① TimeKeyCodec  时间键编解码 + 三类范围构造（扫描的基础）
//   ② IStorageEngine 存储引擎抽象（用 MemoryEngine 演示，无 IO 依赖）
//   ③ MetaCodec     元数据编解码（会话/话题的自描述信息）
//
// 覆盖的 API：
//   storage::TimeKey / TimeKeyCodec::Encode / Decode / LooksLikeTimeKey
//   TimeKeyCodec::MakeTopicRange / MakeTimeRange / MakePrefixRangeUpTo
//   TimeKeyCodec::PeekTopicId / PeekTimestamp / PeekSeqInTs
//   storage::MemoryEngine::Open / Put / Get / NewIterator / Close
//   storage::MetaCodec::EncodeSession / DecodeSession / EncodeTopic / DecodeTopic
//
// 运行：./storage_demo
// ─────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "stratum/storage/memory/memory_engine.h"
#include "stratum/storage/meta_codec.h"
#include "stratum/storage/time_key.h"

using namespace stratum;
using namespace stratum::storage;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "OK" : "FAIL", what);
  if (!ok) ++g_failed;
}

}  // namespace

int main() {
  std::printf("=== storage_demo：时间键 / 存储引擎 / 元数据 ===\n\n");

  // ═══════════════════════════════════════════════════════════════════════
  // ① TimeKeyCodec：时间键编解码
  //
  // 一个时间键 = (topic_id, timestamp_ns, seq_in_ts)，共 20 字节：
  //   magic(4) + topic_id(4) + timestamp_ns(8) + seq_in_ts(4)
  // 前 4 字节是格式版本标记（0x53545231 = "STR1"），为将来格式演进留出识别手段。
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("[1] TimeKeyCodec：编解码\n");
  {
    const TimeKey key(7, 1700000000000000000ULL, 3);
    const std::string raw = TimeKeyCodec::Encode(key);
    Check(raw.size() == 20, "编码结果为 20 字节");

    TimeKey back;
    Check(TimeKeyCodec::Decode(raw, &back), "解码成功");
    Check(back.topic_id == 7, "topic_id 往返一致");
    Check(back.timestamp_ns == 1700000000000000000ULL, "timestamp 往返一致");
    Check(back.seq_in_ts == 3, "seq_in_ts 往返一致");
    Check(back == key, "整个 TimeKey 往返一致");

    // 零拷贝友好：不解码也能取出用于比较的字段
    Check(TimeKeyCodec::PeekTopicId(raw) == 7, "PeekTopicId 可直接提取");
    Check(TimeKeyCodec::PeekTimestamp(raw) == 1700000000000000000ULL,
          "PeekTimestamp 可直接提取");

    // 非法输入必须被明确拒绝（不是返回垃圾）
    Check(!TimeKeyCodec::Decode("short", &back), "长度不对时 Decode 返回 false");
    std::string bad = raw;
    bad[0] = '\0';   // 破坏 magic
    Check(!TimeKeyCodec::Decode(bad, &back), "magic 不对时 Decode 返回 false");
    Check(!TimeKeyCodec::LooksLikeTimeKey("short"), "LooksLikeTimeKey 能快速排除");
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ② 关键不变量：字节序 == 时间序
  //
  // 这是整个时间轴扫描能成立的前提。破坏它的症状很隐蔽：
  // 查询结果"差不多对"但顺序错乱。因此这里直接验证。
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[2] 关键不变量：字节序 == 时间序\n");
  {
    std::vector<TimeKey> keys = {
        {1, 100, 0}, {1, 200, 0}, {1, 200, 1}, {2, 50, 0}, {2, 50, 1},
    };
    // 按 TimeKey 的语义顺序排
    std::sort(keys.begin(), keys.end());

    // 编码后按**字节序**排
    std::vector<std::string> raws;
    for (const auto& k : keys) raws.push_back(TimeKeyCodec::Encode(k));
    std::vector<std::string> sorted_raws = raws;
    std::sort(sorted_raws.begin(), sorted_raws.end());

    Check(raws == sorted_raws,
          "TimeKey 排序结果与编码后的字节序排序完全一致");
    std::printf("      这是范围扫描能成立的根本原因\n");
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ③ 三类范围构造：扫描的基础
  //   MakeTopicRange        —— 某话题的全部数据
  //   MakeTimeRange         —— 某话题在某时间段内的数据
  //   MakePrefixRangeUpTo   —— 某话题"截至某时刻"的数据（定点查询用）
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[3] 三类范围构造\n");
  {
    std::string b, e;

    TimeKeyCodec::MakeTopicRange(5, &b, &e);
    Check(!b.empty() && !e.empty(), "MakeTopicRange 给出有效区间");
    // [begin, end) 恰好覆盖 topic 5 的全部键
    const std::string inside = TimeKeyCodec::Encode(5, 12345, 0);
    Check(inside >= b && inside < e, "该话题的任意键都落在区间内");
    const std::string other = TimeKeyCodec::Encode(6, 0, 0);
    Check(!(other >= b && other < e), "相邻话题的键不在区间内");

    TimeKeyCodec::MakeTimeRange(5, 1000, 2000, &b, &e);
    Check(TimeKeyCodec::Encode(5, 1500, 0) >= b &&
              TimeKeyCodec::Encode(5, 1500, 0) < e,
          "MakeTimeRange 覆盖区间内的键");
    Check(!(TimeKeyCodec::Encode(5, 2500, 0) >= b &&
            TimeKeyCodec::Encode(5, 2500, 0) < e),
          "MakeTimeRange 排除区间外的键");

    TimeKeyCodec::MakePrefixRangeUpTo(5, 2000, &b, &e);
    Check(TimeKeyCodec::Encode(5, 2000, 99) < e,
          "MakePrefixRangeUpTo 包含 ts == 上界的键（闭区间语义）");
    Check(!(TimeKeyCodec::Encode(5, 2001, 0) < e),
          "MakePrefixRangeUpTo 排除 ts 更大的键");
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ④ IStorageEngine：存储引擎抽象
  //
  // Timeline 只依赖这个接口，因此换后端（内存 / LSM）不动上层代码。
  // 这里用 MemoryEngine —— 它不需要真实文件系统，是所有实现的行为基准。
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[4] IStorageEngine（MemoryEngine）\n");
  {
    MemoryEngine engine;
    Status s = engine.Open("/tmp/storage_demo_engine");
    Check(s.ok(), "Open 成功");
    if (!s.ok()) {
      std::printf("      %s\n", s.ToString().c_str());
      return 1;
    }

    WriteOptions wo;
    ReadOptions ro;

    // 写入若干时间键（值用可辨识内容）
    for (uint32_t i = 0; i < 5; ++i) {
      const std::string k = TimeKeyCodec::Encode(1, 1000 + i * 100, 0);
      s = engine.Put(wo, k, "payload-" + std::to_string(i));
    }
    Check(s.ok(), "Put 成功");

    // 单点读
    const std::string k2 = TimeKeyCodec::Encode(1, 1200, 0);
    std::string v;
    s = engine.Get(ro, k2, &v);
    Check(s.ok() && v == "payload-2", "Get 取回正确值");

    // 不存在的键要**明确失败**（而不是返回空串当成成功）
    Status nf = engine.Get(ro, TimeKeyCodec::Encode(1, 99999, 0), &v);
    Check(!nf.ok(), "不存在的键返回明确失败");

    // 范围扫描：这就是"按时间查数据"的底层动作
    std::string b, e;
    TimeKeyCodec::MakeTopicRange(1, &b, &e);
    auto it = engine.NewIterator(ro, b, e);
    int n = 0;
    for (; it && it->Valid(); it->Next()) ++n;
    Check(n == 5, "范围扫描得到全部 5 条");

    // 按时间窗口扫描
    TimeKeyCodec::MakeTimeRange(1, 1100, 1400, &b, &e);
    auto it2 = engine.NewIterator(ro, b, e);
    int n2 = 0;
    for (; it2 && it2->Valid(); it2->Next()) ++n2;
    Check(n2 == 3, "时间窗口 [1100,1400) 扫到 3 条");

    engine.Close();
  }

  // ═══════════════════════════════════════════════════════════════════════
  // ⑤ MetaCodec：元数据自描述
  //
  // 录制目录必须"脱离原程序也能读懂"（见 06-data-trust.md），
  // 所以会话与话题的元信息是显式编码落盘的，而不是靠外部约定。
  // ═══════════════════════════════════════════════════════════════════════
  std::printf("\n[5] MetaCodec：会话与话题元数据\n");
  {
    SessionMetadata meta;
    meta.session_name = "demo-session";
    meta.stratum_version = "1.0.0";
    meta.created_at_ns = 1700000000000000000ULL;
    meta.total_events = 42;

    const std::string raw = MetaCodec::EncodeSession(meta);
    Check(!raw.empty(), "EncodeSession 产出非空");

    SessionMetadata back;
    Status s = MetaCodec::DecodeSession(raw, &back);
    Check(s.ok(), "DecodeSession 成功");
    Check(back.session_name == "demo-session", "会话名往返一致");
    Check(back.stratum_version == "1.0.0", "版本往返一致");
    Check(back.total_events == 42, "事件总数往返一致");
    Check(back.format_version == meta.format_version, "格式版本被保留");

    TopicMetadata t;
    t.topic_id = 3;
    t.topic_name = "demo/temp";
    t.type_name = "DemoTemp";
    const std::string traw = MetaCodec::EncodeTopic(t);
    TopicMetadata tback;
    s = MetaCodec::DecodeTopic(traw, &tback);
    Check(s.ok() && tback.topic_name == "demo/temp", "话题元数据往返一致");

    // 损坏的输入必须被明确拒绝
    Status bad = MetaCodec::DecodeSession("garbage", &back);
    Check(!bad.ok(), "损坏的元数据返回明确失败");
  }

  std::printf("\n");
  if (g_failed == 0) {
    std::printf("全部通过。下一步看 storage_full.cpp —— 三个真实场景。\n");
    return 0;
  }
  std::printf("有 %d 项未通过\n", g_failed);
  return 1;
}
