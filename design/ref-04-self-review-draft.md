# StratumDB 自检草稿（待 ref-04 行级对比报告合并）

> 状态：初步确认项，等行级对比 subagent 完成后合并定稿

## 已确认的问题

### P1. seq_in_ts 计数器未持久化 【Critical —— v1 C1 缺陷在 v2 重现】

`stratum_db.cc` 的 `NextSeqInTs()` 用内存 map `seq_in_ts_state_`，
`LoadTopics()` 只恢复 `topic_ids_` 与 `next_topic_id_`，**不恢复 seq 计数器**。

**后果**：重启后计数器归零。若继续写入与历史相同 `(topic, timestamp)` 的数据，
seq_in_ts 从 0 重新分配 → 与历史 key **完全相同** → LevelDB 认为是同一 key
的覆盖写 → **历史数据被静默覆盖**。

这正是 v1 LSM 引擎的 C1 缺陷（审查员实测：重启后新写入被旧值替换/删除复活）。
我在 v2 编码了 seq_in_ts（"seq 全局递增"的注释也在），但**忘了持久化**。

**修复选项**：
- A. `LoadTopics` 时对每个 topic 做 `Seek(EventKey(topic, MAX, 0))` + Prev，
  取最大 seq_in_ts 恢复计数器（O(话题数) 次 Seek，简洁）
- B. 把计数器写入 `META/next_seq/<topic_id>` 元数据 key（写放大，但精确）

倾向 A。

### P2. AppendBatch 不验证 topic 注册 → 孤儿数据 【High】

`AppendBatch` 里有死代码 `topic_ids_.find("")`（残留），
**没有按 topic_id 验证话题已注册**。

**后果**：传未注册的 topic_id，数据照写，但话题元数据不存在 →
重开后 `ListTopics` 看不到该话题 → **孤儿数据**（占空间、查不到、无归属）。

v1 的 `TimelineImpl::AppendBatch` 会返回 `InvalidArgument("unregistered topic id")`，
v2 丢掉了这个防护。

**修复**：维护 `topic_ids_` 的反向映射（id → name），AppendBatch 校验。

### P3. 保留点创建了但无法使用 【High】

`CreateRetentionPoint` 返回 `RetentionPoint`（包着 LSMengine Snapshot），
但 `QueryRange`/`GetStateAt` 的 `ReadOptions` **没有接 snapshot 参数**——
保留点创建后没有任何读路径能消费它。

**后果**：`CreateRetentionPoint` 是空转的 API。用户想"查保留点时刻的数据"，
没有路径可用。

**修复**：读 API 增加可选 snapshot 参数（或 RetentionPoint 指针），
透传给 `ReadOptions.snapshot`。

### P4. 保留点不持久化（跨重启失效）【High —— 设计已有，未实现】

ref-03 已给出方案：新增 VersionEdit tag `kRetentionPoint=10`
（varint64 seq + length-prefixed name），写进 MANIFEST。
当前 `CreateRetentionPoint` 只是内存快照，**重启即失效**。

用户核心需求"自动监控 A 事件发生前的数据"依赖跨重启的保留点。

### P5. Flush 语义过重 【Medium】

`Flush()` 调 `CompactRange(nullptr, nullptr)` —— 这是**全库 compaction**
（归并所有层），不是轻量的"memtable → L0"。

**后果**：Recorder 的 writer 线程每 200ms 空闲时调 Flush，
等于每 200ms 做一次全库 compaction —— 灾难性的写放大。

**修复选项**：
- A. 空操作 + 注释（WAL 已保证持久性；LSMengine 无公开 Flush API）
- B. 写入一个小 marker key + sync=true（强制 WAL fsync，数据仍持久）

### P6. durability 配置未接入 【Medium】

`AppendBatch` 硬编码 `wo.sync = false`。RecorderConfig 有
`durability`（kRelaxed/kBalanced/kStrict），但 v2 没接。

**后果**：即使配置 kStrict（每条 fsync），v2 也不执行。

### P7. Close 不显式 flush（依赖 WAL 重放）【确认无问题】

`Close()` 只 `delete db_`。LSMengine 的 `~DBImpl` 不做 flush，
未落盘数据留在 WAL，重开时重放 —— **语义正确**（已由 v2 测试的
跨重启恢复验证）。但 sync=false 的写入在进程 kill 时会丢未 fsync 部分
—— 这是 durability 权衡，需在文档明确。

## 待 subagent 补充

- DBImpl 自动继承清单（write stall / group commit / bg_error / LOCK 文件等）
- ReadOptions 生命周期细节
- 时间点查询算法的边界验证（跨 topic / 跨元数据 key / 空库）
- 需求覆盖矩阵的完整版
