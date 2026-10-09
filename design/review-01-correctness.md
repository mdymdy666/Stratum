# 代码审查报告 · 第 1 轮：正确性与内存安全

> 审查人：独立审查员
> **所有 Critical/High 均已在 WSL 内编译真实 reproducer 实测复现**，非纯推理
> 注：任务清单里的 `src/src/storage/lsm/skiplist.cc` **不存在**（全项目无 skiplist 文件）

---

## Critical

### C1 LSM `last_sequence_` 未持久化 → 跨重启后旧数据覆盖新数据、已删除数据复活

**位置**：`lsm_engine.cc:301-312`（`RecoverLocked` 只从 WAL 恢复 `max_seq`）、
`:483`（`FlushMemTableLocked` 成功后 `wal_.Reset()` 清空 WAL）、
`:272`（`wal_.Open(..., mem_->Empty())`）

**证据**：
```cpp
// lsm_engine.cc:301-312
uint64_t max_seq = last_sequence_.load();
s = replay_wal.Replay([&](uint64_t seq, ...) { mem_->Add(...); max_seq = std::max(max_seq, seq); });
last_sequence_.store(max_seq);      // WAL 空 => max_seq 恒为 0
// lsm_engine.cc:483
s = wal_.Reset();                   // 数据落 SSTable 后清空 WAL，sequence 水位随之丢失
```

**实测 1**（`Put(k1,v1)` → `Flush` → `Put(k1,v2)` → `Close` → 重开 → `Put(k1,v3)`）：
```
run1 end   last_sequence=2
reopened   last_sequence=0      <-- 从 0 重新开始
after Flush        Get(k1) -> "v3"
after Compact      Get(k1) -> "v2"      <-- 新写入的 v3 被静默丢弃
```

**实测 2**（`Put(k9,"payload")` → `Flush` → `Close` → 重开 → `Delete(k9)`）：
```
after Delete       Get(k9) -> NotFound
after Compact      Get(k9) -> "payload"   <-- 已删除数据复活
```

**根因**：`SSTable::Get`（`:214-231`）与 `CompactLocked` 的 `std::unique`（`:521-526`）
都以「sequence 最大者胜」判定可见版本。`RecoverLocked` 不扫描 SSTable 的 sequence，
导致重启后 sequence 回绕，**旧数据的 sequence 反而更大**。

**附加放大**：`AppendBatch` 每次 Flush/Close 都重写 `META/session`、`META/topic/*`
等**同一个 key**（`timeline_impl.cc:129/137/695`），所以每次正常运行都会走覆盖路径；
且当新旧版本 sequence 恰好相同时，`std::sort`（不稳定）+ `std::unique`
保留哪一个是**未定义的**。

**修法**：把 `last_sequence_` 持久化（独立 `CURRENT`/`MANIFEST` 文件），
或 `RecoverLocked` 时遍历所有已加载 SSTable 取 `max(entry.sequence)+1` 作为下界；
`FlushMemTableLocked` 里 `wal_.Reset()` 之前必须先落水位。

---

### C2 Compaction 从不删除被合并掉的旧 SSTable，且重启时全量重载为 L0

**位置**：`lsm_engine.cc:541-553`（`obsolete` 收集循环体是空的）、
`:286-295`（`ListTableFiles` 把目录下**所有** `.sst` 无差别塞进 L0）

**实测**（compaction 后 `ls`）：`db1` 目录留下 `1.sst 2.sst 3.sst 4.sst`，
其中 1/2/3 是已被合并掉的输入文件。

**后果**：(a) 磁盘空间无界增长；(b) 重启后 L0 文件数 = 历史 compaction 次数，
立刻再次触发 compaction，形成**正反馈**；(c) 被 compaction 判定为过时的旧版本文件
重新"复活"成为独立 L0 文件，**与 C1 叠加放大错误结果**。

**修法**：`SSTable` 记录 `path_`，compaction 成功后 `unlink` 所有输入文件；
长期方案是引入 MANIFEST 记录层级归属。

