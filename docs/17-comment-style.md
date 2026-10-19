# 17 · 注释规范（可执行模板）

> 本文不是"建议"，是**验收标准**。写注释前先读这一节，写完对照"验收清单"自查。
>
> 适用范围：`src/include/stratum/`、`src/src/`、`src/cli/`
> **不适用**：`src/include/stratum/rtps/`（Fast DDS 移植头，保持原样，不要改）

---

## 一、为什么要有这份规范

这个项目的注释已经有了相当数量，但质量不均：

- 有些函数写着"完成 X 调用，读取或更新相关运行时状态"—— **这句话对每个函数都成立，因此等于没说**
- 有些类没有类级注释，读者只能从 API 反推它的定位
- 成员变量几乎都没有说明「谁在用它、为什么是这个名字」

**判据**：一段注释如果换到另一个函数上也说得通，它就是**无效注释**。

```cpp
// ❌ 无效：任何函数都能套用
/// 函数功能：完成 Validate 调用，读取或更新相关运行时状态。

// ✅ 有效：说了只有这个函数才成立的事实
/// 校验 batch_size 落在 [1, 1'000'000]。
/// 上界来自 WAL 一条记录的最大长度（见 lsm_wal.cc 的 kMaxRecordBytes）；
/// 超界会在 reserve() 时抛 bad_alloc，所以必须在入口就拦住。
```

---

## 二、类级注释（三个必填项 + 一个条件项）

### 必填 1：定位 —— 它在整个模块里是什么角色

不是"这是什么"，而是"**它为什么存在、别人怎么用它**"。

```cpp
/// @brief 会话的写入端：把 Event 追加到时间线，并维护 topic 元数据。
///
/// @par 模块定位
/// 本类是 `stratum::core` 的**唯一写入口**。读路径由 `Query` 负责，
/// 两者共用 `TimelineImplV2` 的同一把锁（`m_mutex`），所以：
///   · 写与读在同一进程内是互斥的
///   · 跨进程（另一个 Recorder 实例）由 LSM 的文件锁拦截
///
/// @par 上层服务
///   · `stratum record`（cmd_record.cc）—— 唯一的生产调用方
///   · `stratum probe`（cmd_probe.cc）—— 通过 Append 写探针数据
///   · 测试直接调用（tests/unit/test_recorder_*.cc）
///
/// @par 与相邻类的边界
///   · **不做** DDS 监听（那是 `DdsListener`）
///   · **不做** 序列化（那是 `types/`）
///   · **不做** 完整性判定（那是 `verify`）
```

**"不做"这一项尤其重要** —— 它防止下一个人把职责塞进来。

### 必填 2：功能 —— 一句话说清它保证什么

```cpp
/// @par 保证
///  · Append 返回成功 ⇒ 数据已进入 WAL（进程崩溃可恢复）
///  · Append 返回成功 ⇒ 事件的 source_timestamp 单调不减（内部会拒绝回退）
///  · 不保证：写入立即可被 Query 看到（需等 memtable 刷新，见 Flush）
```

### 必填 3：向哪些上层服务（用文件路径点名）

不要写"被多处调用"，要写**具体文件**：

```cpp
/// @par 调用方（按文件）
///   · src/cli/cmd_record.cc:112
///   · src/cli/cmd_probe.cc:648
///   · tests/unit/test_recorder_queue.cc
```

### 条件项：状态机转化表（**只有状态机类才要**）

**判定标准**：类内有 `enum class State/Mode/Phase`，且状态之间由成员函数驱动。

满足时**必须做两件事**：

**(a) 单独开一个 md 文件**：`docs/state/<类名>.md`

其中 `<类名>` 用蛇形命名，例如 `recorder` → `docs/state/recorder.md`。

**(b) 类注释里点名这个文件的相对路径**：

```cpp
/// @par 状态机
/// 本类是状态机：Idle → Running → Stopping → Closed。
/// **完整转化表见 `docs/state/recorder.md`**（该文件是状态机检查的权威依据）。
/// 修改任何状态切换前，必须同步更新那个文件。
```

转化表的格式（`docs/state/<类名>.md` 里）：

