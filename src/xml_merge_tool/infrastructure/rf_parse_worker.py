"""用于解析 Robot 输出 XML 的顶层多进程工作函数。"""

from __future__ import annotations

import io
import os
from dataclasses import dataclass, field
from datetime import datetime
from itertools import count
from pathlib import Path
from typing import Dict, List, Tuple

from lxml import etree as ET

from xml_merge_tool.infrastructure.robot_xml import (
    RobotXmlParseFailure,
    build_repair_failure_message,
    extract_robot_payload,
    local_name,
)

WorkerResult = Tuple[int, str, List[Dict[str, object]], Dict[str, object] | None]
_ROBOT_TIME_FORMATS = ("%Y%m%d %H:%M:%S.%f", "%Y%m%d %H:%M:%S")


@dataclass(slots=True)
class _TestFrame:
    name: str
    status: str = "UNKNOWN"
    failure_reason: str = ""
    starttime: str | None = None
    endtime: str | None = None


@dataclass(slots=True)
class _SuiteFrame:
    suite_id: str | None
    suite_name: str
    path_parts: Tuple[str, ...]
    preorder_index: int
    robot_source: str | None = None
    status: str = "UNKNOWN"
    suite_starttime: str | None = None
    suite_endtime: str | None = None
    latest_test_endtime: str | None = None
    direct_test_count: int = 0
    active_test: _TestFrame | None = None
    test_results: list[dict[str, object]] = field(default_factory=list)
    test_names: list[str] = field(default_factory=list)
    failure_reasons: list[str] = field(default_factory=list)


def parse_file_worker(task: Tuple[int, str]) -> WorkerResult:
    """解析单个 XML 文件并返回序列化后的候选记录。"""

    index, file_path = task
    abs_path = os.path.abspath(file_path)
    file_mtime = os.path.getmtime(abs_path)
    file_name = Path(abs_path).name
    strict_error: Exception | None = None
    recover_error: Exception | None = None
    sanitized_error: Exception | None = None

    try:
        candidates = _parse_candidates_streaming(
            source=abs_path,
            file_path=abs_path,
            file_name=file_name,
            file_mtime=file_mtime,
            recover=False,
        )
        return index, abs_path, candidates, None
    except Exception as exc:
        strict_error = exc

    try:
        candidates = _parse_candidates_streaming(
            source=abs_path,
            file_path=abs_path,
            file_name=file_name,
            file_mtime=file_mtime,
            recover=True,
        )
        return index, abs_path, candidates, None
    except Exception as exc:
        recover_error = exc

    try:
        repaired_xml = extract_robot_payload(abs_path)
        candidates = _parse_candidates_streaming(
            source=io.BytesIO(repaired_xml),
            file_path=abs_path,
            file_name=file_name,
            file_mtime=file_mtime,
            recover=True,
        )
        return index, abs_path, candidates, None
    except Exception as exc:
        sanitized_error = exc

    issue = {
        "file_path": abs_path,
        "stage": "parse",
        "message": build_repair_failure_message(
            [
                RobotXmlParseFailure(stage="strict", error=strict_error),
                RobotXmlParseFailure(stage="recover", error=recover_error),
                RobotXmlParseFailure(stage="sanitize", error=sanitized_error),
            ]
        ),
        "recoverable": True,
    }
    return index, abs_path, [], issue


def _parse_candidates_streaming(
    source,
    file_path: str,
    file_name: str,
    file_mtime: float,
    *,
    recover: bool,
) -> List[Dict[str, object]]:
    context = ET.iterparse(
        source,
        events=("start", "end"),
        huge_tree=True,
        recover=recover,
    )
    suite_stack: list[_SuiteFrame] = []
    discovered: list[_SuiteFrame] = []
    preorder_counter = count(1)
    root_tag: str | None = None

    for event, elem in context:
        tag = local_name(elem.tag)
        if event == "start":
            if root_tag is None:
                root_tag = tag
                if root_tag != "robot":
                    raise ValueError(f"解析结果根节点不是 <robot>，实际为 <{root_tag}>。")
            if tag == "suite":
                parent_parts = suite_stack[-1].path_parts if suite_stack else tuple()
                parent_source = suite_stack[-1].robot_source if suite_stack else None
                suite_name = _normalize_suite_name(elem.get("name"))
                suite_stack.append(
                    _SuiteFrame(
                        suite_id=elem.get("id"),
                        suite_name=suite_name,
                        path_parts=parent_parts + (suite_name,),
                        preorder_index=next(preorder_counter),
                        robot_source=(
                            _normalize_optional_text(elem.get("source"))
                            or parent_source
                        ),
                    )
                )
            elif tag == "test" and suite_stack:
                suite_stack[-1].active_test = _TestFrame(
                    name=_normalize_test_name(elem.get("name"))
                )
            continue

        if tag == "status":
            _capture_status(elem, suite_stack)
        elif tag == "test":
            _capture_test(suite_stack)
        elif tag == "suite":
            _capture_suite(discovered, suite_stack)

        _release_element(elem)

    del context

    if root_tag is None:
        raise ValueError("解析结果缺少 <robot> 根节点。")

    discovered.sort(key=lambda frame: frame.preorder_index)
    serial_counter = count(1)
    candidates: list[dict[str, object]] = []
    for frame in discovered:
        candidate_id = _build_candidate_id(
            file_path=file_path,
            suite_id=frame.suite_id,
            suite_path=frame.path_parts,
            serial=next(serial_counter),
        )
        candidates.append(
            {
                "candidate_id": candidate_id,
                "file_path": file_path,
                "file_name": file_name,
                "suite_id": frame.suite_id,
                "suite_name": frame.suite_name,
                "suite_path": " / ".join(frame.path_parts),
                "suite_path_parts": list(frame.path_parts),
                "status": frame.status,
                "suite_starttime": frame.suite_starttime,
                "suite_endtime": frame.suite_endtime,
                "test_endtime": frame.latest_test_endtime,
                "file_mtime": float(file_mtime),
                "robot_source": frame.robot_source,
                "test_results": list(frame.test_results),
                "test_names": list(frame.test_names),
                "failure_reasons": list(frame.failure_reasons),
            }
        )
    return candidates


