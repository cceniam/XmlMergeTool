"""用于套件勾选与合并的 PyQt6 主窗口。"""

from __future__ import annotations

import glob
import os
from datetime import datetime
from pathlib import Path
from typing import Iterable, Mapping, Sequence

from PyQt6.QtCore import (
    QPoint,
    QRect,
    QItemSelection,
    QItemSelectionModel,
    QModelIndex,
    QSignalBlocker,
    QSettings,
    QSortFilterProxyModel,
    Qt,
    QUrl,
    pyqtSignal,
)
from PyQt6.QtGui import (
    QColor,
    QDesktopServices,
    QIcon,
    QPixmap,
    QStandardItem,
    QStandardItemModel,
)
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QCheckBox,
    QComboBox,
    QDialog,
    QFormLayout,
    QFrame,
    QGridLayout,
    QHeaderView,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMenu,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QRubberBand,
    QSpinBox,
    QTreeView,
    QVBoxLayout,
    QWidget,
)

from xml_merge_tool.application.models import RebotOptions, SuiteCandidate
from xml_merge_tool.infrastructure.runtime import resource_path
from xml_merge_tool.ui.theme import APP_STYLESHEET, PALETTE

COL_CHECK = 0
COL_SUITE_PATH = 1
COL_STATUS = 2
COL_DEFAULT = 3
COL_CONFLICT_SIZE = 4
COL_TIME = 5
COL_FILE = 6

_CANDIDATE_ID_ROLE = int(Qt.ItemDataRole.UserRole) + 1
_NODE_KIND_ROLE = int(Qt.ItemDataRole.UserRole) + 2
_FULL_PATH_ROLE = int(Qt.ItemDataRole.UserRole) + 3
_SETTINGS_ORG = "Robotframework-XMLMergeTool"
_SETTINGS_APP = "MainWindow"
_SETTINGS_HEADER_STATE = "candidate/header_state"
_SETTINGS_HEADER_STATE_FLAT = "candidate/header_state_flat"
_SETTINGS_HEADER_STATE_TREE = "candidate/header_state_tree"
_SETTINGS_VISIBLE_COLUMNS_FLAT = "candidate/visible_columns_flat"
_SETTINGS_VISIBLE_COLUMNS_TREE = "candidate/visible_columns_tree"
_SETTINGS_VIEW_MODE = "candidate/view_mode"
_SETTINGS_DETAIL_DIALOG_GEOMETRY = "detail/dialog_geometry"
_SETTINGS_REPORT_OPTIONS_DIALOG_GEOMETRY = "report_options/dialog_geometry"
_ROBOT_TIME_FORMATS = ("%Y%m%d %H:%M:%S.%f", "%Y%m%d %H:%M:%S")
_VIEW_MODE_FLAT = "flat"
_VIEW_MODE_TREE = "tree"
_NODE_KIND_CANDIDATE = "candidate"
_NODE_KIND_GROUP = "group"


def _normalize_status(status: str) -> str:
    return (status or "").strip().upper()


class ClickableLabel(QLabel):
    """保留标签外观，同时提供明确的鼠标点击信号。"""

    clicked = pyqtSignal()

    def mouseReleaseEvent(self, event):
        if (
            self.isEnabled()
            and event.button() == Qt.MouseButton.LeftButton
            and self.rect().contains(event.position().toPoint())
        ):
            self.clicked.emit()
        super().mouseReleaseEvent(event)


class CandidateFilterProxyModel(QSortFilterProxyModel):
    """支持多条件过滤的候选代理模型。"""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._show_conflicts_only = False
        self._show_selected_only = False
        self._status_filter = "ALL"
        self._search_text = ""
        self.setDynamicSortFilter(True)
        self.setSortCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)

    def set_filters(
        self,
        *,
        show_conflicts_only: bool | None = None,
        show_selected_only: bool | None = None,
        status_filter: str | None = None,
        search_text: str | None = None,
    ) -> None:
        if show_conflicts_only is not None and show_conflicts_only != self._show_conflicts_only:
            self._show_conflicts_only = show_conflicts_only
        if show_selected_only is not None and show_selected_only != self._show_selected_only:
            self._show_selected_only = show_selected_only

        normalized_status = (status_filter or "ALL").strip().upper()
        if normalized_status != self._status_filter:
            self._status_filter = normalized_status

        normalized_search = (search_text or "").strip().lower()
        if normalized_search != self._search_text:
            self._search_text = normalized_search
        # Source model updates may happen while signals are blocked, so rebuild mapping.
        self.invalidate()

    def reset_filters(self, *, invalidate: bool = False) -> None:
        self._show_conflicts_only = False
        self._show_selected_only = False
        self._status_filter = "ALL"
        self._search_text = ""
        if invalidate:
            self.invalidate()

    def filterAcceptsRow(self, source_row: int, source_parent: QModelIndex) -> bool:
        source_model = self.sourceModel()
        if source_model is None:
            return True

        check_index = source_model.index(source_row, COL_CHECK, source_parent)
        if not check_index.isValid():
            return False

        node_kind = str(
            source_model.data(check_index, _NODE_KIND_ROLE) or _NODE_KIND_CANDIDATE
        )
        if node_kind == _NODE_KIND_GROUP:
            return self._group_matches(source_model, source_row, source_parent) or self._has_matching_child(
                source_model,
                check_index,
            )

        return self._candidate_matches(source_model, source_row, source_parent)

    def _candidate_matches(
        self,
        source_model,
        source_row: int,
        source_parent: QModelIndex,
    ) -> bool:
        check_index = source_model.index(source_row, COL_CHECK, source_parent)

        if self._show_selected_only:
            check_state = source_model.data(check_index, Qt.ItemDataRole.CheckStateRole)
            if not self._is_checked(check_state):
                return False

        if self._show_conflicts_only:
            conflict_index = source_model.index(source_row, COL_CONFLICT_SIZE, source_parent)
            size_value = str(source_model.data(conflict_index, Qt.ItemDataRole.DisplayRole) or "0")
            try:
                if int(size_value) < 2:
                    return False
            except ValueError:
                return False

        if self._status_filter != "ALL":
            status_index = source_model.index(source_row, COL_STATUS, source_parent)
            status = _normalize_status(
                str(source_model.data(status_index, Qt.ItemDataRole.DisplayRole) or "")
            )
            if status != self._status_filter:
                return False

        if self._search_text:
            suite_index = source_model.index(source_row, COL_SUITE_PATH, source_parent)
            file_index = source_model.index(source_row, COL_FILE, source_parent)
            suite_text = str(source_model.data(suite_index, Qt.ItemDataRole.DisplayRole) or "")
            file_text = str(source_model.data(file_index, Qt.ItemDataRole.DisplayRole) or "")
            haystack = f"{suite_text} {file_text}".lower()
            if (
                self._search_text not in haystack
                and not self._ancestor_group_matches(source_model, source_parent)
            ):
                return False

        return True

    def _group_matches(self, source_model, source_row: int, source_parent: QModelIndex) -> bool:
        if not self._search_text:
            return False

        path_index = source_model.index(source_row, COL_SUITE_PATH, source_parent)
        display_text = str(source_model.data(path_index, Qt.ItemDataRole.DisplayRole) or "")
        full_path = str(source_model.data(path_index, _FULL_PATH_ROLE) or display_text)
        return self._search_text in f"{display_text} {full_path}".lower()

    def _ancestor_group_matches(self, source_model, parent_index: QModelIndex) -> bool:
        if not self._search_text:
            return False

        current_parent = parent_index
        while current_parent.isValid():
            row = current_parent.row()
            grand_parent = current_parent.parent()
            if self._group_matches(source_model, row, grand_parent):
                return True
            current_parent = grand_parent
        return False

    def _has_matching_child(self, source_model, parent_index: QModelIndex) -> bool:
        for row in range(source_model.rowCount(parent_index)):
            if self.filterAcceptsRow(row, parent_index):
                return True
        return False

    @staticmethod
    def _is_checked(value) -> bool:
        if value is None:
            return False
        if isinstance(value, Qt.CheckState):
            return value == Qt.CheckState.Checked
        try:
            return Qt.CheckState(int(value)) == Qt.CheckState.Checked
        except (TypeError, ValueError):
            return False


