#pragma once

#include "report.hpp"
#include <optional>
#include <string>
#include <vector>

namespace xm {

struct TestCaseResult {
    std::string name, status, failure_reason;
    std::optional<double> duration_seconds;
};

struct SuiteCandidate {
    std::string candidate_id, file_path, file_name, suite_id, suite_name, suite_path;
    std::vector<std::string> suite_path_parts;
    std::string status, suite_endtime, test_endtime;
    double file_mtime = 0;
    std::vector<std::string> test_names, failure_reasons;
    std::string robot_source;
    std::vector<TestCaseResult> test_results;
    std::string suite_starttime;
};

struct ConflictGroup {
    std::string suite_key, winner_id;
    std::vector<std::string> loser_ids;
    std::string reason, winner_time_source;
};

struct SelectionSuggestion {
    std::vector<std::string> selected_ids;
    std::vector<ConflictGroup> conflicts;
};

struct LoadIssue {
    std::string file_path, stage, message;
    bool recoverable = true;
};

struct LoadBatchResult {
    std::vector<SuiteCandidate> candidates;
    std::vector<LoadIssue> issues;
    int parsed_file_count = 0, failed_file_count = 0;
    std::vector<std::string> successful_file_paths;
};

struct MergeResult {
    std::string merged_xml_path, log_path, report_path, conflict_report_path;
    std::size_t selected_count = 0, candidate_count = 0;
};

struct ExcelExportResult {
    std::string output_path;
    std::size_t selected_suite_count = 0, test_count = 0;
};

class MergeService {
public:
    LoadBatchResult load_candidates(const std::vector<std::string>& paths, int max_workers = 0) const;
    SelectionSuggestion suggest_selection(const std::vector<SuiteCandidate>& candidates) const;
    MergeResult merge_selected(const std::vector<SuiteCandidate>& candidates,
                               const std::vector<std::string>& selected_ids,
                               const std::string& output_dir,
                               const rf::Options& options = {}) const;
    std::string generate_single_candidate_log(const SuiteCandidate& candidate,
                                             const std::string& output_dir,
                                             const rf::Options& options = {},
                                             const std::string& log_filename = "single-log.html") const;
    std::string export_single_candidate_xml(const SuiteCandidate& candidate,
                                           const std::string& output_path) const;
    std::string export_selected_xml(const std::vector<SuiteCandidate>& candidates,
                                    const std::vector<std::string>& selected_ids,
                                    const std::string& output_dir) const;
    ExcelExportResult export_selected_tests_excel(const std::vector<SuiteCandidate>& candidates,
                                                 const std::vector<std::string>& selected_ids,
                                                 const std::string& output_path) const;
};

} // namespace xm
