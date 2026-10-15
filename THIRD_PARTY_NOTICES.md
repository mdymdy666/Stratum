# 第三方组件与移植来源声明

Stratum 采用**移植优先**的开发策略：能复用成熟实现就不自己重写。
本文档统一声明所有非原创代码的来源、许可与移植范围。

> **为什么必须有这份文件**：项目许可为 Apache-2.0，但移植来的代码
> 各自带有自己的许可（Apache-2.0 / BSD-3-Clause / MIT）。
> 若不在产物中保留上游版权声明，就是**许可合规问题**，而不是文档问题。
>
> 各源文件头部同时带有 `SPDX-License-Identifier` 与来源标注，与本文档互为对照。

---

## 1 汇总

| 组件 | 来源 | 许可 | 移植范围 | 位置 |
|---|---|---|---|---|
| **Fast DDS (RTPS 层)** | eProsima Fast DDS | Apache-2.0 | RTPS 报文/类型/策略层 | `src/include/stratum/rtps/**`、`src/rtps/**` |
| **LSMengine** | 项目自研（见 `design/01-lsmengine-findings.md`） | 本项目 Apache-2.0 | 存储引擎 | `src/include/stratum/storage/lsm/**` |
| **LevelDB** | Google LevelDB | BSD-3-Clause | 存储层设计参考与部分实现思路 | 见 `design/03-lsm-engine-notes.md` |
| **Go2Cpp** | Go2Cpp contributors | **MIT** | 日志模块、INI 配置模块 | `src/include/stratum/logging/**`、`src/include/stratum/config/**`、`src/logging/**`、`src/config/**` |

---

## 2 Fast DDS（RTPS 层）

* **上游**：eProsima Fast DDS <https://github.com/eProsima/Fast-DDS>
* **许可**：Apache License 2.0
* **移植范围**：RTPS 协议栈的报文构造/解析、类型定义、QoS 策略、内建端点等
  （约 83 个头文件，位于 `src/include/stratum/rtps/`）
* **改动**：命名空间 `eprosima::fastdds` → `stratum::rtps`；去除与本项目无关的
  TCP/TLS/共享内存传输；裁剪安全插件
* **策略文档**：`design/09-rtps-porting-strategy.md`
* **版权声明**：Copyright eProsima / Fast DDS contributors

---

## 3 LSMengine

* **来源**：本项目自研 LSM 存储引擎（早期阶段的成果，可由 `design/01-lsmengine-findings.md`
  `design/03-lsm-engine-notes.md` 追溯）
* **许可**：本项目 Apache-2.0
* **说明**：虽属自研，但设计上参考了 LevelDB 的分层结构与 SSTable 布局，
  故在此一并说明，避免"看起来像抄的却没说清来源"。

---

## 4 LevelDB

* **上游**：Google LevelDB <https://github.com/google/leveldb>
* **许可**：BSD 3-Clause License
* **版权声明**：Copyright (c) 2011 The LevelDB Authors. All rights reserved.
* **使用方式**：**设计与算法参考**（MemTable / SSTable / Manifest / 版本机制 /
  比较器语义），并据此自研实现，而非直接复制源码。
* **说明**：本项目**不链接** LevelDB 库，`build/` 产物中不含 LevelDB 目标文件。

---

## 5 Go2Cpp（日志与配置模块）★ 本次新增

* **上游**：`/UserData/CodexWorkSpace/Go2Cpp`（Go2Cpp 运行时语义兼容层）
* **版权**：Copyright (c) 2026 Go2Cpp contributors
* **许可**：**MIT License**
* **移植时间**：2026-10-15
* **移植范围**：

  | 源文件 | 目标 | 行数 |
  |---|---|---|
  | `include/go2cpp/logging/log.hpp` | `src/include/stratum/logging/log.hpp` | 506 |
  | `include/go2cpp/log.hpp` | `src/include/stratum/log.hpp` | 4 |
  | `src/logging/log.cpp` | `src/logging/log.cpp` | 1024 |
  | `include/go2cpp/config/config.hpp` | `src/include/stratum/config/config.hpp` | 496 |
  | `include/go2cpp/config.hpp` | `src/include/stratum/config.hpp` | 4 |
  | `src/config/config.cpp` | `src/config/config.cpp` | 745 |

* **改动**：
  1. 命名空间 `go2cpp::log` → `stratum::log`；`go2cpp::config` → `stratum::config`
  2. 宏 `GO2CPP_LOG_*` → `STRATUM_LOG_*`
  3. **裁剪** `RuntimeConfig` / `LoadRuntimeConfig` / `BindRuntimeConfig` ——
     这三个组件绑定 Go2Cpp 的 M/P/G 调度器，而 Stratum 没有调度器
* **保留**：`IniFile`、`ValueCodec`、`ConfigVarBase`/`ConfigVar`、`Config`
  （含两阶段事务、热更新、校验器/监听器）
* **详细记录**：`docs/10-porting-notes.md`
* **MIT 许可全文**：

```
MIT License

Copyright (c) 2026 Go2Cpp contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

> **合规要点**：MIT 要求"在软件的所有副本或实质性部分中包含上述版权声明和
> 本许可声明"。因此移植文件头部保留了 Go2Cpp 的版权行，且本文档收录了
> 完整的 MIT 文本。**这不是可选项。**

---

## 6 构建期依赖（不进入源码树）

| 依赖 | 用途 | 许可 |
|---|---|---|
| `fastcdr` | CDR 序列化（ROS 2 Jazzy 自带，`/opt/ros/jazzy/include/fastcdr`） | Apache-2.0 |
| 系统 `pthread` / POSIX socket | 线程与网络 | 系统库 |

---

## 7 维护约定

1. **每次移植都必须更新本文档**（新增一节或扩展现有节），包含：来源、版权、
   许可、移植的文件与行数、改动内容、测试情况。
2. **移植文件必须带来源标注**，格式：

   ```cpp
   // Copyright 2026 Stratum Project
   // SPDX-License-Identifier: Apache-2.0
   //
   // 本文件移植自 <上游>（<许可>）
   //   <上游版权行>
   //   Ported to Stratum on <日期>；改动见 docs/10-porting-notes.md
   ```

3. **不得声称移植代码为原创**。上游许可若比 Apache-2.0 更严格
   （如 GPL 系），**不得移植** —— 这一点在选型阶段就要判断。
