# ref-05：Fast DDS RTPS 依赖拆分分析

> 分析方式：源码扫描（目录结构、关键模块定位、依赖引用统计）
> 基准：`/UserData/Fast-DDS`（只读，未修改）

---

## 1 规模总览

| 项 | 数据 |
|---|---|
| `src/cpp/rtps/` 总行数 | **118,039 行** |
| 一级目录 | 17 个 |
| Stratum 需要比例（估算） | 约 20-30%（只订阅 + QoS 入口） |

一级目录：attributes / builtin / common / DataSharing / domain / exceptions /
flowcontrol / history / messages / network / participant / persistence /
reader / resources / security / transport / writer

---

## 2 关键模块定位（线格式兼容必需）

| 能力 | 位置 | 说明 |
|---|---|---|
| **消息编解码** | `rtps/messages/CDRMessage.{hpp,cpp}` | CDR 缓冲区读写（依赖 fastcdr） |
| **消息创建/解析** | `rtps/messages/RTPSMessageCreator.{hpp,cpp}` | Submessage 组装（Writer 侧为主） |
| **消息接收** | `rtps/messages/MessageReceiver.{h,cpp}` | Submessage 分发（**Reader 侧核心**） |
| **子消息** | `rtps/messages/submessages/` | 各 Submessage 的读写 |
| **SPDP** | `rtps/builtin/discovery/participant/PDPSimple.{h,cpp}` | 参与者发现（SPDP） |
| **SEDP** | `rtps/builtin/discovery/endpoint/EDPSimple.{h,cpp}`、`EDP.h` | 端点发现 |
| **匹配判定** | `rtps/builtin/discovery/endpoint/EDPUtils.hpp` | `valid_matching` 14 步（LearnNotes 已深读） |
| **QoS 类型** | `include/fastdds/dds/core/policy/QosPolicies.hpp` | 37 个策略定义 |
| **QoS 序列化** | `src/cpp/fastdds/core/policy/QosPoliciesSerializer.hpp` | 参数列表编解码 |
| **基础类型** | `include/fastdds/rtps/common/`（GUID/Locator/Time_t 等） | 无依赖 |

---

## 3 依赖情况（实测引用统计）

| 依赖 | 引用范围 | 移植判断 |
|---|---|---|
| **fastcdr**（CDR 序列化） | messages + builtin 中 12 个文件 | fastcdr 本身是轻量独立库 → **作为依赖库引入**（同 LSMengine 模式） |
| **foonathan_memory**（内存分配） | 仅 **6 个文件** | 极少 → 移植时用标准分配器替换这 6 处 |
| utils（日志/时间/线程池） | 广泛（Log、Time_t、线程） | 移植时替换为 Stratum 自己的最小实现 |

**结论**：真正的"重依赖"只有 fastcdr（可作独立库引入）；foonathan 仅 6 处，替换成本低。

---

## 4 三类移植清单

### A. 直接移植（无依赖或仅 fastcdr）

| 模块 | 预估 |
|---|---|
| CDRMessage 缓冲区读写 | ~1,500 行 |
| submessages 各子消息（Data/Heartbeat/AckNack/Info*/Gap） | ~4,000 行 |
| 基础类型（GUID/SequenceNumber/Locator/Time_t/InstanceHandle） | ~2,000 行 |
| QoS 类型定义（QosPolicies.hpp 37 策略） | ~3,000 行 |
| QosPoliciesSerializer | ~500 行 |

### B. 裁剪修改后移植（依赖替换：Log/线程/分配器）

| 模块 | 裁剪内容 |
|---|---|
| MessageReceiver | 替换 Log 与线程模型；**只保留 Reader 侧分发** |
| PDPSimple | 大幅裁剪（只订阅：被动响应 + 定期探测，去掉 Writer 公告） |
| EDPSimple + EDPUtils | 保留 `valid_matching` 判定；去掉本地端点公告 |
| History（ReaderHistory） | 保留接收缓存语义 |

### C. 参考实现不移植（强耦合业务逻辑）

| 模块 | 原因 |
|---|---|
| participant/（RTPSParticipant 创建链 22 跳） | 与 DomainParticipant 深度绑定；Stratum 自写最小 Participant |
| writer/ 全部 | 无发布需求 |
| security/ persistence/ statistics/ DataSharing/ flowcontrol | 不需要 |
| transport/（完整 TCP/UDP/SHM 栈） | 自写最小 UDP 接收（或引 asio） |

---

## 5 对里程碑的量化

| 里程碑 | 内容 | 预估规模 |
|---|---|---|
| M1 | 线格式 + QoS 编解码移植，固定向量验证 | ~8,000 行（含 QoS） |
| M2 | SPDP 最小实现（能被发现） | ~1,500 行 |
| M3 | SEDP 消费侧 + valid_matching + Reader 接收 | ~3,000 行 |
| M4 | ACKNACK + 互操作测试 | ~1,000 行 |
| M5 | IDdsBackend 接入 + 端到端 | ~800 行 |

合计约 **14,000 行**（其中约 11,000 行是移植，3,000 行自写）。

---

## 6 待确认项

- fastcdr 库的引入方式（作为第三方依赖构建 vs 直接源码进树）——倾向源码进树（与 LSMengine 模式一致，避免版本问题）
- QoS 兼容判定的确切实现位置（`valid_matching` 在 EDPUtils，但策略级兼容检查可能在 QosPolicies 类内）——移植时精读确认
- UDP 接收的最小实现（Fast DDS 的 transport 太重，倾向 asio 或裸 socket）
