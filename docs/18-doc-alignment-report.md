# 18 · 文档对齐报告

> 任务：把 `docs/` 下的设计文档与代码实现对齐，找出「文档说的」和「代码做的」不一致。
> 时间：2026-10-19（10:15 ~ 12:30）
> 核对方式：**用可执行的事实验证文档**（`--help` 输出、`main.cc` 分派表、`ctest -N`），而不是读代码猜。

---

## 一、已修正的事实性错误

| # | 文档 | 位置 | 原文 | 改为 | 证据 |
|---|---|---|---|---|---|
| 1 | `src/cli/main.cc`（`--help` 文案） | `PrintHelp()` 的「已实现的子命令」 | 只列 **5 组**（session/query/verify/record/config），漏 9 个 | 列全 **14 组** + 各一句职能 | `grep -oE 'cmd == "[a-z]+"' main.cc` 得 14 个；`./build/stratum --help` 实测 |
| 2 | 同上（`--help` 文案） | 同段 | 「尚未实现（明确报错，不静默）：**play / prune / ctl / io / doctor**」 | 「部分**选项**尚未实现：play 的 --inject 等 6 个」 | 这 5 个命令 `main.cc` 里**都有分派**，且都能跑通。原文是自相矛盾的（既没列在已实现，又说未实现，实际已实现） |
| 3 | `docs/07-cli-design.md` | 第 7 行 | `状态：**设计提案，未实现**。日期：2026-10-14。` | 划掉旧状态 + 标注「已实现（2026-10-19 核对）」+ 说明本文档性质（设计，含未实现规划） | 14 个命令全部有实现 |
| 4 | `docs/07-cli-design.md` | 新增 §0.0 | （无） | 新增「实现状态」表：逐命令标注 ✅/⚠️ + 说明；另列「未实现的规划」（watch/plan/stats/daemon/status） | 逐命令实跑 + `main.cc` 分派表 |
| 5 | `docs/08-cli-spec.md` | 新增 §0.0 | （无） | 新增「本文档的覆盖范围」：明确只详细规定 5 个命令，其余 9 个给出规格指针 | `grep -c "stratum diff"` = **0**、`"session merge"` = **0**、`"include-context"` = **0** |

**第 1、2 条是最严重的**：它们不是"少写了细节"，而是**否认了已存在的能力**。
一个新人看 `--help` 会以为一半功能不存在 —— 而 `--help` 恰恰是他第一个会看的。

---

## 二、发现的待改进项（记录，未改代码）

### 2.1 `stratum <命令> --help` 全部不可用（14 个命令）

实测：

```
stratum query --help    → 错误: query 不支持该选项: --help    退出码 2
stratum diff --help     → 错误: diff 不支持该选项: --help     退出码 2
... 14 个全部退出码 2
```

而 `stratum --help`（全局）和 `stratum session merge --help`（子子命令）**都可用** —— 中间层断了。

**这是有意的设计**，`cmd_merge.cc:907` 有注释：

> "其他子命令没有 `--help`（选项少，未知选项的报错里已给出可用列表）"

**但这个理由对 `query` / `record` 不成立** —— 它们选项很多（`--state-at` / `--stream` /
`--retain` / `--on-full` / `--durability` …），而用户要**先触发一次报错**才能看到可用列表。

**为什么不改**：这是功能增强，超出"文档对齐"的范围。已写进 `docs/07` §0.0 作为待改进项。

**建议修法**：在 `main.cc` 分派前统一拦截 `args` 里的 `--help`，
走"未知选项"的同一输出路径但返回 `kOk`（显式请求帮助应成功）。

### 2.2 `io` / `doctor` / `metrics` / `types` / `config` 五个命令没有专题规格文档

它们只有代码和 `--help`。`--version` 里写"CLI 规范: docs/08-cli-spec.md"，
而 `docs/08` 只在表格里提到过一次 `prune`、一次 `doctor`、四次 `config`。

**已写进 `docs/08` §0.0 作为待办**。

---

## 三、核对通过的部分（避免下次重复检查）

