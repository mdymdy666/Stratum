# Stratum C++ 示例

给 **C++ 程序员**的接口用法示例。每个示例都是**独立可执行文件**，只依赖 Stratum 的
核心库（**不需要 Fast DDS 头文件**），演示如何把 Stratum 作为库嵌入自己的程序。

---

## 快速开始

```bash
# 1) 先构建 Stratum 库
cmake -S . -B build -G Ninja && cmake --build build

# 2) 构建示例
cmake -S examples/cpp -B build-examples -G Ninja
cmake --build build-examples

# 3) 跑（选一个）
./build-examples/04_custom_backend /tmp/tl 8      # 最独立，先看这个
./build-examples/02_query_timeline /tmp/tl
./build-examples/03_playback_control /tmp/tl
./build-examples/01_record_and_replay /tmp/tl 0 6 # 需要真实 DDS 流量
```

---

## 示例清单

| 示例 | 演示的 API | 需要网络 | 实测结果 |
|---|---|---|---|
| **`01_record_and_replay`** | `TimelineImplV2` + `RtpsBackend` + `Recorder` + `Query` + `Player` | ✅ 是 | 录制 78 条 → 回放 117 条（100%） |
| **`02_query_timeline`** | `Query::ListTopics / GetInfo / FindTopic / ExportRange / GetStateAt` | ❌ 否 | 4 个 API 全部返回正确结果 |
| **`03_playback_control`** | `Player::Play` 的区间/速率/进度/回调、`RequestStop`、自定义 `ISink` | ❌ 否 | 区间回放 4/8 条、进度 71.3% |
| **`04_custom_backend`** | **`IDdsBackend` / `ISource` / `ISink` 扩展点** | ❌ 否 | 内存后端完整跑通 |

---

## 框架的 API 分层

```
你的程序
   │
   ├── Recorder ──┐                    Recorder(ITimeline*, IDdsBackend*)
   ├── Player   ──┤  用这两个编排       Player  (ITimeline*, IDdsBackend*)
   ├── Query    ──┘                    Query   (ITimeline*)
   │
   ├── ITimeline        存储抽象（自带 TimelineImplV2，基于 LSMengine）
   └── IDdsBackend      协议抽象（自带 RtpsBackend，即本项目的 RTPS 栈）
          ├── ISource   数据来源（录制方向）
          └── ISink     数据去向（回放方向）
```

**两个抽象都可以替换**：换存储不动协议，换协议不动存储 —— 示例 04 演示了后者。

---

## 各示例要点

### 01 · 录制与回放（最短路径）

关键点（都在源码注释里写明原因）：

- **`RecorderConfig::backend` / `PlayerConfig::backend` 必须显式赋值**
  —— 否则内部会用默认构造的 `BackendConfig`，**调用方设置的 domain 被静默丢弃**
  （表现为"无论传什么域都跑在 domain 0"）；
- **`PlayerConfig::discovery_wait_ms` 必须留出发现时间** —— 回放器以 RTPS Writer 身份
  发布，对端要先通过 SPDP/SEDP 完成匹配才会接收；
- **`republish_unacked` 默认开启** —— VOLATILE durability 下 reader 不索要匹配前发出的
  数据，首条会永久丢失（重发相同 SN 无效），该选项用**新 SN** 补回。

**真实链路运行方式**（回放需要一个订阅端）：

```bash
# 终端 1：发布者（供录制）
build/tools/fastdds_pub_diag --domain=78 --topic=hello_world_topic --count=200 --rate=20

# 终端 2：订阅端（供回放）
build/tools/fastdds_sub_diag 120 78 hello_world_topic

# 终端 3：示例本体
./build-examples/01_record_and_replay /tmp/tl 78 6
```

### 02 · 查询（Stratum 区别于普通录制工具的地方）

DDS 只有"现在"，没有"过去"。这四个 API 就是补上的那一层：

| API | 回答的问题 |
|---|---|
| `ListTopics` | 录到了哪些话题、各多少条、时间跨度多少 |
| `GetInfo` | 这份录制的总体信息（格式版本、创建时间、总量） |
| `FindTopic` | 按名字找到话题（拿到稳定的 `topic_id`） |
| `ExportRange` | 某话题某时间段的**事件序列** |
| `GetStateAt` | **某一时刻的状态是什么** |

### 03 · 回放控制

演示控制面：时间区间、速率（`0` = 尽快）、进度读取、发布回调、中途停止、话题重映射。
示例用**内存 sink** 以便自包含；要接真实网络，把 `CountingBackend` 换成
`stratum::protocol::rtps::RtpsBackend` 即可。

### 04 · 自定义后端（扩展点）

实现三个接口即可接入任意数据源/去向：

```cpp
class MySource : public protocol::ISource {
  Status Start(protocol::SampleCallback cb) override;  // 回调在自己线程上，不得阻塞
  Status Stop() override;
  bool   IsRunning() const override;
  std::vector<std::string> DiscoveredTopics() const override;
};

class MySink : public protocol::ISink {
  Status Open() override;
  Status Close() override;
  bool   IsOpen() const override;
  Status Publish(const protocol::ReceivedSample&) override;
  bool   ReadyForPublish() const override;              // 有发现过程就返回真实状态
  Status WaitForAcknowledgments(uint64_t) override;     // 不支持可返回 NotSupported
};

class MyBackend : public protocol::IDdsBackend {
  Status CreateSource(const protocol::BackendConfig&, protocol::SourcePtr*) override;
  Status CreateSink  (const protocol::BackendConfig&, protocol::SinkPtr*)   override;
  Capabilities GetCapabilities() const override;
};
```

**`Recorder` 和 `Player` 一行都不用改** —— 这就是这个抽象的价值。

---

## 编译说明

示例用独立的 `CMakeLists.txt`（`examples/cpp/`），因为它只需要 Stratum 的静态库：

```
build/src/libstratum_rtps_protocol.a
build/src/libstratum_rtps_base.a
build/src/libstratum_recorder.a
build/src/libstratum_timeline.a
build/src/libstratum_lsm_v2.a
build/src/libstratum_storage_lsm.a
build/src/libstratum_storage_memory.a
build/src/libstratum_core.a
```

这些库之间有循环依赖，链接时需要 `-Wl,--start-group ... -Wl,--end-group`
（`CMakeLists.txt` 里已处理）。

---

## 踩过的坑（写示例时亲历）

| 现象 | 原因 |
|---|---|
| `RtpsBackend is not a member of stratum::protocol` | 它在 **`stratum::protocol::rtps`** 下 |
| `reference to 'rtps' is ambiguous` | 同时 `using` 了 `stratum` 与 `stratum::protocol`，`rtps` 有两处可见 → 用全限定名 |
| `invalid new-expression of abstract class` | `ISink::WaitForAcknowledgments` 是纯虚，忘了实现 |
| `BackendConfig::extra` 编译不过 | 它是 **`vector<pair<string,string>>`**，不是 `map` |
| `RecorderStats does not name a type` | 统计类型是嵌套的 **`Recorder::Stats`** |
