# 代码审查报告 · 第 4 轮：功能完整性与注释一致性

> 审查人：作者自查（专项：**声称支持但未实现的功能**）
> 审查范围：`src/` 全部实现 + 接口定义
> 方法：逐个核对接口注释/配置项声明与实现代码

---

## 为什么做这个专项

前三轮审查分别针对正确性、并发、接口设计。

这一轮针对一个**特别危险的问题类别**：
**代码声称支持某项能力，但实际没有实现（或实现错误）。**

这类问题比"崩溃"更危险，因为：
- 编译通过、测试通过（测试不会去测未实现的功能）
- 用户按文档/注释/配置项使用时，以为能力存在
- 出问题时（如磁盘被写满），根因极难定位

---

## 发现的问题

### C-1 `CompactLocked` 从不删除旧 SSTable 文件 【Critical】

**位置**：`src/src/storage/lsm/lsm_engine.cc:541-553`

```cpp
  // 替换 L1，清空 L0；删除旧文件
  std::vector<std::string> obsolete;
  for (const auto& t : inputs) {
    // 无法从 SSTable 反查路径，这里通过文件号约定重建
    // （首版简化：文件名即文件号，见 TablePath）
  }
  levels_[0].clear();
  levels_[1].clear();
  levels_[1].push_back(table);

  compactions_.fetch_add(1, std::memory_order_relaxed);
  (void)obsolete;
```

**问题**：
1. 注释写"删除旧文件"，但**循环体是空的**，`obsolete` 从未被填充也从未被使用
2. `SSTable` 类**不保存自身路径**，因此调用方无法删除它
3. 旧文件永久留在磁盘上

**后果链条**（这是关键）：

```
compaction 合并 A+B+C → 写出 D        （A、B、C 未删）
   ↓
重启时 RecoverLocked() 调用 ListTableFiles()
   ↓
把**所有** .sst 全部载入 L0 → L0 = [A, B, C, D]
   ↓
L0 文件数 ≥ 4 → 立即触发又一次 compaction
   ↓
再次写出新文件，旧文件仍不删 → 恶性循环
```

实际影响：
- **磁盘空间持续泄漏**：同一份数据在磁盘上存 N 份
- **每次重启都会触发 compaction**，且待合并的数据量只增不减
- 长时间运行 + 反复重启会**耗尽磁盘**

**为什么测试没发现**：测试数据量小、重启次数少，
且没有断言"磁盘文件数量"或"文件总大小"。

**建议修法**：
1. 给 `SSTable` 增加 `const std::string& path() const` 成员
2. `CompactLocked` 中收集 `inputs` 的路径，写入新文件成功后 `::unlink()` 它们
3. **补充测试**：compaction 后断言目录内 `.sst` 文件数不超过预期；重启后断言 L0 文件数不增长

---

### C-2 `OverflowPolicy::kBlock` 未实现，会导致 OOM 【High】

**位置**：`src/src/core/recorder.cc:140-142`
**定义**：`src/include/stratum/core/types.h:184`

定义处的注释：

```cpp
    kBlock,       // 阻塞等待（保证不丢，但可能影响业务）
```

实现：

```cpp
      switch (config_.record.overflow_policy) {
        case RecordConfig::OverflowPolicy::kDropNewest:
          dropped_.fetch_add(1, std::memory_order_relaxed);
          return true;
        case RecordConfig::OverflowPolicy::kDropOldest:
          /* ... 有实现 ... */
          break;
        case RecordConfig::OverflowPolicy::kBlock:
          // 阻塞等待消费者（会拖慢后端线程，需用户显式选择）
          break;          // ← 什么都不做
      }
    }

    queue_.push_back(sample);      // ← 照样入队
    queue_bytes_ += sample_bytes;
```

**问题**：`kBlock` 分支只是 `break`，**没有实现任何阻塞**。
超限后仍然无条件入队。

**后果**：用户选择 `kBlock` 的意图是"宁可慢也不要丢数据"，
实际得到的是**队列无限增长直至内存耗尽（OOM 崩溃）**。
这比 `kDropNewest` 的后果更严重——后者至少是有界的。

**建议修法**（两种都可行，需明确选择）：
- **方案 A（真阻塞）**：在 `kBlock` 分支等待 `cv_` 直到队列有空间。
  注意：回调运行在后端线程上，阻塞它会拖慢接收，
  需在文档中明确这一代价（注释已经提到了，但实现没做）。
- **方案 B（诚实降级）**：若决定不实现阻塞，
  应**删除该枚举值**或在运行时报 `kNotSupported`，
  绝不能静默地退化为"无限制入队"。

**我倾向方案 A**：`kBlock` 是合理的用户需求（审计场景宁可慢也不能丢），
且实现只需在回调中等待条件变量。