def _capture_status(elem: ET._Element, suite_stack: list[_SuiteFrame]) -> None:
    if not suite_stack:
        return
    parent = elem.getparent()
    if parent is None:
        return
    parent_tag = local_name(parent.tag)
    frame = suite_stack[-1]
    if parent_tag == "suite":
        frame.status = _normalize_status(elem.get("status"))
        frame.suite_starttime = _normalize_optional_text(elem.get("starttime"))
        frame.suite_endtime = _normalize_optional_text(elem.get("endtime"))
        message = _normalize_optional_text(elem.text)
        if frame.status == "FAIL" and message:
            frame.failure_reasons.append(f"套件：{message}")
        return

    if parent_tag != "test":
        return
    active_test = frame.active_test
    if active_test is None:
        active_test = _TestFrame(name=_normalize_test_name(parent.get("name")))
        frame.active_test = active_test

    active_test.status = _normalize_status(elem.get("status"))
    active_test.starttime = _normalize_optional_text(elem.get("starttime"))
    active_test.endtime = _normalize_optional_text(elem.get("endtime"))
    if active_test.status == "FAIL":
        active_test.failure_reason = (
            _normalize_optional_text(elem.text) or "未提供失败消息"
        )


def _capture_test(suite_stack: list[_SuiteFrame]) -> None:
    if not suite_stack:
        return
    frame = suite_stack[-1]
    frame.direct_test_count += 1
    test = frame.active_test or _TestFrame(name="<unnamed-test>")
    frame.test_results.append(
        {
            "name": test.name,
            "status": test.status,
            "failure_reason": test.failure_reason,
            "duration_seconds": _calculate_duration_seconds(
                test.starttime, test.endtime
            ),
        }
    )
    frame.test_names.append(test.name)
    if test.status == "FAIL":
        frame.failure_reasons.append(f"{test.name}：{test.failure_reason}")
    if test.endtime and (
        frame.latest_test_endtime is None or test.endtime > frame.latest_test_endtime
    ):
        frame.latest_test_endtime = test.endtime
    frame.active_test = None


def _capture_suite(
    discovered: list[_SuiteFrame], suite_stack: list[_SuiteFrame]
) -> None:
    if not suite_stack:
        return
    frame = suite_stack.pop()
    if frame.direct_test_count > 0:
        discovered.append(frame)


def _normalize_status(status: str | None) -> str:
    normalized = (status or "").strip().upper()
    if normalized in {"PASS", "FAIL", "SKIP"}:
        return normalized
    return "UNKNOWN"


def _normalize_suite_name(name: str | None) -> str:
    normalized = (name or "").strip()
    return normalized or "<unnamed-suite>"


def _normalize_test_name(name: str | None) -> str:
    normalized = (name or "").strip()
    return normalized or "<unnamed-test>"


def _normalize_optional_text(value: str | None) -> str | None:
    normalized = (value or "").strip()
    return normalized or None


def _calculate_duration_seconds(
    starttime: str | None, endtime: str | None
) -> float | None:
    start = _parse_robot_time(starttime)
    end = _parse_robot_time(endtime)
    if start is None or end is None or end < start:
        return None
    return (end - start).total_seconds()


def _parse_robot_time(value: str | None) -> datetime | None:
    if not value:
        return None
    cleaned = value.strip()
    if not cleaned:
        return None
    for time_format in _ROBOT_TIME_FORMATS:
        try:
            return datetime.strptime(cleaned, time_format)
        except ValueError:
            continue
    return None


def _release_element(elem: ET._Element) -> None:
    elem.clear()
    parent = elem.getparent()
    if parent is None:
        return
    while elem.getprevious() is not None:
        del parent[0]


def _build_candidate_id(
    file_path: str, suite_id: str | None, suite_path: Tuple[str, ...], serial: int
) -> str:
    path_key = ".".join(suite_path)
    suite_key = suite_id or path_key
    return f"{file_path}::{suite_key}::{serial}"