class CandidateTreeView(QTreeView):
    """支持拖拽 XML 的树形视图。"""

    files_dropped = pyqtSignal(list)

    def __init__(self):
        super().__init__()
        self.setAcceptDrops(True)
        self.setDragEnabled(False)
        self.setDragDropMode(QAbstractItemView.DragDropMode.DropOnly)
        self.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        self._rubber_origin: QPoint | None = None
        self._rubber_current_pos: QPoint | None = None
        self._rubber_active = False
        self._rubber_modifiers = Qt.KeyboardModifier.NoModifier
        self._rubber_anchor_row: int | None = None
        self._rubber_band = QRubberBand(QRubberBand.Shape.Rectangle, self.viewport())

    def mousePressEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton:
            self._rubber_origin = event.pos()
            self._rubber_current_pos = event.pos()
            self._rubber_active = False
            self._rubber_modifiers = event.modifiers()
            self._rubber_anchor_row = self._row_at_position(event.pos())
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event):
        if (
            self._rubber_origin is not None
            and event.buttons() & Qt.MouseButton.LeftButton
        ):
            distance = (event.pos() - self._rubber_origin).manhattanLength()
            if not self._rubber_active and distance >= QApplication.startDragDistance():
                self._rubber_active = True
                self._rubber_band.setGeometry(QRect(self._rubber_origin, self._rubber_origin))
                self._rubber_band.show()

            if self._rubber_active:
                self._rubber_current_pos = event.pos()
                rect = QRect(self._rubber_origin, event.pos()).normalized()
                self._rubber_band.setGeometry(rect)
                self._apply_rubber_selection(event.pos())
                event.accept()
                return

        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event):
        if event.button() == Qt.MouseButton.LeftButton and self._rubber_active:
            self._rubber_band.hide()
            self._rubber_active = False
            self._rubber_origin = None
            self._rubber_current_pos = None
            self._rubber_anchor_row = None
            event.accept()
            return

        if event.button() == Qt.MouseButton.LeftButton:
            self._rubber_origin = None
            self._rubber_current_pos = None
            self._rubber_active = False
            self._rubber_anchor_row = None
            self._rubber_band.hide()

        super().mouseReleaseEvent(event)

    def scrollContentsBy(self, dx: int, dy: int):
        super().scrollContentsBy(dx, dy)
        if (
            not self._rubber_active
            or self._rubber_origin is None
            or self._rubber_current_pos is None
        ):
            return

        current_pos = self._rubber_current_pos
        rect = QRect(self._rubber_origin, current_pos).normalized()
        self._rubber_band.setGeometry(rect)
        self._apply_rubber_selection(current_pos)

    def dragEnterEvent(self, event):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()
            return
        event.ignore()

    def dragMoveEvent(self, event):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()
            return
        event.ignore()

    def dropEvent(self, event):
        if not event.mimeData().hasUrls():
            event.ignore()
            return

        paths: list[str] = []
        for url in event.mimeData().urls():
            local_path = url.toLocalFile()
            if not local_path:
                continue
            if os.path.isdir(local_path):
                paths.extend(glob.glob(os.path.join(local_path, "*.xml")))
            elif local_path.lower().endswith(".xml"):
                paths.append(local_path)

        if paths:
            self.files_dropped.emit(paths)
            event.acceptProposedAction()
        else:
            event.ignore()

    def _apply_rubber_selection(self, current_pos: QPoint):
        model = self.model()
        selection_model = self.selectionModel()
        if model is None or selection_model is None:
            return

        row_count = model.rowCount()
        column_count = model.columnCount()
        if row_count <= 0 or column_count <= 0:
            return

        anchor_row = self._rubber_anchor_row
        if anchor_row is None:
            anchor_row = self._row_at_position(current_pos)
            self._rubber_anchor_row = anchor_row
        if anchor_row is None:
            return

        current_row = self._row_at_position(current_pos)
        if current_row is None:
            return

        if self._rubber_modifiers & Qt.KeyboardModifier.ControlModifier:
            flags = (
                QItemSelectionModel.SelectionFlag.Select
                | QItemSelectionModel.SelectionFlag.Rows
            )
        else:
            selection_model.clearSelection()
            flags = (
                QItemSelectionModel.SelectionFlag.Select
                | QItemSelectionModel.SelectionFlag.Rows
            )

        start_row = max(0, min(anchor_row, current_row))
        end_row = min(row_count - 1, max(anchor_row, current_row))

        selection = QItemSelection()
        last_column = column_count - 1
        top_left = model.index(start_row, 0)
        bottom_right = model.index(end_row, last_column)
        selection.select(top_left, bottom_right)
        selection_model.select(selection, flags)

    def _row_at_position(self, pos: QPoint) -> int | None:
        model = self.model()
        if model is None or model.rowCount() <= 0:
            return None

        viewport_height = self.viewport().height()
        y = max(0, min(pos.y(), max(0, viewport_height - 1)))
        row_probe_x = self._row_probe_x(model.columnCount())
        index = self.indexAt(QPoint(row_probe_x, y))
        if index.isValid():
            return index.row()

        top_index = self.indexAt(QPoint(row_probe_x, 0))
        if top_index.isValid():
            if pos.y() < 0:
                return top_index.row()

            bottom_index = self.indexAt(QPoint(row_probe_x, max(0, viewport_height - 1)))
            if bottom_index.isValid():
                return bottom_index.row()
            return top_index.row()

        return 0

    def _row_probe_x(self, column_count: int) -> int:
        viewport_width = max(1, self.viewport().width())
        probe_column = 0
        for column in range(column_count):
            if not self.isColumnHidden(column):
                probe_column = column
                break

        left = self.columnViewportPosition(probe_column)
        width = max(1, self.columnWidth(probe_column))
        probe_x = left + min(8, width - 1)
        return max(0, min(probe_x, viewport_width - 1))


class DetailDialog(QDialog):
    """用于展示候选详情的非模态对话框。"""

    visibility_changed = pyqtSignal(bool)

    def showEvent(self, event):
        super().showEvent(event)
        self.visibility_changed.emit(True)

    def hideEvent(self, event):
        super().hideEvent(event)
        self.visibility_changed.emit(False)


class ReportOptionsDialog(QDialog):
    """用于展示高级输出选项的非模态对话框。"""

    visibility_changed = pyqtSignal(bool)

    def showEvent(self, event):
        super().showEvent(event)
        self.visibility_changed.emit(True)

    def hideEvent(self, event):
        super().hideEvent(event)
        self.visibility_changed.emit(False)


