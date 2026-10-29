// Native implementation of the RF 6.1.1 report data protocol (Apache-2.0).
#include "report.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <tuple>

namespace rf {
namespace {
using Object = std::map<std::string, std::string>;
std::string number(std::int64_t n) {
    return std::to_string(n);
}
std::string elapsed_string(std::int64_t ms) {
    bool negative = ms < 0;
    auto sec = static_cast<std::int64_t>(std::nearbyint(std::abs(static_cast<double>(ms)) / 1000));
    std::ostringstream out;
    if (negative)
        out << '-';
    out << std::setfill('0') << std::setw(2) << sec / 3600 << ':' << std::setw(2) << (sec / 60) % 60
        << ':' << std::setw(2) << sec % 60;
    return out.str();
}
Object stat(const Counts &c, const std::string &label, std::int64_t elapsed) {
    return {{"pass", number(c.pass)},
            {"fail", number(c.fail)},
            {"skip", number(c.skip)},
            {"label", json_quote(escape_html(label))},
            {"elapsed", json_quote(elapsed_string(elapsed))}};
}
class Encoder {
  public:
    Encoder(const Result &r, const Options &o, bool report) : r(r), o(o), report(report) {
        intern("");
    }
    EncodedPage build() {
        std::vector<std::string> suite_stats;
        gather(r.suite, "s1", r.suite.name, suite_stats, 1);
        std::vector<std::string> tag_stats;
        for (auto &[k, v] : tags)
            tag_stats.push_back(json_object(stat(v.second, v.first, v.second.elapsed)));
        auto statistics = json_array(
            {json_array({json_object(
                 stat(r.suite.counts, r.rpa ? "All Tasks" : "All Tests", r.suite.counts.elapsed))}),
             json_array(tag_stats), json_array(suite_stats)});
        auto root = suite(r.suite, "s1");
        std::vector<std::string> errors;
        for (auto &e : r.errors) {
            auto row = message(e, false, "");
            auto found = links.find(key(e));
            if (found != links.end()) {
                row.pop_back();
                row += "," + number(found->second) + "]";
            }
            errors.push_back(row);
        }
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        std::string js = "window.output = {};\nwindow.output.suite = " + root +
                         ";\nwindow.output.strings = [];\n";
        // Avoid a single enormous literal for strings. RF frontend supports concatenated chunks.
        for (std::size_t i = 0; i < top.strings.size(); i += 9500) {
            std::vector<std::string> chunk;
            auto end = std::min(top.strings.size(), i + 9500);
            for (std::size_t j = i; j < end; ++j)
                chunk.push_back(json_quote(compressed_string(top.strings[j])));
            js += "window.output.strings = window.output.strings.concat(" + json_array(chunk) +
                  ");\n";
        }
        js += "window.output.stats = " + statistics +
              ";\nwindow.output.baseMillis = " + number(base.value_or(0)) +
              ";\nwindow.output.generated = " + number(now - base.value_or(0)) +
              ";\nwindow.output.expand_keywords = null;\n";
        if (!report)
            js += "window.output.errors = " + json_array(errors) + ";\n";
        // Rebot 6.1.1 evaluates log_config before lazy result parsing; its default
        // log rpa setting is null, whereas report_config sees the parsed mode.
        Object settings{{"rpa", report ? (r.rpa ? "true" : "false") : "null"},
                        {"title", json_quote(escape_html(report ? o.reporttitle : o.logtitle))}};
        auto logpath = o.outputdir / fs::u8path(o.log),
             reportpath = o.outputdir / fs::u8path(o.report);
        if (report) {
            settings["logURL"] =
                o.log == "NONE" ? "null" : json_quote(link_path(logpath, reportpath.parent_path()));
            std::vector<std::string> colors{"#9e9", "#f66", "#fed84f"};
            if (!o.report_background.empty()) {
                const auto count = std::count(o.report_background.begin(), o.report_background.end(), ':');
                if (count != 1 && count != 2)
                    throw InputError("Report background must be pass:fail or pass:fail:skip");
                std::size_t start = 0;
                for (int i = 0; i <= count; ++i) {
                    auto end = o.report_background.find(':', start);
                    colors[i] = o.report_background.substr(start, end - start);
                    start = end == std::string::npos ? o.report_background.size() : end + 1;
                }
            }
            settings["background"] = json_object({{"pass", json_quote(colors[0])},
                                                  {"fail", json_quote(colors[1])},
                                                  {"skip", json_quote(colors[2])}});
        } else {
            settings["reportURL"] = o.report == "NONE"
                                        ? "null"
                                        : json_quote(link_path(reportpath, logpath.parent_path()));
            settings["splitLogBase"] = json_quote(logpath.stem().u8string());
            settings["defaultLevel"] = json_quote(o.visible);
            settings["minLevel"] = json_quote(min_level);
        }
        return {js + "window.settings = " + json_object(settings) + ";\n", std::move(split_logs)};
    }