---

### C-3 `RecordConfig::topics` 的通配过滤未实现 【High】

**位置**：`src/include/stratum/core/types.h:171-173`（声明）
**实现**：`src/src/core/recorder.cc:50`

声明处的注释：

```cpp
  // 要录制的话题。空表示"全部"（自动发现模式）。
  // 支持精确匹配与前缀通配（以 '*' 结尾）。
  std::vector<std::string> topics;
```

实现中唯一的引用：

```cpp
  protocol::BackendConfig bcfg;
  bcfg.topics = config.record.topics;   // 只做了转发
```

然后 `bcfg` 被传给 `backend_->CreateSource(bcfg, &source)`。
但 `Recorder::OnSample` / `WriteBatch` 中**没有任何过滤逻辑**。

**问题**：
1. **前缀通配（`'*'`）的匹配代码在整个 `src/` 中不存在**
2. 过滤是否生效**完全取决于后端实现**——而当前唯一的实现
   `MockBackend` 直接忽略 `config.topics`
3. 也就是说：**这个配置项目前的实际效果是"无效果"**

**后果**：用户配置 `topics = {"sensor/*"}` 期望只录制传感器数据，
实际行为取决于（尚未实现的）RTPS 后端是否做了过滤。
若后端也不过滤，会录下**全部**话题数据——可能包含隐私数据或无关的高频数据，
占满磁盘。

**建议修法**：
1. **过滤应在 `Recorder` 层实现**（不依赖后端），
   因为这是录制语义，不是传输层职责
2. 实现 `MatchTopic(pattern, name)`：
   - 精确匹配：`pattern == name`
   - 前缀通配：`pattern` 以 `'*'` 结尾时，比较前缀
3. 在 `OnSample` 开头过滤（**最早处**，避免无谓入队）
4. 补充测试：配置 `{"a/*", "b"}`，验证只收到匹配的样本

---

### C-4 `RecordConfig::retention` 从未生效 【Medium】

**位置**：`src/include/stratum/core/types.h:175`

```cpp
  RetentionPolicy retention;
  SplitPolicy split;
```

**核查结果**：
- `TimelineImpl` **实现了**保留机制：
  `SetRetentionPolicy()`（`timeline_impl.cc:603`）、
  `GetRetentionPolicy()`（`613`）、`RunRetention()`
- 但 `Recorder` 中：
  - **从不调用 `SetRetentionPolicy()`** 把 `config.record.retention` 传给时间线
  - **从不调用 `RunRetention()`** 执行淘汰

**后果**：用户配置"保留 7 天"或"最多 100 GB"后，
录制器不会执行任何淘汰 → **磁盘无限增长**，直到写满。

这是 C-1（磁盘泄漏）之外**第二条导致磁盘耗尽的路径**，且更直接。

**建议修法**：
1. `Recorder::Start` 中调用 `timeline_->SetRetentionPolicy(config.record.retention)`
2. writer 线程的空闲分支（已存在，用于定期 `Flush`）
   中周期性调用 `RunRetention()`，间隔取 `retention.check_interval_ns`
3. 统计并暴露"已淘汰的数据量"，供质量报告使用

---

### C-5 `SplitPolicy` 完全未实现 【Medium】

**位置**：`src/include/stratum/core/types.h:161-165`

```cpp
struct SplitPolicy {
  uint64_t max_layer_bytes = 256ULL * 1024 * 1024;  // 默认 256MB
  uint64_t max_layer_duration_ns = 0;
  uint64_t max_layer_events = 0;
};
```

**核查结果**：`src/src/` 中 `SplitPolicy` **零引用**。

**后果**：声称的分层存储能力不存在。
"录制文件超过 256MB 自动分层"这一能力目前无法使用。

**建议**：明确这是未实现功能。
- 短期：在文档与代码注释中标注"未实现"，
  或在设置非默认值时返回 `kNotSupported`
- 长期：实现分层（每个 layer 一个独立的 LSM 实例，
  或利用 SSTable 已有的文件切分）

---

### C-6 `DeleteRange` 的注释与签名不符 【Medium】

**位置**：`src/include/stratum/storage/istorage_engine.h:147-152`

```cpp
  // 按 key 区间批量删除（**Stratum 环形保留的核心操作**）。
  // 删除 [begin, end) 区间内所有 key。
  // 返回删除的 key 数量近似值（后端可返回上限估计）。
  virtual Status DeleteRange(const WriteOptions& options,
                             const std::string& begin,
                             const std::string& end) = 0;
```

**问题**：
1. 注释说"**返回删除的 key 数量近似值**"，
   但返回类型是 `Status`，**根本不返回数量**