class MainWindow(QMainWindow):
    """包含操作控件、候选列表与冲突详情的主窗口。"""

    focused_candidate_changed = pyqtSignal(object)
    selection_bulk_changed = pyqtSignal()
    candidate_double_clicked = pyqtSignal(str)
    select_all_filtered_requested = pyqtSignal()
    invert_filtered_requested = pyqtSignal()
    open_output_dir_requested = pyqtSignal()
    output_dir_change_requested = pyqtSignal()
    candidate_export_requested = pyqtSignal(str)

    def __init__(self):
        super().__init__()
        self.output_dir = str(Path.cwd())
        self._settings = QSettings(_SETTINGS_ORG, _SETTINGS_APP)
        self._checkbox_item_by_candidate_id: dict[str, QStandardItem] = {}
        self._current_candidates: tuple[SuiteCandidate, ...] = tuple()
        self._default_selected_ids: set[str] = set()
        self._conflict_group_size_by_suite: dict[str, int] = {}
        self._selected_candidate_ids: set[str] = set()
        self._active_view_mode = _VIEW_MODE_FLAT
        self._check_sync_in_progress = False
        self._model_rebuild_in_progress = False
        self._header_restore_in_progress = False
        self._detail_visible = False
        self._detail_geometry_restored = False
        self._report_options_geometry_restored = False
        self._tree_selection_model: QItemSelectionModel | None = None
        self._actions_enabled = True
        self._build_ui()

    def _build_ui(self):
        self.setWindowTitle("Robot XML Merge Tool")
        self._apply_window_icon()
        self.resize(1320, 800)
        self.setStyleSheet(APP_STYLESHEET)

        self.select_folder_button = QPushButton("设置输出目录")
        self.open_button = QPushButton("打开 XML 文件")
        self.restore_default_button = QPushButton("恢复默认勾选")
        self.export_excel_button = QPushButton("导出勾选 Excel")
        self.merge_button = QPushButton("合并勾选结果")
        self.merge_button.setObjectName("PrimaryButton")
        self.clear_button = QPushButton("清空列表")
        self.clear_button.setObjectName("DangerButton")

        self.conflict_only_checkbox = QCheckBox("只看冲突")
        self.selected_only_checkbox = QCheckBox("只看已勾选")
        self.compact_mode_checkbox = QCheckBox("紧凑模式")
        self.view_mode_combo = QComboBox()
        self.view_mode_combo.addItem("平铺表格", _VIEW_MODE_FLAT)
        self.view_mode_combo.addItem("路径树", _VIEW_MODE_TREE)
        self.view_mode_combo.setToolTip("切换候选区域的展示方式。")

        self.status_filter_combo = QComboBox()
        self.status_filter_combo.addItem("全部状态", "ALL")
        self.status_filter_combo.addItem("PASS", "PASS")
        self.status_filter_combo.addItem("SKIP", "SKIP")
        self.status_filter_combo.addItem("FAIL", "FAIL")

        self.search_edit = QLineEdit()
        self.search_edit.setPlaceholderText("搜索：文件名 / 套件路径")
        self.search_edit.setClearButtonEnabled(True)
        self.search_edit.setMinimumWidth(320)

        self.open_report_options_button = QPushButton("高级选项")

        self.tree_view = CandidateTreeView()
        self.tree_view.setRootIsDecorated(False)
        self.tree_view.setAlternatingRowColors(True)
        self.tree_view.setUniformRowHeights(True)
        self.tree_view.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.tree_view.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        self.tree_view.setSortingEnabled(True)
        self.tree_view.setProperty("compact", False)
        self.tree_view.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)

        self.model = QStandardItemModel(0, 7, self)
        self.model.setHorizontalHeaderLabels(
            [
                "勾选",
                "测试套件路径",
                "状态",
                "默认推荐",
                "冲突组大小",
                "开始时间",
                "文件",
            ]
        )

        self.proxy_model = CandidateFilterProxyModel(self)
        self.proxy_model.setSourceModel(self.model)
        self.proxy_model.setRecursiveFilteringEnabled(False)
        self.proxy_model.setAutoAcceptChildRows(False)
        self.tree_view.setModel(self.proxy_model)

        self.summary_label = QLabel("未加载 XML 文件")
        self.summary_label.setObjectName("SummaryLabel")
        self.visible_count_label = QLabel("可见: 0")
        self.visible_count_label.setObjectName("MutedText")
        self.progress_bar = QProgressBar()
        self.progress_bar.setVisible(False)
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        self.progress_bar.setFixedWidth(180)

        self.output_dir_label = ClickableLabel()
        self.output_dir_label.setObjectName("MutedText")
        self.output_dir_label.setCursor(Qt.CursorShape.PointingHandCursor)
        self.output_dir_label.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        self.output_dir_label.clicked.connect(self.output_dir_change_requested.emit)
        self.log_level_combo = QComboBox()
        for level in ("TRACE", "DEBUG", "INFO", "WARN", "NONE"):
            self.log_level_combo.addItem(level, level)
        self.log_level_combo.setToolTip(
            "对应 rebot --loglevel 的消息阈值；TRACE 最详细，NONE 不保留消息。"
        )

        self.visible_log_level_combo = QComboBox()
        self.visible_log_level_combo.addItem("跟随阈值", None)
        for level in ("TRACE", "DEBUG", "INFO", "WARN"):
            self.visible_log_level_combo.addItem(level, level)
        self.visible_log_level_combo.setToolTip(
            "对应 --loglevel LOGLEVEL:DEFAULT 的后半段，控制 Log 页面默认展示级别。"
        )

        self.split_log_checkbox = QCheckBox("拆分大体积 Log")
        self.split_log_checkbox.setToolTip(
            "对应 rebot --splitlog，合并结果较大时可显著降低单页 Log 体积。"
        )

        self.suite_stat_level_spin = QSpinBox()
        self.suite_stat_level_spin.setRange(0, 99)
        self.suite_stat_level_spin.setSpecialValueText("全部层级")
        self.suite_stat_level_spin.setValue(0)
        self.suite_stat_level_spin.setToolTip(
            "对应 rebot --suitestatlevel；0 表示保持默认，显示全部套件层级。"
        )

        self.remove_keywords_combo = QComboBox()
        self.remove_keywords_combo.addItem("保留全部", "")
        self.remove_keywords_combo.addItem("移除通过关键字", "passed")
        self.remove_keywords_combo.addItem("仅保留 WUKS 最后失败", "wuks")
        self.remove_keywords_combo.addItem("裁剪 FOR 通过迭代", "for")
        self.remove_keywords_combo.addItem("裁剪 WHILE 通过迭代", "while")
        self.remove_keywords_combo.addItem("移除全部关键字", "all")
        self.remove_keywords_combo.setToolTip(
            "对应 rebot --removekeywords，可压缩合并后日志体积。"
        )

        self.flatten_keywords_combo = QComboBox()
        self.flatten_keywords_combo.addItem("不扁平化", "")
        self.flatten_keywords_combo.addItem("FOR 循环", "for")
        self.flatten_keywords_combo.addItem("WHILE 循环", "while")
        self.flatten_keywords_combo.addItem("循环迭代", "iteration")
        self.flatten_keywords_combo.setToolTip(
            "对应 rebot --flattenkeywords，可减少日志层级深度。"
        )

        self.log_title_edit = QLineEdit()
        self.log_title_edit.setPlaceholderText("留空使用默认 Log 标题")
        self.log_title_edit.setToolTip("对应 rebot --logtitle。")

        self.report_title_edit = QLineEdit()
        self.report_title_edit.setPlaceholderText("留空使用默认 Report 标题")
        self.report_title_edit.setToolTip("对应 rebot --reporttitle。")

        self.report_background_edit = QLineEdit()
        self.report_background_edit.setPlaceholderText("例如 green:red:yellow")
        self.report_background_edit.setToolTip(
            "对应 rebot --reportbackground，格式为 passed:failed:skipped。"
        )

        self.detail_panel = self._build_detail_panel()
        self.detail_dialog = self._build_detail_dialog()
        self.report_options_dialog = self._build_report_options_dialog()
        self.command_card = self._build_command_card()
        self.workspace_card = self._build_workspace_card()

        layout = QVBoxLayout()
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(8)
        layout.addWidget(self.command_card)
        layout.addWidget(self.workspace_card, 1)

        container = QWidget()
        container.setLayout(layout)
        self.setCentralWidget(container)

        self._bind_internal_signals()
        self._header_restore_in_progress = True
        self._configure_header()
        self._apply_default_column_widths()
        self._load_preferences()
        self._header_restore_in_progress = False
        self.set_output_dir(self.output_dir)
        self.clear_detail_panel()

    def _bind_internal_signals(self):
        self.compact_mode_checkbox.toggled.connect(self._set_compact_mode)
        self.view_mode_combo.currentIndexChanged.connect(self._on_view_mode_changed)
        self.open_report_options_button.clicked.connect(self.open_report_options_dialog)
        self.model.itemChanged.connect(self._on_model_item_changed)
        self.tree_view.customContextMenuRequested.connect(self._show_tree_context_menu)
        self.tree_view.doubleClicked.connect(self._on_tree_double_clicked)
        self.detail_dialog.visibility_changed.connect(
            self._on_detail_dialog_visibility_changed
        )
        self.report_options_dialog.visibility_changed.connect(
            self._on_report_options_dialog_visibility_changed
        )
        self._rebind_tree_selection_model()

    def _rebind_tree_selection_model(self) -> None:
        if self._tree_selection_model is not None:
            try:
                self._tree_selection_model.currentChanged.disconnect(
                    self._on_current_changed
                )
            except TypeError:
                pass

        self._tree_selection_model = self.tree_view.selectionModel()
        if self._tree_selection_model is not None:
            self._tree_selection_model.currentChanged.connect(
                self._on_current_changed
            )

    def _apply_view_model_for_mode(self) -> None:
        # Keep the view and its selection model stable for the lifetime of the
        # window. Replacing them while Cocoa accessibility is inspecting the
        # tree can leave native code holding indexes from the deleted model.
        if self.tree_view.model() is self.proxy_model:
            return
        self.tree_view.setModel(self.proxy_model)
        self._rebind_tree_selection_model()

    def _active_view_model(self):
        return self.tree_view.model()

    def _view_index_to_source(self, index: QModelIndex) -> QModelIndex:
        if not index.isValid():
            return QModelIndex()

        model = index.model()
        if model is self.model:
            return index
        if model is self.proxy_model:
            return self.proxy_model.mapToSource(index)
        return QModelIndex()

    def _source_index_to_view(self, source_index: QModelIndex) -> QModelIndex:
        if not source_index.isValid():
            return QModelIndex()
        return self.proxy_model.mapFromSource(source_index)

    def _candidate_id_from_source_index(self, source_index: QModelIndex) -> str | None:
        checkbox_item = self._checkbox_item_from_index(source_index)
        if checkbox_item is None or not self._is_candidate_item(checkbox_item):
            return None
        candidate_id = checkbox_item.data(_CANDIDATE_ID_ROLE)
        return str(candidate_id) if candidate_id else None

    def _view_index_for_candidate(self, candidate_id: str | None) -> QModelIndex:
        if not candidate_id:
            return QModelIndex()

        checkbox_item = self._checkbox_item_by_candidate_id.get(candidate_id)
        if checkbox_item is None:
            return QModelIndex()

        source_index = checkbox_item.index().siblingAtColumn(COL_SUITE_PATH)
        return self._source_index_to_view(source_index)

    def _apply_window_icon(self):
        icon = QIcon()
        for candidate in self._logo_resource_candidates():
            if not os.path.exists(candidate):
                continue
            next_icon = QIcon(candidate)
            if next_icon.isNull():
                continue
            icon = next_icon
            break
        if icon.isNull():
            return
        self.setWindowIcon(icon)
        app = QApplication.instance()
        if app is not None:
            app.setWindowIcon(icon)

    def _build_command_card(self) -> QFrame:
        card = QFrame()
        card.setObjectName("CommandCard")

        layout = QVBoxLayout(card)
        layout.setContentsMargins(12, 10, 12, 10)
        layout.setSpacing(8)

        primary_row = QHBoxLayout()
        primary_row.setContentsMargins(0, 0, 0, 0)
        primary_row.setSpacing(10)
        primary_row.addWidget(self._build_branding_row(), 1)
        self.header_action_group = self._build_action_strip()
        primary_row.addWidget(self.header_action_group, 0, Qt.AlignmentFlag.AlignVCenter)
        primary_row.addWidget(
            self.export_excel_button,
            0,
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
        )
        primary_row.addWidget(
            self.merge_button,
            0,
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
        )

        layout.addLayout(primary_row)
        layout.addWidget(self._build_command_secondary_row())
        return card

    def _build_branding_row(self) -> QWidget:
        container = QWidget()
        row = QHBoxLayout(container)
        row.setContentsMargins(0, 0, 0, 0)
        row.setSpacing(10)

        logo_label = QLabel()
        logo_label.setObjectName("HeroLogo")
        logo_label.setFixedSize(36, 36)
        pixmap = self._load_logo_pixmap(26)
        if pixmap is not None and not pixmap.isNull():
            logo_label.setPixmap(pixmap)
            logo_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        else:
            logo_label.setText("XML")
            logo_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
            logo_label.setObjectName("MutedText")

        title = QLabel("Robot XML Merge Tool")
        title.setObjectName("HeroTitle")
        subtitle = QLabel("Merge & Resolve Robot Output Suites")
        subtitle.setObjectName("HeroSubtitle")

        text_col = QVBoxLayout()
        text_col.setContentsMargins(0, 0, 0, 0)
        text_col.setSpacing(2)
        text_col.addWidget(title)
        text_col.addWidget(subtitle)

        row.addWidget(logo_label, 0, Qt.AlignmentFlag.AlignVCenter)
        row.addLayout(text_col)
        row.addStretch(1)
        return container

    def _build_action_strip(self) -> QWidget:
        container = QWidget()
        container.setObjectName("HeaderToolGroup")
        layout = QHBoxLayout(container)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        for button in (
            self.open_button,
            self.restore_default_button,
            self.clear_button,
        ):
            layout.addWidget(button)
        return container

    def _build_command_secondary_row(self) -> QWidget:
        row_widget = QWidget()
        row_widget.setObjectName("CommandSecondaryRow")
        layout = QHBoxLayout(row_widget)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        utility_group = QFrame()
        utility_group.setObjectName("ToolbarGroup")
        utility_layout = QHBoxLayout(utility_group)
        utility_layout.setContentsMargins(8, 6, 8, 6)
        utility_layout.setSpacing(8)
        utility_layout.addWidget(self.select_folder_button)
        utility_layout.addWidget(self.open_report_options_button)

        layout.addWidget(self.search_edit, 1)
        layout.addWidget(
            utility_group,
            0,
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
        )
        return row_widget

    def _build_workspace_card(self) -> QFrame:
        card = QFrame()
        card.setObjectName("WorkspaceCard")
        layout = QVBoxLayout(card)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(8)
        layout.addLayout(self._build_workspace_status_row())
        layout.addWidget(self.tree_view, 1)
        layout.addWidget(self._build_workspace_footer())
        return card

    def _build_workspace_status_row(self) -> QHBoxLayout:
        row = QHBoxLayout()
        row.setSpacing(10)
        row.addWidget(self.summary_label, 1)
        row.addWidget(self.visible_count_label)
        row.addWidget(self.progress_bar)
        return row

    def _build_workspace_footer(self) -> QFrame:
        self.workspace_footer_filter_group = QFrame()
        self.workspace_footer_filter_group.setObjectName("FooterFilterGroup")
        layout = QHBoxLayout(self.workspace_footer_filter_group)
        layout.setContentsMargins(10, 6, 10, 6)
        layout.setSpacing(10)

        status_label = QLabel("状态筛选")
        status_label.setObjectName("FieldLabel")
        view_mode_label = QLabel("展示")
        view_mode_label.setObjectName("FieldLabel")

        layout.addWidget(self.conflict_only_checkbox)
        layout.addWidget(self.selected_only_checkbox)
        layout.addWidget(status_label)
        layout.addWidget(self.status_filter_combo)
        layout.addWidget(view_mode_label)
        layout.addWidget(self.view_mode_combo)
        layout.addWidget(self.compact_mode_checkbox)
        layout.addStretch(1)
        layout.addWidget(self.output_dir_label, 1)
        return self.workspace_footer_filter_group

    def _build_detail_dialog(self) -> DetailDialog:
        dialog = DetailDialog(self)
        dialog.setObjectName("DetailDialog")
        dialog.setWindowTitle("候选项详情")
        dialog.setWindowModality(Qt.WindowModality.NonModal)
        dialog.resize(520, 580)

        layout = QVBoxLayout(dialog)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(0)
        layout.addWidget(self.detail_panel)
        return dialog

    def _build_report_options_dialog(self) -> ReportOptionsDialog:
        dialog = ReportOptionsDialog(self)
        dialog.setObjectName("ReportOptionsDialog")
        dialog.setWindowTitle("高级输出选项")
        dialog.setWindowModality(Qt.WindowModality.NonModal)
        dialog.resize(700, 420)

        layout = QVBoxLayout(dialog)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(0)

        self.report_options_panel = QFrame()
        self.report_options_panel.setObjectName("ReportOptionsPanel")
        panel_layout = QVBoxLayout(self.report_options_panel)
        panel_layout.setContentsMargins(14, 12, 14, 12)
        panel_layout.setSpacing(10)

        title = QLabel("报告与输出选项")
        title.setObjectName("SectionTitle")

        self.report_options_hint = QLabel(
            "优先开放影响日志体积、可读性和报告展示的参数；筛选类 rebot 参数暂不放到这里，避免和当前勾选逻辑冲突。"
        )
        self.report_options_hint.setObjectName("MutedText")
        self.report_options_hint.setWordWrap(True)

        self.report_options_content = QWidget()
        grid = QGridLayout(self.report_options_content)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setHorizontalSpacing(12)
        grid.setVerticalSpacing(10)
        grid.setColumnStretch(1, 1)
        grid.setColumnStretch(3, 1)

        self._add_report_option_field(grid, 0, 0, "消息阈值", self.log_level_combo)
        self._add_report_option_field(grid, 0, 2, "默认显示", self.visible_log_level_combo)
        grid.addWidget(self.split_log_checkbox, 1, 0, 1, 2)
        self._add_report_option_field(grid, 1, 2, "套件统计", self.suite_stat_level_spin)
        self._add_report_option_field(grid, 2, 0, "关键字裁剪", self.remove_keywords_combo)
        self._add_report_option_field(grid, 2, 2, "关键字扁平化", self.flatten_keywords_combo)
        self._add_report_option_field(
            grid,
            3,
            0,
            "报告背景",
            self.report_background_edit,
            column_span=3,
        )
        self._add_report_option_field(grid, 4, 0, "Log 标题", self.log_title_edit)
        self._add_report_option_field(grid, 4, 2, "Report 标题", self.report_title_edit)

        panel_layout.addWidget(title)
        panel_layout.addWidget(self.report_options_hint)
        panel_layout.addWidget(self.report_options_content)
        layout.addWidget(self.report_options_panel)
        return dialog

    def _load_logo_pixmap(self, size: int):
        for candidate in self._logo_resource_candidates():
            if not os.path.exists(candidate):
                continue
            pixmap = QPixmap(candidate)
            if pixmap.isNull():
                continue
            return pixmap.scaled(
                size,
                size,
                Qt.AspectRatioMode.KeepAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            )
        return None

    @staticmethod
    def _logo_resource_candidates() -> list[str]:
        return [
            resource_path("image", "icon.webp"),
            resource_path("image", "logo.ico"),
        ]

    def set_output_dir(self, output_dir: str):
        self.output_dir = output_dir
        self.output_dir_label.setText(f"输出目录: {output_dir}")
        self.output_dir_label.setToolTip(output_dir)

    def open_output_dir(self):
        if self.output_dir and os.path.isdir(self.output_dir):
            QDesktopServices.openUrl(QUrl.fromLocalFile(self.output_dir))

    def set_busy(self, busy: bool, message: str = ""):
        if busy:
            self.progress_bar.setVisible(True)
            self.progress_bar.setRange(0, 0)
            self.progress_bar.setFormat("处理中...")
            if message:
                self.summary_label.setText(message)
        else:
            self.progress_bar.setVisible(False)
            self.progress_bar.setRange(0, 100)
            self.progress_bar.setValue(0)
            self.progress_bar.setFormat("%p%")

    def set_action_enabled(self, enabled: bool):
        self._actions_enabled = enabled
        for widget in (
            self.open_button,
            self.restore_default_button,
            self.export_excel_button,
            self.merge_button,
            self.clear_button,
            self.select_folder_button,
            self.conflict_only_checkbox,
            self.selected_only_checkbox,
            self.status_filter_combo,
            self.search_edit,
            self.view_mode_combo,
            self.compact_mode_checkbox,
            self.output_dir_label,
            self.open_report_options_button,
            self.log_level_combo,
            self.visible_log_level_combo,
            self.split_log_checkbox,
            self.suite_stat_level_spin,
            self.remove_keywords_combo,
            self.flatten_keywords_combo,
            self.log_title_edit,
            self.report_title_edit,
            self.report_background_edit,
        ):
            widget.setEnabled(enabled)

    def get_rebot_options(self) -> RebotOptions:
        suite_stat_level = self.suite_stat_level_spin.value()
        visible_log_level = self.visible_log_level_combo.currentData()
        remove_keywords = self.remove_keywords_combo.currentData()
        flatten_keywords = self.flatten_keywords_combo.currentData()
        return RebotOptions(
            log_level=str(self.log_level_combo.currentData() or "TRACE"),
            visible_log_level=(
                str(visible_log_level).strip() if visible_log_level else None
            ),
            split_log=self.split_log_checkbox.isChecked(),
            log_title=self._optional_text(self.log_title_edit.text()),
            report_title=self._optional_text(self.report_title_edit.text()),
            report_background=self._optional_text(self.report_background_edit.text()),
            suite_stat_level=suite_stat_level if suite_stat_level > 0 else None,
            remove_keywords=(
                str(remove_keywords).strip() if remove_keywords else None
            ),
            flatten_keywords=(
                str(flatten_keywords).strip() if flatten_keywords else None
            ),
        )

    @staticmethod
    def _optional_text(value: str) -> str | None:
        cleaned = value.strip()
        return cleaned or None

    @staticmethod
    def _add_report_option_field(
        grid: QGridLayout,
        row: int,
        column: int,
        label_text: str,
        widget: QWidget,
        *,
        column_span: int = 1,
    ) -> None:
        label = QLabel(label_text)
        label.setObjectName("FieldLabel")
        grid.addWidget(label, row, column)
        grid.addWidget(widget, row, column + 1, 1, column_span)

    def clear_candidates(self):
        self.model.removeRows(0, self.model.rowCount())
        self._checkbox_item_by_candidate_id.clear()
        self._current_candidates = tuple()
        self._default_selected_ids = set()
        self._conflict_group_size_by_suite = {}
        self._selected_candidate_ids = set()
        self.summary_label.setText("未加载 XML 文件")
        self.visible_count_label.setText("可见: 0")
        self.clear_detail_panel()
        self.focused_candidate_changed.emit(None)

    def populate_candidates(
        self,
        candidates: Sequence[SuiteCandidate],
        selected_ids: Iterable[str],
        *,
        default_selected_ids: Iterable[str] | None = None,
        conflict_group_size_by_suite: Mapping[str, int] | None = None,
    ):
        self._current_candidates = tuple(candidates)
        self._default_selected_ids = set(default_selected_ids or selected_ids)
        self._conflict_group_size_by_suite = dict(conflict_group_size_by_suite or {})
        self._selected_candidate_ids = set(selected_ids)
        self._rebuild_candidate_model(selected_ids=set(self._selected_candidate_ids))

    def get_selected_ids(self) -> list[str]:
        if self._current_candidates:
            return [
                candidate.candidate_id
                for candidate in self._current_candidates
                if candidate.candidate_id in self._selected_candidate_ids
            ]
        return [
            candidate_id
            for candidate_id, checkbox_item in self._checkbox_item_by_candidate_id.items()
            if checkbox_item.checkState() == Qt.CheckState.Checked
        ]

    def get_filtered_candidate_ids(self) -> list[str]:
        candidate_ids: list[str] = []
        for view_index in self._iter_view_leaf_check_indexes():
            source_index = self._view_index_to_source(view_index)
            checkbox_item = self._checkbox_item_from_index(source_index)
            if checkbox_item is None or not self._is_candidate_item(checkbox_item):
                continue
            candidate_id = checkbox_item.data(_CANDIDATE_ID_ROLE)
            if candidate_id:
                candidate_ids.append(str(candidate_id))
        return candidate_ids

    def is_candidate_selected(self, candidate_id: str) -> bool:
        return candidate_id in self._selected_candidate_ids

    def apply_selection(self, selected_ids: Iterable[str]):
        selected_set = set(selected_ids)
        self._selected_candidate_ids = set(selected_set)
        self.model.blockSignals(True)
        for candidate_id, checkbox_item in self._checkbox_item_by_candidate_id.items():
            checkbox_item.setCheckState(
                Qt.CheckState.Checked if candidate_id in selected_set else Qt.CheckState.Unchecked
            )
        self._sync_all_group_check_states()
        self.model.blockSignals(False)
        self.refresh_filter()

    def set_filtered_selection(self, checked: bool) -> int:
        candidate_ids = self.get_filtered_candidate_ids()
        changed = 0
        self.model.blockSignals(True)
        for candidate_id in candidate_ids:
            checkbox_item = self._checkbox_item_by_candidate_id.get(candidate_id)
            if checkbox_item is None:
                continue
            expected = Qt.CheckState.Checked if checked else Qt.CheckState.Unchecked
            if checkbox_item.checkState() == expected:
                continue
            checkbox_item.setCheckState(expected)
            changed += 1
        self._sync_all_group_check_states()
        self.model.blockSignals(False)
        self._sync_selected_ids_from_visible_items()
        self.refresh_filter()
        return changed

    def invert_filtered_selection(self) -> int:
        candidate_ids = self.get_filtered_candidate_ids()
        changed = 0
        self.model.blockSignals(True)
        for candidate_id in candidate_ids:
            checkbox_item = self._checkbox_item_by_candidate_id.get(candidate_id)
            if checkbox_item is None:
                continue
            next_state = (
                Qt.CheckState.Unchecked
                if checkbox_item.checkState() == Qt.CheckState.Checked
                else Qt.CheckState.Checked
            )
            checkbox_item.setCheckState(next_state)
            changed += 1
        self._sync_all_group_check_states()
        self.model.blockSignals(False)
        self._sync_selected_ids_from_visible_items()
        self.refresh_filter()
        return changed

    def set_source_rows_checked(self, source_rows: Iterable[object], checked: bool) -> int:
        expected = Qt.CheckState.Checked if checked else Qt.CheckState.Unchecked
        changed = 0
        self.model.blockSignals(True)
        for checkbox_item in self._resolve_leaf_checkbox_items(source_rows):
            if checkbox_item.checkState() == expected:
                continue
            checkbox_item.setCheckState(expected)
            changed += 1
        self._sync_all_group_check_states()
        self.model.blockSignals(False)
        self._sync_selected_ids_from_visible_items()
        if changed > 0:
            self.refresh_filter()
            self.selection_bulk_changed.emit()
        return changed

    def apply_filters(
        self,
        *,
        show_conflicts_only: bool,
        show_selected_only: bool,
        status_filter: str,
        search_text: str,
    ):
        preferred_candidate_id = self.current_candidate_id()
        self._apply_proxy_filters(
            show_conflicts_only=show_conflicts_only,
            show_selected_only=show_selected_only,
            status_filter=status_filter,
            search_text=search_text,
            preferred_candidate_id=preferred_candidate_id,
        )

    def _apply_proxy_filters(
        self,
        *,
        show_conflicts_only: bool,
        show_selected_only: bool,
        status_filter: str,
        search_text: str,
        preferred_candidate_id: str | None = None,
    ) -> None:
        self.proxy_model.set_filters(
            show_conflicts_only=show_conflicts_only,
            show_selected_only=show_selected_only,
            status_filter=status_filter,
            search_text=search_text,
        )
        self._update_visible_count()
        self._ensure_current_selection(preferred_candidate_id=preferred_candidate_id)

    def refresh_filter(self):
        self.apply_filters(
            show_conflicts_only=self.conflict_only_checkbox.isChecked(),
            show_selected_only=self.selected_only_checkbox.isChecked(),
            status_filter=self.current_status_filter(),
            search_text=self.search_edit.text(),
        )

    def get_visible_count(self) -> int:
        return len(self.get_filtered_candidate_ids())

    def current_status_filter(self) -> str:
        return str(self.status_filter_combo.currentData() or "ALL")

    def current_candidate_id(self) -> str | None:
        current = self.tree_view.currentIndex()
        if not current.isValid():
            return None
        return self._candidate_id_from_source_index(self._view_index_to_source(current))

    def update_summary(self, text: str):
        self.summary_label.setText(text)

    def update_detail_panel(
        self,
        candidate: SuiteCandidate | None,
        *,
        is_selected: bool = False,
        is_default_selected: bool = False,
    ):
        if candidate is None:
            self.clear_detail_panel()
            return

        self.detail_selected_value.setText("已勾选" if is_selected else "未勾选")
        self.detail_default_value.setText("是" if is_default_selected else "否")
        self.detail_suite_path_value.setText(candidate.suite_path)
        self.detail_file_name_value.setText(candidate.file_name)
        self.detail_file_path_value.setText(candidate.file_path)
        self.detail_test_names_value.setPlainText(
            "\n".join(candidate.test_names) if candidate.test_names else "—"
        )
        self.detail_failure_reasons_value.setPlainText(
            "\n\n".join(candidate.failure_reasons)
            if candidate.failure_reasons
            else "—"
        )
        self._set_status_badge(candidate.status)

        self.detail_start_time_value.setText(self._format_start_time(candidate))

    def clear_detail_panel(self):
        self.detail_selected_value.setText("—")
        self.detail_default_value.setText("—")
        self.detail_suite_path_value.setText("—")
        self.detail_file_name_value.setText("—")
        self.detail_file_path_value.setText("—")
        self.detail_test_names_value.setPlainText("—")
        self.detail_failure_reasons_value.setPlainText("—")
        self.detail_start_time_value.setText("—")
        self._set_status_badge("UNKNOWN")

    def _on_current_changed(self, current: QModelIndex, _previous: QModelIndex):
        if not current.isValid():
            self.focused_candidate_changed.emit(None)
            return
        source_index = self._view_index_to_source(current)
        if not source_index.isValid():
            self.focused_candidate_changed.emit(None)
            return

        checkbox_item = self._checkbox_item_from_index(source_index)
        if checkbox_item is None or not self._is_candidate_item(checkbox_item):
            self.focused_candidate_changed.emit(None)
            return

        candidate_id = checkbox_item.data(_CANDIDATE_ID_ROLE)
        self.focused_candidate_changed.emit(str(candidate_id) if candidate_id else None)

    def _show_tree_context_menu(self, position):
        viewport_pos = self._to_viewport_position(position)
        source_rows = self._selected_source_rows_for_menu(viewport_pos)
        candidate_id = self._candidate_id_at_viewport_position(viewport_pos)
        menu = self._build_tree_context_menu(source_rows, candidate_id)
        menu.exec(self.tree_view.viewport().mapToGlobal(viewport_pos))

    def _build_tree_context_menu(
        self,
        source_rows: Sequence[object],
        candidate_id: str | None,
    ) -> QMenu:
        menu = QMenu(self.tree_view)
        check_action = menu.addAction("勾选")
        uncheck_action = menu.addAction("反勾选")
        check_action.setEnabled(bool(source_rows) and self._actions_enabled)
        uncheck_action.setEnabled(bool(source_rows) and self._actions_enabled)
        check_action.triggered.connect(
            lambda: self.set_source_rows_checked(source_rows, True)
        )
        uncheck_action.triggered.connect(
            lambda: self.set_source_rows_checked(source_rows, False)
        )

        menu.addSeparator()
        select_all_action = menu.addAction("全选当前过滤")
        invert_action = menu.addAction("反选当前过滤")
        has_candidates = bool(self._current_candidates)
        select_all_action.setEnabled(has_candidates and self._actions_enabled)
        invert_action.setEnabled(has_candidates and self._actions_enabled)
        select_all_action.triggered.connect(self.select_all_filtered_requested.emit)
        invert_action.triggered.connect(self.invert_filtered_requested.emit)

        menu.addSeparator()
        detail_action = menu.addAction("查看详情")
        export_action = menu.addAction("导出该测试 XML")
        has_candidate = bool(candidate_id)
        detail_action.setEnabled(has_candidate and self._actions_enabled)
        export_action.setEnabled(has_candidate and self._actions_enabled)
        detail_action.triggered.connect(
            lambda: self._open_candidate_detail(candidate_id)
        )
        export_action.triggered.connect(
            lambda: self.candidate_export_requested.emit(candidate_id)
            if candidate_id
            else None
        )

        menu.addSeparator()
        open_output_action = menu.addAction("打开输出目录")
        open_output_action.setEnabled(self._actions_enabled)
        open_output_action.triggered.connect(self.open_output_dir_requested.emit)
        return menu

    def _candidate_id_at_viewport_position(self, position) -> str | None:
        proxy_index = self.tree_view.indexAt(position)
        if not proxy_index.isValid():
            return None
        source_index = self._view_index_to_source(proxy_index)
        checkbox_item = self._checkbox_item_from_index(source_index)
        if checkbox_item is None or not self._is_candidate_item(checkbox_item):
            return None
        candidate_id = checkbox_item.data(_CANDIDATE_ID_ROLE)
        return str(candidate_id) if candidate_id else None

    def _open_candidate_detail(self, candidate_id: str | None):
        if not candidate_id:
            return
        candidate_index = self._view_index_for_candidate(candidate_id)
        selection_model = self.tree_view.selectionModel()
        if candidate_index.isValid() and selection_model is not None:
            selection_model.setCurrentIndex(
                candidate_index,
                QItemSelectionModel.SelectionFlag.NoUpdate,
            )
        self.set_detail_visible(True)

    def _to_viewport_position(self, position):
        if self.tree_view.viewport().rect().contains(position):
            return position
        return self.tree_view.viewport().mapFrom(self.tree_view, position)

    def _selected_source_rows_for_menu(self, position) -> list[int]:
        selection_model = self.tree_view.selectionModel()
        if selection_model is None:
            return []

        clicked_proxy = self.tree_view.indexAt(position)
        if clicked_proxy.isValid():
            if not selection_model.isSelected(clicked_proxy):
                selection_model.select(
                    clicked_proxy,
                    QItemSelectionModel.SelectionFlag.ClearAndSelect
                    | QItemSelectionModel.SelectionFlag.Rows,
                )
                self.tree_view.setCurrentIndex(clicked_proxy)
        else:
            return []

        if self.is_tree_view_mode():
            candidate_ids: set[str] = set()
            for view_row_index in selection_model.selectedRows(COL_CHECK):
                source_index = self._view_index_to_source(view_row_index)
                candidate_ids.update(self._candidate_ids_from_source_index(source_index))
            return sorted(candidate_ids)

        source_rows: set[int] = set()
        for view_row_index in selection_model.selectedRows(COL_CHECK):
            source_index = self._view_index_to_source(view_row_index)
            if source_index.isValid():
                source_rows.add(source_index.row())
        return sorted(source_rows)

    def _on_tree_double_clicked(self, proxy_index: QModelIndex):
        if not proxy_index.isValid() or proxy_index.column() == COL_CHECK:
            return
        source_index = self._view_index_to_source(proxy_index)
        if not source_index.isValid():
            return
        checkbox_item = self._checkbox_item_from_index(source_index)
        if checkbox_item is None or not self._is_candidate_item(checkbox_item):
            return
        candidate_id = checkbox_item.data(_CANDIDATE_ID_ROLE)
        if candidate_id:
            self.candidate_double_clicked.emit(str(candidate_id))

    def _ensure_current_selection(
        self, *, preferred_candidate_id: str | None = None
    ) -> None:
        view_model = self._active_view_model()
        if view_model is None or view_model.rowCount() == 0:
            self.tree_view.setCurrentIndex(QModelIndex())
            self.tree_view.clearSelection()
            self.focused_candidate_changed.emit(None)
            return

        if preferred_candidate_id:
            preferred_index = self._view_index_for_candidate(preferred_candidate_id)
            if preferred_index.isValid():
                if self.tree_view.currentIndex() != preferred_index:
                    self.tree_view.setCurrentIndex(preferred_index)
                    self.tree_view.scrollTo(
                        preferred_index,
                        QAbstractItemView.ScrollHint.PositionAtCenter,
                    )
                return

        current = self.tree_view.currentIndex()
        if current.isValid():
            source_index = self._view_index_to_source(current)
            if source_index.isValid():
                return

        first_index = self._first_visible_candidate_index()
        if first_index.isValid():
            self.tree_view.setCurrentIndex(first_index)
            self.tree_view.scrollTo(
                first_index, QAbstractItemView.ScrollHint.PositionAtCenter
            )

    def _update_visible_count(self):
        self.visible_count_label.setText(f"可见: {self.get_visible_count()}")

    def _configure_header(self):
        header = self.tree_view.header()
        header.setStretchLastSection(False)
        header.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        for column in range(self.model.columnCount()):
            header.setSectionResizeMode(column, QHeaderView.ResizeMode.Interactive)
        header.customContextMenuRequested.connect(self._show_header_menu)
        header.sectionResized.connect(self._on_header_resized)
        header.sectionMoved.connect(self._on_header_moved)

    def _apply_default_column_widths(self):
        for column, width in self._default_column_widths().items():
            self.tree_view.setColumnWidth(column, width)

    @staticmethod
    def _default_column_widths() -> dict[int, int]:
        return {
            COL_CHECK: 62,
            COL_SUITE_PATH: 520,
            COL_STATUS: 96,
            COL_DEFAULT: 96,
            COL_CONFLICT_SIZE: 120,
            COL_TIME: 178,
            COL_FILE: 240,
        }

    def _show_header_menu(self, position):
        header = self.tree_view.header()
        menu = QMenu(header)
        for column in range(self.model.columnCount()):
            if column == COL_CHECK:
                continue
            title = str(
                self.model.headerData(
                    column, Qt.Orientation.Horizontal, Qt.ItemDataRole.DisplayRole
                )
                or f"列 {column}"
            )
            action = menu.addAction(title)
            action.setCheckable(True)
            action.setChecked(not self.tree_view.isColumnHidden(column))
            if self.is_tree_view_mode() and column == COL_SUITE_PATH:
                action.setEnabled(False)
            action.toggled.connect(
                lambda checked, target=column: self._set_column_visible(target, checked)
            )
        menu.exec(header.mapToGlobal(position))

    def _set_column_visible(self, column: int, visible: bool):
        self.tree_view.setColumnHidden(column, not visible)
        if visible and self.tree_view.columnWidth(column) <= 1:
            self.tree_view.setColumnWidth(
                column, self._default_column_widths().get(column, 120)
            )
        self._save_header_preferences()

    def _save_header_preferences(self):
        state = self.tree_view.header().saveState()
        key = self._header_state_key()
        self._settings.setValue(key, state)
        self._settings.setValue(
            self._visible_columns_key(),
            [column for column in range(self.model.columnCount()) if not self.tree_view.isColumnHidden(column)],
        )
        if self._active_view_mode == _VIEW_MODE_FLAT:
            self._settings.setValue(_SETTINGS_HEADER_STATE, state)

    def _load_header_preferences(self):
        state = self._settings.value(self._header_state_key())
        if state is None and self._active_view_mode == _VIEW_MODE_FLAT:
            state = self._settings.value(_SETTINGS_HEADER_STATE)
        restored = False
        self._header_restore_in_progress = True
        try:
            if state is not None:
                restored = bool(self.tree_view.header().restoreState(state))
            self._load_column_visibility_preferences()
        finally:
            self._header_restore_in_progress = False
        return restored

    def _on_header_resized(self, *_):
        if self._header_restore_in_progress:
            return
        self._save_header_preferences()

    def _on_header_moved(self, *_):
        if self._header_restore_in_progress:
            return
        self._save_header_preferences()

    def _save_detail_preferences(self):
        if self.detail_dialog is None:
            return
        self._settings.setValue(
            _SETTINGS_DETAIL_DIALOG_GEOMETRY,
            self.detail_dialog.saveGeometry(),
        )

    def _save_report_options_preferences(self):
        if self.report_options_dialog is None:
            return
        self._settings.setValue(
            _SETTINGS_REPORT_OPTIONS_DIALOG_GEOMETRY,
            self.report_options_dialog.saveGeometry(),
        )

    def _load_preferences(self):
        stored_view_mode = str(
            self._settings.value(_SETTINGS_VIEW_MODE, _VIEW_MODE_FLAT) or _VIEW_MODE_FLAT
        )
        if stored_view_mode not in {_VIEW_MODE_FLAT, _VIEW_MODE_TREE}:
            stored_view_mode = _VIEW_MODE_FLAT

        with QSignalBlocker(self.view_mode_combo):
            self.view_mode_combo.setCurrentIndex(self.view_mode_combo.findData(stored_view_mode))

        self._apply_view_mode(
            stored_view_mode,
            persist=False,
            rebuild=False,
            save_current=False,
        )
        self._detail_geometry_restored = self._restore_dialog_geometry(
            self.detail_dialog,
            _SETTINGS_DETAIL_DIALOG_GEOMETRY,
        )
        self._report_options_geometry_restored = self._restore_dialog_geometry(
            self.report_options_dialog,
            _SETTINGS_REPORT_OPTIONS_DIALOG_GEOMETRY,
        )

    @staticmethod
    def _parse_splitter_sizes(value) -> list[int]:
        if value is None:
            return []
        if isinstance(value, str):
            parts = [part.strip() for part in value.strip("[]").split(",") if part.strip()]
        elif isinstance(value, (list, tuple)):
            parts = list(value)
        else:
            return []

        parsed: list[int] = []
        for part in parts:
            try:
                parsed.append(int(part))
            except (TypeError, ValueError):
                continue
        return parsed if len(parsed) == 2 and sum(parsed) > 0 else []

    def _restore_dialog_geometry(self, dialog: QDialog, key: str) -> bool:
        geometry = self._settings.value(key)
        if geometry is None:
            return False
        try:
            return bool(dialog.restoreGeometry(geometry))
        except TypeError:
            return False

    def set_detail_visible(self, visible: bool, *, persist: bool = True):
        if visible:
            if not self._detail_geometry_restored:
                self._position_detail_dialog()
            self.detail_dialog.show()
            self.detail_dialog.raise_()
            self.detail_dialog.activateWindow()
        else:
            self.detail_dialog.hide()

        self._detail_visible = visible
        if persist:
            self._save_detail_preferences()

    def toggle_detail_visible(self):
        self.set_detail_visible(not self._detail_visible)

    def open_report_options_dialog(self):
        if not self._report_options_geometry_restored:
            self._position_report_options_dialog()
        self.report_options_dialog.show()
        self.report_options_dialog.raise_()
        self.report_options_dialog.activateWindow()

    def _on_detail_dialog_visibility_changed(self, visible: bool):
        self._detail_visible = visible
        if visible:
            self._detail_geometry_restored = True
            return
        self._save_detail_preferences()

    def _on_report_options_dialog_visibility_changed(self, visible: bool):
        if visible:
            self._report_options_geometry_restored = True
            return
        self._save_report_options_preferences()

    def _position_detail_dialog(self):
        self._position_dialog_near_window(self.detail_dialog, top_offset=48)
        self._detail_geometry_restored = True

    def _position_report_options_dialog(self):
        self._position_dialog_near_window(self.report_options_dialog, top_offset=72)
        self._report_options_geometry_restored = True

    def _position_dialog_near_window(self, dialog: QDialog, *, top_offset: int):
        dialog_width = dialog.width()
        dialog_height = dialog.height()
        origin = self.frameGeometry()
        target_x = origin.right() - dialog_width
        target_y = origin.top() + top_offset

        screen = self.screen() or QApplication.primaryScreen()
        if screen is not None:
            available = screen.availableGeometry()
            target_x = max(
                available.left() + 24,
                min(target_x, available.right() - dialog_width - 24),
            )
            target_y = max(
                available.top() + 24,
                min(target_y, available.bottom() - dialog_height - 24),
            )

        dialog.move(target_x, target_y)

    def closeEvent(self, event):
        self._save_header_preferences()
        self._save_detail_preferences()
        self._save_report_options_preferences()
        self._settings.sync()
        self.report_options_dialog.close()
        self.detail_dialog.close()
        super().closeEvent(event)

    def _set_compact_mode(self, compact: bool):
        self.tree_view.setProperty("compact", compact)
        self.tree_view.style().unpolish(self.tree_view)
        self.tree_view.style().polish(self.tree_view)
        self.tree_view.viewport().update()

    def current_view_mode(self) -> str:
        view_mode = str(self.view_mode_combo.currentData() or _VIEW_MODE_FLAT)
        return view_mode if view_mode in {_VIEW_MODE_FLAT, _VIEW_MODE_TREE} else _VIEW_MODE_FLAT

    def is_tree_view_mode(self) -> bool:
        return self._active_view_mode == _VIEW_MODE_TREE

    def _on_view_mode_changed(self, _index: int):
        self._apply_view_mode(self.current_view_mode(), persist=True, rebuild=True)

    def _apply_view_mode(
        self,
        view_mode: str,
        *,
        persist: bool,
        rebuild: bool,
        save_current: bool = True,
    ) -> None:
        normalized_mode = (
            view_mode if view_mode in {_VIEW_MODE_FLAT, _VIEW_MODE_TREE} else _VIEW_MODE_FLAT
        )
        preferred_candidate_id = self.current_candidate_id()
        if save_current:
            self._save_header_preferences()

        self._active_view_mode = normalized_mode
        self._header_restore_in_progress = True
        try:
            self._configure_tree_for_view_mode()
            self._restore_default_column_visibility()
            self._apply_default_column_widths()
            self._load_header_preferences()
        finally:
            self._header_restore_in_progress = False

        if persist:
            self._settings.setValue(_SETTINGS_VIEW_MODE, normalized_mode)
            self._settings.sync()

        if rebuild:
            self._rebuild_candidate_model(
                preferred_candidate_id=preferred_candidate_id
            )
        elif self.is_tree_view_mode():
            self.tree_view.expandAll()
            self._update_visible_count()
            self._ensure_current_selection(
                preferred_candidate_id=preferred_candidate_id
            )

    def _configure_tree_for_view_mode(self) -> None:
        is_tree = self.is_tree_view_mode()
        self._apply_view_model_for_mode()
        self.tree_view.setRootIsDecorated(is_tree)
        self.tree_view.setItemsExpandable(is_tree)
        self.tree_view.setExpandsOnDoubleClick(is_tree)
        self.proxy_model.setRecursiveFilteringEnabled(False)
        self.proxy_model.setAutoAcceptChildRows(False)
        self.tree_view.setSortingEnabled(not is_tree)
        if not is_tree:
            self.tree_view.sortByColumn(COL_SUITE_PATH, Qt.SortOrder.AscendingOrder)

    def _header_state_key(self) -> str:
        if self._active_view_mode == _VIEW_MODE_TREE:
            return _SETTINGS_HEADER_STATE_TREE
        return _SETTINGS_HEADER_STATE_FLAT

    def _visible_columns_key(self) -> str:
        if self._active_view_mode == _VIEW_MODE_TREE:
            return _SETTINGS_VISIBLE_COLUMNS_TREE
        return _SETTINGS_VISIBLE_COLUMNS_FLAT

    def _restore_default_column_visibility(self) -> None:
        for column in range(self.model.columnCount()):
            self.tree_view.setColumnHidden(
                column,
                column == COL_CONFLICT_SIZE,
            )

    def _load_column_visibility_preferences(self) -> None:
        visible_columns = self._settings.value(self._visible_columns_key())
        if visible_columns is None:
            return

        if isinstance(visible_columns, str):
            parts = [part.strip() for part in visible_columns.strip("[]").split(",") if part.strip()]
        elif isinstance(visible_columns, (list, tuple)):
            parts = list(visible_columns)
        else:
            return

        parsed: set[int] = set()
        for part in parts:
            try:
                parsed.add(int(part))
            except (TypeError, ValueError):
                continue

        if self.is_tree_view_mode():
            parsed.add(COL_SUITE_PATH)

        for column in range(self.model.columnCount()):
            self.tree_view.setColumnHidden(column, column not in parsed)

    def _rebuild_candidate_model(
        self,
        *,
        selected_ids: set[str] | None = None,
        preferred_candidate_id: str | None = None,
    ) -> None:
        if selected_ids is None:
            selected_ids = set(self._selected_candidate_ids)
        if preferred_candidate_id is None:
            preferred_candidate_id = self.current_candidate_id()

        self._model_rebuild_in_progress = True
        self._checkbox_item_by_candidate_id.clear()
        try:
            selection_model = self.tree_view.selectionModel()
            if selection_model is not None:
                selection_model.clear()

            # Structural model signals must reach the proxy and selection model.
            # Blocking them leaves both objects holding indexes for deleted rows.
            self.model.removeRows(0, self.model.rowCount())

            if self._current_candidates:
                if self.is_tree_view_mode():
                    self._populate_tree_candidates(selected_ids)
                    self.model.blockSignals(True)
                    try:
                        self._sync_all_group_check_states()
                    finally:
                        self.model.blockSignals(False)
                else:
                    self._populate_flat_candidates(selected_ids)

            self._apply_proxy_filters(
                show_conflicts_only=self.conflict_only_checkbox.isChecked(),
                show_selected_only=self.selected_only_checkbox.isChecked(),
                status_filter=self.current_status_filter(),
                search_text=self.search_edit.text(),
                preferred_candidate_id=preferred_candidate_id,
            )
            if self.is_tree_view_mode():
                self.tree_view.expandAll()
        finally:
            self._model_rebuild_in_progress = False

    def _populate_flat_candidates(self, selected_ids: set[str]) -> None:
        root_item = self.model.invisibleRootItem()
        for candidate in self._current_candidates:
            conflict_size = int(self._conflict_group_size_by_suite.get(candidate.suite_path, 1))
            is_default = candidate.candidate_id in self._default_selected_ids
            is_selected = candidate.candidate_id in selected_ids
            row_items = self._candidate_row_items(
                candidate,
                is_selected=is_selected,
                is_default=is_default,
                conflict_size=conflict_size,
            )
            root_item.appendRow(row_items)
            self._checkbox_item_by_candidate_id[candidate.candidate_id] = row_items[COL_CHECK]

    def _populate_tree_candidates(self, selected_ids: set[str]) -> None:
        root_item = self.model.invisibleRootItem()
        group_counts: dict[tuple[str, ...], int] = {}
        for candidate in self._current_candidates:
            path_parts = candidate.suite_path_parts or (candidate.suite_path,)
            for depth in range(1, len(path_parts) + 1):
                key = tuple(path_parts[:depth])
                group_counts[key] = group_counts.get(key, 0) + 1

        group_items: dict[tuple[str, ...], QStandardItem] = {}
        sorted_candidates = sorted(
            self._current_candidates,
            key=lambda item: (
                tuple(part.casefold() for part in item.suite_path_parts),
                item.file_name.casefold(),
                item.candidate_id,
            ),
        )
        for candidate in sorted_candidates:
            parent_item = root_item
            path_parts = candidate.suite_path_parts or (candidate.suite_path,)
            for depth, part in enumerate(path_parts, start=1):
                key = tuple(path_parts[:depth])
                group_check_item = group_items.get(key)
                if group_check_item is None:
                    row_items = self._group_row_items(
                        label=part,
                        full_path=" / ".join(key),
                        leaf_count=group_counts.get(key, 0),
                    )
                    parent_item.appendRow(row_items)
                    group_check_item = row_items[COL_CHECK]
                    group_items[key] = group_check_item
                parent_item = group_check_item

            conflict_size = int(self._conflict_group_size_by_suite.get(candidate.suite_path, 1))
            is_default = candidate.candidate_id in self._default_selected_ids
            is_selected = candidate.candidate_id in selected_ids
            row_items = self._candidate_row_items(
                candidate,
                is_selected=is_selected,
                is_default=is_default,
                conflict_size=conflict_size,
            )
            parent_item.appendRow(row_items)
            self._checkbox_item_by_candidate_id[candidate.candidate_id] = row_items[COL_CHECK]

    def _candidate_row_items(
        self,
        candidate: SuiteCandidate,
        *,
        is_selected: bool,
        is_default: bool,
        conflict_size: int,
    ) -> list[QStandardItem]:
        center = Qt.AlignmentFlag.AlignCenter

        checkbox_item = QStandardItem()
        checkbox_item.setEditable(False)
        checkbox_item.setCheckable(True)
        checkbox_item.setData(candidate.candidate_id, _CANDIDATE_ID_ROLE)
        checkbox_item.setData(_NODE_KIND_CANDIDATE, _NODE_KIND_ROLE)
        checkbox_item.setCheckState(
            Qt.CheckState.Checked if is_selected else Qt.CheckState.Unchecked
        )

        path_item = QStandardItem(candidate.suite_path)
        path_item.setEditable(False)
        path_item.setToolTip(candidate.suite_path)
        path_item.setData(candidate.suite_path, _FULL_PATH_ROLE)

        status_item = QStandardItem(_normalize_status(candidate.status))
        status_item.setEditable(False)
        status_item.setTextAlignment(center)
        status_item.setForeground(QColor(self._status_color(candidate.status)))

        default_item = QStandardItem("是" if is_default else "否")
        default_item.setEditable(False)
        default_item.setTextAlignment(center)

        conflict_item = QStandardItem(str(conflict_size))
        conflict_item.setEditable(False)
        conflict_item.setTextAlignment(center)

        time_item = QStandardItem(self._format_start_time(candidate))
        time_item.setEditable(False)

        file_item = QStandardItem(candidate.file_name)
        file_item.setEditable(False)
        file_item.setToolTip(candidate.file_path)

        row_items = [
            checkbox_item,
            path_item,
            status_item,
            default_item,
            conflict_item,
            time_item,
            file_item,
        ]

        if conflict_size > 1:
            conflict_bg = QColor(PALETTE["row_conflict_bg"])
            for row_item in row_items[1:]:
                row_item.setBackground(conflict_bg)

        return row_items

    def _group_row_items(
        self,
        *,
        label: str,
        full_path: str,
        leaf_count: int,
    ) -> list[QStandardItem]:
        checkbox_item = QStandardItem()
        checkbox_item.setEditable(False)
        checkbox_item.setCheckable(True)
        checkbox_item.setUserTristate(True)
        checkbox_item.setData(_NODE_KIND_GROUP, _NODE_KIND_ROLE)
        checkbox_item.setCheckState(Qt.CheckState.Unchecked)

        path_item = QStandardItem(f"{label} ({leaf_count})")
        path_item.setEditable(False)
        path_item.setToolTip(full_path)
        path_item.setData(full_path, _FULL_PATH_ROLE)

        row_items = [checkbox_item, path_item]
        for _ in range(2, self.model.columnCount()):
            empty_item = QStandardItem("")
            empty_item.setEditable(False)
            row_items.append(empty_item)
        return row_items

    def _on_model_item_changed(self, item: QStandardItem) -> None:
        if self._check_sync_in_progress or item.column() != COL_CHECK:
            return
        if not self.is_tree_view_mode():
            if self._is_candidate_item(item):
                self._sync_selected_ids_from_visible_items()
            return
        if not self._is_candidate_item(item) and not self._is_group_item(item):
            return

        self._check_sync_in_progress = True
        self.model.blockSignals(True)
        try:
            if self._is_group_item(item):
                target_checked = item.checkState() != Qt.CheckState.Unchecked
                self._set_descendant_check_state(item, target_checked)
            self._sync_all_group_check_states()
        finally:
            self.model.blockSignals(False)
            self._check_sync_in_progress = False

        if self._is_group_item(item):
            self._sync_selected_ids_from_visible_items()
            self.selection_bulk_changed.emit()
            return

        self._sync_selected_ids_from_visible_items()

    def _set_descendant_check_state(self, parent_item: QStandardItem, checked: bool) -> None:
        expected = Qt.CheckState.Checked if checked else Qt.CheckState.Unchecked
        for child_item in self._iter_child_check_items(parent_item):
            child_item.setCheckState(expected)
            if child_item.rowCount() > 0:
                self._set_descendant_check_state(child_item, checked)

    def _sync_all_group_check_states(self) -> None:
        root_item = self.model.invisibleRootItem()
        for row in range(root_item.rowCount()):
            check_item = root_item.child(row, COL_CHECK)
            if check_item is not None:
                self._sync_group_check_state(check_item)

    def _sync_group_check_state(self, item: QStandardItem) -> tuple[int, int]:
        if self._is_candidate_item(item):
            is_checked = item.checkState() == Qt.CheckState.Checked
            return (1 if is_checked else 0, 1)

        checked_count = 0
        total_count = 0
        for child_item in self._iter_child_check_items(item):
            child_checked, child_total = self._sync_group_check_state(child_item)
            checked_count += child_checked
            total_count += child_total

        if self._is_group_item(item):
            if total_count == 0 or checked_count == 0:
                item.setCheckState(Qt.CheckState.Unchecked)
            elif checked_count == total_count:
                item.setCheckState(Qt.CheckState.Checked)
            else:
                item.setCheckState(Qt.CheckState.PartiallyChecked)
        return checked_count, total_count

    def _resolve_leaf_checkbox_items(self, targets: Iterable[object]) -> list[QStandardItem]:
        resolved: list[QStandardItem] = []
        seen: set[int] = set()
        for target in targets:
            for checkbox_item in self._leaf_checkbox_items_for_target(target):
                identity = id(checkbox_item)
                if identity in seen:
                    continue
                seen.add(identity)
                resolved.append(checkbox_item)
        return resolved

    def _leaf_checkbox_items_for_target(self, target: object) -> list[QStandardItem]:
        if isinstance(target, str):
            checkbox_item = self._checkbox_item_by_candidate_id.get(target)
            return [checkbox_item] if checkbox_item is not None else []

        if isinstance(target, QModelIndex):
            if not target.isValid():
                return []
            return [
                self._checkbox_item_by_candidate_id[candidate_id]
                for candidate_id in self._candidate_ids_from_source_index(target)
                if candidate_id in self._checkbox_item_by_candidate_id
            ]

        if not isinstance(target, int):
            return []
        if target < 0 or target >= self.model.rowCount():
            return []

        checkbox_item = self.model.invisibleRootItem().child(target, COL_CHECK)
        if checkbox_item is None:
            return []
        if self._is_candidate_item(checkbox_item):
            return [checkbox_item]
        return self._descendant_candidate_checkbox_items(checkbox_item)

    def _candidate_ids_from_source_index(self, source_index: QModelIndex) -> list[str]:
        checkbox_item = self._checkbox_item_from_index(source_index)
        if checkbox_item is None:
            return []
        if self._is_candidate_item(checkbox_item):
            candidate_id = checkbox_item.data(_CANDIDATE_ID_ROLE)
            return [str(candidate_id)] if candidate_id else []
        return [
            str(candidate_id)
            for candidate_id in (
                child_item.data(_CANDIDATE_ID_ROLE)
                for child_item in self._descendant_candidate_checkbox_items(checkbox_item)
            )
            if candidate_id
        ]

    def _descendant_candidate_checkbox_items(self, parent_item: QStandardItem) -> list[QStandardItem]:
        items: list[QStandardItem] = []
        for child_item in self._iter_child_check_items(parent_item):
            if self._is_candidate_item(child_item):
                items.append(child_item)
            else:
                items.extend(self._descendant_candidate_checkbox_items(child_item))
        return items

    def _iter_child_check_items(self, parent_item: QStandardItem):
        for row in range(parent_item.rowCount()):
            child_item = parent_item.child(row, COL_CHECK)
            if child_item is not None:
                yield child_item

    def _sync_selected_ids_from_visible_items(self) -> None:
        for candidate_id, checkbox_item in self._checkbox_item_by_candidate_id.items():
            if checkbox_item.checkState() == Qt.CheckState.Checked:
                self._selected_candidate_ids.add(candidate_id)
            else:
                self._selected_candidate_ids.discard(candidate_id)

    def _iter_view_leaf_check_indexes(self, parent: QModelIndex = QModelIndex()):
        view_model = self._active_view_model()
        if view_model is None:
            return

        for row in range(view_model.rowCount(parent)):
            check_index = view_model.index(row, COL_CHECK, parent)
            if not check_index.isValid():
                continue
            source_index = self._view_index_to_source(check_index)
            checkbox_item = self._checkbox_item_from_index(source_index)
            if checkbox_item is None:
                continue
            if self._is_candidate_item(checkbox_item):
                yield check_index
                continue
            yield from self._iter_view_leaf_check_indexes(check_index)

    def _first_visible_candidate_index(self, parent: QModelIndex = QModelIndex()) -> QModelIndex:
        del parent
        for check_index in self._iter_view_leaf_check_indexes():
            return check_index.siblingAtColumn(COL_SUITE_PATH)
        return QModelIndex()

    def _checkbox_item_from_index(self, index: QModelIndex) -> QStandardItem | None:
        if not index.isValid():
            return None
        check_index = index.siblingAtColumn(COL_CHECK)
        if not check_index.isValid():
            return None
        return self.model.itemFromIndex(check_index)

    @staticmethod
    def _is_candidate_item(item: QStandardItem | None) -> bool:
        if item is None:
            return False
        node_kind = item.data(_NODE_KIND_ROLE)
        return not node_kind or str(node_kind) == _NODE_KIND_CANDIDATE

    @staticmethod
    def _is_group_item(item: QStandardItem | None) -> bool:
        if item is None:
            return False
        return str(item.data(_NODE_KIND_ROLE) or "") == _NODE_KIND_GROUP

    def _build_detail_panel(self) -> QFrame:
        panel = QFrame()
        panel.setObjectName("DetailPanel")
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)

        self.detail_status_badge = QLabel("UNKNOWN")
        self.detail_status_badge.setObjectName("StatusBadge")
        self._set_status_badge("UNKNOWN")

        form = QFormLayout()
        form.setLabelAlignment(
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter
        )
        form.setFormAlignment(Qt.AlignmentFlag.AlignTop)
        form.setHorizontalSpacing(10)
        form.setVerticalSpacing(8)

        self.detail_selected_value = self._value_label()
        self.detail_default_value = self._value_label()
        self.detail_suite_path_value = self._value_label(word_wrap=True)
        self.detail_file_name_value = self._value_label()
        self.detail_file_path_value = self._value_label(word_wrap=True)
        self.detail_start_time_value = self._value_label()
        self.detail_test_names_value = self._detail_text_view(maximum_height=100)
        self.detail_failure_reasons_value = self._detail_text_view(maximum_height=140)

        form.addRow("当前勾选", self.detail_selected_value)
        form.addRow("默认推荐", self.detail_default_value)
        form.addRow("状态", self.detail_status_badge)
        form.addRow("套件路径", self.detail_suite_path_value)
        form.addRow("文件名", self.detail_file_name_value)
        form.addRow("文件路径", self.detail_file_path_value)
        form.addRow("测试名称", self.detail_test_names_value)
        form.addRow("失败原因", self.detail_failure_reasons_value)
        form.addRow("开始时间", self.detail_start_time_value)

        layout.addLayout(form)
        layout.addStretch(1)
        return panel

    @staticmethod
    def _value_label(*, word_wrap: bool = False) -> QLabel:
        label = QLabel("—")
        label.setWordWrap(word_wrap)
        label.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        return label

    @staticmethod
    def _detail_text_view(*, maximum_height: int) -> QPlainTextEdit:
        editor = QPlainTextEdit("—")
        editor.setReadOnly(True)
        editor.setMaximumHeight(maximum_height)
        editor.setMinimumHeight(56)
        return editor

    def _set_status_badge(self, status: str):
        normalized = _normalize_status(status)
        badge_status = normalized if normalized in {"PASS", "SKIP", "FAIL"} else "UNKNOWN"
        self.detail_status_badge.setProperty("status", badge_status)
        self.detail_status_badge.setText(badge_status)
        self.detail_status_badge.style().unpolish(self.detail_status_badge)
        self.detail_status_badge.style().polish(self.detail_status_badge)

    @staticmethod
    def _format_start_time(candidate: SuiteCandidate) -> str:
        parsed = MainWindow._parse_robot_time(candidate.suite_starttime)
        if parsed is None:
            return "—"
        return parsed.strftime("%Y-%m-%d %H:%M:%S")

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

    @staticmethod
    def _status_color(status: str) -> str:
        normalized = _normalize_status(status)
        if normalized == "PASS":
            return PALETTE["pass"]
        if normalized == "SKIP":
            return PALETTE["skip"]
        if normalized == "FAIL":
            return PALETTE["fail"]
        return "#394150"
