#include "merge_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlreader.h>
#include <map>
#include <memory>
#include <miniz.h>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <xlsxwriter.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace xm {
namespace {
namespace fs = std::filesystem;
using Doc = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;
using Node = xmlNode *;
std::string name(Node n) { return n && n->name ? reinterpret_cast<const char *>(n->name) : ""; }
std::string attr(Node n, const char *key) {
    auto p = xmlGetNoNsProp(n, BAD_CAST key);
    std::string out = p ? reinterpret_cast<const char *>(p) : "";
    xmlFree(p);
    return out;
}
std::string normalized_status(const std::string &value) {
    auto s = rf::upper(rf::unicode_strip(value));
    return s == "PASS" || s == "FAIL" || s == "SKIP" ? s : "UNKNOWN";
}
std::string absolute(const std::string &p) {
    return fs::absolute(fs::u8path(p)).lexically_normal().u8string();
}
std::string read_file(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("无法读取文件: " + p.u8string());
    return {std::istreambuf_iterator<char>(in), {}};
}
int read_io(void *opaque, char *data, int count) {
    auto &stream = *static_cast<std::ifstream *>(opaque);
    stream.read(data, count);
    if (stream.bad())
        return -1;
    return static_cast<int>(stream.gcount());
}
int write_io(void *opaque, const char *data, int count) {
    auto &stream = *static_cast<std::ofstream *>(opaque);
    stream.write(data, count);
    return stream ? count : -1;
}
constexpr int parse_flags =
    XML_PARSE_HUGE | XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;
Doc parse_tree(const std::string &path) {
    std::string errors;
    for (int stage = 0; stage < 3; ++stage) {
        Doc doc(nullptr, xmlFreeDoc);
        if (stage < 2) {
            std::ifstream in(fs::u8path(path), std::ios::binary);
            if (in)
                doc.reset(xmlReadIO(read_io, nullptr, &in, nullptr, nullptr,
                                    parse_flags | (stage ? XML_PARSE_RECOVER : 0)));
        } else {
            auto text = read_file(fs::u8path(path));
            text.erase(std::remove(text.begin(), text.end(), '\0'), text.end());
            auto first = text.find("<robot"), last = text.rfind("</robot>");
            if (first != std::string::npos && last != std::string::npos && last > first) {
                auto payload = text.substr(first, last + 8 - first);
                if (payload.size() <= static_cast<std::size_t>(INT_MAX))
                    doc.reset(xmlReadMemory(payload.data(), static_cast<int>(payload.size()),
                                            nullptr, nullptr, parse_flags | XML_PARSE_RECOVER));
            }
        }
        if (doc && name(xmlDocGetRootElement(doc.get())) == "robot")
            return doc;
        errors += std::string(stage ? " | " : "") + "[" +
                  (stage == 0   ? "strict"
                   : stage == 1 ? "recover"
                                : "sanitize") +
                  "] 缺少有效的 <robot> 根节点";
    }
    throw std::runtime_error("解析失败，已尝试自动修复: " + errors);
}
struct Date {
    std::tm calendar{};
    std::int64_t microseconds = 0;
    std::int64_t naive = 0;
};
// Gregorian civil days, independent of host timezone (Python naive datetime subtraction).
std::int64_t civil_days(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yo = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    return era * 146097LL + yo * 365 + yo / 4 - yo / 100 + doy - 719468;
}
std::optional<Date> parse_date(const std::string &value) {
    const auto s = rf::unicode_strip(value);
    static const std::regex re(
        R"(^(\d{4})(\d{2})(\d{2}) (\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,6}))?$)");
    std::smatch m;
    if (!std::regex_match(s, m, re))
        return {};
    int y = std::stoi(m[1]), mo = std::stoi(m[2]), day = std::stoi(m[3]);
    int h = std::stoi(m[4]), mi = std::stoi(m[5]), sec = std::stoi(m[6]);
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y < 1 || mo < 1 || mo > 12 || day < 1 || h > 23 || mi > 59 || sec > 59)
        return {};
    int max = days[mo - 1] + (mo == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    if (day > max)
        return {};
    auto fraction = m[7].str();
    fraction.resize(6, '0');
    Date d;
    d.microseconds = std::stoll(fraction);
    d.calendar.tm_year = y - 1900;
    d.calendar.tm_mon = mo - 1;
    d.calendar.tm_mday = day;
    d.calendar.tm_hour = h;
    d.calendar.tm_min = mi;
    d.calendar.tm_sec = sec;
    d.calendar.tm_isdst = -1;
    d.naive = ((civil_days(y, mo, day) * 24 + h) * 3600 + mi * 60 + sec) * 1000000 + d.microseconds;
    return d;
}
std::tm local_time(std::time_t t) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &t);
#else
    localtime_r(&t, &result);
