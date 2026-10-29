from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

SRC_ROOT = Path(__file__).resolve().parents[1] / "src"
if str(SRC_ROOT) not in sys.path:
    sys.path.insert(0, str(SRC_ROOT))

from xml_merge_tool.application.models import SuiteCandidate
from xml_merge_tool.application.selection_policy import SuiteSelectionPolicy


def _candidate(
    *,
    candidate_id: str,
    suite_path: str,
    status: str,
    suite_endtime: str,
    suite_starttime: str | None = None,
) -> SuiteCandidate:
    return SuiteCandidate(
        candidate_id=candidate_id,
        file_path=os.path.abspath(f"/tmp/{candidate_id}.xml"),
        file_name=f"{candidate_id}.xml",
        suite_id=f"id-{candidate_id}",
        suite_name=suite_path.split("/")[-1].strip(),
        suite_path=suite_path,
        suite_path_parts=tuple(part.strip() for part in suite_path.split("/") if part.strip()),
        status=status,
        suite_endtime=suite_endtime,
        test_endtime=None,
        file_mtime=0.0,
        suite_starttime=suite_starttime,
    )


class TestSelectionPolicy(unittest.TestCase):
    def setUp(self):
        self.policy = SuiteSelectionPolicy()

    def test_prefers_latest_within_pass_skip_priority_group(self):
        candidates = [
            _candidate(
                candidate_id="pass-old",
                suite_path="Root / Module / CaseA",
                status="PASS",
                suite_endtime="20260101 10:00:00.000",
            ),
            _candidate(
                candidate_id="skip-new",
                suite_path="Root / Module / CaseA",
                status="SKIP",
                suite_endtime="20260101 10:00:01.000",
            ),
            _candidate(
                candidate_id="fail-newest",
                suite_path="Root / Module / CaseA",
                status="FAIL",
                suite_endtime="20260101 10:00:02.000",
            ),
        ]
        suggestion = self.policy.suggest(candidates)
        self.assertEqual(suggestion.selected_ids, ("skip-new",))

    def test_uses_full_suite_path_as_conflict_key(self):
        candidates = [
            _candidate(
                candidate_id="a",
                suite_path="RootA / SameName",
                status="PASS",
                suite_endtime="20260101 10:00:00.000",
            ),
            _candidate(
                candidate_id="b",
                suite_path="RootB / SameName",
                status="PASS",
                suite_endtime="20260101 10:00:01.000",
            ),
        ]
        suggestion = self.policy.suggest(candidates)
        self.assertEqual(set(suggestion.selected_ids), {"a", "b"})
        self.assertEqual(len(suggestion.conflicts), 0)

    def test_display_start_time_does_not_change_endtime_based_winner(self):
        candidates = [
            _candidate(
                candidate_id="earlier-end",
                suite_path="Root / CaseA",
                status="PASS",
                suite_starttime="20260101 12:00:00.000",
                suite_endtime="20260101 12:01:00.000",
            ),
            _candidate(
                candidate_id="later-end",
                suite_path="Root / CaseA",
                status="PASS",
                suite_starttime="20260101 10:00:00.000",
                suite_endtime="20260101 12:02:00.000",
            ),
        ]

        suggestion = self.policy.suggest(candidates)

        self.assertEqual(suggestion.selected_ids, ("later-end",))


if __name__ == "__main__":
    unittest.main()
