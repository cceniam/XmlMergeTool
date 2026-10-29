#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <miniz.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};

std::wstring wide(const std::string &value) {
    if (value.empty())
        return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0);
    if (!size)
        throw std::runtime_error("The packaged archive contains an invalid UTF-8 path.");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

std::wstring quote(const std::wstring &argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        std::vector<wchar_t> buffer(32768);
        DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (!length || length >= buffer.size())
            throw std::runtime_error("Cannot locate a temporary directory.");
        GUID guid;
        if (FAILED(CoCreateGuid(&guid)))
            throw std::runtime_error("Cannot create a unique launch ID.");
        wchar_t identifier[40];
        StringFromGUID2(guid, identifier, 40);
        path = fs::path(buffer.data()) / (L"XmlMergeTool-" + std::wstring(identifier));
        if (!CreateDirectoryW(path.c_str(), nullptr))
            throw std::runtime_error("Cannot create the application temporary directory.");
    }
    ~TemporaryDirectory() {
        // Only this invocation's unique directory is owned by the launcher.
        std::error_code error;
        for (int attempt = 0; attempt != 5; ++attempt) {
            fs::remove_all(path, error);
            if (!error)
                break;
            Sleep(100);
        }
    }
};

std::size_t write_chunk(void *opaque, mz_uint64, const void *data, std::size_t length) {
    auto handle = static_cast<HANDLE>(opaque);
    const auto *next = static_cast<const unsigned char *>(data);
    std::size_t remaining = length;
    while (remaining) {
        DWORD written = 0;
        DWORD chunk = static_cast<DWORD>((std::min)(remaining, std::size_t(0x40000000)));
        if (!WriteFile(handle, next, chunk, &written, nullptr) || !written)
            return 0;
        next += written;
        remaining -= written;
    }
    return length;
}

void extract(HINSTANCE instance, const fs::path &root) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!resource)
        throw std::runtime_error("Application payload is missing.");
    HGLOBAL memory = LoadResource(instance, resource);
    const void *bytes = memory ? LockResource(memory) : nullptr;
    DWORD size = SizeofResource(instance, resource);
    if (!bytes || !size)
        throw std::runtime_error("Application payload cannot be read.");
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, bytes, size, 0))
        throw std::runtime_error("Application payload is corrupt.");
    struct EndArchive {
        mz_zip_archive *archive;
        ~EndArchive() {
            mz_zip_reader_end(archive);
        }
    } end{&archive};
    for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&archive); ++index) {
        mz_zip_archive_file_stat info{};
        if (!mz_zip_reader_file_stat(&archive, index, &info))
            throw std::runtime_error("Cannot inspect application payload.");
        mz_uint required = mz_zip_reader_get_filename(&archive, index, nullptr, 0);
        if (!required)
            throw std::runtime_error("Missing payload filename.");
        std::vector<char> name(required);
        mz_zip_reader_get_filename(&archive, index, name.data(), required);
        std::string relative(name.data());
        std::replace(relative.begin(), relative.end(), '\\', '/');
        fs::path path = wide(relative);
        if (path.empty() || path.is_absolute() || path.has_root_name() ||
            path.has_root_directory() || relative.find(':') != std::string::npos)
            throw std::runtime_error("Invalid application payload path.");
        for (const auto &component : path)
            if (component == L"..")
                throw std::runtime_error("Invalid application payload traversal.");
        // CMake creates a plain-file ZIP. Do not accept symbolic links in a changed payload.
        if (((info.m_external_attr >> 16) & 0170000) == 0120000)
            throw std::runtime_error("Application payload must not contain symbolic links.");
        fs::path destination = root / path;
        if (mz_zip_reader_is_file_a_directory(&archive, index)) {
            fs::create_directories(destination);
            continue;
        }
        fs::create_directories(destination.parent_path());
        Handle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
        if (output.value == INVALID_HANDLE_VALUE ||
            !mz_zip_reader_extract_to_callback(&archive, index, write_chunk, output.value, 0))
            throw std::runtime_error("Cannot unpack the application. Check free disk space and the "
                                     "temporary directory.");
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    try {
        TemporaryDirectory temporary;
        extract(instance, temporary.path);
        fs::path executable = temporary.path / L"bin" / L"XmlMergeTool-app.exe";
        if (!fs::is_regular_file(executable))
            throw std::runtime_error("Packaged application executable is missing.");
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!argv)
            throw std::runtime_error("Cannot decode command-line arguments.");
        std::wstring command = quote(executable.wstring());
        for (int i = 1; i < argc; ++i)
            command += L" " + quote(argv[i]);
        LocalFree(argv);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = static_cast<WORD>(show);
        PROCESS_INFORMATION process{};
        // Inherit the caller's working directory, never the unpacking directory.
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            nullptr, &startup, &process))
            throw std::runtime_error(
                "Cannot start XmlMergeTool. The package may be incomplete or blocked.");
        Handle child(process.hProcess), thread(process.hThread);
        if (WaitForSingleObject(child.value, INFINITE) != WAIT_OBJECT_0)
            throw std::runtime_error("Cannot wait for XmlMergeTool to finish.");
        DWORD result = 1;
        GetExitCodeProcess(child.value, &result);
        return static_cast<int>(result);
    } catch (const std::exception &error) {
        std::wstring message = L"XmlMergeTool 无法启动。\n\n";
        try {
            message += wide(error.what());
        } catch (...) {
            message += L"Unknown startup error.";
        }
        MessageBoxW(nullptr, message.c_str(), L"XmlMergeTool", MB_OK | MB_ICONERROR);
        return 1;
    }
}
