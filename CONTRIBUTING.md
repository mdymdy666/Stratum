# 参与开发

面向要改 Stratum 代码或跑验证的人。**最重要的不是流程，而是几条用返工换来的纪律。**

---

## 1. 环境

```bash
# 依赖：CMake >= 3.16、Ninja、g++ (C++17)、Fast DDS（仅工具与互操作测试需要）
cmake -S . -B build -G Ninja
cmake --build build
bash tools/fastdds_sub_diag/build_tools.sh    # 诊断工具（不需要 Fast DDS 头）
```

**上游依赖只读**。本项目在 `src/` 内自带 RTPS 协议栈，不修改 `Fast-DDS` 源码。

---

## 2. 提交前必须做的事

### 2.1 跑基线（零回归是硬要求）

```bash
cd build && ctest                              # 期望 34/34
cd .. && bash tools/fastdds_sub_diag/scenarios/08_accounting.sh
#   期望：对账 A 200/200，序号缺口 0
```

改了 `src/` 之后，**必须先重建工具**（`build_tools.sh`），否则验证用的是旧二进制 ——
本项目吃过"验证工具与源码不一致"的亏。

### 2.2 确认改动真的进了二进制

```bash
# 反面教材：ninja 可能认为无事可做，你的改动根本没被编译
cmake --build build          # 若输出 "ninja: no work to do"，务必警惕
# 可靠做法：
find build -name "<file>.cc.o" -delete && cmake --build build
nm -C build/src/libstratum_rtps_protocol.a | grep <新符号>   # 验证符号存在
```

> **这是本项目最高频的误判来源。** 已因此返工至少 4 次
> （DATA_FRAG 接线、MTU 探测被覆盖、对照实验失效等）。

---

## 3. 测试要求

**禁止以冒烟测试作为验收成果。** 必须有压测、随机测试与真实场景模拟：

| 类型 | 位置 | 要求 |
|---|---|---|
| 单元/集成 | `tests/`（ctest 注册） | 新增纯逻辑应有单测 |
| 真实场景 | `tools/fastdds_sub_diag/scenarios/` | 新功能至少一个场景脚本 |
| 压测 / 随机 | 同上（`04_`/`10_`/`13_`/`14_`/`25_`/`26_`） | 涉及性能或并发时必须 |

**观测手段要先自证可靠**：脚本 `FAIL` 时，先怀疑判据，再怀疑被测对象。

---

## 4. 代码规范

- C++17；文件头写 `SPDX-License-Identifier: Apache-2.0`
- **不静默失败**：构造失败、超限、参数非法都要显式报错并计数。
  本项目超过一半的缺陷都是"静默失败"造成的（详见 `docs/01-resolved-issues.md`）
- **不要重复实现**：报文构造/解析一律复用 `packet::*`，不要在业务层再写一份
- 注释写**为什么**，尤其是"这里曾经怎么写错、症状是什么"

---

## 5. 文档

改动按逻辑单元记录到 [`AI_COLLAB.md`](AI_COLLAB.md)：时间、改动内容、关键文件、
测试结果、后续状态。踩到的坑要写清**症状 → 根因 → 处置**，因为这类信息最难重建。

---

## 6. 平台注意

- **Linux/WSL**：所有构建、测试、场景脚本都在这里跑
- **Windows**：跨主机中继（`tools/udp_relay.py`）与一键准备（`tools/start_cross_host.py`）
- **禁止 PowerShell 写脚本逻辑**：Windows 侧杂务写 Python 脚本
- **行尾**：从 Windows 写 WSL 文本文件时**必须显式 `newline=""`**，
  否则 `\n` 变 `\r\n`，shell 脚本会以
  `set: pipefail: invalid option name` 的形式整个失效

---

## 7. 提交信息

说明**为什么**改，而不只是改了什么。若修复了缺陷，写清症状与根因；
若结论被修正过，也写清——本项目多次出现"先误判、后纠正"，这些记录有长期价值。
