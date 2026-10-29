from __future__ import annotations

import shutil
import sys
import unittest
import uuid
from contextlib import contextmanager
from pathlib import Path

SRC_ROOT = Path(__file__).resolve().parents[1] / "src"
if str(SRC_ROOT) not in sys.path:
    sys.path.insert(0, str(SRC_ROOT))

from xml_merge_tool.infrastructure.rf_parse_worker import parse_file_worker


@contextmanager
def _workspace_temp_dir():
    root = Path(__file__).resolve().parents[1] / "tmp" / f"test-worker-{uuid.uuid4().hex}"
    root.mkdir(parents=True, exist_ok=True)
    try:
        yield root
    finally:
        shutil.rmtree(root, ignore_errors=True)


def _write_nested_robot_output(file_path: Path) -> None:
    content = """<?xml version="1.0" encoding="UTF-8"?>
<robot generator="Robot 6.1.1" generated="20260101 10:00:00.000" rpa="false" schemaversion="4">
  <suite id="s1" name="Root" source="D:\\robot\\Root.robot">
    <test id="s1-t1" name="root-case" line="1">
      <status status="PASS" starttime="20260101 10:00:00.000" endtime="20260101 10:00:01.000">msg</status>
    </test>
    <test id="s1-t2" name="root-skip" line="2">
      <status status="SKIP" starttime="20260101 10:00:01.000" endtime="20260101 10:00:01.500">skip reason</status>
    </test>
    <suite id="s1-s1" name="Child">
      <test id="s1-s1-t1" name="child-case" line="2">
        <status status="FAIL" starttime="20260101 10:00:01.000" endtime="20260101 10:00:02.000">msg</status>
      </test>
      <status status="FAIL" starttime="20260101 10:00:01.000" endtime="20260101 10:00:03.000">suite summary</status>
    </suite>
    <status status="PASS" starttime="20260101 10:00:00.000" endtime="20260101 10:00:04.000"/>
  </suite>
  <statistics><total><stat pass="1" fail="1" skip="0">All Tests</stat></total></statistics>
  <errors/>
</robot>
"""
    file_path.write_text(content, encoding="utf-8")


def _write_prefixed_robot_output(file_path: Path) -> None:
    root = """<?xml version="1.0" encoding="UTF-8"?>
<robot generator="Robot 6.1.1" generated="20260101 10:00:00.000" rpa="false" schemaversion="4">
  <suite id="s1" name="Root">
    <suite id="s1-s1" name="Child">
      <test id="s1-s1-t1" name="child-case" line="2">
        <status status="FAIL" starttime="20260101 10:00:01.000" endtime="20260101 10:00:02.000">msg</status>
      </test>
      <status status="FAIL" starttime="20260101 10:00:01.000" endtime="20260101 10:00:03.000"/>
    </suite>
    <status status="PASS" starttime="20260101 10:00:00.000" endtime="20260101 10:00:04.000"/>
  </suite>
  <statistics><total><stat pass="0" fail="1" skip="0">All Tests</stat></total></statistics>
  <errors/>
</robot>
"""
    file_path.write_bytes(b"junk-prefix\x00\x01<broken>" + root.encode("utf-8") + b"\xff\xfejunk-tail")


