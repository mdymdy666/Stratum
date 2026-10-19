# Stratum 生产级 CLI 设计（v1 提案）

> **本文档的定位**：这是"从能跑的库 → 能被人用的工具"的关键一步。
> 所有命令都以项目已有的需求文档为依据，逐条标注来源编号
> （`FR-*` 功能需求 / `SC-*` 场景 / `EC-*` 边缘 / `DP-*` 部署 / `RC-*` 可信度 / `NFR-*` 非功能）。
>
> 状态：~~**设计提案，未实现**~~ → **已实现（2026-10-19 核对）**。
> 原始日期：2026-10-14。
>
> **注意本文档的性质**：它是**设计**，包含当时规划但至今仍未实现的内容
> （如 `stratum watch` / `plan` / `stats` / `daemon`，见 §「未实现的规划」）。
> 若你要查**当前实际支持什么**，以 `stratum --help` 与
> [`08-cli-spec.md`](08-cli-spec.md) 为准 —— 本文档不是现状规格。

---

## 0.0 实现状态（2026-10-19 核对）

> 本表是**逐命令的真实状态**。核对方式：`stratum <命令>` 能否跑通 + `main.cc` 是否有分派。

| 命令 | 状态 | 说明 |
|---|---|---|
| `session` | ✅ 已实现 | `list` / `info` / `merge`（merge 于 2026-10-18 加入） |
| `query` | ✅ 已实现 | 含 `--state-at` 定点状态、`--stream` |
| `verify` | ✅ 已实现 | 含业务层视角（`--business-signals`） |
| `record` | ✅ 已实现 | 含保留策略、队列策略、`--dry-run` |
| `play` | ⚠️ 部分实现 | 基础回放可用；**`--inject` / `--loop-count` 等 6 个选项明确报"暂不支持"**（需 core 层改动） |
| `prune` | ✅ 已实现 | 三层保护，默认 `--dry-run` |
| `types` | ✅ 已实现 | `register` / `list` |
| `ctl` | ✅ 已实现 | |
| `io` | ✅ 已实现 | `export` / `import` / `verify`，含 `--include-context` |
| `doctor` | ✅ 已实现 | 含 `--recover`（只清锁，不修数据） |
| `metrics` | ✅ 已实现 | Prometheus 文本 + JSON，含业务层指标 |
| `probe` | ✅ 已实现 | 读取环形缓冲；`probe status` 另有一入口 |
| `diff` | ✅ 已实现 | 会话对比（2026-10-17 加入），详见 `15-session-diff.md` |
| `config` | ✅ 已实现 | `validate` / `explain` / `lock` |

### 关于 `--help`

- **全局 `stratum --help`** — ✅ 可用，返回 0，列出全部 14 组命令
- **子子命令** `stratum session merge --help` — ✅ 可用
- **中间层 `stratum <命令> --help`** — ❌ **不支持**（返回 2，报"不支持该选项"）

第三条是**有意的设计**（见 `cmd_merge.cc:907` 的注释："其他子命令没有 `--help`，
选项少，未知选项的报错里已给出可用列表"）。

**但这个理由对 `query` / `record` 不成立** —— 它们选项很多。
实测 `stratum query --help` 只会得到 `错误: query 不支持该选项: --help`，
而可用选项列表要等到**报错之后**才显示。

> **2026-10-19 记录为待改进项**：给 14 个命令加 `--help`（或至少让
> `--help` 走"未知选项"的同一报错路径但返回 0）。当前状态不阻塞使用，
> 但违反"显式请求帮助应成功"这条通用约定。

### 未实现的规划（本设计提出、至今未做）

`stratum watch`（topic 行为分析）、`stratum plan`（容量规划）、
`stratum stats`（统计聚合）、`stratum daemon`（守护进程）、`stratum status`（运行时状态）。

这些在本文档的其余章节里作为**设计**出现，**不要**当作可用命令。
其中 `watch` 在 `12-roadmap.md` 里被列为"建议先做"。

