# Recorder 状态机

> **这是状态机检查的权威依据。**
> 修改任何状态切换前，**必须先更新这个文件**；改完后再用本文件末尾的检查方式核对一遍。
>
> 对应代码：`src/src/core/recorder.cc`、`src/include/stratum/core/recorder.h:115`

---

## 一、为什么 Recorder 是一个状态机

`Recorder` 的 `Start()` 是**两段式**的：

1. 锁内抢占 `kIdle → kStarting`（防并发重复启动）
2. **锁外**创建 DDS 数据源（`CreateSource` 可能耗时数百毫秒，因为要做 SPDP 发现）

第 2 步在锁外，所以 **`Stop()` 可能在这期间进来**。这产生了一个必须显式处理的中间态：
`kStarting` 既能走向 `kRunning`（成功），也能走向 `kIdle`（失败或被打断）。

**早期实现的问题**（已修）：`Stop()` 在 `kStarting` 期间只是置标志就返回"成功"，
但 `Start()` 完成后不知道要回滚，于是**录制继续跑、而调用方以为已停止**。

现在的做法：`Stop()` 置 `stop_requested_`，`Start()` 在锁内检查它，看到就回滚。

---

## 二、状态列表

| 状态 | 含义 | 进入条件 | 是否对外可见 |
|---|---|---|---|
| `kIdle` | 未运行（初始态 / 已停止） | 构造完成、`Start` 失败回滚、`Stop` 完成 | 是 |
| `kStarting` | 正在启动（锁外建 DDS 源） | `Start` 抢占成功 | 是（`Stop` 可观察到） |
| `kRunning` | 正在录制 | `Start` 的 `CreateSource` + `source->Start` 都成功 | 是 |
| `kStopping` | 正在停止（锁外停后端 + join） | `Stop` 从 `kStarting`/`kRunning` 转入 | 是（重复 `Stop` 会看到） |

**没有 `kClosed`**：`Recorder` 不持有"已关闭"语义 —— 它可以反复 `Start`/`Stop`。
「关闭会话」是 `RecorderConfig::close_timeline_on_stop` 对 `ITimeline` 的动作，不是 `Recorder` 自身状态。

---

## 三、转化表

> 格式：`起始状态 ——函数名——> 目标状态`
> **一行只允许一个转化**，便于逐行核对；一行写多个转化就等于没写。

```
kIdle -(Start)-> kStarting
kStarting -(Start 内部成功)-> kRunning
kStarting -(Start 内部失败)-> kIdle
kStarting -(Start 检测到 stop_requested_)-> kIdle
kStarting -(Stop)-> kStopping
kRunning -(Stop)-> kStopping
kStopping -(Stop 内部收尾完成)-> kIdle
kIdle -(Stop)-> kIdle
```

**最后一行看着奇怪但是对的**：`Stop()` 在 `kIdle` 时是**幂等**的 —— 返回 `Status::OK()`、
**不改变状态**。所以严格说它不是"转化"，而是"原地不动并成功返回"。
写在这里是为了防止有人以为"`Stop` 一定会改状态"。

---

## 四、非法转化（调用即返回错误，且**不改变状态**）

| 起始状态 | 调用 | 返回 | 说明 |
|---|---|---|---|
| `kStarting` | `Start` | `AlreadyExists("recorder already started (state not idle)")` | 防并发重复启动 |
| `kRunning` | `Start` | `AlreadyExists(...)` | 同上 |
| `kStopping` | `Start` | `AlreadyExists(...)` | 同上 |
| `kStopping` | `Stop` | `AlreadyExists("stop already in progress")` | 防并发重复停止 |
| `kIdle` | `Stop` | `Status::OK()` | **幂等**，不算错误 |

此外 `Start` 还有两个**前置校验**，失败时状态完全不变（连 `kStarting` 都不进）：

| 条件 | 返回 |
|---|---|
| `timeline_ == nullptr` | `InvalidArgument("timeline is null")` |
| `backend_ == nullptr` | `InvalidArgument("backend is null")` |
| `write_batch_size` 不在 `[1, 1000000]` | `InvalidArgument(...)` |

**`write_batch_size` 的边界为什么在那里**（踩过的坑，见 `recorder.cc:46-60` 的长注释）：
- `0` → `while (batch.size() < 0)` 永不成立 → 队列永不消费 → `Stop()` **永久阻塞**
- 超大（`--write-batch-size -1` 经 `strtoull` 变 `SIZE_MAX`）→ `batch.reserve()` 抛
  `length_error`，在后台线程里无人捕获 → `std::terminate`（**SIGABRT，rc=134**）

---

## 五、每个转化对应的代码位置

