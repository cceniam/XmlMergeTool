from __future__ import annotations

import shutil
import sys
import unittest
import uuid
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

SRC_ROOT = Path(__file__).resolve().parents[1] / "src"
if str(SRC_ROOT) not in sys.path:
    sys.path.insert(0, str(SRC_ROOT))

from xml_merge_tool.infrastructure.rf_reader import RFResultReader


@contextmanager
def _workspace_temp_dir():
    root = Path(__file__).resolve().parents[1] / "tmp" / f"test-reader-{uuid.uuid4().hex}"
    root.mkdir(parents=True, exist_ok=True)
    try:
        yield root
    finally:
        shutil.rmtree(root, ignore_errors=True)


def _write_robot_output(
    file_path: Path,
    *,
    root_name: str,
    leaf_name: str,
    status: str = "PASS",
    test_name: str = "case1",
    message: str = "msg",
) -> None:
    content = f"""<?xml version="1.0" encoding="UTF-8"?>
<robot generator="Robot 6.1.1" generated="20260101 10:00:00.000" rpa="false" schemaversion="4">
  <suite id="s1" name="{root_name}" source="{leaf_name}.robot">
    <suite id="s1-s1" name="{leaf_name}" source="{leaf_name}.robot">
      <test id="s1-s1-t1" name="{test_name}" line="1">
        <status status="{status}" starttime="20260101 10:00:00.000" endtime="20260101 10:00:01.000">{message}</status>
      </test>
      <status status="{status}" starttime="20260101 10:00:00.000" endtime="20260101 10:00:01.000"/>
    </suite>
    <status status="{status}" starttime="20260101 10:00:00.000" endtime="20260101 10:00:01.000"/>
  </suite>
  <statistics><total><stat pass="1" fail="0" skip="0">All Tests</stat></total></statistics>
  <errors/>
</robot>
"""
    file_path.write_text(content, encoding="utf-8")


def _write_empty_robot_output(file_path: Path, *, root_name: str) -> None:
    content = f"""<?xml version="1.0" encoding="UTF-8"?>
<robot generator="Robot 6.1.1" generated="20260101 10:00:00.000" rpa="false" schemaversion="4">
  <suite id="s1" name="{root_name}" source="{root_name}.robot">
    <status status="PASS" starttime="20260101 10:00:00.000" endtime="20260101 10:00:01.000"/>
  </suite>
  <statistics><total><stat pass="0" fail="0" skip="0">All Tests</stat></total></statistics>
  <errors/>
</robot>
"""
    file_path.write_text(content, encoding="utf-8")