---

## 0 为什么先做 CLI

现在的入口是两个原始工具：

```
stratum_rtps_record <秒数> <domain> <目录>     # 参数靠位置，配置靠环境变量
stratum_rtps_replay <时间线> <domain> <速率>
```

能力都在，但**表达不出来**：想"只录 sensor/\* 且保留 24 小时且磁盘满了删最旧的"，
今天只能写代码。需求文档里 `FR-R-1/7`、`DP-02`、`EC-11` 描述的东西，
**没有一条能通过命令行表达**。

**CLI 是把已有能力兑现给用户的那一层。** 也是 MCAP 导出、ROS 2 实测、
查询能力对外暴露的共同前提。

---

## 1 设计原则（8 条，每条都有血泪来源）

| # | 原则 | 来源 |
|---|---|---|
| 1 | **默认安全**：破坏性操作默认 `--dry-run`，必须显式 `--yes` 才执行 | `EC-17` 保留窗口设错导致立即删除 |
| 2 | **可预演**：任何会改数据的命令都必须能先告诉用户"会发生什么" | `EC-13` 目录被误删/误改 |
| 3 | **机器可读**：所有命令支持 `--json`，退出码有明确语义 | `DP-15` CI 集成、`NFR-5.1` 运行时指标 |
| 4 | **幂等**：重复执行同一命令不产生额外副作用 | `DP-07` 无人值守 |
| 5 | **可观测**：长时命令输出结构化进度，可被外部监控抓取 | `DP-08` 远程运维 |
| 6 | **不静默失败**：丢弃、降级、拒绝都必须可见并计数 | `RC-1` 队列溢出、`docs/README.md` 排查纪律 |
| 7 | **时间语义显式**：绝对时间、相对时间、会话内偏移必须能区分 | `EC-01` 时钟倒退、`NFR-2.5` |
| 8 | **一条命令一件事**：不设计 `--do-everything` 开关 | `EC-15` 多录制器误用 |

---

## 2 命令族总览（10 组）

```
stratum record     录制（含保留、分片、触发、调度）      FR-R-*
stratum play       回放（含变速、控制、映射、注入）      FR-P-*
stratum query      查询（含时间范围、定点状态、统计）    FR-Q-*
stratum session    会话管理（列表/信息/删除/合并/校验）  FR-M-2
stratum ctl        运行时控制（暂停/停止某话题/快照）    FR-M-1
stratum prune      数据裁剪与删除（按条件）              ← 需求延伸（见 5.4）
stratum verify     完整性校验与质量报告                  FR-Q-4 / RC-1~6
stratum io         导入导出（bundle / MCAP / rosbag2）   DP-11/12/13
stratum config     配置校验与生成                        DP-10
stratum doctor     自检、诊断、恢复                      EC-12 / NFR-2.1
```

---

## 3 统一选择器语法（本设计的核心）

用户举的例子 `"topic = ... since = ... until = ..."` 点到了关键：
**所有面向数据的命令都应共用一套选择器**，而不是每个命令发明一套参数。

### 3.1 语法

```
<选择器> ::= <条件> (("and" | "or") <条件>)*
<条件>   ::= topic <op> <模式>
          | since <时间点>
          | until <时间点>
          | within <时间区间>
          | session <会话名或 id>
          | seq <序号范围>
          | instance <实例 key>
          | size <比较式>
          | type <类型名>
          | qos.<字段> <比较式>
```

### 3.2 三类时间必须能区分（`NFR-2.5` / `EC-01`）

| 写法 | 含义 | 用途 |
|---|---|---|
| `since 2026-10-14T10:00:00+08:00` | **绝对时刻** | 复盘、审计（`SC-01` `SC-12`） |
| `since -30m` | **相对现在** | 快速定位最近半小时 |
| `since @session+5s` | **会话内偏移** | 精确对齐故障点（`SC-02`） |