> 注：与第 4 轮 C-1 是同一处缺陷，由两个独立审查员分别确认。

---

### C3 Recorder 队列字节记账口径不一致 → 录制中途静默丢弃几乎所有数据

**位置**：`recorder.cc:121`、`:147`、`:135`、`:175`

**证据**：
```cpp
const uint64_t sample_bytes = sample.payload.size() + sample.topic_name.size();  // :121
queue_bytes_ -= queue_.front().payload.size();      // :135  入队减：只减 payload
queue_.push_back(sample);
queue_bytes_ += sample_bytes;                       // :147  入队加：payload + topic_name
queue_bytes_ -= batch.back().payload.size();        // :175  出队减：只减 payload
```

每条样本走完队列**净增 `topic_name.size()` 字节**，`queue_bytes_` 单调虚增，
最终恒大于 `max_buffer_bytes`。

**实测**（`max_buffer_bytes=2000`，topic 名 26 字节，payload 10 字节，发送 300 条）：
```
发送 300 条 -> received=300 recorded=66 dropped=234
期望 recorded=300 dropped=0
```

**后果**：长录制中（高速场景下约百万条量级）录制器会开始丢弃**全部**新数据；
`kDropOldest` 模式则会把整个队列清空。丢弃有计数但不影响主流程，
属于**典型静默数据丢失**。

**修法**：入队/出队使用同一口径（把 `sample_bytes` 存进队列元素，
或出队时减 `payload.size() + topic_name.size()`）。

---

## High

### H1 `MemoryIterator::SeekToLast()` 完全无法定位到最后一个元素

**位置**：`memory_engine.cc:350` → `void MemoryIterator::SeekToLast() { SettleBackward(); }`

`SettleBackward()` 是**从当前 `it_` 向前（递减）**扫描，而不是先把 `it_` 置到 `end()` 再回退。

**实测**（keys a,b,c,d）：
```
fresh SeekToLast -> valid=1 key="a"   (期望 "d")
Seek("c") 后 SeekToLast -> key="c"    (期望 "d")
对照 LsmIterator::SeekToLast -> key="d"  ✓ 正确
```

**修法**：`it_ = map_->end(); if (!map_->empty()) --it_; SettleBackward();`

**备注**：`design/05-known-gaps.md` 2.2 只记录了"反向迭代定位不准"的现象并改用正向扫描绕过，
**根因即此处，代码至今未修**；直接使用 `SeekToLast()` 的调用方会静默拿到错误结果。

---

### H2 WAL 记录损坏被静默吞掉，整个 WAL 被截断 → 未落盘数据永久丢失且无报错

**位置**：`memtable.cc:381-387`（CRC 失败 `break`，不记录错误）、
`:421`（无论是否截断都 `return Status::OK()`）、
`lsm_engine.cc:272`（`truncate_existing = imm_ == nullptr && mem_->Empty()`）、
`:289-292`（损坏的 SSTable 直接 `continue` 跳过）

**实测**（写入 2 条后模拟崩溃，篡改 WAL 第一条记录的 CRC 4 字节）：
```
损坏后 WAL 大小  = 40 字节
Open() 返回      = OK          <-- 未报告 corruption
Get(crashkey2)   = NotFound    <-- 数据丢失
重开后 WAL 大小  = 0 字节      <-- 整个 WAL 被 O_TRUNC 清空
```

**后果**：只要第一条记录损坏（磁盘坏块、断电写撕裂），`Replay` 返回 0 条 →
`mem_->Empty()` 为真 → `Open` 用 `O_TRUNC` 清空 WAL，
**全部未落盘数据永久消失，且 Open 返回 OK**。

**修法**：`Replay` 遇 CRC/长度异常返回 `Status::Corruption`；
`Open` 在任何非 OK 时不截断 WAL 并向上报告；损坏的 SSTable 也应计入诊断。

---

### H3 `TimelineImpl::Close()` 不持久化话题元数据 → 统计与时间范围全部丢失

