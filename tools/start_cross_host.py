#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Stratum 跨主机测试环境一键准备（Windows 侧，普通用户权限）。

File    : tools/start_cross_host.py
Purpose : 把"跨主机测试"需要手工做的四件事一次做完：
            1. 确认/启动 VMware 虚拟机（vmrun）
            2. 读取当前 WSL 的 IP
            3. 用正确的端口与地址启动 UDP 中继（后台）
            4. 健康检查：虚拟机 SSH 可达、中继端口在监听
          最后打印可以直接复制执行的测试命令。

为什么需要它：跨主机测试每次都要重复这几步，而其中两个坑很容易踩——
  · 虚拟机隔夜常常已经关机（vmrun start 后要等它起来）；
  · 中继端口必须按 domain 算全：SPDP=7400+d*250、metatraffic=9050+d*10、
    user=9051+d*10。漏一个（尤其是 SPDP 或 metatraffic）就会表现为
    "发现不了对端"或"发现成功但一条数据都收不到"，且没有任何报错。

用法（Windows）：
    python tools\\start_cross_host.py                 # 默认 domain 30
    python tools\\start_cross_host.py --domain 30 --no-vm   # 跳过虚拟机启动

退出码：0 = 环境就绪；非 0 = 有环节未就绪（并打印下一步该做什么）。
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

VMRUN = r"F:\vmware\vmrun.exe"
VMX = r"F:\xvniji\Centos9_1kelong3\centos9_2.vmx"
HERE = Path(__file__).resolve().parent
RELAY = HERE / "udp_relay.py"

WSL_MTU_SAFE_PAYLOAD = 1472      # 记录用：MTU 1500 下不分片上限


def log(msg):
    print(msg, flush=True)


def run(cmd, timeout=180):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=timeout)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, "(timeout)"
    except FileNotFoundError as exc:
        return 127, str(exc)


def wsl(script, timeout=60):
    return run(["wsl", "bash", "-lc", script], timeout=timeout)


def get_wsl_ip():
    """取 WSL 的 eth0 地址。

    为什么用 `hostname -I` 而不是 `ip ... | awk '{print $4}'`：
    从 Windows 通过 wsl.exe 调用时，命令里的 `$4`、引号会被参数处理吞掉，
    得到的是空串或错误输出（本项目在其它脚本上已踩过多次）。
    `hostname -I` 不含 `$`，把解析放到 Python 侧最稳。
    """
    rc, out = wsl("hostname -I")
    if rc != 0:
        return ""
    for tok in (out or "").split():
        tok = tok.strip()
        # 排除回环与 docker 网桥（172.17.0.1 等），优先取 WSL 的 eth0
        if re.match(r"^\d+\.\d+\.\d+\.\d+$", tok) and not tok.startswith("127."):
            return tok
    return ""


def tcp_open(host, port, timeout=4):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def vm_running():
    rc, out = run([VMRUN, "list"])
    return "centos9_2" in (out or "")


