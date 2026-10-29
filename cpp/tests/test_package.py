"""Exercise the shipped executable. Python is a test controller only.

Usage: python cpp/tests/test_package.py /absolute/path/to/XmlMergeTool[.exe]
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def validate_macos_dependencies(executable: Path) -> list[str]:
    """Resolve every non-system Mach-O dependency inside the shipped bundle."""
    bundle = executable.parents[2]
    checked = []
    for binary in bundle.rglob("*"):
        if binary.is_symlink():
            assert binary.exists(), f"Dangling bundle symlink: {binary}"
            assert binary.resolve().is_relative_to(bundle), f"External bundle symlink: {binary}"
            continue
        if not binary.is_file():
            continue
        with binary.open("rb") as stream:
            if stream.read(4) not in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
                continue
        # Official Qt frameworks are universal binaries. Inspect the shipped
        # application's arm64 slice and distinguish load commands from dylib IDs.
        commands = subprocess.check_output(
            ["/usr/bin/otool", "-arch", "arm64", "-l", str(binary)], text=True)
        dependencies = re.findall(
            r"cmd LC_(?:LOAD|LOAD_WEAK|REEXPORT|LOAD_UPWARD|LAZY_LOAD)_DYLIB\n"
            r"\s+cmdsize \d+\n\s+name (.*?) \(offset", commands)
        assert dependencies, f"No arm64 dependency load commands: {binary}"
        for dependency in dependencies:
            if dependency.startswith(("/usr/lib/", "/System/Library/")):
                continue
            if dependency.startswith("@executable_path/"):
                target = executable.parent / dependency.removeprefix("@executable_path/")
            elif dependency.startswith("@loader_path/"):
                target = binary.parent / dependency.removeprefix("@loader_path/")
            elif dependency.startswith("@rpath/"):
                target = bundle / "Contents/Frameworks" / dependency.removeprefix("@rpath/")
            else:
                raise AssertionError(f"External runtime dependency: {binary}: {dependency}")
            assert target.resolve().is_relative_to(bundle), (binary, target)
            assert target.is_file(), (binary, target)
        checked.append(str(binary.relative_to(bundle)))
    assert checked
    return checked


def validate_artifact(executable: Path) -> dict:
    executable = executable.resolve(strict=True)
    dependencies = validate_macos_dependencies(executable) if sys.platform == "darwin" else []
    base = ROOT / "build/package-validation"
    base.mkdir(parents=True, exist_ok=True)
    # Each run is independent, as are concurrent single-file extraction dirs.
    workspace = base / f"run-{time.time_ns()}-中文 路径"
    workspace.mkdir()
    source = workspace / "输入 日志.xml"
    shutil.copyfile(ROOT / "cpp/tests/fixtures/schema3.xml", source)
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    env = os.environ.copy()
    for key in ("PYTHONPATH", "PYTHONHOME", "VIRTUAL_ENV", "QT_PLUGIN_PATH",
                "QT_QPA_PLATFORM_PLUGIN_PATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH"):
        env.pop(key, None)
    env["PATH"] = ""
    # The shipped Cocoa/Windows platform plugin must be exercised here.
    env.pop("QT_QPA_PLATFORM", None)
    temp_root = workspace / "temporary"
    temp_root.mkdir()
    env["TMP"] = env["TEMP"] = str(temp_root)
    env["TMPDIR"] = str(temp_root) + os.sep

    def run(index):
        output = workspace / f"输出 {index}"
        begin = time.perf_counter()
        # Relative Unicode arguments also prove the single-file launcher keeps
        # the caller's working directory instead of using its extraction path.
        process = subprocess.run(
            [str(executable), "--smoke-test", source.name, output.name],
            cwd=workspace, env=env, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=120,
        )
        (workspace / f"stdout-{index}.txt").write_text(process.stdout, encoding="utf-8")
        (workspace / f"stderr-{index}.txt").write_text(process.stderr, encoding="utf-8")
        assert process.returncode == 0, (process.returncode, process.stdout, process.stderr)
        assert (output / "smoke-results.json").is_file(), output
        assert list(output.glob("merged-*.xml")), output
        assert list(output.glob("log-*.html")), output
        assert list(output.glob("report-*.html")), output
        assert list(output.glob("conflict_report-*.csv")), output
        workbooks = list(output.glob("*.xlsx"))
        assert workbooks, output
        for workbook in workbooks:
            with zipfile.ZipFile(workbook) as archive:
                assert archive.testzip() is None
                assert "xl/worksheets/sheet1.xml" in archive.namelist()
        smoke = json.loads((output / "smoke-results.json").read_text(encoding="utf-8"))
        assert smoke["success"] and smoke["source_unchanged"], smoke
        for key in ("single_xml", "single_log"):
            assert Path(smoke[key]).is_file(), smoke
        return {"exit_code": process.returncode, "elapsed_seconds": time.perf_counter() - begin,
                "output": str(output), "smoke": smoke}

    with ThreadPoolExecutor(max_workers=2) as pool:
        runs = list(pool.map(run, range(2)))
    assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
    if os.name == "nt":
        assert not list(temp_root.glob("XmlMergeTool-*")), "Launcher did not clean up its own directories"
    result = {"executable": str(executable), "source_sha256": digest, "runs": runs,
              "developer_path_removed": True, "python_environment_removed": True,
              "bundled_macho_dependencies_checked": dependencies}
    (workspace / "validation.json").write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
    return result


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: test_package.py PATH_TO_DISTRIBUTED_EXECUTABLE")
    # The test controller's Windows console may still use a legacy code page.
    # Keep stdout JSON portable; validation.json itself is written as UTF-8.
    print(json.dumps(validate_artifact(Path(sys.argv[1])), ensure_ascii=True, indent=2))
