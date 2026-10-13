# 04 · 部署与跨主机运行手册

本文档面向"要把 Stratum 跑起来并做跨主机测试"的人，包含**必须的环境配置**、
跨主机拓扑、以及本项目踩过的所有环境类坑。

---

## 1. 必做的系统配置（不做会静默丢包）

### 1.1 调大 UDP 接收缓冲上限

```bash
bash tools/setup_sysctl.sh --persist      # 在【接收端】所在系统执行
# 等价于：
sysctl -w net.core.rmem_max=67108864
```

**为什么必须做**：`net.core.rmem_max` 默认只有 **212992 字节（208KB）**。
Stratum 请求 8MB 接收缓冲，但普通 `SO_RCVBUF` 会被内核**静默截断**到该上限。

代码里已用 `SO_RCVBUFFORCE`（需要 `CAP_NET_ADMIN`）优先尝试绕过，
失败时回退 `SO_RCVBUF` **并打印实际生效值告警**：

```
[reader] 警告：SO_RCVBUF 请求 8388608 字节，实际生效 212992 字节
         （受 net.core.rmem_max 限制，建议 sysctl -w net.core.rmem_max=67108864）
```

**实测影响**（跨主机多 topic 2400 条 × 8KB = 19.2MB）：

| rmem_max | 送达率 |
|---|---|
| 208KB（默认） | **87%** ❌ |
| 64MB | **100%** ✅ |

> **注意**：WSL 内很可能**没有**写 `/proc/sys` 的权限（`permission denied`）。
> 此时请在**接收端所在的实际系统**上执行；WSL 目前只作发送端，影响有限。

### 1.2 确认 systemd/网络就绪

```bash
ip -4 -o addr show eth0        # 确认地址
ip route | head -3             # 确认默认路由
```

---

## 2. 单机运行

```bash
export LD_LIBRARY_PATH=/UserData/Fast-DDS/build-fastdds/src/cpp:/opt/ros/jazzy/lib:/UserData/Fast-DDS/_deps/sysroot/usr/lib/x86_64-linux-gnu
cmake -S . -B build -G Ninja && cmake --build build
bash tools/fastdds_sub_diag/build_tools.sh        # 构建自研工具（不需要 Fast DDS 头）
```

**基线自检**（每次改完代码都应跑）：

```bash
cd build && ctest                                  # 期望 33/33
cd .. && bash tools/fastdds_sub_diag/scenarios/08_accounting.sh
#   期望：对账 A 200/200，序号缺口 0
bash tools/fastdds_sub_diag/scenarios/09_payload_sizes.sh
#   期望：21/20、21/20、11/10（loopback MTU 大，不分片）
```

---

## 3. 跨主机运行（WSL ↔ 独立虚拟机）

### 3.1 拓扑

```
  WSL (172.17.211.143)
        │  直连可达（实测 0% 丢包）──▶  虚拟机 (192.168.100.129)
        ▲                                      │
        └──── Windows 单向 UDP 中继 ◀──────────┘
              （绑 192.168.100.1）
```

* **WSL → 虚拟机**：**直连可达**，不需要中继；
* **虚拟机 → WSL**：被 NAT 挡住，需要中继。

### 3.2 为什么必须是"中继"而不是别的方案

| 方案 | 结论 |
|---|---|
| VMware 桥接 | ❌ Wi-Fi 下不可用（实测：首次 ping 通，随后 ARP 不可达） |
| `netsh portproxy` | ❌ 仅支持 TCP，且需要管理员；RTPS 走 UDP |
| WSL `mirrored` 网络模式 | ❌ 不镜像 VMware 的 VMnet 网卡 |
| **Windows 双向 UDP 中继** | ✅ **可行，且不需要管理员权限** |

### 3.3 启动步骤

**第一步：确认虚拟机在跑**（隔夜/重启后常常已关机）

```powershell
F:\vmware\vmrun.exe list
F:\vmware\vmrun.exe start "F:\xvniji\Centos9_1kelong3\centos9_2.vmx" gui
```

**第二步：启动中继**（Windows 侧，普通用户权限）

```powershell
python -u E:\CodexWorkspace\LearnFastDDS\tools\udp_relay.py ^
    --ports 14900,9360,9361 ^
    --wsl <WSL的IP> --guest 192.168.100.129 ^
    --bind 192.168.100.1
```

端口必须**按 domain 算全**（中继漏了 SPDP 端口就会永远发现不了彼此）：