#endif
    return result;
}
std::string timestamp(bool filename) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto tm = local_time(t);
    std::ostringstream out;
    out << std::put_time(&tm, filename ? "%Y%m%d-%H%M%S" : "%Y%m%d %H:%M:%S");
    if (!filename)
        out << '.' << std::setfill('0') << std::setw(3)
            << (std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                    .count() %
                1000);
    return out.str();
}
double file_mtime(const std::string &path) {
#ifdef _WIN32
    // Open without FILE_FLAG_OPEN_REPARSE_POINT so timestamps follow symlinks,
    // matching Python os.stat/os.path.getmtime and the POSIX stat branch below.
    HANDLE file = CreateFileW(fs::u8path(path).c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("无法读取文件时间");
    FILETIME modified{};
    const bool read = GetFileTime(file, nullptr, nullptr, &modified) != 0;
    CloseHandle(file);
    if (!read)
        throw std::runtime_error("无法读取文件时间");
    ULARGE_INTEGER ticks{};
    ticks.LowPart = modified.dwLowDateTime;
    ticks.HighPart = modified.dwHighDateTime;
    // Split the integral seconds before converting to floating point. Converting
    // the complete 100 ns counter first loses precision and can differ from
    // CPython's stat_result.st_mtime by one ULP. Signed epoch subtraction also
    // preserves file timestamps before 1970 instead of wrapping unsigned values.
    const auto seconds = static_cast<std::int64_t>(ticks.QuadPart / 10000000ULL) -
                         11644473600LL;
    const auto nanoseconds = static_cast<std::uint32_t>(ticks.QuadPart % 10000000ULL) * 100U;
    return static_cast<double>(seconds) + 1e-9 * nanoseconds;
#else
    struct stat st{};
    if (::stat(path.c_str(), &st))
        throw std::runtime_error("无法读取文件时间");
#ifdef __APPLE__
    return static_cast<double>(st.st_mtimespec.tv_sec) + st.st_mtimespec.tv_nsec / 1e9;
#else
    return static_cast<double>(st.st_mtim.tv_sec) + st.st_mtim.tv_nsec / 1e9;
#endif
#endif
}
struct EffectiveTime {
    double seconds;
    std::string source, display;
};
EffectiveTime effective_time(const SuiteCandidate &c) {
    for (const auto &pair : {std::make_pair(c.suite_endtime, "suite_endtime"),
                             std::make_pair(c.test_endtime, "test_endtime")}) {
        auto d = parse_date(pair.first);
        if (!d)
            continue;
        auto tm = d->calendar;
        double seconds = static_cast<double>(std::mktime(&tm)) + d->microseconds / 1e6;
        std::ostringstream out;
        out << std::put_time(&d->calendar, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(6)
            << std::setfill('0') << d->microseconds;
        return {seconds, pair.second, out.str()};
    }
    double whole_seconds = 0;
    const double fraction = std::modf(std::max(0.0, c.file_mtime), &whole_seconds);
    // datetime.fromtimestamp rounds only the fractional second, using ties-to-even.
    // Scaling the full epoch timestamp first introduces a second rounding error.
    auto micros = static_cast<std::int64_t>(std::nearbyint(fraction * 1e6));
    if (micros == 1000000) {
        whole_seconds += 1;
        micros = 0;
    }
    auto tm = local_time(static_cast<std::time_t>(whole_seconds));
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setw(6) << std::setfill('0')
        << micros;
    // Selection compares the same microsecond-resolution datetime as Python,
    // so different 100 ns timestamps within one microsecond remain a path tie.
    return {whole_seconds + micros / 1e6, "file_mtime", out.str()};
}
struct TestFrame {
    TestCaseResult value;
    std::string start, end;
};
struct Frame {
    SuiteCandidate value;
    std::optional<TestFrame> test;
    int index = 0;
};
struct Scanner {
    std::string path;
    double mtime;
    int preorder = 0;
    std::vector<Frame> stack, discovered;
    void start(const std::string &tag, const std::map<std::string, std::string> &attrs) {
        auto get = [&](const char *key) {
            auto it = attrs.find(key);
            return it == attrs.end() ? std::string() : it->second;
        };
        if (tag == "suite") {
            Frame f;
            f.index = ++preorder;
            auto &c = f.value;
            c.suite_id = get("id");
            c.suite_name = rf::unicode_strip(get("name"));
            if (c.suite_name.empty())
                c.suite_name = "<unnamed-suite>";
            if (!stack.empty()) {
                c.suite_path_parts = stack.back().value.suite_path_parts;
                c.robot_source = stack.back().value.robot_source;
            }
            auto source = rf::unicode_strip(get("source"));
            if (!source.empty())
                c.robot_source = source;
            c.suite_path_parts.push_back(c.suite_name);
            c.suite_path = rf::join(c.suite_path_parts, " / ");
            c.status = "UNKNOWN";
            stack.push_back(std::move(f));
        } else if (tag == "test" && !stack.empty()) {
            TestFrame t;
            t.value.name = rf::unicode_strip(get("name"));
            if (t.value.name.empty())
                t.value.name = "<unnamed-test>";
            t.value.status = "UNKNOWN";
            stack.back().test = std::move(t);
        }
    }
    void status(const std::string &parent, const std::map<std::string, std::string> &attrs,
                const std::string &text) {
        if (stack.empty())
            return;
        auto get = [&](const char *key) {
            auto it = attrs.find(key);
            return it == attrs.end() ? std::string() : rf::unicode_strip(it->second);
        };
        auto status = normalized_status(get("status"));
        auto message = rf::unicode_strip(text);
        auto &f = stack.back();
        if (parent == "suite") {
            f.value.status = status;
            f.value.suite_starttime = get("starttime");
            f.value.suite_endtime = get("endtime");
            if (status == "FAIL" && !message.empty())
                f.value.failure_reasons.push_back("套件：" + message);
        } else if (parent == "test" && f.test) {
            auto &t = *f.test;
            t.value.status = status;
            t.start = get("starttime");
            t.end = get("endtime");
            if (status == "FAIL")
                t.value.failure_reason = message.empty() ? "未提供失败消息" : message;
        }
    }
    void end(const std::string &tag) {
        if (stack.empty())
            return;
        if (tag == "test") {
            auto &f = stack.back();
            auto t = f.test.value_or(TestFrame{{"<unnamed-test>", "UNKNOWN", "", {}}, "", ""});
            auto a = parse_date(t.start), b = parse_date(t.end);
            if (a && b && b->naive >= a->naive)
                t.value.duration_seconds = (b->naive - a->naive) / 1e6;
            f.value.test_names.push_back(t.value.name);
            if (t.value.status == "FAIL")
                f.value.failure_reasons.push_back(t.value.name + "：" + t.value.failure_reason);
            if (t.end > f.value.test_endtime)
                f.value.test_endtime = t.end;
            f.value.test_results.push_back(std::move(t.value));
            f.test.reset();
        } else if (tag == "suite") {
            if (!stack.back().value.test_results.empty())
                discovered.push_back(std::move(stack.back()));
            stack.pop_back();
        }
    }
    std::vector<SuiteCandidate> finish() {
        std::sort(discovered.begin(), discovered.end(),
                  [](const auto &a, const auto &b) { return a.index < b.index; });
        std::vector<SuiteCandidate> out;
        for (auto &f : discovered) {
            auto &c = f.value;
            c.file_path = path;
            c.file_name = fs::u8path(path).filename().u8string();
            c.file_mtime = mtime;
            c.candidate_id = path + "::" +
                             (c.suite_id.empty() ? rf::join(c.suite_path_parts, ".") : c.suite_id) +
                             "::" + std::to_string(out.size() + 1);
            out.push_back(std::move(c));
        }
        return out;
    }
};
std::map<std::string, std::string> node_attrs(Node node) {
    std::map<std::string, std::string> attrs;
    for (auto a = node->properties; a; a = a->next)
        if (!a->ns)
            attrs[reinterpret_cast<const char *>(a->name)] =
                attr(node, reinterpret_cast<const char *>(a->name));
    return attrs;
}
void scan_tree(Node node, Scanner &scanner) {
    if (node->type != XML_ELEMENT_NODE)
        return;
    auto tag = name(node);
    auto attrs = node_attrs(node);
    scanner.start(tag, attrs);
    for (auto c = node->children; c; c = c->next)
        scan_tree(c, scanner);
    if (tag == "status") {
        std::string text;
        for (auto c = node->children;
             c && (c->type == XML_TEXT_NODE || c->type == XML_CDATA_SECTION_NODE); c = c->next)
            if (c->content)
                text += reinterpret_cast<const char *>(c->content);
        scanner.status(name(node->parent), attrs, text);
    }
    scanner.end(tag);
}
std::vector<SuiteCandidate> stream_scan(const std::string &path, double mtime) {
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in)
        throw std::runtime_error("无法读取文件");
    std::unique_ptr<xmlTextReader, decltype(&xmlFreeTextReader)> r(
        xmlReaderForIO(read_io, nullptr, &in, nullptr, nullptr, parse_flags), xmlFreeTextReader);
    if (!r)
        throw std::runtime_error("无法创建 XML reader");
    Scanner scanner{path, mtime};
    std::vector<std::string> tags;
    std::map<std::string, std::string> status_attrs;
    std::string status_parent, status_text;
    int status_depth = -1;
    bool root = false;
    int rc = 0;
    while ((rc = xmlTextReaderRead(r.get())) == 1) {
        int type = xmlTextReaderNodeType(r.get()), depth = xmlTextReaderDepth(r.get());
        auto raw = xmlTextReaderConstLocalName(r.get());
        std::string tag = raw ? reinterpret_cast<const char *>(raw) : "";
        if (type == XML_READER_TYPE_ELEMENT) {
            if (!root) {
                root = true;
                if (tag != "robot")
                    throw std::runtime_error("解析结果根节点不是 <robot>");
            }
            std::map<std::string, std::string> attrs;
            if (xmlTextReaderMoveToFirstAttribute(r.get()) == 1)
                do {
                    auto key = xmlTextReaderConstLocalName(r.get()),
                         v = xmlTextReaderConstValue(r.get());
                    // Robot attributes are unqualified. Extension attributes
                    // such as meta:id must not overwrite the standard id.
                    if (key && v && !xmlTextReaderConstNamespaceUri(r.get()))
                        attrs[reinterpret_cast<const char *>(key)] =
                            reinterpret_cast<const char *>(v);
                } while (xmlTextReaderMoveToNextAttribute(r.get()) == 1);
            xmlTextReaderMoveToElement(r.get());
            scanner.start(tag, attrs);
            if (tag == "status") {
                status_depth = depth;
                status_attrs = attrs;
                status_text.clear();
                status_parent = tags.empty() ? "" : tags.back();
            }
            if (xmlTextReaderIsEmptyElement(r.get())) {
                if (tag == "status") {
                    scanner.status(status_parent, status_attrs, "");
                    status_depth = -1;
                }
                scanner.end(tag);
            } else
                tags.push_back(tag);
        } else if (type == XML_READER_TYPE_END_ELEMENT) {
            if (tag == "status" && depth == status_depth) {
                scanner.status(status_parent, status_attrs, status_text);
                status_depth = -1;
            }
            scanner.end(tag);
            if (!tags.empty())
                tags.pop_back();
        } else if (status_depth >= 0 && depth == status_depth + 1 &&
                   (type == XML_READER_TYPE_TEXT || type == XML_READER_TYPE_CDATA ||
                    type == XML_READER_TYPE_SIGNIFICANT_WHITESPACE ||
                    type == XML_READER_TYPE_WHITESPACE)) {
            auto v = xmlTextReaderConstValue(r.get());
            if (v)
                status_text += reinterpret_cast<const char *>(v);
        }
    }
    if (rc < 0 || !root)
        throw std::runtime_error("XML 严格解析失败");
    return scanner.finish();
}
std::vector<SuiteCandidate> parse_candidates(const std::string &path) {
    auto mtime = file_mtime(path);
    try {
        return stream_scan(path, mtime);
    } catch (const std::exception &) {
        auto doc = parse_tree(path);
        Scanner scanner{path, mtime};
        scan_tree(xmlDocGetRootElement(doc.get()), scanner);
        return scanner.finish();
    }
}
std::string random_token() {
    static std::atomic<unsigned long> counter{0};
    std::random_device random;
    std::ostringstream out;
    out << std::hex << random() << random() << ++counter;
    return out.str();
}
struct Cleanup {
    fs::path path;
    ~Cleanup() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
void protect_source(const fs::path &target, const std::vector<SuiteCandidate> &candidates) {
    auto resolved = fs::weakly_canonical(target);
    for (const auto &c : candidates) {
        std::error_code ec;
        if (fs::weakly_canonical(fs::u8path(c.file_path)) == resolved ||
            (fs::exists(target) && fs::equivalent(target, fs::u8path(c.file_path), ec)))
            throw std::runtime_error("导出路径不能覆盖源 XML 文件。");
    }
}
void replace_file(const fs::path &from, const fs::path &to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("无法替换目标文件，请关闭已打开的文件。");
#else
    fs::rename(from, to);
#endif
}
void save_xml(xmlDoc *doc, const fs::path &path) {
    fs::create_directories(path.parent_path());
    auto temp =
        path.parent_path() / fs::u8path("." + path.filename().u8string() + "-" + random_token());
    Cleanup cleanup{temp};
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out)
        throw std::runtime_error("无法写入 XML");
    auto buffer = xmlOutputBufferCreateIO(write_io, nullptr, &out, nullptr);
    if (!buffer)
        throw std::runtime_error("无法创建 XML 输出流");
    int rc = xmlSaveFormatFileTo(buffer, doc, "UTF-8", 0);
    out.close();
    if (rc < 0 || !out)
        throw std::runtime_error("XML 写入失败");
    replace_file(temp, path);
}
Node find_suite(Node root, const SuiteCandidate &candidate) {
    if (!candidate.suite_id.empty()) {
        std::vector<Node> todo;
        for (auto c = root->children; c; c = c->next)
            todo.push_back(c);
        // Traverse document order; duplicate IDs select the same first node as lxml XPath.
        std::reverse(todo.begin(), todo.end());
        while (!todo.empty()) {
            auto n = todo.back();
            todo.pop_back();
            if (name(n) == "suite" && attr(n, "id") == candidate.suite_id)
                return n;
            std::vector<Node> children;
            for (auto c = n->children; c; c = c->next)
                children.push_back(c);
            for (auto it = children.rbegin(); it != children.rend(); ++it)
                todo.push_back(*it);
        }
    }
    std::vector<Node> level;
    for (auto c = root->children; c; c = c->next)
        if (name(c) == "suite")
            level.push_back(c);
    for (const auto &part : candidate.suite_path_parts) {
        std::vector<Node> next;
        for (auto n : level)
            if (attr(n, "name") == part)
                next.push_back(n);
        if (next.empty())
            for (auto n : level)
                for (auto c = n->children; c; c = c->next)
                    if (name(c) == "suite" && attr(c, "name") == part)
                        next.push_back(c);
        level = std::move(next);
        if (level.empty())
            return nullptr;
    }
    return level.empty() ? nullptr : level.front();
}
void reindex(Node suite, const std::string &id) {
    xmlSetProp(suite, BAD_CAST "id", BAD_CAST id.c_str());
    int s = 0, t = 0;
    for (auto c = suite->children; c; c = c->next) {
        if (name(c) == "suite")
            reindex(c, id + "-s" + std::to_string(++s));
        else if (name(c) == "test")
            xmlSetProp(c, BAD_CAST "id", BAD_CAST(id + "-t" + std::to_string(++t)).c_str());
    }
}
void write_merged(const std::vector<SuiteCandidate> &selected, const fs::path &target) {
    if (selected.empty())
        throw std::runtime_error("没有可合并的数据。");
    protect_source(target, selected);
    Doc merged(xmlNewDoc(BAD_CAST "1.0"), xmlFreeDoc);
    auto root = xmlNewNode(nullptr, BAD_CAST "robot");
    xmlDocSetRootElement(merged.get(), root);
    std::map<std::string, std::string> defaults = {{"generator", "Robotframework-XMLMergeTool"},
                                                   {"rpa", "false"},
                                                   {"schemaversion", "4"},
                                                   {"generated", timestamp(false)}};
    auto first = parse_tree(selected.front().file_path);
    auto original = xmlDocGetRootElement(first.get());
    for (auto key : {"generator", "rpa", "schemaversion"}) {
        auto value = attr(original, key);
        if (!value.empty())
            defaults[key] = value;
    }
    for (const auto &[key, value] : defaults)
        xmlSetProp(root, BAD_CAST key.c_str(), BAD_CAST value.c_str());
    first.reset();
    std::vector<std::string> paths;
    std::map<std::string, std::vector<const SuiteCandidate *>> groups;
    for (const auto &c : selected) {
        if (!groups.count(c.file_path))
            paths.push_back(c.file_path);
        groups[c.file_path].push_back(&c);
    }
    std::map<std::vector<std::string>, Node> cache;
    for (const auto &path : paths) {
        auto source = parse_tree(path);
        for (auto c : groups[path]) {
            auto suite = find_suite(xmlDocGetRootElement(source.get()), *c);
            if (!suite)
                throw std::runtime_error("未能在 " + c->file_path +
                                         " 中定位 suite: " + c->suite_path);
            auto parent = root;
            std::vector<std::string> prefix;
            for (std::size_t i = 0; i + 1 < c->suite_path_parts.size(); ++i) {
                auto part = c->suite_path_parts[i];
                prefix.push_back(part);
                auto it = cache.find(prefix);
                if (it != cache.end()) {
                    parent = it->second;
                    continue;
                }
                Node found = nullptr;
                for (auto n = parent->children; n; n = n->next)
                    if (name(n) == "suite" && attr(n, "name") == part) {
                        found = n;
                        break;
                    }
                if (!found) {
                    found = xmlNewChild(parent, nullptr, BAD_CAST "suite", nullptr);
                    xmlSetProp(found, BAD_CAST "name", BAD_CAST part.c_str());
                }
                cache[prefix] = parent = found;
            }
            auto copied = xmlDocCopyNode(suite, merged.get(), 1);
            if (!copied)
                throw std::runtime_error("无法复制 suite");
            xmlAddChild(parent, copied);
            // lxml deepcopy includes the element tail; libxml stores that tail as sibling text
            // nodes.
            for (auto tail = suite->next;
                 tail && (tail->type == XML_TEXT_NODE || tail->type == XML_CDATA_SECTION_NODE);
                 tail = tail->next)
                xmlAddChild(parent, xmlDocCopyNode(tail, merged.get(), 1));
        }
    }
    int index = 0;
    for (auto c = root->children; c; c = c->next)
        if (name(c) == "suite")
            reindex(c, "s" + std::to_string(++index));
    xmlNewChild(root, nullptr, BAD_CAST "statistics", nullptr);
    xmlNewChild(root, nullptr, BAD_CAST "errors", nullptr);
    save_xml(merged.get(), target);
}
std::string csv_cell(const std::string &s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos)
        return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')
            out += '"';
        out += c;
    }
    return out + '"';
}
void write_csv(const std::vector<SuiteCandidate> &all, const std::vector<std::string> &selected,
               const SelectionSuggestion &suggestion, const fs::path &output) {
    std::set<std::string> chosen(selected.begin(), selected.end()),
        defaults(suggestion.selected_ids.begin(), suggestion.selected_ids.end());
    std::map<std::string, std::vector<const SuiteCandidate *>> groups;
    for (const auto &c : all)
        groups[c.suite_path].push_back(&c);
    std::ofstream out(output, std::ios::binary);
    if (!out)
        throw std::runtime_error("无法写入冲突报告");
    out << "suite_key,candidate_id,file_path,status,suite_endtime,test_endtime,effective_time,"
           "effective_time_source,default_selected,selected,decision,group_size\r\n";
    for (const auto &[key, group] : groups)
        for (auto c : group) {
            auto time = effective_time(*c);
            bool def = defaults.count(c->candidate_id), sel = chosen.count(c->candidate_id);
            std::vector<std::string> row = {
                key,
                c->candidate_id,
                c->file_path,
                c->status,
                c->suite_endtime,
                c->test_endtime,
                time.display,
                time.source,
                def ? "yes" : "no",
                sel ? "yes" : "no",
                sel ? (def ? "default-selected" : "user-selected")
                    : (def ? "user-unselected-default" : "filtered-out"),
                std::to_string(group.size())};
            for (std::size_t i = 0; i < row.size(); ++i) {
                if (i)
                    out << ',';
                out << csv_cell(row[i]);
            }
            out << "\r\n";
        }
    out.close();
    if (!out)
        throw std::runtime_error("冲突报告写入失败");
}
void generate_report(const fs::path &input, const fs::path &dir, const std::string &log,
                     const std::string &report, const rf::Options &options,
                     const std::vector<SuiteCandidate> &sources) {
    auto copy = options;
    copy.input = input;
    copy.outputdir = dir;
    copy.log = log;
    copy.report = report;
    for (const auto &filename : {log, report})
        if (filename != "NONE")
            protect_source(dir / fs::u8path(filename), sources);
    if (copy.split_log && log != "NONE") {
        // A sidecar can be an existing symlink/hardlink to an input even though its suffix is .js.
        // Check the complete sidecar namespace before the report engine publishes any files.
        auto target = dir / fs::u8path(log);
        auto prefix = rf::upper(target.stem().u8string()) + "-";
        for (const auto &entry : fs::directory_iterator(target.parent_path())) {
            auto filename = rf::upper(entry.path().filename().u8string());
            if (filename.rfind(prefix, 0) != 0 || entry.path().extension().empty() ||
                rf::upper(entry.path().extension().u8string()) != ".JS")
                continue;
            auto number = filename.substr(prefix.size(), filename.size() - prefix.size() - 3);
            if (!number.empty() && std::all_of(number.begin(), number.end(), [](unsigned char c) {
                    return c >= '0' && c <= '9';
                }))
                protect_source(entry.path(), sources);
        }
    }
    auto result = rf::read_result(input, copy);
    rf::write_outputs(result, copy);
}
void publish_directory(const fs::path &stage, const fs::path &output,
                       const std::vector<SuiteCandidate> &sources) {
    std::vector<fs::path> files;
    for (const auto &entry : fs::directory_iterator(stage))
        if (entry.is_regular_file())
            files.push_back(entry.path());
    for (const auto &path : files)
        protect_source(output / path.filename(), sources);
    // Each rename is atomic; deliberately no claim of a multi-file transaction.
    for (const auto &path : files)
        replace_file(path, output / path.filename());
}
} // namespace

