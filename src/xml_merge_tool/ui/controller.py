"""将 PyQt 界面控件与应用服务连接的控制器。"""

from __future__ import annotations

import os
import re
from collections import Counter
from datetime import datetime
from typing import Iterable, Sequence

from PyQt6.QtCore import QObject, QTimer, QUrl
from PyQt6.QtGui import QDesktopServices, QStandardItem
from PyQt6.QtWidgets import QFileDialog, QMessageBox, QPushButton

from xml_merge_tool.application.merge_service import XmlMergeService
from xml_merge_tool.application.models import (
    ConflictGroup,
    ExcelExportResult,
    LoadIssue,
    MergeResult,
    SelectionSuggestion,
    SuiteCandidate,
)
from xml_merge_tool.ui.main_window import MainWindow
from xml_merge_tool.ui.thread_runner import WorkerThread


class MainController(QObject):
    """负责从图形界面协调加载、默认勾选与合并流程。"""

    def __init__(self, service: XmlMergeService | None = None):
        super().__init__()
        self.window = MainWindow()
        self._service = service or XmlMergeService()
        self._worker: WorkerThread | None = None
        self._active_file_paths: list[str] = []
        self._candidates: list[SuiteCandidate] = []
        self._candidate_by_id: dict[str, SuiteCandidate] = {}
        self._group_size_by_suite_key: dict[str, int] = {}
        self._conflict_by_suite_key: dict[str, ConflictGroup] = {}
        self._default_selected_ids: set[str] = set()
        self._suggestion: SelectionSuggestion = SelectionSuggestion(
            selected_ids=tuple(), conflicts=tuple()
        )
        self._load_issues: tuple[LoadIssue, ...] = tuple()
        self._parsed_file_count = 0
        self._failed_file_count = 0
        self._connect_signals()

    def _connect_signals(self):
        self.window.open_button.clicked.connect(self.open_files_dialog)
        self.window.select_folder_button.clicked.connect(self.select_output_dir)
        self.window.output_dir_change_requested.connect(self.select_output_dir)
        self.window.export_excel_button.clicked.connect(self.export_selected_tests_excel)
        self.window.merge_button.clicked.connect(self.merge_selected)
        self.window.clear_button.clicked.connect(self.clear_state)
        self.window.restore_default_button.clicked.connect(self.restore_default_selection)
        self.window.select_all_filtered_requested.connect(self.select_all_filtered)
        self.window.invert_filtered_requested.connect(self.invert_filtered_selection)
        self.window.open_output_dir_requested.connect(self.open_output_dir)
        self.window.candidate_export_requested.connect(self.export_single_candidate_xml)
        self.window.tree_view.files_dropped.connect(self.load_files)
        self.window.model.itemChanged.connect(self._on_item_changed)
        self.window.selection_bulk_changed.connect(self._on_selection_bulk_changed)
        self.window.candidate_double_clicked.connect(self.open_single_candidate_log)
        self.window.focused_candidate_changed.connect(self._on_focused_candidate_changed)
        self.window.conflict_only_checkbox.toggled.connect(self._on_filters_changed)
        self.window.selected_only_checkbox.toggled.connect(self._on_filters_changed)
        self.window.status_filter_combo.currentIndexChanged.connect(self._on_filters_changed)
        self.window.search_edit.textChanged.connect(self._on_filters_changed)

    def open_files_dialog(self):
        files, _ = QFileDialog.getOpenFileNames(
            self.window, "Open XML Files", "", "XML Files (*.xml);;All Files (*)"
        )
        if files:
            self.load_files(files)

    def select_output_dir(self):
        folder = QFileDialog.getExistingDirectory(
            self.window,
            "选择输出目录",
            self.window.output_dir,
        )
        if folder:
            self.window.set_output_dir(folder)
            self._refresh_summary()

    def open_output_dir(self):
        if not os.path.isdir(self.window.output_dir):
            self._show_error("当前输出目录不存在，请先选择有效目录。")
            return
        QDesktopServices.openUrl(QUrl.fromLocalFile(self.window.output_dir))

    def load_files(self, file_paths: Sequence[str]):
        normalized = self._normalize_input_paths(file_paths)
        if not normalized:
            self._show_error("未检测到可用的 XML 文件。")
            return

        merged_paths = self._merge_active_file_paths(normalized)

        self._run_worker(
            busy_message="读取 XML 并生成默认勾选中...",
            fn=self._load_job,
            args=(merged_paths,),
            on_success=self._on_loaded,
        )

    def clear_state(self):
        self._active_file_paths = []
        self._candidates = []
        self._candidate_by_id = {}
        self._group_size_by_suite_key = {}
        self._conflict_by_suite_key = {}
        self._default_selected_ids = set()
        self._suggestion = SelectionSuggestion(selected_ids=tuple(), conflicts=tuple())
        self._load_issues = tuple()
        self._parsed_file_count = 0
        self._failed_file_count = 0
        self.window.clear_candidates()

    def restore_default_selection(self):
        if not self._candidates:
            self._show_error("当前没有可恢复的数据，请先加载 XML。")
            return
        self.window.apply_selection(self._suggestion.selected_ids)
        self._refresh_summary()
        self._refresh_detail_panel()

    def select_all_filtered(self):
        if not self._candidates:
            self._show_error("当前没有可操作的数据，请先加载 XML。")
            return
        self.window.set_filtered_selection(True)
        self._refresh_summary()
        self._refresh_detail_panel()

    def invert_filtered_selection(self):
        if not self._candidates:
            self._show_error("当前没有可操作的数据，请先加载 XML。")
            return
        self.window.invert_filtered_selection()
        self._refresh_summary()
        self._refresh_detail_panel()

    def merge_selected(self):
        if not self._candidates:
            self._show_error("请先加载 XML 文件。")
            return

        selected_ids = self.window.get_selected_ids()
        if not selected_ids:
            self._show_error("请至少勾选一个测试套件。")
            return

        output_dir = self.window.output_dir
        rebot_options = self.window.get_rebot_options()

        self._run_worker(
            busy_message="合并勾选套件并生成报告中...",
            fn=self._merge_job,
            args=(selected_ids, output_dir, rebot_options),
            on_success=self._on_merged,
        )

    def export_selected_tests_excel(self):
        if not self._candidates:
            self._show_error("请先加载 XML 文件。")
            return

        selected_ids = self.window.get_selected_ids()
        if not selected_ids:
            self._show_error("请至少勾选一个测试套件。")
            return

        default_dir = (
            self.window.output_dir
            if os.path.isdir(self.window.output_dir)
            else os.getcwd()
        )
        timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        default_path = os.path.join(default_dir, f"selected-tests-{timestamp}.xlsx")
        output_path, _ = QFileDialog.getSaveFileName(
            self.window,
            "导出勾选测试 Excel",
            default_path,
            "Excel Files (*.xlsx)",
        )
        if not output_path:
            return
        output_path = self._normalize_xlsx_path(output_path)

        self._run_worker(
            busy_message="导出勾选测试 Excel 中...",
            fn=self._selected_tests_excel_export_job,
            args=(selected_ids, output_path),
            on_success=self._on_selected_tests_excel_exported,
        )

    def _load_job(self, file_paths: Sequence[str]):
        load_result = self._service.load_candidates(file_paths)
        suggestion = self._service.suggest_selection(load_result.candidates)
        return load_result, suggestion

    def _merge_job(
        self,
        selected_ids: Sequence[str],
        output_dir: str,
        rebot_options,
    ) -> MergeResult:
        return self._service.merge_selected(
            self._candidates,
            selected_ids,
            output_dir,
            rebot_options,
        )

    def _selected_tests_excel_export_job(
        self,
        selected_ids: Sequence[str],
        output_path: str,
    ) -> ExcelExportResult:
        return self._service.export_selected_tests_excel(
            self._candidates,
            selected_ids,
            output_path,
        )

    def _on_selected_tests_excel_exported(self, result: ExcelExportResult):
        QMessageBox.information(
            self.window,
            "导出完成",
            (
                f"已导出 {result.test_count} 条测试，"
                f"来自 {result.selected_suite_count} 个勾选套件：\n{result.output_path}"
            ),
        )

    def _single_candidate_log_job(
        self,
        candidate: SuiteCandidate,
        output_dir: str,
        rebot_options,
    ) -> str:
        return self._service.generate_single_candidate_log(
            candidate,
            output_dir,
            rebot_options,
        )

    def _single_candidate_xml_export_job(
        self,
        candidate: SuiteCandidate,
        output_path: str,
    ) -> str:
        return self._service.export_single_candidate_xml(candidate, output_path)

    def _on_loaded(self, payload):
        load_result, suggestion = payload
        self._active_file_paths = list(load_result.successful_file_paths)
        self._candidates = list(load_result.candidates)
        self._candidate_by_id = {
            candidate.candidate_id: candidate for candidate in self._candidates
        }
        self._suggestion = suggestion
        self._default_selected_ids = set(suggestion.selected_ids)
        self._load_issues = load_result.issues
        self._parsed_file_count = load_result.parsed_file_count
        self._failed_file_count = load_result.failed_file_count

        self._group_size_by_suite_key = dict(
            Counter(candidate.suite_path for candidate in self._candidates)
        )
        self._conflict_by_suite_key = {
            conflict.suite_key: conflict for conflict in suggestion.conflicts
        }

        self.window.populate_candidates(
            self._candidates,
            suggestion.selected_ids,
            default_selected_ids=suggestion.selected_ids,
            conflict_group_size_by_suite=self._group_size_by_suite_key,
        )
        self._apply_filters()
        self._show_load_issues_if_needed()
        self._refresh_summary()
        self._refresh_detail_panel()

    def _on_merged(self, result: MergeResult):
        self._refresh_summary()

        dialog = QMessageBox(self.window)
        dialog.setWindowTitle("合并完成")
        dialog.setIcon(QMessageBox.Icon.Information)
        dialog.setText(
            f"勾选整合完成：已选择 {result.selected_count}/{result.candidate_count} 个套件。"
        )
        dialog.setInformativeText(
            "\n".join(
                [
                    f"Merged XML: {result.merged_xml_path}",
                    f"Log: {result.log_path}",
                    f"Report: {result.report_path}",
                    f"Conflict CSV: {result.conflict_report_path}",
                ]
            )
        )

        open_output_btn = dialog.addButton("打开输出目录", QMessageBox.ButtonRole.ActionRole)
        open_log_btn = dialog.addButton("打开 Log", QMessageBox.ButtonRole.ActionRole)
        open_report_btn = dialog.addButton("打开 Report", QMessageBox.ButtonRole.ActionRole)
        open_conflict_btn = dialog.addButton(
            "打开冲突 CSV", QMessageBox.ButtonRole.ActionRole
        )
        dialog.addButton(QMessageBox.StandardButton.Close)
        dialog.exec()

        clicked: QPushButton | None = dialog.clickedButton()  # type: ignore[assignment]
        if clicked == open_output_btn:
            self.open_output_dir()
        elif clicked == open_log_btn:
            self._open_path(result.log_path)
        elif clicked == open_report_btn:
            self._open_path(result.report_path)
        elif clicked == open_conflict_btn:
            self._open_path(result.conflict_report_path)

    def _open_path(self, path: str):
        if not os.path.exists(path):
            self._show_error(f"文件不存在: {path}")
            return
        QDesktopServices.openUrl(QUrl.fromLocalFile(path))

    def _on_item_changed(self, item: QStandardItem):
        if item.column() == 0:
            if self.window.selected_only_checkbox.isChecked():
                if self.window.is_tree_view_mode():
                    QTimer.singleShot(0, self._refresh_after_tree_selection_change)
                    return
                self.window.refresh_filter()
            self._refresh_summary()
            self._refresh_detail_panel()

    def _refresh_after_tree_selection_change(self):
        """在 itemChanged 分发结束后安全重建树形筛选模型。"""
        self.window.refresh_filter()
        self._refresh_summary()
        self._refresh_detail_panel()

    def _on_selection_bulk_changed(self):
        self._refresh_summary()
        self._refresh_detail_panel()

    def _on_focused_candidate_changed(self, _candidate_id):
        self._refresh_detail_panel()

    def _on_filters_changed(self, *_):
        self._apply_filters()
        self._refresh_summary()
        self._refresh_detail_panel()

    def _apply_filters(self):
        self.window.apply_filters(
            show_conflicts_only=self.window.conflict_only_checkbox.isChecked(),
            show_selected_only=self.window.selected_only_checkbox.isChecked(),
            status_filter=self.window.current_status_filter(),
            search_text=self.window.search_edit.text(),
        )

    def _refresh_summary(self):
        if not self._candidates:
            if self._parsed_file_count > 0 or self._failed_file_count > 0:
                self.window.update_summary(
                    f"未加载到可用套件，成功文件 {self._parsed_file_count}，失败文件 {self._failed_file_count}。"
                )
            else:
                self.window.update_summary("未加载 XML 文件")
            return

        selected_count = len(self.window.get_selected_ids())
        conflict_groups = len(self._suggestion.conflicts)
        total = len(self._candidates)
        visible = self.window.get_visible_count()
        self.window.update_summary(
            "已加载 "
            f"{total} 个套件，可见 {visible} 个，当前勾选 {selected_count} 个，冲突组 {conflict_groups}，"
            f"成功文件 {self._parsed_file_count}，失败文件 {self._failed_file_count}。"
        )

    def _refresh_detail_panel(self):
        candidate_id = self.window.current_candidate_id()
        if not candidate_id:
            self.window.clear_detail_panel()
            return

        candidate = self._candidate_by_id.get(candidate_id)
        if candidate is None:
            self.window.clear_detail_panel()
            return

        self.window.update_detail_panel(
            candidate,
            is_selected=self.window.is_candidate_selected(candidate_id),
            is_default_selected=candidate_id in self._default_selected_ids,
        )

    def _run_worker(self, *, busy_message: str, fn, args: Iterable, on_success):
        if self._worker and self._worker.isRunning():
            self._show_error("当前任务仍在执行，请稍后再试。")
            return

        self.window.set_busy(True, busy_message)
        self.window.set_action_enabled(False)
        self._worker = WorkerThread(fn, *args)
        self._worker.succeeded.connect(on_success)
        self._worker.failed.connect(self._on_worker_failed)
        self._worker.finished.connect(self._on_worker_finished)
        self._worker.start()

    def _on_worker_failed(self, message: str):
        self._show_error(message)

    def _on_worker_finished(self):
        self.window.set_busy(False)
        self.window.set_action_enabled(True)
        self._worker = None
        self._refresh_summary()

    def _show_error(self, message: str):
        QMessageBox.critical(self.window, "错误", message)

    def open_single_candidate_log(self, candidate_id: str):
        if not candidate_id:
            return
        if not self._candidates:
            self._show_error("请先加载 XML 文件。")
            return

        candidate = self._candidate_by_id.get(candidate_id)
        if candidate is None:
            self._show_error("当前记录已失效，请重新加载 XML 文件。")
            return

        output_dir = self.window.output_dir
        rebot_options = self.window.get_rebot_options()

        self._run_worker(
            busy_message=f"生成日志中：{candidate.suite_path}",
            fn=self._single_candidate_log_job,
            args=(candidate, output_dir, rebot_options),
            on_success=self._on_single_candidate_log_ready,
        )

    def _on_single_candidate_log_ready(self, log_path: str):
        self._open_path(log_path)

    def export_single_candidate_xml(self, candidate_id: str):
        if not candidate_id:
            return
        if not self._candidates:
            self._show_error("请先加载 XML 文件。")
            return

        candidate = self._candidate_by_id.get(candidate_id)
        if candidate is None:
            self._show_error("当前记录已失效，请重新加载 XML 文件。")
            return

        default_dir = (
            self.window.output_dir
            if os.path.isdir(self.window.output_dir)
            else os.getcwd()
        )
        default_path = os.path.join(
            default_dir,
            f"{self._safe_export_name(candidate.suite_name)}.xml",
        )
        output_path, _ = QFileDialog.getSaveFileName(
            self.window,
            "导出该测试 XML",
            default_path,
            "XML Files (*.xml)",
        )
        if not output_path:
            return
        if not os.path.splitext(output_path)[1]:
            output_path += ".xml"

        self._run_worker(
            busy_message=f"导出 XML 中：{candidate.suite_path}",
            fn=self._single_candidate_xml_export_job,
            args=(candidate, output_path),
            on_success=self._on_single_candidate_xml_exported,
        )

    def _on_single_candidate_xml_exported(self, output_path: str):
        QMessageBox.information(
            self.window,
            "导出完成",
            f"测试 XML 已导出：\n{output_path}",
        )

    @staticmethod
    def _safe_export_name(suite_name: str) -> str:
        safe_name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", suite_name or "")
        safe_name = safe_name.strip(" .")[:120]
        if not safe_name:
            return "suite"
        reserved_names = {
            "CON",
            "PRN",
            "AUX",
            "NUL",
            *(f"COM{index}" for index in range(1, 10)),
            *(f"LPT{index}" for index in range(1, 10)),
        }
        if safe_name.upper() in reserved_names:
            return f"_{safe_name}"
        return safe_name

    @staticmethod
    def _normalize_xlsx_path(output_path: str) -> str:
        root, extension = os.path.splitext(output_path)
        if extension.lower() == ".xlsx":
            return output_path
        if extension:
            return root + ".xlsx"
        return output_path + ".xlsx"

    def _show_load_issues_if_needed(self):
        if not self._load_issues:
            return
        details = self._format_issue_summary(self._load_issues)
        if self._parsed_file_count > 0:
            QMessageBox.warning(
                self.window,
                "部分文件加载失败",
                "已跳过不可解析文件，其余文件可继续使用。\n\n" + details,
            )
            return
        QMessageBox.critical(
            self.window,
            "加载失败",
            "没有可用的 XML 数据。\n\n" + details,
        )

    @staticmethod
    def _normalize_input_paths(file_paths: Sequence[str]) -> list[str]:
        normalized: list[str] = []
        seen: set[str] = set()
        for raw_path in file_paths:
            path = os.path.abspath(raw_path)
            if not path.lower().endswith(".xml"):
                continue
            if path in seen:
                continue
            if not os.path.exists(path):
                continue
            seen.add(path)
            normalized.append(path)
        return normalized

    def _merge_active_file_paths(self, incoming_paths: Sequence[str]) -> list[str]:
        incoming_set = set(incoming_paths)
        retained = [
            path for path in self._active_file_paths if path not in incoming_set
        ]
        return retained + list(incoming_paths)

    @staticmethod
    def _format_issue_summary(issues: Sequence[LoadIssue], limit: int = 8) -> str:
        lines = []
        for issue in issues[:limit]:
            lines.append(f"- {issue.file_path} [{issue.stage}] {issue.message}")
        remaining = len(issues) - len(lines)
        if remaining > 0:
            lines.append(f"... 其余 {remaining} 条省略")
        return "\n".join(lines)
