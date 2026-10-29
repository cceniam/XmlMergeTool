"""基础设施层包（延迟导出以保证多进程导入安全）。"""

from __future__ import annotations

from typing import Any

__all__ = ["RFResultReader", "parse_file_worker", "resource_path"]


def __getattr__(name: str) -> Any:
    if name == "RFResultReader":
        from xml_merge_tool.infrastructure.rf_reader import RFResultReader

        return RFResultReader
    if name == "parse_file_worker":
        from xml_merge_tool.infrastructure.rf_parse_worker import parse_file_worker

        return parse_file_worker
    if name == "resource_path":
        from xml_merge_tool.infrastructure.runtime import resource_path

        return resource_path
    raise AttributeError(name)