**位置**：`timeline_impl.cc:106-123`（`Close` 只调 `PersistSessionLocked`）、
`:359-366`（`kMetaFlushThreshold` 分支是空实现）

```cpp
// Close()：只写 session，topics 一个都不写
Status s = PersistSessionLocked();
// AppendBatch 的"节流"分支：注释说"元数据落盘"，实际只置了个标志
if (pending_meta_updates_ >= kMetaFlushThreshold) {
  for (auto& kv : topics_) { kv.second.active = true; }   // <-- 没有 PersistTopicLocked
  pending_meta_updates_ = 0;
}
```

**实测**（RegisterTopic + AppendBatch 5 条 + `Close()`，重开）：
```
Close 前(内存): message_count=5  first_seen=1000  last_seen=1040
Close 后(重开): message_count=0  first_seen=0     last_seen=0
(对照) 事件数据仍在: CountRange(topic=1) = 5
```

**后果**：`message_count/byte_count/first_seen_ns/last_seen_ns` 全部归零。
`first_seen_ns` 同时是 `GetStateAt` 的 out_of_range 判定依据（`:487-488`），
重启后同一查询会从 `kOutOfRange` 变成 `kNotFound`；
`Query::ListTopics`、回放预扫描（`player.cc:92-99`）一并失真。
**只有显式调用 `Flush()` 才不丢。**

**修法**：`Close()` 中在 `PersistSessionLocked()` 前遍历 `topics_` 调 `PersistTopicLocked`。

---

### H4 `Player::Play` 中途 return 未清理状态 → Player 永久卡死 + sink 泄漏

**位置**：`player.cc:126`
```cpp
do {
  STRATUM_RETURN_IF_ERROR(timeline_->ListTopics(&topics));   // <-- 直接 return
```

该宏失败时直接 `return _s`，**跳过**函数尾部的 `sink->Close();`
和 `playing_.store(false, ...)`（`player.cc:210-211`）。

**后果**：`playing_` 保持 `true`，此后任何 `Play()` 都返回
`AlreadyExists("player already running")`，**只能重启进程**；
`sink` 的 `Close()` 永不执行（对真实 DDS 后端意味着资源/参与者泄漏）。

**修法**：改成显式错误码 + 统一 cleanup 出口。

---

### H5 `QualityReporter::Generate` 的 `(delta / avg) - 1` 无符号下溢

**位置**：`query.cc:129`
```cpp
if (avg > 0) {
  tq.missing += (delta / avg) - 1;    // delta/avg == 0 时 uint64 回绕为 UINT64_MAX
}
```

`avg` 是所有间隔的**平均值**，只要间隔分布不均，
就存在某个 `delta < avg`（同时 `delta > threshold`）使 `delta/avg == 0`。

**实测**（3 条消息，ts = 1e9 / 1.01e11 / 1.03e11）：
```
report: gaps=2 missing=18446744073709551615
JSON: ..."gaps":2,"missing":18446744073709551615...
```

**修法**：`const uint64_t est = delta / avg; if (est > 0) tq.missing += est - 1;`

---

### H6 `LsmEngine::DeleteRange` 分两段加锁，第二段不检查 `opened_` → 空指针解引用

**位置**：`lsm_engine.cc:415-448`（第一段锁 417-431，第二段锁 437-446）
```cpp
{ std::lock_guard<std::mutex> lock(mutex_); ... InternalCollectRange(...); }   // :417-431
if (victims.empty()) return Status::NotFound("no keys in range");
std::lock_guard<std::mutex> lock(mutex_);                                      // :437
for (const auto& key : victims) {
  mem_->Add(seq, lsm::kTypeDeletion, key, std::string());                       // :444 无 opened_ 检查
```

**后果**：两段锁之间存在窗口，`Close()` 会执行 `mem_.reset()`（`:332`）。
窗口内被 Close 后，第二段对 `mem_`（nullptr）解引用 → 崩溃/UB。
`istorage_engine.h:108-110` 明确要求所有方法可并发调用。

