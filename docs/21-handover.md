# 21 · 交接文档（给接手的人 / 新 agent）

> 最后更新：2026-10-20 18:00
> **读这一篇就够上手。** 更细的历史看 `docs/20-project-progress.md` 与 `AI_COLLAB.md`。

---

## 一、项目一句话

**Stratum 是 DDS 感知的录制/回放工具**：它用自研的 RTPS 实现录制任意 DDS 话题的原始字节，回放时再发出去；存储用 LSM，零外部依赖。

```
代码:  /UserData/CodexWorkSpace/Stratum   （WSL 开发目录，注意 WorkSpace 的 W 大写）
GitHub: https://github.com/<repo>        （推送走 Windows 侧 + socks5 代理）
分支:  main
状态:  156 提交，74/74 测试通过，22 篇文档，10/10 examples 通过
```

**⚠️ 有一个容易搞错的目录**：`/mnt/e/CodexWorkspace/Stratum` 是 Windows E: 快照（**过时**），只用于 push。**开发一律在 `/UserData/CodexWorkSpace/Stratum`**。

---

## 二、最近三天做了什么（按时间倒序）

### 1. `docs/20` 项目进度总览 + 定时任务收尾

见 `docs/20-project-progress.md`。

### 2. `docs/06` 的 6 个测试缺口全部补完

| # | 缺口 | 提交 | 关键收获 |
|---|---|---|---|
| 1 | `Player` 控制面 | `d0bca56` | `RequestStop` 380ms 返回、发布 4/100（真中断） |
| 2 | 迭代器 `Seek`/`Prev` | `88ee1d4` | **抓出两个后端同款的 `Prev` 死循环** |
| 3 | `MetaCodec` | `dec4ac2` | 高版本拒绝生效、173 种截断 **0 个假成功** |
| 4/5 | `SplitPolicy` + 工厂 | `abfdc84` | `0` 表示「不限」；`dynamic_cast` 双向验证类型 |
| 6 | `Status` 分类判定 | `c6dd69b` | 12 code × 5 判定 = 60 个布尔值对照表 |

### 3. 回放带宽优化（RTPS 批量聚合）

| 提交 | 内容 |
|---|---|
| `4fcc4ae` | `BuildDataPacketBatch`：一个 Message 装多条 DATA |
| `14f8f44` | `RtpsWriter::PublishBatch` |
| `3e6c893` | `ISink::PublishBatch` + `player.cc` 接入 + **修掉无对端静默丢数据 bug** |

**实测**（同机回环，1000 条）：
```
sendto 次数:   1001 → 64      降 15.6 倍
发送窗口耗时:  0.1789s → 0.0101s  快 17.7 倍
回放吞吐:       5589 → 98881 条/秒
接收端收到:     1000 → 1000 条    完整
```
**收益大头是 syscall 减少，不是省字节。**

### 4. 两项调研（**没有落地，但结论重要**）

#### 4.1 Go2Cpp（GMP 协程）是否该引入 → **不该**

写了压测（`/tmp/udp_bench3`，对比"阻塞 recvfrom 线程" vs "IOManager+fiber"）：

```
单 socket:  A 231307 包/秒  B 17665 包/秒   ← fiber 慢 13 倍
多 socket:  两轮结果完全相反（A 快 1.9x / B 快 1.9x 都出现过）
```

**结论**：
- **单 socket 场景 fiber 明确更慢** —— `wait()` 多一次 `epoll_wait` + 一次 fiber 切换
- **多 socket 数据不可信** —— 压测有缺陷（sender 单线程是瓶颈、无 CPU 隔离、端口复用）
- **Stratum 的会话数通常 1~4，单 socket 是主场景** → **不值得引入整个协程运行时**

#### 4.2 QUIC 能否融合 → **能，但只能用在 Stratum↔Stratum**

**硬约束（不可突破）**：

| 层 | 能否用 QUIC | 原因 |
|---|---|---|
| **回放给外部 DDS 应用** | ❌ **绝对不能** | 对端只认 RTPS over UDP；换 QUIC = 对端收 0 条且无报错 |
| **Stratum → Stratum** | ✅ 可以 | 收发两端都是自己 |
| **探针/监控** | ✅ 可以 | 同上 |

**技术可行**：`picoquic` + 自带 `picotls`（TLS 1.3）可静态链接，**不破坏零依赖**。
**Go2Cpp 已有封装起点**：`include/go2cpp/network/quic/picoquic_connection.hpp`
**但**：picoquic 本体未下载，且 **Stratum 目前没有任何跨机传输需求**（`docs/01-requirements.md` 里没有）。

---

## 三、未完成事项（按价值排，直接可做）

### 高价值

