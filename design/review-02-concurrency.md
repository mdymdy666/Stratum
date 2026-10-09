# 代码审查报告 · 第 2 轮：并发安全与对象生命周期

> 审查人：独立审查员（并发专家）
> 已排除 4 个已知问题。**4 项关键发现均有实测复现**。

---

## 实测复现结果

| 实验 | 结果 |
|---|---|
| 两线程并发 `Recorder::Start()` | **3/3 abort (core dumped)**，`terminate called without an active exception` |
| 两线程并发 `Recorder::Stop()` | **2/3 abort (core dumped)** |
| `queue_bytes_` 记账漂移（500 条，topic 100B） | `recorded=19 dropped=481`，**96% 静默丢失** |
| LSM 重启后 `last_sequence_` | `Open` 后 `=0`（应 6）；写同一 key 后 `Get`→NEW 而 `NewIterator`→**OLD**（两条读路径相反） |
| MemoryEngine 迭代器竞态（TSAN） | 确认 data race：`memory_engine.cc:63` ← 迭代器线程；`:84` `versions.insert` 写 |

---

## Critical

### C1 `Recorder::Start/Stop` 无任何互斥 → 并发调用直接 abort

**位置**：`recorder.cc:34-79`(Start)、`:81-109`(Stop)、`:62`、`:94-96`

四种失效模式：

| # | 场景 | 后果 |
|---|---|---|
| a | 两线程并发 Start 都通过 `:35` 的 `running_.load()` 检查 | 都执行 `:62` `writer_thread_ = std::thread(...)`；第二次赋值给 joinable 的 `std::thread` → **`std::terminate`**。实测 3/3 崩。同时 `:58` 对同一 `unique_ptr` 非原子写（double-free 风险） |
| b | 两线程并发 Stop 都读到 true | 都对同一 `std::thread` 并发 `join()` → 数据竞争 + 重复 `pthread_join` → UB。实测 2/3 崩 |
| c | **Stop 插进 Start 中间**（最危险） | `:59` 才 `running_.store(true)`，`:62` 才建线程。Stop 在两者之间执行 → `:94` `joinable()` 为 false 跳过 join → `:107` **`source_.reset()` 置空**；Start 恢复后 `:64` `source_->Start(...)` → **空指针解引用崩溃**，writer 线程无人 join |
| d | **Stop 插在 `CreateSource` 期间**（最现实） | `:53` DDS 发现可能耗时数百 ms，此时 `running_` 仍为 false → `:82` Stop **静默返回 OK，什么都没做** → Start 完成后录制在跑，调用方以为已停止 |

**修法**：`std::mutex state_mutex_` + 状态机（kIdle/kStarting/kRunning/kStopping）；
所有转移持锁；Start 用 CAS 抢占；析构与 Stop 同路径。

---

### C2 后端回调线程与 Recorder 析构的 use-after-free

**位置**：`recorder.cc:17-21, 106-107, 114-124`；接口 `idds_backend.h:100-114`

`ISource::Stop()` 的契约中**没有**"返回后保证不再有回调"（文档只说"回调在后端自己的线程上被调用"）。

**时序**：A 在 `~Recorder`→`Stop()` 尚未执行到 `:106` 时；
B（后端回调）已在 `:115` 读到 `running_ == true` 并通过检查，被抢占；
A 完成 `:106/:107`，Stop 返回，析构继续销毁 `mutex_`/`cv_`/`queue_`/`topic_ids_`；
B 恢复后执行 `:124` `std::lock_guard<std::mutex> lock(mutex_)` → **访问已析构的 mutex**。

`MockSource` 恰好 join 了回调线程，**真实 DDS 后端不保证**（listener 在协议栈线程池）。

**修法**：(a) 在 `ISource::Stop()` 文档中明确"返回后不得再有回调"并让实现遵守；
(b) 或在 Recorder 侧加 in-flight 回调计数，析构前等待归零。

---

### C3 MemoryEngine 迭代器逃逸锁保护 —— **Timeline 全部读路径在锁外使用它**

**位置**：`memory_engine.cc:224-237, 249-268, 54-69`；`timeline_impl.cc:431-465, 539-561`

明确违约 `istorage_engine.h:158-160`（"迭代期间对数据库的写入不影响已创建的迭代器"）。
`memory_engine.cc:230` 的 `shared_lock` 在**返回前就释放**，
迭代器此后持有 `&map_` 与 `Map::const_iterator`，无任何保护。