class TestRFParseWorker(unittest.TestCase):
    def test_stream_parser_keeps_suite_preorder_and_time_fields(self):
        with _workspace_temp_dir() as root:
            xml_path = root / "nested.xml"
            _write_nested_robot_output(xml_path)

            index, file_path, candidates, issue = parse_file_worker((7, str(xml_path)))
            self.assertEqual(index, 7)
            self.assertEqual(file_path, str(xml_path.resolve()))
            self.assertIsNone(issue)
            self.assertEqual(len(candidates), 2)

            root_candidate = candidates[0]
            child_candidate = candidates[1]

            self.assertEqual(root_candidate["suite_name"], "Root")
            self.assertEqual(root_candidate["suite_path"], "Root")
            self.assertEqual(root_candidate["status"], "PASS")
            self.assertEqual(root_candidate["suite_starttime"], "20260101 10:00:00.000")
            self.assertEqual(root_candidate["suite_endtime"], "20260101 10:00:04.000")
            self.assertEqual(root_candidate["test_endtime"], "20260101 10:00:01.500")
            self.assertEqual(root_candidate["robot_source"], r"D:\robot\Root.robot")
            self.assertEqual(
                root_candidate["test_results"],
                [
                    {
                        "name": "root-case",
                        "status": "PASS",
                        "failure_reason": "",
                        "duration_seconds": 1.0,
                    },
                    {
                        "name": "root-skip",
                        "status": "SKIP",
                        "failure_reason": "",
                        "duration_seconds": 0.5,
                    },
                ],
            )

            self.assertEqual(child_candidate["suite_name"], "Child")
            self.assertEqual(child_candidate["suite_path"], "Root / Child")
            self.assertEqual(child_candidate["status"], "FAIL")
            self.assertEqual(child_candidate["suite_starttime"], "20260101 10:00:01.000")
            self.assertEqual(child_candidate["suite_endtime"], "20260101 10:00:03.000")
            self.assertEqual(child_candidate["test_endtime"], "20260101 10:00:02.000")
            self.assertEqual(child_candidate["robot_source"], r"D:\robot\Root.robot")
            self.assertEqual(
                child_candidate["test_results"],
                [
                    {
                        "name": "child-case",
                        "status": "FAIL",
                        "failure_reason": "msg",
                        "duration_seconds": 1.0,
                    }
                ],
            )
            self.assertEqual(
                child_candidate["failure_reasons"],
                ["child-case：msg", "套件：suite summary"],
            )

            self.assertTrue(str(root_candidate["candidate_id"]).endswith("::s1::1"))
            self.assertTrue(str(child_candidate["candidate_id"]).endswith("::s1-s1::2"))

    def test_duration_handles_whole_seconds_zero_cross_day_and_invalid_times(self):
        with _workspace_temp_dir() as root:
            xml_path = root / "durations.xml"
            xml_path.write_text(
                """<?xml version="1.0" encoding="UTF-8"?>
<robot generator="Robot 6.1.1" schemaversion="4">
  <suite id="s1" name="Durations">
    <test id="s1-t1" name="whole"><status status="PASS" starttime="20260101 10:00:00" endtime="20260101 10:00:02"/></test>
    <test id="s1-t2" name="zero"><status status="SKIP" starttime="20260101 10:00:02.500" endtime="20260101 10:00:02.500"/></test>
    <test id="s1-t3" name="cross-day"><status status="FAIL" starttime="20260101 23:59:59.250" endtime="20260102 00:00:01.500">failed</status></test>
    <test id="s1-t4" name="missing-start"><status status="PASS" endtime="20260102 00:00:02.000"/></test>
    <test id="s1-t5" name="missing-end"><status status="PASS" starttime="20260102 00:00:02.000"/></test>
    <test id="s1-t6" name="invalid"><status status="PASS" starttime="not-a-time" endtime="20260102 00:00:03.000"/></test>
    <test id="s1-t7" name="negative"><status status="PASS" starttime="20260102 00:00:04.000" endtime="20260102 00:00:03.000"/></test>
    <status status="FAIL" endtime="20260102 00:00:04.000"/>
  </suite>
  <statistics/><errors/>
</robot>
""",
                encoding="utf-8",
            )

            _, _, candidates, issue = parse_file_worker((0, str(xml_path)))

            self.assertIsNone(issue)
            self.assertEqual(len(candidates), 1)
            self.assertIsNone(candidates[0]["suite_starttime"])
            durations = {
                result["name"]: result["duration_seconds"]
                for result in candidates[0]["test_results"]
            }
            self.assertEqual(durations["whole"], 2.0)
            self.assertEqual(durations["zero"], 0.0)
            self.assertEqual(durations["cross-day"], 2.25)
            for test_name in ("missing-start", "missing-end", "invalid", "negative"):
                self.assertIsNone(durations[test_name])

    def test_invalid_xml_returns_parse_issue(self):
        with _workspace_temp_dir() as root:
            xml_path = root / "bad.xml"
            xml_path.write_bytes(b"\x00\x01\x02\x03not-an-xml")

            index, file_path, candidates, issue = parse_file_worker((0, str(xml_path)))
            self.assertEqual(index, 0)
            self.assertEqual(file_path, str(xml_path.resolve()))
            self.assertEqual(candidates, [])
            self.assertIsNotNone(issue)
            assert issue is not None
            self.assertEqual(issue["stage"], "parse")
            self.assertTrue(bool(issue["message"]))
            self.assertIn("已尝试自动修复", str(issue["message"]))

    def test_trailing_garbage_is_repaired_and_parsed(self):
        with _workspace_temp_dir() as root:
            xml_path = root / "repairable.xml"
            _write_nested_robot_output(xml_path)
            with xml_path.open("ab") as file:
                file.write(b"\n<broken-tail>")

            index, file_path, candidates, issue = parse_file_worker((2, str(xml_path)))
            self.assertEqual(index, 2)
            self.assertEqual(file_path, str(xml_path.resolve()))
            self.assertIsNone(issue)
            self.assertEqual(len(candidates), 2)

    def test_prefixed_binary_garbage_uses_extracted_robot_payload(self):
        with _workspace_temp_dir() as root:
            xml_path = root / "prefixed.xml"
            _write_prefixed_robot_output(xml_path)

            index, file_path, candidates, issue = parse_file_worker((5, str(xml_path)))
            self.assertEqual(index, 5)
            self.assertEqual(file_path, str(xml_path.resolve()))
            self.assertIsNone(issue)
            self.assertEqual(len(candidates), 1)
            self.assertEqual(candidates[0]["suite_path"], "Root / Child")


if __name__ == "__main__":
    unittest.main()