def ensure_vm(guest_ip, wait_rounds=12):
    log("[1/4] 虚拟机")
    if tcp_open(guest_ip, 22):
        log("      已在运行且 SSH 可达 ✓")
        return True
    if not vm_running():
        log("      未运行，执行 vmrun start ...")
        rc, out = run([VMRUN, "start", VMX, "gui"])
        log("      vmrun rc=%d %s" % (rc, (out or "").strip()[:160]))
    else:
        log("      进程在，但 SSH 未就绪，等待 ...")
    for i in range(1, wait_rounds + 1):
        time.sleep(10)
        if tcp_open(guest_ip, 22):
            log("      第 %d 次探测：就绪 ✓" % i)
            return True
        log("      第 %d 次探测：未就绪" % i)
    log("      ✗ 虚拟机 SSH 仍不可达")
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--domain", type=int, default=30)
    ap.add_argument("--guest", default="192.168.100.129")
    ap.add_argument("--bind", default="192.168.100.1",
                    help="中继绑定的宿主地址（两侧都能到达的那个）")
    ap.add_argument("--no-vm", action="store_true", help="跳过虚拟机启动")
    args = ap.parse_args()

    d = args.domain
    spdp = 7400 + d * 250
    meta = 9050 + d * 10
    user = 9051 + d * 10
    ports = "%d,%d,%d" % (spdp, meta, user)

    log("=" * 68)
    log("Stratum 跨主机环境准备（domain=%d）" % d)
    log("  SPDP=%d  metatraffic=%d  user=%d" % (spdp, meta, user))
    log("=" * 68)

    # ---- 1) 虚拟机 ----
    if args.no_vm:
        log("[1/4] 虚拟机：已按 --no-vm 跳过")
    elif not ensure_vm(args.guest):
        log("\n下一步：确认 VMware 能启动该虚拟机后重跑本脚本。")
        return 1

    # ---- 2) WSL IP ----
    log("[2/4] WSL 地址")
    wsl_ip = get_wsl_ip()
    if not wsl_ip:
        log("      ✗ 取不到 WSL 的 eth0 地址（WSL 是否在运行？）")
        return 2
    log("      %s ✓" % wsl_ip)

    # ---- 3) 启动中继 ----
    log("[3/4] 启动 UDP 中继")
    if not RELAY.exists():
        log("      ✗ 找不到 %s" % RELAY)
        return 3
    cmd = [sys.executable, "-u", str(RELAY),
           "--ports", ports,
           "--wsl", wsl_ip,
           "--guest", args.guest,
           "--bind", args.bind]
    log("      " + " ".join(cmd))
    # 后台启动，日志落到文件；不阻塞本脚本
    logdir = HERE / "logs"
    logdir.mkdir(exist_ok=True)
    logfile = logdir / ("relay_d%d.log" % d)
    try:
        f = open(logfile, "w", encoding="utf-8")
        subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT,
                         creationflags=getattr(subprocess, "CREATE_NEW_CONSOLE", 0))
        log("      已后台启动，日志：%s" % logfile)
    except OSError as exc:
        log("      ✗ 启动失败：%s" % exc)
        return 3
    time.sleep(2)

    # ---- 4) 健康检查 ----
    log("[4/4] 健康检查")
    ok = True
    if tcp_open(args.guest, 22):
        log("      虚拟机 SSH(22) 可达 ✓")
    else:
        log("      虚拟机 SSH(22) 不可达 ✗")
        ok = False
    rc, out = wsl("timeout 3 bash -c 'echo > /dev/tcp/%s/22' 2>/dev/null && echo UP || echo DOWN"
                  % args.guest)
    if "UP" in (out or ""):
        log("      WSL -> 虚拟机 可达 ✓")
    else:
        log("      WSL -> 虚拟机 不可达 ✗")
        ok = False

    log("")
    log("=" * 68)
    if ok:
        log("环境就绪。在 WSL 侧执行：")
        log("")
        log("  cd /UserData/CodexWorkSpace/Stratum")
        log("  bash tools/fastdds_sub_diag/scenarios/23_cross_host_relay.sh 200")
        log("  bash tools/fastdds_sub_diag/scenarios/25_cross_host_stress.sh 6")
        log("")
        log("两侧声明的环境变量（脚本已内置，手工跑时需要）：")
        log("  WSL    : STRATUM_ADVERTISED_ADDRESS=%s  STRATUM_INITIAL_PEERS=%s"
            % (args.bind, args.guest))
        log("  虚拟机 : STRATUM_ADVERTISED_ADDRESS=%s  STRATUM_INITIAL_PEERS=%s"
            % (args.guest, args.bind))
        log("=" * 68)
        return 0
    log("环境未就绪，请按上面 ✗ 的提示处理。")
    log("=" * 68)
    return 4


if __name__ == "__main__":
    sys.exit(main())
