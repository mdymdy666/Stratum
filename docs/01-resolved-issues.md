# 01 · 已解决的问题清单（现象 → 根因 → 修复 → 验证）

> 来源：`AI_COLLAB.md` 续 16~36 的逐轮记录。**每条都有实测证据**，不是推测。
> 相关的根因分析见 [`../../design/12-rtps-interop-rootcause.md`](../../design/12-rtps-interop-rootcause.md)；
> 验证脚本见 [`03-verification-matrix.md`](03-verification-matrix.md)。

---

## 一、按"问题模式"归类（比逐条更有用）

本项目反复出现**四类**问题，后续排查建议优先怀疑它们：

| 模式 | 表现 | 本项目的实例 |
|---|---|---|
| **① 静默失败** | 计数为"已成功"、对端却什么都没收到；无任何报错 | 6 处（见 §二.1） |
| **② 共用标识 / 重复实现** | 同一份逻辑存在两个副本，只修了一个；多个对象共用一个 ID/计数器 | 4 处（见 §二.2） |
| **③ 声明与实际不一致** | 本端按 A 判定"兼容"，对端按 B 判定"不兼容" | 3 处（见 §二.3） |
| **④ RTPS 协议细节** | 偏移、位序、entityId 语义、时间戳语义 | 5 处（见 §二.4） |

---

## 二、逐条记录

### 1. 静默失败类（最重要的一类）

#### 1.1 SEDP Publication 静默不发
* **现象**：Fast DDS 侧完全看不到 Stratum 的 Writer；Stratum 没有任何报错。
* **根因**：`StratumDB::ListTopics` 解码 topic 元数据时**丢失了 `type_name`**
  → Player 发出的样本 `type_name` 为空 → `PID_TYPE_NAME` 序列化失败
  → `BuildPublication` 返回空 vector → 旧代码 `if (!pub.empty())` **静默跳过发送**。
* **证据**：gdb 断点看返回值 —— `BuildPublication(topic="hello_world_topic", type="")`
  → `Value returned is $1 = std::vector of length 0, capacity 0`。
* **修复**：`ListTopics` 回填 `type_name`；`BuildPublication` 失败改为
  **返回明确错误 + 计数 + stderr 提示**。
* **验证**：`scenarios/01_stratum_replay.sh`、`scenarios/07_upstream_subscriber.sh`。

#### 1.2 发送失败被忽略
* **现象**：回放端显示"已发布 N 条"，订阅端**一条都收不到**。
* **根因**：`sendto` 的返回值只判断 `> 0`，失败（`EMSGSIZE` 等）没有记录。
* **修复**：新增 `st_send_errors_` 计数 + `STRATUM_DUMP` 下打印 errno。
* **验证**：`scenarios/14_random_suite.sh`（70000 B 档必须显式失败）。

#### 1.3 `CDRMessage::addData` 失败被忽略 → 发出"空负载"报文
* **现象**：payload 8000 B，实际发出的报文**只有 56 字节**；`sendto` 成功。
* **根因**：**重复实现**（见 2.2）里用固定 2048 字节缓冲，超限时 `addData` 返回
  false 而返回值被忽略 → 只写了头部。
* **修复**：`addData` 失败时打印 `payload/cap/max_size/pos/length` 并返回空报文
  （上层据此计为构造失败，而不是发出去一份坏数据）。
* **验证**：`scenarios/09_payload_sizes.sh`。

#### 1.4 `Player` 忽略 `Publish` 的返回值
* **现象**：样本超过 RTPS 单子消息上限时，"已发布样本"照常计数。
* **修复**：`Player` 统计 `publish_failed_` 并打印首条失败原因（后续同类只计数）。
* **验证**：`scenarios/14`（70000 B 档：`已发布样本=0` + 明确错误）。

#### 1.5 延迟统计只收集、不输出
* **现象**：诊断程序的 `lat_ms` 在增长，但日志里没有 `DIAG_LATENCY_MS`。
* **根因**：插入打印的脚本走了错误分支（`if old4 not in t` 判断反了），代码没被插入。
* **修复**：补上分位输出。
* **验证**：`scenarios/15_latency_jitter.sh`。

#### 1.6 `max < p99` 的统计 bug
* **现象**：`max=0.143` 而 `p99=0.241` —— 自相矛盾。
* **根因**：`printf` 的**参数求值顺序未定义**，代码却依赖"某个参数先做 in-place 排序、
  之后 `back()` 才是最大值"。
* **修复**：显式先排序，再取分位与 max。
* **验证**：同上（修正后 `max=0.429 > p99=0.363`）。

### 2. 共用标识 / 重复实现类