LoadBatchResult MergeService::load_candidates(const std::vector<std::string> &paths,
                                              int max_workers) const {
    static std::once_flag init;
    std::call_once(init, [] { xmlInitParser(); });
    std::vector<std::string> normalized;
    std::set<std::string> seen;
    for (const auto &p : paths) {
        if (p.empty())
            continue;
        auto full = absolute(p);
        if (rf::upper(fs::u8path(full).extension().u8string()) != ".XML" ||
            !fs::exists(fs::u8path(full)) || !seen.insert(full).second)
            continue;
        normalized.push_back(full);
    }
    struct FileResult {
        std::vector<SuiteCandidate> candidates;
        std::string error;
    };
    std::vector<FileResult> files(normalized.size());
    auto count = normalized.size();
    unsigned cpu = std::thread::hardware_concurrency();
    std::size_t workers =
        std::min(count, static_cast<std::size_t>(
                            max_workers > 0 ? max_workers : std::max(1u, cpu > 1 ? cpu - 1 : 1u)));
    std::atomic<std::size_t> next{0};
    auto run = [&] {
        for (;;) {
            auto i = next.fetch_add(1);
            if (i >= count)
                return;
            try {
                files[i].candidates = parse_candidates(normalized[i]);
            } catch (const std::exception &e) {
                files[i].error = e.what();
            }
        }
    };
    std::vector<std::thread> threads;
    std::string parallel_error;
    try {
        for (std::size_t i = 1; i < workers; ++i)
            threads.emplace_back(run);
    } catch (const std::exception &e) {
        parallel_error = e.what();
    }
    run();
    for (auto &t : threads)
        t.join();
    LoadBatchResult result;
    for (std::size_t i = 0; i < count; ++i) {
        if (files[i].error.empty()) {
            ++result.parsed_file_count;
            result.successful_file_paths.push_back(normalized[i]);
        } else {
            ++result.failed_file_count;
            result.issues.push_back({normalized[i], "parse", files[i].error, true});
        }
        for (auto &c : files[i].candidates)
            result.candidates.push_back(std::move(c));
    }
    if (!parallel_error.empty())
        result.issues.push_back({"*", "parallel",
                                 "部分工作线程不可用，已使用其余线程完成解析: " + parallel_error,
                                 true});
    return result;
}
SelectionSuggestion
MergeService::suggest_selection(const std::vector<SuiteCandidate> &candidates) const {
    std::map<std::string, std::vector<const SuiteCandidate *>> groups;
    for (const auto &c : candidates)
        groups[c.suite_path].push_back(&c);
    SelectionSuggestion result;
    std::map<std::string, std::string> winners;
    auto key = [](const SuiteCandidate *c) {
        auto status = normalized_status(c->status);
        return std::make_tuple(status == "PASS" || status == "SKIP" ? 0 : 1,
                               -effective_time(*c).seconds, c->file_path, c->candidate_id);
    };
    for (const auto &[path, group] : groups) {
        auto winner = *std::min_element(group.begin(), group.end(),
                                        [&](auto a, auto b) { return key(a) < key(b); });
        winners[path] = winner->candidate_id;
        ConflictGroup conflict;
        conflict.suite_key = path;
        conflict.winner_id = winner->candidate_id;
        conflict.reason = "status-priority-and-latest-time";
        conflict.winner_time_source = effective_time(*winner).source;
        for (auto c : group)
            if (c->candidate_id != winner->candidate_id)
                conflict.loser_ids.push_back(c->candidate_id);
        if (!conflict.loser_ids.empty())
            result.conflicts.push_back(std::move(conflict));
    }
    for (const auto &c : candidates)
        if (winners[c.suite_path] == c.candidate_id)
            result.selected_ids.push_back(c.candidate_id);
    return result;
}
MergeResult MergeService::merge_selected(const std::vector<SuiteCandidate> &candidates,
                                         const std::vector<std::string> &ids,
                                         const std::string &output_dir,
                                         const rf::Options &options) const {
    std::map<std::string, const SuiteCandidate *> by_id;
    for (const auto &c : candidates)
        by_id[c.candidate_id] = &c;
    std::vector<SuiteCandidate> selected;
    for (const auto &id : ids)
        if (!id.empty() && by_id.count(id))
            selected.push_back(*by_id[id]);
    if (selected.empty())
        throw std::runtime_error("请至少勾选一个有效的测试套件后再合并。");
    auto dir = fs::u8path(absolute(output_dir));
    fs::create_directories(dir);
    auto stage = dir / fs::u8path(".merge-" + random_token());
    fs::create_directory(stage);
    Cleanup cleanup{stage};
    auto stamp = timestamp(true), xml = "merged-" + stamp + ".xml",
         csv = "conflict_report-" + stamp + ".csv", log = "log-" + stamp + ".html",
         report = "report-" + stamp + ".html";
    for (const auto &file : {xml, csv, log, report})
        protect_source(dir / fs::u8path(file), candidates);
    write_merged(selected, stage / fs::u8path(xml));
    write_csv(candidates, ids, suggest_selection(candidates), stage / fs::u8path(csv));
    generate_report(stage / fs::u8path(xml), dir, log, report, options, candidates);
    publish_directory(stage, dir, candidates);
    return {(dir / fs::u8path(xml)).u8string(),
            (dir / fs::u8path(log)).u8string(),
            (dir / fs::u8path(report)).u8string(),
            (dir / fs::u8path(csv)).u8string(),
            selected.size(),
            candidates.size()};
}
std::string MergeService::export_single_candidate_xml(const SuiteCandidate &c,
                                                      const std::string &output_path) const {
    if (output_path.empty())
        throw std::runtime_error("请选择有效的导出路径。");
    auto path = fs::u8path(absolute(output_path));
    protect_source(path, {c});
    write_merged({c}, path);
    return path.u8string();
}
std::string MergeService::export_selected_xml(const std::vector<SuiteCandidate> &candidates,
                                             const std::vector<std::string> &ids,
                                             const std::string &output_dir) const {
    if (output_dir.empty())
        throw std::runtime_error("请选择有效的导出目录。");
    if (ids.empty())
        throw std::runtime_error("请至少选中一个测试套件。");
    std::set<std::string> requested(ids.begin(), ids.end());
    std::vector<SuiteCandidate> selected;
    for (const auto &c : candidates)
        if (requested.erase(c.candidate_id))
            selected.push_back(c);
    if (!requested.empty())
        throw std::runtime_error("选中的记录已失效，请重新加载 XML 文件。");
    const auto dir = fs::u8path(absolute(output_dir));
    const auto stem = "merged-" + timestamp(true);
    auto path = dir / fs::u8path(stem + ".xml");
    for (unsigned suffix = 2; fs::exists(path) || fs::is_symlink(path); ++suffix)
        path = dir / fs::u8path(stem + "-" + std::to_string(suffix) + ".xml");
    protect_source(path, candidates);
    write_merged(selected, path);
    return path.u8string();
}
std::string MergeService::generate_single_candidate_log(const SuiteCandidate &c,
                                                        const std::string &output_dir,
                                                        const rf::Options &options,
                                                        const std::string &log_filename) const {
    if (log_filename.empty())
        throw std::runtime_error("请选择有效的日志路径。");
    auto path =
        fs::u8path(absolute((fs::u8path(output_dir) / fs::u8path(log_filename)).u8string()));
    protect_source(path, {c});
    auto dir = path.parent_path();
    fs::create_directories(dir);
    auto stage = dir / fs::u8path(".single-log-" + random_token());
    fs::create_directory(stage);
    Cleanup cleanup{stage};
    auto input = stage / ".single-log-input.xml";
    write_merged({c}, input);
    // Python only forwards these settings when it also generates a report page.
    auto log_options = options;
    log_options.reporttitle.clear();
    log_options.report_background.clear();
    log_options.suite_stat_level = -1;
    generate_report(input, dir, path.filename().u8string(), "NONE", log_options, {c});
    fs::remove(input);
    return path.u8string();
}