> 只支持前两种是不够的：`SC-02`"从故障点往前追"需要**会话内相对时间**。

### 3.3 话题匹配（对应待决策 `Q9` 通配符语义）

```
topic = "sensor/temperature"        # 精确
topic ~ "sensor/*"                  # 前缀通配（项目已实现该语义）
topic ~re "/^sensor\\/temp_[0-9]+$/" # 正则（可选，需 Q9 决策）
topic in (a, b, c)                  # 枚举
```

### 3.4 为什么用表达式而不是一堆 flag

* 可组合：`topic ~ "sensor/*" and since -1h and size > 1KB`
* 可复用：`record` / `query` / `prune` / `export` / `verify` 共用同一套解析
* 可审计：**选择器字符串本身可以写进审计日志**（`SC-12` 要求证明"当时做了什么"）
* 可测试：解析器是纯函数，易于单测

---

## 4 全局约定

### 4.1 时间格式

接受：RFC 3339（`2026-10-14T10:00:00+08:00`）、`HH:MM:SS`、相对（`-30m`/`+2h`/`-3d`）、
会话内（`@session+5s`）、以及 Unix 纳秒（`--ns` 显式声明）。

**歧义必须报错**：`10:00:00` 在不同时区含义不同 —— 除非 `--tz` 或配置里给了默认时区，否则拒绝执行。

### 4.2 退出码（`DP-15` CI 需要）

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 一般错误 |
| 2 | 用法/参数错误 |
| 3 | 目标已存在（如 `record` 拒绝写非空目录） |
| 4 | **完整性校验失败**（`verify` 用，CI 据此判定） |
| 5 | 部分成功（有数据但存在缺口/丢弃） |
| 6 | 环境不满足（权限、`rmem_max` 过小等） |

> 退出码 6 的存在是因为这个项目反复踩到环境问题：`net.core.rmem_max` 默认 208KB
> 导致突发丢包、WSL 内 `sysctl` 无权限。**环境不满足应当是明确的、可区分的失败**，
> 而不是伪装成一般的 I/O 错误。

### 4.3 输出

`--format table|json|yaml|csv`（默认 `table`；检测到非 TTY 时默认 `json`）。
所有 JSON 输出带 `schema_version`，便于脚本长期使用。

### 4.4 危险操作

```
--dry-run        预演，打印将受影响的范围与条数（默认对 prune/del/repair 开启）
--yes            确认执行
--to-quarantine  不真删，移入隔离目录（可恢复）
```

---

## 5 场景清单（28 个）

分组：录制与保留 / 回放 / 查询 / 数据管理 / 运行时控制 / 运维 / 边缘故障。

### 5.1 录制与保留

**S01 · 黑匣子长期常开**（`SC-01` `DP-02` `FR-R-7` `FR-R-10`）
```bash
stratum record --continuous --retain 24h --retain-bytes 50GB \
               --on-full drop-oldest --on-disk-full warn-and-continue \
               --session-name line3 --dir /data/stratum
```
**延伸**：`--retain` 与 `--retain-bytes` **同时给**时必须定义谁先触发（取先到者）；
需要 `stratum session list --retention` 能回答"现在最老的数据是多久以前"。

**S02 · 长跑的容量自管理**（`EC-11` `DP-04` `NFR-2.4` `FR-R-8`）
```bash
stratum record --split-by-bytes 256MB --split-by-duration 1h \
               --min-free 10GB --pre-delete-before-write
```
**延伸**：`--min-free` 触发时的**动作要可选**（停录 / 删最旧 / 告警继续），
且必须能回答"为什么删了"（审计）。这直接对应 `EC-17` 那类事故。

**S03 · 触发式录制**（`DP-03` `SC-08`）
```bash
stratum record --trigger-on "topic = fault/*" --pre-buffer 30s --post-buffer 60s
```
**延伸**：`--pre-buffer` 要求**内存里始终保留最近 30 秒**，这是 `FR-R-7` 之外的新机制；
且环形缓冲的丢弃策略与 `--on-full` 是两回事，配置里必须分开。