#### 2.1 SEDP Publication 与用户数据**共用 SN 计数器**
* **现象**：心跳声明 `first=1 last=65`，而实际只发布了 32 条 —— 对端认为有缺口，
  **一条样本都不交付**。
* **根因**：`BuildPublication` 与 `Publish` 都做 `++next_sn_`；而 Publication 每 100ms 重发，
  把用户数据的 SN 号段"吃掉"，实际发出 1,5,9,… 这类跳号序列。
* **修复**：新增独立的 `sedp_sn_`。
* **验证**：心跳变成 `first=1 last=32`（与 32 条完全一致）。

#### 2.2 **重复实现**：`RtpsWriter::BuildDataPacket`（固定 2048 缓冲）
* **现象**：payload ≥ ~2 KiB 时回放**完全不可达**（8000 B: 0/20；60000 B: 0/10）。
* **根因**：`packet::BuildDataPacket` **早已修好**这个问题（注释明确记录了同一个坑，
  `cap = payload.size() + 512`，并由 `RtpsPacket.DegenerateInputs` 用 8 KiB 负载守护），
  但 `RtpsWriter` 里的**成员副本没有同步修改**。
* **修复**：成员函数改为**委托**给 `packet::BuildDataPacket`，彻底消除分叉。
* **验证**：`scenarios/09_payload_sizes.sh`（1200/8000/60000 B 全部完整）。

#### 2.3 多话题**共用 writer entityId + 全局 SN**
* **现象**：多话题并发会串扰/丢话题（RTPS 中一个 DataWriter 只能对应一个 topic）。
* **根因**：`my_writer_entity_` 固定 `0x00000103`；`next_sn_` 全局共享。
* **修复**：每话题独立 entityId（`entityKey` 递增，key=1 即原值 → 单话题零回归）；
  每 writer 独立 SN；`SendHeartbeats` 遍历所有 writer。
* **验证**：`scenarios/16_multitopic.sh`（两话题样本数正确、**区间外 0 条**）。

#### 2.4 学到的 user locator **写给所有 peer**
* **现象**：多订阅端时只有一个能收到数据。
* **修复**：按 Subscription 报文的发送方 GUID 前缀**定向**设置对应 peer 的 locator。
* **验证**：`[send] has_user=1 -> …:7411 / :7413 / :7439` 各自独立。

### 3. 声明与实际不一致类

#### 3.1 `Player`/`Recorder` 丢弃调用方的 **domain**
* **现象**：进程打印 `domain=3`，实际绑的是 **domain 0** 的端口（9060）。
  非 0 域永不互通，两端零报错。
* **根因**：`Player::Play` / `Recorder::Start` 内部**默认构造**了 `BackendConfig`，
  只带 `topics`，把 `domain_id` 丢了。
* **修复**：`RecorderConfig::backend` / `PlayerConfig::backend` 字段 + 显式传递。
* **验证**：`/proc/net/udp` 端口对照 + `tests/review/test_backend_config_propagation.cc`。

#### 3.2 SEDP 声明的 **QoS** 与内部 `requested_qos` 不一致
* **现象**：writer 为 `best_effort` 时，Stratum **一条都收不到**，
  而对端 `on_publication_matched` **从不触发**（`matched_now=0`）。
* **根因**：`BuildSedpAnnouncementForReader` 无论 `requested_qos` 是什么，
  都固定声明 `RELIABLE + VOLATILE` → 本端按 BE 判定"兼容"，对端按声明判定"不兼容"。
* **修复**：把 `requested_qos` 的取值传进订阅公告（首次订阅 + 周期重发两处）。
* **验证**：`scenarios/11_qos_matrix.sh`（6/6 符合规则）。

#### 3.3 声明 locator **硬编码 `127.0.0.1`**（跨主机致命）
* **现象**：同机"恰好"能通；**跨主机时对端会把数据发到它自己的回环**。
* **根因**：5 处对外声明的 locator 全部写死 `127.0.0.1`
  （SEDP 公告、SPDP metatraffic/user、Publication）。
* **修复**：`DetectLocalAddress()`（UDP connect + getsockname 按路由表选出口地址）+
  `advertised_address` 可覆盖。本机探测结果 `172.17.211.143`。
* **验证**：全量回归零退化（`design/15` §3）。

### 4. RTPS 协议细节类

#### 4.1 发布端**不处理 HEARTBEAT** → 拿不到对端的 SEDP Subscription
* **现象**：`sedp_sub=0`、`has_user=0`，无从知道把数据发往何处。
* **修复**：新增 `HandleHeartbeat` / `SendAcknackToPeer`（ACKNACK 位图覆盖 `[firstSN,lastSN]`）。
* **验证**：`sedp_sub` 由 0 → 有值。

