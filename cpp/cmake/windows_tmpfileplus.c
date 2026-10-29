/* Windows implementation of libxlsxwriter's tmpfileplus interface.
 * Paths supplied by libxlsxwriter and returned to it are UTF-8. Environment
 * variables and filesystem operations use UTF-16 Windows APIs throughout.
 * File contents stay on disk; only path buffers are allocated in memory.
 */
#ifdef _WIN32
#include <windows.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "xlsxwriter/third_party/tmpfileplus.h"

static wchar_t *utf8_to_wide(const char *text)
{
    int size;
    wchar_t *result;
    if (!text)
        return NULL;
    size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!size) {
        errno = EINVAL;
        return NULL;
    }
    result = (wchar_t *)malloc((size_t)size * sizeof(wchar_t));
    if (!result) {
        errno = ENOMEM;
        return NULL;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result, size)) {
        free(result);
        errno = EINVAL;
        return NULL;
    }
    return result;
}

static char *wide_to_utf8(const wchar_t *text)
{
    int size;
    char *result;
    size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                              NULL, 0, NULL, NULL);
    if (!size) {
        errno = EINVAL;
        return NULL;
    }
    result = (char *)malloc((size_t)size);
    if (!result) {
        errno = ENOMEM;
        return NULL;
    }
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                             result, size, NULL, NULL)) {
        free(result);
        errno = EINVAL;
        return NULL;
    }
    return result;
}

static wchar_t *environment_directory(const wchar_t *name)
{
    DWORD required;
    DWORD copied;
    wchar_t *result;
    int attempt;
    /* Retry if another thread changes the environment between the two calls. */
    for (attempt = 0; attempt < 3; ++attempt) {
        required = GetEnvironmentVariableW(name, NULL, 0);
        if (!required)
            return NULL;
        result = (wchar_t *)malloc((size_t)required * sizeof(wchar_t));
        if (!result) {
            errno = ENOMEM;
            return NULL;
        }
        copied = GetEnvironmentVariableW(name, result, required);
        if (copied && copied < required)
            return result;
        free(result);
        if (!copied)
            return NULL;
    }
    return NULL;
}

static void set_file_error(DWORD error)
{
    switch (error) {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        errno = EACCES;
        break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        errno = ENOSPC;
        break;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
        errno = ENOENT;
        break;
    default:
        errno = EIO;
    }
}

