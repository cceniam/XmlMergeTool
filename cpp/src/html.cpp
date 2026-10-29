#include "report.hpp"
#include "resources.hpp"
#include <chrono>
#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif
namespace rf {
namespace {
std::string resource(const std::string &key) {
    auto it = report_resources.find(key);
    if (it == report_resources.end())
        throw std::runtime_error("Missing embedded resource: " + key);
    std::string out;
    for (auto chunk : it->second)
        out.append(chunk);
    return out;
}
std::string rstrip(std::string line) {
    auto end = line.find_last_not_of(" \t\r\n");
    return end == std::string::npos ? "" : line.substr(0, end + 1);
}
std::string asset_lines(const std::string &key) {
    std::istringstream input(resource(key));
    std::string out, line;
    while (std::getline(input, line))
        out += rstrip(line) + "\n";
    return out;
}
struct PendingFile {
    fs::path target, temp;
    ~PendingFile() {
        if (!temp.empty()) {
            std::error_code ec;
            fs::remove(temp, ec);
        }
    }
};
void commit(PendingFile &f) {
#ifdef _WIN32
    if (!MoveFileExW(f.temp.c_str(), f.target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace output: " + f.target.u8string());
#else
    fs::rename(f.temp, f.target);
#endif
    f.temp.clear();
}
std::pair<std::uint64_t, std::uint64_t> file_identity(const fs::path &path) {
#ifdef _WIN32
    auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot inspect output: " + path.u8string());
    BY_HANDLE_FILE_INFORMATION info{};
    const auto ok = GetFileInformationByHandle(handle, &info);
    CloseHandle(handle);
    if (!ok)
        throw std::runtime_error("Cannot inspect output: " + path.u8string());
    return {info.dwVolumeSerialNumber,
            (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
#else
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0)
        throw std::runtime_error("Cannot inspect output: " + path.u8string());
    return {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)};
#endif
}
} // namespace
std::string render_html(const std::string &model, bool report) {
    std::istringstream input(resource(report ? "rebot/report.html" : "rebot/log.html"));
    static const std::regex js(R"RF(^<script type="text/javascript" src="([^"]+)"></script>$)RF"),
        css(R"RF(^<link rel="stylesheet" type="text/css" href="([^"]+)" media="([^"]+)">$)RF");
    std::string out, line;
    std::smatch match;
    while (std::getline(input, line)) {
        line = rstrip(line);
        if (line.rfind("<!-- JS MODEL -->", 0) == 0)
            out += "<script type=\"text/javascript\">\n" + model + "</script>\n";
        else if (std::regex_match(line, match, js))
            out += "<script type=\"text/javascript\">\n" +
                   asset_lines(
                       (fs::path("rebot") / match[1].str()).lexically_normal().generic_string()) +
                   "</script>\n";
        else if (std::regex_match(line, match, css))
            out += "<style type=\"text/css\" media=\"" + match[2].str() + "\">\n" +
                   asset_lines(
                       (fs::path("rebot") / match[1].str()).lexically_normal().generic_string()) +
                   "</style>\n";
        else if (line.rfind("<meta name=\"Generator\"", 0) == 0)
            out +=
                "<meta name=\"Generator\" content=\"rf-report-cpp 0.1.0 (RF 6.1.1 compatible)\">\n";
        else
            out += line + "\n";
    }
    return out;
}
void write_outputs(const Result &result, const Options &o) {
    auto absolute = [](const fs::path &p) { return fs::weakly_canonical(fs::absolute(p)); };
    std::vector<fs::path> targets;
    std::vector<std::string> contents;
    for (bool report : {false, true}) {
        const auto &name = report ? o.report : o.log;
        if (name != "NONE") {
            auto page = encode_page(result, o, report);
            auto target = o.outputdir / fs::u8path(name);
            // Publish every dependency before the HTML that references it.
            for (std::size_t i = 0; i < page.split_logs.size(); ++i) {
                auto base = target;
                base.replace_extension();
                base += "-" + std::to_string(i + 1) + ".js";
                targets.push_back(base);
                contents.push_back(std::move(page.split_logs[i]));
            }
            targets.push_back(target);
            contents.push_back(render_html(page.model, report));
        }
    }
    auto input = absolute(o.input);
    std::set<fs::path> seen_paths;
    std::set<std::pair<std::uint64_t, std::uint64_t>> seen_files;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (fs::is_directory(targets[i]))
            throw std::runtime_error("Output path is a directory: " + targets[i].u8string());
        if (absolute(targets[i]) == input ||
            (fs::exists(targets[i]) && fs::equivalent(targets[i], o.input)))
            throw InputError("Output must not overwrite input XML");
        if (!seen_paths.insert(absolute(targets[i])).second)
            throw InputError("Log, report and split files must have different output paths");
        // File identity also catches hard links and case aliases. Sets avoid a
        // quadratic scan when regenerating thousands of existing split files.
        if (fs::exists(targets[i]) && !seen_files.insert(file_identity(targets[i])).second)
            throw InputError("Log, report and split files must have different output paths");
    }
    // Stage every page before publishing any page. A rename is atomic per file.
    std::vector<std::unique_ptr<PendingFile>> pending;
    std::random_device random;
    // A shared suffix makes new target aliases collide during staging too.
    // Let the target filesystem apply its case and Unicode normalization rules
    // rather than assuming that differently spelled names are always distinct.
    const auto suffix = ".tmp-" + std::to_string(random()) + "-" + std::to_string(random());
    for (std::size_t i = 0; i < targets.size(); ++i) {
        auto f = std::make_unique<PendingFile>();
        f->target = targets[i];
        fs::create_directories(fs::absolute(f->target).parent_path());
        auto temp = f->target;
        temp += suffix;
        if (fs::exists(temp))
            throw InputError("Log, report and split files must have different output paths");
        f->temp = std::move(temp);
        std::ofstream output(f->temp, std::ios::binary | std::ios::trunc);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.write(contents[i].data(), static_cast<std::streamsize>(contents[i].size()));
        output.close();
        pending.push_back(std::move(f));
    }
    for (auto &f : pending)
        commit(*f);
}
} // namespace rf
