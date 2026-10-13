# 示例 04：跨主机收发（NAT + UDP 中继）

**验证什么**：当两端都在 NAT 之后、互相不可直达时，
借助宿主机上的 UDP 中继仍能完成**双向**发现与数据传输。

## 拓扑（本项目实测环境）

```
  WSL (172.17.x.x)  ──── 直连可达 ────▶  虚拟机 (192.168.100.129)
       ▲                                        │
       └──────── Windows UDP 中继 ◀─────────────┘
                 （绑定 192.168.100.1）
```

- **WSL → 虚拟机**：直连可达，不需要中继
- **虚拟机 → WSL**：被 NAT 挡住，必须经中继

## 前置条件（缺一不可）

1. 虚拟机上已部署 Stratum（`/reactor/DDSProject`）且已构建；
2. Windows 侧中继在跑。**推荐用一键脚本**：

   ```powershell
   python \\wsl.localhost\Ubuntu\UserData\CodexWorkSpace\Stratum\tools\start_cross_host.py
   ```

   它会启动虚拟机、读取当前 WSL IP、**按 domain 算出三个端口**启动中继、做健康检查。

   手工方式：

   ```powershell
   python tools\udp_relay.py --ports 14900,9350,9351 ^
       --wsl <WSL_IP> --guest 192.168.100.129 --bind 192.168.100.1
   ```

## 运行

```bash
bash examples/04_cross_host/run.sh
```

## 期望结果

```
--- 1) WSL 回放 -> 虚拟机接收 ---
  虚拟机收到 = 51（期望 50）
--- 2) 虚拟机回放 -> WSL 接收 ---
  WSL 收到 = 51（期望 50）
✅ PASS —— 跨主机双向收发正常
```

## 端口必须算全（最容易踩的坑）

| 用途 | 公式 | domain 73 |
|---|---|---|
| SPDP | `7400 + d*250` | **25650** |
| metatraffic | `9050 + d*10` | **9780** |
| user data | `9051 + d*10` | **9781** |

**漏掉任一个都会静默失败**：
- 漏 SPDP → 永远发现不了对端
- 漏 metatraffic → 发现成功（`peers=1`）但 `sedp_pub=0`、看不到任何 Writer
- 漏 user → 匹配上了但一条数据都收不到

## 两侧地址声明（对称是关键）

| 侧 | `STRATUM_ADVERTISED_ADDRESS` | `STRATUM_INITIAL_PEERS` |
|---|---|---|
| WSL | `192.168.100.1`（中继地址） | `192.168.100.129`（对端真实地址） |
| 虚拟机 | `192.168.100.129`（自身真实地址） | `192.168.100.1`（中继地址） |

## 可调环境变量

| 变量 | 默认 | 说明 |
|---|---|---|
| `DOMAIN` | 73 | DDS domain |
| `VM` | 192.168.100.129 | 虚拟机地址 |
| `WSL_ADV` | 192.168.100.1 | WSL 对外声明地址 |