static FILE *open_in_directory(const wchar_t *directory, const wchar_t *prefix,
                               char **pathname, int keep)
{
    static volatile LONG sequence = 0;
    DWORD attributes;
    size_t dir_length;
    size_t prefix_length;
    size_t capacity;
    wchar_t *path;
    wchar_t suffix[48];
    LARGE_INTEGER counter;
    DWORD flags;
    HANDLE handle;
    int attempt;
    int fd;
    int saved_errno;
    char *utf8_path;
    FILE *stream;
    if (!directory || !directory[0]) {
        errno = ENOENT;
        return NULL;
    }
    attributes = GetFileAttributesW(directory);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        errno = ENOENT;
        return NULL;
    }
    dir_length = wcslen(directory);
    prefix_length = wcslen(prefix);
    /* A Windows path cannot exceed 32767 UTF-16 code units. */
    if (dir_length > 32700 || prefix_length > 32700 ||
        dir_length + prefix_length + 50 > 32767) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    capacity = dir_length + prefix_length + 50;
    path = (wchar_t *)malloc(capacity * sizeof(wchar_t));
    if (!path) {
        errno = ENOMEM;
        return NULL;
    }
    wcscpy(path, directory);
    if (path[dir_length - 1] != L'\\' && path[dir_length - 1] != L'/')
        path[dir_length++] = L'\\';
    memcpy(path + dir_length, prefix, prefix_length * sizeof(wchar_t));
    flags = FILE_ATTRIBUTE_TEMPORARY;
    if (!keep)
        flags |= FILE_FLAG_DELETE_ON_CLOSE;
    handle = INVALID_HANDLE_VALUE;
    for (attempt = 0; attempt < 32; ++attempt) {
        counter.QuadPart = 0;
        QueryPerformanceCounter(&counter);
        _snwprintf_s(suffix, sizeof(suffix) / sizeof(suffix[0]), _TRUNCATE,
                     L"%08lx-%08lx-%08lx-%08lx",
                     (unsigned long)GetCurrentProcessId(),
                     (unsigned long)counter.HighPart, (unsigned long)counter.LowPart,
                     (unsigned long)InterlockedIncrement(&sequence));
        wcscpy(path + dir_length + prefix_length, suffix);
        handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                             CREATE_NEW, flags, NULL);
        if (handle != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }
    if (handle == INVALID_HANDLE_VALUE) {
        set_file_error(GetLastError());
        free(path);
        return NULL;
    }
    utf8_path = wide_to_utf8(path);
    if (!utf8_path) {
        saved_errno = errno;
        CloseHandle(handle);
        if (keep)
            DeleteFileW(path);
        free(path);
        errno = saved_errno;
        return NULL;
    }
    /* Successful _open_osfhandle transfers HANDLE ownership to the CRT fd. */
    fd = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY);
    if (fd < 0) {
        saved_errno = errno;
        CloseHandle(handle);
        if (keep)
            DeleteFileW(path);
        free(utf8_path);
        free(path);
        errno = saved_errno;
        return NULL;
    }
    stream = _fdopen(fd, "w+b");
    if (!stream) {
        saved_errno = errno;
        _close(fd);
        if (keep)
            DeleteFileW(path);
        free(utf8_path);
        free(path);
        errno = saved_errno;
        return NULL;
    }
    free(path);
    *pathname = utf8_path;
    return stream;
}

FILE *tmpfileplus(const char *dir, const char *prefix, char **pathname, int keep)
{
    wchar_t *owned[3];
    const wchar_t *directories[4];
    wchar_t *wide_prefix;
    char *name;
    FILE *stream;
    int index;
    int saved_errno;
    if (pathname)
        *pathname = NULL;
    wide_prefix = utf8_to_wide(prefix ? prefix : "tmp.");
    if (!wide_prefix)
        return NULL;
    owned[0] = utf8_to_wide(dir);
    owned[1] = environment_directory(L"TMP");
    owned[2] = environment_directory(L"TEMP");
    directories[0] = owned[0];
    directories[1] = owned[1];
    directories[2] = owned[2];
    directories[3] = L".";
    name = NULL;
    stream = NULL;
    for (index = 0; index < 4; ++index) {
        stream = open_in_directory(directories[index], wide_prefix, &name, keep && pathname);
        if (stream)
            break;
    }
    saved_errno = stream ? 0 : errno;
    for (index = 0; index < 3; ++index)
        free(owned[index]);
    free(wide_prefix);
    if (stream && pathname)
        *pathname = name;
    else
        free(name);
    errno = saved_errno;
    return stream;
}

FILE *tmpfileplus_f(const char *dir, const char *prefix, char *pathnamebuf,
                    size_t pathsize, int keep)
{
    char *name;
    wchar_t *wide_name;
    FILE *stream;
    if (!pathnamebuf || !pathsize)
        return tmpfileplus(dir, prefix, NULL, keep);
    pathnamebuf[0] = '\0';
    name = NULL;
    stream = tmpfileplus(dir, prefix, &name, keep);
    if (!stream)
        return NULL;
    if (strlen(name) >= pathsize) {
        fclose(stream);
        if (keep) {
            wide_name = utf8_to_wide(name);
            if (wide_name) {
                DeleteFileW(wide_name);
                free(wide_name);
            }
        }
        free(name);
        errno = E2BIG;
        return NULL;
    }
    strcpy(pathnamebuf, name);
    free(name);
    return stream;
}
#endif
