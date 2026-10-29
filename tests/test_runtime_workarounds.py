from __future__ import annotations

import os
from unittest.mock import patch

from xml_merge_tool.main import _should_disable_macos_window_accessibility


def test_macos_26_disables_window_accessibility_by_default():
    with patch.dict(os.environ, {}, clear=True):
        assert _should_disable_macos_window_accessibility(
            platform_name="darwin",
            mac_version="26.6.2",
        )


def test_macos_accessibility_can_be_explicitly_enabled():
    with patch.dict(
        os.environ,
        {"ROBOT_XML_MERGE_ENABLE_MACOS_ACCESSIBILITY": "1"},
        clear=True,
    ):
        assert not _should_disable_macos_window_accessibility(
            platform_name="darwin",
            mac_version="26.6.2",
        )


def test_workaround_does_not_apply_to_older_macos_or_other_platforms():
    with patch.dict(os.environ, {}, clear=True):
        assert not _should_disable_macos_window_accessibility(
            platform_name="darwin",
            mac_version="15.7",
        )
        assert not _should_disable_macos_window_accessibility(
            platform_name="win32",
            mac_version="26.6.2",
        )
