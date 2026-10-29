"""用于 PyQt6 界面的后台工作线程工具。"""

from __future__ import annotations

from typing import Any, Callable

from PyQt6.QtCore import QThread, pyqtSignal


class WorkerThread(QThread):
    """在线程中执行可调用对象，并发送成功/失败信号。"""

    succeeded = pyqtSignal(object)
    failed = pyqtSignal(str)

    def __init__(self, fn: Callable[..., Any], *args, **kwargs):
        super().__init__()
        self._fn = fn
        self._args = args
        self._kwargs = kwargs

    def run(self):
        try:
            result = self._fn(*self._args, **self._kwargs)
            self.succeeded.emit(result)
        except Exception as exc:  # pragma: no cover - 通过图形界面运行时覆盖
            self.failed.emit(str(exc))