2. 两个实现都用 `Status::NotFound("no keys in range")`
   表示"范围内没有 key"

**为什么 `NotFound` 是个问题**：
- 调用方无法区分"范围内本来就没有 key"（正常情况，不是错误）
  与"删除操作失败"
- 而 `DeleteRange` 用于**环形保留**——保留操作在数据稀疏的时间段
  会频繁遇到"无 key"，每次都返回一个"错误"，
  调用方要么忽略错误码（掩盖真实错误），要么需要特判（易漏）

**建议修法**：
1. 修正注释，删除"返回数量"的说法（或者）
2. **改为返回数量**：`virtual Status DeleteRange(..., uint64_t* deleted_out)`
   —— 调用方需要这个信息来统计淘汰量
3. "无匹配 key" 应返回 `Status::OK()` 并把数量置 0，而非 `NotFound`

---

### C-7 `MemoryEngine::Close()` 清空数据，与 LSM 语义相反 【Medium】

**位置**：`src/src/storage/memory/memory_engine.cc:38-46`

```cpp
Status MemoryEngine::Close() {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  if (!opened_) {
    return Status::OK();
  }
  map_.clear();                    // ← 关闭即丢弃全部数据
  active_snapshot_sequences_.clear();
  opened_ = false;
  return Status::OK();
}
```

对比 `LsmEngine::Close()`：

```cpp
  // 关闭前把内存表落盘并显式 fsync，保证数据不丢
  Status s = FlushMemTableLocked(/*sync=*/true);
  wal_.Sync();
```

**问题**：两个实现对 `Close()` 的语义解释**相反**：
- `LsmEngine::Close()`：保存数据（刷盘）
- `MemoryEngine::Close()`：丢弃数据（清空）

`IStorageEngine::Close()` 的接口注释未规定语义，
于是"可替换的后端"这一承诺在此处**不成立**：
把 Memory 换成 LSM，程序行为会改变。

**后果**：应用层若依赖"Close 后再 Open 数据仍在"，
在 Memory 后端上会静默丢失全部数据。
（本项目的端到端测试中已观察到该现象并加了注释说明。）

**建议修法**（二选一，必须明确）：
- **方案 A**：统一为"Close 不改变数据"，`MemoryEngine::Close()` 不清空。
  与 `Open()` 中同样存在的 `map_.clear()` 一并移除。
- **方案 B**：在 `IStorageEngine::Close()` 的注释中**明确规定**：
  "Close 是否保留数据由后端决定；需要持久性的调用方应使用持久化后端"，
  并让 Memory 的行为成为显式契约而非隐含差异。

**我倾向方案 A**：语义更符合直觉，且让两个后端真正可互换。

---

## 汇总

| 编号 | 问题 | 严重度 | 类型 |
|---|---|---|---|
| C-1 | Compaction 不删旧 SSTable → 磁盘泄漏 + 重启恶性循环 | **Critical** | 未实现（注释声称已删） |
| C-2 | `kBlock` 策略未实现 → OOM | **High** | 未实现 |
| C-3 | `topics` 通配过滤未实现 → 可能录下全部数据 | **High** | 未实现 |
| C-4 | `retention` 从未生效 → 磁盘无限增长 | **Medium** | 未接线 |
| C-5 | `SplitPolicy` 零引用 | **Medium** | 未实现 |
| C-6 | `DeleteRange` 注释与签名不符 + `NotFound` 语义混淆 | **Medium** | 契约缺陷 |
| C-7 | 两个引擎的 `Close()` 语义相反 | **Medium** | 契约不一致 |

**共同模式**：7 个问题中有 **5 个**属于同一类——
**接口/注释声明了某项能力，实现中不存在或未接线**。

---

## 本轮结论

1. **最危险的是 C-1 与 C-4**：两条独立的"磁盘无限增长"路径。
   长时间运行必然出问题，且根因不在报错现场。

2. **C-2 与 C-3 反映同一类隐患**：配置项被当作"已完成"，
   但实际是占位符。这类问题会侵蚀使用者对配置系统的整体信任——
   一旦发现有配置不生效，就不得不逐个验证所有配置。

3. **建议的修复顺序**：
   C-1 → C-4（磁盘安全，必修）
   → C-2、C-3（用户可感知的行为错误）
   → C-6、C-7（契约一致性）
   → C-5（功能补齐）

4. **测试改进建议**：本轮 5 个"未实现"问题**没有任何一个能被现有测试发现**。
   需要补充的测试类型：
   - **资源断言测试**：compaction 后检查文件数与总字节数
   - **配置生效测试**：每个配置项都应有"设置后行为确实改变"的断言
   - **长稳定性测试**：反复 compaction + 重启，检查资源是否收敛