**S04 · 按日程录制（循环时间的第一种含义）**（需求延伸）
```bash
stratum record --schedule "0 8-18 * * 1-5" --window 1h --window-keep 8
```
**延伸**：`--schedule` 用 cron 语义；`--window 1h` 表示**每小时切一个 session**，
`--window-keep 8` 是"保留最近 8 个窗口"（与 `--retain` 的时间单位不同，要能共存）。
时区必须显式，否则跨夏令时会错（`NFR-2.5`）。

**S05 · 只要某些话题，别的不要**（`FR-R-1` `FR-R-2` `EC-16`）
```bash
stratum record --include 'topic ~ "sensor/*" or topic = "odom"' \
               --exclude 'topic ~ "sensor/debug_*"' \
               --on-missing-topic fail
```
**延伸**：`--on-missing-topic`（`fail` / `warn` / `wait`）—— `EC-16` 说"配置了不存在的话题名"是常见误用；
`wait` 语义是"等到它出现为止"，对 `DP-01` 短时录制尤其有用。

**S06 · 录制不干扰业务**（`FR-R-6` `NFR-1.2` `NFR-4`）
```bash
stratum record --budget-cpu 0.5 --budget-io 50MB/s --budget-mem 256MB
```
**延伸**：预算**被超限时的行为**要可选（丢数据 / 降采样 / 告警）；
且预算必须能被 `stratum status` 实时看到余量。

### 5.2 回放

**S07 · 原速 / 变速回放**（`FR-P-1` `FR-P-2` `NFR-1.3`）
```bash
stratum play session-20261014 --rate 1.0
stratum play session-20261014 --rate 10 --rate-mode compress-gaps
```
**延伸**：变速时**空档怎么处理**是关键分歧 —— 是等比压缩（`compress-gaps`）还是保持空档只加速有数据段？
两种都要支持，否则"10 倍速跑完 1 小时"和"10 倍速但仍需 1 小时"是两件事。

**S08 · 回放控制：暂停/恢复/单步/跳转**（`FR-P-3` `Q3` 回放控制通道）
```bash
stratum play session-20261014 --interactive      # 前台交互
stratum play ... --control-socket /run/stratum.sock &   # 后台 + 控制通道
stratum ctl play pause  --socket /run/stratum.sock
stratum ctl play seek   --to @session+120s --socket ...
stratum ctl play step   --n 10 --socket ...
```
**延伸**：`Q3` 尚未决策控制通道形态（socket / 信号 / 文件）。
无论选哪个，**控制命令要与数据命令共用选择器**，且 `seek` 必须能按
"会话内偏移"（`@session+120s`）而不是只能按绝对时间。

**S09 · 回放话题映射与冲突保护**（`FR-P-4` `EC-18`）
```bash
stratum play s --remap "sensor/temp=sensor/temp_replay" --on-topic-conflict abort
stratum play s --require-idle 'topic = "sensor/temp"'   # 原话题在发就拒绝
```
**延伸**：`EC-18`"回放时原话题仍在实时发布"是最危险的误用之一（会让下游看到两份数据）。
必须有 `--require-idle` 与 `--on-topic-conflict`（`abort`/`warn`/`suffix`）。

**S10 · 循环回放（循环时间的第二种含义）**（`FR-P-6`）
```bash
stratum play s --loop --loop-count 100 --loop-gap 5s
stratum play s --loop-until "2026-10-14T18:00:00+08:00"
```
**延伸**：循环时**时间戳是否重写**必须可选 —— 下游按源时间戳排序时，
重放同一段时间会产生"时间倒流"。`--rewrite-ts` 要显式。

