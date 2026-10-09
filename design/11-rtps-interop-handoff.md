# RTPS 互通：下一阶段交接

> ## ⚠️ 本文档的假设已被推翻，请以 [`12-rtps-interop-rootcause.md`](12-rtps-interop-rootcause.md) 为准
>
> 本文第 1 节要求"写 Fast DDS 诊断程序拿拒绝原因"、第 6 节给出
> "Fast DDS 拒绝了 Publication"的因果链 —— **两者都不成立**。
>
> 实测（gdb + 原始抓包）证明：**Stratum 从来没有把 Publication 发出去**。
> `type_name` 在存储层 `StratumDB::ListTopics` 被丢掉 →
> `BuildPublication` 返回空 vector → 旧代码 `if (!pub.empty())` **静默跳过发送**。
> 报文根本没离开本机，因此"补 QoS / 补 KEY_HASH / 改 DATA_REPRESENTATION"
> 全都无效。
>
> 真正的两个缺陷与修复见 12 号文档；续 16 有完整时序记录。
> **本文档第 5 节的"10 条可直接采信的事实"仍然有效**，可继续使用。

> **本文档面向新开的 session。**
> 上一 session 的上下文已被多次压缩，精确细节（行号、字节级证据）可能失真，
> 故把"可直接采信的事实"与"当前唯一卡点"固化在此，避免重新踩坑。
>
> 详细的时序记录见 [`AI_COLLAB.md`](../AI_COLLAB.md)（14 篇），本文只做索引与提炼。

---

## 0. 一句话现状

Stratum（DDS 感知录制器）已能把**真实 DDS 流量录进 LSM-KV 并回放**；
**Stratum ↔ Stratum 的 RTPS 互通完全打通**；
**与 Fast DDS 的互通卡在"我方 SEDP Publication 未被接受"**。

---

## 1. 唯一最高优先级任务

**写一个最小的自定义 Fast DDS 订阅诊断程序**，目的是拿到 Fast DDS
拒绝我方 Publication 的**确切原因**。

已排除的两条路（不要再试）：

| 尝试 | 结果 |
|---|---|
| `FASTDDS_LOG_VERBOSITY` 环境变量 | libfastdds 中**不存在**该符号 |
| XML `<log><verbosity>` | Fast DDS 3.6 的 `parseLogConfig` **不接受**该标签，报 `Not expected tag: 'verbosity'` |

**做法**：

1. 复制 `/UserData/Fast-DDS/build-fastdds/examples/cpp/hello_world/` 的
   `HelloWorld` TypeSupport 生成文件（`.cxx`/`.h`）作为类型支持，
   **不要重新生成 IDL**（省时且与已录数据一致）。
2. 程序要点：
   - `eprosima::fastdds::dds::Log::SetVerbosity(Log::Kind::Info);`
   - 建 participant(domain 0) + subscriber + reader（topic `hello_world_topic`）
   - 用 `DataReaderListener` 实现 `on_subscription_matched`
     （打印 `status.current_count` 与失败原因）
     与 `on_data_available`
3. 运行方式：**先启动它**，再用 `tools/stratum_rtps_replay` 回放
   `/tmp/tl_final`（已有 32 条 `hello_world_topic` 数据）。
4. 观察：Fast DDS 会打印 `INCOMPATIBLE QOS (...)` 或参数表解析警告,
   直接指出是哪一条策略/哪个参数出了问题。

**预期**：`EDP::valid_matching` 的每条失败分支都有 `EPROSIMA_LOG_WARNING`，
开启 Info 级别后必然能看到具体分支（reliability / durability / ownership /
deadline / liveliness / data_representation）。

---

## 2. 硬约束（违反会破坏上游或环境）

### 上游 Fast DDS 仓库只读

`/UserData/Fast-DDS/` —— **绝不修改已跟踪文件**，绝不 `git add/commit/checkout/stash/reset`。
需保留的未跟踪项：`build/`、`build-fastdds/`、`build-tools/`、`install/`、
`install-tools/`、`_deps/`、`docs/`、`UserShow/`、`DEPENDENCIES_WSL.md`。
新增笔记只放 `LearnNotes/`。

### 终端禁止 PowerShell

- Linux/WSL 任务：`wsl bash -lc "..."`
- Windows 杂务：写 Python 脚本到 `.dsh_tmp/`，`python E:\...\.dsh_tmp\x.py` 执行，
  脚本 `finally` 自删；**禁止 `python -c`**
- 用 `write` 工具时注意：`\\wsl.localhost\...` 路径**读会 EIO、写可以**。
  需要读改 WSL 文件时，用「写到 `.dsh_tmp/` → `wsl cp` → WSL 侧 python 处理」

