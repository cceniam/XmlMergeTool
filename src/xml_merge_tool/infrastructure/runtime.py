"""运行时路径辅助函数（兼容源码、PyInstaller 和 Nuitka）。"""

from __future__ import annotations

import os
import sys
from pathlib import Path


def resource_roots() -> list[Path]:
    """返回资源搜索根目录列表，按优先级排序。"""

    candidates: list[Path] = []

    # Legacy PyInstaller support.
    if hasattr(sys, "_MEIPASS"):
        meipass = Path(str(getattr(sys, "_MEIPASS")))
        candidates.append(meipass / "resources")
        candidates.append(meipass)

    # Nuitka/other frozen executables.
    if getattr(sys, "__compiled__", False) or getattr(sys, "frozen", False):
        exe_dir = Path(sys.executable).resolve().parent
        candidates.append(exe_dir / "resources")
        candidates.append(exe_dir)

    if sys.argv and sys.argv[0]:
        argv_dir = Path(sys.argv[0]).resolve().parent
        candidates.append(argv_dir / "resources")
        candidates.append(argv_dir)

    current = Path(__file__).resolve()
    project_root = current.parents[3]
    candidates.append(project_root / "resources")
    candidates.append(project_root)

    cwd = Path(os.getcwd()).resolve()
    candidates.append(cwd / "resources")
    candidates.append(cwd)

    deduped: list[Path] = []
    seen: set[str] = set()
    for candidate in candidates:
        key = str(candidate).lower()
        if key in seen:
            continue
        seen.add(key)
        deduped.append(candidate)
    return deduped


def resource_path(*parts: str) -> str:
    """在源码模式和打包模式下解析资源路径。"""

    for base in resource_roots():
        target = base.joinpath(*parts)
        if target.exists():
            return str(target)

    roots = resource_roots()
    fallback_base = roots[0] if roots else Path(os.getcwd())
    return str(fallback_base.joinpath(*parts))

