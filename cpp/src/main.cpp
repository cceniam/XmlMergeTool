#include "report.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#endif
namespace {
rf::Options parse(const std::vector<std::string> &args) {
    rf::Options o;
    bool positional = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        auto key = args[i];
        if (key == "--splitlog") {
            o.split_log = true;
            continue;
        }
        if (key == "--") {
            if (i + 2 != args.size() || positional)
                throw rf::InputError("Expected exactly one XML input");
            o.input = rf::fs::u8path(args[++i]);
            positional = true;
            continue;
        }
        if (key.rfind("--", 0) == 0) {
            std::string value;
            auto eq = key.find('=');
            if (eq != std::string::npos) {
                value = key.substr(eq + 1);
                key.resize(eq);
            } else {
                if (i + 1 == args.size())
                    throw rf::InputError("Missing value for " + key);
                value = args[++i];
            }
            if (key == "--outputdir")
                o.outputdir = rf::fs::u8path(value);
            else if (key == "--log")
                o.log = value;
            else if (key == "--report")
                o.report = value;
            else if (key == "--logtitle")
                o.logtitle = value;
            else if (key == "--reporttitle")
                o.reporttitle = value;
            else if (key == "--removekeywords")
                o.remove_keywords = value;
            else if (key == "--flattenkeywords")
                o.flatten_keywords = value;
            else if (key == "--reportbackground")
                o.report_background = value;
            else if (key == "--suitestatlevel") {
                std::size_t used = 0;
                try {
                    o.suite_stat_level = std::stoi(value, &used);
                } catch (const std::exception &) {
                    throw rf::InputError("Invalid --suitestatlevel");
                }
                if (used != value.size() || o.suite_stat_level < -1)
                    throw rf::InputError("Invalid --suitestatlevel");
            }
            else if (key == "--loglevel") {
                value = rf::upper(value);
                auto colon = value.find(':');
                o.level = value.substr(0, colon);
                o.visible = colon == std::string::npos ? o.level : value.substr(colon + 1);
                if (o.level != "TRACE" && o.level != "DEBUG" && o.level != "INFO" &&
                    o.level != "WARN" && o.level != "NONE")
                    throw rf::InputError("Invalid --loglevel threshold");
                if (colon != std::string::npos && o.visible != "TRACE" && o.visible != "DEBUG" &&
                    o.visible != "INFO" && o.visible != "WARN")
                    throw rf::InputError("Invalid --loglevel visible level");
            } else
                throw rf::InputError("Unsupported option: " + key);
        } else if (!key.empty() && key[0] == '-')
            throw rf::InputError("Unsupported option: " + key);
        else {
            if (positional)
                throw rf::InputError("Expected exactly one XML input");
            o.input = rf::fs::u8path(key);
            positional = true;
        }
    }
    if (!positional || o.input.empty())
        throw rf::InputError("An input XML is required. Use --help.");
    if (o.log.empty() || o.report.empty())
        throw rf::InputError("Output filename cannot be empty; use NONE to disable");
    if (rf::upper(o.log) == "NONE")
        o.log = "NONE";
    if (rf::upper(o.report) == "NONE")
        o.report = "NONE";
    return o;
}
} // namespace
int main(int argc, char **argv) {
    try {
        std::vector<std::string> args;
#ifdef _WIN32
        int count = 0;
        auto wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!wide)
            throw std::runtime_error("Cannot decode command line");
        for (int i = 0; i < count; ++i) {
            auto size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string value(size, '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, value.data(), size, nullptr, nullptr);
            value.pop_back();
            args.push_back(value);
        }
        LocalFree(wide);
#else
        args.assign(argv, argv + argc);
#endif
        if (args.size() == 2 && (args[1] == "--help" || args[1] == "--version")) {
            std::cout << "rf-report-cpp 0.1.0 (Robot Framework schema 3/4; RF 6.1.1 HTML)\n";
            if (args[1] == "--help")
                std::cout
                    << "Usage: rf-report-cpp input.xml [--outputdir DIR] [--log FILE|NONE]\n  "
                       "[--report FILE|NONE] [--loglevel TRACE|DEBUG|INFO|WARN|NONE[:VISIBLE]]\n  "
                       "[--logtitle TITLE] [--reporttitle TITLE] [--splitlog]\n  "
                       "[--removekeywords passed|wuks|for|while|all]\n  "
                       "[--flattenkeywords for|while|iteration] [--suitestatlevel N]\n  "
                       "[--reportbackground PASS:FAIL[:SKIP]]\nRuntime: native C++; no Python "
                       "or Rebot required.\n";
            return 0;
        }
        auto options = parse(args);
        auto begin = std::chrono::steady_clock::now();
        auto result = rf::read_result(options.input, options);
        auto parsed = std::chrono::steady_clock::now();
        rf::write_outputs(result, options);
        auto end = std::chrono::steady_clock::now();
        auto millis = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        std::cout << "Tests: " << result.suite.counts.total()
                  << ", passed: " << result.suite.counts.pass
                  << ", failed: " << result.suite.counts.fail
                  << ", skipped: " << result.suite.counts.skip << "\n";
        std::cerr << "Timing (ms): parse=" << millis(begin, parsed)
                  << " generate=" << millis(parsed, end) << " total=" << millis(begin, end) << "\n";
        return static_cast<int>(std::min<std::int64_t>(250, result.suite.counts.fail));
    } catch (const rf::InputError &e) {
        std::cerr << "Input error: " << e.what() << "\n";
        return 252;
    } catch (const std::exception &e) {
        std::cerr << "Generation error: " << e.what() << "\n";
        return 255;
    }
}