---

### H7 `MemoryIterator` 持裸指针且全程不持锁 → 违反迭代器契约

**位置**：`memory_engine.cc:230-237`（`NewIterator` 在返回前就释放 `shared_lock`）、
`:249-268`、`:270-307`（遍历全程无锁）、`memory_engine.h:159`

**后果**：
(a) 并发 `Put` 时 `PutLocked` 对已存在 key 执行 `entry.versions.insert(..., begin())`
（`:84`）会使 `FindVisible` 刚返回的 `const ValueVersion*` 所指 vector 重新分配
→ **悬垂指针 + data race（UB）**；
(b) `MemoryEngine` 先于迭代器析构即 heap-use-after-free。
违反 `istorage_engine.h:156-161`「迭代期间对数据库的写入不影响已创建的迭代器」。

---

### H8 `RetentionPolicy::max_bytes` 完全未实现（静默失效）

**位置**：`timeline_impl.cc:637-644` 与 `:618-630`

`ComputeRetentionCutoffLocked` 只读 `retain_duration_ns`，从不读 `max_bytes`。

**实测**（1000 条 × 64 字节 = 64000 字节，`max_bytes = 100`，`retain_duration_ns = 0`）：
```
RunRetention -> OK ; 淘汰后 CountRange = 1000   <-- 期望远小于 1000
```

**后果**：只配置 `max_bytes` 的部署保留策略静默不生效，磁盘无界增长直到写满。

---

### H9 `LsmEngine::Open` 对特定文件名抛 `std::out_of_range`，违反"不抛异常"契约

**位置**：`lsm_engine.cc:96-100`
```cpp
if (num.find_first_not_of("0123456789") != std::string::npos) continue;  // 只校验"全是数字"
numbers.push_back(std::stoull(num));                                    // 超长数字 -> 抛异常
```

**实测**（目录中放入 `999999999999999999999999.sst`）：
```
!! Open() 抛出异常: stoull
```

**后果**：`istorage_engine.h:113-114` 声明"不存在'抛异常表示失败'的路径"，
异常直接穿透到调用方；数据目录里出现一个畸形文件名即可让数据库无法打开。

---

## Medium

| 编号 | 问题 | 位置 |
|---|---|---|
| M1 | `kBlock` 未实现，静默退化为无界队列 → OOM | `recorder.cc:140-142` |
| M2 | `PutBatch` 非原子，违反接口契约 | `lsm_engine.cc:369-379` |
| M3 | `AppendBatch` 存储失败时内存统计已推进 | `timeline_impl.cc:335-357` |
| M4 | `first_seen_ns` 与 `ts == 0` 冲突（kInvalidTimestamp 同为 0） | `timeline_impl.cc:340-343` |
| M5 | `WAL::Append` 短写后留半条记录 → 后续记录全部不可重放 | `memtable.cc:340-344` |
| M6 | `DeleteRange` 空范围返回 `NotFound`（语义混淆） | lsm`:433-435` / memory`:176-178` |
| M7 | **保留点方向：接口文档与实现矛盾**（不确定哪侧是意图） | `itimeline.h:144` vs `timeline_impl.cc:624-628` |
| M8 | 序号缺口统计在 `seq == UINT32_MAX` 时回绕 | `timeline_impl.cc:403-407` |
| M9 | `Crc32` 静态表惰性初始化非线程安全（data race → UB） | `memtable.cc:63-75` |
| M10 | `Open` 忽略保留策略读取的非 NotFound 错误 | `timeline_impl.cc:94-100` |
| M11 | `read` 的 `n <= 0` 一律当 EOF（EINTR 被当截断） | `lsm_engine.cc:53-57`、`memtable.cc:367-373` |

**M7 实测**（保留点设在数据中段 mark，1000 条覆盖 [1e9, 1e9+1e6)）：
```
RunRetention -> OK ; 淘汰后剩余 = 500 条   <-- mark 之前的 500 条被删除
```
头文件说"保留点之前的数据不会被淘汰"，实现却**恰好删掉保留点之前的数据**。
两处注释语义相反。从"配合快照做定点查询"的用途看，
保护 `at_ns` 之前的数据才是合理语义。**需产品侧明确。**

