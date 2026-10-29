"""基于 Robot Framework 的 XML 读取器（支持多进程）。"""

from __future__ import annotations

import multiprocessing
import os
from concurrent.futures import ProcessPoolExecutor
from typing import Iterable, Sequence

from xml_merge_tool.application.models import (
    LoadBatchResult,
    LoadIssue,
    SuiteCandidate,
    TestCaseResult,
)
from xml_merge_tool.infrastructure.rf_parse_worker import WorkerResult, parse_file_worker


class RFResultReader:
    """从 Robot Framework 输出 XML 中加载套件候选项。"""

    def load_candidates(
        self, file_paths: Sequence[str], max_workers: int | None = None
    ) -> LoadBatchResult:
        if not file_paths:
            return LoadBatchResult(
                candidates=tuple(),
                issues=tuple(),
                parsed_file_count=0,
                failed_file_count=0,
                successful_file_paths=tuple(),
            )
        return self.load_candidates_parallel(file_paths, max_workers=max_workers)

    def load_candidates_parallel(
        self, file_paths: Sequence[str], max_workers: int | None = None
    ) -> LoadBatchResult:
        normalized = [os.path.abspath(path) for path in file_paths if path]
        if not normalized:
            return LoadBatchResult(
                candidates=tuple(),
                issues=tuple(),
                parsed_file_count=0,
                failed_file_count=0,
                successful_file_paths=tuple(),
            )

        if len(normalized) == 1:
            return self.load_candidates_serial(normalized)

        worker_count = self._resolve_worker_count(len(normalized), max_workers)
        if worker_count <= 1:
            return self.load_candidates_serial(normalized)

        indexed_tasks = list(enumerate(normalized))
        try:
            mp_context = multiprocessing.get_context("spawn")
            with ProcessPoolExecutor(
                max_workers=worker_count, mp_context=mp_context
            ) as executor:
                raw_results = executor.map(parse_file_worker, indexed_tasks)
                return self._build_batch_result(raw_results)
        except Exception as exc:
            serial_result = self.load_candidates_serial(normalized)
            fallback_issue = LoadIssue(
                file_path="*",
                stage="parallel",
                message=f"并行解析不可用，已回退串行解析: {exc}",
                recoverable=True,
            )
            return LoadBatchResult(
                candidates=serial_result.candidates,
                issues=serial_result.issues + (fallback_issue,),
                parsed_file_count=serial_result.parsed_file_count,
                failed_file_count=serial_result.failed_file_count,
                successful_file_paths=serial_result.successful_file_paths,
            )

    def load_candidates_serial(self, file_paths: Sequence[str]) -> LoadBatchResult:
        indexed_tasks = list(enumerate(file_paths))
        raw_results = (parse_file_worker(task) for task in indexed_tasks)
        return self._build_batch_result(raw_results)

    def _build_batch_result(self, raw_results: Iterable[WorkerResult]) -> LoadBatchResult:
        candidates: list[SuiteCandidate] = []
        issues: list[LoadIssue] = []
        successful_file_paths: list[str] = []
        parsed_file_count = 0
        failed_file_count = 0

        for _, file_path, candidate_data, issue_data in raw_results:
            if issue_data:
                failed_file_count += 1
                issues.append(
                    LoadIssue(
                        file_path=str(issue_data.get("file_path", "")),
                        stage=str(issue_data.get("stage", "parse")),
                        message=str(issue_data.get("message", "")),
                        recoverable=bool(issue_data.get("recoverable", True)),
                    )
                )
            else:
                parsed_file_count += 1
                successful_file_paths.append(file_path)

            for item in candidate_data:
                candidates.append(
                    SuiteCandidate(
                        candidate_id=str(item["candidate_id"]),
                        file_path=str(item["file_path"]),
                        file_name=str(item["file_name"]),
                        suite_id=(
                            str(item["suite_id"])
                            if item.get("suite_id") is not None
                            else None
                        ),
                        suite_name=str(item["suite_name"]),
                        suite_path=str(item["suite_path"]),
                        suite_path_parts=tuple(item["suite_path_parts"]),
                        status=str(item["status"]),
                        suite_starttime=(
                            str(item["suite_starttime"])
                            if item.get("suite_starttime") is not None
                            else None
                        ),
                        suite_endtime=(
                            str(item["suite_endtime"])
                            if item.get("suite_endtime") is not None
                            else None
                        ),
                        test_endtime=(
                            str(item["test_endtime"])
                            if item.get("test_endtime") is not None
                            else None
                        ),
                        file_mtime=float(item["file_mtime"]),
                        robot_source=(
                            str(item["robot_source"])
                            if item.get("robot_source") is not None
                            else None
                        ),
                        test_results=tuple(
                            TestCaseResult(
                                name=str(test.get("name", "<unnamed-test>")),
                                status=str(test.get("status", "UNKNOWN")),
                                failure_reason=str(test.get("failure_reason", "")),
                                duration_seconds=(
                                    float(test["duration_seconds"])
                                    if test.get("duration_seconds") is not None
                                    else None
                                ),
                            )
                            for test in item.get("test_results", ())
                        ),
                        test_names=tuple(str(name) for name in item.get("test_names", ())),
                        failure_reasons=tuple(
                            str(reason) for reason in item.get("failure_reasons", ())
                        ),
                    )
                )

        return LoadBatchResult(
            candidates=tuple(candidates),
            issues=tuple(issues),
            parsed_file_count=parsed_file_count,
            failed_file_count=failed_file_count,
            successful_file_paths=tuple(successful_file_paths),
        )

    @staticmethod
    def _resolve_worker_count(file_count: int, max_workers: int | None) -> int:
        if max_workers is not None:
            return max(1, min(int(max_workers), file_count))
        cpu_count = os.cpu_count() or 2
        return max(1, min(file_count, cpu_count - 1))
