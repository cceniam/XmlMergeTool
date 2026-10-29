#include "merge_service.hpp"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#endif
namespace {
std::string q(const std::string &s) { return rf::json_quote(s); }
std::string nullable(const std::string &s) { return s.empty() ? "null" : q(s); }
std::string array(const std::vector<std::string> &items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i)
            out += ',';
        out += q(items[i]);
    }
    return out + "]";
}
std::string candidate_json(const xm::SuiteCandidate &c) {
    std::ostringstream o;
    o << std::setprecision(17);
    o << "{\"candidate_id\":" << q(c.candidate_id) << ",\"file_path\":" << q(c.file_path)
      << ",\"file_name\":" << q(c.file_name) << ",\"suite_id\":" << nullable(c.suite_id)
      << ",\"suite_name\":" << q(c.suite_name) << ",\"suite_path\":" << q(c.suite_path)
      << ",\"suite_path_parts\":" << array(c.suite_path_parts) << ",\"status\":" << q(c.status)
      << ",\"suite_starttime\":" << nullable(c.suite_starttime)
      << ",\"suite_endtime\":" << nullable(c.suite_endtime)
      << ",\"test_endtime\":" << nullable(c.test_endtime) << ",\"file_mtime\":" << c.file_mtime
      << ",\"robot_source\":" << nullable(c.robot_source)
      << ",\"test_names\":" << array(c.test_names)
      << ",\"failure_reasons\":" << array(c.failure_reasons) << ",\"test_results\":[";
    for (std::size_t i = 0; i < c.test_results.size(); ++i) {
        if (i)
            o << ',';
        const auto &t = c.test_results[i];
        o << "{\"name\":" << q(t.name) << ",\"status\":" << q(t.status)
          << ",\"failure_reason\":" << q(t.failure_reason) << ",\"duration_seconds\":";
        if (t.duration_seconds)
            o << *t.duration_seconds;
        else
            o << "null";
        o << '}';
    }
    o << "]}";
    return o.str();
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::vector<std::string> args;
#ifdef _WIN32
        int count = 0;
        auto wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!wide)
            throw std::runtime_error("Cannot decode arguments");
        for (int i = 0; i < count; ++i) {
            auto n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, s.data(), n, nullptr, nullptr);
            s.pop_back();
            args.push_back(s);
        }
        LocalFree(wide);
#else
        args.assign(argv, argv + argc);
#endif
        if (args.size() < 3)
            throw std::runtime_error("Usage: xml-merge-driver load XML... | merge DIR XML... | "
                                     "excel FILE XML... | xml FILE XML... | log DIR XML...");
        auto command = args[1];
        std::string output;
        std::size_t start = 2;
        if (command != "load")
            output = args[start++];
        std::vector<std::string> paths;
        std::string selection;
        bool explicit_selection = false;
        int workers = 0;
        rf::Options options;
        for (auto i = start; i < args.size(); ++i) {
            if (args[i] == "--splitlog") {
                options.split_log = true;
            } else if (args[i] == "--select" && i + 1 < args.size()) {
                selection = args[++i];
                explicit_selection = true;
            } else if (args[i] == "--suitestatlevel" && i + 1 < args.size())
                options.suite_stat_level = std::stoi(args[++i]);
            else if (args[i] == "--reportbackground" && i + 1 < args.size())
                options.report_background = args[++i];
            else if (args[i] == "--workers" && i + 1 < args.size())
                workers = std::stoi(args[++i]);
            else
                paths.push_back(args[i]);
        }
        xm::MergeService service;
        auto batch = service.load_candidates(paths, workers);
        auto suggestion = service.suggest_selection(batch.candidates);
        auto ids = suggestion.selected_ids;
        if (explicit_selection) {
            ids.clear();
            std::istringstream in(selection);
            std::string token;
            while (std::getline(in, token, ','))
                ids.push_back(batch.candidates.at(std::stoul(token)).candidate_id);
        }
        if (command == "load") {
            std::cout << "{\"candidates\":[";
            for (std::size_t i = 0; i < batch.candidates.size(); ++i) {
                if (i)
                    std::cout << ',';
                std::cout << candidate_json(batch.candidates[i]);
            }
            std::cout << "],\"parsed_file_count\":" << batch.parsed_file_count
                      << ",\"failed_file_count\":" << batch.failed_file_count
                      << ",\"successful_file_paths\":" << array(batch.successful_file_paths)
                      << ",\"selected_ids\":" << array(ids) << ",\"conflicts\":[";
            for (std::size_t i = 0; i < suggestion.conflicts.size(); ++i) {
                if (i)
                    std::cout << ',';
                auto &c = suggestion.conflicts[i];
                std::cout << "{\"suite_key\":" << q(c.suite_key)
                          << ",\"winner_id\":" << q(c.winner_id)
                          << ",\"loser_ids\":" << array(c.loser_ids)
                          << ",\"reason\":" << q(c.reason)
                          << ",\"winner_time_source\":" << q(c.winner_time_source) << '}';
            }
            std::cout << "],\"issues\":[";
            for (std::size_t i = 0; i < batch.issues.size(); ++i) {
                if (i)
                    std::cout << ',';
                auto &issue = batch.issues[i];
                std::cout << "{\"file_path\":" << q(issue.file_path)
                          << ",\"stage\":" << q(issue.stage) << ",\"message\":" << q(issue.message)
                          << ",\"recoverable\":true}";
            }
            std::cout << "]}";
        } else if (command == "merge") {
            auto r = service.merge_selected(batch.candidates, ids, output, options);
            std::cout << "{\"merged_xml_path\":" << q(r.merged_xml_path)
                      << ",\"log_path\":" << q(r.log_path)
                      << ",\"report_path\":" << q(r.report_path)
                      << ",\"conflict_report_path\":" << q(r.conflict_report_path)
                      << ",\"selected_count\":" << r.selected_count
                      << ",\"candidate_count\":" << r.candidate_count << '}';
        } else if (command == "excel") {
            auto r = service.export_selected_tests_excel(batch.candidates, ids, output);
            std::cout << "{\"output_path\":" << q(r.output_path)
                      << ",\"selected_suite_count\":" << r.selected_suite_count
                      << ",\"test_count\":" << r.test_count << '}';
        } else if (command == "xml" || command == "log") {
            if (ids.empty())
                throw std::runtime_error("No selected candidate");
            auto found = std::find_if(batch.candidates.begin(), batch.candidates.end(),
                                      [&](const auto &c) { return c.candidate_id == ids.front(); });
            auto path = command == "xml"
                            ? service.export_single_candidate_xml(*found, output)
                            : service.generate_single_candidate_log(*found, output, options);
            std::cout << "{\"output_path\":" << q(path) << '}';
        } else
            throw std::runtime_error("Unknown command");
        std::cout << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