**S11 · 负载回放压测 / 影子流量**（`SC-09` `SC-11`）
```bash
stratum play s --rate 5 --sink-multiplier 3        # 一份录制，发三份
stratum play s --remap "in=shadow/in" --tag-shadow  # 影子流量，带标记
```
**延伸**：`--tag-shadow` 应当**在负载里加可辨识标记**（而不是只改话题名），
否则下游无法区分真实与影子数据。这是 `SC-09` 能成立的前提。

**S12 · 故障注入**（`SC-08`）
```bash
stratum play s --inject 'drop=5% at=topic:"sensor/*"'
stratum play s --inject 'delay=200ms jitter=50ms'
stratum play s --inject 'reorder=window:10'
```
**延伸**：注入必须**可复现**（`--inject-seed 12345`），否则 `SC-08` 的
"录正常、回放异常"无法做回归对比。

### 5.3 查询与分析

**S13 · 按时间范围读数据**（`FR-Q-2` `SC-01`）
```bash
stratum query s --where 'topic = "sensor/temp" and since @session+60s until @session+120s' \
               --format csv --out temp.csv
```
**延伸**：导出大结果时要支持**流式**（`--stream`）与**分页**（`--limit/--offset`），
否则"查一天的数据"会先把内存吃满。

**S14 · 定点状态重建**（`FR-Q-3` `SC-02`，**这是 Stratum 的差异化能力**）
```bash
stratum query s --state-at @session+90s --topic sensor/temp
stratum query s --state-at 2026-10-14T10:05:00+08:00 --all-topics --format json
```
**延伸**：定点查询要能回答 **dispose 与 out-of-range**（实现里已有 `kDisposed`/`kOutOfRange`），
CLI 必须把它们**区分输出**，而不是都显示成"没有数据"。

**S15 · 完整性报告**（`FR-Q-4` `RC-1~6` `NFR-2.3`）
```bash
stratum verify s --report html --out report.html
stratum verify s --check clock --check gaps --check qos --exit-code-on-issue
```
**延伸**：`RC-1~6` 列了六种"不完整的原因"，报告必须**逐类给出成因**
（本端丢弃 / 发现延迟 / 发布者没发 / 时间轴空洞 / 时钟异常 / 中途重启），
而不是笼统说"有缺口"。这正是项目"数据可信度是一等公民"的落地处。

**S16 · 多会话 / 多节点对齐分析**（`SC-03` `SC-04`）
```bash
stratum query --sessions "line3-*,line7-*" --where 'since -7d' --align-by source_ts
stratum session merge a b c --out merged --align-by source_ts --on-clock-skew warn
```
**延伸**：`EC-02`"多发布者时钟偏差"意味着**跨会话对齐必然有误差**，
必须输出偏差估计，而不是假装对齐是精确的。

**S17 · 性能基线与回归对比**（`SC-05` `SC-10`）
```bash
stratum stats s --group-by topic --percentiles 50,95,99 --json
stratum diff baseline-session new-session --metric rate,latency,p99 --threshold 10%
stratum plan s --horizon 30d --assume-rate-from s   # 容量规划
```
**延伸**：`--threshold` 超限时退出码应为 **4**，这样 CI 能直接用它做性能门禁（`DP-15`）。

### 5.4 数据管理（含用户举例的 `del`）

**S18 · 按条件删除持久化数据**（需求延伸 —— 用户举例）
```bash
# 默认是预演：只打印将被删除的范围与条数
stratum prune s --where 'topic = "sensor/debug_*" and since -7d'
#   将删除: 3 个话题 / 1,284,551 条 / 2.1 GB / 时间范围 2026-10-07T10:00 ~ 2026-10-14T10:00

stratum prune s --where '...' --yes                 # 真删
stratum prune s --where '...' --to-quarantine /data/q --yes   # 移入隔离区（可恢复）
```
**延伸（关键）**：
1. **默认 dry-run 不是可选项** —— `EC-17` 的教训就是"一个设错的窗口立刻删光数据"；
2. 删除要**同时处理元数据**：某话题被删到一条不剩时，`TopicMetadata` 是否保留？
   建议保留并标记 `empty`，否则"这个话题曾存在过"的事实会丢失（违反 `FR-R-11` 自描述）；
