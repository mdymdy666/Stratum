# 用 Python 在 WSL 中准备目录并落盘源码
# 用法: python stage_and_sync.py
import subprocess
import sys
from pathlib import Path

WSL_DISTRO = "Ubuntu"
PROJECT = "/UserData/CodexWorkSpace/Stratum"

# Windows 侧暂存目录
STAGE = Path(r"C:\Users\m\AppData\Local\Temp\stratum_src")


def wsl_bash(cmd: str) -> subprocess.CompletedProcess:
    """在 WSL 中执行 bash 命令"""
    return subprocess.run(
        ["wsl.exe", "-d", WSL_DISTRO, "--", "bash", "-lc", cmd],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def main() -> int:
    # 1) 在 WSL 中建立所需目录
    dirs = [
        "src/include/stratum/core",
        "src/include/stratum/storage",
        "src/include/stratum/protocol",
        "src/include/stratum/types",
        "src/core",
        "src/storage/memory",
        "src/storage/lsm",
        "src/types",
        "src/util",
        "src/cli",
        "src/protocol/rtps",
        "src/protocol/discovery",
        "src/protocol/transport/shm",
        "src/protocol/transport/udp",
        "tests/unit",
        "tests/integration",
        "tests/random",
        "tests/stress",
        "tests/real",
        "benchmarks",
        "design",
        "docs",
    ]
    mk = " && ".join(f"mkdir -p {PROJECT}/{d}" for d in dirs)
    r = wsl_bash(mk)
    if r.returncode != 0:
        print("mkdir failed:", r.stderr)
        return 1

    # 2) 同步暂存区文件到 WSL
    # 用 rsync 语义的 cp：按目录层次复制
    sync_cmd = (
        f'cd /mnt/c/Users/m/AppData/Local/Temp/stratum_src && '
        f'find . -type f \\( -name "*.h" -o -name "*.cc" -o -name "*.cpp" '
        f'-o -name "CMakeLists.txt" -o -name "*.cmake" -o -name "*.in" '
        f'-o -name "*.md" -o -name "*.sh" -o -name "*.py" \\) | '
        f'while read -r f; do mkdir -p "{PROJECT}/$(dirname "$f")"; '
        f'cp -f "$f" "{PROJECT}/$f"; done && echo SYNC_DONE'
    )
    r = wsl_bash(sync_cmd)
    print(r.stdout.strip())
    if r.returncode != 0:
        print("sync failed:", r.stderr)
        return 1

    # 3) 统计
    r = wsl_bash(
        f"cd {PROJECT} && "
        "find src tests benchmarks -type f \\( -name '*.h' -o -name '*.cc' \\) "
        "-print0 2>/dev/null | xargs -0 wc -l 2>/dev/null | tail -1"
    )
    print("code lines:", r.stdout.strip())

    return 0


if __name__ == "__main__":
    sys.exit(main())
