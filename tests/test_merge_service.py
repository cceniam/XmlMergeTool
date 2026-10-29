from __future__ import annotations

import csv
import shutil
import sys
import unittest
import uuid
from contextlib import contextmanager
from pathlib import Path
from unittest import mock

from lxml import etree as ET

SRC_ROOT = Path(__file__).resolve().parents[1] / "src"
if str(SRC_ROOT) not in sys.path:
    sys.path.insert(0, str(SRC_ROOT))

from xml_merge_tool.application.merge_service import XmlMergeService
from xml_merge_tool.application.models import RebotOptions


@contextmanager
def _workspace_temp_dir():
    root = Path(__file__).resolve().parents[1] / "tmp" / f"test-merge-{uuid.uuid4().hex}"
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
    status: str,
    endtime: str,
    source: str,
) -> None:
    starttime = "20260101 10:00:00.000"
    content = f"""<?xml version="1.0" encoding="UTF-8"?>
<robot generator="Robot 6.1.1" generated="20260101 10:00:00.000" rpa="false" schemaversion="4">
  <suite id="s1" name="{root_name}" source="{source}">
    <suite id="s1-s1" name="{leaf_name}" source="{source}">
      <test id="s1-s1-t1" name="case1" line="1">
        <status status="{status}" starttime="{starttime}" endtime="{endtime}">msg</status>
      </test>
      <status status="{status}" starttime="{starttime}" endtime="{endtime}"/>
    </suite>
    <status status="{status}" starttime="{starttime}" endtime="{endtime}"/>
  </suite>
  <statistics><total><stat pass="1" fail="0" skip="0">All Tests</stat></total></statistics>
  <errors/>
</robot>
"""
    file_path.write_text(content, encoding="utf-8")


def _write_prefixed_robot_output(
    file_path: Path,
    *,
    root_name: str,
    leaf_name: str,
    status: str,
    endtime: str,
    source: str,
) -> None:
    starttime = "20260101 10:00:00.000"
    content = f"""<?xml version="1.0" encoding="UTF-8"?>
<robot generator="Robot 6.1.1" generated="20260101 10:00:00.000" rpa="false" schemaversion="4">
  <suite id="s1" name="{root_name}" source="{source}">
    <suite id="s1-s1" name="{leaf_name}" source="{source}">
      <test id="s1-s1-t1" name="case1" line="1">
        <status status="{status}" starttime="{starttime}" endtime="{endtime}">msg</status>
      </test>
      <status status="{status}" starttime="{starttime}" endtime="{endtime}"/>
    </suite>
    <status status="{status}" starttime="{starttime}" endtime="{endtime}"/>
  </suite>
  <statistics><total><stat pass="1" fail="0" skip="0">All Tests</stat></total></statistics>
  <errors/>
</robot>
"""
    file_path.write_bytes(
        b"junk-prefix\x00\x01<broken>"
        + content.encode("utf-8")
        + b"\xff\xfejunk-tail"
    )