3. 删除要**留下审计记录**（谁在什么时候删了什么），因为 `SC-12` 审计场景要求
   "证明当时是这样"，也包括"证明当时删了什么"；
4. 对**正在被录制**的会话执行 prune 需要明确语义（拒绝？还是删除已完成的部分？）。

**S19 · 删除整个会话 / 批量清理**（`FR-M-2` `DP-13`）
```bash
stratum session delete s-20260901 --to-quarantine --yes
stratum session prune --older-than 90d --keep-tagged --dry-run
stratum session tag s-20261001 --tag "release-1.0"
```
**延伸**：`--keep-tagged` 这类"保留例外"是运维必需 —— 没有它，
"清理 90 天前"会把审计要留的那份一起删掉（`SC-12` 与 `DP-04` 的直接冲突）。

**S20 · 归档与冷存**（`DP-13` `DP-11`）
```bash
stratum io export s --bundle out.stratum.tar.zst --include 'topic ~ "sensor/*"'
stratum io archive s --to s3://bucket/ --compress zstd --verify-after
stratum io import out.stratum.tar.zst --into /data/stratum
```
**延伸**：导出必须带**自描述清单**（`FR-R-11`）与**校验和**；
`--verify-after` 是为了避免"归档了但读不回来"（`EC-13`）。

### 5.5 运行时控制（含用户举例的 `stop`）

**S21 · 运行时停止/暂停某些话题**（需求延伸 —— 用户举例）
```bash
stratum ctl stop  --topic 'sensor/debug_*' --socket /run/stratum.sock
stratum ctl resume --topic 'sensor/debug_*' --socket ...
stratum ctl pause  --all   --socket ...      # 全部暂停（保时间轴）
stratum ctl add    --topic 'new/sensor' --socket ...   # 动态加话题
stratum ctl drop   --topic 'sensor/old' --socket ...   # 移除并停止写
```
**延伸（关键）**：
1. **`stop` 与 `pause` 的语义必须区分清楚**：
   * `stop --topic X` = 从此不再录 X（时间轴上 X 有一段空白，**这是真实事实**，应记录）
   * `pause --all` = 暂时不录任何东西（时间轴仍连续，但有一段"无数据"标记）
   两者对 `verify` 报告的影响完全不同，不能合并成一个命令；
2. **`stop` 必须在质量报告里留痕** —— 否则事后看到"X 话题在 10:05 后没数据"，
   会误判成"发布者停了"（`RC-3`）而不是"我们主动停了"。这是可信度的核心问题；
3. **`add` 同样重要**：用户只提到"停"，但现场更常见的是"临时加一个新话题看看"。

**S22 · 快照（录制中固化）**（`FR-R-9` `EC-21`）
```bash
stratum ctl snapshot --name "before-test" --socket ...
stratum session list --snapshots
```
**延伸**：快照必须**可与正在进行的主录制共享底层数据**（不能简单复制，否则
一个 10GB 会话做快照要再花 10GB 与几分钟），但又要**不受后续删除影响** ——
这需要引用计数，属于存储层设计（`FR-S-*`）。

**S23 · 运行时状态与指标**（`NFR-5.1` `DP-08`）
```bash
stratum status --json --socket ...
stratum metrics --format prometheus --listen :9100
```
**延伸**：`RC-1` 的队列溢出必须**在指标里可见**（丢弃计数、队列深度、延迟分位）；
`DP-08` 的远程运维要求的不是"有没有在跑"，而是"**还健康吗、多久会写满**"。

**S24 · 动态调参（不重启）**（`DP-10` `FR-M-1`）
```bash
stratum ctl set --retain 48h --socket ...
stratum ctl set --budget-io 100MB/s --socket ...
stratum ctl rotate --reason "manual-rotate" --socket ...
```
**延伸**：`set --retain` **调小**时会不会立即触发删除？必须要求显式确认
（否则又是 `EC-17`）。且调参要写入会话元数据（`FR-R-11`），
使"这份数据为什么只留了 2 小时"可追溯。

