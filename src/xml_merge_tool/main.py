"""PyQt6 应用程序入口。"""

from __future__ import annotations

import logging
import os
import platform
import signal
import sys
from pathlib import Path

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QFont, QFontDatabase
from PyQt6.QtWidgets import QApplication

# 允许以脚本方式运行。
if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from xml_merge_tool.ui.controller import MainController


_MACOS_ACCESSIBILITY_OPT_IN = "ROBOT_XML_MERGE_ENABLE_MACOS_ACCESSIBILITY"


def _configure_logging():
    log_path = Path("debug.log")
    if sys.platform == "darwin" and getattr(sys, "frozen", False):
        log_dir = Path.home() / "Library" / "Logs" / "XmlMergeTool"
        log_dir.mkdir(parents=True, exist_ok=True)
        log_path = log_dir / "debug.log"
    logging.basicConfig(
        filename=log_path,
        level=logging.INFO,
        format="%(asctime)s - %(levelname)s - %(message)s",
    )


def _configure_hidpi():
    os.environ.setdefault("QT_ENABLE_HIGHDPI_SCALING", "1")
    os.environ.setdefault("QT_AUTO_SCREEN_SCALE_FACTOR", "1")
    os.environ.setdefault("QT_SCALE_FACTOR_ROUNDING_POLICY", "RoundPreferFloor")
    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.RoundPreferFloor
    )


def _configure_app_font(app: QApplication):
    preferred_families = [
        "Microsoft YaHei UI",
        "Segoe UI",
        "Noto Sans CJK SC",
        "Microsoft YaHei",
    ]
    available_families = set(QFontDatabase.families())

    selected_family = next(
        (family for family in preferred_families if family in available_families),
        app.font().family(),
    )

    font = QFont(selected_family, 10)
    font.setHintingPreference(QFont.HintingPreference.PreferFullHinting)
    font.setStyleStrategy(
        QFont.StyleStrategy.PreferAntialias | QFont.StyleStrategy.PreferQuality
    )
    app.setFont(font)


def _should_disable_macos_window_accessibility(
    *,
    platform_name: str | None = None,
    mac_version: str | None = None,
) -> bool:
    current_platform = platform_name or sys.platform
    if current_platform != "darwin":
        return False

    opt_in = os.environ.get(_MACOS_ACCESSIBILITY_OPT_IN, "").strip().lower()
    if opt_in in {"1", "true", "yes", "on"}:
        return False

    version_text = mac_version if mac_version is not None else platform.mac_ver()[0]
    try:
        major_version = int(version_text.split(".", 1)[0])
    except (TypeError, ValueError):
        return False
    return major_version >= 26


def _disable_macos_window_accessibility(window) -> bool:
    """规避 macOS 26 持续 AX 扫描触发的 Qt item-view 原生崩溃。"""
    if not _should_disable_macos_window_accessibility():
        return False

    try:
        import ctypes

        objc = ctypes.CDLL("/usr/lib/libobjc.A.dylib")
        selector = objc.sel_registerName
        selector.argtypes = [ctypes.c_char_p]
        selector.restype = ctypes.c_void_p

        message_send = objc.objc_msgSend
        message_send.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        message_send.restype = ctypes.c_void_p
        native_window = message_send(int(window.winId()), selector(b"window"))
        if not native_window:
            return False

        message_send.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_bool]
        message_send.restype = None
        message_send(
            native_window,
            selector(b"setAccessibilityElement:"),
            False,
        )
    except Exception:
        logging.warning(
            "Unable to apply the macOS 26 Qt accessibility crash workaround.",
            exc_info=True,
        )
        return False

    logging.warning(
        "Disabled native window accessibility on macOS 26+ to avoid a Qt crash. "
        "Set %s=1 to opt back in.",
        _MACOS_ACCESSIBILITY_OPT_IN,
    )
    return True


def main(argv: list[str] | None = None) -> int:
    _configure_logging()
    _configure_hidpi()
    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.RoundPreferFloor
    )
    app = QApplication(argv or sys.argv)
    _configure_app_font(app)
    controller = MainController()
    _disable_macos_window_accessibility(controller.window)
    controller.window.show()

    def _handle_exit_signal(*_):
        app.quit()

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(sig, _handle_exit_signal)
        except Exception:
            logging.debug("Unable to register signal handler for %s", sig, exc_info=True)

    return app.exec()


if __name__ == "__main__":
    import multiprocessing

    multiprocessing.freeze_support()
    sys.exit(main())