#### 4.2 读取 **writerSN 的偏移少 4 字节**
* **现象**：**所有样本的 `sequence` 都是 0**，数据质量报告的"序号缺口"检测形同虚设。
* **根因**：writerSN 是 8 字节，`ids+8` 是**高位**（恒 0），低位在 `ids+12`。
* **验证**：字节 dump `00 00 00 00 | 01 00 00 00` 直接印证。

#### 4.3 声明的 reader entityId 被截成单字节 → ACKNACK 被忽略
* **现象**：`reader_guid.entityId.value[3]` 是**单个字节**，声明出去变成 `0x00000004`
  （丢掉 key 部分），而 ACKNACK 回 `0x00000104` → 对端静默忽略 ACKNACK
  → writer history 永不释放 → **上游 publisher 只发 1 条就停**。
* **修复**：收敛为单一常量 `kUserReaderEntityId`，声明与 ACKNACK 共用。

#### 4.4 **topic_kind 由 entityId 决定** → keyed 话题必然被拒
* **现象**：上游 keyed 示例（ShapeType 带 `@Key`）**一直不匹配**（`matched_now=0`），
  而本端 `sedp_pub=1`、`qos_rej=0`、持续回 Subscription，看起来一切正常。
* **根因**：Fast DDS 按 entityId 末字节推导 topic_kind
  （Writer `0x03`=NO_KEY / `0x02`=WITH_KEY；Reader `0x04`=NO_KEY / `0x07`=WITH_KEY）。
  我们固定用 `0x00000004`(NO_KEY)。
* **修复**：按对端 writer 的 kind 选择我方 reader entityId。
* **验证**：`scenarios/12_keyed_topic.sh`（上游从"0 行发送"变为 20 行 SENT；
  Stratum `samples=24`、**4 种不同 key**）。

#### 4.5 `PID_DATA_REPRESENTATION` **从未被序列化**
* **现象**：对需要 XCDR2 的类型（APPENDABLE 的 ShapeType）判定不兼容。
* **根因**：`kPidDataRepresentation` 常量早已定义，但 `rtps_packet_utils.cc`
  **从未使用它**。
* **踩坑**：`DataRepresentationId` 是 **`int16_t`（2 字节）**，先按 4 字节写导致
  对端解析失败 —— **行为与修复前逐项完全一致**，极易误判为"改动没生效"。

#### 4.6 录制端**不按 SN 去重** → 录制条数被放大
* **现象**：压测时 2000 条被录成 **58497 条（29 倍放大）**。
* **根因**：对端重传的数据被反复交付给上层。
* **修复**：`RtpsReader` 用 `received_sn_` 判定首次，重复只计数不交付。
* **验证**：**58497/2000 → 2001/2000**。

### 5. 其它（时间/基准/语义）

#### 5.1 时间线用**固定的 2023 年基准** → 延迟测量完全失真
* **现象**：端到端延迟测出 `9.1e10 ms`（≈2.9 年）。
* **验证**：改为"当前时刻往前推 count 条"后数值回到毫秒级。

#### 5.2 **回放方向测不出传输延迟**（语义结论，非缺陷）
* INFO_TS 承载样本**源时间戳**（语义正确），而回放是"时间旅行"——
  `now − source` 必然包含"原始生成 → 回放"的间隔。
* 因此回放方向可测的是**到达抖动**；**实时延迟**只能在录制方向
  （对端实时发布）测。

#### 5.3 首条数据落在 VOLATILE 边界 → 靠"新 SN 重发"补救
* **现象**：回放 200 条稳定收到 199 条，**缺的恒为第一条**；
  重发**相同 SN 无效**（reader 认为已过期）。
* **修复**：`Player` 记录首条副本，收尾用**新 SN** 重新发布。
* **效果**：**199/200 → 200/200**；代价是恒多出最多 1 条重复内容。
* **被放弃的路径**：用 `acked` 标志判断（`base_sn` 无法区分"已收到"与"不想要"，
  实测两头落空）。

#### 7.1 录制器**静默清空**已有时间线（数据安全性）
* **现象**：在同一目录重启录制器后，时间线数据从 **301 条 → 0 条**；用户只看到录制正常开始。
* **根因**：`stratum_rtps_record` 启动时**无条件**执行 `system("rm -rf <dir>")`。
* **修复**：默认**拒绝**在非空目录上启动（退出码 3 + `REFUSED` 提示 + 提示用 `--overwrite`
  或换目录）；确需覆盖时显式 `--overwrite`；并改用 `std::filesystem::remove_all`
  消除命令注入面。