### 5.6 运维与部署

**S25 · 无人值守与自动恢复**（`DP-07` `EC-12` `NFR-2.1` `NFR-1.5`）
```bash
stratum daemon --config /etc/stratum/record.yaml --restart-on-crash --health-check
stratum doctor --recover --dir /data/stratum --dry-run
stratum session repair s-20261014 --truncate-broken-tail --dry-run
```
**延伸**：`EC-12`"进程被强杀/断电"后，**恢复必须能区分**：
* 尾部未落盘的数据（可安全截断）
* 元数据与实际数据不一致（需要重建索引）
* 已经损坏无法恢复的部分（必须**明确报告丢了什么**，而不是静默跳过）

**S26 · 配置校验与生成**（`DP-10` `EC-16` `EC-17`）
```bash
stratum config validate /etc/stratum/record.yaml --strict
stratum config explain  /etc/stratum/record.yaml     # 逐条解释每项会怎样生效
stratum config init --template blackbox > record.yaml
```
**延伸**：`config explain` 是为 `EC-17` 设计的 —— 它应当输出
"按此配置，**最老的数据会在 24 小时后被删除**，磁盘占用上限 50GB"，
让人在启动前就能看出错误。配置校验还应**模拟**运行 30 天看会不会写满。

**S27 · 多实例与并发**（`DP-09` `EC-15` `EC-22` `EC-23`）
```bash
stratum record --instance-id camera-a --lock /data/stratum/.lock
stratum session list --all-instances
stratum doctor --detect-conflict --dir /data/stratum
```
**延伸**：`EC-15`"多个录制器写同一目录"是破坏性误用，必须**靠锁拒绝**；
但"**有意**多实例写同一目录"（如 `SC-04` 多节点汇聚）又要能支持 ——
所以设计应是"默认拒绝 + 显式 `--allow-multi-writer`"，而不是简单禁止。

**S28 · 数据流转：跨机回放与容器**（`DP-06` `DP-11` `DP-12` `DP-14`）
```bash
stratum io export s --bundle - | ssh host 'stratum io import - --into /data'
stratum play s --network-advertise 192.168.100.1 --initial-peers 192.168.100.129
stratum record --foreground --health-check /healthz --uid 1000
```
**延伸**：跨机回放**必须能显式设置本端对外地址**（`advertised_address`）——
这是本项目踩过的坑：不设会回退到本地探测地址，在 NAT 下对端不可达，
症状是"发现成功、SEDP 正常、Writer 已匹配，但一条数据都收不到"。
CLI 应当**在检测到跨网段时主动提示**这个参数。

---

### 5.7 边缘与故障（CLI 层的应对）

**S29 · 时钟异常**（`EC-01` `EC-02` `EC-04` `NFR-2.5`）
```bash
stratum record --clock-policy reject|clamp|accept --clock-skew-warn 100ms
stratum verify s --check clock        # 报告倒退次数、跳变量、受影响条数
```
**延伸**：`EC-01` 时钟倒退时，**时间轴键会乱序**，而 LSM 的有序性依赖
"字节序 == 时间序"这一不变量。CLI 必须让用户选择策略，且**默认策略要安全**
（建议 `clamp` + 告警，而非默默 `accept`）。

**S30 · 磁盘写满**（`EC-11` `NFR-2.4`）
```bash
stratum record --on-disk-full 'stop'|'drop-oldest'|'drop-newest'|'warn' \
               --min-free 10GB --min-free-action rotate
```
**延伸**：磁盘满时"停"和"删旧"对**数据可信度**的含义不同：
"停"留下的是**完整的历史到某一刻**；"删旧"留下的是**最近的但不含起因**。
`SC-01` 复盘通常需要前者，`DP-02` 长期运行通常需要后者 —— 所以必须是配置项，
而且**写进质量报告**（让事后知道当时用的是哪种）。