class TestRFReaderParallel(unittest.TestCase):
    def setUp(self):
        self.reader = RFResultReader()

    def test_parallel_preserves_input_file_order(self):
        with _workspace_temp_dir() as root:
            xml_b = root / "b.xml"
            xml_a = root / "a.xml"
            _write_robot_output(xml_b, root_name="RootB", leaf_name="SuiteB")
            _write_robot_output(xml_a, root_name="RootA", leaf_name="SuiteA")

            result = self.reader.load_candidates([str(xml_b), str(xml_a)], max_workers=2)
            self.assertEqual(result.failed_file_count, 0)
            self.assertEqual(result.parsed_file_count, 2)
            self.assertEqual(
                [candidate.file_name for candidate in result.candidates],
                ["b.xml", "a.xml"],
            )
            self.assertEqual(
                result.successful_file_paths,
                (str(xml_b.resolve()), str(xml_a.resolve())),
            )
            self.assertEqual(
                [candidate.suite_starttime for candidate in result.candidates],
                ["20260101 10:00:00.000", "20260101 10:00:00.000"],
            )
            self.assertEqual(
                [
                    candidate.test_results[0].duration_seconds
                    for candidate in result.candidates
                ],
                [1.0, 1.0],
            )

    def test_candidate_includes_test_names_and_failure_reasons(self):
        with _workspace_temp_dir() as root:
            xml_file = root / "failed.xml"
            _write_robot_output(
                xml_file,
                root_name="Root",
                leaf_name="Suite",
                status="FAIL",
                test_name="登录失败场景",
                message="期望状态为 PASS，实际为 FAIL",
            )

            result = self.reader.load_candidates([str(xml_file)])

            self.assertEqual(len(result.candidates), 1)
            candidate = result.candidates[0]
            self.assertEqual(candidate.suite_starttime, "20260101 10:00:00.000")
            self.assertEqual(candidate.robot_source, "Suite.robot")
            self.assertEqual(len(candidate.test_results), 1)
            self.assertEqual(candidate.test_results[0].name, "登录失败场景")
            self.assertEqual(candidate.test_results[0].status, "FAIL")
            self.assertEqual(candidate.test_results[0].duration_seconds, 1.0)
            self.assertEqual(
                candidate.test_results[0].failure_reason,
                "期望状态为 PASS，实际为 FAIL",
            )
            self.assertEqual(candidate.test_names, ("登录失败场景",))
            self.assertEqual(
                candidate.failure_reasons,
                ("登录失败场景：期望状态为 PASS，实际为 FAIL",),
            )

    def test_failed_file_isolated_and_skipped(self):
        with _workspace_temp_dir() as root:
            xml_good = root / "good.xml"
            xml_bad = root / "bad.xml"
            _write_robot_output(xml_good, root_name="Root", leaf_name="SuiteOK")
            xml_bad.write_bytes(b"\x00\x01broken-xml")

            result = self.reader.load_candidates([str(xml_good), str(xml_bad)])
            self.assertEqual(result.parsed_file_count, 1)
            self.assertEqual(result.failed_file_count, 1)
            self.assertEqual(len(result.candidates), 1)
            self.assertEqual(result.candidates[0].suite_name, "SuiteOK")
            self.assertEqual(result.successful_file_paths, (str(xml_good.resolve()),))
            self.assertGreaterEqual(len(result.issues), 1)
            self.assertTrue(any(issue.stage == "parse" for issue in result.issues))

    def test_repairable_file_is_auto_fixed(self):
        with _workspace_temp_dir() as root:
            xml_good = root / "good.xml"
            xml_repairable = root / "repairable.xml"
            _write_robot_output(xml_good, root_name="RootA", leaf_name="SuiteA")
            _write_robot_output(xml_repairable, root_name="RootB", leaf_name="SuiteB")
            with xml_repairable.open("ab") as file:
                file.write(b"\n<extra-tail>")

            result = self.reader.load_candidates([str(xml_good), str(xml_repairable)])
            self.assertEqual(result.parsed_file_count, 2)
            self.assertEqual(result.failed_file_count, 0)
            self.assertEqual(len(result.candidates), 2)
            self.assertEqual(
                result.successful_file_paths,
                (str(xml_good.resolve()), str(xml_repairable.resolve())),
            )

    def test_parallel_failure_falls_back_to_serial(self):
        with _workspace_temp_dir() as root:
            xml_a = root / "a.xml"
            xml_b = root / "b.xml"
            _write_robot_output(xml_a, root_name="RootA", leaf_name="SuiteA")
            _write_robot_output(xml_b, root_name="RootB", leaf_name="SuiteB")

            with patch(
                "xml_merge_tool.infrastructure.rf_reader.ProcessPoolExecutor",
                side_effect=RuntimeError("pool-init-failed"),
            ) as mock_pool:
                # Exercise pool failure even on CI runners where the default
                # CPU-based worker count selects serial parsing.
                result = self.reader.load_candidates([str(xml_a), str(xml_b)], max_workers=2)

            mock_pool.assert_called_once()
            self.assertEqual(mock_pool.call_args.kwargs["max_workers"], 2)

            self.assertEqual(result.parsed_file_count, 2)
            self.assertEqual(result.failed_file_count, 0)
            self.assertEqual(len(result.candidates), 2)
            self.assertEqual(
                result.successful_file_paths,
                (str(xml_a.resolve()), str(xml_b.resolve())),
            )
            self.assertTrue(any(issue.stage == "parallel" for issue in result.issues))

    def test_low_cpu_count_uses_serial_without_parallel_issue(self):
        with _workspace_temp_dir() as root:
            xml_a = root / "a.xml"
            xml_b = root / "b.xml"
            _write_robot_output(xml_a, root_name="RootA", leaf_name="SuiteA")
            _write_robot_output(xml_b, root_name="RootB", leaf_name="SuiteB")

            for cpu_count in (1, 2, None):
                with self.subTest(cpu_count=cpu_count), patch(
                    "xml_merge_tool.infrastructure.rf_reader.os.cpu_count",
                    return_value=cpu_count,
                ), patch(
                    "xml_merge_tool.infrastructure.rf_reader.ProcessPoolExecutor"
                ) as mock_pool:
                    result = self.reader.load_candidates([str(xml_a), str(xml_b)])

                mock_pool.assert_not_called()
                self.assertEqual(result.parsed_file_count, 2)
                self.assertEqual(result.failed_file_count, 0)
                self.assertEqual(len(result.candidates), 2)
                self.assertEqual(
                    result.successful_file_paths,
                    (str(xml_a.resolve()), str(xml_b.resolve())),
                )
                self.assertFalse(any(issue.stage == "parallel" for issue in result.issues))

    def test_single_file_does_not_use_process_pool(self):
        with _workspace_temp_dir() as root:
            xml_a = root / "a.xml"
            _write_robot_output(xml_a, root_name="RootA", leaf_name="SuiteA")

            with patch("xml_merge_tool.infrastructure.rf_reader.ProcessPoolExecutor") as mock_pool:
                result = self.reader.load_candidates([str(xml_a)])

            self.assertEqual(result.parsed_file_count, 1)
            self.assertEqual(result.failed_file_count, 0)
            self.assertEqual(result.successful_file_paths, (str(xml_a.resolve()),))
            mock_pool.assert_not_called()

    def test_successful_file_paths_include_valid_xml_without_candidates(self):
        with _workspace_temp_dir() as root:
            xml_empty = root / "empty.xml"
            xml_suite = root / "suite.xml"
            _write_empty_robot_output(xml_empty, root_name="EmptyRoot")
            _write_robot_output(xml_suite, root_name="RootA", leaf_name="SuiteA")

            result = self.reader.load_candidates([str(xml_empty), str(xml_suite)])

            self.assertEqual(result.parsed_file_count, 2)
            self.assertEqual(result.failed_file_count, 0)
            self.assertEqual(len(result.candidates), 1)
            self.assertEqual(
                result.successful_file_paths,
                (str(xml_empty.resolve()), str(xml_suite.resolve())),
            )


if __name__ == "__main__":
    unittest.main()