### 项目路径

```
WSL 侧:  /UserData/CodexWorkSpace/Stratum
Windows: \\wsl.localhost\Ubuntu\UserData\CodexWorkSpace\Stratum
```

---

## 3. 常用命令

```bash
# 构建
cd /UserData/CodexWorkSpace/Stratum
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

# 测试（30 套件，应全绿）
cd build && ctest

# 运行测试所需环境（Fast DDS 运行库）
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:\
/opt/ros/jazzy/lib:\
/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH
```

**依赖**：fastcdr 2.2.7（`/opt/ros/jazzy/include/fastcdr`）、
LSMengine（`/UserData/LeaningLevelDB/LSMengine`，静态库 `build-lib/libLSMengine.a`）。
`thirdparty/asio` 与 `thirdparty/fastcdr` 是**未初始化的空子模块**（无外网）。

**测试纪律**：测试工作目录必须是 `build/`；向量文件缺失必须**失败**而非跳过；
发现类测试必须用**非 0 domain**（默认 5），因为 ROS 2 daemon 占用 domain 0 的 7400。

---

## 4. 代码地图

### 协议引擎（自研 RTPS，本项目核心）

| 文件 | 职责 |
|---|---|
| `src/include/stratum/protocol/rtps/rtps_reader.{h,cc}` | **订阅端引擎**：SPDP 收发 / SEDP 解析 / QoS 匹配 / 发布 Subscription / ACKNACK / 收用户数据 |
| `src/include/stratum/protocol/rtps/rtps_writer.{h,cc}` | **发布端引擎**：SPDP / Publication / DATA / HEARTBEAT / 重传 |
| `src/include/stratum/protocol/rtps/rtps_packet_utils.{h,cc}` | **共享报文构造**（纯函数，可单测）：头+INFO_DST、HEARTBEAT、ACKNACK、SEDP 公告、DATA |
| `src/include/stratum/protocol/rtps/rtps_backend.{h,cc}` | 适配 `IDdsBackend`：`RtpsSource`(录制) / `RtpsSink`(回放) |

### 已移植的 Fast DDS 基础层（`src/include/stratum/rtps/`，53 文件与上游逐行一致）

| 文件 | 职责 |
|---|---|
| `builtin/spdp_parser.{h,cc}` | SPDP 公告解析 |
| `builtin/spdp_announcer.{h,cc}` | SPDP 公告构造 |
| `builtin/sedp_parser.{h,cc}` | SEDP 端点解析（话题/类型/GUID/locator/QoS）|
| `builtin/qos_matcher.{h,cc}` | **QoS 兼容判定**（11 条规则，移植自 `EDP::valid_matching`）|
| `messages/submessage_parser.{h,cc}` | 子消息解析（移植自 `MessageReceiver.cpp`）+ `DataTypePrefixSize` |
| `core/policy/ParameterSerializer.hpp` | 参数表序列化 |

### 工具（`tools/`）

| 工具 | 用途 |
|---|---|
| `stratum_rtps_record.cc` | **端到端录制**（Recorder + RtpsBackend）|
| `stratum_rtps_replay.cc` | **端到端回放**（Player + RtpsBackend）|
| `rtps_reader_diag.cc` | 订阅端协议诊断（逐秒打印 pkt/spdp/sedp/peers/samples）|
| `rtps_sink_diag2.cc` | 发布端协议诊断（逐秒打印 pkt/spdp/sedp_sub/peers/pubs/acks）|
| `dump_user_pkt.cc` | 单包字节 dump |

**环境变量**：`STRATUM_DUMP=1` 打开收包/发送目标 dump（`[dump]`/`[send]` 行到 stderr）。

---

## 5. 可直接采信的事实（不必重验）

| 事实 | 证据 |
|---|---|
| 报文构造层正确 | 单测 `RtpsPacket.AnnouncementRoundTrip`：用 `SedpParser` 反向解析自己构造的公告，topic/type/GUID/QoS 全对 |
| DATA 子消息前缀**不能按版本号判断** | 存在"版本号 2.2 却含 `extraFlags`"的报文；现用 `DataTypePrefixSize` 按 `octetsToInlineQoS==16` 数据驱动判定 |
| 参数 length 与规范字面不符 | `PID_PROTOCOL_VERSION`/`PID_VENDORID` 实测长度是 **4**（规范字面 2）；写 2 会被 Fast DDS **静默跳过整个参数表** |
| SPDP 组播端口必须单独监听 | `7400 + domain*250`；`IP_ADD_MEMBERSHIP` 只在目标端口 == socket 绑定端口时生效 |
| 组播会回环给自己 | 必须按 GUID 前缀过滤自身公告，否则 `peers` 里只有自己 |
| ACKNACK 位图必须置位 | 只设 base 时对端认为 reader 已拥有全部数据，不重传 |
| ACKNACK 必须发到**学到的对端 metatraffic locator** | 回源地址（可能是组播）无效。**这个坑踩过两次** |
| `octetsToNextHeader` 必须直接写 buffer | 用 `CDRMessage::addUInt16` 回填会 `length += 2` 污染消息总长 |
| `std::mutex` 不可重入 | `HandleSedpSubscription` 曾持锁调用 `FlushPendingToPeers()`（后者也加锁）→ **自死锁**，且因 stdout 全缓冲而完全静默 |
| Fast DDS 3.6 **不发送** `PID_TOPIC_KIND` | 查证 `WriterProxyData.cpp` 后确认，无需实现 |