| 转化 | 实现位置 | 并发保护 | 备注 |
|---|---|---|---|
| `kIdle -(Start)-> kStarting` | `recorder.cc:61-70` | `state_mutex_` | 锁内检查 + 赋值，原子完成"抢占" |
| `kStarting -(Start 内部失败)-> kIdle` | `recorder.cc:98-102` | `state_mutex_` | `CreateSource` 失败 |
| `kStarting -(Start 检测到 stop_requested_)-> kIdle` | `recorder.cc:117-121` | `state_mutex_` | 被 `Stop` 打断，返回 `ShutdownInProgress` |
| `kStarting -(Start 内部成功)-> kRunning` | `recorder.cc:119-121` | `state_mutex_` | `source->Start` 成功 |
| `kStarting -(Stop)-> kStopping` | `recorder.cc:147-149` | `state_mutex_` | `switch` 的 `kStarting` 分支 |
| `kRunning -(Stop)-> kStopping` | `recorder.cc:147-149` | `state_mutex_` | `switch` 的 `kRunning` 分支（与上面共用） |
| `kStopping -(Stop 内部收尾完成)-> kIdle` | `recorder.cc:182-184` | `state_mutex_` | 锁外做完 join/flush 后再取锁赋值 |
| `kIdle -(Stop)-> kIdle`（幂等） | `recorder.cc:141-142` | `state_mutex_` | 直接返回 OK |

> 行号以 2026-10-19 的 `recorder.cc`（407 行）为准。**改代码后请同步更新本表**，
> 否则这张表会变成"看起来权威其实过期"的东西 —— 那比没有更糟。

---

## 六、两个关键时序（必须理解的细节）

### 时序 1：`Start` 与 `Stop` 并发

```
线程 A: Start()
  锁内  kIdle → kStarting                        (recorder.cc:61-67)
  解锁
  锁外  CreateSource(耗时数百 ms) ←────────────┐
                                              │  线程 B: Stop()
                                              │    锁内 看到 kStarting
                                              │         → kStopping
                                              │         置 stop_requested_
                                              │         notify_all
                                              │         移出 writer_thread_/source_
  重新取锁 ────────────────────────────────────┘
  看到 stop_requested_ → state_ = kIdle，返回 ShutdownInProgress
  锁外 join（writer 线程看到 stop_requested_ 会退出）
```

**注意**：`Stop` 已经把 `writer_thread_` 移走了，所以 `Start` 那边 `join` 的是**空线程**，
真正的 join 在 `Stop` 里做。这是刻意的所有权转移（`to_join = std::move(writer_thread_)`）。

### 时序 2：`Stop` 里的顺序不能换

```cpp
// recorder.cc:150-157
stop_requested_.store(true);   // ①
running_.store(false);         // ②
cv_.notify_all();              // ③
cv_space_.notify_all();        // ④
to_join = std::move(writer_thread_);
source_to_stop = std::move(source_);
```

**为什么必须先 notify 再移走 source**（代码里有注释，这里重复一遍）：
后端线程此刻可能正阻塞在 `cv_space_.wait()`（`kBlock` 策略等空间）。
若不先唤醒，`source->Stop()` 里的 join 会永久等待 → **死锁**。

---

## 七、检查方式

```bash
# 1) 列出所有修改状态的地方 —— 逐个核对是否在上面的转化表里
grep -n "state_ *=" src/src/core/recorder.cc

# 预期输出只有这些（10 处）：
#   67   kIdle → kStarting
#   92   → kIdle（CreateSource 失败回滚）
#   102  → kIdle（stop_requested_ 回滚）
#   119  → kIdle（source->Start 失败回滚）
#   121  → kRunning（成功）
#   149  → kStopping
#   183  → kIdle（收尾完成）
# 如果你看到多余的行，说明有人加了未记录的状态切换 → 补进本文件或删掉那段代码

# 2) 列出所有读状态的地方
grep -n "state_ *[!=]=\|switch (state_)" src/src/core/recorder.cc

# 3) 确认每个状态都有出口（不能有"进得去出不来"的状态）
#    对照第二节的状态列表，每个状态在第三节都应至少出现一次作为"起始状态"

# 4) 确认所有状态切换都在锁内
grep -n -B3 "state_ *=" src/src/core/recorder.cc | grep -c "lock_guard"
#   预期与第 1 步的行数一致（每次切换前都取了锁）
```

---

## 八、改状态机时的检查清单

- [ ] 新状态是否在第二节的表格里？没有就加
- [ ] 每个新转化在第三节是否**单独一行**？
- [ ] 非法转化是否在第四节列出？
- [ ] 第五节的行号是否更新？
- [ ] 第五节"并发保护"栏是否写明用哪把锁？
- [ ] 有没有引入"进得去出不来"的状态？
- [ ] 有没有在**锁外**读或写 `state_`？（`state_` 只受 `state_mutex_` 保护）
- [ ] 新增的等待/notify 是否可能在 `source->Stop()` 之前造成死锁？（见第六节时序 2）
- [ ] `Stop` 是否仍然幂等（`kIdle` 时返回 OK）？这是对外契约的一部分
