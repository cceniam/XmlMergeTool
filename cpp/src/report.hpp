#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace rf {
namespace fs = std::filesystem;
using Time = std::optional<std::int64_t>;
struct InputError : std::runtime_error {
    using runtime_error::runtime_error;
};
struct Status {
    std::string value = "FAIL", message;
    Time start, end;
    std::int64_t elapsed = 0;
};
struct Item {
    // kind -1 is a message; remaining values are the RF 6.1.1 JS keyword codes.
    int kind = 0;
    std::string name, library, doc, timeout, text, level = "INFO";
    bool html = false;
    Time timestamp;
    std::vector<std::string> args, assign, tags;
    Status status;
    std::vector<Item> body;
};
struct Test {
    std::string name, doc, timeout;
    std::vector<std::string> tags;
    Status status;
    std::vector<Item> body;
};
struct Counts {
    std::int64_t pass = 0, fail = 0, skip = 0, elapsed = 0;
    std::int64_t total() const {
        return pass + fail + skip;
    }
    void add(const Counts &c) {
        pass += c.pass;
        fail += c.fail;
        skip += c.skip;
        elapsed += c.elapsed;
    }
};
struct Suite {
    std::string name, source, doc;
    std::vector<std::pair<std::string, std::string>> metadata;
    Status status;
    Counts counts;
    std::vector<Suite> suites;
    std::vector<Test> tests;
    std::vector<Item> fixtures;
};
struct Result {
    Suite suite;
    std::vector<Item> errors;
    bool rpa = false;
};
struct Options {
    fs::path input, outputdir = ".";
    std::string log = "log.html", report = "report.html";
    std::string level = "TRACE", visible = "TRACE", logtitle, reporttitle;
    bool split_log = false;
    std::string remove_keywords, flatten_keywords, report_background;
    int suite_stat_level = -1;
};
struct EncodedPage {
    std::string model;
    // Each entry is a complete RF split-log JS file, numbered from one.
    std::vector<std::string> split_logs;
};
std::string trim(std::string s);
std::string unicode_strip(const std::string &s);
std::string upper(std::string s);
std::string normalized(const std::string &s, bool ignore_underscore = true);
std::string join(const std::vector<std::string> &values, const std::string &separator);
std::string escape_html(std::string text, bool attr = false, bool linkify = true);
std::string format_doc(const std::string &text);
std::string json_quote(const std::string &value);
std::string json_array(const std::vector<std::string> &values);
std::string json_object(const std::map<std::string, std::string> &values);
std::string compressed_string(const std::string &text);
std::string link_path(const fs::path &target, const fs::path &directory);
Time parse_time(const std::string &text);
int level_code(const std::string &level);
Result read_result(const fs::path &input, bool include_keywords = true);
Result read_result(const fs::path &input, const Options &options);
std::string encode_model(const Result &result, const Options &options, bool report);
EncodedPage encode_page(const Result &result, const Options &options, bool report);
std::string render_html(const std::string &model, bool report);
void write_outputs(const Result &result, const Options &options);
} // namespace rf