---

## 6. 当前卡点与因果链

**已经闭合的推论**（来自 `STRATUM_DUMP=1` 的实测）：

```
[send] has_user=0 -> 10.255.255.254:7410  bytes=172     ← 全部 32 条都这样
[send] has_user=0 -> 172.17.211.143:7438  bytes=172
```

`has_user` **只能**由 `HandleSedpSubscription` 设置，而它只在收到对端
SEDP Subscription 时被调用。`has_user=0` ⇒ **对端没发 Subscription**
⇒ **对端未接受我方 Publication** ⇒ 我们学不到对端 user locator
⇒ 数据按"退回 metatraffic"分支发出（7410/7438 是 Fast DDS 的**发现端口**，
其 DataReader 不在此接收用户数据）⇒ 对端 0 条。

**对照**：同一个 Stratum Writer 对 Stratum Reader 完全正常（41 条，
`sedp_sub=51`、`acks=100`），故差距在 **Fast DDS 对 Publication 内容的要求**。

**已补但尚未见效的字段**（可能是差一部分，也可能是别的原因）：

| PID | 内容 |
|---|---|
| `0x0070` KEY_HASH | 端点自身 GUID（Fast DDS 约定 unkeyed 话题填 GUID）|
| `0x0060` TYPE_MAX_SIZE_SERIALIZED | 0 |
| `0x0073` DATA_REPRESENTATION | 改为 `{XCDR1, XCDR2}`（原只声明 XCDR1）|
| `0x001a` RELIABILITY | RELIABLE(2) |
| `0x001d` DURABILITY | VOLATILE(0) |

**建议**：先用第 1 节的诊断程序拿到 Fast DDS 的**原话**，
再决定补什么 —— 不要盲目继续加 PID。

---

## 7. 测试标准（项目要求，不可降级）

- **严禁以冒烟测试作为验收成果**
- 必须有：**压力测试**（量化指标）、**随机测试**、**真实场景 ≥5 个且覆盖全部待验证功能**
- 真实流量向量缺失时**必须失败**，不得静默跳过
- 现有 30 套件、31 个 `stratum_add_test` 注册，应保持全绿

---

## 8. 新 session 建议的启动方式

把下面这段直接发给新 session：

```
接手 Stratum 项目（DDS 感知录制器，WSL 路径
/UserData/CodexWorkSpace/Stratum）。

请先读这三个文件建立上下文，不要凭猜：
1. design/11-rtps-interop-handoff.md   ← 交接文档（事实清单 + 当前卡点）
2. AI_COLLAB.md 的最后 3 篇            ← 最近的时序记录
3. design/09b-qos-required.md          ← QoS 匹配要求

硬约束：上游 /UserData/Fast-DDS 只读；禁止 PowerShell；
WSL 侧操作走 wsl bash -lc；构建用 cmake -S . -B build -G Ninja。

当前唯一任务：按交接文档第 1 节，写一个最小的自定义 Fast DDS
订阅诊断程序（复用 hello_world 的 HelloWorld TypeSupport），
开启 Log::SetVerbosity(Info)，拿到 Fast DDS 拒绝我方 SEDP
Publication 的确切原因。

测试标准见交接文档第 7 节，禁止用冒烟测试交差。
```

---

## 9. 为什么建议新开 session（供决策参考）

| 事实 | 影响 |
|---|---|
| 上一 session 上下文已被**多次压缩** | 早期内容只剩摘要，行号/字节级证据可能失真 |
| 累计 14 篇记录、19 个已修缺陷 | 长上下文下容易"以为记得"而重犯已记录的错误 |
| 下一步任务**自包含**（约 100 行诊断程序）| 不需要长上下文，正是新 session 的理想起点 |
| 本文档 + AI_COLLAB + design/ 21 篇 | 上下文重建成本低 |