* **验证**：`scenarios/19_overwrite_guard.sh` —— 非空目录无 `--overwrite` 时
  退出码 3 且数据 **121 → 121** 完好；带 `--overwrite` 明确清空。

#### 7.2 可靠传输在**丢包**下的收敛边界（能力验证，非缺陷）
* **做法**：在 Stratum 发送路径加调试用丢包注入（`STRATUM_DEBUG_DROP_SEND_PERCENT`，
  默认 0）。之所以不用 `tc netem`/UDP 代理：`tc` 在 WSL 不可用、`iptables` 权限不足，
  而用户态代理**插不进 RTPS 之间**（对端 locator 由协商确定、端口固定）。
* **实测**（200 条，RELIABLE）：**1% 丢包 → 100% 收敛**（重传补齐）；
  **5% → 94.5%**、**10% → 92.5%**（残留 11/15 条）。
* **结论**：可靠传输在**低丢包率（≤1%）下可完全收敛**；≥5% 会残留（成因未定位，记为 O20）。
* **脚本**：`scenarios/20_packet_loss.sh`。

#### 7.4 项目未纳入版本控制（工程风险，已解决）
* **现象**：项目**不是 git 仓库** —— 改动无法回滚、无法 diff、无法追溯。
  本会话中我确实误删过 `design/14` 的四条队列项（事后按记录重建），
  这类事故在有版本控制时成本极低。
* **修复**：`git init` + `.gitignore` + 首次提交。
  * `.gitignore` 排除 `build/`（**407 MB**）、`build-asan/`（50 MB）、
    `*.o/*.a/*.so`、日志、编辑器文件，以及运行期时间线文件（`*.ldb`/`CURRENT`/`LOCK` 等）；
  * 提交身份用**仓库级**配置（`Stratum Agent`），**不改动用户的全局 git 配置**。
* **结果**：首次提交 **`2be7156`**，跟踪 **295 个文件**，`.git` 仅 **2.9 MB**，
  `build/` 误纳入 **0** 个，工作区干净。
* **验证**：`git ls-files | wc -l` = 295；`git ls-files | grep -c '^build/'` = 0。

### 6. 工具/测量类：`kill -9` 后数据完好
* **做法**：录制过程中 `kill -9`（不给 flush 机会），再检查时间线。
* **结果**：时间线里**仍有 301 条**，与正常 SIGTERM 退出**完全一致**（目录均 ≈34 KB）。
* **踩坑**：第一次 `kill -9 $!` 打在了外层 `timeout` 上，真正的录制器没死，
  于是"打不开时间线"其实是 **LOCK 被占用**（顺带确认：LOCK 被占用时 `Open` 明确失败，
  属正确的保护）；另一次用"回放条数"判定，但没起订阅端，稳定得 0 条——
  正确判据是**时间线自身还能列出多少条**。
* **脚本**：`scenarios/18_crash_recovery.sh`。

### 6. 工具/测量类

#### 6.1 诊断程序 `samples=0` —— 根因在测试工具自身
* **根因**：给 `DomainParticipant` 传了 `StatusMask::all()` 的 listener。
  `DomainParticipantListener` 继承 `DataReaderListener`，于是 DataReader 的
  `DATA_AVAILABLE` 事件被 participant 级 listener 的**默认空实现吞掉**。
* **证据**：对照实验 `--participant-mask=all` → `samples=0`；
  `=none` → `samples=10`。
* **修复**：默认 `--participant-mask=none`（发现层回调不受影响）。

#### 6.2 吞吐的"420 条/秒"是**测量错误**
* **根因**：外部 shell 计时把**进程启动**（Fast DDS 初始化 + SPDP 发现 1~2 秒）
  与**收尾 ACK 等待**（最多 5 秒）算进了"发送耗时"。
* **证据**：耗时不随条数变化（2000 条 4.76 秒 vs 500 条约 4.7 秒）。
* **修复**：改用 `Player` 内部的**发送窗口**统计 → **≈8 万条/秒**。

#### 6.3 高速丢包是**接收端内核缓冲**不足，不是发送端乱发
* **证据**：`/proc/net/snmp` 的 `RcvbufErrors` 在一次高速回放中 **+317**，
  同时 `OutDatagrams` +6147（确实发出去了）。
* **修复**：录制方向（Stratum 是接收端）加大 `SO_RCVBUF` →
  **5000 条/秒 2000/2000、20000 条/秒 3000/3000（均 100%）**，`RcvbufErrors` 零增长。
  回放方向只能靠对端加大缓冲或发送端限速（限速默认关闭）。
