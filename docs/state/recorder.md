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

> **一个踩过的坑**：本文件最早写的检查是"往前看 8 行有没有 `lock_guard`"，
> 实测 `state_ = kStopping`（149 行）与 `lock_guard`（139 行）相隔 10 行，
> 于是误报"不在锁内"。**固定行数窗口做不了作用域判断** —— 窗口小会漏报，窗口大会假通过。

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

# 4) 确认所有状态切换都在 lock_guard **作用域**内
#
# ⚠️ 这个检查器本身被写错过两次，所以它必须先自检。
#
# 第一版：`grep -B3` 往前看 8 行
#   → 窗口太小。`state_ = kStopping`(149) 与 `lock_guard`(139) 相隔 10 行，误报。
# 第二版：往上找"最近的未闭合 {"，看那个块里有没有 lock_guard
#   → 找到的是**内层块**：
#       {                          ← 外层块，锁在这里
#         lock_guard(...);
#         switch (state_) {
#           case kRunning: {       ← 第二版找到这个
#             state_ = kStopping;  ← 149
#     内层块里没有锁 → 又误报。
# 第三版（下面这个）：逐层往外找，**任一层**开头有 lock_guard 就算锁内。
#   而且它先用合成样例自检（1 条必须判锁内、1 条必须判锁外），
#   自检不过就不去检查真实代码。
#
# 把下面的内容存成 /tmp/check_lock_scope.py 再运行：
python3 /tmp/check_lock_scope.py
#   预期输出结尾：✓ 全部 7 处都在 lock_guard 作用域内
```

<details>
<summary>点开看 <code>check_lock_scope.py</code> 的源码</summary>

```python
import re, sys

SRC = "src/src/core/recorder.cc"

# 合成样例：验证检查器本身（一条应在锁内、一条应在锁外）
SELF_TEST = """
void Demo::Guarded() {
    {
        std::lock_guard<std::mutex> lock(m);
        switch (x) {
          case 1: {
            state_ = kRunning;        // 应判「锁内」
            break;
          }
        }
    }
}

void Demo::Unguarded() {
    state_ = kIdle;                   // 应判「锁外」
}
"""


def classify(src_lines, idx):
    """逐层往外找：任一层块的起始几行有 lock_guard → 锁内。"""
    depth = 0
    j = idx
    while j >= 0:
        code = re.sub(r"//.*", "", src_lines[j])
        depth += code.count("}") - code.count("{")
        if depth < 0:
            head = "\n".join(src_lines[j:j + 4])
            if "lock_guard" in head:
                return True, j + 1
            depth = 0            # 继续往外层找
        j -= 1
    return False, None


def rows_of(src_lines):
    out = []
    for i, line in enumerate(src_lines):
        if re.search(r"state_\s*=", line):
            g, blk = classify(src_lines, i)
            out.append((i + 1, line.strip(), blk, g))
    return out


# --- 1) 自检 ---
demo = SELF_TEST.splitlines()
r = rows_of(demo)
if sum(1 for x in r if x[3]) != 1 or sum(1 for x in r if not x[3]) != 1:
    print("✗ 检查器自检失败，不能用它的结论")
    sys.exit(2)
print("✓ 检查器自检通过")

# --- 2) 检查真实代码 ---
src = open(SRC, encoding="utf-8").read().splitlines()
r = rows_of(src)
bad = [(x[0], x[1]) for x in r if not x[3]]
for ln, stmt, blk, g in r:
    print("  行 %-5d %-38s 锁层=%-5s %s" % (ln, stmt[:38], str(blk), "锁内" if g else "锁外"))
sys.exit(1 if bad else 0)
```

</details>

**这个检查器验证过结论**（2026-10-19）：
```
✓ 检查器自检通过（1 条判锁内、1 条判锁外）
行 67    state_ = kStarting;   锁层=61     锁内
行 92    state_ = kIdle;       锁层=90     锁内
行 102   state_ = kIdle;       锁层=98     锁内
行 119   state_ = kIdle;       锁层=98     锁内
行 121   state_ = kRunning;    锁层=98     锁内
行 149   state_ = kStopping;   锁层=138    锁内
行 183   state_ = kIdle;       锁层=181    锁内
✓ 全部 7 处都在 lock_guard 作用域内
```

**这个检查本身也要验证**：故意把某一处 `state_ = ...` 挪到锁外，脚本必须报错。
如果挪出去它还说"✓"，说明检查失效了（本项目有过"校验自身会错"的教训）。

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
