# 代码审查报告 · 第 3 轮：接口设计、错误处理与可维护性

> 审查人：独立审查员（架构视角）
> 审查范围：`src/include/` + `src/src/` 接口与实现对照
> **本轮不含**第 4 轮已报的 C-1…C-7

---

## Critical

### N-1 质量报告永远报"可信度高"——`QualityStats` 从未累积，且重开后清零又被写回

**位置**：`core/types.h:88-91`、`timeline_impl.cc:391-426`、`:112-115`、`:70-91`、`recorder.cc:223,250`

三段问题叠加：

1. `dropped_messages` / `dropped_bytes` / `clock_jumps` / `interruptions`
   四个字段**全代码库无人写入**（`UpdateQualityOnAppend` 只写
   timestamp_regressions / sequence_gaps / missing_messages / latency）
2. Recorder 四处 `dropped_.fetch_add` 只加自己的原子量，**从不通知 Timeline**
3. `Open()` 不恢复 `total_events_` / `total_bytes_` / `quality_`，
   而 `Close():113-115` 用这些**零值覆盖已落盘的正确值**

**证据**：`types.h:88` 注释写"每个字段都必须在录制过程中持续累积"；
`query.cc:145` 依赖 `dropped_messages > 0` 判定 kBad。

**后果**：队列丢掉 90% 数据 → `level == kGood` → `ToText` 输出
"可用于内容分析、算法调参、回归测试" + "无空洞"。
一次长时间录制重启后，之前累积的统计在 Close 时被清零，**磁盘质量档案被主动破坏**。
`test_record_replay.cc:286-311` 断言 kGood，把该缺陷**固化为期望**。

**修法**：`ITimeline::ReportDrop(count, bytes, at_ns)`，Recorder 四处调用；
Open 恢复三者；补两条测试（队列限 1KB 灌 1MB → 断言 kBad；Close/Open 后 quality 不减少）。

---

### N-2 `reception_timestamp_ns` 从未持久化，回放输出恒为 0

**位置**：`timeline_impl.cc:331-333`、`:783-787`、`player.cc:141`、`types.h:22-26`

`TimeKey` 只编码 `(topic, ts, seq_in_ts)`，value 只存 payload。
而 `types.h:25` 明确写"**两者都必须保存，不能只留一个**"——直接矛盾。

**后果**：回放流无法与真实流区分（reception==0 即"未知"）；
录制文件重开后"录制延迟分布"永久不可分析。
`test_record_replay.cc:151-156` 只比对 payload + source_ts，所以看不出。

**修法**：value 改为定长头 `[8B reception][8B seq][8B iklen][instance_key][payload]`，
升格式版本并保留 v1 只读兼容。

---

### N-3 `WriteOptions` 三字段全库零引用，`verify_checksums` 默认 true 是假承诺

**位置**：`istorage_engine.h:52-61,75-81`；`grep options.sync` 全 src 仅 `lsm_engine.cc:366,397`

`verify_checksums` / `fill_cache` / `immediate_visibility` **无一实现**。
SSTable 格式只有 magic + version，**无 per-record CRC**（`lsm_engine.h:77-83`），
`SSTable::Open` 只查长度与魔数；唯一的 CRC 在 WAL，而 WAL 在 Flush 后即 Reset。

**后果**：刷盘后任何位翻转不会被发现，以**静默损坏**形式进入回放 payload；
调用方读了 `verify_checksums = true` 会以为读路径有端到端校验。

**修法**：删 `fill_cache`；`verify_checksums` 要么真加 CRC32，要么改成明确说明
"后端无记录级校验"的枚举；`immediate_visibility` 删除或标注"预留"。

---

### N-4 `LsmEngine::Close()` 吞 WAL 错误，且先把对象置为已关闭

**位置**：`lsm_engine.cc:318-336`、`:454-495`、`:254`（`~LsmEngine` 调用 Close）

`wal_.Sync()` / `wal_.Close()` 返回值直接丢弃；
`FlushMemTableLocked` 在 `SSTable::Write` 或 `wal_.Reset()` 失败时 return，
此时 `mem_` 未重置（Reset 失败路径下 WAL 已清空、SSTable 已写）。
Close 在返回前已 `opened_ = false` + `mem_.reset()`。

