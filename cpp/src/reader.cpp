#include "report.hpp"
#include <algorithm>
#include <fstream>
#include <libxml/parser.h>
#include <memory>
#include <set>

namespace rf {
namespace {
std::string tag(xmlNode *n) {
    return reinterpret_cast<const char *>(n->name);
}
std::string attr(xmlNode *n, const char *key, const std::string &fallback = "") {
    auto p = xmlGetNoNsProp(n, reinterpret_cast<const xmlChar *>(key));
    if (!p)
        return fallback;
    std::string s(reinterpret_cast<char *>(p));
    xmlFree(p);
    return s;
}
std::string text(xmlNode *n) {
    auto p = xmlNodeGetContent(n);
    if (!p)
        return "";
    std::string s(reinterpret_cast<char *>(p));
    xmlFree(p);
    return s;
}
std::vector<xmlNode *> children(xmlNode *n) {
    std::vector<xmlNode *> out;
    for (auto c = n->children; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE)
            out.push_back(c);
    return out;
}
void require_child(xmlNode *n, const std::string &allowed) {
    if (allowed.find("|" + tag(n) + "|") == std::string::npos)
        throw InputError("Unsupported <" + tag(n) + "> inside <" + tag(n->parent) + "> at line " +
                         std::to_string(xmlGetLineNo(n)));
}
void status(xmlNode *n, Status &s) {
    s.value = attr(n, "status", "FAIL");
    if (s.value != "PASS" && s.value != "FAIL" && s.value != "SKIP" && s.value != "NOT RUN")
        throw InputError("Invalid status: " + s.value);
    s.start = parse_time(attr(n, "starttime"));
    s.end = parse_time(attr(n, "endtime"));
    s.elapsed = s.start && s.end ? *s.end - *s.start : 0;
    s.message = text(n);
}
std::vector<std::string> tags_sorted(const std::vector<std::string> &input) {
    std::map<std::string, std::string> values;
    for (auto &t : input) {
        auto key = normalized(t);
        if (!key.empty() && key != "none")
            values.emplace(key, t);
    }
    std::vector<std::string> out;
    for (auto &[k, v] : values)
        out.push_back(v);
    return out;
}
void depth_check(int depth) {
    if (depth > 512)
        throw InputError("XML nesting exceeds the supported limit (512)");
}
// The DOM is the private semantic layer. Keep fixture boundaries and IF/TRY
// wrappers here until all Rebot transformations have run; Item is the flattened
// presentation model and deliberately cannot represent those distinctions.
bool body_node(xmlNode *n, bool fixtures = false) {
    const auto type = tag(n);
    if (type == "kw" && !fixtures &&
        (attr(n, "type") == "TEARDOWN" ||
         (tag(n->parent) == "test" && attr(n, "type") == "SETUP")))
        return false;
    return std::string("|kw|for|while|iter|if|try|branch|return|break|continue|error|msg|")
               .find("|" + type + "|") != std::string::npos;
}
void erase_node(xmlNode *n) {
    xmlUnlinkNode(n);
    xmlFreeNode(n);
}
void append_doc(xmlNode *n, const std::string &message) {
    xmlNode *doc = nullptr;
    for (auto c : children(n))
        if (tag(c) == "doc")
            doc = c;
    const auto value = trim((doc ? text(doc) : "") + "\n\n" + message);
    if (!doc)
        doc = xmlNewChild(n, nullptr, BAD_CAST "doc", nullptr);
    // xmlNodeSetContent on an element interprets entity references. A text node
    // preserves literal ampersands in the original Robot documentation.
    xmlNodeSetContent(doc, nullptr);
    xmlAddChild(doc, xmlNewText(BAD_CAST value.c_str()));
}
void gather_messages(xmlNode *n, std::vector<xmlNode *> &messages, int depth) {
    depth_check(depth);
    for (auto c : children(n)) {
        if (tag(c) == "msg")
            messages.push_back(xmlCopyNode(c, 1));
        else
            gather_messages(c, messages, depth + 1);
    }
}
void flatten_dom(xmlNode *n, const std::string &mode, int depth = 0) {
    depth_check(depth);
    const auto type = tag(n);
    const bool match = (mode == "FOR" && type == "for") ||
                       (mode == "WHILE" && type == "while") ||
                       (mode == "ITERATION" && type == "iter");
    if (!match) {
        for (auto c : children(n))
            flatten_dom(c, mode, depth + 1);
        return;
    }
    for (auto c : children(n)) {
        if (std::string("|kw|for|while|iter|if|try|").find("|" + tag(c) + "|") ==
            std::string::npos)
            continue;
        std::vector<xmlNode *> messages;
        gather_messages(c, messages, depth + 1);
        for (auto message : messages)
            xmlAddPrevSibling(c, message);
        erase_node(c);
    }
    append_doc(n, "_*Content flattened.*_");
}
bool warning_or_error(xmlNode *n) {
    if (tag(n) == "msg")
        return attr(n, "level") == "WARN" || attr(n, "level") == "ERROR";
    for (auto c : children(n))
        if (warning_or_error(c))
            return true;
    return false;
}
bool passed(xmlNode *n) {
    for (auto c : children(n))
        if (tag(c) == "status")
            return attr(c, "status") == "PASS";
    return false;
}
void clear_content(xmlNode *n) {
    for (auto c : children(n))
        if (body_node(c))
            erase_node(c);
    append_doc(n, "_Keyword data removed using --RemoveKeywords option._");
}
void remove_dom(xmlNode *n, const std::string &mode) {
    const auto type = tag(n);
    if (mode == "ALL" &&
        (type == "kw" || type == "for" || (type == "branch" && tag(n->parent) == "if"))) {
        clear_content(n);
        return;
    }
    const bool loop = (mode == "FOR" && type == "for") ||
                      (mode == "WHILE" && type == "while");
    const bool wuks = mode == "WUKS" && type == "kw" &&
                      attr(n, "library") == "BuiltIn" &&
                      attr(n, "name") == "Wait Until Keyword Succeeds";
    if (loop || wuks) {
        std::vector<xmlNode *> steps;
        for (auto c : children(n))
            if (body_node(c) && tag(c) != "msg")
                steps.push_back(c);
        const std::size_t keep = wuks && !steps.empty() && passed(steps.back()) ? 2 : 1;
        std::size_t removed = 0;
        for (std::size_t i = 0; i + keep < steps.size(); ++i) {
            if ((!loop || passed(steps[i])) && !warning_or_error(steps[i])) {
                erase_node(steps[i]);
                ++removed;
            }
        }
        if (removed)
            append_doc(n, "_" + std::to_string(removed) + (loop ? " passing step" : " failing step") +
                              (removed == 1 ? "" : "s") +
                              " removed using --RemoveKeywords option._");
    }
    for (auto c : children(n))
        if (body_node(c, true) || tag(c) == "suite" || tag(c) == "test")
            remove_dom(c, mode);
}
void remove_passed_dom(xmlNode *n, const Suite &suite, std::size_t &suite_index,
                       std::size_t &test_index) {
    // suite/test statuses already include ancestor teardown effects. The XML
    // itself remains untouched so reparsing never applies a teardown twice.
    for (auto c : children(n)) {
        if (tag(c) == "suite") {
            std::size_t child_suite_index = 0, child_test_index = 0;
            remove_passed_dom(c, suite.suites.at(suite_index++), child_suite_index,
                              child_test_index);
        } else if (tag(c) == "test") {
            const auto &test = suite.tests.at(test_index++);
            if (test.status.value == "PASS" && !warning_or_error(c))
                for (auto item : children(c))
                    if (body_node(item) && tag(item) != "msg")
                        clear_content(item);
        }
    }
}
Item read_item(xmlNode *n, int depth, const std::string &parent_type);
void append_item(std::vector<Item> &body, xmlNode *n, int depth, const std::string &parent_type) {
    // IF and TRY wrappers are flattened by Robot's result model; branch order remains intact.
    if (tag(n) == "if" || tag(n) == "try") {
        depth_check(depth);
        for (auto c : children(n)) {
            require_child(c, "|branch|status|doc|msg|kw|");
            if (tag(c) == "branch" || tag(c) == "msg" || tag(c) == "kw")
                body.push_back(read_item(c, depth + 1, tag(n)));
        }
    } else
        body.push_back(read_item(n, depth, parent_type));
}
Item read_item(xmlNode *n, int depth, const std::string &parent_type) {
    depth_check(depth);
    Item item;
    auto type = tag(n);
    if (type == "msg") {
        item.kind = -1;
        item.text = text(n);
        item.level = attr(n, "level", "INFO");
        level_code(item.level);
        item.html = attr(n, "html") == "true" || attr(n, "html") == "yes";
        item.timestamp = parse_time(attr(n, "timestamp"));
        return item;
    }
    static const std::map<std::string, int> kinds{
        {"KEYWORD", 0},   {"SETUP", 1},     {"TEARDOWN", 2}, {"FOR", 3},
        {"ITERATION", 4}, {"IF", 5},        {"ELSE IF", 6},  {"ELSE", 7},
        {"RETURN", 8},    {"TRY", 9},       {"EXCEPT", 10},  {"FINALLY", 11},
        {"WHILE", 12},    {"CONTINUE", 13}, {"BREAK", 14},   {"ERROR", 15}};
    std::string kind = type == "kw"       ? attr(n, "type", "KEYWORD")
                       : type == "iter"   ? "ITERATION"
                       : type == "branch" ? attr(n, "type", parent_type == "try" ? "TRY" : "IF")
                                          : upper(type);
    auto found = kinds.find(kind);
    if (found == kinds.end())
        throw InputError("Unsupported body element/type: " + type + "/" + kind);
    item.kind = found->second;
    if (type == "kw") {
        item.name = attr(n, "name");
        item.library = attr(n, "library");
    }
    std::vector<std::string> variables, values, patterns;
    std::optional<Item> teardown;
    for (auto c : children(n)) {
        require_child(c, "|doc|arg|var|tag|timeout|status|msg|kw|if|for|try|while|return|break|"
                         "continue|error|iter|pattern|value|");
        auto ct = tag(c);
        if (ct == "status")
            status(c, item.status);
        else if (ct == "doc")
            item.doc = text(c);
        else if (ct == "timeout")
            item.timeout = attr(c, "value");
        else if (ct == "tag")
            item.tags.push_back(text(c));
        else if (ct == "arg")
            item.args.push_back(text(c));
        else if (ct == "value")
            values.push_back(text(c));
        else if (ct == "pattern")
            patterns.push_back(text(c));
        else if (ct == "var") {
            if (type == "kw")
                item.assign.push_back(text(c));
            else if (type == "iter")
                variables.push_back(attr(c, "name") + " = " + text(c));
            else
                variables.push_back(text(c));
        } else if (ct == "kw" && attr(c, "type") == "TEARDOWN")
            teardown = read_item(c, depth + 1, type);
        else
            append_item(item.body, c, depth + 1, type);
    }
    if (teardown && !teardown->name.empty())
        item.body.push_back(std::move(*teardown));
    item.tags = tags_sorted(item.tags);
    if (type == "for") {
        auto extra = join(values, " | ");
        for (const char *key : {"start", "mode", "fill"})
            if (xmlHasProp(n, reinterpret_cast<const xmlChar *>(key)))
                extra += " | " + std::string(key) + "=" + attr(n, key);
        item.name = join(variables, " | ") + " " + attr(n, "flavor", "IN") + " [ " + extra + " ]";
    } else if (type == "while") {
        std::vector<std::string> parts;
        if (!attr(n, "condition").empty())
            parts.push_back(attr(n, "condition"));
        for (const char *key : {"limit", "on_limit", "on_limit_message"})
            if (!attr(n, key).empty())
                parts.push_back(std::string(key) + "=" + attr(n, key));
        item.name = join(parts, " | ");
    } else if (type == "iter")
        item.name = parent_type == "while" ? "" : join(variables, ", ");
    else if (type == "branch") {
        if (parent_type == "if")
            item.name = attr(n, "condition");
        else {
            if (!attr(n, "pattern_type").empty())
                patterns.push_back("type=" + attr(n, "pattern_type"));
            item.name = join(patterns, " | ");
            if (!attr(n, "variable").empty())
                item.name +=
                    (item.name.empty() ? "" : " ") + std::string("AS ") + attr(n, "variable");
        }
    } else if (type == "return" || type == "error")
        item.args = values;
    // Control flow nodes have no result message in RF 6.1.1.
    if (type != "kw")
        item.status.message.clear();
    return item;
}
Test read_test(xmlNode *n, int depth) {
    depth_check(depth);
    Test test;
    test.name = attr(n, "name");
    std::optional<Item> setup, teardown;
    for (auto c : children(n)) {
        require_child(
            c, "|doc|tag|timeout|status|kw|if|for|try|while|return|break|continue|error|msg|");
        auto ct = tag(c);
        if (ct == "doc")
            test.doc = text(c);
        else if (ct == "tag")
            test.tags.push_back(text(c));
        else if (ct == "timeout")
            test.timeout = attr(c, "value");
        else if (ct == "status")
            status(c, test.status);
        else if (ct == "kw" && attr(c, "type") == "SETUP")
            setup = read_item(c, depth + 1, "test");
        else if (ct == "kw" && attr(c, "type") == "TEARDOWN")
            teardown = read_item(c, depth + 1, "test");
        else
            append_item(test.body, c, depth + 1, "test");
    }
    if (setup && !setup->name.empty())
        test.body.insert(test.body.begin(), std::move(*setup));
    if (teardown && !teardown->name.empty())
        test.body.push_back(std::move(*teardown));
    test.tags = tags_sorted(test.tags);
    return test;
}
void read_suite(xmlNode *n, Suite &suite, int depth) {
    depth_check(depth);
    suite.name = attr(n, "name");
    suite.source = attr(n, "source");
    std::optional<Item> setup, teardown;
    for (auto c : children(n)) {
        require_child(c, "|doc|meta|status|kw|test|suite|");
        auto ct = tag(c);
        if (ct == "doc")
            suite.doc = text(c);
        else if (ct == "meta") {
            auto key = attr(c, "name");
            auto it = std::find_if(suite.metadata.begin(), suite.metadata.end(),
                                   [&](auto &p) { return normalized(p.first) == normalized(key); });
            if (it == suite.metadata.end())
                suite.metadata.emplace_back(key, text(c));
            else
                it->second = text(c);
        } else if (ct == "status")
            status(c, suite.status);
        else if (ct == "test")
            suite.tests.push_back(read_test(c, depth + 1));
        else if (ct == "suite") {
            suite.suites.emplace_back();
            read_suite(c, suite.suites.back(), depth + 1);
        } else if (ct == "kw") {
            auto kt = attr(c, "type");
            bool is_teardown =
                kt == "TEARDOWN" || (kt.empty() && (!suite.tests.empty() || !suite.suites.empty()));
            auto &fixture = is_teardown ? teardown : setup;
            if (kt.empty()) {
                if (!fixture) {
                    fixture = Item{};
                    fixture->kind = is_teardown ? 2 : 1;
                    fixture->name = is_teardown ? "Implicit teardown" : "Implicit setup";
                    fixture->status.value = "PASS";
                }
                fixture->body.push_back(read_item(c, depth + 1, "suite"));
            } else {
                auto parsed = read_item(c, depth + 1, "suite");
                if (fixture)
                    parsed.body.insert(parsed.body.begin(),
                                       std::make_move_iterator(fixture->body.begin()),
                                       std::make_move_iterator(fixture->body.end()));
                fixture = std::move(parsed);
            }
        }
    }
    if (setup && !setup->name.empty())
        suite.fixtures.push_back(std::move(*setup));
    if (teardown && !teardown->name.empty())
        suite.fixtures.push_back(std::move(*teardown));
    std::stable_sort(suite.metadata.begin(), suite.metadata.end(),
                     [](auto &a, auto &b) { return normalized(a.first) < normalized(b.first); });
}
void apply_teardown(Suite &suite, const Item &td) {
    for (auto &test : suite.tests) {
        if (td.status.value == "FAIL") {
            if (test.status.value != "SKIP")
                test.status.value = "FAIL";
            test.status.message +=
                (test.status.message.empty() ? "Parent suite teardown failed:\n"
                                             : "\n\nAlso parent suite teardown failed:\n") +
                td.status.message;
        } else {
            test.status.value = "SKIP";
            test.status.message =
                "Skipped in parent suite teardown:\n" + td.status.message +
                (test.status.message.empty() ? "" : "\n\nEarlier message:\n" + test.status.message);
        }
    }
    for (auto &sub : suite.suites)
        apply_teardown(sub, td);
}
void fix_teardowns(Suite &suite) {
    for (auto &sub : suite.suites)
        fix_teardowns(sub);
    for (auto &item : suite.fixtures)
        if (item.kind == 2 && (item.status.value == "FAIL" || item.status.value == "SKIP"))
            apply_teardown(suite, item);
}
void remove_keywords(Suite &suite) {
    suite.fixtures.clear();
    for (auto &t : suite.tests)
        t.body.clear();
    for (auto &sub : suite.suites)
        remove_keywords(sub);
}
void calculate(Suite &suite) {
    suite.counts = {};
    std::int64_t elapsed = 0;
    for (auto &sub : suite.suites)
        calculate(sub);
    for (auto &sub : suite.suites) {
        suite.counts.add(sub.counts);
        elapsed += sub.status.elapsed;
    }
    for (auto &test : suite.tests) {
        if (test.status.value == "PASS")
            suite.counts.pass++;
        else if (test.status.value == "SKIP")
            suite.counts.skip++;
        else
            suite.counts.fail++;
        suite.counts.elapsed += test.status.elapsed;
        elapsed += test.status.elapsed;
    }
    for (auto &item : suite.fixtures)
        elapsed += item.status.elapsed;
    suite.status.value = suite.counts.fail ? "FAIL" : suite.counts.pass ? "PASS" : "SKIP";
    if (!suite.status.start || !suite.status.end)
        suite.status.elapsed = elapsed;
}
bool warning_or_error(const Item &item) {
    if (item.kind == -1)
        return item.level == "WARN" || item.level == "ERROR";
    return std::any_of(item.body.begin(), item.body.end(),
                       [](const Item &child) { return warning_or_error(child); });
}
void remove_passed_fixtures(Suite &suite) {
    if (!suite.counts.fail)
        for (auto &fixture : suite.fixtures)
            if (!warning_or_error(fixture)) {
                // This also handles old XML's implicit suite fixtures. A
                // keyword's own teardown is independent of its removable body.
                fixture.body.erase(std::remove_if(fixture.body.begin(), fixture.body.end(),
                                                  [](const Item &item) { return item.kind != 2; }),
                                   fixture.body.end());
                fixture.doc = trim(fixture.doc +
                                   "\n\n_Keyword data removed using --RemoveKeywords option._");
            }
    for (auto &sub : suite.suites)
        remove_passed_fixtures(sub);
}
Result read_result_impl(const fs::path &input, bool include_keywords, const Options &options) {
    const auto flatten = upper(trim(options.flatten_keywords));
    const auto remove = upper(trim(options.remove_keywords));
    if (!flatten.empty() && flatten != "FOR" && flatten != "WHILE" && flatten != "ITERATION")
        throw InputError("Unsupported --flattenkeywords: " + options.flatten_keywords);
    if (!remove.empty() && remove != "ALL" && remove != "PASSED" && remove != "FOR" &&
        remove != "WHILE" && remove != "WUKS")
        throw InputError("Unsupported --removekeywords: " + options.remove_keywords);
    std::ifstream file(input, std::ios::binary);
    if (!file)
        throw InputError("Cannot open input: " + input.u8string());
    // Read through an existing file handle, allowing Unicode Windows paths.
    auto read = [](void *context, char *buffer, int length) -> int {
        auto &f = *static_cast<std::ifstream *>(context);
        f.read(buffer, length);
        return f.bad() ? -1 : static_cast<int>(f.gcount());
    };
    std::unique_ptr<xmlParserCtxt, decltype(&xmlFreeParserCtxt)> context(xmlNewParserCtxt(),
                                                                         xmlFreeParserCtxt);
    if (!context)
        throw std::runtime_error("Cannot allocate XML parser");
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
        xmlCtxtReadIO(context.get(), read, nullptr, &file, nullptr, nullptr,
                      XML_PARSE_NONET | XML_PARSE_HUGE | XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
        xmlFreeDoc);
    if (!doc)
        throw InputError("Invalid XML: " + trim(context->lastError.message
                                                    ? context->lastError.message
                                                    : "parse failure"));
    if (doc->intSubset || doc->extSubset)
        throw InputError("DTDs/entities are not supported in Robot output");
    auto root = xmlDocGetRootElement(doc.get());
    if (!root || tag(root) != "robot" || root->ns)
        throw InputError("Expected an unqualified <robot> root");
    const auto schema = attr(root, "schemaversion");
    if (schema != "3" && schema != "4")
        throw InputError("Unsupported Robot Framework schema '" +
                         (schema.empty() ? std::string("missing") : schema) +
                         "': supported versions are 3 and 4 (RF 6.0/6.1)");
    if (include_keywords && !flatten.empty())
        flatten_dom(root, flatten);
    Result result;
    result.rpa = attr(root, "rpa", "false") == "true";
    std::vector<xmlNode *> suite_nodes;
    for (auto c : children(root)) {
        require_child(c, "|suite|statistics|errors|");
        if (tag(c) == "suite") {
            read_suite(c, result.suite, 0);
            suite_nodes.push_back(c);
        } else if (tag(c) == "errors")
            for (auto m : children(c)) {
                require_child(m, "|msg|");
                result.errors.push_back(read_item(m, 0, "errors"));
            }
    }
    if (suite_nodes.empty())
        throw InputError("Robot output contains no suite");
    auto generator = upper(attr(root, "generator"));
    const bool teardown_effects = generator == "ROBOT" || generator.rfind("ROBOT ", 0) == 0;
    if (teardown_effects)
        fix_teardowns(result.suite);
    if (include_keywords && !remove.empty()) {
        if (remove == "PASSED") {
            calculate(result.suite);
            // RF accumulates all top-level suites into one model. Continue the
            // model indices across their DOM nodes, restarting only for children.
            std::size_t suite_index = 0, test_index = 0;
            for (auto suite_node : suite_nodes)
                remove_passed_dom(suite_node, result.suite, suite_index, test_index);
        } else
            for (auto suite_node : suite_nodes)
                remove_dom(suite_node, remove);
        result.suite = {};
        for (auto suite_node : suite_nodes)
            read_suite(suite_node, result.suite, 0);
        if (teardown_effects)
            fix_teardowns(result.suite);
    }
    // Rebot applies teardown failures before discarding keywords for report-only output.
    if (!include_keywords)
        remove_keywords(result.suite);
    calculate(result.suite);
    if (include_keywords && remove == "PASSED")
        remove_passed_fixtures(result.suite);
    if (!result.suite.counts.total())
        throw InputError("Suite contains no tests");
    return result;
}
} // namespace
Result read_result(const fs::path &input, bool include_keywords) {
    return read_result_impl(input, include_keywords, Options{});
}
Result read_result(const fs::path &input, const Options &options) {
    return read_result_impl(input, options.log != "NONE", options);
}
} // namespace rf
