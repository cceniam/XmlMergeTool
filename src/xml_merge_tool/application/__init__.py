"""应用层包（延迟导出以避免子进程启动阶段导入循环）。"""

from __future__ import annotations

from typing import Any

__all__ = [
    "ConflictGroup",
    "LoadBatchResult",
    "LoadIssue",
    "MergeResult",
    "SelectionSuggestion",
    "SuiteCandidate",
    "XmlMergeService",
]


def __getattr__(name: str) -> Any:
    if name == "XmlMergeService":
        from xml_merge_tool.application.merge_service import XmlMergeService

        return XmlMergeService

    if name in {
        "ConflictGroup",
        "LoadBatchResult",
        "LoadIssue",
        "MergeResult",
        "SelectionSuggestion",
        "SuiteCandidate",
    }:
        from xml_merge_tool.application import models as _models

        return getattr(_models, name)

    raise AttributeError(name)