| 文档 | 核对方式 | 结论 |
|---|---|---|
| `docs/15-session-diff.md` | `stratum diff` 实跑 + `test_diff` 12 用例 | ✅ 与实现一致（三个裁决都在代码里） |
| `docs/16-session-merge.md` | `stratum session merge --help` + `test_merge` 12 用例 | ✅ 与实现一致（含 `--skew-warn` 两级阈值） |
| `docs/14-verify-business-layer.md` | `stratum verify` 实跑 + `test_verify_business` | ✅ 业务层字段名与文档一致（`dds_events` / `probe_events`） |
| `docs/13-probe-requirements.md` | `stratum probe` 实跑 + `test_probe` 系列 | ✅ 埋点 API 与文档一致（`STRATUM_PROBE` / `_STR`） |
| `docs/17-comment-style.md` | 用它检查 `Recorder` 的注释 | ✅ 规范可执行（`docs/state/recorder.md` 按它产出） |
| `docs/05-example-test-matrix.md` | `ctest --test-dir build -N` | ✅ 已有人更新过（"75 个测试源文件、70 个 ctest 用例"与实测一致） |

---

## 四、未完成（明确列出）

| 文档 | 状态 | 原因 |
|---|---|---|
| `docs/01-resolved-issues.md`（246 行） | ⬜ 未逐条核对 | 需逐条验证"已解决"是否真在代码里，工作量大 |
| `docs/03-verification-matrix.md`（125 行） | ⬜ 未逐项核对 | 同上 |
| `docs/06-test-gaps.md`（88 行） | ⬜ 未核对 | 里面列的"缺口"可能有些已补 |
| `docs/09-cli-scenarios-deep.md`（540 行） | ⬜ 未核对 | 场景需实跑验证，耗时 |
| `docs/02-open-issues.md`（94 行） | ⬜ 未核对 | 有些 O 系列可能已修（如 O-3 的 fsync 取舍） |
| `docs/04/10/11/12` | ⬜ 未核对 | 优先级低 |

**本轮优先保证的是**：`--help`（用户第一个看）、`docs/07`（状态声明）、`docs/08`（规格覆盖范围）。
这三处影响面最大，且都是"一句话误导"型的错误。

---

## 五、核对方法（供后续复用）

**原则：用可执行的事实验证文档，不读代码猜。**

```bash
# 1) 命令是否真的存在
grep -oE 'cmd == "[a-z]+"' src/cli/main.cc | sed 's/cmd == "//;s/"//' | sort

# 2) 选项是否真的支持
./build/stratum <命令> <必填参数>          # 缺参数时的报错会列出可用选项

# 3) 退出码是否真的如文档所说
./build/stratum <命令> ...; echo $?

# 4) 测试数是否如文档所说
ctest --test-dir build -N | tail -2

# 5) 文档提到的命令是否都真实存在
grep -oE "stratum [a-z]+" docs/*.md | sed 's/.*stratum //' | sort -u \
  | while read c; do grep -q "cmd == \"$c\"" src/cli/main.cc || echo "幽灵命令: $c"; done
```

**第 5 条最有价值** —— 它一键找出"文档里提到但代码里没有"的命令。
本轮就是靠它发现 `daemon` / `plan` / `stats` / `watch` / `status` 这 5 个。

---

## 六、一个观察：为什么漂移会积累

`docs/` 的 19 篇文档里，**05 / 07 / 08 / 13 / 14 / 15 / 16** 是逐轮任务中写下的，
每篇都只覆盖当时的功能。**没有一个"总表"** 会随功能增加而更新。

于是：

- 加了 `diff` / `metrics` / `probe` / `session merge` → 各自写了专题文档（15/16/13/14），
  但 `docs/08`（CLI 规格总表）**没回头补**
- `--help` 的文案也是手写的，加了 9 个命令没同步

**这类漂移不是某个人的疏忽，而是"没有单一数据源"的必然结果。**

**根治建议**（写在这里，不本轮做）：让 `--help` 与文档的命令表**从同一个注册表生成**
（如 `cli_common.h` 里一个 `kCommandRegistry[]`），代码与文档都读它。
这样加命令时漏改一处就会被编译期或测试抓到。
