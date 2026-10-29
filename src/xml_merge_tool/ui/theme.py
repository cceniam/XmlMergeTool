"""PyQt6 UI theme tokens and stylesheet."""

from __future__ import annotations

PALETTE = {
    "bg": "#f3f5f8",
    "panel": "#ffffff",
    "panel_alt": "#eef2f7",
    "text": "#111827",
    "muted_text": "#4b5563",
    "border": "#c7d0db",
    "focus": "#0b5fff",
    "focus_dark": "#0848be",
    "danger": "#c62828",
    "warn": "#b06f00",
    "pass": "#0f8f4f",
    "skip": "#b06f00",
    "fail": "#c62828",
    "chip_pass_bg": "#dff5e9",
    "chip_skip_bg": "#fff2dd",
    "chip_fail_bg": "#fde7e7",
    "chip_unknown_bg": "#e7ecf2",
    "row_conflict_bg": "#fff8ea",
}

APP_STYLESHEET = f"""
QMainWindow {{
    background: {PALETTE["bg"]};
}}

QWidget {{
    color: {PALETTE["text"]};
    font-family: "Microsoft YaHei UI", "Segoe UI", "Noto Sans CJK SC", sans-serif;
}}

QDialog#DetailDialog {{
    background: {PALETTE["bg"]};
}}

QDialog#ReportOptionsDialog {{
    background: {PALETTE["bg"]};
}}

QLabel#SummaryLabel {{
    color: {PALETTE["text"]};
    font-weight: 600;
    font-size: 13px;
}}

QLabel#MutedText {{
    color: {PALETTE["muted_text"]};
}}

QLabel#SectionTitle {{
    font-size: 14px;
    font-weight: 700;
}}

QLabel#HeroTitle {{
    font-size: 18px;
    font-weight: 700;
}}

QLabel#HeroSubtitle {{
    color: {PALETTE["muted_text"]};
    font-size: 12px;
}}

QLabel#FieldLabel {{
    color: {PALETTE["muted_text"]};
    font-weight: 600;
}}

QLabel#StatusBadge {{
    border: 1px solid {PALETTE["border"]};
    border-radius: 10px;
    padding: 3px 10px;
    font-weight: 700;
    min-width: 72px;
}}

QLabel#StatusBadge[status="PASS"] {{
    color: {PALETTE["pass"]};
    background: {PALETTE["chip_pass_bg"]};
    border-color: #7bc89d;
}}

QLabel#StatusBadge[status="SKIP"] {{
    color: {PALETTE["skip"]};
    background: {PALETTE["chip_skip_bg"]};
    border-color: #e0b56a;
}}

QLabel#StatusBadge[status="FAIL"] {{
    color: {PALETTE["fail"]};
    background: {PALETTE["chip_fail_bg"]};
    border-color: #e08b8b;
}}

QLabel#StatusBadge[status="UNKNOWN"] {{
    color: {PALETTE["muted_text"]};
    background: {PALETTE["chip_unknown_bg"]};
}}

QFrame#HeroCard {{
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #ffffff, stop:1 #f1f5fb);
    border: 1px solid #d6deea;
    border-radius: 14px;
}}

QLabel#HeroLogo {{
    background: rgba(255, 255, 255, 0.88);
    border: 1px solid #d6deea;
    border-radius: 12px;
}}

QFrame#CommandCard,
QFrame#WorkspaceCard,
QFrame#ToolbarCard,
QFrame#ReportOptionsPanel,
QFrame#DetailPanel {{
    background: {PALETTE["panel"]};
    border: 1px solid {PALETTE["border"]};
    border-radius: 12px;
}}

QFrame#ToolbarGroup,
QFrame#ToolbarBulkGroup,
QFrame#FooterFilterGroup {{
    background: {PALETTE["panel_alt"]};
    border: 1px solid #d8e0ea;
    border-radius: 10px;
}}

QWidget#HeaderToolGroup,
QWidget#CommandSecondaryRow {{
    background: transparent;
}}

QPushButton {{
    background: {PALETTE["panel"]};
    border: 1px solid {PALETTE["border"]};
    border-radius: 8px;
    padding: 6px 12px;
    min-height: 18px;
}}

QPushButton:hover {{
    border-color: {PALETTE["focus"]};
}}

QPushButton:pressed {{
    background: {PALETTE["panel_alt"]};
}}

QPushButton#PrimaryButton {{
    background: {PALETTE["focus"]};
    color: #ffffff;
    border-color: {PALETTE["focus_dark"]};
    font-weight: 700;
}}

QPushButton#PrimaryButton:hover {{
    background: {PALETTE["focus_dark"]};
}}

QPushButton#DangerButton {{
    color: {PALETTE["danger"]};
}}

QPushButton:disabled,
QComboBox:disabled,
QLineEdit:disabled,
QCheckBox:disabled,
QSpinBox:disabled {{
    color: #8f97a3;
}}

QComboBox,
QLineEdit,
QSpinBox {{
    background: #fbfcfe;
    border: 1px solid {PALETTE["border"]};
    border-radius: 8px;
    padding: 5px 8px;
}}

QComboBox:focus,
QLineEdit:focus,
QSpinBox:focus {{
    border-color: {PALETTE["focus"]};
}}

QCheckBox {{
    spacing: 6px;
}}

QTreeView {{
    background: #fbfcff;
    border: 1px solid {PALETTE["border"]};
    border-radius: 12px;
    alternate-background-color: #f8fafc;
    selection-background-color: #d9e8ff;
    selection-color: {PALETTE["text"]};
    show-decoration-selected: 1;
}}

QTreeView::item {{
    padding: 6px 8px;
    border-bottom: 1px solid #edf1f5;
}}

QTreeView::item:selected,
QTreeView::item:selected:active,
QTreeView::item:selected:!active {{
    background: #d9e8ff;
    color: {PALETTE["text"]};
}}

QTreeView[compact="true"]::item {{
    padding: 2px 6px;
}}

QHeaderView::section {{
    background: #e6ecf3;
    color: #1f2937;
    border: 0px;
    border-right: 1px solid #d3dbe6;
    border-bottom: 1px solid #d3dbe6;
    padding: 8px 8px;
    font-weight: 700;
}}

QProgressBar {{
    border: 1px solid {PALETTE["border"]};
    border-radius: 8px;
    background: #edf1f6;
    text-align: center;
    min-height: 16px;
    min-width: 150px;
}}

QProgressBar::chunk {{
    background: {PALETTE["focus"]};
    border-radius: 8px;
}}
"""
