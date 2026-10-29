// RF formatting behavior adapted from Robot Framework 6.1.1 (Apache-2.0).
#include "report.hpp"
#include "unicode_data.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string_view>
#include <zlib.h>

namespace rf {
static void replace(std::string &s, const std::string &from, const std::string &to) {
    std::size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
}
std::string trim(std::string s) {
    auto first = s.find_first_not_of(" \r\n\t\f\v");
    if (first == std::string::npos)
        return "";
    return s.substr(first, s.find_last_not_of(" \r\n\t\f\v") - first + 1);
}
std::string upper(std::string s) {
    for (char &c : s)
        if (c >= 'a' && c <= 'z')
            c -= 32;
    return s;
}
static void utf8_append(std::string &s, char32_t cp) {
    if (cp < 0x80)
        s += char(cp);
    else if (cp < 0x800) {
        s += char(0xc0 | (cp >> 6));
        s += char(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        s += char(0xe0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 63));
        s += char(0x80 | (cp & 63));
    } else {
        s += char(0xf0 | (cp >> 18));
        s += char(0x80 | ((cp >> 12) & 63));
        s += char(0x80 | ((cp >> 6) & 63));
        s += char(0x80 | (cp & 63));
    }
}
static char32_t next_codepoint(const std::string &s, std::size_t &i) {
    unsigned char first = s[i++];
    char32_t cp = first;
    int remaining = first < 0x80 ? 0 : first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
    if (remaining)
        cp = first & ((1 << (6 - remaining)) - 1);
    while (remaining-- && i < s.size())
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 63);
    return cp;
}
std::string unicode_strip(const std::string &s) {
    // Match Python str.strip() for candidate metadata without changing the
    // report formatter's ASCII trim rules or whitespace inside the value.
    std::size_t first = s.size(), end = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto begin = i;
        const auto cp = next_codepoint(s, i);
        if (!std::binary_search(std::begin(whitespace_data), std::end(whitespace_data), cp)) {
            first = std::min(first, begin);
            end = i;
        }
    }
    return end ? s.substr(first, end - first) : "";
}
std::string normalized(const std::string &s, bool ignore_underscore) {
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        auto cp = next_codepoint(s, i);
        if ((ignore_underscore && cp == '_') ||
            std::binary_search(std::begin(whitespace_data), std::end(whitespace_data), cp))
            continue;
        if (cp >= 'A' && cp <= 'Z')
            cp += 32;
        auto found = std::lower_bound(std::begin(lowercase_data), std::end(lowercase_data), cp,
                                      [](const Lowercase &a, char32_t b) { return a.code < b; });
        if (found != std::end(lowercase_data) && found->code == cp)
            for (auto lowered : found->value)
                utf8_append(out, lowered);
        else
            utf8_append(out, cp);
    }
    return out;
}
std::string join(const std::vector<std::string> &values, const std::string &sep) {
    std::string s;
    for (const auto &v : values) {
        if (&v != &values.front())
            s += sep;
        s += v;
    }
    return s;
}
static std::string quot(std::string s) {
    replace(s, "\"", "&quot;");
    return s;
}
static bool is_image(std::string s) {
    s = upper(s);
    if (s.rfind("DATA:IMAGE/", 0) == 0)
        return true;
    for (const std::string ext : {".JPG", ".JPEG", ".PNG", ".GIF", ".BMP", ".SVG"})
        if (s.size() >= ext.size() && s.compare(s.size() - ext.size(), ext.size(), ext) == 0)
            return true;
    return false;
}
static std::string image_link(const std::string &src, const std::string &title = "") {
    return "<img src=\"" + quot(src) + "\" title=\"" + quot(title.empty() ? src : title) + "\">";
}
static std::string anchor(const std::string &href, const std::string &content = "") {
    return "<a href=\"" + quot(href) + "\">" + (content.empty() ? href : content) + "</a>";
}
static bool contains(std::string_view chars, char c) {
    return chars.find(c) != std::string_view::npos;
}
static bool whitespace(char c) {
    return contains(" \t\n\r\f\v", c);
}
// Mark suffixes consisting of allowed punctuation followed by a space or EOL.
// Scanning backwards avoids both regex stack growth and repeated lookahead.
static std::vector<bool> suffixes(const std::string &s, std::string_view punctuation,
                                 bool multiline = false) {
    std::vector<bool> valid(s.size() + 1);
    valid.back() = true;
    for (std::size_t i = s.size(); i-- > 0;)
        valid[i] = s[i] == ' ' || (multiline && s[i] == '\n') ||
                   (contains(punctuation, s[i]) && valid[i + 1]);
    return valid;
}
static std::string urls(const std::string &text, bool images) {
    if (text.find("://") == std::string::npos)
        return text;
    auto suffix = suffixes(text, ")]}\"'.,!?:;|", true);
    std::string out;
    std::size_t copied = 0;
    for (std::size_t i = 0; i < text.size();) {
        if (i && text[i] != ' ' && text[i - 1] != '\n') {
            ++i;
            continue;
        }
        auto start = i + (text[i] == ' ');
        while (start < text.size() && contains("\"'([{", text[start]))
            ++start;
        auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
        if (start == text.size() || !alpha(text[start])) {
            ++i;
            continue;
        }
        auto body = start + 1;
        while (body < text.size() &&
               (alpha(text[body]) || (text[body] >= '0' && text[body] <= '9') ||
                contains("_+.-", text[body])))
            ++body;
        if (text.compare(body, 3, "://") != 0) {
            ++i;
            continue;
        }
        body += 3;
        auto end = body;
        while (end < text.size() && !whitespace(text[end]) && text[end] != '|') {
            ++end;
            if (suffix[end])
                break;
        }
        if (end == body || !suffix[end]) {
            ++i;
            continue;
        }
        out.append(text, copied, start - copied);
        auto url = text.substr(start, end - start);
        out += images && is_image(url) ? image_link(url) : anchor(url);
        copied = i = end;
    }
    out.append(text, copied, text.size() - copied);
    return out;
}
std::string escape_html(std::string s, bool attr, bool linkify) {
    replace(s, "&", "&amp;");
    replace(s, "<", "&lt;");
    replace(s, ">", "&gt;");
    if (attr) {
        replace(s, "\"", "&quot;");
        replace(s, "\n", "&#10;");
        replace(s, "\r", "&#13;");
        replace(s, "\t", "&#09;");
    } else if (linkify && s.find("://") != std::string::npos)
        s = urls(s, false);
    return s;
}
static std::string emphasis(const std::string &s, std::string_view marker,
                           const std::string &tag, char forbidden = '\0') {
    if (s.find(marker) == std::string::npos)
        return s;
    auto suffix = suffixes(s, "\"').,!?:;");
    const bool bold = marker == "*";
    const auto n = s.size(), width = marker.size();
    // Next eligible closing marker, with no recursive or backtracking parser.
    std::vector<std::size_t> closing(n + 1, n);
    for (std::size_t i = n; i-- > 0;) {
        closing[i] = contains("\r\n", s[i]) ? n : closing[i + 1];
        if (s.compare(i, width, marker) == 0) {
            auto end = i + width;
            if (suffix[end] || (bold && end < n && s[end] == '_' && suffix[end + 1]))
                closing[i] = i;
        }
    }
    std::string out;
    std::size_t copied = 0;
    for (std::size_t i = 0; i < n;) {
        if (i && s[i] != ' ') {
            ++i;
            continue;
        }
        auto start = i + (s[i] == ' ');
        while (start < n && contains("\"'(", s[start]))
            ++start;
        if (bold && start < n && s[start] == '_')
            ++start;
        auto content = start + width;
        if (s.compare(start, width, marker) != 0 || content >= n ||
            s[content] == ' ' || s[content] == forbidden || closing[content + 1] == n) {
            ++i;
            continue;
        }
        auto end = closing[content + 1];
        out.append(s, copied, start - copied);
        out += '<' + tag + '>';
        out.append(s, content, end - content);
        out += "</" + tag + '>';
        copied = i = end + width;
    }
    out.append(s, copied, n - copied);
    return out;
}
static std::string inline_format(std::string s) {
    s = emphasis(s, "*", "b");
    s = emphasis(s, "_", "i", '_');
    s = emphasis(s, "``", "code", '`');
    if (s.find('[') == std::string::npos || s.find('|') == std::string::npos)
        return urls(s, true);
    // The link grammar permits brackets in the target before the separator.
    // Precompute delimiters so repeated unmatched '[' stays linear as well.
    const auto n = s.size();
    std::vector<std::size_t> bars(n + 1, n), ends(n + 1, n);
    for (std::size_t i = n; i-- > 0;) {
        bars[i] = s[i] == '|' ? i : contains("\r\n", s[i]) ? n : bars[i + 1];
        ends[i] = s[i] == ']' ? i : contains("\r\n", s[i]) ? n : ends[i + 1];
    }
    std::string out;
    std::size_t p = 0;
    for (std::size_t i = 0; i + 2 < n;) {
        auto bar = bars[i + 2]; // At least one character precedes the separator.
        auto end = bar < n ? ends[bar + 1] : n;
        if (s[i] != '[' || contains("\r\n", s[i + 1]) || end == n) {
            ++i;
            continue;
        }
        out += urls(s.substr(p, i - p), true);
        auto parts = s.substr(i + 1, end - i - 1);
        bar = parts.find('|');
        auto a = trim(parts.substr(0, bar)), b = trim(parts.substr(bar + 1));
        if (is_image(b))
            out += anchor(a, image_link(b, a));
        else if (is_image(a))
            out += image_link(a, b);
        else
            out += anchor(a, b);
        p = i = end + 1;
    }
    return out + urls(s.substr(p), true);
}
static std::size_t header_level(const std::string &s, std::string *content = nullptr) {
    auto level = s.find_first_not_of('=');
    if (level == std::string::npos || level < 1 || level > 3 || !whitespace(s[level]) ||
        s.size() < 2 * level + 3 || s.compare(s.size() - level, level, s, 0, level) != 0)
        return 0;
    auto begin = level, end = s.size() - level;
    if (!whitespace(s[end - 1]))
        return 0;
    while (begin < end && whitespace(s[begin]))
        ++begin;
    while (end > begin && whitespace(s[end - 1]))
        --end;
    if (begin == end || s.find_first_of("\r\n", begin) < end)
        return 0;
    if (content)
        *content = s.substr(begin, end - begin);
    return level;
}
std::string format_doc(const std::string &text) {
    auto escaped = escape_html(text, false, false);
    std::vector<std::string> lines;
    std::istringstream stream(escaped);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    auto kind = [&](const std::string &l) {
        auto t = trim(l);
        if (t.empty())
            return 0;
        if (t.size() >= 3 && t.rfind("| ", 0) == 0 && t.back() == '|' &&
            (t.size() == 3 || t[t.size() - 2] == ' ') &&
            t.find_first_of("\r\n") == std::string::npos)
            return 1;
        if (t == "|" || t.rfind("| ", 0) == 0)
            return 2;
        if (t.rfind("- ", 0) == 0)
            return 3;
        if (header_level(t))
            return 4;
        if (t.size() >= 3 && t.find_first_not_of('-') == std::string::npos)
            return 5;
        return 6;
    };
    std::vector<std::string> blocks;
    for (std::size_t i = 0; i < lines.size();) {
        int k = kind(lines[i]);
        if (!k) {
            ++i;
            continue;
        }
        std::vector<std::string> group{trim(lines[i++])};
        if (k != 4 && k != 5)
            while (i < lines.size() && !trim(lines[i]).empty() &&
                   (kind(lines[i]) == k || (k == 3 && !lines[i].empty() && lines[i][0] == ' ')))
                group.push_back(trim(lines[i++]));
        if (k == 1) {
            std::vector<std::vector<std::string>> rows;
            std::size_t width = 0;
            for (auto row : group) {
                row = row.substr(1, row.size() - 2);
                std::vector<std::string> cells;
                std::size_t p = 0;
                for (std::size_t j = 0; j < row.size(); ++j)
                    if (row[j] == '|' && j && j + 1 < row.size() && row[j - 1] == ' ' &&
                        row[j + 1] == ' ') {
                        cells.push_back(trim(row.substr(p, j - p)));
                        p = j + 1;
                    }
                cells.push_back(trim(row.substr(p)));
                width = std::max(width, cells.size());
                rows.push_back(std::move(cells));
            }
            std::string out = "<table border=\"1\">";
            for (auto &row : rows) {
                row.resize(width);
                out += "\n<tr>";
                for (auto cell : row) {
                    bool h = cell.size() && cell.front() == '=' && cell.back() == '=';
                    if (h)
                        cell = trim(cell.substr(1, cell.size() > 1 ? cell.size() - 2 : 0));
                    std::string tag = h ? "th" : "td";
                    out += "\n<" + tag + ">" + inline_format(cell) + "</" + tag + ">";
                }
                out += "\n</tr>";
            }
            blocks.push_back(out + "\n</table>");
        } else if (k == 2) {
            std::string out = "<pre>";
            for (auto &l : group)
                out += '\n' + inline_format(l.size() > 2 ? l.substr(2) : "");
            blocks.push_back(out + "\n</pre>");
        } else if (k == 3) {
            std::vector<std::string> items;
            for (auto l : group) {
                if (l.rfind("- ", 0) == 0)
                    items.push_back(trim(l.substr(2)));
                else
                    items.back() += " " + l;
            }
            std::string out = "<ul>";
            for (auto &v : items)
                out += "\n<li>" + inline_format(v) + "</li>";
            blocks.push_back(out + "\n</ul>");
        } else if (k == 4) {
            std::string content;
            auto n = std::to_string(header_level(group[0], &content) + 1);
            blocks.push_back("<h" + n + ">" + content + "</h" + n + ">");
        } else if (k == 5)
            blocks.push_back("<hr>");
        else
            blocks.push_back("<p>" + inline_format(join(group, " ")) + "</p>");
    }
    return join(blocks, "\n");
}
std::string json_quote(const std::string &s) {
    std::string out = "\"";
    const char *hex = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 32 || c == '<') {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += char(c);
    }
    replace(out, "\xe2\x80\xa8", "\\u2028");
    replace(out, "\xe2\x80\xa9", "\\u2029");
    return out + '"';
}
std::string json_array(const std::vector<std::string> &v) {
    return "[" + join(v, ",") + "]";
}
std::string json_object(const std::map<std::string, std::string> &v) {
    std::vector<std::string> parts;
    for (auto &[k, x] : v)
        parts.push_back(json_quote(k) + ":" + x);
    return "{" + join(parts, ",") + "}";
}
std::string compressed_string(const std::string &text) {
    auto count =
        std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
    if (count <= 80)
        return "*" + text;
    uLongf size = compressBound(text.size());
    std::string data(size, '\0');
    if (compress2(reinterpret_cast<Bytef *>(data.data()), &size,
                  reinterpret_cast<const Bytef *>(text.data()), text.size(), 9) != Z_OK)
        throw std::runtime_error("zlib compression failed");
    data.resize(size);
    static const char *alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    for (std::size_t i = 0; i < data.size(); i += 3) {
        unsigned v = static_cast<unsigned char>(data[i]) << 16;
        if (i + 1 < data.size())
            v |= static_cast<unsigned char>(data[i + 1]) << 8;
        if (i + 2 < data.size())
            v |= static_cast<unsigned char>(data[i + 2]);
        b64 += alphabet[v >> 18];
        b64 += alphabet[(v >> 12) & 63];
        b64 += i + 1 < data.size() ? alphabet[(v >> 6) & 63] : '=';
        b64 += i + 2 < data.size() ? alphabet[v & 63] : '=';
    }
    return b64.size() * 1.1 < count ? b64 : "*" + text;
}
Time parse_time(const std::string &s) {
    if (s.empty() || s == "N/A")
        return {};
    static const std::regex re(R"((\d{4})(\d{2})(\d{2}) (\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,6}))?)");
    std::smatch m;
    if (!std::regex_match(s, m, re))
        throw InputError("Invalid RF timestamp: " + s);
    std::tm t{};
    t.tm_year = std::stoi(m[1]) - 1900;
    t.tm_mon = std::stoi(m[2]) - 1;
    t.tm_mday = std::stoi(m[3]);
    t.tm_hour = std::stoi(m[4]);
    t.tm_min = std::stoi(m[5]);
    t.tm_sec = std::stoi(m[6]);
    t.tm_isdst = -1;
    // Validate the calendar separately: mktime may legitimately normalize a
    // local time in a DST gap, which RF 6.1.1 accepts as well.
    const int year = t.tm_year + 1900;
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 ||
        t.tm_mday > month_days[t.tm_mon] + (t.tm_mon == 1 && leap) ||
        t.tm_hour > 23 || t.tm_min > 59 || t.tm_sec > 59)
        throw InputError("Invalid RF timestamp: " + s);
    t.tm_wday = -1;
    errno = 0;
    auto seconds = std::mktime(&t);
    // POSIX can successfully encode the second before the epoch as -1.
    // MSVC's supported range starts at the epoch, so -1 always means failure.