  private:
    const Result &r;
    const Options &o;
    bool report, discard = false;
    Time base;
    std::string min_level = "NONE";
    struct StringTable {
        std::vector<std::string> strings;
        std::map<std::string, std::size_t> indices, doc_indices;
    };
    StringTable top;
    StringTable *active = &top;
    std::vector<std::string> split_logs;
    using MessageKey = std::tuple<std::string, std::string, Time>;
    std::map<MessageKey, std::size_t> links;
    std::map<std::string, std::pair<std::string, Counts>> tags;
    MessageKey key(const Item &e) {
        return {e.text, e.level, e.timestamp};
    }
    std::size_t intern(const std::string &s) {
        if (discard)
            return 0;
        auto [it, added] = active->indices.emplace(s, active->strings.size());
        if (added)
            active->strings.push_back(s);
        return it->second;
    }
    std::string str(const std::string &s, bool attribute = false, bool escape = true) {
        if (discard)
            return "0";
        return number(intern(escape ? escape_html(s, attribute) : s));
    }
    std::string doc(const std::string &s) {
        if (discard)
            return "0";
        auto found = active->doc_indices.find(s);
        if (found != active->doc_indices.end())
            return number(found->second);
        auto index = intern(format_doc(s));
        active->doc_indices.emplace(s, index);
        return number(index);
    }
    std::string time(Time t) {
        if (!t)
            return "null";
        // Match int(timestamp_to_secs(t) * 1000), including RF's floating-point truncation.
        auto millis = static_cast<std::int64_t>((static_cast<double>(*t) / 1000.0) * 1000.0);
        if (!base)
            base = millis;
        return number(millis - *base);
    }
    std::string status(const Status &s) {
        int code = s.value == "FAIL" ? 0 : s.value == "PASS" ? 1 : s.value == "SKIP" ? 2 : 3;
        std::vector<std::string> data{number(code), time(s.start), number(s.elapsed)};
        if (!s.message.empty()) {
            bool html = s.message.rfind("*HTML*", 0) == 0;
            auto msg = s.message;
            if (html) {
                msg = msg.substr(6);
                auto p = msg.find_first_not_of(" \t\n\r\f\v");
                msg = p == std::string::npos ? "" : msg.substr(p);
            }
            data.push_back(str(msg, false, !html));
        }
        return json_array(data);
    }
    std::string message(const Item &e, bool in_body, const std::string &parent) {
        if (in_body) {
            if (e.level == "WARN" || e.level == "ERROR") {
                auto previous = active;
                active = &top;
                links[key(e)] = intern(parent);
                active = previous;
            }
            if (level_code(e.level) < level_code(min_level))
                min_level = e.level;
        }
        return json_array(
            {time(e.timestamp), number(level_code(e.level)), str(e.text, false, !e.html)});
    }
    std::string body(const std::vector<Item> &items, const std::string &parent, bool filter,
                     bool split = false, bool split_children = false) {
        std::vector<std::string> data;
        StringTable local;
        auto previous = active;
        const bool splitting = split && o.split_log && !report;
        if (splitting) {
            active = &local;
            intern("");
        }
        int index = 0;
        bool was_discard = discard;
        if (report)
            discard = true;
        for (auto &item : items) {
            if (item.kind < 0) {
                if (!filter || level_code(item.level) >= level_code(o.level))
                    data.push_back(message(item, true, parent));
            } else
                data.push_back(keyword(item, parent + "-k" + std::to_string(++index), split_children));
        }
        discard = was_discard;
        if (splitting) {
            active = previous;
            const auto index = split_logs.size() + 1;
            std::vector<std::string> strings;
            for (const auto &value : local.strings)
                strings.push_back(json_quote(compressed_string(value)));
            const auto filename = fs::u8path(o.log).stem().u8string() + "-" + number(index) + ".js";
            split_logs.push_back("window.keywords" + number(index) + " = " + json_array(data) +
                                 ";\nwindow.strings" + number(index) + " = " + json_array(strings) +
                                 ";\nwindow.fileLoading.notify(" + json_quote(filename) + ");\n");
            return number(index);
        }
        return report ? "[]" : json_array(data);
    }
    std::string keyword(const Item &kw, const std::string &id, bool split = false) {
        std::vector<std::string> data{number(kw.kind),
                                      str(kw.name, true),
                                      str(kw.library, true),
                                      str(kw.timeout),
                                      doc(kw.doc),
                                      str(join(kw.args, ", ")),
                                      str(join(kw.assign, ", ")),
                                      str(join(kw.tags, ", ")),
                                      status(kw.status)};
        // RF 6.1 filters messages in real keyword bodies, not control-flow
        // bodies. Flattened loop/iteration messages therefore survive a higher
        // loglevel threshold, exactly as they do in Rebot.
        data.push_back(body(kw.body, id, kw.kind <= 2, split));
        return json_array(data);
    }
    std::string test(const Test &t, const std::string &id) {
        std::vector<std::string> taglist;
        for (auto &tag : t.tags)
            taglist.push_back(str(tag));
        std::vector<std::string> data{str(t.name, true), str(t.timeout), doc(t.doc),
                                      json_array(taglist), status(t.status)};
        data.push_back(body(t.body, id, false, true));
        return json_array(data);
    }
    std::string suite(const Suite &s, const std::string &id) {
        std::string source = s.source;
        if (!source.empty()) {
            fs::path parsed;
            for (const auto &part : fs::u8path(source))
                if (part != "." && !part.empty())
                    parsed /= part;
            source = parsed.empty() ? "." : parsed.u8string();
        }
        std::string relative;
        if (o.log != "NONE" && !source.empty() && fs::exists(fs::u8path(source)))
            relative =
                link_path(fs::u8path(source), (o.outputdir / fs::u8path(o.log)).parent_path());
        std::vector<std::string> metadata;
        for (auto &[name, value] : s.metadata) {
            metadata.push_back(str(name));
            metadata.push_back(doc(value));
        }
        std::vector<std::string> data{str(s.name, true), str(source),          str(relative),
                                      doc(s.doc),        json_array(metadata), status(s.status)};
        std::vector<std::string> suites, tests;
        int i = 0;
        for (auto &child : s.suites)
            suites.push_back(suite(child, id + "-s" + std::to_string(++i)));
        i = 0;
        for (auto &t : s.tests)
            tests.push_back(test(t, id + "-t" + std::to_string(++i)));
        data.push_back(json_array(suites));
        data.push_back(json_array(tests));
        data.push_back(body(s.fixtures, id, true, false, true));
        data.push_back(json_array({number(s.counts.total()), number(s.counts.pass),
                                   number(s.counts.fail), number(s.counts.skip)}));
        return json_array(data);
    }
    void gather(const Suite &s, const std::string &id, const std::string &longname,
                std::vector<std::string> &stats, int depth) {
        auto entry = stat(s.counts, longname, s.status.elapsed);
        entry["id"] = json_quote(id);
        entry["name"] = json_quote(escape_html(s.name));
        if (o.suite_stat_level <= 0 || depth <= o.suite_stat_level)
            stats.push_back(json_object(entry));
        int i = 0;
        for (auto &child : s.suites)
            gather(child, id + "-s" + std::to_string(++i),
                   longname.empty() ? child.name : longname + "." + child.name, stats, depth + 1);
        for (auto &test : s.tests)
            for (auto &tag : test.tags) {
                auto norm = normalized(tag);
                if (norm.rfind("robot:", 0) == 0)
                    continue;
                auto [it, inserted] = tags.emplace(norm, std::make_pair(tag, Counts{}));
                (void)inserted;
                auto &c = it->second.second;
                c.elapsed += test.status.elapsed;
                if (test.status.value == "PASS")
                    c.pass++;
                else if (test.status.value == "SKIP")
                    c.skip++;
                else
                    c.fail++;
            }
    }
};
} // namespace
std::string encode_model(const Result &r, const Options &o, bool report) {
    return encode_page(r, o, report).model;
}
EncodedPage encode_page(const Result &r, const Options &o, bool report) {
    return Encoder(r, o, report).build();
}
} // namespace rf