| 用途 | 端口公式 | domain 30 |
|---|---|---|
| SPDP | `7400 + d*250` | **14900** |
| metatraffic | `9060 + d*10` | **9360** |
| user data | `9061 + d*10` | **9361** |

**第三步：两侧声明地址**（用环境变量，不改代码）

| 侧 | `STRATUM_ADVERTISED_ADDRESS` | `STRATUM_INITIAL_PEERS` |
|---|---|---|
| WSL | `192.168.100.1` | `192.168.100.129` |
| 虚拟机 | `192.168.100.129` | `192.168.100.1` |

**第四步：跑测试**

```bash
bash tools/fastdds_sub_diag/scenarios/23_cross_host_relay.sh 200    # 基本链路
bash tools/fastdds_sub_diag/scenarios/24_cross_host_complex.sh 5    # 复杂场景
bash tools/fastdds_sub_diag/scenarios/25_cross_host_stress.sh 6     # 工业级压测
```

---

## 4. 大 payload（DATA_FRAG）—— 默认全自动

**不需要任何手工配置**。`RtpsWriter::Open` 会：

1. 用 `getsockopt(IP_MTU)` **按对端探测路径 MTU**：
   * 有 `initial_peers` → 探测第一个对端（跨主机得到 1500）；
   * 没有 → 探测 `127.0.0.1`（单机得到 loopback 的 65535）；
2. 令 `max_udp_payload = mtu - 28`（扣掉 IP 20 + UDP 8）；
3. 超过上限的样本**自动走 `DATA_FRAG` 分片**，并保留重传能力（重传时重新切片）。

可用环境变量覆盖：

```bash
STRATUM_MAX_UDP_PAYLOAD=1472     # 显式指定单报文上限（0 = 交给自动探测）
```

> **不要**把探测目标写成固定的外部地址（如 `1.1.1.1`）—— 那会得到默认路由
> （Wi-Fi 1500）的 MTU，于是**单机 loopback 也被强行分片**，实测单机大 payload
> 会从 21/20 掉到 5/20。这是本项目踩过的坑。

---

## 5. domain 选择（避开 ROS 2）

单机基线脚本 `08_accounting.sh` 固定使用 **domain 40**，而不是 0。

原因：开发机上常同时运行 **ROS 2**（默认占 domain 0）。二者会互相干扰并争抢 CPU，
实测对账 B 从 118~119 掉到 **7~17 条且出现 SN 缺口**，基线完全不可信。

```bash
pgrep -a ros2        # 跑基线前确认 domain 0 上没有 ROS 在跑
```

---

## 6. 环境类坑速查

| 现象 | 根因 | 处置 |
|---|---|---|
| 高吞吐丢包（送达 87%） | `rmem_max` 仅 208KB | `tools/setup_sysctl.sh --persist` |
| 大样本跨主机收 0 | IP 分片在 NAT/中继路径被丢 | 已由 DATA_FRAG 自动解决 |
| 单机大 payload 反而变差 | MTU 探测对错了目标 | 已改为按对端探测 |
| 发现不了对端 | 中继漏转发 SPDP 端口 | 端口按 domain 算全 |
| `WinError 10049` | 绑了 VMware NAT 的*内部*网关 `192.168.100.2` | 应绑宿主接口 `192.168.100.1` |
| `WinError 10051` | 用绑定的 socket 往另一网段发送 | 发送用**未绑定**的 socket |
| 同一样本重复交付 | 重传分片再次收齐 | 已用 `RecordReceivedSn` 去重 |
| 基线对账异常 | ROS 2 占用 domain 0 | 改用 domain 40 |

---

## 7. 相关脚本一览

| 脚本 | 用途 |
|---|---|
| `tools/setup_sysctl.sh` | 调大 UDP 接收缓冲（含持久化） |
| `tools/udp_relay.py` | Windows 侧双向 UDP 中继 |
| `tools/wsl_netmode.py` | WSL 网络模式切换（`status`/`mirrored`/`nat`） |
| `scenarios/22_cross_host.sh` | 跨主机基础场景 |
| `scenarios/23_cross_host_relay.sh` | 跨主机一键验证（中继版） |
| `scenarios/24_cross_host_complex.sh` | 多话题 / 大 payload / 随机 / 回放闭环 |
| `scenarios/25_cross_host_stress.sh` | 工业级压测（多 topic 大数据量 / 吞吐阶梯 / 随机） |
| `tests/manual/test_data_frag.cc` | DATA_FRAG 分片报文的字节级验证 |
