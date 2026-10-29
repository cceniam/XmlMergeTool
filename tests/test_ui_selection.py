from __future__ import annotations

import os
import sys
import unittest
from dataclasses import replace
from pathlib import Path

from PyQt6.QtCore import QItemSelection, QItemSelectionModel, QModelIndex, QPoint, QSettings, Qt
from PyQt6.QtGui import QStandardItem, QStandardItemModel
from PyQt6.QtTest import QTest
from PyQt6.QtWidgets import QApplication, QLabel

SRC_ROOT = Path(__file__).resolve().parents[1] / "src"
if str(SRC_ROOT) not in sys.path:
    sys.path.insert(0, str(SRC_ROOT))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from xml_merge_tool.application.models import SuiteCandidate
from xml_merge_tool.ui.main_window import (
    COL_CHECK,
    COL_CONFLICT_SIZE,
    COL_SUITE_PATH,
    COL_TIME,
    _FULL_PATH_ROLE,
    MainWindow,
    CandidateTreeView,
)


class TestCandidateTreeViewSelection(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        settings = QSettings("Robotframework-XMLMergeTool", "MainWindow")
        settings.clear()
        settings.sync()

    def _create_view(self, row_count: int = 120, col_count: int = 3) -> CandidateTreeView:
        view = CandidateTreeView()
        model = QStandardItemModel(row_count, col_count)
        for row in range(row_count):
            for col in range(col_count):
                model.setItem(row, col, QStandardItem(f"r{row}c{col}"))
        view.setModel(model)
        view.resize(420, 220)
        view.show()
        self.app.processEvents()
        return view

    def _create_main_window_with_rows(self, row_count: int = 20) -> MainWindow:
        window = MainWindow()
        window.model.removeRows(0, window.model.rowCount())
        for row in range(row_count):
            checkbox_item = QStandardItem()
            checkbox_item.setEditable(False)
            checkbox_item.setCheckable(True)
            checkbox_item.setCheckState(Qt.CheckState.Unchecked)

            row_items = [
                checkbox_item,
                QStandardItem(f"suite-{row:03d}"),
                QStandardItem("PASS"),
                QStandardItem("否"),
                QStandardItem("1"),
                QStandardItem("2026-01-01 10:00:00"),
                QStandardItem(f"file-{row:03d}.xml"),
            ]
            for item in row_items[1:]:
                item.setEditable(False)
            window.model.appendRow(row_items)

        window.refresh_filter()
        window.show()
        self.app.processEvents()
        return window

    @staticmethod
    def _sample_candidate(candidate_id: str = "candidate-1") -> SuiteCandidate:
        return SuiteCandidate(
            candidate_id=candidate_id,
            file_path=f"D:/tmp/{candidate_id}.xml",
            file_name=f"{candidate_id}.xml",
            suite_id="s1",
            suite_name="SuiteA",
            suite_path="Root / SuiteA",
            suite_path_parts=("Root", "SuiteA"),
            status="PASS",
            suite_endtime="20260101 10:00:01.000",
            test_endtime="20260101 10:00:01.000",
            file_mtime=0.0,
            suite_starttime="20260101 09:59:59.123",
            test_names=("测试登录", "测试退出"),
            failure_reasons=("测试登录：账号或密码错误",),
        )

    def _tree_candidates(self) -> list[SuiteCandidate]:
        return [
            SuiteCandidate(
                candidate_id="candidate-a1",
                file_path="D:/tmp/a1.xml",
                file_name="a1.xml",
                suite_id="s-a1",
                suite_name="SuiteA",
                suite_path="Root / ModuleA / SuiteA",
                suite_path_parts=("Root", "ModuleA", "SuiteA"),
                status="PASS",
                suite_endtime="20260101 10:00:01.000",
                test_endtime="20260101 10:00:01.000",
                file_mtime=0.0,
                suite_starttime="20260101 10:00:00.000",
            ),
            SuiteCandidate(
                candidate_id="candidate-a2",
                file_path="D:/tmp/a2.xml",
                file_name="a2.xml",
                suite_id="s-a2",
                suite_name="SuiteA",
                suite_path="Root / ModuleA / SuiteA",
                suite_path_parts=("Root", "ModuleA", "SuiteA"),
                status="FAIL",
                suite_endtime="20260101 10:00:02.000",
                test_endtime="20260101 10:00:02.000",
                file_mtime=0.0,
                suite_starttime="20260101 10:00:01.000",
            ),
            SuiteCandidate(
                candidate_id="candidate-b1",
                file_path="D:/tmp/b1.xml",
                file_name="b1.xml",
                suite_id="s-b1",
                suite_name="SuiteB",
                suite_path="Root / ModuleB / SuiteB",
                suite_path_parts=("Root", "ModuleB", "SuiteB"),
                status="SKIP",
                suite_endtime="20260101 10:00:03.000",
                test_endtime="20260101 10:00:03.000",
                file_mtime=0.0,
                suite_starttime="20260101 10:00:02.000",
            ),
        ]

    def _switch_to_tree_mode(self, window: MainWindow) -> None:
        window.view_mode_combo.setCurrentText("路径树")
        self.app.processEvents()

    def _switch_to_flat_mode(self, window: MainWindow) -> None:
        window.view_mode_combo.setCurrentText("平铺表格")
        self.app.processEvents()

    def _find_group_index(self, window: MainWindow, full_path: str):
        def walk(parent_index):
            for row in range(window.model.rowCount(parent_index)):
                path_index = window.model.index(row, COL_SUITE_PATH, parent_index)
                if not path_index.isValid():
                    continue
                if str(path_index.data(_FULL_PATH_ROLE) or "") == full_path:
                    return path_index
                found = walk(window.model.index(row, COL_CHECK, parent_index))
                if found is not None:
                    return found
            return None

        return walk(QModelIndex())

    def _leaf_index(self, window: MainWindow, candidate_id: str):
        checkbox_item = window._checkbox_item_by_candidate_id[candidate_id]
        return checkbox_item.index().siblingAtColumn(COL_SUITE_PATH)

    @staticmethod
    def _to_view_index(window: MainWindow, source_index):
        return window._source_index_to_view(source_index)

    def test_rubber_selection_selects_range_when_dragging_not_on_first_column(self):
        view = self._create_view()
        try:
            anchor_pos = QPoint(150, 20)
            current_pos = QPoint(320, 180)
            anchor_row = view._row_at_position(anchor_pos)
            current_row = view._row_at_position(current_pos)

            self.assertIsNotNone(anchor_row)
            self.assertIsNotNone(current_row)

            view._rubber_origin = anchor_pos
            view._rubber_current_pos = current_pos
            view._rubber_modifiers = Qt.KeyboardModifier.NoModifier
            view._rubber_active = True
            view._rubber_anchor_row = anchor_row
            view._apply_rubber_selection(current_pos)
            self.app.processEvents()

            selected_rows = [index.row() for index in view.selectionModel().selectedRows()]
            self.assertGreater(len(selected_rows), 1)
            self.assertEqual(selected_rows[0], min(anchor_row, current_row))
            self.assertEqual(selected_rows[-1], max(anchor_row, current_row))
        finally:
            view.close()

    def test_rubber_selection_keeps_previous_rows_when_scrolling(self):
        view = self._create_view()
        try:
            anchor_pos = QPoint(160, 20)
            current_pos = QPoint(320, 180)
            view._rubber_origin = anchor_pos
            view._rubber_current_pos = current_pos
            view._rubber_modifiers = Qt.KeyboardModifier.NoModifier
            view._rubber_active = True
            view._rubber_anchor_row = view._row_at_position(anchor_pos)

            view._apply_rubber_selection(current_pos)
            self.app.processEvents()
            pre_rows = [index.row() for index in view.selectionModel().selectedRows()]
            self.assertGreater(len(pre_rows), 1)

            bar = view.verticalScrollBar()
            bar.setValue(min(bar.maximum(), bar.value() + 25))
            self.app.processEvents()

            post_rows = [index.row() for index in view.selectionModel().selectedRows()]
            self.assertGreaterEqual(len(post_rows), len(pre_rows))
            self.assertEqual(post_rows[0], pre_rows[0])
        finally:
            view.close()

    def test_context_menu_scope_clicking_unselected_row_uses_only_that_row(self):
        window = self._create_main_window_with_rows()
        try:
            model = window.model
            assert model is not None
            selection_model = window.tree_view.selectionModel()
            assert selection_model is not None

            initial_selection = QItemSelection()
            initial_selection.select(
                window.proxy_model.index(2, 0),
                window.proxy_model.index(4, window.proxy_model.columnCount() - 1),
            )
            selection_model.select(
                initial_selection,
                QItemSelectionModel.SelectionFlag.ClearAndSelect
                | QItemSelectionModel.SelectionFlag.Rows,
            )
            self.app.processEvents()

            target_index = window.proxy_model.index(10, 1)
            pos = window.tree_view.visualRect(target_index).center()
            rows = window._selected_source_rows_for_menu(pos)
            self.assertEqual(rows, [10])
        finally:
            window.close()

    def test_context_menu_scope_clicking_selected_row_keeps_multi_selection(self):
        window = self._create_main_window_with_rows()
        try:
            model = window.model
            assert model is not None
            selection_model = window.tree_view.selectionModel()
            assert selection_model is not None

            selected = QItemSelection()
            selected.select(
                window.proxy_model.index(6, 0),
                window.proxy_model.index(8, window.proxy_model.columnCount() - 1),
            )
            selection_model.select(
                selected,
                QItemSelectionModel.SelectionFlag.ClearAndSelect
                | QItemSelectionModel.SelectionFlag.Rows,
            )
            self.app.processEvents()

            target_index = window.proxy_model.index(7, 1)
            pos = window.tree_view.visualRect(target_index).center()
            rows = window._selected_source_rows_for_menu(pos)
            self.assertEqual(rows, [6, 7, 8])
        finally:
            window.close()

    def test_rubber_selection_then_context_check_and_uncheck_matches_selection_size(self):
        window = self._create_main_window_with_rows(row_count=60)
        try:
            view = window.tree_view

            anchor_pos = QPoint(160, 20)
            current_pos = QPoint(320, 170)
            view._rubber_origin = anchor_pos
            view._rubber_current_pos = current_pos
            view._rubber_modifiers = Qt.KeyboardModifier.NoModifier
            view._rubber_active = True
            view._rubber_anchor_row = view._row_at_position(anchor_pos)
            view._apply_rubber_selection(current_pos)
            self.app.processEvents()

            selection_model = view.selectionModel()
            assert selection_model is not None
            selected_proxy_rows = [index.row() for index in selection_model.selectedRows()]
            self.assertGreater(len(selected_proxy_rows), 1)

            target_proxy_row = selected_proxy_rows[len(selected_proxy_rows) // 2]
            target_index = window.proxy_model.index(target_proxy_row, 1)
            menu_pos = view.visualRect(target_index).center()
            source_rows = window._selected_source_rows_for_menu(menu_pos)
            self.assertEqual(len(source_rows), len(selected_proxy_rows))

            checked_changed = window.set_source_rows_checked(source_rows, True)
            self.assertEqual(checked_changed, len(source_rows))
            for row in source_rows:
                item = window.model.item(row, 0)
                self.assertIsNotNone(item)
                self.assertEqual(item.checkState(), Qt.CheckState.Checked)

            unchecked_changed = window.set_source_rows_checked(source_rows, False)
            self.assertEqual(unchecked_changed, len(source_rows))
            for row in source_rows:
                item = window.model.item(row, 0)
                self.assertIsNotNone(item)
                self.assertEqual(item.checkState(), Qt.CheckState.Unchecked)
        finally:
            window.close()

    def test_main_window_collects_rebot_options_from_ui(self):
        window = MainWindow()
        try:
            window.log_level_combo.setCurrentText("DEBUG")
            window.visible_log_level_combo.setCurrentText("INFO")
            window.split_log_checkbox.setChecked(True)
            window.suite_stat_level_spin.setValue(4)
            window.remove_keywords_combo.setCurrentText("移除通过关键字")
            window.flatten_keywords_combo.setCurrentText("循环迭代")
            window.log_title_edit.setText("合并日志")
            window.report_title_edit.setText("合并报告")
            window.report_background_edit.setText("green:red:yellow")

            options = window.get_rebot_options()

            self.assertEqual(options.log_level, "DEBUG")
            self.assertEqual(options.visible_log_level, "INFO")
            self.assertTrue(options.split_log)
            self.assertEqual(options.suite_stat_level, 4)
            self.assertEqual(options.remove_keywords, "passed")
            self.assertEqual(options.flatten_keywords, "iteration")
            self.assertEqual(options.log_title, "合并日志")
            self.assertEqual(options.report_title, "合并报告")
            self.assertEqual(options.report_background, "green:red:yellow")
        finally:
            window.close()

    def test_detail_dialog_toggle_keeps_filters_and_double_click_behavior(self):
        window = MainWindow()
        try:
            candidate = self._sample_candidate()
            window.populate_candidates([candidate], [candidate.candidate_id])
            window.show()
            self.app.processEvents()

            self.assertFalse(window.detail_dialog.isVisible())

            window._open_candidate_detail(candidate.candidate_id)
            self.app.processEvents()
            self.assertTrue(window.detail_dialog.isVisible())

            window.selected_only_checkbox.setChecked(True)
            self.app.processEvents()
            self.assertEqual(window.get_visible_count(), 1)

            emitted_ids: list[str] = []
            window.candidate_double_clicked.connect(emitted_ids.append)
            window._on_tree_double_clicked(window.proxy_model.index(0, 1))
            self.assertEqual(emitted_ids, [candidate.candidate_id])

            window.log_title_edit.setText("详情打开时读取参数")
            options = window.get_rebot_options()
            self.assertEqual(options.log_title, "详情打开时读取参数")

            window.set_detail_visible(False)
            self.app.processEvents()
            self.assertFalse(window.detail_dialog.isVisible())
        finally:
            window.close()

    def test_moved_toolbar_actions_are_available_from_context_menu(self):
        window = MainWindow()
        try:
            candidate = self._sample_candidate()
            window.populate_candidates([candidate], [candidate.candidate_id])

            self.assertFalse(hasattr(window, "select_all_filtered_button"))
            self.assertFalse(hasattr(window, "invert_filtered_button"))
            self.assertFalse(hasattr(window, "toggle_detail_button"))
            self.assertFalse(hasattr(window, "open_output_dir_button"))

            menu = window._build_tree_context_menu([0], candidate.candidate_id)
            actions = {
                action.text(): action
                for action in menu.actions()
                if not action.isSeparator()
            }
            self.assertEqual(
                list(actions),
                [
                    "勾选",
                    "反勾选",
                    "全选当前过滤",
                    "反选当前过滤",
                    "查看详情",
                    "导出该测试 XML",
                    "打开输出目录",
                ],
            )
            self.assertTrue(all(action.isEnabled() for action in actions.values()))

            blank_menu = window._build_tree_context_menu([], None)
            blank_actions = {
                action.text(): action
                for action in blank_menu.actions()
                if not action.isSeparator()
            }
            for text in ("勾选", "反勾选", "查看详情", "导出该测试 XML"):
                self.assertFalse(blank_actions[text].isEnabled())
            self.assertTrue(blank_actions["全选当前过滤"].isEnabled())
            self.assertTrue(blank_actions["反选当前过滤"].isEnabled())
            self.assertTrue(blank_actions["打开输出目录"].isEnabled())

            requested: list[str] = []
            window.select_all_filtered_requested.connect(
                lambda: requested.append("select-all")
            )
            window.invert_filtered_requested.connect(
                lambda: requested.append("invert")
            )
            window.open_output_dir_requested.connect(
                lambda: requested.append("open-output")
            )
            actions["全选当前过滤"].trigger()
            actions["反选当前过滤"].trigger()
            actions["打开输出目录"].trigger()
            self.assertEqual(requested, ["select-all", "invert", "open-output"])
        finally:
            window.close()

        empty_window = MainWindow()
        try:
            empty_menu = empty_window._build_tree_context_menu([], None)
            empty_actions = {
                action.text(): action
                for action in empty_menu.actions()
                if not action.isSeparator()
            }
            self.assertFalse(empty_actions["全选当前过滤"].isEnabled())
            self.assertFalse(empty_actions["反选当前过滤"].isEnabled())
            self.assertTrue(empty_actions["打开输出目录"].isEnabled())
        finally:
            empty_window.close()

    def test_context_detail_and_export_target_only_clicked_candidate(self):
        window = MainWindow()
        try:
            candidates = self._tree_candidates()
            window.populate_candidates(candidates, [candidate.candidate_id for candidate in candidates])
            window.show()
            self.app.processEvents()

            first_index = window._view_index_for_candidate("candidate-a1")
            second_index = window._view_index_for_candidate("candidate-a2")
            selection = QItemSelection()
            selection.select(first_index, second_index.siblingAtColumn(window.model.columnCount() - 1))
            window.tree_view.selectionModel().select(
                selection,
                QItemSelectionModel.SelectionFlag.ClearAndSelect
                | QItemSelectionModel.SelectionFlag.Rows,
            )

            exported: list[str] = []
            window.candidate_export_requested.connect(exported.append)
            menu = window._build_tree_context_menu(
                ["candidate-a1", "candidate-a2"],
                "candidate-a2",
            )
            actions = {action.text(): action for action in menu.actions()}
            actions["导出该测试 XML"].trigger()
            actions["查看详情"].trigger()
            self.app.processEvents()

            self.assertEqual(exported, ["candidate-a2"])
            self.assertEqual(window.current_candidate_id(), "candidate-a2")
            self.assertTrue(window.detail_dialog.isVisible())
            self.assertEqual(len(window.tree_view.selectionModel().selectedRows()), 2)
        finally:
            window.close()

    def test_detail_panel_omits_conflict_governance_content(self):
        window = MainWindow()
        try:
            candidate = self._sample_candidate()
            window.update_detail_panel(
                candidate,
                is_selected=True,
                is_default_selected=True,
            )

            labels = [label.text() for label in window.detail_panel.findChildren(QLabel)]
            for removed_text in (
                "冲突治理详情",
                "冲突组大小",
                "冲突角色",
                "默认胜出依据",
            ):
                self.assertNotIn(removed_text, labels)
            self.assertFalse(any(text.startswith("提示：") for text in labels))
            self.assertEqual(window.detail_suite_path_value.text(), candidate.suite_path)
            self.assertEqual(window.detail_selected_value.text(), "已勾选")
            self.assertEqual(
                window.detail_test_names_value.toPlainText(),
                "测试登录\n测试退出",
            )
            self.assertEqual(
                window.detail_failure_reasons_value.toPlainText(),
                "测试登录：账号或密码错误",
            )
            self.assertIn("开始时间", labels)
            self.assertNotIn("有效时间", labels)
            self.assertEqual(
                window.detail_start_time_value.text(),
                "2026-01-01 09:59:59",
            )
        finally:
            window.close()

    def test_start_time_column_and_detail_leave_missing_or_invalid_values_blank(self):
        window = MainWindow()
        try:
            candidate = self._sample_candidate()
            window.populate_candidates([candidate], [candidate.candidate_id])
            self.assertEqual(
                window.model.headerData(COL_TIME, Qt.Orientation.Horizontal),
                "开始时间",
            )
            self.assertEqual(
                window.model.item(0, COL_TIME).text(),
                "2026-01-01 09:59:59",
            )

            for starttime in (None, "", "not-a-robot-time"):
                invalid_candidate = replace(candidate, suite_starttime=starttime)
                window.update_detail_panel(invalid_candidate)
                self.assertEqual(window.detail_start_time_value.text(), "—")
                self.assertEqual(MainWindow._format_start_time(invalid_candidate), "—")
        finally:
            window.close()

    def test_output_directory_label_requests_directory_change(self):
        window = MainWindow()
        try:
            requested = []
            window.output_dir_change_requested.connect(lambda: requested.append(True))
            window.show()
            self.app.processEvents()

            QTest.mouseClick(window.output_dir_label, Qt.MouseButton.LeftButton)

            self.assertEqual(requested, [True])
            self.assertEqual(
                window.output_dir_label.cursor().shape(),
                Qt.CursorShape.PointingHandCursor,
            )
        finally:
            window.close()

    def test_main_window_uses_two_section_layout(self):
        window = MainWindow()
        try:
            window.show()
            self.app.processEvents()

            central_layout = window.centralWidget().layout()
            self.assertEqual(central_layout.count(), 2)
            self.assertIs(central_layout.itemAt(0).widget(), window.command_card)
            self.assertIs(central_layout.itemAt(1).widget(), window.workspace_card)
        finally:
            window.close()

    def test_conflict_size_column_is_hidden_by_default_in_both_view_modes(self):
        window = MainWindow()
        try:
            self.assertFalse(window.is_tree_view_mode())
            self.assertTrue(window.tree_view.isColumnHidden(COL_CONFLICT_SIZE))

            self._switch_to_tree_mode(window)

            self.assertTrue(window.is_tree_view_mode())
            self.assertTrue(window.tree_view.isColumnHidden(COL_CONFLICT_SIZE))
        finally:
            window.close()

    def test_conflict_size_visibility_preference_is_preserved_per_view_mode(self):
        window = MainWindow()
        try:
            self.assertTrue(window.tree_view.isColumnHidden(COL_CONFLICT_SIZE))
            window._set_column_visible(COL_CONFLICT_SIZE, True)
            self.assertFalse(window.tree_view.isColumnHidden(COL_CONFLICT_SIZE))

            self._switch_to_tree_mode(window)
            self.assertTrue(window.tree_view.isColumnHidden(COL_CONFLICT_SIZE))
            window._set_column_visible(COL_CONFLICT_SIZE, True)
            self.assertFalse(window.tree_view.isColumnHidden(COL_CONFLICT_SIZE))
        finally:
            window.close()
            self.app.processEvents()

        reopened = MainWindow()
        try:
            self.assertTrue(reopened.is_tree_view_mode())
            self.assertFalse(
                reopened.tree_view.isColumnHidden(COL_CONFLICT_SIZE)
            )

            self._switch_to_flat_mode(reopened)
            self.assertFalse(
                reopened.tree_view.isColumnHidden(COL_CONFLICT_SIZE)
            )
        finally:
            reopened.close()
            self.app.processEvents()

    def test_report_options_dialog_is_hidden_by_default_and_opens_from_button(self):
        window = MainWindow()
        try:
            window.show()
            self.app.processEvents()

            self.assertFalse(window.report_options_dialog.isVisible())

            window.open_report_options_button.click()
            self.app.processEvents()

            self.assertTrue(window.report_options_dialog.isVisible())
            self.assertTrue(window.report_options_content.isVisible())
            self.assertTrue(window.report_options_hint.isVisible())

            window.report_title_edit.setText("弹窗中编辑")
            options = window.get_rebot_options()
            self.assertEqual(options.report_title, "弹窗中编辑")

            window.report_options_dialog.hide()
            self.app.processEvents()
            self.assertFalse(window.report_options_dialog.isVisible())
        finally:
            window.close()

    def test_merge_button_is_pinned_to_header_right_of_tool_group(self):
        window = MainWindow()
        try:
            window.show()
            self.app.processEvents()

            header_group = window.header_action_group
            header_card = window.merge_button.parentWidget()

            self.assertEqual(header_group.objectName(), "HeaderToolGroup")
            self.assertIsNot(header_group, header_card)
            self.assertEqual(header_group.layout().indexOf(window.merge_button), -1)
            self.assertIs(window.export_excel_button.parentWidget(), header_card)
            self.assertLess(
                window.export_excel_button.geometry().left(),
                window.merge_button.geometry().left(),
            )
            self.assertLessEqual(
                window.merge_button.geometry().left()
                - window.export_excel_button.geometry().right(),
                16,
            )
            self.assertGreater(
                window.merge_button.geometry().left(),
                header_group.geometry().right(),
            )
            self.assertLessEqual(
                header_card.contentsRect().right() - window.merge_button.geometry().right(),
                24,
            )
        finally:
            window.close()

    def test_excel_export_button_follows_global_action_enabled_state(self):
        window = MainWindow()
        try:
            self.assertEqual(window.export_excel_button.text(), "导出勾选 Excel")
            self.assertTrue(window.export_excel_button.isEnabled())
            window.set_action_enabled(False)
            self.assertFalse(window.export_excel_button.isEnabled())
            window.set_action_enabled(True)
            self.assertTrue(window.export_excel_button.isEnabled())
        finally:
            window.close()

    def test_footer_filters_share_single_row_with_output_dir(self):
        window = MainWindow()
        try:
            window.show()
            self.app.processEvents()

            footer_group = window.workspace_footer_filter_group

            self.assertEqual(footer_group.objectName(), "FooterFilterGroup")
            for widget in (
                window.conflict_only_checkbox,
                window.selected_only_checkbox,
                window.status_filter_combo,
                window.view_mode_combo,
                window.compact_mode_checkbox,
            ):
                self.assertIs(widget.parentWidget(), footer_group)

            self.assertIs(window.output_dir_label.parentWidget(), footer_group)
            self.assertEqual(window.output_dir_label.toolTip(), window.output_dir)
            self.assertGreater(
                window.output_dir_label.geometry().left(),
                window.compact_mode_checkbox.geometry().right(),
            )
        finally:
            window.close()

    def test_tree_view_mode_builds_path_hierarchy_and_preserves_selection(self):
        window = MainWindow()
        try:
            candidates = self._tree_candidates()
            window.populate_candidates(candidates, ["candidate-a2"])
            window.show()
            self.app.processEvents()
            self.assertEqual(window.get_selected_ids(), ["candidate-a2"])

            self._switch_to_tree_mode(window)

            self.assertTrue(window.is_tree_view_mode())
            self.assertTrue(window.tree_view.rootIsDecorated())
            self.assertEqual(window.get_selected_ids(), ["candidate-a2"])
            self.assertEqual(window.model.rowCount(), 1)

            root_index = self._find_group_index(window, "Root")
            self.assertIsNotNone(root_index)
            module_a_index = self._find_group_index(window, "Root / ModuleA")
            module_b_index = self._find_group_index(window, "Root / ModuleB")
            self.assertIsNotNone(module_a_index)
            self.assertIsNotNone(module_b_index)
            self.assertTrue(
                window.tree_view.isExpanded(
                    self._to_view_index(window, root_index.siblingAtColumn(COL_CHECK))
                )
            )
            candidate_index = window._view_index_for_candidate("candidate-a2")
            self.assertTrue(candidate_index.isValid())
            self.assertEqual(
                candidate_index.siblingAtColumn(COL_TIME).data(),
                "2026-01-01 10:00:01",
            )
        finally:
            window.close()

    def test_tree_view_mode_populate_after_switch_keeps_proxy_rows_visible(self):
        window = MainWindow()
        try:
            self._switch_to_tree_mode(window)
            window.populate_candidates(self._tree_candidates(), ["candidate-a2"])
            window.refresh_filter()
            window.show()
            self.app.processEvents()

            self.assertTrue(window.is_tree_view_mode())
            self.assertEqual(window.model.rowCount(), 1)
            self.assertGreater(window.tree_view.model().rowCount(), 0)
            self.assertEqual(
                set(window.get_filtered_candidate_ids()),
                {"candidate-a1", "candidate-a2", "candidate-b1"},
            )

            root_index = self._find_group_index(window, "Root")
            self.assertIsNotNone(root_index)
            view_root_index = self._to_view_index(
                window, root_index.siblingAtColumn(COL_CHECK)
            )
            self.assertTrue(view_root_index.isValid())
            self.assertEqual(
                str(view_root_index.siblingAtColumn(COL_SUITE_PATH).data()),
                "Root (3)",
            )
        finally:
            window.close()

    def test_repeated_view_mode_switch_keeps_rows_visible(self):
        window = MainWindow()
        try:
            window.populate_candidates(self._tree_candidates(), ["candidate-a2"])
            window.show()
            self.app.processEvents()
            stable_view_model = window.tree_view.model()
            stable_selection_model = window.tree_view.selectionModel()

            self._switch_to_tree_mode(window)
            self.assertIs(window.tree_view.model(), stable_view_model)
            self.assertIs(window.tree_view.selectionModel(), stable_selection_model)
            self.assertGreater(window.tree_view.model().rowCount(), 0)
            self.assertEqual(
                set(window.get_filtered_candidate_ids()),
                {"candidate-a1", "candidate-a2", "candidate-b1"},
            )

            self._switch_to_flat_mode(window)
            self.assertIs(window.tree_view.model(), stable_view_model)
            self.assertIs(window.tree_view.selectionModel(), stable_selection_model)
            self.assertGreater(window.tree_view.model().rowCount(), 0)
            self.assertEqual(
                set(window.get_filtered_candidate_ids()),
                {"candidate-a1", "candidate-a2", "candidate-b1"},
            )

            self._switch_to_tree_mode(window)
            self.assertIs(window.tree_view.model(), stable_view_model)
            self.assertIs(window.tree_view.selectionModel(), stable_selection_model)
            self.assertTrue(window.is_tree_view_mode())
            self.assertGreater(window.tree_view.model().rowCount(), 0)
            self.assertEqual(
                set(window.get_filtered_candidate_ids()),
                {"candidate-a1", "candidate-a2", "candidate-b1"},
            )
        finally:
            window.close()

    def test_tree_parent_checkbox_cascades_children_and_updates_partial_state(self):
        window = MainWindow()
        try:
            window.populate_candidates(self._tree_candidates(), ["candidate-a2"])
            window.show()
            self.app.processEvents()
            self._switch_to_tree_mode(window)

            root_index = self._find_group_index(window, "Root")
            module_a_index = self._find_group_index(window, "Root / ModuleA")
            self.assertIsNotNone(root_index)
            self.assertIsNotNone(module_a_index)

            root_checkbox = window._checkbox_item_from_index(root_index)
            module_a_checkbox = window._checkbox_item_from_index(module_a_index)
            self.assertIsNotNone(root_checkbox)
            self.assertIsNotNone(module_a_checkbox)

            root_checkbox.setCheckState(Qt.CheckState.Checked)
            self.app.processEvents()
            self.assertEqual(
                set(window.get_selected_ids()),
                {"candidate-a1", "candidate-a2", "candidate-b1"},
            )
            module_a_checkbox = window._checkbox_item_from_index(module_a_index)
            self.assertIsNotNone(module_a_checkbox)
            self.assertEqual(module_a_checkbox.checkState(), Qt.CheckState.Checked)

            changed = window.set_source_rows_checked(["candidate-a1"], False)
            self.app.processEvents()
            self.assertEqual(changed, 1)

            root_index = self._find_group_index(window, "Root")
            module_a_index = self._find_group_index(window, "Root / ModuleA")
            self.assertIsNotNone(root_index)
            self.assertIsNotNone(module_a_index)
            root_checkbox = window._checkbox_item_from_index(root_index)
            module_a_checkbox = window._checkbox_item_from_index(module_a_index)
            self.assertIsNotNone(root_checkbox)
            self.assertIsNotNone(module_a_checkbox)
            self.assertEqual(module_a_checkbox.checkState(), Qt.CheckState.PartiallyChecked)
            self.assertEqual(root_checkbox.checkState(), Qt.CheckState.PartiallyChecked)
            self.assertEqual(
                set(window.get_selected_ids()),
                {"candidate-a2", "candidate-b1"},
            )
        finally:
            window.close()

    def test_tree_filters_count_only_leaf_candidates_and_keep_matching_parents(self):
        window = MainWindow()
        try:
            conflict_sizes = {
                "Root / ModuleA / SuiteA": 2,
                "Root / ModuleB / SuiteB": 1,
            }
            window.populate_candidates(self._tree_candidates(), ["candidate-a2"], conflict_group_size_by_suite=conflict_sizes)
            window.show()
            self.app.processEvents()
            self._switch_to_tree_mode(window)

            window.selected_only_checkbox.setChecked(True)
            window.refresh_filter()
            self.app.processEvents()
            self.assertEqual(window.get_visible_count(), 1)

            window.selected_only_checkbox.setChecked(False)
            window.conflict_only_checkbox.setChecked(True)
            window.refresh_filter()
            self.app.processEvents()
            self.assertEqual(set(window.get_filtered_candidate_ids()), {"candidate-a1", "candidate-a2"})

            window.conflict_only_checkbox.setChecked(False)
            window.search_edit.setText("Root")
            window.refresh_filter()
            self.app.processEvents()
            self.assertEqual(
                set(window.get_filtered_candidate_ids()),
                {"candidate-a1", "candidate-a2", "candidate-b1"},
            )
            self.assertIsNotNone(self._find_group_index(window, "Root / ModuleA"))
        finally:
            window.close()

    def test_tree_double_click_ignores_parent_and_header_state_restores_per_mode(self):
        window = MainWindow()
        try:
            window.populate_candidates(self._tree_candidates(), ["candidate-a2"])
            window.show()
            self.app.processEvents()

            window._set_column_visible(6, False)
            self.assertTrue(window.tree_view.isColumnHidden(6))

            self._switch_to_tree_mode(window)
            window._set_column_visible(2, False)
            self.assertTrue(window.tree_view.isColumnHidden(2))

            emitted_ids: list[str] = []
            window.candidate_double_clicked.connect(emitted_ids.append)

            parent_index = self._find_group_index(window, "Root / ModuleA")
            leaf_index = self._to_view_index(
                window, self._leaf_index(window, "candidate-a2")
            )
            assert parent_index is not None

            window._on_tree_double_clicked(self._to_view_index(window, parent_index))
            window._on_tree_double_clicked(leaf_index)
            self.assertEqual(emitted_ids, ["candidate-a2"])
        finally:
            window.close()
            self.app.processEvents()

        reopened = MainWindow()
        try:
            reopened.show()
            self.app.processEvents()
            self.assertEqual(reopened.current_view_mode(), "tree")
            self.assertTrue(reopened.tree_view.isColumnHidden(2))
            self.assertFalse(reopened.tree_view.isColumnHidden(6))

            reopened.view_mode_combo.setCurrentText("平铺表格")
            self.app.processEvents()

            self.assertTrue(reopened.tree_view.isColumnHidden(6))
            self.assertFalse(reopened.tree_view.isColumnHidden(2))
        finally:
            reopened.close()
            self.app.processEvents()


if __name__ == "__main__":
    unittest.main()