**后果**：磁盘满/EIO 时 Close 可能返回 OK 而最后一批只在内核缓存；
析构路径的失败无人可见。

**修法**：逐个检查 `wal_.Sync/Close`；失败时不置 `opened_=false`
让上层可重试；补"不可写目录 → 断言 Close 非 OK"测试。

---

## High

### N-5 插件注册表是文档幻觉

`glob **/plugin_registry*` **无结果**；`istorage_engine.h:236` 引用的
`STRATUM_REGISTER_STORAGE_ENGINE` 宏**全仓库无定义**；
而 `design/02-progress-report.md:27` 把该文件标为 ✅、
`:85-86` 称"四类插件共用同一个 `PluginRegistry` 模板"。

`MemoryEngineFactory`（`memory_engine.h:173`）/ `LsmEngineFactory`（`lsm_engine.h:227`）
**定义了但零处实例化**；测试全部硬编码 `new XxxEngine()`
（`test_storage_contract.cc:281,294,309-310,357-358`）；`ConfigSchema()` 零调用。

**结论**：**插件注册机制零实现**，只有接口隔离是真的。

**修法**：新建 `plugin_registry.h` 实现 `Register/Create/Names`
（未知名返回 kNotFound 且 message 带已注册名列表），定义注册宏并在两引擎 .cc 各加一行，
把 `RunContract` 改为按名字从注册表取引擎。

---

### N-6 `PutBatch` 原子性契约被违反，失败后内存元数据已污染

`istorage_engine.h:134` 绝对承诺"整批要么全生效要么全不生效"；
`lsm_engine.cc:369-379` 逐条 WAL + MemTable，中途失败即 return
（前缀已永久生效，Status 里无提交条数）。

更糟：`timeline_impl.cc:335-349` **在 PutBatch 之前**就已
`UpdateQualityOnAppend` + `message_count += 1` + `first_seen_ns` + `total_events_ += 1`，
`:355` 失败直接 return **不回滚**；且 AppendBatch 不持久化话题元数据
（`:360-366` 只改 active 标志），偏差重启后不会自动纠正。

**修法**：`PutBatch` 加 `uint64_t* committed_out` 并把注释改为"尽力而为"；
Timeline 把元数据修改移到写成功之后。

---

### N-7 delete/dispose 语义在接口层被抹掉，`GetStateAt` 永远不可能返回 kDisposed

`SSTable::Get` 有 `out_is_deletion`，但 `InternalGet`
（`lsm_engine.cc:567,571,581,591`）压成 `return !del;`——
"从未存在"与"已删除"返回同一 bool；`Get`(`:642`) 无差别返回 NotFound。

`StateAtResult::Kind::kDisposed`（`itimeline.h:81`）**永不设置**
（`timeline_impl.cc:529-537` 只有 kNotFound/kFound）；
`ReceivedSample::is_dispose`（`idds_backend.h:91`）在 `recorder.cc:228-240` 被丢弃，
`Event` 里没这个字段。

**后果**：回放含 dispose 的录制时，订阅方看到实例**一直保持最后一个有效值**——
错误状态，比丢数据更危险。

**修法**：Event 加 is_dispose（或 EventKind）；存储层加 dispose 键空间；
至少不要留悬空枚举值。

---

### N-8 Recorder 生命周期缺陷：Stop 后不能 Start，且所有错误被丢弃

`close_timeline_on_stop = true` 默认开启（`recorder.h:56`），
`Stop():101-103` 据此 `timeline_->Close()`；
但 `Start():34-79` **从不调用 `timeline_->Open()`**（grep 确认 Open 只在测试/benchmark 里）。

**第二次 Start 后每次 AppendBatch 得 kShutdownInProgress**，
被 `WriteBatch:250` 变成 dropped 计数——**录制器"正常运行"、一字节未写、Start 返回 OK**。

`Stop()` 丢弃 `source_->Stop()` / `Flush()` / `Close()` 三个返回值，恒返回 OK，
而注释承诺"已入队的数据不丢"。

`WriteBatch:245-252` 把任何 AppendBatch 错误一律变丢弃计数，无日志、无 `LastError()` 出口。