---

## Low

- **L1** `istorage_engine.h:173-174` 的 `SeekForPrev` 文档与两个后端实现都不符
  （两后端一致，判断为**文档写错**）
- **L2** `memtable.h:49-51` 与 `lsm_engine.cc:132-137` 两处注释都声称
  「大端 tag 追加在键尾 ⇒ sequence **降序**」，实际字节序升序 ⇒ **升序**。
  照注释写代码会踩坑。
- **L3** `memtable.cc:150-158` `MemTable::Add` 覆盖时 `approximate_size_` 单调虚高
- **L4** `query.cc:140` `truncated_at_start = false` 是恒假占位实现
- **L5** `query.cc:251` `ToJson()` 未做 JSON 转义

---

## 确认**不是** bug 的几处（避免重复排查）

- `time_key.cc:122-131` `MakePrefixRangeUpTo` 的 `+1` 回绕：有兜底
- `memtable.cc:378-419` WAL `Replay` 的边界检查：逐分支验证无越界读
- `lsm_engine.cc:142-205` `SSTable::Open` 解析：无整数溢出
- `memory_engine.cc:88-99` 版本链裁剪条件与 `FindVisible` 语义一致
- `lsm_engine.cc:574-595` `InternalGet` 的查找顺序 + tombstone 遮蔽：
  在 sequence 全局单调的前提下正确（**问题出在 C1 破坏了这个前提**）
- `timeline_impl.cc:445-454` 全话题范围构造：结论正确，
  但 `:30-31` 注释「'S' < 'M'」**把大小关系写反了**（结论凑巧仍正确）

---

## 总体评价

**架构分层与注释质量明显高于平均水平**（每个非显然决策都写了理由，
并明确记录了已知缺口），**但"错误路径"和"跨重启"这两条主线上的正确性尚未建立**。

45 个测试用例全绿，是因为它们集中在「单进程、单次打开、数据量小到不触发 compaction」
的甜区；本次 9 个 Critical/High 里有 **7 个是在测试未覆盖的路径上实测复现的**。

### 最值得担心的三个方面

1. **跨重启的 sequence 水位丢失（C1）—— 本项目最危险的缺陷。**
   它不是"某个 API 返回值不对"，而是**持久化数据在重启 + compaction 后被静默
   替换成更旧的版本、甚至已删除的数据复活**。它同时污染所有 key 的覆盖写路径，
   包括每次都写的 META 键。
   根因与已知缺口 1.1（compaction 不尊重快照）是两个独立的洞，
   叠加后"快照/保留点"这一整条产品卖点实际不可信。
   **修它应该排在 RTPS 协议层之前**——协议层一旦接入，
   错误的存储层会让"录到了但读回来是旧的"这种故障极难定位。

2. **静默失败成体系（H2、H8、C3、M1、M10）。**
   WAL 损坏 → Open 返回 OK 且清空数据；`max_bytes` 策略 → 完全不生效；
   Recorder 队列 → 记账错误导致中途丢弃全部数据；`kBlock` → 未实现的空 case。
   共同特征是**没有崩溃、没有错误码、只有个别计数器缓慢变化**。
   建议把所有"降级/跳过/未实现"路径改为显式非 OK 状态或至少一次性告警，
   这比修任何单个 bug 的收益都大。

3. **接口契约与实现系统性不一致（M2、H7、H9、H4、L1）。**
   头文件里写着很强的承诺——"批量原子写"、"写入不影响已创建的迭代器"、
   "不存在抛异常表示失败的路径"、"保留点保护其之前的数据"——
   但实现逐条违反，且没有机制防止后来者继续违反。
   建议为 `IStorageEngine` 补一组**契约测试**
   （AtomicBatch、IteratorIsolation、NoThrow、SnapshotRetention），让双后端都跑。