**S31 · 目录被误删/误改**（`EC-13` `NFR-2.3`）
```bash
stratum verify s --checksum --repair-index --dry-run
stratum doctor --scan --report missing,truncated,corrupt
```
**延伸**：必须能区分三类损坏：**缺失**（文件不在）、**截断**（尾部不完整）、
**损坏**（内容对不上校验和）。三者的可恢复性完全不同。

**S32 · 录制器自身被发现 / 话题抖动**（`EC-05` `EC-09` `EC-10`）
```bash
stratum record --ignore-self --self-participant-name stratum-recorder
stratum verify s --check discovery   # 报告"因发现延迟可能漏录的窗口"
```
**延伸**：`RC-2`"发现延迟导致漏录"是 DDS 固有问题 —— 录制器加入域后
需要时间发现已有发布者，这期间的数据**必然丢失**。CLI 应当
**把这个窗口显式报告出来**（而不是让用户以为录全了）。

---

## 6 优先级与实施顺序

### 第一批（让项目可用）—— 建议先做

| 命令 | 理由 |
|---|---|
| `record`（S01/S05 + 配置文件） | 最核心，`DP-02` 是首要设计目标 |
| `session list / info` | 没有它用户连"录了什么"都不知道 |
| `play`（S07/S09） | 核心闭环 |
| `query --where`（S13） | 把已有的查询能力暴露出来 |
| `verify --report`（S15） | **项目的差异化价值**，必须尽早可见 |
| `config validate / explain`（S26） | 防 `EC-16/17` 这两类最常见误用 |

### 第二批（让它进入工作流）

`ctl`（S21~S24，含用户举例的 `stop`）、`prune`（S18，含用户举例的 `del`）、
`io export/import`、`doctor --recover`、`status --json`。

### 第三批（生态与差异化）

MCAP / rosbag2 导入导出、`play --inject`（S12）、`diff/plan`（S17）、
`metrics --format prometheus`（S23）、Python 绑定。

---

## 7 与待决策问题的关系

| 待决策 | 影响本设计的哪部分 |
|---|---|
| `Q2` 回放追平后切实时 | `play` 的终态语义（结束后是否继续跟随实时） |
| `Q3` 回放控制通道 | S08/S21 的控制通道形态（socket / 文件 / 信号） |
| `Q9` 通配符语义 | 3.3 节选择器的 `~` 到底支持到什么程度 |
| `Q10` 队列满策略 | S02/S30 的 `--on-full` 默认值 |
| `Q7` 是否压缩 | `io export --compress` 的默认行为 |

**本文档不擅自替这些决策拍板**，而是把每处依赖标出来。

---

## 8 一个自我批判：哪些可能是过度设计

诚实列出本设计里**风险较高**的部分：

1. **选择器表达式语言** —— 强大但需要写解析器、写测试、写文档。
   若目标用户只是"录一段、看一眼"，`--topic/--since/--until` 三个 flag 就够了。
   **建议**：先实现三个 flag，把表达式语言作为**语法糖**逐步引入，而不是一开始就上。
2. **`S03` 触发式录制的 pre-buffer** —— 需要在内存里常驻环形缓冲，
   与"录制器不干扰业务"（`FR-R-6`）的预算模型交互复杂。
   `DP-03` 的优先级是 **P2**，建议推迟。
3. **`S17` 的 `stratum plan`** —— 容量规划很有价值，但它依赖
   "速率稳定"的假设，现实中常常不成立。可能不如直接给
   "过去 7 天的增长率"这一条指标实用。
4. **10 组命令族** —— 对 v1 偏多。真实的 v1 大概是
   `record / play / query / session / verify / config` 六组。

**结论**：**先做第一批 6 条命令 + 3 个 flag 的选择器**，
其余按需求反馈增长。CLI 的价值在于"把已有能力兑现"，
不在于"设计得多完整"。