**修法**：加 `Status LastError()`；Start 前置检查时间线是否 open；Stop 汇总错误。

---

### N-9 `MemoryEngine` 迭代器与 `NewIterator` 契约冲突 + 数据竞争 + 悬垂

`istorage_engine.h:158-160` 承诺"迭代期间对数据库的写入不影响已创建的迭代器"；
`memory_engine.cc:224-237` 只传 `&map_` + snapshot_seq 后**立刻解锁**，
实现注释自己承认以"迭代期间不 Close"为前提（该前提从未写入接口契约）。

**真 UB**：`SettleForward:249-265` 无锁解引用 `versions`（`std::vector`），
并发 `PutLocked:84` 的 `versions.insert(begin())` 使其全部引用失效。

`Close():43` 清空 `map_` → 迭代器悬垂。

> 未用 TSan 复现，是从代码推断。

**修法**：`MemoryIterator` 内部持一个快照 + 改传 `MemoryEngine*` 在 shared_lock 下 Settle；
或物化（但会失去参考实现意义）。

---

### N-10 `MemTable::Get` 是全表扫描；`CompareInternalKey` 是死代码且注释描述废弃设计

`memtable.cc:162-210` 对整张 memtable **线性扫描**（`flush_threshold_ = 4MB`）。

`memtable.cc:125-141` 注释称"容器用本函数排序、`lower_bound` 也用它"——
但 `CompareInternalKey` **全库零调用**，`std::map<std::string,std::string>`
用默认比较器；`Get` 恰恰是为绕开它才改全表扫描的。**注释谎报了两件事**。

**修法**：删 `CompareInternalKey`；MemTable 换
`std::map<std::string, std::string, InternalKeyLess>`；
`CollectRange` 的 begin/end 恒空（`lsm_engine.cc:613-621`）那 20 行边界逻辑是死代码，删掉。

---

### N-11 `QualityReporter::Generate` 吞掉 `QueryRange` 失败

`query.cc:115` `if (timeline_->QueryRange(...).ok())` 失败静默忽略 →
`tq.gaps = 0`、`truncated_at_start = false` → 可能 kGood +
`ToText` 输出"无空洞/可用于内容分析"。
`kCorruption`（`timeline_impl.cc:779`）或 `kShutdownInProgress`
都会变成**最高信任等级**。

**修法**：`TopicQuality` 加 `scan_status`，失败即 kBad 并在文本/JSON 显式输出
"扫描失败，本报告不可用"。

---

### N-12 `TimelineEventIterator::topics_` 是未使用的悬垂裸指针

`timeline_impl.h:51` + `timeline_impl.cc:753-754` `(void)topics_;`；
构造时传入 `TimelineImpl::topics_` 的**地址**（`:463`）。
`Query::ExportRange`、`Player::Play` 都可能持有迭代器到 `TimelineImpl` 之后。
将来按注释"用 `topics_` 做话题过滤"即 use-after-free。

**修法**：删成员与参数。

---

## Medium

- **N-13 SSTable 无大小上限**（`lsm_engine.cc:41-61,149-189`）：
  恢复时每个 .sst 全量 resize 进内存且无上限；加 `kMaxTableBytes`。
- **N-14 `LsmEngine::Open` 失败不回滚**（`:256-279,281-316`）：
  `levels_[0]` 已塞入文件、`next_file_number_` 已推进，重试会重复入 L0 并翻倍内存占用。
- **N-15 `RecoverLocked` 静默跳过损坏 SSTable**（`:286-295`）：
  `continue` 无计数无日志，质量报告仍 kGood——**降级变成静默数据丢失**。
- **N-16 `DeleteRange` 与快照视图不一致**（`:407-448,676-696`）：
  `LsmIterator` 去重只看最大 sequence、**不检查 `seq <= snapshot`**，
  所以旧快照下点查能读到旧值、范围扫描却看不到（`MemoryEngine` 的 `SettleForward:257` 是对的）
  → **两引擎行为不一致，MemoryEngine 正确**。