**时序（边录边放，Stratum 核心场景）**：
A(Player) 调 `QueryRange()` → `NewIterator`（`:230`）→ 释放锁 → 开始 `Next()`；
B(Recorder) 调 `AppendBatch` → `PutLocked` 的 `map_[key]`（`:76`）插入节点并 rebalance 红黑树；
A 此刻在 `SettleForward()` 读到同一批节点 → **data race**：漏读/重复/跳错节点/**无限循环（活锁）**。

**TSAN 实证**（行号精确）：read `:63` `FindVisible` ← `:258` `SettleForward` ← `:314` `Next`；
previous write `:84` `versions.insert` → `_M_realloc_insert`。

**同根因附带**：`CompactRange`（`:397-401` `versions.resize(1)`）与正在 `FindVisible`
解引用 `ValueVersion&` 的迭代器线程并发 → vector 元素被析构；`Close()/Open()` 的
`map_.clear()`（`:30,43`）同理。**ASAN 实测迭代器读到损坏值。**

**影响面**：`QueryRange`/`CountRange`/`GetStateAt`/`Player::Play`/
`QualityReporter::Generate` **全部走这条路径**。

**修法（按 LevelDB 的做法）**：迭代器必须自带保护 ——
(a) 像 `LsmIterator` 那样在构造时值拷贝成快照；
(b) 迭代器内持有 `std::shared_lock`（需把 `shared_mutex` 放进 `shared_ptr`）；
(c) 引入 epoch/hazard pointer。
> LevelDB 的对应做法：`DBImpl::NewInternalIterator` 让迭代器持有
> `SuperVersion` 的引用计数，版本在迭代器析构前不被回收。

---

### C4 LsmEngine 重启后 `last_sequence_` 未从 SSTable 恢复 → **两条读路径结果相反**

**位置**：`lsm_engine.cc:281-316`(RecoverLocked)、`:286-295`、`:301`、`:483`

**因果链**：正常 `Close()`（`:325` FlushMemTableLocked → `:483` `wal_.Reset()`）后
**WAL 为空，最大 sequence 只存在于 SSTable**；重开时载入 SSTable（`:288-295`）
但**不更新 `last_sequence_`** → `last_sequence_.store(0)`（`:312`）→
后续 `NextSequence()` 从 1 开始，新版本 sequence **小于**磁盘旧版本。

**两条路径分叉**：
- `InternalGet`（`:559-598`）按"层优先"，`mem_` 命中即返回 → **新值**
- `InternalCollectRange`（`:600-623`）+ `LsmIterator`（`:665-695`）全层排序、
  同 key 只留 **sequence 最大** → **旧值**

而 Timeline/Player **全走迭代器**。

**实测**：
```
[1] Put k=OLD CurrentSequence=6 → Close
[2] Open CurrentSequence=0 → Put k=NEW CurrentSequence=1
    Get k = NEW      ← 点查
    Iter k = OLD     ← 范围扫描（Timeline 用的就是这条）
```

> 注：本项与第 1 轮 C1 同源，已修复（`SSTable::max_sequence_` +
> `RecoverLocked` 恢复水位）。**但"两条读路径可见性规则不统一"这个更深的问题仍在**：
> `LsmIterator` 的去重只看最大 sequence，不检查 `seq <= snapshot`（见第 3 轮 N-16）。

---

## High

### H1 `queue_bytes_` 记账漏算 `topic_name` → 队列上限被虚增穿透

**位置**：`recorder.cc:121, 135, 147, 175`

入队 `queue_bytes_ += payload + topic_name`（`:147`）；
出队（`:175`）与丢弃（`:135`）**都漏了 `topic_name.size()`**。
每次"入队+出队"净漂移 `+len(topic_name)`，**单调累积、永不回落**。

漂移量超过上限后，即使 `queue_depth == 0`，`:127` 条件恒真 →
默认 `kDropNewest` 下丢弃每一条新样本，而 `OnSample` 仍 `return true`，**后端无感知**。

**实测**：`received=500 recorded=19 dropped=481 queue_depth=0`（96% 静默丢失）。

> 注：与第 1 轮 C3 同源，**已修复**（抽出 `SampleBytes()` 供三处共用）。

---

### H2 停机期间仍入队且新数据永不被消费（丢数据 + 跨会话污染）

**位置**：`recorder.cc:87-107, 114-117, 164-199`

Stop 顺序：`source_->Stop()` → 置 stop+notify → join → Flush/Close →
**`:106` 才 `running_.store(false)`** → `:107` reset。

**时序**：A 已从 join 返回；B（后端回调，`source_->Stop()` 未等它）在 `:115` 读到
`running_ == true` → `:147` push；writer 早已 break → 这批样本**永久留在 `queue_`，
静默丢失**。若 `close_timeline_on_stop=true`，此时 timeline 已在 `:102` 被 Close。

**二次污染**：再次 `Start()` **不清空** `queue_`/`queue_bytes_`/`topic_ids_`，
残留样本带旧会话时间戳写入新会话。

> 注：我在修 M1 时已把 `running_.store(false)` 提到 `source_->Stop()` **之前**
> （为解 kBlock 阻塞导致的 join 死锁），**恰好修掉了 H2 的主路径**。
> 但 `Start()` 不清空队列/缓存的问题**仍在**（见 M5）。

---

### H3 `OverflowPolicy::kBlock` 完全未实现

**位置**：`recorder.cc:140-142`；`types.h:184`（注释承诺"阻塞等待，保证不丢"）

`case kBlock: break;` 只跳出 switch，随后无条件 push_back。
用户得到"不丢、不阻塞、内存无限增长" → OOM。

> 注：与第 1 轮 M1 同源，**已修复**（真阻塞 + `cv_space_` 通知 + Stop 唤醒）。

---

### H4 `LsmEngine::DeleteRange` 跨锁读改写导致**丢失更新**

**位置**：`lsm_engine.cc:415-448`

**时序**：A 在 `:416-431` 持锁收集 victims={k1,k2}，`:432` 释放锁；
B `Put("k1","new")` 持锁写入 seq=100；
A 在 `:437` 重新加锁，`:438-446` 为 k1 写 tombstone seq=101
→ **B 刚写入的新值被删除**。

范围删除的语义变成"也删除执行期间新写入的数据"；
**对 Recorder 正在录制的区间执行保留策略时会静默删掉刚录的数据**。

第二段（`:437`）**未重新检查 `opened_`**：两段之间若 `Close()`，
`wal_` 已 `fd_=-1` → 返回 IOError 而非 ShutdownInProgress；
`:444` `mem_->Add` 在 `mem_` 已 reset 时是空指针解引用 ——
目前**仅靠 wal_.Append 先失败挡住**，属侥幸正确。

**修法**：整个操作单次持锁完成。

---

### H5 Player 会清除自身的停止请求；载入阶段完全不响应停止

**位置**：`player.cc:50-51, 113-147, 158-160`

**竞态**：窗口在 `:50` 与 `:51` 之间 —— 另一线程此刻 `RequestStop()`（`:30-32`）
的请求被 `:51` `stop_requested_.store(false)` **直接清除** → 回放不停；
`loop=true` 时 `:205` 检查永远为 false，`Play()` **永久阻塞**。

**更常见（窗口极大，不需竞态）**：`stop_requested_` 只在 `:158` 被检查；
`:82-147` 的预扫描（每话题 `CountRange` 全量扫描）与"整个范围载入内存"期间
**完全无检查**。数小时数据下可达数十秒至数分钟，用户按停止后长时间无响应
且内存持续增长。

**修法**：仅在确认上一次 Play 已结束时重置；在每话题循环内与每次
`push_back` 前检查停止。

---

## Medium

| # | 位置 | 问题 |
|---|---|---|
| **M1** | `player.cc:39-42,50,126,210-212` | 缺状态机：早返回让 `playing_` 永久为 true → **Player 永久不可用**。更现实的触发是超大范围下 `std::bad_alloc`（"回放需载入整个范围"正是已知问题 4）——**内存不足一次，Player 永久不可用** |
| **M2** | `player.cc:40-42,50-51,154,193-211` | 并发 Play 无互斥：同一范围被两路发布，`play_start_`/`progress_` 互相覆盖 |
| **M3** | `timeline_impl.cc:678-700,632-673,467-538` | **Timeline 持写锁做 IO** → 阻塞录制线程 → 队列积压 → 丢数据。`Flush()` 持锁逐话题落盘 + LSM fsync；`RunRetention()` 持锁做全库扫描 + 每 key 一次带 fsync 的 WAL 写；`GetStateAt()` 锁内全范围扫描并拷贝 payload |
| **M4** | `memory_engine.cc:25-36,38-47,394-396`；`timeline_impl.h:148` | 快照存活期跨越引擎关闭/重开，无失效机制；`TimelineImpl::retention_points_` 持 `SnapshotPtr` 但 Close/析构都不 Release → MemoryEngine 的 `CompactRange` 因"有活跃快照"**永远拒绝回收版本链**，内存单调增长 |
| **M5** | `recorder.cc:34-79,207-226` | `Start()` 复用实例时不清 `topic_ids_` → 命中旧缓存 → `AppendBatch` 遇非法 id **立即 return** → **整批（最多 256 条）连合法样本一起丢弃**，错误粒度是"批"而非"条" |
| **M6** | `memtable.cc:340-343` | `WAL::Append` 不做短写循环：残片留在文件里，恢复时 `Replay` 在此 `break` → **其后完整记录全部静默丢弃**，且返回 OK |

## Low

- **L1** `memtable.cc:63-81` `Crc32` 静态表惰性初始化非线程安全（当前调用点都在锁下，**未观察到实际问题**）
- **L2** `lsm_engine.h:167` `SetMemTableFlushThreshold` 无同步
- **L3** `recorder.cc:180-182,196-198` "冲刷剩余"是**死代码**（break 时 batch 必为空），注释给人虚假安全感
- **L4** `player.cc:180` 忽略 `sink->Publish()` 返回值 → 后端失败时仍报"回放成功"
- **L5** `player.cc:28` `Player` 析构既不请求停止也不等回放线程 → 跨线程销毁即 UAF
- **L6** `mock_backend.h:90-105` `MockSource::Stop()` 丢弃 `pending_` 未投递样本

---

## 明确**未**发现问题的方面

- **内存序**：逐一核对 `running_`/`stop_requested_` 的 release-acquire 配对、
  `config_` 的发布顺序、统计计数器的 relaxed —— **未发现误用**
- **锁顺序/经典死锁**：全项目锁只有 `Recorder::mutex_`、`TimelineImpl::write_mutex_`、
  `Player::progress_mutex_` 加各引擎自己的锁；调用图中无反向获取 ——
  **未发现锁顺序死锁，也未发现递归加锁**
- **条件变量**：`WriterLoop` 的 `wait_for` 谓词与 `stop_requested_`/`queue_`
  同受 `mutex_` 保护，notify 在状态修改后调用 ——
  **未发现丢失唤醒或未处理的虚假唤醒**（谓词形式天然免疫）
- **LsmEngine 读路径**：公开方法均持 `mutex_`；`SSTable` 打开后只读，
  并发 `Get`/`CollectRange` 安全；`LsmIterator` 持值拷贝 ——
  读路径本身无数据竞争，唯一问题是 H4 的跨锁窗口

---

## 总体评价

1. **二段式录制的方向正确**：回调只入队、writer 线程消费、按字节限流三条都对；
   `WriterLoop` 的谓词式等待、Flush 节流、release/acquire 配对也都写对了。
   作者对"不在回调里做 IO"和"不丢唤醒"有明确意识。

2. **但"线程安全"只落实到"单个操作内部"，没有落实到"对象生命周期"**：
   `Recorder` 与 `Player` 都没有状态机，`Start/Stop/Play` 都是
   "检查标志位→做事→改标志位"的裸序列，无互斥、无 CAS、无 RAII 回滚。
   **最严重的问题（C1/C2/M1/M2/L5）全部出自这一模式**，而非锁的细节。
   实测中并发 Start/Stop **分别在 3/3 和 2/3 的尝试中直接 abort**。

3. **锁的粒度与边界没有分层考虑**：`TimelineImpl` 一把 `write_mutex_` 包办一切
   （含 Flush/Retention/GetStateAt 重 IO，M3）；更关键的是**读路径把迭代器
   送出锁的保护范围**（C3），使"Timeline 有锁"在上层失效 ——
   `IStorageEngine` 的契约在 MemoryEngine 上被明确违反（TSAN 已证实）。
   **这是分层契约被打破，比单点 bug 更该优先修。**

4. **失败路径与生命周期几乎没被当回事**：`playing_` 早返回泄漏（M1）、
   快照无失效机制与 RAII（M4）、`topic_ids_` 跨会话残留（M5）、析构不等回调（C2/L5）
   —— 都属于"正常路径能跑通，异常/重开/复用路径崩"。

### 最危险的并发问题：C1

同时满足三点：后果是进程 abort 或"停止静默失败"；
**触发门槛极低**（只要"启动"与"停止"来自不同线程 —— GUI 按钮、信号处理器、
监控线程、超时看门狗），实测 2~3 次里就崩一次；
其兄弟问题 H2 会在**不崩溃的那几次**里静默丢数据。
**"停止录制"这一最基本操作的正确性目前没有保障。**

### 次危险：C3

它是唯一一个**在正常单线程语义的并发使用（边录边放）下就会命中**的问题，
覆盖全部读路径，后果从"读到不一致数据"到"红黑树遍历失控（活锁）"不等，
**且修复需要动接口** —— 越早定契约越好。

### 建议修复顺序

**C1**（状态机 + 互斥）→ **H1/H2**（记账与停机顺序，改动小收益大）→
**C3**（迭代器所有权契约，影响接口）→ **H4/C4**（存储层正确性）→
**M1/M2/M4**（RAII 化）→ 其余
