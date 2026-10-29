"""Measure native load, end-to-end merge, and report CLI on macOS.

Test harness only: neither native binary launches Python. Artifacts stay under build/.
Default large input is a deterministic 64 MiB generated Robot XML; --large-source
copies an existing real input and hashes the original before and after the run.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
GENERATOR_VERSION = 1


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def synthetic(path: Path, mib: int) -> dict:
    """Stream a stable input with unique messages, 256 tests per leaf suite."""
    budget = mib * 1024 * 1024
    count = 0
    suites = 0
    status = '<status status="PASS" starttime="20260101 00:00:00.000" endtime="20260101 00:00:00.001"/>'
    payload = "Measured native XML parsing and report generation. " * 80
    with path.open("wb") as out:
        def write(text):
            out.write(text.encode("utf-8"))
        write('<?xml version="1.0" encoding="UTF-8"?>\n<robot generator="Robot 6.1.1" generated="20260101 00:00:00.000" schemaversion="4" rpa="false">\n<suite name="Benchmark" id="s1" source="synthetic.robot">\n')
        while out.tell() < budget:
            if count % 256 == 0:
                if count:
                    write(status + '</suite>\n')
                suites += 1
                write(f'<suite name="Batch {suites:05d}" id="s1-s{suites}">\n')
            count += 1
            test_id = f"s1-s{suites}-t{(count - 1) % 256 + 1}"
            write(f'<test name="Case {count:08d}" id="{test_id}"><kw name="Log" library="BuiltIn"><arg>item {count:08d}</arg><msg level="INFO" timestamp="20260101 00:00:00.000">Event {count:08d}: {payload}</msg>{status}</kw><tag>benchmark</tag>{status}</test>\n')
        write(status + '</suite>\n' + status + '</suite><statistics/><errors/></robot>\n')
    return {"generator_version": GENERATOR_VERSION, "target_mib": mib,
            "tests": count, "candidate_suites": suites,
            "description": "256 tests per leaf suite; one keyword and a unique ~4 KiB INFO message per test; all PASS"}


def measured(command: list[str], directory: Path, operation: str) -> dict:
    directory.mkdir(parents=True, exist_ok=True)
    stdout_path = directory / "stdout.txt"
    stderr_path = directory / "stderr.txt"
    system = platform.system()
    if system == "Darwin":
        invocation = ["/usr/bin/time", "-l", *command]
        rss_pattern = r"(\d+)\s+maximum resident set size"
        rss_multiplier = 1
    elif system == "Linux" and Path("/usr/bin/time").is_file():
        invocation = ["/usr/bin/time", "-v", *command]
        rss_pattern = r"Maximum resident set size \(kbytes\):\s*(\d+)"
        rss_multiplier = 1024
    else:
        raise RuntimeError("Peak RSS measurement requires macOS /usr/bin/time -l or Linux GNU time.")
    env = os.environ.copy()
    env["LC_ALL"] = "C"
    begin = time.perf_counter()
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        process = subprocess.run(invocation, cwd=ROOT, env=env, stdout=stdout, stderr=stderr,
                                 timeout=900)
    wall_ms = (time.perf_counter() - begin) * 1000
    err = stderr_path.read_text(encoding="utf-8")
    if process.returncode != 0 and not (operation == "report" and 0 < process.returncode <= 250):
        raise RuntimeError(f"{operation} failed ({process.returncode}): {err}")
    memory = re.search(rss_pattern, err)
    if not memory:
        raise RuntimeError(f"Missing maximum RSS measurement in {stderr_path}")
    record = {"command": command, "operation": operation, "wall_ms": wall_ms,
              "peak_rss_bytes": int(memory[1]) * rss_multiplier,
              "exit_code": process.returncode, "stdout_path": str(stdout_path),
              "stderr_path": str(stderr_path)}
    timing = re.search(r"Timing \(ms\): parse=(\S+) generate=(\S+) total=(\S+)", err)
    if timing:
        record.update(zip(("report_parse_ms", "report_generate_ms", "report_internal_total_ms"),
                          map(float, timing.groups())))
    if operation in {"load", "merge"}:
        data = json.loads(stdout_path.read_text(encoding="utf-8"))
        if operation == "load":
            assert data["failed_file_count"] == 0, data["issues"]
            record.update(candidate_count=len(data["candidates"]),
                          selected_count=len(data["selected_ids"]),
                          direct_test_count=sum(len(c["test_results"]) for c in data["candidates"]))
        else:
            record["result"] = data
            record["output_bytes"] = {key: Path(value).stat().st_size for key, value in data.items()
                                      if key.endswith("_path")}
    print(f"{directory.parent.name}/{directory.name}: {wall_ms:.1f} ms, "
          f"{record['peak_rss_bytes'] / 1024**2:.1f} MiB RSS", flush=True)
    return record


def aggregate(records: list[dict]) -> dict:
    return {key: {"min": min(values), "median": statistics.median(values), "max": max(values)}
            for key in ("wall_ms", "peak_rss_bytes")
            for values in [[r[key] for r in records]]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", type=Path, default=ROOT / "build/desktop/xml-merge-driver")
    parser.add_argument("--report", type=Path, default=ROOT / "build/desktop/rf-report-cpp")
    parser.add_argument("--out", type=Path, default=ROOT / "build/app-benchmark")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--large-mib", type=int, default=64)
    parser.add_argument("--large-source", type=Path)
    args = parser.parse_args()
    if args.repeats < 1 or args.large_mib < 1:
        parser.error("repeats and large-mib must be positive")
    driver, report = args.driver.resolve(strict=True), args.report.resolve(strict=True)
    out = args.out.resolve()
    if not out.is_relative_to(ROOT / "build"):
        parser.error("Benchmark output must remain under the ignored repository build/ directory")
    out.mkdir(parents=True, exist_ok=True)
    run = out / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    run.mkdir()
    original_fixture = ROOT / "tests/fixtures/test.xml"
    originals = {str(original_fixture): sha256(original_fixture)}
    fixture = run / "repository-fixture.xml"
    shutil.copyfile(original_fixture, fixture)
    large = run / "large.xml"
    if args.large_source:
        original_large = args.large_source.resolve(strict=True)
        originals[str(original_large)] = sha256(original_large)
        shutil.copyfile(original_large, large)
        large_description = {"original": str(original_large), "kind": "read-only copy of real XML"}
    else:
        large_description = {"kind": "synthetic", **synthetic(large, args.large_mib)}
    benchmark = {"created_at_utc": datetime.now(timezone.utc).isoformat(),
                 "platform": platform.platform(), "machine": platform.machine(),
                 "processor": platform.processor(), "logical_cpus": os.cpu_count(),
                 "repeats": args.repeats, "warmup_runs": 0, "run_order": "load, merge, report per repeat",
                 "method": {"wall": "Python perf_counter around /usr/bin/time and native subprocess",
                            "peak_rss": "macOS /usr/bin/time -l maximum resident set size in bytes; GNU time KiB converted on Linux",
                            "load": "candidate parsing + selection + JSON serialization in driver",
                            "merge": "end-to-end driver loading, selection, full DOM merge, CSV and both HTML reports",
                            "report": "standalone report CLI on the first native merged XML; parsing and generation are also timed internally",
                            "cache": "No OS cache purge or warmup; all measured rounds retained",
                            "limits": "No Python performance comparison, UI responsiveness measurement or Windows measurements"},
                 "binaries": {"driver": {"path": str(driver), "sha256": sha256(driver)},
                              "report": {"path": str(report), "sha256": sha256(report)}},
                 "original_sha256_before": originals, "samples": []}
    for name, source, description in (("repository-fixture", fixture, {"original": str(original_fixture), "kind": "truncated synthetic fixture; native repair path"}),
                                       ("large", large, large_description)):
        sample = {"name": name, "input": str(source), "input_bytes": source.stat().st_size,
                  "input_sha256_before": sha256(source), "description": description,
                  "load": [], "merge": [], "report": []}
        report_input = None
        for repeat in range(1, args.repeats + 1):
            repeat_dir = run / name / f"repeat-{repeat}"
            sample["load"].append(measured([str(driver), "load", str(source)], repeat_dir / "load", "load"))
            merge = measured([str(driver), "merge", str(repeat_dir / "merged"), str(source)], repeat_dir / "merge", "merge")
            sample["merge"].append(merge)
            if report_input is None:
                report_input = Path(merge["result"]["merged_xml_path"])
                sample["report_input"] = str(report_input)
                sample["report_input_bytes"] = report_input.stat().st_size
                sample["report_input_sha256_before"] = sha256(report_input)
            sample["report"].append(measured([str(report), str(report_input), "--outputdir", str(repeat_dir / "report-output")], repeat_dir / "report", "report"))
        sample["summary"] = {operation: aggregate(sample[operation]) for operation in ("load", "merge", "report")}
        sample["input_sha256_after"] = sha256(source)
        sample["report_input_sha256_after"] = sha256(report_input)
        assert sample["input_sha256_before"] == sample["input_sha256_after"]
        assert sample["report_input_sha256_before"] == sample["report_input_sha256_after"]
        benchmark["samples"].append(sample)
        (run / "benchmark.json").write_text(json.dumps(benchmark, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    benchmark["original_sha256_after"] = {path: sha256(Path(path)) for path in originals}
    assert benchmark["original_sha256_before"] == benchmark["original_sha256_after"]
    for record in benchmark["binaries"].values():
        record["sha256_after"] = sha256(Path(record["path"]))
        assert record["sha256"] == record["sha256_after"], "Binary changed during measurement"
    benchmark["sources_unchanged"] = True
    evidence = run / "benchmark.json"
    evidence.write_text(json.dumps(benchmark, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (out / "latest.json").write_text(json.dumps({"evidence": str(evidence)}, indent=2) + "\n", encoding="utf-8")
    print(f"Evidence: {evidence}", flush=True)


if __name__ == "__main__":
    main()