namespace {
std::string excel_text(const std::string &value) {
    std::vector<std::string> points;
    for (std::size_t i = 0; i < value.size();) {
        auto c = static_cast<unsigned char>(value[i]);
        std::size_t width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        width = std::min(width, value.size() - i);
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
            points.push_back("�");
        else
            points.push_back(value.substr(i, width));
        i += width;
    }
    constexpr std::size_t limit = 32767;
    const std::string marker = "\n[内容过长，已截断]";
    // Marker contains eleven Unicode codepoints, including newline and brackets.
    if (points.size() > limit) {
        points.resize(limit - 11);
        points.push_back(marker);
    }
    std::string out;
    for (const auto &p : points)
        out += p;
    return out;
}
std::size_t unicode_size(const std::string &s) {
    return std::count_if(s.begin(), s.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
}
struct ExcelRow {
    std::string test, file, status, reason;
    std::optional<double> duration;
};
void check_xlsx(lxw_error code) {
    if (code != LXW_NO_ERROR)
        throw std::runtime_error(std::string("Excel 写入失败: ") + lxw_strerror(code));
}
lxw_format *body_format(lxw_workbook *book, int band, int column) {
    auto f = workbook_add_format(book);
    format_set_font_name(f, "Microsoft YaHei");
    format_set_font_size(f, 10.5);
    format_set_font_color(f, 0x1F2937);
    format_set_bg_color(f, band ? 0xEAF2F8 : 0xFFFFFF);
    format_set_pattern(f, LXW_PATTERN_SOLID);
    format_set_bottom(f, LXW_BORDER_THIN);
    format_set_bottom_color(f, 0xD6E2EC);
    format_set_align(f, column == 2 || column == 3 ? LXW_ALIGN_CENTER : LXW_ALIGN_LEFT);
    format_set_align(f, column == 2 || column == 3 ? LXW_ALIGN_VERTICAL_CENTER
                                                   : LXW_ALIGN_VERTICAL_TOP);
    if (column != 2 && column != 3)
        format_set_text_wrap(f);
    if (column == 3)
        format_set_num_format(f, "[h]:mm:ss.000");
    return f;
}
void write_workbook(const std::vector<ExcelRow> &rows, const fs::path &path) {
    // libxlsxwriter's Windows file helper accepts UTF-8 filenames and opens them with _wfopen.
    auto book = workbook_new(path.u8string().c_str());
    if (!book)
        throw std::runtime_error("无法创建 Excel 文件。");
    struct BookGuard {
        lxw_workbook *book;
        ~BookGuard() {
            if (book)
                workbook_close(book);
        }
    } guard{book};
    auto sheet = workbook_add_worksheet(book, "测试结果");
    if (!sheet)
        throw std::runtime_error("无法创建 Excel 工作表。");
    worksheet_gridlines(sheet, LXW_HIDE_ALL_GRIDLINES);
    worksheet_set_zoom(sheet, 90);
    worksheet_set_tab_color(sheet, 0x1F4E78);
    worksheet_freeze_panes(sheet, 1, 0);
    auto header = workbook_add_format(book);
    format_set_font_name(header, "Microsoft YaHei");
    format_set_font_size(header, 11);
    format_set_font_color(header, 0xFFFFFF);
    format_set_bold(header);
    format_set_bg_color(header, 0x1F4E78);
    format_set_pattern(header, LXW_PATTERN_SOLID);
    format_set_bottom(header, LXW_BORDER_MEDIUM);
    format_set_bottom_color(header, 0x163A5C);
    format_set_align(header, LXW_ALIGN_CENTER);
    format_set_align(header, LXW_ALIGN_VERTICAL_CENTER);
    const char *headers[] = {"测试名称", "Robot 文件名", "状态", "耗时", "失败原因"};
    for (int col = 0; col < 5; ++col)
        check_xlsx(worksheet_write_string(sheet, 0, col, headers[col], header));
    check_xlsx(worksheet_set_row(sheet, 0, 26, nullptr));
    lxw_format *formats[2][5];
    for (int band = 0; band < 2; ++band)
        for (int col = 0; col < 5; ++col)
            formats[band][col] = body_format(book, band, col);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto &r = rows[i];
        auto row = static_cast<lxw_row_t>(i + 1);
        auto band = i % 2;
        auto test = excel_text(r.test), file = excel_text(r.file), status = excel_text(r.status),
             reason = excel_text(r.reason);
        check_xlsx(worksheet_write_string(sheet, row, 0, test.c_str(), formats[band][0]));
        check_xlsx(worksheet_write_string(sheet, row, 1, file.c_str(), formats[band][1]));
        check_xlsx(worksheet_write_string(sheet, row, 2, status.c_str(), formats[band][2]));
        check_xlsx(worksheet_write_string(sheet, row, 4, reason.c_str(), formats[band][4]));
        if (r.duration && std::isfinite(*r.duration) && *r.duration >= 0)
            check_xlsx(
                worksheet_write_number(sheet, row, 3, *r.duration / 86400.0, formats[band][3]));
        else
            check_xlsx(worksheet_write_blank(sheet, row, 3, formats[band][3]));
        auto lines = std::max(
            {std::size_t(1), (unicode_size(test) + 39) / 40, (unicode_size(reason) + 69) / 70,
             static_cast<std::size_t>(std::count(test.begin(), test.end(), '\n') + 1),
             static_cast<std::size_t>(std::count(reason.begin(), reason.end(), '\n') + 1)});
        check_xlsx(worksheet_set_row(
            sheet, row,
            static_cast<double>(std::min(std::size_t(90), std::max(std::size_t(22), lines * 18))),
            nullptr));
    }
    auto last = static_cast<lxw_row_t>(rows.size());
    check_xlsx(worksheet_autofilter(sheet, 0, 0, last, 4));
    double widths[] = {44, 36, 12, 18, 80};
    for (int col = 0; col < 5; ++col)
        check_xlsx(worksheet_set_column(sheet, col, col, widths[col], nullptr));
    const char *statuses[] = {"PASS", "SKIP", "FAIL"};
    lxw_color_t fills[] = {0xDFF5E9, 0xFFF2DD, 0xFDE7E7}, colors[] = {0x0F7A45, 0x9A5D00, 0xB42318};
    for (int i = 0; i < 3; ++i) {
        auto f = workbook_add_format(book);
        format_set_bg_color(f, fills[i]);
        format_set_font_color(f, colors[i]);
        format_set_bold(f);
        format_set_font_name(f, "Microsoft YaHei");
        auto formula = std::string("$C2=\"") + statuses[i] + "\"";
        lxw_conditional_format rule{};
        rule.type = LXW_CONDITIONAL_TYPE_FORMULA;
        rule.value_string = const_cast<char *>(formula.c_str());
        rule.format = f;
        check_xlsx(worksheet_conditional_format_range(sheet, 1, 2, last, 2, &rule));
    }
    guard.book = nullptr;
    check_xlsx(workbook_close(book));
}
std::string zip_member(mz_zip_archive &zip, const char *filename) {
    size_t size = 0;
    void *data = mz_zip_reader_extract_file_to_heap(&zip, filename, &size, 0);
    if (!data)
        throw std::runtime_error(std::string("Excel ZIP 验证失败: ") + filename);
    std::unique_ptr<void, decltype(&mz_free)> owned(data, mz_free);
    return {static_cast<char *>(data), size};
}
Doc member_xml(mz_zip_archive &zip, const char *filename) {
    auto s = zip_member(zip, filename);
    Doc doc(xmlReadMemory(s.data(), static_cast<int>(s.size()), filename, nullptr, parse_flags),
            xmlFreeDoc);
    if (!doc)
        throw std::runtime_error(std::string("Excel XML 验证失败: ") + filename);
    return doc;
}
Node child(Node n, const char *tag) {
    for (auto c = n ? n->children : nullptr; c; c = c->next)
        if (name(c) == tag)
            return c;
    return nullptr;
}
std::string content(Node n) {
    auto p = n ? xmlNodeGetContent(n) : nullptr;
    std::string s = p ? reinterpret_cast<const char *>(p) : "";
    xmlFree(p);
    return s;
}
void verify_workbook(const fs::path &path, std::size_t expected_rows) {
    auto bytes = read_file(path);
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0))
        throw std::runtime_error("Excel ZIP 无法读取。");
    struct Guard {
        mz_zip_archive *zip;
        ~Guard() { mz_zip_reader_end(zip); }
    } guard{&zip};
    auto workbook = member_xml(zip, "xl/workbook.xml");
    auto sheets = child(xmlDocGetRootElement(workbook.get()), "sheets");
    int sheet_count = 0;
    for (auto n = sheets ? sheets->children : nullptr; n; n = n->next)
        if (name(n) == "sheet") {
            ++sheet_count;
            if (attr(n, "name") != "测试结果")
                throw std::runtime_error("Excel 工作表结构验证失败。");
        }
    if (sheet_count != 1)
        throw std::runtime_error("Excel 工作表数量验证失败。");
    auto strings_doc = member_xml(zip, "xl/sharedStrings.xml");
    std::vector<std::string> strings;
    for (auto n = xmlDocGetRootElement(strings_doc.get())->children; n; n = n->next)
        if (name(n) == "si")
            strings.push_back(content(n));
    auto styles = member_xml(zip, "xl/styles.xml");
    auto root = xmlDocGetRootElement(styles.get());
    std::set<std::string> duration_ids;
    for (auto n = child(root, "numFmts") ? child(root, "numFmts")->children : nullptr; n;
         n = n->next)
        if (name(n) == "numFmt" && attr(n, "formatCode") == "[h]:mm:ss.000")
            duration_ids.insert(attr(n, "numFmtId"));
    std::set<int> duration_styles;
    int style = 0;
    for (auto n = child(root, "cellXfs") ? child(root, "cellXfs")->children : nullptr; n;
         n = n->next)
        if (name(n) == "xf") {
            if (duration_ids.count(attr(n, "numFmtId")))
                duration_styles.insert(style);
            ++style;
        }
    auto sheet = member_xml(zip, "xl/worksheets/sheet1.xml");
    auto data = child(xmlDocGetRootElement(sheet.get()), "sheetData");
    std::size_t rows = 0;
    const std::vector<std::string> headers = {"测试名称", "Robot 文件名", "状态", "耗时",
                                              "失败原因"};
    for (auto row = data ? data->children : nullptr; row; row = row->next)
        if (name(row) == "row") {
            ++rows;
            bool duration = false;
            int cells = 0;
            for (auto cell = row->children; cell; cell = cell->next)
                if (name(cell) == "c") {
                    ++cells;
                    auto ref = attr(cell, "r");
                    if (ref.empty() || ref[0] < 'A' || ref[0] > 'E')
                        throw std::runtime_error("Excel 列结构验证失败。");
                    if (rows == 1) {
                        auto v = content(child(cell, "v"));
                        if (attr(cell, "t") != "s" || v.empty() ||
                            strings.at(std::stoul(v)) != headers.at(ref[0] - 'A'))
                            throw std::runtime_error("Excel 表头验证失败。");
                    } else if (ref[0] == 'D') {
                        duration = true;
                        if (!duration_styles.count(std::stoi(attr(cell, "s"))))
                            throw std::runtime_error("Excel 耗时格式验证失败。");
                        if (!attr(cell, "t").empty() && attr(cell, "t") != "n")
                            throw std::runtime_error("Excel 耗时必须是数值。");
                    }
                }
            if (cells != 5 || (rows > 1 && !duration))
                throw std::runtime_error("Excel 数据列数验证失败。");
        }
    if (rows != expected_rows + 1)
        throw std::runtime_error("Excel 数据行数验证失败。");
}
} // namespace
ExcelExportResult
MergeService::export_selected_tests_excel(const std::vector<SuiteCandidate> &candidates,
                                          const std::vector<std::string> &ids,
                                          const std::string &output_path) const {
    std::set<std::string> chosen;
    for (const auto &id : ids)
        if (!id.empty())
            chosen.insert(id);
    if (chosen.empty())
        throw std::runtime_error("请至少勾选一个测试套件。");
    std::set<std::string> available;
    for (const auto &c : candidates)
        available.insert(c.candidate_id);
    for (const auto &id : chosen)
        if (!available.count(id))
            throw std::runtime_error("勾选项已失效，请重新加载 XML 文件后再试。");
    std::vector<ExcelRow> rows;
    std::size_t selected = 0;
    for (const auto &c : candidates)
        if (chosen.count(c.candidate_id)) {
            ++selected;
            auto source = rf::trim(c.robot_source);
            while (!source.empty() && (source.back() == '/' || source.back() == '\\'))
                source.pop_back();
            auto sep = source.find_last_of("/\\");
            auto file = rf::trim(source.substr(sep == std::string::npos ? 0 : sep + 1));
            if (file.empty())
                file = c.file_name;
            for (const auto &t : c.test_results) {
                auto status = normalized_status(t.status);
                rows.push_back({t.name, file, status, status == "FAIL" ? t.failure_reason : "",
                                t.duration_seconds});
            }
        }
    if (rows.empty())
        throw std::runtime_error("勾选套件中没有可导出的测试结果，请重新加载 XML 后再试。");
    if (rows.size() > 1048575)
        throw std::runtime_error("勾选测试数量超过单个 Excel 工作表上限。");
    if (output_path.empty())
        throw std::runtime_error("请选择有效的 Excel 导出路径。");
    auto target = fs::u8path(absolute(output_path));
    if (rf::upper(target.extension().u8string()) != ".XLSX")
        throw std::runtime_error("Excel 导出路径必须使用 .xlsx 扩展名。");
    protect_source(target, candidates);
    fs::create_directories(target.parent_path());
    auto temp = target.parent_path() /
                fs::u8path("." + target.stem().u8string() + "-" + random_token() + ".xlsx");
    Cleanup cleanup{temp};
    write_workbook(rows, temp);
    verify_workbook(temp, rows.size());
    replace_file(temp, target);
    return {target.u8string(), selected, rows.size()};
}
} // namespace xm