```markdown
# Recorder 状态机

## 状态列表

| 状态 | 含义 | 进入条件 | 允许的出口 |
|---|---|---|---|
| `kIdle` | 已构造未启动 | 构造函数完成 | `Start` |
| `kRunning` | 正在接收 | `Start` 成功 | `Stop`, `Abort` |
| `kStopping` | 正在收尾 | `Stop` 调用 | （自动）→ `kClosed` |
| `kClosed` | 已关闭 | `Stop` 完成 或 `Abort` | （无） |

## 转化表

> 格式：`起始状态 ——函数名——> 目标状态`
> **一行只允许一个转化**，便于逐行核对。

```
kIdle -(Start)-> kRunning
kIdle -(Abort)-> kClosed
kRunning -(Stop)-> kStopping
kRunning -(Abort)-> kClosed
kStopping -(内部收尾)-> kClosed
```

## 非法转化（调用即返回错误，且不改变状态）

```
kClosed -(Start)-> ✗ 返回 kError("会话已关闭")
kClosed -(Stop)->  ✗ 幂等返回 kOk（不报错，但状态不变）
kIdle   -(Stop)->  ✗ 返回 kError("未启动")
```

## 每个转化对应的代码位置

| 转化 | 实现位置 | 是否有并发保护 |
|---|---|---|
| `kIdle -(Start)-> kRunning` | `recorder.cc:Start()` | `m_mutex` |
| `kRunning -(Stop)-> kStopping` | `recorder.cc:Stop()` | `m_mutex` |
| ... | ... | ... |

## 检查方式

```bash
# 列出所有修改状态的地方，逐个核对是否在上表里
grep -n "m_state" src/src/core/recorder.cc
```
```

---

## 二点五、注释语法：`//` 与 `///` 都接受

项目里两种风格并存（`//` 普通行注释、`///` Doxygen 风格）。**两种都接受**，
但有两条要求：

1. **同一文件内保持一致** —— 不要一个类用 `///`、下一个类用 `//`
2. **复杂函数用 `///`** —— 因为它的 step 分解会被 IDE 的悬停提示读走；
   简单的一行说明用 `//` 就够

> **为什么不强制统一**：全项目统一成 `///` 需要改动几十个文件的每一行注释，
> 而收益只是"看起来整齐"。相比之下，"每个类都有三要素"才是真正影响可读性的。
> **优先补内容，不优先改语法。**

---

## 三、成员变量注释（三个必填项）

**每个成员都要写**，包括私有的。格式：

```cpp
/// 写锁：保护 m_topics / m_next_seq / m_state。
/// 与 `m_index_mutex` 是**两个独立的锁**，嵌套顺序固定为 m_mutex → m_index_mutex
/// （反过来会死锁，见 docs/state/timeline.md 的说明）。
/// 使用者：Append / Flush / Stop / Query::ListTopics
std::mutex m_mutex;

/// 下一个可用序号。只在持有 m_mutex 时读写。
/// 单调递增，不回收；会话重开后从 1 重新开始（这是刻意的，见 docs/15 §四）。
/// 使用者：Append（取号）、Verify（检测缺口）
uint32_t m_next_seq{1};
```

**三个必填项**：
1. **定位** —— 它是干什么的（一句话）
2. **功能/约束** —— 有什么不变量、和别的成员什么关系
3. **被哪些函数使用** —— 具体函数名

对于**锁、原子量、迭代器**这类容易出问题的成员，额外写：
- 谁保护谁（锁的职责）
- 嵌套顺序（多锁时）
- 是否可重入

---

## 四、复杂成员函数注释（step 分解 + 行内标注）

**判定"复杂"的标准**（满足任一）：
- 函数体超过 **30 行**
- 有 **3 个以上**的分支/循环
- 涉及**加锁 / IO / 状态切换**
- 有**不明显的顺序依赖**

### 开头：step 分解

```cpp
/// 把一批事件写入 WAL 并更新内存索引。
///
/// @par 为什么需要批量
/// 逐条 Append 时每条都要 fsync，实测 2000 条要 3.2s；批量后 0.04s。
/// 所以这是热路径上最重要的优化点。
///
/// @par 执行步骤
///  **step 1** 校验入参（batch 非空、state 为 kRunning、时间戳单调）
///  **step 2** 取序号区间（一次原子加法拿 N 个，避免 N 次 CAS）
///  **step 3** 序列化进 WAL 缓冲区（失败则回滚序号区间）
///  **step 4** 提交 WAL（fsync，这是唯一可能阻塞的步骤）
///  **step 5** 更新 memtable 与 topic 统计（此时对外可见）
///
/// @par 失败处理
///  step 1/2/3 失败：不产生任何副作用，序号区间归还
///  step 4 失败：WAL 可能写了一半 —— 标记会话为 degraded，后续写入全部拒绝
///  step 5 失败：不可能失败（纯内存操作，已预留容量）
///
/// @par 并发
///  全程持有 `m_mutex`。step 4 的 fsync 在锁内是刻意的取舍：
///  让读方看到一致的"已提交"边界，代价是写延迟抖动（见 docs/02 open issue O-3）。
```

### 函数体内：标注"到哪一步"

