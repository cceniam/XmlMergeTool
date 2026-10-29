"""XML 合并应用流程中使用的数据模型。"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple

_REBOT_LOG_LEVELS = ("TRACE", "DEBUG", "INFO", "WARN", "NONE")
_REBOT_VISIBLE_LOG_LEVELS = ("TRACE", "DEBUG", "INFO", "WARN")


@dataclass(frozen=True)
class TestCaseResult:
    """勾选套件中可导出的单条 Robot 测试结果。"""

    name: str
    status: str
    failure_reason: str = ""
    duration_seconds: Optional[float] = None


@dataclass(frozen=True)
class SuiteCandidate:
    """从输入的 Robot 输出 XML 中发现的可勾选套件。"""

    candidate_id: str
    file_path: str
    file_name: str
    suite_id: Optional[str]
    suite_name: str
    suite_path: str
    suite_path_parts: Tuple[str, ...]
    status: str
    suite_endtime: Optional[str]
    test_endtime: Optional[str]
    file_mtime: float
    test_names: Tuple[str, ...] = tuple()
    failure_reasons: Tuple[str, ...] = tuple()
    robot_source: Optional[str] = None
    test_results: Tuple[TestCaseResult, ...] = tuple()
    suite_starttime: Optional[str] = None


@dataclass(frozen=True)
class ConflictGroup:
    """同一套件键下候选项的冲突解析结果。"""

    suite_key: str
    winner_id: str
    loser_ids: Tuple[str, ...]
    reason: str
    winner_time_source: str


@dataclass(frozen=True)
class SelectionSuggestion:
    """针对全部候选项的默认自动勾选结果。"""

    selected_ids: Tuple[str, ...]
    conflicts: Tuple[ConflictGroup, ...]


@dataclass(frozen=True)
class MergeResult:
    """勾选套件合并后生成的产物信息。"""

    merged_xml_path: str
    log_path: str
    report_path: str
    conflict_report_path: str
    selected_count: int
    candidate_count: int


@dataclass(frozen=True)
class ExcelExportResult:
    """勾选测试导出为 Excel 后的产物信息。"""

    output_path: str
    selected_suite_count: int
    test_count: int


@dataclass(frozen=True)
class RebotOptions:
    """UI 侧可配置的 Rebot 日志与报告参数。"""

    log_level: str = "TRACE"
    visible_log_level: Optional[str] = None
    split_log: bool = False
    log_title: Optional[str] = None
    report_title: Optional[str] = None
    report_background: Optional[str] = None
    suite_stat_level: Optional[int] = None
    remove_keywords: Optional[str] = None
    flatten_keywords: Optional[str] = None

    def to_rebot_kwargs(self, *, include_report: bool) -> dict[str, object]:
        options: dict[str, object] = {
            "loglevel": self._resolved_log_level(),
        }

        if self.split_log:
            options["splitlog"] = True

        log_title = self._clean_text(self.log_title)
        if log_title:
            options["logtitle"] = log_title

        if include_report:
            report_title = self._clean_text(self.report_title)
            if report_title:
                options["reporttitle"] = report_title

            report_background = self._clean_text(self.report_background)
            if report_background:
                options["reportbackground"] = report_background

            if self.suite_stat_level and self.suite_stat_level > 0:
                options["suitestatlevel"] = int(self.suite_stat_level)

        remove_keywords = self._clean_text(self.remove_keywords)
        if remove_keywords:
            options["removekeywords"] = remove_keywords

        flatten_keywords = self._clean_text(self.flatten_keywords)
        if flatten_keywords:
            options["flattenkeywords"] = flatten_keywords

        return options

    def _resolved_log_level(self) -> str:
        threshold = self._normalize_log_level(
            self.log_level,
            allowed=_REBOT_LOG_LEVELS,
            fallback="TRACE",
        )
        visible = self._normalize_log_level(
            self.visible_log_level,
            allowed=_REBOT_VISIBLE_LOG_LEVELS,
            fallback=None,
        )
        if not visible or threshold == "NONE" or visible == threshold:
            return threshold
        return f"{threshold}:{visible}"

    @staticmethod
    def _clean_text(value: Optional[str]) -> Optional[str]:
        if value is None:
            return None
        cleaned = value.strip()
        return cleaned or None

    @classmethod
    def _normalize_log_level(
        cls,
        value: Optional[str],
        *,
        allowed: Tuple[str, ...],
        fallback: Optional[str],
    ) -> Optional[str]:
        cleaned = cls._clean_text(value)
        if not cleaned:
            return fallback
        normalized = cleaned.upper()
        if normalized in allowed:
            return normalized
        return fallback


@dataclass(frozen=True)
class LoadIssue:
    """候选项加载阶段报告的文件级或批次级问题。"""

    file_path: str
    stage: str
    message: str
    recoverable: bool


@dataclass(frozen=True)
class LoadBatchResult:
    """多文件解析的批量加载结果。"""

    candidates: Tuple[SuiteCandidate, ...]
    issues: Tuple[LoadIssue, ...]
    parsed_file_count: int
    failed_file_count: int
    successful_file_paths: Tuple[str, ...]
