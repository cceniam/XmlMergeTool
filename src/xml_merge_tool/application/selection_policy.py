"""用于选择默认套件候选项的策略。"""

from __future__ import annotations

from collections import defaultdict
from datetime import datetime
from typing import Dict, Iterable, Tuple

from xml_merge_tool.application.models import ConflictGroup, SelectionSuggestion, SuiteCandidate

_ROBOT_TIME_FORMATS = ("%Y%m%d %H:%M:%S.%f", "%Y%m%d %H:%M:%S")
_PRIORITY_STATUSES = {"PASS", "SKIP"}


class SuiteSelectionPolicy:
    """处理候选冲突并给出确定性的默认勾选结果。"""

    def suggest(self, candidates: Iterable[SuiteCandidate]) -> SelectionSuggestion:
        candidate_list = list(candidates)
        grouped: Dict[str, list[SuiteCandidate]] = defaultdict(list)
        for candidate in candidate_list:
            grouped[candidate.suite_path].append(candidate)

        winner_by_key: Dict[str, str] = {}
        conflicts: list[ConflictGroup] = []

        for suite_key, group in grouped.items():
            winner = self._pick_winner(group)
            winner_by_key[suite_key] = winner.candidate_id
            loser_ids = tuple(
                candidate.candidate_id
                for candidate in group
                if candidate.candidate_id != winner.candidate_id
            )
            if loser_ids:
                _, winner_time_source = self.resolve_effective_time(winner)
                conflicts.append(
                    ConflictGroup(
                        suite_key=suite_key,
                        winner_id=winner.candidate_id,
                        loser_ids=loser_ids,
                        reason="status-priority-and-latest-time",
                        winner_time_source=winner_time_source,
                    )
                )

        selected_ids = tuple(
            candidate.candidate_id
            for candidate in candidate_list
            if winner_by_key.get(candidate.suite_path) == candidate.candidate_id
        )
        conflicts.sort(key=lambda item: item.suite_key)
        return SelectionSuggestion(selected_ids=selected_ids, conflicts=tuple(conflicts))

    def resolve_effective_time(self, candidate: SuiteCandidate) -> Tuple[datetime, str]:
        parsed = self._parse_robot_time(candidate.suite_endtime)
        if parsed is not None:
            return parsed, "suite_endtime"

        parsed = self._parse_robot_time(candidate.test_endtime)
        if parsed is not None:
            return parsed, "test_endtime"

        return datetime.fromtimestamp(max(candidate.file_mtime, 0.0)), "file_mtime"

    def _pick_winner(self, group: Iterable[SuiteCandidate]) -> SuiteCandidate:
        entries: list[Tuple[SuiteCandidate, datetime, str]] = []
        for candidate in group:
            effective_time, time_source = self.resolve_effective_time(candidate)
            entries.append((candidate, effective_time, time_source))

        if not entries:
            raise ValueError("Suite conflict group is empty.")

        def sort_key(entry: Tuple[SuiteCandidate, datetime, str]):
            candidate, effective_time, _ = entry
            return (
                self._status_priority(candidate.status),
                -effective_time.timestamp(),
                candidate.file_path,
                candidate.candidate_id,
            )

        winner, _, _ = min(entries, key=sort_key)
        return winner

    @staticmethod
    def _status_priority(status: str) -> int:
        normalized = (status or "").strip().upper()
        return 0 if normalized in _PRIORITY_STATUSES else 1

    @staticmethod
    def _parse_robot_time(value: str | None) -> datetime | None:
        if not value:
            return None
        cleaned = value.strip()
        if not cleaned:
            return None

        for fmt in _ROBOT_TIME_FORMATS:
            try:
                return datetime.strptime(cleaned, fmt)
            except ValueError:
                continue
        return None

