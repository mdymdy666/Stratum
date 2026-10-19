#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查文档体系的一致性（可放进 CI 或改完文档后手动跑）。

三条检查：
  1. **相对链接是否断链** —— 用户点进去 404 比少写一段话更伤信任
  2. **docs/ 下每篇是否被 docs/README.md 索引** —— 否则等于藏起来了
  3. **文档里提到的 stratum 子命令是否真实存在** —— 防止"幽灵命令"

用法：
    python3 tools/check_doc_links.py            # 全部检查
    python3 tools/check_doc_links.py --links    # 只查断链

退出码：0 全部通过 / 1 发现问题
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOCS = REPO / "docs"
DESIGN = REPO / "design"
MAIN_CC = REPO / "src" / "cli" / "main.cc"

LINK_RE = re.compile(r"\[([^\]]*)\]\(([^)\s]+)\)")


def check_links() -> list:
    """返回 [(md, 行号, 链接文字, 目标)]"""
    problems = []
    files = sorted(DOCS.rglob("*.md")) + sorted(DESIGN.rglob("*.md"))
    for md in files:
        try:
            text = md.read_text(encoding="utf-8")
        except OSError:
            continue
        for m in LINK_RE.finditer(text):
            label, target = m.group(1), m.group(2)
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            path_part = target.split("#")[0]
            if not path_part:
                continue
            # Markdown 规则：相对当前 md 所在目录
            resolved = (md.parent / path_part).resolve()
            if not resolved.exists():
                line_no = text[: m.start()].count("\n") + 1
                problems.append((md.relative_to(REPO), line_no, label, target))
    return problems


def check_indexed() -> list:
    """docs/ 下每篇是否出现在 docs/README.md 里"""
    index = DOCS / "README.md"
    if not index.exists():
        return []
    idx_text = index.read_text(encoding="utf-8")
    missing = []
    for md in sorted(DOCS.glob("*.md")):
        if md.name == "README.md":
            continue
        if md.name not in idx_text:
            missing.append(md.name)
    return missing


def check_ghost_commands() -> list:
    """文档里提到的 `stratum <cmd>` 是否在 main.cc 里有分派"""
    if not MAIN_CC.exists():
        return []
    cc = MAIN_CC.read_text(encoding="utf-8")
    implemented = set(re.findall(r'cmd == "([a-z]+)"', cc))
    # 不是子命令的常见词（出现在 `stratum <这里>` 位置但其实是别的东西）
    noise = {"is", "was", "will", "can", "the", "and", "or", "to", "from",
             "with", "for", "in", "on", "at", "by", "as", "see", "e.g"}
    ghosts = {}
    for md in sorted(DOCS.glob("*.md")) + sorted(DESIGN.glob("*.md")):
        try:
            text = md.read_text(encoding="utf-8")
        except OSError:
            continue
        for m in re.finditer(r"\bstratum ([a-z][a-z0-9]+)\b", text):
            word = m.group(1)
            if word in noise or word in implemented:
                continue
            ghosts.setdefault(word, set()).add(md.name)
    return ghosts


def main() -> int:
    only_links = "--links" in sys.argv
    rc = 0

    print("=== 1) 相对链接 ===")
    links = check_links()
    n_files = len(list(DOCS.rglob("*.md"))) + len(list(DESIGN.rglob("*.md")))
    print("  扫描 %d 个 md 文件" % n_files)
    if links:
        rc = 1
        print("  ✗ %d 个断链：" % len(links))
        for md, ln, label, target in links:
            print("    %s:%d  [%s] -> %s" % (md, ln, label[:26], target[:60]))
    else:
        print("  ✓ 没有断链")

    if only_links:
        return rc

    print()
    print("=== 2) docs/README.md 索引完整性 ===")
    missing = check_indexed()
    if missing:
        rc = 1
        print("  ✗ 以下文件未被 README.md 索引：")
        for name in missing:
            print("    %s" % name)
    else:
        print("  ✓ docs/ 下每篇都被索引")

    print()
    print("=== 3) 文档里的「幽灵命令」 ===")
    ghosts = check_ghost_commands()
    if ghosts:
        # 幽灵命令不一定是错误（可能是设计规划），所以只警告不失败
        print("  ⚠ %d 个词在文档里以 `stratum X` 出现但 main.cc 无分派：" % len(ghosts))
        for word, mds in sorted(ghosts.items()):
            print("    %-14s 出现在: %s" % (word, ", ".join(sorted(mds))[:70]))
        print()
        print("  这些可能是「设计规划」而非错误。若确实是规划，")
        print("  应在文档里明确标注「未实现」，避免读者当成可用命令。")
    else:
        print("  ✓ 没有幽灵命令")

    print()
    print("结果: %s" % ("✓ 通过" if rc == 0 else "✗ 有问题"))
    return rc


if __name__ == "__main__":
    sys.exit(main())