1. **`Sink::Print` 固定 4096 缓冲** —— `--decode` 大 JSON 会被**静默截断成非法 JSON**。
   位置：`src/cli/`（grep `4096`）
   修法：按需 `resize`，或直接 `write` 到 stdout 不用中间缓冲。

2. **`cmd_query --stream` 不是真流式** —— 先全量入 `vector` 再 limit，内存峰值 = 全量。
   修法：改成边读边输出，limit 到量就停。

3. **`Flush`/`GetStats`/`Close` 在未 `Open` 时的状态校验**
   判据（来自 `PublishBatchRequiresOpen`）：
   > 如果一个函数的成功返回值会让调用方**误以为某个前提已满足**，它就不该在前提未满足时返回成功 —— 哪怕输入是"无害的空"。

### 中价值

4. **`recvmmsg` 批量收包** —— 与已验证成功的发送批量对称。
   发送侧批量带来了 17.7 倍吞吐提升，接收侧同理。
   **这是当前最值得做的性能优化**（比 GMP/QUIC 都直接）。

5. **`BuildDataPacket` 两个并存入口整改**（结构体版 + 5 参数版）——
   同一件事两个入口，读代码的人会形成两套心智模型。
   整改：5 参数版改成 `DataPacketParams` 的内联包装。**动的是 `Publish` 热路径，需要字节级回归。**

6. **QUIC 跨机传输**（如果确实有需求）——
   优先做"跨机录制同步"（替代 `scp`），因为 `io export` 已经打好包了。

### 低价值

7. 编译警告（6 个，`-Wall -Wextra`）
8. `stratum <cmd> --help` 不支持（14 个命令全返回 2）
9. `io`/`doctor`/`metrics`/`types`/`config` 没有专门 spec 文档
10. `design/` 下 27 篇 markdown 从未审计

---

## 四、铁律（踩过的坑，别重复）

### 命令环境

| 规则 | 说明 |
|---|---|
| **禁止 PowerShell** | 只用 `wsl bash -lc '...'` |
| **含 `$VAR`/`$()`/反引号的命令必须落盘成 `.sh`** | **已踩 20 次** —— Windows 侧 PowerShell 会展开 `$` |
| **禁止 `python -c "..."`** | 一律落盘成 `.py` |
| **git 网络操作只在 Windows 侧** | WSL 里 `getent hosts github.com` 返回 `::1`，push 必败 |

### Git

| 规则 | 说明 |
|---|---|
| **提交用 `git commit -F <文件>`** | `-m` 多行消息会被 shell 破坏（踩过） |
| **不要 push** | 推送走 Windows 侧 Python + socks5 (`127.0.0.1:7899`)，502 长退避 30/60/90 秒 |
| **推送后校验** | `ls-remote` 取到的 HEAD 必须 == 本地 HEAD |

### 测试

| 规则 | 说明 |
|---|---|
| **不能低于 74 个测试** | 当前 74/74 |
| **`test_cli_probe` 单独要 103 秒** | 跑全量时若有并发进程会**假失败**，先确认无并发 |
| **测试会话放 `$HOME`** | 不放 `/tmp`（会被清理） |
| **修 bug 前先写能复现它的测试** | 项目规则 §3 |
| **`enum class` 不能直接喂 `STRATUM_CHECK_EQ`** | 用 `static_cast<int>` |
| **契约测试只测"任何后端都必须满足的性质"** | 别把某个实现的选择写成断言 |

### 编码

| 规则 | 说明 |
|---|---|
| **多步 patch 脚本每步独立判断** | 一处 `sys.exit` 会让后面步骤**静默丢失**（踩过，多绕两轮） |
| **文本锚点替换前确认锚点全文件唯一** | `kSubInfoTs` 这种协议常量名天然多处出现（踩过，改坏了 `BuildSedpAnnouncement`） |
| **写测试前先读结构定义** | 字段名（`first_seen_ns` vs `first_ns`）猜错是编译期才报 |

---

## 五、架构速查

```
Recorder（录制）
  ├─ RtpsReader（自研 RTPS，recvfrom 单线程）
  │    └─ 入 deque<ReceivedSample>（mutex 保护）
  └─ WriterLoop（单线程，批量 256 条 → LSM）

Player（回放）
  ├─ 读 LSM（按时间范围）
  ├─ 按 rate 控速（每条按目标时刻 sleep，不累积漂移）
  └─ RtpsWriter（自研 RTPS，sendto；支持 --batch-size 1~32）

存储
  ├─ TimelineImplV2 → StratumDB → LSMengine（外部 LevelDB，有 Block 前缀压缩）
  └─ TimelineImpl(v1) → LsmEngine（自研 SSTable，无 Block 压缩）

网络
  └─ 只有 RTPS/UDP —— 没有自己的传输层，io export 是文件
```

