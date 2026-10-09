# 新 session 启动提示词（复制下面代码块全部内容）

```text
接手 Stratum 项目（DDS 感知录制器 / rosbag 风格 + LSM-KV 存储）。
WSL 路径：/UserData/CodexWorkSpace/Stratum
上游只读：/UserData/Fast-DDS

────────────────────────────────────────
第一步：先读这四个文件建立上下文，不要凭印象
────────────────────────────────────────
1. design/12-rtps-interop-rootcause.md   ←【权威】根因结论 + 复验步骤 + 未修问题
2. tools/fastdds_sub_diag/README.md      ← 诊断工具用法（三个关键观测量）
3. design/11-rtps-interop-handoff.md     ← 事实清单（注意：其第 1/6 节假设已被 12 号推翻，顶部有更正说明）
4. AI_COLLAB.md 的"续 16"                 ← 最新时序记录

────────────────────────────────────────
项目现状（截至 2026-10-08，已有实测证据）
────────────────────────────────────────
✅ 实现层已修复：Fast DDS 能发现并匹配 Stratum 的 Writer
   - Fast DDS 侧原话：[diag][EDP] DISCOVERED_WRITER topic="hello_world_topic"
     type="HelloWorld"；[diag][MATCH] current_count=1
   - Stratum 侧：sedp_sub 0→240，[send] has_user=1 -> ...:7439
   - 两个真缺陷已修：
     (a) StratumDB::ListTopics 丢掉 type_name → BuildPublication 返回空
         → 旧代码静默跳过发送（报文从未离开本机）
     (b) RtpsWriter::ReceiveLoop 缺 HEARTBEAT 分支 → 不回 ACKNACK
         → 拿不到对端 SEDP Subscription
✅ 测试：ctest 32/32 通过
⚠️ 剩余阻塞在【环境层】，不是实现缺陷：
   - WSL mirrored networking：7400/7438/7439 被 WSL 之外的进程占用
   - 对照实验：上游 hello_world 自己的 publisher→subscriber 也收不到数据
   - 非 0 domain（3/5）连 SPDP 都收不到
   - 已试无效：禁 SHM、固定端口、initialPeersList、interfaceWhiteList、换 domain

────────────────────────────────────────
可选路线（建议先做 B，它不依赖网络环境）
────────────────────────────────────────
【路线 A】在正常网络环境复验端到端
  步骤见 design/12-rtps-interop-rootcause.md 第 5 节：
    bash tools/fastdds_sub_diag/build.sh
    bash tools/fastdds_sub_diag/build_tools.sh
    DOMAIN=0 bash tools/fastdds_sub_diag/scenarios/01_stratum_replay.sh
  期望：DIAG_SUMMARY ... matched_events=1 samples=32 data_available_calls>=1

【路线 B】修 12 号文档第 6 节的三个已知问题（本机可做完，有明确验收标准）
  B1. ACKNACK 内容不精确：每收到 HEARTBEAT 就请求整个 [firstSN,lastSN]，
      Fast DDS 持续打印 "Inconsistent acknack received ... requested range is [1, 9]"。
      → 改为只请求当前缺失区间。RtpsReader 侧同样需要核对。
  B2. HandleSedpSubscription 把对端 user locator 写给【所有】peer
      （for (auto& p : peers_)）→ 多对端时数据发错目标。
      → 应按 GUID 前缀匹配对应 peer。
  B3. 重传放大：FlushPendingToPeers 每次 ACKNACK/Subscription 后重发【全部】pending，
      实测 8 秒 retrans 涨到 920。
      → 改为按 SN 精确重传并去重。

  验收：每项都要有 test 覆盖 + 量化指标（B3 尤其需要"retrans 不再随
        ACKNACK 次数线性增长"的实测对比）。

────────────────────────────────────────
硬约束（违反会破坏上游或环境）
────────────────────────────────────────
- 上游 /UserData/Fast-DDS 只读：绝不修改已跟踪文件，绝不 git add/commit/checkout/stash/reset
- 禁止 PowerShell：Linux 用 `wsl bash -lc "..."`；Windows 杂务写 Python 脚本到
  .dsh_tmp/ 执行并自删；禁止 `python -c`
- `write` 工具对 \\wsl.localhost\... 路径【读会 EIO、写可以】；
  需要读改 WSL 文件时：写到 .dsh_tmp/ → wsl cp → WSL 侧 python 处理
- tools/ 下的程序不纳入 Stratum 的 CMake；src/ 改动后必须重跑
  tools/fastdds_sub_diag/build_tools.sh，否则验证用的是旧二进制

────────────────────────────────────────
构建与测试
────────────────────────────────────────
cd /UserData/CodexWorkSpace/Stratum
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
cd build && ctest        # 应 32/32 绿

export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:\
/opt/ros/jazzy/lib:\
/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH

────────────────────────────────────────
测试标准（项目要求，不可降级）
────────────────────────────────────────
- 严禁以冒烟测试作为验收成果
- 必须有：压力测试（量化指标）、随机测试、真实场景 ≥5 个且覆盖全部待验证功能
- 真实流量向量缺失时必须【失败】而非静默跳过
- 发现类测试必须用非 0 domain（ROS 2 daemon 占用 domain 0 的 7400）

────────────────────────────────────────
方法论提醒（本项目反复吃亏的地方）
────────────────────────────────────────
1. 不要相信"上游拒绝了"这类推断 —— 拿到上游的原话或返回值
   （本次靠 gdb 看 BuildPublication 返回空 vector 一步定位，胜过几十轮字段猜测）
2. 警惕静默分支：if (!pub.empty()) / if (read_from_cdr_message(...)) 失败时不报错，
   这类分支是排查黑洞；修完顺手加护栏
3. 必须有对照组：没有上游自己也不通的对照，就会把环境问题误判成实现缺陷
4. 不信元信息：版本号、字段长度都可能是错的，用数据自洽性判定
5. 复用优先也适用于重构：把已验证逻辑"重新实现一遍"会引入难察觉偏差
```
