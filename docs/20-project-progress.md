# 20 · 项目进度总览

> 最后更新：2026-10-19 23:30
> 用途：新接手的人（或下一轮对话）读这一篇就能知道"做到哪了、还差什么"。

---

## 一、当前状态（一句话）

**功能完整、74 个测试全过、文档 22 篇；本阶段做了两件事：回放带宽优化（RTPS 批量聚合），以及把 `docs/06` 列的 6 个测试缺口全部补完。**

```
仓库:     /UserData/CodexWorkSpace/Stratum（WSL 开发目录）
分支:     main
HEAD:     c6dd69b
提交数:   155
测试:     74 / 74 通过（ctest -j1 串行）
文档:     22 篇（docs/）
示例:     10 / 10 通过（5 模块 × demo/full）
本地 == 远端: 是
```

---

## 二、本阶段完成的两件大事

### A. 回放带宽优化：一个 RTPS Message 装多条 DATA

**问题**：`RtpsWriter::Publish` 是「一条样本一个 UDP 包」，固定开销：
```
IP(20) + UDP(8) + Message Header(20) + INFO_DST(16) + INFO_TS(12) + DATA 头(24) = 100 字节
```
HelloWorld 的 payload 只有 24 字节 → **payload 只占 19%**。

**方案**（依据 RTPS 2.3 §8.3.3：一个 Message 可含多个子消息）：

| 提交 | 内容 |
|---|---|
| `4fcc4ae` | `BuildDataPacketBatch`：Header+INFO_DST 只写一次，循环追加 N 个 INFO_TS+DATA |
| `14f8f44` | `RtpsWriter::PublishBatch`：一次拿 N 个 SN、一次 `sendto`/peer |
| `3e6c893` | `ISink::PublishBatch`（默认循环单发）+ `RtpsSink` override + `player.cc` 接入 + `--batch-size` |

**实测数字（分两层，必须分开看）**：

```
【报文构造层】
payload=24B  N=10
RTPS 字节:    800 → 620    省 22%
含 IP/UDP:   1080 → 648    省 40%
sendto 次数:   10 → 1

【同机端到端】1000 条、真实接收端、strace 统计
sendto 次数:     1001 → 64      降 15.6 倍
发送窗口耗时:  0.1789s → 0.0101s  快 17.7 倍
回放吞吐:       5589 → 98881 条/秒
接收端收到:     1000 → 1000 条    完整无丢失
```

**★ 最重要的发现**：**收益的大头是 syscall 减少，不是省字节**。
如果只省 40% 字节，吞吐不该提升 17.7 倍 —— 说明回放慢的主因是"每条一次系统调用"。

**⚠️ 限定**：同机回环（127.0.0.1）。跨主机还要看 RTT/MTU/IP 分片，**不能直接外推**。
**"省 40%"不能对外当承诺** —— 真实链路可能只有 15%。

**过程中修掉的一个真 bug**（`3e6c893`）：

> `Publish` 在无对端时把数据放进 `pending_` 缓存并返回 `NotFound`，
> 而 `PublishBatch` **无条件返回 OK** —— 数据没发出、没进 pending、调用方以为成功：
> **永久丢失且两端不报错**。
> 修法：`sent == 0` 时降级为逐条 `Publish`（让每条进 pending）+ 返回 `NotFound`。

### B. `docs/06` 的 6 个测试缺口全部补完

| # | 缺口 | 提交 | 关键收获 |
|---|---|---|---|
| 1 | `Player` 控制面 | `d0bca56` | `RequestStop` 380ms 返回、发布 4/100（真中断）、进度 50 次采样无倒退 |
| 2 | 迭代器 `Seek`/`Prev` | `88ee1d4` | **抓出两个后端同款的 `Prev` 死循环** |
| 3 | `MetaCodec` | `dec4ac2` | 高版本拒绝生效、173 种截断 **0 个假成功**、超界无越界读 |
| 4 | `SplitPolicy` | `abfdc84` | **`0` 表示「不限」不是「零上限」** |
| 5 | 工厂 `Create()` | `abfdc84` | `dynamic_cast` 双向验证类型（不是"非空即过"） |
| 6 | `Status` 分类判定 | `c6dd69b` | 12 个 code × 5 个判定 = 60 个布尔值的完整对应表 |

**其中第 2 项找到的是真缺陷**：

> `MemoryIterator` 与 `LsmIterator` 都写 `if (pos_ > 0) --pos_;` + `Valid() = pos_ < size`。
> 在第一个元素处 `Prev()`：`pos_` 不变、`Valid()` 仍 true →
> **`while (Valid()) Prev();` 永不终止**。
> 修法：加 `bool valid_`，`Prev` 越过 first 时置 false。

**为什么两个都有**：各自写的，但用了同一个思路。
**这正是契约测试的价值** —— 同一套断言跑两个后端，立刻发现"参考实现和自研实现犯了同一个错"。