**关键约束**：DDS/RTPS 互操作 → **回放格式不能改**（Block 压缩、QUIC 都只能用在 Stratum 内部）。

---

## 六、关键文件索引

| 目的 | 文件 |
|---|---|
| 进度总览 | `docs/20-project-progress.md` |
| 交接（本篇） | `docs/21-handover.md` |
| 回放批量优化 | `docs/19-play-batch-optimization.md` |
| 测试缺口 | `docs/06-test-gaps.md` |
| 协作记录 / 发现但未改 | `AI_COLLAB.md` |
| RTPS 报文构造 | `src/rtps/protocol/rtps_packet_utils.cc` |
| RTPS 发送 | `src/rtps/protocol/rtps_writer.cc` |
| 回放主循环 | `src/src/core/player.cc` |
| 录制主循环 | `src/src/core/recorder.cc` |
| 存储契约测试 | `tests/unit/test_storage_contract.cc` |
| 元数据编解码 | `src/src/storage/meta_codec.cc` |

---

## 七、★ 给新 agent 的提示词（直接粘贴）

```text
【接手 Stratum 项目】

## 项目位置
/UserData/CodexWorkSpace/Stratum（WSL 开发目录，注意 WorkSpace 的 W 大写）
⚠️ /mnt/e/CodexWorkspace/Stratum 是过时的 Windows 快照，只用于 push，不要在那里开发。

## 先读这三篇（按顺序）
1. docs/21-handover.md   ← 交接文档（铁律、未完成事项、架构）
2. docs/20-project-progress.md  ← 进度总览
3. AI_COLLAB.md          ← 协作记录与"发现但未改"

## 当前状态
156 提交 / 74 测试全过 / 22 篇文档 / 10 个 examples 全过
分支 main，本地 == 远端

## 优先做这些（按价值排，一篇只做一件）
1. Sink::Print 固定 4096 缓冲 → --decode 大 JSON 被静默截断成非法 JSON（最高价值）
2. cmd_query --stream 不是真流式（内存峰值 = 全量）
3. recvmmsg 批量收包（与已验证的发送批量对称，预期收益最大）
4. Flush/GetStats/Close 在未 Open 时的状态校验
5. BuildDataPacket 两个并存入口整改（需字节级回归）

## 已经做过、不要重复的调研结论
- Go2Cpp（GMP 协程）不该引入：单 socket 场景 fiber 慢 13 倍，
  多 socket 数据不可信；Stratum 会话数通常 1~4。
- QUIC 只能用于 Stratum↔Stratum（跨机传输），绝不能用于回放给外部 DDS 应用
  （对端只认 RTPS over UDP，换了就是收 0 条且无报错）。
- Block 前缀压缩只适用于 Stratum 内部存储/传输，不能用于 RTPS payload。

## 铁律（踩过的坑）
- 禁止 PowerShell，只用 wsl bash -lc
- 含 $VAR/$()/反引号的命令必须落盘成 .sh（已踩 20 次）
- 禁止 python -c "..."，一律落盘 .py
- git 网络操作只在 Windows 侧（Python + socks5 127.0.0.1:7899）
- 提交用 git commit -F <文件>（-m 多行会被 shell 破坏）
- 不要 push（除非明确要求）
- 测试会话放 $HOME 不放 /tmp
- 修 bug 前先写能复现它的测试
- 每轮结束跑 ctest -j1，不能低于 74
- enum class 不能直接喂 STRATUM_CHECK_EQ，用 static_cast<int>
- 多步 patch 脚本每步独立判断，一处 sys.exit 会让后面步骤静默丢失
- 文本锚点替换前确认锚点全文件唯一

## 报告格式（每次实质性回复）
已完成：<本轮的产出>
下一步：<具体动作 或 具体问题>

## 防循环
本 agent 历史上有"连续输出无意义确认词（做/好/执行）"的退化。
- 每轮开始直接调工具，不要先输出长段规划
- 报告只写在最终回复里，不要写在思考块里
- 同一工具同参数不要连续调超过 2 次
- 卡住就停下来提一个具体问题
```

---

## 八、如果只想快速验证环境是否正常

```bash
wsl bash -lc 'cd /UserData/CodexWorkSpace/Stratum && \
  ./build/stratum --version && \
  ./build/stratum query --help > /dev/null; echo "CLI ok" && \
  ctest --test-dir build -j1 --output-on-failure 2>&1 | tail -3'
```

期望：`stratum` 版本号正常、`ctest` 显示 `100% tests passed, 0 tests failed out of 74`。
