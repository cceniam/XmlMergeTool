#include "report.hpp"
#include <ctime>
#include <iostream>
#include <stdexcept>
#define check(expression)                                                                     \
    do {                                                                                      \
        if (!(expression))                                                                    \
            throw std::runtime_error("Unit check failed at line " + std::to_string(__LINE__) + \
                                     ": " #expression);                                       \
    } while (false)
int main() try {
    check(rf::unicode_strip("").empty());
    for (const std::string space : {
             "\t", "\n", "\v", "\f", "\r", "\x1c", "\x1d", "\x1e", "\x1f", " ",
             u8"\u0085", u8"\u00a0", u8"\u1680", u8"\u2000", u8"\u2001", u8"\u2002", u8"\u2003",
             u8"\u2004", u8"\u2005", u8"\u2006", u8"\u2007", u8"\u2008", u8"\u2009", u8"\u200a",
             u8"\u2028", u8"\u2029", u8"\u202f", u8"\u205f", u8"\u3000"}) {
        check(rf::unicode_strip(space).empty());
        check(rf::unicode_strip(space + u8"测试🧪" + space) == u8"测试🧪");
        check(rf::unicode_strip("A" + space + "B") == "A" + space + "B");
    }
    // These invisible characters are not Python whitespace.
    check(rf::unicode_strip(u8"\u200bvalue\ufeff") == u8"\u200bvalue\ufeff");
    check(rf::format_doc("*bold* and _italic_ and ``code``") ==
          "<p><b>bold</b> and <i>italic</i> and <code>code</code></p>");
    check(rf::format_doc("= Heading =\n\n- one\n- two") ==
          "<h2>Heading</h2>\n<ul>\n<li>one</li>\n<li>two</li>\n</ul>");
    check(rf::escape_html("<test> & \"x\"", true) == "&lt;test&gt; &amp; &quot;x&quot;");
    check(rf::json_quote("</script>\n") == "\"\\u003c/script>\\u000a\"");
    check(*rf::parse_time("20260102 00:00:00.001") - *rf::parse_time("20260101 23:59:59.999") == 2);
    check(rf::compressed_string(std::string(1000, 'x')).front() != '*');
    // Far beyond the libstdc++ regex recursion limit. Check complete output,
    // not just survival, so truncation or disabling formatting cannot pass.
    const std::string long_text(300000, 'x');
    const auto url = "https://example.test/" + long_text;
    check(rf::escape_html(url + ").") == "<a href=\"" + url + "\">" + url + "</a>).");
    for (const auto &[marker, tag] : std::vector<std::pair<std::string, std::string>>{
             {"*", "b"}, {"_", "i"}, {"``", "code"}}) {
        check(rf::format_doc(marker + long_text + marker) ==
              "<p><" + tag + ">" + long_text + "</" + tag + "></p>");
        check(rf::format_doc(marker + long_text) == "<p>" + marker + long_text + "</p>");
    }
    const auto data = "data:image/png;base64," + long_text;
    check(rf::format_doc("[" + data + "|image]") ==
          "<p><img src=\"" + data + "\" title=\"image\"></p>");
    check(rf::format_doc("[" + url + "|label]") ==
          "<p><a href=\"" + url + "\">label</a></p>");
    check(rf::format_doc("= " + long_text + " =") == "<h2>" + long_text + "</h2>");
    check(rf::format_doc("| " + long_text + " |") ==
          "<table border=\"1\">\n<tr>\n<td>" + long_text + "</td>\n</tr>\n</table>");
    check(rf::format_doc(std::string(300000, '-')) == "<hr>");
    const std::string unclosed(300000, '[');
    check(rf::format_doc(unclosed + "x|label") == "<p>" + unclosed + "x|label</p>");
    for (const auto &invalid : {"20260229 00:00:00.000", "19000229 00:00:00.000",
                               "20260431 00:00:00.000", "20260001 00:00:00.000",
                               "20261301 00:00:00.000", "20260100 00:00:00.000",
                               "20260101 24:00:00.000", "20260101 00:60:00.000",
                               "20260101 00:00:60.000"}) {
        bool rejected = false;
        try {
            rf::parse_time(invalid);
        } catch (const rf::InputError &) {
            rejected = true;
        }
        check(rejected);
    }
    check(rf::parse_time("20000229 12:00:00.000").has_value());
    check(rf::parse_time("20240229 12:00:00.000").has_value());
    check(!rf::parse_time("").has_value());
    check(!rf::parse_time("N/A").has_value());
#ifdef _WIN32
    check(rf::link_path(rf::fs::u8path("D:/source case/中文#&%.robot"), "C:/reports") ==
          "file:///D:/source%20case/%E4%B8%AD%E6%96%87%23%26%25.robot");
    check(rf::link_path("d:/source.robot", "C:/reports") == "file:///D:/source.robot");
    check(rf::link_path("D:/source.robot", "D:/reports") == "../source.robot");
    check(rf::link_path("//server/share/source.robot", "C:/reports") ==
          "file://server/share/source.robot");
    // UCRT's actual upper boundary differs from its published year-3000 limit.
    // Match the running CRT instead of assuming January 3001 is invalid.
    for (const std::string stamp : {"19000101 00:00:00.000", "30010101 00:00:00.000",
                                    "99991231 23:59:59.000"}) {
        std::cerr << "Platform timestamp case: " << stamp << std::endl;
        std::tm t{};
        t.tm_year = std::stoi(stamp.substr(0, 4)) - 1900;
        t.tm_mon = std::stoi(stamp.substr(4, 2)) - 1;
        t.tm_mday = std::stoi(stamp.substr(6, 2));
        t.tm_hour = std::stoi(stamp.substr(9, 2));
        t.tm_min = std::stoi(stamp.substr(12, 2));
        t.tm_sec = std::stoi(stamp.substr(15, 2));
        t.tm_isdst = -1;
        const auto expected = std::mktime(&t);
        bool rejected = false;
        rf::Time actual;
        try {
            actual = rf::parse_time(stamp);
        } catch (const rf::InputError &) {
            rejected = true;
        }
        if (expected == static_cast<std::time_t>(-1))
            check(rejected);
        else {
            check(!rejected);
            check(actual == static_cast<std::int64_t>(expected) * 1000);
        }
    }
#endif
    std::cout << "Formatting and encoding checks passed\n";
} catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    return 1;
}