---

## 三、`docs/06` 缺口表（已全部完成）

| # | 缺口 | 状态 |
|---|---|---|
| 1 | `Player` 控制面（`GetProgress`/`IsPlaying`/`RequestStop`） | ✅ |
| 2 | 迭代器 `SeekToFirst`/`SeekForPrev`/`Prev` | ✅ |
| 3 | `MetaCodec` | ✅ |
| 4 | `SplitPolicy` | ✅ |
| 5 | 工厂 `Create()` | ✅ |
| 6 | `Status` 分类判定 | ✅ |

---

## 四、未完成的事项（按价值排）

### 高价值

1. **`Sink::Print` 固定 4096 缓冲**（`AI_COLLAB` 挂着的）
   - `--decode` 大 JSON 会被**静默截断成非法 JSON**
   - 属于"输出看起来有内容但解析失败"，最难查的一类
2. **`cmd_query --stream` 不是真流式**
   - 先全量入 `vector` 再 limit，内存峰值 = 全量
   - 大会话查询会 OOM
3. **`Flush`/`GetStats`/`Close` 在未 `Open` 时**是否也有"合法空输入绕过状态校验"
   - 用 `PublishBatchRequiresOpen` 的判据扫一遍
   - 判据：*如果一个函数的成功返回值会让调用方误以为某个前提已满足，它就不该在前提未满足时返回成功*

### 中价值

4. **`BuildDataPacket` 两个并存入口**（结构体版 + 5 参数版）
   - 同一件事两个入口 → 读代码的人形成两套心智模型
   - 整改方向：5 参数版改成 `DataPacketParams` 的内联包装
   - 需要单独一轮 + 字节级回归（动的是 `Publish` 热路径）
5. **自适应批大小**：按 `max_udp_payload` 反推，避免大 payload 时单包超 MTU
6. **跨主机量化**：需要第二台机器（唯一还没测的维度）

### 低价值

7. 编译警告（6 个，`-Wall -Wextra`）
8. `stratum <cmd> --help` 不支持（所有 14 个命令返回 2）
9. `io`/`doctor`/`metrics`/`types`/`config` 没有专门的 spec 文档
10. `design/` 下 27 篇 markdown 从未审计

---

## 五、本阶段踩过的坑（后人不必再踩）

| 坑 | 教训 |
|---|---|
| **锚点歧义**：`CDRMessage::addOctet(&msg, kSubInfoTs)` 在 `BuildSedpAnnouncement` 里也有 | 用文本锚点做大范围替换前，**必须确认锚点在全文件唯一**。协议常量名天然多处出现 |
| **多步 patch 脚本里 `sys.exit`** | 第一步失败就 `sys.exit(1)` → 后面步骤**静默丢失**。改成每步独立判断、失败只跳过 |
| **`git commit -m` 多行消息被 shell 破坏** | 一律用 `git commit -F <文件>` |
| **`STRATUM_CHECK_EQ(a.code(), b.code())`** | `enum class` 没有 `operator<<`，要 `static_cast<int>`。项目规则早写了，还是踩了 |
| **字段名猜错**（`first_ns` / `max_age_ns`） | 编译期才报。写测试前先读一眼结构定义 |
| **`$VAR` 被 PowerShell 展开**（共 17 次） | 任何含 `$` 的命令必须落盘成 `.sh` 再执行 |
| **`test_cli_probe` 单独要 103 秒** | 跑全量时若有并发进程会假失败。先确认无并发 |
| **生成退化**（连续输出无意义确认词） | 主/子 agent 各发生数次。对策：把"我要做 X"直接变成"调用工具做X"；报告只写在最终回复里，不要写在思考块里 |

---

## 六、给下一轮的建议顺序

1. 先修 **`Sink::Print` 的 4096 截断**（小改动、高危害）
2. 再修 **`cmd_query --stream`**（需要重构查询路径，中等改动）
3. 然后扫 **未 `Open` 时的状态校验**（用已有的判据，容易做成一张表）
4. `BuildDataPacket` 两个入口整改（需要单独一轮 + 字节级回归）
5. 自适应批大小（可选，当前 `--batch-size` 手动控制够用）

---

## 七、关键文件索引

| 目的 | 文件 |
|---|---|
| 回放批量优化细节 | `docs/19-play-batch-optimization.md` |
| 测试缺口清单 | `docs/06-test-gaps.md` |
| 协作记录与"发现但未改" | `AI_COLLAB.md` |
| RTPS 报文构造 | `src/rtps/protocol/rtps_packet_utils.cc` |
| RTPS 发送 | `src/rtps/protocol/rtps_writer.cc` |
| 回放主循环 | `src/src/core/player.cc` |
| 存储契约测试 | `tests/unit/test_storage_contract.cc` |
| 元数据编解码 | `src/src/storage/meta_codec.cc` |