class TestMergeService(unittest.TestCase):
    def setUp(self):
        self.service = XmlMergeService()

    def test_handles_basename_source_and_generates_outputs(self):
        with _workspace_temp_dir() as temp_root:
            xml_a = temp_root / "a.xml"
            xml_b = temp_root / "b.xml"

            _write_robot_output(
                xml_a,
                root_name="Root",
                leaf_name="SuiteA",
                status="PASS",
                endtime="20260101 10:00:01.000",
                source="a.robot",
            )
            _write_robot_output(
                xml_b,
                root_name="Root",
                leaf_name="SuiteA",
                status="SKIP",
                endtime="20260101 10:00:02.000",
                source="b.robot",
            )

            load_result = self.service.load_candidates([str(xml_a), str(xml_b)])
            candidates = list(load_result.candidates)
            suggestion = self.service.suggest_selection(candidates)
            self.assertEqual(len(candidates), 2)
            self.assertEqual(load_result.failed_file_count, 0)
            self.assertEqual(len(suggestion.selected_ids), 1)

            selected = {c.candidate_id: c for c in candidates}[suggestion.selected_ids[0]]
            self.assertEqual(selected.status, "SKIP")

            result = self.service.merge_selected(
                candidates, list(suggestion.selected_ids), str(temp_root)
            )
            self.assertTrue(Path(result.merged_xml_path).exists())
            self.assertTrue(Path(result.log_path).exists())
            self.assertTrue(Path(result.report_path).exists())
            self.assertTrue(Path(result.conflict_report_path).exists())

            tree = ET.parse(result.merged_xml_path)
            merged_leafs = tree.xpath(".//suite[@name='SuiteA']")
            self.assertEqual(len(merged_leafs), 1)
            self.assertEqual(
                merged_leafs[0].find("status").get("status"),  # type: ignore[union-attr]
                "SKIP",
            )

            with open(result.conflict_report_path, encoding="utf-8") as file:
                rows = list(csv.DictReader(file))
            self.assertEqual(len(rows), 2)

    def test_allows_manual_override_selection(self):
        with _workspace_temp_dir() as temp_root:
            xml_a = temp_root / "a.xml"
            xml_b = temp_root / "b.xml"

            _write_robot_output(
                xml_a,
                root_name="Root",
                leaf_name="SuiteA",
                status="PASS",
                endtime="20260101 10:00:01.000",
                source="a.robot",
            )
            _write_robot_output(
                xml_b,
                root_name="Root",
                leaf_name="SuiteA",
                status="SKIP",
                endtime="20260101 10:00:02.000",
                source="b.robot",
            )

            load_result = self.service.load_candidates([str(xml_a), str(xml_b)])
            candidates = list(load_result.candidates)
            pass_candidate = next(candidate for candidate in candidates if candidate.status == "PASS")

            result = self.service.merge_selected(
                candidates, [pass_candidate.candidate_id], str(temp_root)
            )
            tree = ET.parse(result.merged_xml_path)
            merged_leaf = tree.xpath(".//suite[@name='SuiteA']")[0]
            self.assertEqual(
                merged_leaf.find("status").get("status"),  # type: ignore[union-attr]
                "PASS",
            )

    def test_generates_single_candidate_log_only(self):
        with _workspace_temp_dir() as temp_root:
            xml_a = temp_root / "a.xml"
            _write_robot_output(
                xml_a,
                root_name="Root",
                leaf_name="SuiteA",
                status="PASS",
                endtime="20260101 10:00:01.000",
                source="a.robot",
            )

            load_result = self.service.load_candidates([str(xml_a)])
            candidate = load_result.candidates[0]
            log_path = self.service.generate_single_candidate_log(candidate, str(temp_root))

            self.assertEqual(log_path, str(temp_root / "single-log.html"))
            self.assertTrue(Path(log_path).exists())
            self.assertFalse(any(temp_root.glob("report*.html")))
            self.assertFalse(any(temp_root.glob(".single-log-input-*.xml")))

    def test_exports_single_candidate_xml_without_modifying_source(self):
        with _workspace_temp_dir() as temp_root:
            xml_a = temp_root / "a.xml"
            _write_robot_output(
                xml_a,
                root_name="Root",
                leaf_name="SuiteA",
                status="PASS",
                endtime="20260101 10:00:01.000",
                source="a.robot",
            )
            source_content = xml_a.read_bytes()
            candidate = self.service.load_candidates([str(xml_a)]).candidates[0]
            export_path = temp_root / "exports" / "SuiteA.xml"

            result = self.service.export_single_candidate_xml(
                candidate,
                str(export_path),
            )

            self.assertEqual(result, str(export_path.resolve()))
            self.assertEqual(xml_a.read_bytes(), source_content)
            tree = ET.parse(result)
            self.assertEqual(tree.getroot().tag, "robot")
            self.assertEqual(
                [node.get("name") for node in tree.xpath("./suite/suite")],
                ["SuiteA"],
            )
            self.assertEqual(tree.xpath("./suite")[0].get("id"), "s1")
            self.assertEqual(tree.xpath("./suite/suite")[0].get("id"), "s1-s1")
            self.assertEqual(tree.xpath("count(.//test)"), 1.0)
            self.assertEqual(len(tree.xpath("./statistics")), 1)
            self.assertEqual(len(tree.xpath("./errors")), 1)

    def test_single_candidate_export_rejects_missing_source_and_source_overwrite(self):
        with _workspace_temp_dir() as temp_root:
            xml_a = temp_root / "a.xml"
            _write_robot_output(
                xml_a,
                root_name="Root",
                leaf_name="SuiteA",
                status="PASS",
                endtime="20260101 10:00:01.000",
                source="a.robot",
            )
            candidate = self.service.load_candidates([str(xml_a)]).candidates[0]

            with self.assertRaisesRegex(ValueError, "不能覆盖源 XML"):
                self.service.export_single_candidate_xml(candidate, str(xml_a))

            xml_a.unlink()
            with self.assertRaisesRegex(ValueError, "源文件不存在"):
                self.service.export_single_candidate_xml(
                    candidate,
                    str(temp_root / "export.xml"),
                )

    def test_cleans_temp_xml_when_single_log_generation_fails(self):
        with _workspace_temp_dir() as temp_root:
            xml_a = temp_root / "a.xml"
            _write_robot_output(
                xml_a,
                root_name="Root",
                leaf_name="SuiteA",
                status="PASS",
                endtime="20260101 10:00:01.000",
                source="a.robot",
            )

            load_result = self.service.load_candidates([str(xml_a)])
            candidate = load_result.candidates[0]

            with mock.patch.object(
                XmlMergeService, "_generate_robot_log", side_effect=ValueError("boom")
            ):
                with self.assertRaises(ValueError):
                    self.service.generate_single_candidate_log(candidate, str(temp_root))

            self.assertFalse(any(temp_root.glob(".single-log-input-*.xml")))

    def test_repaired_input_is_used_for_single_log_and_merge(self):
        with _workspace_temp_dir() as temp_root:
            xml_a = temp_root / "a.xml"
            _write_prefixed_robot_output(
                xml_a,
                root_name="Root",
                leaf_name="SuiteA",
                status="PASS",
                endtime="20260101 10:00:01.000",
                source="a.robot",
            )

            load_result = self.service.load_candidates([str(xml_a)])
            self.assertEqual(load_result.failed_file_count, 0)
            self.assertEqual(len(load_result.candidates), 1)

            candidate = load_result.candidates[0]
            log_path = self.service.generate_single_candidate_log(candidate, str(temp_root))
            self.assertTrue(Path(log_path).exists())

            result = self.service.merge_selected(
                load_result.candidates, [candidate.candidate_id], str(temp_root)
            )
            self.assertTrue(Path(result.merged_xml_path).exists())
            self.assertTrue(Path(result.log_path).exists())
            self.assertTrue(Path(result.report_path).exists())

    def test_rebot_generation_works_without_console_streams(self):
        with _workspace_temp_dir() as temp_root:
            xml_a = temp_root / "a.xml"
            _write_robot_output(
                xml_a,
                root_name="Root",
                leaf_name="SuiteA",
                status="PASS",
                endtime="20260101 10:00:01.000",
                source="a.robot",
            )

            load_result = self.service.load_candidates([str(xml_a)])
            candidate = load_result.candidates[0]

            with mock.patch.object(sys, "stdout", None), mock.patch.object(
                sys, "stderr", None
            ), mock.patch.object(sys, "__stdout__", None), mock.patch.object(
                sys, "__stderr__", None
            ):
                log_path = self.service.generate_single_candidate_log(
                    candidate,
                    str(temp_root),
                )
                result = self.service.merge_selected(
                    load_result.candidates, [candidate.candidate_id], str(temp_root)
                )

            self.assertTrue(Path(log_path).exists())
            self.assertTrue(Path(result.log_path).exists())
            self.assertTrue(Path(result.report_path).exists())

    def test_rebot_options_map_to_report_and_log_kwargs(self):
        options = RebotOptions(
            log_level="debug",
            visible_log_level="info",
            split_log=True,
            log_title="Merged Log",
            report_title="Merged Report",
            report_background="green:red:yellow",
            suite_stat_level=3,
            remove_keywords="passed",
            flatten_keywords="iteration",
        )

        self.assertEqual(
            options.to_rebot_kwargs(include_report=True),
            {
                "loglevel": "DEBUG:INFO",
                "splitlog": True,
                "logtitle": "Merged Log",
                "reporttitle": "Merged Report",
                "reportbackground": "green:red:yellow",
                "suitestatlevel": 3,
                "removekeywords": "passed",
                "flattenkeywords": "iteration",
            },
        )
        self.assertEqual(
            options.to_rebot_kwargs(include_report=False),
            {
                "loglevel": "DEBUG:INFO",
                "splitlog": True,
                "logtitle": "Merged Log",
                "removekeywords": "passed",
                "flattenkeywords": "iteration",
            },
        )

    def test_rebot_options_ignore_invalid_or_empty_values(self):
        options = RebotOptions(
            log_level="invalid",
            visible_log_level="bad",
            log_title="  ",
            report_title="",
            report_background=" ",
            suite_stat_level=0,
            remove_keywords=" ",
            flatten_keywords="",
        )

        self.assertEqual(
            options.to_rebot_kwargs(include_report=True),
            {"loglevel": "TRACE"},
        )


if __name__ == "__main__":
    unittest.main()
