"""Robot XML parsing helpers with repair fallbacks."""

from __future__ import annotations

import io
from dataclasses import dataclass

from lxml import etree as ET


@dataclass(frozen=True)
class RobotXmlParseFailure:
    stage: str
    error: Exception


def local_name(tag: object) -> str:
    if not isinstance(tag, str):
        return ""
    if "}" in tag:
        return tag.rsplit("}", maxsplit=1)[-1]
    return tag


def extract_robot_payload(file_path: str) -> bytes:
    with open(file_path, "rb") as file:
        content = file.read()

    # Remove obvious NUL bytes that can break XML parsing.
    content = content.replace(b"\x00", b"")

    start = content.find(b"<robot")
    end = content.rfind(b"</robot>")
    if start == -1 or end == -1 or end <= start:
        raise ValueError("无法在文件中识别完整的 <robot> 根节点。")
    return content[start : end + len(b"</robot>")]


def parse_robot_tree(file_path: str) -> ET.ElementTree:
    failures: list[RobotXmlParseFailure] = []

    for stage, recover in (("strict", False), ("recover", True)):
        try:
            tree = ET.parse(file_path, parser=_build_parser(recover=recover))
            _validate_robot_root(tree, stage)
            return tree
        except Exception as exc:
            failures.append(RobotXmlParseFailure(stage=stage, error=exc))

    try:
        repaired = io.BytesIO(extract_robot_payload(file_path))
        tree = ET.parse(repaired, parser=_build_parser(recover=True))
        _validate_robot_root(tree, "sanitize")
        return tree
    except Exception as exc:
        failures.append(RobotXmlParseFailure(stage="sanitize", error=exc))

    raise ValueError(build_repair_failure_message(failures))


def build_repair_failure_message(failures: list[RobotXmlParseFailure]) -> str:
    messages = [f"[{failure.stage}] {failure.error}" for failure in failures]
    return "解析失败，已尝试自动修复: " + " | ".join(messages)


def _build_parser(*, recover: bool) -> ET.XMLParser:
    return ET.XMLParser(recover=recover, huge_tree=True)


def _validate_robot_root(tree: ET.ElementTree, stage: str) -> None:
    root = tree.getroot()
    if root is None:
        raise ValueError(f"{stage} 解析结果缺少根节点。")
    if local_name(root.tag) != "robot":
        raise ValueError(f"{stage} 解析结果根节点不是 <robot>。")
