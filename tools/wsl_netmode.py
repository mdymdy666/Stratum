#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""WSL 网络模式切换工具（本项目的跨主机测试专用）

文件名：tools/wsl_netmode.py
用途 ：在 `mirrored` 与默认 NAT 之间切换 WSL 的网络模式。

为什么需要它：跨主机 RTPS 测试要求**双向可达**。WSL 处于 NAT 模式时，
虚拟机无法反向访问 WSL（实测：`ping <WSL IP>` 100% 丢包），而 RTPS 的
SPDP/SEDP/ACKNACK 都必须能回得来 —— 发现阶段就完不成。
`networkingMode=mirrored` 让 WSL 与 Windows 共享网络栈，从而拿到宿主机
的可达地址（本例 `192.168.100.1`），并且**组播也能通**。

⚠️ 注意：改完必须 `wsl --shutdown` 重启才生效，**会终止所有 WSL 会话**。

用法（在 Windows 侧执行，需用 `python` 而不是 WSL 里的 python3）：
    python tools/wsl_netmode.py status     # 查看当前模式
    python tools/wsl_netmode.py mirrored   # 切到 mirrored（会备份原配置）
    python tools/wsl_netmode.py nat        # 改回默认 NAT
"""
import shutil
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

CFG = Path(r"C:\Users\m\.wslconfig")
BAK = Path(r"C:\Users\m\.wslconfig.bak-before-mirrored")

MARK_BEGIN = "# === 跨主机测试：WSL 网络模式（由 tools/wsl_netmode.py 管理）==="
MARK_END = "# === 网络模式配置结束 ==="


def read_cfg() -> str:
    if not CFG.exists():
        return ""
    return CFG.read_text(encoding="utf-8", errors="replace")


def active_modes(text: str):
    """返回生效（未被注释）的 networkingMode 行。"""
    out = []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("networkingMode") and not s.startswith("#"):
            out.append(s)
    return out


def strip_managed(text: str) -> str:
    """删掉本工具此前插入的托管块。"""
    lines = text.splitlines()
    out, skipping = [], False
    for line in lines:
        if line.strip() == MARK_BEGIN:
            skipping = True
            continue
        if line.strip() == MARK_END:
            skipping = False
            continue
        if not skipping:
            out.append(line)
    return "\n".join(out)


def ensure_backup() -> None:
    if CFG.exists() and not BAK.exists():
        shutil.copy2(CFG, BAK)
        print("  已备份原配置 -> %s" % BAK)
    elif BAK.exists():
        print("  备份已存在（未覆盖）-> %s" % BAK)


def write_mode(mode: str) -> int:
    text = read_cfg()
    if not text.strip():
        print("错误：%s 不存在或为空" % CFG)
        return 1

    ensure_backup()

    base = strip_managed(text)

    # 同时清掉用户手工加的旧 networkingMode 行，避免重复生效
    kept = []
    removed = []
    for line in base.splitlines():
        s = line.strip()
        if s.startswith("networkingMode") and not s.startswith("#"):
            removed.append(s)
            continue
        kept.append(line)
    if removed:
        print("  已移除旧的 networkingMode 行: %s" % ", ".join(removed))

    block = [MARK_BEGIN,
             "# 由 tools/wsl_netmode.py 管理；改完需 wsl --shutdown 生效。"]
    if mode == "mirrored":
        block.append("networkingMode=mirrored")
        block.append("# 测试结束后请执行：python tools/wsl_netmode.py nat")
    else:
        block.append("# networkingMode=mirrored   # 已改回默认 NAT（此行被注释）")
    block.append(MARK_END)

    body = "\n".join(kept)
    # 插到 [wsl2] 之后（若无该段则放最前）
    lines = body.splitlines()
    out, done = [], False
    for line in lines:
        out.append(line)
        if not done and line.strip().lower() == "[wsl2]":
            out.extend([""] + block)
            done = True
    if not done:
        out = block + [""] + lines

    CFG.write_text("\n".join(out) + "\n", encoding="utf-8")
    return 0


def main() -> int:
    action = sys.argv[1] if len(sys.argv) > 1 else "status"

    if action == "status":
        text = read_cfg()
        modes = active_modes(text)
        print("配置文件: %s（存在=%s）" % (CFG, CFG.exists()))
        print("生效的网络模式: %s" % (", ".join(modes) if modes else "默认 NAT（未显式设置）"))
        print("备份文件: %s（存在=%s）" % (BAK, BAK.exists()))
        return 0

    if action == "mirrored":
        if write_mode("mirrored") != 0:
            return 1
        print("已切换为 mirrored。")
        print("请执行：wsl --shutdown  （会终止所有 WSL 会话）")
        return 0

    if action == "nat":
        if write_mode("nat") != 0:
            return 1
        print("已改回默认 NAT。")
        print("请执行：wsl --shutdown  （会终止所有 WSL 会话）")
        return 0

    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