```cpp
Status Recorder::AppendBatch(const std::vector<Event>& events) {
    // ---- step 1: 校验入参 ----
    if (events.empty()) return Status::Ok();          // 空批次是合法的 no-op
    if (m_state != State::kRunning) {
        return Status::Error("会话未运行");
    }
    for (const auto& e : events) {
        if (e.source_timestamp_ns < m_last_ts) {
            return Status::Error("时间戳回退");       // 见 docs/02 O-5
        }
    }

    // ---- step 2: 一次性取序号区间（N 次 CAS → 1 次）----
    const uint32_t base_seq = m_next_seq.fetch_add(
        static_cast<uint32_t>(events.size()));

    // ---- step 3: 序列化（失败要归还区间）----
    if (!m_wal->Encode(events, base_seq)) {
        m_next_seq.fetch_sub(static_cast<uint32_t>(events.size()));  // 归还
        return Status::Error("WAL 编码失败");
    }

    // ---- step 4: 提交（唯一阻塞点）----
    if (!m_wal->Commit()) {
        m_state = State::kDegraded;                   // 见 docs/state/recorder.md
        return Status::Error("WAL 提交失败");
    }

    // ---- step 5: 更新内存（此时才对 Query 可见）----
    m_memtable->Insert(events, base_seq);
    ...
}
```

**要求**：
- 开头的 step 列表与体内的 `---- step N ----` 标注**必须一一对应**
- 每一段的注释要说清"**为什么这么做**"，不是"做了什么"
- 失败路径也要标注（属于哪一步的失败处理）

---

## 五、验收清单（提交前自查）

一条一条打勾：

- [ ] 类有 `@par 模块定位`（说清为什么存在）
- [ ] 类有 `@par 上层服务`（点名具体文件，不是"多处调用"）
- [ ] 类有 `@par 与相邻类的边界`（至少一条"不做 X"）
- [ ] 若类内有状态枚举：有 `docs/state/<类名>.md`，且类注释里点了这个路径
- [ ] 若类是状态机：转化表**一行一个转化**，非法转化也列了
- [ ] 每个成员变量都有三要素（定位 / 约束 / 使用者）
- [ ] 锁类成员额外写了：保护谁、嵌套顺序
- [ ] 复杂函数（>30 行或有 3+ 分支或有锁/IO）开头有 step 分解
- [ ] 函数体内有 `---- step N ----` 标注，且与开头列表一一对应
- [ ] **没有任何"放在哪个函数上都成立"的空话注释**

### 反例自查（最容易犯的三条）

**① 复述函数名**
```cpp
// ❌ 「完成 Start 调用」——函数名已经说了
void Recorder::Start() { ... }
```

**② 复述代码**
```cpp
// ❌ 「把 i 加 1」——代码自己说了
++i;
```

**③ 万能句**
```cpp
// ❌ 「读取或更新相关运行时状态」——每个函数都是这样
```

**合格的注释回答"为什么"，不是"是什么"**：
```cpp
// ✅ 解释为什么不能用更简单的方式
// 这里不能用 std::filesystem::file_size：它在文件被并发追加时返回旧值，
// 而我们需要"写完之后的大小"才能算出 padding。改用 ftell（见 lsm_wal.cc:212）。
```

---

## 六、状态机类的识别方法

```bash
# 候选 1：有状态枚举
grep -rn "enum class State\|enum class Mode\|enum class Phase" src/include/stratum

# 候选 2：有 kIdle/kRunning 之类的状态常量
grep -rn "kIdle\|kRunning\|kStopping\|kClosed\|kPlaying\|kPaused" src/include/stratum

# 候选 3：有由成员函数驱动的状态字段
grep -rn "m_state\|state_" src/src | grep -v "static"
```

**判定**：若状态只被构造函数设置、之后不变（如"配置快照"），**不算**状态机。
若状态由多个成员函数在不同条件下切换，**算**，必须开 `docs/state/<类名>.md`。

---

## 七、和文档的关系

这份规范只管**代码注释**。`docs/` 下的设计文档管**决策记录**（为什么这样设计、否决了哪些方案）。

两者**不能互相复制**：
- 注释里写"为什么这个字段是这个类型"（贴近代码）
- 文档里写"为什么选这个方案，否决了哪些"（贴近决策）

若注释与文档冲突，**以代码为准，并修正文档** —— 因为代码是可执行的事实。

---

## 八、推进顺序（本规范落地用）

1. **状态机类优先**（`Recorder`，以及盘点出的其它状态机）—— 因为它们最难读懂，收益最大
2. `core/` 的其余类（`TimelineImplV2`、`Query`、`Player`）
3. `storage/`（`ITimelineStorage`、`LsmEngine`、`WAL`、`MemTable`、`StratumDb`）
4. `probe/`（`probe.h` 的 C 接口 + `ProbeConfig`）
5. `config/`（`Config`、`ConfigVar`、`IniFile`）
6. `types/`（`ISerializer` / `JsonSerializer`）
7. `cli/` 的 `cmd_*.cc`（这些不是类，只要求函数级 step 分解）