#ifdef _WIN32
    const bool failed = seconds == static_cast<std::time_t>(-1);
#else
    const bool failed = seconds == static_cast<std::time_t>(-1) &&
                        (errno != 0 || t.tm_wday == -1);
#endif
    if (failed)
        throw InputError("RF timestamp is outside the platform time range: " + s);
    auto fraction = m[7].str();
    fraction.resize(6, '0');
    return static_cast<std::int64_t>(seconds) * 1000 +
           static_cast<std::int64_t>(std::nearbyint(std::stoi(fraction) / 1000.0));
}
int level_code(const std::string &level) {
    static const std::map<std::string, int> levels{{"TRACE", 0}, {"DEBUG", 1}, {"INFO", 2},
                                                   {"WARN", 3},  {"ERROR", 4}, {"FAIL", 5},
                                                   {"SKIP", 6},  {"NONE", 7}};
    auto it = levels.find(level);
    if (it == levels.end())
        throw InputError("Invalid log level: " + level);
    return it->second;
}
std::string link_path(const fs::path &target, const fs::path &directory) {
    const auto absolute = fs::absolute(target).lexically_normal();
    const auto relative = absolute.lexically_relative(fs::absolute(directory).lexically_normal());
    auto rel = (relative.empty() ? absolute : relative).generic_u8string();
    std::string out;
    std::size_t drive_colon = std::string::npos;
    if (relative.empty()) {
        // Different Windows drives cannot have a relative link. Match RF's
        // file: + pathname2url fallback, including an unescaped drive colon.
        out = "file:";
#ifdef _WIN32
        if (rel.rfind("//?/", 0) == 0) {
            rel.erase(0, 4);
            if (upper(rel.substr(0, 4)) == "UNC/")
                rel = "//" + rel.substr(4);
        }
        if (rel.size() > 1 && rel[1] == ':') {
            rel[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(rel[0])));
            out += "///";
            drive_colon = 1;
        }
#endif
    }
    const char *hex = "0123456789ABCDEF";
    for (std::size_t i = 0; i < rel.size(); ++i) {
        const auto c = static_cast<unsigned char>(rel[i]);
        if ((std::isalnum(c) && c < 128) || c == '/' || c == '-' || c == '_' || c == '.' ||
            c == '~' || i == drive_colon)
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
} // namespace rf