- **N-17 `EngineStats` 跨引擎语义不一致**（`memory_engine.cc:422` vs `lsm_engine.cc:815-825`）：
  一个是 `map_.size()`（含 tombstone key，且 Delete 后不减少），
  另一个是所有层所有文件 `entry_count` 求和。换后端后监控数字差数量级。
- **N-18 `ITimeline` 无 dispose 能力**（`itimeline.h:107-119`）：与 N-7 同源，接口不对称。
- **N-19 `TimeRange::Empty()` 零调用**（`itimeline.h:48`）：
  `QueryRange(begin=100, end=50)` 两引擎都返回 OK + 空结果——
  "参数写错"与"真的没数据"不可区分。
- **N-20 `GetStats` 持全局写锁 + Close 后可调用性未定义**（`lsm_engine.cc:805-826`）：
  每次统计要抢 `mutex_` 阻塞写入；`istorage_engine.h:126` 说 Close 后
  "除析构外不应再调用其他方法"却又列了 GetStats/Name/Version。

## Low

- **N-21 `ReleaseSnapshot` 契约被两实现主动违反**（`istorage_engine.h:194-196` vs 两实现）：
  注释说 UB，实现友好处理；且同一 sequence 的两个快照重复释放时
  `live_snapshots_` 不会被减（multiset 只 erase 一个），计数器长期偏差。
- **N-22 格式版本三处硬编码**：`timeline_impl.h:28`、`meta_codec.cc:15`、`types.h:133`；
  按 `types.h:7` 的指示升级必然漏改。
- **N-23 `check_interval_ns` 零消费者 + `RunRetention` 每条 tombstone 一次 fsync**：
  `LsmEngine::DeleteRange:407` 参数被注释掉、`:440` 硬编码 `sync = true`
  （与 Put 只对 kAlways sync 不一致）；`RunRetention` 全程持 `write_mutex_`，
  淘汰百万 key = 百万次 fsync 并阻塞录制。
- **N-24 `ToJson` 无转义 + 字段缺失**（`query.cc:227-258`）：
  手写拼接话题名，含 `"` 即产出非法 JSON；latency / dropped_bytes 完全没输出。
- **N-25 Stop 的"除非超时"不存在**（`recorder.cc:95` 无超时 join、`:88` 无超时 Stop）：
  注释描述了一个不存在的机制。
- **N-26 死代码**：`ConfigSchema`、`MakeTopicRange`、`Peek*3`、
  `MemTable::GetFirstKey/GetLastKey`、`ExtractUserKey`、`TimeRange::Empty`、
  `CurrentSequence`、`TimelineImpl::engine()` 全部零调用且无 `[未接线]` 标记。

---

## 总体评价

**插件化目标：一半达成。**

| 维度 | 结论 |
|---|---|
| 语义隔离（上层不依赖后端类型） | ✅ 真实。`ITimeline` 确实是唯一知道 `TimeKey` 编码的地方 |
| 可替换性 | ❌ 零实现（注册表/宏/工厂接线全缺） |
| 语义一致性 | ❌ 多处不成立（N-7/N-9/N-16/N-17/N-20 换后端行为会变） |

**最该先修的：N-1 + N-11（质量报告不可信）。**
它是"静默错误"的放大器：其他所有缺陷造成的丢失都会被这份报告宣布为没发生，
而且默认值最乐观、测试把它固化为期望、损坏时输出最高信任等级。

其次是接口谎言类（N-3 / N-2 / N-5），再次是批写原子性（N-6）与状态机（N-4 / N-8 / N-14）。

### 可自动化的注释一致性规则

本次 12 处注释-实现不一致呈三种**可检测模式**：

- **(a) 接口字段零引用**：`verify_checksums` / `SplitPolicy` / `kBlock` /
  `check_interval_ns` / `ConfigSchema`
- **(b) 注释引用不存在的对象**：`plugin_registry.h`、`STRATUM_REGISTER_*`、
  已删除的 `lower_bound` 用法
- **(c) 注释语义与另一处实现矛盾**：`types.h:25` vs 盘面格式、
  `istorage_engine.h:158-160` vs 懒迭代器、`memtable.cc:128-130` vs `std::map` 默认比较器

**建议 CI 加两条脚本**：接口字段引用计数断言 + 注释里反引号路径的存在性检查。
